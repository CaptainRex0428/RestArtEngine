#pragma once

#include <cstdint>
#include <string>

namespace rae::renderer { class SceneRenderer; }

namespace rae::editor {

enum class DiagnosticsTab { Profiler, FrameInspector };

// Read-only diagnostics UI. Captured images never participate in scene input,
// selection or undo; the renderer owns all live and captured GPU resources.
class DiagnosticsPanel {
public:
    void show(DiagnosticsTab tab = DiagnosticsTab::Profiler);
    void close() noexcept { open_ = focused_ = false; }
    void requestCapture(renderer::SceneRenderer& renderer);
    void draw(renderer::SceneRenderer& renderer);
    [[nodiscard]] bool isOpen() const noexcept { return open_; }
    [[nodiscard]] bool isFocused() const noexcept { return focused_; }
private:
    void drawProfiler(renderer::SceneRenderer& renderer);
    void drawFrameInspector(renderer::SceneRenderer& renderer);
    bool open_ = false, focused_ = false, focusRequested_ = false, selectTab_ = false;
    DiagnosticsTab tab_ = DiagnosticsTab::Profiler;
    std::uint64_t selectedTraceFrame_ = 0, displayedCaptureFrame_ = 0;
    int resource_ = 0, channel_ = 0, pixelX_ = 0, pixelY_ = 0;
    bool pixelValid_ = false;
    float pixel_[4]{};
    std::string traceMessage_, captureMessage_;
};

} // namespace rae::editor
