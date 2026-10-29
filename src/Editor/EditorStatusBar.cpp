#include <RestArtEngine/Editor/SceneEditor.h>
#include <RestArtEngine/Core/TraceProfiler.h>
#include <imgui.h>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <sstream>

namespace rae::editor {
namespace {
ConsoleResult usage(std::string text) { return {false, "Usage: " + std::move(text)}; }
bool readActorId(const std::string& text, std::uint64_t& id) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), id);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && id != 0;
}
}

void SceneEditor::registerConsole(renderer::SceneRenderer& renderer) {
    console_ = EditorConsole{};
    for (const auto& command : commands_.commands()) {
        if (command.info.group == "hidden") continue;
        const auto id = command.info.id;
        console_.add(id, command.info.label, [this, id](auto args) -> ConsoleResult {
            if (!args.empty()) return usage(id);
            if (!commands_.enabled(id)) return {false, "Command is currently unavailable: " + id};
            if (!commands_.invoke(id)) return {false, "Could not execute " + id};
            return {true, "Executed " + id};
        });
    }
    // Save callbacks report the actual operation result, not just dispatch.
    console_.add("scene.save", "Save only the active scene", [this](auto args) -> ConsoleResult {
        if (!args.empty()) return usage("scene.save");
        const bool ok = saveScene(); return {ok, status_};
    });
    console_.add("save.all", "Apply drafts and save assets, scene and settings", [this, &renderer](auto args) -> ConsoleResult {
        if (!args.empty()) return usage("save.all");
        const bool ok = saveAll(renderer); return {ok, status_};
    });
    console_.add("material.save", "Save the currently selected applied material", [this](auto args) -> ConsoleResult {
        if (!args.empty()) return usage("material.save");
        std::string error;
        if (!saveMaterialAsset(selectedMaterial_, error)) return {false, error};
        return {true, status_};
    });
    console_.add("scene.info", "List scene Actors and their IDs", [this](auto args) -> ConsoleResult {
        if (!args.empty()) return usage("scene.info");
        std::ostringstream text;
        text << world_.actors.size() << " Actors; selected " << selectedActor_ << '\n';
        for (std::size_t i = 0; i < std::min<std::size_t>(world_.actors.size(), 100); ++i)
            text << world_.actors[i].id << "  " << world_.actors[i].name << '\n';
        if (world_.actors.size() > 100) text << "Showing the first 100 Actors.\n";
        return {true, text.str()};
    });
    console_.add("select", "Select an Actor: select <decimal-id>", [this](auto args) -> ConsoleResult {
        std::uint64_t id = 0;
        if (args.size() != 1 || !readActorId(args[0], id)) return usage("select <decimal-id>");
        if (!world_.findActor(id)) return {false, "No Actor with ID " + args[0]};
        selectActor(id, false);
        return {true, "Selected Actor " + args[0]};
    });
    console_.add("content", "Focus the Content browser", [this](auto args) -> ConsoleResult {
        if (!args.empty()) return usage("content");
        dockManager_.requestFocus(EditorPanel::Content); return {true, "Content browser focused."};
    });
    console_.add("project.settings", "Open Project Settings", [this](auto args) -> ConsoleResult {
        if (!args.empty()) return usage("project.settings");
        showProjectSettings(); return {true, "Project Settings opened."};
    });
    console_.add("gc.settings", "Open garbage collection settings", [this](auto args) -> ConsoleResult {
        if (!args.empty()) return usage("gc.settings");
        garbageCollectionOpen_ = true; return {true, "Garbage collection settings opened."};
    });
    console_.add("gc.collect", "Request collection at the end of the frame", [this](auto args) -> ConsoleResult {
        if (!args.empty()) return usage("gc.collect");
        if (!garbageCollector_) return {false, "Garbage collector is not attached."};
        garbageCollector_->requestCollection(); return {true, "Garbage collection requested."};
    });
    console_.add("trace.start", "Start a new CPU trace recording", [](auto args) -> ConsoleResult {
        if (!args.empty()) return usage("trace.start");
        diagnostics::profiler().start(); return {true, "CPU trace recording started."};
    });
    console_.add("trace.stop", "Stop CPU tracing and retain the recording", [](auto args) -> ConsoleResult {
        if (!args.empty()) return usage("trace.stop");
        diagnostics::profiler().stop(); return {true, "CPU trace stopped. Export it from Diagnostics."};
    });
    console_.append("RestArtEngine command console. Enter help to list commands.");
    consoleLastStatus_.clear();
}

ConsoleResult SceneEditor::executeConsoleCommand(std::string_view command) {
    auto result = console_.execute(command);
    if (!result.success) outputLogOpen_ = true;
    return result;
}

void SceneEditor::recordConsoleStatus() {
    if (status_.empty() || status_ == consoleLastStatus_) return;
    consoleLastStatus_ = status_;
    std::string folded = status_;
    for (auto& c : folded) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    const bool error = folded.find("failed") != std::string::npos || folded.find("error") != std::string::npos ||
        folded.find("rejected") != std::string::npos;
    console_.append(status_, error);
}

void SceneEditor::drawConsoleInput(const char* id, float width, bool acceptFocusRequest) {
    const auto callback = [](ImGuiInputTextCallbackData* data) -> int {
        auto& self = *static_cast<SceneEditor*>(data->UserData);
        std::string replacement;
        if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
            if (data->EventKey == ImGuiKey_DownArrow && !self.console_.historyBrowsing()) return 0;
            replacement = data->EventKey == ImGuiKey_UpArrow ? self.console_.historyPrevious(data->Buf) : self.console_.historyNext();
        } else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
            const auto matches = self.console_.complete(data->Buf);
            if (matches.empty()) return 0;
            replacement = matches.front();
            for (const auto& match : matches) {
                std::size_t same = 0;
                while (same < replacement.size() && same < match.size() && replacement[same] == match[same]) ++same;
                replacement.resize(same);
            }
            if (matches.size() == 1) replacement += ' ';
            else {
                std::string choices = "Completions:";
                for (const auto& match : matches) choices += "\n  " + match;
                self.console_.append(choices);
                self.outputLogOpen_ = true;
            }
        } else return 0;
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, replacement.c_str());
        return 0;
    };
    ImGui::SetNextItemWidth(std::max(1.0f, width));
    if (acceptFocusRequest && consoleFocusRequested_) {
        ImGui::SetKeyboardFocusHere(); consoleFocusRequested_ = false;
    }
    const bool enter = ImGui::InputTextWithHint(id, "Enter command...", consoleInput_.data(), consoleInput_.size(),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackHistory,
        callback, this);
    consoleInputActive_ |= ImGui::IsItemActive();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Engine commands | help | Tab: complete | Up/Down: history | Ctrl+`: focus");
    if (enter) {
        executeConsoleCommand(consoleInput_.data());
        consoleInput_.fill(0);
        ImGui::SetKeyboardFocusHere(-1);
        consoleInputActive_ = true;
    }
}

void SceneEditor::drawStatusBar(renderer::SceneRenderer& renderer, float height) {
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowPos({viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - height});
    ImGui::SetNextWindowSize({viewport->WorkSize.x, height});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8, 4});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
    ImGui::Begin("Editor Status Bar", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings);
    const auto& style = ImGui::GetStyle();
    const float spacing = style.ItemSpacing.x;
    const auto buttonWidth = [&](const char* label) { return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2; };
    const bool unsaved = dirty(renderer);
    const char* saveLabel = unsaved ? "Unsaved" : "All Saved";
    const bool directDiagnostics = ImGui::GetContentRegionAvail().x > ImGui::GetFontSize() * 75;
    const float rightWidth = buttonWidth(saveLabel) + buttonWidth("Tools") + spacing +
        (directDiagnostics ? buttonWidth("Diagnostics") + spacing : 0);
    const float leftWidth = buttonWidth("Content") + buttonWidth("Output Log") + spacing * 2;
    const float minInput = ImGui::GetFontSize() * 10;
    const bool compact = ImGui::GetContentRegionAvail().x < leftWidth + rightWidth + minInput + 45;
    if (!compact) {
        if (ImGui::Button("Content")) dockManager_.requestFocus(EditorPanel::Content);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Focus Content browser (Ctrl+Space)");
        ImGui::SameLine();
        if (ImGui::Button("Output Log")) { outputLogOpen_ = !outputLogOpen_; outputLogFocusRequested_ = outputLogOpen_; }
        ImGui::SameLine();
    }
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Cmd"); ImGui::SameLine();
    drawConsoleInput("##command", std::min(ImGui::GetFontSize() * 40, ImGui::GetContentRegionAvail().x - rightWidth - spacing), true);
    ImGui::SameLine(ImGui::GetWindowWidth() - style.WindowPadding.x - rightWidth);
    if (directDiagnostics) {
        if (ImGui::Button("Diagnostics")) showDiagnostics(DiagnosticsTab::Profiler);
        ImGui::SameLine();
    }
    if (ImGui::Button("Tools")) ImGui::OpenPopup("##status-tools");
    if (ImGui::BeginPopup("##status-tools")) {
        if (compact) {
            if (ImGui::MenuItem("Content", "Ctrl+Space")) dockManager_.requestFocus(EditorPanel::Content);
            if (ImGui::MenuItem("Output Log")) { outputLogOpen_ = true; outputLogFocusRequested_ = true; }
            ImGui::Separator();
        }
        if (ImGui::MenuItem("Diagnostics / Profiler")) showDiagnostics(DiagnosticsTab::Profiler);
        if (ImGui::MenuItem("Frame Inspector")) showDiagnostics(DiagnosticsTab::FrameInspector);
        if (ImGui::MenuItem("Garbage collection...")) garbageCollectionOpen_ = true;
        if (ImGui::MenuItem("Project Settings...")) showProjectSettings();
        ImGui::Separator();
        const bool recording = diagnostics::profiler().isRecording();
        if (ImGui::MenuItem(recording ? "Stop CPU trace" : "Start CPU trace"))
            executeConsoleCommand(recording ? "trace.stop" : "trace.start");
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (unsaved) ImGui::PushStyleColor(ImGuiCol_Text, {1.0f, .65f, .25f, 1});
    if (ImGui::Button(saveLabel)) ImGui::OpenPopup("##save-status");
    if (unsaved) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scene, materials, shader and project settings. Click for details and save actions.");
    if (ImGui::BeginPopup("##save-status")) {
        ImGui::Text("Scene: %s", sceneDirty_ ? "unsaved" : "saved");
        ImGui::Text("Applied materials: %zu unsaved", dirtyMaterials_.size());
        ImGui::Text("Material draft: %s", materialDraftDirty_ ? "unapplied" : "clean");
        ImGui::Text("Shader: %s", shaderDraft_ != renderer.shaderSource() || renderer.shaderSource() != shaderSaved_ ? "unsaved" : "saved");
        ImGui::Text("Project settings: %s", projectSettingsDirty_ ? "unsaved" : "saved");
        ImGui::Separator();
        if (ImGui::MenuItem("Save Scene", "Ctrl+S")) executeConsoleCommand("scene.save");
        if (ImGui::MenuItem("Save current material")) executeConsoleCommand("material.save");
        if (ImGui::MenuItem("Save All")) executeConsoleCommand("save.all");
        ImGui::EndPopup();
    }
    ImGui::End();
    ImGui::PopStyleColor(); ImGui::PopStyleVar(2);
}

void SceneEditor::drawOutputLog() {
    outputLogFocused_ = false;
    if (!outputLogOpen_) return;
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->WorkPos.x + 16, viewport->WorkPos.y + std::max(0.0f, viewport->WorkSize.y - 320)}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({std::min(860.0f, viewport->WorkSize.x), 270}, ImGuiCond_FirstUseEver);
    const bool focus = outputLogFocusRequested_;
    if (focus) ImGui::SetNextWindowFocus();
    outputLogFocusRequested_ = false;
    const bool visible = ImGui::Begin("Output Log###RestArtEngine.OutputLog", &outputLogOpen_);
    outputLogFocused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (visible) {
        outputLogFocused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (ImGui::Button("Clear")) console_.clearOutput();
        ImGui::SameLine();
        if (ImGui::Button("Copy")) {
            std::string text;
            for (const auto& entry : console_.entries()) text += entry.text + '\n';
            ImGui::SetClipboardText(text.c_str());
        }
        ImGui::SameLine(); ImGui::Checkbox("Follow", &outputAutoScroll_);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##log-filter", "Filter output...", consoleFilter_.data(), consoleFilter_.size());
        ImGui::BeginChild("##output", {0, -ImGui::GetFrameHeightWithSpacing()}, ImGuiChildFlags_Borders,
            ImGuiWindowFlags_HorizontalScrollbar);
        const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1;
        for (const auto& entry : console_.entries()) {
            if (consoleFilter_[0] && entry.text.find(consoleFilter_.data()) == std::string::npos) continue;
            if (entry.error) ImGui::PushStyleColor(ImGuiCol_Text, {1.0f, .4f, .35f, 1});
            ImGui::TextUnformatted(entry.text.c_str());
            if (entry.error) ImGui::PopStyleColor();
        }
        if (outputAutoScroll_ && atBottom) ImGui::SetScrollHereY(1);
        ImGui::EndChild();
        if (focus) ImGui::SetKeyboardFocusHere();
        drawConsoleInput("##log-command", ImGui::GetContentRegionAvail().x, false);
    }
    ImGui::End();
}
} // namespace rae::editor
