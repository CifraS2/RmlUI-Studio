#include "viewport_controller.h"

#include <algorithm>
#include <cmath>

void ViewportController::ScreenToCanvas(int mx, int my, float& cx, float& cy) const {
    cx = (static_cast<float>(mx) - pan_x) / std::max(0.01f, zoom);
    cy = (static_cast<float>(my) - pan_y) / std::max(0.01f, zoom);
}

void ViewportController::CanvasToScreen(float cx, float cy, float& sx, float& sy) const {
    sx = pan_x + cx * zoom;
    sy = pan_y + cy * zoom;
}

void ViewportController::ZoomAtPoint(float anchor_sx, float anchor_sy, float factor, float min_zoom, float max_zoom) {
    float old_zoom = zoom;
    zoom = std::clamp(zoom * factor, min_zoom, max_zoom);
    float ratio = zoom / std::max(0.001f, old_zoom);
    pan_x = anchor_sx - (anchor_sx - pan_x) * ratio;
    pan_y = anchor_sy - (anchor_sy - pan_y) * ratio;
}

void ViewportController::ResetZoom100(float left_total, float top_total) {
    zoom = 1.0f;
    pan_x = left_total + 30.0f;
    pan_y = top_total + 30.0f;
}

void ViewportController::FitToViewport(float vp_x0, float vp_y0, float vp_w, float vp_h, int doc_w, int doc_h) {
    float zx = (vp_w - 60.0f) / std::max(1.0f, static_cast<float>(doc_w));
    float zy = (vp_h - 60.0f) / std::max(1.0f, static_cast<float>(doc_h));
    zoom = std::clamp(std::min(zx, zy), 0.10f, 2.0f);
    pan_x = vp_x0 + (vp_w - static_cast<float>(doc_w) * zoom) * 0.5f;
    pan_y = vp_y0 + (vp_h - static_cast<float>(doc_h) * zoom) * 0.5f;
}

void ViewportController::BeginPan(int mx, int my) {
    panning = true;
    pan_start_mx = mx;
    pan_start_my = my;
    pan_start_x = pan_x;
    pan_start_y = pan_y;
}

bool ViewportController::UpdatePan(int mx, int my) {
    if (!panning) return false;
    pan_x = pan_start_x + static_cast<float>(mx - pan_start_mx);
    pan_y = pan_start_y + static_cast<float>(my - pan_start_my);
    return true;
}

void ViewportController::EndPan() {
    panning = false;
}

void ViewportController::RenderBlueprintGrid(
    GlRenderer2D& gl,
    float px,
    float py,
    float cw,
    float ch,
    uint8_t bg_r,
    uint8_t bg_g,
    uint8_t bg_b
) const {
    if (!show_grid) return;
    float lum = 0.299f * static_cast<float>(bg_r) + 0.587f * static_cast<float>(bg_g) + 0.114f * static_cast<float>(bg_b);
    uint8_t gr = (lum > 140.0f) ? 0 : 255;
    uint8_t ga = (lum > 140.0f) ? 24 : 11;
    float step = std::max(14.0f, 32.0f * (zoom < 0.7f ? 1.0f : zoom));
    for (float gx = px + step; gx < px + cw; gx += step) {
        gl.DrawRectFilled(gx, py, 1.0f, ch, gr, gr, gr, ga);
    }
    for (float gy = py + step; gy < py + ch; gy += step) {
        gl.DrawRectFilled(px, gy, cw, 1.0f, gr, gr, gr, ga);
    }
}
