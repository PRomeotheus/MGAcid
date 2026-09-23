#pragma once

// MGA_WATCH=<hex address>[:<bytes>][,...] -- logs the arguments of the guest
// functions it names, then runs them. See call_watch.cpp.

#include "kernel/kernel.hpp"

namespace mga {

// Called once the recompiled corpora are registered.
void install_call_watches(Runtime &runtime);

} // namespace mga
