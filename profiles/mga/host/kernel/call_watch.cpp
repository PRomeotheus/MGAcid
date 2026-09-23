// MGA_WATCH: log the arguments of chosen guest functions.
//
// The game's own debug prints say what it meant to do; this says what it
// actually asked for. A watch wraps the recompiled entry registered at a guest
// address, prints $a0-$a3 and what pointer arguments point at, then runs the
// real code, so it costs nothing but a line of output per call.
//
//   MGA_WATCH=0886F5CC:4,08870678:12f,0881C77C:40c
//
// ":4" dumps four bytes of each pointer argument as signed bytes; ":12f" dumps
// twelve as three floats, at full precision, because an off-by-one in a
// float-to-cell conversion is a value that prints as "-16" and is not -16;
// ":40c" reads ten words as pointers to cells and prints the (x,y,z) each one
// names, which is how a path reads as a route rather than as ten addresses.
// Where the function returns to its caller in one go, its result and whatever
// it wrote through those pointers are printed too.
//
// A watch fires only where the runtime dispatches: a call from another
// generated unit, from a loaded module, or from the interpreter. A call inside
// the same unit is a local jump and is not seen, so watch the function a stage
// or another unit calls rather than its helpers.
#include "kernel/call_watch.hpp"

#include "psprecomp/common.hpp"

#include <array>
#include <bit>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace mga {
namespace {

struct Watch {
    std::uint32_t address{};
    std::uint32_t bytes{};  // of each pointer argument to dump, 0 for none
    bool as_floats{};       // "12f": words as floats rather than signed bytes
    bool as_cells{};        // "32c": words as pointers to a cell's x, y, z bytes
    bool as_strings{};      // "8s": words as pointers to NUL-terminated strings
    std::string name;
    Runtime::RecompiledFunction original{};
    std::uint64_t calls{};
};

// A fixed pool, because each watch needs its own function pointer.
constexpr std::size_t kMaxWatches = 16u;

std::array<Watch, kMaxWatches> &watches() {
    static std::array<Watch, kMaxWatches> table;
    return table;
}

std::size_t &watch_count() {
    static std::size_t count = 0u;
    return count;
}

// Addresses MGA_WATCH names that nothing is registered at yet. A stage's
// functions exist only once the game has loaded it, so the module loader asks
// again after every load and the watch attaches then.
std::vector<std::string> &pending_watches() {
    static std::vector<std::string> pending;
    return pending;
}

// The bytes behind the four argument registers, as the watch asked for them.
void dump_arguments(Runtime &rt, const Watch &watch, const std::array<std::uint32_t, 4> &arguments,
                    const char *prefix) {
    for (unsigned i = 0; watch.bytes != 0u && i < 4u; ++i) {
        const std::uint32_t address = arguments[i];
        if (!rt.memory().contains(address, watch.bytes)) continue;
        std::cout << "\n         " << prefix << "a" << i << " " << psprecomp::hex32(address) << ":";
        if (watch.as_strings) {
            // An array of char *: a queued file name is a path, and a path
            // that is not the one the game passed is the whole bug.
            for (std::uint32_t offset = 0; offset + 4u <= watch.bytes; offset += 4u) {
                const std::uint32_t text = rt.memory().load32(address + offset);
                if (!rt.memory().contains(text, 1u)) {
                    std::cout << " " << psprecomp::hex32(text);
                    continue;
                }
                std::string value;
                for (std::uint32_t i = 0; i < 64u && rt.memory().contains(text + i, 1u); ++i) {
                    const char c = static_cast<char>(rt.memory().load8(text + i));
                    if (c == '\0') break;
                    value += (c >= 0x20 && c < 0x7F) ? c : '.';
                }
                std::cout << " " << psprecomp::hex32(text) << "=\"" << value << "\"";
            }
        } else if (watch.as_cells) {
            // An array of node pointers: what matters is the cell each names,
            // which is the first three signed bytes of the node.
            for (std::uint32_t offset = 0; offset + 4u <= watch.bytes; offset += 4u) {
                const std::uint32_t node = rt.memory().load32(address + offset);
                if (!rt.memory().contains(node, 3u)) {
                    std::cout << " " << psprecomp::hex32(node);
                    continue;
                }
                std::cout << " (";
                for (std::uint32_t axis = 0; axis < 3u; ++axis)
                    std::cout << (axis != 0u ? "," : "")
                              << static_cast<int>(static_cast<std::int8_t>(rt.memory().load8(node + axis)));
                std::cout << ")";
            }
        } else if (watch.as_floats) {
            const std::streamsize previous = std::cout.precision(9);
            for (std::uint32_t offset = 0; offset + 4u <= watch.bytes; offset += 4u)
                std::cout << " " << std::bit_cast<float>(rt.memory().load32(address + offset));
            std::cout.precision(previous);
        } else {
            // Signed, because the coordinates this exists for are signed bytes.
            for (std::uint32_t offset = 0; offset < watch.bytes; ++offset)
                std::cout << " " << static_cast<int>(static_cast<std::int8_t>(rt.memory().load8(address + offset)));
        }
    }
}

void report(Runtime &rt, Watch &watch, const AllegrexContext &ctx) {
    std::cout << "[watch] " << watch.name << "(" << psprecomp::hex32(ctx.gpr[4]) << ", "
              << psprecomp::hex32(ctx.gpr[5]) << ", " << psprecomp::hex32(ctx.gpr[6]) << ", "
              << psprecomp::hex32(ctx.gpr[7]) << ") ra=" << psprecomp::hex32(ctx.gpr[31])
              << " #" << ++watch.calls;
    dump_arguments(rt, watch, {ctx.gpr[4], ctx.gpr[5], ctx.gpr[6], ctx.gpr[7]}, "*");
    std::cout << std::endl;
}

template <std::size_t I>
void watch_trampoline(Runtime &rt, AllegrexContext &ctx) {
    Watch &watch = watches()[I];
    const std::uint32_t return_address = ctx.gpr[31];
    const std::array<std::uint32_t, 4> arguments{ctx.gpr[4], ctx.gpr[5], ctx.gpr[6], ctx.gpr[7]};
    report(rt, watch, ctx);
    if (watch.original == nullptr) return;
    watch.original(rt, ctx);
    // The unit usually runs the whole function and comes back at its caller,
    // in which case $v0 is the result and anything written through a pointer
    // argument is there to see. When it left part way through -- into another
    // unit, or a thread switch -- there is nothing to report yet.
    if (ctx.pc != return_address) return;
    std::cout << "[watch] " << watch.name << " -> " << psprecomp::hex32(ctx.gpr[2]) << " ("
              << static_cast<std::int32_t>(ctx.gpr[2]) << ")";
    dump_arguments(rt, watch, arguments, "out *");
    std::cout << std::endl;
}

template <std::size_t... I>
constexpr std::array<Runtime::RecompiledFunction, sizeof...(I)> make_trampolines(std::index_sequence<I...>) {
    return {&watch_trampoline<I>...};
}

const std::array<Runtime::RecompiledFunction, kMaxWatches> &trampolines() {
    static const auto table = make_trampolines(std::make_index_sequence<kMaxWatches>{});
    return table;
}

// Attaches one MGA_WATCH entry, or keeps it for the next module load.
void bind_watch(Runtime &runtime, const std::string &entry) {
    std::string item = entry;
    std::uint32_t bytes = 0u;
    bool as_floats = false;
    bool as_cells = false;
    bool as_strings = false;
    if (const std::size_t colon = item.find(':'); colon != std::string::npos) {
        bytes = static_cast<std::uint32_t>(std::strtoul(item.c_str() + colon + 1u, nullptr, 0));
        as_floats = item.back() == 'f';
        as_cells = item.back() == 'c';
        as_strings = item.back() == 's';
        item.resize(colon);
    }
    const std::uint32_t address = static_cast<std::uint32_t>(std::strtoul(item.c_str(), nullptr, 16));
    std::size_t &count = watch_count();
    if (count == kMaxWatches) {
        std::cerr << "[watch] no room for " << item << "\n";
        return;
    }
    const Runtime::RecompiledFunction original = runtime.registered_function(address);
    if (original == nullptr) {
        // The address is wrong, it sits inside a unit rather than at a
        // dispatchable entry, or its module is not loaded yet.
        pending_watches().push_back(entry);
        return;
    }
    Watch &watch = watches()[count];
    watch.address = address;
    watch.bytes = bytes;
    watch.as_floats = as_floats;
    watch.as_cells = as_cells;
    watch.as_strings = as_strings;
    watch.name = psprecomp::hex32(address);
    watch.original = original;
    runtime.register_function(address, trampolines()[count], "mga_watch_" + watch.name);
    std::cout << "[watch] watching " << psprecomp::hex32(address);
    if (bytes != 0u)
        std::cout << ", dumping " << bytes
                  << (as_floats ? " bytes as floats"
                                : (as_cells ? " bytes as cell pointers"
                                            : (as_strings ? " bytes as string pointers" : " bytes")))
                  << " of each pointer argument";
    std::cout << "\n";
    ++count;
}

} // namespace

void install_call_watches(Runtime &runtime) {
    static bool parsed = false;
    if (parsed) {
        // A stage that was unloaded took its registrations with it, and
        // reloading re-registered the real code over the trampoline. Put the
        // watch back, and pick up any address that only now exists.
        for (std::size_t i = 0; i < watch_count(); ++i) {
            Watch &watch = watches()[i];
            const Runtime::RecompiledFunction current = runtime.registered_function(watch.address);
            if (current == nullptr || current == trampolines()[i]) continue;
            watch.original = current;
            runtime.register_function(watch.address, trampolines()[i], "mga_watch_" + watch.name);
        }
        std::vector<std::string> retry;
        retry.swap(pending_watches());
        for (const std::string &entry : retry) bind_watch(runtime, entry);
        return;
    }
    parsed = true;
    const char *spec = std::getenv("MGA_WATCH");
    if (spec == nullptr) return;
    const std::string list = spec;
    std::size_t start = 0u;
    while (start < list.size()) {
        const std::size_t end = std::min(list.find(',', start), list.size());
        const std::string entry = list.substr(start, end - start);
        start = end + 1u;
        if (!entry.empty()) bind_watch(runtime, entry);
    }
}

} // namespace mga
