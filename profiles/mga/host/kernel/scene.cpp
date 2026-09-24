#include "kernel/scene.hpp"

#include "kernel/kernel.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>

namespace mga::scene {
namespace {

// See the header for where each of these came from.
constexpr std::uint32_t kSceneRootPointer = 0x089A91B8u;
constexpr std::uint32_t kListHead = 0x44u;
constexpr std::uint32_t kCameraOverride = 0x1C8u;
constexpr std::uint32_t kCameraSecondary = 0x1E0u;
constexpr std::uint32_t kCameraDefault = 0x1B0u;

constexpr std::uint32_t kNext = 0x40u;
constexpr std::uint32_t kHandle = 0x48u;
constexpr std::uint32_t kFlagsLow = 0x828u;
constexpr std::uint32_t kFlagsHigh = 0x82Cu;
constexpr std::uint32_t kPosition = 0xA10u;
constexpr std::uint32_t kCell = 0xA38u;

// The world-to-clip matrix the game maintains for its own on-screen tests.
constexpr std::uint32_t kWorldToClip = 0x089E1870u;

constexpr std::uint32_t kCameraFlags = 0x08u;
constexpr std::uint32_t kCameraInUse = 0x400u;
constexpr std::uint32_t kCameraEye = 0x10u;

// A character record runs to roughly 0x8000 bytes, so anything the walk
// touches has to be readable that far out.
constexpr std::size_t kCharacterSpan = 0xA40u;
// The list is short -- a handful of characters. A cap stops a corrupt or
// half-written link from spinning forever.
constexpr std::size_t kMaxCharacters = 64u;
constexpr std::uint64_t kTraceIntervalUs = 250000u;

[[nodiscard]] float read_float(psprecomp::GuestMemory &memory, std::uint32_t address) {
    return std::bit_cast<float>(memory.load32(address));
}

[[nodiscard]] bool readable(psprecomp::GuestMemory &memory, std::uint32_t address, std::size_t span) {
    // Guest pointers are word-aligned; a misaligned one is garbage, not a
    // record, and chasing it would only read noise.
    return address != 0u && (address & 3u) == 0u && memory.contains(address, span);
}

// The game's own camera choice, from FUN_08813818: the override camera wins
// when it is flagged in use, then the secondary, then the default.
[[nodiscard]] std::uint32_t active_camera(psprecomp::GuestMemory &memory, std::uint32_t root) {
    const auto in_use = [&](std::uint32_t slot) {
        const std::uint32_t camera = memory.load32(root + slot);
        if (!readable(memory, camera, kCameraEye + 16u)) return 0u;
        return (memory.load32(camera + kCameraFlags) & kCameraInUse) != 0u ? camera : 0u;
    };
    if (const std::uint32_t camera = in_use(kCameraOverride); camera != 0u) return camera;
    if (const std::uint32_t camera = in_use(kCameraSecondary); camera != 0u) return camera;
    const std::uint32_t fallback = memory.load32(root + kCameraDefault);
    return readable(memory, fallback, kCameraEye + 16u) ? fallback : 0u;
}

// A record is only believed when its position agrees with its cell. The grid
// is 2000 units and a character's x and z sit at the centre of their cell, so
// a record being read at the wrong offset, or mid-write, fails this at once.
[[nodiscard]] bool consistent(const Character &character) {
    for (const int axis : {0, 2}) {
        const float expected = static_cast<float>(character.cell[static_cast<std::size_t>(axis)]) * 2000.0f + 1000.0f;
        const float actual = character.position[static_cast<std::size_t>(axis)];
        if (!std::isfinite(actual) || std::abs(actual - expected) > 2000.0f) return false;
    }
    return std::isfinite(character.position[1]);
}

View &storage() {
    static View view;
    return view;
}

} // namespace

const View &read(Runtime &runtime) {
    View &view = storage();
    view.valid = false;
    view.characters.clear();

    auto &memory = runtime.memory();
    if (!memory.contains(kSceneRootPointer, 4u)) return view;
    const std::uint32_t root = memory.load32(kSceneRootPointer);
    // Outside a battle there is no scene at all, which is the common case.
    if (!readable(memory, root, 0x200u)) return view;

    const std::uint32_t camera = active_camera(memory, root);
    if (camera == 0u) return view;
    for (std::size_t axis = 0; axis < 3u; ++axis)
        view.camera[axis] = read_float(memory, camera + kCameraEye + static_cast<std::uint32_t>(axis) * 4u);
    for (const float value : view.camera)
        if (!std::isfinite(value)) return view;

    // The matrix the blobs are actually drawn with. Without it there is
    // nothing to draw into, so a scene without one is not usable.
    if (!memory.contains(kWorldToClip, 64u)) return view;
    for (std::size_t i = 0; i < 16u; ++i)
        view.world_to_clip[i] = read_float(memory, kWorldToClip + static_cast<std::uint32_t>(i) * 4u);
    bool finite = true;
    for (const float value : view.world_to_clip) finite = finite && std::isfinite(value);
    // All zeroes is what it holds before the first frame of a scene is set up.
    const bool empty = std::all_of(view.world_to_clip.begin(), view.world_to_clip.end(),
                                   [](float value) { return value == 0.0f; });
    if (!finite || empty) return view;

    std::uint32_t node = memory.load32(root + kListHead);
    for (std::size_t seen = 0; seen < kMaxCharacters; ++seen) {
        if (!readable(memory, node, kCharacterSpan)) break;
        Character character{};
        character.address = node;
        character.handle = memory.load32(node + kHandle);
        character.flags_low = memory.load32(node + kFlagsLow);
        character.flags_high = memory.load32(node + kFlagsHigh);
        for (std::size_t axis = 0; axis < 3u; ++axis) {
            character.position[axis] = read_float(memory, node + kPosition + static_cast<std::uint32_t>(axis) * 4u);
            character.cell[axis] =
                static_cast<std::int8_t>(memory.load8(node + kCell + static_cast<std::uint32_t>(axis)));
        }
        if (consistent(character)) view.characters.push_back(character);
        node = memory.load32(node + kNext);
        if (node == 0u) break;
    }

    view.valid = true;
    return view;
}

void trace(Runtime &runtime) {
    static const bool wanted = std::getenv("MGA_TRACE_SCENE") != nullptr;
    if (!wanted) return;
    static std::uint64_t next_sample = 0u;
    const std::uint64_t now = kernel().now_us();
    if (now < next_sample) return;
    next_sample = now + kTraceIntervalUs;

    const View &view = read(runtime);
    if (!view.valid) {
        std::cout << "[scene] no scene\n";
        return;
    }
    std::cout << std::fixed << std::setprecision(1) << "[scene] camera (" << view.camera[0] << ", " << view.camera[1]
              << ", " << view.camera[2] << ")  characters=" << view.characters.size() << "\n";
    for (const Character &character : view.characters) {
        std::cout << "[scene]   " << psprecomp::hex32(character.address) << " handle "
                  << psprecomp::hex32(character.handle) << "  cell (" << character.cell[0] << ", " << character.cell[1]
                  << ", " << character.cell[2] << ")  world (" << std::fixed << std::setprecision(1)
                  << character.position[0] << ", " << character.position[1] << ", " << character.position[2]
                  << ")  flags " << psprecomp::hex32(character.flags_low) << "/"
                  << psprecomp::hex32(character.flags_high) << "\n";
    }
}

} // namespace mga::scene
