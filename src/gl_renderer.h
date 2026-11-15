#pragma once

#include <cstdint>

struct GlTexture {
    unsigned int tex = 0;
    int width = 0;
    int height = 0;

    void Release();
    void Upload(int w, int h, const uint8_t* premul_rgba);
};

class GlRenderer2D {
public:
    bool Init();
    void Shutdown();

    void BeginFrame(int viewport_w, int viewport_h);
    void SetScissor(int x, int y, int w, int h);
    void ClearScissor();
    void DrawTexturedQuad(unsigned int tex, float x, float y, float w, float h);
    void DrawRectFilled(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void DrawRectOutline(float x, float y, float w, float h, float thickness, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void DrawDashedRectOutline(float x, float y, float w, float h, float thickness, float dash_len, float gap_len, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

private:
    unsigned int prog_tex_ = 0;
    unsigned int prog_solid_ = 0;
    int u_vp_tex_ = -1;
    int u_col_tex_ = -1;
    int u_samp_tex_ = -1;
    int u_vp_solid_ = -1;
    int u_col_solid_ = -1;
    unsigned int vao_ = 0;
    unsigned int vbo_ = 0;
    int vp_w_ = 1;
    int vp_h_ = 1;

    void DrawQuadInternal(unsigned int prog, int u_vp, int u_col, unsigned int tex, float x, float y, float w, float h, float r, float g, float b, float a);
};
