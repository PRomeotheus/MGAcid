// The running game's fast-loading state: the detector from load_detector.cpp,
// the things that feed it, and the answer the kernel asks for.
//
// Everything here is called on the emulation thread.
#include "kernel/fast_loading.hpp"

#include "kernel/kernel.hpp"
#include "settings/settings.hpp"
#include "ui/ui.hpp"

#include <cstdlib>
#include <iostream>

namespace mga::fast_loading {
namespace {

Detector &detector() {
    static Detector instance;
    return instance;
}

bool &buttons_held() {
    static bool held = false;
    return held;
}

bool &was_fast() {
    static bool fast = false;
    return fast;
}

std::uint64_t &started_us() {
    static std::uint64_t started = 0u;
    return started;
}

bool enabled() {
    // MGA_FAST_LOADING=0 overrides the setting either way.
    static const char *override_text = std::getenv("MGA_FAST_LOADING");
    if (override_text != nullptr) return override_text[0] != '0';
    return settings::current().fast_loading;
}

} // namespace

void note_disc_read() {
    if (!enabled()) return;
    detector().disc_read(kernel().now_us());
}

bool note_audio(int peak) {
    return detector().audio(kernel().now_us(), peak);
}

void note_buttons(bool held) { buttons_held() = held; }

void update() {
    Guards guards{};
    // Game speed Unlimited already lets time run free, and the menu being open
    // means the player is doing something rather than waiting for a load.
    guards.enabled = enabled() && !settings::current().unthrottled;
    guards.buttons_held = buttons_held();
    guards.menu = ui::overlay_drawn();
    // Movie and ad hoc guards are not wired: this port has no movie path that
    // reaches here, and ad hoc play is reported below rather than guarded,
    // because letting one player's clock run ahead of the others would be
    // worse than a slow load. Left as fields so the reason names still mean
    // what they say if either is ever connected.
    const std::uint64_t now = kernel().now_us();
    const bool fast = detector().update(now, guards);
    if (fast == was_fast()) return;
    was_fast() = fast;
    if (fast) {
        started_us() = now;
    } else if (started_us() != 0u) {
        std::cout << "[load] ran ahead for " << (now - started_us()) / 1000u << " ms of game time, ended by "
                  << reason_name(detector().reason()) << "\n";
    }
}

bool active() { return was_fast(); }

} // namespace mga::fast_loading
