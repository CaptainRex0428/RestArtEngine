#include <RestArtEngine/ImGui/ImGuiLayer.h>
#include <RestArtEngine/ImGui/EditorTheme.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <cmath>
#include <iostream>
#include <string>

// The backend intentionally leaves this declaration to its consumer.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace rae::imgui {
namespace {
// Private property on engine-owned HWNDs. Integer atoms do not allocate from
// the session-wide string atom table (SetProp with a string can fail with
// ERROR_NOT_ENOUGH_MEMORY when that table is exhausted).
const ATOM contextAtom = GlobalAddAtomW(MAKEINTATOM(0x5241));
const wchar_t* const contextProperty = MAKEINTATOM(contextAtom);

} // namespace

ImGuiLayer::~ImGuiLayer() { shutdown(); }

bool ImGuiLayer::initialize(void* window, ID3D11Device* device, ID3D11DeviceContext* deviceContext) {
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
    // Native docking owns split/tab/float interactions. Platform multi-viewports
    // stay disabled: floating panels remain inside the main DX11 swap chain.
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
    if (!platformInitialized_ || !rendererInitialized_)
        std::cerr << "ImGui: backend initialization failed (platform=" << platformInitialized_
            << ", renderer=" << rendererInitialized_ << ")\n";
    if (!initialized_) shutdown();
    ImGui::SetCurrentContext(previous);
    return initialized_;
}

void ImGuiLayer::shutdown() noexcept {
    if (window_ && GetPropW(static_cast<HWND>(window_), contextProperty) == this)
        RemovePropW(static_cast<HWND>(window_), contextProperty);
    if (context_) {
        auto* owned = static_cast<ImGuiContext*>(context_);
        auto* previous = ImGui::GetCurrentContext();
        if (previous == owned)
            previous = frameActive_ ? static_cast<ImGuiContext*>(previousFrameContext_) : nullptr;
        if (previous == owned) previous = nullptr;
        ImGui::SetCurrentContext(owned);
        if (frameActive_) ImGui::EndFrame();
        if (rendererInitialized_) ImGui_ImplDX11_Shutdown();
        if (platformInitialized_) ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(owned);
        ImGui::SetCurrentContext(previous);
    }
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

void ImGuiLayer::endFrame() noexcept {
    if (!initialized_ || !frameActive_) return;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context_));
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    ++frameCount_;
    frameActive_ = false;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(previousFrameContext_));
    previousFrameContext_ = nullptr;
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