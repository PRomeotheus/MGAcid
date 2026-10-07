// Calls only this game needs, registered after the shared host's so that one
// here also replaces a shared one for this game alone.
//
// Nothing yet: every call this game makes is in the shared host. Put a call
// here only when it really is this game's (an address in its code, a quirk no
// other game has); a PSP library call that is merely new belongs in the
// framework, where every game gets it.
#include "hle/hle_common.hpp"

namespace psphost {

void register_game_hle(HleRegistrar &) {}

} // namespace psphost
