#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <RestArtEngine/Renderer/SceneRenderer.h>
#include <RestArtEngine/ImGui/ImGuiLayer.h>
#include <RestArtEngine/ImGui/EditorTheme.h>
#include <RestArtEngine/ImGui/WindowChrome.h>
#include <RestArtEngine/Editor/SceneEditor.h>
#include <RestArtEngine/Editor/LoadingScreen.h>
#include <RestArtEngine/Core/GarbageCollector.h>
#include "EditorSmoke.h"
#include "GarbageCollectionSmoke.h"
#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

namespace {
using namespace rae::renderer;
struct WindowState {
    rae::imgui::WindowChrome chrome;
    unsigned width = 1440, height = 900;
    bool resized = false, minimized = false, closeRequested = false;
    // Only the window that owns the main loop may post WM_QUIT when destroyed.
    // The project manager and the editor share `windowProc`; without this gate,
    // destroying the project manager can leave a thread-wide WM_QUIT in the
    // queue that terminates the editor loop before its first frame.
    bool quitOnDestroy = false;
    // F11 borderless fullscreen restores this captured state on toggle back.
    bool fullscreen = false;
    bool uiBeforeFullscreen = true;
    LONG_PTR windowedStyle = 0;
    WINDOWPLACEMENT windowedPlacement{};
};

// RestArtEngine.rc embeds assets/icons/RAEngine.ico (generated from the project logo
// assets/icons/RAEngine.png) as resource 101. A missing resource leaves the
// default Windows icon.
constexpr int kEngineIconResource = 101;
HICON loadEngineIcon(int width, int height) {
    return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kEngineIconResource),
        IMAGE_ICON, width, height, LR_DEFAULTCOLOR));
}

// Presents one loading frame: clear, draw the overlay and flip the swap chain.
// Called between the staged renderer startup steps so the wait is never a blank
// window.
void presentLoading(SceneRenderer& renderer, rae::imgui::ImGuiLayer& ui,
                    const rae::editor::LoadingScreen& screen, float progress, const char* text) {
    std::string ignored;
    const float clear[4] = {0.02f, 0.025f, 0.035f, 1.0f};
    if (!renderer.clear(clear, ignored)) return;
    ui.beginFrame(1.0f / 60.0f);
    screen.draw(progress, text);
    ui.endFrame();
    renderer.present(false, ignored);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        state = static_cast<WindowState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    std::intptr_t chromeResult = 0;
    if (state && state->chrome.handleMessage(window, message, wp, lp, chromeResult)) return chromeResult;
    const bool uiHandled = rae::imgui::ImGuiLayer::handleMessage(window, message, wp, lp);
    switch (message) {
    case WM_SIZE:
        if (state) {
            state->minimized = wp == SIZE_MINIMIZED;
            if (LOWORD(lp) && HIWORD(lp)) {
                state->width = LOWORD(lp); state->height = HIWORD(lp); state->resized = true;
            }
        }
        return 0;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {900, 600}; return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU && lp != VK_SPACE) return 0;
        break;
    case WM_CLOSE:
        if (state) state->closeRequested = true;
        return 0;
    case WM_DESTROY:
        if (!state || state->quitOnDestroy) PostQuitMessage(0);
        return 0;
    }
    if (uiHandled) return 1;
    return DefWindowProcW(window, message, wp, lp);
}

// F11 toggles a borderless fullscreen covering the monitor that owns the window
// (the same immersive behaviour UE's editor uses). The previous style and
// placement are restored on the way back; the swap chain follows through the
// regular WM_SIZE -> renderer.resize path.
void toggleFullscreen(HWND window, WindowState& state) {
    if (!window) return;
    if (!state.fullscreen) {
        state.windowedPlacement.length = sizeof(WINDOWPLACEMENT);
        if (!GetWindowPlacement(window, &state.windowedPlacement)) return;
        state.windowedStyle = GetWindowLongPtrW(window, GWL_STYLE);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(MONITORINFO);
        if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return;
        state.fullscreen = true;
        state.chrome.setFullscreen(true);
        SetWindowLongPtrW(window, GWL_STYLE, (state.windowedStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
            monitor.rcMonitor.right - monitor.rcMonitor.left,
            monitor.rcMonitor.bottom - monitor.rcMonitor.top,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        state.fullscreen = false;
        state.chrome.setFullscreen(false);
        SetWindowLongPtrW(window, GWL_STYLE, state.windowedStyle);
        SetWindowPlacement(window, &state.windowedPlacement);
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
}

RECT primaryMonitorRect() {
    POINT origin{0, 0};
    const HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(MONITORINFO)};
    if (monitor && GetMonitorInfoW(monitor, &info)) return info.rcMonitor;
    return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

// Centres a client-size rectangle on the monitor. `frame` adds the standard
// resizable border so a WS_OVERLAPPEDWINDOW client area matches the request.
RECT centredWindowRect(const RECT& monitor, int clientWidth, int clientHeight, bool frame) {
    RECT rect{0, 0, clientWidth, clientHeight};
    if (frame) AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int x = monitor.left + ((monitor.right - monitor.left) - width) / 2;
    const int y = monitor.top + ((monitor.bottom - monitor.top) - height) / 2;
    return RECT{x, y, x + width, y + height};
}

struct WindowOwner {
    HWND handle{};
    ~WindowOwner() { if (IsWindow(handle)) DestroyWindow(handle); }
};

std::string utf8Path(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::wstring windowTitle(const std::string& projectName) {
    const int count = MultiByteToWideChar(CP_UTF8, 0, projectName.data(), static_cast<int>(projectName.size()), nullptr, 0);
    std::wstring name(static_cast<std::size_t>(count), L'\0');
    if (count) MultiByteToWideChar(CP_UTF8, 0, projectName.data(), static_cast<int>(projectName.size()), name.data(), count);
    return L"RestArtEngine | " + name;
}

std::filesystem::path findEngineRoot() {
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    const std::filesystem::path starts[] = {
        std::filesystem::current_path(), std::filesystem::path(executable).parent_path()};
    for (const auto& start : starts) {
        for (auto root = start; !root.empty(); root = root.parent_path()) {
            if (std::filesystem::is_regular_file(root / "Shaders/Private/Scene.hlsl"))
                return std::filesystem::weakly_canonical(root);
            if (root.parent_path() == root) break;
        }
    }
    return {};
}

bool browseForFolder(std::filesystem::path& result, HWND owner) {
    BROWSEINFOW info{};
    wchar_t displayName[MAX_PATH]{};
    info.hwndOwner = owner;
    info.pszDisplayName = displayName;
    info.lpszTitle = L"Choose a folder";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE list = SHBrowseForFolderW(&info);
    if (!list) return false;
    wchar_t path[MAX_PATH]{};
    const bool ok = SHGetPathFromIDListW(list, path) != FALSE;
    CoTaskMemFree(list);
    if (!ok) return false;
    result = std::filesystem::path(path);
    return true;
}

bool browseForProject(std::filesystem::path& result, HWND owner) {
    wchar_t fileName[32768]{};
    OPENFILENAMEW info{};
    info.lStructSize = sizeof(info);
    info.hwndOwner = owner;
    info.lpstrFile = fileName;
    info.nMaxFile = static_cast<DWORD>(std::size(fileName));
    info.lpstrFilter = L"RestArtEngine Project (*.raproject)\0*.raproject\0All files (*.*)\0*.*\0\0";
    info.lpstrTitle = L"Open RestArtEngine Project";
    info.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&info)) return false;
    result = std::filesystem::path(fileName);
    return true;
}

void saveUserCapture(SceneRenderer& renderer, HWND owner, std::filesystem::path& lastFolder) {
    wchar_t fileName[32768]{};
    const auto suggested = L"capture_" + std::to_wstring(GetTickCount64()) + L".bmp";
    std::copy(suggested.begin(), suggested.end(), fileName);
    wchar_t pictures[MAX_PATH]{};
    if (lastFolder.empty() && SUCCEEDED(SHGetFolderPathW(owner, CSIDL_MYPICTURES, nullptr, 0, pictures)))
        lastFolder = pictures;
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = owner;
    dialog.lpstrTitle = L"Save capture";
    dialog.lpstrFilter = L"Bitmap image (*.bmp)\0*.bmp\0\0";
    dialog.lpstrDefExt = L"bmp";
    dialog.lpstrFile = fileName;
    dialog.nMaxFile = static_cast<DWORD>(std::size(fileName));
    dialog.lpstrInitialDir = lastFolder.empty() ? nullptr : lastFolder.c_str();
    dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog)) {
        const DWORD code = CommDlgExtendedError();
        if (code) {
            const auto message = L"Could not open the save dialog (error " + std::to_wstring(code) + L").";
            MessageBoxW(owner, message.c_str(), L"Capture", MB_OK | MB_ICONERROR);
        }
        return; // Cancel never falls back to an automatic output path.
    }
    const std::filesystem::path file(fileName);
    if (_wcsicmp(file.extension().c_str(), L".bmp") != 0) {
        MessageBoxW(owner, L"Capture supports BMP images. Please choose a filename ending in .bmp.",
            L"Capture", MB_OK | MB_ICONERROR);
        return;
    }
    lastFolder = file.parent_path();
    std::string error;
    if (!renderer.saveScreenshot(file.c_str(), error)) {
        // A failed user export must not close the editor or discard scene edits.
        MessageBoxA(owner, error.c_str(), "Could not save capture", MB_OK | MB_ICONERROR);
        return;
    }
    std::wcout << L"Captured " << file.native() << L'\n';
}

bool createProjectFromTemplate(const std::filesystem::path& engineRoot,
    const std::string& name, const std::filesystem::path& parent, std::filesystem::path& result,
    std::string& error) {
    try {
        if (name.empty()) throw std::runtime_error("Project name cannot be empty.");
        for (const auto character : name)
            if (character == '/' || character == '\\' || character == ':' || character == '"')
                throw std::runtime_error("Project name contains an invalid path character.");
        const auto templateRoot = engineRoot / "Projects/Sandbox";
        if (!std::filesystem::is_regular_file(templateRoot / "Sandbox.raproject"))
            throw std::runtime_error("The engine does not contain Projects/Sandbox/Sandbox.raproject.");
        const auto projectRoot = std::filesystem::weakly_canonical(parent / name);
        if (std::filesystem::exists(projectRoot))
            throw std::runtime_error("The target project folder already exists: " + utf8Path(projectRoot));
        std::filesystem::copy(templateRoot, projectRoot, std::filesystem::copy_options::recursive);
        const auto manifest = projectRoot / "Sandbox.raproject";
        std::ifstream input(manifest);
        if (!input) throw std::runtime_error("Cannot read the project template manifest.");
        std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const auto replaceString = [&json](const char* key, const std::string& value) {
            const auto keyPosition = json.find(std::string("\"") + key + "\"");
            if (keyPosition == std::string::npos) throw std::runtime_error(std::string("Project template is missing ") + key + ".");
            const auto colon = json.find(':', keyPosition);
            const auto firstQuote = json.find('"', colon + 1);
            if (colon == std::string::npos || firstQuote == std::string::npos) throw std::runtime_error(std::string("Invalid project template field: ") + key + ".");
            auto endQuote = firstQuote + 1;
            while (endQuote < json.size()) {
                if (json[endQuote] == '"' && json[endQuote - 1] != '\\') break;
                ++endQuote;
            }
            if (endQuote >= json.size()) throw std::runtime_error(std::string("Invalid project template field: ") + key + ".");
            json.replace(firstQuote + 1, endQuote - firstQuote - 1, value);
        };
        replaceString("name", name);
        // Project manifests resolve engineRoot from the new project's folder.
        // Compute that path in the same direction so projects created beneath
        // the engine (the default project manager location) still link to the engine.
        replaceString("engineRoot", utf8Path(std::filesystem::relative(projectRoot, engineRoot)));
        const auto output = projectRoot / (name + ".raproject");
        std::ofstream stream(output);
        if (!stream) throw std::runtime_error("Cannot write the new project manifest.");
        stream << json;
        std::filesystem::remove(manifest);
        result = output;
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

struct ProjectExample {
    std::filesystem::path manifest;
    std::string name;
};

std::vector<ProjectExample> findExamples(const std::filesystem::path& engineRoot) {
    std::vector<ProjectExample> examples;
    const auto root = engineRoot / "Projects";
    if (!std::filesystem::is_directory(root)) return examples;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".raproject") continue;
        rae::project::ProjectPaths project;
        std::string error;
        if (rae::project::loadProject(entry.path(), project, error))
            examples.push_back({entry.path(), project.name});
    }
    return examples;
}

rae::GarbageCollectionMemoryStats collectMemory(SceneRenderer& renderer,
    rae::imgui::ImGuiLayer& ui, double idleSeconds) {
    auto result = renderer.collectGarbage(idleSeconds);
    const auto uiResult = ui.collectGarbage(idleSeconds);
    result.cpuBytesReleased += uiResult.cpuBytesReleased;
    result.gpuBytesReleasedEstimate += uiResult.gpuBytesReleasedEstimate;
    result.objectsReleased += uiResult.objectsReleased;
    return result;
}

void updateGarbageCollection(rae::GarbageCollector& gc, SceneRenderer& renderer,
    rae::imgui::ImGuiLayer& ui, std::chrono::steady_clock::time_point& previous) {
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - previous).count();
    previous = now;
    if (gc.update(elapsed, [&](double idle) { return collectMemory(renderer, ui, idle); })) {
        const auto& stats = gc.stats();
        std::cout << "GC: CPU bytes " << stats.memory.cpuBytesReleased
            << ", GPU bytes (estimate) " << stats.memory.gpuBytesReleasedEstimate
            << ", temporary captures " << stats.temporaryFilesRemoved << '\n';
        if (!stats.lastError.empty()) std::cerr << "GC: " << stats.lastError << '\n';
    }
}

bool runProjectManager(const std::filesystem::path& engineRoot, rae::project::ProjectPaths& selected,
    rae::GarbageCollector& gc, std::string& error) {
    if (engineRoot.empty()) { error = "Cannot locate the RestArtEngine root. Start the project manager inside an engine checkout."; return false; }
    WindowState windowState;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(WNDCLASSEXW)};
    wc.style = CS_CLASSDC; wc.lpfnWndProc = windowProc; wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = L"RAEngineProjectManager";
    wc.hIcon = loadEngineIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    wc.hIconSm = loadEngineIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { error = "RegisterClassEx failed for project manager."; return false; }
    RECT bounds{0, 0, 1080, 720};
    // The launcher keeps its 1080x720 size but opens centred on the primary
    // monitor, matching the loading and editor window placement.
    const RECT launchMonitor = primaryMonitorRect();
    const RECT placed = centredWindowRect(launchMonitor, 1080, 720, false);
    WindowOwner window{CreateWindowW(wc.lpszClassName, L"RestArtEngine | Project Manager", WS_OVERLAPPEDWINDOW,
        placed.left, placed.top, placed.right - placed.left, placed.bottom - placed.top,
        nullptr, nullptr, instance, &windowState)};
    if (!window.handle) { error = "CreateWindow failed for project manager."; return false; }
    SceneRenderer renderer;
    renderer.setShaderRoot(engineRoot / "Shaders");
    renderer.setMaterialShaderPath(engineRoot / "Shaders/Materials/Standard.hlsli");
    if (!renderer.initialize(window.handle, windowState.width, windowState.height, error)) return false;
    rae::imgui::ImGuiLayer ui;
    if (!ui.initialize(window.handle, renderer.device(), renderer.context())) { error = "Dear ImGui initialization failed for project manager."; return false; }
    ui.setWindowChrome(&windowState.chrome);
    ShowWindow(window.handle, SW_SHOW);
    char projectName[256] = "MyProject";
    char parentText[1024]{};
    const auto defaultParent = utf8Path(engineRoot / "Projects");
    std::strncpy(parentText, defaultParent.c_str(), sizeof(parentText) - 1);
    std::string status;
    auto examples = findExamples(engineRoot);
    rae::project::EngineIdentity currentIdentity;
    if (!rae::project::loadEngineIdentity(engineRoot, currentIdentity, status)) {
        error = "Cannot load current engine identity: " + status;
        ui.shutdown();
        return false;
    }
    // Keep this running engine discoverable for version switching. Best-effort:
    // an unwritable registry never blocks opening a project.
    rae::project::EngineRegistry registry;
    if (rae::project::loadEngineRegistry(rae::project::defaultEngineRegistryPath(), registry, status)) {
        rae::project::registerEngineInRegistry(registry, currentIdentity.version, currentIdentity.root);
        rae::project::saveEngineRegistry(rae::project::defaultEngineRegistryPath(), registry, status);
    }
    std::optional<rae::project::ProjectPaths> mismatch;
    rae::project::EngineCompatibilityReport mismatchReport;
    bool running = true, accepted = false;
    auto previous = std::chrono::steady_clock::now();
    auto previousGc = previous;
    while (running) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (!running) break;
        if (windowState.resized && !windowState.minimized) {
            if (!renderer.resize(windowState.width, windowState.height, error)) {
                ui.shutdown();
                return false;
            }
            windowState.resized = false;
        }
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - previous).count(); previous = now;
        ui.beginFrame(std::clamp(dt, 0.0001f, 0.1f));
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->WorkPos);
        ImGui::SetNextWindowSize(ImGui::GetMainViewport()->WorkSize);
        ImGui::Begin("RestArtEngine Project Manager", nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
        ImGui::SetWindowFontScale(1.15f);
        ImGui::Text("RestArtEngine");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextWrapped("Choose an existing project, create a project from an engine example, or close this window.");
        ImGui::Separator();
        const auto selectCandidate = [&](rae::project::ProjectPaths candidate) {
            const auto report = rae::project::checkEngineCompatibility(candidate, currentIdentity);
            // Legacy v1 projects can still be opened, but route them through
            // the compatibility dialog so the project manager can persist an
            // engine link on first use.
            if (report.compatible() && report.state != rae::project::EngineCompatibility::LegacyProject) {
                selected = std::move(candidate);
                accepted = true;
                running = false;
            } else {
                mismatchReport = report;
                mismatch = std::move(candidate);
            }
        };
        const auto rebindAndOpen = [&](const rae::project::EngineIdentity& identity) {
            if (!mismatch) return;
            std::string updateError;
            if (!rae::project::updateProjectEngineBinding(mismatch->manifest, identity, updateError)) {
                status = "Cannot update project engine binding: " + updateError;
                return;
            }
            rae::project::registerEngineInRegistry(registry, identity.version, identity.root);
            rae::project::saveEngineRegistry(rae::project::defaultEngineRegistryPath(), registry, updateError);
            rae::project::ProjectPaths updated;
            if (!rae::project::loadProject(mismatch->manifest, updated, updateError)) {
                status = "Binding was updated, but project reload failed: " + updateError;
                return;
            }
            selected = std::move(updated);
            mismatch.reset();
            accepted = true;
            running = false;
            ImGui::CloseCurrentPopup();
        };
        // Open a project, recovering a missing or stale engine link through the
        // named association when possible. Failure produces a "cannot open"
        // diagnostic so a broken or read-only project is never silently ignored.
        const auto openProject = [&](const std::filesystem::path& path, rae::project::ProjectPaths& candidate) {
            std::string openError;
            if (rae::project::loadProject(path, candidate, openError)) return true;
            rae::project::ProjectManifestInfo info;
            if (!rae::project::loadProjectManifestInfo(path, info, openError)) {
                status = "Cannot open project: " + openError;
                return false;
            }
            if (info.engineAssociation.empty()) {
                status = "Cannot open project: " + openError + " (the project has no engine association to recover).";
                return false;
            }
            rae::project::EngineRegistryEntry entry;
            if (!rae::project::findEngineByAssociation(registry, info.engineAssociation, entry)) {
                status = "Cannot open project: no installed engine matches association '" + info.engineAssociation + "'.";
                return false;
            }
            rae::project::EngineIdentity recovered;
            if (!rae::project::loadEngineIdentity(entry.root, recovered, openError)) {
                status = "Cannot open project: the registered engine is unavailable: " + openError;
                return false;
            }
            if (!rae::project::updateProjectEngineBinding(info.manifest, recovered, openError)) {
                status = "Cannot open project: the engine link cannot be updated: " + openError;
                return false;
            }
            rae::project::registerEngineInRegistry(registry, recovered.version, recovered.root);
            rae::project::saveEngineRegistry(rae::project::defaultEngineRegistryPath(), registry, openError);
            if (!rae::project::loadProject(info.manifest, candidate, openError)) {
                status = "Cannot open project after recovery: " + openError;
                return false;
            }
            status = "Recovered project engine link to " + recovered.version + " (" + utf8Path(recovered.root) + ").";
            return true;
        };
        if (ImGui::Button("Open Existing Project", ImVec2(240, 34))) {
            std::filesystem::path path;
            if (browseForProject(path, window.handle)) {
                rae::project::ProjectPaths candidate;
                if (openProject(path, candidate)) selectCandidate(std::move(candidate));
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(130, 34))) running = false;
        ImGui::Spacing();
        ImGui::Text("Create a new project");
        ImGui::InputText("Project name", projectName, sizeof(projectName));
        ImGui::InputText("Parent folder", parentText, sizeof(parentText));
        ImGui::SameLine();
        if (ImGui::Button("Browse##parent")) {
            std::filesystem::path folder;
            if (browseForFolder(folder, window.handle)) {
                const auto text = utf8Path(folder);
                std::strncpy(parentText, text.c_str(), sizeof(parentText) - 1);
                parentText[sizeof(parentText) - 1] = '\0';
            }
        }
        if (ImGui::Button("Create Project", ImVec2(240, 34))) {
            std::filesystem::path manifest;
            if (!createProjectFromTemplate(engineRoot, projectName, std::filesystem::path(parentText), manifest, status)) {
                status = "Cannot create project: " + status;
            } else {
                rae::project::ProjectPaths candidate;
                if (!rae::project::loadProject(manifest, candidate, status)) status = "Created project cannot be opened: " + status;
                else if (!rae::project::updateProjectEngineBinding(manifest, currentIdentity, status)) {
                    status = "Created project opened, but its engine binding could not be written: " + status;
                } else if (!rae::project::loadProject(manifest, candidate, status)) {
                    status = "Created project binding was written, but reload failed: " + status;
                } else {
                    selectCandidate(std::move(candidate));
                }
            }
        }
        ImGui::Spacing();
        ImGui::Text("Example projects");
        if (examples.empty()) ImGui::TextDisabled("No examples were found in this engine.");
        for (const auto& example : examples) {
            if (ImGui::Button((example.name + "##example").c_str(), ImVec2(240, 30))) {
                rae::project::ProjectPaths candidate;
                if (openProject(example.manifest, candidate)) selectCandidate(std::move(candidate));
            }
            ImGui::SameLine(); ImGui::TextDisabled("%s", utf8Path(example.manifest).c_str());
        }
        if (!status.empty()) { ImGui::Spacing(); ImGui::TextWrapped("%s", status.c_str()); }
        ImGui::End();
        if (mismatch) {
            ImGui::OpenPopup("Engine version mismatch");
            if (ImGui::BeginPopupModal("Engine version mismatch", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextWrapped("%s", mismatchReport.message.c_str());
                ImGui::TextWrapped("Referenced engine:\n%s", utf8Path(mismatch->engineRoot).c_str());
                ImGui::TextWrapped("Choose an engine for this project, update its binding, or close the project manager.");
                if (mismatch->engine.manifestPresent && ImGui::Button("Use referenced engine", ImVec2(240, 32))) {
                    const auto referencedReport = rae::project::checkEngineCompatibility(*mismatch, mismatch->engine);
                    if (referencedReport.compatible()) {
                        rae::project::registerEngineInRegistry(registry, mismatch->engine.version, mismatch->engine.root);
                        rae::project::saveEngineRegistry(rae::project::defaultEngineRegistryPath(), registry, status);
                        selected = *mismatch;
                        mismatch.reset();
                        accepted = true;
                        running = false;
                        ImGui::CloseCurrentPopup();
                    } else {
                        status = "Referenced engine identity does not satisfy the project binding: " + referencedReport.message;
                    }
                }
                if (mismatchReport.canForceUpdate && ImGui::Button("Update to current engine", ImVec2(240, 32)))
                    rebindAndOpen(currentIdentity);
                if (ImGui::Button("Choose engine...", ImVec2(240, 32))) {
                    std::filesystem::path engineDir;
                    if (browseForFolder(engineDir, window.handle)) {
                        rae::project::EngineIdentity chosen;
                        std::string chosenError;
                        if (!rae::project::loadEngineIdentity(engineDir, chosen, chosenError)) {
                            status = "Cannot open selected engine: " + chosenError;
                        } else {
                            rebindAndOpen(chosen);
                        }
                    }
                }
                if (!registry.engines.empty()) {
                    ImGui::Separator();
                    ImGui::TextDisabled("Discovered engines");
                    for (const auto& entry : registry.engines) {
                        if (ImGui::Button(("Use " + entry.association + "##" + utf8Path(entry.root)).c_str(), ImVec2(220, 26))) {
                            rae::project::EngineIdentity chosen;
                            std::string chosenError;
                            if (!rae::project::loadEngineIdentity(entry.root, chosen, chosenError))
                                status = "Discovered engine is unavailable: " + chosenError;
                            else rebindAndOpen(chosen);
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", utf8Path(entry.root).c_str());
                    }
                }
                if (ImGui::Button("Close project manager", ImVec2(220, 32))) { mismatch.reset(); running = false; ImGui::CloseCurrentPopup(); }
                ImGui::EndPopup();
            }
        }
        if (windowState.closeRequested) running = false;
        // The project manager is intentionally a pure project-selection surface. It
        // still uses the lightweight D3D11 presentation path required by
        // Dear ImGui, but never renders the scene, grid, or preview geometry.
        const float projectManagerClear[4] = {0.018f, 0.024f, 0.038f, 1.0f};
        if (!renderer.clear(projectManagerClear, error)) { ui.shutdown(); return false; }
        ui.endFrame();
        if (!renderer.present(true, error)) { ui.shutdown(); return false; }
        updateGarbageCollection(gc, renderer, ui, previousGc);
    }
    ui.shutdown();
    // The project manager owns a separate Win32 window. Destroy it before the
    // function returns so the WM_QUIT posted by its WM_DESTROY handler can be
    // drained here; otherwise the editor loop that starts next would consume the
    // stale quit message and exit immediately.
    if (window.handle && IsWindow(window.handle)) {
        DestroyWindow(window.handle);
        window.handle = nullptr;
    }
    MSG leftover{};
    while (PeekMessageW(&leftover, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
    return accepted;
}
}

int wmain(int argc, wchar_t** argv) {
    bool selfTest = false, editorSelfTest = false, gcSelfTest = false, showUI = true, vsync = true;
    bool temporaryCaptures = false, explicitCaptureDir = false, disableGc = false;
    std::optional<double> gcInterval, gcIdle, gcCaptureAge;
    std::filesystem::path gcSettingsPath = "RestArtEngine.gc.json";
    int frameLimit = 0;
    std::filesystem::path captureDir;
    std::filesystem::path projectPath;
    std::filesystem::path enginePath;
    SceneSettings settings;
    try {
        for (int i = 1; i < argc; ++i) {
            const auto argument = std::filesystem::path(argv[i]).u8string();
            const std::string arg(reinterpret_cast<const char*>(argument.data()), argument.size());
            if (arg == "--self-test") selfTest = true;
            else if (arg == "--editor-self-test") editorSelfTest = true;
            else if (arg == "--gc-self-test") gcSelfTest = true;
            else if (arg == "--capture-temp") temporaryCaptures = true;
            else if (arg == "--no-gc") disableGc = true;
            else if (arg == "--gc-settings" && i + 1 < argc) gcSettingsPath = argv[++i];
            else if ((arg == "--gc-interval" || arg == "--gc-idle" || arg == "--gc-capture-age") && i + 1 < argc) {
                const std::wstring value = argv[++i];
                std::size_t consumed = 0;
                const double seconds = std::stod(value, &consumed);
                if (consumed != value.size()) throw std::runtime_error(arg + " requires seconds.");
                if (arg == "--gc-interval") gcInterval = seconds;
                else if (arg == "--gc-idle") gcIdle = seconds;
                else gcCaptureAge = seconds;
            }
            else if (arg == "--project" && i + 1 < argc) projectPath = argv[++i];
            else if (arg == "--engine" && i + 1 < argc) enginePath = argv[++i];
            else if (arg == "--no-ui") showUI = false;
            else if (arg == "--forward") settings.path = RenderPath::Forward;
            else if (arg == "--deferred") settings.path = RenderPath::Deferred;
            else if (arg == "--rt") settings.rayTracing = true;
            else if (arg == "--frames" && i + 1 < argc) {
                frameLimit = std::stoi(argv[++i]);
                if (frameLimit <= 0) throw std::runtime_error("--frames must be positive");
            }
            else if (arg == "--capture-dir" && i + 1 < argc) { captureDir = argv[++i]; explicitCaptureDir = true; }
            else if (arg == "auto" || arg == "dx11") {}
            else if (arg == "--help") {
                std::cout << "RestArtEngine: DX11 scene and asset editor\n"
                    "--forward | --deferred   --rt   --no-ui   --frames N\n"
                    "--self-test --editor-self-test --capture-dir PATH (GPU validation and BMP captures)\n"
                    "F9/Capture: Save As dialog; --frames alone does not save an image.\n"
                    "--gc-settings PATH --gc-interval SECONDS --gc-idle SECONDS --gc-capture-age SECONDS --no-gc\n"
                    "--capture-temp (managed temporary final-frame capture; requires --frames, excludes --capture-dir)\n"
                    "--gc-self-test (isolated resource GC and GUI validation)\n"
                    "--engine PATH (engine root used by the project manager)\n"
                    "--project PATH (project directory or .raproject file)\n"
                    "PATH                shorthand for --project PATH (e.g. Projects/Sandbox/Sandbox.raproject)\n";
                return 0;
            }
            // A bare argument that is not a flag is treated as the project path,
            // so passing (or double-clicking) a .raproject opens it directly.
            else if (projectPath.empty() && !arg.starts_with("--")) projectPath = argv[i];
            else throw std::runtime_error("Unsupported argument: " + arg + ". This scene currently runs on DX11. Use --help.");
        }
        if (temporaryCaptures && (frameLimit <= 0 || explicitCaptureDir || selfTest || editorSelfTest || gcSelfTest))
            throw std::runtime_error("--capture-temp requires --frames and cannot be combined with --capture-dir or self-tests.");
        if ((selfTest || editorSelfTest || gcSelfTest) && !explicitCaptureDir)
            captureDir = std::filesystem::temp_directory_path() / "RestArtEngine/Validation";
        rae::GarbageCollector gc;
        std::string gcError;
        if (!gc.loadSettings(gcSettingsPath, gcError)) std::cerr << "GC settings: " << gcError << '\n';
        auto gcSettings = gc.settings();
        if (gcInterval) gcSettings.intervalSeconds = *gcInterval;
        if (gcIdle) gcSettings.memoryIdleSeconds = *gcIdle;
        if (gcCaptureAge) gcSettings.temporaryCaptureMaxAgeSeconds = *gcCaptureAge;
        if (disableGc) gcSettings.enabled = false;
        if (!gc.setSettings(gcSettings, gcError)) throw std::runtime_error(gcError);
        // Validation outputs are durable evidence; self-tests use isolated GC services.
        if (!selfTest && !editorSelfTest && !gcSelfTest) {
            std::error_code tempError;
            const auto root = std::filesystem::temp_directory_path(tempError) / "RestArtEngine/Captures";
            if (tempError || !gc.initializeTemporaryCaptures(root, gcError)) {
                if (tempError) gcError = tempError.message();
                if (temporaryCaptures) throw std::runtime_error(gcError);
                std::cerr << "Temporary capture GC unavailable: " << gcError << '\n';
            }
        }
        SetProcessDPIAware();
        rae::imgui::uiPreferences() = rae::imgui::loadUiPreferences(rae::imgui::defaultUiPreferencesPath());
        rae::imgui::currentTheme() = rae::imgui::uiPreferences().theme;
        if (selfTest) { vsync = false; frameLimit = 14; }
        rae::project::ProjectPaths project;
        std::string error;
        // Self-tests run against a throwaway copy so they never write into the
        // user project; the copy is removed when the process exits.
        std::filesystem::path isolatedProjectRoot;
        struct TempProjectCleanup {
            std::filesystem::path path;
            ~TempProjectCleanup() {
                if (path.empty()) return;
                std::error_code cleanupCode;
                std::filesystem::remove_all(path, cleanupCode);
            }
        } tempProjectCleanup;
        if (projectPath.empty() && !selfTest && !editorSelfTest && !gcSelfTest) {
            const auto engineRoot = enginePath.empty() ? findEngineRoot() : std::filesystem::weakly_canonical(enginePath);
            if (!runProjectManager(engineRoot, project, gc, error)) {
                if (!error.empty()) throw std::runtime_error(error);
                return 0; // User closed the project manager without opening a project.
            }
        } else {
            // Pass the running engine so a moved project can be rebound without
            // opening the project manager first.
            const auto engineHint = enginePath.empty() ? findEngineRoot() : std::filesystem::weakly_canonical(enginePath);
            project = rae::editor::resolveProject(projectPath, engineHint);
            if (editorSelfTest) {
                std::error_code copyCode;
                isolatedProjectRoot = std::filesystem::temp_directory_path(copyCode) /
                    ("RAEngineSelfTest-" + std::to_string(GetCurrentProcessId()));
                std::filesystem::remove_all(isolatedProjectRoot, copyCode);
                std::filesystem::copy(project.projectRoot, isolatedProjectRoot,
                    std::filesystem::copy_options::recursive |
                    std::filesystem::copy_options::overwrite_existing, copyCode);
                if (copyCode)
                    throw std::runtime_error("Failed to copy the project for the self-test: " + copyCode.message());
                project = rae::editor::resolveProject(isolatedProjectRoot, project.engineRoot);
                tempProjectCleanup.path = isolatedProjectRoot;
            }
        }
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{sizeof(WNDCLASSEXW)};
        wc.style = CS_CLASSDC; wc.lpfnWndProc = windowProc; wc.hInstance = instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = L"RAEngineMaterialLab";
        wc.hIcon = loadEngineIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
        wc.hIconSm = loadEngineIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
        if (!RegisterClassExW(&wc)) throw std::runtime_error("RegisterClassEx failed");
        const bool interactive = !selfTest && !editorSelfTest && !gcSelfTest;
        const auto title = windowTitle(project.name);
        const RECT monitor = primaryMonitorRect();
        SceneRenderer renderer;
        renderer.setShaderRoot(project.engineRoot / "Shaders");
        renderer.setMaterialShaderPath(project.shaderRoot / "Materials/Standard.hlsli");
        rae::imgui::ImGuiLayer ui;
        rae::editor::LoadingScreen loading;
        if (interactive) {
            // Phase 1: borderless loading window, 28% of the monitor width at the
            // artwork aspect, centred. It is destroyed before the editor window
            // exists, so no runtime window-style switching is involved.
            WindowState loadingState;
            loadingState.chrome.setEnabled(false);
            loadingState.width = static_cast<unsigned>(std::max(320, static_cast<int>((monitor.right - monitor.left) * 0.28f)));
            loadingState.height = static_cast<unsigned>(std::max(180, static_cast<int>(loadingState.width * 9 / 16)));
            const RECT placed = centredWindowRect(monitor, static_cast<int>(loadingState.width),
                static_cast<int>(loadingState.height), false);
            {
                WindowOwner loadingWindow{CreateWindowW(wc.lpszClassName, title.c_str(), WS_POPUP,
                    placed.left, placed.top, placed.right - placed.left, placed.bottom - placed.top,
                    nullptr, nullptr, instance, &loadingState)};
                if (!loadingWindow.handle) throw std::runtime_error("CreateWindow failed");
                ShowWindow(loadingWindow.handle, SW_SHOW);
                if (!renderer.initializeDevice(loadingWindow.handle, loadingState.width, loadingState.height, error))
                    throw std::runtime_error(error);
                if (!ui.initialize(loadingWindow.handle, renderer.device(), renderer.context(), true))
                    throw std::runtime_error("Dear ImGui initialization failed");
                std::string loadingError;
                if (!loading.initialize(renderer.device(), project.engineRoot / "assets/load/load.png", loadingError))
                    std::cerr << "Loading screen unavailable: " << loadingError << '\n';
                if (loading.ready()) {
                    const int width = std::max(320, static_cast<int>((monitor.right - monitor.left) * 0.28f));
                    const int height = std::max(180, static_cast<int>(width / loading.aspect()));
                    if (width != static_cast<int>(loadingState.width) || height != static_cast<int>(loadingState.height)) {
                        const RECT aspectRect = centredWindowRect(monitor, width, height, false);
                        SetWindowPos(loadingWindow.handle, nullptr, aspectRect.left, aspectRect.top, width, height,
                            SWP_NOZORDER | SWP_NOACTIVATE);
                        loadingState.width = static_cast<unsigned>(width);
                        loadingState.height = static_cast<unsigned>(height);
                        if (!renderer.resize(loadingState.width, loadingState.height, error))
                            throw std::runtime_error(error);
                    }
                }
                presentLoading(renderer, ui, loading, 0.15f, "Initializing render device");
                if (!renderer.initializePipeline(error)) throw std::runtime_error(error);
                presentLoading(renderer, ui, loading, 0.65f, "Compiling shader pipeline");
                presentLoading(renderer, ui, loading, 1.0f, "Ready");
                loading.release();
                // Release the swap chain while the boot HWND is alive, then tear
                // down the UI that was bound to it.
                renderer.detachWindow();
                ui.shutdown();
            }
        }
        WindowState windowState;
        windowState.chrome.setEnabled(!selfTest);
        windowState.quitOnDestroy = true;
        RECT bounds{0, 0, static_cast<LONG>(windowState.width), static_cast<LONG>(windowState.height)};
        int windowX = CW_USEDEFAULT, windowY = CW_USEDEFAULT;
        if (interactive) {
            // Phase 2: editor window, 80% of the monitor, centred with the native
            // frame and the custom title bar.
            const int width = std::max(640, static_cast<int>((monitor.right - monitor.left) * 0.8f));
            const int height = std::max(480, static_cast<int>((monitor.bottom - monitor.top) * 0.8f));
            const RECT placed = centredWindowRect(monitor, width, height, true);
            windowState.width = static_cast<unsigned>(width);
            windowState.height = static_cast<unsigned>(height);
            windowX = placed.left;
            windowY = placed.top;
            bounds = RECT{0, 0, placed.right - placed.left, placed.bottom - placed.top};
        } else if (selfTest) {
            AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);
        }
        WindowOwner window{CreateWindowW(wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW,
            windowX, windowY, bounds.right - bounds.left, bounds.bottom - bounds.top,
            nullptr, nullptr, instance, &windowState)};
        if (interactive) ShowWindow(window.handle, SW_SHOW);
        if (!window.handle) throw std::runtime_error("CreateWindow failed");
        if (interactive) {
            if (!renderer.attachWindow(window.handle, windowState.width, windowState.height, error))
                throw std::runtime_error(error);
            if (!ui.initialize(window.handle, renderer.device(), renderer.context(), true))
                throw std::runtime_error("Dear ImGui initialization failed");
            ui.setIniFilename("RestArtEngine.imgui.ini");
        } else {
            if (!renderer.initializeDevice(window.handle, windowState.width, windowState.height, error))
                throw std::runtime_error(error);
            if (!ui.initialize(window.handle, renderer.device(), renderer.context(), !selfTest && !gcSelfTest))
                throw std::runtime_error("Dear ImGui initialization failed");
            if (!renderer.initializePipeline(error)) throw std::runtime_error(error);
        }
        ui.setWindowChrome(&windowState.chrome);
        if (interactive) presentLoading(renderer, ui, loading, 0.65f, "Compiling shader pipeline");
        rae::editor::SceneEditor editor;
        if (!editor.initialize(project, renderer, error)) throw std::runtime_error(error);
        if (interactive) editor.setGarbageCollector(&gc, gcSettingsPath);
        if (gcSelfTest) {
            runGarbageCollectionSmoke(renderer, ui, editor, captureDir);
            return 0;
        }
        if (editorSelfTest) {
            runEditorSmoke(renderer, ui, editor, project, captureDir);
            return 0;
        }
        std::cout << "RestArtEngine / D3D11 / " << renderer.adapterName() << '\n';
        std::cout << "Forward + deferred rasterization; directional shadow maps, selectable analytic shadows, and GPU ray reflections.\n";
        bool running = true;
        int frame = 0;
        std::filesystem::path lastUserCaptureFolder;
        // Defensive: a WM_QUIT posted by a window destroyed during startup would
        // end the loop before its first frame. Only windows with quitOnDestroy
        // set can post it now, so draining here is a cheap safety net.
        {
            MSG stale{};
            while (PeekMessageW(&stale, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE))
                std::cerr << "Drained a stale WM_QUIT left over from startup.\n";
        }
        auto previous = std::chrono::steady_clock::now();
        auto previousGc = previous;
        while (running) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) running = false;
                TranslateMessage(&msg); DispatchMessageW(&msg);
            }
            if (!running) break;
            // A taskbar close request must still expose the main editor's dirty
            // confirmation even while every editor surface is minimized.
            if (windowState.minimized && windowState.closeRequested)
                ShowWindow(window.handle, SW_RESTORE);
            if (windowState.minimized) {
                if (ui.hasVisiblePlatformWindows() || ui.hasPendingPlatformWindowRequests()) {
                    const auto now = std::chrono::steady_clock::now();
                    const float dt = std::chrono::duration<float>(now - previous).count();
                    previous = now;
                    ui.beginFrame(std::clamp(dt, 0.0001f, 0.1f));
                    editor.drawMaterialWindow(renderer);
                    if (windowState.closeRequested) {
                        editor.requestClose();
                        windowState.closeRequested = false;
                    }
                    if (editor.drawCloseDialog(renderer)) running = false;
                    ui.endFrame(false);
                }
                updateGarbageCollection(gc, renderer, ui, previousGc);
                Sleep(16); continue;
            }
            if (windowState.resized) {
                if (!renderer.resize(windowState.width, windowState.height, error)) throw std::runtime_error(error);
                windowState.resized = false;
            }
            if (selfTest) {
                settings.path = (frame % 4 < 2) ? RenderPath::Forward : RenderPath::Deferred;
                settings.rayTracing = (frame % 2) != 0;
                settings.debugView = frame == 8 ? 3 : 0;
                if (frame >= 10) {
                    settings.debugView = 1; // Unlit must ignore lights and ray effects in either path.
                    settings.path = frame < 12 ? RenderPath::Forward : RenderPath::Deferred;
                    settings.lightIntensity = frame % 2 ? 12.0f : 5.0f;
                }
                if (frame == 4 && !renderer.resize(1024, 640, error)) throw std::runtime_error(error);
                if (frame == 8 && !renderer.resize(1440, 900, error)) throw std::runtime_error(error);
            }
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::chrono::duration<float>(now - previous).count(); previous = now;
            if (selfTest && frame == 9) {
                // Native docking needs its initial layout/auto-fit frames before
                // a first screenshot, just as in the editor interaction tests.
                for (int warmup = 0; warmup < 2; ++warmup) {
                    ui.beginFrame(1.0f / 60.0f);
                    bool unusedCapture = false;
                    editor.draw(settings, renderer, vsync, unusedCapture);
                    if (!renderer.render(settings, error)) throw std::runtime_error(error);
                    ui.endFrame();
                }
            }
            ui.beginFrame(std::clamp(dt, 0.0001f, 0.1f));
            auto& io = ImGui::GetIO();
            const bool mainWindowFocused = GetForegroundWindow() == window.handle;
            if (mainWindowFocused && !io.WantTextInput) {
                // F11 is "viewport fullscreen", not "UI fullscreen": the window
                // goes borderless over the monitor and the editor panels are
                // hidden so the scene covers the whole screen. Escape and F1
                // leave that mode instead of closing the window.
                if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                    if (windowState.fullscreen) {
                        toggleFullscreen(window.handle, windowState);
                        showUI = windowState.uiBeforeFullscreen;
                    } else {
                        editor.requestClose();
                    }
                }
                if (ImGui::IsKeyPressed(ImGuiKey_F1)) {
                    if (windowState.fullscreen) {
                        toggleFullscreen(window.handle, windowState);
                        showUI = windowState.uiBeforeFullscreen;
                    } else {
                        showUI = !showUI;
                    }
                }
                if (ImGui::IsKeyPressed(ImGuiKey_F2)) settings.path = settings.path == RenderPath::Forward ? RenderPath::Deferred : RenderPath::Forward;
                if (ImGui::IsKeyPressed(ImGuiKey_F3)) settings.rayTracing = !settings.rayTracing;
                if (ImGui::IsKeyPressed(ImGuiKey_F11)) {
                    const bool entering = !windowState.fullscreen;
                    toggleFullscreen(window.handle, windowState);
                    if (entering) {
                        windowState.uiBeforeFullscreen = showUI;
                        showUI = false;
                    } else {
                        showUI = windowState.uiBeforeFullscreen;
                    }
                }
            }
            if (!showUI && mainWindowFocused && !io.WantCaptureMouse && !selfTest) {
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
                    settings.cameraYaw -= io.MouseDelta.x * .006f;
                    settings.cameraPitch = std::clamp(settings.cameraPitch + io.MouseDelta.y * .006f,
                        SceneSettings::minPitch, SceneSettings::maxPitch);
                }
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                    const float p = std::clamp(settings.cameraPitch, SceneSettings::minPitch, SceneSettings::maxPitch);
                    const float y = settings.cameraYaw;
                    const float cp = std::cos(p), sp = std::sin(p);
                    const float cy = std::cos(y), sy = std::sin(y);
                    const float scale = std::clamp(settings.cameraDistance, 2.5f, 30.0f) * 0.002f;
                    const float dx = io.MouseDelta.x * scale;
                    const float dy = io.MouseDelta.y * scale;
                    // Grab style pan: the camera moves opposite to the cursor drag.
                    settings.cameraTarget[0] += -cy * dx - sy * sp * dy;
                    settings.cameraTarget[1] += cp * dy;
                    settings.cameraTarget[2] += sy * dx - cy * sp * dy;
                }
                settings.cameraDistance = std::clamp(settings.cameraDistance - io.MouseWheel * .4f, 2.5f, 14.0f);
            }
            bool capture = mainWindowFocused && ImGui::IsKeyPressed(ImGuiKey_F9, false);
            settings.viewportX = settings.viewportY = settings.viewportWidth = settings.viewportHeight = 0;
            settings.editorViewport = false;
            settings.viewportVisible = true;
            if (showUI && (!selfTest || frame == 9)) editor.draw(settings, renderer, vsync, capture);
            else if (!selfTest) editor.drawMaterialWindow(renderer);
            if (windowState.closeRequested) { editor.requestClose(); windowState.closeRequested = false; }
            if (editor.drawCloseDialog(renderer)) running = false;
            const bool rendered = selfTest ? renderer.render(settings, error)
                : renderer.render(settings, editor.world(), editor.assets(), error, editor.materialPreview());
            if (!rendered) throw std::runtime_error(error);
            ui.endFrame();
            if (capture && !selfTest) {
                saveUserCapture(renderer, window.handle, lastUserCaptureFolder);
                previous = std::chrono::steady_clock::now();
            } else if (selfTest || (frameLimit > 0 && frame == frameLimit - 1 &&
                (explicitCaptureDir || temporaryCaptures))) {
                const bool temporary = temporaryCaptures;
                if (!temporary) std::filesystem::create_directories(captureDir);
                std::string name;
                if (selfTest) {
                    const char* labels[] = {"forward_off", "forward_on", "deferred_off", "deferred_on",
                        "resized_forward_off", "resized_forward_on", "resized_deferred_off", "resized_deferred_on", "normals", "editor",
                        "unlit_forward", "unlit_forward_changed", "unlit_deferred", "unlit_deferred_changed"};
                    name = labels[frame];
                } else name = "capture_" + std::to_string(GetTickCount64());
                auto file = captureDir / (name + ".bmp");
                if (temporary) {
                    if (!gc.captureTemporary([&](std::FILE* stream, std::string& captureError) {
                        return renderer.saveScreenshot(stream, captureError);
                    }, file, error)) throw std::runtime_error(error);
                } else if (!renderer.saveScreenshot(file.c_str(), error)) throw std::runtime_error(error);
                std::cout << (temporary ? "Temporary capture " : "Captured ") << file.string() << '\n';
            }
            if (!renderer.present(vsync, error)) throw std::runtime_error(error);
            if (!selfTest) updateGarbageCollection(gc, renderer, ui, previousGc);
            ++frame;
            if (frameLimit && frame >= frameLimit) running = false;
        }
        ui.shutdown();
        const auto validationErrors = renderer.validationErrorCount();
        std::cout << "D3D11 debug layer: " << (renderer.validationEnabled() ? "enabled" : "unavailable")
            << "; validation errors: " << validationErrors << '\n';
        if (validationErrors) throw std::runtime_error("D3D11 validation reported errors");
        std::cout << "Completed " << frame << " frames.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "RestArtEngine error: " << e.what() << '\n';
        if (!selfTest && !editorSelfTest && !gcSelfTest) MessageBoxA(nullptr, e.what(), "RestArtEngine initialization/rendering error", MB_OK | MB_ICONERROR);
        return 1;
    }
}

