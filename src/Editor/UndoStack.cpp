#include <RestArtEngine/Editor/UndoStack.h>

namespace rae::editor {

void UndoStack::push(std::string label, Callback undo, Callback redo) {
    if (!undo || !redo) return;
    // Committing a new action invalidates everything that was undone.
    entries_.resize(cursor_);
    entries_.push_back(Entry{std::move(label), std::move(undo), std::move(redo)});
    cursor_ = entries_.size();
    // Drop the oldest steps once the cap is exceeded so memory stays bounded.
    if (maxSteps_ > 0 && entries_.size() > maxSteps_) {
        const auto excess = entries_.size() - maxSteps_;
        entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(excess));
        cursor_ -= excess;
    }
}

bool UndoStack::undo() {
    if (!canUndo()) return false;
    --cursor_;
    entries_[cursor_].undo();
    return true;
}

bool UndoStack::redo() {
    if (!canRedo()) return false;
    entries_[cursor_].redo();
    ++cursor_;
    return true;
}

std::string UndoStack::undoLabel() const {
    return canUndo() ? entries_[cursor_ - 1].label : std::string{};
}

std::string UndoStack::redoLabel() const {
    return canRedo() ? entries_[cursor_].label : std::string{};
}

void UndoStack::clear() noexcept {
    entries_.clear();
    cursor_ = 0;
}

void UndoStack::setMaxSteps(std::size_t steps) {
    maxSteps_ = steps;
    if (maxSteps_ > 0 && entries_.size() > maxSteps_) {
        const auto excess = entries_.size() - maxSteps_;
        entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(excess));
        cursor_ = cursor_ > excess ? cursor_ - excess : 0;
    }
}

} // namespace rae::editor
