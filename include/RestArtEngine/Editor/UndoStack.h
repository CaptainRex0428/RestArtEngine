#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace rae::editor {

// Editor transaction stack. Modeled on Godot's UndoRedo (callers declare both
// directions up front, a committed action truncates the redo tail, history keeps
// a bounded number of steps) and on UE's FScopedTransaction (an edit is scoped
// around the mutation instead of being reconstructed afterwards).
//
// The engine has no object reflection, so an entry stores two closures rather
// than serialized object state: `undo` restores the previous editor state and
// `redo` re-applies the edit. Both must be self-contained (capture copies), so
// replaying them never depends on the current selection.
class UndoStack {
public:
    using Callback = std::function<void()>;

    // Merge policy for consecutive commits of the same logical edit. Modeled on
    // Godot's UndoRedo::MERGE_ENDS: a same-key commit inside the window keeps the
    // oldest undo direction and refreshes only the redo direction, so a slider
    // dragged back and forth still undoes in one step.
    struct Merge {
        std::string key;   // empty disables merging
        int windowMs = 0;  // merge only when the previous commit is this recent
    };

    // Records a new action after `cursor_`, dropping any redo tail. The caller
    // is responsible for applying the edit itself; `redo` only replays it.
    void push(std::string label, Callback undo, Callback redo, Merge merge = {});
    bool undo();
    bool redo();

    [[nodiscard]] bool canUndo() const noexcept { return cursor_ > 0; }
    [[nodiscard]] bool canRedo() const noexcept { return cursor_ < entries_.size(); }
    [[nodiscard]] std::string undoLabel() const;
    [[nodiscard]] std::string redoLabel() const;

    void clear() noexcept;
    void setMaxSteps(std::size_t steps);
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }

private:
    struct Entry {
        std::string label;
        Callback undo;
        Callback redo;
        std::string mergeKey;
        long long commitMs = 0;
    };
    std::vector<Entry> entries_;
    // entries_[0, cursor_) is the applied prefix; the rest is the redo tail.
    std::size_t cursor_ = 0;
    std::size_t maxSteps_ = 64;
};

} // namespace rae::editor
