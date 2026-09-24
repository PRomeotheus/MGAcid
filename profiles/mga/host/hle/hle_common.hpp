#pragma once

#include "kernel/kernel.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace mga {

using HleFunction = std::function<void(Runtime &, AllegrexContext &)>;

// PSP EABI argument registers: a0-a3 then t0-t3.
[[nodiscard]] inline std::uint32_t arg(const AllegrexContext &ctx, unsigned index) noexcept {
    return index < 4u ? ctx.gpr[4u + index] : ctx.gpr[8u + (index - 4u)];
}

// 64-bit arguments occupy an even-aligned register pair, low word first.
[[nodiscard]] inline std::uint64_t arg64(const AllegrexContext &ctx, unsigned low_index) noexcept {
    return static_cast<std::uint64_t>(arg(ctx, low_index)) |
           (static_cast<std::uint64_t>(arg(ctx, low_index + 1u)) << 32u);
}

[[nodiscard]] std::string read_cstring(const psprecomp::GuestMemory &memory, std::uint32_t address,
                                       std::size_t max_length = 512u);
void write_cstring(psprecomp::GuestMemory &memory, std::uint32_t address, std::string_view text,
                   std::size_t capacity);
void store64(psprecomp::GuestMemory &memory, std::uint32_t address, std::uint64_t value);

// Registers HLE handlers by function name, resolving NIDs from the runtime's
// NID registry, and remembers what was bound so the profile can stub the rest.
class HleRegistrar {
public:
    explicit HleRegistrar(Runtime &runtime);

    void add(std::string_view library, std::string_view name, HleFunction function);
    // Binds `function` if the name is known; returns false otherwise.
    bool try_add(std::string_view library, std::string_view name, HleFunction function);
    [[nodiscard]] bool bound(const std::string &library, std::uint32_t nid) const;
    [[nodiscard]] std::size_t count() const noexcept { return bound_.size(); }
    [[nodiscard]] const std::set<std::pair<std::string, std::uint32_t>> &bound_imports() const noexcept {
        return bound_;
    }

private:
    Runtime &runtime_;
    std::map<std::pair<std::string, std::string>, std::uint32_t> nids_by_name_;
    std::set<std::pair<std::string, std::uint32_t>> bound_;
};

// Reads from an open guest file descriptor without moving its position.
// Returns the number of bytes read; 0 when the descriptor is unknown.
std::size_t read_open_file(std::uint32_t fd, std::uint64_t offset, std::uint8_t *output, std::size_t size);

// True when MGA_TRACE_SYNC is set: logs kernel object activity.
[[nodiscard]] bool trace_sync();
void log_sync(const std::string &message);

// Prints a message the first time `key` is seen.
void log_once(const std::string &key, const std::string &message);

void register_threadman(HleRegistrar &hle);
void register_sysmem(HleRegistrar &hle);
void register_io(HleRegistrar &hle, const std::filesystem::path &disc_image, const std::filesystem::path &memory_stick);
void register_system(HleRegistrar &hle);
void register_media(HleRegistrar &hle);
void register_atrac(HleRegistrar &hle);
void register_mpeg(HleRegistrar &hle);
void register_font(HleRegistrar &hle);
void register_utility(HleRegistrar &hle, const std::filesystem::path &memory_stick);
void register_savedata(HleRegistrar &hle, const std::filesystem::path &memory_stick);
void register_adhoc(HleRegistrar &hle);
// Calls only Metal Gear Ac!d needs (hle_mga.cpp).
void register_mga(HleRegistrar &hle);

// Save states ---------------------------------------------------------------
// Each part of the HLE layer that holds state the game can observe writes and
// reads its own section, because only the translation unit that owns a
// structure can be relied on to keep its serialisation in step with it.
//
// Why a state cannot be taken, or empty when it can. The HLE layer's answer is
// separate from the kernel's: this is about work in flight that lives on the
// host rather than in the guest -- a video being decoded, a display list part
// way through.
[[nodiscard]] std::string why_no_media_state();
void write_media_state(psprecomp::SnapshotWriter &out);
[[nodiscard]] bool read_media_state(psprecomp::SnapshotReader &in);

// Open files. A host file comes back by being opened again and seeked to where
// it was, a file on the disc image by its offset, so these do restore -- which
// matters because the game keeps the disc open the whole time it is running,
// and refusing a save while any file was open would refuse every save.
[[nodiscard]] std::string why_no_io_state();
// The disc, for host code that needs a file the game has not asked for.
// Empty when there is no disc or no such file.
[[nodiscard]] std::vector<std::uint8_t> read_disc_file(const std::string &path);
// Names directly inside a directory on the disc, files and directories both.
[[nodiscard]] std::vector<std::string> list_disc_directory(const std::string &path);

void write_io_state(psprecomp::SnapshotWriter &out);
[[nodiscard]] bool read_io_state(psprecomp::SnapshotReader &in);

// The rest of the HLE layer holds work that only exists on the host. Each of
// these says whether it has any in flight; none of them has anything to write,
// because in every case the answer to having some is to refuse.
//
// A video in progress is decoders full of state built up frame by frame; an ad
// hoc session is a socket and another player; an open dialog is a conversation
// the guest is part way through. None survives being replaced underneath.
[[nodiscard]] std::string why_no_mpeg_state();
[[nodiscard]] std::string why_no_adhoc_state();
[[nodiscard]] std::string why_no_savedata_state();
[[nodiscard]] std::string why_no_utility_state();

// Music is the exception, and has to be: it plays for as long as the game runs,
// so refusing while a track is open would refuse every save. A track is
// rebuildable -- the header and the encoded data are in the guest's own buffer,
// which a state restores -- so only the position is kept and the decoder is
// opened again on the way back in.
[[nodiscard]] std::string why_no_atrac_state();
void write_atrac_state(psprecomp::SnapshotWriter &out);
[[nodiscard]] bool read_atrac_state(psprecomp::SnapshotReader &in, const psprecomp::GuestMemory &memory);

#if defined(MGA_HAS_RENDERER)
namespace gpu { class VulkanRenderer; }
// The renderer owns the window, so HLE that needs host input goes through it.
[[nodiscard]] gpu::VulkanRenderer *active_renderer();
// Creates the renderer, with the window and the interface on it, the first
// time it is needed: by the setup screens before the game starts, otherwise
// when the game registers its media imports. Null without a window.
gpu::VulkanRenderer *ensure_renderer();
#endif

} // namespace mga
