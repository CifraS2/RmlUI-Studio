#pragma once

#include "gl_renderer.h"
#include "rml_doc.h"
#include "rml_engine.h"

#include <functional>
#include <memory>
#include <string>

struct GLFWwindow;

class StudioUI {
public:
    StudioUI();
    ~StudioUI();

    bool Init(GLFWwindow* window, RmlDocument& doc, RmlEngineHost& engine);
    void Shutdown();

    void OnResize(int width, int height);
    void OnMouseMove(int x, int y);
    void OnMouseButton(int button, bool down); // 0=left, 1=right, 2=middle
    void OnMouseWheel(float delta);
    void OnKey(int key, bool down, int mods);
    void OnChar(uint32_t codepoint);

    void UpdateAndRender(GlRenderer2D& gl, int width, int height);

    void SetStatus(const std::string& msg);
    bool ImportRcssPath(const std::string& path);
    void SetToggleFullscreenCallback(std::function<void()> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
