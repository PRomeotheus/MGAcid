#include "ui/savedata_dialog.hpp"

#include "ui/layer.hpp"
#include "ui/widgets.hpp"

#include "gpu/vulkan_renderer.hpp"

#include "imgui.h"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <utility>

namespace mga::ui {
namespace {

using Clock = std::chrono::steady_clock;

// How long after its last frame a dialog still counts as on screen.
constexpr auto kDrawnFor = std::chrono::seconds(2);

struct Dialog {
    bool open{};
    Clock::time_point drawn{};
    SavedataRequest request;
    SavedataDone on_done;
    // The slot a save is about to overwrite, while the player answers, and
    // whether its rows have been focused yet: focusing them every frame would
    // pin the cursor to the first one and make the second unreachable.
    std::optional<std::size_t> confirming;
    bool confirm_focused{};
    // The row to put the cursor on when the list is next built.
    std::optional<std::size_t> focus;
};

Dialog &dialog() {
    static Dialog d;
    return d;
}

const char *action_title(SavedataAction action) {
    switch (action) {
    case SavedataAction::Load: return "Load";
    case SavedataAction::Delete: return "Delete";
    case SavedataAction::Save: break;
    }
    return "Save";
}

const char *action_subtitle(SavedataAction action) {
    switch (action) {
    case SavedataAction::Load: return "Choose a save to load";
    case SavedataAction::Delete: return "Choose a save to delete";
    case SavedataAction::Save: break;
    }
    return "Choose where to save";
}

std::string human_size(std::uint64_t bytes) {
    char text[32];
    if (bytes >= 1024u * 1024u) std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else if (bytes >= 1024u) std::snprintf(text, sizeof(text), "%llu KB", static_cast<unsigned long long>(bytes / 1024u));
    else std::snprintf(text, sizeof(text), "%llu B", static_cast<unsigned long long>(bytes));
    return text;
}

// Back: Esc, and the pad's cancel button, which each screen reads for itself.
bool back_pressed() {
    Layer &layer = Layer::get();
    const bool escape = layer.take_back();
    const ImGuiKey cancel = layer.confirm_south() ? ImGuiKey_GamepadFaceRight : ImGuiKey_GamepadFaceDown;
    return escape || ImGui::IsKeyPressed(cancel, false);
}

// The separator between the parts of a row. ASCII on purpose: the build hands
// MSVC no /utf-8, so its execution charset is the system code page and a
// non-ASCII escape in a narrow literal comes out as one byte that is not valid
// UTF-8 -- which ImGui draws as a replacement character.
constexpr const char *kSeparator = "  -  ";

// The PSP's savedata title and detail are free text the game fills in, and
// Metal Gear Ac!d puts the difficulty and the play time on separate lines. A
// row is one line, so the lines are joined rather than clipped.
std::string flatten(const std::string &text) {
    std::string out;
    std::string line;
    const auto flush = [&] {
        const auto first = line.find_first_not_of(" \t");
        if (first != std::string::npos) {
            const auto last = line.find_last_not_of(" \t");
            if (!out.empty()) out += kSeparator;
            out.append(line, first, last - first + 1);
        }
        line.clear();
    };
    for (const char c : text) {
        if (c == '\n' || c == '\r') flush();
        else line += c;
    }
    flush();
    return out;
}

// What a slot shows: its own title, and the detail that tells two saves of the
// same game apart.
std::string slot_name(const SaveSlot &slot, SavedataAction action) {
    if (!slot.exists) return action == SavedataAction::Save ? "New save" : "Empty";
    std::string name = flatten(slot.title.empty() ? "Save " + slot.save_name : slot.title);
    const std::string detail = flatten(slot.detail);
    if (!detail.empty() && detail != name) name += kSeparator + detail;
    return name;
}

std::string slot_detail(const SaveSlot &slot) {
    if (!slot.exists) return {};
    std::string detail = slot.date;
    if (slot.bytes != 0u) detail += (detail.empty() ? "" : "   ") + human_size(slot.bytes);
    return detail;
}

void close(std::optional<std::size_t> chosen) {
    Dialog &d = dialog();
    if (!d.open) return;
    d.open = false;
    d.confirming.reset();
    Layer &layer = Layer::get();
    layer.set_interactive(false);
    layer.renderer().set_game_input(true);
    layer.renderer().hold_frame(false);
    SavedataDone done = std::move(d.on_done);
    d.on_done = nullptr;
    if (done) done(chosen);
}

// The overwrite question, in place of the list.
void confirm_frame() {
    Dialog &d = dialog();
    const SaveSlot &slot = d.request.slots[*d.confirming];
    begin_panel("##savedata", "Overwrite save", slot_name(slot, d.request.action), true);
    begin_content();
    paragraph("This slot already holds a save. Saving replaces it, and what is there now cannot be "
              "brought back.",
              colors::kTextDim);
    ImGui::Dummy({0.0f, Layer::get().font_size() * 0.6f});
    const bool overwrite = button_row("Overwrite", {}, colors::kDanger);
    // Keeping the save is what the cursor starts on: a stray press on the way
    // in should not be what destroys a save.
    if (!d.confirm_focused) {
        focus_next_row();
        d.confirm_focused = true;
    }
    const bool keep = button_row("Keep this save");
    begin_footer();
    hints({{Control::Confirm, "Choose"}, {Control::Back, "Back"}});
    end_panel();

    if (back_pressed() || keep) {
        d.focus = d.confirming;
        d.confirming.reset();
        return;
    }
    if (overwrite) close(d.confirming);
}

void list_frame() {
    Dialog &d = dialog();
    const SavedataAction action = d.request.action;
    begin_panel("##savedata", action_title(action), action_subtitle(action), true);
    begin_content();

    std::optional<std::size_t> activated;
    std::size_t shown = 0u;
    for (std::size_t i = 0; i < d.request.slots.size(); ++i) {
        const SaveSlot &slot = d.request.slots[i];
        // Loading and deleting only offer the slots that hold something, as
        // the PSP's own dialog does.
        if (!slot.exists && action != SavedataAction::Save) continue;
        if (d.focus && *d.focus == i) {
            focus_next_row();
            d.focus.reset();
        }
        ImGui::PushID(static_cast<int>(i));
        if (list_row("##slot", slot_name(slot, action), slot_detail(slot), ListIcon::None, slot.exists))
            activated = i;
        ImGui::PopID();
        ++shown;
    }
    d.focus.reset();
    if (shown == 0u) paragraph("There are no saves here yet.", colors::kTextDim);

    begin_footer();
    hints({{Control::Confirm, action_title(action)}, {Control::Back, "Back"}});
    end_panel();

    if (back_pressed()) {
        close(std::nullopt);
        return;
    }
    if (!activated) return;
    // Saving over a slot that already holds a save asks first; everything
    // else acts at once.
    if (action == SavedataAction::Save && d.request.slots[*activated].exists) {
        d.confirming = activated;
        d.confirm_focused = false;
        return;
    }
    close(activated);
}

} // namespace

bool open_savedata_dialog(SavedataRequest request, SavedataDone on_done) {
    Layer &layer = Layer::get();
    if (!layer.attached() || request.slots.empty()) return false;
    if (dialog().open) cancel_savedata_dialog();
    Dialog &d = dialog();
    d = Dialog{};
    d.open = true;
    d.request = std::move(request);
    d.on_done = std::move(on_done);
    d.focus = d.request.focus < d.request.slots.size() ? std::optional<std::size_t>(d.request.focus)
                                                       : std::optional<std::size_t>(0u);
    // The frame on screen now stays behind the dialog: the game blanks the
    // screen while the PSP's own dialog would cover it.
    layer.renderer().hold_frame(true);
    layer.renderer().set_game_input(false);
    layer.set_interactive(true);
    return true;
}

bool savedata_dialog_open() { return dialog().open; }

void cancel_savedata_dialog() { close(std::nullopt); }

void savedata_dialog_frame() {
    Dialog &d = dialog();
    if (!d.open) return;
    d.drawn = Clock::now();
    // The menu does not open over the dialog.
    (void)Layer::get().take_menu_toggle();
    if (d.confirming) confirm_frame();
    else list_frame();
}

bool savedata_dialog_drawn_recently() {
    const Dialog &d = dialog();
    return d.drawn != Clock::time_point{} && Clock::now() - d.drawn < kDrawnFor;
}

} // namespace mga::ui
