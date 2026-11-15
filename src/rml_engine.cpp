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

#include <DirectXMath.h>
#include <d3dcompiler.h>
#include <wincodec.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
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

bool decode_image_wic(const uint8_t* bytes, size_t len, int& out_w, int& out_h, std::vector<uint8_t>& out_premul_rgba) {
    if (!bytes || len == 0) return false;
    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) return false;

    IWICStream* stream = nullptr;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr) || !stream) { factory->Release(); return false; }
    hr = stream->InitializeFromMemory(const_cast<BYTE*>(bytes), static_cast<DWORD>(len));
    if (FAILED(hr)) { stream->Release(); factory->Release(); return false; }

    IWICBitmapDecoder* decoder = nullptr;
    hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr) || !decoder) { stream->Release(); factory->Release(); return false; }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) { decoder->Release(); stream->Release(); factory->Release(); return false; }

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    IWICFormatConverter* conv = nullptr;
    hr = factory->CreateFormatConverter(&conv);
    if (FAILED(hr) || !conv) { frame->Release(); decoder->Release(); stream->Release(); factory->Release(); return false; }

    hr = conv->Initialize(frame, GUID_WICPixelFormat32bppPRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr) && w > 0 && h > 0) {
        out_w = static_cast<int>(w);
        out_h = static_cast<int>(h);
        out_premul_rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
        hr = conv->CopyPixels(nullptr, w * 4u, static_cast<UINT>(out_premul_rgba.size()), out_premul_rgba.data());
    }

    conv->Release();
    frame->Release();
    decoder->Release();
    stream->Release();
    factory->Release();
    return SUCCEEDED(hr);
}

struct OpenFile {
    std::vector<uint8_t> owned;
    const std::vector<uint8_t>* data = nullptr;
    size_t pos = 0;
};

class StudioSystemInterface final : public Rml::SystemInterface {
public:
    StudioSystemInterface() : start_(std::chrono::steady_clock::now()) {}
    int xml_error_count = 0;

    double GetElapsedTime() override {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        if (type == Rml::Log::LT_ERROR) {
            if (message.find("XML") != Rml::String::npos || message.find("Failed to load") != Rml::String::npos) {
                ++xml_error_count;
            }
        }
        if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_WARNING || type == Rml::Log::LT_ASSERT) {
            std::fprintf(stderr, "[Rml:%d] %s\n", (int)type, message.c_str());
        }
        return true;
    }
    void SetClipboardText(const Rml::String& text) override {
        if (!OpenClipboard(nullptr)) return;
        EmptyClipboard();
        if (!text.empty()) {
            int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
            if (len > 0) {
                HGLOBAL hmem = GlobalAlloc(GMEM_MOVEABLE, static_cast<size_t>(len + 1) * sizeof(wchar_t));
                if (hmem) {
                    if (auto* ptr = static_cast<wchar_t*>(GlobalLock(hmem))) {
                        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), ptr, len);
                        ptr[len] = L'\0';
                        GlobalUnlock(hmem);
                        SetClipboardData(CF_UNICODETEXT, hmem);
                    } else {
                        GlobalFree(hmem);
                    }
                }
            }
        }
        CloseClipboard();
    }
    void GetClipboardText(Rml::String& text) override {
        text.clear();
        if (!OpenClipboard(nullptr)) return;
        if (HANDLE hdata = GetClipboardData(CF_UNICODETEXT)) {
            if (const auto* wptr = static_cast<const wchar_t*>(GlobalLock(hdata))) {
                int bytes = WideCharToMultiByte(CP_UTF8, 0, wptr, -1, nullptr, 0, nullptr, nullptr);
                if (bytes > 1) {
                    text.resize(static_cast<size_t>(bytes - 1));
                    WideCharToMultiByte(CP_UTF8, 0, wptr, -1, text.data(), bytes, nullptr, nullptr);
                }
                GlobalUnlock(hdata);
            }
        }
        CloseClipboard();
    }
private:
    std::chrono::steady_clock::time_point start_;
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

struct GpuGeometry {
    ID3D11Buffer* vb = nullptr;
    ID3D11Buffer* ib = nullptr;
    UINT index_count = 0;
};

struct GpuTexture {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    int width = 0;
    int height = 0;
};

struct alignas(16) RmlCB {
    float mvp[16];
    float translation[2];
    float has_tex;
    float pad;
};

const char* kRmlHlsl = R"HLSL(
cbuffer CB : register(b0) {
    row_major float4x4 mvp;
    float2 translation;
    float has_tex;
    float pad;
};

struct VSIn {
    float2 pos : POSITION;
    float4 col : COLOR;
    float2 uv  : TEXCOORD;
};

struct PSIn {
    float4 pos : SV_POSITION;
    float4 col : COLOR;
    float2 uv  : TEXCOORD;
};

PSIn VSMain(VSIn i) {
    PSIn o;
    float2 p = i.pos + translation;
    o.pos = mul(mvp, float4(p, 0.0, 1.0));
    o.col = i.col;
    o.uv = i.uv;
    return o;
}

Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);

float4 PSMain(PSIn i) : SV_TARGET {
    if (has_tex > 0.5) {
        return tex0.Sample(samp0, i.uv) * i.col;
    }
    return i.col;
}
)HLSL";

class Dx11RmlRenderInterface final : public Rml::RenderInterface {
public:
    StudioFileInterface* file_iface = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;

    int vp_w = 1440;
    int vp_h = 860;

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11BlendState* blend = nullptr;
    ID3D11RasterizerState* rast = nullptr;
    ID3D11DepthStencilState* depth = nullptr;
    ID3D11SamplerState* sampler = nullptr;

    bool scissor_enabled = false;
    Rml::Rectanglei scissor_rect = Rml::Rectanglei::MakeInvalid();
    bool has_transform = false;
    Rml::Matrix4f transform = Rml::Matrix4f::Identity();
    DirectX::XMMATRIX ortho = DirectX::XMMatrixIdentity();

    bool Init(ID3D11Device* dev, ID3D11DeviceContext* dc) {
        static_assert(sizeof(Rml::Vertex) == 20, "Rml::Vertex must be 20 bytes");
        device = dev;
        ctx = dc;
        if (!device || !ctx) return false;

        ID3DBlob* vs_blob = nullptr;
        ID3DBlob* ps_blob = nullptr;
        ID3DBlob* err_blob = nullptr;
        if (FAILED(D3DCompile(kRmlHlsl, std::strlen(kRmlHlsl), nullptr, nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vs_blob, &err_blob))) {
            if (err_blob) err_blob->Release();
            return false;
        }
        if (FAILED(D3DCompile(kRmlHlsl, std::strlen(kRmlHlsl), nullptr, nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &ps_blob, &err_blob))) {
            vs_blob->Release();
            if (err_blob) err_blob->Release();
            return false;
        }
        device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vs);
        device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &ps);

        D3D11_INPUT_ELEMENT_DESC ied[3] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        device->CreateInputLayout(ied, 3, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &layout);
        vs_blob->Release();
        ps_blob->Release();

        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = sizeof(RmlCB);
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device->CreateBuffer(&cbd, nullptr, &cb);

        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device->CreateBlendState(&bd, &blend);

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.ScissorEnable = TRUE;
        rd.DepthClipEnable = TRUE;
        device->CreateRasterizerState(&rd, &rast);

        D3D11_DEPTH_STENCIL_DESC dsd{};
        dsd.DepthEnable = FALSE;
        device->CreateDepthStencilState(&dsd, &depth);

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        device->CreateSamplerState(&sd, &sampler);

        SetViewport(vp_w, vp_h);
        return vs && ps && layout && cb && blend && rast && depth && sampler;
    }

    void Shutdown() {
        if (sampler) { sampler->Release(); sampler = nullptr; }
        if (depth) { depth->Release(); depth = nullptr; }
        if (rast) { rast->Release(); rast = nullptr; }
        if (blend) { blend->Release(); blend = nullptr; }
        if (cb) { cb->Release(); cb = nullptr; }
        if (layout) { layout->Release(); layout = nullptr; }
        if (ps) { ps->Release(); ps = nullptr; }
        if (vs) { vs->Release(); vs = nullptr; }
    }

    void SetViewport(int w, int h) {
        vp_w = std::max(1, w);
        vp_h = std::max(1, h);
        ortho = DirectX::XMMatrixOrthographicOffCenterLH(0.0f, (float)vp_w, (float)vp_h, 0.0f, -1.0f, 1.0f);
    }

    void UpdateCB(Rml::Vector2f translation, bool has_tex) {
        if (!ctx || !cb) return;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            auto* data = reinterpret_cast<RmlCB*>(mapped.pData);
            DirectX::XMMATRIX m = ortho;
            if (has_transform) {
                Rml::Matrix4f t = transform.Transpose();
                DirectX::XMMATRIX model(t.data());
                m = DirectX::XMMatrixMultiply(model, ortho);
            }
            DirectX::XMMATRIX mvp_t = DirectX::XMMatrixTranspose(m);
            DirectX::XMStoreFloat4x4(reinterpret_cast<DirectX::XMFLOAT4X4*>(data->mvp), mvp_t);
            data->translation[0] = translation.x;
            data->translation[1] = translation.y;
            data->has_tex = has_tex ? 1.0f : 0.0f;
            data->pad = 0.0f;
            ctx->Unmap(cb, 0);
        }
    }

    void BeginPass() {
        if (!ctx) return;
        has_transform = false;
        scissor_enabled = false;
        D3D11_VIEWPORT vp{0.0f, 0.0f, (float)vp_w, (float)vp_h, 0.0f, 1.0f};
        ctx->RSSetViewports(1, &vp);
        ctx->IASetInputLayout(layout);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        UpdateCB({0.0f, 0.0f}, false);
        ctx->VSSetConstantBuffers(0, 1, &cb);
        ctx->PSSetConstantBuffers(0, 1, &cb);
        ctx->PSSetSamplers(0, 1, &sampler);
        float bf[4] = {0, 0, 0, 0};
        ctx->OMSetBlendState(blend, bf, 0xFFFFFFFF);
        ctx->OMSetDepthStencilState(depth, 0);
        ctx->RSSetState(rast);
        ApplyScissor();
    }

    void ApplyScissor() {
        if (!ctx) return;
        D3D11_RECT r{0, 0, vp_w, vp_h};
        if (scissor_enabled && scissor_rect.Valid()) {
            r.left = std::clamp<int>(scissor_rect.Left(), 0, vp_w);
            r.top = std::clamp<int>(scissor_rect.Top(), 0, vp_h);
            r.right = std::clamp<int>(scissor_rect.Right(), (int)r.left, vp_w);
            r.bottom = std::clamp<int>(scissor_rect.Bottom(), (int)r.top, vp_h);
        }
        ctx->RSSetScissorRects(1, &r);
    }

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override {
        if (!device || vertices.empty() || indices.empty()) return 0;
        auto* geom = new GpuGeometry();
        geom->index_count = (UINT)indices.size();

        D3D11_BUFFER_DESC vbd{};
        vbd.ByteWidth = (UINT)(vertices.size() * sizeof(Rml::Vertex));
        vbd.Usage = D3D11_USAGE_IMMUTABLE;
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vsd{vertices.data(), 0, 0};
        if (FAILED(device->CreateBuffer(&vbd, &vsd, &geom->vb))) {
            delete geom;
            return 0;
        }

        D3D11_BUFFER_DESC ibd{};
        ibd.ByteWidth = (UINT)(indices.size() * sizeof(int));
        ibd.Usage = D3D11_USAGE_IMMUTABLE;
        ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA isd{indices.data(), 0, 0};
        if (FAILED(device->CreateBuffer(&ibd, &isd, &geom->ib))) {
            geom->vb->Release();
            delete geom;
            return 0;
        }
        return reinterpret_cast<Rml::CompiledGeometryHandle>(geom);
    }

    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override {
        auto* geom = reinterpret_cast<GpuGeometry*>(geometry);
        if (!ctx || !geom || !geom->vb || !geom->ib) return;

        UpdateCB(translation, texture != 0);
        ctx->VSSetConstantBuffers(0, 1, &cb);
        ctx->PSSetConstantBuffers(0, 1, &cb);

        ID3D11ShaderResourceView* srv = nullptr;
        if (texture) {
            srv = reinterpret_cast<GpuTexture*>(texture)->srv;
        }
        ctx->PSSetShaderResources(0, 1, &srv);

        UINT stride = sizeof(Rml::Vertex);
        UINT offset = 0;
        ctx->IASetVertexBuffers(0, 1, &geom->vb, &stride, &offset);
        ctx->IASetIndexBuffer(geom->ib, DXGI_FORMAT_R32_UINT, 0);
        ctx->DrawIndexed(geom->index_count, 0, 0);
    }

    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override {
        auto* geom = reinterpret_cast<GpuGeometry*>(geometry);
        if (!geom) return;
        if (geom->vb) geom->vb->Release();
        if (geom->ib) geom->ib->Release();
        delete geom;
    }

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
            std::vector<uint8_t> rgba((size_t)w * (size_t)h * 4u, 255);
            for (int y = 0; y < h; ++y) {
                float hue = 360.0f * (1.0f - (float)y / (float)(h - 1));
                if (hue >= 360.0f) hue = 0.0f;
                float sec = hue / 60.0f;
                int idx = (int)std::floor(sec) % 6;
                float f = sec - std::floor(sec);
                uint8_t up = (uint8_t)std::clamp((int)std::round(f * 255.0f), 0, 255);
                uint8_t dn = (uint8_t)std::clamp((int)std::round((1.0f - f) * 255.0f), 0, 255);
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
                    size_t p = ((size_t)y * (size_t)w + (size_t)x) * 4u;
                    rgba[p + 0] = r; rgba[p + 1] = g; rgba[p + 2] = b; rgba[p + 3] = 255;
                }
            }
            texture_dimensions = {w, h};
            return GenerateTexture(Rml::Span<const Rml::byte>(rgba.data(), rgba.size()), {w, h});
        }
        if (source == "studio_cp_sv_white") {
            const int w = 256, h = 2;
            std::vector<uint8_t> rgba((size_t)w * (size_t)h * 4u, 0);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    uint8_t a = (uint8_t)(255 - x);
                    size_t p = ((size_t)y * (size_t)w + (size_t)x) * 4u;
                    rgba[p + 0] = a; rgba[p + 1] = a; rgba[p + 2] = a; rgba[p + 3] = a;
                }
            }
            texture_dimensions = {w, h};
            return GenerateTexture(Rml::Span<const Rml::byte>(rgba.data(), rgba.size()), {w, h});
        }
        if (source == "studio_cp_sv_black") {
            const int w = 2, h = 256;
            std::vector<uint8_t> rgba((size_t)w * (size_t)h * 4u, 0);
            for (int y = 0; y < h; ++y) {
                uint8_t a = (uint8_t)y;
                for (int x = 0; x < w; ++x) {
                    size_t p = ((size_t)y * (size_t)w + (size_t)x) * 4u;
                    rgba[p + 0] = 0; rgba[p + 1] = 0; rgba[p + 2] = 0; rgba[p + 3] = a;
                }
            }
            texture_dimensions = {w, h};
            return GenerateTexture(Rml::Span<const Rml::byte>(rgba.data(), rgba.size()), {w, h});
        }
        if (source == "studio_cp_checker") {
            const int w = 16, h = 16;
            std::vector<uint8_t> rgba((size_t)w * (size_t)h * 4u, 255);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    bool light = (((x / 4) + (y / 4)) & 1) == 0;
                    uint8_t c = light ? 96 : 56;
                    size_t p = ((size_t)y * (size_t)w + (size_t)x) * 4u;
                    rgba[p + 0] = c; rgba[p + 1] = c; rgba[p + 2] = c; rgba[p + 3] = 255;
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
        if (!decode_image_wic(raw.data(), raw.size(), w, h, rgba)) return 0;
        texture_dimensions = {w, h};
        return GenerateTexture(Rml::Span<const Rml::byte>(rgba.data(), rgba.size()), {w, h});
    }

    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override {
        if (!device || source_dimensions.x <= 0 || source_dimensions.y <= 0) return 0;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)source_dimensions.x;
        td.Height = (UINT)source_dimensions.y;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA sd{source.data(), (UINT)(source_dimensions.x * 4), 0};
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(device->CreateTexture2D(&td, &sd, &tex)) || !tex) return 0;

        ID3D11ShaderResourceView* srv = nullptr;
        if (FAILED(device->CreateShaderResourceView(tex, nullptr, &srv)) || !srv) {
            tex->Release();
            return 0;
        }

        auto* gt = new GpuTexture();
        gt->tex = tex;
        gt->srv = srv;
        gt->width = source_dimensions.x;
        gt->height = source_dimensions.y;
        return reinterpret_cast<Rml::TextureHandle>(gt);
    }

    void ReleaseTexture(Rml::TextureHandle texture) override {
        auto* gt = reinterpret_cast<GpuTexture*>(texture);
        if (!gt) return;
        if (gt->srv) gt->srv->Release();
        if (gt->tex) gt->tex->Release();
        delete gt;
    }

    void EnableScissorRegion(bool enable) override {
        scissor_enabled = enable;
        ApplyScissor();
    }

    void SetScissorRegion(Rml::Rectanglei region) override {
        scissor_rect = region;
        ApplyScissor();
    }

    void SetTransform(const Rml::Matrix4f* t) override {
        if (t) {
            has_transform = true;
            transform = *t;
        } else {
            has_transform = false;
            transform = Rml::Matrix4f::Identity();
        }
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
                    if (auto h = inner_->GetFontFaceHandle("consolas", style, weight, size)) return h;
                } else if (cand == "sans-serif" || cand == "system-ui") {
                    if (auto h = inner_->GetFontFaceHandle("segoe ui", style, weight, size)) return h;
                    if (auto h = inner_->GetFontFaceHandle("arial", style, weight, size)) return h;
                }
            }
        }
        const char* defaults[] = {"segoe ui", "arial", "consolas"};
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
    StudioSystemInterface sys;
    StudioFileInterface file;
    Dx11RmlRenderInterface render;
    FallbackFontEngineInterface font_fallback;

    Rml::Context* canvas_ctx = nullptr;
    Rml::Context* editor_ctx = nullptr;
    Rml::ElementDocument* canvas_doc = nullptr;
    Rml::ElementDocument* editor_doc = nullptr;

    ID3D11Texture2D* canvas_rt = nullptr;
    ID3D11RenderTargetView* canvas_rtv = nullptr;
    ID3D11ShaderResourceView* canvas_srv = nullptr;
    int canvas_w = 0;
    int canvas_h = 0;

    int editor_w = 1440;
    int editor_h = 860;
    bool initialized = false;

    void ReleaseCanvasRT() {
        if (canvas_srv) { canvas_srv->Release(); canvas_srv = nullptr; }
        if (canvas_rtv) { canvas_rtv->Release(); canvas_rtv = nullptr; }
        if (canvas_rt) { canvas_rt->Release(); canvas_rt = nullptr; }
        canvas_w = 0;
        canvas_h = 0;
    }

    bool EnsureCanvasRT(ID3D11Device* device, int w, int h) {
        if (canvas_rt && canvas_w == w && canvas_h == h) return true;
        ReleaseCanvasRT();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)w;
        td.Height = (UINT)h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &canvas_rt)) || !canvas_rt) return false;
        if (FAILED(device->CreateRenderTargetView(canvas_rt, nullptr, &canvas_rtv)) || !canvas_rtv) {
            ReleaseCanvasRT();
            return false;
        }
        if (FAILED(device->CreateShaderResourceView(canvas_rt, nullptr, &canvas_srv)) || !canvas_srv) {
            ReleaseCanvasRT();
            return false;
        }
        canvas_w = w;
        canvas_h = h;
        return true;
    }

    void RenderCanvasToTexture(ID3D11Device* device, ID3D11DeviceContext* ctx, int w, int h) {
        if (!EnsureCanvasRT(device, w, h)) return;

        ID3D11RenderTargetView* prev_rtv = nullptr;
        ID3D11DepthStencilView* prev_dsv = nullptr;
        ctx->OMGetRenderTargets(1, &prev_rtv, &prev_dsv);

        const float clear_col[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        ctx->OMSetRenderTargets(1, &canvas_rtv, nullptr);
        ctx->ClearRenderTargetView(canvas_rtv, clear_col);

        render.SetViewport(w, h);
        render.BeginPass();
        canvas_ctx->Render();

        ctx->OMSetRenderTargets(1, &prev_rtv, prev_dsv);
        if (prev_rtv) prev_rtv->Release();
        if (prev_dsv) prev_dsv->Release();
    }
};

RmlEngineHost::RmlEngineHost() : impl_(new Impl()) {}
RmlEngineHost::~RmlEngineHost() {
    Shutdown();
    delete impl_;
    impl_ = nullptr;
}

bool RmlEngineHost::Init(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    if (impl_->initialized) return true;
    impl_->render.file_iface = &impl_->file;
    if (!impl_->render.Init(device, ctx)) return false;

    Rml::SetSystemInterface(&impl_->sys);
    Rml::SetFileInterface(&impl_->file);
    Rml::SetRenderInterface(&impl_->render);
    if (!Rml::Initialise()) {
        impl_->render.Shutdown();
        return false;
    }
    impl_->font_fallback.SetInner(Rml::GetFontEngineInterface());
    Rml::SetFontEngineInterface(&impl_->font_fallback);

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

    impl_->canvas_ctx = Rml::CreateContext("rmlui_studio_canvas", Rml::Vector2i(1920, 1080), &impl_->render);
    impl_->editor_ctx = Rml::CreateContext("rmlui_studio_editor", Rml::Vector2i(1440, 860), &impl_->render);
    if (!impl_->canvas_ctx || !impl_->editor_ctx) {
        if (impl_->font_fallback.Inner()) Rml::SetFontEngineInterface(impl_->font_fallback.Inner());
        Rml::Shutdown();
        impl_->render.Shutdown();
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
        impl_->render.Shutdown();
        impl_->initialized = false;
    }
}

void RmlEngineHost::SyncAndRenderCanvas(const RmlDocument& doc, ID3D11Device* device, ID3D11DeviceContext* ctx, bool force) {
    if (!impl_->initialized || !impl_->canvas_ctx) return;
    if (!force && doc.revision == last_revision_ && impl_->canvas_srv) return;
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
    impl_->RenderCanvasToTexture(device, ctx, w, h);
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

void RmlEngineHost::FlushCanvasRender(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    if (!impl_->initialized || !impl_->canvas_ctx) return;
    impl_->canvas_ctx->Update();
    auto dims = impl_->canvas_ctx->GetDimensions();
    impl_->RenderCanvasToTexture(device, ctx, std::max(64, dims.x), std::max(64, dims.y));
}

bool RmlEngineHost::UpdateCanvasNodeBox(int node_id, float x, float y, float w, float h,
                                        bool use_pct, float screen_w, float screen_h,
                                        ID3D11Device* device, ID3D11DeviceContext* ctx) {
    if (!SetCanvasNodeBoxProps(node_id, x, y, w, h, use_pct, screen_w, screen_h)) return false;
    FlushCanvasRender(device, ctx);
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

void RmlEngineHost::RenderCanvasInteractive(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    if (!impl_->initialized || !impl_->canvas_ctx) return;
    impl_->canvas_ctx->Update();
    auto dims = impl_->canvas_ctx->GetDimensions();
    impl_->RenderCanvasToTexture(device, ctx, std::max(64, dims.x), std::max(64, dims.y));
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
ID3D11ShaderResourceView* RmlEngineHost::CanvasSRV() const {
    return impl_ ? impl_->canvas_srv : nullptr;
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

void RmlEngineHost::RenderEditor(ID3D11Device* /*device*/, ID3D11DeviceContext* /*ctx*/) {
    if (!impl_->initialized || !impl_->editor_ctx) return;
    impl_->editor_ctx->Update();
    impl_->render.SetViewport(impl_->editor_w, impl_->editor_h);
    impl_->render.BeginPass();
    impl_->editor_ctx->Render();
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
        int prev_xml_errs = impl_->sys.xml_error_count;
        Rml::ElementDocument* parsed = impl_->canvas_ctx->LoadDocumentFromMemory(load_rml);
        if (!parsed || impl_->sys.xml_error_count > prev_xml_errs) {
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
