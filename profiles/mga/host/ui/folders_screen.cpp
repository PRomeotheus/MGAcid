#include "ui/folders_screen.hpp"

#include "ui/file_browser.hpp"
#include "ui/layer.hpp"
#include "ui/widgets.hpp"

#include "install/user_data.hpp"
#include "settings/settings.hpp"

#include "imgui.h"

#include <cmath>
#include <memory>
#include <string>

namespace mga::ui {
namespace {

namespace fs = std::filesystem;
using install::DataFolder;

struct State {
    std::unique_ptr<FileBrowser> browser;
    DataFolder which{DataFolder::Saves};
    fs::path last_folder;  // where the browser opens next time
    bool focus_row{};
};

State &state() {
    static State s;
    return s;
}

std::string utf8(const fs::path &path) { return install::path_to_utf8(path); }

// The same scaling the other screens indent by; it is file-local there too.
float px(float value) { return std::round(value * Layer::get().scale()); }

// The stored choice for a folder, which is empty while the default is in use.
const std::string &choice_of(const settings::Settings &s, DataFolder which) {
    switch (which) {
    case DataFolder::Saves: return s.saves_folder;
    case DataFolder::Textures: return s.textures_folder;
    case DataFolder::Music: return s.music_folder;
    }
    return s.saves_folder;
}

std::string &choice_of(settings::Settings &s, DataFolder which) {
    return const_cast<std::string &>(choice_of(static_cast<const settings::Settings &>(s), which));
}

const char *title_of(DataFolder which) {
    switch (which) {
    case DataFolder::Saves: return "Choose the saves folder";
    case DataFolder::Textures: return "Choose the textures folder";
    case DataFolder::Music: return "Choose the music folder";
    }
    return "Choose a folder";
}

void open_browser(DataFolder which) {
    State &s = state();
    s.which = which;
    FileBrowser::Options options;
    options.extensions = {};  // folders only: nothing is listed as a file
    options.filter_name = "folders";
    options.listed_name = "folders";
    options.empty_note = "No folders here.";
    options.choose_folder = "Use this folder";
    // Opening the browser where the folder already is, rather than at home, so
    // moving it one directory across is not a walk from the top.
    fs::path start = s.last_folder;
    if (start.empty()) {
        std::error_code ec;
        const fs::path current = install::data_folder(which);
        if (fs::is_directory(current, ec)) start = current;
    }
    s.browser = std::make_unique<FileBrowser>(start.empty() ? FileBrowser::home() : start, std::move(options));
    ImGui::SetScrollY(0.0f);
}

// The folder in use, and where it came from, for the row's note.
std::string note_for(const settings::Settings &s, DataFolder which) {
    const std::string &chosen = choice_of(s, which);
    const std::string folder = utf8(install::data_folder(which));
    return chosen.empty() ? folder + "  (default)" : folder;
}

void folder_row(const char *label, DataFolder which, const std::string &description) {
    settings::Settings &s = settings::current();
    RowOptions options;
    options.note = note_for(s, which);
    options.description = description;
    if (button_row(label, options)) open_browser(which);
}

} // namespace

void folder_rows() {
    State &s = state();
    if (s.focus_row) {
        focus_next_row();
        s.focus_row = false;
    }
    settings::Settings &settings_values = settings::current();

    folder_row("Saves folder…", DataFolder::Saves,
               "Where the memory stick lives. Takes effect on restart; copy your saves across first, or the "
               "game will find none.");
    folder_row("Textures folder…", DataFolder::Textures,
               "Where the replacement textures are read from. Takes effect when the game is restarted.");
    folder_row("Music folder…", DataFolder::Music,
               "Where the replacement music is read from. Takes effect when the game is restarted.");

    const bool any_chosen = !settings_values.saves_folder.empty() || !settings_values.textures_folder.empty() ||
                            !settings_values.music_folder.empty();
    RowOptions reset;
    reset.disabled = !any_chosen;
    reset.description = "Put all three back to the folders beside the game, where a new installation keeps them.";
    if (button_row("Use the default folders", reset)) {
        settings_values.saves_folder.clear();
        settings_values.textures_folder.clear();
        settings_values.music_folder.clear();
        settings::save();
        settings::apply_folder_choices();
    }
}

bool folder_screen_open() { return state().browser != nullptr; }

bool folder_screen(bool back) {
    State &s = state();
    if (!s.browser) return false;
    section(title_of(s.which));
    ImGui::Indent(px(16.0f));
    paragraph(s.which == DataFolder::Saves
                  ? "Open the folder the memory stick should live in and choose it. The change takes effect when "
                    "the game is restarted; copy your existing saves across before then."
                  : "Open the folder to read from and choose it. The change takes effect when the game is restarted.",
              colors::kTextDim);
    ImGui::Unindent(px(16.0f));

    const FileBrowser::Result result = s.browser->frame(back);
    if (result == FileBrowser::Result::Browsing) return true;
    s.last_folder = s.browser->folder();
    const fs::path chosen = s.browser->chosen();
    const DataFolder which = s.which;
    s.browser.reset();
    s.focus_row = true;
    if (result == FileBrowser::Result::Cancelled) return false;

    settings::Settings &values = settings::current();
    choice_of(values, which) = utf8(chosen);
    settings::save();
    // So the rows read the new folder at once, even though what reads from it
    // only picks it up on the next run.
    settings::apply_folder_choices();
    return false;
}

} // namespace mga::ui
