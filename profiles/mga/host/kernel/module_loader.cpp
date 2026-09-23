#include "kernel/module_loader.hpp"

#include "kernel/call_watch.hpp"
#include "kernel/sound_paths.hpp"

#include "hle/hle_common.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string_view>
#include <utility>

namespace mga {
namespace {

// syslib export NIDs.
constexpr std::uint32_t kModuleStartNid = 0xD632ACDBu;
constexpr std::uint32_t kModuleStopNid = 0xCEE8593Cu;

// Modules are placed on this alignment in user memory.
constexpr std::uint32_t kModuleAlignment = 0x100u;

struct LoaderState {
    std::map<SceUID, std::unique_ptr<LoadedModule>> modules;
    std::set<std::pair<std::string, std::uint32_t>> bound;
    // Exports of loaded modules, which later loads link against.
    std::map<std::pair<std::string, std::uint32_t>, std::uint32_t> exports;
};

LoaderState &state() {
    static LoaderState s;
    return s;
}

bool trace_modules() {
    static const bool enabled = std::getenv("MGA_TRACE_MODULES") != nullptr;
    return enabled;
}

// --- import stubs of loaded modules ---------------------------------------
//
// The runtime dispatches registered addresses to plain function pointers, so
// each stub of a loaded module gets one of a fixed pool of trampolines, and the
// trampoline finds which import it stands for in a table. This mirrors the
// import wrappers psp_recomp generates for the main executable.

constexpr std::size_t kTrampolineCount = 2048u;

struct TrampolineSlot {
    std::string library;
    std::uint32_t nid{};
    std::uint32_t stub{};
};

std::array<TrampolineSlot, kTrampolineCount> &trampoline_slots() {
    static std::array<TrampolineSlot, kTrampolineCount> slots;
    return slots;
}

std::size_t &trampolines_used() {
    static std::size_t used = 0u;
    return used;
}

template <std::size_t I>
void import_trampoline(Runtime &rt, AllegrexContext &ctx) {
    const TrampolineSlot &slot = trampoline_slots()[I];
    const psprecomp::RuntimeExecutionContextToken caller = psprecomp::capture_runtime_execution_context();
    const std::uint32_t import_pc = ctx.pc;
    const std::uint32_t return_address = ctx.gpr[31];
    rt.invoke_import(slot.library, slot.nid, ctx);
    if (!rt.stopped() && psprecomp::runtime_execution_context_matches(caller) && ctx.pc == import_pc)
        ctx.pc = return_address;
}

template <std::size_t... I>
constexpr std::array<Runtime::RecompiledFunction, sizeof...(I)> make_trampolines(std::index_sequence<I...>) {
    return {&import_trampoline<I>...};
}

const std::array<Runtime::RecompiledFunction, kTrampolineCount> &trampolines() {
    static const auto table = make_trampolines(std::make_index_sequence<kTrampolineCount>{});
    return table;
}

// Reuses the slot of a stub address seen before (a stage reloaded at the same
// place), otherwise takes a new one.
std::optional<std::size_t> trampoline_for(std::uint32_t stub, const std::string &library, std::uint32_t nid) {
    auto &slots = trampoline_slots();
    std::size_t &used = trampolines_used();
    for (std::size_t i = 0; i < used; ++i) {
        if (slots[i].stub == stub) {
            slots[i].library = library;
            slots[i].nid = nid;
            return i;
        }
    }
    if (used == kTrampolineCount) return std::nullopt;
    slots[used] = TrampolineSlot{library, nid, stub};
    return used++;
}

// Binds a logging stub for an import nothing serves, as the profile does for
// the main executable's.
void ensure_import_bound(Runtime &runtime, const std::string &library, std::uint32_t nid) {
    auto &s = state();
    const auto key = std::make_pair(library, nid);
    if (s.bound.contains(key) || s.exports.contains(key)) return;
    const std::string name = runtime.nids().resolve(library, nid).value_or(psprecomp::hex32(nid));
    auto calls = std::make_shared<std::uint64_t>(0u);
    const std::string label = library + "::" + name;
    runtime.register_hle(library, nid, [calls, label](Runtime &, AllegrexContext &ctx) {
        if ((*calls)++ == 0u) {
            std::cerr << "[hle-stub] " << label << " a0=" << psprecomp::hex32(ctx.gpr[4])
                      << " a1=" << psprecomp::hex32(ctx.gpr[5]) << " ra=" << psprecomp::hex32(ctx.gpr[31]) << "\n";
        }
        kernel().finish(ctx, 0u);
    });
    s.bound.insert(key);
}

// An export satisfies an import by jumping to it: the import wrapper sees a
// new pc and leaves $ra alone, so the export returns straight to the caller.
void link_export(Runtime &runtime, const ModuleExport &entry, const std::string &module_name) {
    const std::uint32_t target = entry.address;
    static const bool trace_calls = std::getenv("MGA_TRACE_EXPORTS") != nullptr;
    if (trace_calls) {
        const std::string label = entry.library + "::" +
            runtime.nids().resolve(entry.library, entry.nid).value_or(psprecomp::hex32(entry.nid));
        runtime.register_hle(entry.library, entry.nid, [target, label](Runtime &rt, AllegrexContext &ctx) {
            std::cout << "[export] " << label << "(" << psprecomp::hex32(ctx.gpr[4]) << ", "
                      << psprecomp::hex32(ctx.gpr[5]) << ", " << psprecomp::hex32(ctx.gpr[6]) << ", "
                      << psprecomp::hex32(ctx.gpr[7]) << ") ra=" << psprecomp::hex32(ctx.gpr[31]);
            // Show pointer arguments that hold text, such as file names.
            for (unsigned i = 4u; i <= 7u; ++i) {
                if (!rt.memory().contains(ctx.gpr[i], 4u)) continue;
                const std::string text = read_cstring(rt.memory(), ctx.gpr[i], 48u);
                if (text.size() >= 3u && std::all_of(text.begin(), text.end(), [](char c) { return c >= 0x20 && c < 0x7F; }))
                    std::cout << " a" << (i - 4u) << "=\"" << text << "\"";
            }
            std::cout << "\n";
            // MGA_TRACE_EXPORTS=2 also dumps what pointer arguments point at.
            static const bool dump = std::string(std::getenv("MGA_TRACE_EXPORTS")) == "2";
            for (unsigned i = 4u; dump && i <= 7u; ++i) {
                if (!rt.memory().contains(ctx.gpr[i], 32u)) continue;
                std::cout << "         *a" << (i - 4u) << ":";
                for (std::uint32_t w = 0; w < 32u; w += 4u)
                    std::cout << " " << psprecomp::hex32(rt.memory().load32(ctx.gpr[i] + w));
                std::cout << "\n";
            }
            ctx.pc = target;
        });
    } else {
        runtime.register_hle(entry.library, entry.nid, [target](Runtime &, AllegrexContext &ctx) { ctx.pc = target; });
    }
    state().exports[{entry.library, entry.nid}] = target;
    if (trace_modules())
        std::cout << "[module] " << module_name << " exports " << entry.library << "::"
                  << runtime.nids().resolve(entry.library, entry.nid).value_or(psprecomp::hex32(entry.nid))
                  << " at " << psprecomp::hex32(target) << "\n";
}

std::vector<ModuleExport> read_exports(const psprecomp::GuestMemory &memory, const psprecomp::PspModuleInfo &info) {
    std::vector<ModuleExport> exports;
    std::uint32_t cursor = info.ent_top;
    while (cursor < info.ent_end) {
        if (!memory.contains(cursor, 16u)) break;
        const std::uint32_t name_address = memory.load32(cursor);
        const std::uint32_t length_words = memory.load8(cursor + 8u);
        const std::uint32_t variables = memory.load8(cursor + 9u);
        const std::uint32_t functions = memory.load16(cursor + 10u);
        const std::uint32_t table = memory.load32(cursor + 12u);
        const std::string library = name_address != 0u ? read_cstring(memory, name_address, 64u) : std::string();
        const std::uint32_t total = functions + variables;
        for (std::uint32_t i = 0; i < total; ++i) {
            const std::uint32_t nid_address = table + i * 4u;
            const std::uint32_t value_address = table + (total + i) * 4u;
            if (!memory.contains(nid_address, 4u) || !memory.contains(value_address, 4u)) break;
            // Variables are exported data, not code; only functions link.
            if (i >= functions && !library.empty()) continue;
            exports.push_back(ModuleExport{library, memory.load32(nid_address), memory.load32(value_address)});
        }
        cursor += std::max<std::uint32_t>(4u, length_words) * 4u;
    }
    return exports;
}

} // namespace

std::uint32_t module_image_hash(const std::vector<std::uint8_t> &image) noexcept {
    std::uint32_t hash = 2166136261u;
    for (const std::uint8_t byte : image) hash = (hash ^ byte) * 16777619u;
    return hash;
}

void set_bound_imports(std::set<std::pair<std::string, std::uint32_t>> bound) { state().bound = std::move(bound); }

bool module_is_hle(const std::string &module_name) {
    static const bool load_all = std::getenv("MGA_LOAD_ALL_MODULES") != nullptr;
    if (load_all) return false;
    return module_name.size() >= 3u && (module_name.compare(0, 3, "sce") == 0 || module_name.compare(0, 3, "Sce") == 0);
}

std::optional<std::string> elf_module_name(const std::vector<std::uint8_t> &image) {
    if (image.size() < 4u || std::memcmp(image.data(), "\x7F" "ELF", 4u) != 0) return std::nullopt;
    try {
        const psprecomp::Elf32Image elf = psprecomp::Elf32Image::from_bytes(image, "module");
        for (const psprecomp::ElfSection &section : elf.sections()) {
            if (section.name != ".rodata.sceModuleInfo" && section.name != ".sceModuleInfo") continue;
            if (section.offset + 32u > image.size()) return std::nullopt;
            const char *text = reinterpret_cast<const char *>(image.data() + section.offset + 4u);
            return std::string(text, strnlen(text, 28u));
        }
    } catch (const std::exception &) {
    }
    return std::nullopt;
}

const LoadedModule *load_module(Runtime &runtime, std::vector<std::uint8_t> image, const std::string &file_name) {
    if (image.size() < 4u || std::memcmp(image.data(), "\x7F" "ELF", 4u) != 0) return nullptr;
    const std::uint32_t image_hash = module_image_hash(image);
    const psprecomp::Elf32Image elf = psprecomp::Elf32Image::from_bytes(std::move(image), file_name);
    if (!elf.is_psp_prx()) throw psprecomp::Error(file_name + " is not a relocatable PRX");

    // Size of the image when loaded at 0: every PT_LOAD segment laid out.
    std::uint32_t size = 0u;
    for (std::size_t i = 0; i < elf.segments().size(); ++i) {
        const auto &segment = elf.segments()[i];
        if (segment.type != 1u) continue;
        size = std::max(size, elf.segment_runtime_address(i, 0u) + segment.memory_size);
    }
    if (size == 0u) throw psprecomp::Error(file_name + " has nothing to load");

    Kernel &k = kernel();
    const SceUID block = k.allocate_block(file_name, 3u, size, kModuleAlignment);
    if (block < 0) throw psprecomp::Error("No memory to load " + file_name);
    const std::uint32_t base = k.find_block(block)->address;

    psprecomp::GuestMemory &memory = runtime.memory();
    // Uninitialised data starts zeroed, as the module manager leaves it.
    for (std::uint32_t offset = 0; offset < size; offset += 4u) memory.store32(base + offset, 0u);
    const psprecomp::RelocationStats relocations = elf.load_and_relocate(memory, base);
    (void)relocations;

    const auto info = elf.find_module_info(memory, base);
    if (!info) throw psprecomp::Error(file_name + " has no module info");

    auto module = std::make_unique<LoadedModule>();
    module->uid = k.allocate_uid();
    module->name = info->name;
    module->file_name = file_name;
    module->image_hash = image_hash;
    module->base = base;
    module->size = size;
    module->block = block;
    module->gp = info->gp;
    module->exports = read_exports(memory, *info);
    for (const ModuleExport &entry : module->exports) {
        if (!entry.library.empty()) continue;
        if (entry.nid == kModuleStartNid) module->start = entry.address;
        if (entry.nid == kModuleStopNid) module->stop = entry.address;
    }
    if (module->start == 0u) module->start = elf.runtime_entry(base);

    // Its exports first, so its own imports of them (rare) resolve too.
    for (const ModuleExport &entry : module->exports) {
        if (!entry.library.empty()) link_export(runtime, entry, module->name);
    }

    // Its imports: a trampoline at each stub address.
    std::size_t imports = 0u;
    for (const psprecomp::PspImport &import : elf.scan_imports(memory, *info)) {
        ensure_import_bound(runtime, import.library, import.nid);
        const auto slot = trampoline_for(import.stub_address, import.library, import.nid);
        if (!slot) throw psprecomp::Error("Too many imports in loaded modules");
        runtime.register_function(import.stub_address, trampolines()[*slot],
                                  import.library + "::" + psprecomp::hex32(import.nid));
        ++imports;
    }

    // Native code for it, when this build has a corpus generated at this address.
    // MGA_NO_MODULE_CORPORA with no value interprets every loaded module; with
    // one it interprets only the modules it names ("KCEJ_SOUND,ZLIB"), which is
    // how a bug is pinned on one corpus rather than on the timing around it.
    const char *code = "interpreted";
    static const char *no_corpora_names = std::getenv("MGA_NO_MODULE_CORPORA");
    static const bool no_corpora_all = no_corpora_names != nullptr &&
                                       (std::string_view(no_corpora_names).empty() ||
                                        std::string_view(no_corpora_names) == "1" ||
                                        std::string_view(no_corpora_names) == "all");
    const bool no_corpora =
        no_corpora_all || (no_corpora_names != nullptr && std::string_view(no_corpora_names).find(module->name) !=
                                                              std::string_view::npos);
    for (const ModuleCorpus &corpus : module_corpora()) {
        if (module->name != corpus.name || corpus.image_hash != image_hash) continue;
        if (corpus.base != base) {
            std::cerr << "[module] " << module->name << " was recompiled for " << psprecomp::hex32(corpus.base)
                      << " but loaded at " << psprecomp::hex32(base)
                      << "; interpreting it (run scripts/generate_modules.sh again)\n";
            break;
        }
        if (no_corpora) break;
        corpus.install(runtime);
        code = "recompiled";
        break;
    }

    std::cout << "[module] loaded " << module->name << " (" << psprecomp::hex32(image_hash) << ", " << code << ") at "
              << psprecomp::hex32(base)
              << ", " << size / 1024u << " KiB, " << module->exports.size() << " exports, " << imports
              << " imports, gp " << psprecomp::hex32(module->gp) << "\n";

    const LoadedModule *result = module.get();
    state().modules[module->uid] = std::move(module);
    // An MGA_WATCH address inside this module can be attached now.
    install_call_watches(runtime);
    // And the sound module's file names can be kept alive from here on.
    install_sound_path_keeper(runtime);
    return result;
}

LoadedModule *find_module(SceUID uid) {
    auto &modules = state().modules;
    const auto found = modules.find(uid);
    return found != modules.end() ? found->second.get() : nullptr;
}

// MGA_DUMP_MODULES=<dir>: the module's memory once the game has linked it,
// which is what scripts/generate_modules.sh recompiles a stage from.
void dump_module(Runtime &runtime, const LoadedModule &module) {
    const char *dir = std::getenv("MGA_DUMP_MODULES");
    if (dir == nullptr) return;
    std::vector<std::uint8_t> image(module.size);
    runtime.memory().copy_out(module.base, image);
    const std::string path =
        std::string(dir) + "/" + module.name + "_" + psprecomp::hex32(module.image_hash) + ".bin";
    if (std::FILE *out = std::fopen(path.c_str(), "wb")) {
        std::fwrite(image.data(), 1u, image.size(), out);
        std::fclose(out);
        std::cout << "[module] dumped " << path << "\n";
    } else {
        std::cerr << "[module] cannot write " << path << "\n";
    }
}

void dump_loaded_modules(Runtime &runtime) {
    for (const auto &[uid, module] : state().modules) {
        (void)uid;
        dump_module(runtime, *module);
    }
}

void unload_module(Runtime &runtime, SceUID uid) {
    auto &modules = state().modules;
    const auto found = modules.find(uid);
    if (found == modules.end()) return;
    const LoadedModule &module = *found->second;
    if (const char *peek = std::getenv("MGA_PEEK_MODULE"); peek != nullptr) {
        const std::uint32_t offset = static_cast<std::uint32_t>(std::strtoul(peek, nullptr, 0));
        std::cout << "[module] word at +" << psprecomp::hex32(offset) << " = "
                  << psprecomp::hex32(runtime.memory().load32(module.base + offset)) << " (at unload)\n";
    }
    dump_module(runtime, module);
    runtime.unregister_functions(module.base, module.base + module.size);
    (void)kernel().free_block(module.block);
    std::cout << "[module] unloaded " << module.name << " from " << psprecomp::hex32(module.base) << "\n";
    modules.erase(found);
}

} // namespace mga
