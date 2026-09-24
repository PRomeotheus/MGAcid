#include "psprecomp/snapshot.hpp"

#include <algorithm>

namespace psprecomp {
namespace {

// A block is a length, then a sequence of runs. Each run is a count of zero
// bytes followed by a count of literal bytes and the bytes themselves, so a
// block of nothing but zeros costs a handful of bytes and a block of noise costs
// its own size plus a little.
//
// Counts are varints, because the interesting sizes span everything from a few
// bytes to a whole 24 MiB of untouched memory and a fixed width would either
// waste space or fail on the large end.
void write_varint(SnapshotWriter &out, std::uint64_t value) {
    while (value >= 0x80u) {
        out.u8(static_cast<std::uint8_t>(value & 0x7Fu) | 0x80u);
        value >>= 7u;
    }
    out.u8(static_cast<std::uint8_t>(value));
}

[[nodiscard]] std::uint64_t read_varint(SnapshotReader &in) {
    std::uint64_t value = 0u;
    for (unsigned shift = 0; shift < 64u; shift += 7u) {
        const std::uint8_t byte = in.u8();
        value |= static_cast<std::uint64_t>(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0u) return value;
        if (!in.ok()) break;
    }
    // More continuation bytes than a 64-bit value can hold: the stream is not
    // what it claims to be.
    in.fail();
    return 0u;
}

} // namespace

void SnapshotWriter::memory(const void *data, std::size_t size) {
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    u64(static_cast<std::uint64_t>(size));
    std::size_t at = 0u;
    while (at < size) {
        std::size_t zeros = 0u;
        while (at + zeros < size && bytes[at + zeros] == 0u) ++zeros;
        at += zeros;
        std::size_t literals = 0u;
        while (at + literals < size && bytes[at + literals] != 0u) ++literals;
        write_varint(*this, zeros);
        write_varint(*this, literals);
        if (literals != 0u) raw(bytes + at, literals);
        at += literals;
    }
    // A trailing pair of zeros ends the block, so a reader knows it has the
    // whole thing rather than inferring it from the byte count.
    write_varint(*this, 0u);
    write_varint(*this, 0u);
}

bool SnapshotReader::memory(void *out, std::size_t size) {
    const std::uint64_t stored = u64();
    if (!ok_ || stored != static_cast<std::uint64_t>(size)) {
        ok_ = false;
        return false;
    }
    auto *bytes = static_cast<std::uint8_t *>(out);
    std::fill(bytes, bytes + size, static_cast<std::uint8_t>(0));
    std::size_t at = 0u;
    for (;;) {
        const std::uint64_t zeros = read_varint(*this);
        const std::uint64_t literals = read_varint(*this);
        if (!ok_) return false;
        if (zeros == 0u && literals == 0u) return at == size;
        // Both counts have to fit what is left, or the block describes more
        // memory than the caller has.
        if (zeros > size - at) {
            ok_ = false;
            return false;
        }
        at += static_cast<std::size_t>(zeros);
        if (literals > size - at) {
            ok_ = false;
            return false;
        }
        if (literals != 0u && !raw(bytes + at, static_cast<std::size_t>(literals))) return false;
        at += static_cast<std::size_t>(literals);
    }
}

} // namespace psprecomp
