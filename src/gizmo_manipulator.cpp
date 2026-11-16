#include "gizmo_manipulator.h"

#include <algorithm>
#include <cmath>

namespace {

struct RefTarget {
    float coord = 0.0f;
    float s0 = 0.0f;
    float s1 = 0.0f;
};

void CollectSnapTargets(
    const GizmoManipulator& gizmo,
    const RmlDocument& doc,
    const RmlEngineHost* engine,
    const RmlNode& target_node,
    std::vector<RefTarget>& refs_x,
    std::vector<RefTarget>& refs_y,
    bool skip_drag_set
) {
    auto push_rect_refs = [&](float rx, float ry, float rw, float rh) {
        refs_x.push_back({rx, ry, ry + rh});
        refs_x.push_back({rx + rw * 0.5f, ry, ry + rh});
        refs_x.push_back({rx + rw, ry, ry + rh});
        refs_y.push_back({ry, rx, rx + rw});
        refs_y.push_back({ry + rh * 0.5f, rx, rx + rw});
        refs_y.push_back({ry + rh, rx, rx + rw});
    };

    push_rect_refs(0.0f, 0.0f, static_cast<float>(doc.screen_width), static_cast<float>(doc.screen_height));
    if (target_node.parent_id != doc.root_id) {
        float prx = 0.0f, pry = 0.0f, prw = 0.0f, prh = 0.0f;
        if (gizmo.GetNodeCanvasBounds(&doc, engine, target_node.parent_id, prx, pry, prw, prh)) {
            push_rect_refs(prx, pry, prw, prh);
        }
    }
    if (const auto* parent = doc.FindNode(target_node.parent_id)) {
        for (int sib_id : parent->children) {
            if (sib_id == target_node.id) continue;
            if (skip_drag_set && gizmo.IsInDragOrigSet(sib_id)) continue;
            const auto* sib = doc.FindNode(sib_id);
            if (!sib || !sib->visible) continue;
            float sx = 0.0f, sy = 0.0f, sw = 0.0f, sh = 0.0f;
            if (gizmo.GetNodeCanvasBounds(&doc, engine, sib_id, sx, sy, sw, sh)) {
                push_rect_refs(sx, sy, sw, sh);
            }
        }
    }
}

} // namespace

int GizmoManipulator::PrimaryId(const RmlDocument* doc) const {
    if (!selected_ids.empty()) return selected_ids.back();
    return doc ? doc->root_id : 0;
}

bool GizmoManipulator::IsSelected(int id) const {
    return std::find(selected_ids.begin(), selected_ids.end(), id) != selected_ids.end();
}

void GizmoManipulator::SetSingleSelection(int id) {
    selected_ids.clear();
    selected_ids.push_back(id);
    selection_anchor_id = id;
}

void GizmoManipulator::PruneMissingSelection(const RmlDocument& doc) {
    selected_ids.erase(
        std::remove_if(selected_ids.begin(), selected_ids.end(),
                       [&](int id) { return doc.FindNode(id) == nullptr; }),
        selected_ids.end());
    if (selected_ids.empty()) {
        SetSingleSelection(doc.root_id);
    }
}

bool GizmoManipulator::GetNodeCanvasBounds(
    const RmlDocument* doc,
    const RmlEngineHost* engine,
    int id,
    float& wx,
    float& wy,
    float& ww,
    float& wh
) const {
    if (!doc) return false;
    const auto* n = doc->FindNode(id);
    if (!n) return false;
    if (engine && engine->GetCanvasElementRect(id, wx, wy, ww, wh)) {
        ww = std::max(1.0f, ww);
        wh = std::max(1.0f, wh);
        return true;
    }
    doc->GetWorldPos(id, wx, wy);
    ww = n->width;
    wh = n->height;
    return true;
}

void GizmoManipulator::GetParentCanvasOrigin(
    const RmlDocument* doc,
    const RmlEngineHost* engine,
    int parent_id,
    float& ox,
    float& oy,
    int child_id
) const {
    if (child_id > 0 && doc && engine) {
        const RmlNode* cn = doc->FindNode(child_id);
        const RmlNode* pn = doc->FindNode(parent_id);
        if (cn && pn && !pn->is_body_root && !pn->display_flex && !pn->css_positioned &&
            (cn->css_positioned || !cn->from_import)) {
            if (engine->GetCanvasElementPaddingOrigin(parent_id, ox, oy)) {
                return;
            }
        }
        if (engine->GetCanvasElementOffsetParentOrigin(child_id, ox, oy)) {
            return;
        }
    }
    ox = 0.0f;
    oy = 0.0f;
    if (!doc || parent_id == doc->root_id) return;
    float pw = 0.0f, ph = 0.0f;
    GetNodeCanvasBounds(doc, engine, parent_id, ox, oy, pw, ph);
}

bool GizmoManipulator::IsNodeFlowManaged(const RmlDocument* doc, const RmlNode& n) const {
    if (!doc || n.id == doc->root_id) return false;
    return n.pos_mode != RmlPositionMode::Absolute;
}

int GizmoManipulator::HitTestHandle(
    int mx,
    int my,
    const RmlNode& n,
    const RmlDocument* doc,
    const RmlEngineHost* engine,
    const ViewportController& vp
) const {
    float wx = 0.0f, wy = 0.0f, ww = n.width, wh = n.height;
    GetNodeCanvasBounds(doc, engine, n.id, wx, wy, ww, wh);
    float x0 = vp.pan_x + wx * vp.zoom;
    float y0 = vp.pan_y + wy * vp.zoom;
    float x1 = x0 + ww * vp.zoom;
    float y1 = y0 + wh * vp.zoom;
    float xm = (x0 + x1) * 0.5f;
    float ym = (y0 + y1) * 0.5f;
    float pts[8][2] = {
        {x0, y0}, {xm, y0}, {x1, y0}, {x1, ym},
        {x1, y1}, {xm, y1}, {x0, y1}, {x0, ym}
    };
    for (int i = 0; i < 8; ++i) {
        if (std::fabs(static_cast<float>(mx) - pts[i][0]) <= 7.0f &&
            std::fabs(static_cast<float>(my) - pts[i][1]) <= 7.0f) {
            return i;
        }
    }
    return -1;
}

bool GizmoManipulator::IsInDragOrigSet(int node_id) const {
    for (const auto& d : drag_multi_orig) {
        if (d.id == node_id) return true;
    }
    return false;
}

void GizmoManipulator::RestoreSubtreeForConstraintDrag(RmlDocument* doc, const RmlDocument& snapshot, int container_id) const {
    if (!doc) return;
    const auto* c = doc->FindNode(container_id);
    if (!c) return;
    std::vector<int> stack = c->children;
    while (!stack.empty()) {
        int cid = stack.back();
        stack.pop_back();
        if (auto* cur = doc->FindNode(cid)) {
            if (const auto* orig = snapshot.FindNode(cid)) {
                cur->x = orig->x;
                cur->y = orig->y;
                cur->width = orig->width;
                cur->height = orig->height;
            }
            for (int sub : cur->children) stack.push_back(sub);
        }
    }
}

void GizmoManipulator::CaptureFlowDragState(const RmlDocument* doc, const RmlEngineHost* engine, int node_id) {
    drag_sib_bounds.clear();
    drag_flow_reordered = false;
    if (!doc) return;
    const RmlNode* moved = doc->FindNode(node_id);
    if (!moved) return;

    drag_orig_margin_left = moved->margin_left;
    drag_orig_margin_top = moved->margin_top;
    drag_orig_has_margin_left = moved->has_margin_left;
    drag_orig_has_margin_top = moved->has_margin_top;
    drag_orig_rel_x = moved->rel_x;
    drag_orig_rel_y = moved->rel_y;

    const RmlNode* par = doc->FindNode(moved->parent_id);
    if (!par || par->children.size() < 2) return;

    drag_sib_bounds.reserve(par->children.size());
    for (int idx = 0; idx < static_cast<int>(par->children.size()); ++idx) {
        int sid = par->children[static_cast<size_t>(idx)];
        if (sid == node_id) continue;
        const RmlNode* sn = doc->FindNode(sid);
        if (!sn || !sn->visible || sn->pos_mode == RmlPositionMode::Absolute) continue;
        float sx = 0.0f, sy = 0.0f, sw = sn->width, sh = sn->height;
        GetNodeCanvasBounds(doc, engine, sid, sx, sy, sw, sh);
        drag_sib_bounds.push_back({sid, sx, sy, std::max(1.0f, sw), std::max(1.0f, sh), idx});
    }
}

void GizmoManipulator::ComputeAndApplyFlowDragMove(RmlDocument* doc, const RmlEngineHost* engine, int node_id, float cx, float cy) {
    flow_guide.active = false;
    active_guides.clear();
    if (!doc) return;
    RmlNode* moved = doc->FindNode(node_id);
    if (!moved) return;
    const RmlNode* par = doc->FindNode(moved->parent_id);

    // Check adjacent visible flow siblings for a 50% midpoint swap along the main axis
    int swap_target_idx = -1;
    if (par && par->children.size() >= 2 && !drag_sib_bounds.empty()) {
        int old_idx = -1;
        for (int idx = 0; idx < static_cast<int>(par->children.size()); ++idx) {
            if (par->children[static_cast<size_t>(idx)] == node_id) {
                old_idx = idx;
                break;
            }
        }

        if (old_idx >= 0) {
            const FlowSibBounds* prev_sib = nullptr;
            const FlowSibBounds* next_sib = nullptr;
            for (const auto& s : drag_sib_bounds) {
                if (s.par_idx < old_idx) {
                    if (!prev_sib || s.par_idx > prev_sib->par_idx) prev_sib = &s;
                } else if (s.par_idx > old_idx) {
                    if (!next_sib || s.par_idx < next_sib->par_idx) next_sib = &s;
                }
            }

            bool horizontal = false;
            if (par->display_flex) {
                horizontal = (par->flex_direction == 0);
            } else {
                float min_x = drag_sib_bounds[0].x, max_x = drag_sib_bounds[0].x + drag_sib_bounds[0].w;
                float min_y = drag_sib_bounds[0].y, max_y = drag_sib_bounds[0].y + drag_sib_bounds[0].h;
                for (const auto& s : drag_sib_bounds) {
                    min_x = std::min(min_x, s.x);
                    max_x = std::max(max_x, s.x + s.w);
                    min_y = std::min(min_y, s.y);
                    max_y = std::max(max_y, s.y + s.h);
                }
                float span_x = max_x - min_x;
                float span_y = max_y - min_y;
                if (drag_sib_bounds.size() >= 2) {
                    bool shared_row = std::fabs((drag_sib_bounds[0].y + drag_sib_bounds[0].h * 0.5f) -
                                                (drag_sib_bounds[1].y + drag_sib_bounds[1].h * 0.5f)) <
                                      std::min(drag_sib_bounds[0].h, drag_sib_bounds[1].h) * 0.5f;
                    horizontal = shared_row && (span_x > span_y * 0.65f);
                }
            }

            if (horizontal) {
                if (prev_sib && cx < prev_sib->x + prev_sib->w * 0.5f && cy <= prev_sib->y + prev_sib->h + 8.0f) {
                    swap_target_idx = prev_sib->par_idx;
                    flow_guide = {true, true, prev_sib->x - 3.0f, prev_sib->y - 2.0f, prev_sib->h + 4.0f};
                } else if (next_sib && cx > next_sib->x + next_sib->w * 0.5f && cy >= next_sib->y - 8.0f) {
                    swap_target_idx = next_sib->par_idx;
                    flow_guide = {true, true, next_sib->x + next_sib->w + 3.0f, next_sib->y - 2.0f, next_sib->h + 4.0f};
                }
            } else {
                if (prev_sib && cy < prev_sib->y + prev_sib->h * 0.5f) {
                    swap_target_idx = prev_sib->par_idx;
                    flow_guide = {true, false, prev_sib->x - 2.0f, prev_sib->y - 3.0f, prev_sib->w + 4.0f};
                } else if (next_sib && cy > next_sib->y + next_sib->h * 0.5f) {
                    swap_target_idx = next_sib->par_idx;
                    flow_guide = {true, false, next_sib->x - 2.0f, next_sib->y + next_sib->h + 3.0f, next_sib->w + 4.0f};
                }
            }

            if (swap_target_idx >= 0 && swap_target_idx != old_idx) {
                if (moved->pos_mode == RmlPositionMode::Static) {
                    moved->margin_left = drag_orig_margin_left;
                    moved->margin_top = drag_orig_margin_top;
                    moved->has_margin_left = drag_orig_has_margin_left;
                    moved->has_margin_top = drag_orig_has_margin_top;
                } else {
                    moved->rel_x = drag_orig_rel_x;
                    moved->rel_y = drag_orig_rel_y;
                }
                doc->ReorderChildNode(par->id, node_id, swap_target_idx);
                drag_start_sx = cx;
                drag_start_sy = cy;
                drag_flow_reordered = true;
                drag_dirty_node_ids.clear();
                drag_dirty_node_ids.push_back(node_id);
                drag_transform_ui_dirty = true;
                return;
            }
        }
    }

    // Continuous free-offset drag inside the node's current flow/flex slot
    float dx = cx - drag_start_sx;
    float dy = cy - drag_start_sy;
    if (moved->pos_mode == RmlPositionMode::Relative) {
        moved->rel_x = std::round(drag_orig_rel_x + dx);
        moved->rel_y = std::round(drag_orig_rel_y + dy);
    } else {
        moved->margin_left = std::round(drag_orig_margin_left + dx);
        moved->margin_top = std::round(drag_orig_margin_top + dy);
        moved->has_margin_left = drag_orig_has_margin_left || (std::fabs(dx) >= 1.0f);
        moved->has_margin_top = drag_orig_has_margin_top || (std::fabs(dy) >= 1.0f);
    }
    moved->modified_transform = true;
    drag_dirty_node_ids.clear();
    drag_dirty_node_ids.push_back(moved->id);
    drag_transform_ui_dirty = true;
}

void GizmoManipulator::ComputeDragSnapMove(
    const RmlDocument* doc,
    const RmlEngineHost* engine,
    const ViewportController& vp,
    int primary_id,
    float raw_dx,
    float raw_dy,
    bool bypass_snap,
    float& out_dx,
    float& out_dy
) {
    active_guides.clear();
    out_dx = raw_dx;
    out_dy = raw_dy;
    if (!snap_guides || bypass_snap || !doc) return;
    const auto* pn = doc->FindNode(primary_id);
    if (!pn) return;

    float ox = 0.0f, oy = 0.0f;
    GetParentCanvasOrigin(doc, engine, pn->parent_id, ox, oy, pn->id);
    float px = ox + orig_x + raw_dx;
    float py = oy + orig_y + raw_dy;
    float pw = std::max(1.0f, orig_w);
    float ph = std::max(1.0f, orig_h);
    for (const auto& d : drag_multi_orig) {
        if (d.id == primary_id) {
            px = ox + d.x + raw_dx;
            py = oy + d.y + raw_dy;
            pw = std::max(1.0f, d.w);
            ph = std::max(1.0f, d.h);
            break;
        }
    }

    std::vector<RefTarget> refs_x, refs_y;
    CollectSnapTargets(*this, *doc, engine, *pn, refs_x, refs_y, true);

    float thresh = std::clamp(6.0f / std::max(0.25f, vp.zoom), 2.5f, 14.0f);
    float edges_x[3] = {px, px + pw * 0.5f, px + pw};
    float best_dx = 0.0f, best_ax = thresh + 0.001f;
    for (float ex : edges_x) {
        for (const auto& rx : refs_x) {
            float diff = rx.coord - ex;
            if (std::fabs(diff) < best_ax) { best_ax = std::fabs(diff); best_dx = diff; }
        }
    }
    if (best_ax <= thresh) { out_dx += best_dx; px += best_dx; }

    float edges_y[3] = {py, py + ph * 0.5f, py + ph};
    float best_dy = 0.0f, best_ay = thresh + 0.001f;
    for (float ey : edges_y) {
        for (const auto& ry : refs_y) {
            float diff = ry.coord - ey;
            if (std::fabs(diff) < best_ay) { best_ay = std::fabs(diff); best_dy = diff; }
        }
    }
    if (best_ay <= thresh) { out_dy += best_dy; py += best_dy; }

    constexpr float kEps = 0.55f;
    float final_ex[3] = {px, px + pw * 0.5f, px + pw};
    for (float ex : final_ex) {
        bool matched = false;
        float mn = py, mx = py + ph;
        for (const auto& rx : refs_x) {
            if (std::fabs(rx.coord - ex) <= kEps) { matched = true; mn = std::min(mn, rx.s0); mx = std::max(mx, rx.s1); }
        }
        if (matched) active_guides.push_back({true, ex, mn, mx});
    }
    float final_ey[3] = {py, py + ph * 0.5f, py + ph};
    for (float ey : final_ey) {
        bool matched = false;
        float mn = px, mx = px + pw;
        for (const auto& ry : refs_y) {
            if (std::fabs(ry.coord - ey) <= kEps) { matched = true; mn = std::min(mn, ry.s0); mx = std::max(mx, ry.s1); }
        }
        if (matched) active_guides.push_back({false, ey, mn, mx});
    }
}

void GizmoManipulator::ComputeDragSnapResize(
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
) {
    active_guides.clear();
    if (!snap_guides || bypass_snap || !doc) return;
    const auto* pn = doc->FindNode(node_id);
    if (!pn) return;

    float ox = 0.0f, oy = 0.0f;
    GetParentCanvasOrigin(doc, engine, pn->parent_id, ox, oy, pn->id);
    float px = ox + nx;
    float py = oy + ny;

    std::vector<RefTarget> refs_x, refs_y;
    CollectSnapTargets(*this, *doc, engine, *pn, refs_x, refs_y, false);

    float thresh = std::clamp(6.0f / std::max(0.25f, vp.zoom), 2.5f, 14.0f);
    bool move_l = (handle == 0 || handle == 6 || handle == 7);
    bool move_r = (handle == 2 || handle == 3 || handle == 4);
    bool move_t = (handle == 0 || handle == 1 || handle == 2);
    bool move_b = (handle == 4 || handle == 5 || handle == 6);

    if (move_l) {
        float best = thresh + 0.001f, delta = 0.0f;
        for (const auto& rx : refs_x) {
            float d = rx.coord - px;
            if (std::fabs(d) < best && (nw - d) >= 12.0f) { best = std::fabs(d); delta = d; }
        }
        if (best <= thresh) { nx += delta; px += delta; nw -= delta; }
    } else if (move_r) {
        float right_x = px + nw, best = thresh + 0.001f, delta = 0.0f;
        for (const auto& rx : refs_x) {
            float d = rx.coord - right_x;
            if (std::fabs(d) < best && (nw + d) >= 12.0f) { best = std::fabs(d); delta = d; }
        }
        if (best <= thresh) nw += delta;
    }

    if (move_t) {
        float best = thresh + 0.001f, delta = 0.0f;
        for (const auto& ry : refs_y) {
            float d = ry.coord - py;
            if (std::fabs(d) < best && (nh - d) >= 12.0f) { best = std::fabs(d); delta = d; }
        }
        if (best <= thresh) { ny += delta; py += delta; nh -= delta; }
    } else if (move_b) {
        float bot_y = py + nh, best = thresh + 0.001f, delta = 0.0f;
        for (const auto& ry : refs_y) {
            float d = ry.coord - bot_y;
            if (std::fabs(d) < best && (nh + d) >= 12.0f) { best = std::fabs(d); delta = d; }
        }
        if (best <= thresh) nh += delta;
    }

    constexpr float kEps = 0.55f;
    auto push_match_v = [&](float cur_x) {
        float mn = py, mx = py + nh;
        bool m = false;
        for (const auto& rx : refs_x) {
            if (std::fabs(rx.coord - cur_x) <= kEps) { m = true; mn = std::min(mn, rx.s0); mx = std::max(mx, rx.s1); }
        }
        if (m) active_guides.push_back({true, cur_x, mn, mx});
    };
    auto push_match_h = [&](float cur_y) {
        float mn = px, mx = px + nw;
        bool m = false;
        for (const auto& ry : refs_y) {
            if (std::fabs(ry.coord - cur_y) <= kEps) { m = true; mn = std::min(mn, ry.s0); mx = std::max(mx, ry.s1); }
        }
        if (m) active_guides.push_back({false, cur_y, mn, mx});
    };
    if (move_l) push_match_v(px);
    if (move_r) push_match_v(px + nw);
    if (move_t) push_match_h(py);
    if (move_b) push_match_h(py + nh);
}

void GizmoManipulator::RenderOverlay(
    GlRenderer2D& gl,
    const RmlDocument* doc,
    const RmlEngineHost* engine,
    const ViewportController& vp,
    float px,
    float py,
    float cw,
    float ch
) const {
    if (!doc) return;
    int primary = PrimaryId(doc);
    for (int sid : selected_ids) {
        if (const auto* sel = doc->FindNode(sid)) {
            if (sel->id == doc->root_id) {
                gl.DrawRectOutline(px, py, cw, ch, 1.0f, 71, 120, 245, 110);
                gl.DrawRectFilled(px - 3.0f, py - 3.0f, 6.0f, 6.0f, 71, 120, 245, 255);
                gl.DrawRectOutline(px - 3.0f, py - 3.0f, 6.0f, 6.0f, 1.0f, 168, 190, 255, 255);
                gl.DrawRectFilled(px + cw - 3.0f, py + ch - 3.0f, 6.0f, 6.0f, 71, 120, 245, 255);
                gl.DrawRectOutline(px + cw - 3.0f, py + ch - 3.0f, 6.0f, 6.0f, 1.0f, 168, 190, 255, 255);
                continue;
            }
            float wx = 0.0f, wy = 0.0f, ww = sel->width, wh = sel->height;
            GetNodeCanvasBounds(doc, engine, sel->id, wx, wy, ww, wh);
            float sx = px + wx * vp.zoom;
            float sy = py + wy * vp.zoom;
            float sw = ww * vp.zoom;
            float sh = wh * vp.zoom;

            gl.DrawRectOutline(sx, sy, sw, sh, (sid == primary) ? 1.5f : 1.0f, 71, 120, 245, 255);
            if (selected_ids.size() == 1 && !sel->locked) {
                float xm = sx + sw * 0.5f;
                float ym = sy + sh * 0.5f;
                float pts[8][2] = {
                    {sx, sy}, {xm, sy}, {sx + sw, sy}, {sx + sw, ym},
                    {sx + sw, sy + sh}, {xm, sy + sh}, {sx, sy + sh}, {sx, ym}
                };
                for (int i = 0; i < 8; ++i) {
                    gl.DrawRectFilled(pts[i][0] - 4.0f, pts[i][1] - 4.0f, 8.0f, 8.0f, 168, 190, 255, 255);
                    gl.DrawRectOutline(pts[i][0] - 4.0f, pts[i][1] - 4.0f, 8.0f, 8.0f, 1.2f, 71, 120, 245, 255);
                }
            }
        }
    }

    for (const auto& g : active_guides) {
        if (g.vertical) {
            float gx = px + g.coord * vp.zoom;
            float gy0 = py + g.span_min * vp.zoom - 6.0f;
            float gy1 = py + g.span_max * vp.zoom + 6.0f;
            gl.DrawRectFilled(gx, gy0, 1.0f, std::max(2.0f, gy1 - gy0), 255, 46, 147, 235);
            gl.DrawRectFilled(gx - 2.5f, gy0, 6.0f, 1.0f, 255, 46, 147, 235);
            gl.DrawRectFilled(gx - 2.5f, gy1 - 1.0f, 6.0f, 1.0f, 255, 46, 147, 235);
        } else {
            float gy = py + g.coord * vp.zoom;
            float gx0 = px + g.span_min * vp.zoom - 6.0f;
            float gx1 = px + g.span_max * vp.zoom + 6.0f;
            gl.DrawRectFilled(gx0, gy, std::max(2.0f, gx1 - gx0), 1.0f, 255, 46, 147, 235);
            gl.DrawRectFilled(gx0, gy - 2.5f, 1.0f, 6.0f, 255, 46, 147, 235);
            gl.DrawRectFilled(gx1 - 1.0f, gy - 2.5f, 1.0f, 6.0f, 255, 46, 147, 235);
        }
    }

    if (flow_guide.active) {
        if (flow_guide.vertical) {
            float gx = px + flow_guide.x * vp.zoom;
            float gy = py + flow_guide.y * vp.zoom;
            float gh = std::max(8.0f, flow_guide.length * vp.zoom);
            gl.DrawRectFilled(gx - 1.0f, gy, 2.5f, gh, 56, 189, 248, 255);
            gl.DrawRectFilled(gx - 3.5f, gy - 1.0f, 7.5f, 2.5f, 56, 189, 248, 255);
            gl.DrawRectFilled(gx - 3.5f, gy + gh - 1.5f, 7.5f, 2.5f, 56, 189, 248, 255);
        } else {
            float gx = px + flow_guide.x * vp.zoom;
            float gy = py + flow_guide.y * vp.zoom;
            float gw = std::max(8.0f, flow_guide.length * vp.zoom);
            gl.DrawRectFilled(gx, gy - 1.0f, gw, 2.5f, 56, 189, 248, 255);
            gl.DrawRectFilled(gx - 1.0f, gy - 3.5f, 2.5f, 7.5f, 56, 189, 248, 255);
            gl.DrawRectFilled(gx + gw - 1.5f, gy - 3.5f, 2.5f, 7.5f, 56, 189, 248, 255);
        }
    }
}
