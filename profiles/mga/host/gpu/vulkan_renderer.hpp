#pragma once

#include "ge_state.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "settings/settings.hpp"

union SDL_Event;
struct SDL_Window;
struct SDL_Gamepad;
struct ImDrawData;

namespace mga::gpu {

// PSP pad state gathered from the keyboard and the gamepad.
struct PadState {
    std::uint32_t buttons{};
    std::uint8_t analog_x{0x80u};
    std::uint8_t analog_y{0x80u};
    // The HD release reads a second stick from the two SceCtrlData bytes after
    // Ly, which the PSP itself left reserved. 0x80 is its centre: the guest
    // skips its camera path entirely only when both bytes are exactly centred.
    std::uint8_t right_x{0x80u};
    std::uint8_t right_y{0x80u};
};

struct RendererConfig {
    std::string title{"MGAcid"};
};

// Vulkan backend for the GE. Draw calls are rendered into an offscreen target
// the size of the PSP framebuffer times the internal scale, which is blitted to
// the window once per guest frame.
class VulkanRenderer {
public:
    VulkanRenderer();
    ~VulkanRenderer();
    VulkanRenderer(const VulkanRenderer &) = delete;
    VulkanRenderer &operator=(const VulkanRenderer &) = delete;

    // Returns false and fills `error` when the window or device cannot be created.
    bool initialize(const RendererConfig &config, std::string &error);
    void shutdown();
    [[nodiscard]] bool available() const noexcept;

    // Pumps window events; returns false once the window has been closed.
    bool pump_events();
    [[nodiscard]] PadState pad() const noexcept;

    [[nodiscard]] bool quit_requested() const noexcept;

    void begin_frame();
    // Call before walking each display list. Guest memory cannot change while a
    // list is walked, so texture contents are hashed once per list, not per draw.
    void begin_display_list();
    void submit(const DrawCall &call, const GuestMemory &memory);
    // Writes the framebuffer shown a frame or two ago back to guest VRAM, in
    // the guest's pixel format at 480x272, so game code that copies a frame
    // out of VRAM with the CPU or DMA finds the picture instead of stale
    // bytes. Call once per frame before present(). MGA_NO_FB_TEXTURES turns
    // it off along with sampling render targets as textures.
    void write_back_frame(GuestMemory &memory);
    // Before a GE block transfer reads guest memory: when `source` lies in a
    // framebuffer the renderer drew, finishes the work queued so far and
    // writes that framebuffer back to guest memory, so the copy gets the
    // picture. Waits for the GPU. MGA_NO_FB_TEXTURES turns it off.
    void read_back_framebuffer(std::uint32_t source, GuestMemory &memory);
    // Ends the frame and shows the target the guest just flipped to. Draws go to
    // a separate offscreen target per guest framebuffer address, so only the
    // displayed one reaches the window.
    void present(std::uint32_t display_address);
    // Shows a frame the game wrote to memory itself instead of drawing it
    // with the GE, as the movie player does: the next present of
    // `display_address` shows these `width` x `height` pixels (R, G, B, A in
    // memory order, rows `stride` pixels apart), scaled to the target. Call
    // it at most once per presented frame.
    void upload_frame(std::uint32_t display_address, const std::uint8_t *pixels, std::uint32_t width,
                      std::uint32_t height, std::uint32_t stride);

    // Writes the last rendered frame as a BMP; returns false if it could not be
    // read back. Used for screenshots without touching the window system.
    bool capture_frame(const std::string &path);
    // Writes the next presented window image, with the interface over it, as
    // a BMP once it has been drawn.
    void capture_window(const std::string &path);

    // Display settings, applied at once. The initial values come from
    // settings::current() in initialize().
    void set_internal_scale(std::uint32_t scale);
    void set_window_scale(std::uint32_t scale);
    void set_fullscreen(bool fullscreen);
    void set_present_mode(settings::PresentMode mode);
    [[nodiscard]] bool supports_present_mode(settings::PresentMode mode) const;
    void set_keep_aspect(bool keep_aspect);
    // Show the frame at a whole multiple of the PSP's 480x272 instead of the
    // largest fraction of the window that fits. Every PSP pixel then covers
    // the same square block of screen pixels, where an uneven scale gives
    // some of them one more row or column than their neighbours. Does
    // nothing while the frame is stretched to the window rather than
    // letterboxed.
    void set_pixel_perfect(bool pixel_perfect);
    // Work the game's lights out per fragment rather than per vertex. The
    // lights, materials and falloff are the GE's own; only the place they are
    // evaluated changes. The hardware had no choice, and it shows on models of
    // a few hundred triangles: shading goes flat across each triangle, and a
    // specular highlight -- a power of a dot product -- jumps from one vertex to
    // the next instead of travelling over a surface.
    void set_light_per_pixel(bool per_pixel);
    // Shade lit geometry in linear space and tonemap it. curve is how much of
    // the film curve to apply: zero is linear shading with a plain clip, and
    // 0.47 leaves a mid grey where the old path had it.
    void set_linear_light(bool enabled);
    // Read a texture's own light and dark as relief, and light it accordingly.
    // Works only alongside per-pixel lighting, which is where it is applied.
    void set_surface_relief(float strength);
    // Multiplies the field of view the game asks for. 1 is the game's own.
    void set_field_of_view(float factor);
    // A real viewing direction for the specular half vector, and a Schlick
    // Fresnel term that needs one.
    void set_accurate_specular(bool enabled, float fresnel);
    // How much of the ambient favours the side facing the dominant light.
    void set_ambient_shape(float strength);
    // Scales the light before the tonemap, so there is something above one for
    // the curve to work on, and dithers the result against banding.
    void set_light_intensity(float intensity, float dither);
    void set_tonemap(float curve);
    void set_sharp_screen(bool sharp);
    void set_sharp_textures(bool sharp);
    void set_smooth_textures(bool smooth);
    void set_texture_scale(std::uint32_t factor, bool sharp);
    // Use replacement textures from the pack in the data directory, when one is
    // there. Does nothing without a pack.
    void set_texture_pack(bool enabled);
    [[nodiscard]] bool texture_pack_available() const noexcept;
    void set_smart_2d(bool smart);
    // Shows the frame through a shader pass instead of a blit, and turns
    // anti-aliasing on within it. Anti-aliasing does nothing on its own.
    void set_post_processing(bool enabled, bool fxaa);
    // Darkens the creases where geometry meets, read out of the depth
    // buffer in the post pass. 0 turns it off. Needs post-processing.
    void set_contact_shadows(float strength);
    // Lifts contrast and saturation in the post pass, to put the art back in
    // the range it was authored for. 0 turns it off. Needs post-processing.
    void set_colour_grade(float strength);
    // Replaces the built-in contrast-and-saturation grade with a lookup table
    // read from an Adobe .cube, which is what every colour grading tool
    // exports. False with `error` set when the file is missing or not one, and
    // the grade already in use is kept. set_colour_grade still sets how far
    // the picture is taken towards it, so a table does nothing at strength 0.
    bool set_colour_lut(const std::filesystem::path &path, std::string &error);
    // Back to the built-in grade.
    void clear_colour_lut();
    // The table in use, or empty when it is the built-in grade.
    [[nodiscard]] std::filesystem::path colour_lut_path() const;
    // A glow around bright things: a lens scatters light from a bright source
    // across what is near it and a sensor blooms outright, and neither happens
    // on a PSP. Only light above a threshold spills, so ordinary surfaces do
    // not glow. 0 turns it off. Needs post-processing.
    void set_bloom(float strength);
    // Contrast-adaptive sharpening over the finished frame. The game's own
    // picture is 480x272 and is being magnified several times over, and a
    // magnified picture is a soft one however good the filter; this puts the
    // edges back without the white outlines a plain unsharp mask leaves.
    // 0 turns it off. Needs post-processing.
    void set_sharpen(float strength);
    // The background goes soft with distance, focused on whatever is under the
    // middle of the screen. Drawn at the seam between the world and the
    // interface, so the heads-up display stays sharp. Everything nearer than
    // the focal plane stays sharp too -- a real lens blurs the near field as
    // well, and here that would be the player's own shoulder. 0 turns it off.
    void set_depth_of_field(float strength);
    // Reflections on the floor, traced through the picture itself. Only on
    // surfaces that face up: there is nothing in a PSP display list that says
    // which materials are polished, so this asks the geometry instead, and a
    // floor is the case it can do well. It can only reflect what is already on
    // screen, which in a corridor seen from above is not much. 0 turns it off.
    void set_reflections(float strength);

    // Vertices and uniform blocks come out of one per-frame arena, and a draw
    // that does not fit is dropped. These report how close a frame came to the
    // end of it and how many draws have been lost, so "something flickers" can
    // be answered with a number rather than a theory.
    [[nodiscard]] std::uint64_t arena_peak_bytes() const noexcept;
    [[nodiscard]] std::uint64_t arena_bytes() const noexcept;
    [[nodiscard]] std::uint64_t dropped_draws() const noexcept;
    // What the two caches that hold video memory are holding, against what
    // they are allowed. A budget nothing reports is a budget nobody can tell
    // is working.
    [[nodiscard]] std::string video_memory_report() const;
    // Shadows cast from the game's own lights, through a depth map rendered
    // from where the brightest light stands. 0 turns them off.
    void set_shadow_maps(float strength);
    // The matrix that takes world space to clip space -- the game's own, so
    // the renderer needs no assumption about what space the display list is
    // in. The shadow map needs it to recover world space and keep its sun
    // pointing the same way whichever way the camera turns. Call once a frame;
    // the renderer keeps it until replaced.
    // `valid` is false when the kernel is not looking at a scene at all --
    // a menu, a map screen, a load. Two things are switched off for those.
    // Shadows, because the light's box still holds the last real frame's
    // casters and menu geometry landing inside it gets shadowed by a scene
    // that is not on the screen any more. And the field-of-view control,
    // because a menu backdrop is a finite piece of geometry and widening the
    // view slides its edge into frame with nothing behind it.
    void set_world_transform(const std::array<float, 16> &world_to_clip, bool valid);
    void set_perf_overlay(bool visible);

    [[nodiscard]] SDL_Window *window() const noexcept;
    [[nodiscard]] std::string device_name() const;
    // The pad the game reads, or null.
    [[nodiscard]] SDL_Gamepad *gamepad() const noexcept;

    // The port's own interface (host/ui). Every window event is offered to
    // the hook first; returning true keeps it from the game.
    void set_event_hook(std::function<bool(const SDL_Event &)> hook);
    // While off, the game reads a neutral pad. Turning it back on ignores the
    // buttons still held until they are released, so the button that closed
    // a menu does not reach the game.
    void set_game_input(bool enabled);
    void request_quit() noexcept;
    // While held, the window keeps showing the frame on screen when hold
    // began instead of the frames the game flips to. The game blanks its
    // screen while the PSP's own keyboard would cover it; the port's keyboard
    // is drawn over the held frame instead.
    void hold_frame(bool hold);

    // Sets up Dear ImGui's Vulkan backend on this window; the caller has
    // created the ImGui context and its SDL3 backend.
    bool initialize_ui(std::string &error);
    void shutdown_ui();
    // ImGui_ImplVulkan_NewFrame, before ImGui::NewFrame.
    void begin_ui_frame();
    // Draw data from ImGui::Render, drawn over the next presented image.
    void set_ui_draw_data(ImDrawData *draw_data);
    // Presents a frame outside the game's own flips: the last game frame when
    // there is one and `show_game` is set, a plain background otherwise, with
    // the interface over it. Used while the game is paused or not started.
    void present_ui(bool show_game);

    [[nodiscard]] std::uint64_t frames_presented() const noexcept;
    [[nodiscard]] std::uint64_t draws_submitted() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mga::gpu
