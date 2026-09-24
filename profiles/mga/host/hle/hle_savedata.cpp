// sceUtilitySavedata*: the PSP's save-data dialog, backed by PSP-layout
// folders on the host (see save_data/savedata_store.hpp).
//
// What the game asks for:
//   - at boot, AUTOLOAD of its own save; when there is none, AUTOLOAD of the
//     saves of two other games (ULJM05500, ULJM05710) and SIZES for its own,
//     and "no data" answers lead to a new game;
//   - to save, AUTOLOAD of its own save followed by AUTOSAVE;
//   - after the title screen, AUTOLOAD again to read the characters.
// The list modes (LISTSAVE, LISTLOAD, LISTDELETE) open the port's own slot
// dialog (host/ui/savedata_dialog.hpp) and hold the life cycle until the
// player answers. Everything else is settled in InitStart. Without a window --
// or if nothing draws the dialog -- the list modes fall back to acting as if
// the player confirmed the first entry, so no path can leave the guest
// polling for good; see utility_dialog.hpp.
#include "hle_common.hpp"
#include "utility_dialog.hpp"

#include "save_data/savedata_crypto.hpp"
#include "save_data/savedata_store.hpp"
#include "save_data/save_transfer.hpp"

#if defined(MGA_HAS_RENDERER)
#include "ui/savedata_dialog.hpp"
#endif

#include "psprecomp/common.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace mga {
namespace {

// SceUtilitySavedataParam, after the common dialog header.
namespace param {
constexpr std::uint32_t kMode = 0x30u;
constexpr std::uint32_t kOverwrite = 0x38u;
constexpr std::uint32_t kGameName = 0x3Cu;      // char[13]
constexpr std::uint32_t kSaveName = 0x4Cu;      // char[20]
constexpr std::uint32_t kSaveNameList = 0x60u;  // pointer to char[20] entries, "" terminated
constexpr std::uint32_t kFileName = 0x64u;      // char[13]
constexpr std::uint32_t kDataBuf = 0x74u;
constexpr std::uint32_t kDataBufSize = 0x78u;
constexpr std::uint32_t kDataSize = 0x7Cu;
constexpr std::uint32_t kTitle = 0x80u;          // char[128]
constexpr std::uint32_t kSavedataTitle = 0x100u; // char[128]
constexpr std::uint32_t kDetail = 0x180u;        // char[1024]
constexpr std::uint32_t kParentalLevel = 0x580u;
constexpr std::uint32_t kIcon0 = 0x584u;  // {buf, bufSize, size, unknown}
constexpr std::uint32_t kIcon1 = 0x594u;
constexpr std::uint32_t kPic1 = 0x5A4u;
constexpr std::uint32_t kSnd0 = 0x5B4u;
constexpr std::uint32_t kFocus = 0x5C8u;
constexpr std::uint32_t kMsFree = 0x5D0u;
constexpr std::uint32_t kMsData = 0x5D4u;
constexpr std::uint32_t kUtilityData = 0x5D8u;
constexpr std::uint32_t kKey = 0x5DCu;  // char[16], firmware 2.00 and later
constexpr std::uint32_t kSecureVersion = 0x5ECu;
constexpr std::uint32_t kMinimumSizeWithKey = 0x5ECu;
constexpr std::uint32_t kSizeInfo = 0x5FCu;  // GETSIZE (22) only
} // namespace param

enum Mode : std::uint32_t {
    kAutoLoad = 0,
    kAutoSave = 1,
    kLoad = 2,
    kSave = 3,
    kListLoad = 4,
    kListSave = 5,
    kListDelete = 6,
    kDelete = 7,
    kSizes = 8,
    kAutoDelete = 9,
    kSingleDelete = 10,
    kGetSize = 22,
};

const char *mode_name(std::uint32_t mode) {
    static const char *const names[] = {"AUTOLOAD", "AUTOSAVE", "LOAD", "SAVE", "LISTLOAD", "LISTSAVE",
                                        "LISTDELETE", "DELETE", "SIZES", "AUTODELETE", "SINGLEDELETE", "LIST",
                                        "FILES", "MAKEDATASECURE", "MAKEDATA", "READDATASECURE", "READDATA",
                                        "WRITEDATASECURE", "WRITEDATA", "ERASESECURE", "ERASE", "DELETEDATA",
                                        "GETSIZE"};
    return mode < std::size(names) ? names[mode] : "UNKNOWN";
}

// Result codes written to the parameter block's result field.
namespace result {
constexpr std::uint32_t kOk = 0u;
// The common dialog's "the player backed out". Every other code here is a
// savedata error; this one says nothing went wrong and nothing was done.
constexpr std::uint32_t kCancelled = 1u;
constexpr std::uint32_t kLoadDataBroken = 0x80110306u;
constexpr std::uint32_t kLoadNoData = 0x80110307u;
constexpr std::uint32_t kLoadParam = 0x80110308u;
constexpr std::uint32_t kDeleteNoData = 0x80110347u;
constexpr std::uint32_t kSaveAccessError = 0x80110385u;
constexpr std::uint32_t kSaveParam = 0x80110388u;
constexpr std::uint32_t kSizesNoData = 0x801103C7u;
constexpr std::uint32_t kReadWriteNoData = 0x80110327u;
} // namespace result

constexpr std::uint32_t kClusterSize = 0x8000u;
constexpr std::uint32_t kFreeClusters = 0x8000u;  // 1 GiB

// A list dialog waiting on the player. The chosen slot is an index into
// `names`; `fell_back` means nothing drew the dialog and the port should
// decide instead.
struct PendingList {
    std::uint32_t params{};
    std::uint32_t mode{};
    std::vector<std::string> names;
    bool answered{};
    bool fell_back{};
    std::optional<std::size_t> chosen;
    std::chrono::steady_clock::time_point opened{};
};

struct SavedataState {
    std::filesystem::path memory_stick;
    DialogLifecycle dialog;
    std::optional<PendingList> pending;
};

SavedataState &state() {
    static SavedataState s;
    return s;
}

bool trace_savedata() {
    static const bool enabled = std::getenv("MGA_TRACE_SAVEDATA") != nullptr;
    return enabled;
}

std::string hex_bytes(const savedata::Block &block) {
    std::string text;
    char digits[3];
    for (const std::uint8_t b : block) {
        std::snprintf(digits, sizeof digits, "%02x", b);
        text += digits;
    }
    return text;
}

// "<>" stands for "no save name": the folder is the game name alone.
std::string save_name_at(const psprecomp::GuestMemory &memory, std::uint32_t address) {
    std::string name = read_cstring(memory, address, 20u);
    return name == "<>" ? std::string{} : name;
}

std::vector<std::string> save_name_list(const psprecomp::GuestMemory &memory, std::uint32_t list) {
    std::vector<std::string> names;
    if (list == 0u) return names;
    for (std::uint32_t i = 0; i < 64u; ++i) {
        const std::uint32_t entry = list + i * 20u;
        if (memory.load8(entry) == 0u) break;
        names.push_back(save_name_at(memory, entry));
    }
    return names;
}

std::vector<std::uint8_t> read_guest(const psprecomp::GuestMemory &memory, std::uint32_t address, std::uint32_t size) {
    std::vector<std::uint8_t> bytes(size);
    for (std::uint32_t i = 0; i < size; ++i) bytes[i] = memory.load8(address + i);
    return bytes;
}

// {buf, bufSize, size, unknown}: the bytes the game supplies for an icon file.
std::vector<std::uint8_t> read_file_data(const psprecomp::GuestMemory &memory, std::uint32_t field) {
    const std::uint32_t buffer = memory.load32(field);
    const std::uint32_t size = std::min(memory.load32(field + 8u), memory.load32(field + 4u));
    if (buffer == 0u || size == 0u) return {};
    return read_guest(memory, buffer, size);
}

savedata::SaveFiles files_for(const psprecomp::GuestMemory &memory, std::uint32_t params, const std::string &save_name) {
    savedata::SaveFiles files;
    files.game_name = read_cstring(memory, params + param::kGameName, 13u);
    files.save_name = save_name;
    files.file_name = read_cstring(memory, params + param::kFileName, 13u);
    if (memory.load32(params + dialog_common::kSizeOffset) > param::kMinimumSizeWithKey) {
        savedata::Block key{};
        for (std::uint32_t i = 0; i < key.size(); ++i) key[i] = memory.load8(params + param::kKey + i);
        if (!savedata::is_zero(key)) {
            files.key = key;
            // The menu's Import checks saves with it.
            savedata::remember_game_key(files.game_name, key);
        }
    }
    return files;
}

void log_request(const psprecomp::GuestMemory &memory, std::uint32_t params) {
    const std::uint32_t mode = memory.load32(params + param::kMode);
    std::cerr << "[savedata] " << mode_name(mode) << " (" << mode << ")"
              << " game=\"" << read_cstring(memory, params + param::kGameName, 13u) << "\""
              << " save=\"" << read_cstring(memory, params + param::kSaveName, 20u) << "\""
              << " file=\"" << read_cstring(memory, params + param::kFileName, 13u) << "\"";
    if (trace_savedata()) {
        const std::uint32_t size = memory.load32(params + dialog_common::kSizeOffset);
        std::cerr << " size=" << psprecomp::hex32(size)
                  << " buf=" << psprecomp::hex32(memory.load32(params + param::kDataBuf))
                  << " bufSize=" << psprecomp::hex32(memory.load32(params + param::kDataBufSize))
                  << " dataSize=" << psprecomp::hex32(memory.load32(params + param::kDataSize))
                  << " overwrite=" << memory.load32(params + param::kOverwrite)
                  << " focus=" << memory.load32(params + param::kFocus)
                  << " icon0=" << memory.load32(params + param::kIcon0 + 8u)
                  << " icon1=" << memory.load32(params + param::kIcon1 + 8u)
                  << " pic1=" << memory.load32(params + param::kPic1 + 8u)
                  << " snd0=" << memory.load32(params + param::kSnd0 + 8u);
        if (size > param::kMinimumSizeWithKey) {
            savedata::Block key{};
            for (std::uint32_t i = 0; i < key.size(); ++i) key[i] = memory.load8(params + param::kKey + i);
            std::cerr << " key=" << hex_bytes(key)
                      << " secureVersion=" << memory.load32(params + param::kSecureVersion);
        }
        const auto names = save_name_list(memory, memory.load32(params + param::kSaveNameList));
        if (!names.empty()) {
            std::cerr << " list=[";
            for (std::size_t i = 0; i < names.size(); ++i) std::cerr << (i ? "," : "") << "\"" << names[i] << "\"";
            std::cerr << "]";
        }
    }
    std::cerr << "\n";
}

std::uint32_t do_load(psprecomp::GuestMemory &memory, std::uint32_t params, const std::string &save_name) {
    const auto files = files_for(memory, params, save_name);
    const std::uint32_t buffer = memory.load32(params + param::kDataBuf);
    const std::uint32_t capacity = memory.load32(params + param::kDataBufSize);
    if (files.game_name.empty() || files.file_name.empty() || buffer == 0u) return result::kLoadParam;

    const auto loaded = savedata::load_save(state().memory_stick, files);
    if (loaded.status == savedata::LoadStatus::NoData) {
        std::cerr << "[savedata] no save data in " << savedata::save_folder(state().memory_stick, files).string() << "\n";
        return result::kLoadNoData;
    }
    if (loaded.status == savedata::LoadStatus::Broken) {
        std::cerr << "[savedata] broken save in " << savedata::save_folder(state().memory_stick, files).string()
                  << ": " << loaded.reason << "\n";
        return result::kLoadDataBroken;
    }
    const auto &data = loaded.contents.data;
    const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(data.size(), capacity));
    memory.copy_in(buffer, std::span<const std::uint8_t>(data.data(), count));
    memory.store32(params + param::kDataSize, count);
    write_cstring(memory, params + param::kTitle, loaded.contents.title, 128u);
    write_cstring(memory, params + param::kSavedataTitle, loaded.contents.savedata_title, 128u);
    write_cstring(memory, params + param::kDetail, loaded.contents.detail, 1024u);
    std::cerr << "[savedata] loaded " << count << " bytes from "
              << savedata::save_folder(state().memory_stick, files).string()
              << (files.key ? " (decrypted)" : "") << "\n";
    if (data.size() > capacity)
        std::cerr << "[savedata] " << files.file_name << " holds " << data.size() << " bytes; the buffer takes "
                  << capacity << "\n";
    return result::kOk;
}

std::uint32_t do_save(psprecomp::GuestMemory &memory, std::uint32_t params, const std::string &save_name) {
    const auto files = files_for(memory, params, save_name);
    const std::uint32_t buffer = memory.load32(params + param::kDataBuf);
    const std::uint32_t size = memory.load32(params + param::kDataSize);
    if (files.game_name.empty() || files.file_name.empty() || buffer == 0u) return result::kSaveParam;

    savedata::SaveContents contents;
    contents.data = read_guest(memory, buffer, size);
    contents.title = read_cstring(memory, params + param::kTitle, 128u);
    contents.savedata_title = read_cstring(memory, params + param::kSavedataTitle, 128u);
    contents.detail = read_cstring(memory, params + param::kDetail, 1024u);
    contents.parental_level = memory.load8(params + param::kParentalLevel);
    contents.icon0 = read_file_data(memory, params + param::kIcon0);
    contents.icon1 = read_file_data(memory, params + param::kIcon1);
    contents.pic1 = read_file_data(memory, params + param::kPic1);
    contents.snd0 = read_file_data(memory, params + param::kSnd0);

    std::string error;
    if (!savedata::write_save(state().memory_stick, files, contents, error)) {
        std::cerr << "[savedata] save failed: " << error << "\n";
        return result::kSaveAccessError;
    }
    std::cerr << "[savedata] saved " << size << " bytes to " << savedata::save_folder(state().memory_stick, files).string()
              << (files.key ? " (encrypted)" : "") << "\n";
    return result::kOk;
}

std::uint32_t do_delete(psprecomp::GuestMemory &memory, std::uint32_t params, const std::string &save_name) {
    const auto files = files_for(memory, params, save_name);
    if (!savedata::delete_save(state().memory_stick, files)) return result::kDeleteNoData;
    std::cerr << "[savedata] deleted " << savedata::save_folder(state().memory_stick, files).string() << "\n";
    return result::kOk;
}

void write_size_string(psprecomp::GuestMemory &memory, std::uint32_t address, std::uint64_t kilobytes) {
    const std::string text = kilobytes >= 1024u * 1024u ? std::to_string(kilobytes / (1024u * 1024u)) + " GB"
                             : kilobytes >= 1024u     ? std::to_string(kilobytes / 1024u) + " MB"
                                                      : std::to_string(kilobytes) + " KB";
    write_cstring(memory, address, text, 8u);
}

// SIZES: free space on the stick, the space an existing save takes, and the
// space the requested save would need.
std::uint32_t do_sizes(psprecomp::GuestMemory &memory, std::uint32_t params) {
    std::uint32_t outcome = result::kOk;
    if (const std::uint32_t free = memory.load32(params + param::kMsFree); free != 0u) {
        memory.store32(free, kClusterSize);
        memory.store32(free + 4u, kFreeClusters);
        const std::uint64_t free_kb = static_cast<std::uint64_t>(kClusterSize) * kFreeClusters / 1024u;
        memory.store32(free + 8u, static_cast<std::uint32_t>(free_kb));
        write_size_string(memory, free + 12u, free_kb);
    }
    if (const std::uint32_t data = memory.load32(params + param::kMsData); data != 0u) {
        savedata::SaveFiles files;
        files.game_name = read_cstring(memory, data, 13u);
        files.save_name = save_name_at(memory, data + 16u);
        const std::uint64_t bytes = savedata::save_size(state().memory_stick, files);
        const std::uint32_t info = data + 36u;
        const auto clusters = static_cast<std::uint32_t>((bytes + kClusterSize - 1u) / kClusterSize);
        memory.store32(info, clusters);
        memory.store32(info + 4u, static_cast<std::uint32_t>((bytes + 1023u) / 1024u));
        write_size_string(memory, info + 8u, (bytes + 1023u) / 1024u);
        memory.store32(info + 16u, static_cast<std::uint32_t>((bytes + 1023u) / 1024u));
        write_size_string(memory, info + 20u, (bytes + 1023u) / 1024u);
        if (bytes == 0u) outcome = result::kSizesNoData;
    }
    if (const std::uint32_t needed = memory.load32(params + param::kUtilityData); needed != 0u) {
        std::uint64_t bytes = memory.load32(params + param::kDataSize) + 16u;
        for (const std::uint32_t field : {param::kIcon0, param::kIcon1, param::kPic1, param::kSnd0})
            bytes += memory.load32(params + field + 8u);
        bytes += 0x2000u;  // PARAM.SFO and directory entries
        const auto clusters = static_cast<std::uint32_t>((bytes + kClusterSize - 1u) / kClusterSize);
        const std::uint64_t kb = static_cast<std::uint64_t>(clusters) * kClusterSize / 1024u;
        memory.store32(needed, clusters);
        memory.store32(needed + 4u, static_cast<std::uint32_t>(kb));
        write_size_string(memory, needed + 8u, kb);
        memory.store32(needed + 16u, static_cast<std::uint32_t>(kb));
        write_size_string(memory, needed + 20u, kb);
    }
    return outcome;
}

// GETSIZE: free space, and the space the files listed in the size-info block
// would need beyond it (never any: the stick reports ample room). Fails with
// "no data" when the save folder does not exist yet.
std::uint32_t do_getsize(psprecomp::GuestMemory &memory, std::uint32_t params, const std::string &save_name) {
    if (const std::uint32_t info = memory.load32(params + param::kSizeInfo); info != 0u) {
        const std::uint64_t free_kb = static_cast<std::uint64_t>(kClusterSize) * kFreeClusters / 1024u;
        memory.store32(info + 16u, kClusterSize);
        memory.store32(info + 20u, kFreeClusters);
        memory.store32(info + 24u, static_cast<std::uint32_t>(free_kb));
        write_size_string(memory, info + 28u, free_kb);
        memory.store32(info + 36u, 0u);
        write_size_string(memory, info + 40u, 0u);
        memory.store32(info + 48u, 0u);
        write_size_string(memory, info + 52u, 0u);
    }
    std::error_code ec;
    const auto folder = savedata::save_folder(state().memory_stick, files_for(memory, params, save_name));
    return std::filesystem::is_directory(folder, ec) ? result::kOk : result::kReadWriteNoData;
}

// Picks the save a list dialog would land on when the player just confirms:
// for loading and deleting the first listed save that exists, for saving the
// first listed name.
std::optional<std::string> pick_from_list(psprecomp::GuestMemory &memory, std::uint32_t params, bool must_exist) {
    const auto names = save_name_list(memory, memory.load32(params + param::kSaveNameList));
    for (const auto &name : names) {
        if (!must_exist || savedata::save_exists(state().memory_stick, files_for(memory, params, name))) return name;
    }
    if (!must_exist) return save_name_at(memory, params + param::kSaveName);
    return std::nullopt;
}

std::uint32_t run_request(psprecomp::GuestMemory &memory, std::uint32_t params) {
    const std::uint32_t mode = memory.load32(params + param::kMode);
    const std::string save_name = save_name_at(memory, params + param::kSaveName);
    switch (mode) {
    case kAutoLoad:
    case kLoad:
        return do_load(memory, params, save_name);
    case kAutoSave:
    case kSave:
        return do_save(memory, params, save_name);
    case kListLoad: {
        const auto chosen = pick_from_list(memory, params, true);
        if (!chosen) return result::kLoadNoData;
        write_cstring(memory, params + param::kSaveName, *chosen, 20u);
        return do_load(memory, params, *chosen);
    }
    case kListSave: {
        const auto chosen = pick_from_list(memory, params, false);
        write_cstring(memory, params + param::kSaveName, *chosen, 20u);
        return do_save(memory, params, *chosen);
    }
    case kListDelete: {
        const auto chosen = pick_from_list(memory, params, true);
        if (!chosen) return result::kDeleteNoData;
        write_cstring(memory, params + param::kSaveName, *chosen, 20u);
        return do_delete(memory, params, *chosen);
    }
    case kDelete:
    case kAutoDelete:
    case kSingleDelete:
        return do_delete(memory, params, save_name);
    case kSizes:
        return do_sizes(memory, params);
    case kGetSize:
        return do_getsize(memory, params, save_name);
    default:
        // Not used by this game. Report a parameter error so the guest takes
        // its failure path instead of reading results that were never written.
        std::cerr << "[savedata] mode " << mode << " (" << mode_name(mode) << ") is not implemented\n";
        return result::kLoadParam;
    }
}

[[nodiscard]] bool is_list_mode(std::uint32_t mode) {
    return mode == kListSave || mode == kListLoad || mode == kListDelete;
}

// How long a list dialog may go undrawn before the port decides for itself.
// The guest polls GetStatus every frame, so a dialog the player can see is
// drawn long before this.
constexpr auto kDialogGrace = std::chrono::seconds(3);

#if defined(MGA_HAS_RENDERER)

// PSP_UTILITY_SAVEDATA_FOCUS_*: where the dialog puts the cursor.
std::size_t focused_slot(std::uint32_t focus, const std::vector<ui::SaveSlot> &slots) {
    const auto first_of = [&slots](bool exists) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < slots.size(); ++i)
            if (slots[i].exists == exists) return i;
        return std::nullopt;
    };
    const auto last_of = [&slots](bool exists) -> std::optional<std::size_t> {
        for (std::size_t i = slots.size(); i-- > 0;)
            if (slots[i].exists == exists) return i;
        return std::nullopt;
    };
    const auto by_time = [&slots](bool newest) -> std::optional<std::size_t> {
        std::optional<std::size_t> best;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (!slots[i].exists) continue;
            if (!best || (newest ? slots[i].modified > slots[*best].modified
                                 : slots[i].modified < slots[*best].modified))
                best = i;
        }
        return best;
    };
    std::optional<std::size_t> chosen;
    switch (focus) {
    case 2u: chosen = slots.empty() ? std::nullopt : std::optional<std::size_t>(slots.size() - 1u); break;
    case 3u: chosen = by_time(true); break;
    case 4u: chosen = by_time(false); break;
    case 5u: chosen = first_of(true); break;
    case 6u: chosen = last_of(true); break;
    case 7u: chosen = first_of(false); break;
    case 8u: chosen = last_of(false); break;
    default: break;
    }
    return chosen.value_or(0u);
}

// What each listed slot holds, for the dialog to show.
ui::SaveSlot describe_slot(psprecomp::GuestMemory &memory, std::uint32_t params, const std::string &name) {
    ui::SaveSlot slot;
    slot.save_name = name.empty() ? std::string("(no name)") : name;
    const savedata::SaveFiles files = files_for(memory, params, name);
    const savedata::FolderSummary folder = savedata::summarize_folder(savedata::save_folder(state().memory_stick, files));
    slot.exists = folder.exists;
    slot.bytes = folder.bytes;
    slot.modified = folder.modified;
    if (!slot.exists) return slot;
    slot.date = savedata::timestamp_for_display(folder.modified);
    const savedata::LoadResult loaded = savedata::load_save(state().memory_stick, files);
    if (loaded.status == savedata::LoadStatus::Broken) {
        slot.title = "Damaged save";
        return slot;
    }
    slot.title = loaded.contents.savedata_title.empty() ? loaded.contents.title : loaded.contents.savedata_title;
    slot.detail = loaded.contents.detail;
    return slot;
}

// Opens the slot dialog for a list mode. False: no window, or no slots to
// offer, and the caller settles the request itself.
bool open_list_dialog(psprecomp::GuestMemory &memory, std::uint32_t params, std::uint32_t mode) {
    std::vector<std::string> names = save_name_list(memory, memory.load32(params + param::kSaveNameList));
    if (names.empty()) names.push_back(save_name_at(memory, params + param::kSaveName));

    ui::SavedataRequest request;
    request.action = mode == kListLoad     ? ui::SavedataAction::Load
                     : mode == kListDelete ? ui::SavedataAction::Delete
                                           : ui::SavedataAction::Save;
    for (const std::string &name : names) request.slots.push_back(describe_slot(memory, params, name));
    // Nothing to load or delete: let the guest take its "no data" path rather
    // than showing the player an empty list.
    if (mode != kListSave) {
        const bool any = std::any_of(request.slots.begin(), request.slots.end(),
                                     [](const ui::SaveSlot &slot) { return slot.exists; });
        if (!any) return false;
    }
    request.focus = focused_slot(memory.load32(params + param::kFocus), request.slots);

    const bool opened = ui::open_savedata_dialog(std::move(request), [](std::optional<std::size_t> chosen) {
        SavedataState &s = state();
        if (!s.pending || s.pending->answered) return;
        s.pending->answered = true;
        s.pending->chosen = chosen;
    });
    if (!opened) return false;
    PendingList pending;
    pending.params = params;
    pending.mode = mode;
    pending.names = std::move(names);
    pending.opened = std::chrono::steady_clock::now();
    state().pending = std::move(pending);
    std::cout << "[savedata] " << mode_name(mode) << ": asking the player, "
              << state().pending->names.size() << " slot(s)" << std::endl;
    return true;
}

#endif  // MGA_HAS_RENDERER

// Finishes a list dialog once the player has answered it. Called from every
// poll, so the guest's own polling drives it.
void settle_list(psprecomp::GuestMemory &memory) {
    SavedataState &s = state();
    if (!s.pending) return;
#if defined(MGA_HAS_RENDERER)
    if (!s.pending->answered) {
        // The guest polls without presenting, or the interface is gone: the
        // dialog would never be answered, so decide as the port did before it
        // existed rather than leave the guest waiting.
        if (ui::savedata_dialog_drawn_recently() ||
            std::chrono::steady_clock::now() - s.pending->opened < kDialogGrace)
            return;
        std::cerr << "[savedata] nothing is drawing the list dialog; choosing for the player\n";
        s.pending->fell_back = true;
        ui::cancel_savedata_dialog();
        s.pending->answered = true;
        s.pending->chosen.reset();
    }
#else
    s.pending->fell_back = true;
    s.pending->answered = true;
#endif

    const PendingList pending = std::move(*s.pending);
    s.pending.reset();
    std::uint32_t outcome;
    if (pending.fell_back) {
        outcome = run_request(memory, pending.params);
    } else if (!pending.chosen || *pending.chosen >= pending.names.size()) {
        outcome = result::kCancelled;
        std::cout << "[savedata] " << mode_name(pending.mode) << " cancelled" << std::endl;
    } else {
        const std::string &name = pending.names[*pending.chosen];
        write_cstring(memory, pending.params + param::kSaveName, name, 20u);
        outcome = pending.mode == kListSave   ? do_save(memory, pending.params, name)
                  : pending.mode == kListLoad ? do_load(memory, pending.params, name)
                                              : do_delete(memory, pending.params, name);
        std::cout << "[savedata] " << mode_name(pending.mode) << " \"" << name << "\"" << std::endl;
    }
    memory.store32(pending.params + dialog_common::kResultOffset, outcome);
    if (outcome != result::kOk && outcome != result::kCancelled)
        std::cerr << "[savedata] result " << psprecomp::hex32(outcome) << "\n";
    s.dialog.release();
}

} // namespace

void register_savedata(HleRegistrar &hle, const std::filesystem::path &memory_stick) {
    state().memory_stick = memory_stick;
    savedata::set_memory_stick(memory_stick);

    hle.add("sceUtility", "sceUtilitySavedataInitStart", [](Runtime &rt, AllegrexContext &ctx) {
        auto &memory = rt.memory();
        const std::uint32_t params = arg(ctx, 0);
        // A new request while one is still active replaces it rather than
        // failing: the earlier request's work is already done.
        if (state().dialog.active())
            std::cerr << "[savedata] InitStart while a dialog is active (status " << state().dialog.status() << ")\n";
        log_request(memory, params);
#if defined(MGA_HAS_RENDERER)
        // A list mode asks the player, and the life cycle waits at VISIBLE
        // while the guest's own frames draw the dialog.
        if (const std::uint32_t mode = memory.load32(params + param::kMode);
            is_list_mode(mode) && open_list_dialog(memory, params, mode)) {
            state().dialog.start();
            state().dialog.hold();
            kernel().finish(ctx, 0u);
            return;
        }
#endif
        const std::uint32_t outcome = run_request(memory, params);
        memory.store32(params + dialog_common::kResultOffset, outcome);
        if (outcome != result::kOk || trace_savedata())
            std::cerr << "[savedata] result " << psprecomp::hex32(outcome) << "\n";
        state().dialog.start();
        kernel().finish(ctx, 0u);
    });

    hle.add("sceUtility", "sceUtilitySavedataUpdate", [](Runtime &rt, AllegrexContext &ctx) {
        settle_list(rt.memory());
        (void)state().dialog.poll();
        kernel().finish(ctx, 0u);
    });

    hle.add("sceUtility", "sceUtilitySavedataGetStatus", [](Runtime &rt, AllegrexContext &ctx) {
        settle_list(rt.memory());
        const std::uint32_t status = state().dialog.poll();
        if (trace_savedata()) std::cerr << "[savedata] status " << status << "\n";
        kernel().finish(ctx, status);
    });

    hle.add("sceUtility", "sceUtilitySavedataShutdownStart", [](Runtime &, AllegrexContext &ctx) {
        // The guest gave up on a dialog it was still showing: close it, so the
        // pad goes back to the game.
#if defined(MGA_HAS_RENDERER)
        if (state().pending) {
            state().pending.reset();
            ui::cancel_savedata_dialog();
            state().dialog.release();
        }
#endif
        if (!state().dialog.active()) {
            kernel().finish(ctx, kErrorUtilityInvalidStatus);
            return;
        }
        if (!state().dialog.shutdown())
            std::cerr << "[savedata] ShutdownStart before the dialog finished\n";
        kernel().finish(ctx, 0u);
    });
}

} // namespace mga
