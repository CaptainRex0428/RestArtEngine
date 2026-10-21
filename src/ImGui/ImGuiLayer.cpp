#include <RestArtEngine/ImGui/ImGuiLayer.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <cmath>
#include <string>

// The backend intentionally leaves this declaration to its consumer.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace rae::imgui {
namespace {
constexpr wchar_t contextProperty[] = L"RAEngine.ImGuiLayer";

void applyEditorTheme() {
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(18, 18);
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(10, 10);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.IndentSpacing = 20;
    style.ScrollbarSize = 12;
    style.GrabMinSize = 10;
    style.WindowRounding = 9;
    style.ChildRounding = 7;
    style.FrameRounding = 5;
    style.PopupRounding = 7;
    style.ScrollbarRounding = 8;
    style.GrabRounding = 4;
    style.TabRounding = 5;
    style.WindowBorderSize = 1;
    style.ChildBorderSize = 1;
    style.FrameBorderSize = 0;
    auto* c = style.Colors;
    c[ImGuiCol_Text] = ImVec4(.89F, .93F, .98F, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(.46F, .54F, .65F, 1);
    c[ImGuiCol_WindowBg] = ImVec4(.055F, .075F, .11F, .98F);
    c[ImGuiCol_ChildBg] = ImVec4(.07F, .095F, .14F, .96F);
    c[ImGuiCol_PopupBg] = ImVec4(.085F, .11F, .16F, .99F);
    c[ImGuiCol_Border] = ImVec4(.21F, .28F, .38F, .55F);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = ImVec4(.12F, .17F, .24F, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(.18F, .28F, .40F, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(.19F, .36F, .50F, 1);
    c[ImGuiCol_TitleBg] = ImVec4(.065F, .09F, .135F, 1);
    c[ImGuiCol_TitleBgActive] = ImVec4(.10F, .15F, .22F, 1);
    c[ImGuiCol_TitleBgCollapsed] = c[ImGuiCol_TitleBg];
    c[ImGuiCol_MenuBarBg] = ImVec4(.085F, .115F, .17F, 1);
    c[ImGuiCol_ScrollbarBg] = ImVec4(.05F, .07F, .10F, .70F);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(.22F, .30F, .40F, 1);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(.29F, .41F, .54F, 1);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(.32F, .52F, .67F, 1);
    c[ImGuiCol_CheckMark] = ImVec4(.29F, .79F, .95F, 1);
    c[ImGuiCol_SliderGrab] = ImVec4(.24F, .65F, .86F, 1);
    c[ImGuiCol_SliderGrabActive] = ImVec4(.39F, .83F, 1, 1);
    c[ImGuiCol_Button] = ImVec4(.15F, .29F, .41F, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(.19F, .43F, .59F, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(.23F, .56F, .71F, 1);
    c[ImGuiCol_Header] = ImVec4(.13F, .25F, .36F, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(.18F, .36F, .49F, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(.20F, .43F, .59F, 1);
    c[ImGuiCol_Separator] = ImVec4(.22F, .31F, .42F, .75F);
    c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_ButtonHovered];
    c[ImGuiCol_SeparatorActive] = c[ImGuiCol_ButtonActive];
    c[ImGuiCol_ResizeGrip] = ImVec4(.25F, .65F, .85F, .25F);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(.25F, .65F, .85F, .65F);
    c[ImGuiCol_ResizeGripActive] = ImVec4(.25F, .65F, .85F, .95F);
    c[ImGuiCol_Tab] = ImVec4(.10F, .18F, .27F, 1);
    c[ImGuiCol_TabHovered] = c[ImGuiCol_HeaderHovered];
    c[ImGuiCol_TabSelected] = ImVec4(.17F, .34F, .47F, 1);
    c[ImGuiCol_PlotLines] = ImVec4(.32F, .78F, .94F, 1);
    c[ImGuiCol_PlotHistogram] = ImVec4(.35F, .77F, .73F, 1);
    c[ImGuiCol_TextSelectedBg] = ImVec4(.22F, .55F, .72F, .40F);
}
} // namespace

ImGuiLayer::~ImGuiLayer() { shutdown(); }

bool ImGuiLayer::initialize(void* window, ID3D11Device* device, ID3D11DeviceContext* deviceContext) {
    shutdown();
    if (!window || !device || !deviceContext) return false;
    const auto hwnd = static_cast<HWND>(window);
    if (!IsWindow(hwnd) || GetPropW(hwnd, contextProperty)) return false;
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
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
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
    applyEditorTheme();
    platformInitialized_ = ImGui_ImplWin32_Init(window);
    if (platformInitialized_) rendererInitialized_ = ImGui_ImplDX11_Init(device, deviceContext);
    initialized_ = platformInitialized_ && rendererInitialized_;
    if (initialized_ && !SetPropW(hwnd, contextProperty, this)) initialized_ = false;
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
