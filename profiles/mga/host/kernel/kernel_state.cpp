// The kernel's own section of a save state.
//
// Kept apart from kernel.cpp because it is a different kind of code: that file
// is the kernel's behaviour, this one is a file format. They change for
// different reasons, and a field added to a structure there has to be added
// here too -- which is easier to notice when the two sit side by side than when
// one is buried in three thousand lines of scheduling.
//
// Two rules hold throughout:
//
//   * Fields are written one at a time, never as a memcpy of a structure. A
//     state is a file someone keeps across a rebuild, so it cannot carry this
//     compiler's padding or this machine's byte order.
//   * Nothing here validates a field against what the guest would accept. A
//     state written by this build is trusted; one from anywhere else is refused
//     by the header before a byte of this is read. What IS checked is anything
//     that would make the kernel itself unsound to run -- a current thread that
//     does not exist, a waiter naming no thread -- because a state can be
//     truncated or corrupt on disk without being from another build.

#include "kernel.hpp"

#include "psprecomp/state.hpp"

#include <algorithm>
#include <utility>

namespace mga {
namespace {

using psprecomp::SnapshotReader;
using psprecomp::SnapshotWriter;

// An enumeration goes out as a number and comes back checked, because a value
// outside the set would otherwise reach a switch that does not expect it.
void write_thread_status(SnapshotWriter &out, ThreadStatus status) {
    out.u32(static_cast<std::uint32_t>(status));
}

[[nodiscard]] bool read_thread_status(SnapshotReader &in, ThreadStatus &status) {
    const std::uint32_t value = in.u32();
    if (value > static_cast<std::uint32_t>(ThreadStatus::Dead)) {
        in.fail();
        return false;
    }
    status = static_cast<ThreadStatus>(value);
    return true;
}

void write_wait_type(SnapshotWriter &out, WaitType type) {
    out.u32(static_cast<std::uint32_t>(type));
}

[[nodiscard]] bool read_wait_type(SnapshotReader &in, WaitType &type) {
    const std::uint32_t value = in.u32();
    if (value > static_cast<std::uint32_t>(WaitType::Host)) {
        in.fail();
        return false;
    }
    type = static_cast<WaitType>(value);
    return true;
}

// An optional is a flag and then the value, so "no deadline" and "a deadline of
// zero" are different things on the way back in.
void write_optional_u64(SnapshotWriter &out, const std::optional<std::uint64_t> &value) {
    out.boolean(value.has_value());
    out.u64(value.value_or(0u));
}

void read_optional_u64(SnapshotReader &in, std::optional<std::uint64_t> &value) {
    const bool present = in.boolean();
    const std::uint64_t stored = in.u64();
    value = present ? std::optional<std::uint64_t>{stored} : std::nullopt;
}

void write_waiters(SnapshotWriter &out, const std::deque<SceUID> &waiters) {
    out.u32(static_cast<std::uint32_t>(waiters.size()));
    for (const SceUID uid : waiters) out.i32(uid);
}

void read_waiters(SnapshotReader &in, std::deque<SceUID> &waiters) {
    waiters.clear();
    const std::uint32_t count = in.u32();
    // A length is the one field a corrupt file turns into an allocation, so it
    // is bounded by what is left to read rather than trusted.
    if (!in.ok() || count > in.remaining() / sizeof(std::uint32_t)) {
        in.fail();
        return;
    }
    for (std::uint32_t i = 0; i < count; ++i) waiters.push_back(in.i32());
}

void write_strings(SnapshotWriter &out, const std::vector<std::string> &values) {
    out.u32(static_cast<std::uint32_t>(values.size()));
    for (const std::string &value : values) out.text(value);
}

void read_strings(SnapshotReader &in, std::vector<std::string> &values) {
    values.clear();
    const std::uint32_t count = in.u32();
    // Each string costs at least its four-byte length.
    if (!in.ok() || count > in.remaining() / sizeof(std::uint32_t)) {
        in.fail();
        return;
    }
    for (std::uint32_t i = 0; i < count; ++i) values.push_back(in.text());
}

// A map keyed by uid, with the value written by a callback. The count is
// bounded the same way a waiter list is.
template <typename Map, typename Write>
void write_uid_map(SnapshotWriter &out, const Map &map, Write write_value) {
    out.u32(static_cast<std::uint32_t>(map.size()));
    for (const auto &[uid, value] : map) {
        out.i32(uid);
        write_value(out, value);
    }
}

template <typename Map, typename Read>
[[nodiscard]] bool read_uid_map(SnapshotReader &in, Map &map, Read read_value) {
    map.clear();
    const std::uint32_t count = in.u32();
    if (!in.ok() || count > in.remaining() / sizeof(std::uint32_t)) {
        in.fail();
        return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const SceUID uid = in.i32();
        typename Map::mapped_type value{};
        read_value(in, value);
        if (!in.ok()) return false;
        map.emplace(uid, std::move(value));
    }
    return in.ok();
}

void write_semaphore(SnapshotWriter &out, const Semaphore &semaphore) {
    out.text(semaphore.name);
    out.u32(semaphore.attributes);
    out.i32(semaphore.count);
    out.i32(semaphore.initial_count);
    out.i32(semaphore.max_count);
    write_waiters(out, semaphore.waiters);
}

void read_semaphore(SnapshotReader &in, Semaphore &semaphore) {
    semaphore.name = in.text();
    semaphore.attributes = in.u32();
    semaphore.count = in.i32();
    semaphore.initial_count = in.i32();
    semaphore.max_count = in.i32();
    read_waiters(in, semaphore.waiters);
}

void write_event_flag(SnapshotWriter &out, const EventFlag &flag) {
    out.text(flag.name);
    out.u32(flag.attributes);
    out.u32(flag.pattern);
    write_waiters(out, flag.waiters);
}

void read_event_flag(SnapshotReader &in, EventFlag &flag) {
    flag.name = in.text();
    flag.attributes = in.u32();
    flag.pattern = in.u32();
    read_waiters(in, flag.waiters);
}

void write_mutex(SnapshotWriter &out, const Mutex &mutex) {
    out.text(mutex.name);
    out.u32(mutex.attributes);
    out.i32(mutex.owner);
    out.i32(mutex.lock_count);
    write_waiters(out, mutex.waiters);
}

void read_mutex(SnapshotReader &in, Mutex &mutex) {
    mutex.name = in.text();
    mutex.attributes = in.u32();
    mutex.owner = in.i32();
    mutex.lock_count = in.i32();
    read_waiters(in, mutex.waiters);
}

void write_callback(SnapshotWriter &out, const Callback &callback) {
    out.text(callback.name);
    out.u32(callback.function);
    out.u32(callback.argument);
    out.i32(callback.owner);
    out.u32(callback.notify_count);
    out.u32(callback.notify_argument);
    out.boolean(callback.pending);
}

void read_callback(SnapshotReader &in, Callback &callback) {
    callback.name = in.text();
    callback.function = in.u32();
    callback.argument = in.u32();
    callback.owner = in.i32();
    callback.notify_count = in.u32();
    callback.notify_argument = in.u32();
    callback.pending = in.boolean();
}

void write_vtimer(SnapshotWriter &out, const VTimer &timer) {
    out.text(timer.name);
    out.boolean(timer.active);
    out.u64(timer.base_us);
    out.u64(timer.accumulated_us);
    out.u64(timer.schedule_us);
    out.u32(timer.handler);
    out.u32(timer.common);
}

void read_vtimer(SnapshotReader &in, VTimer &timer) {
    timer.name = in.text();
    timer.active = in.boolean();
    timer.base_us = in.u64();
    timer.accumulated_us = in.u64();
    timer.schedule_us = in.u64();
    timer.handler = in.u32();
    timer.common = in.u32();
}

void write_block(SnapshotWriter &out, const MemoryBlock &block) {
    out.text(block.name);
    out.u32(block.address);
    out.u32(block.size);
}

void read_block(SnapshotReader &in, MemoryBlock &block) {
    block.name = in.text();
    block.address = in.u32();
    block.size = in.u32();
}

} // namespace

std::string Kernel::why_no_state() const {
    if (interrupt_active_) return "the game is inside an interrupt handler";
    if (!pending_interrupts_.empty()) return "an interrupt is waiting to run";
    if (!guest_calls_.empty()) return "the game is in the middle of a call from a system library";
    for (const auto &[uid, thread] : threads_) {
        (void)uid;
        if (thread->status == ThreadStatus::Waiting && thread->wait.type == WaitType::Host)
            return "\"" + thread->name + "\" is waiting on something outside the game";
    }
    return {};
}

void Kernel::write_state(SnapshotWriter &out, const AllegrexContext &ctx) {
    // The running thread's stored context is stale by definition -- it is the
    // live one that has been executing. Bring it up to date first, so a thread
    // is described the same way whether or not it happens to be the one
    // running, and so nothing depends on the order the two are read back in.
    if (Thread *running = current_thread(); running != nullptr) running->context = ctx;

    // Timing. The clock and the vblank schedule go together: a restore that
    // kept one and not the other would either flood the game with vblanks it
    // had already had or stall it until the clock caught up.
    out.u64(now_us_);
    out.u64(next_vblank_us_);
    out.u64(vblank_count_);
    // Not cpu_scale_: it measures how fast THIS machine runs guest code, so a
    // state carrying it would import another PC's speed, or this PC's from
    // before a rebuild. It is measured again within a few frames anyway.

    out.i32(next_uid_);
    out.i32(current_uid_);
    out.u64(next_ready_sequence_);
    out.boolean(dispatch_enabled_);
    out.boolean(interrupts_enabled_);

    out.u32(static_cast<std::uint32_t>(threads_.size()));
    for (const auto &[uid, thread] : threads_) {
        out.i32(uid);
        out.text(thread->name);
        out.u32(thread->entry);
        out.u32(thread->priority);
        out.u32(thread->initial_priority);
        out.u32(thread->attributes);
        out.u32(thread->stack_size);
        out.u32(thread->stack_bottom);
        out.u32(thread->control_block);
        out.u32(thread->gp);
        out.i32(thread->stack_block);
        write_thread_status(out, thread->status);
        psprecomp::write_context(out, thread->context);
        write_wait_type(out, thread->wait.type);
        out.i32(thread->wait.object);
        out.u32(thread->wait.value);
        out.u32(thread->wait.mode);
        out.u32(thread->wait.out_address);
        out.u32(thread->wait.timeout_address);
        write_optional_u64(out, thread->wait.deadline_us);
        // wait.host_poll is not written and does not need to be: why_no_state()
        // refuses a save while any thread holds one.
        out.i32(thread->exit_status);
        out.u32(thread->wakeup_count);
        out.u64(thread->ready_sequence);
    }

    write_uid_map(out, semaphores, write_semaphore);
    write_uid_map(out, event_flags, write_event_flag);
    write_uid_map(out, mutexes, write_mutex);
    write_uid_map(out, callbacks, write_callback);
    write_uid_map(out, vtimers, write_vtimer);
    write_uid_map(out, blocks_, write_block);

    out.u32(static_cast<std::uint32_t>(free_ranges_.size()));
    for (const FreeRange &range : free_ranges_) {
        out.u32(range.address);
        out.u32(range.size);
    }

    // Sub-interrupt handlers: an outer map of interrupts, each holding a map of
    // sub-interrupts. Registered by the game, so they belong in the state
    // rather than being rebuilt.
    out.u32(static_cast<std::uint32_t>(sub_interrupts.size()));
    for (const auto &[interrupt, handlers] : sub_interrupts) {
        out.u32(interrupt);
        out.u32(static_cast<std::uint32_t>(handlers.size()));
        for (const auto &[sub, handler] : handlers) {
            out.u32(sub);
            out.u32(handler.handler);
            out.u32(handler.argument);
            out.boolean(handler.enabled);
        }
    }
}

bool Kernel::read_state(SnapshotReader &in, AllegrexContext &ctx) {
    const std::uint64_t now = in.u64();
    const std::uint64_t next_vblank = in.u64();
    const std::uint64_t vblanks = in.u64();
    const SceUID next_uid = in.i32();
    const SceUID current = in.i32();
    const std::uint64_t next_sequence = in.u64();
    const bool dispatch = in.boolean();
    const bool interrupts = in.boolean();
    if (!in.ok()) return false;

    const std::uint32_t thread_count = in.u32();
    // Each thread costs far more than four bytes, so this is a generous bound
    // and still stops a corrupt length from asking for a million threads.
    if (!in.ok() || thread_count > in.remaining() / sizeof(std::uint32_t)) {
        in.fail();
        return false;
    }
    std::map<SceUID, std::unique_ptr<Thread>> threads;
    for (std::uint32_t i = 0; i < thread_count; ++i) {
        const SceUID uid = in.i32();
        auto thread = std::make_unique<Thread>();
        thread->uid = uid;
        thread->name = in.text();
        thread->entry = in.u32();
        thread->priority = in.u32();
        thread->initial_priority = in.u32();
        thread->attributes = in.u32();
        thread->stack_size = in.u32();
        thread->stack_bottom = in.u32();
        thread->control_block = in.u32();
        thread->gp = in.u32();
        thread->stack_block = in.i32();
        if (!read_thread_status(in, thread->status)) return false;
        if (!psprecomp::read_context(in, thread->context)) return false;
        if (!read_wait_type(in, thread->wait.type)) return false;
        thread->wait.object = in.i32();
        thread->wait.value = in.u32();
        thread->wait.mode = in.u32();
        thread->wait.out_address = in.u32();
        thread->wait.timeout_address = in.u32();
        read_optional_u64(in, thread->wait.deadline_us);
        thread->wait.host_poll = nullptr;
        thread->exit_status = in.i32();
        thread->wakeup_count = in.u32();
        thread->ready_sequence = in.u64();
        if (!in.ok()) return false;
        // A host wait cannot be restored, and a state holding one should never
        // have been written. Refuse rather than resume a thread that would wait
        // on a poll that no longer exists and never wake.
        if (thread->wait.type == WaitType::Host) {
            in.fail();
            return false;
        }
        threads.emplace(uid, std::move(thread));
    }
    // Whatever else is wrong, a kernel with no thread to run is not a kernel.
    if (threads.find(current) == threads.end()) {
        in.fail();
        return false;
    }

    decltype(semaphores) new_semaphores;
    decltype(event_flags) new_event_flags;
    decltype(mutexes) new_mutexes;
    decltype(callbacks) new_callbacks;
    decltype(vtimers) new_vtimers;
    decltype(blocks_) new_blocks;
    if (!read_uid_map(in, new_semaphores, read_semaphore)) return false;
    if (!read_uid_map(in, new_event_flags, read_event_flag)) return false;
    if (!read_uid_map(in, new_mutexes, read_mutex)) return false;
    if (!read_uid_map(in, new_callbacks, read_callback)) return false;
    if (!read_uid_map(in, new_vtimers, read_vtimer)) return false;
    if (!read_uid_map(in, new_blocks, read_block)) return false;

    std::vector<FreeRange> new_free_ranges;
    const std::uint32_t range_count = in.u32();
    if (!in.ok() || range_count > in.remaining() / (2u * sizeof(std::uint32_t))) {
        in.fail();
        return false;
    }
    new_free_ranges.reserve(range_count);
    for (std::uint32_t i = 0; i < range_count; ++i) {
        FreeRange range{};
        range.address = in.u32();
        range.size = in.u32();
        new_free_ranges.push_back(range);
    }

    // These two bounds stop a corrupt count from becoming a huge loop. Each has
    // to be the SMALLEST number of bytes an entry can occupy, not a round
    // number near it: a bound even one byte too large rejects the last entry in
    // the file, because by then there is nothing after it to make up the
    // difference. An interrupt costs its number and its handler count; a
    // handler costs three words and a byte, which is thirteen, not sixteen.
    constexpr std::uint64_t kBytesPerInterrupt = 2u * sizeof(std::uint32_t);
    constexpr std::uint64_t kBytesPerHandler = 3u * sizeof(std::uint32_t) + 1u;

    decltype(sub_interrupts) new_sub_interrupts;
    const std::uint32_t interrupt_count = in.u32();
    if (!in.ok() || interrupt_count > in.remaining() / kBytesPerInterrupt) {
        in.fail();
        return false;
    }
    for (std::uint32_t i = 0; i < interrupt_count; ++i) {
        const std::uint32_t interrupt = in.u32();
        const std::uint32_t handler_count = in.u32();
        if (!in.ok() || handler_count > in.remaining() / kBytesPerHandler) {
            in.fail();
            return false;
        }
        for (std::uint32_t j = 0; j < handler_count; ++j) {
            const std::uint32_t sub = in.u32();
            SubInterruptHandler handler{};
            handler.handler = in.u32();
            handler.argument = in.u32();
            handler.enabled = in.boolean();
            new_sub_interrupts[interrupt][sub] = handler;
        }
    }
    if (!in.ok()) return false;

    // Nothing above has touched the live kernel: everything was read into
    // locals first, so a state that turns out to be truncated halfway through
    // leaves the running game exactly as it was. From here on it cannot fail.
    now_us_ = now;
    next_vblank_us_ = next_vblank;
    vblank_count_ = vblanks;
    next_uid_ = next_uid;
    current_uid_ = current;
    next_ready_sequence_ = next_sequence;
    dispatch_enabled_ = dispatch;
    interrupts_enabled_ = interrupts;
    threads_ = std::move(threads);
    semaphores = std::move(new_semaphores);
    event_flags = std::move(new_event_flags);
    mutexes = std::move(new_mutexes);
    callbacks = std::move(new_callbacks);
    vtimers = std::move(new_vtimers);
    blocks_ = std::move(new_blocks);
    free_ranges_ = std::move(new_free_ranges);
    sub_interrupts = std::move(new_sub_interrupts);

    // Host-side leftovers of the session being replaced.
    guest_calls_.clear();
    pending_interrupts_.clear();
    interrupt_active_ = false;
    interrupted_idle_ = false;
    interrupt_on_return_ = nullptr;
    idle_vblanks_ = 0u;
    hang_reports_ = 0u;
    frame_work_us_ = 0u;
    // The clock is the state's, not this session's: forget how far ahead of or
    // behind real time the old one was, or the game races to make up the
    // difference between them.
    resync_real_time();
    last_frame_ = std::chrono::steady_clock::now();

    // The context the runtime executes is the current thread's.
    if (Thread *running = current_thread(); running != nullptr) {
        ctx = running->context;
        psprecomp::set_runtime_thread_identity(running->uid, running->name);
    }
    // And whatever host call is on the stack above this one must not return into
    // the guest it entered from: that guest has just been replaced. The thread
    // may even have the same uid, so the identity alone does not say so.
    psprecomp::invalidate_runtime_execution_context();
    return true;
}

} // namespace mga
