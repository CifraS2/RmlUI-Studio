#include "rml_engine.h"
#include "studio_icons.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>

#include "../third_party/RmlUi/Backends/RmlUi_Include_GL3.h"
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include "../third_party/RmlUi/Backends/RmlUi_Platform_GLFW.h"
#include "../third_party/RmlUi/Backends/RmlUi_Renderer_GL3.h"
#include <GLFW/glfw3.h>

#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb/stb_image.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace {

std::string normalize_vpath(const Rml::String& path) {
    std::string s(path.begin(), path.end());
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.size() >= 2 && s[0] == '.' && s[1] == '/') {
        s.erase(0, 2);
    }
    if (!s.empty() && s[0] == '/') {
        s.erase(0, 1);
    }
    return s;
}

bool decode_image_stb(const uint8_t* bytes, size_t len, int& out_w, int& out_h, std::vector<uint8_t>& out_premul_rgba) {
    if (!bytes || len == 0) return false;
    int w = 0, h = 0, ch = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes, static_cast<int>(len), &w, &h, &ch, 4);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        return false;
    }
    const size_t nbytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
    out_premul_rgba.resize(nbytes);
    for (size_t i = 0; i < nbytes; i += 4u) {
        uint32_t a = pixels[i + 3];
        out_premul_rgba[i + 0] = static_cast<uint8_t>((static_cast<uint32_t>(pixels[i + 0]) * a + 127u) / 255u);
        out_premul_rgba[i + 1] = static_cast<uint8_t>((static_cast<uint32_t>(pixels[i + 1]) * a + 127u) / 255u);
        out_premul_rgba[i + 2] = static_cast<uint8_t>((static_cast<uint32_t>(pixels[i + 2]) * a + 127u) / 255u);
        out_premul_rgba[i + 3] = static_cast<uint8_t>(a);
    }
    stbi_image_free(pixels);
    out_w = w;
    out_h = h;
    return true;
}

struct OpenFile {
    std::vector<uint8_t> owned;
    std::shared_ptr<const std::vector<uint8_t>> shared_ref;
    const std::vector<uint8_t>* data = nullptr;
    size_t pos = 0;
};

class StudioSystemInterface final : public SystemInterface_GLFW {
public:
    explicit StudioSystemInterface(GLFWwindow* window) : SystemInterface_GLFW(window) {}
    int xml_error_count = 0;
    bool capture_logs = false;
    std::vector<RmlEngineLogEntry> captured_logs;

    void BeginCapture() {
        captured_logs.clear();
        capture_logs = true;
    }

    void EndCapture() {
        capture_logs = false;
    }

    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        if (type == Rml::Log::LT_ERROR) {
            if (message.find("XML") != Rml::String::npos || message.find("Failed to load") != Rml::String::npos) {
                ++xml_error_count;
            }
        }
        if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_WARNING || type == Rml::Log::LT_ASSERT) {
            if (capture_logs && captured_logs.size() < 64u) {
                bool is_err = (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT);
                captured_logs.push_back({is_err, std::string(message.begin(), message.end())});
            }
            std::fprintf(stderr, "[Rml:%d] %s\n", static_cast<int>(type), message.c_str());
        }
        return true;
    }
};

struct ScopedCanvasLogCapture {
    StudioSystemInterface* sys = nullptr;
    explicit ScopedCanvasLogCapture(StudioSystemInterface* s) : sys(s) {
        if (sys) sys->BeginCapture();
    }
    ~ScopedCanvasLogCapture() {
        if (sys) sys->EndCapture();
    }
};

class StudioFileInterface final : public Rml::FileInterface {
public:
    std::unordered_map<std::string, std::vector<uint8_t>> virtual_files;
    std::unordered_map<std::string, std::shared_ptr<const std::vector<uint8_t>>> virtual_assets;
    std::string base_dir;

    Rml::FileHandle Open(const Rml::String& path) override {
        std::string norm = normalize_vpath(path);
        auto ait = virtual_assets.find(norm);
        if (ait == virtual_assets.end()) {
            auto slash = norm.find_last_of('/');
            if (slash != std::string::npos) {
                ait = virtual_assets.find(norm.substr(slash + 1));
            }
        }
        if (ait != virtual_assets.end() && ait->second) {
            auto* f = new OpenFile();
            f->shared_ref = ait->second;
            f->data = f->shared_ref.get();
            return reinterpret_cast<Rml::FileHandle>(f);
        }

        auto it = virtual_files.find(norm);
        if (it == virtual_files.end()) {
            auto slash = norm.find_last_of('/');
            if (slash != std::string::npos) {
                it = virtual_files.find(norm.substr(slash + 1));
            }
        }
        if (it != virtual_files.end()) {
            auto* f = new OpenFile();
            f->data = &it->second;
            return reinterpret_cast<Rml::FileHandle>(f);
        }

        fs::path disk_path(std::string(path.begin(), path.end()));
        if (disk_path.is_relative() && !base_dir.empty()) {
            disk_path = fs::path(base_dir) / disk_path;
        }
        std::ifstream ifs(disk_path, std::ios::binary | std::ios::ate);
        if (ifs) {
            auto sz = ifs.tellg();
            ifs.seekg(0, std::ios::beg);
            auto* f = new OpenFile();
            f->owned.resize(static_cast<size_t>(sz));
            if (sz > 0) ifs.read(reinterpret_cast<char*>(f->owned.data()), sz);
            f->data = &f->owned;
            return reinterpret_cast<Rml::FileHandle>(f);
        }
        return 0;
    }

    void Close(Rml::FileHandle file) override { delete reinterpret_cast<OpenFile*>(file); }
    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override {
        auto* f = reinterpret_cast<OpenFile*>(file);
        if (!f || !f->data || f->pos >= f->data->size()) return 0;
        size_t n = std::min(size, f->data->size() - f->pos);
        std::memcpy(buffer, f->data->data() + f->pos, n);
        f->pos += n;
        return n;
    }
    bool Seek(Rml::FileHandle file, long offset, int origin) override {
        auto* f = reinterpret_cast<OpenFile*>(file);
        if (!f || !f->data) return false;
        long long base = (origin == SEEK_CUR) ? static_cast<long long>(f->pos)
            : ((origin == SEEK_END) ? static_cast<long long>(f->data->size()) : 0LL);
        f->pos = static_cast<size_t>(std::clamp(base + static_cast<long long>(offset), 0LL, static_cast<long long>(f->data->size())));
        return true;
    }
    size_t Tell(Rml::FileHandle file) override {
        auto* f = reinterpret_cast<OpenFile*>(file);
        return f ? f->pos : 0;
    }
    size_t Length(Rml::FileHandle file) override {
        auto* f = reinterpret_cast<OpenFile*>(file);
        return (f && f->data) ? f->data->size() : 0;
    }
};

class StudioGl3RenderInterface final : public RenderInterface_GL3 {
public:
    StudioFileInterface* file_iface = nullptr;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override {
        auto ico_pos = source.rfind("studio_ico_");
        if (ico_pos != Rml::String::npos) {
            std::string ico_name = source.substr(ico_pos + 11);
            int iw = 0, ih = 0;
            std::vector<uint8_t> irgba;
            if (RenderStudioIcon32(ico_name, iw, ih, irgba)) {
                texture_dimensions = {iw, ih};
                return GenerateTexture(Rml::Span<const Rml::byte>(irgba.data(), irgba.size()), {iw, ih});
            }
        }
        if (source == "studio_cp_hue") {
            const int w = 24, h = 256;
            std::vector<uint8_t> rgba(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 255);
            for (int y = 0; y < h; ++y) {
                float hue = 360.0f * (1.0f - static_cast<float>(y) / static_cast<float>(h - 1));
                if (hue >= 360.0f) hue = 0.0f;
                float sec = hue / 60.0f;
                int idx = static_cast<int>(std::floor(sec)) % 6;
                float f = sec - std::floor(sec);
                uint8_t up = static_cast<uint8_t>(std::clamp(static_cast<int>(std::round(f * 255.0f)), 0, 255));
                uint8_t dn = static_cast<uint8_t>(std::clamp(static_cast<int>(std::round((1.0f - f) * 255.0f)), 0, 255));
                uint8_t r = 255, g = 0, b = 0;
                switch (idx) {
                    case 0: r = 255; g = up;  b = 0;   break;
                    case 1: r = dn;  g = 255; b = 0;   break;
                    case 2: r = 0;   g = 255; b = up;  break;
                    case 3: r = 0;   g = dn;  b = 255; break;
                    case 4: r = up;  g = 0;   b = 255; break;
                    default: r = 255; g = 0;  b = dn;  break;
                }
                for (int x = 0; x < w; ++x) {
                    size_t pos = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
                    rgba[pos + 0] = r; rgba[pos + 1] = g; rgba[pos + 2] = b; rgba[pos + 3] = 255;
                }
            }
            texture_dimensions = {w, h};
            return GenerateTexture(Rml::Span<const Rml::byte>(rgba.data(), rgba.size()), {w, h});
        }
        if (source == "studio_cp_sv_white") {
            const int w = 256, h = 2;
            std::vector<uint8_t> rgba(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    uint8_t a = static_cast<uint8_t>(255 - x);
                    size_t pos = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
                    rgba[pos + 0] = a; rgba[pos + 1] = a; rgba[pos + 2] = a; rgba[pos + 3] = a;
                }
            }
            texture_dimensions = {w, h};
            return GenerateTexture(Rml::Span<const Rml::byte>(rgba.data(), rgba.size()), {w, h});
        }
        if (source == "studio_cp_sv_black") {
            const int w = 2, h = 256;
            std::vector<uint8_t> rgba(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0);
            for (int y = 0; y < h; ++y) {
                uint8_t a = static_cast<uint8_t>(y);
                for (int x = 0; x < w; ++x) {
                    size_t pos = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
                    rgba[pos + 0] = 0; rgba[pos + 1] = 0; rgba[pos + 2] = 0; rgba[pos + 3] = a;
                }
            }
            texture_dimensions = {w, h};
            return GenerateTexture(Rml::Span<const Rml::byte>(rgba.data(), rgba.size()), {w, h});
        }
        if (source == "studio_cp_checker") {
            const int w = 16, h = 16;
            std::vector<uint8_t> rgba(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 255);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    bool light = (((x / 4) + (y / 4)) & 1) == 0;
                    uint8_t c = light ? 96 : 56;
                    size_t pos = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
                    rgba[pos + 0] = c; rgba[pos + 1] = c; rgba[pos + 2] = c; rgba[pos + 3] = 255;
                }
            }
            texture_dimensions = {w, h};
            return GenerateTexture(Rml::Span<const Rml::byte>(rgba.data(), rgba.size()), {w, h});
        }
        if (!file_iface) return 0;
        Rml::FileHandle fh = file_iface->Open(source);
        if (!fh) return 0;
        size_t len = file_iface->Length(fh);
        std::vector<uint8_t> raw(len);
        if (len > 0) file_iface->Read(raw.data(), len, fh);
        file_iface->Close(fh);

        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        if (!decode_image_stb(raw.data(), raw.size(), w, h, rgba)) return 0;
        texture_dimensions = {w, h};
        return GenerateTexture(Rml::Span<const Rml::byte>(rgba.data(), rgba.size()), {w, h});
    }
};
} // namespace

namespace {

class FallbackFontEngineInterface final : public Rml::FontEngineInterface {
public:
    void SetInner(Rml::FontEngineInterface* inner) { inner_ = inner; }
    Rml::FontEngineInterface* Inner() const { return inner_; }

    void Initialize() override { if (inner_) inner_->Initialize(); }
    void Shutdown() override { if (inner_) inner_->Shutdown(); }
    bool LoadFontFace(const Rml::String& file_name, int face_index, bool fallback_face, Rml::Style::FontWeight weight) override {
        return inner_ ? inner_->LoadFontFace(file_name, face_index, fallback_face, weight) : false;
    }
    bool LoadFontFace(const Rml::String& file_name, int face_index, const Rml::String& font_family,
                      Rml::Style::FontStyle style, Rml::Style::FontWeight weight, bool fallback_face) override {
        return inner_ ? inner_->LoadFontFace(file_name, face_index, font_family, style, weight, fallback_face) : false;
    }
    bool LoadFontFace(Rml::Span<const Rml::byte> data, int face_index, const Rml::String& font_family,
                      Rml::Style::FontStyle style, Rml::Style::FontWeight weight, bool fallback_face) override {
        return inner_ ? inner_->LoadFontFace(data, face_index, font_family, style, weight, fallback_face) : false;
    }
    Rml::FontFaceHandle GetFontFaceHandle(const Rml::String& family, Rml::Style::FontStyle style,
                                          Rml::Style::FontWeight weight, int size) override {
        if (!inner_) return 0;
        if (!family.empty()) {
            if (auto h = inner_->GetFontFaceHandle(family, style, weight, size)) return h;
            size_t pos = 0;
            while (pos < family.size()) {
                size_t comma = family.find(',', pos);
                std::string tok = (comma == std::string::npos) ? family.substr(pos) : family.substr(pos, comma - pos);
                pos = (comma == std::string::npos) ? family.size() : (comma + 1);
                auto trim_q = [](std::string s) {
                    size_t a = s.find_first_not_of(" \t\r\n\"'");
                    if (a == std::string::npos) return std::string();
                    size_t b = s.find_last_not_of(" \t\r\n\"'");
                    s = s.substr(a, b - a + 1);
                    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    return s;
                };
                std::string cand = trim_q(tok);
                if (cand.empty()) continue;
                if (auto h = inner_->GetFontFaceHandle(cand, style, weight, size)) return h;
                if (cand == "monospace" || cand == "consolas" || cand == "courier new" || cand == "courier") {
                    const char* mono_fallbacks[] = {"consolas", "liberation mono", "noto sans mono", "dejavu sans mono", "ubuntu mono"};
                    for (const char* mf : mono_fallbacks) {
                        if (auto h = inner_->GetFontFaceHandle(mf, style, weight, size)) return h;
                    }
                } else if (cand == "sans-serif" || cand == "system-ui" || cand == "segoe ui" ||
                           cand == "arial" || cand == "helvetica" || cand == "tahoma" || cand == "verdana") {
                    const char* sans_fallbacks[] = {"segoe ui", "arial", "liberation sans", "noto sans", "ubuntu", "dejavu sans"};
                    for (const char* sf : sans_fallbacks) {
                        if (auto h = inner_->GetFontFaceHandle(sf, style, weight, size)) return h;
                    }
                }
            }
        }
        const char* defaults[] = {"segoe ui", "arial", "liberation sans", "noto sans", "ubuntu", "dejavu sans", "consolas", "liberation mono", "dejavu sans mono"};
        for (const char* def : defaults) {
            if (auto h = inner_->GetFontFaceHandle(def, style, weight, size)) return h;
        }
        return 0;
    }
    Rml::FontEffectsHandle PrepareFontEffects(Rml::FontFaceHandle handle, const Rml::FontEffectList& font_effects) override {
        return inner_ ? inner_->PrepareFontEffects(handle, font_effects) : 0;
    }
    const Rml::FontMetrics& GetFontMetrics(Rml::FontFaceHandle handle) override {
        static Rml::FontMetrics empty{};
        return inner_ ? inner_->GetFontMetrics(handle) : empty;
    }
    int GetStringWidth(Rml::FontFaceHandle handle, Rml::StringView string, const Rml::TextShapingContext& tsc, Rml::Character prior) override {
        return inner_ ? inner_->GetStringWidth(handle, string, tsc, prior) : 0;
    }
    int GenerateString(Rml::RenderManager& rm, Rml::FontFaceHandle fh, Rml::FontEffectsHandle feh, Rml::StringView string,
                       Rml::Vector2f pos, Rml::ColourbPremultiplied col, float op, const Rml::TextShapingContext& tsc,
                       Rml::TexturedMeshList& mesh_list) override {
        return inner_ ? inner_->GenerateString(rm, fh, feh, string, pos, col, op, tsc, mesh_list) : 0;
    }
    int GetVersion(Rml::FontFaceHandle handle) override {
        return inner_ ? inner_->GetVersion(handle) : 0;
    }
    void ReleaseFontResources() override {
        if (inner_) inner_->ReleaseFontResources();
    }
private:
    Rml::FontEngineInterface* inner_ = nullptr;
};

} // namespace

struct RmlEngineHost::Impl {
    std::unique_ptr<StudioSystemInterface> sys;
    StudioFileInterface file;
    std::unique_ptr<StudioGl3RenderInterface> render;
    FallbackFontEngineInterface font_fallback;

    Rml::Context* canvas_ctx = nullptr;
    Rml::Context* editor_ctx = nullptr;
    Rml::ElementDocument* canvas_doc = nullptr;
    Rml::ElementDocument* editor_doc = nullptr;

    Rml::TextureHandle canvas_tex = 0;
    uint64_t canvas_tex_gen = 0;
    int canvas_w = 0;
    int canvas_h = 0;
    bool canvas_interactive_dirty = true;
    double last_interactive_update_sec = 0.0;

    int editor_w = 1440;
    int editor_h = 860;
    bool initialized = false;

    // Incremental DOM cache (Fase 4)
    std::unordered_map<int, Rml::Element*> dom_node_map_;
    std::unordered_set<int> dirty_inline_box_ids_;
    std::vector<RmlEngineLogEntry> last_canvas_logs;
    uint64_t canvas_log_seq = 1;
    bool has_synced_doc = false;
    bool last_synced_imported = false;
    int last_synced_root_id = -1;
    std::string last_synced_body_class;
    std::string last_synced_body_style;
    std::string last_synced_title;
    std::string last_synced_rcss;
    std::unordered_map<int, RmlNode> last_synced_nodes;
    std::unordered_map<int, std::string> last_synced_classes;

    void InvalidateDomCache() {
        dom_node_map_.clear();
        dirty_inline_box_ids_.clear();
        has_synced_doc = false;
        canvas_interactive_dirty = true;
        last_synced_rcss.clear();
        last_synced_nodes.clear();
        last_synced_classes.clear();
    }

    void RebuildDomNodeMap() {
        dom_node_map_.clear();
        if (!canvas_doc) return;
        std::function<void(Rml::Element*)> walk = [&](Rml::Element* el) {
            if (!el) return;
            const std::string& tag = el->GetTagName();
            if (tag.empty() || tag[0] == '#') return;
            Rml::String fcid = el->GetAttribute<Rml::String>("data-fcid", "");
            if (!fcid.empty()) {
                char* endp = nullptr;
                long nid = std::strtol(fcid.c_str(), &endp, 10);
                if (endp != fcid.c_str() && *endp == '\0') {
                    dom_node_map_[static_cast<int>(nid)] = el;
                }
            }
            const int n = el->GetNumChildren();
            for (int i = 0; i < n; ++i) {
                walk(el->GetChild(i));
            }
        };
        walk(canvas_doc);
    }

    Rml::Element* FindDomNode(int node_id) {
        if (!canvas_doc) return nullptr;
        auto it = dom_node_map_.find(node_id);
        if (it != dom_node_map_.end() && it->second) {
            return it->second;
        }
        return nullptr;
    }

    void ClearTrackedInlineBoxProps() {
        if (dirty_inline_box_ids_.empty()) return;
        static const char* kBoxProps[] = {
            "box-sizing", "margin", "position",
            "left", "right", "top", "bottom", "width", "height"
        };
        for (int nid : dirty_inline_box_ids_) {
            if (Rml::Element* el = FindDomNode(nid)) {
                for (const char* prop : kBoxProps) {
                    el->RemoveProperty(prop);
                }
            }
        }
        dirty_inline_box_ids_.clear();
    }

    void ReleaseCanvasRT() {
        if (canvas_tex && render) {
            render->ReleaseTexture(canvas_tex);
        }
        canvas_tex = 0;
        canvas_w = 0;
        canvas_h = 0;
    }

    void RenderCanvasToTexture(int w, int h) {
        if (!render || !canvas_ctx) return;
        ReleaseCanvasRT();
        w = std::max(64, w);
        h = std::max(64, h);

        render->SetViewport(w, h);
        render->BeginFrame();
        render->EnableScissorRegion(true);
        render->SetScissorRegion(Rml::Rectanglei::FromSize({w, h}));
        canvas_ctx->Render();
        render->EnableScissorRegion(true);
        render->SetScissorRegion(Rml::Rectanglei::FromSize({w, h}));
        canvas_tex = render->SaveLayerAsTexture();
        render->EndFrame();
        glBindVertexArray(0);
        glUseProgram(0);

        ++canvas_tex_gen;
        canvas_w = w;
        canvas_h = h;
        last_interactive_update_sec = glfwGetTime();
        canvas_interactive_dirty = false;
    }
};

RmlEngineHost::RmlEngineHost() : impl_(new Impl()) {}
RmlEngineHost::~RmlEngineHost() {
    Shutdown();
    delete impl_;
    impl_ = nullptr;
}

bool RmlEngineHost::Init(GLFWwindow* window) {
    if (impl_->initialized) return true;
    Rml::String gl_msg;
    if (!RmlGL3::Initialize(&gl_msg)) {
        std::fprintf(stderr, "[RmlGL3] %s\n", gl_msg.c_str());
        return false;
    }

    impl_->sys = std::make_unique<StudioSystemInterface>(window);
    impl_->render = std::make_unique<StudioGl3RenderInterface>();
    if (!(*impl_->render)) {
        impl_->render.reset();
        impl_->sys.reset();
        RmlGL3::Shutdown();
        return false;
    }
    impl_->render->file_iface = &impl_->file;

    Rml::SetSystemInterface(impl_->sys.get());
    Rml::SetFileInterface(&impl_->file);
    Rml::SetRenderInterface(impl_->render.get());
    if (!Rml::Initialise()) {
        impl_->render.reset();
        impl_->sys.reset();
        RmlGL3::Shutdown();
        return false;
    }
    impl_->font_fallback.SetInner(Rml::GetFontEngineInterface());
    Rml::SetFontEngineInterface(&impl_->font_fallback);

#ifdef _WIN32
    if (fs::exists(R"(C:\Windows\Fonts\segoeui.ttf)")) {
        Rml::LoadFontFace(R"(C:\Windows\Fonts\segoeui.ttf)", true);
    }
    if (fs::exists(R"(C:\Windows\Fonts\consola.ttf)")) {
        Rml::LoadFontFace(R"(C:\Windows\Fonts\consola.ttf)", true);
    }
    const char* extra_fonts[] = {
        R"(C:\Windows\Fonts\segoeuib.ttf)",
        R"(C:\Windows\Fonts\arial.ttf)",
        R"(C:\Windows\Fonts\arialbd.ttf)",
    };
    for (const char* fp : extra_fonts) {
        if (fs::exists(fp)) {
            Rml::LoadFontFace(fp, false);
        }
    }
#else
    const char* linux_fallback_fonts[] = {
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    for (const char* fp : linux_fallback_fonts) {
        if (fs::exists(fp)) {
            Rml::LoadFontFace(fp, true);
            break;
        }
    }
    const char* linux_extra_fonts[] = {
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/truetype/ubuntu/Ubuntu-B.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    };
    for (const char* fp : linux_extra_fonts) {
        if (fs::exists(fp)) {
            Rml::LoadFontFace(fp, false);
        }
    }
#endif

    impl_->canvas_ctx = Rml::CreateContext("rmlui_studio_canvas", Rml::Vector2i(1920, 1080), impl_->render.get());
    impl_->editor_ctx = Rml::CreateContext("rmlui_studio_editor", Rml::Vector2i(1440, 860), impl_->render.get());
    if (!impl_->canvas_ctx || !impl_->editor_ctx) {
        if (impl_->font_fallback.Inner()) Rml::SetFontEngineInterface(impl_->font_fallback.Inner());
        Rml::Shutdown();
        impl_->render.reset();
        impl_->sys.reset();
        RmlGL3::Shutdown();
        return false;
    }
    if (window) {
        float xscale = 1.0f, yscale = 1.0f;
        glfwGetWindowContentScale(window, &xscale, &yscale);
        impl_->editor_ctx->SetDensityIndependentPixelRatio(xscale > 0.0f ? xscale : 1.0f);
    }
    impl_->initialized = true;
    return true;
}

void RmlEngineHost::Shutdown() {
    if (!impl_) return;
    impl_->ReleaseCanvasRT();
    impl_->InvalidateDomCache();
    if (impl_->initialized) {
        if (impl_->canvas_doc) { impl_->canvas_doc->Close(); impl_->canvas_doc = nullptr; }
        if (impl_->editor_doc) { impl_->editor_doc->Close(); impl_->editor_doc = nullptr; }
        if (impl_->canvas_ctx) { Rml::RemoveContext("rmlui_studio_canvas"); impl_->canvas_ctx = nullptr; }
        if (impl_->editor_ctx) { Rml::RemoveContext("rmlui_studio_editor"); impl_->editor_ctx = nullptr; }
        if (impl_->font_fallback.Inner()) Rml::SetFontEngineInterface(impl_->font_fallback.Inner());
        Rml::Shutdown();
        impl_->render.reset();
        impl_->sys.reset();
        RmlGL3::Shutdown();
        impl_->initialized = false;
    }
}

namespace {

std::string sanitize_greenfield_class_local(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char ch : s) {
        if (std::isalnum(ch) || ch == '-' || ch == '_') {
            out.push_back(static_cast<char>(std::tolower(ch)));
        } else if (ch == ' ' || ch == '.' || ch == '/') {
            out.push_back('-');
        }
    }
    if (out.empty()) out = "elem";
    if (std::isdigit(static_cast<unsigned char>(out[0]))) out = "el-" + out;
    return out;
}

Rml::Element* find_direct_fcid_child_at(Rml::Element* parent, int target_idx) {
    int count = 0;
    const int total = parent ? parent->GetNumChildren() : 0;
    for (int i = 0; i < total; ++i) {
        Rml::Element* ch = parent->GetChild(i);
        if (!ch) continue;
        const std::string& tg = ch->GetTagName();
        if (tg.empty() || tg[0] == '#') continue;
        if (ch->GetAttribute<Rml::String>("data-fcid", "").empty()) continue;
        if (count == target_idx) return ch;
        ++count;
    }
    return nullptr;
}

void set_element_direct_text(Rml::ElementDocument* doc_owner, Rml::Element* host, const std::string& txt, bool prepend_if_missing) {
    if (!doc_owner || !host) return;
    Rml::Element* first_text = nullptr;
    for (int i = 0; i < host->GetNumChildren(); ++i) {
        Rml::Element* ch = host->GetChild(i);
        if (ch && ch->GetTagName() == "#text") {
            first_text = ch;
            break;
        }
    }
    if (txt.empty()) {
        if (first_text) host->RemoveChild(first_text);
    } else if (auto* et = dynamic_cast<Rml::ElementText*>(first_text)) {
        et->SetText(txt);
    } else {
        Rml::ElementPtr tn = doc_owner->CreateTextNode(txt);
        if (prepend_if_missing && host->GetNumChildren() > 0) {
            host->InsertBefore(std::move(tn), host->GetChild(0));
        } else {
            host->AppendChild(std::move(tn));
        }
    }
}

void apply_node_props_to_element(
    Rml::ElementDocument* canvas_doc,
    Rml::Element* el,
    const RmlNode& n,
    const RmlNode* prev,
    const std::string& cls_val,
    bool has_imported_source
) {
    if (!canvas_doc || !el) return;

    if (!n.rml_id.empty()) {
        if (!prev || prev->rml_id != n.rml_id) el->SetAttribute("id", n.rml_id);
    } else if (!prev || !prev->rml_id.empty()) {
        el->RemoveAttribute("id");
    }

    if (!cls_val.empty()) {
        el->SetAttribute("class", cls_val);
    } else {
        el->RemoveAttribute("class");
    }
    el->SetAttribute("data-fcid", std::to_string(n.id));

    if (prev) {
        for (const auto& [old_k, _] : prev->extra_attrs) {
            if (old_k == "id" || old_k == "class" || old_k == "data-fcid") continue;
            bool still_present = false;
            for (const auto& [new_k, _2] : n.extra_attrs) {
                if (new_k == old_k) { still_present = true; break; }
            }
            if (!still_present) el->RemoveAttribute(old_k);
        }
    }
    for (const auto& [ak, av] : n.extra_attrs) {
        if (ak == "id" || ak == "class" || ak == "data-fcid") continue;
        el->SetAttribute(ak, av);
    }

    if (n.tag == "input") {
        bool has_type = false, has_ph = false;
        for (const auto& [ak, _] : n.extra_attrs) {
            if (ak == "type") has_type = true;
            if (ak == "placeholder") has_ph = true;
        }
        if (!has_type) el->SetAttribute("type", "text");
        if (!has_ph) {
            if (!n.placeholder.empty() || !has_imported_source) {
                el->SetAttribute("placeholder", n.placeholder);
            } else {
                el->RemoveAttribute("placeholder");
            }
        }
    } else if (n.tag == "img") {
        bool has_src = false;
        for (const auto& [ak, _] : n.extra_attrs) {
            if (ak == "src") has_src = true;
        }
        if (!has_imported_source) {
            std::string src = n.image_src.empty() ? "image.png" : n.image_src;
            el->SetAttribute("src", src);
        } else if (!has_src && !n.image_src.empty()) {
            el->SetAttribute("src", n.image_src);
        }
    }

    if (!has_imported_source) {
        if (!n.visible) el->SetProperty("display", "none");
        else el->RemoveProperty("display");
    } else {
        el->RemoveProperty("display");
    }

    if (!prev || prev->text != n.text) {
        if (n.tag == "button" && !has_imported_source) {
            Rml::Element* label_span = nullptr;
            for (int i = 0; i < el->GetNumChildren(); ++i) {
                Rml::Element* ch = el->GetChild(i);
                if (ch && ch->GetTagName() == "span" && ch->GetAttribute<Rml::String>("data-fcid", "").empty()) {
                    label_span = ch;
                    break;
                }
            }
            if (!label_span) {
                Rml::ElementPtr sp = canvas_doc->CreateElement("span");
                label_span = el->AppendChild(std::move(sp));
            }
            set_element_direct_text(canvas_doc, label_span, n.text, false);
        } else if (n.tag != "input" && n.tag != "img" && n.tag != "br" && n.tag != "hr") {
            set_element_direct_text(canvas_doc, el, n.text, true);
        }
    }
}

void push_stylesheet_to_doc(Rml::ElementDocument* canvas_doc, const std::string& rcss_str, std::string& cached_rcss) {
    if (!canvas_doc) return;
    if (rcss_str == cached_rcss && canvas_doc->GetStyleSheet() != nullptr) return;
    Rml::SharedPtr<Rml::StyleSheetContainer> sheet = Rml::Factory::InstanceStyleSheetString(rcss_str);
    if (sheet) {
        canvas_doc->SetStyleSheetContainer(sheet);
        cached_rcss = rcss_str;
    }
}

} // namespace

void RmlEngineHost::SyncAndRenderCanvas(const RmlDocument& doc, bool force) {
    if (!impl_->initialized || !impl_->canvas_ctx) return;
    if (!force && doc.revision == last_revision_ && impl_->canvas_tex) return;
    last_revision_ = doc.revision;

    const int w = std::clamp(doc.screen_width, 64, 4096);
    const int h = std::clamp(doc.screen_height, 64, 4096);
    if (impl_->canvas_ctx->GetDimensions().x != w || impl_->canvas_ctx->GetDimensions().y != h) {
        impl_->canvas_ctx->SetDimensions({w, h});
    }

    std::string rml_str, rcss_str;
    std::vector<RmlAsset> assets;
    std::unordered_map<int, std::string> node_classes;
    doc.GenerateRmlAndRcss(rml_str, rcss_str, assets, "style.rcss", true, &node_classes);

    impl_->file.base_dir = doc.base_dir;
    impl_->file.virtual_files["style.rcss"].assign(rcss_str.begin(), rcss_str.end());
    for (const auto& a : assets) {
        if (a.data) impl_->file.virtual_assets[normalize_vpath(a.filename)] = a.data;
    }

    bool need_full_reload = (!impl_->canvas_doc || !impl_->has_synced_doc ||
                             impl_->last_synced_imported != doc.has_imported_source ||
                             impl_->last_synced_root_id != doc.root_id ||
                             impl_->last_synced_body_class != doc.imported_body_class ||
                             impl_->last_synced_body_style != doc.imported_body_style);
    bool rcss_changed = (rcss_str != impl_->last_synced_rcss);

    {
        ScopedCanvasLogCapture log_scope(impl_->sys.get());
        if (need_full_reload) {
            impl_->InvalidateDomCache();
            if (impl_->canvas_doc) {
                impl_->canvas_doc->Close();
                impl_->canvas_doc = nullptr;
            }
            // Carrega RML sem limpar o cache global de RCSS da IDE; aplica stylesheet diretamente no documento
            impl_->canvas_doc = impl_->canvas_ctx->LoadDocumentFromMemory(rml_str);
            if (impl_->canvas_doc) {
                impl_->canvas_doc->Show();
                push_stylesheet_to_doc(impl_->canvas_doc, rcss_str, impl_->last_synced_rcss);
                impl_->RebuildDomNodeMap();
                if (!doc.has_imported_source) {
                    // Garante nos ocultos tambem presentes no mapa DOM incremental
                    std::function<void(int, Rml::Element*)> ensure_hidden = [&](int nid, Rml::Element* parent_el) {
                        const RmlNode* n = doc.FindNode(nid);
                        if (!n || !parent_el) return;
                        Rml::Element* el = impl_->FindDomNode(n->id);
                        if (!el) {
                            std::string cls = node_classes.count(n->id) ? node_classes[n->id]
                                : sanitize_greenfield_class_local(n->rml_class.empty() ? n->name : n->rml_class);
                            Rml::ElementPtr ptr = impl_->canvas_doc->CreateElement(n->tag);
                            el = parent_el->AppendChild(std::move(ptr));
                            apply_node_props_to_element(impl_->canvas_doc, el, *n, nullptr, cls, false);
                            impl_->dom_node_map_[n->id] = el;
                        }
                        for (int cid : n->children) ensure_hidden(cid, el);
                    };
                    ensure_hidden(doc.root_id, impl_->canvas_doc);
                }
            }
        } else {
            impl_->ClearTrackedInlineBoxProps();

            if (impl_->last_synced_title != doc.title) {
                impl_->canvas_doc->SetTitle(doc.title);
            }

            std::unordered_map<int, const RmlNode*> desired;
            desired.reserve(doc.nodes.size());
            for (const auto& n : doc.nodes) {
                desired[n.id] = &n;
            }

            // 1. Destaca elementos existentes que serao preservados se seu pai mudar ou ser removido
            std::unordered_map<int, Rml::ElementPtr> owned_elements;
            std::vector<int> obsolete_ids;
            for (const auto& [nid, el] : impl_->dom_node_map_) {
                auto dit = desired.find(nid);
                if (dit == desired.end() || dit->second->is_body_root || !el || el->GetTagName() != dit->second->tag) {
                    obsolete_ids.push_back(nid);
                }
            }

            // Remove todos os nos gerenciados do pai temporariamente caso precisemos destruir nos obsoletos ou reordenar
            // Apenas nos obsoletos precisam destruir seus subtrees sem levar filhos preservados
            for (int nid : obsolete_ids) {
                Rml::Element* obs_el = impl_->dom_node_map_[nid];
                if (!obs_el) continue;
                // Salva descendentes preservados que ainda existam em desired antes de destruir obs_el
                std::function<void(Rml::Element*)> rescue_kept = [&](Rml::Element* cur) {
                    if (!cur) return;
                    for (int i = cur->GetNumChildren() - 1; i >= 0; --i) {
                        Rml::Element* ch = cur->GetChild(i);
                        if (!ch) continue;
                        rescue_kept(ch);
                        Rml::String fcid = ch->GetAttribute<Rml::String>("data-fcid", "");
                        if (!fcid.empty()) {
                            char* endp = nullptr;
                            long cid = std::strtol(fcid.c_str(), &endp, 10);
                            if (endp != fcid.c_str() && *endp == '\0') {
                                int child_id = static_cast<int>(cid);
                                auto dit = desired.find(child_id);
                                if (dit != desired.end() && !dit->second->is_body_root && ch->GetTagName() == dit->second->tag) {
                                    owned_elements[child_id] = cur->RemoveChild(ch);
                                }
                            }
                        }
                    }
                };
                rescue_kept(obs_el);
                if (Rml::Element* p = obs_el->GetParentNode()) {
                    p->RemoveChild(obs_el);
                }
                impl_->dom_node_map_.erase(nid);
            }

            // 2. Cria novos nos e sincroniza propriedades/atributos/texto dos existentes
            for (const auto& n : doc.nodes) {
                if (n.is_body_root) continue;
                std::string cls_val = node_classes.count(n.id) ? node_classes[n.id] : n.rml_class;
                auto prev_it = impl_->last_synced_nodes.find(n.id);
                const RmlNode* prev_ptr = (prev_it != impl_->last_synced_nodes.end()) ? &prev_it->second : nullptr;

                Rml::Element* el = impl_->FindDomNode(n.id);
                if (!el) {
                    auto oit = owned_elements.find(n.id);
                    if (oit != owned_elements.end() && oit->second) {
                        el = oit->second.get();
                    } else {
                        Rml::ElementPtr created = impl_->canvas_doc->CreateElement(n.tag);
                        el = created.get();
                        owned_elements[n.id] = std::move(created);
                        prev_ptr = nullptr;
                    }
                    impl_->dom_node_map_[n.id] = el;
                }
                apply_node_props_to_element(impl_->canvas_doc, el, n, prev_ptr, cls_val, doc.has_imported_source);
            }

            // 3. Reconcilia hierarquia e ordem dos filhos mantendo zero alocacao se ordem nao mudou
            auto reconcile_children = [&](Rml::Element* parent_el, const std::vector<int>& child_ids) {
                if (!parent_el) return;
                for (size_t idx = 0; idx < child_ids.size(); ++idx) {
                    int cid = child_ids[idx];
                    auto dit = desired.find(cid);
                    if (dit == desired.end() || dit->second->is_body_root) continue;

                    Rml::Element* target_el = impl_->FindDomNode(cid);
                    Rml::Element* at_slot = find_direct_fcid_child_at(parent_el, static_cast<int>(idx));
                    if (at_slot == target_el && owned_elements.find(cid) == owned_elements.end()) {
                        continue;
                    }

                    Rml::ElementPtr ptr;
                    auto oit = owned_elements.find(cid);
                    if (oit != owned_elements.end()) {
                        ptr = std::move(oit->second);
                        owned_elements.erase(oit);
                    } else if (target_el) {
                        if (Rml::Element* old_par = target_el->GetParentNode()) {
                            ptr = old_par->RemoveChild(target_el);
                            // Reavalia slot apos remocao se estava no mesmo pai
                            at_slot = find_direct_fcid_child_at(parent_el, static_cast<int>(idx));
                        }
                    }
                    if (ptr) {
                        Rml::Element* inserted = nullptr;
                        if (at_slot) {
                            inserted = parent_el->InsertBefore(std::move(ptr), at_slot);
                        } else {
                            inserted = parent_el->AppendChild(std::move(ptr));
                        }
                        impl_->dom_node_map_[cid] = inserted;
                    }
                }
            };

            if (doc.has_imported_source) {
                if (const RmlNode* body_root = doc.FindNode(doc.root_id)) {
                    reconcile_children(impl_->canvas_doc, body_root->children);
                }
            } else {
                reconcile_children(impl_->canvas_doc, {doc.root_id});
            }
            for (const auto& n : doc.nodes) {
                if (n.is_body_root) continue;
                if (Rml::Element* par_el = impl_->FindDomNode(n.id)) {
                    reconcile_children(par_el, n.children);
                }
            }

            // 4. Aplica RCSS compilado diretamente no ElementDocument sem tocar no StyleSheetCache global
            push_stylesheet_to_doc(impl_->canvas_doc, rcss_str, impl_->last_synced_rcss);
        }

        impl_->has_synced_doc = (impl_->canvas_doc != nullptr);
        impl_->last_synced_imported = doc.has_imported_source;
        impl_->last_synced_root_id = doc.root_id;
        impl_->last_synced_body_class = doc.imported_body_class;
        impl_->last_synced_body_style = doc.imported_body_style;
        impl_->last_synced_title = doc.title;
        impl_->last_synced_classes = std::move(node_classes);
        impl_->last_synced_nodes.clear();
        impl_->last_synced_nodes.reserve(doc.nodes.size());
        for (const auto& n : doc.nodes) {
            impl_->last_synced_nodes[n.id] = n;
        }

        impl_->canvas_ctx->Update();
        if (need_full_reload || rcss_changed) {
            impl_->last_canvas_logs = impl_->sys->captured_logs;
            ++impl_->canvas_log_seq;
        } else if (!impl_->sys->captured_logs.empty()) {
            for (const auto& le : impl_->sys->captured_logs) {
                if (impl_->last_canvas_logs.size() < 64u) impl_->last_canvas_logs.push_back(le);
            }
            ++impl_->canvas_log_seq;
        }
    }
    impl_->RenderCanvasToTexture(w, h);
}

namespace {
Rml::Element* FindElementByFcid(Rml::Element* root, const std::string& fcid) {
    if (!root) return nullptr;
    if (root->GetAttribute<Rml::String>("data-fcid", "") == fcid) return root;
    const int n = root->GetNumChildren();
    for (int i = 0; i < n; ++i) {
        if (auto* found = FindElementByFcid(root->GetChild(i), fcid)) return found;
    }
    return nullptr;
}
} // namespace

namespace {
Rml::Element* FindAbsContainingBlock(Rml::Element* el, Rml::ElementDocument* doc) {
    if (!el || !doc) return doc;
    Rml::Element* cur = el->GetParentNode();
    int guard = 0;
    while (cur && cur != doc && guard++ < 256) {
        const auto& comp = cur->GetComputedValues();
        if (comp.position() != Rml::Style::Position::Static ||
            comp.has_local_transform() ||
            comp.has_local_perspective() ||
            comp.has_filter() ||
            comp.has_backdrop_filter() ||
            comp.has_mask_image()) {
            return cur;
        }
        cur = cur->GetParentNode();
    }
    return doc;
}

void ReadNodePositionFromElement(Rml::Element* el, Rml::ElementDocument* doc_el, RmlNode& node) {
    if (!el) return;
    const auto& comp = el->GetComputedValues();
    const auto& box = el->GetBox();
    const auto pos = comp.position();
    if (pos == Rml::Style::Position::Static) {
        node.pos_mode = RmlPositionMode::Static;
        node.css_positioned = false;
    } else if (pos == Rml::Style::Position::Relative) {
        node.pos_mode = RmlPositionMode::Relative;
        node.css_positioned = true;
    } else {
        node.pos_mode = RmlPositionMode::Absolute;
        node.css_positioned = true;
    }
    node.margin_left = box.GetEdge(Rml::BoxArea::Margin, Rml::BoxEdge::Left);
    node.margin_top = box.GetEdge(Rml::BoxArea::Margin, Rml::BoxEdge::Top);
    node.has_margin_left = false;
    node.has_margin_top = false;
    float ref_pw = 1920.0f, ref_ph = 1080.0f;
    if (auto* off_par = FindAbsContainingBlock(el, doc_el)) {
        auto psize = off_par->GetBox().GetSize(Rml::BoxArea::Padding);
        if (psize.x > 1.0f) ref_pw = psize.x;
        if (psize.y > 1.0f) ref_ph = psize.y;
    }
    auto resolve_lpa = [](Rml::Style::LengthPercentageAuto lp, float base) -> float {
        if (lp.type == Rml::Style::LengthPercentageAuto::Length) return lp.value;
        if (lp.type == Rml::Style::LengthPercentageAuto::Percentage) return lp.value * base * 0.01f;
        return 0.0f;
    };
    node.rel_x = (node.pos_mode == RmlPositionMode::Relative) ? resolve_lpa(comp.left(), ref_pw) : 0.0f;
    node.rel_y = (node.pos_mode == RmlPositionMode::Relative) ? resolve_lpa(comp.top(), ref_ph) : 0.0f;
}
} // namespace

bool RmlEngineHost::SetCanvasNodeBoxProps(const RmlNode& node, float screen_w, float screen_h) {
    if (!impl_->initialized || !impl_->canvas_ctx || !impl_->canvas_doc) return false;
    Rml::Element* el = impl_->FindDomNode(node.id);
    if (!el) el = FindElementByFcid(impl_->canvas_doc, std::to_string(node.id));
    if (!el) return false;

    float ref_w = screen_w;
    float ref_h = screen_h;
    if (Rml::Element* par = el->GetParentNode(); par && par != impl_->canvas_doc) {
        bool par_flex = (par->GetComputedValues().display() == Rml::Style::Display::Flex ||
                         par->GetComputedValues().display() == Rml::Style::Display::InlineFlex);
        if (node.pos_mode == RmlPositionMode::Absolute && !par_flex &&
            par->GetComputedValues().position() == Rml::Style::Position::Static) {
            par->SetProperty("position", "relative");
            Rml::String pfcid = par->GetAttribute<Rml::String>("data-fcid", "");
            if (!pfcid.empty()) {
                char* endp = nullptr;
                long pid = std::strtol(pfcid.c_str(), &endp, 10);
                if (endp != pfcid.c_str() && *endp == '\0') {
                    impl_->dirty_inline_box_ids_.insert(static_cast<int>(pid));
                }
            }
        }
    }
    if (Rml::Element* off_par = FindAbsContainingBlock(el, impl_->canvas_doc)) {
        if (off_par != impl_->canvas_doc) {
            auto psize = off_par->GetBox().GetSize(Rml::BoxArea::Padding);
            if (psize.x > 1.0f) ref_w = psize.x;
            if (psize.y > 1.0f) ref_h = psize.y;
        }
    }

    impl_->dirty_inline_box_ids_.insert(node.id);
    el->SetProperty("box-sizing", "border-box");
    bool emit_margin_auto = (!node.from_import || node.explicit_anchor_or_margin);

    if (node.pos_mode == RmlPositionMode::Static) {
        el->SetProperty("position", "static");
        if (node.has_margin_left) {
            el->SetProperty("margin-left", FormatRmlLength(node.margin_left, ref_w, node.unit_x, node.em_base_px, node.dp_ratio));
        }
        if (node.has_margin_top) {
            el->SetProperty("margin-top", FormatRmlLength(node.margin_top, ref_h, node.unit_y, node.em_base_px, node.dp_ratio));
        }
        if (!node.has_margin_left && !node.has_margin_top && emit_margin_auto) {
            el->SetProperty("margin", "0px");
        }
        if (emit_margin_auto) {
            el->SetProperty("left", "auto");
            el->SetProperty("right", "auto");
            el->SetProperty("top", "auto");
            el->SetProperty("bottom", "auto");
        }
    } else if (node.pos_mode == RmlPositionMode::Relative) {
        el->SetProperty("position", "relative");
        if (node.has_margin_left) {
            el->SetProperty("margin-left", FormatRmlLength(node.margin_left, ref_w, node.unit_x, node.em_base_px, node.dp_ratio));
        }
        if (node.has_margin_top) {
            el->SetProperty("margin-top", FormatRmlLength(node.margin_top, ref_h, node.unit_y, node.em_base_px, node.dp_ratio));
        }
        if (!node.has_margin_left && !node.has_margin_top && emit_margin_auto) {
            el->SetProperty("margin", "0px");
        }
        el->SetProperty("left", FormatRmlLength(node.rel_x, ref_w, node.unit_x, node.em_base_px, node.dp_ratio));
        el->SetProperty("top", FormatRmlLength(node.rel_y, ref_h, node.unit_y, node.em_base_px, node.dp_ratio));
        if (emit_margin_auto) {
            el->SetProperty("right", "auto");
            el->SetProperty("bottom", "auto");
        }
    } else {
        if (emit_margin_auto) {
            el->SetProperty("margin", "0px");
        }
        el->SetProperty("position", "absolute");
        if (node.size_mode_w == 2) {
            el->SetProperty("left", "0px");
            if (emit_margin_auto) el->SetProperty("right", "auto");
        } else if (node.anchor_h == 1) {
            el->SetProperty("right", FormatRmlLength(ref_w - (node.x + node.width), ref_w, node.unit_x, node.em_base_px, node.dp_ratio));
            if (emit_margin_auto) el->SetProperty("left", "auto");
        } else if (node.anchor_h == 2) {
            el->SetProperty("left", FormatRmlLength(node.x, ref_w, node.unit_x, node.em_base_px, node.dp_ratio));
            el->SetProperty("right", FormatRmlLength(ref_w - (node.x + node.width), ref_w, node.unit_x, node.em_base_px, node.dp_ratio));
        } else {
            el->SetProperty("left", FormatRmlLength(node.x, ref_w, node.unit_x, node.em_base_px, node.dp_ratio));
            if (emit_margin_auto) el->SetProperty("right", "auto");
        }
        if (node.size_mode_h == 2) {
            el->SetProperty("top", "0px");
            if (emit_margin_auto) el->SetProperty("bottom", "auto");
        } else if (node.anchor_v == 1) {
            el->SetProperty("bottom", FormatRmlLength(ref_h - (node.y + node.height), ref_h, node.unit_y, node.em_base_px, node.dp_ratio));
            if (emit_margin_auto) el->SetProperty("top", "auto");
        } else if (node.anchor_v == 2) {
            el->SetProperty("top", FormatRmlLength(node.y, ref_h, node.unit_y, node.em_base_px, node.dp_ratio));
            el->SetProperty("bottom", FormatRmlLength(ref_h - (node.y + node.height), ref_h, node.unit_y, node.em_base_px, node.dp_ratio));
        } else {
            el->SetProperty("top", FormatRmlLength(node.y, ref_h, node.unit_y, node.em_base_px, node.dp_ratio));
            if (emit_margin_auto) el->SetProperty("bottom", "auto");
        }
    }

    if (node.size_mode_w == 1) {
        el->SetProperty("width", "auto");
    } else if (node.size_mode_w == 2) {
        el->SetProperty("width", "100%");
    } else if (node.pos_mode == RmlPositionMode::Absolute && node.anchor_h == 2) {
        el->SetProperty("width", "auto");
    } else {
        el->SetProperty("width", FormatRmlLength(node.width, ref_w, node.unit_w, node.em_base_px, node.dp_ratio));
    }

    if (node.size_mode_h == 1) {
        el->SetProperty("height", "auto");
    } else if (node.size_mode_h == 2) {
        el->SetProperty("height", "100%");
    } else if (node.pos_mode == RmlPositionMode::Absolute && node.anchor_v == 2) {
        el->SetProperty("height", "auto");
    } else {
        el->SetProperty("height", FormatRmlLength(node.height, ref_h, node.unit_h, node.em_base_px, node.dp_ratio));
    }
    return true;
}

void RmlEngineHost::FlushCanvasRender() {
    if (!impl_->initialized || !impl_->canvas_ctx) return;
    impl_->canvas_ctx->Update();
    auto dims = impl_->canvas_ctx->GetDimensions();
    impl_->RenderCanvasToTexture(std::max(64, dims.x), std::max(64, dims.y));
}

bool RmlEngineHost::GetCanvasElementRect(int node_id, float& out_x, float& out_y, float& out_w, float& out_h) const {
    if (!impl_->initialized || !impl_->canvas_doc) return false;
    Rml::Element* el = impl_->FindDomNode(node_id);
    if (!el) el = FindElementByFcid(impl_->canvas_doc, std::to_string(node_id));
    if (!el) return false;
    out_x = el->GetAbsoluteLeft();
    out_y = el->GetAbsoluteTop();
    out_w = el->GetOffsetWidth();
    out_h = el->GetOffsetHeight();
    return out_w > 0.0f || out_h > 0.0f;
}

bool RmlEngineHost::GetCanvasElementOffsetParentOrigin(int node_id, float& out_x, float& out_y) const {
    if (!impl_->initialized || !impl_->canvas_doc) return false;
    Rml::Element* el = impl_->FindDomNode(node_id);
    if (!el) el = FindElementByFcid(impl_->canvas_doc, std::to_string(node_id));
    if (!el) return false;
    Rml::Element* off_par = FindAbsContainingBlock(el, impl_->canvas_doc);
    if (!off_par || off_par == impl_->canvas_doc) {
        out_x = 0.0f;
        out_y = 0.0f;
        return true;
    }
    Rml::Vector2f pad_off = off_par->GetAbsoluteOffset(Rml::BoxArea::Padding);
    out_x = pad_off.x;
    out_y = pad_off.y;
    return true;
}

bool RmlEngineHost::GetCanvasElementPaddingOrigin(int node_id, float& out_x, float& out_y) const {
    if (!impl_->initialized || !impl_->canvas_doc) return false;
    Rml::Element* el = impl_->FindDomNode(node_id);
    if (!el) el = FindElementByFcid(impl_->canvas_doc, std::to_string(node_id));
    if (!el || el == impl_->canvas_doc) {
        out_x = 0.0f;
        out_y = 0.0f;
        return el == impl_->canvas_doc;
    }
    Rml::Vector2f pad_off = el->GetAbsoluteOffset(Rml::BoxArea::Padding);
    out_x = pad_off.x;
    out_y = pad_off.y;
    return true;
}

int RmlEngineHost::PickNodeAtCanvasPoint(float x, float y) const {
    if (!impl_ || !impl_->initialized || !impl_->canvas_doc) return -1;

    struct ClipBox { float x0, y0, x1, y1; bool active; };
    auto intersect_clip = [](ClipBox a, ClipBox b) -> ClipBox {
        if (!a.active) return b;
        if (!b.active) return a;
        ClipBox c;
        c.x0 = std::max(a.x0, b.x0);
        c.y0 = std::max(a.y0, b.y0);
        c.x1 = std::min(a.x1, b.x1);
        c.y1 = std::min(a.y1, b.y1);
        c.active = true;
        return c;
    };

    std::function<int(Rml::Element*, ClipBox)> visit = [&](Rml::Element* el, ClipBox clip) -> int {
        if (!el) return -1;
        const std::string& tag = el->GetTagName();
        if (tag.empty() || tag[0] == '#') return -1;

        const auto& comp = el->GetComputedValues();
        if (comp.display() == Rml::Style::Display::None ||
            comp.visibility() == Rml::Style::Visibility::Hidden) {
            return -1;
        }

        float ax = el->GetAbsoluteLeft();
        float ay = el->GetAbsoluteTop();
        float aw = el->GetOffsetWidth();
        float ah = el->GetOffsetHeight();

        ClipBox child_clip = clip;
        if (el != impl_->canvas_doc) {
            bool clips = (comp.overflow_x() != Rml::Style::Overflow::Visible) ||
                         (comp.overflow_y() != Rml::Style::Overflow::Visible);
            if (clips) {
                child_clip = intersect_clip(clip, {ax, ay, ax + aw, ay + ah, true});
                if (x < child_clip.x0 || x > child_clip.x1 || y < child_clip.y0 || y > child_clip.y1) {
                    return -1;
                }
            }
        }

        const int n = el->GetNumChildren();
        for (int i = n - 1; i >= 0; --i) {
            int ch_hit = visit(el->GetChild(i), child_clip);
            if (ch_hit > 0) return ch_hit;
        }

        if (el == impl_->canvas_doc) return -1;
        if (aw <= 0.0f || ah <= 0.0f) return -1;
        if (clip.active && (x < clip.x0 || x > clip.x1 || y < clip.y0 || y > clip.y1)) return -1;

        if (x >= ax && x <= ax + aw && y >= ay && y <= ay + ah) {
            std::string fcid = el->GetAttribute<Rml::String>("data-fcid", "");
            if (!fcid.empty()) {
                char* endp = nullptr;
                long v = std::strtol(fcid.c_str(), &endp, 10);
                if (endp != fcid.c_str() && v > 0) return static_cast<int>(v);
            }
        }
        return -1;
    };

    return visit(impl_->canvas_doc, {0.0f, 0.0f, 0.0f, 0.0f, false});
}

void RmlEngineHost::RenderCanvasInteractive() {
    if (!impl_->initialized || !impl_->canvas_ctx) return;
    auto dims = impl_->canvas_ctx->GetDimensions();
    const int target_w = std::max(64, dims.x);
    const int target_h = std::max(64, dims.y);
    const bool dims_changed = (target_w != impl_->canvas_w || target_h != impl_->canvas_h || impl_->canvas_tex == 0);
    const double now = glfwGetTime();
    const double next_delay = impl_->canvas_ctx->GetNextUpdateDelay();
    const bool scheduled_due = std::isfinite(next_delay) && ((now - impl_->last_interactive_update_sec) >= next_delay);

    if (!impl_->canvas_interactive_dirty && !dims_changed && !scheduled_due) {
        return;
    }

    impl_->canvas_ctx->Update();
    impl_->RenderCanvasToTexture(target_w, target_h);
}

void RmlEngineHost::MarkCanvasInteractiveDirty() {
    if (impl_) impl_->canvas_interactive_dirty = true;
}

void RmlEngineHost::CanvasMouseMove(int x, int y) {
    if (impl_->initialized && impl_->canvas_ctx) {
        impl_->canvas_ctx->ProcessMouseMove(x, y, 0);
        impl_->canvas_interactive_dirty = true;
    }
}
void RmlEngineHost::CanvasMouseButton(int button, bool down) {
    if (!impl_->initialized || !impl_->canvas_ctx) return;
    if (down) impl_->canvas_ctx->ProcessMouseButtonDown(button, 0);
    else impl_->canvas_ctx->ProcessMouseButtonUp(button, 0);
    impl_->canvas_interactive_dirty = true;
}
void RmlEngineHost::CanvasMouseWheel(float delta) {
    if (impl_->initialized && impl_->canvas_ctx) {
        impl_->canvas_ctx->ProcessMouseWheel(Rml::Vector2f(0.0f, -delta), 0);
        impl_->canvas_interactive_dirty = true;
    }
}
void RmlEngineHost::CanvasTextInput(uint32_t codepoint) {
    if (impl_->initialized && impl_->canvas_ctx) {
        impl_->canvas_ctx->ProcessTextInput(static_cast<Rml::Character>(codepoint));
        impl_->canvas_interactive_dirty = true;
    }
}
unsigned int RmlEngineHost::CanvasTexture() const {
    return impl_ ? static_cast<unsigned int>(impl_->canvas_tex) : 0u;
}
uint64_t RmlEngineHost::CanvasTextureVersion() const {
    return impl_ ? impl_->canvas_tex_gen : 0ull;
}

Rml::ElementDocument* RmlEngineHost::LoadEditorDocument(const std::string& rml, const std::string& rcss) {
    if (!impl_->initialized || !impl_->editor_ctx) return nullptr;
    impl_->file.virtual_files["studio_ui.rcss"].assign(rcss.begin(), rcss.end());
    if (impl_->editor_doc) {
        impl_->editor_doc->Close();
        impl_->editor_doc = nullptr;
    }
    impl_->editor_doc = impl_->editor_ctx->LoadDocumentFromMemory(rml);
    if (impl_->editor_doc) {
        impl_->editor_doc->Show();
    }
    return impl_->editor_doc;
}

void RmlEngineHost::ResizeEditor(int w, int h) {
    if (!impl_->initialized || !impl_->editor_ctx) return;
    w = std::max(64, w);
    h = std::max(64, h);
    impl_->editor_w = w;
    impl_->editor_h = h;
    impl_->editor_ctx->SetDimensions({w, h});
}

void RmlEngineHost::SetEditorDpiScale(float scale) {
    if (!impl_ || !impl_->initialized || !impl_->editor_ctx) return;
    if (scale > 0.0f) {
        impl_->editor_ctx->SetDensityIndependentPixelRatio(scale);
    }
}

void RmlEngineHost::RenderEditor() {
    if (!impl_->initialized || !impl_->editor_ctx || !impl_->render) return;
    impl_->editor_ctx->Update();
    impl_->render->SetViewport(impl_->editor_w, impl_->editor_h);
    impl_->render->BeginFrame();
    impl_->editor_ctx->Render();
    impl_->render->EndFrame();
    glBindVertexArray(0);
    glUseProgram(0);
}

Rml::Context* RmlEngineHost::EditorContext() {
    return impl_ ? impl_->editor_ctx : nullptr;
}

Rml::Context* RmlEngineHost::CanvasContext() {
    return impl_ ? impl_->canvas_ctx : nullptr;
}

Rml::ElementDocument* RmlEngineHost::CanvasDocument() const {
    return impl_ ? impl_->canvas_doc : nullptr;
}

const std::vector<RmlEngineLogEntry>& RmlEngineHost::LastCanvasLogs() const {
    static const std::vector<RmlEngineLogEntry> kEmptyLogs;
    return impl_ ? impl_->last_canvas_logs : kEmptyLogs;
}

uint64_t RmlEngineHost::LastCanvasLogSeq() const {
    return impl_ ? impl_->canvas_log_seq : 0;
}

bool RmlEngineHost::HasCanvasAsset(const std::string& path) const {
    if (path.empty()) return false;
    if (path.find("studio_ico_") != std::string::npos ||
        path == "studio_cp_hue" ||
        path == "studio_cp_sv_white" ||
        path == "studio_cp_sv_black") {
        return true;
    }
    if (!impl_) return false;
    std::string norm = normalize_vpath(path);
    std::string base = norm;
    auto slash = norm.find_last_of('/');
    if (slash != std::string::npos) base = norm.substr(slash + 1);
    if (impl_->file.virtual_assets.count(norm) || impl_->file.virtual_assets.count(base)) return true;
    if (impl_->file.virtual_files.count(norm) || impl_->file.virtual_files.count(base)) return true;
    return false;
}

static void SanitizeInlineStylesInRml(std::string& rml_io, std::string* appended_raw_rcss) {
    std::string low_work = rml_io;
    std::transform(low_work.begin(), low_work.end(), low_work.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    size_t spos = 0;
    while ((spos = low_work.find("<style", spos)) != std::string::npos) {
        size_t gt = low_work.find('>', spos);
        size_t end_st = low_work.find("</style>", gt == std::string::npos ? spos : gt);
        if (gt == std::string::npos || end_st == std::string::npos) break;
        std::string inline_raw = rml_io.substr(gt + 1, end_st - (gt + 1));
        std::string inline_clean = SanitizeRcssForRmlUi(inline_raw);
        if (appended_raw_rcss) {
            std::string raw_no_bom = StripUtf8Bom(inline_raw);
            if (!appended_raw_rcss->empty() && appended_raw_rcss->back() != '\n') *appended_raw_rcss += "\n";
            *appended_raw_rcss += raw_no_bom;
        }
        rml_io.replace(gt + 1, end_st - (gt + 1), inline_clean);
        low_work = rml_io;
        std::transform(low_work.begin(), low_work.end(), low_work.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        spos = gt + 1 + inline_clean.size() + 8;
    }
}

static void PopulateDocFromElementDocument(
    Rml::ElementDocument* dom,
    RmlDocument& out_doc,
    StudioFileInterface& file_iface,
    const std::string& default_title,
    const std::string& asset_dir,
    const std::vector<RmlNode>* fallback_nodes
) {
    out_doc.nodes.clear();
    out_doc.next_id = 1;
    out_doc.has_imported_source = true;
    out_doc.title = dom->GetTitle().empty()
        ? default_title
        : std::string(dom->GetTitle().begin(), dom->GetTitle().end());

    if (auto* bcls = dom->GetAttribute("class")) out_doc.imported_body_class = bcls->Get<Rml::String>();
    else out_doc.imported_body_class.clear();
    if (auto* bsty = dom->GetAttribute("style")) out_doc.imported_body_style = bsty->Get<Rml::String>();
    else out_doc.imported_body_style.clear();

    RmlNode root;
    root.id = out_doc.next_id++;
    root.parent_id = -1;
    root.is_body_root = true;
    root.from_import = true;
    root.name = "body";
    root.tag = "body";
    root.rml_class = out_doc.imported_body_class;
    root.x = 0.0f;
    root.y = 0.0f;
    root.width = static_cast<float>(out_doc.screen_width);
    root.height = static_cast<float>(out_doc.screen_height);
    root.has_bg = false;
    out_doc.root_id = root.id;
    out_doc.nodes.push_back(root);

    auto trim_ws = [](const std::string& s) -> std::string {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return {};
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    };

    std::function<void(Rml::Element*, int, float, float)> walk = [&](Rml::Element* el, int parent_id, float pwx, float pwy) {
        if (!el) return;
        std::string tag = el->GetTagName();
        if (tag.empty() || tag[0] == '#') return;

        float abs_x = el->GetAbsoluteLeft();
        float abs_y = el->GetAbsoluteTop();
        float el_w = el->GetOffsetWidth();
        float el_h = el->GetOffsetHeight();

        RmlNode node;
        node.id = out_doc.next_id++;
        node.parent_id = parent_id;
        node.from_import = true;
        node.tag = tag;
        node.rml_id = el->GetId();
        if (auto* cls_attr = el->GetAttribute("class")) {
            std::istringstream css_in(cls_attr->Get<Rml::String>());
            std::string tok;
            std::string cleaned;
            while (css_in >> tok) {
                if (tok.size() > 4 && tok.rfind("fc-n", 0) == 0 &&
                    std::all_of(tok.begin() + 4, tok.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
                    continue;
                }
                if (!cleaned.empty()) cleaned += ' ';
                cleaned += tok;
            }
            node.rml_class = cleaned;
        }
        for (const auto& [ak, av] : el->GetAttributes()) {
            if (ak == "id" || ak == "class" || ak == "data-fcid") continue;
            node.extra_attrs.push_back({ak, av.Get<Rml::String>()});
        }

        node.x = abs_x - pwx;
        node.y = abs_y - pwy;
        node.width = std::max(8.0f, el_w);
        node.height = std::max(8.0f, el_h);

        const auto& comp = el->GetComputedValues();
        const auto& box = el->GetBox();
        ReadNodePositionFromElement(el, dom, node);
        const auto disp = comp.display();
        node.display_flex = (disp == Rml::Style::Display::Flex || disp == Rml::Style::Display::InlineFlex);
        auto fd = comp.flex_direction();
        node.flex_direction = (fd == Rml::Style::FlexDirection::Column || fd == Rml::Style::FlexDirection::ColumnReverse) ? 1 : 0;
        auto fw = comp.flex_wrap();
        node.flex_wrap = (fw == Rml::Style::FlexWrap::Wrap || fw == Rml::Style::FlexWrap::WrapReverse);
        auto jc = comp.justify_content();
        if (jc == Rml::Style::JustifyContent::Center) node.justify_content = 1;
        else if (jc == Rml::Style::JustifyContent::FlexEnd) node.justify_content = 2;
        else if (jc == Rml::Style::JustifyContent::SpaceBetween || jc == Rml::Style::JustifyContent::SpaceAround || jc == Rml::Style::JustifyContent::SpaceEvenly) node.justify_content = 3;
        else node.justify_content = 0;
        auto ai = comp.align_items();
        if (ai == Rml::Style::AlignItems::Center) node.align_items = 1;
        else if (ai == Rml::Style::AlignItems::FlexEnd) node.align_items = 2;
        else node.align_items = 0;
        float rg = (comp.row_gap().type == Rml::Style::LengthPercentage::Length) ? comp.row_gap().value : 0.0f;
        float cg = (comp.column_gap().type == Rml::Style::LengthPercentage::Length) ? comp.column_gap().value : 0.0f;
        node.gap = std::max(0.0f, (node.flex_direction == 1) ? (rg > 0.0f ? rg : cg) : (cg > 0.0f ? cg : rg));
        node.padding[0] = std::max(0.0f, box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Top));
        node.padding[1] = std::max(0.0f, box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Right));
        node.padding[2] = std::max(0.0f, box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Bottom));
        node.padding[3] = std::max(0.0f, box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Left));
        node.em_base_px = std::max(8.0f, comp.font_size());
        if (auto* ctx = el->GetContext()) {
            node.dp_ratio = std::max(0.01f, ctx->GetDensityIndependentPixelRatio());
        }
        auto map_unit = [](const Rml::Property* prop) -> RmlCssUnit {
            if (!prop) return RmlCssUnit::Px;
            switch (prop->unit) {
                case Rml::Unit::PERCENT: return RmlCssUnit::Percent;
                case Rml::Unit::DP:      return RmlCssUnit::Dp;
                case Rml::Unit::EM:
                case Rml::Unit::REM:     return RmlCssUnit::Em;
                default:                 return RmlCssUnit::Px;
            }
        };
        auto is_len_prop = [](const Rml::Property* prop) -> bool {
            if (!prop) return false;
            return (prop->unit == Rml::Unit::PX || prop->unit == Rml::Unit::PERCENT ||
                    prop->unit == Rml::Unit::DP || prop->unit == Rml::Unit::EM ||
                    prop->unit == Rml::Unit::REM || prop->unit == Rml::Unit::NUMBER);
        };
        const Rml::Property* p_left = el->GetLocalProperty("left");
        const Rml::Property* p_right = el->GetLocalProperty("right");
        const Rml::Property* p_top = el->GetLocalProperty("top");
        const Rml::Property* p_bottom = el->GetLocalProperty("bottom");
        const Rml::Property* p_width = el->GetLocalProperty("width");
        const Rml::Property* p_height = el->GetLocalProperty("height");
        if (is_len_prop(p_left) && is_len_prop(p_right)) node.anchor_h = 2;
        else if (is_len_prop(p_right) && !is_len_prop(p_left)) node.anchor_h = 1;
        if (is_len_prop(p_top) && is_len_prop(p_bottom)) node.anchor_v = 2;
        else if (is_len_prop(p_bottom) && !is_len_prop(p_top)) node.anchor_v = 1;
        node.unit_x = is_len_prop(p_left) ? map_unit(p_left) : (is_len_prop(p_right) ? map_unit(p_right) : RmlCssUnit::Px);
        node.unit_y = is_len_prop(p_top) ? map_unit(p_top) : (is_len_prop(p_bottom) ? map_unit(p_bottom) : RmlCssUnit::Px);
        node.unit_w = is_len_prop(p_width) ? map_unit(p_width) : RmlCssUnit::Px;
        node.unit_h = is_len_prop(p_height) ? map_unit(p_height) : RmlCssUnit::Px;
        bool is_inline_tag = (tag == "span" || tag == "a" || tag == "b" || tag == "i" ||
                              tag == "strong" || tag == "em" || tag == "label" || tag == "small");
        if (is_inline_tag) {
            if (comp.width().type == Rml::Style::LengthPercentageAuto::Auto) node.size_mode_w = 1;
            if (comp.height().type == Rml::Style::LengthPercentageAuto::Auto) node.size_mode_h = 1;
        }
        Rml::Colourb bg = comp.background_color();
        node.has_bg = bg.alpha > 0;
        node.bg_color = {bg.red, bg.green, bg.blue, bg.alpha};

        Rml::Colourb fg = comp.color();
        node.text_color = {fg.red, fg.green, fg.blue, fg.alpha};
        node.font_size = std::max(8.0f, comp.font_size());
        node.opacity = comp.opacity();
        node.border_radius = el->GetProperty<float>("border-top-left-radius");
        node.border_width = el->GetBox().GetEdge(Rml::BoxArea::Border, Rml::BoxEdge::Top);
        Rml::Colourb bc = el->GetProperty<Rml::Colourb>("border-top-color");
        node.border_color = {bc.red, bc.green, bc.blue, bc.alpha};

        if (tag == "input") {
            if (auto* ph = el->GetAttribute("placeholder")) {
                node.placeholder = ph->Get<Rml::String>();
            }
        } else if (tag == "img") {
            if (auto* src = el->GetAttribute("src")) {
                node.image_src = src->Get<Rml::String>();
                node.image_data = out_doc.FindCachedAsset(node.image_src);
                if (!node.image_data && !asset_dir.empty()) {
                    fs::path imgp = fs::u8path(asset_dir) / fs::u8path(node.image_src);
                    std::ifstream ifs(imgp, std::ios::binary | std::ios::ate);
                    if (ifs) {
                        auto sz = ifs.tellg();
                        ifs.seekg(0, std::ios::beg);
                        std::vector<uint8_t> buf(static_cast<size_t>(sz));
                        if (sz > 0) ifs.read(reinterpret_cast<char*>(buf.data()), sz);
                        node.image_data = out_doc.CacheAssetBytes(node.image_src, std::move(buf));
                    }
                }
                if (!node.image_data) {
                    auto ait = file_iface.virtual_assets.find(normalize_vpath(node.image_src));
                    if (ait != file_iface.virtual_assets.end() && ait->second) {
                        node.image_data = ait->second;
                        out_doc.asset_cache[node.image_src] = ait->second;
                    } else {
                        auto vit = file_iface.virtual_files.find(normalize_vpath(node.image_src));
                        if (vit != file_iface.virtual_files.end()) {
                            node.image_data = out_doc.CacheAssetBytes(node.image_src, vit->second);
                        } else if (fallback_nodes) {
                            for (const auto& fn : *fallback_nodes) {
                                if (fn.image_src == node.image_src && fn.image_data) {
                                    node.image_data = fn.image_data;
                                    out_doc.asset_cache[node.image_src] = fn.image_data;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }

        for (int i = 0; i < el->GetNumChildren(); ++i) {
            Rml::Element* ch = el->GetChild(i);
            if (ch && ch->GetTagName() == "#text") {
                if (auto* txt = dynamic_cast<Rml::ElementText*>(ch)) {
                    std::string s = trim_ws(txt->GetText());
                    if (!s.empty()) {
                        if (!node.text.empty()) node.text += " ";
                        node.text += s;
                    }
                }
            }
        }

        node.name = node.DefaultDisplayName(22);

        int nid = node.id;
        out_doc.nodes.push_back(node);
        if (auto* pnode = out_doc.FindNode(parent_id)) {
            pnode->children.push_back(nid);
        }

        if (tag != "input" && tag != "img" && tag != "br" && tag != "hr") {
            for (int i = 0; i < el->GetNumChildren(); ++i) {
                walk(el->GetChild(i), nid, abs_x, abs_y);
            }
        }
    };

    for (int i = 0; i < dom->GetNumChildren(); ++i) {
        walk(dom->GetChild(i), out_doc.root_id, 0.0f, 0.0f);
    }
}

bool RmlEngineHost::ImportRmlFile(const std::string& rml_path, RmlDocument& out_doc, std::string& out_error) {
    if (!impl_->initialized || !impl_->canvas_ctx) {
        out_error = "RmlUi engine nao inicializado.";
        return false;
    }
    try {
    fs::path p = fs::u8path(rml_path);
    std::string dir = p.parent_path().u8string();
    impl_->file.base_dir = dir;

    auto read_text_file = [](const fs::path& fp) -> std::string {
        std::ifstream ifs(fp, std::ios::binary);
        if (!ifs) return {};
        return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    };

    std::string raw_rml = read_text_file(p);
    if (raw_rml.empty()) {
        out_error = "Falha ao ler arquivo RML: " + rml_path;
        return false;
    }

    // Limpar cache RCSS virtual anterior antes de carregar novo documento
    impl_->file.virtual_files.erase("style.rcss");

    // Extrair, sanitizar e registrar 100% dos .rcss referenciados no <head> + <style> inline
    std::string combined_rcss;
    std::string load_rml = raw_rml;
    {
        std::string lower_rml = raw_rml;
        std::transform(lower_rml.begin(), lower_rml.end(), lower_rml.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        bool found_link = false;
        size_t pos = 0;
        while ((pos = lower_rml.find("<link", pos)) != std::string::npos) {
            size_t end_tag = lower_rml.find('>', pos);
            if (end_tag == std::string::npos) break;
            std::string tag_orig = raw_rml.substr(pos, end_tag - pos + 1);
            std::string tag_low = lower_rml.substr(pos, end_tag - pos + 1);
            if (tag_low.find("rcss") != std::string::npos || tag_low.find(".css") != std::string::npos) {
                size_t hp = tag_low.find("href=");
                if (hp != std::string::npos) {
                    size_t q1 = tag_orig.find_first_of("\"'", hp + 5);
                    if (q1 != std::string::npos) {
                        char qc = tag_orig[q1];
                        size_t q2 = tag_orig.find(qc, q1 + 1);
                        if (q2 != std::string::npos) {
                            std::string href = tag_orig.substr(q1 + 1, q2 - q1 - 1);
                            fs::path rcss_fp = p.parent_path() / fs::u8path(href);
                            std::string rcss_txt = read_text_file(rcss_fp);
                            if (!rcss_txt.empty()) {
                                found_link = true;
                                std::string sanitized_link = SanitizeRcssForRmlUi(rcss_txt);
                                std::vector<uint8_t> bytes(sanitized_link.begin(), sanitized_link.end());
                                impl_->file.virtual_files[normalize_vpath(href)] = bytes;
                                std::string base_name = fs::u8path(href).filename().u8string();
                                if (!base_name.empty()) {
                                    impl_->file.virtual_files[normalize_vpath(base_name)] = bytes;
                                }
                                if (!combined_rcss.empty() && combined_rcss.back() != '\n') combined_rcss += "\n";
                                combined_rcss += StripUtf8Bom(rcss_txt);
                            }
                        }
                    }
                }
            }
            pos = end_tag + 1;
        }
        // Fallback: se nao achou via <link>, tenta <stem>.rcss na mesma pasta
        if (!found_link) {
            fs::path fallback_rcss = p.parent_path() / (p.stem().u8string() + ".rcss");
            if (fs::exists(fallback_rcss)) {
                std::string raw_fb = read_text_file(fallback_rcss);
                if (!raw_fb.empty()) {
                    combined_rcss = StripUtf8Bom(raw_fb);
                    std::string clean_fb = SanitizeRcssForRmlUi(combined_rcss);
                    impl_->file.virtual_files["style.rcss"].assign(clean_fb.begin(), clean_fb.end());
                    size_t head_end = lower_rml.find("</head>");
                    if (head_end != std::string::npos) {
                        load_rml.insert(head_end, "\n<link type=\"text/rcss\" href=\"style.rcss\"/>\n");
                    }
                }
            }
        }
    }
    SanitizeInlineStylesInRml(load_rml, &combined_rcss);

    const int cw = std::clamp(out_doc.screen_width, 64, 4096);
    const int ch = std::clamp(out_doc.screen_height, 64, 4096);
    if (impl_->canvas_ctx->GetDimensions().x != cw || impl_->canvas_ctx->GetDimensions().y != ch) {
        impl_->canvas_ctx->SetDimensions({cw, ch});
    }

    Rml::Factory::ClearStyleSheetCache();
    Rml::ElementDocument* imported = nullptr;
    {
        ScopedCanvasLogCapture log_scope(impl_->sys.get());
        imported = impl_->canvas_ctx->LoadDocumentFromMemory(load_rml);
        if (imported) {
            imported->Show();
            impl_->canvas_ctx->Update();
        }
        impl_->last_canvas_logs = impl_->sys->captured_logs;
        ++impl_->canvas_log_seq;
    }
    if (!imported) {
        out_error = "Falha ao carregar o arquivo RML: " + rml_path;
        return false;
    }

    out_doc.base_dir = dir;
    out_doc.imported_raw_rml = raw_rml;
    out_doc.imported_raw_rcss = combined_rcss;
    PopulateDocFromElementDocument(imported, out_doc, impl_->file, p.stem().u8string(), dir, nullptr);

    imported->Close();
    impl_->InvalidateDomCache();
    out_doc.Touch();
    last_revision_ = 0;
    return true;
    } catch (const std::exception& e) {
        out_error = std::string("Falha ao importar .rml: ") + e.what();
        return false;
    } catch (...) {
        out_error = "Erro desconhecido ao importar .rml.";
        return false;
    }
}

bool RmlEngineHost::ApplyRmlStringAndSync(const std::string& raw_rml, RmlDocument& doc, std::string& out_error) {
    if (!impl_ || !impl_->initialized || !impl_->canvas_ctx) {
        out_error = "RmlUi engine nao inicializado.";
        return false;
    }
    try {
        std::string low = raw_rml;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (low.find("<rml") == std::string::npos || low.find("<body") == std::string::npos ||
            low.find("</body>") == std::string::npos || low.find("</rml>") == std::string::npos) {
            out_error = "Aguardando estrutura <rml>/<body> completa...";
            impl_->last_canvas_logs = {{true, out_error}};
            ++impl_->canvas_log_seq;
            return false;
        }
        // Verificacao rapida de colchetes '<' / '>' nao balanceados fora de aspas enquanto digita
        bool in_sq = false, in_dq = false, in_tag = false;
        for (char c : raw_rml) {
            if (!in_tag) {
                if (c == '<') { in_tag = true; in_sq = false; in_dq = false; }
            } else {
                if (c == '\'' && !in_dq) in_sq = !in_sq;
                else if (c == '"' && !in_sq) in_dq = !in_dq;
                else if (c == '>' && !in_sq && !in_dq) in_tag = false;
            }
        }
        if (in_tag || in_sq || in_dq) {
            out_error = "Tag RML em aberto...";
            impl_->last_canvas_logs = {{true, out_error}};
            ++impl_->canvas_log_seq;
            return false;
        }

        // Se ainda era documento do zero, preserva o .rcss gerado atual intacto antes de virar imported_source
        if (!doc.has_imported_source) {
            std::string init_rml, init_rcss;
            std::vector<RmlAsset> init_assets;
            doc.GenerateRmlAndRcss(init_rml, init_rcss, init_assets, "style.rcss", false);
            doc.imported_raw_rcss = StripUtf8Bom(init_rcss);
            std::string clean_css = SanitizeRcssForRmlUi(doc.imported_raw_rcss);
            impl_->file.virtual_files["style.rcss"].assign(clean_css.begin(), clean_css.end());
            for (const auto& a : init_assets) {
                if (a.data) impl_->file.virtual_assets[normalize_vpath(a.filename)] = a.data;
            }
            doc.has_imported_source = true;
        } else if (!doc.imported_raw_rcss.empty()) {
            std::string clean_css = SanitizeRcssForRmlUi(doc.imported_raw_rcss);
            impl_->file.virtual_files["style.rcss"].assign(clean_css.begin(), clean_css.end());
        }

        std::string load_rml = raw_rml;
        SanitizeInlineStylesInRml(load_rml, nullptr);

        const int cw = std::clamp(doc.screen_width, 64, 4096);
        const int ch = std::clamp(doc.screen_height, 64, 4096);
        if (impl_->canvas_ctx->GetDimensions().x != cw || impl_->canvas_ctx->GetDimensions().y != ch) {
            impl_->canvas_ctx->SetDimensions({cw, ch});
        }

        Rml::Factory::ClearStyleSheetCache();
        int prev_xml_errs = impl_->sys->xml_error_count;
        Rml::ElementDocument* parsed = nullptr;
        {
            ScopedCanvasLogCapture log_scope(impl_->sys.get());
            parsed = impl_->canvas_ctx->LoadDocumentFromMemory(load_rml);
            if (!parsed || impl_->sys->xml_error_count > prev_xml_errs) {
                impl_->last_canvas_logs = impl_->sys->captured_logs;
                if (impl_->last_canvas_logs.empty()) {
                    impl_->last_canvas_logs.push_back({true, "Sintaxe RML incompleta ou invalida."});
                }
                ++impl_->canvas_log_seq;
                if (parsed) parsed->Close();
                out_error = "Sintaxe RML incompleta ou invalida.";
                return false;
            }
            parsed->Show();
            impl_->canvas_ctx->Update();
            impl_->last_canvas_logs = impl_->sys->captured_logs;
            ++impl_->canvas_log_seq;
        }

        std::vector<RmlNode> prev_nodes = doc.nodes;
        doc.imported_raw_rml = raw_rml;
        std::string fallback_title = doc.title.empty() ? "Documento" : doc.title;
        PopulateDocFromElementDocument(parsed, doc, impl_->file, fallback_title, doc.base_dir, &prev_nodes);

        parsed->Close();
        impl_->InvalidateDomCache();
        doc.Touch();
        last_revision_ = 0;
        return true;
    } catch (const std::exception& e) {
        out_error = std::string("Erro ao aplicar RML: ") + e.what();
        return false;
    } catch (...) {
        out_error = "Erro desconhecido ao aplicar RML.";
        return false;
    }
}

static void ReloadCanvasDocAndSyncNodes(
    Rml::Context* canvas_ctx,
    StudioFileInterface& file_iface,
    Rml::ElementDocument*& canvas_doc,
    RmlDocument& doc,
    const std::string& extra_rcss_vname
) {
    const int cw = std::clamp(doc.screen_width, 64, 4096);
    const int ch = std::clamp(doc.screen_height, 64, 4096);
    if (canvas_ctx->GetDimensions().x != cw || canvas_ctx->GetDimensions().y != ch) {
        canvas_ctx->SetDimensions({cw, ch});
    }

    std::string rml_str, rcss_str;
    std::vector<RmlAsset> assets;
    doc.GenerateRmlAndRcss(rml_str, rcss_str, assets, "style.rcss", true);

    file_iface.virtual_files["style.rcss"].assign(rcss_str.begin(), rcss_str.end());
    if (!extra_rcss_vname.empty()) {
        file_iface.virtual_files[normalize_vpath(extra_rcss_vname)].assign(rcss_str.begin(), rcss_str.end());
    }
    for (const auto& a : assets) {
        if (a.data) file_iface.virtual_assets[normalize_vpath(a.filename)] = a.data;
    }

    if (canvas_doc) {
        canvas_doc->Close();
        canvas_doc = nullptr;
    }
    canvas_doc = canvas_ctx->LoadDocumentFromMemory(rml_str);
    if (canvas_doc) {
        if (auto sheet = Rml::Factory::InstanceStyleSheetString(rcss_str)) {
            canvas_doc->SetStyleSheetContainer(sheet);
        }
        canvas_doc->Show();
        canvas_ctx->Update();

        std::function<void(Rml::Element*, float, float)> sync_el = [&](Rml::Element* el, float pwx, float pwy) {
            if (!el) return;
            std::string tag = el->GetTagName();
            if (tag.empty() || tag[0] == '#') return;
            float ax = el->GetAbsoluteLeft();
            float ay = el->GetAbsoluteTop();
            std::string fcid = el->GetAttribute<Rml::String>("data-fcid", "");
            if (!fcid.empty()) {
                char* endp = nullptr;
                long nid = std::strtol(fcid.c_str(), &endp, 10);
                if (endp != fcid.c_str()) {
                    if (auto* node = doc.FindNode(static_cast<int>(nid))) {
                        node->x = ax - pwx;
                        node->y = ay - pwy;
                        node->width = std::max(8.0f, el->GetOffsetWidth());
                        node->height = std::max(8.0f, el->GetOffsetHeight());
                        const auto& comp = el->GetComputedValues();
                        ReadNodePositionFromElement(el, canvas_doc, *node);
                        Rml::Colourb bg = comp.background_color();
                        node->has_bg = bg.alpha > 0;
                        node->bg_color = {bg.red, bg.green, bg.blue, bg.alpha};
                        Rml::Colourb fg = comp.color();
                        node->text_color = {fg.red, fg.green, fg.blue, fg.alpha};
                        node->font_size = std::max(8.0f, comp.font_size());
                        node->opacity = comp.opacity();
                        node->border_radius = el->GetProperty<float>("border-top-left-radius");
                        node->border_width = el->GetBox().GetEdge(Rml::BoxArea::Border, Rml::BoxEdge::Top);
                        Rml::Colourb bc = el->GetProperty<Rml::Colourb>("border-top-color");
                        node->border_color = {bc.red, bc.green, bc.blue, bc.alpha};
                    }
                }
            }
            for (int i = 0; i < el->GetNumChildren(); ++i) {
                sync_el(el->GetChild(i), ax, ay);
            }
        };
        for (int i = 0; i < canvas_doc->GetNumChildren(); ++i) {
            sync_el(canvas_doc->GetChild(i), 0.0f, 0.0f);
        }
    }
}

bool RmlEngineHost::ImportRcssAndSync(const std::string& rcss_path, RmlDocument& doc, std::string& out_error) {
    if (!impl_ || !impl_->initialized || !impl_->canvas_ctx) {
        out_error = "RmlUi engine nao inicializado.";
        return false;
    }
    try {
        if (!doc.ImportRcssFile(rcss_path, out_error)) {
            return false;
        }
        std::string fname = fs::u8path(rcss_path).filename().u8string();
        {
            ScopedCanvasLogCapture log_scope(impl_->sys.get());
            ReloadCanvasDocAndSyncNodes(impl_->canvas_ctx, impl_->file, impl_->canvas_doc, doc, fname);
            impl_->last_canvas_logs = impl_->sys->captured_logs;
            ++impl_->canvas_log_seq;
        }
        impl_->InvalidateDomCache();
        doc.Touch();
        last_revision_ = 0;
        return true;
    } catch (const std::exception& e) {
        out_error = std::string("Falha ao importar .rcss: ") + e.what();
        return false;
    } catch (...) {
        out_error = "Erro desconhecido ao importar .rcss.";
        return false;
    }
}

bool RmlEngineHost::ApplyRcssStringAndSync(const std::string& raw_rcss, RmlDocument& doc, std::string& out_error) {
    if (!impl_ || !impl_->initialized || !impl_->canvas_ctx) {
        out_error = "RmlUi engine nao inicializado.";
        return false;
    }
    try {
        if (!doc.has_imported_source) {
            std::string dummy_rml, dummy_rcss;
            std::vector<RmlAsset> init_assets;
            doc.GenerateRmlAndRcss(dummy_rml, dummy_rcss, init_assets, "style.rcss", false);
            doc.imported_raw_rml = dummy_rml;
            doc.has_imported_source = true;
            for (auto& n : doc.nodes) {
                n.from_import = true;
            }
            for (const auto& a : init_assets) {
                if (a.data) impl_->file.virtual_assets[normalize_vpath(a.filename)] = a.data;
            }
        }
        doc.imported_raw_rcss = StripUtf8Bom(raw_rcss);
        for (auto& n : doc.nodes) {
            n.modified_transform = false;
            n.modified_style = false;
        }
        {
            ScopedCanvasLogCapture log_scope(impl_->sys.get());
            ReloadCanvasDocAndSyncNodes(impl_->canvas_ctx, impl_->file, impl_->canvas_doc, doc, "");
            impl_->last_canvas_logs = impl_->sys->captured_logs;
            ++impl_->canvas_log_seq;
        }
        impl_->InvalidateDomCache();
        doc.Touch();
        last_revision_ = 0;
        return true;
    } catch (const std::exception& e) {
        out_error = std::string("Erro ao aplicar .rcss: ") + e.what();
        return false;
    } catch (...) {
        out_error = "Erro desconhecido ao aplicar .rcss.";
        return false;
    }
}
