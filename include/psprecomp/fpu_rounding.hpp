#pragma once

// The Allegrex FPU's rounding mode, FCR31's RM field, applies to arithmetic
// and not only to cvt.w.s -- and games depend on it. Metal Gear Ac!d sets RM
// to 1 (toward zero) in its C startup at 0x088044B0, then floors negative
// coordinates with trunc(x - 0.99999994f). That idiom is a correct floor only
// in that mode: rounding to nearest makes the subtraction land exactly on the
// next integer, so every exact negative integer comes out one lower. It put a
// character one cell short of a doorway for a long time before anyone noticed.
//
// Emulating the mode per instruction would cost more than it is worth, so the
// guest's mode is mapped onto the host FPU's whenever the guest changes it.
// Games set it once at startup, so in the steady state this costs nothing.
//
// Caveat worth knowing: the host rounding mode is per thread and applies to
// host floating point as well, so anything the guest thread calls into runs
// under the guest's mode too. For the emulated machine that is the faithful
// choice.

#include <cfenv>
#include <cstdint>
#include <cstdlib>

namespace psprecomp {

// Idempotent and cheap: the host call happens only when the mode really
// changes, so callers on warm paths may invoke it freely.
// PSPRECOMP_NO_FPU_ROUNDING=1 pins the host to round-to-nearest, for comparing
// against the old behaviour.
inline void apply_host_rounding(std::uint32_t fcr31_value) noexcept {
    static const bool disabled = std::getenv("PSPRECOMP_NO_FPU_ROUNDING") != nullptr;
    if (disabled) return;
    int mode = FE_TONEAREST;
    switch (fcr31_value & 3u) {
    case 1u: mode = FE_TOWARDZERO; break;
    case 2u: mode = FE_UPWARD; break;
    case 3u: mode = FE_DOWNWARD; break;
    default: break;
    }
    static thread_local int applied = FE_TONEAREST;
    if (mode == applied) return;
    applied = mode;
    (void)std::fesetround(mode);
}

} // namespace psprecomp
