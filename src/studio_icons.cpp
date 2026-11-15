#include "studio_icons.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr int kIconSize = 32;
constexpr float kScaleTo24 = 24.0f / static_cast<float>(kIconSize);

float Clamp01(float v) {
    return std::max(0.0f, std::min(1.0f, v));
}

float SdSegment(float px, float py, float ax, float ay, float bx, float by) {
    float pax = px - ax, pay = py - ay;
    float bax = bx - ax, bay = by - ay;
    float len2 = bax * bax + bay * bay;
    float h = (len2 > 1e-6f) ? Clamp01((pax * bax + pay * bay) / len2) : 0.0f;
    float dx = pax - bax * h;
    float dy = pay - bay * h;
    return std::sqrt(dx * dx + dy * dy);
}

float SdBox(float px, float py, float cx, float cy, float hw, float hh) {
    float dx = std::fabs(px - cx) - hw;
    float dy = std::fabs(py - cy) - hh;
    float ox = std::max(dx, 0.0f);
    float oy = std::max(dy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(dx, dy), 0.0f);
}

float SdRoundRect(float px, float py, float x0, float y0, float x1, float y1, float r) {
    float cx = (x0 + x1) * 0.5f;
    float cy = (y0 + y1) * 0.5f;
    float hw = std::max(0.0f, (x1 - x0) * 0.5f - r);
    float hh = std::max(0.0f, (y1 - y0) * 0.5f - r);
    return SdBox(px, py, cx, cy, hw, hh) - r;
}

float SdCircle(float px, float py, float cx, float cy, float r) {
    float dx = px - cx;
    float dy = py - cy;
    return std::sqrt(dx * dx + dy * dy) - r;
}

float SdArcApprox(float px, float py, float cx, float cy, float r, float a0_deg, float a1_deg) {
    // Evaluate shortest distance across piecewise arc segments (simple & exact enough for 32x32)
    constexpr int kSteps = 14;
    constexpr float kDeg2Rad = 3.14159265f / 180.0f;
    float d = 1e9f;
    for (int i = 0; i < kSteps; ++i) {
        float t0 = a0_deg + (a1_deg - a0_deg) * (static_cast<float>(i) / kSteps);
        float t1 = a0_deg + (a1_deg - a0_deg) * (static_cast<float>(i + 1) / kSteps);
        float x0 = cx + r * std::cos(t0 * kDeg2Rad);
        float y0 = cy + r * std::sin(t0 * kDeg2Rad);
        float x1 = cx + r * std::cos(t1 * kDeg2Rad);
        float y1 = cy + r * std::sin(t1 * kDeg2Rad);
        d = std::min(d, SdSegment(px, py, x0, y0, x1, y1));
    }
    return d;
}

float Stroke(float sdf, float half_w = 0.92f) {
    float edge = std::fabs(sdf) - half_w;
    return Clamp01(0.5f - edge / 0.58f);
}

float Line(float sdf, float half_w = 0.92f) {
    float edge = sdf - half_w;
    return Clamp01(0.5f - edge / 0.58f);
}

float Fill(float sdf) {
    return Clamp01(0.5f - sdf / 0.58f);
}

float SampleIcon(const std::string& name, float x, float y) {
    float a = 0.0f;
    auto add = [&](float v) { a = std::max(a, v); };

    if (name == "layers") {
        // Upper diamond
        add(Line(SdSegment(x, y, 12.0f, 3.0f, 20.5f, 7.5f)));
        add(Line(SdSegment(x, y, 20.5f, 7.5f, 12.0f, 12.0f)));
        add(Line(SdSegment(x, y, 12.0f, 12.0f, 3.5f, 7.5f)));
        add(Line(SdSegment(x, y, 3.5f, 7.5f, 12.0f, 3.0f)));
        // Middle chevron
        add(Line(SdSegment(x, y, 3.5f, 12.2f, 12.0f, 16.6f)));
        add(Line(SdSegment(x, y, 12.0f, 16.6f, 20.5f, 12.2f)));
        // Lower chevron
        add(Line(SdSegment(x, y, 3.5f, 16.8f, 12.0f, 21.0f)));
        add(Line(SdSegment(x, y, 12.0f, 21.0f, 20.5f, 16.8f)));
        return a;
    }
    if (name == "chevron_down") {
        add(Line(SdSegment(x, y, 6.0f, 9.0f, 12.0f, 15.0f), 1.05f));
        add(Line(SdSegment(x, y, 12.0f, 15.0f, 18.0f, 9.0f), 1.05f));
        return a;
    }
    if (name == "chevron_right") {
        add(Line(SdSegment(x, y, 9.0f, 6.0f, 15.0f, 12.0f), 1.05f));
        add(Line(SdSegment(x, y, 15.0f, 12.0f, 9.0f, 18.0f), 1.05f));
        return a;
    }
    if (name == "plus") {
        add(Line(SdSegment(x, y, 12.0f, 5.0f, 12.0f, 19.0f), 1.0f));
        add(Line(SdSegment(x, y, 5.0f, 12.0f, 19.0f, 12.0f), 1.0f));
        return a;
    }
    if (name == "minus") {
        add(Line(SdSegment(x, y, 5.0f, 12.0f, 19.0f, 12.0f), 1.05f));
        return a;
    }
    if (name == "close") {
        add(Line(SdSegment(x, y, 6.5f, 6.5f, 17.5f, 17.5f), 1.05f));
        add(Line(SdSegment(x, y, 17.5f, 6.5f, 6.5f, 17.5f), 1.05f));
        return a;
    }
    if (name == "check") {
        add(Line(SdSegment(x, y, 4.8f, 12.5f, 9.8f, 17.5f), 1.1f));
        add(Line(SdSegment(x, y, 9.8f, 17.5f, 19.5f, 7.0f), 1.1f));
        return a;
    }
    if (name == "help") {
        add(Stroke(SdCircle(x, y, 12.0f, 12.0f, 9.2f), 0.9f));
        add(Line(SdArcApprox(x, y, 12.0f, 9.4f, 2.7f, 195.0f, 385.0f), 0.88f));
        add(Line(SdSegment(x, y, 12.8f, 11.8f, 12.0f, 13.6f), 0.88f));
        add(Fill(SdCircle(x, y, 12.0f, 16.9f, 1.05f)));
        return a;
    }
    if (name == "settings") {
        // Horizontal slider tracks (Settings2 style)
        add(Line(SdSegment(x, y, 4.0f, 7.5f, 14.0f, 7.5f), 0.88f));
        add(Line(SdSegment(x, y, 19.0f, 7.5f, 20.5f, 7.5f), 0.88f));
        add(Stroke(SdCircle(x, y, 16.5f, 7.5f, 2.5f), 0.88f));
        add(Line(SdSegment(x, y, 3.5f, 16.5f, 5.0f, 16.5f), 0.88f));
        add(Line(SdSegment(x, y, 10.0f, 16.5f, 20.0f, 16.5f), 0.88f));
        add(Stroke(SdCircle(x, y, 7.5f, 16.5f, 2.5f), 0.88f));
        return a;
    }
    if (name == "upload") {
        add(Line(SdSegment(x, y, 12.0f, 4.0f, 12.0f, 15.2f), 0.95f));
        add(Line(SdSegment(x, y, 7.5f, 8.5f, 12.0f, 4.0f), 0.95f));
        add(Line(SdSegment(x, y, 12.0f, 4.0f, 16.5f, 8.5f), 0.95f));
        add(Line(SdSegment(x, y, 4.2f, 15.0f, 4.2f, 19.2f), 0.95f));
        add(Line(SdSegment(x, y, 4.2f, 19.2f, 19.8f, 19.2f), 0.95f));
        add(Line(SdSegment(x, y, 19.8f, 19.2f, 19.8f, 15.0f), 0.95f));
        return a;
    }
    if (name == "undo") {
        add(Line(SdSegment(x, y, 8.2f, 5.5f, 4.0f, 9.7f), 0.95f));
        add(Line(SdSegment(x, y, 4.0f, 9.7f, 8.2f, 13.9f), 0.95f));
        add(Line(SdSegment(x, y, 4.4f, 9.7f, 14.5f, 9.7f), 0.95f));
        add(Line(SdArcApprox(x, y, 14.5f, 14.0f, 4.3f, -90.0f, 90.0f), 0.95f));
        add(Line(SdSegment(x, y, 14.5f, 18.3f, 8.5f, 18.3f), 0.95f));
        return a;
    }
    if (name == "redo") {
        add(Line(SdSegment(x, y, 15.8f, 5.5f, 20.0f, 9.7f), 0.95f));
        add(Line(SdSegment(x, y, 20.0f, 9.7f, 15.8f, 13.9f), 0.95f));
        add(Line(SdSegment(x, y, 19.6f, 9.7f, 9.5f, 9.7f), 0.95f));
        add(Line(SdArcApprox(x, y, 9.5f, 14.0f, 4.3f, 90.0f, 270.0f), 0.95f));
        add(Line(SdSegment(x, y, 9.5f, 18.3f, 15.5f, 18.3f), 0.95f));
        return a;
    }
    if (name == "desktop") {
        add(Stroke(SdRoundRect(x, y, 3.2f, 4.2f, 20.8f, 16.5f, 1.8f), 0.9f));
        add(Line(SdSegment(x, y, 8.5f, 20.0f, 15.5f, 20.0f), 0.9f));
        add(Line(SdSegment(x, y, 12.0f, 16.6f, 12.0f, 20.0f), 0.9f));
        return a;
    }
    if (name == "mobile") {
        add(Stroke(SdRoundRect(x, y, 6.2f, 2.8f, 17.8f, 21.2f, 2.0f), 0.9f));
        add(Fill(SdCircle(x, y, 12.0f, 18.0f, 0.95f)));
        return a;
    }
    if (name == "grid") {
        add(Stroke(SdRoundRect(x, y, 3.5f, 3.5f, 20.5f, 20.5f, 2.0f), 0.85f));
        add(Line(SdSegment(x, y, 12.0f, 3.8f, 12.0f, 20.2f), 0.85f));
        add(Line(SdSegment(x, y, 3.8f, 12.0f, 20.2f, 12.0f), 0.85f));
        return a;
    }
    if (name == "play") {
        add(Line(SdSegment(x, y, 6.5f, 4.5f, 18.8f, 12.0f), 1.0f));
        add(Line(SdSegment(x, y, 18.8f, 12.0f, 6.5f, 19.5f), 1.0f));
        add(Line(SdSegment(x, y, 6.5f, 19.5f, 6.5f, 4.5f), 1.0f));
        return a;
    }
    if (name == "stop") {
        add(Stroke(SdRoundRect(x, y, 5.5f, 5.5f, 18.5f, 18.5f, 1.5f), 1.0f));
        return a;
    }
    if (name == "code") {
        // < / >
        add(Line(SdSegment(x, y, 8.2f, 7.5f, 3.5f, 12.0f), 0.92f));
        add(Line(SdSegment(x, y, 3.5f, 12.0f, 8.2f, 16.5f), 0.92f));
        add(Line(SdSegment(x, y, 15.8f, 7.5f, 20.5f, 12.0f), 0.92f));
        add(Line(SdSegment(x, y, 20.5f, 12.0f, 15.8f, 16.5f), 0.92f));
        add(Line(SdSegment(x, y, 14.0f, 5.5f, 10.0f, 18.5f), 0.92f));
        return a;
    }
    if (name == "braces") {
        // { }
        add(Line(SdSegment(x, y, 9.2f, 4.5f, 7.0f, 4.5f), 0.9f));
        add(Line(SdSegment(x, y, 7.0f, 4.5f, 7.0f, 10.2f), 0.9f));
        add(Line(SdSegment(x, y, 7.0f, 10.2f, 4.6f, 12.0f), 0.9f));
        add(Line(SdSegment(x, y, 4.6f, 12.0f, 7.0f, 13.8f), 0.9f));
        add(Line(SdSegment(x, y, 7.0f, 13.8f, 7.0f, 19.5f), 0.9f));
        add(Line(SdSegment(x, y, 7.0f, 19.5f, 9.2f, 19.5f), 0.9f));

        add(Line(SdSegment(x, y, 14.8f, 4.5f, 17.0f, 4.5f), 0.9f));
        add(Line(SdSegment(x, y, 17.0f, 4.5f, 17.0f, 10.2f), 0.9f));
        add(Line(SdSegment(x, y, 17.0f, 10.2f, 19.4f, 12.0f), 0.9f));
        add(Line(SdSegment(x, y, 19.4f, 12.0f, 17.0f, 13.8f), 0.9f));
        add(Line(SdSegment(x, y, 17.0f, 13.8f, 17.0f, 19.5f), 0.9f));
        add(Line(SdSegment(x, y, 17.0f, 19.5f, 14.8f, 19.5f), 0.9f));
        return a;
    }
    if (name == "search") {
        add(Stroke(SdCircle(x, y, 10.5f, 10.5f, 6.2f), 0.95f));
        add(Line(SdSegment(x, y, 15.2f, 15.2f, 20.2f, 20.2f), 1.0f));
        return a;
    }
    if (name == "box") {
        // Container rounded box / cube
        add(Stroke(SdRoundRect(x, y, 4.0f, 4.0f, 20.0f, 20.0f, 2.4f), 0.9f));
        add(Line(SdSegment(x, y, 4.5f, 9.0f, 19.5f, 9.0f), 0.8f));
        return a;
    }
    if (name == "align_left") {
        add(Line(SdSegment(x, y, 4.0f, 6.2f, 20.0f, 6.2f), 0.95f));
        add(Line(SdSegment(x, y, 4.0f, 12.0f, 14.5f, 12.0f), 0.95f));
        add(Line(SdSegment(x, y, 4.0f, 17.8f, 18.0f, 17.8f), 0.95f));
        return a;
    }
    if (name == "pointer") {
        add(Line(SdSegment(x, y, 5.2f, 4.0f, 9.5f, 19.0f), 0.95f));
        add(Line(SdSegment(x, y, 5.2f, 4.0f, 19.0f, 11.2f), 0.95f));
        add(Line(SdSegment(x, y, 19.0f, 11.2f, 12.6f, 13.4f), 0.95f));
        add(Line(SdSegment(x, y, 12.6f, 13.4f, 9.5f, 19.0f), 0.95f));
        add(Line(SdSegment(x, y, 13.2f, 13.8f, 18.8f, 19.4f), 0.95f));
        return a;
    }
    if (name == "hand") {
        // 4-way move / pan icon
        add(Line(SdSegment(x, y, 12.0f, 3.5f, 12.0f, 20.5f), 0.9f));
        add(Line(SdSegment(x, y, 3.5f, 12.0f, 20.5f, 12.0f), 0.9f));
        add(Line(SdSegment(x, y, 9.5f, 6.0f, 12.0f, 3.5f), 0.9f));
        add(Line(SdSegment(x, y, 14.5f, 6.0f, 12.0f, 3.5f), 0.9f));
        add(Line(SdSegment(x, y, 9.5f, 18.0f, 12.0f, 20.5f), 0.9f));
        add(Line(SdSegment(x, y, 14.5f, 18.0f, 12.0f, 20.5f), 0.9f));
        add(Line(SdSegment(x, y, 6.0f, 9.5f, 3.5f, 12.0f), 0.9f));
        add(Line(SdSegment(x, y, 6.0f, 14.5f, 3.5f, 12.0f), 0.9f));
        add(Line(SdSegment(x, y, 18.0f, 9.5f, 20.5f, 12.0f), 0.9f));
        add(Line(SdSegment(x, y, 18.0f, 14.5f, 20.5f, 12.0f), 0.9f));
        return a;
    }
    if (name == "image") {
        add(Stroke(SdRoundRect(x, y, 3.5f, 4.0f, 20.5f, 20.0f, 2.2f), 0.9f));
        add(Fill(SdCircle(x, y, 8.8f, 8.8f, 1.35f)));
        add(Line(SdSegment(x, y, 4.2f, 17.5f, 14.0f, 10.2f), 0.88f));
        add(Line(SdSegment(x, y, 14.0f, 10.2f, 20.0f, 15.8f), 0.88f));
        return a;
    }
    if (name == "input") {
        add(Stroke(SdRoundRect(x, y, 3.2f, 6.5f, 20.8f, 17.5f, 2.0f), 0.9f));
        add(Line(SdSegment(x, y, 8.0f, 9.5f, 8.0f, 14.5f), 0.9f));
        return a;
    }
    if (name == "bar") {
        add(Stroke(SdRoundRect(x, y, 3.2f, 8.2f, 20.8f, 15.8f, 2.5f), 0.9f));
        add(Fill(SdRoundRect(x, y, 5.2f, 10.2f, 14.2f, 13.8f, 1.2f)));
        return a;
    }
    if (name == "eye") {
        add(Line(SdArcApprox(x, y, 12.0f, 19.5f, 12.5f, 224.0f, 316.0f), 0.9f));
        add(Line(SdArcApprox(x, y, 12.0f, 4.5f, 12.5f, 44.0f, 136.0f), 0.9f));
        add(Stroke(SdCircle(x, y, 12.0f, 12.0f, 2.7f), 0.9f));
        return a;
    }
    if (name == "eye_off") {
        add(Line(SdArcApprox(x, y, 12.0f, 19.5f, 12.5f, 226.0f, 314.0f), 0.85f));
        add(Line(SdArcApprox(x, y, 12.0f, 4.5f, 12.5f, 46.0f, 134.0f), 0.85f));
        add(Line(SdSegment(x, y, 4.0f, 4.0f, 20.0f, 20.0f), 1.0f));
        return a;
    }
    if (name == "file_code") {
        add(Line(SdSegment(x, y, 5.0f, 3.5f, 14.2f, 3.5f), 0.85f));
        add(Line(SdSegment(x, y, 14.2f, 3.5f, 19.0f, 8.3f), 0.85f));
        add(Line(SdSegment(x, y, 19.0f, 8.3f, 19.0f, 20.5f), 0.85f));
        add(Line(SdSegment(x, y, 19.0f, 20.5f, 5.0f, 20.5f), 0.85f));
        add(Line(SdSegment(x, y, 5.0f, 20.5f, 5.0f, 3.5f), 0.85f));
        add(Line(SdSegment(x, y, 14.0f, 3.5f, 14.0f, 8.5f), 0.85f));
        add(Line(SdSegment(x, y, 14.0f, 8.5f, 19.0f, 8.5f), 0.85f));
        // < > inside document
        add(Line(SdSegment(x, y, 9.8f, 12.2f, 7.6f, 14.3f), 0.85f));
        add(Line(SdSegment(x, y, 7.6f, 14.3f, 9.8f, 16.4f), 0.85f));
        add(Line(SdSegment(x, y, 14.2f, 12.2f, 16.4f, 14.3f), 0.85f));
        add(Line(SdSegment(x, y, 16.4f, 14.3f, 14.2f, 16.4f), 0.85f));
        return a;
    }
    if (name == "copy") {
        add(Stroke(SdRoundRect(x, y, 8.2f, 8.2f, 19.5f, 19.5f, 1.8f), 0.9f));
        add(Line(SdSegment(x, y, 15.0f, 5.0f, 5.5f, 5.0f), 0.9f));
        add(Line(SdSegment(x, y, 5.5f, 5.0f, 5.5f, 15.0f), 0.9f));
        return a;
    }
    if (name == "more") {
        add(Fill(SdCircle(x, y, 6.2f, 12.0f, 1.35f)));
        add(Fill(SdCircle(x, y, 12.0f, 12.0f, 1.35f)));
        add(Fill(SdCircle(x, y, 17.8f, 12.0f, 1.35f)));
        return a;
    }
    if (name == "lock") {
        add(Stroke(SdRoundRect(x, y, 5.0f, 10.8f, 19.0f, 20.2f, 2.0f), 0.92f));
        add(Line(SdArcApprox(x, y, 12.0f, 8.2f, 3.8f, 180.0f, 360.0f), 0.92f));
        add(Line(SdSegment(x, y, 8.2f, 8.2f, 8.2f, 10.8f), 0.92f));
        add(Line(SdSegment(x, y, 15.8f, 8.2f, 15.8f, 10.8f), 0.92f));
        return a;
    }
    if (name == "al_left") {
        add(Line(SdSegment(x, y, 4.0f, 3.5f, 4.0f, 20.5f), 0.95f));
        add(Fill(SdRoundRect(x, y, 6.8f, 6.2f, 19.5f, 10.4f, 1.0f)));
        add(Fill(SdRoundRect(x, y, 6.8f, 13.6f, 14.5f, 17.8f, 1.0f)));
        return a;
    }
    if (name == "al_hcenter") {
        add(Line(SdSegment(x, y, 12.0f, 3.5f, 12.0f, 20.5f), 0.85f));
        add(Fill(SdRoundRect(x, y, 5.2f, 6.2f, 18.8f, 10.4f, 1.0f)));
        add(Fill(SdRoundRect(x, y, 7.6f, 13.6f, 16.4f, 17.8f, 1.0f)));
        return a;
    }
    if (name == "al_right") {
        add(Line(SdSegment(x, y, 20.0f, 3.5f, 20.0f, 20.5f), 0.95f));
        add(Fill(SdRoundRect(x, y, 4.5f, 6.2f, 17.2f, 10.4f, 1.0f)));
        add(Fill(SdRoundRect(x, y, 9.5f, 13.6f, 17.2f, 17.8f, 1.0f)));
        return a;
    }
    if (name == "al_top") {
        add(Line(SdSegment(x, y, 3.5f, 4.0f, 20.5f, 4.0f), 0.95f));
        add(Fill(SdRoundRect(x, y, 6.2f, 6.8f, 10.4f, 19.5f, 1.0f)));
        add(Fill(SdRoundRect(x, y, 13.6f, 6.8f, 17.8f, 14.5f, 1.0f)));
        return a;
    }
    if (name == "al_vcenter") {
        add(Line(SdSegment(x, y, 3.5f, 12.0f, 20.5f, 12.0f), 0.85f));
        add(Fill(SdRoundRect(x, y, 6.2f, 5.2f, 10.4f, 18.8f, 1.0f)));
        add(Fill(SdRoundRect(x, y, 13.6f, 7.6f, 17.8f, 16.4f, 1.0f)));
        return a;
    }
    if (name == "al_bottom") {
        add(Line(SdSegment(x, y, 3.5f, 20.0f, 20.5f, 20.0f), 0.95f));
        add(Fill(SdRoundRect(x, y, 6.2f, 4.5f, 10.4f, 17.2f, 1.0f)));
        add(Fill(SdRoundRect(x, y, 13.6f, 9.5f, 17.8f, 17.2f, 1.0f)));
        return a;
    }
    if (name == "dist_h") {
        add(Line(SdSegment(x, y, 4.2f, 4.0f, 4.2f, 20.0f), 0.9f));
        add(Line(SdSegment(x, y, 19.8f, 4.0f, 19.8f, 20.0f), 0.9f));
        add(Fill(SdRoundRect(x, y, 9.8f, 6.5f, 14.2f, 17.5f, 1.0f)));
        return a;
    }
    if (name == "dist_v") {
        add(Line(SdSegment(x, y, 4.0f, 4.2f, 20.0f, 4.2f), 0.9f));
        add(Line(SdSegment(x, y, 4.0f, 19.8f, 20.0f, 19.8f), 0.9f));
        add(Fill(SdRoundRect(x, y, 6.5f, 9.8f, 17.5f, 14.2f, 1.0f)));
        return a;
    }
    if (name == "magnet") {
        add(Line(SdArcApprox(x, y, 12.0f, 10.5f, 6.2f, 180.0f, 360.0f), 1.0f));
        add(Line(SdSegment(x, y, 5.8f, 10.5f, 5.8f, 18.8f), 1.0f));
        add(Line(SdSegment(x, y, 18.2f, 10.5f, 18.2f, 18.8f), 1.0f));
        add(Line(SdSegment(x, y, 4.8f, 15.8f, 6.8f, 15.8f), 0.9f));
        add(Line(SdSegment(x, y, 17.2f, 15.8f, 19.2f, 15.8f), 0.9f));
        return a;
    }
    return -1.0f;
}

} // namespace

bool RenderStudioIcon32(const std::string& name, int& out_w, int& out_h, std::vector<uint8_t>& out_premul_rgba) {
    if (SampleIcon(name, 12.0f, 12.0f) < 0.0f) {
        return false;
    }
    out_w = kIconSize;
    out_h = kIconSize;
    out_premul_rgba.assign(static_cast<size_t>(kIconSize * kIconSize * 4), 0);

    // 2x2 rotated-grid supersampling per pixel for super crisp anti-aliased strokes
    constexpr float kSub[4][2] = {
        {0.25f, 0.35f},
        {0.65f, 0.25f},
        {0.35f, 0.75f},
        {0.75f, 0.65f}
    };

    for (int py = 0; py < kIconSize; ++py) {
        for (int px = 0; px < kIconSize; ++px) {
            float cov = 0.0f;
            for (int s = 0; s < 4; ++s) {
                float sx = (static_cast<float>(px) + kSub[s][0]) * kScaleTo24;
                float sy = (static_cast<float>(py) + kSub[s][1]) * kScaleTo24;
                cov += SampleIcon(name, sx, sy);
            }
            cov *= 0.25f;
            uint8_t c = static_cast<uint8_t>(std::round(Clamp01(cov) * 255.0f));
            size_t idx = static_cast<size_t>((py * kIconSize + px) * 4);
            // Premultiplied white (RGB = A)
            out_premul_rgba[idx + 0] = c;
            out_premul_rgba[idx + 1] = c;
            out_premul_rgba[idx + 2] = c;
            out_premul_rgba[idx + 3] = c;
        }
    }
    return true;
}
