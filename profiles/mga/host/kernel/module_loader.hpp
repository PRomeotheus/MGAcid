#pragma once

// Loads the game's own relocatable PRX modules into guest memory, the way the
// PSP's module manager does, so their code can run: through a recompiled
// corpus when one exists for the address it lands at, and through the
// runtime's interpreter otherwise.
//
// Metal Gear Ac!d ships its libraries (kjfs, sound, zlibdec, ...) and every
// stage as plain ELF PRX files and loads them with sceKernelLoadModuleByID.
// Sony's own libraries on the disc (names starting with "sce") keep the HLE
// path: their exports are served by the host, so their images are not loaded.
//
// Linking follows the hardware: a loaded module's exports satisfy the imports
// of every module, including ones loaded earlier, and its own imports resolve
// to the host HLE or to exports of modules already loaded.

#include "kernel/kernel.hpp"

#include "psprecomp/elf32.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mga {

struct ModuleExport {
    std::string library;  // empty for the "syslib" entry (module_start, ...)
    std::uint32_t nid{};
    std::uint32_t address{};
};

struct LoadedModule {
    SceUID uid{};
    std::string name;       // from the module info, e.g. "KCEJ_FS"
    std::string file_name;  // for logs
    std::uint32_t image_hash{};  // FNV-1a of the file, which identifies stages that share a name
    std::uint32_t base{};
    std::uint32_t size{};
    SceUID block{};
    std::uint32_t gp{};
    std::uint32_t start{};  // module_start, 0 when there is none
    std::uint32_t stop{};   // module_stop, 0 when there is none
    std::vector<ModuleExport> exports;
    bool started{};
};

// Which imports the host serves. The profile fills it once after registering
// the HLE modules; the loader adds logging stubs for anything still missing.
void set_bound_imports(std::set<std::pair<std::string, std::uint32_t>> bound);

// True when an image with this module name is left to the HLE instead of being
// loaded (Sony's libraries). MGA_LOAD_ALL_MODULES=1 loads every plain ELF.
[[nodiscard]] bool module_is_hle(const std::string &module_name);

// Loads a plain ELF PRX image into newly allocated user memory, relocates it
// and links it. Returns the module, or nothing when the image is not a plain
// ELF (the caller then treats it as a stock module).
[[nodiscard]] const LoadedModule *load_module(Runtime &runtime, std::vector<std::uint8_t> image,
                                              const std::string &file_name);

[[nodiscard]] LoadedModule *find_module(SceUID uid);

// A recompiled corpus of a module, valid only at the address it was generated
// for. The table is generated at configure time (module_corpora.cpp.in).
struct ModuleCorpus {
    const char *name;
    std::uint32_t base;
    std::uint32_t image_hash;  // FNV-1a of the PRX file (tools/module_hash.py)
    void (*install)(Runtime &runtime);
};
[[nodiscard]] std::uint32_t module_image_hash(const std::vector<std::uint8_t> &image) noexcept;
[[nodiscard]] const std::vector<ModuleCorpus> &module_corpora();

// Writes a loaded module's memory to MGA_DUMP_MODULES, if that is set.
void dump_module(Runtime &runtime, const LoadedModule &module);
// The same for every module still loaded; called when the game stops.
void dump_loaded_modules(Runtime &runtime);

// Removes a module's code registrations and frees its memory.
void unload_module(Runtime &runtime, SceUID uid);

// Reads the module name from an ELF image's module info without loading it.
[[nodiscard]] std::optional<std::string> elf_module_name(const std::vector<std::uint8_t> &image);

} // namespace mga
