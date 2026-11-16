#include "gl_renderer.h"
#include "../third_party/RmlUi/Backends/RmlUi_Include_GL3.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

const char* kVertSrc = R"GLSL(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTex;
layout(location = 2) in vec4 aCol;
uniform vec2 uViewport;
out vec2 vTex;
out vec4 vCol;
void main() {
    float nx = (aPos.x / uViewport.x) * 2.0 - 1.0;
    float ny = 1.0 - (aPos.y / uViewport.y) * 2.0;
    gl_Position = vec4(nx, ny, 0.0, 1.0);
    vTex = aTex;
    vCol = aCol;
}
)GLSL";

const char* kFragTexSrc = R"GLSL(
#version 330 core
in vec2 vTex;
in vec4 vCol;
uniform sampler2D uTex;
out vec4 FragColor;
void main() {
    FragColor = texture(uTex, vTex) * vCol;
}
)GLSL";

const char* kFragSolidSrc = R"GLSL(
#version 330 core
in vec2 vTex;
in vec4 vCol;
out vec4 FragColor;
void main() {
    FragColor = vCol;
}
)GLSL";

GLuint CompileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint LinkProgram(const char* vs_src, const char* fs_src) {
    GLuint vs = CompileShader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

constexpr int kGlyphW = 8;
constexpr int kGlyphH = 12;
constexpr int kGlyphCount = 12; // '0'..'9', '-', '.'

// Crisp 5x9 bitmap glyphs placed inside 8x12 cells (top-left padded by (1, 1)).
unsigned int CreateRulerGlyphAtlasTexture() {
    static const uint8_t kGlyphs[kGlyphCount][9] = {
        {0b01110, 0b10001, 0b10011, 0b10101, 0b10101, 0b11001, 0b10001, 0b10001, 0b01110}, // 0
        {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}, // 1
        {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b10001, 0b11111}, // 2
        {0b01110, 0b10001, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b10001, 0b01110}, // 3
        {0b00010, 0b00110, 0b01010, 0b10010, 0b10010, 0b11111, 0b00010, 0b00010, 0b00010}, // 4
        {0b11111, 0b10000, 0b10000, 0b11110, 0b00001, 0b00001, 0b00001, 0b10001, 0b01110}, // 5
        {0b01110, 0b10000, 0b10000, 0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}, // 6
        {0b11111, 0b00001, 0b00010, 0b00100, 0b00100, 0b01000, 0b01000, 0b01000, 0b01000}, // 7
        {0b01110, 0b10001, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b10001, 0b01110}, // 8
        {0b01110, 0b10001, 0b10001, 0b10001, 0b01111, 0b00001, 0b00001, 0b10001, 0b01110}, // 9
        {0b00000, 0b00000, 0b00000, 0b00000, 0b01110, 0b00000, 0b00000, 0b00000, 0b00000}, // -
        {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00110, 0b00110}, // .
    };

    const int atlas_w = kGlyphW * kGlyphCount;
    const int atlas_h = kGlyphH;
    std::vector<uint8_t> rgba(static_cast<size_t>(atlas_w * atlas_h * 4), 0u);
    for (int g = 0; g < kGlyphCount; ++g) {
        for (int row = 0; row < 9; ++row) {
            const uint8_t bits = kGlyphs[g][row];
            for (int col = 0; col < 5; ++col) {
                if ((bits >> (4 - col)) & 1u) {
                    const int px = g * kGlyphW + 1 + col;
                    const int py = 1 + row;
                    const size_t idx = static_cast<size_t>((py * atlas_w + px) * 4);
                    rgba[idx + 0] = 255;
                    rgba[idx + 1] = 255;
                    rgba[idx + 2] = 255;
                    rgba[idx + 3] = 255;
                }
            }
        }
    }

    GLuint t = 0;
    glGenTextures(1, &t);
    if (!t) return 0;
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, atlas_w, atlas_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return static_cast<unsigned int>(t);
}

int GlyphIndexForChar(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c == '-') return 10;
    if (c == '.') return 11;
    return -1;
}

float ChooseRulerMajorStep(float zoom) {
    static constexpr float kSteps[] = {10.0f, 20.0f, 25.0f, 50.0f, 100.0f, 200.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f};
    const float z = std::max(0.01f, zoom);
    for (float s : kSteps) {
        if (s * z >= 75.0f) return s;
    }
    return 5000.0f;
}

} // namespace

void GlTexture::Release() {
    if (tex) {
        GLuint t = static_cast<GLuint>(tex);
        glDeleteTextures(1, &t);
        tex = 0;
    }
    width = 0;
    height = 0;
}

void GlTexture::Upload(int w, int h, const uint8_t* premul_rgba) {
    Release();
    if (w <= 0 || h <= 0 || !premul_rgba) return;

    GLuint t = 0;
    glGenTextures(1, &t);
    if (!t) return;

    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, premul_rgba);
    glBindTexture(GL_TEXTURE_2D, 0);

    tex = static_cast<unsigned int>(t);
    width = w;
    height = h;
}

bool GlRenderer2D::Init() {
    if (gladLoaderLoadGL() == 0) return false;

    prog_tex_ = LinkProgram(kVertSrc, kFragTexSrc);
    prog_solid_ = LinkProgram(kVertSrc, kFragSolidSrc);
    if (!prog_tex_ || !prog_solid_) {
        Shutdown();
        return false;
    }

    u_vp_tex_ = glGetUniformLocation(prog_tex_, "uViewport");
    u_samp_tex_ = glGetUniformLocation(prog_tex_, "uTex");
    u_vp_solid_ = glGetUniformLocation(prog_solid_, "uViewport");

    constexpr size_t kInitialQuadCap = 1024;
    vbo_capacity_bytes_ = sizeof(Vertex2D) * 6 * kInitialQuadCap;
    vertices_.reserve(6 * kInitialQuadCap);

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vbo_capacity_bytes_), nullptr, GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex2D), reinterpret_cast<void*>(offsetof(Vertex2D, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex2D), reinterpret_cast<void*>(offsetof(Vertex2D, u)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex2D), reinterpret_cast<void*>(offsetof(Vertex2D, r)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    ruler_atlas_tex_ = CreateRulerGlyphAtlasTexture();
    return true;
}

void GlRenderer2D::Shutdown() {
    vertices_.clear();
    if (ruler_atlas_tex_) {
        GLuint t = ruler_atlas_tex_;
        glDeleteTextures(1, &t);
        ruler_atlas_tex_ = 0;
    }
    if (vbo_) { GLuint b = vbo_; glDeleteBuffers(1, &b); vbo_ = 0; }
    if (vao_) { GLuint a = vao_; glDeleteVertexArrays(1, &a); vao_ = 0; }
    if (prog_tex_) { glDeleteProgram(prog_tex_); prog_tex_ = 0; }
    if (prog_solid_) { glDeleteProgram(prog_solid_); prog_solid_ = 0; }
    vbo_capacity_bytes_ = 0;
    gladLoaderUnloadGL();
}

void GlRenderer2D::BeginFrame(int viewport_w, int viewport_h) {
    vp_w_ = std::max(1, viewport_w);
    vp_h_ = std::max(1, viewport_h);
    vertices_.clear();
    batch_textured_ = false;
    batch_tex_ = 0;
    draw_calls_last_frame_ = draw_calls_this_frame_;
    draw_calls_this_frame_ = 0;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, vp_w_, vp_h_);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glClearColor(0.055f, 0.067f, 0.090f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void GlRenderer2D::SetScissor(int x, int y, int w, int h) {
    Flush();
    int x0 = std::max(0, x);
    int y0 = std::max(0, y);
    int x1 = std::min(vp_w_, x + w);
    int y1 = std::min(vp_h_, y + h);
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0, vp_h_ - y1, x1 - x0, y1 - y0);
}

void GlRenderer2D::ClearScissor() {
    Flush();
    glDisable(GL_SCISSOR_TEST);
}

void GlRenderer2D::Flush() {
    if (vertices_.empty()) return;

    const size_t needed_bytes = vertices_.size() * sizeof(Vertex2D);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    if (needed_bytes > vbo_capacity_bytes_) {
        while (vbo_capacity_bytes_ < needed_bytes) {
            vbo_capacity_bytes_ = std::max<size_t>(vbo_capacity_bytes_ * 2, needed_bytes);
        }
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vbo_capacity_bytes_), nullptr, GL_DYNAMIC_DRAW);
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(needed_bytes), vertices_.data());

    if (batch_textured_ && batch_tex_ != 0) {
        glUseProgram(prog_tex_);
        glUniform2f(u_vp_tex_, static_cast<float>(vp_w_), static_cast<float>(vp_h_));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, batch_tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (std::abs(batch_zoom_ - 1.0f) < 0.001f) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        } else if (batch_zoom_ < 1.0f) {
            if (batch_tex_gen_ == 0 || batch_tex_ != last_mip_tex_ || batch_tex_gen_ != last_mip_gen_) {
                glGenerateMipmap(GL_TEXTURE_2D);
                last_mip_tex_ = batch_tex_;
                last_mip_gen_ = batch_tex_gen_;
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        } else {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
        glUniform1i(u_samp_tex_, 0);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices_.size()));
        glBindTexture(GL_TEXTURE_2D, 0);
    } else {
        glUseProgram(prog_solid_);
        glUniform2f(u_vp_solid_, static_cast<float>(vp_w_), static_cast<float>(vp_h_));
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices_.size()));
    }

    glBindVertexArray(0);
    glUseProgram(0);
    vertices_.clear();
    batch_textured_ = false;
    batch_tex_ = 0;
    ++draw_calls_this_frame_;
}

void GlRenderer2D::PushQuadVertices(float x, float y, float w, float h, float r, float g, float b, float a,
                                    float u0, float v0, float u1, float v1) {
    vertices_.push_back({x,     y,     u0, v0, r, g, b, a});
    vertices_.push_back({x + w, y,     u1, v0, r, g, b, a});
    vertices_.push_back({x + w, y + h, u1, v1, r, g, b, a});
    vertices_.push_back({x,     y,     u0, v0, r, g, b, a});
    vertices_.push_back({x + w, y + h, u1, v1, r, g, b, a});
    vertices_.push_back({x,     y + h, u0, v1, r, g, b, a});
}

void GlRenderer2D::DrawBitmapDigits(const char* text, float x, float y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!ruler_atlas_tex_ || !text || !*text) return;
    if (!vertices_.empty() && (!batch_textured_ || batch_tex_ != ruler_atlas_tex_)) {
        Flush();
    }
    batch_textured_ = true;
    batch_tex_ = ruler_atlas_tex_;
    batch_zoom_ = 1.0f;
    batch_tex_gen_ = 1;

    const float af = a / 255.0f;
    const float rf = (r / 255.0f) * af;
    const float gf = (g / 255.0f) * af;
    const float bf = (b / 255.0f) * af;
    const float inv_atlas_w = 1.0f / static_cast<float>(kGlyphW * kGlyphCount);

    float cx = std::round(x);
    const float cy = std::round(y);
    for (const char* p = text; *p; ++p) {
        const int idx = GlyphIndexForChar(*p);
        if (idx >= 0) {
            const float u0 = static_cast<float>(idx * kGlyphW) * inv_atlas_w;
            const float u1 = static_cast<float>((idx + 1) * kGlyphW) * inv_atlas_w;
            PushQuadVertices(cx, cy, static_cast<float>(kGlyphW), static_cast<float>(kGlyphH),
                             rf, gf, bf, af, u0, 0.0f, u1, 1.0f);
        }
        cx += 6.0f;
    }
}

void GlRenderer2D::DrawRulerOverlay(const RulerOverlayDesc& d) {
    if (!d.show_top || d.vp_w <= 1.0f || d.vp_h <= 1.0f) return;

    const float z = std::max(0.01f, d.zoom);
    const float major_step = ChooseRulerMajorStep(z);
    const float minor_step = major_step * 0.1f;
    const bool draw_minor = (minor_step * z >= 4.0f);
    const bool draw_med = ((major_step * 0.5f) * z >= 10.0f);

    const float top_x0 = d.vp_x;
    const float top_y0 = d.vp_y - d.ruler_h;
    const float top_x1 = d.vp_x + d.vp_w;

    const float left_x0 = d.vp_x - d.ruler_left_w;
    const float left_y0 = d.vp_y;
    const float left_y1 = d.vp_y + d.vp_h;

    // 1) Solid pass: backgrounds, selection highlight bands, border dividers & all ticks
    DrawRectFilled(top_x0, top_y0, d.vp_w, d.ruler_h, 18, 21, 29, 255);
    DrawRectFilled(top_x0, d.vp_y - 1.0f, d.vp_w, 1.0f, 37, 42, 52, 255);
    if (d.show_left) {
        DrawRectFilled(left_x0, left_y0, d.ruler_left_w, d.vp_h, 18, 21, 29, 255);
        DrawRectFilled(d.vp_x - 1.0f, left_y0, 1.0f, d.vp_h, 37, 42, 52, 255);
    }

    if (d.has_selection) {
        const float hsx0 = std::clamp(d.pan_x + d.sel_x * z, top_x0, top_x1);
        const float hsx1 = std::clamp(d.pan_x + (d.sel_x + d.sel_w) * z, top_x0, top_x1);
        if (hsx1 > hsx0) {
            DrawRectFilled(hsx0, top_y0, hsx1 - hsx0, d.ruler_h - 1.0f, 59, 130, 246, 42);
            DrawRectFilled(hsx0, d.vp_y - 2.0f, hsx1 - hsx0, 2.0f, 59, 130, 246, 200);
        }
        if (d.show_left) {
            const float hsy0 = std::clamp(d.pan_y + d.sel_y * z, left_y0, left_y1);
            const float hsy1 = std::clamp(d.pan_y + (d.sel_y + d.sel_h) * z, left_y0, left_y1);
            if (hsy1 > hsy0) {
                DrawRectFilled(left_x0, hsy0, d.ruler_left_w - 1.0f, hsy1 - hsy0, 59, 130, 246, 42);
                DrawRectFilled(d.vp_x - 2.0f, hsy0, 2.0f, hsy1 - hsy0, 59, 130, 246, 200);
            }
        }
    }

    const int start_idx_x = static_cast<int>(std::floor((top_x0 - d.pan_x) / (major_step * z)));
    const int end_idx_x = static_cast<int>(std::ceil((top_x1 - d.pan_x) / (major_step * z)));
    for (int m = start_idx_x; m <= end_idx_x; ++m) {
        const float c_base = static_cast<float>(m) * major_step;
        for (int sub = 0; sub < 10; ++sub) {
            if (sub != 0 && !(sub == 5 && draw_med) && !draw_minor) continue;
            const float sx = std::round(d.pan_x + (c_base + static_cast<float>(sub) * minor_step) * z);
            if (sx < top_x0 || sx >= top_x1) continue;
            const float tick_h = (sub == 0) ? 14.0f : ((sub == 5) ? 7.0f : 4.0f);
            const uint8_t col = (sub == 0) ? 88 : 53;
            DrawRectFilled(sx, d.vp_y - tick_h, 1.0f, tick_h - 1.0f, col, col + 6, col + 18, 255);
        }
    }

    if (d.show_left) {
        const int start_idx_y = static_cast<int>(std::floor((left_y0 - d.pan_y) / (major_step * z)));
        const int end_idx_y = static_cast<int>(std::ceil((left_y1 - d.pan_y) / (major_step * z)));
        for (int m = start_idx_y; m <= end_idx_y; ++m) {
            const float c_base = static_cast<float>(m) * major_step;
            for (int sub = 0; sub < 10; ++sub) {
                if (sub != 0 && !(sub == 5 && draw_med) && !draw_minor) continue;
                const float sy = std::round(d.pan_y + (c_base + static_cast<float>(sub) * minor_step) * z);
                if (sy < left_y0 || sy >= left_y1) continue;
                const float tick_w = (sub == 0) ? 12.0f : ((sub == 5) ? 6.0f : 3.0f);
                const uint8_t col = (sub == 0) ? 88 : 53;
                DrawRectFilled(d.vp_x - tick_w, sy, tick_w - 1.0f, 1.0f, col, col + 6, col + 18, 255);
            }
        }
    }

    // 2) Textured bitmap digit pass (automatically flushes solid batch once)
    char buf[24];
    for (int m = start_idx_x; m <= end_idx_x; ++m) {
        const float c_val = static_cast<float>(m) * major_step;
        const float sx = std::round(d.pan_x + c_val * z);
        if (sx + 3.0f < top_x0 || sx + 28.0f >= top_x1) continue;
        std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::round(c_val)));
        DrawBitmapDigits(buf, sx + 3.0f, top_y0 + 3.0f, 115, 128, 151, 255);
    }

    if (d.show_left) {
        const int start_idx_y = static_cast<int>(std::floor((left_y0 - d.pan_y) / (major_step * z)));
        const int end_idx_y = static_cast<int>(std::ceil((left_y1 - d.pan_y) / (major_step * z)));
        for (int m = start_idx_y; m <= end_idx_y; ++m) {
            const float c_val = static_cast<float>(m) * major_step;
            const float sy = std::round(d.pan_y + c_val * z);
            if (sy + 2.0f < left_y0 || sy + 13.0f >= left_y1) continue;
            std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::round(c_val)));
            DrawBitmapDigits(buf, left_x0 + 2.0f, sy + 2.0f, 115, 128, 151, 255);
        }
    }
}

void GlRenderer2D::DrawTexturedQuad(unsigned int tex, float x, float y, float w, float h, float zoom, uint64_t tex_gen) {
    if (!tex || w <= 0.0f || h <= 0.0f) return;
    if (std::abs(zoom - 1.0f) < 0.001f) {
        x = std::round(x);
        y = std::round(y);
        w = std::round(w);
        h = std::round(h);
    }
    if (!vertices_.empty() && (!batch_textured_ || batch_tex_ != tex ||
        std::abs(batch_zoom_ - zoom) > 0.0001f || batch_tex_gen_ != tex_gen)) {
        Flush();
    }
    batch_textured_ = true;
    batch_tex_ = tex;
    batch_zoom_ = zoom;
    batch_tex_gen_ = tex_gen;
    PushQuadVertices(x, y, w, h, 1.0f, 1.0f, 1.0f, 1.0f);
}

void GlRenderer2D::DrawRectFilled(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (w <= 0.0f || h <= 0.0f) return;
    if (!vertices_.empty() && batch_textured_) {
        Flush();
    }
    batch_textured_ = false;
    batch_tex_ = 0;
    const float af = a / 255.0f;
    PushQuadVertices(x, y, w, h, (r / 255.0f) * af, (g / 255.0f) * af, (b / 255.0f) * af, af);
}

void GlRenderer2D::DrawRectOutline(float x, float y, float w, float h, float thickness, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (w <= 0.0f || h <= 0.0f) return;
    DrawRectFilled(x, y, w, thickness, r, g, b, a);
    DrawRectFilled(x, y + h - thickness, w, thickness, r, g, b, a);
    DrawRectFilled(x, y + thickness, thickness, std::max(0.0f, h - thickness * 2.0f), r, g, b, a);
    DrawRectFilled(x + w - thickness, y + thickness, thickness, std::max(0.0f, h - thickness * 2.0f), r, g, b, a);
}

void GlRenderer2D::DrawDashedRectOutline(float x, float y, float w, float h, float thickness, float dash_len, float gap_len, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (w <= 0.0f || h <= 0.0f) return;
    float step = std::max(1.0f, dash_len + gap_len);
    for (float dx = 0.0f; dx < w; dx += step) {
        float seg = std::min(dash_len, w - dx);
        DrawRectFilled(x + dx, y, seg, thickness, r, g, b, a);
        DrawRectFilled(x + dx, y + h - thickness, seg, thickness, r, g, b, a);
    }
    for (float dy = 0.0f; dy < h; dy += step) {
        float seg = std::min(dash_len, h - dy);
        DrawRectFilled(x, y + dy, thickness, seg, r, g, b, a);
        DrawRectFilled(x + w - thickness, y + dy, thickness, seg, r, g, b, a);
    }
}
