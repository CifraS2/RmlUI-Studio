#pragma once

#include "gl_renderer.h"
#include "rml_doc.h"
#include "rml_engine.h"
#include "viewport_controller.h"

#include <vector>

/// Handles node selection, gizmo handle hit-testing, move/resize snapping, smart guides, and flow insertion bars.
class GizmoManipulator {
public:
    enum class DragMode { None, Move, Resize };

    struct DragOrigNode {
        int id = 0;
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;
    };

    struct SmartGuideLine {
        bool vertical = true;
        float coord = 0.0f;
        float span_min = 0.0f;
        float span_max = 0.0f;
    };

    struct FlowInsertGuide {
        bool active = false;
        bool vertical = true;
        float x = 0.0f;
        float y = 0.0f;
        float length = 0.0f;
    };

    struct FlowSibBounds {
        int id = 0;
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;
        int par_idx = 0;
    };

    std::vector<int> selected_ids;
    int selection_anchor_id = 0;

    DragMode drag_mode = DragMode::None;
    bool drag_pending = false;
    int drag_down_mx = 0;
    int drag_down_my = 0;
    bool drag_did_move = false;
    bool drag_transform_ui_dirty = false;
    int drag_handle = -1;
    float drag_start_sx = 0.0f;
    float drag_start_sy = 0.0f;
    float orig_x = 0.0f;
    float orig_y = 0.0f;
    float orig_w = 0.0f;
    float orig_h = 0.0f;
    std::vector<DragOrigNode> drag_multi_orig;
    std::vector<int> drag_dirty_node_ids;
    bool drag_is_flow = false;
    bool drag_flow_reordered = false;
    float drag_orig_margin_left = 0.0f;
    float drag_orig_margin_top = 0.0f;
    bool drag_orig_has_margin_left = false;
    bool drag_orig_has_margin_top = false;
    float drag_orig_rel_x = 0.0f;
    float drag_orig_rel_y = 0.0f;
    std::vector<FlowSibBounds> drag_sib_bounds;

    bool snap_guides = true;
    std::vector<SmartGuideLine> active_guides;
    FlowInsertGuide flow_guide;

    int PrimaryId(const RmlDocument* doc) const;
    bool IsSelected(int id) const;
    void SetSingleSelection(int id);
    void PruneMissingSelection(const RmlDocument& doc);

    bool GetNodeCanvasBounds(const RmlDocument* doc, const RmlEngineHost* engine, int id, float& wx, float& wy, float& ww, float& wh) const;
    void GetParentCanvasOrigin(const RmlDocument* doc, const RmlEngineHost* engine, int parent_id, float& ox, float& oy, int child_id = -1) const;
    bool IsNodeFlowManaged(const RmlDocument* doc, const RmlNode& n) const;
    int HitTestHandle(int mx, int my, const RmlNode& n, const RmlDocument* doc, const RmlEngineHost* engine, const ViewportController& vp) const;

    bool IsInDragOrigSet(int node_id) const;
    void RestoreSubtreeForConstraintDrag(RmlDocument* doc, const RmlDocument& snapshot, int container_id) const;

    void CaptureFlowDragState(const RmlDocument* doc, const RmlEngineHost* engine, int node_id);
    void ComputeAndApplyFlowDragMove(RmlDocument* doc, const RmlEngineHost* engine, int node_id, float cx, float cy);
    void ComputeDragSnapMove(
        const RmlDocument* doc,
        const RmlEngineHost* engine,
        const ViewportController& vp,
        int primary_id,
        float raw_dx,
        float raw_dy,
        bool bypass_snap,
        float& out_dx,
        float& out_dy
    );
    void ComputeDragSnapResize(
        const RmlDocument* doc,
        const RmlEngineHost* engine,
        const ViewportController& vp,
        int node_id,
        int handle,
        bool bypass_snap,
        float& nx,
        float& ny,
        float& nw,
        float& nh
    );

    void RenderOverlay(
        GlRenderer2D& gl,
        const RmlDocument* doc,
        const RmlEngineHost* engine,
        const ViewportController& vp,
        float px,
        float py,
        float cw,
        float ch
    ) const;
};
