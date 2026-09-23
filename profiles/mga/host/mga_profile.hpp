#pragma once

#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <iterator>

namespace mga {

inline constexpr std::uint32_t kLoadBase = psprecomp::kDefaultPspUserLoadBase;
inline constexpr std::uint32_t kGuestRamBytes = 32u * 1024u * 1024u;

struct ProfilePaths {
    std::filesystem::path disc_image;    // UMD ISO; empty disables disc0:
    std::filesystem::path memory_stick;  // host directory backing ms0:
};

// Installs the kernel and HLE modules, binds logging stubs for the remaining
// imports (unless MGA_STRICT_HLE is set) and prepares the loader thread
// that runs module_start.
void install_profile(psprecomp::Runtime &runtime, const psprecomp::Elf32Image &elf, const ProfilePaths &paths);

// The host directory that backs ms0:, where the game's saves live under
// PSP/SAVEDATA. This is the only place that decides it; the rest of the host
// receives the result, so moving saves to a per-user location changes only
// this function.
[[nodiscard]] std::filesystem::path memory_stick_directory(const std::filesystem::path &game_dir);

// Fixed overlay slots (Yakumo's MHP3rd mechanism). Metal Gear Ac!d instead
// loads relocatable stage modules through ModuleMgr, so there are none yet;
// the single entry is only an end marker.
inline constexpr std::uint32_t kOverlaySlots[] = {0u};

} // namespace mga
