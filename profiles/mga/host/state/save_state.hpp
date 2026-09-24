#pragma once

// Save states: the whole machine written to a file, and read back into a
// running process.
//
// Where one can be taken
// ----------------------
// The runtime dispatches one guest function at a time from an outer loop, and
// those functions return to it. Between two iterations of that loop the host
// stack holds no guest frames at all, and the whole of the guest lives in its
// memory and its contexts. That is the only kind of moment a state can be taken
// at, and in practice it means from inside an HLE call, which is reached from
// that loop and is handed the live context.
//
// What is refused, and why that is the design rather than a shortcut
// ------------------------------------------------------------------
// Almost everything the host holds on the guest's behalf is plain data. The
// exception is a host continuation: a thread waiting on a condition only the
// host can check, an interrupt queued with a host callback to run when the
// guest handler returns, a call into guest code made by an HLE import that is
// waiting to be resumed. Each of those is a closure. A closure cannot be
// written to a file, and it cannot be rebuilt on the way back in, because what
// it captures is host state that no longer exists.
//
// There are two ways to deal with that. One is to make every host wait
// resumable -- give each a name and a table of how to restart it -- which is a
// large change across the whole HLE layer. The other is to notice where those
// continuations actually occur: during a load, while a video plays, while the
// network is being polled. None of them is a moment a player wants to save at.
// So a save is refused while one is live, and says which one, and that is the
// whole of it.
//
// What is not in a state
// ----------------------
// Anything belonging to this session rather than to the game: how fast this
// machine runs guest code, where the disc image is, the renderer's targets and
// caches, host clock readings. A state that carried those would import another
// PC's measurements, or point a restore at a disc the player no longer has.

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace mga::state {

using psprecomp::AllegrexContext;
using psprecomp::Runtime;

// "MGAc": which game's host layer wrote the sections after the common header.
// A state from another profile is refused by psprecomp::read_state_header.
inline constexpr std::uint32_t kProfileTag = 0x4341474Du;
// The layout of the sections this profile writes. Raise it whenever a field is
// added, removed or reordered anywhere in them: a reader refuses a version it
// does not know rather than reading a different layout as if it were this one,
// which is the failure that loads nonsense instead of reporting a mismatch.
// 2: the music section was added after the display and graphics one.
inline constexpr std::uint32_t kProfileVersion = 2u;

// How many slots the interface offers.
inline constexpr unsigned kSlotCount = 4u;

// <user data>/states/slot<n>.mgastate
[[nodiscard]] std::filesystem::path slot_path(unsigned slot);
// Whether that file exists, and when it was written -- for the menu to show.
[[nodiscard]] bool slot_exists(unsigned slot);
[[nodiscard]] std::string slot_description(unsigned slot);

// Why a state cannot be taken right now, in a form fit to show the player, or
// empty when one can. Asked by the interface to grey a row out before the player
// picks it, and again by save() so the answer cannot go stale in between.
[[nodiscard]] std::string why_not_now();

// Writes the whole machine to `slot`. False with `reason` set when it cannot:
// either because of the above, or because the file could not be written.
[[nodiscard]] bool save(Runtime &runtime, const AllegrexContext &ctx, unsigned slot, std::string &reason);

// Reads `slot` back into this process. False with `reason` set when the file is
// missing, from another profile or build, or corrupt.
//
// A failed load leaves the running game untouched. Every section reads into
// locals and commits only once all of them have succeeded, so a truncated file
// is a refusal rather than half a restore -- which would be worse than no save
// state at all.
[[nodiscard]] bool load(Runtime &runtime, AllegrexContext &ctx, unsigned slot, std::string &reason);

// Asking for a save or a load from somewhere that cannot do one
// ------------------------------------------------------------
// The menu runs from inside the frame's display call and has no guest context
// to hand, and a hotkey is read even further from one. Neither may call save()
// or load() directly: the whole design rests on those happening at a dispatch
// boundary, and code that merely happens to be near one is not the same thing.
//
// So they record what the player asked for, and the display call -- which does
// hold the live context, at exactly the right moment -- carries it out. One
// place performs a state, and it is the place that is allowed to.
enum class Request { None, Save, Load };

void request(Request kind, unsigned slot);
[[nodiscard]] bool request_pending();

// Carries out a pending request, if there is one. Returns true when a state was
// LOADED, which tells the caller that the context it was given now belongs to a
// different session: it must not finish the call it was in the middle of, since
// the guest that made that call has been replaced.
bool run_pending(Runtime &runtime, AllegrexContext &ctx);

// What happened, for the interface to show: empty until something has.
[[nodiscard]] const std::string &last_message();

} // namespace mga::state
