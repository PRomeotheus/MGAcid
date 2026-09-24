#include "kernel/module_sweep.hpp"

#include "hle/hle_common.hpp"
#include "kernel/kernel.hpp"
#include "kernel/module_loader.hpp"
#include "psprecomp/common.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace mga {
namespace {

constexpr const char *kStageDirectory = "PSP_GAME/USRDIR/stage";

struct Sweep {
    bool started{};
    std::vector<std::string> remaining;  // paths on the disc, still to do
    std::size_t done{};
    std::size_t failed{};
};

Sweep &sweep() {
    static Sweep state;
    return state;
}

// Every <stage>/<stage>.prx under the stage directory. The game names each
// stage's module after its folder, so the listing is enough; a folder without
// one is a stage whose data the executable draws itself.
std::vector<std::string> stage_modules() {
    std::vector<std::string> paths;
    for (const std::string &stage : list_disc_directory(kStageDirectory)) {
        std::string name = stage;
        // Directory entries can carry the ISO9660 ";1" version suffix.
        if (const std::size_t semicolon = name.find(';'); semicolon != std::string::npos) name.resize(semicolon);
        if (name.empty() || name == "." || name == "..") continue;
        const std::string path = std::string(kStageDirectory) + "/" + name + "/" + name + ".prx";
        if (!read_disc_file(path).empty()) paths.push_back(path);
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

void finish(psprecomp::Runtime &runtime) {
    std::cout << "[sweep] done: " << sweep().done << " stages dumped, " << sweep().failed << " could not be\n"
              << "[sweep] now run scripts/generate_modules.sh, then reconfigure and rebuild\n";
    runtime.stop("module sweep complete");
}

void next(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx);

// Dumps and unloads the module just started, then moves on.
void after_start(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx, SceUID uid) {
    if (LoadedModule *module = find_module(uid); module != nullptr) {
        dump_module(runtime, *module);
        ++sweep().done;
    }
    unload_module(runtime, uid);
    next(runtime, ctx);
}

void next(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    Sweep &state = sweep();
    while (!state.remaining.empty()) {
        const std::string path = state.remaining.back();
        state.remaining.pop_back();
        std::cout << "[sweep] " << path << " (" << state.remaining.size() << " to go)\n";

        std::vector<std::uint8_t> image = read_disc_file(path);
        if (image.empty()) {
            std::cerr << "[sweep] cannot read " << path << "\n";
            ++state.failed;
            continue;
        }
        const LoadedModule *loaded = nullptr;
        try {
            loaded = load_module(runtime, std::move(image), path);
        } catch (const std::exception &error) {
            std::cerr << "[sweep] cannot load " << path << ": " << error.what() << "\n";
            ++state.failed;
            continue;
        }
        if (loaded == nullptr) {
            ++state.failed;
            continue;
        }
        const SceUID uid = loaded->uid;
        if (loaded->start == 0u) {
            // Nothing to run: what was loaded is already all there is. The
            // link happens in module_start, so such a module needs no dump --
            // but dump it anyway rather than decide that here.
            after_start(runtime, ctx, uid);
            return;
        }
        ctx.set_gpr(28, loaded->gp);
        kernel().call_guest(ctx, loaded->start, {0u, 0u, 0u, 0u},
                            [uid](psprecomp::AllegrexContext &c, std::uint32_t) {
                                after_start(kernel().runtime(), c, uid);
                            });
        return;
    }
    finish(runtime);
}

} // namespace

bool sweeping_modules() {
    static const bool enabled = [] {
        const char *dir = std::getenv("MGA_SWEEP_MODULES");
        return dir != nullptr && *dir != '\0';
    }();
    return enabled;
}

bool sweep_take_over(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx, LoadedModule &module) {
    if (!sweeping_modules() || sweep().started) return false;
    // Only a stage proves the environment is ready: the libraries load long
    // before the executable has anything for a stage to link against.
    if (module.name != "mgp_stage") return false;

    Sweep &state = sweep();
    state.started = true;
    state.remaining = stage_modules();
    std::cout << "[sweep] taking over at the game's first stage; " << state.remaining.size()
              << " stage modules on the disc\n";
    if (std::getenv("MGA_DUMP_MODULES") == nullptr)
        std::cerr << "[sweep] MGA_DUMP_MODULES is not set, so nothing will be written\n";

    // The module the game was about to start is one of them, and it is already
    // loaded at the address the rest will use. Start it, dump it and unload it
    // first, so the sweep begins from an empty slot.
    const SceUID uid = module.uid;
    module.started = true;
    if (module.start == 0u) {
        after_start(runtime, ctx, uid);
        return true;
    }
    ctx.set_gpr(28, module.gp);
    kernel().call_guest(ctx, module.start, {0u, 0u, 0u, 0u},
                        [uid](psprecomp::AllegrexContext &c, std::uint32_t) {
                            after_start(kernel().runtime(), c, uid);
                        });
    return true;
}

} // namespace mga
