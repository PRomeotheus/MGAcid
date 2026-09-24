#pragma once

// Shadow maps cast from the game's own lights.
//
// Metal Gear Ac!d's scenery has baked shadows and its characters have none, so
// nothing in the game drops a shadow that answers to where the light is. The
// pieces to build one are all present in the display list, though, and they all
// happen to live in the same space:
//
//   * The GE's lighting is evaluated after the world matrix, so the light
//     positions the game sets and the geometry it draws share one space --
//     world space with the camera's position folded in, which is what every
//     world matrix in an MGA frame carries. Call it draw space. No camera, no
//     inverse projection and no guesswork is needed to relate the two.
//   * Characters arrive skinned on the CPU, pre-transformed, with an identity
//     world matrix. Their vertices are therefore already in draw space and can
//     be copied straight into a shadow pass.
//
// So: collect the casters' positions as the frame is drawn, work out where the
// game's brightest light is, and render those positions into a depth map from
// the light's point of view. Geometry drawn afterwards compares its own depth
// in that map to find out whether something stands between it and the light.
//
// The awkward part is ordering. A forward renderer has already shaded the floor
// by the time it has seen the characters standing on it, so the map cannot be
// built where it is needed. Using the previous frame's map instead does not
// work either: draw space moves with the camera, so a map built one frame ago
// would slide across the scene whenever the camera turned.
//
// So the map is built from THIS frame's casters, into a command buffer of its
// own that is submitted ahead of the scene's. Recording order and execution
// order are different things, which is what makes that possible: the casters
// can be collected while the scene is being recorded, and the pass that uses
// them still runs first.
//
// The light's box has to be sized before the casters are known, though, so it
// is sized from the previous frame's extent. A box is a loose fit by nature and
// both its centre and its size move slowly, so the cost is at worst a shadow
// clipped at the edge of the box for a single frame after something moves
// sharply.
//
// This file deliberately knows nothing about the renderer that drives it: it
// takes a device and a command buffer and owns only its own target, pipeline
// and geometry. That keeps it compilable on its own.

#include "ge_state.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mga::gpu {

class ShadowMap {
public:
    // What the map needs to know about the device it lives on.
    struct DeviceContext {
        VkDevice device{};
        VkPhysicalDevice physical_device{};
    };

    ShadowMap() = default;
    ~ShadowMap();
    ShadowMap(const ShadowMap &) = delete;
    ShadowMap &operator=(const ShadowMap &) = delete;

    // `resolution` is the square map's edge. `vertex_spirv` is the compiled
    // shadow.vert. Returns false with `error` set; a failure leaves nothing
    // behind and ready() false, and the caller should carry on without
    // shadows rather than treat it as fatal.
    bool create(const DeviceContext &context, std::uint32_t resolution, const std::uint32_t *vertex_spirv,
                std::size_t vertex_spirv_bytes, std::string &error);
    void destroy();
    [[nodiscard]] bool ready() const noexcept { return pipeline_ != VK_NULL_HANDLE; }

    // The depth map, for the shading pass to sample. Valid once created; its
    // contents are meaningful only after a frame in which recorded() was true.
    [[nodiscard]] VkImageView view() const noexcept { return depth_view_; }
    [[nodiscard]] VkImage image() const noexcept { return depth_; }
    [[nodiscard]] std::uint32_t resolution() const noexcept { return resolution_; }

    // --- collecting this frame's casters -------------------------------------

    // Drops whatever the frame before last left behind and starts again.
    void begin_frame();
    // Adds one draw's worth of triangle positions. `stride` is the distance
    // between vertices in floats, so the renderer can hand over its own vertex
    // structure without repacking it, and `world` is the draw's world matrix,
    // applied here so that object-space and pre-transformed geometry can both
    // be handed over as they arrive. Positions past the capacity are dropped
    // rather than the buffer growing without bound.
    void add_casters(const float *positions, std::size_t vertex_count, std::size_t stride,
                     const std::array<float, 16> &world);
    // How many caster vertices this frame collected.
    [[nodiscard]] std::size_t captured() const noexcept { return captured_.size(); }
    // For MGA_TRACE_SHADOWS: the extent the light's box was built around, and
    // the direction the light was taken to be in. Both in draw space.
    [[nodiscard]] const std::array<float, 3> &box_minimum() const noexcept { return previous_minimum_; }
    [[nodiscard]] const std::array<float, 3> &box_maximum() const noexcept { return previous_maximum_; }
    [[nodiscard]] const std::array<float, 3> &light_direction() const noexcept { return light_direction_; }
    [[nodiscard]] bool has_box() const noexcept { return previous_valid_; }

    // Chooses the light to cast from and works out the matrix taking draw space
    // to its clip space, sized to the PREVIOUS frame's caster extent. Call once
    // a frame, as early as the lighting state is known, because the shading
    // pass needs the matrix while it draws. Returns false when there is no
    // usable light or nothing was seen last frame, and shading should then skip
    // the map entirely.
    // `draw_to_clip` is the renderer's projection times view -- what takes a
    // pre-transformed vertex to clip space -- and `world_to_clip` is the
    // equivalent matrix the game keeps for itself. Both land in the same clip
    // space, so one composed with the inverse of the other is the transform
    // from world space into the space the display list is drawn in. That is
    // what lets a sun stay where it is put while the camera turns; without it
    // the only light available is one fixed in a space that turns with the
    // camera, and its shadows swing with it.
    //
    // Pass have_world_matrix false when there is no scene to read one from,
    // and the game's own light is used instead.
    [[nodiscard]] bool resolve_light(const LightingState &lighting, const std::array<float, 16> &draw_to_clip,
                                     const std::array<float, 16> &world_to_clip, bool have_world_matrix);
    // Whether the light in use is fixed in the world, or only in draw space.
    // False means shadows will swing as the camera turns, and the trace says so.
    [[nodiscard]] bool light_is_world_fixed() const noexcept { return world_fixed_; }

    // The matrix the shading pass must use, matching what record() draws with.
    // Column-major, as the GE's matrices are.
    [[nodiscard]] const std::array<float, 16> &transform() const noexcept { return capture_transform_; }
    // Which of the four lights the map is cast from, so the shading pass knows
    // whose contribution to take away. -1 when there is none.
    [[nodiscard]] int light_index() const noexcept { return capture_light_; }

    // Uploads this frame's casters and records the depth pass. Give it a
    // command buffer that will be submitted BEFORE the scene's. The image is
    // left ready to sample. Returns false when there was nothing to draw, in
    // which case nothing was recorded.
    bool record(VkCommandBuffer commands);

private:
    [[nodiscard]] bool create_target(std::string &error);
    [[nodiscard]] bool create_pipeline(const std::uint32_t *spirv, std::size_t bytes, std::string &error);
    [[nodiscard]] bool create_buffer(std::string &error);
    [[nodiscard]] std::uint32_t memory_type(std::uint32_t mask, VkMemoryPropertyFlags properties) const;

    DeviceContext context_{};
    std::uint32_t resolution_{};

    VkImage depth_{};
    VkDeviceMemory depth_memory_{};
    VkImageView depth_view_{};
    VkFramebuffer framebuffer_{};
    VkRenderPass render_pass_{};
    VkShaderModule vertex_shader_{};
    VkPipelineLayout layout_{};
    VkPipeline pipeline_{};

    VkBuffer buffer_{};
    VkDeviceMemory buffer_memory_{};
    void *mapped_{};

    // Positions are collected on the host, so they survive the renderer's own
    // per-frame vertex buffer being reset under them.
    std::vector<std::array<float, 4>> captured_;
    std::array<float, 16> capture_transform_{};
    bool capture_has_light_{};
    int capture_light_{-1};
    // The casters' extent in draw space. The current frame's is being filled
    // while the previous frame's is what sizes the light's box.
    std::array<float, 3> minimum_{};
    std::array<float, 3> maximum_{};
    std::array<float, 3> previous_minimum_{};
    std::array<float, 3> previous_maximum_{};
    bool previous_valid_{};
    std::array<float, 3> light_direction_{};
    bool world_fixed_{};
};

} // namespace mga::gpu
