#include <RestArtEngine/Renderer/FrameCaptureArchive.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace {
using namespace rae::renderer::captureArchive;
int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("RestArtEngine-FrameArchive-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    TemporaryDirectory() {
        if (!std::filesystem::create_directory(path))
            throw std::runtime_error("Could not create isolated archive test directory.");
    }
    ~TemporaryDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
std::vector<std::uint8_t> read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
void write(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    stream.close();
    require(static_cast<bool>(stream), "Could not write malformed archive fixture.");
}
void integer(std::vector<std::uint8_t>& data, std::size_t offset, std::uint64_t value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        data.at(offset + i) = static_cast<std::uint8_t>(value & 0xff);
        value >>= 8;
    }
}
} // namespace

int main() {
    try {
        TemporaryDirectory temporary;
        const auto file = temporary.path / "captured-frame.raframe";
        const auto malformed = temporary.path / "malformed.raframe";
        std::string error;
        Archive source;
        source.metadataJson = "{\"frameId\":42,\"supportsReplay\":false,\"name\":\"\xe5\x85\x89\xe7\x85\xa7\"}";
        const float raw[] = {-3.5f, 0.f, 0.25f, 1.f, 65504.f};
        source.payloads.resize(3);
        source.payloads[0].resize(sizeof(raw));
        std::memcpy(source.payloads[0].data(), raw, sizeof(raw));
        source.payloads[1] = {0, 255, 0, 10, 13, 128, 1};
        require(save(file, source, error) && error.empty(), "Valid archive save failed.");
        Archive loaded;
        require(load(file, loaded, error) && error.empty(), "Valid archive load failed.");
        require(source.metadataJson == loaded.metadataJson && source.payloads == loaded.payloads,
            "Archive must preserve metadata and exact binary float/zero/high-byte payloads.");
        const auto original = read(file);
        require(original.size() == 40 + source.metadataJson.size() + 3 * 16 + sizeof(raw) + 7,
            "Container v1 byte count is incorrect.");

        const Archive sentinel{"previous capture", {{4, 2}}};
        const auto reject = [&](const std::vector<std::uint8_t>& damaged) {
            write(malformed, damaged);
            loaded = sentinel;
            error = "old error";
            require(!load(malformed, loaded, error) && !error.empty(), "Invalid archive was accepted.");
            require(loaded.metadataJson == sentinel.metadataJson && loaded.payloads == sentinel.payloads,
                "Failed load must preserve the previous capture.");
        };
        auto damaged = original;
        damaged[0] ^= 1; reject(damaged);
        damaged = original; integer(damaged, 8, 2, 4); reject(damaged);
        damaged = original; integer(damaged, 12, 1, 4); reject(damaged);
        damaged = original; integer(damaged, 28, 1, 4); reject(damaged);
        damaged = original; damaged[40] ^= 1; reject(damaged);
        damaged = original; damaged[40 + source.metadataJson.size() + 16] ^= 1; reject(damaged);
        damaged = original; damaged.resize(19); reject(damaged);
        damaged = original; damaged.pop_back(); reject(damaged);
        damaged = original; damaged.push_back(0); reject(damaged);
        damaged = original; integer(damaged, 16, MaxMetadataBytes + 1, 8); reject(damaged);
        damaged = original; integer(damaged, 24, MaxPayloadCount + 1, 4); reject(damaged);
        damaged = original;
        integer(damaged, 40 + source.metadataJson.size(), MaxPayloadBytes + 1, 8); reject(damaged);
        damaged = original;
        integer(damaged, 40 + source.metadataJson.size(), (std::numeric_limits<std::uint64_t>::max)(), 8);
        reject(damaged);

        Archive invalid = source;
        invalid.metadataJson.assign(static_cast<std::size_t>(MaxMetadataBytes + 1), ' ');
        require(!save(file, invalid, error) && !error.empty() && read(file) == original,
            "Oversized metadata save must preserve the previous file.");
        invalid.metadataJson = "{}";
        invalid.payloads.resize(MaxPayloadCount + 1);
        require(!save(file, invalid, error) && read(file) == original,
            "Oversized payload table must not replace the previous file.");
        invalid = source;
        invalid.metadataJson = std::string("\xc0\xaf", 2);
        require(!save(file, invalid, error) && read(file) == original,
            "Invalid UTF-8 metadata must fail before replacing the previous file.");
        require(!save(temporary.path / "missing" / "frame.raframe", source, error) && !error.empty(),
            "A missing destination parent must report a write failure.");
        const auto directoryTarget = temporary.path / "existing-directory";
        std::filesystem::create_directory(directoryTarget);
        write(directoryTarget / "keep.txt", {1, 2, 3});
        require(!save(directoryTarget, source, error) &&
            read(directoryTarget / "keep.txt") == std::vector<std::uint8_t>({1, 2, 3}),
            "An atomic publication failure must preserve an existing target directory.");
        for (const auto& entry : std::filesystem::directory_iterator(temporary.path))
            require(entry.path().filename().string().find(".tmp-") == std::string::npos,
                "Failed or successful saves must not leave owned temporary files behind.");

        Archive empty{"{}", {}};
        require(save(file, empty, error) && load(file, loaded, error) &&
            loaded.metadataJson == "{}" && loaded.payloads.empty(),
            "An empty-payload archive must replace and roundtrip successfully.");
        require(!load(temporary.path / "missing.raframe", loaded, error) &&
            loaded.metadataJson == "{}" && loaded.payloads.empty(),
            "A missing archive must preserve the previously loaded snapshot.");
        std::cout << "PASS: " << checks << " FrameCaptureArchive checks.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
}
