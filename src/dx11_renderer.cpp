#include "dx11_renderer.h"

#include <d3dcompiler.h>
#include <cstring>

namespace {

struct Vertex2D {
    float x, y;
    float u, v;
};

struct alignas(16) Constants2D {
    float vp_size[2];
    float pad[2];
    float color[4];
};

const char* kShaderSrc = R"(
cbuffer Constants : register(b0) {
    float2 vp_size;
    float2 _pad;
    float4 color;
};

struct VSIn {
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

struct PSIn {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

PSIn VSMain(VSIn input) {
    PSIn o;
    float nx = (input.pos.x / vp_size.x) * 2.0 - 1.0;
    float ny = 1.0 - (input.pos.y / vp_size.y) * 2.0;
    o.pos = float4(nx, ny, 0.0, 1.0);
    o.uv = input.uv;
    return o;
}

Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);

float4 PSTex(PSIn input) : SV_TARGET {
    return tex0.Sample(samp0, input.uv);
}

float4 PSSolid(PSIn input) : SV_TARGET {
    return float4(color.rgb * color.a, color.a);
}
)";

} // namespace

void Dx11Texture::Release() {
    if (srv) { srv->Release(); srv = nullptr; }
    if (tex) { tex->Release(); tex = nullptr; }
    width = 0;
    height = 0;
}

void Dx11Texture::Upload(ID3D11Device* device, ID3D11DeviceContext* ctx, int w, int h, const uint8_t* premul_rgba) {
    if (!device || !ctx || !premul_rgba || w <= 0 || h <= 0) return;
    if (!tex || width != w || height != h) {
        Release();
        width = w;
        height = h;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(w);
        desc.Height = static_cast<UINT>(h);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = premul_rgba;
        init.SysMemPitch = static_cast<UINT>(w * 4);
        if (SUCCEEDED(device->CreateTexture2D(&desc, &init, &tex)) && tex) {
            device->CreateShaderResourceView(tex, nullptr, &srv);
        }
    } else {
        ctx->UpdateSubresource(tex, 0, nullptr, premul_rgba, static_cast<UINT>(w * 4), 0);
    }
}

bool Dx11Renderer2D::Init(ID3D11Device* device) {
    ID3DBlob* vs_blob = nullptr;
    ID3DBlob* ps_tex_blob = nullptr;
    ID3DBlob* ps_solid_blob = nullptr;
    ID3DBlob* err = nullptr;

    if (FAILED(D3DCompile(kShaderSrc, std::strlen(kShaderSrc), nullptr, nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vs_blob, &err))) {
        if (err) err->Release();
        return false;
    }
    if (FAILED(D3DCompile(kShaderSrc, std::strlen(kShaderSrc), nullptr, nullptr, nullptr, "PSTex", "ps_4_0", 0, 0, &ps_tex_blob, &err))) {
        if (err) err->Release();
        vs_blob->Release();
        return false;
    }
    if (FAILED(D3DCompile(kShaderSrc, std::strlen(kShaderSrc), nullptr, nullptr, nullptr, "PSSolid", "ps_4_0", 0, 0, &ps_solid_blob, &err))) {
        if (err) err->Release();
        vs_blob->Release();
        ps_tex_blob->Release();
        return false;
    }

    device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vs_);
    device->CreatePixelShader(ps_tex_blob->GetBufferPointer(), ps_tex_blob->GetBufferSize(), nullptr, &ps_tex_);
    device->CreatePixelShader(ps_solid_blob->GetBufferPointer(), ps_solid_blob->GetBufferSize(), nullptr, &ps_solid_);

    D3D11_INPUT_ELEMENT_DESC ied[2] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device->CreateInputLayout(ied, 2, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &layout_);

    vs_blob->Release();
    ps_tex_blob->Release();
    ps_solid_blob->Release();

    D3D11_BUFFER_DESC vbd{};
    vbd.Usage = D3D11_USAGE_DYNAMIC;
    vbd.ByteWidth = sizeof(Vertex2D) * 6;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&vbd, nullptr, &vb_);

    D3D11_BUFFER_DESC cbd{};
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.ByteWidth = sizeof(Constants2D);
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&cbd, nullptr, &cb_);

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&bd, &blend_premul_);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = FALSE;
    rd.DepthClipEnable = TRUE;
    device->CreateRasterizerState(&rd, &rast_);

    rd.ScissorEnable = TRUE;
    device->CreateRasterizerState(&rd, &rast_scissor_);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device->CreateSamplerState(&sd, &sampler_);

    return vs_ && ps_tex_ && ps_solid_ && layout_ && vb_ && cb_;
}

void Dx11Renderer2D::Shutdown() {
    if (sampler_) { sampler_->Release(); sampler_ = nullptr; }
    if (rast_scissor_) { rast_scissor_->Release(); rast_scissor_ = nullptr; }
    if (rast_) { rast_->Release(); rast_ = nullptr; }
    if (blend_premul_) { blend_premul_->Release(); blend_premul_ = nullptr; }
    if (cb_) { cb_->Release(); cb_ = nullptr; }
    if (vb_) { vb_->Release(); vb_ = nullptr; }
    if (layout_) { layout_->Release(); layout_ = nullptr; }
    if (ps_solid_) { ps_solid_->Release(); ps_solid_ = nullptr; }
    if (ps_tex_) { ps_tex_->Release(); ps_tex_ = nullptr; }
    if (vs_) { vs_->Release(); vs_ = nullptr; }
}

void Dx11Renderer2D::BeginFrame(ID3D11DeviceContext* ctx, int viewport_w, int viewport_h) {
    vp_w_ = viewport_w > 0 ? viewport_w : 1;
    vp_h_ = viewport_h > 0 ? viewport_h : 1;

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(vp_w_);
    vp.Height = static_cast<float>(vp_h_);
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(rast_);
    const float bf[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(blend_premul_, bf, 0xffffffff);
    ctx->IASetInputLayout(layout_);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &cb_);
    ctx->PSSetConstantBuffers(0, 1, &cb_);
    ctx->PSSetSamplers(0, 1, &sampler_);
}

void Dx11Renderer2D::DrawQuadInternal(
    ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11ShaderResourceView* srv,
    float x, float y, float w, float h, float r, float g, float b, float a
) {
    Vertex2D verts[6] = {
        {x,     y,     0.0f, 0.0f},
        {x + w, y,     1.0f, 0.0f},
        {x,     y + h, 0.0f, 1.0f},
        {x + w, y,     1.0f, 0.0f},
        {x + w, y + h, 1.0f, 1.0f},
        {x,     y + h, 0.0f, 1.0f},
    };
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(ctx->Map(vb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, verts, sizeof(verts));
        ctx->Unmap(vb_, 0);
    }
    if (SUCCEEDED(ctx->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        Constants2D c{};
        c.vp_size[0] = static_cast<float>(vp_w_);
        c.vp_size[1] = static_cast<float>(vp_h_);
        c.color[0] = r; c.color[1] = g; c.color[2] = b; c.color[3] = a;
        std::memcpy(mapped.pData, &c, sizeof(c));
        ctx->Unmap(cb_, 0);
    }

    UINT stride = sizeof(Vertex2D), offset = 0;
    ctx->IASetVertexBuffers(0, 1, &vb_, &stride, &offset);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->Draw(6, 0);
}

void Dx11Renderer2D::SetScissor(ID3D11DeviceContext* ctx, int x, int y, int w, int h) {
    if (!ctx || !rast_scissor_) return;
    D3D11_RECT r{};
    r.left = x < 0 ? 0 : x;
    r.top = y < 0 ? 0 : y;
    r.right = x + (w < 0 ? 0 : w);
    r.bottom = y + (h < 0 ? 0 : h);
    if (r.right < r.left) r.right = r.left;
    if (r.bottom < r.top) r.bottom = r.top;
    ctx->RSSetState(rast_scissor_);
    ctx->RSSetScissorRects(1, &r);
}

void Dx11Renderer2D::ClearScissor(ID3D11DeviceContext* ctx) {
    if (!ctx || !rast_) return;
    ctx->RSSetState(rast_);
}

void Dx11Renderer2D::DrawTexturedQuad(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srv, float x, float y, float w, float h) {
    if (!srv || w <= 0.0f || h <= 0.0f) return;
    DrawQuadInternal(ctx, ps_tex_, srv, x, y, w, h, 1, 1, 1, 1);
}

void Dx11Renderer2D::DrawRectFilled(ID3D11DeviceContext* ctx, float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (w <= 0.0f || h <= 0.0f) return;
    DrawQuadInternal(ctx, ps_solid_, nullptr, x, y, w, h, r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

void Dx11Renderer2D::DrawRectOutline(ID3D11DeviceContext* ctx, float x, float y, float w, float h, float t, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    DrawRectFilled(ctx, x, y, w, t, r, g, b, a);
    DrawRectFilled(ctx, x, y + h - t, w, t, r, g, b, a);
    DrawRectFilled(ctx, x, y, t, h, r, g, b, a);
    DrawRectFilled(ctx, x + w - t, y, t, h, r, g, b, a);
}

void Dx11Renderer2D::DrawDashedRectOutline(ID3D11DeviceContext* ctx, float x, float y, float w, float h, float t, float dash_len, float gap_len, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (w <= 0.0f || h <= 0.0f || dash_len <= 0.0f) return;
    float step = dash_len + gap_len;
    for (float cx = 0.0f; cx < w; cx += step) {
        float seg = (cx + dash_len > w) ? (w - cx) : dash_len;
        DrawRectFilled(ctx, x + cx, y, seg, t, r, g, b, a);
        DrawRectFilled(ctx, x + cx, y + h - t, seg, t, r, g, b, a);
    }
    for (float cy = 0.0f; cy < h; cy += step) {
        float seg = (cy + dash_len > h) ? (h - cy) : dash_len;
        DrawRectFilled(ctx, x, y + cy, t, seg, r, g, b, a);
        DrawRectFilled(ctx, x + w - t, y + cy, t, seg, r, g, b, a);
    }
}
