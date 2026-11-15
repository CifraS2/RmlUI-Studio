#pragma once

#include <d3d11.h>
#include <cstdint>

struct Dx11Texture {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    int width = 0;
    int height = 0;

    void Release();
    void Upload(ID3D11Device* device, ID3D11DeviceContext* ctx, int w, int h, const uint8_t* premul_rgba);
};

class Dx11Renderer2D {
public:
    bool Init(ID3D11Device* device);
    void Shutdown();

    void BeginFrame(ID3D11DeviceContext* ctx, int viewport_w, int viewport_h);
    void SetScissor(ID3D11DeviceContext* ctx, int x, int y, int w, int h);
    void ClearScissor(ID3D11DeviceContext* ctx);
    void DrawTexturedQuad(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srv, float x, float y, float w, float h);
    void DrawRectFilled(ID3D11DeviceContext* ctx, float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void DrawRectOutline(ID3D11DeviceContext* ctx, float x, float y, float w, float h, float thickness, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void DrawDashedRectOutline(ID3D11DeviceContext* ctx, float x, float y, float w, float h, float thickness, float dash_len, float gap_len, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

private:
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_tex_ = nullptr;
    ID3D11PixelShader* ps_solid_ = nullptr;
    ID3D11InputLayout* layout_ = nullptr;
    ID3D11Buffer* vb_ = nullptr;
    ID3D11Buffer* cb_ = nullptr;
    ID3D11BlendState* blend_premul_ = nullptr;
    ID3D11RasterizerState* rast_ = nullptr;
    ID3D11RasterizerState* rast_scissor_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;
    int vp_w_ = 1;
    int vp_h_ = 1;

    void DrawQuadInternal(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11ShaderResourceView* srv, float x, float y, float w, float h, float r, float g, float b, float a);
};
