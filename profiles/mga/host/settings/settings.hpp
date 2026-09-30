#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The player's settings: what the in-game menu changes and settings.ini in the
// per-user data directory keeps.
//
// A value comes from its environment variable when that is set, otherwise from
// settings.ini, otherwise from the default below. A variable decides the value
// for the whole run: the menu shows it but cannot change it, and it is never
// written to settings.ini, so unsetting the variable brings the player's own
// choice back.
//
// Only the main thread reads or writes these.
namespace mga::settings {

enum class PresentMode { Fifo, Mailbox, Immediate };
enum class PerfDisplay { Off, Overlay, OverlayAndLog, Log };
enum class RightStick { Camera, DPad, Off };
// What answers the game when it asks for text such as the hunter's name.
enum class NameEntry { Keyboard, Fixed };

struct Settings {
    // Video
    std::uint32_t internal_scale{2u};  // render resolution, multiples of 480x272
    std::uint32_t window_scale{2u};    // windowed size, multiples of 480x272
    bool fullscreen{};
    PresentMode present_mode{PresentMode::Fifo};
    bool keep_aspect{true};            // letterbox rather than stretch to the window
    bool sharp_screen{};               // nearest instead of linear scaling to the window
    // Show the frame at a whole multiple of 480x272 rather than at whatever
    // fraction fills the window. Needs keep_aspect.
    bool pixel_perfect{};
    // Evaluate the game's lights per fragment rather than per vertex.
    bool light_per_pixel{true};
    // Shade lit geometry in linear space and tonemap it, instead of
    // multiplying encoded values together and clamping at one.
    bool linear_light{};
    float tonemap_curve{0.47f};        // 0 linear and clipped .. 4 heavily filmic; 0.47 holds mid grey
    float surface_relief{};            // 0 off .. 8; texture shading read as relief; needs per-pixel lighting
    bool fast_loading{};               // let emulated time run ahead while the game loads and is silent
    bool accurate_specular{};          // a real view direction for highlights, and a Fresnel rim with it
    float fresnel{1.0f};               // 0 .. 4; how strong that rim is
    float ambient_shape{};             // 0 flat .. 1; how much ambient favours the lit side
    float light_intensity{1.0f};       // 0.25 .. 8; scales light before the tonemap; needs linear lighting
    float dither{1.0f};                // 0 .. 4 eighth-bits of noise against banding; needs linear lighting
    bool sharp_textures{};             // nearest instead of linear texture sampling
    bool smooth_textures{};            // mipmaps and anisotropic filtering, which the PSP had no room for
    std::uint32_t texture_scale{1u};   // 1 is off; 2/3/4 upscale decoded textures before they are uploaded
    bool texture_scale_sharp{true};    // edge-preserving rather than plain bicubic, for art drawn texel by texel
    bool texture_pack{true};           // use replacement textures from <data>/textures when there are any
    bool smart_2d{true};               // sample pixel-mapped 2D sharp, whatever the 3D filter is
    bool post_process{};               // show the frame through a shader pass rather than a plain blit
    bool fxaa{};                       // anti-alias the finished frame; needs post_process
    float contact_shadows{};           // 0 off .. 1 strongest; darkens creases from depth, drawn with the scene
    float shadow_maps{};               // 0 off .. 1 strongest; shadows cast from the game's own lights
    float colour_grade{};              // 0 off .. 1 strongest; how far towards the grade; needs post_process
    // A .cube colour grading table in <data>/grades, by file name. Empty means
    // the built-in contrast and saturation lift. colour_grade is the strength
    // either way, so a table with colour_grade at 0 does nothing.
    std::string colour_lut;
    float bloom{};                     // 0 off .. 1 strongest; glow around bright things; needs post_process
    float sharpen{};                   // 0 off .. 1 strongest; contrast-adaptive sharpen; needs post_process
    float reflections{};               // 0 off .. 1 strongest; screen-space reflections, floors only
    // Show an extra image between the game's own frames, built by carrying the
    // last frame's motion forward. The game itself is untouched and still runs
    // at thirty: it takes one simulation step per frame and cannot be made to
    // take more without running at double speed.
    bool frame_smoothing{};
    bool unthrottled{};                // let emulated time run ahead of real time
    PerfDisplay perf{PerfDisplay::Off};

    // Text
    std::string font;                  // the game's text font: path, "#face" for a collection; empty: the default
    std::uint32_t font_weight{1u};     // columns the game's glyphs are thickened by, 0 to kMaxFontWeight

    // Audio
    std::uint32_t volume{100u};        // percent
    std::uint32_t effects_volume{100};  // 0 .. 100; the SAS voices: footsteps, gunfire, the interface
    std::uint32_t music_volume{100};    // 0 .. 100; the ATRAC streams: music and recorded speech
    bool mute{};

    // Controls
    bool state_hotkeys{true};          // F1-F4 save a state, with shift load it
    bool confirm_south{};              // confirm (circle) on the south face button
    float dead_zone{0.15f};
    float trigger{0.25f};
    RightStick right_stick{RightStick::Camera};
    float right_stick_zone{0.5f};
    bool invert_camera_x{};
    bool invert_camera_y{};
    NameEntry name_entry{NameEntry::Keyboard};  // on-screen keyboard, or the name below at once
    std::string name{"Hunter"};        // the fixed name

    // Network (ad hoc play through a PSP ad hoc server)
    bool adhoc{};                      // wireless switch on: the game may go on line
    std::string adhoc_server;          // host or host:port of the server; empty: none
    std::string adhoc_nickname;        // shown to other players; empty: the hunter name
    std::string adhoc_mac;             // this player's virtual MAC, made up on first use
    std::vector<std::string> adhoc_recent;  // sessions joined lately, the latest first
    std::uint32_t adhoc_host_port{27312};   // the built-in server's adhocctl port; the relay is on the next

    // Interface
    bool menu_pause{true};             // opening the menu pauses the game
    bool menu_pause_multiplayer{};     // ...also during ad hoc play, where a paused game stops answering its peers
    bool menu_hint_seen{};             // the "Esc / L3+R3 opens the menu" hint was shown
    std::string last_folder;           // where the setup's file browser was last used

    // Saves
    bool backup_timestamp{true};       // a backup made from the menu goes to a new folder named by its time
};

inline constexpr std::uint32_t kMaxInternalScale = 8u;
inline constexpr std::uint32_t kMaxWindowScale = 4u;
inline constexpr std::uint32_t kMaxFontWeight = 2u;
// Past 4x a scaled texture is mostly invented detail, and the memory it costs
// is better spent on internal resolution.
inline constexpr std::uint32_t kMaxTextureScale = 4u;

// Loads the settings on first use.
[[nodiscard]] Settings &current();
// The defaults, for "Restore defaults".
[[nodiscard]] const Settings &defaults();
// Writes current() to settings.ini, leaving values set by environment
// variables at what the file had. Failures are reported on the console.
void save();

// The environment variable that decides the setting stored under `key`
// (for example "video.internal_scale") for this run, or null.
[[nodiscard]] const char *overridden_by(const char *key);

} // namespace mga::settings
