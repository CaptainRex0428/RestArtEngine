#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <RestArtEngine/ImGui/WindowChrome.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <string>

namespace rae::imgui {
namespace {
float windowScale(HWND window) { return static_cast<float>(GetDpiForWindow(window)) / 96.0f; }
bool captionButton(int hit) { return hit == HTMINBUTTON || hit == HTMAXBUTTON || hit == HTCLOSE; }
}

int WindowChrome::hitTest(void* handle, long screenX, long screenY) const {
    const auto window = static_cast<HWND>(handle);
    if (fullscreen_) return HTCLIENT;
    POINT point{screenX, screenY};
    ScreenToClient(window, &point);
    RECT client{};
    GetClientRect(window, &client);
    if (!PtInRect(&client, point)) return HTNOWHERE;
    if (!IsZoomed(window)) {
        const UINT dpi = GetDpiForWindow(window);
        const int border = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
            GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        const bool left = point.x < border, right = point.x >= client.right - border;
        const bool top = point.y < border, bottom = point.y >= client.bottom - border;
        if (top) return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
        if (bottom) return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
    }
    const float scale = windowScale(window);
    const float height = captionHeight_ > 0 ? captionHeight_ : 36.0f * scale;
    if (point.y >= height) return HTCLIENT;
    const float buttonWidth = 46.0f * scale;
    if (point.x >= client.right - buttonWidth) return HTCLOSE;
    if (point.x >= client.right - buttonWidth * 2) return HTMAXBUTTON;
    if (point.x >= client.right - buttonWidth * 3) return HTMINBUTTON;
    if (point.x < 38.0f * scale) return HTSYSMENU;
    return HTCAPTION;
}

bool WindowChrome::handleMessage(void* handle, unsigned message, std::uintptr_t wp,
                                std::intptr_t lp, std::intptr_t& result) {
    if (!enabled_) return false;
    const auto window = static_cast<HWND>(handle);
    result = 0;
    switch (message) {
    case WM_CREATE: {
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        const MARGINS margins{1, 1, 1, 1};
        DwmExtendFrameIntoClientArea(window, &margins);
        return false;
    }
    case WM_NCCALCSIZE:
        // Retain WS_OVERLAPPEDWINDOW for native resize, snap and system menus,
        // but draw the entire caption ourselves inside the client surface.
        if (wp && !fullscreen_ && IsZoomed(window)) {
            auto& rect = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0];
            MONITORINFO monitor{sizeof(MONITORINFO)};
            if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
                rect.left = std::max(rect.left, monitor.rcWork.left);
                rect.top = std::max(rect.top, monitor.rcWork.top);
                rect.right = std::min(rect.right, monitor.rcWork.right);
                rect.bottom = std::min(rect.bottom, monitor.rcWork.bottom);
            }
        }
        return true;
    case WM_GETMINMAXINFO: {
        auto& info = *reinterpret_cast<MINMAXINFO*>(lp);
        const float scale = windowScale(window);
        info.ptMinTrackSize = {static_cast<LONG>(480 * scale), static_cast<LONG>(360 * scale)};
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (!fullscreen_ && GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
            info.ptMaxPosition = {monitor.rcWork.left - monitor.rcMonitor.left,
                                  monitor.rcWork.top - monitor.rcMonitor.top};
            info.ptMaxSize = {monitor.rcWork.right - monitor.rcWork.left,
                             monitor.rcWork.bottom - monitor.rcWork.top};
        }
        return true;
    }
    case WM_NCHITTEST:
        result = hitTest(window, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return true;
    case WM_NCPAINT:
        return true;
    case WM_NCACTIVATE:
        // The entire caption is client-drawn. DefWindowProc can still paint a
        // classic frame here when a modal dialog disables/activates its owner,
        // even with lParam == -1. TRUE permits deactivation without native paint;
        // WM_ACTIVATE and EnableWindow retain their normal focus/modal behavior.
        result = IsIconic(window) ? DefWindowProcW(window, message, wp, lp) : TRUE;
        return true;
    case WM_NCMOUSEMOVE: {
        hovered_ = static_cast<int>(wp);
        TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE | TME_NONCLIENT, window, 0};
        TrackMouseEvent(&tracking);
        // DefWindowProc still sees HTMAXBUTTON, enabling Windows 11 snap UI.
        return false;
    }
    case WM_NCMOUSELEAVE:
        hovered_ = 0;
        return false;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (!fullscreen_ && captionButton(static_cast<int>(wp))) {
            pressed_ = hovered_ = static_cast<int>(wp);
            SetCapture(window);
            return true;
        }
        return false; // HTCAPTION / HTSYSMENU retain native drag, double click and system menu.
    case WM_MOUSEMOVE:
        if (pressed_) {
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(window, &point);
            hovered_ = hitTest(window, point.x, point.y);
            return true;
        }
        return false;
    case WM_LBUTTONUP:
    case WM_NCLBUTTONUP:
        if (pressed_) {
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (message == WM_LBUTTONUP) ClientToScreen(window, &point);
            const int pressed = pressed_;
            pressed_ = 0;
            ReleaseCapture();
            if (hitTest(window, point.x, point.y) == pressed) {
                const WPARAM command = pressed == HTCLOSE ? SC_CLOSE : pressed == HTMINBUTTON ? SC_MINIMIZE :
                    IsZoomed(window) ? SC_RESTORE : SC_MAXIMIZE;
                PostMessageW(window, WM_SYSCOMMAND, command, 0);
            }
            return true;
        }
        return false;
    case WM_CANCELMODE:
        if (pressed_) { pressed_ = 0; ReleaseCapture(); }
        return false;
    case WM_CAPTURECHANGED:
        pressed_ = 0;
        return false;
    }
    return false;
}

void WindowChrome::draw(void* handle, void* logoTexture) {
    if (!enabled_) return;
    const auto window = static_cast<HWND>(handle);
    auto* viewport = static_cast<ImGuiViewportP*>(ImGui::GetMainViewport());
    if (fullscreen_) {
        viewport->WorkPos = viewport->Pos;
        viewport->WorkSize = viewport->Size;
        return;
    }
    const float scale = windowScale(window);
    captionHeight_ = std::max(36.0f * scale, ImGui::GetFontSize() + 16.0f * scale);
    const auto flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::BeginViewportSideBar("##EngineWindowChrome", viewport, ImGuiDir_Up, captionHeight_, flags);
    const auto pos = ImGui::GetWindowPos();
    const auto size = ImGui::GetWindowSize();
    auto* list = ImGui::GetWindowDrawList();
    list->PushClipRect(pos, {pos.x + size.x, pos.y + size.y}, false);
    const auto color = [](ImGuiCol index) { return ImGui::GetColorU32(index); };
    const bool active = GetForegroundWindow() == window;
    list->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y}, color(ImGuiCol_TitleBg));
    list->AddLine({pos.x, pos.y + size.y - 1}, {pos.x + size.x, pos.y + size.y - 1}, color(ImGuiCol_Border));
    const float badge = 28 * scale, inset = 7 * scale;
    const ImVec2 badgeMin{pos.x + inset, pos.y + (size.y - badge) * .5f};
    if (logoTexture)
        list->AddImage(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(logoTexture)),
            badgeMin, {badgeMin.x + badge, badgeMin.y + badge});

    const float buttonWidth = 46 * scale;
    wchar_t title[512]{};
    GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    char utf8[2048]{};
    WideCharToMultiByte(CP_UTF8, 0, title, -1, utf8, sizeof(utf8), nullptr, nullptr);
    list->PushClipRect({badgeMin.x + badge + inset, pos.y}, {pos.x + size.x - 3 * buttonWidth - inset, pos.y + size.y}, true);
    list->AddText({badgeMin.x + badge + inset, pos.y + (size.y - ImGui::GetFontSize()) * .5f},
        color(active ? ImGuiCol_Text : ImGuiCol_TextDisabled), utf8);
    list->PopClipRect();
    for (int index = 0; index < 3; ++index) {
        const int hit = index == 0 ? HTMINBUTTON : index == 1 ? HTMAXBUTTON : HTCLOSE;
        const ImVec2 min{pos.x + size.x - (3 - index) * buttonWidth, pos.y};
        const ImVec2 max{min.x + buttonWidth, pos.y + size.y};
        const bool hover = hovered_ == hit;
        if (hover) list->AddRectFilled(min, max, hit == HTCLOSE ? IM_COL32(196, 43, 55, 255) :
            color(pressed_ == hit ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered));
        const ImU32 ink = hover && hit == HTCLOSE ? IM_COL32_WHITE : color(active ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        const ImVec2 center{(min.x + max.x) * .5f, (min.y + max.y) * .5f};
        const float r = 5 * scale, line = std::max(1.0f, scale);
        if (hit == HTMINBUTTON) list->AddLine({center.x - r, center.y}, {center.x + r, center.y}, ink, line);
        else if (hit == HTCLOSE) {
            list->AddLine({center.x - r, center.y - r}, {center.x + r, center.y + r}, ink, line);
            list->AddLine({center.x - r, center.y + r}, {center.x + r, center.y - r}, ink, line);
        } else if (IsZoomed(window)) {
            list->AddRect({center.x - r + 3 * scale, center.y - r}, {center.x + r, center.y + r - 3 * scale}, ink, 0, 0, line);
            list->AddRectFilled({center.x - r, center.y - r + 3 * scale}, {center.x + r - 3 * scale, center.y + r},
                color(hover ? ImGuiCol_ButtonHovered : ImGuiCol_TitleBg));
            list->AddRect({center.x - r, center.y - r + 3 * scale}, {center.x + r - 3 * scale, center.y + r}, ink, 0, 0, line);
        } else list->AddRect({center.x - r, center.y - r}, {center.x + r, center.y + r}, ink, 0, 0, line);
    }
    list->PopClipRect();
    ImGui::End();
    // Sidebars normally publish work insets next frame. The editor needs the
    // current frame's rect, including on first show, theme/font changes and F11.
    const auto work = viewport->GetBuildWorkRect();
    viewport->WorkPos = work.Min;
    viewport->WorkSize = work.GetSize();
    const auto border = ImGui::GetStyleColorVec4(ImGuiCol_Border);
    const COLORREF nativeColor = RGB(static_cast<BYTE>(border.x * 255),
        static_cast<BYTE>(border.y * 255), static_cast<BYTE>(border.z * 255));
    if (borderColor_ != nativeColor) {
        DwmSetWindowAttribute(window, DWMWA_BORDER_COLOR, &nativeColor, sizeof(nativeColor));
        borderColor_ = nativeColor;
    }
}

} // namespace rae::imgui
