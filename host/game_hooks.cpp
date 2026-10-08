// Metal Gear Ac!d's answers to the shared host's game_hooks.hpp.
#include "game_hooks.hpp"

#include "kernel/module_sweep.hpp"
#include "kernel/sound_paths.hpp"
#include "settings/settings.hpp"

namespace psphost::game {

// Ac!d paces itself in the loop at 0x0887B308, which reads as:
//
//     sceDisplayWaitVblankStart();
//     elapsed = *kVBlanksThisFrame;                     // 0x089B7508
//     while (elapsed < *kVBlanksPerFrame)               // 0x089E111C
//         sceDisplayWaitVblankStart();
//     *kVBlanksPerFrame = clamp(elapsed, 2, 6);
//     *kVBlanksThisFrame = 0;
//
// So the cadence for the next frame is whatever the last one cost, in whole
// vblanks, and that clamp is the frame rate: a floor of two vblanks is 30 fps
// and can never be anything else, and a ceiling of six means a frame that
// overruns falls to 10 rather than tearing. Nothing else in the game reads
// either variable -- they belong to this loop alone -- so writing the floor
// down to one vblank changes the game's pacing and nothing else about it.
//
// What that does NOT settle is whether the simulation advances by a fixed step
// per frame or by elapsed time. If it is fixed, this runs the game at double
// speed rather than at 60 fps, and the honest fix is a much larger one.
//
// MGA_60FPS=1 (or the menu's native 60 fps setting, which reads it) asks for it.
constexpr std::uint32_t kVBlanksPerFrame = 0x089E111Cu;

void raise_frame_rate_cap(psprecomp::Runtime &runtime) {
    if (!settings::current().native_60fps) return;
    // Written on the way into every vblank wait, which is always just before
    // the loop reads it, so the value the game wrote a moment ago never gets
    // the chance to take effect.
    runtime.memory().store32(kVBlanksPerFrame, 1u);
}

// The sound module's file names can be kept alive from here on.
void on_modules_loaded(psprecomp::Runtime &runtime) { install_sound_path_keeper(runtime); }

// MGA_SWEEP_MODULES: the first stage the game starts is the signal that a
// stage has something to link against, and from there the sweep drives the
// loader itself. It never gives the call back.
bool module_start_take_over(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx, LoadedModule &module) {
    return sweep_take_over(runtime, ctx, module);
}

// No options of its own.
const std::vector<Option> &options() {
    static const std::vector<Option> none;
    return none;
}

} // namespace psphost::game
