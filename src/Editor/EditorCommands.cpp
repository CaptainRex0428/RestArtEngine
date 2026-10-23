#include <RestArtEngine/Editor/EditorCommands.h>
#include <imgui.h>
#include <algorithm>

namespace rae::editor {

void EditorCommandRegistry::add(std::string id, std::string group, std::string label, std::string shortcut,
                                int chord, Action action, Predicate enabled) {
    if (id.empty() || !action) return;
    Command command;
    command.info.id = std::move(id);
    command.info.group = std::move(group);
    command.info.label = std::move(label);
    command.info.shortcut = std::move(shortcut);
    command.info.chord = chord;
    command.action = std::move(action);
    command.enabled = std::move(enabled);
    // Replace an existing registration so a re-initialized editor cannot end up
    // with duplicate shortcuts.
    if (auto* existing = find(command.info.id)) {
        *existing = std::move(command);
        return;
    }
    commands_.push_back(std::move(command));
}

EditorCommandRegistry::Command* EditorCommandRegistry::find(std::string_view id) {
    for (auto& command : commands_)
        if (command.info.id == id) return &command;
    return nullptr;
}

bool EditorCommandRegistry::contains(std::string_view id) const {
    return std::any_of(commands_.begin(), commands_.end(),
        [id](const Command& command) { return command.info.id == id; });
}

bool EditorCommandRegistry::enabled(std::string_view id) const {
    for (const auto& command : commands_) {
        if (command.info.id != id) continue;
        return !command.enabled || command.enabled();
    }
    return false;
}

const EditorCommandInfo* EditorCommandRegistry::info(std::string_view id) const {
    for (const auto& command : commands_)
        if (command.info.id == id) return &command.info;
    return nullptr;
}

bool EditorCommandRegistry::invoke(std::string_view id) {
    auto* command = find(id);
    if (!command) return false;
    if (command->enabled && !command->enabled()) return false;
    command->action();
    return true;
}

std::string EditorCommandRegistry::processShortcuts() {
    for (auto& command : commands_) {
        if (command.info.chord == 0) continue;
        if (!ImGui::IsKeyChordPressed(static_cast<ImGuiKeyChord>(command.info.chord))) continue;
        if (command.enabled && !command.enabled()) continue;
        command.action();
        return command.info.id;
    }
    return {};
}

} // namespace rae::editor
