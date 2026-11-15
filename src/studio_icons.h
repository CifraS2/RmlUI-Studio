#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Renders an original anti-aliased 32x32 premultiplied-RGBA white vector icon
// (tinted in RmlUi via `image-color`). Returns true if `name` is known.
bool RenderStudioIcon32(const std::string& name, int& out_w, int& out_h, std::vector<uint8_t>& out_premul_rgba);
