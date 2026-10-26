#include <RestArtEngine/Core/GarbageCollector.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winternl.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace rae {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr std::size_t kEntriesPerCollection = 512;
constexpr std::uintmax_t kMaxJsonBytes = 4096;

void fail(std::string& error, const std::string& message) { error = message; }

std::string uniqueId() {
    std::random_device random;
    constexpr char digits[] = "0123456789abcdef";
    std::string id(32, '0');
    for (auto& c : id) c = digits[random() & 15];
    return id;
}

bool captureName(const std::string& name) {
    constexpr std::size_t prefix = 12; // rae_capture_
    return name.size() == prefix + 32 + 4 && name.compare(0, prefix, "rae_capture_") == 0 &&
        name.compare(prefix + 32, 4, ".bmp") == 0 &&
        std::all_of(name.begin() + prefix, name.begin() + prefix + 32,
            [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// Pin every existing ancestor on Windows without write/delete sharing. This
// prevents both replacement and in-place conversion into a reparse point.
struct DirectoryGuard {
#ifdef _WIN32
    std::vector<HANDLE> handles;
    ~DirectoryGuard() { for (auto handle : handles) CloseHandle(handle); }
#endif
    bool check(const fs::path& path, bool allowMissing, std::string& error) {
        fs::path current;
        for (const auto& part : path) {
            current /= part;
            if (current == path.root_name()) continue;
            std::error_code ec;
            const auto status = fs::symlink_status(current, ec);
            if (ec == std::errc::no_such_file_or_directory || (!ec && !fs::exists(status))) {
                if (allowMissing) continue;
                fail(error, "Temporary capture directory is missing.");
                return false;
            }
            if (ec || fs::is_symlink(status) || !fs::is_directory(status)) {
                fail(error, "Directory path contains a link, inaccessible entry, or non-directory.");
                return false;
            }
#ifdef _WIN32
            // Attribute-only opens do not establish the required Windows
            // sharing constraints. FILE_LIST_DIRECTORY (FILE_READ_DATA for
            // directories) makes denial of subsequent write/delete opens real.
            HANDLE handle = CreateFileW(current.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            BY_HANDLE_FILE_INFORMATION info{};
            if (handle == INVALID_HANDLE_VALUE) {
                fail(error, "Cannot safely open directory ancestors.");
                return false;
            }
            if (!GetFileInformationByHandle(handle, &info) ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                CloseHandle(handle);
                fail(error, "Directory path contains a reparse point.");
                return false;
            }
            handles.push_back(handle);
#endif
        }
        return true;
    }
};

bool regularFile(const fs::path& path, std::string& error) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec || !fs::is_regular_file(status) || fs::is_symlink(status)) {
        fail(error, "Capture or registration is missing, linked, or not a regular file.");
        return false;
    }
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        fail(error, "Capture or registration is a reparse point or inaccessible.");
        return false;
    }
#endif
    if (fs::hard_link_count(path, ec) != 1 || ec) {
        fail(error, "Capture or registration has multiple links or is inaccessible.");
        return false;
    }
    return true;
}

// Deny writes and renames for the entire validation/deletion operation on Windows.
struct FileGuard {
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~FileGuard() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
#endif
    bool open(const fs::path& path, bool deletion, std::string& error) {
        if (!regularFile(path, error)) return false;
#ifdef _WIN32
        handle = CreateFileW(path.c_str(), GENERIC_READ | (deletion ? DELETE : 0),
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        if (handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
            info.nNumberOfLinks != 1) {
            fail(error, "Capture or registration is busy, linked, or inaccessible.");
            return false;
        }
#else
        (void)deletion;
#endif
        return true;
    }
    bool remove(const fs::path& path, std::string& error) {
#ifdef _WIN32
        (void)path;
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition))) {
            fail(error, "Cannot remove expired temporary capture or registration.");
            return false;
        }
        return true;
#else
        std::error_code ec;
        if (!fs::remove(path, ec) || ec) {
            fail(error, "Cannot remove expired temporary capture or registration.");
            return false;
        }
        return true;
#endif
    }
};

#ifndef _WIN32
bool writeExclusive(const fs::path& path, const std::string& data, std::string& error) {
    const int file = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (file < 0) { fail(error, "Cannot create temporary settings/registration file."); return false; }
    const auto written = ::write(file, data.data(), data.size());
    const bool ok = written == static_cast<ssize_t>(data.size()) && ::fsync(file) == 0;
    ::close(file);
    if (!ok) {
        std::error_code ignored;
        fs::remove(path, ignored);
        fail(error, "Cannot finish writing settings/registration file.");
    }
    return ok;
}
#endif

bool writeAtomic(const fs::path& target, const Json& value, bool replace, std::string& error) {
    const fs::path temporary = target.parent_path() / (".rae_gc_" + uniqueId() + ".tmp");
    const std::string data = value.dump(2) + "\n";
    bool ok = false;
#ifdef _WIN32
    FileGuard source;
    source.handle = CreateFileW(temporary.c_str(), GENERIC_WRITE | DELETE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (source.handle == INVALID_HANDLE_VALUE) {
        fail(error, "Cannot exclusively create temporary settings/registration file.");
        return false;
    }
    DWORD written = 0;
    DWORD publishError = ERROR_SUCCESS;
    const bool ready = WriteFile(source.handle, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
        written == data.size() && FlushFileBuffers(source.handle);
    if (ready) {
        const auto name = target.filename().native();
        const auto nameBytes = name.size() * sizeof(wchar_t);
        std::vector<unsigned char> storage(sizeof(FILE_RENAME_INFO) + nameBytes);
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        rename->ReplaceIfExists = replace ? TRUE : FALSE;
        rename->RootDirectory = nullptr;
        rename->FileNameLength = static_cast<DWORD>(nameBytes);
        std::memcpy(rename->FileName, name.data(), nameBytes);
        using RenameFile = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
        const auto renameFile = reinterpret_cast<RenameFile>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile"));
        IO_STATUS_BLOCK status{};
        if (renameFile) {
            // A simple filename with RootDirectory=nullptr renames within the
            // source directory. Unlike MoveFileEx/full-path rename it needs no
            // second writable directory open, so the strong directory guard
            // remains held. The same CREATE_NEW handle owns write through rename.
            // FILE_RENAME_INFORMATION contract:
            // https://learn.microsoft.com/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_rename_information
            const auto result = renameFile(source.handle, &status, rename, static_cast<ULONG>(storage.size()),
                static_cast<FILE_INFORMATION_CLASS>(10)); // FileRenameInformation
            ok = result >= 0;
            publishError = static_cast<DWORD>(result);
        } else publishError = ERROR_PROC_NOT_FOUND;
    } else publishError = GetLastError();
#else
    if (!writeExclusive(temporary, data, error)) return false;
    if (replace) {
        std::error_code ec;
        fs::rename(temporary, target, ec);
        ok = !ec;
    } else {
        ok = ::link(temporary.c_str(), target.c_str()) == 0;
        if (ok) ::unlink(temporary.c_str());
    }
#endif
    if (!ok) {
#ifdef _WIN32
        std::string ignored;
        source.remove(temporary, ignored);
#else
        std::error_code ignored;
        fs::remove(temporary, ignored);
#endif
        fail(error, "Cannot atomically publish settings/registration; previous file was preserved.");
#ifdef _WIN32
        error += " Windows/native status " + std::to_string(publishError) + ".";
#endif
    }
    return ok;
}

// Caller must pin both the containing directory and this exact capture file.
bool writeCaptureRegistration(const fs::path& path, std::string& error) {
    const auto size = fs::file_size(path);
    if (size == 0) { fail(error, "Cannot register an empty capture."); return false; }
    const auto modified = fs::last_write_time(path).time_since_epoch().count();
    auto metadata = path;
    metadata += ".gc.json";
    return writeAtomic(metadata, Json{{"format", "RAEngine.TemporaryCapture"}, {"version", 1},
        {"bytes", size}, {"modifiedTicks", std::to_string(modified)}}, false, error);
}

bool readJson(const fs::path& path, Json& value, std::string& error, const FileGuard& guard) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size > kMaxJsonBytes) { fail(error, "Settings/registration file is inaccessible or too large."); return false; }
    std::string content(static_cast<std::size_t>(size), '\0');
#ifdef _WIN32
    DWORD read = 0;
    if (!ReadFile(guard.handle, content.data(), static_cast<DWORD>(content.size()), &read, nullptr) || read != content.size()) {
        fail(error, "Cannot read settings/registration file."); return false;
    }
#else
    (void)guard;
    std::ifstream stream(path, std::ios::binary);
    if (!stream || !stream.read(content.data(), static_cast<std::streamsize>(content.size()))) {
        fail(error, "Cannot read settings/registration file."); return false;
    }
#endif
    value = Json::parse(content, nullptr, false);
    if (value.is_discarded() || !value.is_object()) { fail(error, "Invalid settings/registration JSON."); return false; }
    return true;
}

bool validNumber(double value, double minimum, double maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}
} // namespace

bool GarbageCollector::setSettings(const GarbageCollectionSettings& settings, std::string& error) {
    error.clear();
    if (!validNumber(settings.intervalSeconds, 1, 86400) ||
        !validNumber(settings.memoryIdleSeconds, 0, 604800) ||
        !validNumber(settings.temporaryCaptureMaxAgeSeconds, 1, 31536000)) {
        fail(error, "GC settings must be finite: interval 1..86400 s, memory idle 0..604800 s, capture age 1..31536000 s.");
        return false;
    }
    settings_ = settings;
    return true;
}

bool GarbageCollector::loadSettings(const fs::path& path, std::string& error) {
    error.clear();
    try {
        std::error_code ec;
        const auto status = fs::symlink_status(path, ec);
        if (ec == std::errc::no_such_file_or_directory || (!ec && !fs::exists(status))) return true;
        FileGuard guard;
        if (!guard.open(path, false, error)) return false;
        Json json;
        if (!readJson(path, json, error, guard)) return false;
        if (json.value("format", "") != "RAEngine.GarbageCollection" || json.at("version") != 1 ||
            !json.at("enabled").is_boolean() || !json.at("intervalSeconds").is_number() ||
            !json.at("memoryIdleSeconds").is_number() || !json.at("temporaryCaptureMaxAgeSeconds").is_number()) {
            fail(error, "Unsupported or invalid GC settings."); return false;
        }
        GarbageCollectionSettings candidate;
        candidate.enabled = json.at("enabled").get<bool>();
        candidate.intervalSeconds = json.at("intervalSeconds").get<double>();
        candidate.memoryIdleSeconds = json.at("memoryIdleSeconds").get<double>();
        candidate.temporaryCaptureMaxAgeSeconds = json.at("temporaryCaptureMaxAgeSeconds").get<double>();
        return setSettings(candidate, error);
    } catch (const std::exception& exception) { fail(error, std::string("Cannot load GC settings: ") + exception.what()); return false; }
}

bool GarbageCollector::saveSettings(const fs::path& path, std::string& error) const {
    error.clear();
    try {
        const auto target = fs::absolute(path).lexically_normal();
        DirectoryGuard before;
        if (!before.check(target.parent_path(), true, error)) return false;
        fs::create_directories(target.parent_path());
        DirectoryGuard guard;
        if (!guard.check(target.parent_path(), false, error)) return false;
        std::error_code ec;
        const auto status = fs::symlink_status(target, ec);
        if (ec && ec != std::errc::no_such_file_or_directory) { fail(error, "Cannot inspect existing GC settings."); return false; }
        if (fs::exists(status) && !regularFile(target, error)) return false;
        return writeAtomic(target, Json{{"format", "RAEngine.GarbageCollection"}, {"version", 1},
            {"enabled", settings_.enabled}, {"intervalSeconds", settings_.intervalSeconds},
            {"memoryIdleSeconds", settings_.memoryIdleSeconds},
            {"temporaryCaptureMaxAgeSeconds", settings_.temporaryCaptureMaxAgeSeconds}}, true, error);
    } catch (const std::exception& exception) { fail(error, std::string("Cannot save GC settings: ") + exception.what()); return false; }
}

bool GarbageCollector::update(double elapsedSeconds,
    const std::function<GarbageCollectionMemoryStats(double)>& collectMemory) {
    if (std::isfinite(elapsedSeconds) && elapsedSeconds > 0 && settings_.enabled)
        elapsedSeconds_ = std::min(86400.0, elapsedSeconds_ + std::min(elapsedSeconds, 86400.0));
    if (!requested_ && (!settings_.enabled || elapsedSeconds_ < settings_.intervalSeconds)) return false;
    requested_ = false;
    elapsedSeconds_ = 0;
    const auto begin = std::chrono::steady_clock::now();
    const auto count = stats_.collections + 1;
    stats_ = {};
    stats_.collections = count;
    try { if (collectMemory) stats_.memory = collectMemory(settings_.memoryIdleSeconds); }
    catch (const std::exception& exception) { stats_.lastError = std::string("Memory collection failed: ") + exception.what(); }
    catch (...) { stats_.lastError = "Memory collection failed."; }
    try { collectTemporaryCaptures(); }
    catch (const std::exception& exception) { stats_.lastError = std::string("Temporary capture collection failed: ") + exception.what(); scan_ = {}; }
    stats_.lastDurationMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    return true;
}

bool GarbageCollector::initializeTemporaryCaptures(const fs::path& root, std::string& error) {
    error.clear();
    try {
        if (root.empty()) { fail(error, "Temporary capture root is empty."); return false; }
        const auto absolute = fs::absolute(root).lexically_normal();
        if (absolute == absolute.root_path()) { fail(error, "A filesystem root cannot hold temporary captures."); return false; }
        DirectoryGuard before;
        if (!before.check(absolute, true, error)) return false;
        fs::create_directories(absolute);
        DirectoryGuard guard;
        if (!guard.check(absolute, false, error)) return false;
        const auto canonical = fs::canonical(absolute);
        temporaryRoot_ = canonical;
        issuedCaptures_.clear();
        scan_ = {};
        return true;
    } catch (const std::exception& exception) { fail(error, std::string("Cannot initialize temporary captures: ") + exception.what()); return false; }
}

fs::path GarbageCollector::temporaryCapturePath(std::string& error) {
    error.clear();
    try {
        DirectoryGuard guard;
        if (temporaryRoot_.empty() || !guard.check(temporaryRoot_, false, error)) {
            if (error.empty()) error = "Temporary captures are not initialized.";
            return {};
        }
        if (issuedCaptures_.size() >= 8192) { fail(error, "Too many unregistered temporary capture requests."); return {}; }
        for (int attempt = 0; attempt < 8; ++attempt) {
            const auto name = "rae_capture_" + uniqueId() + ".bmp";
            const auto path = temporaryRoot_ / name;
            auto metadata = path;
            metadata += ".gc.json";
            if (!fs::exists(fs::symlink_status(path)) && !fs::exists(fs::symlink_status(metadata)) &&
                issuedCaptures_.insert(name).second) return path;
        }
        fail(error, "Cannot allocate a unique temporary capture name.");
    } catch (const std::exception& exception) { fail(error, std::string("Cannot allocate temporary capture: ") + exception.what()); }
    return {};
}

bool GarbageCollector::registerTemporaryCapture(const fs::path& path, std::string& error) {
    error.clear();
    try {
        const auto absolute = fs::absolute(path).lexically_normal();
        const auto name = absolute.filename().string();
        if (temporaryRoot_.empty() || absolute.parent_path() != temporaryRoot_ || !captureName(name) ||
            issuedCaptures_.count(name) == 0) { fail(error, "Only captures issued by this collector may be registered."); return false; }
        DirectoryGuard directory;
        if (!directory.check(temporaryRoot_, false, error)) return false;
        FileGuard capture;
        if (!capture.open(absolute, false, error)) return false;
        if (!writeCaptureRegistration(absolute, error)) return false;
        issuedCaptures_.erase(name);
        return true;
    } catch (const std::exception& exception) { fail(error, std::string("Cannot register temporary capture: ") + exception.what()); return false; }
}

bool GarbageCollector::captureTemporary(const std::function<bool(std::FILE*, std::string&)>& writer,
    fs::path& output, std::string& error) {
    output.clear();
    error.clear();
#ifdef _WIN32
    fs::path path;
    try {
        if (!writer) { fail(error, "Temporary capture writer is missing."); return false; }
        DirectoryGuard directory;
        if (temporaryRoot_.empty() || !directory.check(temporaryRoot_, false, error)) {
            if (error.empty()) error = "Temporary captures are not initialized.";
            return false;
        }
        path = temporaryCapturePath(error);
        if (path.empty()) return false;
        const auto name = path.filename().string();
        // CREATE_NEW never follows or truncates an existing symlink/file. The
        // sharing mode prevents writes, renames and deletion for the whole write.
        FileGuard created;
        created.handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
            FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (created.handle == INVALID_HANDLE_VALUE) {
            issuedCaptures_.erase(name);
            fail(error, "Cannot exclusively create temporary capture.");
            return false;
        }
        BY_HANDLE_FILE_INFORMATION identity{};
        if (!GetFileInformationByHandle(created.handle, &identity)) {
            std::string ignored;
            created.remove(path, ignored);
            issuedCaptures_.erase(name);
            fail(error, "Cannot identify newly created temporary capture.");
            return false;
        }
        const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(created.handle), _O_BINARY | _O_WRONLY);
        if (descriptor < 0) {
            std::string ignored;
            created.remove(path, ignored);
            issuedCaptures_.erase(name);
            fail(error, "Cannot create temporary capture stream descriptor.");
            return false;
        }
        const HANDLE writingHandle = created.handle;
        created.handle = INVALID_HANDLE_VALUE; // descriptor now owns the handle
        std::FILE* rawStream = _fdopen(descriptor, "wb");
        if (!rawStream) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            SetFileInformationByHandle(writingHandle, FileDispositionInfo, &disposition, sizeof(disposition));
            _close(descriptor);
            issuedCaptures_.erase(name);
            fail(error, "Cannot open temporary capture stream.");
            return false;
        }
        std::unique_ptr<std::FILE, decltype(&std::fclose)> stream(rawStream, &std::fclose);
        bool wrote = false;
        try { wrote = writer(stream.get(), error); }
        catch (const std::exception& exception) { error = std::string("Temporary capture writer failed: ") + exception.what(); }
        catch (...) { error = "Temporary capture writer failed."; }
        const bool streamError = std::ferror(stream.get()) != 0;
        const bool flushed = std::fflush(stream.get()) == 0 && FlushFileBuffers(writingHandle) != FALSE;
        wrote = wrote && !streamError && flushed;
        if (!wrote) {
            // The still-open handle identifies only the file we created. Never
            // remove a pathname after releasing this protection.
            FILE_DISPOSITION_INFO disposition{TRUE};
            SetFileInformationByHandle(writingHandle, FileDispositionInfo, &disposition, sizeof(disposition));
        }
        const bool closed = std::fclose(stream.release()) == 0;
        if (!wrote) {
            issuedCaptures_.erase(name);
            if (error.empty()) error = "Temporary capture write/flush failed.";
            return false;
        }
        // Closing the writing handle finalizes the file's modification time.
        // Reopen without write/delete sharing, then prove it is the same file
        // before publishing ownership metadata or cleaning up a close failure.
        FileGuard completed;
        if (!completed.open(path, true, error)) { issuedCaptures_.erase(name); return false; }
        BY_HANDLE_FILE_INFORMATION current{};
        if (!GetFileInformationByHandle(completed.handle, &current) ||
            current.dwVolumeSerialNumber != identity.dwVolumeSerialNumber ||
            current.nFileIndexHigh != identity.nFileIndexHigh || current.nFileIndexLow != identity.nFileIndexLow) {
            issuedCaptures_.erase(name);
            fail(error, "Temporary capture changed identity before registration; file preserved.");
            return false;
        }
        if (!closed || !writeCaptureRegistration(path, error)) {
            std::string ignored;
            completed.remove(path, ignored);
            issuedCaptures_.erase(name);
            if (error.empty()) error = "Temporary capture close/registration failed.";
            return false;
        }
        issuedCaptures_.erase(name);
        output = path;
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        if (!path.empty()) issuedCaptures_.erase(path.filename().string());
        fail(error, std::string("Cannot capture temporary image: ") + exception.what());
        return false;
    }
#else
    (void)writer;
    fail(error, "Protected temporary capture writing currently requires Windows.");
    return false;
#endif
}

void GarbageCollector::collectTemporaryCaptures() {
    if (temporaryRoot_.empty()) return;
    DirectoryGuard directory;
    if (!directory.check(temporaryRoot_, false, stats_.lastError)) { scan_ = {}; return; }
    if (scan_ == fs::directory_iterator{}) scan_ = fs::directory_iterator(temporaryRoot_);
    std::size_t inspected = 0;
    while (scan_ != fs::directory_iterator{} && inspected++ < kEntriesPerCollection) {
        const auto metadata = scan_->path();
        ++scan_;
        const auto name = metadata.filename().string();
        constexpr std::size_t suffix = 8; // .gc.json
        if (name.size() < suffix || name.compare(name.size() - suffix, suffix, ".gc.json") != 0) continue;
        const auto captureFileName = name.substr(0, name.size() - suffix);
        if (!captureName(captureFileName)) continue;
        std::string error;
        FileGuard registration;
        if (!registration.open(metadata, true, error)) { stats_.lastError = error; continue; }
        Json json;
        if (!readJson(metadata, json, error, registration)) { stats_.lastError = error; continue; }
        if (json.size() != 4 || !json.contains("format") || !json["format"].is_string() ||
            json["format"] != "RAEngine.TemporaryCapture" || !json.contains("version") || json["version"] != 1 ||
            !json.contains("bytes") || !json["bytes"].is_number_unsigned() ||
            !json.contains("modifiedTicks") || !json["modifiedTicks"].is_string()) continue;
        const auto capture = temporaryRoot_ / captureFileName;
        std::error_code statusError;
        const auto captureStatus = fs::symlink_status(capture, statusError);
        if (statusError == std::errc::no_such_file_or_directory || (!statusError && !fs::exists(captureStatus))) {
            // A prior round may have removed the capture but failed to remove
            // its registration. Retry only this strictly validated sidecar.
            if (!registration.remove(metadata, error)) stats_.lastError = error;
            continue;
        }
        FileGuard file;
        if (!file.open(capture, true, error)) { stats_.lastError = error; continue; }
        const auto bytes = fs::file_size(capture);
        const auto modified = fs::last_write_time(capture);
        if (bytes == 0 || bytes != json["bytes"].get<std::uintmax_t>() ||
            std::to_string(modified.time_since_epoch().count()) != json["modifiedTicks"].get<std::string>()) continue;
        const double age = std::chrono::duration<double>(fs::file_time_type::clock::now().time_since_epoch()).count() -
            std::chrono::duration<double>(modified.time_since_epoch()).count();
        if (age < settings_.temporaryCaptureMaxAgeSeconds) continue;
        if (!file.remove(capture, error)) { stats_.lastError = error; continue; }
        ++stats_.temporaryFilesRemoved;
        stats_.temporaryBytesRemoved += bytes;
        if (!registration.remove(metadata, error)) stats_.lastError = error;
    }
}

} // namespace rae
