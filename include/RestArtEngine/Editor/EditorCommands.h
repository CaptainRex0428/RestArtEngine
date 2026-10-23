#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace rae::editor {

// One editor action: stable id, menu group/label, display shortcut and key chord.
// The chord is stored as an int so this header stays independent of Dear ImGui;
// the implementation casts it back to ImGuiKeyChord.
//
// Modeled on UE's FUICommandInfo / FUICommandList (id + label + execute +
// can-execute) and Godot's editor command entries (commands are looked up by
// name, and the UI only reflects the registry instead of hard-coding actions).
struct EditorCommandInfo {
    std::string id;        // e.g. "actor.delete"
    std::string group;     // menu grouping, e.g. "edit", "actor", "scene"
    std::string label;     // button / menu label
    std::string shortcut;  // human readable, e.g. "Ctrl+Z"
    int chord = 0;         // ImGuiKeyChord; 0 means no shortcut
};

class EditorCommandRegistry {
public:
    using Action = std::function<void()>;
    using Predicate = std::function<bool()>;

    struct Command {
        EditorCommandInfo info;
        Action action;
        Predicate enabled; // empty means always enabled
    };

    // Registers (or replaces) a command. An empty `enabled` predicate means the
    // command is always available.
    void add(std::string id, std::string group, std::string label, std::string shortcut, int chord,
             Action action, Predicate enabled = {});

    // Runs the command when it exists and is currently enabled.
    bool invoke(std::string_view id);

    [[nodiscard]] bool contains(std::string_view id) const;
    [[nodiscard]] bool enabled(std::string_view id) const;
    [[nodiscard]] const EditorCommandInfo* info(std::string_view id) const;

    // Invokes the first enabled command whose chord was pressed this frame.
    // Returns the invoked id, or an empty string when nothing fired.
    std::string processShortcuts();

    [[nodiscard]] const std::vector<Command>& commands() const noexcept { return commands_; }
    [[nodiscard]] std::size_t size() const noexcept { return commands_.size(); }

private:
    [[nodiscard]] Command* find(std::string_view id);

    std::vector<Command> commands_;
};

} // namespace rae::editor
