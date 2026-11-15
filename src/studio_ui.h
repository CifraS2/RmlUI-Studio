#pragma once

#include "dx11_renderer.h"
#include "rml_doc.h"
#include "rml_engine.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>
#include <memory>
#include <string>

class StudioUI {
public:
    StudioUI();
    ~StudioUI();

    bool Init(HWND hwnd, RmlDocument& doc, RmlEngineHost& engine);
    void Shutdown();

    void OnResize(int width, int height);
    void OnMouseMove(int x, int y);
    void OnMouseButton(int button, bool down); // 0=left, 1=right, 2=middle
    void OnMouseWheel(float delta);
    void OnKey(UINT vk, bool down, WPARAM wparam, LPARAM lparam);
    void OnChar(uint32_t codepoint);

    void UpdateAndRender(Dx11Renderer2D& dx, ID3D11Device* device, ID3D11DeviceContext* ctx, int width, int height);

    void SetStatus(const std::string& msg);
    bool ImportRcssPath(const std::string& path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
