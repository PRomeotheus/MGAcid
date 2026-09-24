#pragma once

// Reads the battle scene out of the game's own records: where every character
// stands, and where the camera is.
//
// The renderer cannot answer either question. Metal Gear Ac!d skins its
// characters on the CPU and submits them pre-transformed with an identity world
// matrix, all batched at the origin, so the display list carries no per-
// character position; and the view matrix it sets carries rotation only, with
// the camera's position baked into every world matrix, so the camera is not in
// the list either. Both live in guest memory.
//
// The addresses come from reading mgp_main, not from guessing:
//
//   0x089A91B8            pointer to the scene root; null outside a battle
//   root + 0x44           head of the character list   (FUN_08811E90)
//   root + 0x1B0          the default camera           (FUN_08813818)
//   root + 0x1E0          the secondary camera, used when its in-use flag is set
//   root + 0x1C8          the override camera, which wins over both
//
//   character + 0x40      next in the list
//   character + 0x48      handle
//   character + 0x828     flags, low word
//   character + 0x82C     flags, high word
//   character + 0xA10     world position, as four floats
//   character + 0xA38     cell, one signed byte per axis
//
//   camera + 0x08         flags; 0x400 means this camera is the one in use
//   camera + 0x10         eye position in world space, as four floats
//
//   0x089E1870            the world-to-clip matrix the game keeps for itself
//
// That last one matters more than the camera does. FUN_0880FBBC -- the game's
// own "is this point on screen" test -- multiplies a WORLD position by it and
// divides through, so it maps world space straight to clip space. Anything the
// port wants to draw in the scene can use it and never has to work out what
// space the display list is in, or where the camera is.
//
// The list walk is the same one FUN_088B686C makes, so it sees exactly what the
// game sees -- the player included, which the old registration hook at
// 0x088BA684 did not: that one fires on the enemy-queueing path and misses the
// character the player controls.

#include "psprecomp/runtime.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace mga::scene {

using psprecomp::Runtime;

struct Character {
    std::uint32_t address{};
    std::uint32_t handle{};
    // World position. The cell is the game's coarse grid: a cell is 2000 units
    // across and a character's x and z sit at its centre, so
    // position = cell * 2000 + 1000 holds on those two axes.
    std::array<float, 3> position{};
    std::array<int, 3> cell{};
    std::uint32_t flags_low{};
    std::uint32_t flags_high{};
};

struct View {
    // False when no battle scene is loaded, which is most of the time: menus,
    // the map screen, movies.
    bool valid{};
    // The eye, in world space. Useful to know, but not what the renderer
    // draws with: see world_to_clip.
    std::array<float, 3> camera{};
    // The game's own world-to-clip matrix, column-major, as the GE's matrices
    // are. Drawing with this needs no assumption about the space the display
    // list uses.
    std::array<float, 16> world_to_clip{};
    std::vector<Character> characters{};
};

// Reads the scene as it stands right now. Cheap enough to call once a frame:
// a pointer chase and a short list walk, with no allocation after the first
// few frames. Returns a view with `valid` false when there is no scene.
[[nodiscard]] const View &read(Runtime &runtime);

// MGA_TRACE_SCENE=1 prints one line per character on a timer, with the camera.
// A diagnostic, not a feature.
void trace(Runtime &runtime);

} // namespace mga::scene
