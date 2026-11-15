#include "gl_renderer.h"
#include "../third_party/RmlUi/Backends/RmlUi_Include_GL3.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

const char* kVertSrc = R"GLSL(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTex;
uniform vec2 uViewport;
out vec2 vTex;
void main() {
    float nx = (aPos.x / uViewport.x) * 2.0 - 1.0;
    float ny = 1.0 - (aPos.y / uViewport.y) * 2.0;
    gl_Position = vec4(nx, ny, 0.0, 1.0);
    vTex = aTex;
}
)GLSL";

const char* kFragTexSrc = R"GLSL(
#version 330 core
in vec2 vTex;
uniform sampler2D uTex;
uniform vec4 uColor;
out vec4 FragColor;
void main() {
    FragColor = texture(uTex, vTex) * uColor;
}
)GLSL";

const char* kFragSolidSrc = R"GLSL(
#version 330 core
in vec2 vTex;
uniform vec4 uColor;
out vec4 FragColor;
void main() {
    FragColor = uColor;
}
)GLSL";

struct Vertex2D {
    float x, y;
    float u, v;
};

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
    u_col_tex_ = glGetUniformLocation(prog_tex_, "uColor");
    u_samp_tex_ = glGetUniformLocation(prog_tex_, "uTex");
    u_vp_solid_ = glGetUniformLocation(prog_solid_, "uViewport");
    u_col_solid_ = glGetUniformLocation(prog_solid_, "uColor");

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex2D) * 6, nullptr, GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex2D), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex2D), reinterpret_cast<void*>(sizeof(float) * 2));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

void GlRenderer2D::Shutdown() {
    if (vbo_) { GLuint b = vbo_; glDeleteBuffers(1, &b); vbo_ = 0; }
    if (vao_) { GLuint a = vao_; glDeleteVertexArrays(1, &a); vao_ = 0; }
    if (prog_tex_) { glDeleteProgram(prog_tex_); prog_tex_ = 0; }
    if (prog_solid_) { glDeleteProgram(prog_solid_); prog_solid_ = 0; }
    gladLoaderUnloadGL();
}

void GlRenderer2D::BeginFrame(int viewport_w, int viewport_h) {
    vp_w_ = std::max(1, viewport_w);
    vp_h_ = std::max(1, viewport_h);

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
    glDisable(GL_SCISSOR_TEST);
}

void GlRenderer2D::DrawQuadInternal(unsigned int prog, int u_vp, int u_col, unsigned int tex, float x, float y, float w, float h, float r, float g, float b, float a) {
    if (w <= 0.0f || h <= 0.0f) return;

    const Vertex2D verts[6] = {
        {x,     y,     0.0f, 0.0f},
        {x + w, y,     1.0f, 0.0f},
        {x + w, y + h, 1.0f, 1.0f},
        {x,     y,     0.0f, 0.0f},
        {x + w, y + h, 1.0f, 1.0f},
        {x,     y + h, 0.0f, 1.0f},
    };

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);

    glUseProgram(prog);
    glUniform2f(u_vp, static_cast<float>(vp_w_), static_cast<float>(vp_h_));
    glUniform4f(u_col, r, g, b, a);
    if (tex != 0) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glUniform1i(u_samp_tex_, 0);
    }

    glDrawArrays(GL_TRIANGLES, 0, 6);

    if (tex != 0) glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
}

void GlRenderer2D::DrawTexturedQuad(unsigned int tex, float x, float y, float w, float h) {
    if (!tex) return;
    DrawQuadInternal(prog_tex_, u_vp_tex_, u_col_tex_, tex, x, y, w, h, 1.0f, 1.0f, 1.0f, 1.0f);
}

void GlRenderer2D::DrawRectFilled(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    float af = a / 255.0f;
    DrawQuadInternal(prog_solid_, u_vp_solid_, u_col_solid_, 0, x, y, w, h, (r / 255.0f) * af, (g / 255.0f) * af, (b / 255.0f) * af, af);
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
