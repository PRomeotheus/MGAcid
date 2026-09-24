#include "vulkan_renderer.hpp"

#include "shadow_map.hpp"
#include "texture_decode.hpp"
#include "texture_pack.hpp"
#include "texture_scale.hpp"

#include "install/user_data.hpp"
#include "perf/frame_stats.hpp"
#include "perf/perf_overlay.hpp"
#include "settings/settings.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include "backends/imgui_impl_vulkan.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mga::gpu {
namespace {

constexpr std::uint32_t kPspWidth = 480u;
constexpr std::uint32_t kPspHeight = 272u;
constexpr VkDeviceSize kVertexBufferBytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaxCachedTextures = 1024u;
// Descriptor sets for sampling render targets as textures: two per target
// (with its alpha, and with alpha forced to one for 5650 textures).
constexpr std::size_t kMaxFramebufferTextureSets = 64u;

// Compiled SPIR-V, generated from host/gpu/shaders by the build.
#include "ge_shaders.inc"

struct PushConstants {
    std::array<float, 16> transform{};
    std::array<float, 4> viewport{};        // x,y: target size; z: through flag; w: 1 fog + 2 lighting
    std::array<float, 4> texture_params{};  // x: enabled, y: function | 8 when doubled, z: alpha ref, w: alpha func
    std::array<float, 4> uv_transform{1.0f, 1.0f, 0.0f, 0.0f};
    std::array<float, 4> view_z{};          // row of view * world that gives view-space z, for fog
};

// 128 bytes is the most every Vulkan implementation has to accept.
static_assert(sizeof(PushConstants) == 128u, "PushConstants must fit the guaranteed push constant size");
constexpr float kPushFog = 1.0f;
constexpr float kPushLighting = 2.0f;

// What the post-processing pass needs: where the game rectangle sits inside
// the window, the scene target's texel size for neighbour taps, and which
// effects are switched on. Matches the push block in shaders/post.frag.
struct PostPush {
    std::array<float, 4> rect{};
    std::array<float, 2> texel{};
    std::array<float, 2> effects{};
    // x: +1 when a larger depth value is nearer, -1 otherwise; y: the tap
    // radius in target pixels; z: the depth an untouched pixel holds;
    // w: contact shadow strength, 0 for off.
    std::array<float, 4> depth{};
};
static_assert(sizeof(PostPush) == 48u, "PostPush must match the post shader's push block");

// Descriptor sets the post pass needs: one per render target it samples, and
// there are only ever a handful of targets.
constexpr std::size_t kMaxPostSets = 8u;

struct GpuVertex {
    float x{}, y{}, z{}, w{1.0f};
    float u{}, v{};
    std::uint32_t color{};
    float nx{}, ny{}, nz{};
};

constexpr float kNoClamp = 1e30f;

// True when a through-mode draw maps one texel to one PSP pixel -- the HUD,
// the cards, text, anything authored to sit on the screen exactly as drawn.
// Filtering those smoothly is what makes a 2D interface look soft once the
// internal resolution is raised, so they are sampled sharp instead.
//
// Through-mode positions are already in PSP pixels and texture coordinates in
// texels, so the test is just whether the two spans agree.
[[nodiscard]] bool pixel_mapped_2d(const std::vector<GpuVertex> &vertices) {
    if (vertices.size() < 2u) return false;
    float min_x = vertices[0].x, max_x = vertices[0].x;
    float min_y = vertices[0].y, max_y = vertices[0].y;
    float min_u = vertices[0].u, max_u = vertices[0].u;
    float min_v = vertices[0].v, max_v = vertices[0].v;
    for (const GpuVertex &v : vertices) {
        min_x = std::min(min_x, v.x); max_x = std::max(max_x, v.x);
        min_y = std::min(min_y, v.y); max_y = std::max(max_y, v.y);
        min_u = std::min(min_u, v.u); max_u = std::max(max_u, v.u);
        min_v = std::min(min_v, v.v); max_v = std::max(max_v, v.v);
    }
    const float du = max_u - min_u;
    const float dv = max_v - min_v;
    // A degenerate span says nothing; leave those to ordinary filtering.
    if (du < 1.0f || dv < 1.0f) return false;
    constexpr float kTolerance = 0.02f;
    return std::abs((max_x - min_x) / du - 1.0f) <= kTolerance &&
           std::abs((max_y - min_y) / dv - 1.0f) <= kTolerance;
}

void set_uv_rect(GpuVertex &vertex, float u_min, float v_min, float u_max, float v_max) {
    vertex.nx = u_min;
    vertex.ny = v_min;
    vertex.nz = u_max;
    vertex.w = v_max;
}

// A through-mode tile is an axis-aligned rectangle of pixels showing an
// axis-aligned rectangle of texels, usually a piece of an atlas. At 480x272
// the raster samples the texels only between the centres of its edge pixels;
// at a higher internal resolution it also samples between those centres and
// the edges, where bilinear filtering pulls in the neighbouring atlas texels
// (or the far side of the texture) and leaves a faint grid along the tile
// edges. The six vertices from `first` get the texel range the 480x272
// raster samples, which the fragment shader clamps to. That range contains
// every sample a 480x272 target takes, so at x1 nothing changes.
void clamp_through_quad(std::vector<GpuVertex> &vertices, std::size_t first, float texture_width,
                        float texture_height) {
    if (first + 6u > vertices.size()) return;
    float x0 = vertices[first].x, x1 = x0, y0 = vertices[first].y, y1 = y0;
    for (std::size_t i = first; i < first + 6u; ++i) {
        x0 = std::min(x0, vertices[i].x);
        x1 = std::max(x1, vertices[i].x);
        y0 = std::min(y0, vertices[i].y);
        y1 = std::max(y1, vertices[i].y);
    }
    if (x1 - x0 < 1.0f || y1 - y0 < 1.0f) return;
    // Texture coordinates must follow x and y alone: one u at each vertical
    // edge, one v at each horizontal edge.
    float u_at_x0 = 0.0f, u_at_x1 = 0.0f, v_at_y0 = 0.0f, v_at_y1 = 0.0f;
    bool seen[4]{};
    for (std::size_t i = first; i < first + 6u; ++i) {
        const GpuVertex &vertex = vertices[i];
        const bool left = vertex.x == x0, top = vertex.y == y0;
        if (!left && vertex.x != x1) return;
        if (!top && vertex.y != y1) return;
        float &u = left ? u_at_x0 : u_at_x1;
        float &v = top ? v_at_y0 : v_at_y1;
        bool &u_seen = seen[left ? 0 : 1];
        bool &v_seen = seen[top ? 2 : 3];
        if (u_seen && u != vertex.u) return;
        if (v_seen && v != vertex.v) return;
        u = vertex.u;
        v = vertex.v;
        u_seen = v_seen = true;
    }
    const float u_min = std::min(u_at_x0, u_at_x1), u_max = std::max(u_at_x0, u_at_x1);
    const float v_min = std::min(v_at_y0, v_at_y1), v_max = std::max(v_at_y0, v_at_y1);
    // A range beyond the texture repeats it on purpose.
    if (u_min < 0.0f || v_min < 0.0f || u_max > texture_width || v_max > texture_height) return;
    // Half a pixel, in texels.
    const float inset_u = 0.5f * (u_max - u_min) / (x1 - x0);
    const float inset_v = 0.5f * (v_max - v_min) / (y1 - y0);
    for (std::size_t i = first; i < first + 6u; ++i)
        set_uv_rect(vertices[i], u_min + inset_u, v_min + inset_v, u_max - inset_u, v_max - inset_v);
}

// Lighting reaches the shaders in two std140 uniform blocks that live in the
// vertex buffer and are bound through dynamic offsets (set 1).
//
// The environment is what the game changes a few times a frame: the global
// ambient light, the four lights and the fog parameters. It is written once per
// change of GeState's environment version and shared by every draw after it.
struct EnvironmentBlock {
    std::array<float, 4> ambient{};
    std::array<float, 4> fog{};
    std::array<float, 4> fog_color{};
    std::array<std::array<float, 4>, 4> light_position{};
    std::array<std::array<float, 4>, 4> light_direction{};
    std::array<std::array<float, 4>, 4> light_attenuation{};
    std::array<std::array<float, 4>, 4> light_spot{};
    std::array<std::array<float, 4>, 4> light_ambient{};
    std::array<std::array<float, 4>, 4> light_diffuse{};
    std::array<std::array<float, 4>, 4> light_specular{};
    // Draw space to the shadow light's clip space, then x: strength (0 off),
    // y: which light casts, z: one shadow map texel, w: depth bias.
    std::array<float, 16> shadow_transform{};
    std::array<float, 4> shadow_params{};
};

static_assert(sizeof(EnvironmentBlock) == 576u, "EnvironmentBlock must match the std140 layout in ge.vert");

// What a lit draw adds: its world matrix and material. Consecutive draws of
// one mesh share it, so it is written only when it differs from the last one.
struct ObjectBlock {
    std::array<float, 16> world{};
    std::array<float, 4> flags{};             // y: vertex colour, w: material update mask
    std::array<float, 4> emissive{};          // w: specular power
    std::array<float, 4> material_ambient{};
    std::array<float, 4> material_diffuse{};  // w: separate specular
    std::array<float, 4> material_specular{}; // w: reverse normals
};

static_assert(sizeof(ObjectBlock) == 144u, "ObjectBlock must match the std140 layout in ge.vert");

// A GE colour register (0x00BBGGRR) as 0..1 floats, with an explicit alpha.
std::array<float, 4> unpack_color(std::uint32_t color, float alpha = 1.0f) {
    return {static_cast<float>(color & 0xFFu) / 255.0f, static_cast<float>((color >> 8u) & 0xFFu) / 255.0f,
            static_cast<float>((color >> 16u) & 0xFFu) / 255.0f, alpha};
}

// Pipeline variants the GE state can produce.
struct PipelineKey {
    bool blend{};
    std::uint32_t source_factor{};
    std::uint32_t destination_factor{};
    std::uint32_t equation{};
    bool depth_test{};
    bool depth_write{};
    std::uint32_t depth_function{};
    bool cull{};
    bool cull_clockwise{};
    std::uint32_t color_mask{VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                             VK_COLOR_COMPONENT_A_BIT};

    auto operator<=>(const PipelineKey &) const = default;
};

// GU_FIX takes its factor from a colour register rather than from the source or
// destination pixel. Vulkan offers a single blend constant per attachment, so a
// fixed factor collapses to ONE or ZERO when the colour is white or black, and
// only the remaining cases need the constant itself.
constexpr std::uint32_t kFactorFixed = 10u;
constexpr std::uint32_t kFactorOne = 16u;
constexpr std::uint32_t kFactorZero = 17u;
constexpr std::uint32_t kFactorInverseConstant = 18u;

std::uint32_t resolve_fixed_factor(std::uint32_t factor, std::uint32_t color) {
    if (factor != kFactorFixed) return factor;
    const std::uint32_t rgb = color & 0x00FFFFFFu;
    if (rgb == 0x00FFFFFFu) return kFactorOne;
    if (rgb == 0u) return kFactorZero;
    return kFactorFixed;
}

VkBlendFactor to_blend_factor(std::uint32_t factor, bool source) {
    switch (factor) {
    // Factor 0 names the *other* pixel: the source side scales by the
    // destination colour and the destination side by the source colour.
    case 0u: return source ? VK_BLEND_FACTOR_DST_COLOR : VK_BLEND_FACTOR_SRC_COLOR;
    case 1u: return source ? VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 2u: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 3u: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 4u: return VK_BLEND_FACTOR_DST_ALPHA;
    case 5u: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 6u: return VK_BLEND_FACTOR_SRC_ALPHA;             // doubled variants
    case 7u: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8u: return VK_BLEND_FACTOR_DST_ALPHA;
    case 9u: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case kFactorFixed: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case kFactorOne: return VK_BLEND_FACTOR_ONE;
    case kFactorZero: return VK_BLEND_FACTOR_ZERO;
    case kFactorInverseConstant: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    default: return source ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
    }
}

VkBlendOp to_blend_op(std::uint32_t equation) {
    switch (equation) {
    // GU_SUBTRACT is source minus destination; GU_REVERSE_SUBTRACT is the other
    // way round, and it is what darkening effects such as blob shadows use.
    case 1u: return VK_BLEND_OP_SUBTRACT;
    case 2u: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 3u: return VK_BLEND_OP_MIN;
    case 4u: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

VkCompareOp to_compare_op(std::uint32_t function) {
    switch (function) {
    case 0u: return VK_COMPARE_OP_NEVER;
    case 1u: return VK_COMPARE_OP_ALWAYS;
    case 2u: return VK_COMPARE_OP_EQUAL;
    case 3u: return VK_COMPARE_OP_NOT_EQUAL;
    case 4u: return VK_COMPARE_OP_LESS;
    case 5u: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 6u: return VK_COMPARE_OP_GREATER;
    default: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    }
}

// MGA skins its characters on the CPU and submits them pre-transformed with an
// identity world matrix, while scenery carries a real one. That identity is how
// the shadow casters are told apart from everything else -- and it is also what
// makes their vertices usable as they stand, already in the space the GE's
// lighting and the shadow map share.
[[nodiscard]] bool is_identity(const std::array<float, 16> &m) {
    for (std::uint32_t column = 0; column < 4u; ++column)
        for (std::uint32_t row = 0; row < 4u; ++row) {
            const float expected = column == row ? 1.0f : 0.0f;
            if (std::abs(m[column * 4u + row] - expected) > 1e-4f) return false;
        }
    return true;
}

// A GE colour register as hex, for the shadow trace.
[[nodiscard]] std::string psprecomp_hex(std::uint32_t value) {
    static const char *digits = "0123456789abcdef";
    std::string out = "0x";
    for (int shift = 28; shift >= 0; shift -= 4) out.push_back(digits[(value >> shift) & 0xFu]);
    return out;
}

// How far into the light's depth range a receiver may be before it shadows
// itself. The light's box spans tens of thousands of units, so this is a few
// tens of units of slack -- well under one grid cell.
constexpr float kShadowBias = 0.0015f;

std::array<float, 16> multiply(const std::array<float, 16> &a, const std::array<float, 16> &b) {
    std::array<float, 16> result{};
    for (std::uint32_t column = 0; column < 4u; ++column) {
        for (std::uint32_t row = 0; row < 4u; ++row) {
            float sum = 0.0f;
            for (std::uint32_t k = 0; k < 4u; ++k) sum += a[k * 4u + row] * b[column * 4u + k];
            result[column * 4u + row] = sum;
        }
    }
    return result;
}

bool check(VkResult result, const char *what, std::string &error) {
    if (result == VK_SUCCESS) return true;
    error = std::string(what) + " failed with VkResult " + std::to_string(static_cast<int>(result));
    return false;
}

const char *present_mode_name(VkPresentModeKHR mode) {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
    default: return "OTHER";
    }
}

// Writes 4-byte pixels, top row first, as a 24-bit bottom-up BMP: no encoder
// needed and every viewer reads it.
bool write_bmp(const std::string &path, const std::uint8_t *pixels, std::uint32_t width, std::uint32_t height,
               bool bgra) {
    const std::uint32_t row_bytes = (width * 3u + 3u) & ~3u;
    const std::uint32_t image_bytes = row_bytes * height;
    std::vector<std::uint8_t> file(54u + image_bytes, 0u);
    const auto put32 = [&](std::size_t at, std::uint32_t value) {
        file[at] = static_cast<std::uint8_t>(value);
        file[at + 1u] = static_cast<std::uint8_t>(value >> 8u);
        file[at + 2u] = static_cast<std::uint8_t>(value >> 16u);
        file[at + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };
    file[0] = 'B';
    file[1] = 'M';
    put32(2u, 54u + image_bytes);
    put32(10u, 54u);
    put32(14u, 40u);
    put32(18u, width);
    put32(22u, height);
    file[26] = 1u;
    file[28] = 24u;
    put32(34u, image_bytes);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t *source = pixels + static_cast<std::size_t>(y) * width * 4u;
        std::uint8_t *destination = file.data() + 54u + static_cast<std::size_t>(height - 1u - y) * row_bytes;
        for (std::uint32_t x = 0; x < width; ++x) {
            destination[x * 3u + 0u] = source[x * 4u + (bgra ? 0u : 2u)];
            destination[x * 3u + 1u] = source[x * 4u + 1u];
            destination[x * 3u + 2u] = source[x * 4u + (bgra ? 2u : 0u)];
        }
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char *>(file.data()), static_cast<std::streamsize>(file.size()));
    return static_cast<bool>(out);
}

struct PadTuning {
    float dead_zone{0.15f};
    float trigger{0.25f};
    float right_stick{0.5f};
    settings::RightStick right_stick_mode{settings::RightStick::Camera};
    bool invert_x{};
    bool invert_y{};
    bool confirm_south{};
    bool trace{false};
};

// Read on every poll, so the in-game menu's changes apply at once.
PadTuning pad_tuning() {
    static const bool trace = std::getenv("MGA_TRACE_PAD") != nullptr;
    const settings::Settings &player = settings::current();
    PadTuning value{};
    value.dead_zone = player.dead_zone;
    value.trigger = player.trigger;
    value.right_stick = player.right_stick_zone;
    // The right stick is a real nub on this release, so driving the D-pad
    // from it as well would turn the camera twice.
    value.right_stick_mode = player.right_stick;
    value.invert_x = player.invert_camera_x;
    value.invert_y = player.invert_camera_y;
    // A PlayStation pad already carries the PSP's own face buttons, so the
    // positional mapping puts confirm on circle where the prompts want it.
    value.confirm_south = player.confirm_south;
    value.trace = trace;
    return value;
}

// Adds one gamepad's state to the pad bits and to the analog offsets the
// keyboard path also writes, so the two sources simply OR together.
void read_gamepad(SDL_Gamepad *device, PadState &pad, int &analog_x, int &analog_y) {
    const PadTuning tuning = pad_tuning();
    std::uint32_t &buttons = pad.buttons;
    const auto held = [&](SDL_GamepadButton button, std::uint32_t bit) {
        if (SDL_GetGamepadButton(device, button)) buttons |= bit;
    };
    held(SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0010u);
    held(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0020u);
    held(SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0040u);
    held(SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0080u);
    held(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100u);
    held(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200u);
    held(SDL_GAMEPAD_BUTTON_START, 0x0008u);
    held(SDL_GAMEPAD_BUTTON_BACK, 0x0001u);
    held(SDL_GAMEPAD_BUTTON_NORTH, 0x1000u);
    held(SDL_GAMEPAD_BUTTON_WEST, 0x8000u);
    // The game prompts "circle Enter / cross Back", and on a PlayStation pad
    // those are the same two buttons in the same two places.
    held(SDL_GAMEPAD_BUTTON_SOUTH, tuning.confirm_south ? 0x2000u : 0x4000u);
    held(SDL_GAMEPAD_BUTTON_EAST, tuning.confirm_south ? 0x4000u : 0x2000u);

    // The PSP triggers are digital, but hunters hold L for the camera all the
    // time, so the analog triggers press the same bits as the shoulders.
    const auto axis = [&](SDL_GamepadAxis id) {
        return std::clamp(static_cast<float>(SDL_GetGamepadAxis(device, id)) / 32767.0f, -1.0f, 1.0f);
    };
    if (axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > tuning.trigger) buttons |= 0x0100u;
    if (axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > tuning.trigger) buttons |= 0x0200u;

    const float right_x = axis(SDL_GAMEPAD_AXIS_RIGHTX);
    const float right_y = axis(SDL_GAMEPAD_AXIS_RIGHTY);
    // The HD release has its own right-stick camera, so the stick normally goes
    // there. Pressing the D-pad bits as well would turn the camera twice, hence
    // the either/or: the claw emulation is only for builds where that path is
    // not wanted.
    if (tuning.right_stick_mode == settings::RightStick::DPad) {
        if (right_x < -tuning.right_stick) buttons |= 0x0080u;
        if (right_x > tuning.right_stick) buttons |= 0x0020u;
        if (right_y < -tuning.right_stick) buttons |= 0x0010u;
        if (right_y > tuning.right_stick) buttons |= 0x0040u;
    }

    // Rescale the live range, otherwise leaving the dead zone snaps the stick
    // straight to a sixth of its travel.
    const auto deflect = [&](float x, float y, std::uint8_t &out_x, std::uint8_t &out_y) {
        const float length = std::sqrt(x * x + y * y);
        if (length <= tuning.dead_zone) return;
        const float scale = std::min((length - tuning.dead_zone) / (1.0f - tuning.dead_zone), 1.0f) / length;
        out_x = static_cast<std::uint8_t>(std::clamp(0x80 + static_cast<int>(x * scale * 127.0f), 0, 255));
        out_y = static_cast<std::uint8_t>(std::clamp(0x80 + static_cast<int>(y * scale * 127.0f), 0, 255));
    };
    if (tuning.right_stick_mode == settings::RightStick::Camera)
        deflect(tuning.invert_x ? -right_x : right_x, tuning.invert_y ? -right_y : right_y, pad.right_x, pad.right_y);

    const float left_x = axis(SDL_GAMEPAD_AXIS_LEFTX);
    const float left_y = axis(SDL_GAMEPAD_AXIS_LEFTY);
    std::uint8_t nub_x = 0x80u;
    std::uint8_t nub_y = 0x80u;
    deflect(left_x, left_y, nub_x, nub_y);
    analog_x += static_cast<int>(nub_x) - 0x80;
    analog_y += static_cast<int>(nub_y) - 0x80;
}

} // namespace

struct VulkanRenderer::Impl {
    struct Texture {
        VkImage image{};
        VkDeviceMemory memory{};
        VkImageView view{};
        VkDescriptorSet descriptor{};
        std::uint64_t last_used{};
    };

    RendererConfig config;
    SDL_Window *window{};
    VkInstance instance{};
    VkSurfaceKHR surface{};
    VkPhysicalDevice physical_device{};
    VkDevice device{};
    std::uint32_t queue_family{};
    VkQueue queue{};
    VkSwapchainKHR swapchain{};
    VkFormat swapchain_format{VK_FORMAT_B8G8R8A8_UNORM};
    VkExtent2D swapchain_extent{};
    std::vector<VkImage> swapchain_images;
    VkCommandPool command_pool{};
    VkCommandBuffer command_buffer{};
    // The shadow pass, recorded once the frame's casters are known but
    // submitted before the scene, so the scene can sample what it produced.
    // Recording order and execution order are not the same thing, which is the
    // whole reason this is a buffer of its own rather than part of the first.
    VkCommandBuffer shadow_command_buffer{};
    bool shadow_recorded{};
    VkFence frame_fence{};
    VkSemaphore image_available{};
    VkSemaphore render_finished{};
    VkPresentModeKHR present_mode{VK_PRESENT_MODE_FIFO_KHR};
    VkSurfaceFormatKHR surface_format{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    VkImageUsageFlags swapchain_usage{};
    std::vector<VkPresentModeKHR> present_modes;
    std::vector<VkImageView> swapchain_views;
    std::uint32_t swapchain_min_images{2u};
    // Set by a resize, a present mode change or an out-of-date swapchain;
    // the swapchain is rebuilt before the next acquire.
    bool swapchain_dirty{};

    // Display settings.
    settings::PresentMode requested_present{settings::PresentMode::Fifo};
    bool keep_aspect{true};
    bool sharp_screen{};
    bool sharp_textures{};
    // Mipmaps and anisotropic filtering: the PSP sampled one level whatever the
    // distance, which shimmers badly once the internal resolution is raised
    // above the 480x272 that hid it.
    bool smooth_textures{};
    // Replacement textures from a pack, and the dumps that let someone make
    // them; see gpu/texture_pack.hpp.
    TexturePack texture_pack;
    bool texture_pack_enabled{};
    // Decoded textures are upscaled by this factor before upload, so they
    // still have detail to give at high internal resolution.
    std::uint32_t texture_scale{1u};
    bool texture_scale_sharp{true};
    // Sample pixel-mapped 2D sharp even when 3D is filtered smoothly.
    bool smart_2d{true};
    float max_anisotropy{1.0f};
    std::string device_name;

    // Dear ImGui draws in its own render pass over the finished swapchain
    // image, after the game frame and the performance overlay.
    VkRenderPass ui_render_pass{};
    std::vector<VkFramebuffer> ui_framebuffers;
    bool ui_ready{};
    ImDrawData *ui_draw_data{};
    std::function<bool(const SDL_Event &)> event_hook;
    bool game_input{true};
    bool suppress_held{};
    std::uint32_t suppressed_buttons{};

    // Post-processing. With it on, the game frame reaches the window as a
    // fullscreen triangle that samples the render target, instead of the blit
    // that used to copy it. A draw is what makes room for an effect that has
    // to read more than one texel: anti-aliasing now, and ambient occlusion
    // once the depth target is bound alongside the colour.
    //
    // The pass also takes over the scaling and the letterboxing the blit did,
    // so turning it on must not move the picture: the shader is handed the
    // same rectangle the blit computed.
    bool post_enabled{};
    bool post_fxaa{};
    float post_ao{};
    // Blob shadows. The casters come from the kernel, which reads them out of
    // the game's records; the renderer only draws them.
    float blob_strength{};
    // Shadow maps cast from the game's own lights; gpu/shadow_map.hpp explains
    // how the spaces line up and why the map is a frame behind.
    // Behind a pointer because Impl is move-assigned when the renderer shuts
    // down, and a type holding raw Vulkan handles has no business being
    // movable.
    std::unique_ptr<ShadowMap> shadow_map;
    float shadow_strength{};
    std::uint32_t shadow_resolution{1024u};
    bool shadow_available{};
    // What binding 2 of the lighting set holds, so it is rewritten only when
    // the answer changes.
    VkImageView shadow_bound_view{};
    // Draw space to the casting light's clip space, and which light that is.
    std::array<float, 16> shadow_light_transform{};
    int shadow_light{-1};
    // The lighting the last lit draw used, which the casting light is chosen
    // out of.
    LightingState last_lighting{};
    bool last_lighting_valid{};
    void bind_shadow_map();
    // MGA_TRACE_SHADOWS: every link in the shadow chain, counted per frame, so
    // one run says which of them is the one that is failing.
    struct ShadowTrace {
        std::uint32_t transformed{};   // transformed draws
        std::uint32_t lit{};           // ... of them lit, so they can receive
        std::uint32_t identity{};      // ... with an identity world matrix, so they can cast
        std::uint32_t identity_lit{};  // ... both, which is what is actually collected
        std::uint32_t casters{};       // caster vertices handed to the map
        bool resolved{};               // a light was found and a box built
    };
    ShadowTrace shadow_trace{};
    // The game's own world-to-clip matrix. Drawing the blobs with it means the
    // port never has to decide what space the display list is in.
    std::array<float, 16> shadow_transform{};
    bool shadow_transform_valid{};
    std::vector<ShadowCaster> shadow_casters;
    bool blobs_drawn{};
    // Enough of the last transformed draw to put a blob into the same space:
    // the scene's matrices, its viewport and the target it went to. Copying
    // the whole DrawCall every draw would mean copying its vertices with it.
    struct SceneView {
        bool valid{};
        std::array<float, 16> view{};
        std::array<float, 16> projection{};
        ViewportState viewport{};
        RenderTarget target{};
        DepthState depth{};
        std::uint64_t environment_version{};
        std::uint64_t material_version{};
    };
    SceneView last_scene;
    // Which way round the depth buffer runs. The GE's viewport decides it per
    // draw, and MGA does use sceGuDepthRange(65535, 0) in places, so it is
    // taken from the last transformed draw rather than assumed.
    bool depth_reversed{};
    VkRenderPass post_render_pass{};
    std::vector<VkFramebuffer> post_framebuffers;
    VkShaderModule post_vertex_shader{};
    VkShaderModule post_fragment_shader{};
    VkDescriptorSetLayout post_set_layout{};
    VkPipelineLayout post_pipeline_layout{};
    VkPipeline post_pipeline{};
    // Kept per target view rather than rebuilt: targets outlive the frame.
    // Changing the screen filtering, or destroying a target, drops them all.
    std::map<VkImageView, VkDescriptorSet> post_descriptors;
    bool post_descriptors_sharp{};
    bool post_descriptors_depth{};
    bool create_post_pipeline(std::string &error);
    bool create_post_framebuffers(std::string &error);
    void drop_post_descriptors();
    [[nodiscard]] bool post_active() const noexcept {
        return post_enabled && post_pipeline != VK_NULL_HANDLE && !post_framebuffers.empty();
    }
    [[nodiscard]] VkDescriptorSet post_descriptor_for(VkImageView color, VkImageView depth);
    void record_post(VkImageView color, VkImageView depth, std::uint32_t image_index);

    // Window capture: the presented image is copied here and written after
    // its frame completes.
    std::string capture_path;
    VkBuffer capture_buffer{};
    VkDeviceMemory capture_memory{};
    VkExtent2D capture_extent{};
    bool capture_recorded{};

    // Performance overlay: drawn on the CPU, copied through a mapped staging
    // buffer into a small image and scaled onto the swapchain image after the
    // game frame, so screenshots of the window include it.
    bool overlay_visible{};
    bool overlay_ready{};
    VkImage overlay_image{};
    VkDeviceMemory overlay_memory{};
    VkImageView overlay_view{};
    VkBuffer overlay_staging{};
    VkDeviceMemory overlay_staging_memory{};
    void *overlay_mapped{};
    std::vector<std::uint32_t> overlay_pixels;

    // Frames the game writes to memory without the GE (upload_frame): copied
    // through a mapped staging buffer into an image the size of the frame,
    // then scaled into the target of the address the game shows.
    VkImage upload_image{};
    VkDeviceMemory upload_memory{};
    VkImageView upload_view{};
    VkExtent2D upload_extent{};
    VkBuffer upload_staging{};
    VkDeviceMemory upload_staging_memory{};
    void *upload_mapped{};
    void destroy_upload();
    bool create_upload(std::uint32_t width, std::uint32_t height, std::string &error);

    // One offscreen target per guest framebuffer address. The game draws into
    // several (double buffering, render to texture), and only the address passed
    // to sceDisplaySetFrameBuf is shown.
    struct Target {
        VkImage color{};
        VkDeviceMemory color_memory{};
        VkImageView color_view{};
        VkImage depth{};
        VkDeviceMemory depth_memory{};
        VkImageView depth_view{};
        VkFramebuffer framebuffer{};
        bool initialized{};
        // The guest's view of the buffer when it was last drawn to: row length
        // in pixels and pixel format (0:5650 1:5551 2:4444 3:8888).
        std::uint32_t stride{512u};
        std::uint32_t format{3u};
        std::uint64_t last_drawn_frame{};
        // Bumped by every draw into the target, so a copy made for sampling
        // knows when it is out of date.
        std::uint64_t draw_serial{};
        // One guest word from every 256 bytes of the buffer, read when it was
        // last drawn to. The renderer never writes guest memory, so a word that
        // has changed since means the game put something else there.
        std::vector<std::uint32_t> guest_words;
        // The copy that draws sample when the game textures from this buffer:
        // a render pass cannot read its own attachment.
        VkImage copy{};
        VkDeviceMemory copy_memory{};
        VkImageView copy_view{};
        VkImageView copy_opaque_view{};
        std::array<VkDescriptorSet, 2> copy_descriptors{};
        std::uint64_t copy_serial{};
        bool copy_valid{};
    };
    std::map<std::uint32_t, Target> targets;
    // Write-back of the displayed framebuffer to guest VRAM (write_back_frame):
    // present() scales the target to 480x272 and copies it into a mapped
    // buffer; begin_frame(), after the frame fence, takes the pixels; the next
    // write_back_frame() stores them in the guest's format.
    VkImage writeback_image{};
    VkDeviceMemory writeback_memory{};
    VkImageView writeback_view{};
    VkBuffer writeback_buffer{};
    VkDeviceMemory writeback_buffer_memory{};
    void *writeback_mapped{};
    struct WritebackFrame {
        std::uint32_t address{};
        std::uint32_t stride{};
        std::uint32_t format{};
    };
    WritebackFrame writeback_recorded{};  // copied by the frame in flight
    bool writeback_in_flight{};
    WritebackFrame writeback_ready{};     // pixels waiting in writeback_pixels
    bool writeback_has_pixels{};
    std::vector<std::uint32_t> writeback_pixels;
    bool create_writeback(std::string &error);
    void destroy_writeback();
    void record_writeback(std::uint32_t address);
    // Stores 480x272 RGBA pixels into the guest framebuffer `frame` describes.
    void store_frame(GuestMemory &memory, const WritebackFrame &frame, const std::uint32_t *pixels);
    // Numbers draws into targets across all of them, for Target::draw_serial.
    std::uint64_t target_draw_counter{};
    std::uint32_t current_target{};
    std::uint32_t last_drawn_target{};
    std::uint32_t presented_target{};
    // A copy of the frame shown when hold_frame() began; only its colour
    // image is used.
    Target held{};
    bool holding{};
    // Per-frame tally, so "no 3D" can be told from "3D drawn somewhere else".
    std::uint32_t frame_through_draws{};
    std::uint32_t frame_transformed_draws{};
    std::uint32_t frame_transformed_vertices{};
    std::uint32_t frame_onscreen_vertices{};
    std::uint32_t frame_behind_camera{};
    std::array<float, 3> frame_ndc_min{1e30f, 1e30f, 1e30f};
    std::array<float, 3> frame_ndc_max{-1e30f, -1e30f, -1e30f};
    std::map<std::uint32_t, std::uint32_t> frame_transformed_targets;
    // MGA_FRAME_DIGEST: one record per draw, so a frame can be compared with
    // the one before it. Frame interpolation needs three things to be true --
    // that consecutive frames differ at all, that the draw list keeps its
    // shape, and that the motion shows up in the vertices -- and this answers
    // all three without guessing.
    struct DigestDraw {
        bool through{};
        std::uint32_t target{};
        std::uint32_t vertices{};
        std::uint64_t hash{};
    };
    std::vector<DigestDraw> frame_digest;
    std::vector<DigestDraw> previous_digest;
    // MGA_TRACE_OBJECTS: where the frame's transformed draws sit. The view
    // matrix carries rotation only and the camera's position is baked into
    // each world matrix, so a world matrix's translation is the object's
    // position in the space the scene is actually drawn in -- which is the
    // space a blob shadow would have to be placed in too.
    struct FrameObject {
        std::array<float, 3> at{};
        std::uint32_t vertices{};
        std::uint32_t draws{};
        // Combined hash of the object's vertex data. A character standing
        // still animates in its vertices, not its matrix -- the position
        // never moves, so only this changes.
        std::uint64_t hash{};
    };
    std::vector<FrameObject> frame_objects;
    std::vector<FrameObject> previous_objects;
    // MGA_TRACE_CAMERA: the view matrices this frame's transformed draws
    // used and how many vertices each of them covered. A frame holds a handful
    // (the scene, a reflection, a shadow pass), and the busiest one is the
    // camera the player sees, so the frame's line can report that one. Only
    // filled while the trace is on.
    std::vector<std::pair<std::array<float, 16>, std::uint32_t>> frame_views;
    // The yaw of the previous traced frame, so each line can carry the turn.
    float traced_yaw{};
    bool pass_active{};
    VkRenderPass render_pass{};
    VkExtent2D target_extent{};

    VkShaderModule vertex_shader{};
    VkShaderModule fragment_shader{};
    VkPipelineLayout pipeline_layout{};
    VkDescriptorSetLayout descriptor_layout{};
    VkDescriptorPool descriptor_pool{};
    VkDescriptorSetLayout lighting_layout{};
    VkDescriptorSet lighting_descriptor{};  // binding 0: environment, binding 1: object
    VkDeviceSize uniform_alignment{256u};
    VkSampler sampler{};        // linear
    VkSampler sharp_sampler{};  // nearest, for the sharp texture setting
    VkSampler mip_sampler{};    // trilinear + anisotropic, for smooth textures
    // The same, clamped to the edge, for render targets sampled as textures:
    // the game's texture is usually larger than the 480x272 the target holds.
    VkSampler clamp_sampler{};
    VkSampler clamp_sharp_sampler{};
    std::map<PipelineKey, VkPipeline> pipelines;

    VkBuffer vertex_buffer{};
    VkDeviceMemory vertex_memory{};
    void *vertex_mapped{};
    VkDeviceSize vertex_offset{};
    // The lighting blocks most recently written this frame, reused while the
    // state stays the same; begin_frame() drops them with the vertex buffer.
    std::uint64_t environment_version{};    // 0: none written this frame
    std::uint32_t environment_offset{};
    ObjectBlock last_object{};
    std::uint32_t object_offset{};
    bool object_valid{};
    // The material colours of the last lit draw, by GeState's material version.
    std::uint64_t material_version{};       // 0: none unpacked yet
    std::array<std::array<float, 4>, 4> material{};
    // What the command buffer has bound, so that unchanged state is not bound
    // again; begin_frame() and begin_pass() forget it.
    VkPipeline bound_pipeline{};
    std::array<std::uint32_t, 2> bound_lighting_offsets{};
    bool lighting_bound{};

    // Copies a uniform block into the vertex buffer at the next aligned offset.
    // Returns false, writing nothing, when the buffer is full.
    bool write_uniform(const void *data, std::size_t size, std::uint32_t &offset) {
        const VkDeviceSize at = (vertex_offset + uniform_alignment - 1u) / uniform_alignment * uniform_alignment;
        if (at + size > kVertexBufferBytes) return false;
        std::memcpy(static_cast<std::uint8_t *>(vertex_mapped) + at, data, size);
        offset = static_cast<std::uint32_t>(at);
        vertex_offset = at + size;
        return true;
    }
    void forget_bindings() {
        bound_pipeline = VK_NULL_HANDLE;
        lighting_bound = false;
    }

    Texture white_texture{};
    std::map<std::uint64_t, Texture> textures;
    std::uint64_t texture_clock{};
    // texture_key results for the display list being walked, by the state that
    // feeds the key; cleared by begin_display_list().
    struct TextureKeyInput {
        std::uint32_t address{};
        std::uint32_t buffer_width{};
        std::uint32_t size{};
        std::uint32_t format{};
        std::uint32_t clut_address{};
        std::uint32_t clut_format{};
        bool swizzled{};
        auto operator<=>(const TextureKeyInput &) const = default;
    };
    std::map<TextureKeyInput, std::uint64_t> list_texture_keys;

    std::vector<GpuVertex> scratch;
    PadState pad{};
    SDL_Gamepad *gamepad{};
    SDL_JoystickID gamepad_id{};
    bool recording{};
    bool quit{};
    bool ready{};
    std::uint64_t frames{};
    std::uint64_t draws{};

    // Only the first pad is used; a second one arriving is ignored rather than
    // stealing the stick from whoever is already playing.
    void open_gamepad(SDL_JoystickID id) {
        if (gamepad != nullptr) {
            // The virtual pad of MGA_INPUT_SCRIPT takes over from a real
            // one, so a controller within reach does not steal a scripted run.
            const char *name = SDL_GetGamepadNameForID(id);
            if (name == nullptr || std::strcmp(name, "Yakumo input script") != 0) return;
            SDL_CloseGamepad(gamepad);
            gamepad = nullptr;
        }
        SDL_Gamepad *device = SDL_OpenGamepad(id);
        if (device == nullptr) {
            std::cout << "[pad] SDL_OpenGamepad failed: " << SDL_GetError() << "\n";
            return;
        }
        gamepad = device;
        gamepad_id = id;
        const char *name = SDL_GetGamepadName(device);
        const PadTuning tuning = pad_tuning();
        std::cout << "[pad] " << (name != nullptr ? name : "gamepad") << " connected; confirm on "
                  << (tuning.confirm_south ? "the south button" : "circle") << ", right stick "
                  << (tuning.right_stick_mode == settings::RightStick::DPad     ? "as D-pad"
                      : tuning.right_stick_mode == settings::RightStick::Camera ? "as camera"
                                                                                : "off")
                  << "\n";
    }

    // Picks up a pad that was already plugged in before the window existed, and
    // falls back to a still-connected second pad when the first one is unplugged.
    void scan_gamepads() {
        int count = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&count);
        if (ids != nullptr) {
            for (int i = 0; i < count && gamepad == nullptr; ++i) open_gamepad(ids[i]);
            SDL_free(ids);
        }
        if (gamepad != nullptr) return;
        // Say why there is no pad rather than staying silent: a stick with no
        // entry in SDL's mapping database enumerates as a joystick only, which
        // looks identical to "nothing plugged in" from the player's side.
        int joysticks = 0;
        SDL_JoystickID *sticks = SDL_GetJoysticks(&joysticks);
        if (sticks != nullptr) {
            for (int i = 0; i < joysticks; ++i) {
                if (SDL_IsGamepad(sticks[i])) continue;
                const char *name = SDL_GetJoystickNameForID(sticks[i]);
                std::cout << "[pad] " << (name != nullptr ? name : "joystick")
                          << " has no gamepad mapping, ignored\n";
            }
            SDL_free(sticks);
        }
        if (joysticks == 0) std::cout << "[pad] no gamepad connected, keyboard only\n";
    }

    void close_gamepad(SDL_JoystickID id) {
        if (gamepad == nullptr || id != gamepad_id) return;
        SDL_CloseGamepad(gamepad);
        gamepad = nullptr;
        gamepad_id = 0;
        std::cout << "[pad] gamepad disconnected\n";
        scan_gamepads();
    }

    [[nodiscard]] std::uint32_t find_memory_type(std::uint32_t mask, VkMemoryPropertyFlags properties) const {
        VkPhysicalDeviceMemoryProperties memory_properties{};
        vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
        for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            if ((mask & (1u << i)) != 0u &&
                (memory_properties.memoryTypes[i].propertyFlags & properties) == properties)
                return i;
        }
        return 0u;
    }

    bool create_image(std::uint32_t width, std::uint32_t height, VkFormat format, VkImageUsageFlags usage,
                      VkImage &image, VkDeviceMemory &memory, VkImageView &view, VkImageAspectFlags aspect,
                      std::string &error, std::uint32_t mip_levels = 1u) {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {width, height, 1u};
        info.mipLevels = mip_levels;
        info.arrayLayers = 1u;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!check(vkCreateImage(device, &info, nullptr, &image), "vkCreateImage", error)) return false;

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, image, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!check(vkAllocateMemory(device, &allocate, nullptr, &memory), "vkAllocateMemory", error)) return false;
        vkBindImageMemory(device, image, memory, 0u);

        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format;
        view_info.subresourceRange = {aspect, 0u, mip_levels, 0u, 1u};
        return check(vkCreateImageView(device, &view_info, nullptr, &view), "vkCreateImageView", error);
    }

    void transition(VkCommandBuffer commands, VkImage image, VkImageLayout from, VkImageLayout to,
                    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, std::uint32_t levels = 1u) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {aspect, 0u, levels, 0u, 1u};
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u,
                             0u, nullptr, 0u, nullptr, 1u, &barrier);
    }

    [[nodiscard]] VkSampler texture_sampler() const {
        if (sharp_textures) return sharp_sampler;
        return smooth_textures ? mip_sampler : sampler;
    }
    // How many mip levels a texture of this size gets; 1 when smoothing is off.
    [[nodiscard]] std::uint32_t mip_levels_for(std::uint32_t width, std::uint32_t height) const {
        if (!smooth_textures || sharp_textures) return 1u;
        std::uint32_t levels = 1u;
        for (std::uint32_t size = std::max(width, height); size > 1u; size /= 2u) ++levels;
        return levels;
    }
    [[nodiscard]] VkSampler framebuffer_sampler() const {
        return sharp_textures ? clamp_sharp_sampler : clamp_sampler;
    }
    [[nodiscard]] VkPresentModeKHR wanted_present_mode() const;
    bool create_swapchain(std::string &error);
    void destroy_swapchain_views();
    void recreate_swapchain();
    bool create_ui_framebuffers(std::string &error);
    void record_game_blit(VkImage source, VkImage destination);
    void submit_and_present(const Target *source_target, bool game_frame);
    void write_capture();
    void destroy_target(Target &target);
    void run_commands(const std::function<void(VkCommandBuffer)> &record);
    Target *target_for(std::uint32_t address, std::string &error);
    bool create_overlay(std::string &error);
    void record_overlay(VkImage destination);
    void update_display_info();
    void begin_pass(std::uint32_t address);
    void end_pass();
    VkPipeline pipeline_for(const PipelineKey &key);
    Texture &texture_for(const GuestMemory &memory, const TextureState &state);
    void trace_framebuffer_texture(const DrawCall &call);
    // A render target the texture reads, with the texture's first texel as a
    // pixel position inside it; see framebuffer_texture().
    struct FramebufferTexture {
        Target *target{};
        std::uint32_t x{};
        std::uint32_t y{};
    };
    [[nodiscard]] FramebufferTexture find_framebuffer_texture(const GuestMemory &memory,
                                                              const TextureState &texture);
    VkDescriptorSet framebuffer_descriptor(Target &target, bool opaque);
    void snapshot_guest_words(const GuestMemory &memory, std::uint32_t address, Target &target);
    Texture create_texture(std::uint32_t width, std::uint32_t height, const std::uint32_t *pixels);
    void destroy_texture(Texture &texture);
};

VulkanRenderer::VulkanRenderer() : impl_(std::make_unique<Impl>()) {}
VulkanRenderer::~VulkanRenderer() { shutdown(); }

bool VulkanRenderer::available() const noexcept { return impl_ && impl_->ready; }
bool VulkanRenderer::quit_requested() const noexcept { return impl_ && impl_->quit; }
std::uint64_t VulkanRenderer::frames_presented() const noexcept { return impl_ ? impl_->frames : 0u; }
std::uint64_t VulkanRenderer::draws_submitted() const noexcept { return impl_ ? impl_->draws : 0u; }

bool VulkanRenderer::initialize(const RendererConfig &config, std::string &error) {
    Impl &impl = *impl_;
    impl.config = config;
    const settings::Settings &player = settings::current();
    const std::uint32_t scale = std::clamp<std::uint32_t>(player.internal_scale, 1u, settings::kMaxInternalScale);
    impl.target_extent = {kPspWidth * scale, kPspHeight * scale};
    impl.requested_present = player.present_mode;
    impl.keep_aspect = player.keep_aspect;
    impl.sharp_screen = player.sharp_screen;
    impl.sharp_textures = player.sharp_textures;
    impl.smooth_textures = player.smooth_textures;
    impl.texture_scale = std::clamp<std::uint32_t>(player.texture_scale, 1u, settings::kMaxTextureScale);
    impl.texture_scale_sharp = player.texture_scale_sharp;
    impl.smart_2d = player.smart_2d;
    impl.post_enabled = player.post_process;
    impl.post_fxaa = player.fxaa;
    impl.post_ao = std::clamp(player.contact_shadows, 0.0f, 1.0f);
    impl.blob_strength = std::clamp(player.blob_shadows, 0.0f, 1.0f);
    impl.shadow_strength = std::clamp(player.shadow_maps, 0.0f, 1.0f);
    impl.texture_pack_enabled = player.texture_pack;
    try {
        impl.texture_pack.open(install::user_data_directory());
    } catch (const std::exception &e) {
        std::cout << "[textures] cannot look for a texture pack: " << e.what() << "\n";
    }
    // Dumping is a developer's errand rather than a setting: it writes a file
    // for every texture the game draws.
    if (std::getenv("MGA_DUMP_TEXTURES") != nullptr) impl.texture_pack.set_dumping(true);
    if (impl.texture_pack.available())
        std::cout << "[textures] a texture pack is present\n";
    const std::uint32_t window_scale = std::clamp<std::uint32_t>(player.window_scale, 1u, settings::kMaxWindowScale);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        error = std::string("SDL_Init failed: ") + SDL_GetError();
        return false;
    }
    // A missing gamepad subsystem is not fatal; the keyboard still drives the pad.
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) std::cout << "[pad] no gamepad support: " << SDL_GetError() << "\n";
    else impl.scan_gamepads();
    SDL_WindowFlags window_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
    if (player.fullscreen) window_flags |= SDL_WINDOW_FULLSCREEN;
    impl.window = SDL_CreateWindow(config.title.c_str(), static_cast<int>(kPspWidth * window_scale),
                                   static_cast<int>(kPspHeight * window_scale), window_flags);
    if (impl.window == nullptr) {
        error = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
        return false;
    }

    std::uint32_t extension_count = 0u;
    const char *const *sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
    std::vector<const char *> extensions(sdl_extensions, sdl_extensions + extension_count);
    extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "Yakumo";
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &application;
    instance_info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    instance_info.ppEnabledExtensionNames = extensions.data();
    // MoltenVK reports itself as a portability driver and refuses the instance
    // without this flag.
    instance_info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    if (!check(vkCreateInstance(&instance_info, nullptr, &impl.instance), "vkCreateInstance", error)) return false;

    if (!SDL_Vulkan_CreateSurface(impl.window, impl.instance, nullptr, &impl.surface)) {
        error = std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError();
        return false;
    }

    std::uint32_t device_count = 0u;
    vkEnumeratePhysicalDevices(impl.instance, &device_count, nullptr);
    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(impl.instance, &device_count, devices.data());
    if (devices.empty()) {
        error = "no Vulkan device found";
        return false;
    }
    impl.physical_device = devices.front();
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            impl.physical_device = candidate;
            break;
        }
    }

    std::uint32_t family_count = 0u;
    vkGetPhysicalDeviceQueueFamilyProperties(impl.physical_device, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(impl.physical_device, &family_count, families.data());
    bool found_family = false;
    for (std::uint32_t i = 0; i < family_count; ++i) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(impl.physical_device, i, impl.surface, &present);
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u && present == VK_TRUE) {
            impl.queue_family = i;
            found_family = true;
            break;
        }
    }
    if (!found_family) {
        error = "no graphics queue with presentation support";
        return false;
    }

    std::uint32_t device_extension_count = 0u;
    vkEnumerateDeviceExtensionProperties(impl.physical_device, nullptr, &device_extension_count, nullptr);
    std::vector<VkExtensionProperties> device_extensions(device_extension_count);
    vkEnumerateDeviceExtensionProperties(impl.physical_device, nullptr, &device_extension_count,
                                         device_extensions.data());
    std::vector<const char *> enabled_device_extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    for (const VkExtensionProperties &extension : device_extensions) {
        if (std::strcmp(extension.extensionName, "VK_KHR_portability_subset") == 0)
            enabled_device_extensions.push_back("VK_KHR_portability_subset");
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = impl.queue_family;
    queue_info.queueCount = 1u;
    queue_info.pQueuePriorities = &priority;
    // Anisotropic filtering has to be asked for, and the limit read, before
    // the device exists.
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(impl.physical_device, &supported);
    VkPhysicalDeviceFeatures wanted{};
    if (supported.samplerAnisotropy == VK_TRUE) {
        wanted.samplerAnisotropy = VK_TRUE;
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(impl.physical_device, &properties);
        impl.max_anisotropy = std::min(16.0f, properties.limits.maxSamplerAnisotropy);
    }

    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pEnabledFeatures = &wanted;
    device_info.queueCreateInfoCount = 1u;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = static_cast<std::uint32_t>(enabled_device_extensions.size());
    device_info.ppEnabledExtensionNames = enabled_device_extensions.data();
    if (!check(vkCreateDevice(impl.physical_device, &device_info, nullptr, &impl.device), "vkCreateDevice", error))
        return false;
    vkGetDeviceQueue(impl.device, impl.queue_family, 0u, &impl.queue);

    // Swapchain.
    std::uint32_t format_count = 0u;
    vkGetPhysicalDeviceSurfaceFormatsKHR(impl.physical_device, impl.surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(impl.physical_device, impl.surface, &format_count, formats.data());
    // The GE's colours are already gamma-encoded, so they must reach the display
    // unchanged: prefer a plain 8-bit UNORM format. Gamescope (Steam Deck Game
    // Mode) lists an _SRGB format first, and presenting through it encodes the
    // colours a second time and washes the picture out.
    if (!formats.empty()) {
        impl.surface_format = formats.front();
        for (const VkSurfaceFormatKHR &candidate : formats) {
            if ((candidate.format == VK_FORMAT_B8G8R8A8_UNORM || candidate.format == VK_FORMAT_R8G8B8A8_UNORM) &&
                candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                impl.surface_format = candidate;
                break;
            }
        }
    }
    impl.swapchain_format = impl.surface_format.format;
    std::uint32_t mode_count = 0u;
    vkGetPhysicalDeviceSurfacePresentModesKHR(impl.physical_device, impl.surface, &mode_count, nullptr);
    impl.present_modes.resize(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(impl.physical_device, impl.surface, &mode_count,
                                              impl.present_modes.data());
    if (!impl.create_swapchain(error)) return false;

    std::array<VkAttachmentDescription, 2> attachments{};
    attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1] = attachments[0];
    attachments[1].format = VK_FORMAT_D32_SFLOAT;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color_reference{0u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_reference{1u, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1u;
    subpass.pColorAttachments = &color_reference;
    subpass.pDepthStencilAttachment = &depth_reference;
    VkRenderPassCreateInfo render_pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    render_pass_info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    render_pass_info.pAttachments = attachments.data();
    render_pass_info.subpassCount = 1u;
    render_pass_info.pSubpasses = &subpass;
    if (!check(vkCreateRenderPass(impl.device, &render_pass_info, nullptr, &impl.render_pass), "vkCreateRenderPass",
               error))
        return false;

    // Shaders, descriptors and pipeline layout.
    const auto create_shader = [&](const std::uint32_t *code, std::size_t size, VkShaderModule &module) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = size;
        info.pCode = code;
        return check(vkCreateShaderModule(impl.device, &info, nullptr, &module), "vkCreateShaderModule", error);
    };
    if (!create_shader(kGeVertexShader, sizeof(kGeVertexShader), impl.vertex_shader)) return false;
    if (!create_shader(kGeFragmentShader, sizeof(kGeFragmentShader), impl.fragment_shader)) return false;

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0u;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1u;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.bindingCount = 1u;
    layout_info.pBindings = &binding;
    if (!check(vkCreateDescriptorSetLayout(impl.device, &layout_info, nullptr, &impl.descriptor_layout),
               "vkCreateDescriptorSetLayout", error))
        return false;

    // Set 1: the lighting environment and the lit object, windows into the
    // vertex buffer.
    std::array<VkDescriptorSetLayoutBinding, 3> lighting_bindings{};
    lighting_bindings[0].binding = 0u;
    lighting_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    lighting_bindings[0].descriptorCount = 1u;
    lighting_bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    lighting_bindings[1].binding = 1u;
    lighting_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    lighting_bindings[1].descriptorCount = 1u;
    lighting_bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    // The shadow map. Bound once with the rest of the lighting rather than per
    // draw, because every lit draw reads the same one.
    lighting_bindings[2].binding = 2u;
    lighting_bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    lighting_bindings[2].descriptorCount = 1u;
    lighting_bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lighting_layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lighting_layout_info.bindingCount = static_cast<std::uint32_t>(lighting_bindings.size());
    lighting_layout_info.pBindings = lighting_bindings.data();
    if (!check(vkCreateDescriptorSetLayout(impl.device, &lighting_layout_info, nullptr, &impl.lighting_layout),
               "vkCreateDescriptorSetLayout", error))
        return false;

    const std::array<VkDescriptorPoolSize, 2> pool_sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                             static_cast<std::uint32_t>(kMaxCachedTextures + 1u + kMaxFramebufferTextureSets +
                                                        kMaxPostSets + 1u)},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2u},
    };
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets =
        static_cast<std::uint32_t>(kMaxCachedTextures + 2u + kMaxFramebufferTextureSets + kMaxPostSets);
    pool_info.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();
    if (!check(vkCreateDescriptorPool(impl.device, &pool_info, nullptr, &impl.descriptor_pool),
               "vkCreateDescriptorPool", error))
        return false;

    VkPushConstantRange push_range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0u,
                                   sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo pipeline_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    const std::array<VkDescriptorSetLayout, 2> set_layouts{impl.descriptor_layout, impl.lighting_layout};
    pipeline_layout_info.setLayoutCount = static_cast<std::uint32_t>(set_layouts.size());
    pipeline_layout_info.pSetLayouts = set_layouts.data();
    pipeline_layout_info.pushConstantRangeCount = 1u;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    if (!check(vkCreatePipelineLayout(impl.device, &pipeline_layout_info, nullptr, &impl.pipeline_layout),
               "vkCreatePipelineLayout", error))
        return false;

    // The post-processing pass. Built whether or not it is switched on, so it
    // can be turned on and off from the menu without rebuilding the device;
    // if it cannot be built the renderer carries on blitting.
    // MGA_NO_POST skips building it at all, which is the way to tell a fault
    // in this pass from one anywhere else.
    static const bool no_post = std::getenv("MGA_NO_POST") != nullptr;
    if (no_post) {
        std::cout << "[render] post-processing disabled by MGA_NO_POST\n";
    } else if (!impl.create_post_pipeline(error)) {
        std::cout << "[render] post-processing unavailable: " << error << "\n";
        error.clear();
    } else if (!impl.create_post_framebuffers(error)) {
        std::cout << "[render] post-processing unavailable: " << error << "\n";
        error.clear();
    }

    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.maxLod = 1.0f;
    if (!check(vkCreateSampler(impl.device, &sampler_info, nullptr, &impl.sampler), "vkCreateSampler", error))
        return false;
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    if (!check(vkCreateSampler(impl.device, &sampler_info, nullptr, &impl.sharp_sampler), "vkCreateSampler", error))
        return false;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!check(vkCreateSampler(impl.device, &sampler_info, nullptr, &impl.clamp_sharp_sampler), "vkCreateSampler",
               error))
        return false;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    if (!check(vkCreateSampler(impl.device, &sampler_info, nullptr, &impl.clamp_sampler), "vkCreateSampler", error))
        return false;

    // Trilinear across the mip chain, with anisotropy so floors and walls seen
    // at a glancing angle keep their detail instead of blurring.
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    sampler_info.anisotropyEnable = impl.max_anisotropy > 1.0f ? VK_TRUE : VK_FALSE;
    sampler_info.maxAnisotropy = impl.max_anisotropy;
    if (!check(vkCreateSampler(impl.device, &sampler_info, nullptr, &impl.mip_sampler), "vkCreateSampler", error))
        return false;
    if (impl.smooth_textures)
        std::cout << "[render] smooth textures: mipmaps, anisotropy " << impl.max_anisotropy << "x\n";

    // Command buffer, synchronization and the vertex staging buffer.
    VkCommandPoolCreateInfo command_pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    command_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    command_pool_info.queueFamilyIndex = impl.queue_family;
    if (!check(vkCreateCommandPool(impl.device, &command_pool_info, nullptr, &impl.command_pool), "vkCreateCommandPool",
               error))
        return false;
    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = impl.command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1u;
    if (!check(vkAllocateCommandBuffers(impl.device, &command_info, &impl.command_buffer), "vkAllocateCommandBuffers",
               error))
        return false;
    if (!check(vkAllocateCommandBuffers(impl.device, &command_info, &impl.shadow_command_buffer),
               "vkAllocateCommandBuffers", error))
        return false;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(impl.device, &fence_info, nullptr, &impl.frame_fence);
    VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(impl.device, &semaphore_info, nullptr, &impl.image_available);
    vkCreateSemaphore(impl.device, &semaphore_info, nullptr, &impl.render_finished);

    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = kVertexBufferBytes;
    buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(vkCreateBuffer(impl.device, &buffer_info, nullptr, &impl.vertex_buffer), "vkCreateBuffer", error))
        return false;
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(impl.device, impl.vertex_buffer, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = impl.find_memory_type(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!check(vkAllocateMemory(impl.device, &allocate, nullptr, &impl.vertex_memory), "vkAllocateMemory", error))
        return false;
    vkBindBufferMemory(impl.device, impl.vertex_buffer, impl.vertex_memory, 0u);
    vkMapMemory(impl.device, impl.vertex_memory, 0u, kVertexBufferBytes, 0u, &impl.vertex_mapped);

    {
        VkPhysicalDeviceProperties device_properties{};
        vkGetPhysicalDeviceProperties(impl.physical_device, &device_properties);
        impl.uniform_alignment = std::max<VkDeviceSize>(device_properties.limits.minUniformBufferOffsetAlignment, 16u);
        VkDescriptorSetAllocateInfo lighting_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        lighting_info.descriptorPool = impl.descriptor_pool;
        lighting_info.descriptorSetCount = 1u;
        lighting_info.pSetLayouts = &impl.lighting_layout;
        if (!check(vkAllocateDescriptorSets(impl.device, &lighting_info, &impl.lighting_descriptor),
                   "vkAllocateDescriptorSets", error))
            return false;
        const std::array<VkDescriptorBufferInfo, 2> lighting_buffers{
            VkDescriptorBufferInfo{impl.vertex_buffer, 0u, sizeof(EnvironmentBlock)},
            VkDescriptorBufferInfo{impl.vertex_buffer, 0u, sizeof(ObjectBlock)},
        };
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = impl.lighting_descriptor;
        write.descriptorCount = 1u;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        std::array<VkWriteDescriptorSet, 2> writes{write, write};
        for (std::uint32_t i = 0; i < 2u; ++i) {
            writes[i].dstBinding = i;
            writes[i].pBufferInfo = &lighting_buffers[i];
        }
        vkUpdateDescriptorSets(impl.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0u, nullptr);
    }

    const std::uint32_t white = 0xFFFFFFFFu;
    impl.white_texture = impl.create_texture(1u, 1u, &white);

    // Shadow maps cast from the game's own lights. Optional in every sense:
    // without them the scene draws exactly as it did before, and the setting
    // defaults to off. MGA_NO_SHADOW_MAP skips building it.
    static const bool no_shadow_map = std::getenv("MGA_NO_SHADOW_MAP") != nullptr;
    if (!no_shadow_map) {
        std::string shadow_error;
        const ShadowMap::DeviceContext shadow_context{impl.device, impl.physical_device};
        impl.shadow_map = std::make_unique<ShadowMap>();
        if (impl.shadow_map->create(shadow_context, impl.shadow_resolution, kShadowVertexShader,
                                    sizeof(kShadowVertexShader), shadow_error)) {
            impl.shadow_available = true;
            // It can be sampled before it has ever been drawn into, so it has
            // to start in the layout its descriptor claims.
            impl.run_commands([&](VkCommandBuffer commands) {
                impl.transition(commands, impl.shadow_map->image(), VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            });
        } else {
            std::cout << "[render] shadow maps unavailable: " << shadow_error << "\n";
        }
    }
    impl.bind_shadow_map();

    // The overlay is optional: without it the game still runs, only unmeasured
    // on screen.
    std::string overlay_error;
    impl.overlay_ready = impl.create_overlay(overlay_error);
    if (!impl.overlay_ready) std::cout << "[perf] overlay unavailable: " << overlay_error << "\n";
    impl.overlay_visible = impl.overlay_ready && perf::options().overlay;

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(impl.physical_device, &properties);
    impl.device_name = properties.deviceName;
    std::cout << "Renderer: Vulkan on " << properties.deviceName << ", target " << impl.target_extent.width << "x"
              << impl.target_extent.height << "\n";
    impl.ready = true;
    return true;
}

bool VulkanRenderer::Impl::create_overlay(std::string &error) {
    if (!create_image(perf::kOverlayWidth, perf::kOverlayHeight, VK_FORMAT_R8G8B8A8_UNORM,
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, overlay_image, overlay_memory,
                      overlay_view, VK_IMAGE_ASPECT_COLOR_BIT, error))
        return false;
    overlay_pixels.assign(static_cast<std::size_t>(perf::kOverlayWidth) * perf::kOverlayHeight, 0u);
    const VkDeviceSize bytes = overlay_pixels.size() * sizeof(std::uint32_t);
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = bytes;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(vkCreateBuffer(device, &buffer_info, nullptr, &overlay_staging), "vkCreateBuffer", error)) return false;
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, overlay_staging, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = find_memory_type(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!check(vkAllocateMemory(device, &allocate, nullptr, &overlay_staging_memory), "vkAllocateMemory", error))
        return false;
    vkBindBufferMemory(device, overlay_staging, overlay_staging_memory, 0u);
    return check(vkMapMemory(device, overlay_staging_memory, 0u, bytes, 0u, &overlay_mapped), "vkMapMemory", error);
}

// Draws the overlay over the top-left corner of the swapchain image, which is
// in TRANSFER_DST layout with the game frame already blitted into it. The
// staging buffer is free to rewrite: the frame fence has been waited on before
// any recording of this frame started.
void VulkanRenderer::Impl::record_overlay(VkImage destination) {
    const std::uint32_t scale = perf::overlay_scale(swapchain_extent.height);
    const std::uint32_t inset = 4u * scale;
    const std::uint32_t width = perf::kOverlayWidth * scale;
    const std::uint32_t height = perf::kOverlayHeight * scale;
    if (inset + width > swapchain_extent.width || inset + height > swapchain_extent.height) return;

    perf::draw_overlay(overlay_pixels.data());
    std::memcpy(overlay_mapped, overlay_pixels.data(), overlay_pixels.size() * sizeof(std::uint32_t));
    transition(command_buffer, overlay_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
    copy.imageExtent = {perf::kOverlayWidth, perf::kOverlayHeight, 1u};
    vkCmdCopyBufferToImage(command_buffer, overlay_staging, overlay_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u,
                           &copy);
    transition(command_buffer, overlay_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    // The game frame was blitted into the same image just before; order the
    // two writes.
    transition(command_buffer, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
    blit.srcOffsets[1] = {static_cast<std::int32_t>(perf::kOverlayWidth),
                          static_cast<std::int32_t>(perf::kOverlayHeight), 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[0] = {static_cast<std::int32_t>(inset), static_cast<std::int32_t>(inset), 0};
    blit.dstOffsets[1] = {static_cast<std::int32_t>(inset + width), static_cast<std::int32_t>(inset + height), 1};
    vkCmdBlitImage(command_buffer, overlay_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &blit, VK_FILTER_NEAREST);
}

void VulkanRenderer::Impl::update_display_info() {
    float refresh = 0.0f;
    if (const SDL_DisplayID display = SDL_GetDisplayForWindow(window); display != 0u) {
        if (const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(display); mode != nullptr)
            refresh = mode->refresh_rate;
    }
    perf::set_display_info(present_mode_name(present_mode), swapchain_extent.width, swapchain_extent.height, refresh);
}

VkPresentModeKHR VulkanRenderer::Impl::wanted_present_mode() const {
    VkPresentModeKHR wanted = VK_PRESENT_MODE_FIFO_KHR;
    if (requested_present == settings::PresentMode::Mailbox) wanted = VK_PRESENT_MODE_MAILBOX_KHR;
    if (requested_present == settings::PresentMode::Immediate) wanted = VK_PRESENT_MODE_IMMEDIATE_KHR;
    // FIFO is the one mode every surface supports.
    return std::find(present_modes.begin(), present_modes.end(), wanted) != present_modes.end()
               ? wanted
               : VK_PRESENT_MODE_FIFO_KHR;
}

// Builds the swapchain for the window's current size, replacing the old one.
bool VulkanRenderer::Impl::create_swapchain(std::string &error) {
    VkSurfaceCapabilitiesKHR capabilities{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &capabilities);
    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        extent.width = std::clamp(static_cast<std::uint32_t>(std::max(width, 0)), capabilities.minImageExtent.width,
                                  capabilities.maxImageExtent.width);
        extent.height = std::clamp(static_cast<std::uint32_t>(std::max(height, 0)),
                                   capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    if (extent.width == 0u || extent.height == 0u) extent = target_extent;
    swapchain_extent = extent;
    present_mode = wanted_present_mode();

    std::uint32_t image_count =
        std::max(capabilities.minImageCount, present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? 3u : 2u);
    if (capabilities.maxImageCount != 0u) image_count = std::min(image_count, capabilities.maxImageCount);
    // Transfer source only where offered: it is just for window captures.
    swapchain_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    const VkSwapchainKHR old_swapchain = swapchain;
    VkSwapchainCreateInfoKHR swapchain_info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swapchain_info.surface = surface;
    swapchain_info.minImageCount = image_count;
    swapchain_info.imageFormat = swapchain_format;
    swapchain_info.imageColorSpace = surface_format.colorSpace;
    swapchain_info.imageExtent = swapchain_extent;
    swapchain_info.imageArrayLayers = 1u;
    swapchain_info.imageUsage = swapchain_usage;
    swapchain_info.preTransform = capabilities.currentTransform;
    swapchain_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchain_info.presentMode = present_mode;
    swapchain_info.clipped = VK_TRUE;
    swapchain_info.oldSwapchain = old_swapchain;
    VkSwapchainKHR created{};
    const VkResult result = vkCreateSwapchainKHR(device, &swapchain_info, nullptr, &created);
    if (old_swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(device, old_swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
    if (!check(result, "vkCreateSwapchainKHR", error)) return false;
    swapchain = created;
    swapchain_min_images = image_count;

    std::uint32_t count = 0u;
    vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
    swapchain_images.resize(count);
    vkGetSwapchainImagesKHR(device, swapchain, &count, swapchain_images.data());
    for (VkImage image : swapchain_images) {
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = swapchain_format;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
        VkImageView view{};
        if (!check(vkCreateImageView(device, &view_info, nullptr, &view), "vkCreateImageView", error)) return false;
        swapchain_views.push_back(view);
    }
    if (ui_render_pass != VK_NULL_HANDLE && !create_ui_framebuffers(error)) return false;
    if (post_render_pass != VK_NULL_HANDLE && !create_post_framebuffers(error)) return false;
    swapchain_dirty = false;
    update_display_info();
    return true;
}

void VulkanRenderer::Impl::destroy_swapchain_views() {
    for (VkFramebuffer framebuffer : ui_framebuffers) vkDestroyFramebuffer(device, framebuffer, nullptr);
    ui_framebuffers.clear();
    for (VkFramebuffer framebuffer : post_framebuffers) vkDestroyFramebuffer(device, framebuffer, nullptr);
    post_framebuffers.clear();
    for (VkImageView view : swapchain_views) vkDestroyImageView(device, view, nullptr);
    swapchain_views.clear();
}

void VulkanRenderer::Impl::recreate_swapchain() {
    // A minimised window has no area to present to; keep the old swapchain
    // and try again once it has one.
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    if (width <= 0 || height <= 0) return;
    vkDeviceWaitIdle(device);
    destroy_swapchain_views();
    const std::uint32_t previous_min_images = swapchain_min_images;
    std::string error;
    if (!create_swapchain(error)) {
        std::cout << "[render] cannot recreate the swapchain: " << error << "\n";
        return;
    }
    if (ui_ready && swapchain_min_images != previous_min_images) ImGui_ImplVulkan_SetMinImageCount(swapchain_min_images);
}

bool VulkanRenderer::Impl::create_ui_framebuffers(std::string &error) {
    for (VkImageView view : swapchain_views) {
        VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        info.renderPass = ui_render_pass;
        info.attachmentCount = 1u;
        info.pAttachments = &view;
        info.width = swapchain_extent.width;
        info.height = swapchain_extent.height;
        info.layers = 1u;
        VkFramebuffer framebuffer{};
        if (!check(vkCreateFramebuffer(device, &info, nullptr, &framebuffer), "vkCreateFramebuffer", error))
            return false;
        ui_framebuffers.push_back(framebuffer);
    }
    return true;
}

// The post-processing pass draws into the swapchain image and leaves it in
// TRANSFER_DST, which is the layout the blit used to leave behind, so
// everything after it -- the performance overlay, the interface, the window
// capture -- is unchanged.
bool VulkanRenderer::Impl::create_post_pipeline(std::string &error) {
    VkAttachmentDescription attachment{};
    attachment.format = swapchain_format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    // Every pixel is written: the shader fills the letterbox with black
    // rather than relying on what was there.
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkAttachmentReference reference{0u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1u;
    subpass.pColorAttachments = &reference;
    // The scene target is sampled here, so its writes have to be finished.
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0u;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    // Every access bit has to be one the stages it is paired with can perform:
    // COLOR_ATTACHMENT_WRITE belongs to the colour output stage and
    // TRANSFER_WRITE to the transfer stage, and naming a stage without its
    // access bit is a specification violation the layers will report.
    dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass_info.attachmentCount = 1u;
    pass_info.pAttachments = &attachment;
    pass_info.subpassCount = 1u;
    pass_info.pSubpasses = &subpass;
    pass_info.dependencyCount = 1u;
    pass_info.pDependencies = &dependency;
    if (!check(vkCreateRenderPass(device, &pass_info, nullptr, &post_render_pass), "vkCreateRenderPass", error))
        return false;

    const auto create_shader = [&](const std::uint32_t *code, std::size_t size, VkShaderModule &module) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = size;
        info.pCode = code;
        return check(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule", error);
    };
    if (!create_shader(kPostVertexShader, sizeof(kPostVertexShader), post_vertex_shader)) return false;
    if (!create_shader(kPostFragmentShader, sizeof(kPostFragmentShader), post_fragment_shader)) return false;

    // Binding 0 is the finished colour, binding 1 the depth it was drawn with.
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (std::uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1u;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layout_info.pBindings = bindings.data();
    if (!check(vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &post_set_layout),
               "vkCreateDescriptorSetLayout", error))
        return false;

    VkPushConstantRange push_range{VK_SHADER_STAGE_FRAGMENT_BIT, 0u, sizeof(PostPush)};
    VkPipelineLayoutCreateInfo pipeline_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout_info.setLayoutCount = 1u;
    pipeline_layout_info.pSetLayouts = &post_set_layout;
    pipeline_layout_info.pushConstantRangeCount = 1u;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    if (!check(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &post_pipeline_layout),
               "vkCreatePipelineLayout", error))
        return false;

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = post_vertex_shader;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = post_fragment_shader;
    stages[1].pName = "main";

    // The fullscreen triangle's positions come from gl_VertexIndex, so there
    // is no vertex buffer and no vertex input state to describe.
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1u;
    viewport_state.scissorCount = 1u;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = 1u;
    blending.pAttachments = &blend;
    const std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = static_cast<std::uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blending;
    info.pDynamicState = &dynamic;
    info.layout = post_pipeline_layout;
    info.renderPass = post_render_pass;
    return check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1u, &info, nullptr, &post_pipeline),
                 "vkCreateGraphicsPipelines", error);
}

bool VulkanRenderer::Impl::create_post_framebuffers(std::string &error) {
    for (VkImageView view : swapchain_views) {
        VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        info.renderPass = post_render_pass;
        info.attachmentCount = 1u;
        info.pAttachments = &view;
        info.width = swapchain_extent.width;
        info.height = swapchain_extent.height;
        info.layers = 1u;
        VkFramebuffer framebuffer{};
        if (!check(vkCreateFramebuffer(device, &info, nullptr, &framebuffer), "vkCreateFramebuffer", error))
            return false;
        post_framebuffers.push_back(framebuffer);
    }
    return true;
}

// Points binding 2 of the lighting set at the shadow map, or at the 1x1 white
// texture when there is nothing to sample. A descriptor a shader can reach has
// to hold something valid whether or not the shader reads it.
void VulkanRenderer::Impl::bind_shadow_map() {
    if (lighting_descriptor == VK_NULL_HANDLE) return;
    const bool use_map =
        shadow_available && shadow_strength > 0.0f && shadow_map != nullptr && shadow_map->view() != VK_NULL_HANDLE;
    const VkImageView wanted = use_map ? shadow_map->view() : white_texture.view;
    if (wanted == VK_NULL_HANDLE || wanted == shadow_bound_view) return;
    // Rewriting a set that a queued frame may still be reading is not allowed,
    // and this happens only when the setting changes.
    if (shadow_bound_view != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
    // Nearest, always: a depth format is not required to support linear
    // filtering, and the shader takes its own nine samples anyway.
    VkDescriptorImageInfo image{clamp_sharp_sampler, wanted, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = lighting_descriptor;
    write.dstBinding = 2u;
    write.descriptorCount = 1u;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device, 1u, &write, 0u, nullptr);
    shadow_bound_view = wanted;
}

void VulkanRenderer::Impl::drop_post_descriptors() {
    if (post_descriptors.empty()) return;
    // Rare -- a settings change or a target being rebuilt -- so waiting is
    // cheaper than tracking which frame each set was last used by.
    vkDeviceWaitIdle(device);
    for (auto &[view, set] : post_descriptors) {
        (void)view;
        if (set != VK_NULL_HANDLE) vkFreeDescriptorSets(device, descriptor_pool, 1u, &set);
    }
    post_descriptors.clear();
}

VkDescriptorSet VulkanRenderer::Impl::post_descriptor_for(VkImageView color, VkImageView depth) {
    if (color == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    // A set is kept under its colour view, so anything else that decides what
    // it holds has to invalidate the whole cache when it changes -- otherwise
    // a set built while there was no depth to bind would be reused once
    // there is, and the shader would read colour as depth.
    const bool has_depth = depth != VK_NULL_HANDLE;
    if (post_descriptors_sharp != sharp_screen || post_descriptors_depth != has_depth) drop_post_descriptors();
    post_descriptors_depth = has_depth;
    if (const auto found = post_descriptors.find(color); found != post_descriptors.end()) return found->second;
    if (post_descriptors.size() >= kMaxPostSets) drop_post_descriptors();
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = descriptor_pool;
    allocate.descriptorSetCount = 1u;
    allocate.pSetLayouts = &post_set_layout;
    VkDescriptorSet set{};
    if (vkAllocateDescriptorSets(device, &allocate, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    // A held frame is a plain colour copy with no depth of its own. Binding
    // the colour twice keeps the set complete; the shader is told the
    // strength is zero, so it never reads it.
    //
    // Depth is always sampled with the nearest filter, never the screen's.
    // A 32-bit float depth format is not required to support linear
    // filtering, and most drivers do not offer it, so asking for it would be
    // undefined -- and a depth buffer has nothing to gain from it anyway.
    const VkSampler chosen = sharp_screen ? clamp_sharp_sampler : clamp_sampler;
    std::array<VkDescriptorImageInfo, 2> images{};
    images[0] = {chosen, color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    images[1] = {clamp_sharp_sampler, depth != VK_NULL_HANDLE ? depth : color,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (std::uint32_t i = 0; i < writes.size(); ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1u;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0u, nullptr);
    post_descriptors_sharp = sharp_screen;
    post_descriptors.emplace(color, set);
    return set;
}

void VulkanRenderer::Impl::record_post(VkImageView color, VkImageView depth, std::uint32_t image_index) {
    if (image_index >= post_framebuffers.size()) return;
    const VkDescriptorSet set = post_descriptor_for(color, depth);
    if (set == VK_NULL_HANDLE) return;

    const auto width = static_cast<float>(swapchain_extent.width);
    const auto height = static_cast<float>(swapchain_extent.height);
    PostPush push{};
    push.rect = {0.0f, 0.0f, 1.0f, 1.0f};
    // The same rectangle record_game_blit works out, in normalised
    // coordinates, so switching the pass on does not nudge the picture.
    if (keep_aspect && width > 0.0f && height > 0.0f) {
        const float scale = std::min(width / static_cast<float>(kPspWidth), height / static_cast<float>(kPspHeight));
        const float shown_width = std::min(std::max(1.0f, std::round(kPspWidth * scale)), width);
        const float shown_height = std::min(std::max(1.0f, std::round(kPspHeight * scale)), height);
        push.rect = {std::floor((width - shown_width) * 0.5f) / width,
                     std::floor((height - shown_height) * 0.5f) / height, shown_width / width,
                     shown_height / height};
    }
    push.texel = {1.0f / static_cast<float>(std::max(target_extent.width, 1u)),
                  1.0f / static_cast<float>(std::max(target_extent.height, 1u))};
    push.effects = {post_fxaa ? 1.0f : 0.0f, 0.0f};
    // With a reversed range the buffer holds 0 at the far plane and a larger
    // value means nearer; with the ordinary range it is the other way round.
    // The tap radius follows the internal resolution so the shadow keeps the
    // same size on screen however far the render target is scaled up.
    const float radius = std::max(1.5f, static_cast<float>(target_extent.height) / 272.0f * 1.5f);
    push.depth = {depth_reversed ? 1.0f : -1.0f, radius, depth_reversed ? 0.0f : 1.0f,
                  depth != VK_NULL_HANDLE ? post_ao : 0.0f};

    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = post_render_pass;
    pass.framebuffer = post_framebuffers[image_index];
    pass.renderArea = {{0, 0}, swapchain_extent};
    vkCmdBeginRenderPass(command_buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0.0f, 0.0f, width, height, 0.0f, 1.0f};
    vkCmdSetViewport(command_buffer, 0u, 1u, &viewport);
    const VkRect2D scissor{{0, 0}, swapchain_extent};
    vkCmdSetScissor(command_buffer, 0u, 1u, &scissor);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, post_pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, post_pipeline_layout, 0u, 1u, &set, 0u,
                            nullptr);
    vkCmdPushConstants(command_buffer, post_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0u, sizeof(push), &push);
    vkCmdDraw(command_buffer, 3u, 1u, 0u, 0u);
    vkCmdEndRenderPass(command_buffer);
    // The scene pipeline and its descriptor bindings are no longer current.
    bound_pipeline = VK_NULL_HANDLE;
    lighting_bound = false;
}

// Scales the game frame onto the swapchain image, which is in TRANSFER_DST
// layout: stretched over the whole window, or at the PSP's aspect ratio with
// black bars.
void VulkanRenderer::Impl::record_game_blit(VkImage source, VkImage destination) {
    const auto width = static_cast<std::int32_t>(swapchain_extent.width);
    const auto height = static_cast<std::int32_t>(swapchain_extent.height);
    VkOffset3D low{0, 0, 0};
    VkOffset3D high{width, height, 1};
    if (keep_aspect) {
        const double scale = std::min(static_cast<double>(width) / kPspWidth, static_cast<double>(height) / kPspHeight);
        const auto shown_width = std::clamp(static_cast<std::int32_t>(std::lround(kPspWidth * scale)), 1, width);
        const auto shown_height = std::clamp(static_cast<std::int32_t>(std::lround(kPspHeight * scale)), 1, height);
        low = {(width - shown_width) / 2, (height - shown_height) / 2, 0};
        high = {low.x + shown_width, low.y + shown_height, 1};
        if (shown_width < width || shown_height < height) {
            const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
            vkCmdClearColorImage(command_buffer, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1u, &range);
            // Order the clear before the blit into the same image.
            transition(command_buffer, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        }
    }
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
    blit.srcOffsets[1] = {static_cast<std::int32_t>(target_extent.width),
                          static_cast<std::int32_t>(target_extent.height), 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[0] = low;
    blit.dstOffsets[1] = high;
    vkCmdBlitImage(command_buffer, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &blit, sharp_screen ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
}

// Finishes the frame being recorded: the game frame (or a plain background
// when `source` is null), the performance overlay on game frames, the
// interface, then submit and present.
void VulkanRenderer::Impl::submit_and_present(const Target *source_target, bool game_frame) {
    const VkImage source = source_target != nullptr ? source_target->color : VK_NULL_HANDLE;
    const VkImageView source_view = source_target != nullptr ? source_target->color_view : VK_NULL_HANDLE;
    const VkImage source_depth = source_target != nullptr ? source_target->depth : VK_NULL_HANDLE;
    const VkImageView source_depth_view = source_target != nullptr ? source_target->depth_view : VK_NULL_HANDLE;
    if (swapchain_dirty || swapchain == VK_NULL_HANDLE) recreate_swapchain();
    ImDrawData *ui = ui_ready ? ui_draw_data : nullptr;
    ui_draw_data = nullptr;
    const bool draw_ui = ui != nullptr && ui->CmdListsCount > 0;
    // A game flip before anything was drawn has nothing to show.
    const bool has_content = source != VK_NULL_HANDLE || !game_frame || draw_ui;

    std::uint32_t image_index = 0u;
    VkResult acquired = VK_ERROR_OUT_OF_DATE_KHR;
    if (has_content && swapchain != VK_NULL_HANDLE) {
        const perf::Clock::time_point acquire_start = perf::Clock::now();
        acquired = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, image_available, VK_NULL_HANDLE, &image_index);
        perf::add_wait_time(perf::Clock::now() - acquire_start);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) swapchain_dirty = true;
    }
    const bool can_present = acquired == VK_SUCCESS || acquired == VK_SUBOPTIMAL_KHR;
    bool capture = false;
    if (can_present) {
        VkImage target = swapchain_images[image_index];
        transition(command_buffer, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (source != VK_NULL_HANDLE && post_active() && source_view != VK_NULL_HANDLE) {
            // Whenever the target has a depth image, not only when the
            // effect that reads it is switched on: the descriptor set is
            // built once and would otherwise have to be rebuilt the moment
            // the setting changed.
            const bool read_depth = source_depth != VK_NULL_HANDLE;
            transition(command_buffer, source, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if (read_depth)
                transition(command_buffer, source_depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            record_post(source_view, read_depth ? source_depth_view : VK_NULL_HANDLE, image_index);
            if (read_depth)
                transition(command_buffer, source_depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            transition(command_buffer, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        } else if (source != VK_NULL_HANDLE) {
            transition(command_buffer, source, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            record_game_blit(source, target);
            transition(command_buffer, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        } else {
            // Behind the setup screens: the dark brown of the project's logo.
            const VkClearColorValue background{{0.075f, 0.055f, 0.045f, 1.0f}};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
            vkCmdClearColorImage(command_buffer, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &background, 1u,
                                 &range);
        }
        if (game_frame && overlay_visible) {
            const perf::Clock::time_point overlay_start = perf::Clock::now();
            record_overlay(target);
            perf::add_overlay_time(perf::Clock::now() - overlay_start);
        }
        VkImageLayout layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        if (draw_ui && image_index < ui_framebuffers.size()) {
            transition(command_buffer, target, layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            pass.renderPass = ui_render_pass;
            pass.framebuffer = ui_framebuffers[image_index];
            pass.renderArea = {{0, 0}, swapchain_extent};
            vkCmdBeginRenderPass(command_buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
            ImGui_ImplVulkan_RenderDrawData(ui, command_buffer);
            vkCmdEndRenderPass(command_buffer);
        }
        if (!capture_path.empty() && (swapchain_usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0u) {
            const VkDeviceSize bytes = static_cast<VkDeviceSize>(swapchain_extent.width) * swapchain_extent.height * 4u;
            VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            buffer_info.size = bytes;
            buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            if (vkCreateBuffer(device, &buffer_info, nullptr, &capture_buffer) == VK_SUCCESS) {
                VkMemoryRequirements requirements{};
                vkGetBufferMemoryRequirements(device, capture_buffer, &requirements);
                VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                allocate.allocationSize = requirements.size;
                allocate.memoryTypeIndex =
                    find_memory_type(requirements.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                vkAllocateMemory(device, &allocate, nullptr, &capture_memory);
                vkBindBufferMemory(device, capture_buffer, capture_memory, 0u);
                transition(command_buffer, target, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
                copy.imageExtent = {swapchain_extent.width, swapchain_extent.height, 1u};
                vkCmdCopyImageToBuffer(command_buffer, target, layout, capture_buffer, 1u, &copy);
                capture_extent = swapchain_extent;
                capture = true;
            }
        }
        transition(command_buffer, target, layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    // The casters are known by now, so the shadow pass can be recorded -- into
    // its own buffer, which is submitted first and therefore runs before the
    // scene draws that sample it.
    if (shadow_available) {
        shadow_recorded = shadow_map->record(shadow_command_buffer);
        vkEndCommandBuffer(shadow_command_buffer);
    }
    vkEndCommandBuffer(command_buffer);

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const std::array<VkCommandBuffer, 2> buffers{shadow_command_buffer, command_buffer};
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = shadow_available ? 2u : 1u;
    submit.pCommandBuffers = shadow_available ? buffers.data() : &command_buffer;
    if (can_present) {
        submit.waitSemaphoreCount = 1u;
        submit.pWaitSemaphores = &image_available;
        submit.pWaitDstStageMask = &wait_stage;
        submit.signalSemaphoreCount = 1u;
        submit.pSignalSemaphores = &render_finished;
    }
    // MoltenVK waits for the next drawable here rather than in the acquire.
    const perf::Clock::time_point submit_start = perf::Clock::now();
    vkQueueSubmit(queue, 1u, &submit, frame_fence);
    perf::add_wait_time(perf::Clock::now() - submit_start);

    if (can_present) {
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1u;
        present.pWaitSemaphores = &render_finished;
        present.swapchainCount = 1u;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &image_index;
        const perf::Clock::time_point present_start = perf::Clock::now();
        const VkResult presented = vkQueuePresentKHR(queue, &present);
        perf::add_wait_time(perf::Clock::now() - present_start);
        if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) swapchain_dirty = true;
    }
    recording = false;
    if (capture) write_capture();
}

// Writes the image copied by submit_and_present once its frame has finished.
void VulkanRenderer::Impl::write_capture() {
    vkWaitForFences(device, 1u, &frame_fence, VK_TRUE, UINT64_MAX);
    void *mapped = nullptr;
    vkMapMemory(device, capture_memory, 0u, VK_WHOLE_SIZE, 0u, &mapped);
    const bool bgra = swapchain_format == VK_FORMAT_B8G8R8A8_UNORM || swapchain_format == VK_FORMAT_B8G8R8A8_SRGB;
    if (write_bmp(capture_path, static_cast<const std::uint8_t *>(mapped), capture_extent.width,
                  capture_extent.height, bgra))
        std::cout << "[render] window " << capture_extent.width << "x" << capture_extent.height << " -> "
                  << capture_path << std::endl;
    else
        std::cout << "[render] cannot write " << capture_path << std::endl;
    vkUnmapMemory(device, capture_memory);
    vkDestroyBuffer(device, capture_buffer, nullptr);
    vkFreeMemory(device, capture_memory, nullptr);
    capture_buffer = VK_NULL_HANDLE;
    capture_memory = VK_NULL_HANDLE;
    capture_path.clear();
}

void VulkanRenderer::Impl::destroy_target(Target &target) {
    // A descriptor set still pointing at this target's view would outlive it,
    // and a later view can be handed back the same handle value.
    if (target.color_view != VK_NULL_HANDLE && post_descriptors.count(target.color_view) != 0u)
        drop_post_descriptors();
    for (VkDescriptorSet &descriptor : target.copy_descriptors)
        if (descriptor != VK_NULL_HANDLE) vkFreeDescriptorSets(device, descriptor_pool, 1u, &descriptor);
    vkDestroyImageView(device, target.copy_opaque_view, nullptr);
    vkDestroyImageView(device, target.copy_view, nullptr);
    vkDestroyImage(device, target.copy, nullptr);
    vkFreeMemory(device, target.copy_memory, nullptr);
    vkDestroyFramebuffer(device, target.framebuffer, nullptr);
    vkDestroyImageView(device, target.depth_view, nullptr);
    vkDestroyImage(device, target.depth, nullptr);
    vkFreeMemory(device, target.depth_memory, nullptr);
    vkDestroyImageView(device, target.color_view, nullptr);
    vkDestroyImage(device, target.color, nullptr);
    vkFreeMemory(device, target.color_memory, nullptr);
    target = Target{};
}

// Records commands into a one-time buffer and waits for them to finish.
void VulkanRenderer::Impl::run_commands(const std::function<void(VkCommandBuffer)> &record) {
    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1u;
    VkCommandBuffer commands{};
    vkAllocateCommandBuffers(device, &command_info, &commands);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commands, &begin);
    record(commands);
    vkEndCommandBuffer(commands);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &commands;
    vkQueueSubmit(queue, 1u, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, command_pool, 1u, &commands);
}

VulkanRenderer::Impl::Target *VulkanRenderer::Impl::target_for(std::uint32_t address, std::string &error) {
    const auto found = targets.find(address);
    if (found != targets.end()) return &found->second;

    Target target{};
    // SAMPLED so the post-processing pass can read the finished frame as a
    // texture instead of blitting it, which is what lets an effect look at more
    // than one texel at a time.
    if (!create_image(target_extent.width, target_extent.height, VK_FORMAT_R8G8B8A8_UNORM,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      target.color,
                      target.color_memory, target.color_view, VK_IMAGE_ASPECT_COLOR_BIT, error))
        return nullptr;
    // SAMPLED too: the post-processing pass reads the depth to find the
    // creases where geometry meets, which is where contact shadows go.
    if (!create_image(target_extent.width, target_extent.height, VK_FORMAT_D32_SFLOAT,
                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, target.depth,
                      target.depth_memory, target.depth_view, VK_IMAGE_ASPECT_DEPTH_BIT, error))
        return nullptr;
    const std::array<VkImageView, 2> views{target.color_view, target.depth_view};
    VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    info.renderPass = render_pass;
    info.attachmentCount = static_cast<std::uint32_t>(views.size());
    info.pAttachments = views.data();
    info.width = target_extent.width;
    info.height = target_extent.height;
    info.layers = 1u;
    if (!check(vkCreateFramebuffer(device, &info, nullptr, &target.framebuffer), "vkCreateFramebuffer", error))
        return nullptr;
    static const bool trace = std::getenv("MGA_TRACE_FB_TEXTURES") != nullptr;
    if (trace) std::cout << "[fbtex] frame " << frames << " new render target 0x" << std::hex << address << std::dec << "\n";
    return &targets.emplace(address, target).first->second;
}

void VulkanRenderer::Impl::destroy_upload() {
    if (upload_mapped != nullptr) vkUnmapMemory(device, upload_staging_memory);
    vkDestroyBuffer(device, upload_staging, nullptr);
    vkFreeMemory(device, upload_staging_memory, nullptr);
    vkDestroyImageView(device, upload_view, nullptr);
    vkDestroyImage(device, upload_image, nullptr);
    vkFreeMemory(device, upload_memory, nullptr);
    upload_mapped = nullptr;
    upload_staging = VK_NULL_HANDLE;
    upload_staging_memory = VK_NULL_HANDLE;
    upload_view = VK_NULL_HANDLE;
    upload_image = VK_NULL_HANDLE;
    upload_memory = VK_NULL_HANDLE;
    upload_extent = {};
}

bool VulkanRenderer::Impl::create_upload(std::uint32_t width, std::uint32_t height, std::string &error) {
    destroy_upload();
    if (!create_image(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, upload_image, upload_memory,
                      upload_view, VK_IMAGE_ASPECT_COLOR_BIT, error))
        return false;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4u;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = bytes;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(vkCreateBuffer(device, &buffer_info, nullptr, &upload_staging), "vkCreateBuffer", error)) return false;
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, upload_staging, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = find_memory_type(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!check(vkAllocateMemory(device, &allocate, nullptr, &upload_staging_memory), "vkAllocateMemory", error))
        return false;
    vkBindBufferMemory(device, upload_staging, upload_staging_memory, 0u);
    if (!check(vkMapMemory(device, upload_staging_memory, 0u, bytes, 0u, &upload_mapped), "vkMapMemory", error))
        return false;
    upload_extent = {width, height};
    return true;
}

void VulkanRenderer::Impl::begin_pass(std::uint32_t address) {
    std::string error;
    Target *target = target_for(address, error);
    if (target == nullptr) return;
    if (!target->initialized) {
        // Attachments are loaded, not cleared, so a new target starts undefined:
        // move it into the layouts the render pass expects once.
        transition(command_buffer, target->color, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        transition(command_buffer, target->depth, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        target->initialized = true;
    }
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = render_pass;
    pass.framebuffer = target->framebuffer;
    pass.renderArea = {{0, 0}, target_extent};
    vkCmdBeginRenderPass(command_buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
    forget_bindings();
    current_target = address;
    last_drawn_target = address;
    pass_active = true;
}

void VulkanRenderer::Impl::end_pass() {
    if (!pass_active) return;
    vkCmdEndRenderPass(command_buffer);
    pass_active = false;
}

VulkanRenderer::Impl::Texture VulkanRenderer::Impl::create_texture(std::uint32_t width, std::uint32_t height,
                                                                   const std::uint32_t *pixels) {
    Texture texture{};
    std::string error;
    const std::uint32_t levels = mip_levels_for(width, height);
    // Generating the chain blits level n to level n + 1, so the image is a
    // transfer source as well.
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (levels > 1u) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!create_image(width, height, VK_FORMAT_R8G8B8A8_UNORM, usage, texture.image, texture.memory, texture.view,
                      VK_IMAGE_ASPECT_COLOR_BIT, error, levels))
        return texture;

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4u;
    VkBuffer staging{};
    VkDeviceMemory staging_memory{};
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = bytes;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkCreateBuffer(device, &buffer_info, nullptr, &staging);
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, staging, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = find_memory_type(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(device, &allocate, nullptr, &staging_memory);
    vkBindBufferMemory(device, staging, staging_memory, 0u);
    void *mapped = nullptr;
    vkMapMemory(device, staging_memory, 0u, bytes, 0u, &mapped);
    std::memcpy(mapped, pixels, static_cast<std::size_t>(bytes));
    vkUnmapMemory(device, staging_memory);

    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1u;
    VkCommandBuffer commands{};
    vkAllocateCommandBuffers(device, &command_info, &commands);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commands, &begin);
    transition(commands, texture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_ASPECT_COLOR_BIT, levels);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
    copy.imageExtent = {width, height, 1u};
    vkCmdCopyBufferToImage(commands, staging, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);
    // Each level is halved from the one above it. Level n has to finish being
    // written, and become a transfer source, before level n + 1 reads it.
    std::int32_t level_width = static_cast<std::int32_t>(width);
    std::int32_t level_height = static_cast<std::int32_t>(height);
    for (std::uint32_t level = 1u; level < levels; ++level) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = texture.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1u, 1u, 0u, 1u};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr,
                             0u, nullptr, 1u, &barrier);

        const std::int32_t next_width = std::max(1, level_width / 2);
        const std::int32_t next_height = std::max(1, level_height / 2);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1u, 0u, 1u};
        blit.srcOffsets[1] = {level_width, level_height, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0u, 1u};
        blit.dstOffsets[1] = {next_width, next_height, 1};
        vkCmdBlitImage(commands, texture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, texture.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &blit, VK_FILTER_LINEAR);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0u, 0u,
                             nullptr, 0u, nullptr, 1u, &barrier);
        level_width = next_width;
        level_height = next_height;
    }
    // The last level is still a transfer destination.
    {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = texture.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, levels - 1u, 1u, 0u, 1u};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0u, 0u,
                             nullptr, 0u, nullptr, 1u, &barrier);
    }
    vkEndCommandBuffer(commands);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &commands;
    // Uploads are synchronous: the GPU finishes everything queued before this
    // returns, which counts as waiting rather than rendering.
    const perf::Clock::time_point wait_start = perf::Clock::now();
    vkQueueSubmit(queue, 1u, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    perf::add_wait_time(perf::Clock::now() - wait_start);
    vkFreeCommandBuffers(device, command_pool, 1u, &commands);
    vkDestroyBuffer(device, staging, nullptr);
    vkFreeMemory(device, staging_memory, nullptr);

    VkDescriptorSetAllocateInfo descriptor_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    descriptor_info.descriptorPool = descriptor_pool;
    descriptor_info.descriptorSetCount = 1u;
    descriptor_info.pSetLayouts = &descriptor_layout;
    vkAllocateDescriptorSets(device, &descriptor_info, &texture.descriptor);
    VkDescriptorImageInfo image_info{texture_sampler(), texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = texture.descriptor;
    write.descriptorCount = 1u;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(device, 1u, &write, 0u, nullptr);
    texture.last_used = ++texture_clock;
    return texture;
}

void VulkanRenderer::Impl::destroy_texture(Texture &texture) {
    if (texture.descriptor != VK_NULL_HANDLE) vkFreeDescriptorSets(device, descriptor_pool, 1u, &texture.descriptor);
    if (texture.view != VK_NULL_HANDLE) vkDestroyImageView(device, texture.view, nullptr);
    if (texture.image != VK_NULL_HANDLE) vkDestroyImage(device, texture.image, nullptr);
    if (texture.memory != VK_NULL_HANDLE) vkFreeMemory(device, texture.memory, nullptr);
    texture = Texture{};
}

VulkanRenderer::Impl::Texture &VulkanRenderer::Impl::texture_for(const GuestMemory &memory,
                                                                 const TextureState &state) {
    const TextureKeyInput input{state.address,
                                state.buffer_width,
                                static_cast<std::uint32_t>(state.width) << 16u | state.height,
                                static_cast<std::uint32_t>(state.format),
                                state.clut_address,
                                state.clut_format,
                                state.swizzled};
    auto [memo, inserted] = list_texture_keys.try_emplace(input, 0u);
    if (inserted) memo->second = texture_key(memory, state);
    const std::uint64_t key = memo->second;
    const auto found = textures.find(key);
    if (found != textures.end()) {
        found->second.last_used = ++texture_clock;
        return found->second;
    }
    std::vector<std::uint32_t> pixels;
    if (!decode_texture(memory, state, pixels) || pixels.empty()) return white_texture;

    if (textures.size() >= kMaxCachedTextures) {
        auto oldest = textures.begin();
        for (auto it = textures.begin(); it != textures.end(); ++it) {
            if (it->second.last_used < oldest->second.last_used) oldest = it;
        }
        const perf::Clock::time_point wait_start = perf::Clock::now();
        vkQueueWaitIdle(queue);
        perf::add_wait_time(perf::Clock::now() - wait_start);
        destroy_texture(oldest->second);
        textures.erase(oldest);
    }
    // All of this happens once per texture, behind the same cache as the
    // decode, so a texture the game reuses every frame is paid for on its
    // first use.
    std::uint32_t width = state.width;
    std::uint32_t height = state.height;
    // Dumped at the size the game drew it, which is what a replacement has to
    // stand in for.
    texture_pack.dump(key, width, height, pixels.data());
    const PackedTexture *replacement = texture_pack_enabled ? texture_pack.find(key) : nullptr;
    if (replacement != nullptr) {
        // Someone has drawn this at a resolution of their choosing, so the
        // renderer's own upscaling has no business enlarging it further.
        pixels = replacement->pixels;
        width = replacement->width;
        height = replacement->height;
    } else if (texture_scale > 1u) {
        (void)scale_texture(pixels, width, height, texture_scale,
                            texture_scale_sharp ? TextureScaleMode::Sharp : TextureScaleMode::Smooth);
    }
    Texture texture = create_texture(width, height, pixels.data());
    if (texture.descriptor == VK_NULL_HANDLE) return white_texture;
    return textures.emplace(key, texture).first->second;
}

namespace {

std::uint32_t texture_bits_per_pixel(TextureFormat format) {
    switch (format) {
    case TextureFormat::Rgba8888:
    case TextureFormat::Clut32: return 32u;
    case TextureFormat::Clut8: return 8u;
    case TextureFormat::Clut4:
    case TextureFormat::Dxt1: return 4u;
    case TextureFormat::Dxt3:
    case TextureFormat::Dxt5: return 8u;
    default: return 16u;
    }
}

std::uint32_t framebuffer_bytes_per_pixel(std::uint32_t format) { return format == 3u ? 4u : 2u; }

} // namespace

// MGA_TRACE_FB_TEXTURES: every distinct texture whose memory overlaps a
// guest framebuffer the renderer has drawn to, once, with where it is drawn.
void VulkanRenderer::Impl::trace_framebuffer_texture(const DrawCall &call) {
    const TextureState &texture = call.texture;
    const std::uint64_t texture_bytes = static_cast<std::uint64_t>(std::max<std::uint32_t>(
                                            texture.buffer_width, texture.width)) *
                                        texture.height * texture_bits_per_pixel(texture.format) / 8u;
    // Textures filled by a DMA copy out of VRAM, and large textures in
    // general: a screen-sized background the game copied with the CPU shows up
    // as one of these, next to [vram] lines for the reads.
    static std::map<std::array<std::uint32_t, 4>, std::uint32_t> seen_textures;
    const std::array<std::uint32_t, 4> texture_id{texture.address, static_cast<std::uint32_t>(texture.format),
                                                  static_cast<std::uint32_t>(texture.width) << 16u | texture.height,
                                                  texture.buffer_width};
    std::uint32_t copy_source = 0u;
    std::uint32_t copy_destination = 0u;
    const bool copied = find_vram_copy(texture.address, copy_source, copy_destination);
    if ((copied || (texture.width >= 256u && texture.height >= 128u)) && seen_textures.size() < 400u &&
        seen_textures[texture_id]++ == 0u) {
        std::cout << "[fbtex] frame " << frames << " large or copied texture 0x" << std::hex << texture.address
                  << std::dec << " fmt=" << static_cast<int>(texture.format) << " " << texture.width << "x"
                  << texture.height << " bufw=" << texture.buffer_width << " swizzled=" << (texture.swizzled ? 1 : 0)
                  << " drawn to 0x" << std::hex << call.target.color_address << std::dec
                  << (call.through ? " through" : " transform");
        if (copied)
            std::cout << " copied from VRAM 0x" << std::hex << copy_source << " to 0x" << copy_destination << std::dec;
        std::cout << "\n";
    }

    static std::map<std::array<std::uint32_t, 7>, std::uint32_t> seen;
    for (const auto &[address, target] : targets) {
        const std::uint64_t target_bytes =
            static_cast<std::uint64_t>(target.stride) * kPspHeight * framebuffer_bytes_per_pixel(target.format);
        const std::uint64_t start = texture.address;
        if (start + texture_bytes <= address || start >= address + target_bytes) continue;
        const std::array<std::uint32_t, 7> key{texture.address, static_cast<std::uint32_t>(texture.format),
                                               texture.width,   texture.height,
                                               texture.buffer_width, address, call.target.color_address};
        std::uint32_t &count = seen[key];
        if (count++ != 0u || seen.size() > 400u) continue;
        const Vertex &first = call.vertices.front();
        const Vertex &last = call.vertices.back();
        std::cout << "[fbtex] frame " << frames << " texture 0x" << std::hex << texture.address << std::dec
                  << " fmt=" << static_cast<int>(texture.format) << " " << texture.width << "x" << texture.height
                  << " bufw=" << texture.buffer_width << " swizzled=" << (texture.swizzled ? 1 : 0)
                  << " filter=" << texture.min_filter << "/" << texture.mag_filter << " wrap=" << texture.wrap_s
                  << "/" << texture.wrap_t << " in framebuffer 0x" << std::hex << address << std::dec
                  << " (stride=" << target.stride << " fmt=" << target.format
                  << " last drawn frame " << target.last_drawn_frame << ", offset "
                  << (texture.address - address) << ") drawn to 0x" << std::hex << call.target.color_address
                  << std::dec << " fmt=" << call.target.color_format << (call.through ? " through" : " transform")
                  << " prim=" << static_cast<int>(call.primitive) << " verts=" << call.vertices.size()
                  << " pos=(" << first.position[0] << "," << first.position[1] << ")-(" << last.position[0] << ","
                  << last.position[1] << ") uv=(" << first.texcoord[0] << "," << first.texcoord[1] << ")-("
                  << last.texcoord[0] << "," << last.texcoord[1] << ") uvscale=(" << texture.scale_u << ","
                  << texture.scale_v << "," << texture.offset_u << "," << texture.offset_v << ") blend="
                  << (call.blend.enabled ? 1 : 0) << " tfx=" << texture.function << "\n";
    }
}

bool VulkanRenderer::Impl::create_writeback(std::string &error) {
    if (!create_image(kPspWidth, kPspHeight, VK_FORMAT_R8G8B8A8_UNORM,
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, writeback_image,
                      writeback_memory, writeback_view, VK_IMAGE_ASPECT_COLOR_BIT, error))
        return false;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(kPspWidth) * kPspHeight * 4u;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = bytes;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(vkCreateBuffer(device, &buffer_info, nullptr, &writeback_buffer), "vkCreateBuffer", error))
        return false;
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, writeback_buffer, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = find_memory_type(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!check(vkAllocateMemory(device, &allocate, nullptr, &writeback_buffer_memory), "vkAllocateMemory", error))
        return false;
    vkBindBufferMemory(device, writeback_buffer, writeback_buffer_memory, 0u);
    return check(vkMapMemory(device, writeback_buffer_memory, 0u, bytes, 0u, &writeback_mapped), "vkMapMemory",
                 error);
}

void VulkanRenderer::Impl::destroy_writeback() {
    if (writeback_mapped != nullptr) vkUnmapMemory(device, writeback_buffer_memory);
    vkDestroyBuffer(device, writeback_buffer, nullptr);
    vkFreeMemory(device, writeback_buffer_memory, nullptr);
    vkDestroyImageView(device, writeback_view, nullptr);
    vkDestroyImage(device, writeback_image, nullptr);
    vkFreeMemory(device, writeback_memory, nullptr);
    writeback_mapped = nullptr;
    writeback_buffer = VK_NULL_HANDLE;
    writeback_buffer_memory = VK_NULL_HANDLE;
    writeback_view = VK_NULL_HANDLE;
    writeback_image = VK_NULL_HANDLE;
    writeback_memory = VK_NULL_HANDLE;
    writeback_in_flight = false;
    writeback_has_pixels = false;
}

// Records the copy of the displayed target that write_back_frame() stores in
// guest memory. Only framebuffers in VRAM that the GE drew are written back;
// a frame the CPU wrote itself is in memory already.
void VulkanRenderer::Impl::record_writeback(std::uint32_t address) {
    const auto found = targets.find(address);
    if (found == targets.end() || (GuestMemory::canonical(address) & 0x1F000000u) != 0x04000000u) return;
    Target &target = found->second;
    if (!target.initialized || target.guest_words.empty() || target.stride < kPspWidth) return;
    if (writeback_image == VK_NULL_HANDLE) {
        std::string error;
        if (!create_writeback(error)) {
            std::cout << "[render] cannot write frames back to guest memory: " << error << "\n";
            destroy_writeback();
            return;
        }
    }
    transition(command_buffer, target.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(command_buffer, writeback_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
    blit.srcOffsets[1] = {static_cast<std::int32_t>(target_extent.width),
                          static_cast<std::int32_t>(target_extent.height), 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[1] = {static_cast<std::int32_t>(kPspWidth), static_cast<std::int32_t>(kPspHeight), 1};
    vkCmdBlitImage(command_buffer, target.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, writeback_image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &blit, VK_FILTER_LINEAR);
    transition(command_buffer, writeback_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
    copy.imageExtent = {kPspWidth, kPspHeight, 1u};
    vkCmdCopyImageToBuffer(command_buffer, writeback_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, writeback_buffer,
                           1u, &copy);
    transition(command_buffer, target.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    writeback_recorded = {address, target.stride, target.format};
    writeback_in_flight = true;
}

// Reads one guest word from every 256 bytes of the buffer a target stands for.
void VulkanRenderer::Impl::snapshot_guest_words(const GuestMemory &memory, std::uint32_t address, Target &target) {
    const std::uint32_t bytes = target.stride * kPspHeight * framebuffer_bytes_per_pixel(target.format);
    target.guest_words.clear();
    if (!memory.contains(address, bytes)) return;
    target.guest_words.reserve(bytes / 256u + 1u);
    for (std::uint32_t offset = 0; offset + 4u <= bytes; offset += 256u)
        target.guest_words.push_back(memory.load32(address + offset));
}

// The render target a texture lies in, if the texture reads it the way it was
// drawn: the same row length, a direct colour format of the same pixel size,
// and guest memory under the texture unchanged since the target was last drawn
// to. Anything else (a palette or swizzled view of a framebuffer, or a texture
// the game has since put where a framebuffer was) is decoded from guest memory.
VulkanRenderer::Impl::FramebufferTexture VulkanRenderer::Impl::find_framebuffer_texture(
    const GuestMemory &memory, const TextureState &texture) {
    if (texture.swizzled || static_cast<std::uint32_t>(texture.format) > 3u || texture.width == 0u ||
        texture.height == 0u)
        return {};
    const std::uint32_t texture_address = GuestMemory::canonical(texture.address);
    Target *best = nullptr;
    std::uint32_t best_base = 0u;
    for (auto &[address, target] : targets) {
        if (!target.initialized || target.guest_words.empty()) continue;
        const std::uint32_t bytes_per_pixel = framebuffer_bytes_per_pixel(target.format);
        if (texture_bits_per_pixel(texture.format) != bytes_per_pixel * 8u) continue;
        const std::uint32_t base = GuestMemory::canonical(address);
        const std::uint32_t bytes = target.stride * kPspHeight * bytes_per_pixel;
        if (texture_address < base || texture_address - base >= bytes) continue;
        if (texture.buffer_width != target.stride || (texture_address - base) % bytes_per_pixel != 0u) continue;
        if (best == nullptr || target.draw_serial > best->draw_serial) {
            best = &target;
            best_base = base;
        }
    }
    if (best == nullptr) return {};

    const std::uint32_t bytes_per_pixel = framebuffer_bytes_per_pixel(best->format);
    const std::uint64_t texture_end = static_cast<std::uint64_t>(texture_address) +
                                      static_cast<std::uint64_t>(texture.buffer_width) * texture.height *
                                          bytes_per_pixel;
    for (std::size_t i = 0; i < best->guest_words.size(); ++i) {
        const std::uint32_t word_address = best_base + static_cast<std::uint32_t>(i) * 256u;
        if (word_address < texture_address || word_address >= texture_end) continue;
        if (!memory.contains(word_address, 4u) || memory.load32(word_address) != best->guest_words[i]) return {};
    }
    const std::uint32_t pixel = (texture_address - best_base) / bytes_per_pixel;
    FramebufferTexture found{best, pixel % best->stride, pixel / best->stride};
    if (found.x >= kPspWidth || found.y >= kPspHeight) return {};
    return found;
}

// A sampled copy of the target, brought up to date with its latest draw. The
// copy is recorded between render passes, so the pass in progress ends here.
VkDescriptorSet VulkanRenderer::Impl::framebuffer_descriptor(Target &target, bool opaque) {
    if (target.copy == VK_NULL_HANDLE) {
        std::string error;
        if (!create_image(target_extent.width, target_extent.height, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, target.copy,
                          target.copy_memory, target.copy_view, VK_IMAGE_ASPECT_COLOR_BIT, error)) {
            std::cout << "[render] cannot sample a render target: " << error << "\n";
            return VK_NULL_HANDLE;
        }
        // A 5650 texture has no alpha: the GE reads it as opaque, whatever the
        // target's alpha channel holds.
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = target.copy;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.components.a = VK_COMPONENT_SWIZZLE_ONE;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
        if (vkCreateImageView(device, &view_info, nullptr, &target.copy_opaque_view) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        for (std::size_t i = 0; i < target.copy_descriptors.size(); ++i) {
            VkDescriptorSetAllocateInfo descriptor_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            descriptor_info.descriptorPool = descriptor_pool;
            descriptor_info.descriptorSetCount = 1u;
            descriptor_info.pSetLayouts = &descriptor_layout;
            if (vkAllocateDescriptorSets(device, &descriptor_info, &target.copy_descriptors[i]) != VK_SUCCESS) {
                target.copy_descriptors[i] = VK_NULL_HANDLE;
                return VK_NULL_HANDLE;
            }
            VkDescriptorImageInfo image_info{framebuffer_sampler(), i == 0u ? target.copy_view : target.copy_opaque_view,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = target.copy_descriptors[i];
            write.descriptorCount = 1u;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &image_info;
            vkUpdateDescriptorSets(device, 1u, &write, 0u, nullptr);
        }
        target.copy_valid = false;
    }
    if (!target.copy_valid || target.copy_serial != target.draw_serial) {
        end_pass();
        transition(command_buffer, target.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition(command_buffer, target.copy,
                   target.copy_valid ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
        region.dstSubresource = region.srcSubresource;
        region.extent = {target_extent.width, target_extent.height, 1u};
        vkCmdCopyImage(command_buffer, target.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.copy,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region);
        transition(command_buffer, target.copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transition(command_buffer, target.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        target.copy_serial = target.draw_serial;
        target.copy_valid = true;
    }
    return target.copy_descriptors[opaque ? 1u : 0u];
}

VkPipeline VulkanRenderer::Impl::pipeline_for(const PipelineKey &key) {
    const auto found = pipelines.find(key);
    if (found != pipelines.end()) return found->second;

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_shader;
    stages[0].pName = "main";
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_shader;

    VkVertexInputBindingDescription binding{0u, sizeof(GpuVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    std::array<VkVertexInputAttributeDescription, 4> attributes{
        VkVertexInputAttributeDescription{0u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, x)},
        VkVertexInputAttributeDescription{1u, 0u, VK_FORMAT_R32G32_SFLOAT, offsetof(GpuVertex, u)},
        VkVertexInputAttributeDescription{2u, 0u, VK_FORMAT_R8G8B8A8_UNORM, offsetof(GpuVertex, color)},
        VkVertexInputAttributeDescription{3u, 0u, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, nx)},
    };
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1u;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // The GE viewport and scissor change per draw, so they are dynamic state.
    VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1u;
    viewport_state.scissorCount = 1u;
    const std::array<VkDynamicState, 3> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                                       VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic_state{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic_state.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic_state.pDynamicStates = dynamic_states.data();

    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = key.cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    raster.frontFace = key.cull_clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = key.depth_test ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = key.depth_write ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = to_compare_op(key.depth_function);

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.blendEnable = key.blend ? VK_TRUE : VK_FALSE;
    blend_attachment.srcColorBlendFactor = to_blend_factor(key.source_factor, true);
    blend_attachment.dstColorBlendFactor = to_blend_factor(key.destination_factor, false);
    blend_attachment.colorBlendOp = to_blend_op(key.equation);
    blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.colorWriteMask = key.color_mask;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1u;
    blend.pAttachments = &blend_attachment;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = static_cast<std::uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic_state;
    info.layout = pipeline_layout;
    info.renderPass = render_pass;
    VkPipeline pipeline{};
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1u, &info, nullptr, &pipeline) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    pipelines.emplace(key, pipeline);
    return pipeline;
}

bool VulkanRenderer::pump_events() {
    if (!impl_ || impl_->window == nullptr) return false;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // Closing the window is the only way out. Esc used to quit as well, but
        // Steam's desktop controller layout on a Steam Deck sends Esc from the B
        // button, which is also the game's confirm button, so confirming a menu
        // closed the game. Esc is reserved for the in-game menu instead.
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) impl_->quit = true;
        // Hot-plug is handled before the text-input branch below, which skips
        // every other event while the on-screen keyboard is up.
        if (event.type == SDL_EVENT_GAMEPAD_ADDED) impl_->open_gamepad(event.gdevice.which);
        if (event.type == SDL_EVENT_GAMEPAD_REMOVED) impl_->close_gamepad(event.gdevice.which);
        // F3 toggles the performance overlay. No pad combination: L3+R3 is
        // reserved for the in-game menu.
        if (event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED) impl_->update_display_info();
        if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) impl_->swapchain_dirty = true;
        if (impl_->event_hook && impl_->event_hook(event)) continue;
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F3 && !event.key.repeat && impl_->overlay_ready)
            impl_->overlay_visible = !impl_->overlay_visible;
    }
    // While a menu or the on-screen keyboard is open, nothing reaches the game.
    if (!impl_->game_input) {
        impl_->pad = PadState{};
        return !impl_->quit;
    }

    // Keyboard to PSP pad. Bits follow SceCtrlButtons.
    // Only sample while the window has focus: a key still down when focus is
    // lost stays down in SDL's snapshot, which the guest sees as a held
    // direction it can never release.
    const bool focused = (SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_INPUT_FOCUS) != 0u;
    const bool *keys = SDL_GetKeyboardState(nullptr);
    PadState pad{};
    const auto held = [&](SDL_Scancode code, std::uint32_t bit) {
        if (focused && keys[code]) pad.buttons |= bit;
    };
    held(SDL_SCANCODE_UP, 0x0010u);
    held(SDL_SCANCODE_RIGHT, 0x0020u);
    held(SDL_SCANCODE_DOWN, 0x0040u);
    held(SDL_SCANCODE_LEFT, 0x0080u);
    held(SDL_SCANCODE_Q, 0x0100u);      // L
    held(SDL_SCANCODE_W, 0x0200u);      // R
    held(SDL_SCANCODE_S, 0x1000u);      // triangle
    held(SDL_SCANCODE_X, 0x2000u);      // circle (confirm in Japanese titles)
    held(SDL_SCANCODE_Z, 0x4000u);      // cross
    held(SDL_SCANCODE_A, 0x8000u);      // square
    held(SDL_SCANCODE_RETURN, 0x0008u); // start
    held(SDL_SCANCODE_RSHIFT, 0x0001u); // select
    held(SDL_SCANCODE_BACKSPACE, 0x0001u);
    // Analog stick on IJKL, centred at 0x80.
    int analog_x = 0;
    int analog_y = 0;
    if (focused && keys[SDL_SCANCODE_J]) analog_x -= 127;
    if (focused && keys[SDL_SCANCODE_L]) analog_x += 127;
    if (focused && keys[SDL_SCANCODE_I]) analog_y -= 127;
    if (focused && keys[SDL_SCANCODE_K]) analog_y += 127;

    // The gamepad adds to the same bits and offsets, so both sources are live.
    if (impl_->gamepad != nullptr) read_gamepad(impl_->gamepad, pad, analog_x, analog_y);

    pad.analog_x = static_cast<std::uint8_t>(std::clamp(0x80 + analog_x, 0, 255));
    pad.analog_y = static_cast<std::uint8_t>(std::clamp(0x80 + analog_y, 0, 255));

    // Unattended runs (overlay bootstrapping) press confirm periodically so the
    // game walks through title screens and dialogs on its own.
    static const std::uint64_t auto_confirm = [] {
        const char *text = std::getenv("MGA_AUTO_CONFIRM");
        return text != nullptr ? std::strtoull(text, nullptr, 10) : 0ull;
    }();
    if (auto_confirm != 0u) {
        const std::uint64_t phase = impl_->frames % auto_confirm;
        if (phase < auto_confirm / 8u) pad.buttons |= 0x2000u;  // circle
    }
    // Buttons held when the game got its input back stay hidden from it until
    // they are released.
    if (impl_->suppress_held) {
        impl_->suppressed_buttons = pad.buttons;
        impl_->suppress_held = false;
    }
    impl_->suppressed_buttons &= pad.buttons;
    pad.buttons &= ~impl_->suppressed_buttons;
    if (pad_tuning().trace && (pad.buttons != impl_->pad.buttons || pad.analog_x != impl_->pad.analog_x ||
                               pad.analog_y != impl_->pad.analog_y || pad.right_x != impl_->pad.right_x ||
                               pad.right_y != impl_->pad.right_y)) {
        std::cout << "[pad] buttons 0x" << std::hex << pad.buttons << std::dec << " analog "
                  << static_cast<int>(pad.analog_x) << "," << static_cast<int>(pad.analog_y) << " right "
                  << static_cast<int>(pad.right_x) << "," << static_cast<int>(pad.right_y) << std::endl;
    }
    impl_->pad = pad;
    return !impl_->quit;
}

PadState VulkanRenderer::pad() const noexcept { return impl_ ? impl_->pad : PadState{}; }

void VulkanRenderer::set_event_hook(std::function<bool(const SDL_Event &)> hook) {
    if (impl_) impl_->event_hook = std::move(hook);
}

void VulkanRenderer::set_game_input(bool enabled) {
    if (!impl_) return;
    if (enabled && !impl_->game_input) impl_->suppress_held = true;
    impl_->game_input = enabled;
}

void VulkanRenderer::request_quit() noexcept {
    if (impl_) impl_->quit = true;
}

void VulkanRenderer::hold_frame(bool hold) {
    if (!impl_) return;
    Impl &impl = *impl_;
    if (!impl.ready || hold == impl.holding) return;
    if (!hold) {
        impl.holding = false;
        vkDeviceWaitIdle(impl.device);
        impl.destroy_target(impl.held);
        impl.held = {};
        return;
    }
    const auto shown = impl.targets.find(impl.presented_target);
    if (shown == impl.targets.end() || !shown->second.initialized) return;
    std::string error;
    if (!impl.create_image(impl.target_extent.width, impl.target_extent.height, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                           impl.held.color, impl.held.color_memory, impl.held.color_view, VK_IMAGE_ASPECT_COLOR_BIT,
                           error)) {
        std::cerr << "Renderer: cannot hold the frame (" << error << ")\n";
        impl.destroy_target(impl.held);
        impl.held = {};
        return;
    }
    // The shown target and the copy both rest in the layout presenting
    // expects of a game frame.
    const VkImage source = shown->second.color;
    const VkImage copy_to = impl.held.color;
    const VkExtent2D extent = impl.target_extent;
    impl.run_commands([&](VkCommandBuffer commands) {
        impl.transition(commands, source, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        impl.transition(commands, copy_to, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
        region.dstSubresource = region.srcSubresource;
        region.extent = {extent.width, extent.height, 1u};
        vkCmdCopyImage(commands, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy_to,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region);
        impl.transition(commands, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        impl.transition(commands, copy_to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    });
    impl.holding = true;
}

SDL_Window *VulkanRenderer::window() const noexcept { return impl_ ? impl_->window : nullptr; }
std::string VulkanRenderer::device_name() const { return impl_ ? impl_->device_name : std::string{}; }
SDL_Gamepad *VulkanRenderer::gamepad() const noexcept { return impl_ ? impl_->gamepad : nullptr; }

void VulkanRenderer::set_internal_scale(std::uint32_t scale) {
    Impl &impl = *impl_;
    scale = std::clamp<std::uint32_t>(scale, 1u, settings::kMaxInternalScale);
    const VkExtent2D extent{kPspWidth * scale, kPspHeight * scale};
    if (!impl.ready || impl.recording ||
        (extent.width == impl.target_extent.width && extent.height == impl.target_extent.height))
        return;
    // Every target is rebuilt at the new size with its current picture scaled
    // into it, so the paused frame behind the menu, and render-to-texture
    // targets the game reads back, stay intact.
    vkDeviceWaitIdle(impl.device);
    // A held frame has the old size; let the game's own frames show again.
    hold_frame(false);
    std::map<std::uint32_t, Impl::Target> old_targets = std::move(impl.targets);
    impl.targets.clear();
    const VkExtent2D old_extent = impl.target_extent;
    impl.target_extent = extent;
    std::vector<std::pair<Impl::Target *, const Impl::Target *>> copies;
    for (const auto &[address, old_target] : old_targets) {
        std::string error;
        Impl::Target *target = impl.target_for(address, error);
        if (target == nullptr) {
            std::cout << "[render] cannot resize a render target: " << error << "\n";
            continue;
        }
        target->stride = old_target.stride;
        target->format = old_target.format;
        target->last_drawn_frame = old_target.last_drawn_frame;
        target->draw_serial = old_target.draw_serial;
        target->guest_words = old_target.guest_words;
        if (old_target.initialized) copies.emplace_back(target, &old_target);
    }
    impl.run_commands([&](VkCommandBuffer commands) {
        for (const auto &[target, old_target] : copies) {
            impl.transition(commands, old_target->color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            impl.transition(commands, target->color, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
            blit.srcOffsets[1] = {static_cast<std::int32_t>(old_extent.width),
                                  static_cast<std::int32_t>(old_extent.height), 1};
            blit.dstSubresource = blit.srcSubresource;
            blit.dstOffsets[1] = {static_cast<std::int32_t>(extent.width), static_cast<std::int32_t>(extent.height), 1};
            vkCmdBlitImage(commands, old_target->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target->color,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &blit, VK_FILTER_LINEAR);
            impl.transition(commands, target->color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            impl.transition(commands, target->depth, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            target->initialized = true;
        }
    });
    for (auto &[address, old_target] : old_targets) impl.destroy_target(old_target);
    std::cout << "[render] internal resolution " << extent.width << "x" << extent.height << "\n";
}

void VulkanRenderer::set_window_scale(std::uint32_t scale) {
    if (!impl_ || impl_->window == nullptr) return;
    scale = std::clamp<std::uint32_t>(scale, 1u, settings::kMaxWindowScale);
    if ((SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_FULLSCREEN) != 0u) return;
    SDL_SetWindowSize(impl_->window, static_cast<int>(kPspWidth * scale), static_cast<int>(kPspHeight * scale));
    impl_->swapchain_dirty = true;
}

void VulkanRenderer::set_fullscreen(bool fullscreen) {
    if (!impl_ || impl_->window == nullptr) return;
    SDL_SetWindowFullscreen(impl_->window, fullscreen);
    impl_->swapchain_dirty = true;
}

void VulkanRenderer::set_present_mode(settings::PresentMode mode) {
    if (!impl_) return;
    impl_->requested_present = mode;
    impl_->swapchain_dirty = true;
}

bool VulkanRenderer::supports_present_mode(settings::PresentMode mode) const {
    if (!impl_) return false;
    VkPresentModeKHR wanted = VK_PRESENT_MODE_FIFO_KHR;
    if (mode == settings::PresentMode::Mailbox) wanted = VK_PRESENT_MODE_MAILBOX_KHR;
    if (mode == settings::PresentMode::Immediate) wanted = VK_PRESENT_MODE_IMMEDIATE_KHR;
    const auto &modes = impl_->present_modes;
    return wanted == VK_PRESENT_MODE_FIFO_KHR || std::find(modes.begin(), modes.end(), wanted) != modes.end();
}

void VulkanRenderer::set_keep_aspect(bool keep_aspect) {
    if (impl_) impl_->keep_aspect = keep_aspect;
}

void VulkanRenderer::set_sharp_screen(bool sharp) {
    if (impl_) impl_->sharp_screen = sharp;
}

// Mip levels are part of the image, not of the descriptor, so every cached
// texture has to be built again. Dropping the cache does that on next use.
void VulkanRenderer::set_smooth_textures(bool smooth) {
    Impl &impl = *impl_;
    if (!impl.ready || impl.recording || impl.smooth_textures == smooth) return;
    impl.smooth_textures = smooth;
    vkDeviceWaitIdle(impl.device);
    for (auto &[key, texture] : impl.textures) impl.destroy_texture(texture);
    impl.textures.clear();
    impl.list_texture_keys.clear();
    std::cout << "[render] texture smoothing " << (smooth ? "on" : "off") << "\n";
}

void VulkanRenderer::set_shadow_maps(float strength) {
    Impl &impl = *impl_;
    if (!impl.ready) return;
    const float wanted = std::clamp(strength, 0.0f, 1.0f);
    if (wanted > 0.0f && !impl.shadow_available) {
        std::cout << "[render] shadow maps are not available on this device\n";
        return;
    }
    if (impl.shadow_strength == wanted) return;
    impl.shadow_strength = wanted;
    impl.bind_shadow_map();
    std::cout << "[render] cast shadows " << (wanted > 0.0f ? "on" : "off") << "\n";
}

void VulkanRenderer::set_blob_shadows(float strength) {
    Impl &impl = *impl_;
    if (!impl.ready) return;
    const float wanted = std::clamp(strength, 0.0f, 1.0f);
    if (impl.blob_strength == wanted) return;
    impl.blob_strength = wanted;
    std::cout << "[render] character shadows " << (wanted > 0.0f ? "on" : "off") << "\n";
}

void VulkanRenderer::set_shadow_casters(const std::array<float, 16> &world_to_clip,
                                        const std::vector<ShadowCaster> &casters) {
    Impl &impl = *impl_;
    if (!impl.ready) return;
    impl.shadow_transform = world_to_clip;
    impl.shadow_transform_valid = true;
    impl.shadow_casters = casters;
}

void VulkanRenderer::set_contact_shadows(float strength) {
    Impl &impl = *impl_;
    if (!impl.ready) return;
    const float wanted = std::clamp(strength, 0.0f, 1.0f);
    if (impl.post_ao == wanted) return;
    impl.post_ao = wanted;
    std::cout << "[render] contact shadows " << (wanted > 0.0f ? "on" : "off") << "\n";
}

void VulkanRenderer::set_post_processing(bool enabled, bool fxaa) {
    Impl &impl = *impl_;
    if (!impl.ready) return;
    if (enabled && impl.post_pipeline == VK_NULL_HANDLE) {
        // The pass could not be built on this device; say so once rather than
        // leaving a setting that silently does nothing.
        std::cout << "[render] post-processing is not available on this device\n";
        return;
    }
    if (impl.post_enabled == enabled && impl.post_fxaa == fxaa) return;
    impl.post_enabled = enabled;
    impl.post_fxaa = fxaa;
    std::cout << "[render] post-processing " << (enabled ? "on" : "off") << ", anti-aliasing "
              << (enabled && fxaa ? "on" : "off") << "\n";
}

void VulkanRenderer::set_smart_2d(bool smart) {
    Impl &impl = *impl_;
    if (!impl.ready || impl.smart_2d == smart) return;
    impl.smart_2d = smart;
    // Nothing cached depends on it: the decision is made per draw.
    std::cout << "[render] sharp 2D " << (smart ? "on" : "off") << "\n";
}

void VulkanRenderer::set_texture_pack(bool enabled) {
    Impl &impl = *impl_;
    if (!impl.ready || impl.recording || impl.texture_pack_enabled == enabled) return;
    impl.texture_pack_enabled = enabled;
    // A replacement is baked into the uploaded image, so the cache has to go --
    // the same reason changing the scaling drops it.
    vkDeviceWaitIdle(impl.device);
    for (auto &[key, texture] : impl.textures) impl.destroy_texture(texture);
    impl.textures.clear();
    impl.list_texture_keys.clear();
    std::cout << "[textures] texture pack " << (enabled ? "on" : "off");
    if (enabled && !impl.texture_pack.available()) std::cout << " (none found)";
    std::cout << "\n";
}

[[nodiscard]] bool VulkanRenderer::texture_pack_available() const noexcept {
    return impl_ && impl_->texture_pack.available();
}

void VulkanRenderer::set_texture_scale(std::uint32_t factor, bool sharp) {
    Impl &impl = *impl_;
    factor = std::clamp<std::uint32_t>(factor, 1u, settings::kMaxTextureScale);
    if (!impl.ready || impl.recording) return;
    if (impl.texture_scale == factor && impl.texture_scale_sharp == sharp) return;
    impl.texture_scale = factor;
    impl.texture_scale_sharp = sharp;
    // The scale is baked into the uploaded image, not into the sampler, so
    // every cached texture has to go -- the same reason smoothing drops them.
    vkDeviceWaitIdle(impl.device);
    for (auto &[key, texture] : impl.textures) impl.destroy_texture(texture);
    impl.textures.clear();
    impl.list_texture_keys.clear();
    if (factor <= 1u) std::cout << "[render] texture scaling off\n";
    else std::cout << "[render] texture scaling " << factor << "x, " << (sharp ? "edge-preserving" : "bicubic") << "\n";
}

void VulkanRenderer::set_sharp_textures(bool sharp) {
    Impl &impl = *impl_;
    if (!impl.ready || impl.recording || impl.sharp_textures == sharp) return;
    impl.sharp_textures = sharp;
    // The sampler is part of each texture's descriptor; rewrite them all.
    vkDeviceWaitIdle(impl.device);
    const auto rewrite = [&](Impl::Texture &texture) {
        if (texture.descriptor == VK_NULL_HANDLE) return;
        VkDescriptorImageInfo image_info{impl.texture_sampler(), texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = texture.descriptor;
        write.descriptorCount = 1u;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image_info;
        vkUpdateDescriptorSets(impl.device, 1u, &write, 0u, nullptr);
    };
    rewrite(impl.white_texture);
    for (auto &[key, texture] : impl.textures) rewrite(texture);
    for (auto &[address, target] : impl.targets) {
        for (std::size_t i = 0; i < target.copy_descriptors.size(); ++i) {
            if (target.copy_descriptors[i] == VK_NULL_HANDLE) continue;
            VkDescriptorImageInfo image_info{impl.framebuffer_sampler(), i == 0u ? target.copy_view : target.copy_opaque_view,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = target.copy_descriptors[i];
            write.descriptorCount = 1u;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &image_info;
            vkUpdateDescriptorSets(impl.device, 1u, &write, 0u, nullptr);
        }
    }
}

void VulkanRenderer::set_perf_overlay(bool visible) {
    if (impl_) impl_->overlay_visible = impl_->overlay_ready && visible;
}

void VulkanRenderer::capture_window(const std::string &path) {
    if (!impl_ || !impl_->ready) return;
    if ((impl_->swapchain_usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0u) {
        std::cout << "[render] this swapchain cannot be read back; no window capture\n";
        return;
    }
    impl_->capture_path = path;
}

bool VulkanRenderer::initialize_ui(std::string &error) {
    Impl &impl = *impl_;
    if (!impl.ready) {
        error = "no renderer";
        return false;
    }
    if (impl.ui_ready) return true;
    // Loads what the game frame and the overlay left in the swapchain image
    // and draws over it; submit_and_present moves it on to presentation.
    VkAttachmentDescription attachment{};
    attachment.format = impl.swapchain_format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference reference{0u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1u;
    subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass_info.attachmentCount = 1u;
    pass_info.pAttachments = &attachment;
    pass_info.subpassCount = 1u;
    pass_info.pSubpasses = &subpass;
    if (!check(vkCreateRenderPass(impl.device, &pass_info, nullptr, &impl.ui_render_pass), "vkCreateRenderPass",
               error))
        return false;
    if (!impl.create_ui_framebuffers(error)) return false;

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_1;
    info.Instance = impl.instance;
    info.PhysicalDevice = impl.physical_device;
    info.Device = impl.device;
    info.QueueFamily = impl.queue_family;
    info.Queue = impl.queue;
    info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE;
    info.MinImageCount = impl.swapchain_min_images;
    info.ImageCount = std::max<std::uint32_t>(static_cast<std::uint32_t>(impl.swapchain_images.size()),
                                              impl.swapchain_min_images);
    info.PipelineInfoMain.RenderPass = impl.ui_render_pass;
    info.PipelineInfoMain.Subpass = 0u;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (!ImGui_ImplVulkan_Init(&info)) {
        error = "ImGui_ImplVulkan_Init failed";
        return false;
    }
    impl.ui_ready = true;
    return true;
}

void VulkanRenderer::shutdown_ui() {
    Impl &impl = *impl_;
    if (!impl.ui_ready) return;
    vkDeviceWaitIdle(impl.device);
    ImGui_ImplVulkan_Shutdown();
    impl.ui_ready = false;
    for (VkFramebuffer framebuffer : impl.ui_framebuffers) vkDestroyFramebuffer(impl.device, framebuffer, nullptr);
    impl.ui_framebuffers.clear();
    vkDestroyRenderPass(impl.device, impl.ui_render_pass, nullptr);
    impl.ui_render_pass = VK_NULL_HANDLE;
}

void VulkanRenderer::begin_ui_frame() {
    if (impl_ && impl_->ui_ready) ImGui_ImplVulkan_NewFrame();
}

void VulkanRenderer::set_ui_draw_data(ImDrawData *draw_data) {
    if (impl_) impl_->ui_draw_data = draw_data;
}

void VulkanRenderer::present_ui(bool show_game) {
    Impl &impl = *impl_;
    if (!impl.ready) return;
    if (!impl.recording) begin_frame();
    impl.end_pass();
    const Impl::Target *source = nullptr;
    if (show_game) {
        if (const auto shown = impl.targets.find(impl.presented_target); shown != impl.targets.end())
            source = &shown->second;
    }
    impl.submit_and_present(source, false);
}

void VulkanRenderer::begin_frame() {
    Impl &impl = *impl_;
    if (!impl.ready || impl.recording) return;
    const perf::Clock::time_point wait_start = perf::Clock::now();
    vkWaitForFences(impl.device, 1u, &impl.frame_fence, VK_TRUE, UINT64_MAX);
    perf::add_wait_time(perf::Clock::now() - wait_start);
    vkResetFences(impl.device, 1u, &impl.frame_fence);
    if (impl.writeback_in_flight) {
        impl.writeback_pixels.resize(static_cast<std::size_t>(kPspWidth) * kPspHeight);
        std::memcpy(impl.writeback_pixels.data(), impl.writeback_mapped, impl.writeback_pixels.size() * 4u);
        impl.writeback_ready = impl.writeback_recorded;
        impl.writeback_has_pixels = true;
        impl.writeback_in_flight = false;
    }
    vkResetCommandBuffer(impl.command_buffer, 0u);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(impl.command_buffer, &begin);
    impl.vertex_offset = 0u;
    impl.environment_version = 0u;
    impl.object_valid = false;
    impl.forget_bindings();
    impl.shadow_recorded = false;
    impl.shadow_light = -1;
    if (impl.shadow_available) {
        vkResetCommandBuffer(impl.shadow_command_buffer, 0u);
        VkCommandBufferBeginInfo shadow_begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        shadow_begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(impl.shadow_command_buffer, &shadow_begin);
        impl.shadow_map->begin_frame();
    }
    impl.pass_active = false;
    impl.recording = true;
}

void VulkanRenderer::upload_frame(std::uint32_t display_address, const std::uint8_t *pixels, std::uint32_t width,
                                  std::uint32_t height, std::uint32_t stride) {
    Impl &impl = *impl_;
    if (!impl.ready || pixels == nullptr || width == 0u || height == 0u || stride < width) return;
    if (!impl.recording) begin_frame();
    impl.end_pass();
    std::string error;
    if (impl.upload_extent.width != width || impl.upload_extent.height != height) {
        // Nothing recorded so far this frame uses the old image yet.
        vkDeviceWaitIdle(impl.device);
        if (!impl.create_upload(width, height, error)) {
            std::cerr << "Renderer: cannot upload frames (" << error << ")\n";
            impl.destroy_upload();
            return;
        }
    }
    Impl::Target *target = impl.target_for(display_address, error);
    if (target == nullptr) return;

    // The staging buffer is free: the frame fence was waited on in begin_frame.
    auto *staging = static_cast<std::uint8_t *>(impl.upload_mapped);
    for (std::uint32_t row = 0; row < height; ++row)
        std::memcpy(staging + static_cast<std::size_t>(row) * width * 4u,
                    pixels + static_cast<std::size_t>(row) * stride * 4u, static_cast<std::size_t>(width) * 4u);
    impl.transition(impl.command_buffer, impl.upload_image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
    copy.imageExtent = {width, height, 1u};
    vkCmdCopyBufferToImage(impl.command_buffer, impl.upload_staging, impl.upload_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);
    impl.transition(impl.command_buffer, impl.upload_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    // A new target has no contents to keep; an old one rests in the layout
    // the render pass expects.
    impl.transition(impl.command_buffer, target->color,
                    target->initialized ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (!target->initialized) {
        impl.transition(impl.command_buffer, target->depth, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        target->initialized = true;
    }
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
    blit.srcOffsets[1] = {static_cast<std::int32_t>(width), static_cast<std::int32_t>(height), 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[1] = {static_cast<std::int32_t>(impl.target_extent.width),
                          static_cast<std::int32_t>(impl.target_extent.height), 1};
    vkCmdBlitImage(impl.command_buffer, impl.upload_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target->color,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &blit, VK_FILTER_LINEAR);
    impl.transition(impl.command_buffer, target->color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    impl.last_drawn_target = display_address;
    // The frame came from guest memory, which texturing reads correctly anyway.
    ++target->draw_serial;
    target->guest_words.clear();
}

void VulkanRenderer::begin_display_list() {
    if (impl_) impl_->list_texture_keys.clear();
}

void VulkanRenderer::submit(const DrawCall &call, const GuestMemory &memory) {
    Impl &impl = *impl_;
    if (!impl.ready) return;
    if (!impl.recording) begin_frame();
    if (call.vertices.empty()) return;

    // The blobs belong after the scene and before the interface. The first
    // through-mode draw of a frame that has drawn 3D is exactly that seam:
    // everything from here on is flat and would paint over them.
    if (call.through && !impl.blobs_drawn && impl.frame_transformed_draws != 0u && impl.blob_strength > 0.0f &&
        impl.shadow_transform_valid && !impl.shadow_casters.empty()) {
        impl.blobs_drawn = true;
        draw_shadow_blobs(memory);
    }

    // Everything becomes a triangle list; sprites expand to two triangles.
    impl.scratch.clear();
    // A vertex format without a colour field leaves every vertex white, and the
    // GE supplies the colour from the material registers instead. That is where
    // the marker over an NPC's head and the shadow blobs under characters get
    // both their colour and the alpha that makes them faint.
    //
    // Only unlit draws, though. Lighting on means the material colour is one term
    // of a sum the lights complete, which the vertex shader evaluates; handing
    // it over as the finished colour turned every character a flat muddy brown.
    // MGA_NO_LIGHTING leaves lit geometry with the old white stand-in and
    // turns fog off too, which is how everything was drawn before either
    // existed; MGA_NO_FOG turns off fog alone.
    static const bool no_material_color = std::getenv("MGA_NO_MATERIAL_COLOR") != nullptr;
    static const bool no_lighting = std::getenv("MGA_NO_LIGHTING") != nullptr;
    static const bool no_fog = no_lighting || std::getenv("MGA_NO_FOG") != nullptr;
    const bool lit = call.lighting_enabled && !no_lighting && !call.through && !call.clear_mode;
    const bool use_material_color = !no_material_color && !call.has_vertex_color && !call.lighting_enabled;
    const auto push_vertex = [&](const Vertex &vertex) {
        GpuVertex out{};
        out.x = vertex.position[0];
        out.y = vertex.position[1];
        out.z = vertex.position[2];
        out.u = vertex.texcoord[0];
        out.v = vertex.texcoord[1];
        out.color = use_material_color ? call.material_color : vertex.color;
        out.nx = vertex.normal[0];
        out.ny = vertex.normal[1];
        out.nz = vertex.normal[2];
        impl.scratch.push_back(out);
    };
    const auto vertex_at = [&](std::size_t index) -> const Vertex & {
        if (!call.indices.empty()) {
            const std::size_t mapped = call.indices[index];
            return call.vertices[std::min(mapped, call.vertices.size() - 1u)];
        }
        return call.vertices[std::min(index, call.vertices.size() - 1u)];
    };
    const std::size_t count = call.indices.empty() ? call.vertices.size() : call.indices.size();

    switch (call.primitive) {
    case PrimitiveType::Triangles:
        for (std::size_t i = 0; i + 2u < count; i += 3u) {
            push_vertex(vertex_at(i));
            push_vertex(vertex_at(i + 1u));
            push_vertex(vertex_at(i + 2u));
        }
        break;
    case PrimitiveType::TriangleStrip:
        for (std::size_t i = 0; i + 2u < count; ++i) {
            const bool odd = (i & 1u) != 0u;
            push_vertex(vertex_at(i));
            push_vertex(vertex_at(odd ? i + 2u : i + 1u));
            push_vertex(vertex_at(odd ? i + 1u : i + 2u));
        }
        break;
    case PrimitiveType::TriangleFan:
        for (std::size_t i = 1u; i + 1u < count; ++i) {
            push_vertex(vertex_at(0));
            push_vertex(vertex_at(i));
            push_vertex(vertex_at(i + 1u));
        }
        break;
    case PrimitiveType::Sprites:
        // Vertex pairs describe the opposite corners of a rectangle.
        for (std::size_t i = 0; i + 1u < count; i += 2u) {
            Vertex a = vertex_at(i);
            const Vertex &b = vertex_at(i + 1u);
            // The GE flat-shades sprites from the second vertex: its depth (and
            // colour) cover the whole rectangle. sceGuClear relies on this, as
            // its first vertex carries z = 0 and only the second the clear depth.
            a.position[2] = b.position[2];
            a.color = b.color;
            a.normal = b.normal;
            Vertex top_right = b;
            top_right.position[1] = a.position[1];
            top_right.texcoord[1] = a.texcoord[1];
            Vertex bottom_left = a;
            bottom_left.position[1] = b.position[1];
            bottom_left.texcoord[1] = b.texcoord[1];
            push_vertex(a);
            push_vertex(top_right);
            push_vertex(b);
            push_vertex(a);
            push_vertex(b);
            push_vertex(bottom_left);
        }
        break;
    default:
        return;  // points and lines are not drawn yet
    }
    if (impl.scratch.empty()) return;

    // Through-mode vertices carry the texel range they may sample in the
    // otherwise unused normal and w; see clamp_through_quad().
    // MGA_NO_SPRITE_CLAMP lets them sample anywhere, as before.
    if (call.through) {
        for (GpuVertex &vertex : impl.scratch) set_uv_rect(vertex, -kNoClamp, -kNoClamp, kNoClamp, kNoClamp);
        static const bool no_sprite_clamp = std::getenv("MGA_NO_SPRITE_CLAMP") != nullptr;
        const bool quads = call.primitive == PrimitiveType::Sprites ||
                           ((call.primitive == PrimitiveType::TriangleStrip ||
                             call.primitive == PrimitiveType::TriangleFan) &&
                            count == 4u);
        if (!no_sprite_clamp && call.texture.enabled && !call.clear_mode && quads) {
            for (std::size_t first = 0; first + 6u <= impl.scratch.size(); first += 6u)
                clamp_through_quad(impl.scratch, first, static_cast<float>(call.texture.width),
                                   static_cast<float>(call.texture.height));
        }
    }

    static const bool trace = std::getenv("MGA_TRACE_GE") != nullptr;
    if (trace && impl.draws < 400u) {
        const GpuVertex &first = impl.scratch.front();
        const GpuVertex &second = impl.scratch[std::min<std::size_t>(1u, impl.scratch.size() - 1u)];
        std::cout << "[ge] prim=" << static_cast<int>(call.primitive) << (call.through ? " through" : " transform")
                  << " verts=" << impl.scratch.size() << " p0=(" << first.x << "," << first.y << "," << first.z
                  << ") p1=(" << second.x << "," << second.y << ") uv0=(" << first.u << "," << first.v << ") uv1=("
                  << second.u << "," << second.v << ") tex=" << (call.texture.enabled ? 1 : 0)
                  << " fmt=" << static_cast<int>(call.texture.format) << " size=" << call.texture.width << "x"
                  << call.texture.height << " bufw=" << call.texture.buffer_width
                  << " swizzled=" << (call.texture.swizzled ? 1 : 0) << " addr=0x" << std::hex
                  << call.texture.address << " target=0x" << call.target.color_address << std::dec
                  << " stride=" << call.target.color_stride << " blend=" << (call.blend.enabled ? 1 : 0) << "\n";
    }


    // MGA_TRACE_SPRITES=N: every through-mode sprite of frame N, with the
    // texture state it samples, to find the tiles a 2D screen is built from.
    static const std::uint64_t trace_sprites_frame = [] {
        const char *text = std::getenv("MGA_TRACE_SPRITES");
        return text != nullptr ? std::strtoull(text, nullptr, 10) : ~0ull;
    }();
    if (impl.frames == trace_sprites_frame && call.primitive != PrimitiveType::Sprites) {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
        for (std::size_t i = 0; i < count; ++i) {
            const Vertex &v = vertex_at(i);
            x0 = std::min(x0, v.position[0]); x1 = std::max(x1, v.position[0]);
            y0 = std::min(y0, v.position[1]); y1 = std::max(y1, v.position[1]);
            u0 = std::min(u0, v.texcoord[0]); u1 = std::max(u1, v.texcoord[0]);
            v0 = std::min(v0, v.texcoord[1]); v1 = std::max(v1, v.texcoord[1]);
        }
        std::cout << "[sprite] other prim=" << static_cast<int>(call.primitive) << (call.through ? " through" : " transform")
                  << " n=" << count << " pos=(" << x0 << "," << y0 << ")-(" << x1 << "," << y1 << ") uv=(" << u0
                  << "," << v0 << ")-(" << u1 << "," << v1 << ") tex=" << (call.texture.enabled ? 1 : 0) << " 0x"
                  << std::hex << call.texture.address << std::dec << " " << call.texture.width << "x"
                  << call.texture.height << " fmt=" << static_cast<int>(call.texture.format)
                  << " filter=" << call.texture.min_filter << "/" << call.texture.mag_filter << "\n";
    }
    if (call.through && call.primitive == PrimitiveType::Sprites && impl.frames == trace_sprites_frame) {
        for (std::size_t i = 0; i + 1u < count; i += 2u) {
            const Vertex &a = vertex_at(i);
            const Vertex &b = vertex_at(i + 1u);
            std::cout << "[sprite] pos=(" << a.position[0] << "," << a.position[1] << ")-(" << b.position[0] << ","
                      << b.position[1] << ") uv=(" << a.texcoord[0] << "," << a.texcoord[1] << ")-("
                      << b.texcoord[0] << "," << b.texcoord[1] << ") tex=" << (call.texture.enabled ? 1 : 0)
                      << " 0x" << std::hex << call.texture.address << std::dec << " " << call.texture.width << "x"
                      << call.texture.height << " fmt=" << static_cast<int>(call.texture.format)
                      << " filter=" << call.texture.min_filter << "/" << call.texture.mag_filter
                      << " wrap=" << call.texture.wrap_s << "/" << call.texture.wrap_t
                      << " blend=" << (call.blend.enabled ? 1 : 0) << " color=0x" << std::hex << b.color << std::dec
                      << "\n";
        }
    }

    // Deep dump of the first transformed draws: matrices, raw positions and the
    // same positions after a CPU-side transform, so a geometry that never shows
    // up can be traced to the stage that loses it.
    static const bool trace3d = std::getenv("MGA_TRACE_3D") != nullptr;
    static const bool trace_camera = std::getenv("MGA_TRACE_CAMERA") != nullptr;
    static std::uint32_t traced_3d = 0u;
    static std::uint32_t traced_clears = 0u;
    if (trace3d && call.clear_mode && traced_clears < 4u) {
        ++traced_clears;
        const GpuVertex &first = impl.scratch.front();
        const GpuVertex &last = impl.scratch.back();
        std::cout << "[3d] clear#" << traced_clears << " flags=" << call.clear_flags
                  << " (colour=" << ((call.clear_flags & 1u) != 0u) << " alpha=" << ((call.clear_flags & 2u) != 0u)
                  << " depth=" << ((call.clear_flags & 4u) != 0u) << ") through=" << (call.through ? 1 : 0)
                  << " verts=" << impl.scratch.size() << " z=" << first.z << ".." << last.z << "\n";
    }
    if (trace3d && !call.through && traced_3d < 12u) {
        ++traced_3d;
        const auto wvp = multiply(call.projection, multiply(call.view, call.world));
        const auto dump = [](const char *name, const std::array<float, 16> &m) {
            std::cout << "  " << name << " =";
            for (std::uint32_t row = 0; row < 4u; ++row) {
                std::cout << " [";
                for (std::uint32_t col = 0; col < 4u; ++col)
                    std::cout << (col ? " " : "") << m[col * 4u + row];
                std::cout << "]";
            }
            std::cout << "\n";
        };
        std::cout << "[3d] draw#" << traced_3d << " prim=" << static_cast<int>(call.primitive)
                  << " vtype=0x" << std::hex << call.vertex_type << std::dec << " verts=" << impl.scratch.size()
                  << " clear=" << (call.clear_mode ? 1 : 0) << "/" << call.clear_flags
                  << " ztest=" << (call.depth.test_enabled ? 1 : 0) << " zfunc=" << call.depth.function
                  << " zwrite=" << (call.depth.write_enabled ? 1 : 0) << " zrange=[" << call.depth.range_near << ","
                  << call.depth.range_far << "]"
                  << " cull=" << (call.culling_enabled ? 1 : 0) << " cw=" << (call.cull_clockwise ? 1 : 0)
                  << " blend=" << (call.blend.enabled ? 1 : 0) << " tex=" << (call.texture.enabled ? 1 : 0) << "\n";
        std::cout << "  viewport scale=(" << call.viewport.x_scale << "," << call.viewport.y_scale << ","
                  << call.viewport.z_scale << ") offset=(" << call.viewport.x_offset << "," << call.viewport.y_offset
                  << "," << call.viewport.z_offset << ") region=(" << call.viewport.offset_x << ","
                  << call.viewport.offset_y << ") scissor=(" << call.viewport.scissor_x1 << ","
                  << call.viewport.scissor_y1 << ")-(" << call.viewport.scissor_x2 << "," << call.viewport.scissor_y2
                  << ")\n";
        dump("world", call.world);
        dump("view ", call.view);
        dump("proj ", call.projection);
        dump("wvp  ", wvp);
        for (std::size_t i = 0; i < std::min<std::size_t>(3u, impl.scratch.size()); ++i) {
            const GpuVertex &v = impl.scratch[i];
            float clip[4]{};
            for (std::uint32_t row = 0; row < 4u; ++row)
                clip[row] = wvp[0 * 4u + row] * v.x + wvp[1 * 4u + row] * v.y + wvp[2 * 4u + row] * v.z +
                            wvp[3 * 4u + row];
            std::cout << "  v" << i << " obj=(" << v.x << "," << v.y << "," << v.z << ") clip=(" << clip[0] << ","
                      << clip[1] << "," << clip[2] << "," << clip[3] << ")";
            if (clip[3] != 0.0f)
                std::cout << " ndc=(" << clip[0] / clip[3] << "," << clip[1] / clip[3] << "," << clip[2] / clip[3]
                          << ")";
            std::cout << "\n";
        }
    }

    static const bool frame_digest = std::getenv("MGA_FRAME_DIGEST") != nullptr;
    static const bool want_hash = frame_digest || std::getenv("MGA_TRACE_OBJECTS") != nullptr;
    std::uint64_t vertex_hash = 0u;
    if (want_hash) {
        vertex_hash = 1469598103934665603ull;
        const auto *bytes = reinterpret_cast<const std::uint8_t *>(impl.scratch.data());
        const std::size_t count = impl.scratch.size() * sizeof(GpuVertex);
        for (std::size_t i = 0; i < count; ++i) vertex_hash = (vertex_hash ^ bytes[i]) * 1099511628211ull;
    }
    if (frame_digest)
        impl.frame_digest.push_back({call.through, call.target.color_address,
                                     static_cast<std::uint32_t>(impl.scratch.size()), vertex_hash});
    if (call.through) {
        ++impl.frame_through_draws;
    } else {
        ++impl.frame_transformed_draws;
        if (!call.clear_mode) {
            ++impl.shadow_trace.transformed;
            const bool identity = is_identity(call.world);
            if (lit) ++impl.shadow_trace.lit;
            if (identity) ++impl.shadow_trace.identity;
            if (lit && identity) ++impl.shadow_trace.identity_lit;
        }
        // Everything the scene draws casts. Restricting this to the
        // characters meant relying on spotting them by their identity world
        // matrix, and if that test is ever wrong there are simply no casters
        // and no shadows, with nothing to show why. Casting from all of it
        // cannot fail that way, and it gets the scenery shadowing the floor as
        // a bonus. MGA_SHADOW_CHARACTERS_ONLY goes back to the narrow test.
        static const bool characters_only = std::getenv("MGA_SHADOW_CHARACTERS_ONLY") != nullptr;
        if (impl.shadow_available && impl.shadow_map != nullptr && impl.shadow_strength > 0.0f &&
            !impl.scratch.empty() && !call.clear_mode && (!characters_only || is_identity(call.world))) {
            impl.shadow_map->add_casters(&impl.scratch[0].x, impl.scratch.size(),
                                        sizeof(GpuVertex) / sizeof(float), call.world);
            impl.shadow_trace.casters = static_cast<std::uint32_t>(impl.shadow_map->captured());
        }
        if (!call.clear_mode) {
            impl.last_scene = {true,  call.view,  call.projection,          call.viewport,
                               call.target, call.depth, call.environment_version, call.material_version};
        }
        ++impl.frame_transformed_targets[call.target.color_address];
        static const bool trace_objects = std::getenv("MGA_TRACE_OBJECTS") != nullptr;
        if (trace_objects) {
            const std::array<float, 3> at{call.world[12], call.world[13], call.world[14]};
            const auto same = std::find_if(impl.frame_objects.begin(), impl.frame_objects.end(),
                                           [&](const Impl::FrameObject &o) {
                                               return std::abs(o.at[0] - at[0]) < 0.01f &&
                                                      std::abs(o.at[1] - at[1]) < 0.01f &&
                                                      std::abs(o.at[2] - at[2]) < 0.01f;
                                           });
            if (same == impl.frame_objects.end())
                impl.frame_objects.push_back(
                    {at, static_cast<std::uint32_t>(impl.scratch.size()), 1u, vertex_hash});
            else {
                same->vertices += static_cast<std::uint32_t>(impl.scratch.size());
                ++same->draws;
                same->hash ^= vertex_hash * 1099511628211ull;
            }
        }
        if (trace_camera) {
            const auto same = std::find_if(impl.frame_views.begin(), impl.frame_views.end(),
                                           [&](const auto &entry) { return entry.first == call.view; });
            const auto vertices = static_cast<std::uint32_t>(impl.scratch.size());
            if (same == impl.frame_views.end())
                impl.frame_views.emplace_back(call.view, vertices);
            else
                same->second += vertices;
        }
        if (trace3d) {
            // Where does this frame's transformed geometry actually land? A
            // bounding box in normalised device coordinates separates "clipped
            // away" from "drawn but invisible".
            const auto wvp = multiply(call.projection, multiply(call.view, call.world));
            for (const GpuVertex &v : impl.scratch) {
                float clip[4]{};
                for (std::uint32_t row = 0; row < 4u; ++row)
                    clip[row] = wvp[0 * 4u + row] * v.x + wvp[1 * 4u + row] * v.y + wvp[2 * 4u + row] * v.z +
                                wvp[3 * 4u + row];
                if (clip[3] <= 0.0f) {
                    ++impl.frame_behind_camera;
                    continue;
                }
                const float ndc[3]{clip[0] / clip[3], clip[1] / clip[3], clip[2] / clip[3]};
                for (std::uint32_t axis = 0; axis < 3u; ++axis) {
                    impl.frame_ndc_min[axis] = std::min(impl.frame_ndc_min[axis], ndc[axis]);
                    impl.frame_ndc_max[axis] = std::max(impl.frame_ndc_max[axis], ndc[axis]);
                }
                if (ndc[0] >= -1.0f && ndc[0] <= 1.0f && ndc[1] >= -1.0f && ndc[1] <= 1.0f && ndc[2] >= -1.0f &&
                    ndc[2] <= 1.0f)
                    ++impl.frame_onscreen_vertices;
                ++impl.frame_transformed_vertices;
            }
        }
    }

    // Lighting and fog read the environment block; lit draws also read an
    // object block. Each is written into the vertex buffer only when it
    // changed, and the vertices follow whatever was written.
    const bool fogged = call.fog.enabled && !no_fog && !call.through && !call.clear_mode;
    // The shading pass needs the light's matrix while it draws, so it is
    // settled at the first lit draw of the frame, before the environment block
    // below is written with it.
    // Any transformed draw will do to settle the light: the lighting registers
    // are the same for the whole frame, and waiting for a lit one meant no
    // shadows at all in a scene that draws its world unlit.
    if (!call.through && !call.clear_mode && impl.shadow_available && impl.shadow_map != nullptr &&
        impl.shadow_strength > 0.0f && impl.shadow_light < 0) {
        impl.last_lighting = call.lighting;
        impl.last_lighting_valid = true;
        impl.shadow_trace.resolved = impl.shadow_map->resolve_light(call.lighting);
        if (impl.shadow_trace.resolved) {
            impl.shadow_light_transform = impl.shadow_map->transform();
            impl.shadow_light = impl.shadow_map->light_index();
        }
    }
    const auto view_world = multiply(call.view, call.world);
    if ((lit || fogged) && impl.environment_version != call.environment_version) {
        const LightingState &state = call.lighting;
        EnvironmentBlock environment{};
        environment.ambient = unpack_color(state.ambient_color, static_cast<float>(state.ambient_alpha) / 255.0f);
        environment.fog = {call.fog.end, call.fog.scale, 0.0f, 0.0f};
        environment.fog_color = unpack_color(call.fog.color);
        // MGA_SHADOW_DEBUG: a negative texel size tells the shader to paint
        // the shadow red instead of darkening, so it cannot be missed.
        static const bool shadow_debug = std::getenv("MGA_SHADOW_DEBUG") != nullptr;
        environment.shadow_transform = impl.shadow_light_transform;
        const bool casting = impl.shadow_light >= 0 && impl.shadow_strength > 0.0f;
        environment.shadow_params = {casting ? impl.shadow_strength : 0.0f,
                                     static_cast<float>(std::max(impl.shadow_light, 0)),
                                     casting ? (shadow_debug ? -1.0f : 1.0f) /
                                                   static_cast<float>(impl.shadow_map->resolution())
                                             : 0.0f,
                                     kShadowBias};
        for (std::size_t i = 0; i < state.lights.size(); ++i) {
            const LightState &light = state.lights[i];
            environment.light_position[i] = {light.position[0], light.position[1], light.position[2],
                                             light.enabled ? 1.0f : 0.0f};
            environment.light_direction[i] = {light.direction[0], light.direction[1], light.direction[2],
                                              static_cast<float>(light.type)};
            environment.light_attenuation[i] = {light.attenuation[0], light.attenuation[1], light.attenuation[2],
                                                static_cast<float>(light.kind)};
            environment.light_spot[i] = {light.spot_exponent, light.spot_cutoff, 0.0f, 0.0f};
            environment.light_ambient[i] = unpack_color(light.ambient);
            environment.light_diffuse[i] = unpack_color(light.diffuse);
            environment.light_specular[i] = unpack_color(light.specular);
        }
        if (!impl.write_uniform(&environment, sizeof(environment), impl.environment_offset)) return;
        impl.environment_version = call.environment_version;
    }
    // Unlit geometry receives shadows too, and to find itself in the light's
    // map it needs its world matrix, which lives in the object block. So while
    // shadows are on the block is written for every transformed draw, not only
    // the lit ones -- otherwise an unlit draw would read whichever world matrix
    // the last lit draw happened to leave there.
    const bool needs_object =
        lit || (impl.shadow_light >= 0 && impl.shadow_strength > 0.0f && !call.through && !call.clear_mode);
    if (needs_object) {
        const LightingState &state = call.lighting;
        if (impl.material_version != call.material_version) {
            impl.material[0] = unpack_color(state.material_emissive, state.specular_power);
            impl.material[1] =
                unpack_color(call.material_color, static_cast<float>(call.material_color >> 24u) / 255.0f);
            impl.material[2] = unpack_color(state.material_diffuse, static_cast<float>(state.mode));
            impl.material[3] = unpack_color(state.material_specular, state.reverse_normals ? 1.0f : 0.0f);
            impl.material_version = call.material_version;
        }
        ObjectBlock object{};
        object.world = call.world;
        object.flags = {1.0f, call.has_vertex_color ? 1.0f : 0.0f, 0.0f, static_cast<float>(state.material_update)};
        object.emissive = impl.material[0];
        object.material_ambient = impl.material[1];
        object.material_diffuse = impl.material[2];
        object.material_specular = impl.material[3];
        if (!impl.object_valid || std::memcmp(&object, &impl.last_object, sizeof(object)) != 0) {
            if (!impl.write_uniform(&object, sizeof(object), impl.object_offset)) return;
            impl.last_object = object;
            impl.object_valid = true;
        }
    }
    const VkDeviceSize bytes = impl.scratch.size() * sizeof(GpuVertex);
    if (impl.vertex_offset + bytes > kVertexBufferBytes) return;
    std::memcpy(static_cast<std::uint8_t *>(impl.vertex_mapped) + impl.vertex_offset, impl.scratch.data(),
                static_cast<std::size_t>(bytes));

    PipelineKey key{};
    if (call.clear_mode) {
        // sceGuClear draws a screen-sized sprite with CLEARMODE on: blending,
        // the depth test and the texture are bypassed and bits 8..10 say which
        // buffers it is allowed to write. Treating it as an ordinary draw left
        // the depth buffer at whatever the allocation happened to contain.
        key.blend = false;
        // Vulkan ties depth writes to the depth test: with depthTestEnable false
        // the attachment is never updated, so the test has to stay on and always
        // pass for the clear to reach the depth buffer at all.
        key.depth_test = true;
        key.depth_function = 1u;  // always
        key.depth_write = (call.clear_flags & 4u) != 0u;
        key.cull = false;
        key.color_mask = ((call.clear_flags & 1u) != 0u ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                          VK_COLOR_COMPONENT_B_BIT)
                                                       : 0u) |
                         ((call.clear_flags & 2u) != 0u ? VK_COLOR_COMPONENT_A_BIT : 0u);
    } else {
        key.blend = call.blend.enabled;
        key.source_factor = resolve_fixed_factor(call.blend.source_factor, call.blend.fixed_source);
        key.destination_factor = resolve_fixed_factor(call.blend.destination_factor, call.blend.fixed_destination);
        // Both sides fixed is common for fog and steam, and one constant cannot
        // serve two colours. When the pair adds up to white the destination is
        // exactly the complement of the source, which Vulkan does express.
        if (key.source_factor == kFactorFixed && key.destination_factor == kFactorFixed &&
            ((call.blend.fixed_source + call.blend.fixed_destination) & 0x00FFFFFFu) == 0x00FFFFFFu)
            key.destination_factor = kFactorInverseConstant;
        key.equation = call.blend.equation;
        key.depth_test = call.depth.test_enabled && !call.through;
        key.depth_write = call.depth.write_enabled;
        key.depth_function = call.depth.function;
        key.cull = call.culling_enabled && !call.through && call.primitive != PrimitiveType::Sprites;
        key.cull_clockwise = call.cull_clockwise;
    }
    // Escape hatch for bisecting "nothing is visible" reports.
    static const bool no_cull = std::getenv("MGA_NO_CULL") != nullptr;
    static const bool no_depth = std::getenv("MGA_NO_DEPTH") != nullptr;
    if (no_cull) key.cull = false;
    if (no_depth && !call.clear_mode) key.depth_test = false;
    VkPipeline pipeline = impl.pipeline_for(key);
    if (pipeline == VK_NULL_HANDLE) return;

    PushConstants push{};
    push.transform = multiply(call.projection, view_world);
    push.viewport = {static_cast<float>(kPspWidth), static_cast<float>(kPspHeight), call.through ? 1.0f : 0.0f,
                     (fogged ? kPushFog : 0.0f) + (lit ? kPushLighting : 0.0f)};
    push.view_z = {view_world[2], view_world[6], view_world[10], view_world[14]};
    // Bit 4 asks the shader to snap texture coordinates to texel centres,
    // which turns the smooth sampler into a sharp one for this draw alone.
    // uv_transform carries 1/size in through mode, which is what it needs.
    //
    // Not when texture scaling is on: the shader snaps to the GUEST texel
    // grid, so snapping an enlarged texture would throw away every texel the
    // scaling just added and put the blocks straight back. With a scaled
    // texture, ordinary sampling already lands close to one screen pixel per
    // texel, which is the crispness the snap was there to get.
    const bool snap_uv = impl.smart_2d && !impl.sharp_textures && impl.texture_scale <= 1u &&
                         call.through && call.texture.enabled && call.texture.width != 0u &&
                         pixel_mapped_2d(impl.scratch);
    push.texture_params = {call.texture.enabled ? 1.0f : 0.0f,
                           static_cast<float>(call.texture.function | (call.texture.color_double ? 8u : 0u) |
                                              (snap_uv ? 16u : 0u)),
                           static_cast<float>(call.alpha_test.enabled ? call.alpha_test.reference : 0u),
                           static_cast<float>(call.alpha_test.enabled ? call.alpha_test.function : 0u)};
    // Through-mode texture coordinates are in texels, transformed ones in [0,1].
    push.uv_transform = call.through && call.texture.width != 0u
                            ? std::array<float, 4>{1.0f / static_cast<float>(call.texture.width),
                                                   1.0f / static_cast<float>(call.texture.height), 0.0f, 0.0f}
                            : std::array<float, 4>{call.texture.scale_u, call.texture.scale_v, call.texture.offset_u,
                                                   call.texture.offset_v};

    if (call.clear_mode) push.texture_params = {0.0f, 0.0f, 0.0f, 0.0f};

    static const bool trace_fb = std::getenv("MGA_TRACE_FB_TEXTURES") != nullptr;
    if (trace_fb && call.texture.enabled && !call.clear_mode) impl.trace_framebuffer_texture(call);

    // A texture in a framebuffer the renderer drew is read from that render
    // target: the pixels never reach guest memory, which holds whatever was
    // there before. MGA_NO_FB_TEXTURES decodes guest memory as before.
    static const bool no_fb_textures = std::getenv("MGA_NO_FB_TEXTURES") != nullptr;
    VkDescriptorSet texture_descriptor = impl.white_texture.descriptor;
    if (call.texture.enabled && !call.clear_mode) {
        const Impl::FramebufferTexture source =
            no_fb_textures ? Impl::FramebufferTexture{} : impl.find_framebuffer_texture(memory, call.texture);
        const VkDescriptorSet copy =
            source.target != nullptr
                ? impl.framebuffer_descriptor(*source.target, call.texture.format == TextureFormat::Rgba5650)
                : VK_NULL_HANDLE;
        if (copy != VK_NULL_HANDLE) {
            // Texture coordinates address the game's texture, whose first texel
            // is pixel (x, y) of a target that holds 480x272 guest pixels at
            // any internal scale.
            texture_descriptor = copy;
            const float width = static_cast<float>(call.texture.width);
            const float height = static_cast<float>(call.texture.height);
            const std::array<float, 4> uv = push.uv_transform;
            push.uv_transform = {uv[0] * width / static_cast<float>(kPspWidth),
                                 uv[1] * height / static_cast<float>(kPspHeight),
                                 (uv[2] * width + static_cast<float>(source.x)) / static_cast<float>(kPspWidth),
                                 (uv[3] * height + static_cast<float>(source.y)) / static_cast<float>(kPspHeight)};
        } else {
            texture_descriptor = impl.texture_for(memory, call.texture).descriptor;
        }
    }

    if (!impl.pass_active || impl.current_target != call.target.color_address) {
        impl.end_pass();
        impl.begin_pass(call.target.color_address);
        if (!impl.pass_active) return;
    }
    {
        Impl::Target &drawn = impl.targets.at(call.target.color_address);
        const bool layout_changed =
            drawn.stride != call.target.color_stride || drawn.format != call.target.color_format;
        drawn.stride = call.target.color_stride;
        drawn.format = call.target.color_format;
        if (layout_changed || drawn.guest_words.empty() || drawn.last_drawn_frame != impl.frames)
            impl.snapshot_guest_words(memory, call.target.color_address, drawn);
        drawn.last_drawn_frame = impl.frames;
        drawn.draw_serial = ++impl.target_draw_counter;
    }
    // The GE viewport maps normalised device coordinates onto the screen as
    //   screen = ndc * scale + offset - region_offset
    // with a negative y scale (PSP device y points up, the screen down) and a z
    // scale/offset that drives the 16-bit depth buffer. Folding all of that into
    // the Vulkan viewport reproduces the PSP's screen space exactly, including a
    // reversed depth range, and keeps triangle winding as the GE sees it. Through
    // draws bypass the transform, so they keep the plain full-target viewport.
    const float render_scale = static_cast<float>(impl.target_extent.width) / static_cast<float>(kPspWidth);
    VkViewport vk_viewport{0.0f, 0.0f, static_cast<float>(impl.target_extent.width),
                           static_cast<float>(impl.target_extent.height), 0.0f, 1.0f};
    if (!call.through && call.viewport.x_scale != 0.0f && call.viewport.y_scale != 0.0f) {
        const ViewportState &vp = call.viewport;
        vk_viewport.x = (vp.x_offset - vp.offset_x - vp.x_scale) * render_scale;
        vk_viewport.y = (vp.y_offset - vp.offset_y - vp.y_scale) * render_scale;
        vk_viewport.width = 2.0f * vp.x_scale * render_scale;
        // The GE's y scale is negative (device y points up, the screen down), so
        // this is a flipping viewport. That keeps framebuffer space identical to
        // the PSP's screen space, which is what the cull winding is defined in.
        vk_viewport.height = 2.0f * vp.y_scale * render_scale;
        // Which way the depth buffer runs, for the contact shadows in the
        // post pass: a negative z scale puts the far plane at 0.
        impl.depth_reversed = vp.z_scale < 0.0f;
        if (vp.z_scale != 0.0f) {
            // The shader hands over device z in [0, 1]; this undoes that halving
            // and applies the GE's z scale/offset, reproducing a reversed range
            // when the guest set one with sceGuDepthRange(65535, 0).
            vk_viewport.minDepth = std::clamp((vp.z_offset - vp.z_scale) / 65535.0f, 0.0f, 1.0f);
            vk_viewport.maxDepth = std::clamp((vp.z_offset + vp.z_scale) / 65535.0f, 0.0f, 1.0f);
        }
    }
    vkCmdSetViewport(impl.command_buffer, 0u, 1u, &vk_viewport);

    // Whichever side asked for the constant decides its colour; the source wins
    // when both do, because the destination is then its complement.
    const std::uint32_t constant_color =
        key.source_factor == kFactorFixed ? call.blend.fixed_source : call.blend.fixed_destination;
    const std::array<float, 4> blend_constants{static_cast<float>(constant_color & 0xFFu) / 255.0f,
                                               static_cast<float>((constant_color >> 8u) & 0xFFu) / 255.0f,
                                               static_cast<float>((constant_color >> 16u) & 0xFFu) / 255.0f, 1.0f};
    vkCmdSetBlendConstants(impl.command_buffer, blend_constants.data());

    const auto clamp_axis = [](std::uint32_t value, std::uint32_t limit) { return std::min(value, limit); };
    const std::uint32_t sx1 = clamp_axis(call.viewport.scissor_x1, kPspWidth - 1u);
    const std::uint32_t sy1 = clamp_axis(call.viewport.scissor_y1, kPspHeight - 1u);
    const std::uint32_t sx2 = clamp_axis(std::max(call.viewport.scissor_x2, sx1), kPspWidth - 1u);
    const std::uint32_t sy2 = clamp_axis(std::max(call.viewport.scissor_y2, sy1), kPspHeight - 1u);
    VkRect2D vk_scissor{};
    vk_scissor.offset = {static_cast<std::int32_t>(sx1 * static_cast<std::uint32_t>(render_scale)),
                         static_cast<std::int32_t>(sy1 * static_cast<std::uint32_t>(render_scale))};
    vk_scissor.extent = {(sx2 - sx1 + 1u) * static_cast<std::uint32_t>(render_scale),
                         (sy2 - sy1 + 1u) * static_cast<std::uint32_t>(render_scale)};
    vkCmdSetScissor(impl.command_buffer, 0u, 1u, &vk_scissor);

    if (pipeline != impl.bound_pipeline) {
        vkCmdBindPipeline(impl.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        impl.bound_pipeline = pipeline;
    }
    vkCmdBindDescriptorSets(impl.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, impl.pipeline_layout, 0u, 1u,
                            &texture_descriptor, 0u, nullptr);
    // Every pipeline shares one layout, so set 1 stays bound across pipeline
    // and texture changes; it is bound again only when a block moved.
    const std::array<std::uint32_t, 2> lighting_offsets{impl.environment_offset, impl.object_offset};
    if (!impl.lighting_bound || lighting_offsets != impl.bound_lighting_offsets) {
        vkCmdBindDescriptorSets(impl.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, impl.pipeline_layout, 1u, 1u,
                                &impl.lighting_descriptor, static_cast<std::uint32_t>(lighting_offsets.size()),
                                lighting_offsets.data());
        impl.bound_lighting_offsets = lighting_offsets;
        impl.lighting_bound = true;
    }
    vkCmdPushConstants(impl.command_buffer, impl.pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0u, sizeof(push), &push);
    const VkDeviceSize offset = impl.vertex_offset;
    vkCmdBindVertexBuffers(impl.command_buffer, 0u, 1u, &impl.vertex_buffer, &offset);
    vkCmdDraw(impl.command_buffer, static_cast<std::uint32_t>(impl.scratch.size()), 1u, 0u, 0u);
    impl.vertex_offset += bytes;
    ++impl.draws;
}

void VulkanRenderer::draw_shadow_blobs(const GuestMemory &memory) {
    Impl &impl = *impl_;
    if (!impl.last_scene.valid || !impl.shadow_transform_valid) return;

    // A fan: one dark vertex at the centre and a ring of transparent ones, so
    // the blob fades out towards its edge without needing a texture.
    constexpr std::size_t kSegments = 16u;
    // High enough off the floor not to fight it for the depth buffer, low
    // enough to stay hidden under a step or a crate.
    constexpr float kLift = 10.0f;
    constexpr float kMaxAlpha = 165.0f;

    const auto alpha =
        static_cast<std::uint32_t>(std::lround(std::clamp(impl.blob_strength, 0.0f, 1.0f) * kMaxAlpha));
    if (alpha == 0u) return;
    // The GE's vertex colour is ABGR, so black with an alpha is the alpha
    // alone in the top byte.
    const std::uint32_t centre = alpha << 24u;
    const std::uint32_t rim = 0u;

    DrawCall blob{};
    blob.primitive = PrimitiveType::Triangles;
    blob.target = impl.last_scene.target;
    blob.viewport = impl.last_scene.viewport;
    // The vertices are in world space and the game's own world-to-clip matrix
    // takes them the whole way, so the view and world matrices are the
    // identity and the renderer's projection slot carries that matrix. This
    // sidesteps the question of what space the display list is drawn in --
    // which is what put the first attempt's blobs somewhere out in the scene
    // instead of under the characters.
    constexpr std::array<float, 16> kIdentity{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                              0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    blob.view = kIdentity;
    blob.world = kIdentity;
    blob.projection = impl.shadow_transform;
    blob.has_vertex_color = true;
    blob.lighting_enabled = false;
    blob.culling_enabled = false;
    blob.blend.enabled = true;
    blob.blend.source_factor = 2u;       // source alpha
    blob.blend.destination_factor = 3u;  // one minus source alpha
    // Tested against the scene so a blob is hidden by anything in front of
    // it, but never written, so it cannot occlude what comes after.
    blob.depth = impl.last_scene.depth;
    blob.depth.test_enabled = true;
    blob.depth.write_enabled = false;
    blob.environment_version = impl.last_scene.environment_version;
    blob.material_version = impl.last_scene.material_version;
    blob.vertices.reserve(kSegments * 3u);

    for (const ShadowCaster &caster : impl.shadow_casters) {
        const float radius = caster.radius;
        if (!(radius > 0.0f)) continue;
        const std::array<float, 3> at{caster.position[0], caster.ground + kLift, caster.position[2]};
        blob.vertices.clear();
        for (std::size_t i = 0; i < kSegments; ++i) {
            const auto angle = [&](std::size_t step) {
                return 6.283185307f * static_cast<float>(step % kSegments) / static_cast<float>(kSegments);
            };
            const float a0 = angle(i);
            const float a1 = angle(i + 1u);
            Vertex middle{};
            middle.position = {at[0], at[1], at[2], 1.0f};
            middle.color = centre;
            Vertex first{};
            first.position = {at[0] + std::cos(a0) * radius, at[1], at[2] + std::sin(a0) * radius, 1.0f};
            first.color = rim;
            Vertex second{};
            second.position = {at[0] + std::cos(a1) * radius, at[1], at[2] + std::sin(a1) * radius, 1.0f};
            second.color = rim;
            blob.vertices.push_back(middle);
            blob.vertices.push_back(first);
            blob.vertices.push_back(second);
        }
        submit(blob, memory);
    }
}

void VulkanRenderer::write_back_frame(GuestMemory &memory) {
    Impl &impl = *impl_;
    if (!impl.ready || !impl.writeback_has_pixels) return;
    impl.writeback_has_pixels = false;
    impl.store_frame(memory, impl.writeback_ready, impl.writeback_pixels.data());
}

void VulkanRenderer::read_back_framebuffer(std::uint32_t source, GuestMemory &memory) {
    Impl &impl = *impl_;
    static const bool disabled = std::getenv("MGA_NO_FB_TEXTURES") != nullptr;
    if (!impl.ready || disabled) return;
    const std::uint32_t wanted = GuestMemory::canonical(source);
    std::uint32_t found = 0u;
    bool any = false;
    for (const auto &[address, target] : impl.targets) {
        if (!target.initialized || target.guest_words.empty()) continue;
        const std::uint32_t base = GuestMemory::canonical(address);
        const std::uint32_t bytes = target.stride * kPspHeight * framebuffer_bytes_per_pixel(target.format);
        if (wanted >= base && wanted - base < bytes) {
            found = address;
            any = true;
            break;
        }
    }
    if (!any) return;
    if (!impl.recording) begin_frame();
    impl.end_pass();
    impl.record_writeback(found);
    if (!impl.writeback_in_flight) return;
    // Run everything recorded so far and carry on recording afterwards.
    vkEndCommandBuffer(impl.command_buffer);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &impl.command_buffer;
    const perf::Clock::time_point wait_start = perf::Clock::now();
    vkQueueSubmit(impl.queue, 1u, &submit, impl.frame_fence);
    vkWaitForFences(impl.device, 1u, &impl.frame_fence, VK_TRUE, UINT64_MAX);
    perf::add_wait_time(perf::Clock::now() - wait_start);
    vkResetFences(impl.device, 1u, &impl.frame_fence);
    vkResetCommandBuffer(impl.command_buffer, 0u);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(impl.command_buffer, &begin);
    impl.vertex_offset = 0u;
    impl.environment_version = 0u;
    impl.object_valid = false;
    impl.forget_bindings();
    impl.writeback_in_flight = false;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(kPspWidth) * kPspHeight);
    std::memcpy(pixels.data(), impl.writeback_mapped, pixels.size() * 4u);
    impl.store_frame(memory, impl.writeback_recorded, pixels.data());
    // A write-back still waiting from an earlier frame is older than this one.
    if (impl.writeback_ready.address == found) impl.writeback_has_pixels = false;
    static const bool trace = std::getenv("MGA_TRACE_FB_TEXTURES") != nullptr;
    if (trace)
        std::cout << "[fbtex] frame " << impl.frames << " read framebuffer 0x" << std::hex << found << std::dec
                  << " back for a block transfer\n";
}

void VulkanRenderer::Impl::store_frame(GuestMemory &memory, const WritebackFrame &frame,
                                       const std::uint32_t *pixels) {
    Impl &impl = *this;
    const std::uint32_t bytes_per_pixel = framebuffer_bytes_per_pixel(frame.format);
    const std::size_t bytes = static_cast<std::size_t>(frame.stride) * kPspHeight * bytes_per_pixel;
    std::uint8_t *out = memory.raw_pointer(frame.address, bytes);
    if (out == nullptr) return;
    for (std::uint32_t y = 0; y < kPspHeight; ++y) {
        const std::uint32_t *row = pixels + static_cast<std::size_t>(y) * kPspWidth;
        std::uint8_t *line = out + static_cast<std::size_t>(y) * frame.stride * bytes_per_pixel;
        if (frame.format == 3u) {
            std::memcpy(line, row, static_cast<std::size_t>(kPspWidth) * 4u);
            continue;
        }
        for (std::uint32_t x = 0; x < kPspWidth; ++x) {
            // Red in the low bits, as texture_decode's expand_* read them.
            const std::uint32_t pixel = row[x];
            const std::uint32_t r = pixel & 0xFFu;
            const std::uint32_t g = (pixel >> 8u) & 0xFFu;
            const std::uint32_t b = (pixel >> 16u) & 0xFFu;
            const std::uint32_t a = pixel >> 24u;
            std::uint32_t value = 0u;
            if (frame.format == 0u)
                value = (r >> 3u) | (g >> 2u) << 5u | (b >> 3u) << 11u;
            else if (frame.format == 1u)
                value = (r >> 3u) | (g >> 3u) << 5u | (b >> 3u) << 10u | (a >> 7u) << 15u;
            else
                value = (r >> 4u) | (g >> 4u) << 4u | (b >> 4u) << 8u | (a >> 4u) << 12u;
            line[x * 2u] = static_cast<std::uint8_t>(value & 0xFFu);
            line[x * 2u + 1u] = static_cast<std::uint8_t>(value >> 8u);
        }
    }
    // The bytes now differ from the snapshot taken when the target was drawn,
    // and are exactly what it shows: take the snapshot again.
    const auto target = impl.targets.find(frame.address);
    if (target != impl.targets.end()) impl.snapshot_guest_words(memory, frame.address, target->second);
}

void VulkanRenderer::present(std::uint32_t display_address) {
    Impl &impl = *impl_;
    if (!impl.ready) return;
    if (!impl.recording) begin_frame();
    impl.end_pass();

    static const bool frame_digest_on = std::getenv("MGA_FRAME_DIGEST") != nullptr;
    if (frame_digest_on) {
        const auto &now = impl.frame_digest;
        const auto &before = impl.previous_digest;
        // Same count, order, kind and size of draws: the pairing that frame
        // interpolation would rely on is available.
        bool same_shape = now.size() == before.size();
        std::size_t moved = 0u;
        std::size_t moved_through = 0u;
        if (same_shape) {
            for (std::size_t i = 0; i < now.size(); ++i) {
                // The target address is deliberately not compared: the game
                // double-buffers, so it alternates every frame and would make
                // every frame look like a changed list.
                if (now[i].through != before[i].through || now[i].vertices != before[i].vertices) {
                    same_shape = false;
                    break;
                }
                if (now[i].hash != before[i].hash) {
                    ++moved;
                    if (now[i].through) ++moved_through;
                }
            }
        }
        std::size_t through = 0u;
        for (const auto &draw : now) through += draw.through ? 1u : 0u;
        std::cout << "[digest] frame " << impl.frames << " draws=" << now.size() << " (through " << through
                  << ") shape=" << (same_shape ? "same" : "CHANGED");
        if (same_shape)
            std::cout << " moved=" << moved << "/" << now.size() << " (2D " << moved_through << ")"
                      << (moved == 0u ? "  (identical frame)" : "");
        std::cout << "\n";
        impl.previous_digest = impl.frame_digest;
        impl.frame_digest.clear();
    }
    static const bool trace_objects_on = std::getenv("MGA_TRACE_OBJECTS") != nullptr;
    if (trace_objects_on && !impl.frame_objects.empty()) {
        // The camera's position is baked into every world matrix, so when the
        // camera moves EVERY object's translation shifts by the same amount.
        // Comparing translations between frames therefore finds the camera, not
        // the characters. The median shift is the camera; an object whose shift
        // differs from it is one that genuinely moved.
        //
        // Objects are matched between frames by their draw signature (vertex
        // and draw count) rather than position, for the same reason.
        std::vector<std::array<float, 3>> deltas;
        std::vector<std::pair<const Impl::FrameObject *, std::array<float, 3>>> shifted;
        for (const Impl::FrameObject &o : impl.frame_objects) {
            const Impl::FrameObject *best = nullptr;
            float best_distance = 1e30f;
            for (const Impl::FrameObject &q : impl.previous_objects) {
                if (q.vertices != o.vertices || q.draws != o.draws) continue;
                const float dx = q.at[0] - o.at[0], dy = q.at[1] - o.at[1], dz = q.at[2] - o.at[2];
                const float distance = dx * dx + dy * dy + dz * dz;
                if (distance < best_distance) { best_distance = distance; best = &q; }
            }
            if (best == nullptr) continue;
            const std::array<float, 3> delta{o.at[0] - best->at[0], o.at[1] - best->at[1], o.at[2] - best->at[2]};
            deltas.push_back(delta);
            shifted.emplace_back(&o, delta);
        }
        std::array<float, 3> camera{};
        for (std::uint32_t axis = 0; axis < 3u; ++axis) {
            std::vector<float> values;
            values.reserve(deltas.size());
            for (const auto &delta : deltas) values.push_back(delta[axis]);
            if (values.empty()) continue;
            std::nth_element(values.begin(), values.begin() + values.size() / 2u, values.end());
            camera[axis] = values[values.size() / 2u];
        }
        std::size_t movers = 0u;
        std::cout << "[objects] frame " << impl.frames << " distinct=" << impl.frame_objects.size()
                  << " matched=" << shifted.size() << " camera=(" << camera[0] << ", " << camera[1] << ", "
                  << camera[2] << ")\n";
        for (const auto &[object, delta] : shifted) {
            if (std::abs(delta[0] - camera[0]) < 0.5f && std::abs(delta[1] - camera[1]) < 0.5f &&
                std::abs(delta[2] - camera[2]) < 0.5f)
                continue;
            if (++movers > 12u) break;
            std::cout << "   moving at (" << object->at[0] << ", " << object->at[1] << ", " << object->at[2]
                      << ") verts=" << object->vertices << " draws=" << object->draws << " by ("
                      << delta[0] - camera[0] << ", " << delta[1] - camera[1] << ", " << delta[2] - camera[2]
                      << ")\n";
        }
        // Objects that hold their position but whose vertices change: a
        // character animating in place. This is what a matrix-only test misses
        // entirely, and for a mostly-fixed camera it is most of them.
        std::size_t animated = 0u;
        for (const Impl::FrameObject &o : impl.frame_objects) {
            const auto same = std::find_if(impl.previous_objects.begin(), impl.previous_objects.end(),
                                           [&](const Impl::FrameObject &q) {
                                               return q.vertices == o.vertices && q.draws == o.draws &&
                                                      std::abs(q.at[0] - o.at[0]) < 0.5f &&
                                                      std::abs(q.at[1] - o.at[1]) < 0.5f &&
                                                      std::abs(q.at[2] - o.at[2]) < 0.5f;
                                           });
            if (same == impl.previous_objects.end() || same->hash == o.hash) continue;
            if (++animated > 12u) break;
            std::cout << "   animated at (" << o.at[0] << ", " << o.at[1] << ", " << o.at[2] << ") verts="
                      << o.vertices << " draws=" << o.draws << "\n";
        }
        // Every object once, smallest first, so characters are not hidden behind
        // the scenery in a top-by-size list. One frame in sixty is plenty.
        if (impl.frames % 60u == 0u) {
            auto sorted = impl.frame_objects;
            std::sort(sorted.begin(), sorted.end(),
                      [](const Impl::FrameObject &a, const Impl::FrameObject &b) { return a.vertices < b.vertices; });
            std::cout << "   -- all objects, smallest first --\n";
            for (const Impl::FrameObject &o : sorted)
                std::cout << "   at (" << o.at[0] << ", " << o.at[1] << ", " << o.at[2] << ") verts=" << o.vertices
                          << " draws=" << o.draws << "\n";
        }
        impl.previous_objects = impl.frame_objects;
        impl.frame_objects.clear();
    }
    static const bool trace3d = std::getenv("MGA_TRACE_3D") != nullptr;
    static const bool trace_camera = std::getenv("MGA_TRACE_CAMERA") != nullptr;
    if (trace3d && impl.frame_transformed_draws != 0u) {
        std::cout << "[3d] frame " << impl.frames << " through=" << impl.frame_through_draws
                  << " transformed=" << impl.frame_transformed_draws << " showing=0x" << std::hex << display_address
                  << " transformed targets:";
        for (const auto &[address, count] : impl.frame_transformed_targets)
            std::cout << " 0x" << address << "x" << std::dec << count << std::hex;
        std::cout << std::dec << "\n";
        std::cout << "     verts=" << impl.frame_transformed_vertices << " onscreen=" << impl.frame_onscreen_vertices
                  << " behind=" << impl.frame_behind_camera << " ndc x[" << impl.frame_ndc_min[0] << ","
                  << impl.frame_ndc_max[0] << "] y[" << impl.frame_ndc_min[1] << "," << impl.frame_ndc_max[1]
                  << "] z[" << impl.frame_ndc_min[2] << "," << impl.frame_ndc_max[2] << "]\n";
    }
    if (trace_camera && !impl.frame_views.empty()) {
        // The busiest view matrix of the frame is the scene the player looks
        // at; the others belong to reflections and shadow passes.
        const auto scene = std::max_element(impl.frame_views.begin(), impl.frame_views.end(),
                                            [](const auto &a, const auto &b) { return a.second < b.second; });
        const std::array<float, 16> &view = scene->first;
        // A view matrix holds the camera's own axes as the rows of its
        // rotation part, and the array is column major, so row 2 — the
        // direction the camera looks along — is elements 2, 6 and 10.
        const float forward_x = view[2];
        const float forward_y = view[6];
        const float forward_z = view[10];
        constexpr float kDegrees = 57.29577951308232f;
        const float yaw = std::atan2(forward_x, forward_z) * kDegrees;
        const float pitch = std::asin(std::clamp(forward_y, -1.0f, 1.0f)) * kDegrees;
        // The camera's world position is the rotation applied backwards to the
        // translation, which tells a turn in place from the hunter walking.
        const float tx = view[12];
        const float ty = view[13];
        const float tz = view[14];
        const float px = -(view[0] * tx + view[1] * ty + view[2] * tz);
        const float py = -(view[4] * tx + view[5] * ty + view[6] * tz);
        const float pz = -(view[8] * tx + view[9] * ty + view[10] * tz);
        float turn = yaw - impl.traced_yaw;
        while (turn > 180.0f) turn -= 360.0f;
        while (turn < -180.0f) turn += 360.0f;
        impl.traced_yaw = yaw;
        const std::ios::fmtflags flags = std::cout.flags();
        std::cout << std::fixed << std::setprecision(4) << "[camera] frame " << impl.frames << " stick="
                  << static_cast<int>(impl.pad.right_x) - 0x80 << "," << static_cast<int>(impl.pad.right_y) - 0x80
                  << " yaw=" << yaw << " pitch=" << pitch << " turn=" << turn << " pos=" << std::setprecision(1) << px
                  << "," << py << "," << pz << " views=" << impl.frame_views.size() << "\n";
        std::cout.flags(flags);
    }
    // MGA_TRACE_SHADOWS: one line a second naming every link in the chain, so
    // a single run says which one is failing rather than which might be.
    static const bool trace_shadows = std::getenv("MGA_TRACE_SHADOWS") != nullptr;
    if (trace_shadows) {
        static std::uint64_t last_report = 0u;
        if (impl.frames >= last_report + 60u) {
            last_report = impl.frames;
            const Impl::ShadowTrace &s = impl.shadow_trace;
            std::cout << "[shadow] built=" << (impl.shadow_available ? 1 : 0)
                      << " strength=" << std::fixed << std::setprecision(2) << impl.shadow_strength
                      << " | transformed=" << s.transformed << " lit=" << s.lit << " identity=" << s.identity
                      << " both=" << s.identity_lit << " casterverts=" << s.casters
                      << " | resolved=" << (s.resolved ? 1 : 0) << " light=" << impl.shadow_light;
            if (impl.shadow_light >= 0 && impl.last_lighting_valid) {
                const LightState &light = impl.last_lighting.lights[static_cast<std::size_t>(impl.shadow_light)];
                std::cout << " type=" << light.type << " diffuse=" << psprecomp_hex(light.diffuse);
            }
            if (impl.shadow_map != nullptr && impl.shadow_map->has_box()) {
                const auto &lo = impl.shadow_map->box_minimum();
                const auto &hi = impl.shadow_map->box_maximum();
                const auto &dir = impl.shadow_map->light_direction();
                std::cout << "\n[shadow] box (" << lo[0] << ", " << lo[1] << ", " << lo[2] << ") .. (" << hi[0]
                          << ", " << hi[1] << ", " << hi[2] << ")  towards light (" << dir[0] << ", " << dir[1]
                          << ", " << dir[2] << ")";
            }
            std::cout << std::endl;
        }
        impl.shadow_trace = {};
    }
    impl.frame_views.clear();
    impl.frame_through_draws = 0u;
    impl.blobs_drawn = false;
    impl.frame_transformed_draws = 0u;
    impl.frame_transformed_vertices = 0u;
    impl.frame_onscreen_vertices = 0u;
    impl.frame_behind_camera = 0u;
    impl.frame_ndc_min = {1e30f, 1e30f, 1e30f};
    impl.frame_ndc_max = {-1e30f, -1e30f, -1e30f};
    impl.frame_transformed_targets.clear();

    static const bool no_writeback = std::getenv("MGA_NO_FB_TEXTURES") != nullptr;
    if (!no_writeback) impl.record_writeback(display_address);

    // Show the target the guest flipped to; fall back to whatever was drawn last.
    auto displayed = impl.targets.find(display_address);
    if (displayed == impl.targets.end()) displayed = impl.targets.find(impl.last_drawn_target);
    const Impl::Target *source = displayed != impl.targets.end() ? &displayed->second : nullptr;
    impl.presented_target = displayed != impl.targets.end() ? displayed->first : 0u;
    if (impl.holding) source = &impl.held;
    impl.submit_and_present(source, true);
    ++impl.frames;
}

bool VulkanRenderer::capture_frame(const std::string &path) {
    Impl &impl = *impl_;
    if (!impl.ready) return false;
    vkDeviceWaitIdle(impl.device);

    auto shown = impl.targets.find(impl.presented_target);
    if (shown == impl.targets.end()) return false;
    const VkImage captured = shown->second.color;
    const std::uint32_t width = impl.target_extent.width;
    const std::uint32_t height = impl.target_extent.height;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4u;
    VkBuffer staging{};
    VkDeviceMemory staging_memory{};
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = bytes;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(impl.device, &buffer_info, nullptr, &staging) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(impl.device, staging, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = impl.find_memory_type(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(impl.device, &allocate, nullptr, &staging_memory);
    vkBindBufferMemory(impl.device, staging, staging_memory, 0u);

    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = impl.command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1u;
    VkCommandBuffer commands{};
    vkAllocateCommandBuffers(impl.device, &command_info, &commands);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commands, &begin);
    impl.transition(commands, captured, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
    copy.imageExtent = {width, height, 1u};
    vkCmdCopyImageToBuffer(commands, captured, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1u, &copy);
    impl.transition(commands, captured, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vkEndCommandBuffer(commands);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &commands;
    vkQueueSubmit(impl.queue, 1u, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(impl.queue);
    vkFreeCommandBuffers(impl.device, impl.command_pool, 1u, &commands);

    void *mapped = nullptr;
    vkMapMemory(impl.device, staging_memory, 0u, bytes, 0u, &mapped);
    std::vector<std::uint8_t> pixels(static_cast<const std::uint8_t *>(mapped),
                                     static_cast<const std::uint8_t *>(mapped) + bytes);
    vkUnmapMemory(impl.device, staging_memory);
    vkDestroyBuffer(impl.device, staging, nullptr);
    vkFreeMemory(impl.device, staging_memory, nullptr);

    // The capture is the game's own target, before the window blit; draw the
    // overlay over it the same way, so captures show what the window shows.
    if (impl.overlay_visible) {
        const std::uint32_t scale = perf::overlay_scale(height);
        const std::uint32_t inset = 4u * scale;
        if (inset + perf::kOverlayWidth * scale <= width && inset + perf::kOverlayHeight * scale <= height) {
            for (std::uint32_t y = 0; y < perf::kOverlayHeight * scale; ++y) {
                std::uint8_t *row = pixels.data() + static_cast<std::size_t>(inset + y) * width * 4u;
                for (std::uint32_t x = 0; x < perf::kOverlayWidth * scale; ++x) {
                    const std::uint32_t pixel = impl.overlay_pixels[(y / scale) * perf::kOverlayWidth + x / scale];
                    std::memcpy(row + (inset + x) * 4u, &pixel, 4u);
                }
            }
        }
    }
    return write_bmp(path, pixels.data(), width, height, false);
}

void VulkanRenderer::shutdown() {
    if (impl_ && impl_->gamepad != nullptr) {
        SDL_CloseGamepad(impl_->gamepad);
        impl_->gamepad = nullptr;
    }
    if (!impl_ || impl_->device == VK_NULL_HANDLE) {
        if (impl_ && impl_->window != nullptr) {
            SDL_DestroyWindow(impl_->window);
            impl_->window = nullptr;
        }
        return;
    }
    Impl &impl = *impl_;
    vkDeviceWaitIdle(impl.device);
    // The interface may still be up when the game quits from its menu.
    if (impl.ui_ready && ImGui::GetCurrentContext() != nullptr) ImGui_ImplVulkan_Shutdown();
    impl.ui_ready = false;
    impl.destroy_swapchain_views();
    if (impl.ui_render_pass != VK_NULL_HANDLE) vkDestroyRenderPass(impl.device, impl.ui_render_pass, nullptr);
    for (auto &[key, texture] : impl.textures) impl.destroy_texture(texture);
    impl.textures.clear();
    impl.destroy_texture(impl.white_texture);
    if (impl.overlay_mapped != nullptr) vkUnmapMemory(impl.device, impl.overlay_staging_memory);
    vkDestroyBuffer(impl.device, impl.overlay_staging, nullptr);
    vkFreeMemory(impl.device, impl.overlay_staging_memory, nullptr);
    vkDestroyImageView(impl.device, impl.overlay_view, nullptr);
    vkDestroyImage(impl.device, impl.overlay_image, nullptr);
    vkFreeMemory(impl.device, impl.overlay_memory, nullptr);
    impl.destroy_upload();
    impl.destroy_writeback();
    for (auto &[key, pipeline] : impl.pipelines) vkDestroyPipeline(impl.device, pipeline, nullptr);
    impl.pipelines.clear();
    if (impl.vertex_mapped != nullptr) vkUnmapMemory(impl.device, impl.vertex_memory);
    vkDestroyBuffer(impl.device, impl.vertex_buffer, nullptr);
    vkFreeMemory(impl.device, impl.vertex_memory, nullptr);
    vkDestroySampler(impl.device, impl.sampler, nullptr);
    vkDestroySampler(impl.device, impl.sharp_sampler, nullptr);
    vkDestroySampler(impl.device, impl.clamp_sampler, nullptr);
    vkDestroySampler(impl.device, impl.clamp_sharp_sampler, nullptr);
    if (impl.shadow_map) impl.shadow_map->destroy();
    impl.drop_post_descriptors();
    vkDestroyPipeline(impl.device, impl.post_pipeline, nullptr);
    vkDestroyPipelineLayout(impl.device, impl.post_pipeline_layout, nullptr);
    vkDestroyDescriptorSetLayout(impl.device, impl.post_set_layout, nullptr);
    vkDestroyShaderModule(impl.device, impl.post_vertex_shader, nullptr);
    vkDestroyShaderModule(impl.device, impl.post_fragment_shader, nullptr);
    vkDestroyRenderPass(impl.device, impl.post_render_pass, nullptr);
    vkDestroyDescriptorPool(impl.device, impl.descriptor_pool, nullptr);
    vkDestroyDescriptorSetLayout(impl.device, impl.descriptor_layout, nullptr);
    vkDestroyDescriptorSetLayout(impl.device, impl.lighting_layout, nullptr);
    vkDestroyPipelineLayout(impl.device, impl.pipeline_layout, nullptr);
    vkDestroyShaderModule(impl.device, impl.vertex_shader, nullptr);
    vkDestroyShaderModule(impl.device, impl.fragment_shader, nullptr);
    for (auto &[address, target] : impl.targets) impl.destroy_target(target);
    impl.targets.clear();
    impl.destroy_target(impl.held);
    impl.held = {};
    impl.holding = false;
    vkDestroyRenderPass(impl.device, impl.render_pass, nullptr);
    vkDestroySemaphore(impl.device, impl.image_available, nullptr);
    vkDestroySemaphore(impl.device, impl.render_finished, nullptr);
    vkDestroyFence(impl.device, impl.frame_fence, nullptr);
    vkDestroyCommandPool(impl.device, impl.command_pool, nullptr);
    vkDestroySwapchainKHR(impl.device, impl.swapchain, nullptr);
    vkDestroyDevice(impl.device, nullptr);
    if (impl.surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(impl.instance, impl.surface, nullptr);
    vkDestroyInstance(impl.instance, nullptr);
    if (impl.window != nullptr) SDL_DestroyWindow(impl.window);
    impl = Impl{};
}

} // namespace mga::gpu
