// The one symbol the kernel needs from a subsystem these tests do not exercise.
//
// kernel.cpp reaches into the module loader exactly once, to find the $gp a
// guest interrupt handler should run with. Linking the real module loader to get
// it would pull in the ELF loader, the module corpora generated at build time
// and the game-specific patches behind them -- a large amount of code, none of
// it anything a save state test looks at.
//
// So the test provides it. Not as a convenience: an interrupt handler is one of
// the things a save is refused while it is live, so these tests never reach the
// call site at all, and stubbing it states that rather than hiding it.

#include "kernel/module_loader.hpp"

namespace mga {

std::optional<std::uint32_t> module_gp_for_address(std::uint32_t) {
    return std::nullopt;
}

} // namespace mga
