// Save states, checked without the game.
//
// The thing that actually goes wrong with a hand-written serialiser is not a
// clever bug. It is a field added to a structure and written but never read, or
// read in a different order from the one it was written in. Both produce a state
// that loads without complaint and puts the wrong numbers in place, which is the
// worst failure a save state has: the file looked fine and the game is ruined.
//
// Comparing fields one at a time would not catch it, because the comparison is
// written from the same list of fields as the serialiser, so a field missing
// from one is missing from the other. What catches it is round-tripping the
// stream instead:
//
//     write the state  ->  A
//     read A back into a fresh object
//     write that object ->  B
//     A must equal B, byte for byte
//
// A field that is written but not read comes back as its default and B differs.
// A field read in the wrong order corrupts everything after it and B differs.
// And nothing in the test mentions the field list, so it cannot fall out of step
// with the code the way a field-by-field comparison does.
//
// The state is perturbed by running a display list rather than by setting
// members, because the members are private -- which is the right way round: the
// test drives the GE the way the game does.

#include "gpu/ge_state.hpp"
#include "kernel/kernel.hpp"

#include "psprecomp/guest_memory.hpp"
#include "psprecomp/snapshot.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using mga::gpu::GeState;
using mga::Kernel;
using psprecomp::GuestMemory;
using psprecomp::SnapshotReader;
using psprecomp::SnapshotWriter;

namespace {

int failures = 0;

void check(bool ok, const std::string &what) {
    std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n";
    if (!ok) ++failures;
}

// Commands that move the list along or draw, rather than setting a register.
// The point here is to disturb as much register state as possible, so these are
// left out: a jump would leave the list, and a draw would need vertices.
[[nodiscard]] bool is_control_or_draw(std::uint32_t command) {
    switch (command) {
    case 0x04u:  // PRIM
    case 0x05u:  // BEZIER
    case 0x06u:  // SPLINE
    case 0x08u:  // JUMP
    case 0x09u:  // BJUMP
    case 0x0Au:  // CALL
    case 0x0Bu:  // RET
    case 0x0Cu:  // END
    case 0x0Eu:  // SIGNAL
    case 0x0Fu:  // FINISH
        return true;
    default:
        return false;
    }
}

// A list that writes `data` to every register command there is. Run twice with
// different data, this leaves almost nothing at its default -- including the
// matrix write indices, because the matrix upload commands are in the sweep.
void build_sweep(GuestMemory &memory, std::uint32_t address, std::uint32_t data) {
    std::uint32_t at = address;
    for (std::uint32_t command = 0; command < 0x100u; ++command) {
        if (is_control_or_draw(command)) continue;
        memory.store32(at, command << 24u | (data & 0x00FFFFFFu));
        at += 4u;
    }
    // END, so execute() stops here rather than reading whatever follows.
    memory.store32(at, 0x0Cu << 24u);
}

[[nodiscard]] std::vector<std::uint8_t> write_of(const GeState &ge) {
    SnapshotWriter out;
    ge.write_state(out);
    return out.data();
}

} // namespace

int main() {
    GuestMemory memory;

    constexpr std::uint32_t kListOne = GuestMemory::kPhysicalBase + 0x1000u;
    constexpr std::uint32_t kListTwo = GuestMemory::kPhysicalBase + 0x3000u;
    build_sweep(memory, kListOne, 0x00ABCDEFu);
    build_sweep(memory, kListTwo, 0x00135724u);

    GeState source;
    bool finished = false;
    source.execute(memory, kListOne, 0u, finished);
    source.execute(memory, kListTwo, 0u, finished);

    const std::vector<std::uint8_t> first = write_of(source);
    check(!first.empty(), "a GE state writes something");

    // The round trip. If any field is written and not read, or read out of
    // order, the second stream differs from the first.
    GeState restored;
    SnapshotReader in(first);
    check(restored.read_state(in), "a GE state reads back");
    check(in.ok() && in.remaining() == 0u, "reading a GE state consumes exactly what writing it produced");
    const std::vector<std::uint8_t> second = write_of(restored);
    check(second == first, "every GE field written comes back -- the two streams are identical");

    // A fresh state must not accidentally match, or the test above would pass
    // whatever read_state did.
    GeState untouched;
    check(write_of(untouched) != first, "the sweep really did change the state");

    // ---- truncation --------------------------------------------------------
    // A state on disk can be short without being from another build: a full
    // disk, a power cut mid-write, a half-copied file. Every prefix has to be
    // refused rather than half-applied or read past the end.
    //
    // Stepping four bytes at a time rather than one: every field this writes is
    // four bytes or a multiple of it, so a one-byte step would only re-test the
    // same boundaries more slowly.
    bool every_prefix_refused = true;
    bool any_accepted_short = false;
    for (std::size_t length = 0; length < first.size(); length += 4u) {
        GeState victim;
        SnapshotReader partial(first.data(), length);
        if (victim.read_state(partial)) {
            any_accepted_short = true;
            every_prefix_refused = false;
        }
    }
    check(every_prefix_refused, "every truncated GE state is refused");
    check(!any_accepted_short, "no truncated state was read as if it were whole");

    // A stream of the right length but the wrong contents must not be read past
    // its end either. It may well be accepted -- there is no checksum, and the
    // profile version is what distinguishes a state from a different layout --
    // but it must not overrun, and reading it twice must give the same answer.
    {
        std::vector<std::uint8_t> noise(first.size());
        for (std::size_t i = 0; i < noise.size(); ++i) noise[i] = static_cast<std::uint8_t>(i * 31u + 7u);
        GeState victim;
        SnapshotReader reader(noise);
        const bool accepted = victim.read_state(reader);
        check(!accepted || reader.remaining() == 0u, "reading noise did not run off the end of the stream");
        GeState twin;
        SnapshotReader again(noise);
        const bool accepted_again = twin.read_state(again);
        check(accepted == accepted_again && write_of(victim) == write_of(twin),
              "reading the same bytes twice gives the same state");
    }

    // ---- the kernel's objects ----------------------------------------------
    // The same round trip, for the other hand-written field list. Only the
    // public objects are reachable without a running game -- threads need a
    // runtime to create -- but those objects are where most of the fields are,
    // and a field missed in any of them shows up here.
    {
        Kernel kernel;
        auto &semaphore = kernel.semaphores[0x201];
        semaphore.name = "sema";
        semaphore.attributes = 0x1234u;
        semaphore.count = 3;
        semaphore.initial_count = 5;
        semaphore.max_count = 9;
        semaphore.waiters = {0x310, 0x311, 0x312};

        auto &flag = kernel.event_flags[0x202];
        flag.name = "flag";
        flag.attributes = 0x5678u;
        flag.pattern = 0xDEADBEEFu;
        flag.waiters = {0x320};

        auto &mutex = kernel.mutexes[0x203];
        mutex.name = "lock";
        mutex.attributes = 0x9ABCu;
        mutex.owner = 0x330;
        mutex.lock_count = 2;
        mutex.waiters = {0x331, 0x332};

        auto &callback = kernel.callbacks[0x204];
        callback.name = "cb";
        callback.function = 0x08900000u;
        callback.argument = 0x11223344u;
        callback.owner = 0x340;
        callback.notify_count = 7u;
        callback.notify_argument = 0x55667788u;
        callback.pending = true;

        auto &timer = kernel.vtimers[0x205];
        timer.name = "vt";
        timer.active = true;
        timer.base_us = 1'000'003u;
        timer.accumulated_us = 40'009u;
        timer.schedule_us = 500'021u;
        timer.handler = 0x08910000u;
        timer.common = 0x99AABBCCu;

        kernel.sub_interrupts[30u][0u] = mga::SubInterruptHandler{0x08920000u, 0x0D0E0F00u, true};
        kernel.sub_interrupts[30u][2u] = mga::SubInterruptHandler{0x08920040u, 0x01020304u, false};
        kernel.sub_interrupts[7u][1u] = mga::SubInterruptHandler{0x08920080u, 0x05060708u, true};

        mga::AllegrexContext ctx{};
        ctx.pc = 0x08900100u;
        ctx.gpr[4] = 0x42u;

        SnapshotWriter out;
        kernel.write_state(out, ctx);
        const std::vector<std::uint8_t> written = out.data();
        check(!written.empty(), "a kernel writes something");

        Kernel back;
        mga::AllegrexContext restored_ctx{};
        SnapshotReader reader(written);
        // Refused, and rightly: the state names a current thread and there are
        // no threads in it, which is not a kernel that could be run. That the
        // refusal happens is the check; the round trip below is done on a state
        // whose thread list matches its current thread.
        check(!back.read_state(reader, restored_ctx), "a kernel with no thread to run is refused");

        // Truncation, as above: no prefix may be accepted.
        bool refused_all = true;
        for (std::size_t length = 0; length < written.size(); length += 4u) {
            Kernel victim;
            mga::AllegrexContext victim_ctx{};
            SnapshotReader partial(written.data(), length);
            if (victim.read_state(partial, victim_ctx)) refused_all = false;
        }
        check(refused_all, "every truncated kernel state is refused");

        // And a save is allowed when nothing holds a host continuation. An
        // empty kernel holds none.
        check(kernel.why_no_state().empty(), "a quiet kernel allows a save");
    }

    // ---- the kernel, with threads in it ------------------------------------
    // The check above proves a kernel with nothing to run is refused, which is
    // right but leaves the reader's field list untested: nothing was read back.
    // So here a real kernel is built -- installed on a runtime, with threads
    // created and one of them running -- and put through the same stream round
    // trip as the GE state. This is the check that a thread or object field
    // written but not read cannot pass unnoticed.
    {
        psprecomp::Runtime runtime;
        Kernel kernel;
        mga::AllegrexContext ctx{};
        constexpr std::uint32_t kGp = 0x08900000u;
        constexpr std::uint32_t kImageEnd = 0x08A00000u;
        kernel.install(runtime, kGp, kImageEnd);
        kernel.start_loader_thread(ctx, 0x08900100u, 0u);

        // A second thread, left dormant, and a third waiting on a semaphore --
        // so the state carries more than one status and a wait that is not None.
        const std::int32_t second = kernel.create_thread("dormant", 0x08900200u, 0x20u, 0x4000u, 0u, kGp);
        check(second > 0, "a second thread was created");
        const std::int32_t third = kernel.create_thread("waiter", 0x08900300u, 0x18u, 0x4000u, 0u, kGp);
        check(third > 0, "a third thread was created");

        auto &semaphore = kernel.semaphores[0x401];
        semaphore.name = "shared";
        semaphore.attributes = 0u;
        semaphore.count = 0;
        semaphore.initial_count = 1;
        semaphore.max_count = 1;
        semaphore.waiters = {third};
        auto &flag = kernel.event_flags[0x402];
        flag.name = "bits";
        flag.pattern = 0xA5A5A5A5u;
        auto &timer = kernel.vtimers[0x403];
        timer.name = "tick";
        timer.active = true;
        timer.schedule_us = 123'456u;
        timer.handler = 0x08900400u;
        kernel.sub_interrupts[30u][0u] = mga::SubInterruptHandler{0x08900500u, 0x1234u, true};

        SnapshotWriter out;
        kernel.write_state(out, ctx);
        const std::vector<std::uint8_t> first_pass = out.data();

        Kernel restored;
        // Installed too: read_state replaces what a kernel holds, it does not
        // build one from nothing, and a kernel that was never installed has no
        // runtime to hand its threads to.
        mga::AllegrexContext scratch{};
        restored.install(runtime, kGp, kImageEnd);
        SnapshotReader reader(first_pass);
        mga::AllegrexContext restored_ctx{};
        check(restored.read_state(reader, restored_ctx), "a kernel with threads reads back");
        check(reader.ok() && reader.remaining() == 0u,
              "reading a kernel consumes exactly what writing it produced");
        (void)scratch;

        SnapshotWriter again;
        restored.write_state(again, restored_ctx);
        check(again.data() == first_pass,
              "every kernel field written comes back -- the two streams are identical");
        check(restored_ctx.pc == ctx.pc, "the running thread's program counter came back");
        check(restored.why_no_state().empty(), "a restored kernel still allows a save");
    }

    std::cout << (failures == 0 ? "ALL PASS\n" : "FAILURES\n");
    return failures == 0 ? 0 : 1;
}
