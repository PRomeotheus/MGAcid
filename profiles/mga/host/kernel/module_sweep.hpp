#pragma once

// MGA_SWEEP_MODULES=<dir>: collect a memory dump of every stage module on the
// disc without playing the game to each of them.
//
// Why a dump is needed at all
// ---------------------------
// A stage PRX on the disc is not finished code. It refers to the executable
// through offsets with no relocation entries, and to a set of symbols the game
// resolves by hash once the module is in memory. Only afterwards does its code
// hold real addresses, so a recompiled corpus can only be generated from the
// module as the game left it -- which is why scripts/generate_modules.sh wants
// a dump per stage, and why only the four stages anyone has played through
// have corpora. The other forty run interpreted.
//
// Why this rather than reimplementing the link
// --------------------------------------------
// Reproducing the link statically gets most of the way: with the ELF
// relocations applied, a linked stage differs from the game's own version in
// under three per cent of its words, and every one of those is a data address
// rather than a branch. Closing that last gap means finding and reading the
// game's hash table, which is a reversing job and a source of quiet errors.
//
// The game already contains a correct implementation of its own linker. This
// runs it: each stage is loaded through the same module loader the game uses
// and started the same way, and what lands in memory is by construction what
// the game itself would have produced. The four stages that already have dumps
// are the proof -- a sweep that does not reproduce them byte for byte is
// wrong, and says so.
//
// When it runs
// ------------
// A stage links against the executable's state, so the sweep cannot run before
// the game has got far enough to load a stage of its own. It therefore waits
// for the game to start its first stage module and takes over from there. The
// run is spent afterwards: each stage is loaded at the address the last one
// occupied, so the game's own stage is gone by the second iteration and the
// process leaves once the sweep is done.

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/runtime.hpp"

#include <string>

namespace mga {

struct LoadedModule;

// Whether MGA_SWEEP_MODULES is set, and where the dumps go.
[[nodiscard]] bool sweeping_modules();

// Offered every sceKernelStartModule. Returns true when the sweep has taken
// the call over, in which case the caller must not start the module itself:
// the sweep starts it, dumps it, and goes on to the next stage.
bool sweep_take_over(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx, LoadedModule &module);

} // namespace mga
