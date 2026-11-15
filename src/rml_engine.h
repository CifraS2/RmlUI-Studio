#pragma once

#include "rml_doc.h"

#include <d3d11.h>
#include <cstdint>
#include <string>
#include <vector>

namespace Rml {
class Context;
class ElementDocument;
}

class RmlEngineHost {
public:
    RmlEngineHost();
    ~RmlEngineHost();

    RmlEngineHost(const RmlEngineHost&) = delete;
    RmlEngineHost& operator=(const RmlEngineHost&) = delete;

    bool Init(ID3D11Device* device, ID3D11DeviceContext* ctx);
    void Shutdown();

    // User Canvas Context (renders on GPU into a DX11 RenderTarget texture)
    void SyncAndRenderCanvas(const RmlDocument& doc, ID3D11Device* device, ID3D11DeviceContext* ctx, bool force = false);
    bool SetCanvasNodeBoxProps(int node_id, float x, float y, float w, float h, bool use_pct, float screen_w, float screen_h);
    void FlushCanvasRender(ID3D11Device* device, ID3D11DeviceContext* ctx);
    bool UpdateCanvasNodeBox(int node_id, float x, float y, float w, float h, bool use_pct, float screen_w, float screen_h, ID3D11Device* device, ID3D11DeviceContext* ctx);
    bool GetCanvasElementRect(int node_id, float& out_x, float& out_y, float& out_w, float& out_h) const;
    bool GetCanvasElementOffsetParentOrigin(int node_id, float& out_x, float& out_y) const;
    int PickNodeAtCanvasPoint(float x, float y) const;
    void RenderCanvasInteractive(ID3D11Device* device, ID3D11DeviceContext* ctx);
    void CanvasMouseMove(int x, int y);
    void CanvasMouseButton(int button, bool down);
    void CanvasMouseWheel(float delta);
    void CanvasTextInput(uint32_t codepoint);
    ID3D11ShaderResourceView* CanvasSRV() const;

    // Studio Editor Chrome Context (renders directly on GPU onto active backbuffer)
    Rml::ElementDocument* LoadEditorDocument(const std::string& rml, const std::string& rcss);
    void ResizeEditor(int w, int h);
    void RenderEditor(ID3D11Device* device, ID3D11DeviceContext* ctx);
    Rml::Context* EditorContext();
    Rml::Context* CanvasContext();

    // Import an existing .rml (+ linked .rcss and images) using the real RmlUi DOM
    bool ImportRmlFile(const std::string& rml_path, RmlDocument& out_doc, std::string& out_error);
    bool ImportRcssAndSync(const std::string& rcss_path, RmlDocument& doc, std::string& out_error);

    // Live-apply edited .rml / .rcss code strings into the active document & canvas DOM
    bool ApplyRmlStringAndSync(const std::string& raw_rml, RmlDocument& doc, std::string& out_error);
    bool ApplyRcssStringAndSync(const std::string& raw_rcss, RmlDocument& doc, std::string& out_error);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    uint64_t last_revision_ = 0;
};
