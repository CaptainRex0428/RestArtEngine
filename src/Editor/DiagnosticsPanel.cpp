#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <commdlg.h>
#include <RestArtEngine/Editor/DiagnosticsPanel.h>
#include <RestArtEngine/Core/TraceProfiler.h>
#include <RestArtEngine/Renderer/RenderDiagnostics.h>
#include <RestArtEngine/Renderer/SceneRenderer.h>
#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <vector>

namespace rae::editor {
namespace {

void nextButton(const char* label) {
    const auto& style = ImGui::GetStyle();
    const float width = ImGui::CalcTextSize(label).x + style.FramePadding.x * 2;
    if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + width <=
        ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x)
        ImGui::SameLine();
}

std::filesystem::path saveJsonDialog(const wchar_t* title, const wchar_t* suggested, std::string& error) {
    wchar_t filename[32768]{};
    wcsncpy_s(filename, suggested, std::size(filename) - 1);
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFile = filename;
    dialog.nMaxFile = static_cast<DWORD>(std::size(filename));
    dialog.lpstrFilter = L"JSON (*.json)\0*.json\0All files (*.*)\0*.*\0\0";
    dialog.lpstrDefExt = L"json";
    dialog.lpstrTitle = title;
    dialog.Flags = OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    error.clear();
    if (GetSaveFileNameW(&dialog)) return filename;
    const DWORD code = CommDlgExtendedError();
    if (code) error = "Save dialog failed (" + std::to_string(code) + ").";
    return {};
}

void eventTable(const std::vector<renderer::RenderDrawEvent>& events) {
    if (events.empty()) { ImGui::TextDisabled("No draw events in this frame."); return; }
    if (ImGui::BeginTable("Draw events", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY, {0, 200})) {
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Actor ID", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Mesh ID", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Material ID", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Indices / vertices", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("Shader revision", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (const auto& event : events) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(event.pass.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%016llX", static_cast<unsigned long long>(event.actorId));
            ImGui::TableNextColumn(); ImGui::Text("%016llX", static_cast<unsigned long long>(event.meshId));
            ImGui::TableNextColumn(); ImGui::Text("%016llX", static_cast<unsigned long long>(event.materialId));
            ImGui::TableNextColumn(); ImGui::Text("%u / %u", event.indexCount, event.vertexCount);
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(event.shaderRevision));
        }
        ImGui::EndTable();
    }
}

void timingTable(const std::vector<renderer::RenderPassTiming>& passes, bool gpu) {
    if (ImGui::BeginTable(gpu ? "GPU passes" : "CPU passes", 3,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Pass"); ImGui::TableSetupColumn(gpu ? "GPU ms" : "CPU submit ms");
        ImGui::TableSetupColumn("Draw calls"); ImGui::TableHeadersRow();
        for (const auto& pass : passes) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(pass.name.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.3f", gpu ? pass.gpuMs : pass.cpuMs);
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(pass.drawCalls));
        }
        ImGui::EndTable();
    }
}
} // namespace

void DiagnosticsPanel::show(DiagnosticsTab tab) {
    open_ = focusRequested_ = selectTab_ = true;
    tab_ = tab;
}

void DiagnosticsPanel::requestCapture(renderer::SceneRenderer& renderer) {
    show(DiagnosticsTab::FrameInspector);
    renderer.setDiagnosticsEnabled(true);
    renderer.requestFrameCapture();
    captureMessage_.clear();
}

void DiagnosticsPanel::draw(renderer::SceneRenderer& renderer) {
    focused_ = false;
    renderer.setDiagnosticsEnabled(open_ || diagnostics::profiler().isRecording());
    if (!open_) return;
    ImGui::SetNextWindowSize({840, 680}, ImGuiCond_FirstUseEver);
    if (focusRequested_) { ImGui::SetNextWindowFocus(); focusRequested_ = false; }
    const bool visible = ImGui::Begin("Diagnostics##RestArtEngine.Diagnostics", &open_);
    focused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (visible && ImGui::BeginTabBar("Diagnostics tabs")) {
        const auto requestedTab = tab_;
        const auto flags = [this, requestedTab](DiagnosticsTab tab) {
            return selectTab_ && requestedTab == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        };
        if (ImGui::BeginTabItem("Profiler", nullptr, flags(DiagnosticsTab::Profiler))) {
            tab_ = DiagnosticsTab::Profiler;
            drawProfiler(renderer);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Frame Inspector", nullptr, flags(DiagnosticsTab::FrameInspector))) {
            tab_ = DiagnosticsTab::FrameInspector;
            drawFrameInspector(renderer);
            ImGui::EndTabItem();
        }
        selectTab_ = false;
        ImGui::EndTabBar();
    }
    ImGui::End();
    renderer.setDiagnosticsEnabled(open_ || diagnostics::profiler().isRecording());
}

void DiagnosticsPanel::drawProfiler(renderer::SceneRenderer& renderer) {
    auto& profiler = diagnostics::profiler();
    if (!profiler.isRecording()) {
        if (ImGui::Button("Record CPU trace")) { profiler.start(); selectedTraceFrame_ = 0; traceMessage_.clear(); }
    } else if (ImGui::Button("Stop CPU trace")) profiler.stop();
    nextButton("Clear trace");
    if (ImGui::Button("Clear trace")) { profiler.clear(); selectedTraceFrame_ = 0; traceMessage_.clear(); }
    nextButton("Export trace JSON...");
    if (ImGui::Button("Export trace JSON...")) {
        const auto path = saveJsonDialog(L"Export CPU trace", L"trace.json", traceMessage_);
        if (!path.empty() && profiler.save(path, traceMessage_)) traceMessage_ = "CPU trace JSON exported.";
    }
    const auto trace = profiler.snapshot();
    ImGui::TextWrapped("CPU trace: %s | %zu / %zu events | %llu dropped", trace.recording ? "Recording" : "Stopped",
        trace.eventCount, trace.eventCapacity, static_cast<unsigned long long>(trace.droppedEvents));
    ImGui::TextWrapped("Frame duration is main-loop wall time, including Present / VSync waits. CPU scopes and GPU timings measure different work.");
    ImGui::TextWrapped("The scope table uses the latest 512 retained events; export includes the entire retained recording.");
    if (!traceMessage_.empty()) ImGui::TextWrapped("%s", traceMessage_.c_str());
    if (!trace.frames.empty()) {
        std::vector<float> history; history.reserve(trace.frames.size());
        for (const auto& frame : trace.frames) history.push_back(static_cast<float>(frame.durationMs));
        ImGui::PlotLines("##Frame time", history.data(), static_cast<int>(history.size()), 0,
            "Recent complete frames (ms)", 0.0f, FLT_MAX, {-1, 85});
    }
    std::vector<std::uint64_t> recordedFrames;
    for (const auto& event : trace.events) {
        if (event.frameId <= trace.lastFrame.frameId || trace.lastFrame.frameId == 0)
            recordedFrames.push_back(event.frameId);
    }
    std::sort(recordedFrames.begin(), recordedFrames.end(), [](auto a, auto b) { return a > b; });
    recordedFrames.erase(std::unique(recordedFrames.begin(), recordedFrames.end()), recordedFrames.end());
    if (!recordedFrames.empty()) {
        const auto label = selectedTraceFrame_ ? "Frame " + std::to_string(selectedTraceFrame_) : std::string("Latest recorded complete frame");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##Trace frame", label.c_str())) {
            if (ImGui::Selectable("Latest recorded complete frame", selectedTraceFrame_ == 0)) selectedTraceFrame_ = 0;
            for (const auto id : recordedFrames) {
                const std::string frameLabel = "Frame " + std::to_string(id);
                if (ImGui::Selectable(frameLabel.c_str(), selectedTraceFrame_ == id)) selectedTraceFrame_ = id;
            }
            ImGui::EndCombo();
        }
        const auto frameId = selectedTraceFrame_ ? selectedTraceFrame_ : recordedFrames.front();
        ImGui::TextWrapped("CPU scopes: frame %llu (latest 256 matching events)", static_cast<unsigned long long>(frameId));
        if (ImGui::BeginTable("CPU trace events", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX, {0, 190})) {
            ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthFixed, 230);
            ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 110);
            ImGui::TableSetupColumn("CPU ms", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("Thread", ImGuiTableColumnFlags_WidthFixed, 130);
            ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
            std::size_t shown = 0;
            for (auto it = trace.events.rbegin(); it != trace.events.rend() && shown < 256; ++it) {
                if (it->frameId != frameId) continue;
                ++shown;
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(it->name.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Event %llu | Parent %llu | Start %.3f us",
                    static_cast<unsigned long long>(it->eventId), static_cast<unsigned long long>(it->parentEventId), static_cast<double>(it->startUs));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(it->category.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.3f", it->durationUs / 1000.0);
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(it->threadId));
            }
            ImGui::EndTable();
            if (!shown) ImGui::TextWrapped("This frame is outside the recent event window. Export the trace to inspect the full recording.");
        }
    } else ImGui::TextDisabled("Record a CPU trace to inspect complete frames and scopes.");

    const auto& frame = renderer.diagnosticsFrame();
    ImGui::SeparatorText("Live renderer");
    ImGui::TextWrapped("CPU frame %llu | submit %.3f ms | %llu draws | %llu submitted triangles (all passes)",
        static_cast<unsigned long long>(frame.frameId), frame.cpuSubmitMs,
        static_cast<unsigned long long>(frame.drawCalls), static_cast<unsigned long long>(frame.triangles));
    const auto& geometry = renderer.geometryStatistics();
    ImGui::TextWrapped("Scene triangles: %llu | scene frame %llu",
        static_cast<unsigned long long>(geometry.sceneTriangles), static_cast<unsigned long long>(geometry.frameId));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Counts every Actor instance, including hidden instances.\nShared meshes count once per instance; unused assets are excluded.");
    if (geometry.status == renderer::GeometrySampleStatus::Ready) {
        ImGui::TextWrapped("After clipping (GPU): %llu | input: %llu | GPU frame %llu (asynchronous sample)",
            static_cast<unsigned long long>(geometry.postClipTriangles), static_cast<unsigned long long>(geometry.inputTriangles),
            static_cast<unsigned long long>(geometry.gpuFrameId));
    } else {
        const char* status = "Unavailable";
        switch (geometry.status) {
        case renderer::GeometrySampleStatus::Pending: status = "Pending"; break;
        case renderer::GeometrySampleStatus::NotApplicable: status = "N/A"; break;
        case renderer::GeometrySampleStatus::NoViewport: status = "No viewport"; break;
        default: break;
        }
        ImGui::TextWrapped("After clipping (GPU): %s", status);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Main raster geometry output before depth/occlusion tests.\nClipping can split triangles, so output may exceed input.\nGPU samples arrive after the scene frame.");
    if (!geometry.statusMessage.empty()) ImGui::TextWrapped("%s", geometry.statusMessage.c_str());
    timingTable(frame.passes, false);
    switch (frame.gpuStatus) {
    case renderer::GpuTimingStatus::Ready:
        ImGui::TextWrapped("GPU frame %llu | %.3f ms (asynchronous result)",
            static_cast<unsigned long long>(frame.gpuFrameId), frame.gpuTotalMs);
        timingTable(frame.gpuPasses, true);
        break;
    case renderer::GpuTimingStatus::Pending: ImGui::TextWrapped("GPU timing pending. Results arrive after the CPU frame."); break;
    case renderer::GpuTimingStatus::Disjoint: ImGui::TextWrapped("GPU timing invalid: timestamp disjoint. Waiting for a valid sample."); break;
    default: ImGui::TextWrapped("GPU timing unavailable for this device or session."); break;
    }
}

void DiagnosticsPanel::drawFrameInspector(renderer::SceneRenderer& renderer) {
    if (ImGui::Button("Capture next scene frame")) requestCapture(renderer);
    nextButton("Clear capture");
    if (ImGui::Button("Clear capture")) {
        renderer.clearFrameCapture(); resource_ = 0; pixelValid_ = false; captureMessage_.clear();
    }
    const auto& capture = renderer.frameCapture();
    nextButton("Export metadata JSON...");
    ImGui::BeginDisabled(!capture.valid);
    if (ImGui::Button("Export metadata JSON...")) {
        const auto path = saveJsonDialog(L"Export frame metadata (no images)", L"frame-metadata.json", captureMessage_);
        if (!path.empty() && renderer.exportFrameCapture(path, captureMessage_)) captureMessage_ = "Frame metadata JSON exported; images are not included.";
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped("%s", capture.status.c_str());
    ImGui::TextWrapped("Engine-owned frame snapshot. Metadata export contains no images and does not provide API replay.");
    if (!captureMessage_.empty()) ImGui::TextWrapped("%s", captureMessage_.c_str());
    if (!capture.valid) return;
    if (displayedCaptureFrame_ != capture.frameId) {
        displayedCaptureFrame_ = capture.frameId;
        resource_ = 0; pixelX_ = pixelY_ = 0; pixelValid_ = false;
    }
    ImGui::TextWrapped("Captured frame %llu | %s | shader revision %llu | CPU submit %.3f ms",
        static_cast<unsigned long long>(capture.frameId), capture.renderPath.c_str(),
        static_cast<unsigned long long>(capture.shaderRevision), capture.cpuSubmitMs);
    ImGui::TextWrapped("Frozen resources remain independent of live edits, resize and shader compilation.");
    if (!capture.resources.empty()) {
        resource_ = std::clamp(resource_, 0, static_cast<int>(capture.resources.size()) - 1);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##Captured resource", capture.resources[static_cast<std::size_t>(resource_)].name.c_str())) {
            for (std::size_t index = 0; index < capture.resources.size(); ++index) {
                if (ImGui::Selectable(capture.resources[index].name.c_str(), resource_ == static_cast<int>(index))) {
                    resource_ = static_cast<int>(index); pixelValid_ = false; pixelX_ = pixelY_ = 0;
                }
            }
            ImGui::EndCombo();
        }
        const auto& resource = capture.resources[static_cast<std::size_t>(resource_)];
        ImGui::TextWrapped("%s | %u x %u | %.2f MiB", resource.format.c_str(), resource.width, resource.height,
            static_cast<double>(resource.bytes) / (1024.0 * 1024.0));
        ImGui::TextUnformatted("Display channel");
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##Display channel", &channel_, "RGB\0R\0G\0B\0A\0");
        ImGui::TextWrapped("Display preview is mapped for visibility; Read pixel reports stored numeric values before display mapping.");
        if (resource.name.find("Depth") != std::string::npos || resource.name.find("depth") != std::string::npos)
            ImGui::TextWrapped("Depth preview: linear view-space Z / 100 m. Raw pixel depth is device depth in [0, 1].");
        void* texture = renderer.capturedTexture(static_cast<std::size_t>(resource_), static_cast<renderer::DebugTextureChannel>(channel_));
        if (texture && resource.width && resource.height) {
            const float width = std::max(1.0f, std::min(ImGui::GetContentRegionAvail().x, 640.0f));
            const float scale = std::min(width / static_cast<float>(resource.width), 360.0f / static_cast<float>(resource.height));
            const ImVec2 size{static_cast<float>(resource.width) * scale, static_cast<float>(resource.height) * scale};
            ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture)), size);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                const auto origin = ImGui::GetItemRectMin();
                const auto mouse = ImGui::GetMousePos();
                pixelX_ = std::clamp(static_cast<int>((mouse.x - origin.x) / scale), 0, static_cast<int>(resource.width) - 1);
                pixelY_ = std::clamp(static_cast<int>((mouse.y - origin.y) / scale), 0, static_cast<int>(resource.height) - 1);
                pixelValid_ = false;
            }
        } else ImGui::TextWrapped("Preview unavailable for this resource. Metadata and raw pixel inspection remain available where supported.");
        ImGui::TextWrapped("Click the captured image to choose a texel, then Read pixel. This does not select or modify scene objects.");
        ImGui::SetNextItemWidth(100);
        if (ImGui::InputInt("Pixel X", &pixelX_)) pixelValid_ = false;
        ImGui::SetNextItemWidth(100);
        if (ImGui::InputInt("Pixel Y", &pixelY_)) pixelValid_ = false;
        pixelX_ = std::clamp(pixelX_, 0, std::max(0, static_cast<int>(resource.width) - 1));
        pixelY_ = std::clamp(pixelY_, 0, std::max(0, static_cast<int>(resource.height) - 1));
        if (ImGui::Button("Read pixel")) {
            pixelValid_ = renderer.readCapturedPixel(static_cast<std::size_t>(resource_),
                static_cast<std::uint32_t>(pixelX_), static_cast<std::uint32_t>(pixelY_), pixel_, captureMessage_);
            if (pixelValid_) captureMessage_.clear();
        }
        ImGui::TextWrapped("Explicit readback may wait for the GPU; it is performed only on request.");
        if (pixelValid_) ImGui::TextWrapped("Frame %llu | (%d, %d) | RGBA = %.7g, %.7g, %.7g, %.7g",
            static_cast<unsigned long long>(capture.frameId), pixelX_, pixelY_, pixel_[0], pixel_[1], pixel_[2], pixel_[3]);
    } else ImGui::TextDisabled("This capture has no inspectable texture resources.");
    ImGui::SeparatorText("Captured draw events");
    eventTable(capture.events);
}
} // namespace rae::editor
