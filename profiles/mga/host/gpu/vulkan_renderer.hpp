#pragma once

#include "ge_state.hpp"

#include <cstdint>
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

// Something that should cast a shadow on the ground: a character, in world
// space. The renderer cannot work these out for itself -- Metal Gear Ac!d
// skins its characters on the CPU and submits them pre-transformed with an
// identity world matrix, all batched at the origin -- so the kernel reads them
// out of the game's own records and hands them over once a frame.
struct ShadowCaster {
    std::array<float, 3> position{};  // world space
    float ground{};                   // world height of the floor beneath it
    float radius{};                   // how wide the shadow should be
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
    void set_sharp_screen(bool sharp);
    void set_sharp_textures(bool sharp);
    void set_smooth_textures(bool smooth);
    void set_texture_scale(std::uint32_t factor, bool sharp);
    void set_smart_2d(bool smart);
    // Shows the frame through a shader pass instead of a blit, and turns
    // anti-aliasing on within it. Anti-aliasing does nothing on its own.
    void set_post_processing(bool enabled, bool fxaa);
    // Darkens the creases where geometry meets, read out of the depth
    // buffer in the post pass. 0 turns it off. Needs post-processing.
    void set_contact_shadows(float strength);
    // Blob shadows under the characters. 0 turns them off. Unlike the effects
    // above this does not need the post-processing pass: the blobs are drawn
    // with the scene, so they sit under the geometry properly.
    void set_blob_shadows(float strength);
    // Shadows cast from the game's own lights, through a depth map rendered
    // from where the brightest light stands. 0 turns them off.
    void set_shadow_maps(float strength);
    // The characters to put a blob under, in world space, and the matrix that
    // takes world space to clip space -- the game's own, so the renderer needs
    // no assumption about what space the display list is in. Call once a
    // frame; the renderer keeps them until replaced.
    void set_shadow_casters(const std::array<float, 16> &world_to_clip, const std::vector<ShadowCaster> &casters);
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
    // Draws a blob under each caster. Called from submit() at the moment the
    // frame turns from 3D to the interface.
    void draw_shadow_blobs(const GuestMemory &memory);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mga::gpu
