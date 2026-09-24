#include "common/texture_scale.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace psp::gpu {
namespace {

struct Rgba {
    float r{}, g{}, b{}, a{};
};

[[nodiscard]] Rgba unpack(std::uint32_t texel) {
    return {static_cast<float>(texel & 0xFFu), static_cast<float>((texel >> 8u) & 0xFFu),
            static_cast<float>((texel >> 16u) & 0xFFu), static_cast<float>((texel >> 24u) & 0xFFu)};
}

[[nodiscard]] std::uint32_t pack(const Rgba &c) {
    const auto clamp = [](float v) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 255.0f)));
    };
    return clamp(c.r) | (clamp(c.g) << 8u) | (clamp(c.b) << 16u) | (clamp(c.a) << 24u);
}

// Textures repeat far more often than they clamp, but a texture that does
// clamp would pick up the opposite edge's colour if this wrapped. Clamping is
// the safer of the two mistakes: at worst an edge texel is duplicated.
[[nodiscard]] std::uint32_t at(const std::vector<std::uint32_t> &pixels, std::uint32_t width, std::uint32_t height,
                               std::int32_t x, std::int32_t y) {
    const auto cx = static_cast<std::uint32_t>(std::clamp(x, 0, static_cast<std::int32_t>(width) - 1));
    const auto cy = static_cast<std::uint32_t>(std::clamp(y, 0, static_cast<std::int32_t>(height) - 1));
    return pixels[static_cast<std::size_t>(cy) * width + cx];
}

// Catmull-Rom: interpolating (it passes through the source texels rather than
// approximating them) and cheap, with a mild overshoot at edges that reads as
// sharpening rather than ringing at these scale factors.
[[nodiscard]] std::array<float, 4> catmull_rom_weights(float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return {0.5f * (-t3 + 2.0f * t2 - t), 0.5f * (3.0f * t3 - 5.0f * t2 + 2.0f),
            0.5f * (-3.0f * t3 + 4.0f * t2 + t), 0.5f * (t3 - t2)};
}

[[nodiscard]] Rgba sample_bicubic(const std::vector<std::uint32_t> &pixels, std::uint32_t width, std::uint32_t height,
                                  float sx, float sy) {
    const auto x0 = static_cast<std::int32_t>(std::floor(sx));
    const auto y0 = static_cast<std::int32_t>(std::floor(sy));
    const std::array<float, 4> wx = catmull_rom_weights(sx - static_cast<float>(x0));
    const std::array<float, 4> wy = catmull_rom_weights(sy - static_cast<float>(y0));

    Rgba out{};
    for (std::int32_t j = 0; j < 4; ++j) {
        Rgba row{};
        for (std::int32_t i = 0; i < 4; ++i) {
            const Rgba c = unpack(at(pixels, width, height, x0 + i - 1, y0 + j - 1));
            row.r += c.r * wx[static_cast<std::size_t>(i)];
            row.g += c.g * wx[static_cast<std::size_t>(i)];
            row.b += c.b * wx[static_cast<std::size_t>(i)];
            row.a += c.a * wx[static_cast<std::size_t>(i)];
        }
        const float w = wy[static_cast<std::size_t>(j)];
        out.r += row.r * w;
        out.g += row.g * w;
        out.b += row.b * w;
        out.a += row.a * w;
    }
    return out;
}

// How much the four texels around this point disagree, as a fraction of full
// range. Flat neighbourhoods score near zero; a hard edge between two solid
// colours scores near one.
//
// The spread is measured per channel and the widest one wins, rather than
// through luma. Saturated red against saturated blue is about as hard an edge
// as a texture can have, but their lumas differ by only a seventh of the
// range, so a luma test reads it as nearly flat and leaves it blurred.
[[nodiscard]] float local_contrast(const std::vector<std::uint32_t> &pixels, std::uint32_t width,
                                   std::uint32_t height, std::int32_t x0, std::int32_t y0) {
    std::array<float, 4> low{255.0f, 255.0f, 255.0f, 255.0f};
    std::array<float, 4> high{0.0f, 0.0f, 0.0f, 0.0f};
    for (std::int32_t j = 0; j < 2; ++j) {
        for (std::int32_t i = 0; i < 2; ++i) {
            const Rgba c = unpack(at(pixels, width, height, x0 + i, y0 + j));
            const std::array<float, 4> channels{c.r, c.g, c.b, c.a};
            for (std::size_t k = 0; k < channels.size(); ++k) {
                low[k] = std::min(low[k], channels[k]);
                high[k] = std::max(high[k], channels[k]);
            }
        }
    }
    float spread = 0.0f;
    for (std::size_t k = 0; k < low.size(); ++k) spread = std::max(spread, high[k] - low[k]);
    return spread / 255.0f;
}

} // namespace

bool scale_texture(std::vector<std::uint32_t> &pixels, std::uint32_t &width, std::uint32_t &height,
                   std::uint32_t factor, TextureScaleMode mode) {
    if (factor <= 1u || width == 0u || height == 0u) return false;
    if (pixels.size() < static_cast<std::size_t>(width) * height) return false;
    if (width * factor > kMaxScaledEdge || height * factor > kMaxScaledEdge) return false;

    const std::uint32_t out_width = width * factor;
    const std::uint32_t out_height = height * factor;
    std::vector<std::uint32_t> out(static_cast<std::size_t>(out_width) * out_height);

    const float step = 1.0f / static_cast<float>(factor);
    for (std::uint32_t y = 0; y < out_height; ++y) {
        // Sample at texel centres, so the scaled texture lines up with the
        // original rather than drifting half a texel towards the origin.
        const float sy = (static_cast<float>(y) + 0.5f) * step - 0.5f;
        for (std::uint32_t x = 0; x < out_width; ++x) {
            const float sx = (static_cast<float>(x) + 0.5f) * step - 0.5f;
            Rgba color = sample_bicubic(pixels, width, height, sx, sy);
            if (mode == TextureScaleMode::Sharp) {
                const auto x0 = static_cast<std::int32_t>(std::floor(sx));
                const auto y0 = static_cast<std::int32_t>(std::floor(sy));
                // Below the low threshold the neighbourhood is flat and
                // bicubic is left alone; above the high one it is a hard edge
                // and the nearest texel wins outright. Between them the two
                // are mixed, so an edge does not appear or vanish abruptly
                // across a gradient.
                constexpr float kLow = 0.10f;
                constexpr float kHigh = 0.45f;
                const float contrast = local_contrast(pixels, width, height, x0, y0);
                const float sharpness = std::clamp((contrast - kLow) / (kHigh - kLow), 0.0f, 1.0f);
                if (sharpness > 0.0f) {
                    const Rgba nearest =
                        unpack(at(pixels, width, height, static_cast<std::int32_t>(std::lround(sx)),
                                  static_cast<std::int32_t>(std::lround(sy))));
                    color.r += (nearest.r - color.r) * sharpness;
                    color.g += (nearest.g - color.g) * sharpness;
                    color.b += (nearest.b - color.b) * sharpness;
                    color.a += (nearest.a - color.a) * sharpness;
                }
            }
            out[static_cast<std::size_t>(y) * out_width + x] = pack(color);
        }
    }

    pixels = std::move(out);
    width = out_width;
    height = out_height;
    return true;
}

} // namespace psp::gpu
