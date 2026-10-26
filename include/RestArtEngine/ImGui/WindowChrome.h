#pragma once

#include <cstdint>

namespace rae::imgui {

// Engine-drawn caption, native Win32 move/resize/system commands. Owned by the
// window state; the ImGui layer borrows it only while that HWND is alive.
class WindowChrome {
public:
    void setEnabled(bool value) { enabled_ = value; }
    void setFullscreen(bool value) { fullscreen_ = value; }
    void draw(void* window, void* logoTexture);
    bool handleMessage(void* window, unsigned message, std::uintptr_t wp,
                       std::intptr_t lp, std::intptr_t& result);

private:
    int hitTest(void* window, long screenX, long screenY) const;
    bool enabled_ = true, fullscreen_ = false;
    int pressed_ = 0, hovered_ = 0;
    float captionHeight_ = 0;
    std::uint32_t borderColor_ = 0xffffffff;
};

} // namespace rae::imgui
