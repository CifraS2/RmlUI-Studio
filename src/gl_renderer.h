#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct GlTexture {
    unsigned int tex = 0;
    int width = 0;
    int height = 0;

    void Release();
    void Upload(int w, int h, const uint8_t* premul_rgba);
};

class GlRenderer2D {
public:
    struct Vertex2D {
        float x, y;
        float u, v;
        float r, g, b, a;
    };

    struct RulerOverlayDesc {
        bool show_top = false;
        bool show_left = false;
        float vp_x = 0.0f;
        float vp_y = 0.0f;
        float vp_w = 0.0f;
        float vp_h = 0.0f;
        float ruler_h = 20.0f;
        float ruler_left_w = 28.0f;
        float pan_x = 0.0f;
        float pan_y = 0.0f;
        float zoom = 1.0f;
        bool has_selection = false;
        float sel_x = 0.0f;
        float sel_y = 0.0f;
        float sel_w = 0.0f;
        float sel_h = 0.0f;
    };

    bool Init();
    void Shutdown();

    void BeginFrame(int viewport_w, int viewport_h);
    void SetScissor(int x, int y, int w, int h);
    void ClearScissor();
    void Flush();
    void DrawTexturedQuad(unsigned int tex, float x, float y, float w, float h, float zoom = 1.0f, uint64_t tex_gen = 0);
    void DrawRectFilled(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void DrawRectOutline(float x, float y, float w, float h, float thickness, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void DrawDashedRectOutline(float x, float y, float w, float h, float thickness, float dash_len, float gap_len, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void DrawRulerOverlay(const RulerOverlayDesc& desc);
    void DrawBitmapDigits(const char* text, float x, float y, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

    uint32_t LastFrameDrawCalls() const { return draw_calls_last_frame_; }

private:
    unsigned int prog_tex_ = 0;
    unsigned int prog_solid_ = 0;
    int u_vp_tex_ = -1;
    int u_samp_tex_ = -1;
    int u_vp_solid_ = -1;
    unsigned int vao_ = 0;
    unsigned int vbo_ = 0;
    size_t vbo_capacity_bytes_ = 0;
    int vp_w_ = 1;
    int vp_h_ = 1;
    unsigned int last_mip_tex_ = 0;
    uint64_t last_mip_gen_ = 0;
    unsigned int ruler_atlas_tex_ = 0;

    std::vector<Vertex2D> vertices_;
    bool batch_textured_ = false;
    unsigned int batch_tex_ = 0;
    float batch_zoom_ = 1.0f;
    uint64_t batch_tex_gen_ = 0;
    uint32_t draw_calls_this_frame_ = 0;
    uint32_t draw_calls_last_frame_ = 0;

    void PushQuadVertices(float x, float y, float w, float h, float r, float g, float b, float a,
                          float u0 = 0.0f, float v0 = 0.0f, float u1 = 1.0f, float v1 = 1.0f);
};
