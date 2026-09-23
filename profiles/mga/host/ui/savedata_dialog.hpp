#pragma once

// The save-data list: the dialog a PSP's firmware draws when a game asks to
// save, load or delete with a list of slots, drawn here by the port over the
// running game.
//
// It works like the on-screen keyboard (ui/text_input.hpp): the game keeps
// running and presenting frames behind it, as it does behind the PSP's own
// dialog, but reads a neutral pad while the dialog has the input. The savedata
// HLE opens it, holds its dialog life cycle, and acts on the answer.
//
// Gamepad: up and down move over the slots, confirm chooses, back cancels.
// Keyboard: the arrow keys, Enter and Esc. Saving over a slot that already
// holds data asks first.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mga::ui {

// One slot the dialog offers, in the order the game listed them.
struct SaveSlot {
    std::string save_name;  // the savedata folder's suffix, e.g. "001"
    std::string title;      // the save's own title, empty when the slot is free
    std::string detail;     // its second line
    std::string date;       // when it was written, in the host's local time
    std::chrono::system_clock::time_point modified{};  // the same, for sorting
    std::uint64_t bytes{};
    bool exists{};
};

enum class SavedataAction { Save, Load, Delete };

struct SavedataRequest {
    SavedataAction action{SavedataAction::Save};
    std::vector<SaveSlot> slots;
    std::size_t focus{};  // the slot to start on
};

// Called once when the dialog closes: which slot was chosen, or nullopt when
// the player backed out.
using SavedataDone = std::function<void(std::optional<std::size_t>)>;

// Opens the dialog over the running game. False when there is no window to
// draw it in, or when the request offers no slots; the caller then falls back
// to deciding for itself.
bool open_savedata_dialog(SavedataRequest request, SavedataDone on_done);
[[nodiscard]] bool savedata_dialog_open();
// Closes an open dialog as cancelled.
void cancel_savedata_dialog();
// Builds this frame of the dialog and handles its input. Call inside an
// interface frame (begin_frame/end_frame).
void savedata_dialog_frame();

// Whether the dialog has been drawn in the last moment. The HLE waits only
// while this is true: a dialog the player is looking at is redrawn with every
// game frame, so if frames stop -- the window closed, the game stopped
// presenting -- this goes false and the HLE settles the request itself rather
// than leaving the game waiting for good.
[[nodiscard]] bool savedata_dialog_drawn_recently();

} // namespace mga::ui
