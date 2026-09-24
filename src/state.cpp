#include "psprecomp/state.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/guest_memory.hpp"

namespace psprecomp {
namespace {

// Each region is written as base, length, then the block. The base and length
// go in so that a state from a build with a different memory size is refused
// here, with a clear reason, rather than further in where the symptom would be
// a guest that restores into nonsense.
struct Region {
    std::uint32_t base;
    std::size_t size;
};

[[nodiscard]] Region ram_region(const GuestMemory &memory) {
    return {GuestMemory::kPhysicalBase, memory.size()};
}
[[nodiscard]] Region vram_region(const GuestMemory &memory) {
    return {GuestMemory::kVramPhysicalBase, memory.vram_size()};
}
[[nodiscard]] Region scratchpad_region(const GuestMemory &) {
    return {GuestMemory::kScratchpadBase, GuestMemory::kScratchpadSize};
}

void write_region(SnapshotWriter &out, const GuestMemory &memory, const Region &region) {
    out.u32(region.base);
    out.u64(static_cast<std::uint64_t>(region.size));
    const std::uint8_t *bytes = memory.raw_pointer(region.base, region.size);
    if (bytes == nullptr) {
        // Not reachable for the three real regions, but a state that silently
        // dropped one would restore a guest with a hole in it.
        out.u64(0u);
        return;
    }
    out.memory(bytes, region.size);
}

[[nodiscard]] bool read_region(SnapshotReader &in, GuestMemory &memory, const Region &region) {
    const std::uint32_t base = in.u32();
    const std::uint64_t size = in.u64();
    if (!in.ok() || base != region.base || size != static_cast<std::uint64_t>(region.size)) {
        in.fail();
        return false;
    }
    std::uint8_t *bytes = memory.raw_pointer(region.base, region.size);
    if (bytes == nullptr) {
        in.fail();
        return false;
    }
    return in.memory(bytes, region.size);
}

} // namespace

void write_state_header(SnapshotWriter &out, std::uint32_t profile_tag, std::uint32_t profile_version) {
    out.u32(kStateMagic);
    out.u32(kStateVersion);
    out.u32(profile_tag);
    out.u32(profile_version);
}

bool read_state_header(SnapshotReader &in, std::uint32_t profile_tag, std::uint32_t profile_version) {
    const std::uint32_t magic = in.u32();
    const std::uint32_t version = in.u32();
    const std::uint32_t tag = in.u32();
    const std::uint32_t profile = in.u32();
    if (!in.ok() || magic != kStateMagic || version != kStateVersion || tag != profile_tag ||
        profile != profile_version) {
        in.fail();
        return false;
    }
    return true;
}

void write_context(SnapshotWriter &out, const AllegrexContext &context) {
    for (const std::uint32_t value : context.gpr) out.u32(value);
    out.u32(context.hi);
    out.u32(context.lo);
    out.u32(context.pc);
    // Float registers as their bit patterns: a NaN the guest put there has to
    // come back as the same NaN, and a decimal round trip would not promise
    // that.
    for (std::uint32_t i = 0; i < context.fpr.size(); ++i) out.u32(context.fpr_bits(i));
    out.u32(context.fcr31);
    for (const float value : context.vfpu) out.f32(value);
    for (const std::uint32_t value : context.vfpu_ctrl) out.u32(value);
}

bool read_context(SnapshotReader &in, AllegrexContext &context) {
    for (std::uint32_t &value : context.gpr) value = in.u32();
    context.hi = in.u32();
    context.lo = in.u32();
    context.pc = in.u32();
    for (std::uint32_t i = 0; i < context.fpr.size(); ++i) context.set_fpr_bits(i, in.u32());
    context.fcr31 = in.u32();
    for (float &value : context.vfpu) value = in.f32();
    for (std::uint32_t &value : context.vfpu_ctrl) value = in.u32();
    // $zero is $zero whatever a file says it is.
    context.gpr[0] = 0u;
    return in.ok();
}

void write_machine(SnapshotWriter &out, const GuestMemory &memory, const AllegrexContext &context) {
    // A rough guess at the final size, so the vector does not grow a dozen
    // times while a mostly-empty 24 MiB is encoded.
    out.reserve(out.size() + 2u * 1024u * 1024u);
    write_region(out, memory, ram_region(memory));
    write_region(out, memory, vram_region(memory));
    write_region(out, memory, scratchpad_region(memory));
    write_context(out, context);
}

bool read_machine(SnapshotReader &in, GuestMemory &memory, AllegrexContext &context) {
    if (!read_region(in, memory, ram_region(memory))) return false;
    if (!read_region(in, memory, vram_region(memory))) return false;
    if (!read_region(in, memory, scratchpad_region(memory))) return false;
    return read_context(in, context);
}

} // namespace psprecomp
