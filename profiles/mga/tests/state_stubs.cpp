// The symbols the kernel needs from subsystems these tests do not exercise.
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
//
// Fast loading is here for the same reason. advance_clock() asks whether
// emulated time may run ahead of real time, and the real answer comes from
// fast_loading.cpp, which includes ui/ui.hpp and would drag the whole interface
// into a test that serialises structures. False is not a placeholder: it is
// what the detector returns whenever the game is not loading, so the clock
// these tests see is the ordinary one.

#include "kernel/fast_loading.hpp"
#include "kernel/module_loader.hpp"

namespace mga {

std::optional<std::uint32_t> module_gp_for_address(std::uint32_t) {
    return std::nullopt;
}

namespace fast_loading {

bool active() {
    return false;
}

} // namespace fast_loading

} // namespace mga
