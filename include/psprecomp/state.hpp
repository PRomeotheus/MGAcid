#pragma once

// The part of a save state that every project has in common: all of the guest's
// memory, and a CPU context.
//
// A profile's own state -- its kernel objects, open files, whatever its host
// layer is holding -- is appended by the profile, because only it knows what
// those are. This header owns the header and the machine, and nothing else.
//
// Where a save state can be taken from
// ------------------------------------
// The runtime dispatches one guest function at a time from an outer loop and
// those functions return to it, so between two iterations of that loop the host
// stack holds no guest frames at all and the whole of the guest lives in its
// memory and its context. That is the only kind of moment a state can be taken
// at, and in practice it means from inside an HLE call, which is reached from
// that loop and is handed the live context.
//
// Taking one from anywhere else would capture a guest halfway through a native
// frame that a restore has no way to rebuild.
//
// Why the fields are written out one at a time
// -------------------------------------------
// A context is trivially copyable, so it could be memcpy'd in one go. It is not,
// because that would bake in this compiler's padding and this machine's byte
// order, and a state is a file someone may keep across a rebuild or move between
// machines. Writing the fields costs a loop and buys a format that means the
// same thing everywhere.

#include "psprecomp/snapshot.hpp"

#include <cstdint>

namespace psprecomp {

struct AllegrexContext;
class GuestMemory;

// "PSRS": a PSPRecomp save state. Checked before anything else is read, so a
// file that is not one at all is refused rather than misread.
inline constexpr std::uint32_t kStateMagic = 0x53525350u;
// The layout of what this header writes. A reader refuses a version it does not
// know rather than reading a newer layout as if it were this one.
inline constexpr std::uint32_t kStateVersion = 1u;

// `profile_tag` identifies which game's host layer wrote the sections that
// follow, and `profile_version` their layout. A state from another profile, or
// from an older layout of the same one, is refused by read_state_header.
void write_state_header(SnapshotWriter &out, std::uint32_t profile_tag, std::uint32_t profile_version);
[[nodiscard]] bool read_state_header(SnapshotReader &in, std::uint32_t profile_tag, std::uint32_t profile_version);

// Main RAM, video RAM and the scratchpad, plus the context. Runs of zero bytes
// are counted rather than stored, which takes a mostly-untouched 24 MiB of guest
// memory down to a fraction of its size.
void write_machine(SnapshotWriter &out, const GuestMemory &memory, const AllegrexContext &context);
[[nodiscard]] bool read_machine(SnapshotReader &in, GuestMemory &memory, AllegrexContext &context);

// One context on its own, for a profile writing out its threads.
void write_context(SnapshotWriter &out, const AllegrexContext &context);
[[nodiscard]] bool read_context(SnapshotReader &in, AllegrexContext &context);

} // namespace psprecomp
