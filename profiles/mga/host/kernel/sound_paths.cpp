// The file names a sound command queues, kept alive until the sound module
// reads them.
//
// KCEJ_SOUND takes its orders through one entry point, sound::0x531B15D1 at
// 0x09B0A244. Commands 0x0C and 0x4C ("load these files") are asynchronous:
// the dispatcher copies four words into a ring buffer and returns, and the
// module's own KCEJW-SD thread drains that ring on a ~20 ms cadence. One of
// those four words is a pointer to an array of char *, and the game frees the
// object holding that array as soon as the command is in the ring -- on the
// next few hundred instructions, before any 20 ms have passed.
//
// On a PSP the worker still wins that race. Here the game does: by the time
// KCEJW-SD looks, the array has been reused by the next object constructed in
// that memory, so the module queues a file whose name is whatever the
// allocator left there. That name ends in neither ".psq" nor ".a3p", so the
// loader at 0x09B0F894 drops it without reading a file, without calling the
// game's completion callback, and -- the part that hangs the game -- without
// decrementing the outstanding-load counter at 0x09B33650. The main thread
// polls that counter with command 0x0D000000 and waits for zero, so one lost
// request stalls it for good: the wait that never ends at MISSION START, with
// the card battle behind it.
//
// So copy the array, and the names it points at, into memory of our own before
// the command is queued, and hand the module that copy. The module then reads
// what the game meant no matter which thread gets there first. The copies live
// in a ring of slots, reused far later than any queued command is read.
#include "kernel/sound_paths.hpp"

#include "psprecomp/common.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace mga {
namespace {

// KCEJ_SOUND's command entry, and the two command classes that queue a load.
constexpr std::uint32_t kSoundCommandEntry = 0x09B0A244u;
constexpr std::uint32_t kLoadFileCommand = 0x0Cu;
constexpr std::uint32_t kLoadBankCommand = 0x4Cu;

// The module's own ring holds 256 commands and its queue 256 files, so a copy
// is read long before this many more commands have been issued.
constexpr std::uint32_t kSlots = 24u;
constexpr std::uint32_t kNamesPerSlot = 4u;    // the longest load seen asks for two
constexpr std::uint32_t kNameBytes = 96u;      // "psq/e002st00_1.psq" and friends
constexpr std::uint32_t kSlotBytes = kNamesPerSlot * (4u + kNameBytes);

struct State {
    Runtime::RecompiledFunction original{};
    std::uint32_t base{};   // guest address of the slot ring, 0 until allocated
    std::uint32_t next{};   // slot to use for the next command
    bool trace{};
    std::uint64_t kept{};
};

State &state() {
    static State value;
    return value;
}

// One slot's array of char *, and the bytes each of those points at.
std::uint32_t slot_address(std::uint32_t slot) { return state().base + slot * kSlotBytes; }
std::uint32_t name_address(std::uint32_t slot, std::uint32_t index) {
    return slot_address(slot) + kNamesPerSlot * 4u + index * kNameBytes;
}

// Copies count names out of the game's array into a slot, and returns the
// address of the copied array, or the original where nothing was copied.
std::uint32_t keep_names(Runtime &rt, std::uint32_t array, std::uint32_t count) {
    State &s = state();
    if (s.base == 0u || count == 0u || count > kNamesPerSlot) return array;
    if (!rt.memory().contains(array, count * 4u)) return array;

    const std::uint32_t slot = s.next;
    s.next = (s.next + 1u) % kSlots;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t source = rt.memory().load32(array + i * 4u);
        // A name the game has already lost is not worth copying: leave the
        // pointer alone and let the module decide, as it does today.
        if (!rt.memory().contains(source, 1u)) {
            rt.memory().store32(slot_address(slot) + i * 4u, source);
            continue;
        }
        const std::uint32_t destination = name_address(slot, i);
        std::uint32_t length = 0u;
        while (length + 1u < kNameBytes && rt.memory().contains(source + length, 1u)) {
            const std::uint8_t byte = rt.memory().load8(source + length);
            rt.memory().store8(destination + length, byte);
            if (byte == 0u) break;
            ++length;
        }
        rt.memory().store8(destination + length, 0u);
        rt.memory().store32(slot_address(slot) + i * 4u, destination);
        if (s.trace) {
            std::string text;
            for (std::uint32_t k = 0; k < length; ++k)
                text += static_cast<char>(rt.memory().load8(destination + k));
            std::cout << "[sound] keeping \"" << text << "\" for the sound queue\n";
        }
    }
    ++s.kept;
    return slot_address(slot);
}

void sound_command(Runtime &rt, AllegrexContext &ctx) {
    State &s = state();
    const std::uint32_t command = ctx.gpr[4];
    const std::uint32_t command_class = command >> 24u;
    if (command_class == kLoadFileCommand || command_class == kLoadBankCommand)
        ctx.gpr[5] = keep_names(rt, ctx.gpr[5], command & 0x3FFFu);
    if (s.original != nullptr) s.original(rt, ctx);
}

} // namespace

void install_sound_path_keeper(Runtime &runtime) {
    State &s = state();
    static const bool disabled = std::getenv("MGA_NO_SOUND_PATH_KEEPER") != nullptr;
    if (disabled) return;
    s.trace = std::getenv("MGA_TRACE_SOUND_PATHS") != nullptr;

    const Runtime::RecompiledFunction current = runtime.registered_function(kSoundCommandEntry);
    // Not loaded yet, or already ours.
    if (current == nullptr || current == &sound_command) return;

    if (s.base == 0u) {
        // From the top of memory (type 4), never the bottom: the module loader
        // allocates low, and a stage that lands anywhere but 0x09B38700 does not
        // match its recompiled corpus and drops back to the interpreter.
        const SceUID block =
            kernel().allocate_block("mga_sound_paths", 4u, kSlots * kSlotBytes, 64u);
        if (block < 0) {
            std::cerr << "[sound] no memory to keep queued file names; leaving them to the game\n";
            return;
        }
        s.base = kernel().find_block(block)->address;
        for (std::uint32_t offset = 0; offset < kSlots * kSlotBytes; offset += 4u)
            runtime.memory().store32(s.base + offset, 0u);
    }

    s.original = current;
    runtime.register_function(kSoundCommandEntry, &sound_command, "mga_sound_paths");
}

} // namespace mga
