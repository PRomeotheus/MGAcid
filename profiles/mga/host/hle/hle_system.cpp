// UtilsForUser, LoadExecForUser, StdioForUser, ModuleMgrForUser,
// InterruptManager, scePower, sceRtc, sceImpose, sceOpenPSID and the parameter
// part of sceUtility. sceWlanDrv is with the ad hoc calls in hle_adhoc.cpp.
#include "hle_common.hpp"

#include "kernel/module_loader.hpp"
#include "kernel/module_sweep.hpp"
#include "overlays.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <vector>

namespace mga {
namespace {

// Epoch the virtual PSP clock starts at, so guest wall time advances with it.
std::uint64_t boot_unix_us() {
    static const std::uint64_t value = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    return value;
}

std::uint64_t guest_unix_us() { return boot_unix_us() + kernel().now_us(); }

void success(Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 0u); }

void register_utils(HleRegistrar &hle) {
    for (const char *name : {"sceKernelDcacheWritebackAll", "sceKernelDcacheWritebackInvalidateAll",
                             "sceKernelDcacheInvalidateRange", "sceKernelDcacheWritebackRange", "sceKernelSetGPO"})
        hle.add("UtilsForUser", name, success);
    // The guest flushes the instruction cache after copying code into an overlay
    // slot, which is the profile's cue to re-check which overlay is loaded.
    const auto flush_icache = [](Runtime &rt, AllegrexContext &ctx) {
        revalidate_overlays(rt);
        kernel().finish(ctx, 0u);
    };
    hle.add("UtilsForUser", "sceKernelIcacheInvalidateAll", flush_icache);
    hle.add("UtilsForUser", "sceKernelIcacheInvalidateRange", flush_icache);
    hle.add("UtilsForUser", "sceKernelLibcTime", [](Runtime &rt, AllegrexContext &ctx) {
        const auto seconds = static_cast<std::uint32_t>(guest_unix_us() / 1'000'000u);
        if (arg(ctx, 0) != 0u) rt.memory().store32(arg(ctx, 0), seconds);
        kernel().finish(ctx, seconds);
    });
    hle.add("UtilsForUser", "sceKernelLibcGettimeofday", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint64_t now = guest_unix_us();
        if (arg(ctx, 0) != 0u) {
            rt.memory().store32(arg(ctx, 0), static_cast<std::uint32_t>(now / 1'000'000u));
            rt.memory().store32(arg(ctx, 0) + 4u, static_cast<std::uint32_t>(now % 1'000'000u));
        }
        if (arg(ctx, 1) != 0u) {
            rt.memory().store32(arg(ctx, 1), 0u);
            rt.memory().store32(arg(ctx, 1) + 4u, 0u);
        }
        kernel().finish(ctx, 0u);
    });
    hle.add("UtilsForUser", "sceKernelLibcClock", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, static_cast<std::uint32_t>(kernel().now_us()));
    });
}

void register_process(HleRegistrar &hle) {
    hle.add("LoadExecForUser", "sceKernelRegisterExitCallback", success);
    hle.add("LoadExecForUser", "sceKernelExitGame", [](Runtime &rt, AllegrexContext &ctx) {
        (void)ctx;
        rt.stop("guest called sceKernelExitGame");
    });
    hle.add("StdioForUser", "sceKernelStdin", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 0u); });
    hle.add("StdioForUser", "sceKernelStdout", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 1u); });
    hle.add("StdioForUser", "sceKernelStderr", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 2u); });

    // The main executable is the only module; overlays are loaded by game code.
    constexpr std::uint32_t kMainModuleId = 0x01000001u;
    hle.add("ModuleMgrForUser", "sceKernelGetModuleId", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, kMainModuleId);
    });
    hle.add("ModuleMgrForUser", "sceKernelGetModuleIdByAddress", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, kMainModuleId);
    });
    // Metal Gear Ac!d loads its own libraries and every stage as plain ELF
    // PRX files: those are loaded, relocated and linked for real (see
    // kernel/module_loader.hpp). Sony's stock modules, encrypted or plain,
    // are served by this profile's HLE, so their load is acknowledged with a
    // module id and the image itself is never touched.
    hle.add("ModuleMgrForUser", "sceKernelLoadModuleByID", [](Runtime &rt, AllegrexContext &ctx) {
        std::vector<std::uint8_t> image;
        {
            constexpr std::size_t kChunk = 256u * 1024u;
            std::size_t got = 0u;
            do {
                const std::size_t offset = image.size();
                image.resize(offset + kChunk);
                got = read_open_file(arg(ctx, 0), offset, image.data() + offset, kChunk);
                image.resize(offset + got);
            } while (got == kChunk);
        }
        if (const auto module_name = elf_module_name(image); module_name && !module_is_hle(*module_name)) {
            try {
                const LoadedModule *module = load_module(rt, std::move(image), *module_name);
                kernel().finish(ctx, static_cast<std::uint32_t>(module->uid));
            } catch (const std::exception &e) {
                std::cerr << "[module] cannot load " << *module_name << ": " << e.what() << "\n";
                kernel().finish(ctx, error::kNoMemory);
            }
            return;
        }
        std::array<std::uint8_t, 0x100> header{};
        std::memcpy(header.data(), image.data(), std::min(header.size(), image.size()));
        const std::size_t count = std::min(header.size(), image.size());
        std::string name = "unknown";
        if (const auto module_name = elf_module_name(image)) name = *module_name;
        for (std::size_t i = 0; i + 0x2Au < count; ++i) {
            if (std::memcmp(header.data() + i, "~PSP", 4u) != 0) continue;
            const char *text = reinterpret_cast<const char *>(header.data() + i + 0x0Au);
            name.assign(text, strnlen(text, 28u));
            break;
        }
        const auto uid = static_cast<std::uint32_t>(kernel().allocate_uid());
        log_once("module:" + name, "[module] loaded stock module " + name + " (HLE)");
        kernel().finish(ctx, uid);
    });
    // sceKernelStartModule(uid, argsize, argp, status*, option*). A loaded
    // module's module_start runs in the calling thread with the module's own
    // gp, which the module manager's start thread would give it, and the call
    // returns once it has, with the module id.
    hle.add("ModuleMgrForUser", "sceKernelStartModule", [](Runtime &, AllegrexContext &ctx) {
        const SceUID uid = static_cast<SceUID>(arg(ctx, 0));
        LoadedModule *module = find_module(uid);
        if (module == nullptr || module->start == 0u || module->started) {
            log_once("module-op:start", "[module] sceKernelStartModule (HLE module)");
            kernel().finish(ctx, arg(ctx, 0));
            return;
        }
        // MGA_SWEEP_MODULES: the first stage the game starts is the signal
        // that a stage has something to link against, and from there the
        // sweep drives the loader itself. It never gives the call back.
        if (sweep_take_over(kernel().runtime(), ctx, *module)) return;
        module->started = true;
        const std::uint32_t status_address = arg(ctx, 3);
        const std::uint32_t caller_gp = ctx.gpr[28];
        std::cout << "[module] starting " << module->name << " at " << psprecomp::hex32(module->start) << "\n";
        if (const char *peek = std::getenv("MGA_PEEK_MODULE"); peek != nullptr) {
            const std::uint32_t offset = static_cast<std::uint32_t>(std::strtoul(peek, nullptr, 0));
            std::cout << "[module] word at +" << psprecomp::hex32(offset) << " = "
                      << psprecomp::hex32(kernel().runtime().memory().load32(module->base + offset)) << " (at start)\n";
        }
        ctx.set_gpr(28, module->gp);
        kernel().call_guest(ctx, module->start, {arg(ctx, 1), arg(ctx, 2), 0u, 0u},
                            [uid, status_address, caller_gp](AllegrexContext &c, std::uint32_t result) {
                                c.set_gpr(28, caller_gp);
                                if (status_address != 0u) kernel().runtime().memory().store32(status_address, result);
                                kernel().finish(c, static_cast<std::uint32_t>(uid));
                            });
    });
    hle.add("ModuleMgrForUser", "sceKernelStopModule", [](Runtime &, AllegrexContext &ctx) {
        const SceUID uid = static_cast<SceUID>(arg(ctx, 0));
        LoadedModule *module = find_module(uid);
        if (module == nullptr || module->stop == 0u || !module->started) {
            if (module != nullptr) module->started = false;
            kernel().finish(ctx, arg(ctx, 0));
            return;
        }
        module->started = false;
        const std::uint32_t status_address = arg(ctx, 3);
        const std::uint32_t caller_gp = ctx.gpr[28];
        ctx.set_gpr(28, module->gp);
        kernel().call_guest(ctx, module->stop, {arg(ctx, 1), arg(ctx, 2), 0u, 0u},
                            [uid, status_address, caller_gp](AllegrexContext &c, std::uint32_t result) {
                                c.set_gpr(28, caller_gp);
                                if (status_address != 0u) kernel().runtime().memory().store32(status_address, result);
                                kernel().finish(c, static_cast<std::uint32_t>(uid));
                            });
    });
    hle.add("ModuleMgrForUser", "sceKernelUnloadModule", [](Runtime &rt, AllegrexContext &ctx) {
        unload_module(rt, static_cast<SceUID>(arg(ctx, 0)));
        kernel().finish(ctx, arg(ctx, 0));
    });
    hle.add("ModuleMgrForUser", "sceKernelStopUnloadSelfModuleWithStatus", [](Runtime &, AllegrexContext &ctx) {
        kernel().exit_current_thread(ctx, static_cast<std::int32_t>(arg(ctx, 1)), true);
    });
}

void register_interrupts(HleRegistrar &hle) {
    hle.add("InterruptManager", "sceKernelRegisterSubIntrHandler", [](Runtime &, AllegrexContext &ctx) {
        auto &handler = kernel().sub_interrupts[arg(ctx, 0)][arg(ctx, 1)];
        handler.handler = arg(ctx, 2);
        handler.argument = arg(ctx, 3);
        kernel().finish(ctx, 0u);
    });
    hle.add("InterruptManager", "sceKernelReleaseSubIntrHandler", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, kernel().sub_interrupts[arg(ctx, 0)].erase(arg(ctx, 1)) != 0u ? 0u : error::kIllegalArgument);
    });
    hle.add("InterruptManager", "sceKernelEnableSubIntr", [](Runtime &, AllegrexContext &ctx) {
        auto &handlers = kernel().sub_interrupts[arg(ctx, 0)];
        const auto found = handlers.find(arg(ctx, 1));
        if (found != handlers.end()) found->second.enabled = true;
        kernel().finish(ctx, found != handlers.end() ? 0u : error::kIllegalArgument);
    });
}

void register_platform(HleRegistrar &hle) {
    hle.add("scePower", "scePowerRegisterCallback", [](Runtime &, AllegrexContext &ctx) {
        // Report mains power with a healthy battery so the game stops waiting for
        // the first power notification.
        constexpr std::uint32_t kAcPower = 0x00001000u;
        constexpr std::uint32_t kBatteryExists = 0x00000080u;
        constexpr std::uint32_t kBatteryPower = 0x00000004u;
        kernel().notify_callback(static_cast<SceUID>(arg(ctx, 1)), kAcPower | kBatteryExists | kBatteryPower);
        kernel().finish(ctx, 0u);
    });
    hle.add("scePower", "scePowerSetClockFrequency630", success);
    hle.add("scePower", "scePowerCheckWlanCoexistenceClock", success);

    hle.add("sceRtc", "sceRtcGetCurrentClockLocalTime", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint64_t now = guest_unix_us();
        const auto seconds = static_cast<std::time_t>(now / 1'000'000u);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &seconds);
#else
        localtime_r(&seconds, &tm);
#endif
        auto &memory = rt.memory();
        const std::uint32_t out = arg(ctx, 0);
        memory.store16(out, static_cast<std::uint16_t>(tm.tm_year + 1900));
        memory.store16(out + 2u, static_cast<std::uint16_t>(tm.tm_mon + 1));
        memory.store16(out + 4u, static_cast<std::uint16_t>(tm.tm_mday));
        memory.store16(out + 6u, static_cast<std::uint16_t>(tm.tm_hour));
        memory.store16(out + 8u, static_cast<std::uint16_t>(tm.tm_min));
        memory.store16(out + 10u, static_cast<std::uint16_t>(tm.tm_sec));
        memory.store32(out + 12u, static_cast<std::uint32_t>(now % 1'000'000u));
        kernel().finish(ctx, 0u);
    });
    hle.add("sceImpose", "sceImposeSetLanguageMode", success);
    hle.add("sceOpenPSID", "sceOpenPSIDGetOpenPSID", [](Runtime &rt, AllegrexContext &ctx) {
        for (std::uint32_t i = 0; i < 16u; ++i) rt.memory().store8(arg(ctx, 0) + i, static_cast<std::uint8_t>(0x10u + i));
        kernel().finish(ctx, 0u);
    });
    hle.add("sceUtility", "sceUtilityGetSystemParamInt", [](Runtime &rt, AllegrexContext &ctx) {
        std::uint32_t value = 0u;
        switch (arg(ctx, 0)) {
        case 2u: value = 1u; break;  // ad hoc channel: automatic
        case 4u: value = 0u; break;  // date format YYYYMMDD
        case 5u: value = 0u; break;  // 24-hour clock
        case 8u: value = 0u; break;  // language: Japanese
        case 9u: value = 0u; break;  // confirm button: circle
        default: break;
        }
        rt.memory().store32(arg(ctx, 1), value);
        kernel().finish(ctx, 0u);
    });
    hle.add("sceUtility", "sceUtilityLoadModule", success);
    // The dialogs themselves live in hle_utility.cpp.
    hle.add("sceUtility", "sceUtilityUnloadModule", success);
}

} // namespace

void register_system(HleRegistrar &hle) {
    register_utils(hle);
    register_process(hle);
    register_interrupts(hle);
    register_platform(hle);
}

} // namespace mga
