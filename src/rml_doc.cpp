#include "rml_doc.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;

std::string RmlColor::ToCss() const {
    char buf[64];
    if (a == 255) {
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
    } else {
        std::snprintf(buf, sizeof(buf), "rgba(%u, %u, %u, %u)", r, g, b, a);
    }
    return std::string(buf);
}

void RmlColor::ToImGuiFloat4(float out[4]) const {
    out[0] = r / 255.0f;
    out[1] = g / 255.0f;
    out[2] = b / 255.0f;
    out[3] = a / 255.0f;
}

void RmlColor::FromImGuiFloat4(const float in[4]) {
    auto clamp8 = [](float v) -> uint8_t {
        int iv = static_cast<int>(std::round(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        return static_cast<uint8_t>(std::clamp(iv, 0, 255));
    };
    r = clamp8(in[0]);
    g = clamp8(in[1]);
    b = clamp8(in[2]);
    a = clamp8(in[3]);
}

namespace {

std::string sanitize_class(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    bool prev_dash = false;
    for (unsigned char ch : raw) {
        if (std::isalnum(ch)) {
            out.push_back(static_cast<char>(std::tolower(ch)));
            prev_dash = false;
        } else if (!prev_dash && !out.empty()) {
            out.push_back('-');
            prev_dash = true;
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out.front()))) {
        out = "el-" + out;
    }
    return out;
}

std::string html_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string fmt_pct(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f%%", v);
    return std::string(buf);
}

std::string fmt_px(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0fpx", std::round(v));
    return std::string(buf);
}

} // namespace

RmlDocument::RmlDocument() {
    ResetDefaultScreen();
}

void RmlDocument::ResetDefaultScreen() {
    nodes.clear();
    next_id = 1;
    screen_width = 1920;
    screen_height = 1080;
    base_dir.clear();
    has_imported_source = false;
    imported_raw_rml.clear();
    imported_raw_rcss.clear();
    imported_body_class.clear();
    imported_body_style.clear();

    RmlNode root;
    root.id = next_id++;
    root.parent_id = -1;
    root.name = "screen-root";
    root.tag = "div";
    root.rml_class = "screen-root";
    root.x = 0.0f;
    root.y = 0.0f;
    root.width = 1920.0f;
    root.height = 1080.0f;
    root.has_bg = true;
    root.bg_color = {11, 16, 32, 255};
    root.border_color = {59, 130, 246, 255};
    root.border_width = 1.0f;
    root.border_radius = 2.0f;
    root.display_flex = false;
    root.flex_direction = 1;
    root.justify_content = 0;
    root.align_items = 0;
    root_id = root.id;
    nodes.push_back(std::move(root));
    Touch();
}

RmlNode* RmlDocument::FindNode(int id) {
    for (auto& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

const RmlNode* RmlDocument::FindNode(int id) const {
    for (const auto& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

void RmlDocument::GetWorldPos(int id, float& out_x, float& out_y) const {
    out_x = 0.0f;
    out_y = 0.0f;
    int cur = id;
    int guard = 0;
    while (cur >= 0 && guard++ < 256) {
        const RmlNode* n = FindNode(cur);
        if (!n) break;
        out_x += n->x;
        out_y += n->y;
        cur = n->parent_id;
    }
}

int RmlDocument::HitTest(float screen_x, float screen_y) const {
    // Walk reverse-paint order (last child on top)
    int hit = root_id;
    std::function<bool(int)> visit = [&](int id) -> bool {
        const RmlNode* n = FindNode(id);
        if (!n || !n->visible || n->locked) return false;
        for (auto it = n->children.rbegin(); it != n->children.rend(); ++it) {
            if (visit(*it)) return true;
        }
        if (id == root_id) return false;
        float wx = 0.0f, wy = 0.0f;
        GetWorldPos(id, wx, wy);
        if (screen_x >= wx && screen_x <= wx + n->width && screen_y >= wy && screen_y <= wy + n->height) {
            hit = id;
            return true;
        }
        return false;
    };
    visit(root_id);
    return hit;
}

int RmlDocument::AddDiv(int parent_id, const std::string& name, float x, float y, float w, float h) {
    if (!FindNode(parent_id)) parent_id = root_id;
    RmlNode n;
    n.id = next_id++;
    n.parent_id = parent_id;
    n.name = name;
    n.tag = "div";
    n.rml_class = sanitize_class(name);
    n.x = x;
    n.y = y;
    n.width = w;
    n.height = h;
    n.has_bg = true;
    n.bg_color = {32, 34, 42, 240};
    n.border_color = {64, 68, 82, 255};
    n.border_width = 1.0f;
    n.border_radius = 8.0f;
    int nid = n.id;
    nodes.push_back(std::move(n));
    if (auto* p = FindNode(parent_id)) {
        p->children.push_back(nid);
    }
    Touch();
    return nid;
}

int RmlDocument::AddButton(int parent_id, const std::string& label, float x, float y, float w, float h) {
    if (!FindNode(parent_id)) parent_id = root_id;
    RmlNode n;
    n.id = next_id++;
    n.parent_id = parent_id;
    n.name = "Button_" + label;
    n.tag = "button";
    n.rml_class = sanitize_class("btn-" + label);
    n.x = x;
    n.y = y;
    n.width = w;
    n.height = h;
    n.text = label;
    n.has_bg = true;
    n.bg_color = {0, 122, 255, 255};
    n.border_radius = 6.0f;
    n.text_color = {255, 255, 255, 255};
    n.font_size = 15.0f;
    n.font_bold = true;
    n.text_align = 1;
    n.display_flex = true;
    n.justify_content = 1;
    n.align_items = 1;
    n.has_hover = true;
    n.hover_opacity = 0.85f;
    int nid = n.id;
    nodes.push_back(std::move(n));
    if (auto* p = FindNode(parent_id)) {
        p->children.push_back(nid);
    }
    Touch();
    return nid;
}

int RmlDocument::AddInput(int parent_id, const std::string& placeholder, float x, float y, float w, float h) {
    if (!FindNode(parent_id)) parent_id = root_id;
    RmlNode n;
    n.id = next_id++;
    n.parent_id = parent_id;
    n.name = "Input_" + placeholder;
    n.tag = "input";
    n.rml_class = sanitize_class("input-" + placeholder);
    n.x = x;
    n.y = y;
    n.width = w;
    n.height = h;
    n.placeholder = placeholder;
    n.has_bg = true;
    n.bg_color = {28, 28, 30, 255};
    n.border_color = {58, 58, 60, 255};
    n.border_width = 1.0f;
    n.border_radius = 6.0f;
    n.text_color = {220, 220, 225, 255};
    n.font_size = 14.0f;
    n.padding[0] = 8.0f;
    n.padding[1] = 10.0f;
    n.padding[2] = 8.0f;
    n.padding[3] = 10.0f;
    int nid = n.id;
    nodes.push_back(std::move(n));
    if (auto* p = FindNode(parent_id)) {
        p->children.push_back(nid);
    }
    Touch();
    return nid;
}

int RmlDocument::AddText(int parent_id, const std::string& text, float x, float y, float w, float h) {
    if (!FindNode(parent_id)) parent_id = root_id;
    RmlNode n;
    n.id = next_id++;
    n.parent_id = parent_id;
    n.name = "Text_" + text.substr(0, 16);
    n.tag = "span";
    n.rml_class = sanitize_class("txt-" + text.substr(0, 16));
    n.x = x;
    n.y = y;
    n.width = w;
    n.height = h;
    n.text = text;
    n.has_bg = false;
    n.text_color = {255, 255, 255, 255};
    n.font_size = 18.0f;
    int nid = n.id;
    nodes.push_back(std::move(n));
    if (auto* p = FindNode(parent_id)) {
        p->children.push_back(nid);
    }
    Touch();
    return nid;
}

int RmlDocument::AddImage(
    int parent_id,
    const std::string& filename,
    const std::vector<uint8_t>& bytes,
    float x,
    float y,
    float w,
    float h
) {
    if (!FindNode(parent_id)) parent_id = root_id;
    RmlNode n;
    n.id = next_id++;
    n.parent_id = parent_id;
    n.name = filename;
    n.tag = "img";
    n.rml_class = sanitize_class("img-" + filename);
    n.x = x;
    n.y = y;
    n.width = w;
    n.height = h;
    n.has_bg = false;
    n.image_src = filename;
    n.image_bytes = bytes;
    int nid = n.id;
    nodes.push_back(std::move(n));
    if (auto* p = FindNode(parent_id)) {
        p->children.push_back(nid);
    }
    Touch();
    return nid;
}

bool RmlDocument::DeleteNode(int id) {
    if (id == root_id) return false;
    RmlNode* n = FindNode(id);
    if (!n) return false;

    // Collect subtree ids
    std::vector<int> to_remove;
    std::function<void(int)> collect = [&](int cur) {
        to_remove.push_back(cur);
        if (const RmlNode* cn = FindNode(cur)) {
            for (int cid : cn->children) collect(cid);
        }
    };
    collect(id);

    if (auto* p = FindNode(n->parent_id)) {
        p->children.erase(std::remove(p->children.begin(), p->children.end(), id), p->children.end());
    }
    nodes.erase(
        std::remove_if(nodes.begin(), nodes.end(), [&](const RmlNode& item) {
            return std::find(to_remove.begin(), to_remove.end(), item.id) != to_remove.end();
        }),
        nodes.end()
    );
    Touch();
    return true;
}

int RmlDocument::DuplicateNode(int id) {
    if (id == root_id) return -1;
    const RmlNode* src = FindNode(id);
    if (!src) return -1;
    RmlNode copy = *src;
    copy.id = next_id++;
    copy.name += " Copy";
    copy.x += 24.0f;
    copy.y += 24.0f;
    copy.children.clear();
    int new_id = copy.id;
    int parent_id = copy.parent_id;
    std::vector<int> orig_children = src->children;
    nodes.push_back(std::move(copy));
    if (auto* p = FindNode(parent_id)) {
        p->children.push_back(new_id);
    }
    for (int cid : orig_children) {
        if (const RmlNode* csrc = FindNode(cid)) {
            RmlNode ccopy = *csrc;
            ccopy.id = next_id++;
            ccopy.parent_id = new_id;
            ccopy.children.clear();
            int cnid = ccopy.id;
            nodes.push_back(std::move(ccopy));
            if (auto* np = FindNode(new_id)) {
                np->children.push_back(cnid);
            }
        }
    }
    Touch();
    return new_id;
}

void RmlDocument::MoveNode(int id, float dx, float dy) {
    if (id == root_id) return;
    if (auto* n = FindNode(id)) {
        n->x += dx;
        n->y += dy;
        Touch();
    }
}

bool RmlDocument::ReparentNode(int id, int new_parent_id) {
    if (id == root_id || id == new_parent_id) return false;
    RmlNode* node = FindNode(id);
    RmlNode* new_parent = FindNode(new_parent_id);
    if (!node || !new_parent || node->parent_id == new_parent_id) return false;

    // Prevent cycles (new_parent cannot be a descendant of id)
    int cur = new_parent_id;
    int guard = 0;
    while (cur >= 0 && guard++ < 256) {
        if (cur == id) return false;
        const RmlNode* p = FindNode(cur);
        if (!p) break;
        cur = p->parent_id;
    }

    float wx = 0.0f, wy = 0.0f;
    GetWorldPos(id, wx, wy);
    float pwx = 0.0f, pwy = 0.0f;
    GetWorldPos(new_parent_id, pwx, pwy);

    if (auto* old_parent = FindNode(node->parent_id)) {
        auto& ch = old_parent->children;
        ch.erase(std::remove(ch.begin(), ch.end(), id), ch.end());
    }
    node->parent_id = new_parent_id;
    node->x = wx - pwx;
    node->y = wy - pwy;
    new_parent->children.push_back(id);
    Touch();
    return true;
}

void RmlDocument::ApplyConstraintsOnParentResize(int parent_id, float old_w, float old_h, float new_w, float new_h) {
    if (old_w < 1.0f || old_h < 1.0f || new_w < 1.0f || new_h < 1.0f) return;
    if (std::fabs(old_w - new_w) < 0.01f && std::fabs(old_h - new_h) < 0.01f) return;
    const RmlNode* p = FindNode(parent_id);
    if (!p || p->display_flex) return;

    std::vector<int> child_ids = p->children;
    for (int cid : child_ids) {
        RmlNode* c = FindNode(cid);
        if (!c) continue;
        float prev_w = c->width;
        float prev_h = c->height;

        // Horizontal constraint
        if (c->size_mode_w == 2) { // Fill container
            c->x = 0.0f;
            c->width = std::max(8.0f, new_w);
        } else if (c->size_mode_w == 0) {
            if (c->anchor_h == 1) { // Right
                float right_gap = old_w - (c->x + prev_w);
                c->x = new_w - right_gap - prev_w;
            } else if (c->anchor_h == 2) { // Left + Right
                float right_gap = old_w - (c->x + prev_w);
                c->width = std::max(8.0f, new_w - c->x - right_gap);
            } else if (c->anchor_h == 3) { // Center
                float delta_c = (c->x + prev_w * 0.5f) - (old_w * 0.5f);
                c->x = (new_w * 0.5f + delta_c) - prev_w * 0.5f;
            } else if (c->anchor_h == 4) { // Scale
                float sx = new_w / old_w;
                c->x *= sx;
                c->width = std::max(8.0f, prev_w * sx);
            }
        }

        // Vertical constraint
        if (c->size_mode_h == 2) { // Fill container
            c->y = 0.0f;
            c->height = std::max(8.0f, new_h);
        } else if (c->size_mode_h == 0) {
            if (c->anchor_v == 1) { // Bottom
                float bot_gap = old_h - (c->y + prev_h);
                c->y = new_h - bot_gap - prev_h;
            } else if (c->anchor_v == 2) { // Top + Bottom
                float bot_gap = old_h - (c->y + prev_h);
                c->height = std::max(8.0f, new_h - c->y - bot_gap);
            } else if (c->anchor_v == 3) { // Center
                float delta_c = (c->y + prev_h * 0.5f) - (old_h * 0.5f);
                c->y = (new_h * 0.5f + delta_c) - prev_h * 0.5f;
            } else if (c->anchor_v == 4) { // Scale
                float sy = new_h / old_h;
                c->y *= sy;
                c->height = std::max(8.0f, prev_h * sy);
            }
        }

        if (c->from_import) c->modified_transform = true;
        if (std::fabs(prev_w - c->width) > 0.01f || std::fabs(prev_h - c->height) > 0.01f) {
            ApplyConstraintsOnParentResize(cid, prev_w, prev_h, c->width, c->height);
        }
    }
}

void RmlDocument::GenerateRmlAndRcss(
    std::string& out_rml,
    std::string& out_rcss,
    std::vector<RmlAsset>& out_assets,
    const std::string& rcss_filename,
    bool include_editor_ids
) const {
    out_assets.clear();
    std::ostringstream rml_body;
    std::ostringstream rcss;

    const float sw = static_cast<float>(std::max(1, screen_width));
    const float sh = static_cast<float>(std::max(1, screen_height));

    if (has_imported_source) {
        // Preserva 100% do RCSS original importado (sanitizado para RmlUi) e anexa overrides editados
        std::string clean_imported = SanitizeRcssForRmlUi(imported_raw_rcss);
        rcss << clean_imported;
        if (!clean_imported.empty() && clean_imported.back() != '\n') rcss << "\n";
        std::ostringstream overrides;

        std::function<void(int, int)> emit_imported = [&](int id, int depth) {
            const RmlNode* n = FindNode(id);
            if (!n) return;

            if (n->is_body_root) {
                for (int cid : n->children) emit_imported(cid, depth);
                return;
            }

            std::string indent(static_cast<size_t>(depth) * 2 + 4, ' ');
            bool need_override = (!n->from_import || n->modified_transform || n->modified_style || !n->visible);
            std::string ov_cls = "fc-n" + std::to_string(n->id);

            std::string cls_val = n->rml_class;
            if (need_override) {
                if (!cls_val.empty()) cls_val += " ";
                cls_val += ov_cls;
            }

            if (need_override) {
                const RmlNode* par = FindNode(n->parent_id);
                bool in_flex = (par && par->display_flex && !par->is_body_root);
                float pw = (par && par->width > 1.0f) ? par->width : sw;
                float ph = (par && par->height > 1.0f) ? par->height : sh;
                std::string sel = !n->rml_id.empty() ? ("#" + sanitize_class(n->rml_id)) : ("." + ov_cls);
                overrides << sel << " {\n";
                if (!n->visible) {
                    overrides << "  display: none;\n";
                }
                if (!n->from_import || n->modified_transform) {
                    overrides << "  box-sizing: border-box;\n"
                              << "  margin: 0px;\n";
                    if (in_flex) {
                        overrides << "  position: relative;\n"
                                  << "  left: auto;\n"
                                  << "  top: auto;\n";
                    } else {
                        overrides << "  position: absolute;\n";
                        if (n->size_mode_w != 2) {
                            if (n->anchor_h == 1) overrides << "  right: " << fmt_px(pw - (n->x + n->width)) << ";\n  left: auto;\n";
                            else if (n->anchor_h == 2) overrides << "  left: " << fmt_px(n->x) << ";\n  right: " << fmt_px(pw - (n->x + n->width)) << ";\n";
                            else overrides << "  left: " << fmt_px(n->x) << ";\n";
                        } else {
                            overrides << "  left: 0px;\n";
                        }
                        if (n->size_mode_h != 2) {
                            if (n->anchor_v == 1) overrides << "  bottom: " << fmt_px(ph - (n->y + n->height)) << ";\n  top: auto;\n";
                            else if (n->anchor_v == 2) overrides << "  top: " << fmt_px(n->y) << ";\n  bottom: " << fmt_px(ph - (n->y + n->height)) << ";\n";
                            else overrides << "  top: " << fmt_px(n->y) << ";\n";
                        } else {
                            overrides << "  top: 0px;\n";
                        }
                    }

                    bool fill_main = in_flex && ((par->flex_direction == 0 && n->size_mode_w == 2) ||
                                                 (par->flex_direction == 1 && n->size_mode_h == 2));
                    if (fill_main) overrides << "  flex: 1;\n";
                    else if (in_flex) overrides << "  flex-shrink: 0;\n";

                    if (n->size_mode_w == 1) overrides << "  width: auto;\n";
                    else if (n->size_mode_w == 2) overrides << (in_flex && par->flex_direction == 0 ? "  min-width: 0px;\n  width: auto;\n" : "  width: 100%;\n");
                    else if (!in_flex && n->anchor_h == 2) overrides << "  width: auto;\n";
                    else overrides << "  width: " << fmt_px(n->width) << ";\n";

                    if (n->size_mode_h == 1) overrides << "  height: auto;\n";
                    else if (n->size_mode_h == 2) overrides << (in_flex && par->flex_direction == 1 ? "  min-height: 0px;\n  height: auto;\n" : "  height: 100%;\n");
                    else if (!in_flex && n->anchor_v == 2) overrides << "  height: auto;\n";
                    else overrides << "  height: " << fmt_px(n->height) << ";\n";
                }
                if (!n->from_import || n->modified_style) {
                    if (n->has_bg) overrides << "  background-color: " << n->bg_color.ToCss() << ";\n";
                    else if (n->modified_style) overrides << "  background-color: rgba(0, 0, 0, 0);\n";
                    if (n->border_width > 0.0f) overrides << "  border: " << fmt_px(n->border_width) << " " << n->border_color.ToCss() << ";\n";
                    if (n->border_radius > 0.0f) overrides << "  border-radius: " << fmt_px(n->border_radius) << ";\n";
                    if (n->opacity < 0.999f) {
                        char obuf[32];
                        std::snprintf(obuf, sizeof(obuf), "%.2f", std::clamp(n->opacity, 0.0f, 1.0f));
                        overrides << "  opacity: " << obuf << ";\n";
                    }
                    if (n->display_flex) {
                        overrides << "  display: flex;\n";
                        overrides << "  flex-direction: " << (n->flex_direction == 1 ? "column" : "row") << ";\n";
                        if (n->flex_wrap) overrides << "  flex-wrap: wrap;\n";
                        const char* jc = "flex-start";
                        if (n->justify_content == 1) jc = "center";
                        else if (n->justify_content == 2) jc = "flex-end";
                        else if (n->justify_content == 3) jc = "space-between";
                        overrides << "  justify-content: " << jc << ";\n";
                        const char* ai = "flex-start";
                        if (n->align_items == 1) ai = "center";
                        else if (n->align_items == 2) ai = "flex-end";
                        overrides << "  align-items: " << ai << ";\n";
                        if (n->gap > 0.0f) overrides << "  gap: " << fmt_px(n->gap) << ";\n";
                    }
                    if (n->padding[0] > 0 || n->padding[1] > 0 || n->padding[2] > 0 || n->padding[3] > 0) {
                        overrides << "  padding: " << fmt_px(n->padding[0]) << " " << fmt_px(n->padding[1]) << " "
                                  << fmt_px(n->padding[2]) << " " << fmt_px(n->padding[3]) << ";\n";
                    }
                    overrides << "  color: " << n->text_color.ToCss() << ";\n";
                    overrides << "  font-size: " << fmt_px(n->font_size) << ";\n";
                    if (n->text_align == 1) overrides << "  text-align: center;\n";
                    else if (n->text_align == 2) overrides << "  text-align: right;\n";
                }
                overrides << "}\n\n";
            }

            std::string attrs_str;
            if (!n->rml_id.empty()) attrs_str += " id=\"" + html_escape(n->rml_id) + "\"";
            if (!cls_val.empty()) attrs_str += " class=\"" + html_escape(cls_val) + "\"";
            if (include_editor_ids) attrs_str += " data-fcid=\"" + std::to_string(n->id) + "\"";
            for (const auto& [ak, av] : n->extra_attrs) {
                if (ak == "id" || ak == "class" || ak == "data-fcid") continue;
                attrs_str += " " + ak + "=\"" + html_escape(av) + "\"";
            }

            if (n->tag == "input") {
                bool has_type = false, has_ph = false;
                for (const auto& [ak, _] : n->extra_attrs) {
                    if (ak == "type") has_type = true;
                    if (ak == "placeholder") has_ph = true;
                }
                rml_body << indent << "<input" << attrs_str;
                if (!has_type) rml_body << " type=\"text\"";
                if (!has_ph && !n->placeholder.empty()) rml_body << " placeholder=\"" << html_escape(n->placeholder) << "\"";
                rml_body << " />\n";
            } else if (n->tag == "img") {
                bool has_src = false;
                for (const auto& [ak, _] : n->extra_attrs) {
                    if (ak == "src") has_src = true;
                }
                if (!n->image_bytes.empty() && !n->image_src.empty()) {
                    out_assets.push_back({n->image_src, n->image_bytes});
                }
                rml_body << indent << "<img" << attrs_str;
                if (!has_src && !n->image_src.empty()) rml_body << " src=\"" << html_escape(n->image_src) << "\"";
                rml_body << " />\n";
            } else if (n->tag == "br" || n->tag == "hr") {
                rml_body << indent << "<" << n->tag << attrs_str << " />\n";
            } else {
                rml_body << indent << "<" << n->tag << attrs_str << ">";
                if (!n->text.empty() && n->children.empty()) {
                    rml_body << html_escape(n->text) << "</" << n->tag << ">\n";
                } else {
                    rml_body << "\n";
                    if (!n->text.empty()) {
                        rml_body << indent << "  " << html_escape(n->text) << "\n";
                    }
                    for (int cid : n->children) emit_imported(cid, depth + 1);
                    rml_body << indent << "</" << n->tag << ">\n";
                }
            }
        };

        emit_imported(root_id, 0);

        std::string ov_str = overrides.str();
        if (!ov_str.empty()) {
            rcss << "\n/* === RmlUI Studio Inspector Overrides === */\n" << ov_str;
        }

        std::ostringstream rml;
        rml << "<rml>\n"
            << "  <head>\n"
            << "    <title>" << html_escape(title) << "</title>\n"
            << "    <link type=\"text/rcss\" href=\"" << html_escape(rcss_filename) << "\"/>\n"
            << "  </head>\n"
            << "  <body";
        if (!imported_body_class.empty()) rml << " class=\"" << html_escape(imported_body_class) << "\"";
        if (!imported_body_style.empty()) rml << " style=\"" << html_escape(imported_body_style) << "\"";
        rml << ">\n"
            << rml_body.str()
            << "  </body>\n"
            << "</rml>\n";

        out_rml = rml.str();
        out_rcss = rcss.str();
        return;
    }

    rcss << "/* RCSS Stylesheet - Generated by RmlUI Studio */\n";
    rcss << "body, body.samp-root {\n"
         << "  position: absolute;\n"
         << "  top: 0px;\n"
         << "  left: 0px;\n"
         << "  right: 0px;\n"
         << "  bottom: 0px;\n"
         << "  width: 100%;\n"
         << "  height: 100%;\n"
         << "  margin: 0px;\n"
         << "  padding: 0px;\n"
         << "  overflow: hidden;\n"
         << "  font-family: Arial;\n"
         << "}\n\n";

    std::unordered_set<std::string> used_classes;
    auto unique_class = [&](const std::string& base) -> std::string {
        std::string cls = sanitize_class(base);
        if (used_classes.insert(cls).second) return cls;
        for (int i = 2;; ++i) {
            std::string cand = cls + "-" + std::to_string(i);
            if (used_classes.insert(cand).second) return cand;
        }
    };

    std::function<void(int, int)> emit_node = [&](int id, int depth) {
        const RmlNode* n = FindNode(id);
        if (!n || !n->visible) return;

        std::string cls = unique_class(n->rml_class.empty() ? n->name : n->rml_class);
        std::string indent(static_cast<size_t>(depth) * 2 + 4, ' ');
        bool is_root = (id == root_id);
        bool parent_is_root = (n->parent_id == root_id);

        const RmlNode* par = (!is_root) ? FindNode(n->parent_id) : nullptr;
        bool in_flex = (par && par->display_flex);
        float pw = (par && par->width > 1.0f) ? par->width : sw;
        float ph = (par && par->height > 1.0f) ? par->height : sh;

        // Emit RCSS rule
        rcss << "." << cls << " {\n";
        rcss << "  box-sizing: border-box;\n";
        if (is_root) {
            rcss << "  position: absolute;\n"
                 << "  top: 0px;\n"
                 << "  left: 0px;\n"
                 << "  width: 100%;\n"
                 << "  height: 100%;\n";
        } else if (in_flex) {
            rcss << "  position: relative;\n"
                 << "  margin: 0px;\n";
            bool fill_main = ((par->flex_direction == 0 && n->size_mode_w == 2) ||
                              (par->flex_direction == 1 && n->size_mode_h == 2));
            if (fill_main) rcss << "  flex: 1;\n";
            else rcss << "  flex-shrink: 0;\n";

            if (n->size_mode_w == 1) {
                rcss << "  width: auto;\n";
            } else if (n->size_mode_w == 2) {
                if (par->flex_direction == 0) rcss << "  min-width: 0px;\n  width: auto;\n";
                else rcss << "  width: 100%;\n";
            } else {
                rcss << "  width: " << fmt_px(n->width) << ";\n";
            }

            if (n->size_mode_h == 1) {
                rcss << "  height: auto;\n";
            } else if (n->size_mode_h == 2) {
                if (par->flex_direction == 1) rcss << "  min-height: 0px;\n  height: auto;\n";
                else rcss << "  height: 100%;\n";
            } else {
                rcss << "  height: " << fmt_px(n->height) << ";\n";
            }
        } else if (parent_is_root && n->anchor_h == 0 && n->anchor_v == 0 && n->size_mode_w == 0 && n->size_mode_h == 0) {
            rcss << "  position: absolute;\n"
                 << "  left: " << fmt_pct((n->x / sw) * 100.0f) << ";\n"
                 << "  top: " << fmt_pct((n->y / sh) * 100.0f) << ";\n"
                 << "  width: " << fmt_pct((n->width / sw) * 100.0f) << ";\n"
                 << "  height: " << fmt_pct((n->height / sh) * 100.0f) << ";\n";
        } else {
            rcss << "  position: absolute;\n"
                 << "  margin: 0px;\n";
            if (n->size_mode_w == 1) {
                rcss << "  left: " << fmt_px(n->x) << ";\n"
                     << "  width: auto;\n";
            } else if (n->size_mode_w == 2) {
                rcss << "  left: 0px;\n"
                     << "  width: 100%;\n";
            } else if (n->anchor_h == 1) {
                rcss << "  right: " << fmt_px(pw - (n->x + n->width)) << ";\n"
                     << "  width: " << fmt_px(n->width) << ";\n";
            } else if (n->anchor_h == 2) {
                rcss << "  left: " << fmt_px(n->x) << ";\n"
                     << "  right: " << fmt_px(pw - (n->x + n->width)) << ";\n"
                     << "  width: auto;\n";
            } else if (n->anchor_h == 4) {
                rcss << "  left: " << fmt_pct((n->x / pw) * 100.0f) << ";\n"
                     << "  width: " << fmt_pct((n->width / pw) * 100.0f) << ";\n";
            } else {
                rcss << "  left: " << fmt_px(n->x) << ";\n"
                     << "  width: " << fmt_px(n->width) << ";\n";
            }

            if (n->size_mode_h == 1) {
                rcss << "  top: " << fmt_px(n->y) << ";\n"
                     << "  height: auto;\n";
            } else if (n->size_mode_h == 2) {
                rcss << "  top: 0px;\n"
                     << "  height: 100%;\n";
            } else if (n->anchor_v == 1) {
                rcss << "  bottom: " << fmt_px(ph - (n->y + n->height)) << ";\n"
                     << "  height: " << fmt_px(n->height) << ";\n";
            } else if (n->anchor_v == 2) {
                rcss << "  top: " << fmt_px(n->y) << ";\n"
                     << "  bottom: " << fmt_px(ph - (n->y + n->height)) << ";\n"
                     << "  height: auto;\n";
            } else if (n->anchor_v == 4) {
                rcss << "  top: " << fmt_pct((n->y / ph) * 100.0f) << ";\n"
                     << "  height: " << fmt_pct((n->height / ph) * 100.0f) << ";\n";
            } else {
                rcss << "  top: " << fmt_px(n->y) << ";\n"
                     << "  height: " << fmt_px(n->height) << ";\n";
            }
        }

        if (n->has_bg) {
            rcss << "  background-color: " << n->bg_color.ToCss() << ";\n";
        }
        if (n->border_width > 0.0f) {
            rcss << "  border: " << fmt_px(n->border_width) << " " << n->border_color.ToCss() << ";\n";
        }
        if (n->border_radius > 0.0f) {
            rcss << "  border-radius: " << fmt_px(n->border_radius) << ";\n";
        }
        if (n->opacity < 0.999f) {
            char obuf[32];
            std::snprintf(obuf, sizeof(obuf), "%.2f", std::clamp(n->opacity, 0.0f, 1.0f));
            rcss << "  opacity: " << obuf << ";\n";
        }

        if (n->display_flex) {
            rcss << "  display: flex;\n";
            rcss << "  flex-direction: " << (n->flex_direction == 1 ? "column" : "row") << ";\n";
            if (n->flex_wrap) {
                rcss << "  flex-wrap: wrap;\n";
            }
            const char* jc = "flex-start";
            if (n->justify_content == 1) jc = "center";
            else if (n->justify_content == 2) jc = "flex-end";
            else if (n->justify_content == 3) jc = "space-between";
            rcss << "  justify-content: " << jc << ";\n";
            const char* ai = "flex-start";
            if (n->align_items == 1) ai = "center";
            else if (n->align_items == 2) ai = "flex-end";
            rcss << "  align-items: " << ai << ";\n";
            if (n->gap > 0.0f) {
                rcss << "  gap: " << fmt_px(n->gap) << ";\n";
            }
        }
        if (n->padding[0] > 0 || n->padding[1] > 0 || n->padding[2] > 0 || n->padding[3] > 0) {
            rcss << "  padding: " << fmt_px(n->padding[0]) << " " << fmt_px(n->padding[1]) << " "
                 << fmt_px(n->padding[2]) << " " << fmt_px(n->padding[3]) << ";\n";
        }

        if (n->tag == "button" || n->tag == "input" || n->tag == "span" || !n->text.empty()) {
            rcss << "  color: " << n->text_color.ToCss() << ";\n";
            rcss << "  font-size: " << fmt_px(n->font_size) << ";\n";
            rcss << "  font-weight: " << (n->font_bold ? "bold" : "normal") << ";\n";
            if (n->text_align == 1) rcss << "  text-align: center;\n";
            else if (n->text_align == 2) rcss << "  text-align: right;\n";
        }
        rcss << "}\n\n";

        if (n->has_hover) {
            char hbuf[32];
            std::snprintf(hbuf, sizeof(hbuf), "%.2f", std::clamp(n->hover_opacity, 0.0f, 1.0f));
            rcss << "." << cls << ":hover {\n";
            rcss << "  opacity: " << hbuf << ";\n";
            if (n->has_hover_bg) {
                rcss << "  background-color: " << n->hover_bg.ToCss() << ";\n";
            }
            rcss << "}\n";
            rcss << "." << cls << ":active {\n  opacity: 0.70;\n}\n\n";
        }

        // Emit RML tag
        std::string id_attr = n->rml_id.empty() ? "" : (" id=\"" + html_escape(n->rml_id) + "\"");
        std::string fc_attr = include_editor_ids ? (" data-fcid=\"" + std::to_string(n->id) + "\"") : "";
        if (n->tag == "input") {
            rml_body << indent << "<input type=\"text\"" << id_attr << fc_attr << " class=\"" << cls << "\" placeholder=\""
                     << html_escape(n->placeholder) << "\" />\n";
        } else if (n->tag == "img") {
            std::string src = n->image_src.empty() ? "image.png" : n->image_src;
            if (!n->image_bytes.empty()) {
                out_assets.push_back({src, n->image_bytes});
            }
            rml_body << indent << "<img" << id_attr << fc_attr << " class=\"" << cls << "\" src=\"" << html_escape(src) << "\" />\n";
        } else if (n->tag == "button") {
            rml_body << indent << "<button" << id_attr << fc_attr << " class=\"" << cls << "\"><span>"
                     << html_escape(n->text) << "</span></button>\n";
        } else if (n->tag == "span") {
            rml_body << indent << "<span" << id_attr << fc_attr << " class=\"" << cls << "\">"
                     << html_escape(n->text) << "</span>\n";
        } else {
            rml_body << indent << "<" << n->tag << id_attr << fc_attr << " class=\"" << cls << "\">\n";
            if (!n->text.empty()) {
                rml_body << indent << "  " << html_escape(n->text) << "\n";
            }
            for (int cid : n->children) {
                emit_node(cid, depth + 1);
            }
            rml_body << indent << "</" << n->tag << ">\n";
        }
    };

    emit_node(root_id, 0);

    std::ostringstream rml;
    rml << "<rml>\n"
        << "  <head>\n"
        << "    <title>" << html_escape(title) << "</title>\n"
        << "    <link type=\"text/rcss\" href=\"" << html_escape(rcss_filename) << "\"/>\n"
        << "  </head>\n"
        << "  <body class=\"samp-root\">\n"
        << rml_body.str()
        << "  </body>\n"
        << "</rml>\n";

    out_rml = rml.str();
    out_rcss = rcss.str();
}

bool RmlDocument::ExportToPath(const std::string& rml_path, std::string& out_error) const {
    try {
        fs::path p(rml_path);
        if (p.extension() != ".rml") {
            p.replace_extension(".rml");
        }
        fs::path dir = p.parent_path();
        if (!dir.empty()) {
            fs::create_directories(dir);
        }
        std::string rcss_name = p.stem().string() + ".rcss";
        fs::path rcss_path = dir.empty() ? fs::path(rcss_name) : (dir / rcss_name);

        std::string rml_str, rcss_str;
        std::vector<RmlAsset> assets;
        GenerateRmlAndRcss(rml_str, rcss_str, assets, rcss_name);

        {
            std::ofstream f(p, std::ios::binary);
            if (!f) {
                out_error = "Nao foi possivel gravar " + p.string();
                return false;
            }
            f.write(rml_str.data(), static_cast<std::streamsize>(rml_str.size()));
        }
        {
            std::ofstream f(rcss_path, std::ios::binary);
            if (!f) {
                out_error = "Nao foi possivel gravar " + rcss_path.string();
                return false;
            }
            f.write(rcss_str.data(), static_cast<std::streamsize>(rcss_str.size()));
        }
        for (const auto& asset : assets) {
            fs::path ap = dir.empty() ? fs::path(asset.filename) : (dir / asset.filename);
            if (!ap.parent_path().empty()) {
                fs::create_directories(ap.parent_path());
            }
            std::ofstream af(ap, std::ios::binary);
            if (af) {
                af.write(reinterpret_cast<const char*>(asset.bytes.data()), static_cast<std::streamsize>(asset.bytes.size()));
            }
        }
        return true;
    } catch (const std::exception& e) {
        out_error = e.what();
        return false;
    }
}

namespace {

std::string trim_ws(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool parse_css_len(const std::string& v, float ref_dim, float& out_px) {
    std::string s = trim_ws(v);
    if (s.empty()) return false;
    char* end = nullptr;
    float num = std::strtof(s.c_str(), &end);
    if (end == s.c_str()) return false;
    if (s.find('%') != std::string::npos) {
        out_px = (num / 100.0f) * ref_dim;
        return true;
    }
    out_px = num;
    return true;
}

bool parse_css_color(const std::string& v, RmlColor& out) {
    std::string s = trim_ws(v);
    if (s.empty()) return false;
    if (s[0] == '#') {
        std::string hex = s.substr(1);
        if (hex.size() == 3) {
            std::string e;
            for (char c : hex) { e.push_back(c); e.push_back(c); }
            hex = e;
        }
        unsigned int val = 0;
        if (std::sscanf(hex.c_str(), "%x", &val) != 1) return false;
        if (hex.size() == 6) {
            out.r = (uint8_t)((val >> 16) & 0xFF);
            out.g = (uint8_t)((val >> 8) & 0xFF);
            out.b = (uint8_t)(val & 0xFF);
            out.a = 255;
            return true;
        } else if (hex.size() == 8) {
            out.r = (uint8_t)((val >> 24) & 0xFF);
            out.g = (uint8_t)((val >> 16) & 0xFF);
            out.b = (uint8_t)((val >> 8) & 0xFF);
            out.a = (uint8_t)(val & 0xFF);
            return true;
        }
        return false;
    }
    int r = 0, g = 0, b = 0;
    float af = 255.0f;
    if (std::sscanf(s.c_str(), "rgba(%d,%d,%d,%f)", &r, &g, &b, &af) == 4) {
        out.r = (uint8_t)std::clamp(r, 0, 255);
        out.g = (uint8_t)std::clamp(g, 0, 255);
        out.b = (uint8_t)std::clamp(b, 0, 255);
        out.a = (uint8_t)std::clamp((int)(af <= 1.0f ? af * 255.0f : af), 0, 255);
        return true;
    }
    if (std::sscanf(s.c_str(), "rgb(%d,%d,%d)", &r, &g, &b) == 3) {
        out.r = (uint8_t)std::clamp(r, 0, 255);
        out.g = (uint8_t)std::clamp(g, 0, 255);
        out.b = (uint8_t)std::clamp(b, 0, 255);
        out.a = 255;
        return true;
    }
    return false;
}

} // namespace

namespace {

std::string sanitize_rgba_calls(const std::string& val) {
    std::string out;
    out.reserve(val.size() + 16);
    size_t i = 0;
    const size_t n = val.size();
    while (i < n) {
        if (val[i] == '"' || val[i] == '\'') {
            char q = val[i];
            out.push_back(val[i++]);
            while (i < n) {
                if (val[i] == '\\' && i + 1 < n) {
                    out.push_back(val[i++]);
                    out.push_back(val[i++]);
                } else {
                    char c = val[i++];
                    out.push_back(c);
                    if (c == q) break;
                }
            }
            continue;
        }
        if (i + 1 < n && val[i] == '/' && val[i + 1] == '*') {
            size_t end = val.find("*/", i + 2);
            if (end == std::string::npos) {
                out.append(val, i, n - i);
                break;
            }
            out.append(val, i, end + 2 - i);
            i = end + 2;
            continue;
        }
        bool boundary_ok = (i == 0) || !(std::isalnum(static_cast<unsigned char>(val[i - 1])) || val[i - 1] == '_' || val[i - 1] == '-');
        if (boundary_ok && i + 5 <= n &&
            (val[i] == 'r' || val[i] == 'R') &&
            (val[i + 1] == 'g' || val[i + 1] == 'G') &&
            (val[i + 2] == 'b' || val[i + 2] == 'B') &&
            (val[i + 3] == 'a' || val[i + 3] == 'A') &&
            val[i + 4] == '(') {
            size_t close_p = val.find(')', i + 5);
            if (close_p != std::string::npos) {
                std::string inner = val.substr(i + 5, close_p - (i + 5));
                std::vector<std::string> parts;
                size_t p0 = 0;
                for (;;) {
                    size_t comma = inner.find(',', p0);
                    if (comma == std::string::npos) {
                        parts.push_back(trim_ws(inner.substr(p0)));
                        break;
                    }
                    parts.push_back(trim_ws(inner.substr(p0, comma - p0)));
                    p0 = comma + 1;
                }
                if (parts.size() == 4) {
                    const std::string& a_raw = parts[3];
                    bool is_pct = (!a_raw.empty() && a_raw.back() == '%');
                    bool has_dot = (a_raw.find('.') != std::string::npos);
                    if (!is_pct && has_dot) {
                        char* end_ptr = nullptr;
                        float af = std::strtof(a_raw.c_str(), &end_ptr);
                        if (end_ptr != a_raw.c_str() && *trim_ws(end_ptr).c_str() == '\0') {
                            if (af >= 0.0f && af <= 1.0f) {
                                int ai = static_cast<int>(std::lround(std::clamp(af, 0.0f, 1.0f) * 255.0f));
                                parts[3] = std::to_string(std::clamp(ai, 0, 255));
                            }
                        }
                    }
                    out += "rgba(" + parts[0] + ", " + parts[1] + ", " + parts[2] + ", " + parts[3] + ")";
                    i = close_p + 1;
                    continue;
                }
            }
        }
        out.push_back(val[i++]);
    }
    return out;
}

std::string strip_border_style_keywords(const std::string& val) {
    std::vector<std::string> tokens;
    std::string cur;
    int paren = 0;
    char quote = 0;
    for (size_t i = 0; i < val.size(); ++i) {
        char c = val[i];
        if (quote) {
            cur.push_back(c);
            if (c == '\\' && i + 1 < val.size()) {
                cur.push_back(val[++i]);
            } else if (c == quote) {
                quote = 0;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            cur.push_back(c);
            continue;
        }
        if (c == '(') ++paren;
        else if (c == ')' && paren > 0) --paren;

        if (paren == 0 && (c == ' ' || c == '\t' || c == '\r' || c == '\n')) {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) tokens.push_back(cur);

    auto is_border_style_kw = [](std::string t) {
        std::transform(t.begin(), t.end(), t.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return t == "solid" || t == "dashed" || t == "dotted" || t == "double" ||
               t == "groove" || t == "ridge" || t == "inset" || t == "outset" ||
               t == "none" || t == "hidden";
    };

    std::string joined;
    for (const auto& t : tokens) {
        if (is_border_style_kw(t)) continue;
        if (!joined.empty()) joined.push_back(' ');
        joined += t;
    }
    return joined.empty() ? std::string("0px") : joined;
}

std::string sanitize_single_declaration(const std::string& decl) {
    // Preserve leading whitespace so indentation survives cleanly
    size_t first_non_ws = decl.find_first_not_of(" \t\r\n");
    if (first_non_ws == std::string::npos) return decl;
    std::string leading = decl.substr(0, first_non_ws);
    std::string body = decl.substr(first_non_ws);

    // Find first ':' outside quotes/comments
    size_t colon = std::string::npos;
    for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '"' || body[i] == '\'') {
            char q = body[i++];
            while (i < body.size()) {
                if (body[i] == '\\' && i + 1 < body.size()) i += 2;
                else if (body[i++] == q) break;
            }
            --i;
            continue;
        }
        if (i + 1 < body.size() && body[i] == '/' && body[i + 1] == '*') {
            size_t end = body.find("*/", i + 2);
            if (end == std::string::npos) break;
            i = end + 1;
            continue;
        }
        if (body[i] == ':') {
            colon = i;
            break;
        }
    }
    if (colon == std::string::npos) return decl;

    std::string prop = trim_ws(body.substr(0, colon));
    std::string prop_low = prop;
    std::transform(prop_low.begin(), prop_low.end(), prop_low.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    std::string val_raw = body.substr(colon + 1);
    std::string val = sanitize_rgba_calls(trim_ws(val_raw));
    if (prop_low == "border" || prop_low == "border-top" || prop_low == "border-right" ||
        prop_low == "border-bottom" || prop_low == "border-left") {
        val = strip_border_style_keywords(val);
    } else if (prop_low == "font-family" && !val.empty()) {
        std::string val_low = val;
        std::transform(val_low.begin(), val_low.end(), val_low.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (val_low.find("segoe ui") == std::string::npos) {
            size_t cmt_pos = val.rfind("/*");
            if (cmt_pos != std::string::npos && val.find("*/", cmt_pos + 2) != std::string::npos &&
                trim_ws(val.substr(val.find("*/", cmt_pos + 2) + 2)).empty()) {
                std::string before_cmt = trim_ws(val.substr(0, cmt_pos));
                if (!before_cmt.empty()) {
                    val = before_cmt + ", \"Segoe UI\", \"Consolas\" " + val.substr(cmt_pos);
                } else {
                    val += ", \"Segoe UI\", \"Consolas\"";
                }
            } else {
                val += ", \"Segoe UI\", \"Consolas\"";
            }
        }
    }
    return leading + prop + ": " + val;
}

} // namespace

std::string SanitizeRcssForRmlUi(const std::string& raw_rcss) {
    try {
        std::string src = raw_rcss;
        if (src.size() >= 3 &&
            static_cast<unsigned char>(src[0]) == 0xEF &&
            static_cast<unsigned char>(src[1]) == 0xBB &&
            static_cast<unsigned char>(src[2]) == 0xBF) {
            src.erase(0, 3);
        }

        std::string out;
        out.reserve(src.size() + 64);

        int brace_depth = 0;
        std::string decl_buf;
        size_t i = 0;
        const size_t n = src.size();
        auto flush_decl = [&](bool append_semi) {
            if (!decl_buf.empty()) {
                out += sanitize_single_declaration(decl_buf);
                decl_buf.clear();
            }
            if (append_semi) out.push_back(';');
        };

        while (i < n) {
            char c = src[i];
            if (i + 1 < n && c == '/' && src[i + 1] == '*') {
                size_t end = src.find("*/", i + 2);
                std::string cmt = (end == std::string::npos) ? src.substr(i) : src.substr(i, end + 2 - i);
                if (brace_depth > 0) decl_buf += cmt;
                else out += cmt;
                i = (end == std::string::npos) ? n : (end + 2);
                continue;
            }
            if (c == '"' || c == '\'') {
                size_t start = i++;
                while (i < n) {
                    if (src[i] == '\\' && i + 1 < n) i += 2;
                    else if (src[i++] == c) break;
                }
                std::string lit = src.substr(start, i - start);
                if (brace_depth > 0) decl_buf += lit;
                else out += lit;
                continue;
            }
            if (c == '{') {
                if (brace_depth > 0 && !decl_buf.empty()) {
                    out += decl_buf;
                    decl_buf.clear();
                }
                ++brace_depth;
                out.push_back('{');
                ++i;
                continue;
            }
            if (c == '}') {
                if (brace_depth > 0) {
                    flush_decl(false);
                    --brace_depth;
                }
                out.push_back('}');
                ++i;
                continue;
            }
            if (brace_depth > 0 && c == ';') {
                flush_decl(true);
                ++i;
                continue;
            }
            if (brace_depth > 0) {
                decl_buf.push_back(c);
            } else {
                out.push_back(c);
            }
            ++i;
        }
        if (!decl_buf.empty()) {
            out += sanitize_single_declaration(decl_buf);
        }
        return out;
    } catch (...) {
        return raw_rcss;
    }
}

bool RmlDocument::ImportRcssFile(const std::string& rcss_path, std::string& out_error) {
    try {
        std::ifstream ifs(fs::u8path(rcss_path), std::ios::binary);
        if (!ifs) {
            out_error = "Nao foi possivel abrir o arquivo .rcss: " + rcss_path;
            return false;
        }
        std::string raw_css((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        has_imported_source = true;
        imported_raw_rcss = SanitizeRcssForRmlUi(raw_css);

        for (auto& n : nodes) {
            n.modified_transform = false;
            n.modified_style = false;
        }
        Touch();
        return true;
    } catch (const std::exception& e) {
        out_error = std::string("Falha ao importar .rcss: ") + e.what();
        return false;
    } catch (...) {
        out_error = "Erro desconhecido ao importar .rcss.";
        return false;
    }
}
