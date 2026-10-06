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

namespace psp::gpu {

enum class TextureScaleMode : std::uint32_t {
    // Catmull-Rom bicubic. The right choice for gradients, skies, lighting
    // ramps and anything painted rather than drawn texel by texel.
    Smooth = 0u,
    // Bicubic blended back towards nearest wherever the neighbourhood has
    // strong local contrast. Smooth areas stay smooth; hard edges, text and
    // pixel art keep their edges instead of being softened into a gradient.
    //
    // A texture drawn from a handful of colours is enlarged as Pixel instead:
    // the blend is a compromise, and on lettering a compromise still rounds the
    // corners off. Measured on the game's own menu text, this mode alone turns
    // six alpha levels into sixty-six.
    Sharp = 1u,
    // No interpolation at all: every output texel is the source texel it came
    // from. The original art at a larger size -- identical texels, perfectly
    // hard edges. Nothing is invented, so nothing can be invented wrongly; the
    // cost is that a diagonal stays as blocky as it was, only bigger.
    Pixel = 2u,
    // Hard edges without the staircase.
    //
    // Flat areas and edges that run along a row or a column come out exactly as
    // Pixel leaves them -- untouched, full contrast. What differs is a diagonal.
    // Where two texels meet corner to corner, the boundary between them really
    // is a diagonal line, and drawing it as a staircase of squares is an
    // artefact of the grid rather than anything the artist drew. This finds
    // those corners and fills them across a band one output pixel wide, so the
    // diagonal reads as a line instead of steps.
    //
    // It invents no detail and no intermediate colours beyond that band: every
    // colour in the result is one of the two the corner was already made of.
    // That is the difference from a resampler, which averages a neighbourhood
    // and softens every edge whether it was diagonal or not.
    Edge = 3u,
};

// Whether a texture is drawn from few enough colours to be hand-drawn rather
// than photographic. Cheap: it gives up as soon as the count is exceeded, which
// for a photograph is within the first few texels.
[[nodiscard]] bool looks_like_pixel_art(const std::vector<std::uint32_t> &pixels, std::uint32_t width,
                                        std::uint32_t height);

// Above this a texture is treated as photographic. Measured over the Ac!d
// archives: interface art has a median of one distinct colour and 90% of it has
// ten or fewer, while 3D art sits at sixteen, which is CLUT4's whole palette.
inline constexpr std::size_t kPixelArtColours = 12u;

// Scales `pixels` (RGBA8888, one word per texel, red in the low byte) by
// `factor` in both axes, rewriting `pixels`, `width` and `height`. A factor of
// 1 or less does nothing. Returns false and leaves everything untouched when
// the texture is unsuitable -- degenerate, or large enough that scaling it
// would cost more memory than it is worth.
bool scale_texture(std::vector<std::uint32_t> &pixels, std::uint32_t &width, std::uint32_t &height,
                   std::uint32_t factor, TextureScaleMode mode);

// The largest edge a scaled texture may reach. Past this the result costs more
// in upload bandwidth and cache pressure than it returns in sharpness.
//
// Measured across all 3,562 texture slots in the game's stages: at 4x, a limit
// of 2048 refuses exactly two of them, both the 544x80 title graphics, which
// come to 2176x320. Everything else already fits. Raising the limit to cover
// them costs nothing, because what actually bounds the memory is area rather
// than edge -- the largest texture either way is a 512x512 enlarged to
// 2048x2048, at 16 MB, and a wide short texture like 2176x320 is 2.8 MB. A
// title screen where every element is enlarged except its two largest pieces
// is the worse outcome.
//
// It stays well inside the 4096 that Vulkan guarantees on any device that can
// run this at all.
inline constexpr std::uint32_t kMaxScaledEdge = 2304u;

} // namespace psp::gpu
