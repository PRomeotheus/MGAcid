// Kernel and utility calls Metal Gear Ac!d imports that the MHP3rd host never
// needed: fixed-size memory pools, waiting for a thread to end, and a few
// calls of the first PSP firmware generation.
#include "hle_common.hpp"

#include "psprecomp/common.hpp"

#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mga {
namespace {

void success(Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 0u); }

[[nodiscard]] SceUID as_uid(std::uint32_t value) noexcept { return static_cast<SceUID>(value); }

// --- fixed-size pools (Fpl) -------------------------------------------------

struct FixedPool {
    std::string name;
    SceUID block{};
    std::uint32_t base{};
    std::uint32_t block_size{};
    std::vector<bool> used;
};

std::map<SceUID, FixedPool> &pools() {
    static std::map<SceUID, FixedPool> p;
    return p;
}

std::optional<std::uint32_t> take_block(FixedPool &pool) {
    for (std::size_t i = 0; i < pool.used.size(); ++i) {
        if (pool.used[i]) continue;
        pool.used[i] = true;
        return pool.base + static_cast<std::uint32_t>(i) * pool.block_size;
    }
    return std::nullopt;
}

bool trace_fpl() {
    static const bool enabled = std::getenv("MGA_TRACE_FPL") != nullptr;
    return enabled;
}

// sceKernelAllocateFpl and its CB variant: wait until a block is free.
void allocate_fpl(Runtime &rt, AllegrexContext &ctx, bool callbacks) {
    const SceUID uid = as_uid(arg(ctx, 0));
    const std::uint32_t out = arg(ctx, 1);
    const std::uint32_t timeout_address = arg(ctx, 2);
    auto found = pools().find(uid);
    if (found == pools().end()) {
        kernel().finish(ctx, error::kUnknownUid);
        return;
    }
    if (const auto address = take_block(found->second)) {
        rt.memory().store32(out, *address);
        if (trace_fpl()) std::cout << "[fpl] " << found->second.name << " -> " << psprecomp::hex32(*address) << "\n";
        kernel().finish(ctx, 0u);
        return;
    }
    if (callbacks) (void)kernel().deliver_callbacks();
    std::optional<std::uint64_t> timeout;
    if (timeout_address != 0u) timeout = rt.memory().load32(timeout_address);
    kernel().wait_host(ctx, timeout, [uid, out, &rt](bool timed_out) -> std::optional<std::uint32_t> {
        auto pool = pools().find(uid);
        if (pool == pools().end()) return error::kUnknownUid;
        if (const auto address = take_block(pool->second)) {
            rt.memory().store32(out, *address);
            return 0u;
        }
        if (timed_out) return error::kWaitTimeout;
        return std::nullopt;
    });
}

void register_fpl(HleRegistrar &hle) {
    // sceKernelCreateFpl(name, partition, attributes, block_size, blocks, option)
    hle.add("ThreadManForUser", "sceKernelCreateFpl", [](Runtime &rt, AllegrexContext &ctx) {
        const std::string name = read_cstring(rt.memory(), arg(ctx, 0), 32u);
        const std::uint32_t attributes = arg(ctx, 2);
        const std::uint32_t block_size = (arg(ctx, 3) + 3u) & ~3u;
        const std::uint32_t blocks = arg(ctx, 4);
        if (block_size == 0u || blocks == 0u) {
            kernel().finish(ctx, error::kIllegalArgument);
            return;
        }
        // Attribute 0x4000 places the pool at the high end of the partition.
        const std::uint32_t type = (attributes & 0x4000u) != 0u ? 1u : 0u;
        const SceUID block = kernel().allocate_block("Fpl " + name, type, block_size * blocks, 0u);
        if (block < 0) {
            std::cerr << "[fpl] no memory for " << name << " (" << block_size << " x " << blocks << ")\n";
            kernel().finish(ctx, error::kNoMemory);
            return;
        }
        const SceUID uid = kernel().allocate_uid();
        FixedPool pool;
        pool.name = name;
        pool.block = block;
        pool.base = kernel().find_block(block)->address;
        pool.block_size = block_size;
        pool.used.assign(blocks, false);
        std::cout << "[fpl] created " << name << ": " << blocks << " x " << block_size << " bytes at "
                  << psprecomp::hex32(pool.base) << "\n";
        pools()[uid] = std::move(pool);
        kernel().finish(ctx, static_cast<std::uint32_t>(uid));
    });
    hle.add("ThreadManForUser", "sceKernelAllocateFpl",
            [](Runtime &rt, AllegrexContext &ctx) { allocate_fpl(rt, ctx, false); });
    hle.add("ThreadManForUser", "sceKernelAllocateFplCB",
            [](Runtime &rt, AllegrexContext &ctx) { allocate_fpl(rt, ctx, true); });
    hle.add("ThreadManForUser", "sceKernelTryAllocateFpl", [](Runtime &rt, AllegrexContext &ctx) {
        auto found = pools().find(as_uid(arg(ctx, 0)));
        if (found == pools().end()) {
            kernel().finish(ctx, error::kUnknownUid);
            return;
        }
        const auto address = take_block(found->second);
        if (!address) {
            kernel().finish(ctx, error::kNoMemory);
            return;
        }
        rt.memory().store32(arg(ctx, 1), *address);
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelFreeFpl", [](Runtime &, AllegrexContext &ctx) {
        auto found = pools().find(as_uid(arg(ctx, 0)));
        if (found == pools().end()) {
            kernel().finish(ctx, error::kUnknownUid);
            return;
        }
        FixedPool &pool = found->second;
        const std::uint32_t address = arg(ctx, 1);
        if (address < pool.base || (address - pool.base) % pool.block_size != 0u ||
            (address - pool.base) / pool.block_size >= pool.used.size()) {
            kernel().finish(ctx, error::kIllegalMemblock);
            return;
        }
        pool.used[(address - pool.base) / pool.block_size] = false;
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelDeleteFpl", [](Runtime &, AllegrexContext &ctx) {
        auto found = pools().find(as_uid(arg(ctx, 0)));
        if (found == pools().end()) {
            kernel().finish(ctx, error::kUnknownUid);
            return;
        }
        (void)kernel().free_block(found->second.block);
        pools().erase(found);
        kernel().finish(ctx, 0u);
    });
}

// --- threads ----------------------------------------------------------------

constexpr std::uint32_t kReleaseWait = 0x800201AAu;  // SCE_KERNEL_ERROR_RELEASE_WAIT

void wait_thread_end(Runtime &rt, AllegrexContext &ctx, bool callbacks) {
    const SceUID uid = as_uid(arg(ctx, 0));
    Thread *thread = kernel().find_thread(uid);
    if (thread == nullptr) {
        kernel().finish(ctx, error::kUnknownThid);
        return;
    }
    if (thread->status == ThreadStatus::Dormant || thread->status == ThreadStatus::Dead) {
        kernel().finish(ctx, static_cast<std::uint32_t>(thread->exit_status));
        return;
    }
    if (callbacks) (void)kernel().deliver_callbacks();
    WaitState wait{};
    wait.type = WaitType::ThreadEnd;
    wait.object = uid;
    if (const std::uint32_t timeout = arg(ctx, 1); timeout != 0u) {
        wait.timeout_address = timeout;
        wait.deadline_us = kernel().now_us() + rt.memory().load32(timeout);
    }
    kernel().block(ctx, wait);
}

void register_threads(HleRegistrar &hle) {
    hle.add("ThreadManForUser", "sceKernelWaitThreadEnd",
            [](Runtime &rt, AllegrexContext &ctx) { wait_thread_end(rt, ctx, false); });
    hle.add("ThreadManForUser", "sceKernelWaitThreadEndCB",
            [](Runtime &rt, AllegrexContext &ctx) { wait_thread_end(rt, ctx, true); });
    hle.add("ThreadManForUser", "sceKernelReleaseWaitThread", [](Runtime &, AllegrexContext &ctx) {
        Thread *thread = kernel().find_thread(as_uid(arg(ctx, 0)));
        if (thread == nullptr) {
            kernel().finish(ctx, error::kUnknownThid);
            return;
        }
        if (thread->status != ThreadStatus::Waiting) {
            kernel().finish(ctx, 0x800201A4u);  // SCE_KERNEL_ERROR_NOT_WAIT
            return;
        }
        kernel().wake(*thread, kReleaseWait);
        kernel().finish(ctx, 0u);
    });
    hle.add("ModuleMgrForUser", "sceKernelSelfStopUnloadModule", [](Runtime &, AllegrexContext &ctx) {
        kernel().exit_current_thread(ctx, static_cast<std::int32_t>(arg(ctx, 1)), true);
    });
}

} // namespace

void register_mga(HleRegistrar &hle) {
    register_fpl(hle);
    register_threads(hle);
    hle.add("IoFileMgrForUser", "sceIoAssign", success);
    hle.add("UtilsForUser", "sceKernelGetGPI", success);
    hle.add("UtilsForUser", "sceKernelDcacheWritebackInvalidateRange", success);
    hle.add("sceSuspendForUser", "sceKernelPowerLock", success);
    hle.add("sceSuspendForUser", "sceKernelPowerUnlock", success);
    // No decoder error to report: *error = 0.
    hle.add("sceAtrac3plus", "sceAtracGetInternalErrorInfo", [](Runtime &rt, AllegrexContext &ctx) {
        if (arg(ctx, 1) != 0u) rt.memory().store32(arg(ctx, 1), 0u);
        kernel().finish(ctx, 0u);
    });
}

} // namespace mga
