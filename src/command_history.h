#pragma once

#include "rml_doc.h"

#include <cstddef>
#include <vector>

/// Bounded Undo/Redo history stack storing RmlDocument snapshots with shared image asset pointers.
class CommandHistory {
public:
    static constexpr size_t kMaxHistory = 40;

    void PushSnapshot(const RmlDocument& doc);
    bool CanUndo() const { return !undo_stack_.empty(); }
    bool CanRedo() const { return !redo_stack_.empty(); }

    bool Undo(RmlDocument& doc);
    bool Redo(RmlDocument& doc);
    void Clear();

    const RmlDocument* LatestUndoSnapshot() const;

private:
    std::vector<RmlDocument> undo_stack_;
    std::vector<RmlDocument> redo_stack_;
};
