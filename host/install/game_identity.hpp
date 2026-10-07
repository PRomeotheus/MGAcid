#pragma once

#include <array>
#include <cstdint>
#include "psphost_game.hpp"

// Identity of the one release this profile supports: the game, USA.
namespace psphost::install {

// DISC_ID in PSP_GAME/PARAM.SFO.
inline constexpr const char *kDiscId = "ULUS10006";
inline constexpr const char *kDiscIdDisplay = "ULUS-10006";
inline constexpr const char *kGameTitle = PSPHOST_GAME_TITLE;

// The game's executable. PSP_GAME/SYSDIR/BOOT.BIN on this disc is the plain,
// unencrypted ELF of the same program EBOOT.BIN carries encrypted (and the same
// file as PSP_GAME/USRDIR/program.prx), so no decryption is needed.
inline constexpr const char *kExecutablePathOnDisc = "PSP_GAME/SYSDIR/BOOT.BIN";
inline constexpr const char *kParamSfoPathOnDisc = "PSP_GAME/PARAM.SFO";

// SHA-256 of kExecutablePathOnDisc as it is on the disc.
inline constexpr const char *kEncryptedExecutableSha256 =
    "f0886cc094dcab4001383fdae5c347cf0365be1deddb88008c1ee66ea6299c29";
// SHA-256 of the executable the recompiled code was generated from.
inline constexpr const char *kExecutableSha256 =
    "f0886cc094dcab4001383fdae5c347cf0365be1deddb88008c1ee66ea6299c29";

// --- What the shared host asks of this game ------------------------------

// The tag of the disc's encrypted EBOOT.BIN, at 0xD0 of its header, and the
// key it selects from the public PSP key tables.
inline constexpr std::uint32_t kEbootTag = 0xD9160BF0u;
inline constexpr std::array<std::uint8_t, 16> kEbootTagKey = {0x83, 0x83, 0xF1, 0x37, 0x53, 0xD0, 0xBE, 0xFC,
                                       0x8D, 0xA7, 0x32, 0x52, 0x46, 0x0A, 0xC2, 0xC2};

// Save states: which game's host layer wrote one ("MGAc"). A state
// from another game is refused.
inline constexpr std::uint32_t kStateProfileTag = 0x4341474Du;

// The console settings the game is told about through sceUtility: language
// (0 Japanese, 1 English) and the confirm button (0 circle, 1 cross), and
// whether the port's own menu confirms with the bottom face button by default.
inline constexpr std::uint32_t kSystemLanguage = 0u;
inline constexpr std::uint32_t kConfirmButton = 0u;
inline constexpr bool kDefaultConfirmSouth = false;

// The font cache reset (hle/hle_font.cpp): return address of the game's only call that asks for glyph images.
inline constexpr std::uint32_t kGlyphImageCaller = 0x088EA3A4u;

// Fixed overlay slots, ending with an end marker. Ac!d loads relocatable stage modules through ModuleMgr instead; the single entry is only an end marker.
inline constexpr std::uint32_t kOverlaySlots[] = {
    0u
};

} // namespace psphost::install
