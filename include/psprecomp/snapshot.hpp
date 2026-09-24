#pragma once

// The byte stream a save state is written to and read back from.
//
// Nothing here knows what a PSP is. It is a small, strict reader and writer, so
// that the code which does know -- the runtime, the kernel, a profile's host
// layer -- can describe its own state without each part inventing its own
// format or its own way of coping with a file that is truncated, corrupt or
// from a different build.
//
// Two rules make it safe to use:
//
//   * A reader never throws and never reads past the end. The first overrun
//     sets a flag that stays set, and every read after it returns zero. So a
//     restore can be written as straight-line code and checked once at the end,
//     instead of testing after every field.
//   * Every value is written little-endian and every size is explicit, so a
//     state written on one machine reads on another.
//
// A state also carries a version. Restoring refuses a version it does not know
// rather than reading a newer layout as if it were the old one, which is the
// failure that corrupts a save rather than rejecting it.

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace psprecomp {

class SnapshotWriter {
public:
    void u8(std::uint8_t value) { bytes_.push_back(value); }
    void u16(std::uint16_t value) {
        for (int shift = 0; shift < 16; shift += 8) bytes_.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void u32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) bytes_.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void u64(std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) bytes_.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void i32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }
    void boolean(bool value) { u8(value ? 1u : 0u); }
    // Bit patterns, not decimal text: a float has to come back exactly.
    void f32(float value) {
        std::uint32_t bits = 0u;
        std::memcpy(&bits, &value, sizeof(bits));
        u32(bits);
    }

    void raw(const void *data, std::size_t size) {
        const auto *first = static_cast<const std::uint8_t *>(data);
        bytes_.insert(bytes_.end(), first, first + size);
    }
    // Length-prefixed, so a string containing anything at all reads back.
    void text(const std::string &value) {
        u32(static_cast<std::uint32_t>(value.size()));
        raw(value.data(), value.size());
    }

    // A block of guest memory, with runs of zero bytes counted rather than
    // stored. Most of a PSP's memory is untouched at any moment, and a state
    // that wrote it all out would be twenty-odd megabytes of mostly nothing.
    void memory(const void *data, std::size_t size);

    [[nodiscard]] const std::vector<std::uint8_t> &data() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    void reserve(std::size_t size) { bytes_.reserve(size); }

private:
    std::vector<std::uint8_t> bytes_;
};

class SnapshotReader {
public:
    SnapshotReader(const std::uint8_t *data, std::size_t size) : data_(data), size_(size) {}
    explicit SnapshotReader(const std::vector<std::uint8_t> &bytes) : data_(bytes.data()), size_(bytes.size()) {}

    [[nodiscard]] std::uint8_t u8() {
        if (!take(1u)) return 0u;
        return data_[at_ - 1u];
    }
    [[nodiscard]] std::uint16_t u16() {
        const std::size_t start = at_;
        if (!take(2u)) return 0u;
        return static_cast<std::uint16_t>(data_[start] | data_[start + 1u] << 8);
    }
    [[nodiscard]] std::uint32_t u32() {
        const std::size_t start = at_;
        if (!take(4u)) return 0u;
        std::uint32_t value = 0u;
        for (int i = 3; i >= 0; --i) value = value << 8 | data_[start + static_cast<std::size_t>(i)];
        return value;
    }
    [[nodiscard]] std::uint64_t u64() {
        const std::size_t start = at_;
        if (!take(8u)) return 0u;
        std::uint64_t value = 0u;
        for (int i = 7; i >= 0; --i) value = value << 8 | data_[start + static_cast<std::size_t>(i)];
        return value;
    }
    [[nodiscard]] std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    [[nodiscard]] bool boolean() { return u8() != 0u; }
    [[nodiscard]] float f32() {
        const std::uint32_t bits = u32();
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    // Fails rather than partly filling, so a caller cannot act on half a
    // structure believing it read all of one.
    bool raw(void *out, std::size_t size) {
        const std::size_t start = at_;
        if (!take(size)) return false;
        std::memcpy(out, data_ + start, size);
        return true;
    }
    [[nodiscard]] std::string text() {
        const std::uint32_t length = u32();
        const std::size_t start = at_;
        if (!take(length)) return {};
        return std::string(reinterpret_cast<const char *>(data_ + start), length);
    }

    // The counterpart of SnapshotWriter::memory. Refuses a block whose stored
    // length does not match what the caller expects, which is what catches a
    // state from a build with a different memory layout.
    bool memory(void *out, std::size_t size);

    // False once anything has been read past the end. Sticky: once a stream has
    // overrun, nothing read from it afterwards means anything.
    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return ok_ ? size_ - at_ : 0u; }
    // Marks the stream bad by hand, for a caller that has found the contents
    // inconsistent even though every read fitted.
    void fail() noexcept { ok_ = false; }

private:
    bool take(std::size_t size) {
        if (!ok_ || size > size_ - at_) {
            ok_ = false;
            return false;
        }
        at_ += size;
        return true;
    }

    const std::uint8_t *data_{};
    std::size_t size_{};
    std::size_t at_{};
    bool ok_{true};
};

// A trivially copyable structure, byte for byte. Only for types whose layout
// this build controls -- a context, a small POD -- never for anything with a
// pointer in it, which is why the constraint is enforced rather than trusted.
template <typename T>
void write_pod(SnapshotWriter &out, const T &value) {
    static_assert(std::is_trivially_copyable_v<T>, "a snapshot can only hold a trivially copyable structure");
    out.raw(&value, sizeof(T));
}

template <typename T>
[[nodiscard]] bool read_pod(SnapshotReader &in, T &value) {
    static_assert(std::is_trivially_copyable_v<T>, "a snapshot can only hold a trivially copyable structure");
    return in.raw(&value, sizeof(T));
}

} // namespace psprecomp
