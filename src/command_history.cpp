#include "command_history.h"

#include <algorithm>
#include <utility>

void CommandHistory::PushSnapshot(const RmlDocument& doc) {
    undo_stack_.push_back(doc);
    if (undo_stack_.size() > kMaxHistory) {
        undo_stack_.erase(undo_stack_.begin());
    }
    redo_stack_.clear();
}

bool CommandHistory::Undo(RmlDocument& doc) {
    if (undo_stack_.empty()) return false;
    uint64_t prev_rev = doc.revision;
    redo_stack_.push_back(doc);
    doc = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    doc.revision = std::max(doc.revision, prev_rev) + 1;
    return true;
}

bool CommandHistory::Redo(RmlDocument& doc) {
    if (redo_stack_.empty()) return false;
    uint64_t prev_rev = doc.revision;
    undo_stack_.push_back(doc);
    doc = std::move(redo_stack_.back());
    redo_stack_.pop_back();
    doc.revision = std::max(doc.revision, prev_rev) + 1;
    return true;
}

void CommandHistory::Clear() {
    undo_stack_.clear();
    redo_stack_.clear();
}

const RmlDocument* CommandHistory::LatestUndoSnapshot() const {
    return undo_stack_.empty() ? nullptr : &undo_stack_.back();
}
