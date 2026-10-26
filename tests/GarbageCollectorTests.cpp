#include <RestArtEngine/Core/GarbageCollector.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winioctl.h>
#endif

namespace {
namespace fs = std::filesystem;

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void write(const fs::path& path, const std::string& data = "BMtemporary test capture") {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << data;
    stream.close();
    check(!stream.fail(), "Cannot write test fixture");
}

std::string read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void expire(const fs::path& path) {
    fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::hours(2));
}

struct Fixture {
    fs::path root;
    Fixture() {
        const auto parent = fs::temp_directory_path();
        std::random_device random;
        for (int i = 0; i < 16; ++i) {
            const auto candidate = parent / ("rae_gc_tests_" + std::to_string(random()) + "_" + std::to_string(random()));
            if (fs::create_directory(candidate)) { root = fs::canonical(candidate); break; }
        }
        check(!root.empty(), "Cannot create isolated test directory");
    }
    ~Fixture() {
        // root is the exact exclusively-created fixture; never a user directory.
        std::error_code ec;
        for (const auto* name : {"directory-link", "junction", "root-swap"}) fs::remove(root / name, ec);
        if (!root.empty() && root.filename().string().find("rae_gc_tests_") == 0 &&
            !fs::is_symlink(fs::symlink_status(root, ec))) fs::remove_all(root, ec);
    }
};

void timerAndSettings(const fs::path& root) {
    rae::GarbageCollector gc;
    std::string error;
    int calls = 0;
    double idle = -1;
    const auto collect = [&](double value) {
        ++calls;
        idle = value;
        return rae::GarbageCollectionMemoryStats{10, 20, 3};
    };
    check(!gc.update(59, collect) && calls == 0, "Premature periodic collection");
    check(gc.update(1, collect) && calls == 1 && idle == 60, "Periodic collection did not run exactly once");
    check(!gc.update(0, collect), "Collected again without elapsed time");
    check(gc.stats().collections == 1 && gc.stats().memory.cpuBytesReleased == 10, "Collection statistics incorrect");
    check(gc.update(600, collect) && calls == 2, "Long frame must run only one collection");
    check(!gc.update(0, collect), "Long frame caused repeated catch-up collections");

    auto settings = gc.settings();
    check(!gc.update(10, collect), "Unexpected early collection");
    settings.intervalSeconds = 5;
    settings.memoryIdleSeconds = 7;
    check(gc.setSettings(settings, error), "Valid settings rejected");
    check(gc.update(0, collect) && idle == 7, "Changed interval/idle not applied on next update");
    settings.enabled = false;
    check(gc.setSettings(settings, error), "Disable settings rejected");
    check(!gc.update(500, collect), "Disabled automatic collector ran");
    gc.requestCollection();
    gc.requestCollection();
    const int before = calls;
    check(gc.update(0, collect) && calls == before + 1, "Manual requests not coalesced or disabled manual collection failed");
    check(!gc.update(0, collect), "Manual request remained active");
    check(gc.stats().memory.cpuBytesReleased == 10, "Recent memory counters incorrectly accumulated");

    auto invalid = settings;
    invalid.intervalSeconds = std::numeric_limits<double>::quiet_NaN();
    check(!gc.setSettings(invalid, error) && gc.settings().intervalSeconds == 5, "NaN accepted or settings modified on failure");
    invalid = settings;
    invalid.memoryIdleSeconds = std::numeric_limits<double>::infinity();
    check(!gc.setSettings(invalid, error), "Infinite idle accepted");
    invalid = settings;
    invalid.temporaryCaptureMaxAgeSeconds = 0;
    check(!gc.setSettings(invalid, error), "Zero capture age accepted");
    invalid = settings;
    invalid.intervalSeconds = 86401;
    check(!gc.setSettings(invalid, error), "Excessive interval accepted");
    invalid = settings;
    invalid.memoryIdleSeconds = -1;
    check(!gc.setSettings(invalid, error), "Negative idle accepted");
    invalid = settings;
    invalid.memoryIdleSeconds = 604801;
    check(!gc.setSettings(invalid, error), "Excessive idle accepted");
    invalid = settings;
    invalid.temporaryCaptureMaxAgeSeconds = 31536001;
    check(!gc.setSettings(invalid, error), "Excessive capture age accepted");
    settings.enabled = true;
    check(gc.setSettings(settings, error), "Enable settings rejected");
    check(!gc.update(std::numeric_limits<double>::infinity(), collect), "Infinite delta triggered GC");
    check(!gc.update(std::numeric_limits<double>::quiet_NaN(), collect), "NaN delta triggered GC");
    check(!gc.update(-100, collect), "Negative delta triggered GC");
    check(gc.update(5, collect), "Invalid delta poisoned clock");

    const auto config = root / "settings" / "gc.json";
    const bool savedSettings = gc.saveSettings(config, error);
    if (!savedSettings) throw std::runtime_error("Cannot atomically save settings: " + error);
    rae::GarbageCollector loaded;
    check(loaded.loadSettings(config, error), "Cannot load saved settings");
    check(loaded.settings().intervalSeconds == 5 && loaded.settings().memoryIdleSeconds == 7,
        "Settings round trip changed values");
    check(loaded.loadSettings(root / "missing.json", error), "Missing config should succeed");
    write(config, "{bad json");
    check(!loaded.loadSettings(config, error) && loaded.settings().intervalSeconds == 5, "Malformed config changed settings");
    write(config, "{\"format\":\"RAEngine.GarbageCollection\",\"version\":1,\"enabled\":true}");
    check(!loaded.loadSettings(config, error) && loaded.settings().intervalSeconds == 5, "Incomplete config changed settings");
    check(gc.saveSettings(config, error), "Cannot replace damaged settings");
#ifdef _WIN32
    const auto contents = read(config);
    check(SetFileAttributesW(config.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "Cannot make config read-only");
    const bool saved = gc.saveSettings(config, error);
    SetFileAttributesW(config.c_str(), FILE_ATTRIBUTE_NORMAL);
    check(!saved && read(config) == contents, "Failed atomic replacement damaged old settings");
    for (const auto& entry : fs::directory_iterator(config.parent_path()))
        check(entry.path() == config, "Failed atomic settings replacement left a temporary file");
#endif
    gc.requestCollection();
    check(gc.update(0, [](double) -> rae::GarbageCollectionMemoryStats { throw std::runtime_error("test"); }),
        "Callback exception escaped update");
    check(!gc.stats().lastError.empty() && gc.stats().memory.cpuBytesReleased == 0, "Failure stats not reset/reported");
}

fs::path registeredCapture(rae::GarbageCollector& gc, std::string& error, bool old = true) {
    const auto path = gc.temporaryCapturePath(error);
    check(!path.empty(), "Cannot issue temporary capture name");
    write(path);
    if (old) expire(path);
    check(gc.registerTemporaryCapture(path, error), "Cannot register completed capture");
    return path;
}

void captures(const fs::path& root) {
    std::string error;
    rae::GarbageCollector gc;
    check(gc.temporaryCapturePath(error).empty(), "Uninitialized capture path succeeded");
    check(!gc.initializeTemporaryCaptures({}, error), "Empty root accepted");
    check(!gc.initializeTemporaryCaptures(root.root_path(), error), "Filesystem root accepted");
    const auto temporary = root / "captures";
    check(gc.initializeTemporaryCaptures(temporary, error), "Cannot initialize capture directory");
    const auto expired = registeredCapture(gc, error);
    const auto recent = registeredCapture(gc, error, false);
    const auto modified = registeredCapture(gc, error);
    write(modified, "BMchanged after registration");
    expire(modified);
    const auto sameSizeModified = registeredCapture(gc, error);
    fs::last_write_time(sameSizeModified, fs::file_time_type::clock::now() - std::chrono::hours(3));
    const auto unregistered = gc.temporaryCapturePath(error);
    write(unregistered);
    expire(unregistered);
    const auto registrationCollision = gc.temporaryCapturePath(error);
    write(registrationCollision);
    auto collisionMetadata = registrationCollision;
    collisionMetadata += ".gc.json";
    write(collisionMetadata, "unknown preexisting registration must remain unchanged");
    const auto collisionBefore = read(collisionMetadata);
    check(!gc.registerTemporaryCapture(registrationCollision, error), "Registration overwrote an existing sidecar");
    check(read(collisionMetadata) == collisionBefore && fs::exists(registrationCollision),
        "No-replace registration failure changed existing files");
    for (const auto& entry : fs::directory_iterator(temporary))
        check(entry.path().extension() != ".tmp", "Failed no-replace registration left a temporary file");
    const auto empty = gc.temporaryCapturePath(error);
    write(empty, "");
    check(!gc.registerTemporaryCapture(empty, error), "Empty unfinished capture registered");
    const auto user = temporary / "capture_001.bmp";
    write(user);
    expire(user);
    check(!gc.registerTemporaryCapture(user, error), "User capture was accepted for registration");
    const auto arbitrary = temporary / "rae_capture_00000000000000000000000000000000.bmp";
    write(arbitrary);
    expire(arbitrary);
    check(!gc.registerTemporaryCapture(arbitrary, error), "Unissued canonical capture was accepted");
    auto arbitraryMetadata = arbitrary;
    arbitraryMetadata += ".gc.json";
    write(arbitraryMetadata, "{\"format\":\"unknown\",\"path\":\"../outside.bmp\"}");
    const auto outside = root / "outside.bmp";
    write(outside);
    check(!gc.registerTemporaryCapture(outside, error), "Outside capture accepted");
    check(!gc.registerTemporaryCapture(temporary / ".." / "outside.bmp", error), "Parent traversal accepted");
    const auto nested = temporary / "nested";
    fs::create_directory(nested);
    write(nested / "capture.bmp");

    rae::GarbageCollector nextSession;
    check(nextSession.initializeTemporaryCaptures(temporary, error), "Cannot reopen temporary directory");
    nextSession.requestCollection();
    check(nextSession.update(0, {}), "Manual disk collection failed");
    check(!fs::exists(expired), "Cross-session expired registered capture was not deleted");
    check(nextSession.stats().temporaryFilesRemoved == 1 && nextSession.stats().temporaryBytesRemoved > 0,
        "Removed file statistics incorrect");
    check(fs::exists(recent) && fs::exists(modified) && fs::exists(sameSizeModified) && fs::exists(unregistered) &&
        fs::exists(empty) && fs::exists(user) && fs::exists(arbitrary) && fs::exists(arbitraryMetadata) && fs::exists(outside) &&
        fs::exists(nested / "capture.bmp"), "Protected file was removed");
    nextSession.requestCollection();
    nextSession.update(0, {});
    check(nextSession.stats().temporaryFilesRemoved == 0, "File statistics accumulated across collections");

#ifdef _WIN32
    const auto busy = registeredCapture(gc, error);
    HANDLE writer = CreateFileW(busy.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(writer != INVALID_HANDLE_VALUE, "Cannot open writer fixture");
    gc.requestCollection();
    gc.update(0, {});
    const bool preserved = fs::exists(busy);
    CloseHandle(writer);
    check(preserved, "Active writer's file was removed");
    gc.requestCollection();
    gc.update(0, {});
    check(!fs::exists(busy), "Busy file was not retried after writer closed");
#endif

    const auto bounded = root / "bounded";
    rae::GarbageCollector boundedGc;
    check(boundedGc.initializeTemporaryCaptures(bounded, error), "Cannot initialize bounded scan");
    for (int i = 0; i < 520; ++i) write(bounded / ("unknown_" + std::to_string(i)), "user data");
    const auto afterLimit = registeredCapture(boundedGc, error);
    for (int i = 0; i < 3; ++i) { boundedGc.requestCollection(); boundedGc.update(0, {}); }
    check(!fs::exists(afterLimit), "Bounded scan failed to progress beyond first batch");
}

#ifdef _WIN32
void protectedCaptureWriter(const fs::path& root) {
    const auto capturesRoot = root / "protected-writer";
    const auto sentinel = root / "writer-sentinel.bmp";
    write(sentinel, "user sentinel must remain unchanged");
    const auto sentinelBefore = read(sentinel);
    rae::GarbageCollector gc;
    std::string error;
    check(gc.initializeTemporaryCaptures(capturesRoot, error), "Cannot initialize protected writer");
    fs::path output;
    bool callbackRan = false;
    const bool captured = gc.captureTemporary([&](std::FILE* stream, std::string&) {
        callbackRan = true;
        fs::path active;
        for (const auto& entry : fs::directory_iterator(capturesRoot)) {
            if (entry.path().extension() == ".bmp") active = entry.path();
        }
        check(!active.empty(), "Protected writer has no created capture");
        const HANDLE directoryWriter = CreateFileW(capturesRoot.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (directoryWriter != INVALID_HANDLE_VALUE) CloseHandle(directoryWriter);
        check(directoryWriter == INVALID_HANDLE_VALUE, "Directory allowed a write handle during capture callback");
        std::error_code ec;
        fs::rename(capturesRoot, root / "renamed-protected-writer", ec);
        check(static_cast<bool>(ec), "Capture directory could be renamed during writer callback");
        fs::rename(active, capturesRoot / "replaced.bmp", ec);
        check(static_cast<bool>(ec), "Capture file could be renamed during writer callback");
        const bool removed = fs::remove(active, ec);
        check(!removed && static_cast<bool>(ec), "Capture file could be deleted during writer callback");
        fs::create_symlink(sentinel, active, ec);
        check(static_cast<bool>(ec), "Capture file could be replaced by a symlink during writer callback");
        const HANDLE truncatingWriter = CreateFileW(active.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (truncatingWriter != INVALID_HANDLE_VALUE) CloseHandle(truncatingWriter);
        check(truncatingWriter == INVALID_HANDLE_VALUE, "Second writer could truncate active capture");
        const char bytes[] = "BMprotected stream capture";
        return std::fwrite(bytes, 1, sizeof(bytes) - 1, stream) == sizeof(bytes) - 1;
    }, output, error);
    if (!captured) throw std::runtime_error("Protected capture callback failed: " + error);
    check(callbackRan && !output.empty() && fs::exists(output), "Protected capture did not return completed file");
    auto metadata = output;
    metadata += ".gc.json";
    check(fs::exists(metadata), "Protected capture was not registered");
    check(read(sentinel) == sentinelBefore, "User sentinel changed during protected capture");
    fs::remove(output);
    fs::remove(metadata);

    check(!gc.captureTemporary([](std::FILE* stream, std::string& writerError) {
        const char bytes[] = "partial capture";
        std::fwrite(bytes, 1, sizeof(bytes) - 1, stream);
        writerError = "deliberate writer failure";
        return false;
    }, output, error), "Failed writer unexpectedly succeeded");
    check(output.empty() && error == "deliberate writer failure", "Writer failure output/error contract broken");
    check(fs::is_empty(capturesRoot), "Failed writer left a capture or registration");
    check(!gc.captureTemporary([](std::FILE*, std::string&) -> bool {
        throw std::runtime_error("deliberate callback exception");
    }, output, error), "Throwing writer unexpectedly succeeded");
    check(fs::is_empty(capturesRoot), "Throwing writer left a capture or registration");
    check(!gc.captureTemporary([](std::FILE*, std::string&) { return true; }, output, error),
        "Empty writer result was registered");
    check(fs::is_empty(capturesRoot), "Empty writer result was not cleaned up");
    check(read(sentinel) == sentinelBefore, "Failure cleanup changed user sentinel");
}
#endif

#ifdef _WIN32
bool createJunction(const fs::path& link, const fs::path& target) {
    if (!fs::create_directory(link)) return false;
    HANDLE directory = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE) { fs::remove(link); return false; }
    struct JunctionData {
        DWORD tag;
        WORD dataLength;
        WORD reserved;
        WORD substituteOffset;
        WORD substituteLength;
        WORD printOffset;
        WORD printLength;
        wchar_t paths[2048];
    } data{};
    const auto substitute = std::wstring(L"\\??\\") + target.native();
    const auto print = target.native();
    if (substitute.size() + print.size() + 2 > 2048) { CloseHandle(directory); fs::remove(link); return false; }
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substituteLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.printOffset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    data.printLength = static_cast<WORD>(print.size() * sizeof(wchar_t));
    std::copy(substitute.begin(), substitute.end(), data.paths);
    std::copy(print.begin(), print.end(), data.paths + substitute.size() + 1);
    data.dataLength = static_cast<WORD>(8 + (substitute.size() + print.size() + 2) * sizeof(wchar_t));
    DWORD returned = 0;
    const bool ok = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, &data,
        static_cast<DWORD>(data.dataLength + 8), nullptr, 0, &returned, nullptr) != FALSE;
    CloseHandle(directory);
    if (!ok) fs::remove(link);
    return ok;
}
#endif

void links(const fs::path& root) {
    std::string error;
    const auto target = root / "link-target";
    fs::create_directory(target);
    write(target / "user.bmp");
    const auto link = root / "directory-link";
    std::error_code ec;
    fs::create_directory_symlink(target, link, ec);
    if (!ec) {
        rae::GarbageCollector gc;
        check(!gc.initializeTemporaryCaptures(link, error), "Symlink capture root accepted");
        check(!gc.initializeTemporaryCaptures(link / "nested", error), "Symlink ancestor accepted");
        fs::remove(link);
    } else std::cout << "SKIP directory symlink creation: " << ec.message() << '\n';

    rae::GarbageCollector gc;
    check(gc.initializeTemporaryCaptures(root / "linked-files", error), "Cannot initialize linked-file test");
    auto capture = registeredCapture(gc, error);
    const auto external = root / "external.bmp";
    fs::copy_file(capture, external);
    fs::last_write_time(external, fs::last_write_time(capture));
    fs::remove(capture);
    ec.clear();
    fs::create_symlink(external, capture, ec);
    if (!ec) {
        gc.requestCollection();
        gc.update(0, {});
        check(fs::exists(capture) && fs::exists(external), "Linked capture or its target was deleted");
        fs::remove(capture);
    } else std::cout << "SKIP file symlink creation: " << ec.message() << '\n';

    capture = registeredCapture(gc, error);
    auto metadata = capture;
    metadata += ".gc.json";
    const auto externalMetadata = root / "external-metadata.json";
    fs::copy_file(metadata, externalMetadata);
    fs::remove(metadata);
    ec.clear();
    fs::create_symlink(externalMetadata, metadata, ec);
    if (!ec) {
        gc.requestCollection();
        gc.update(0, {});
        check(fs::exists(capture) && fs::exists(metadata) && fs::exists(externalMetadata), "Linked registration or capture was deleted");
        fs::remove(metadata);
    } else std::cout << "SKIP registration symlink creation: " << ec.message() << '\n';

    capture = registeredCapture(gc, error);
    const auto hardlink = root / "hardlink.bmp";
    fs::create_hard_link(capture, hardlink, ec);
    if (!ec) {
        gc.requestCollection();
        gc.update(0, {});
        check(fs::exists(capture) && fs::exists(hardlink), "Hard-linked capture was deleted");
    } else std::cout << "SKIP hard link creation: " << ec.message() << '\n';

#ifdef _WIN32
    const auto junction = root / "junction";
    check(createJunction(junction, target), "Cannot create junction safety fixture");
    rae::GarbageCollector guarded;
    check(!guarded.initializeTemporaryCaptures(junction, error), "Junction capture root accepted");
    check(!guarded.initializeTemporaryCaptures(junction / "nested", error), "Junction ancestor accepted");
    check(fs::exists(target / "user.bmp") && !fs::exists(target / "nested"), "Junction target was changed");
    fs::remove(junction);

    const auto swap = root / "root-swap";
    check(guarded.initializeTemporaryCaptures(swap, error), "Cannot initialize root-swap fixture");
    fs::remove(swap);
    check(createJunction(swap, target), "Cannot replace root with junction fixture");
    check(guarded.temporaryCapturePath(error).empty(), "Changed root junction allowed a capture allocation");
    fs::path protectedOutput;
    check(!guarded.captureTemporary([](std::FILE* stream, std::string&) {
        return std::fwrite("bad", 1, 3, stream) == 3;
    }, protectedOutput, error), "Changed root junction allowed protected writing");
    guarded.requestCollection();
    guarded.update(0, {});
    check(!guarded.stats().lastError.empty() && fs::exists(target / "user.bmp"), "Changed root junction not rejected during GC");
    fs::remove(swap);
#endif
}
} // namespace

int main() {
    try {
        Fixture fixture;
        timerAndSettings(fixture.root);
        captures(fixture.root);
#ifdef _WIN32
        protectedCaptureWriter(fixture.root);
#endif
        links(fixture.root);
        std::cout << "GarbageCollectorTests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "GarbageCollectorTests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
