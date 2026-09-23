#pragma once

// Keeps the file names a sound command queues alive until the sound module
// reads them. See sound_paths.cpp.

#include "kernel/kernel.hpp"

namespace mga {

// Called after every module load, once KCEJ_SOUND's entry point is registered.
void install_sound_path_keeper(Runtime &runtime);

} // namespace mga
