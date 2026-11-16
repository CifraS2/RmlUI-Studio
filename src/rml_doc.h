#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct RmlColor {
    uint8_t r = 255;
    uint8_t g = 255;
    uint8_t b = 255;
    uint8_t a = 255;

    std::string ToCss() const;
    void ToImGuiFloat4(float out[4]) const;
    void FromImGuiFloat4(const float in[4]);
};

enum class RmlCssUnit : uint8_t {
    Px = 0,
    Percent = 1,
    Dp = 2,
    Em = 3
};

enum class RmlPositionMode : uint8_t {
    Static = 0,
    Relative = 1,
    Absolute = 2
};

struct RmlNode {
    int id = 0;
    int parent_id = -1;
    std::string name;
    std::string tag = "div"; // div, button, input, span, img, progress
    std::string rml_id;
    std::string rml_class;

    // Position relative to parent (for root, 0,0)
    float x = 0.0f;
    float y = 0.0f;
    float width = 200.0f;
    float height = 100.0f;
    bool visible = true;
    bool locked = false;
    bool collapsed = false;

    // Content
    std::string text;
    std::string placeholder;

    // Appearance
    bool has_bg = true;
    RmlColor bg_color{28, 28, 34, 255};
    RmlColor border_color{0, 122, 255, 255};
    float border_width = 0.0f;
    float border_radius = 0.0f;
    float opacity = 1.0f;

    // Typography
    RmlColor text_color{255, 255, 255, 255};
    float font_size = 14.0f;
    bool font_bold = false;
    int text_align = 0; // 0=left, 1=center, 2=right

    // Flex layout
    bool display_flex = false;
    int flex_direction = 0;  // 0=row, 1=column
    int justify_content = 0; // 0=flex-start, 1=center, 2=flex-end, 3=space-between
    int align_items = 0;     // 0=flex-start, 1=center, 2=flex-end
    bool flex_wrap = false;
    float gap = 0.0f;
    float padding[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // top, right, bottom, left

    // Resizing & Constraints (size_mode: 0=Fixed, 1=Hug, 2=Fill)
    // anchor_h: 0=Left, 1=Right, 2=Left+Right, 3=Center, 4=Scale
    // anchor_v: 0=Top,  1=Bottom,2=Top+Bottom, 3=Center, 4=Scale
    int size_mode_w = 0;
    int size_mode_h = 0;
    int anchor_h = 0;
    int anchor_v = 0;

    // Layout Grids (per-frame / container visual columns, rows, or grid)
    bool grid_enabled = false;
    int grid_type = 0;      // 0=Columns, 1=Rows, 2=Grid
    int grid_count = 12;    // columns / rows count
    float grid_gutter = 20.0f;
    float grid_margin = 24.0f;
    float grid_size = 16.0f; // cell size for Uniform Grid
    RmlColor grid_color{239, 68, 68, 46};

    // Interactive :hover
    bool has_hover = false;
    float hover_opacity = 0.85f;
    bool has_hover_bg = false;
    RmlColor hover_bg{0, 100, 220, 255};

    // Image asset (for <img>) — shared immutable buffer across Undo/Redo snapshots
    std::string image_src;
    std::shared_ptr<const std::vector<uint8_t>> image_data;

    // Raw DOM / preservation metadata (for imported .rml/.rcss)
    bool is_body_root = false;
    bool from_import = false;
    bool css_positioned = false;
    RmlPositionMode pos_mode = RmlPositionMode::Absolute;
    float rel_x = 0.0f;
    float rel_y = 0.0f;
    float margin_left = 0.0f;
    float margin_top = 0.0f;
    bool has_margin_left = false;
    bool has_margin_top = false;
    bool modified_transform = false;
    bool modified_style = false;
    bool explicit_anchor_or_margin = false;
    RmlCssUnit unit_x = RmlCssUnit::Px;
    RmlCssUnit unit_y = RmlCssUnit::Px;
    RmlCssUnit unit_w = RmlCssUnit::Px;
    RmlCssUnit unit_h = RmlCssUnit::Px;
    float em_base_px = 14.0f;
    float dp_ratio = 1.0f;
    std::vector<std::pair<std::string, std::string>> extra_attrs;

    std::vector<int> children;

    /// Computes a concise fallback display label from DOM id, text/placeholder, first class, or tag.
    std::string DefaultDisplayName(size_t max_text_bytes = 22) const;
};

struct RmlAsset {
    std::string filename;
    std::shared_ptr<const std::vector<uint8_t>> data;
};

class RmlDocument {
public:
    std::string title = "RmlUI Studio Interface";
    int screen_width = 1920;
    int screen_height = 1080;
    int root_id = 0;
    int next_id = 1;
    uint64_t revision = 1;
    std::vector<RmlNode> nodes;
    std::string base_dir; // directory of imported .rml for resolving relative assets
    // Shared immutable image asset cache (key -> buffer), preserved by pointer across Undo/Redo snapshots
    std::unordered_map<std::string, std::shared_ptr<const std::vector<uint8_t>>> asset_cache;

    // Preserved imported RML/RCSS data
    bool has_imported_source = false;
    std::string imported_raw_rml;
    std::string imported_raw_rcss;
    std::string imported_body_class;
    std::string imported_body_style;

    RmlDocument();

    void ResetDefaultScreen();
    RmlNode* FindNode(int id);
    const RmlNode* FindNode(int id) const;

    std::shared_ptr<const std::vector<uint8_t>> CacheAssetBytes(const std::string& key, std::vector<uint8_t> bytes);
    std::shared_ptr<const std::vector<uint8_t>> FindCachedAsset(const std::string& key) const;
    void SetNodeImageBytes(int node_id, const std::string& filename, std::vector<uint8_t> bytes);

    // World/screen-space coordinates (in 1920x1080 space)
    void GetWorldPos(int id, float& out_x, float& out_y) const;
    int HitTest(float screen_x, float screen_y) const;

    int AddDiv(int parent_id, const std::string& name, float x, float y, float w, float h);
    int AddButton(int parent_id, const std::string& label, float x, float y, float w, float h);
    int AddInput(int parent_id, const std::string& placeholder, float x, float y, float w, float h);
    int AddText(int parent_id, const std::string& text, float x, float y, float w, float h);
    int AddImage(int parent_id, const std::string& filename, const std::vector<uint8_t>& bytes, float x, float y, float w, float h);

    bool DeleteNode(int id);
    int DuplicateNode(int id);
    void MoveNode(int id, float dx, float dy);
    bool ReparentNode(int id, int new_parent_id);
    bool ReorderChildNode(int parent_id, int child_id, int new_index);
    void ApplyConstraintsOnParentResize(int parent_id, float old_w, float old_h, float new_w, float new_h);
    void Touch() { ++revision; }

    void GenerateRmlAndRcss(
        std::string& out_rml,
        std::string& out_rcss,
        std::vector<RmlAsset>& out_assets,
        const std::string& rcss_filename = "style.rcss",
        bool include_editor_ids = false,
        std::unordered_map<int, std::string>* out_node_classes = nullptr
    ) const;

    bool ExportToPath(const std::string& rml_path, std::string& out_error) const;
    bool ImportRcssFile(const std::string& rcss_path, std::string& out_error);
};

std::string StripUtf8Bom(const std::string& raw);
std::string SanitizeRcssForRmlUi(const std::string& raw_rcss);
std::string FormatRmlLength(float px_val, float ref_dim_px, RmlCssUnit unit, float em_base_px = 14.0f, float dp_ratio = 1.0f);
