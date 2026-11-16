#pragma once

#include "gl_renderer.h"

#include <string>

/// Manages 2D viewport camera pan/zoom, screen <-> artboard coordinate transforms, grid, and rulers.
class ViewportController {
public:
    float pan_x = 300.0f;
    float pan_y = 180.0f;
    float zoom = 0.40f;

    bool panning = false;
    int pan_start_mx = 0;
    int pan_start_my = 0;
    float pan_start_x = 0.0f;
    float pan_start_y = 0.0f;

    bool show_grid = true;
    bool show_rulers = true;
    bool show_left_ruler = false;

    void ScreenToCanvas(int mx, int my, float& cx, float& cy) const;
    void CanvasToScreen(float cx, float cy, float& sx, float& sy) const;

    void ZoomAtPoint(float anchor_sx, float anchor_sy, float factor, float min_zoom = 0.10f, float max_zoom = 4.0f);
    void ResetZoom100(float left_total, float top_total);
    void FitToViewport(float vp_x0, float vp_y0, float vp_w, float vp_h, int doc_w, int doc_h);

    void BeginPan(int mx, int my);
    bool UpdatePan(int mx, int my);
    void EndPan();

    void RenderBlueprintGrid(
        GlRenderer2D& gl,
        float px,
        float py,
        float cw,
        float ch,
        uint8_t bg_r,
        uint8_t bg_g,
        uint8_t bg_b
    ) const;
};
