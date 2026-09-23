#pragma once

// Identity of the one release this profile supports: Metal Gear Ac!d, USA.
namespace mga::install {

// DISC_ID in PSP_GAME/PARAM.SFO.
inline constexpr const char *kDiscId = "ULUS10006";
inline constexpr const char *kDiscIdDisplay = "ULUS-10006";
inline constexpr const char *kGameTitle = "Metal Gear Ac!d";

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

} // namespace mga::install
