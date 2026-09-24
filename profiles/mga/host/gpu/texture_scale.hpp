#pragma once

// Upscales a decoded guest texture before it is uploaded, so it still holds
// up when the scene is rendered well above the PSP's 480x272. Bilinear
// filtering alone turns a 64x64 texture into mush at 4x internal resolution;
// scaling the texture itself first gives the filter something to work with.
//
// PPSSPP offers xBRZ here. xBRZ is GPL and this project is MIT, so none of it
// is used or adapted: the two scalers below are written for this port.

#include <cstdint>
#include <vector>

namespace mga::gpu {

enum class TextureScaleMode : std::uint32_t {
    // Catmull-Rom bicubic. The right choice for gradients, skies, lighting
    // ramps and anything painted rather than drawn texel by texel.
    Smooth = 0u,
    // Bicubic blended back towards nearest wherever the neighbourhood has
    // strong local contrast. Smooth areas stay smooth; hard edges, text and
    // pixel art keep their edges instead of being softened into a gradient.
    Sharp = 1u,
};

// Scales `pixels` (RGBA8888, one word per texel, red in the low byte) by
// `factor` in both axes, rewriting `pixels`, `width` and `height`. A factor of
// 1 or less does nothing. Returns false and leaves everything untouched when
// the texture is unsuitable -- degenerate, or large enough that scaling it
// would cost more memory than it is worth.
bool scale_texture(std::vector<std::uint32_t> &pixels, std::uint32_t &width, std::uint32_t &height,
                   std::uint32_t factor, TextureScaleMode mode);

// The largest edge a scaled texture may reach. Past this the result costs more
// in upload bandwidth and cache pressure than it returns in sharpness.
inline constexpr std::uint32_t kMaxScaledEdge = 2048u;

} // namespace mga::gpu
