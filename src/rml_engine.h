#pragma once

#include "rml_doc.h"

#include <cstdint>
#include <string>
#include <vector>

struct GLFWwindow;

namespace Rml {
class Context;
class ElementDocument;
}

struct RmlEngineLogEntry {
    bool is_error = false;
    std::string message;
};

class RmlEngineHost {
public:
    RmlEngineHost();
    ~RmlEngineHost();

    RmlEngineHost(const RmlEngineHost&) = delete;
    RmlEngineHost& operator=(const RmlEngineHost&) = delete;

    bool Init(GLFWwindow* window);
    void Shutdown();

    // User Canvas Context (renders on GPU into an OpenGL texture via FBO)
    void SyncAndRenderCanvas(const RmlDocument& doc, bool force = false);
    bool SetCanvasNodeBoxProps(const RmlNode& node, float screen_w, float screen_h);
    void FlushCanvasRender();
    bool GetCanvasElementRect(int node_id, float& out_x, float& out_y, float& out_w, float& out_h) const;
    bool GetCanvasElementOffsetParentOrigin(int node_id, float& out_x, float& out_y) const;
    bool GetCanvasElementPaddingOrigin(int node_id, float& out_x, float& out_y) const;
    int PickNodeAtCanvasPoint(float x, float y) const;
    void RenderCanvasInteractive();
    void MarkCanvasInteractiveDirty();
    void CanvasMouseMove(int x, int y);
    void CanvasMouseButton(int button, bool down);
    void CanvasMouseWheel(float delta);
    void CanvasTextInput(uint32_t codepoint);
    unsigned int CanvasTexture() const;
    uint64_t CanvasTextureVersion() const;

    // Studio Editor Chrome Context (renders directly on GPU onto active backbuffer)
    Rml::ElementDocument* LoadEditorDocument(const std::string& rml, const std::string& rcss);
    void ResizeEditor(int w, int h);
    void SetEditorDpiScale(float scale);
    void RenderEditor();
    Rml::Context* EditorContext();
    Rml::Context* CanvasContext();
    Rml::ElementDocument* CanvasDocument() const;
    const std::vector<RmlEngineLogEntry>& LastCanvasLogs() const;
    uint64_t LastCanvasLogSeq() const;
    bool HasCanvasAsset(const std::string& path) const;

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
