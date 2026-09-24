#include "state/save_state.hpp"

#include "hle/hle_common.hpp"
#include "install/user_data.hpp"
#include "kernel/kernel.hpp"
#if defined(MGA_HAS_RENDERER)
#include "gpu/vulkan_renderer.hpp"
#endif

#include "psprecomp/guest_memory.hpp"
#include "psprecomp/snapshot.hpp"
#include "psprecomp/state.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>

namespace mga::state {
namespace {

using psprecomp::SnapshotReader;
using psprecomp::SnapshotWriter;

[[nodiscard]] std::filesystem::path states_directory() {
    return install::user_data_directory() / "states";
}

// Written to a temporary name and moved into place, so a save interrupted part
// way through -- no disk space, the process killed -- does not leave a slot
// holding half a file where a good one used to be.
[[nodiscard]] bool write_file(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes,
                              std::string &reason) {
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);
    if (code) {
        reason = "cannot create the states folder: " + code.message();
        return false;
    }
    const std::filesystem::path temporary = std::filesystem::path(path).concat(".part");
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            reason = "cannot open the file for writing";
            return false;
        }
        file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        file.close();
        if (!file) {
            reason = "the file could not be written in full";
            std::filesystem::remove(temporary, code);
            return false;
        }
    }
    std::filesystem::rename(temporary, path, code);
    if (code) {
        // Rename across the same directory should not fail, but a virus scanner
        // or a file left open can make it.
        std::filesystem::remove(temporary, code);
        reason = "the file could not be put in place";
        return false;
    }
    return true;
}

[[nodiscard]] bool read_file(const std::filesystem::path &path, std::vector<std::uint8_t> &bytes,
                             std::string &reason) {
    std::error_code code;
    if (!std::filesystem::exists(path, code)) {
        reason = "there is nothing saved in that slot";
        return false;
    }
    const std::uintmax_t size = std::filesystem::file_size(path, code);
    if (code) {
        reason = "cannot read the file: " + code.message();
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        reason = "cannot open the file";
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<std::uintmax_t>(file.gcount()) != size) {
        reason = "the file is shorter than it says it is";
        return false;
    }
    return true;
}

} // namespace

std::filesystem::path slot_path(unsigned slot) {
    return states_directory() / ("slot" + std::to_string(slot) + ".mgastate");
}

bool slot_exists(unsigned slot) {
    std::error_code code;
    return std::filesystem::exists(slot_path(slot), code);
}

std::string slot_description(unsigned slot) {
    std::error_code code;
    const std::filesystem::path path = slot_path(slot);
    if (!std::filesystem::exists(path, code)) return "empty";
    const auto written = std::filesystem::last_write_time(path, code);
    if (code) return "saved";
    // file_time_type has no portable calendar conversion before C++20's
    // clock_cast, and not every standard library this builds against has that,
    // so the age is shown rather than the date. It is also more useful: a
    // player wants to know which slot is the recent one.
    const auto age = std::filesystem::file_time_type::clock::now() - written;
    const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(age).count();
    if (minutes < 1) return "saved just now";
    if (minutes < 60) return "saved " + std::to_string(minutes) + " min ago";
    const auto hours = minutes / 60;
    if (hours < 48) return "saved " + std::to_string(hours) + (hours == 1 ? " hour ago" : " hours ago");
    const auto days = hours / 24;
    return "saved " + std::to_string(days) + " days ago";
}

std::string why_not_now() {
    // Asked of everything that holds work of its own, in the order a player is
    // most likely to hit: the kernel first, because a host wait is the common
    // case, then the parts of the HLE layer that can have something in flight.
    if (std::string reason = kernel().why_no_state(); !reason.empty()) return reason;
    if (std::string reason = why_no_media_state(); !reason.empty()) return reason;
    if (std::string reason = why_no_mpeg_state(); !reason.empty()) return reason;
    if (std::string reason = why_no_adhoc_state(); !reason.empty()) return reason;
    if (std::string reason = why_no_savedata_state(); !reason.empty()) return reason;
    if (std::string reason = why_no_utility_state(); !reason.empty()) return reason;
    if (std::string reason = why_no_atrac_state(); !reason.empty()) return reason;
    if (std::string reason = why_no_io_state(); !reason.empty()) return reason;
    return {};
}

bool save(Runtime &runtime, const AllegrexContext &ctx, unsigned slot, std::string &reason) {
    if (slot >= kSlotCount) {
        reason = "there is no such slot";
        return false;
    }
    // Asked again here rather than trusting the answer the interface got: the
    // player picks a row, and between the row being drawn and the button being
    // pressed the game has gone on running.
    reason = why_not_now();
    if (!reason.empty()) return false;

    SnapshotWriter out;
    psprecomp::write_state_header(out, kProfileTag, kProfileVersion);
    psprecomp::write_machine(out, runtime.memory(), ctx);
    kernel().write_state(out, ctx);
    write_media_state(out);
    write_atrac_state(out);
    write_io_state(out);

    if (!write_file(slot_path(slot), out.data(), reason)) return false;
    std::cout << "[state] saved slot " << slot << ", " << out.size() / 1024u << " KB\n";
    return true;
}

bool load(Runtime &runtime, AllegrexContext &ctx, unsigned slot, std::string &reason) {
    if (slot >= kSlotCount) {
        reason = "there is no such slot";
        return false;
    }
    std::vector<std::uint8_t> bytes;
    if (!read_file(slot_path(slot), bytes, reason)) return false;

    SnapshotReader in(bytes);
    if (!psprecomp::read_state_header(in, kProfileTag, kProfileVersion)) {
        reason = "that state was written by a different version of the game or of this port";
        return false;
    }

    // The machine is read aside rather than applied, because the sections after
    // it can still fail and overwriting guest memory before they have been
    // checked would destroy the running game for the sake of a file that turns
    // out not to be loadable.
    psprecomp::StagedMachine machine;
    if (!psprecomp::read_machine_staged(in, runtime.memory(), machine)) {
        reason = "that state does not match this build's memory layout, or is damaged";
        return false;
    }

    // Each of these reads into locals of its own and commits only when it has
    // read everything, so the first one to fail leaves the game as it was.
    psprecomp::apply_machine(runtime.memory(), ctx, machine);
    if (!kernel().read_state(in, ctx)) {
        reason = "the state's threads are damaged";
        return false;
    }
    if (!read_media_state(in)) {
        reason = "the state's display and graphics state is damaged";
        return false;
    }
    if (!read_atrac_state(in, runtime.memory())) {
        reason = "the state's music is damaged";
        return false;
    }
    if (!read_io_state(in)) {
        reason = "the state's open files are damaged";
        return false;
    }
    if (in.remaining() != 0u) {
        // Every section read what it expected and there is still more in the
        // file. Not fatal -- nothing read was wrong -- but it means the writer
        // and the reader disagree, which the version is supposed to prevent.
        std::cout << "[state] " << in.remaining() << " bytes at the end of slot " << slot
                  << " were not read; the version may need raising\n";
    }

#if defined(MGA_HAS_RENDERER)
    // The renderer's caches describe the memory that has just been replaced:
    // textures looked up from guest addresses, and whatever it had bound.
    if (gpu::VulkanRenderer *renderer = active_renderer(); renderer != nullptr) renderer->begin_display_list();
#endif
    std::cout << "[state] loaded slot " << slot << "\n";
    return true;
}

namespace {

Request pending_kind = Request::None;
unsigned pending_slot = 0u;
std::string message;

} // namespace

void request(Request kind, unsigned slot) {
    pending_kind = kind;
    pending_slot = slot;
}

bool request_pending() { return pending_kind != Request::None; }

const std::string &last_message() { return message; }

bool run_pending(Runtime &runtime, AllegrexContext &ctx) {
    const Request kind = pending_kind;
    const unsigned slot = pending_slot;
    pending_kind = Request::None;
    if (kind == Request::None) return false;

    std::string reason;
    if (kind == Request::Save) {
        message = save(runtime, ctx, slot, reason) ? "Saved to slot " + std::to_string(slot + 1u)
                                                   : "Cannot save: " + reason;
        if (!reason.empty()) std::cout << "[state] save refused: " << reason << "\n";
        // A save leaves the guest exactly as it was, so the caller carries on.
        return false;
    }

    if (!load(runtime, ctx, slot, reason)) {
        message = "Cannot load: " + reason;
        std::cout << "[state] load refused: " << reason << "\n";
        // Nothing was changed, so the caller carries on here too.
        return false;
    }
    message = "Loaded slot " + std::to_string(slot + 1u);
    return true;
}

} // namespace mga::state
