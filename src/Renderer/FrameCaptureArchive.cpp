#include <RestArtEngine/Renderer/FrameCaptureArchive.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace rae::renderer::captureArchive {
namespace {
// v1 is little-endian: magic[8], version:u32, reserved:u32,
// metadataBytes:u64, payloadCount:u32, reserved:u32, metadataChecksum:u64,
// metadata UTF-8 bytes, then {byteCount:u64, checksum:u64, bytes} per payload.
constexpr std::array<std::uint8_t, 8> Magic{'R', 'A', 'F', 'R', 'A', 'M', 'E', 0};
constexpr std::uint32_t Version = 1;
constexpr std::uint64_t HeaderBytes = 40;
constexpr std::uint64_t MaxFileBytes = HeaderBytes + MaxMetadataBytes +
    MaxPayloadBytes + static_cast<std::uint64_t>(MaxPayloadCount) * 16;

std::span<const std::uint8_t> bytes(const std::string& value) {
    return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

// FNV-1a detects accidental corruption. It does not authenticate the file.
std::uint64_t checksum(std::span<const std::uint8_t> data) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto byte : data) { hash ^= byte; hash *= 1099511628211ull; }
    return hash;
}

bool validUtf8(std::span<const std::uint8_t> data) noexcept {
    std::size_t position = 0;
    while (position < data.size()) {
        const auto first = data[position++];
        if (first < 0x80) continue;
        unsigned continuation = 0;
        std::uint32_t codepoint = 0, minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            continuation = 1; codepoint = first & 0x1fu; minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            continuation = 2; codepoint = first & 0x0fu; minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            continuation = 3; codepoint = first & 0x07u; minimum = 0x10000;
        } else return false;
        if (continuation > data.size() - position) return false;
        for (unsigned i = 0; i < continuation; ++i) {
            const auto next = data[position++];
            if ((next & 0xc0u) != 0x80u) return false;
            codepoint = (codepoint << 6) | (next & 0x3fu);
        }
        if (codepoint < minimum || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
    }
    return true;
}

void validate(const Archive& archive) {
    if (archive.metadataJson.size() > MaxMetadataBytes)
        throw std::runtime_error("Capture metadata exceeds the 16 MiB limit.");
    if (archive.payloads.size() > MaxPayloadCount)
        throw std::runtime_error("Capture exceeds the 4096 payload limit.");
    if (!validUtf8(bytes(archive.metadataJson)))
        throw std::runtime_error("Capture metadata is not valid UTF-8.");
    std::uint64_t total = 0;
    for (const auto& payload : archive.payloads) {
        if (payload.size() > MaxPayloadBytes - total)
            throw std::runtime_error("Capture payloads exceed the 256 MiB limit.");
        total += payload.size();
    }
}

class TemporaryOutput {
public:
    explicit TemporaryOutput(const std::filesystem::path& destination) {
        static std::atomic<std::uint64_t> sequence{0};
        for (unsigned attempt = 0; attempt != 32; ++attempt) {
            path_ = destination;
            path_ += ".tmp-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) +
                "-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
#ifdef _WIN32
            handle_ = CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) { owned_ = true; return; }
            const auto failure = GetLastError();
            if (failure != ERROR_FILE_EXISTS && failure != ERROR_ALREADY_EXISTS)
                throw std::system_error(static_cast<int>(failure), std::system_category(),
                    "Could not create adjacent capture temporary file");
#else
            handle_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (handle_ != -1) { owned_ = true; return; }
            if (errno != EEXIST) throw std::system_error(errno, std::generic_category(),
                "Could not create adjacent capture temporary file");
#endif
        }
        throw std::runtime_error("Could not reserve a unique capture temporary file.");
    }
    ~TemporaryOutput() {
        close();
        if (owned_) { std::error_code ignored; std::filesystem::remove(path_, ignored); }
    }
    TemporaryOutput(const TemporaryOutput&) = delete;
    TemporaryOutput& operator=(const TemporaryOutput&) = delete;

    void write(std::span<const std::uint8_t> data) {
        while (!data.empty()) {
#ifdef _WIN32
            const auto request = static_cast<DWORD>((std::min)(data.size(),
                static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD written = 0;
            if (!WriteFile(handle_, data.data(), request, &written, nullptr) || written == 0)
                throw std::runtime_error("Could not write complete capture temporary file.");
#else
            const auto written = ::write(handle_, data.data(), data.size());
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw std::runtime_error("Could not write complete capture temporary file.");
#endif
            data = data.subspan(static_cast<std::size_t>(written));
        }
    }
    template<class T> void integer(T value) {
        std::array<std::uint8_t, sizeof(T)> encoded{};
        for (std::size_t i = 0; i < encoded.size(); ++i) {
            encoded[i] = static_cast<std::uint8_t>(value & 0xffu);
            value >>= 8;
        }
        write(encoded);
    }
    void publish(const std::filesystem::path& destination) {
#ifdef _WIN32
        if (!FlushFileBuffers(handle_)) throw std::runtime_error("Could not flush capture output.");
        const auto closed = CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
        if (!closed) throw std::runtime_error("Could not close capture output.");
        if (!MoveFileExW(path_.c_str(), destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                "Could not atomically replace capture output");
#else
        if (::fsync(handle_) != 0) throw std::runtime_error("Could not flush capture output.");
        const auto closed = ::close(handle_);
        handle_ = -1;
        if (closed != 0) throw std::runtime_error("Could not close capture output.");
        std::filesystem::rename(path_, destination);
#endif
        owned_ = false;
    }
private:
    void close() noexcept {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) { CloseHandle(handle_); handle_ = INVALID_HANDLE_VALUE; }
#else
        if (handle_ != -1) { ::close(handle_); handle_ = -1; }
#endif
    }
    std::filesystem::path path_;
    bool owned_ = false;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int handle_ = -1;
#endif
};

class Input {
public:
    explicit Input(const std::filesystem::path& path) : stream_(path, std::ios::binary) {
        if (!stream_) throw std::runtime_error("Could not open capture archive.");
        stream_.seekg(0, std::ios::end);
        const auto length = stream_.tellg();
        if (length < 0 || static_cast<std::uint64_t>(length) > MaxFileBytes)
            throw std::runtime_error("Capture archive exceeds the container size limit.");
        remaining = static_cast<std::uint64_t>(length);
        stream_.seekg(0);
    }
    void read(std::span<std::uint8_t> data) {
        if (data.size() > remaining) throw std::runtime_error("Capture archive is truncated.");
        if (!data.empty() && !stream_.read(reinterpret_cast<char*>(data.data()),
            static_cast<std::streamsize>(data.size())))
            throw std::runtime_error("Could not read complete capture archive.");
        remaining -= data.size();
    }
    template<class T> T integer() {
        std::array<std::uint8_t, sizeof(T)> encoded{};
        read(encoded);
        T value = 0;
        for (std::size_t i = 0; i < encoded.size(); ++i)
            value |= static_cast<T>(encoded[i]) << (8 * i);
        return value;
    }
    void finish() {
        if (remaining != 0 || stream_.peek() != std::char_traits<char>::eof())
            throw std::runtime_error("Capture archive contains unexpected trailing bytes.");
        if (stream_.bad()) throw std::runtime_error("Could not finish reading capture archive.");
    }
    std::uint64_t remaining = 0;
private:
    std::ifstream stream_;
};
} // namespace

bool save(const std::filesystem::path& path, const Archive& archive, std::string& error) {
    error.clear();
    try {
        validate(archive);
        TemporaryOutput output(path);
        output.write(Magic);
        output.integer(Version);
        output.integer(std::uint32_t{0});
        output.integer(static_cast<std::uint64_t>(archive.metadataJson.size()));
        output.integer(static_cast<std::uint32_t>(archive.payloads.size()));
        output.integer(std::uint32_t{0});
        output.integer(checksum(bytes(archive.metadataJson)));
        output.write(bytes(archive.metadataJson));
        for (const auto& payload : archive.payloads) {
            output.integer(static_cast<std::uint64_t>(payload.size()));
            output.integer(checksum(payload));
            output.write(payload);
        }
        output.publish(path);
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Capture archive save failed: ") + exception.what();
        return false;
    }
}

bool load(const std::filesystem::path& path, Archive& result, std::string& error) {
    error.clear();
    try {
        Input input(path);
        std::array<std::uint8_t, Magic.size()> magic{};
        input.read(magic);
        if (magic != Magic) throw std::runtime_error("Not a RestArtEngine frame archive.");
        if (input.integer<std::uint32_t>() != Version)
            throw std::runtime_error("Unsupported capture archive version.");
        if (input.integer<std::uint32_t>() != 0)
            throw std::runtime_error("Unsupported capture archive header flags.");
        const auto metadataSize = input.integer<std::uint64_t>();
        const auto payloadCount = input.integer<std::uint32_t>();
        if (input.integer<std::uint32_t>() != 0)
            throw std::runtime_error("Unsupported capture archive header flags.");
        const auto metadataHash = input.integer<std::uint64_t>();
        if (metadataSize > MaxMetadataBytes || payloadCount > MaxPayloadCount)
            throw std::runtime_error("Capture metadata or payload count exceeds the limit.");
        // Check minimum bytes before allocating either metadata or payload table.
        const auto tableBytes = static_cast<std::uint64_t>(payloadCount) * 16;
        if (metadataSize > input.remaining || tableBytes > input.remaining - metadataSize)
            throw std::runtime_error("Capture archive is truncated.");
        Archive candidate;
        candidate.metadataJson.resize(static_cast<std::size_t>(metadataSize));
        input.read({reinterpret_cast<std::uint8_t*>(candidate.metadataJson.data()), candidate.metadataJson.size()});
        if (checksum(bytes(candidate.metadataJson)) != metadataHash)
            throw std::runtime_error("Capture metadata checksum does not match.");
        if (!validUtf8(bytes(candidate.metadataJson)))
            throw std::runtime_error("Capture metadata is not valid UTF-8.");
        candidate.payloads.reserve(payloadCount);
        std::uint64_t total = 0;
        for (std::uint32_t i = 0; i < payloadCount; ++i) {
            const auto size = input.integer<std::uint64_t>();
            const auto hash = input.integer<std::uint64_t>();
            if (size > MaxPayloadBytes - total)
                throw std::runtime_error("Capture payloads exceed the 256 MiB limit.");
            const auto remainingTable = static_cast<std::uint64_t>(payloadCount - i - 1) * 16;
            if (size > input.remaining || remainingTable > input.remaining - size)
                throw std::runtime_error("Capture payload is truncated.");
            auto& payload = candidate.payloads.emplace_back(static_cast<std::size_t>(size));
            input.read(payload);
            if (checksum(payload) != hash)
                throw std::runtime_error("Capture payload checksum does not match.");
            total += size;
        }
        input.finish();
        result = std::move(candidate);
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Capture archive load failed: ") + exception.what();
        return false;
    }
}

} // namespace rae::renderer::captureArchive
