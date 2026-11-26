#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <commdlg.h>
#include <RestArtEngine/Editor/DiagnosticsPanel.h>
#include <RestArtEngine/Core/TraceProfiler.h>
#include <RestArtEngine/Core/RuntimeDiagnostics.h>
#include <RestArtEngine/Renderer/RenderDiagnostics.h>
#include <RestArtEngine/Renderer/SceneRenderer.h>
#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <limits>
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

std::filesystem::path captureFileDialog(bool save, std::string& error) {
    wchar_t filename[32768] = L"frame.raframe";
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFile = filename;
    dialog.nMaxFile = static_cast<DWORD>(std::size(filename));
    dialog.lpstrFilter = L"RestArtEngine frame (*.raframe)\0*.raframe\0All files (*.*)\0*.*\0\0";
    dialog.lpstrDefExt = L"raframe";
    dialog.lpstrTitle = save ? L"Save frozen frame package (not command replay)" : L"Open frozen frame package";
    dialog.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST |
        (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    error.clear();
    if (save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog)) return filename;
    if (const DWORD code = CommDlgExtendedError())
        error = "Frame file dialog failed (" + std::to_string(code) + ").";
    return {};
}

bool containsFolded(std::string_view text, std::string_view filter) {
    const auto fold = [](unsigned char value) {
        return value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
    };
    return filter.empty() || std::search(text.begin(), text.end(), filter.begin(), filter.end(),
        [&](unsigned char a, unsigned char b) { return fold(a) == fold(b); }) != text.end();
}

bool matchesEvent(const renderer::RenderDrawEvent& event, const char* filter) {
    if (!filter[0]) return true;
    return containsFolded(event.pass, filter) || containsFolded(event.command, filter) ||
        containsFolded(event.actorName, filter) || containsFolded(event.meshName, filter) ||
        containsFolded(event.materialName, filter) || containsFolded(event.vertexShader, filter) ||
        containsFolded(event.pixelShader, filter) || containsFolded(std::to_string(event.eventId), filter);
}

void eventTable(const std::vector<renderer::RenderDrawEvent>& events, int& selected, const char* filter) {
    if (events.empty()) { ImGui::TextDisabled("No draw events in this frame."); return; }
    std::size_t matching = 0;
    if (ImGui::BeginTable("Draw events", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY, {0, 200})) {
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Event / command", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Actor ID", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Mesh ID", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Material ID", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Indices / vertices", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("Shader revision", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Actor / mesh / material", ImGuiTableColumnFlags_WidthFixed, 260);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (std::size_t index = 0; index < events.size(); ++index) {
            const auto& event = events[index];
            if (!matchesEvent(event, filter)) continue;
            ++matching;
            ImGui::PushID(static_cast<int>(index));
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            if (ImGui::Selectable(event.pass.c_str(), selected == static_cast<int>(index), ImGuiSelectableFlags_SpanAllColumns))
                selected = static_cast<int>(index);
            ImGui::TableNextColumn(); ImGui::Text("%llu  %s", static_cast<unsigned long long>(event.eventId), event.command.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%016llX", static_cast<unsigned long long>(event.actorId));
            ImGui::TableNextColumn(); ImGui::Text("%016llX", static_cast<unsigned long long>(event.meshId));
            ImGui::TableNextColumn(); ImGui::Text("%016llX", static_cast<unsigned long long>(event.materialId));
            ImGui::TableNextColumn(); ImGui::Text("%u / %u", event.indexCount, event.vertexCount);
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(event.shaderRevision));
            ImGui::TableNextColumn();
            ImGui::Text("%s / %s / %s", event.actorName.c_str(), event.meshName.c_str(), event.materialName.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (filter[0]) ImGui::TextWrapped("%zu / %zu events match the filter.", matching, events.size());
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

void drawEventDetails(const renderer::RenderDrawEvent& event) {
    ImGui::SeparatorText("Selected draw state");
    ImGui::TextWrapped("Event %llu | Pass %u: %s | %s", static_cast<unsigned long long>(event.eventId),
        event.passIndex, event.pass.c_str(), event.command.c_str());
    ImGui::TextWrapped("Actor: %s (%016llX)", event.actorName.c_str(), static_cast<unsigned long long>(event.actorId));
    ImGui::TextWrapped("Mesh: %s (%016llX)", event.meshName.c_str(), static_cast<unsigned long long>(event.meshId));
    ImGui::TextWrapped("Material: %s (%016llX) | slot %u", event.materialName.c_str(),
        static_cast<unsigned long long>(event.materialId), event.materialSlot);
    ImGui::TextWrapped("Indices %u | vertices %u | first %u | base vertex %d | Shader revision %llu",
        event.indexCount, event.vertexCount, event.firstIndex, event.baseVertex,
        static_cast<unsigned long long>(event.shaderRevision));
    if (!event.detailedState) {
        ImGui::TextWrapped("Detailed RHI binding state was not recorded for this event. Missing state is not a default or inferred state.");
        ImGui::TextWrapped("Partial state reason: %s", event.pipelineState.empty() ?
            "No detailed-state reason was retained." : event.pipelineState.c_str());
        ImGui::TextWrapped("Viewport state is unavailable for this partial event. Missing Shader fields do not describe an unbound Shader.");
        if (!event.vertexShader.empty()) ImGui::TextWrapped("Recorded vertex shader: %s", event.vertexShader.c_str());
        if (!event.pixelShader.empty()) ImGui::TextWrapped("Recorded pixel shader: %s", event.pixelShader.c_str());
        ImGui::TextWrapped("Retained partial data: %zu resource bindings, %zu constant buffers. Omitted bindings and constants were not recorded; they are not known to be unbound.",
            event.bindings.size(), event.constants.size());
    } else {
        ImGui::TextWrapped("Viewport: (%.1f, %.1f), %.1f x %.1f | depth %.5g .. %.5g",
            event.viewport[0], event.viewport[1], event.viewport[2], event.viewport[3], event.viewport[4], event.viewport[5]);
        ImGui::TextWrapped("Vertex shader: %s", event.vertexShader.c_str());
        ImGui::TextWrapped("Pixel shader: %s", event.pixelShader.c_str());
        ImGui::TextWrapped("Pipeline: %s", event.pipelineState.c_str());
    }
    if ((event.detailedState || !event.bindings.empty()) && ImGui::TreeNode("Bound resources")) {
        if (event.bindings.empty()) ImGui::TextDisabled("No resource bindings were recorded.");
        if (!event.bindings.empty() && ImGui::BeginTable("Resource bindings", 8,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY, {0, 180})) {
            for (const auto* label : {"Stage / kind", "Slot", "Resource", "Format", "Size / layer", "Stride", "Offset", "Bytes"})
                ImGui::TableSetupColumn(label, ImGuiTableColumnFlags_WidthFixed, std::string_view(label) == "Resource" ? 210.0f : 120.0f);
            ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
            for (const auto& binding : event.bindings) {
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::Text("%s / %s", binding.stage.c_str(), binding.kind.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%u", binding.slot);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(binding.name.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(binding.format.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%u x %u / %u", binding.width, binding.height, binding.layer);
                ImGui::TableNextColumn(); ImGui::Text("%u", binding.stride);
                ImGui::TableNextColumn(); ImGui::Text("%u", binding.offset);
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(binding.bytes));
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
    if ((event.detailedState || !event.constants.empty()) && ImGui::TreeNode("Constant buffer contents")) {
        ImGui::TextWrapped("Immutable bytes at this draw. Float columns reinterpret raw IEEE-754 float32 bits; integer fields and padding are not typed Shader variables.");
        if (event.constants.empty()) ImGui::TextDisabled("No constant buffers were recorded.");
        for (std::size_t index = 0; index < event.constants.size(); ++index) {
            const auto& constant = event.constants[index];
            ImGui::PushID(static_cast<int>(index));
            const std::string title = constant.stage + " b" + std::to_string(constant.slot) + " " + constant.name +
                " | " + std::to_string(constant.bytes.size()) + " bytes | version " + std::to_string(constant.version);
            if (ImGui::TreeNode("##Constant", "%s", title.c_str())) {
                if (ImGui::Button("Copy hex bytes")) {
                    static constexpr char digits[] = "0123456789ABCDEF";
                    std::string text; text.reserve(constant.bytes.size() * 3);
                    for (const auto byte : constant.bytes) {
                        text += digits[byte >> 4]; text += digits[byte & 15]; text += ' ';
                    }
                    ImGui::SetClipboardText(text.c_str());
                }
                if (ImGui::BeginTable("Constant bytes", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, {0, 180})) {
                    ImGui::TableSetupColumn("Byte offset", ImGuiTableColumnFlags_WidthFixed, 90);
                    ImGui::TableSetupColumn("Raw hex", ImGuiTableColumnFlags_WidthFixed, 350);
                    ImGui::TableSetupColumn("Float32 interpretation", ImGuiTableColumnFlags_WidthFixed, 360);
                    ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
                    const auto rows = static_cast<int>(std::min<std::size_t>((constant.bytes.size() + 15) / 16,
                        static_cast<std::size_t>(std::numeric_limits<int>::max())));
                    ImGuiListClipper clipper; clipper.Begin(rows);
                    while (clipper.Step()) for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                        const auto offset = static_cast<std::size_t>(row) * 16;
                        std::string hex, floats;
                        for (std::size_t byte = offset; byte < std::min(offset + 16, constant.bytes.size()); ++byte) {
                            char text[4]{}; std::snprintf(text, sizeof(text), "%02X ", static_cast<unsigned>(constant.bytes[byte])); hex += text;
                        }
                        for (std::size_t column = 0; column < 4; ++column) {
                            const auto at = offset + column * sizeof(float);
                            if (column) floats += " | ";
                            if (at + sizeof(float) <= constant.bytes.size()) {
                                float value = 0; std::memcpy(&value, constant.bytes.data() + at, sizeof(value));
                                char text[48]{}; std::snprintf(text, sizeof(text), "%.7g", static_cast<double>(value)); floats += text;
                            } else floats += "--";
                        }
                        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("0x%04llX", static_cast<unsigned long long>(offset));
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(hex.c_str());
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(floats.c_str());
                    }
                    ImGui::EndTable();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
}

struct ScopeAggregate {
    std::size_t calls = 0;
    double inclusiveUs = 0, retainedSelfUs = 0, maximumUs = 0;
};

// Subtract only the union of retained direct children on the same thread.
// If the recording/window is incomplete this is an upper bound, not exact self time.
double retainedSelf(const diagnostics::TraceEvent& event, const std::vector<const diagnostics::TraceEvent*>& events) {
    std::vector<std::pair<double, double>> children;
    const double end = event.startUs + std::max(0.0, event.durationUs);
    for (const auto* child : events) {
        if (child->parentEventId != event.eventId || child->threadId != event.threadId) continue;
        const double begin = std::max(child->startUs, event.startUs);
        const double childEnd = std::min(child->startUs + std::max(0.0, child->durationUs), end);
        if (childEnd > begin) children.emplace_back(begin, childEnd);
    }
    std::sort(children.begin(), children.end());
    double covered = 0, lastEnd = event.startUs;
    for (const auto& interval : children) {
        covered += std::max(0.0, interval.second - std::max(interval.first, lastEnd));
        lastEnd = std::max(lastEnd, interval.second);
    }
    return std::max(0.0, event.durationUs - covered);
}

void traceDetails(const diagnostics::TraceSnapshot& trace, std::uint64_t frameId,
                  std::uint64_t& selectedEvent, float zoom) {
    std::vector<const diagnostics::TraceEvent*> events;
    std::map<std::uint64_t, const diagnostics::TraceEvent*> identities;
    for (const auto& event : trace.events) if (event.frameId == frameId) {
        events.push_back(&event); identities.emplace(event.eventId, &event);
    }
    if (events.empty()) { ImGui::TextWrapped("No retained CPU scopes for this frame. An unrecorded or truncated scope is not zero work."); return; }
    std::sort(events.begin(), events.end(), [](const auto* a, const auto* b) {
        return a->startUs == b->startUs ? a->durationUs > b->durationUs : a->startUs < b->startUs;
    });
    bool incomplete = trace.droppedEvents != 0 || trace.eventCount > trace.events.size();
    std::map<std::uint64_t, std::size_t> depths;
    std::map<std::uint64_t, std::size_t> threadDepths;
    std::map<std::uint64_t, std::vector<const diagnostics::TraceEvent*>> children;
    for (const auto* event : events) if (event->parentEventId) children[event->parentEventId].push_back(event);
    std::map<std::uint64_t, double> selfTimes;
    bool depthClamped = false;
    double begin = events.front()->startUs, end = begin + 1.0;
    for (const auto* event : events) {
        std::size_t depth = 0;
        auto parent = event->parentEventId;
        std::set<std::uint64_t> visited;
        while (parent && depth < 32 && visited.insert(parent).second) {
            const auto it = identities.find(parent);
            if (it == identities.end()) { incomplete = true; break; }
            if (it->second->threadId != event->threadId) break;
            ++depth; parent = it->second->parentEventId;
        }
        depthClamped |= depth > 8;
        depths.emplace(event->eventId, std::min<std::size_t>(depth, 8));
        threadDepths[event->threadId] = std::max(threadDepths[event->threadId], std::min<std::size_t>(depth, 8));
        end = std::max(end, event->startUs + std::max(0.0, event->durationUs));
        const auto retainedChildren = children.find(event->eventId);
        selfTimes.emplace(event->eventId, retainedChildren == children.end() ? std::max(0.0, event->durationUs) :
            retainedSelf(*event, retainedChildren->second));
    }
    ImGui::TextWrapped("CPU thread timeline | frame %llu | retained span %.3f ms",
        static_cast<unsigned long long>(frameId), (end - begin) / 1000.0);
    ImGui::TextWrapped("Bars are actual CPU wall-clock scopes, including waits. GPU timing has a separate clock; these tracks do not imply CPU/GPU overlap.");
    if (depthClamped) ImGui::TextWrapped("Nesting beyond depth 8 shares the last visual lane. The scope table and aggregates retain all recorded depths.");
    const float rowHeight = ImGui::GetTextLineHeight() + 6.0f;
    const float labelWidth = ImGui::GetFontSize() * 7;
    if (ImGui::BeginChild("CPU timeline", {0, 230}, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float chartWidth = std::max(200.0f, ImGui::GetContentRegionAvail().x - labelWidth) * std::clamp(zoom, 1.0f, 16.0f);
        auto* list = ImGui::GetWindowDrawList();
        std::map<std::uint64_t, float> tracks;
        float nextY = origin.y + rowHeight;
        for (const auto& [thread, depth] : threadDepths) {
            tracks.emplace(thread, nextY);
            const std::string label = "Thread " + std::to_string(thread);
            list->AddText({origin.x, nextY + 2}, ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
            nextY += rowHeight * static_cast<float>(depth + 1) + 7.0f;
        }
        for (int tick = 0; tick <= 5; ++tick) {
            const float x = origin.x + labelWidth + chartWidth * static_cast<float>(tick) / 5.0f;
            char label[48]{};
            std::snprintf(label, sizeof(label), "%.3f ms", (end - begin) * tick / 5000.0);
            list->AddText({x + 2, origin.y}, ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
            list->AddLine({x, origin.y + rowHeight}, {x, nextY}, ImGui::GetColorU32(ImGuiCol_Border));
        }
        for (const auto* event : events) {
            const float left = origin.x + labelWidth + static_cast<float>((event->startUs - begin) / (end - begin)) * chartWidth;
            const float width = std::max(2.0f, static_cast<float>(std::max(0.0, event->durationUs) / (end - begin)) * chartWidth);
            const float top = tracks.at(event->threadId) + rowHeight * static_cast<float>(depths.at(event->eventId));
            ImGui::SetCursorScreenPos({left, top});
            ImGui::PushID(std::to_string(event->eventId).c_str());
            ImGui::InvisibleButton("Scope", {width, rowHeight - 2});
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) selectedEvent = event->eventId;
            list->AddRectFilled({left, top}, {left + width, top + rowHeight - 2},
                ImGui::GetColorU32(selectedEvent == event->eventId ? ImGuiCol_PlotHistogram :
                    hovered ? ImGuiCol_HeaderHovered : ImGuiCol_Header), 2);
            if (width > ImGui::CalcTextSize(event->name.c_str()).x + 6)
                list->AddText({left + 3, top + 2}, ImGui::GetColorU32(ImGuiCol_Text), event->name.c_str());
            if (hovered) ImGui::SetTooltip("%s / %s\nEvent %llu | Parent %llu | Thread %llu\nStart %.3f us | Inclusive %.3f ms",
                event->name.c_str(), event->category.c_str(), static_cast<unsigned long long>(event->eventId),
                static_cast<unsigned long long>(event->parentEventId), static_cast<unsigned long long>(event->threadId),
                event->startUs, event->durationUs / 1000.0);
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy({labelWidth + chartWidth + 60, std::max(rowHeight, nextY - origin.y)});
    }
    ImGui::EndChild();
    if (const auto it = identities.find(selectedEvent); it != identities.end()) {
        const auto& event = *it->second;
        ImGui::TextWrapped("Selected scope: %s / %s | Event %llu | Parent %llu | Thread %llu",
            event.name.c_str(), event.category.c_str(), static_cast<unsigned long long>(event.eventId),
            static_cast<unsigned long long>(event.parentEventId), static_cast<unsigned long long>(event.threadId));
        ImGui::TextWrapped("Start %.3f us | inclusive %.3f ms | retained self %.3f ms%s",
            event.startUs, event.durationUs / 1000.0, selfTimes.at(event.eventId) / 1000.0,
            incomplete ? " (upper bound: incomplete recording/window)" : "");
    }
    ImGui::TextWrapped(incomplete ?
        "Incomplete recording/window: scope counts and inclusive sums cover retained events only. Self values are upper bounds after subtracting retained direct children; they are not exact exclusive CPU time." :
        "Scope aggregation uses retained events in this frame. Inclusive sums include nested work and can exceed frame duration. Self subtracts the union of direct child scopes on the same thread.");
    std::map<std::pair<std::string, std::string>, ScopeAggregate> aggregates;
    for (const auto* event : events) {
        auto& aggregate = aggregates[{event->name, event->category}];
        ++aggregate.calls; aggregate.inclusiveUs += event->durationUs;
        aggregate.retainedSelfUs += selfTimes.at(event->eventId);
        aggregate.maximumUs = std::max(aggregate.maximumUs, event->durationUs);
    }
    std::vector<decltype(aggregates)::const_iterator> ordered;
    for (auto it = aggregates.cbegin(); it != aggregates.cend(); ++it) ordered.push_back(it);
    std::sort(ordered.begin(), ordered.end(), [](auto a, auto b) { return a->second.inclusiveUs > b->second.inclusiveUs; });
    if (ImGui::BeginTable("Scope aggregate", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY, {0, 175})) {
        for (const auto* label : {"Scope", "Category", "Calls", "Inclusive ms", "Retained self ms", "Max ms"})
            ImGui::TableSetupColumn(label, ImGuiTableColumnFlags_WidthFixed, std::string_view(label) == "Scope" ? 220.0f : 130.0f);
        ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
        for (const auto it : ordered) {
            const auto& aggregate = it->second;
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(it->first.first.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(it->first.second.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%zu", aggregate.calls);
            ImGui::TableNextColumn(); ImGui::Text("%.3f", aggregate.inclusiveUs / 1000.0);
            ImGui::TableNextColumn(); ImGui::Text("%.3f%s", aggregate.retainedSelfUs / 1000.0, incomplete ? " *" : "");
            ImGui::TableNextColumn(); ImGui::Text("%.3f", aggregate.maximumUs / 1000.0);
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
        if (ImGui::BeginTabItem("Runtime", nullptr, flags(DiagnosticsTab::Runtime))) {
            tab_ = DiagnosticsTab::Runtime;
            drawRuntime(renderer);
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
        if (ImGui::Button("Record CPU trace")) { profiler.start(); selectedTraceFrame_ = selectedTraceEvent_ = 0; traceMessage_.clear(); }
    } else if (ImGui::Button("Stop CPU trace")) profiler.stop();
    nextButton("Clear trace");
    if (ImGui::Button("Clear trace")) { profiler.clear(); selectedTraceFrame_ = selectedTraceEvent_ = 0; traceMessage_.clear(); }
    nextButton("Export trace JSON...");
    if (ImGui::Button("Export trace JSON...")) {
        const auto path = saveJsonDialog(L"Export CPU trace", L"trace.json", traceMessage_);
        if (!path.empty() && profiler.save(path, traceMessage_)) traceMessage_ = "CPU trace JSON exported.";
    }
    const auto trace = profiler.snapshot(4096);
    ImGui::TextWrapped("CPU trace: %s | %zu / %zu events | %llu dropped", trace.recording ? "Recording" : "Stopped",
        trace.eventCount, trace.eventCapacity, static_cast<unsigned long long>(trace.droppedEvents));
    ImGui::TextWrapped("Frame duration is main-loop wall time, including Present / VSync waits. CPU scopes and GPU timings measure different work.");
    ImGui::TextWrapped("The CPU views use up to 4096 recent retained events; export includes the entire retained recording. This is instrumented tracing, not a sampling profiler.");
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
            if (ImGui::Selectable("Latest recorded complete frame", selectedTraceFrame_ == 0)) {
                selectedTraceFrame_ = 0; selectedTraceEvent_ = 0;
            }
            for (const auto id : recordedFrames) {
                const std::string frameLabel = "Frame " + std::to_string(id);
                if (ImGui::Selectable(frameLabel.c_str(), selectedTraceFrame_ == id)) {
                    selectedTraceFrame_ = id; selectedTraceEvent_ = 0;
                }
            }
            ImGui::EndCombo();
        }
        const auto frameId = selectedTraceFrame_ ? selectedTraceFrame_ : recordedFrames.front();
        if (ImGui::CollapsingHeader("CPU timeline and scope aggregates")) {
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##Timeline zoom", &timelineZoom_, 1.0f, 16.0f, "Timeline zoom %.1fx");
            traceDetails(trace, frameId, selectedTraceEvent_, timelineZoom_);
        }
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
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::PushID(std::to_string(it->eventId).c_str());
                if (ImGui::Selectable(it->name.c_str(), selectedTraceEvent_ == it->eventId)) selectedTraceEvent_ = it->eventId;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Event %llu | Parent %llu | Start %.3f us",
                    static_cast<unsigned long long>(it->eventId), static_cast<unsigned long long>(it->parentEventId), static_cast<double>(it->startUs));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(it->category.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.3f", it->durationUs / 1000.0);
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(it->threadId));
                ImGui::PopID();
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

    // Declared composition for the latest main-scene frame. This is the same
    // list the renderer executed, not a hand-maintained description.
    ImGui::SeparatorText("Render pipeline");
    const auto& pipeline = renderer.activePipeline();
    ImGui::TextWrapped("%s composition: %zu declared passes (latest main-scene frame).",
        renderer::toString(pipeline.path), pipeline.passes.size());
    if (!pipeline.passes.empty() &&
        ImGui::BeginTable("Pipeline passes", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthFixed, 190);
        ImGui::TableSetupColumn("Reads", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Writes", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& pass : pipeline.passes) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(std::string(pass.name).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(renderer::describePipelineResources(pass.reads).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(renderer::describePipelineResources(pass.writes).c_str());
        }
        ImGui::EndTable();
    }
    const auto& features = renderer.renderFeatures();
    ImGui::TextWrapped("%zu features contribute pixels. Requests are reported next to the effective state; "
        "a capability check never rewrites the request.",
        features.effectiveCount());
    if (ImGui::BeginTable("Render features", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, {0, 150})) {
        ImGui::TableSetupColumn("Feature", ImGuiTableColumnFlags_WidthFixed, 190);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (const auto& status : features.features) {
            if (status.state == renderer::FeatureState::Off) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(renderer::toString(status.feature));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(renderer::toString(status.state));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(status.reason.c_str());
            if (!status.reason.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", status.reason.c_str());
        }
        ImGui::EndTable();
    }
    const auto gi = renderer.globalIlluminationStatus();
    const auto dxr = renderer.hardwareTransportTiming();
    if (dxr.frameId && ((dxr.probeUpdate && gi.usesHardwareProbeTrace) ||
        (dxr.reflectionOnly && gi.usesHardwareReflections))) {
        const char* work = dxr.probeUpdate && dxr.reflectionOnly ? "probes + reflections" :
            dxr.probeUpdate ? "probes" : "reflections";
        ImGui::TextWrapped("DXR %s | %.3f ms | GPU frame %llu", work, dxr.gpuMs,
            static_cast<unsigned long long>(dxr.frameId));
    }
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
    if (ImGui::CollapsingHeader("Capture settings")) {
        auto options = renderer.frameCaptureOptions();
        bool changed = false;
        ImGui::TextUnformatted("Capture boundary");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##Capture pass", options.pass.empty() ? "Final frame outputs" : options.pass.c_str())) {
            if (ImGui::Selectable("Final frame outputs", options.pass.empty())) {
                options.pass.clear(); changed = true;
            }
            for (const auto& pass : renderer.activePipeline().passes) {
                const std::string name(pass.name);
                if (ImGui::Selectable(name.c_str(), options.pass == name)) {
                    options.pass = name; changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::BeginDisabled(options.pass.empty());
        changed |= ImGui::Checkbox("Capture before and after selected pass", &options.beforeAfter);
        ImGui::EndDisabled();
        int budget = static_cast<int>(std::clamp<std::uint64_t>(options.budgetBytes / (1024 * 1024), 16, 256));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##Capture budget", &budget, 16, 256, "Resource budget %d MiB")) {
            options.budgetBytes = static_cast<std::uint64_t>(budget) * 1024 * 1024; changed = true;
        }
        if (changed) {
            try { renderer.setFrameCaptureOptions(options); }
            catch (const std::exception& error) { captureMessage_ = error.what(); }
        }
        ImGui::TextWrapped("The next main-scene frame captures the chosen boundary. Only initialized before-images are retained. The budget bounds frozen resources, not total process or GPU peak memory.");
    }
    if (ImGui::Button("Capture next scene frame")) requestCapture(renderer);
    nextButton("Clear capture");
    if (ImGui::Button("Clear capture")) {
        renderer.clearFrameCapture(); resource_ = 0; selectedDraw_ = -1; pixelValid_ = false; captureMessage_.clear();
    }
    const auto& capture = renderer.frameCapture();
    nextButton("Export metadata JSON...");
    ImGui::BeginDisabled(!capture.valid);
    if (ImGui::Button("Export metadata JSON...")) {
        const auto path = saveJsonDialog(L"Export frame metadata (no images)", L"frame-metadata.json", captureMessage_);
        if (!path.empty() && renderer.exportFrameCapture(path, captureMessage_)) captureMessage_ = "Frame metadata JSON exported; images are not included.";
    }
    ImGui::EndDisabled();
    nextButton("Save frame package...");
    ImGui::BeginDisabled(!capture.valid);
    if (ImGui::Button("Save frame package...")) {
        const auto path = captureFileDialog(true, captureMessage_);
        if (!path.empty() && renderer.saveFrameCapture(path, captureMessage_))
            captureMessage_ = "Frozen frame package saved with resource bytes, events and constants. It is not API replay.";
    }
    ImGui::EndDisabled();
    nextButton("Open frame package...");
    if (ImGui::Button("Open frame package...")) {
        const auto path = captureFileDialog(false, captureMessage_);
        if (!path.empty() && renderer.loadFrameCapture(path, captureMessage_)) {
            displayedCaptureFrame_ = 0; resource_ = 0; selectedDraw_ = -1; capturedTraceEvent_ = 0;
            pixelValid_ = false; eventFilter_.fill(0);
            captureMessage_ = "Offline frozen frame opened. Live rendering and scene data are unchanged.";
        }
    }
    ImGui::TextWrapped("%s", capture.status.c_str());
    ImGui::TextWrapped("Engine-owned frame snapshot. Metadata export contains no images and does not provide API replay.");
    if (!captureMessage_.empty()) ImGui::TextWrapped("%s", captureMessage_.c_str());
    if (!capture.valid) return;
    if (displayedCaptureFrame_ != capture.frameId) {
        displayedCaptureFrame_ = capture.frameId;
        resource_ = 0; selectedDraw_ = -1; capturedTraceEvent_ = 0;
        pixelX_ = pixelY_ = 0; pixelValid_ = false;
    }
    ImGui::TextWrapped("Captured frame %llu | %s | shader revision %llu | CPU submit %.3f ms",
        static_cast<unsigned long long>(capture.frameId), capture.renderPath.c_str(),
        static_cast<unsigned long long>(capture.shaderRevision), capture.cpuSubmitMs);
    ImGui::TextWrapped("%s | backend %s | %u x %u | %zu events | %llu dropped",
        capture.offline ? "Offline capture" : "Session capture", capture.backend.c_str(), capture.width, capture.height,
        capture.events.size(), static_cast<unsigned long long>(capture.droppedEvents));
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
        ImGui::TextWrapped("Pass: %s | boundary: %s | layer %u | event %llu",
            resource.pass.empty() ? "Final frame outputs" : resource.pass.c_str(), resource.boundary.c_str(), resource.layer,
            static_cast<unsigned long long>(resource.eventId));
        ImGui::TextWrapped("The image belongs to this recorded capture boundary. Selecting a draw below does not reconstruct that draw's output or pixel history.");
        ImGui::TextWrapped("%s | %u x %u | %.2f MiB", resource.format.c_str(), resource.width, resource.height,
            static_cast<double>(resource.bytes) / (1024.0 * 1024.0));
        ImGui::TextUnformatted("Display channel");
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##Display channel", &channel_, "RGB\0R\0G\0B\0A\0");
        ImGui::TextWrapped("Display preview is mapped for visibility; Read pixel reports stored numeric values before display mapping.");
        if (resource.displayKind == 5)
            ImGui::TextWrapped("Shadow depth preview: raw light-space device depth in [0, 1]. Camera near/far and projection are not used to decode shadow layers.");
        else if (resource.name.find("Depth") != std::string::npos || resource.name.find("depth") != std::string::npos)
            ImGui::TextWrapped("Camera depth preview: linear view-space Z / 100 m, using captured %s projection (near %.5g, far %.5g). Raw pixel depth is device depth in [0, 1].",
                capture.orthographic ? "orthographic" : "perspective", static_cast<double>(capture.nearPlane), static_cast<double>(capture.farPlane));
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
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##Event filter", "Filter pass, actor, mesh, material, Shader or event ID...",
        eventFilter_.data(), eventFilter_.size());
    eventTable(capture.events, selectedDraw_, eventFilter_.data());
    if (selectedDraw_ >= 0 && static_cast<std::size_t>(selectedDraw_) < capture.events.size())
        drawEventDetails(capture.events[static_cast<std::size_t>(selectedDraw_)]);
    else ImGui::TextWrapped("Select a draw to inspect recorded pipeline, bindings and constant bytes. Resource images retain their own boundary identity.");

    if (ImGui::CollapsingHeader("Captured pass timings and resources")) {
        ImGui::TextWrapped("CPU values measure submission wall time. The captured pass list and resource dependencies belong to the frozen frame.");
        timingTable(capture.passes, false);
        if (capture.gpuStatus == renderer::GpuTimingStatus::Ready) {
            ImGui::TextWrapped("Captured GPU sample: frame %llu | %.3f ms, matched to this captured frame. This measures the main-scene queue interval, not total GPU busy time, hybrid DXR work or UI rendering.",
                static_cast<unsigned long long>(capture.gpuFrameId), capture.gpuTotalMs);
            timingTable(capture.gpuPasses, true);
        } else ImGui::TextWrapped("The package has no ready GPU timing sample. Pending, invalid or unavailable measurements are not zero cost.");
        for (std::size_t index = 0; index < capture.passes.size(); ++index) {
            const auto& pass = capture.passes[index];
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::TreeNode(pass.name.c_str())) {
                ImGui::TextWrapped("CPU offset %.3f ms | event range %llu .. %llu", pass.cpuStartMs,
                    static_cast<unsigned long long>(pass.firstEventId), static_cast<unsigned long long>(pass.lastEventId));
                ImGui::TextUnformatted("Recorded reads:");
                if (pass.reads.empty()) ImGui::TextDisabled("None recorded");
                for (const auto& name : pass.reads) ImGui::BulletText("%s", name.c_str());
                ImGui::TextUnformatted("Recorded writes:");
                if (pass.writes.empty()) ImGui::TextDisabled("None recorded");
                for (const auto& name : pass.writes) ImGui::BulletText("%s", name.c_str());
                if (ImGui::Button("Filter events to this pass")) {
                    std::snprintf(eventFilter_.data(), eventFilter_.size(), "%s", pass.name.c_str());
                    selectedDraw_ = -1;
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
    if (ImGui::CollapsingHeader("Captured CPU trace")) {
        ImGui::TextWrapped("Retained CPU scopes were copied with the capture. In-flight/unrecorded scopes may be absent; this is not a CPU call-stack snapshot.");
        ImGui::PushID("Captured trace");
        traceDetails(capture.cpuTrace, capture.frameId, capturedTraceEvent_, 1.0f);
        ImGui::PopID();
    }
    if (ImGui::CollapsingHeader("Frozen Shader sources")) {
        ImGui::TextWrapped("These two source texts were stored with this frame: material Shader source and scene pipeline Shader source. They are not fully expanded include trees, executable bytecode or Shader stepping/debugging state.");
        for (const auto& source : {std::pair{"Material Shader source", &capture.materialShaderSource},
            std::pair{"Pipeline Shader source", &capture.pipelineShaderSource}}) {
            ImGui::PushID(source.first);
            if (ImGui::TreeNode(source.first)) {
                if (source.second->empty()) ImGui::TextDisabled("Source text was not retained in this capture.");
                else {
                    if (ImGui::Button("Copy source")) ImGui::SetClipboardText(source.second->c_str());
                    if (ImGui::BeginChild("Source text", {0, 200}, ImGuiChildFlags_Borders,
                        ImGuiWindowFlags_HorizontalScrollbar)) ImGui::TextUnformatted(source.second->c_str());
                    ImGui::EndChild();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
}

void DiagnosticsPanel::drawRuntime(renderer::SceneRenderer& renderer) {
    const auto& backend = renderer.mainBackendSelection();
    ImGui::SeparatorText("Runtime and graphics validation");
    ImGui::TextWrapped("Requested backend: %s | active: %s | fallback: %s",
        renderer::mainRenderBackendKey(backend.requested.preference),
        renderer.mainBackendReady() ? renderer::mainRenderBackendKey(backend.effective) : "Not initialized",
        backend.fallback ? "yes" : "no");
    ImGui::TextWrapped("%s", backend.reason);
    ImGui::TextWrapped("Graphics validation: %s | error count %u", renderer.validationEnabled() ? "Enabled" : "Unavailable",
        renderer.validationErrorCount());
    ImGui::TextWrapped("Program debugging here means structured engine messages, Shader/command errors and validation. CPU breakpoints, stack inspection and sampling/allocation profiling are not provided by this tool.");
    const auto runtime = diagnostics::runtimeDiagnostics().snapshot();
    ImGui::SeparatorText("Memory and resource counters");
    if (runtime.processMemoryAvailable)
        ImGui::TextWrapped("Process working set: %.2f MiB | private committed memory: %.2f MiB",
            static_cast<double>(runtime.workingSetBytes) / (1024.0 * 1024.0),
            static_cast<double>(runtime.privateBytes) / (1024.0 * 1024.0));
    else ImGui::TextWrapped("Process memory measurements are unavailable on this platform/session.");
    const auto cache = renderer.cacheStats();
    ImGui::TextWrapped("Mesh cache: %llu meshes, %.2f MiB GPU estimate | software ray cache: %llu triangles, %.2f MiB GPU estimate",
        static_cast<unsigned long long>(cache.meshCount), static_cast<double>(cache.meshGpuBytesEstimate) / (1024.0 * 1024.0),
        static_cast<unsigned long long>(cache.rayTriangleCount), static_cast<double>(cache.rayGpuBytesEstimate) / (1024.0 * 1024.0));
    ImGui::TextWrapped("Cache estimates describe logical allocations. They are not driver residency, total VRAM usage or process memory; these values must not be added together.");
    if (const auto device = renderer.graphicsDevice()) {
        const auto& stats = device->statistics();
        ImGui::TextWrapped("RHI cumulative counters: %llu buffers | %llu textures | %llu Shaders | %llu pipelines",
            static_cast<unsigned long long>(stats.buffersCreated), static_cast<unsigned long long>(stats.texturesCreated),
            static_cast<unsigned long long>(stats.shadersCreated), static_cast<unsigned long long>(stats.pipelinesCreated));
        ImGui::TextWrapped("%llu buffer uploads | %llu passes | %llu draws (%llu indexed)",
            static_cast<unsigned long long>(stats.bufferUploads), static_cast<unsigned long long>(stats.renderPasses),
            static_cast<unsigned long long>(stats.drawCalls), static_cast<unsigned long long>(stats.indexedDrawCalls));
        ImGui::TextWrapped("Cumulative creation/submission counters are not live object counts or per-frame measurements.");
    }

    ImGui::SeparatorText("Structured program messages");
    if (ImGui::Button("Clear runtime messages")) diagnostics::runtimeDiagnostics().clear();
    nextButton("Copy runtime messages");
    if (ImGui::Button("Copy runtime messages")) {
        std::ostringstream text;
        for (const auto& message : runtime.messages) {
            const char* level = message.severity == diagnostics::MessageSeverity::Error ? "Error" :
                message.severity == diagnostics::MessageSeverity::Warning ? "Warning" : "Info";
            text << message.id << " [" << level << "] " << message.timeMs << "ms frame=" << message.frameId <<
                " thread=" << message.threadId << " " << message.category << ": " << message.text << '\n';
        }
        ImGui::SetClipboardText(text.str().c_str());
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##Message severity", &severity_, "All severities\0Info\0Warning\0Error\0");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##Runtime filter", "Filter category or message...", runtimeFilter_.data(), runtimeFilter_.size());
    ImGui::TextWrapped("%zu / %zu retained messages | %llu dropped", runtime.messages.size(), diagnostics::RuntimeDiagnostics::capacity,
        static_cast<unsigned long long>(runtime.droppedMessages));
    if (ImGui::BeginTable("Runtime messages", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY, {0, 220})) {
        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Time ms", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("Frame / thread", ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthFixed, 600);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
        for (auto it = runtime.messages.rbegin(); it != runtime.messages.rend(); ++it) {
            const auto& message = *it;
            const int level = message.severity == diagnostics::MessageSeverity::Error ? 3 :
                message.severity == diagnostics::MessageSeverity::Warning ? 2 : 1;
            if (severity_ && severity_ != level) continue;
            if (!containsFolded(message.category, runtimeFilter_.data()) && !containsFolded(message.text, runtimeFilter_.data())) continue;
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            const auto color = level == 3 ? ImVec4(1.0f, .4f, .35f, 1) : level == 2 ? ImVec4(1.0f, .75f, .3f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImGui::TextColored(color, "%s", level == 3 ? "Error" : level == 2 ? "Warning" : "Info");
            ImGui::TableNextColumn(); ImGui::Text("%.3f", message.timeMs);
            ImGui::TableNextColumn(); ImGui::Text("%llu / %llu", static_cast<unsigned long long>(message.frameId),
                static_cast<unsigned long long>(message.threadId));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(message.category.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(message.text.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", message.text.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(message.id));
        }
        ImGui::EndTable();
    }
    if (runtime.messages.empty()) ImGui::TextWrapped("No structured messages have been recorded. This view is not a redirect of every process stdout/stderr stream.");

    ImGui::SeparatorText("Graphics debug messages");
    const auto messages = renderer.debugMessages();
    if (ImGui::Button("Copy graphics messages")) {
        std::string text;
        for (const auto& message : messages) text += message + '\n';
        ImGui::SetClipboardText(text.c_str());
    }
    if (messages.empty()) ImGui::TextWrapped(renderer.validationEnabled() ?
        "No graphics debug messages are available in the retained device queue." : "Graphics debug message collection is unavailable.");
    if (ImGui::BeginChild("Graphics message log", {0, 150}, ImGuiChildFlags_Borders))
        for (const auto& message : messages) ImGui::TextWrapped("%s", message.c_str());
    ImGui::EndChild();
}
} // namespace rae::editor
