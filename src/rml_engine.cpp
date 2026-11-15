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

#include "../third_party/RmlUi/Backends/RmlUi_Platform_GLFW.h"
#include "../third_party/RmlUi/Backends/RmlUi_Renderer_GL3.h"

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
    const std::vector<uint8_t>* data = nullptr;
    size_t pos = 0;
};

class StudioSystemInterface final : public SystemInterface_GLFW {
public:
    explicit StudioSystemInterface(GLFWwindow* window) : SystemInterface_GLFW(window) {}
    int xml_error_count = 0;

    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        if (type == Rml::Log::LT_ERROR) {
            if (message.find("XML") != Rml::String::npos || message.find("Failed to load") != Rml::String::npos) {
                ++xml_error_count;
            }
        }
        if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_WARNING || type == Rml::Log::LT_ASSERT) {
            std::fprintf(stderr, "[Rml:%d] %s\n", static_cast<int>(type), message.c_str());
        }
        return true;
    }
};

class StudioFileInterface final : public Rml::FileInterface {
public:
    std::unordered_map<std::string, std::vector<uint8_t>> virtual_files;
    std::string base_dir;

    Rml::FileHandle Open(const Rml::String& path) override {
        std::string norm = normalize_vpath(path);
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
                if (cand == "monospace") {
                    const char* mono_fallbacks[] = {"consolas", "dejavu sans mono", "liberation mono", "ubuntu mono", "noto sans mono"};
                    for (const char* mf : mono_fallbacks) {
                        if (auto h = inner_->GetFontFaceHandle(mf, style, weight, size)) return h;
                    }
                } else if (cand == "sans-serif" || cand == "system-ui") {
                    const char* sans_fallbacks[] = {"segoe ui", "arial", "dejavu sans", "liberation sans", "noto sans", "ubuntu"};
                    for (const char* sf : sans_fallbacks) {
                        if (auto h = inner_->GetFontFaceHandle(sf, style, weight, size)) return h;
                    }
                }
            }
        }
        const char* defaults[] = {"segoe ui", "arial", "consolas", "dejavu sans", "liberation sans", "noto sans", "ubuntu", "dejavu sans mono"};
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
    int canvas_w = 0;
    int canvas_h = 0;

    int editor_w = 1440;
    int editor_h = 860;
    bool initialized = false;

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

        canvas_w = w;
        canvas_h = h;
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
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
    };
    for (const char* fp : linux_fallback_fonts) {
        if (fs::exists(fp)) {
            Rml::LoadFontFace(fp, true);
            break;
        }
    }
    const char* linux_extra_fonts[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/truetype/ubuntu/Ubuntu-B.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
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
    impl_->initialized = true;
    return true;
}

void RmlEngineHost::Shutdown() {
    if (!impl_) return;
    impl_->ReleaseCanvasRT();
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
    doc.GenerateRmlAndRcss(rml_str, rcss_str, assets, "style.rcss", true);

    impl_->file.base_dir = doc.base_dir;
    impl_->file.virtual_files["style.rcss"].assign(rcss_str.begin(), rcss_str.end());
    for (const auto& a : assets) {
        impl_->file.virtual_files[normalize_vpath(a.filename)] = a.bytes;
    }

    if (impl_->canvas_doc) {
        impl_->canvas_doc->Close();
        impl_->canvas_doc = nullptr;
    }
    Rml::Factory::ClearStyleSheetCache();
    impl_->canvas_doc = impl_->canvas_ctx->LoadDocumentFromMemory(rml_str);
    if (impl_->canvas_doc) {
        impl_->canvas_doc->Show();
    }
    impl_->canvas_ctx->Update();
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
        auto pos = cur->GetComputedValues().position();
        if (pos == Rml::Style::Position::Absolute || pos == Rml::Style::Position::Fixed) {
            return cur;
        }
        cur = cur->GetParentNode();
    }
    return doc;
}
} // namespace

bool RmlEngineHost::SetCanvasNodeBoxProps(int node_id, float x, float y, float w, float h,
                                          bool use_pct, float screen_w, float screen_h) {
    if (!impl_->initialized || !impl_->canvas_ctx || !impl_->canvas_doc) return false;
    Rml::Element* el = FindElementByFcid(impl_->canvas_doc, std::to_string(node_id));
    if (!el) return false;

    auto fmt = [](float v, const char* unit) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.2f%s", v, unit);
        return std::string(buf);
    };

    el->SetProperty("position", "absolute");
    el->SetProperty("margin", "0px");
    el->SetProperty("box-sizing", "border-box");

    float ref_w = screen_w;
    float ref_h = screen_h;
    if (Rml::Element* off_par = FindAbsContainingBlock(el, impl_->canvas_doc)) {
        if (off_par != impl_->canvas_doc) {
            auto psize = off_par->GetBox().GetSize(Rml::BoxArea::Padding);
            if (psize.x > 1.0f) ref_w = psize.x;
            if (psize.y > 1.0f) ref_h = psize.y;
        }
    }

    if (use_pct && ref_w > 1.0f && ref_h > 1.0f) {
        el->SetProperty("left", fmt((x / ref_w) * 100.0f, "%"));
        el->SetProperty("top", fmt((y / ref_h) * 100.0f, "%"));
        el->SetProperty("width", fmt((w / ref_w) * 100.0f, "%"));
        el->SetProperty("height", fmt((h / ref_h) * 100.0f, "%"));
    } else {
        el->SetProperty("left", fmt(x, "px"));
        el->SetProperty("top", fmt(y, "px"));
        el->SetProperty("width", fmt(w, "px"));
        el->SetProperty("height", fmt(h, "px"));
    }
    return true;
}

void RmlEngineHost::FlushCanvasRender() {
    if (!impl_->initialized || !impl_->canvas_ctx) return;
    impl_->canvas_ctx->Update();
    auto dims = impl_->canvas_ctx->GetDimensions();
    impl_->RenderCanvasToTexture(std::max(64, dims.x), std::max(64, dims.y));
}

bool RmlEngineHost::UpdateCanvasNodeBox(int node_id, float x, float y, float w, float h,
                                        bool use_pct, float screen_w, float screen_h) {
    if (!SetCanvasNodeBoxProps(node_id, x, y, w, h, use_pct, screen_w, screen_h)) return false;
    FlushCanvasRender();
    return true;
}

bool RmlEngineHost::GetCanvasElementRect(int node_id, float& out_x, float& out_y, float& out_w, float& out_h) const {
    if (!impl_->initialized || !impl_->canvas_doc) return false;
    Rml::Element* el = FindElementByFcid(impl_->canvas_doc, std::to_string(node_id));
    if (!el) return false;
    out_x = el->GetAbsoluteLeft();
    out_y = el->GetAbsoluteTop();
    out_w = el->GetOffsetWidth();
    out_h = el->GetOffsetHeight();
    return out_w > 0.0f || out_h > 0.0f;
}

bool RmlEngineHost::GetCanvasElementOffsetParentOrigin(int node_id, float& out_x, float& out_y) const {
    if (!impl_->initialized || !impl_->canvas_doc) return false;
    Rml::Element* el = FindElementByFcid(impl_->canvas_doc, std::to_string(node_id));
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
    impl_->canvas_ctx->Update();
    auto dims = impl_->canvas_ctx->GetDimensions();
    impl_->RenderCanvasToTexture(std::max(64, dims.x), std::max(64, dims.y));
}

void RmlEngineHost::CanvasMouseMove(int x, int y) {
    if (impl_->initialized && impl_->canvas_ctx) impl_->canvas_ctx->ProcessMouseMove(x, y, 0);
}
void RmlEngineHost::CanvasMouseButton(int button, bool down) {
    if (!impl_->initialized || !impl_->canvas_ctx) return;
    if (down) impl_->canvas_ctx->ProcessMouseButtonDown(button, 0);
    else impl_->canvas_ctx->ProcessMouseButtonUp(button, 0);
}
void RmlEngineHost::CanvasMouseWheel(float delta) {
    if (impl_->initialized && impl_->canvas_ctx) impl_->canvas_ctx->ProcessMouseWheel(Rml::Vector2f(0.0f, -delta), 0);
}
void RmlEngineHost::CanvasTextInput(uint32_t codepoint) {
    if (impl_->initialized && impl_->canvas_ctx) impl_->canvas_ctx->ProcessTextInput(static_cast<Rml::Character>(codepoint));
}
unsigned int RmlEngineHost::CanvasTexture() const {
    return impl_ ? static_cast<unsigned int>(impl_->canvas_tex) : 0u;
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

void RmlEngineHost::RenderEditor() {
    if (!impl_->initialized || !impl_->editor_ctx || !impl_->render) return;
    impl_->editor_ctx->Update();
    impl_->render->SetViewport(impl_->editor_w, impl_->editor_h);
    impl_->render->BeginFrame();
    impl_->editor_ctx->Render();
    impl_->render->EndFrame();
}

Rml::Context* RmlEngineHost::EditorContext() {
    return impl_ ? impl_->editor_ctx : nullptr;
}

Rml::Context* RmlEngineHost::CanvasContext() {
    return impl_ ? impl_->canvas_ctx : nullptr;
}

static void SanitizeInlineStylesInRml(std::string& rml_io, std::string* appended_rcss) {
    std::string low_work = rml_io;
    std::transform(low_work.begin(), low_work.end(), low_work.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    size_t spos = 0;
    while ((spos = low_work.find("<style", spos)) != std::string::npos) {
        size_t gt = low_work.find('>', spos);
        size_t end_st = low_work.find("</style>", gt == std::string::npos ? spos : gt);
        if (gt == std::string::npos || end_st == std::string::npos) break;
        std::string inline_raw = rml_io.substr(gt + 1, end_st - (gt + 1));
        std::string inline_clean = SanitizeRcssForRmlUi(inline_raw);
        if (appended_rcss) {
            if (!appended_rcss->empty() && appended_rcss->back() != '\n') *appended_rcss += "\n";
            *appended_rcss += inline_clean;
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
    auto first_class_tok = [](const std::string& cls) -> std::string {
        std::istringstream ss(cls);
        std::string tok;
        if (ss >> tok) return tok;
        return {};
    };
    auto truncate_utf8 = [](const std::string& s, size_t max_bytes) -> std::string {
        if (s.size() <= max_bytes) return s;
        size_t len = max_bytes;
        while (len > 0 && (static_cast<unsigned char>(s[len]) & 0xC0) == 0x80) {
            --len;
        }
        return s.substr(0, len);
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
            node.rml_class = cls_attr->Get<Rml::String>();
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
        node.css_positioned = (comp.position() != Rml::Style::Position::Static);
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
                if (!asset_dir.empty()) {
                    fs::path imgp = fs::u8path(asset_dir) / fs::u8path(node.image_src);
                    std::ifstream ifs(imgp, std::ios::binary | std::ios::ate);
                    if (ifs) {
                        auto sz = ifs.tellg();
                        ifs.seekg(0, std::ios::beg);
                        node.image_bytes.resize(static_cast<size_t>(sz));
                        if (sz > 0) ifs.read(reinterpret_cast<char*>(node.image_bytes.data()), sz);
                    }
                }
                if (node.image_bytes.empty()) {
                    auto vit = file_iface.virtual_files.find(normalize_vpath(node.image_src));
                    if (vit != file_iface.virtual_files.end()) {
                        node.image_bytes = vit->second;
                    } else if (fallback_nodes) {
                        for (const auto& fn : *fallback_nodes) {
                            if (fn.image_src == node.image_src && !fn.image_bytes.empty()) {
                                node.image_bytes = fn.image_bytes;
                                break;
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

        std::string first_cls = first_class_tok(node.rml_class);
        std::string txt_preview = !node.text.empty() ? node.text : node.placeholder;
        if (!node.rml_id.empty()) {
            node.name = "#" + node.rml_id;
        } else if (!txt_preview.empty()) {
            node.name = txt_preview.size() > 22 ? (truncate_utf8(txt_preview, 22) + "...") : txt_preview;
        } else if (!first_cls.empty()) {
            node.name = "." + first_cls;
        } else {
            node.name = node.tag;
        }

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
                                combined_rcss += sanitized_link;
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
                    combined_rcss = SanitizeRcssForRmlUi(raw_fb);
                    impl_->file.virtual_files["style.rcss"].assign(combined_rcss.begin(), combined_rcss.end());
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
    Rml::ElementDocument* imported = impl_->canvas_ctx->LoadDocumentFromMemory(load_rml);
    if (!imported) {
        out_error = "Falha ao carregar o arquivo RML: " + rml_path;
        return false;
    }
    imported->Show();
    impl_->canvas_ctx->Update();

    out_doc.base_dir = dir;
    out_doc.imported_raw_rml = raw_rml;
    out_doc.imported_raw_rcss = combined_rcss;
    PopulateDocFromElementDocument(imported, out_doc, impl_->file, p.stem().u8string(), dir, nullptr);

    imported->Close();
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
            return false;
        }

        // Se ainda era documento do zero, preserva o .rcss gerado atual antes de virar imported_source
        if (!doc.has_imported_source) {
            std::string init_rml, init_rcss;
            std::vector<RmlAsset> init_assets;
            doc.GenerateRmlAndRcss(init_rml, init_rcss, init_assets, "style.rcss", false);
            doc.imported_raw_rcss = SanitizeRcssForRmlUi(init_rcss);
            impl_->file.virtual_files["style.rcss"].assign(doc.imported_raw_rcss.begin(), doc.imported_raw_rcss.end());
            for (const auto& a : init_assets) {
                impl_->file.virtual_files[normalize_vpath(a.filename)] = a.bytes;
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
        Rml::ElementDocument* parsed = impl_->canvas_ctx->LoadDocumentFromMemory(load_rml);
        if (!parsed || impl_->sys->xml_error_count > prev_xml_errs) {
            if (parsed) parsed->Close();
            out_error = "Sintaxe RML incompleta ou invalida.";
            return false;
        }
        parsed->Show();
        impl_->canvas_ctx->Update();

        std::vector<RmlNode> prev_nodes = doc.nodes;
        doc.imported_raw_rml = raw_rml;
        std::string fallback_title = doc.title.empty() ? "Documento" : doc.title;
        PopulateDocFromElementDocument(parsed, doc, impl_->file, fallback_title, doc.base_dir, &prev_nodes);

        parsed->Close();
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
        file_iface.virtual_files[normalize_vpath(a.filename)] = a.bytes;
    }

    if (canvas_doc) {
        canvas_doc->Close();
        canvas_doc = nullptr;
    }
    Rml::Factory::ClearStyleSheetCache();
    canvas_doc = canvas_ctx->LoadDocumentFromMemory(rml_str);
    if (canvas_doc) {
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
                        node->css_positioned = (comp.position() != Rml::Style::Position::Static);
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
        ReloadCanvasDocAndSyncNodes(impl_->canvas_ctx, impl_->file, impl_->canvas_doc, doc, fname);
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
                impl_->file.virtual_files[normalize_vpath(a.filename)] = a.bytes;
            }
        }
        doc.imported_raw_rcss = SanitizeRcssForRmlUi(raw_rcss);
        for (auto& n : doc.nodes) {
            n.modified_transform = false;
            n.modified_style = false;
        }
        ReloadCanvasDocAndSyncNodes(impl_->canvas_ctx, impl_->file, impl_->canvas_doc, doc, "");
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
