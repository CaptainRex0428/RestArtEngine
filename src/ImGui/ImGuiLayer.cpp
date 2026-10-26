#include <RestArtEngine/ImGui/ImGuiLayer.h>
#include <RestArtEngine/ImGui/EditorTheme.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <cmath>
#include <iostream>
#include <string>
#include <unordered_map>

// The backend intentionally leaves this declaration to its consumer.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace rae::imgui {
namespace {
// Private property on engine-owned HWNDs. Integer atoms do not allocate from
// the session-wide string atom table (SetProp with a string can fail with
// ERROR_NOT_ENOUGH_MEMORY when that table is exhausted).
const ATOM contextAtom = GlobalAddAtomW(MAKEINTATOM(0x5241));
const wchar_t* const contextProperty = MAKEINTATOM(contextAtom);
const ATOM platformProcedureAtom = GlobalAddAtomW(MAKEINTATOM(0x5242));
const wchar_t* const platformProcedureProperty = MAKEINTATOM(platformProcedureAtom);
const ATOM platformContextAtom = GlobalAddAtomW(MAKEINTATOM(0x5243));
const wchar_t* const platformContextProperty = MAKEINTATOM(platformContextAtom);

struct PlatformWindowBinding {
    ImGuiContext* context;
    WNDPROC procedure;
};
// A native property is normally sufficient. Keep the process-local binding as
// a fallback if Win32 cannot allocate even an integer-keyed property; a visible
// secondary must never be left with no input/context routing.
std::unordered_map<HWND, PlatformWindowBinding> platformWindowBindings;
bool backendContextBindingWarningReported = false;

// The upstream platform callback can run between frames when the engine has
// restored another (or no) current context. Bind the context stored by Win32's
// backend for the entire native dispatch, including its default procedure.
LRESULT CALLBACK platformWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    const auto binding = platformWindowBindings.find(window);
    auto original = reinterpret_cast<WNDPROC>(GetPropW(window, platformProcedureProperty));
    auto* owned = static_cast<ImGuiContext*>(GetPropW(window, platformContextProperty));
    if (binding != platformWindowBindings.end()) {
        if (!original) original = binding->second.procedure;
        if (!owned) owned = binding->second.context;
    }
    auto* backendContext = static_cast<ImGuiContext*>(GetPropA(window, "IMGUI_CONTEXT"));
    if (!owned) owned = backendContext;
    auto* previous = ImGui::GetCurrentContext();
    if (owned) ImGui::SetCurrentContext(owned);
    LRESULT result = 0;
    if (message == WM_CLOSE && owned) {
        // Closing a material window only hides its UI. Let ImGui update the
        // p_open flag next frame, then retire both HWND and swap chain together.
        // In this pinned backend DefWindowProc would destroy the HWND too soon.
        if (auto* viewport = ImGui::FindViewportByPlatformHandle(window))
            viewport->PlatformRequestClose = true;
    } else if (owned && backendContext != owned) {
        // The vendored platform procedure returns DefWindowProc immediately
        // when its unchecked string property allocation failed. Its ordinary
        // input handler can instead use our correctly bound current context.
        result = ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam);
        if (!result) {
            if (auto* viewport = ImGui::FindViewportByPlatformHandle(window)) {
                if (message == WM_MOVE) viewport->PlatformRequestMove = true;
                if (message == WM_SIZE) viewport->PlatformRequestResize = true;
                if (message == WM_MOUSEACTIVATE && (viewport->Flags & ImGuiViewportFlags_NoFocusOnClick))
                    result = MA_NOACTIVATE;
                if (message == WM_NCHITTEST && (viewport->Flags & ImGuiViewportFlags_NoInputs))
                    result = HTTRANSPARENT;
            }
            if (!result) result = DefWindowProcW(window, message, wparam, lparam);
        }
    } else {
        result = original ? CallWindowProcW(original, window, message, wparam, lparam)
                          : DefWindowProcW(window, message, wparam, lparam);
    }
    if (message == WM_NCDESTROY) {
        RemovePropW(window, platformProcedureProperty);
        RemovePropW(window, platformContextProperty);
        platformWindowBindings.erase(window);
    }
    ImGui::SetCurrentContext(previous);
    return result;
}

} // namespace

ImGuiLayer::~ImGuiLayer() { shutdown(); }

bool ImGuiLayer::initialize(void* window, ID3D11Device* device, ID3D11DeviceContext* deviceContext,
                            bool enablePlatformWindows) {
    shutdown();
    if (!window || !device || !deviceContext) { std::cerr << "ImGui: missing window/device/context\n"; return false; }
    const auto hwnd = static_cast<HWND>(window);
    if (!IsWindow(hwnd) || GetPropW(hwnd, contextProperty)) { std::cerr << "ImGui: invalid or already attached window\n"; return false; }
    IMGUI_CHECKVERSION();
    auto* previous = ImGui::GetCurrentContext();
    context_ = ImGui::CreateContext();
    if (!context_) {
        ImGui::SetCurrentContext(previous);
        return false;
    }
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context_));
    window_ = window;
    frameCount_ = 0;
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    if (enablePlatformWindows) io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    // The editor constrains its dock panels to the main viewport. Only an
    // explicitly separate workspace requests its own native platform window.
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigWindowsResizeFromEdges = true;
    io.IniFilename = nullptr;
    io.Fonts->AddFontDefault();
    // Keep the built-in monospace Latin glyphs and merge a locally installed CJK font.
    char windowsFolder[MAX_PATH]{};
    if (GetWindowsDirectoryA(windowsFolder, MAX_PATH)) {
        const std::string fontPath = std::string(windowsFolder) + "/Fonts/msyh.ttc";
        if (GetFileAttributesA(fontPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            ImFontConfig fontConfig;
            fontConfig.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f, &fontConfig, io.Fonts->GetGlyphRangesChineseFull());
        }
    }
    applyTheme(currentTheme());
    platformInitialized_ = ImGui_ImplWin32_Init(window);
    if (platformInitialized_) rendererInitialized_ = ImGui_ImplDX11_Init(device, deviceContext);
    initialized_ = platformInitialized_ && rendererInitialized_;
    if (initialized_ && !SetPropW(hwnd, contextProperty, this)) {
        std::cerr << "ImGui: cannot attach window context, Win32 error " << GetLastError() << '\n';
        initialized_ = false;
    }
    if (initialized_ && enablePlatformWindows) {
        auto& platform = ImGui::GetPlatformIO();
        createPlatformWindow_ = platform.Platform_CreateWindow;
        platform.Platform_CreateWindow = createPlatformWindow;
    }
    if (!platformInitialized_ || !rendererInitialized_)
        std::cerr << "ImGui: backend initialization failed (platform=" << platformInitialized_
            << ", renderer=" << rendererInitialized_ << ")\n";
    if (!initialized_) shutdown();
    ImGui::SetCurrentContext(previous);
    return initialized_;
}

void ImGuiLayer::shutdown() noexcept {
    if (context_) {
        auto* owned = static_cast<ImGuiContext*>(context_);
        auto* previous = ImGui::GetCurrentContext();
        if (previous == owned)
            previous = frameActive_ ? static_cast<ImGuiContext*>(previousFrameContext_) : nullptr;
        if (previous == owned) previous = nullptr;
        ImGui::SetCurrentContext(owned);
        if (frameActive_) ImGui::EndFrame();
        // Native windows and their swap chains borrow the backend/device. They
        // must be gone before either backend releases its device references.
        if (platformInitialized_ && rendererInitialized_) ImGui::DestroyPlatformWindows();
        if (rendererInitialized_) ImGui_ImplDX11_Shutdown();
        if (platformInitialized_) ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(owned);
        ImGui::SetCurrentContext(previous);
    }
    if (window_ && GetPropW(static_cast<HWND>(window_), contextProperty) == this)
        RemovePropW(static_cast<HWND>(window_), contextProperty);
    createPlatformWindow_ = nullptr;
    context_ = nullptr;
    previousFrameContext_ = nullptr;
    window_ = nullptr;
    platformInitialized_ = false;
    rendererInitialized_ = false;
    frameActive_ = false;
    initialized_ = false;
}

void ImGuiLayer::beginFrame(float deltaSeconds) noexcept {
    if (!initialized_ || frameActive_) return;
    previousFrameContext_ = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context_));
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::GetIO().DeltaTime = std::isfinite(deltaSeconds) && deltaSeconds > 0
        ? deltaSeconds : (1.0F / 60.0F);
    ImGui::NewFrame();
    frameActive_ = true;
}

void ImGuiLayer::endFrame(bool renderMain) noexcept {
    if (!initialized_ || !frameActive_) return;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context_));
    ImGui::Render();
    if (renderMain) ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
    ++frameCount_;
    frameActive_ = false;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(previousFrameContext_));
    previousFrameContext_ = nullptr;
}

bool ImGuiLayer::hasVisiblePlatformWindows() const noexcept {
    if (!context_ || !initialized_) return false;
    const auto& viewports = static_cast<const ImGuiContext*>(context_)->Viewports;
    for (int index = 1; index < viewports.Size; ++index) {
        const auto* viewport = viewports[index];
        const auto hwnd = static_cast<HWND>(viewport->PlatformHandleRaw
            ? viewport->PlatformHandleRaw : viewport->PlatformHandle);
        if (hwnd && IsWindowVisible(hwnd) && !IsIconic(hwnd)) return true;
    }
    return false;
}

bool ImGuiLayer::hasPendingPlatformWindowRequests() const noexcept {
    if (!context_ || !initialized_) return false;
    const auto& viewports = static_cast<const ImGuiContext*>(context_)->Viewports;
    for (int index = 1; index < viewports.Size; ++index) {
        const auto* viewport = viewports[index];
        if (viewport->PlatformRequestClose || viewport->PlatformRequestMove || viewport->PlatformRequestResize)
            return true;
    }
    return false;
}

void ImGuiLayer::createPlatformWindow(ImGuiViewport* viewport) {
    const auto* mainViewport = ImGui::GetMainViewport();
    const auto mainWindow = static_cast<HWND>(mainViewport->PlatformHandleRaw
        ? mainViewport->PlatformHandleRaw : mainViewport->PlatformHandle);
    auto* layer = static_cast<ImGuiLayer*>(GetPropW(mainWindow, contextProperty));
    if (!layer || !layer->createPlatformWindow_) return;
    layer->createPlatformWindow_(viewport);
    const auto hwnd = static_cast<HWND>(viewport->PlatformHandleRaw
        ? viewport->PlatformHandleRaw : viewport->PlatformHandle);
    if (!hwnd) return;
    const auto original = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    auto* owned = ImGui::GetCurrentContext();
    platformWindowBindings.insert_or_assign(hwnd, PlatformWindowBinding{owned, original});
    if (!SetPropW(hwnd, platformProcedureProperty, reinterpret_cast<HANDLE>(original))) {
        std::cerr << "ImGui: native procedure property failed; retaining the engine registry binding.\n";
    }
    if (!SetPropW(hwnd, platformContextProperty, owned)) {
        std::cerr << "ImGui: native context property failed; retaining the engine registry binding.\n";
    }
    SetLastError(0);
    if (!SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(platformWindowProc)) && GetLastError()) {
        RemovePropW(hwnd, platformProcedureProperty);
        RemovePropW(hwnd, platformContextProperty);
        platformWindowBindings.erase(hwnd);
        std::cerr << "ImGui: cannot subclass native window, Win32 error " << GetLastError() << '\n';
    } else if (GetPropA(hwnd, "IMGUI_CONTEXT") != owned && !backendContextBindingWarningReported) {
        backendContextBindingWarningReported = true;
        std::cerr << "ImGui: native backend context property unavailable; using the engine binding.\n";
    }
    // Reuse the main window's embedded application icon without owning a second
    // icon resource lifetime. The OS/backend owns the secondary window itself.
    for (const auto size : {ICON_BIG, ICON_SMALL}) {
        const auto icon = SendMessageW(mainWindow, WM_GETICON, size, 0);
        if (icon) SendMessageW(hwnd, WM_SETICON, size, icon);
    }
}

void ImGuiLayer::setIniFilename(const char* filename) noexcept {
    if (!context_) return;
    auto* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context_));
    ImGui::GetIO().IniFilename = filename;
    ImGui::SetCurrentContext(previous);
}

bool ImGuiLayer::handleMessage(void* window, unsigned message,
                               std::uintptr_t wparam, std::intptr_t lparam) {
    if (!window) return false;
    auto* layer = static_cast<ImGuiLayer*>(GetPropW(static_cast<HWND>(window), contextProperty));
    if (!layer || !layer->initialized_) return false;
    auto* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(layer->context_));
    const auto result = ImGui_ImplWin32_WndProcHandler(
        static_cast<HWND>(window), message, static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam));
    ImGui::SetCurrentContext(previous);
    return result != 0;
}

} // namespace rae::imgui
