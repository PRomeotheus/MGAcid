#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace mga::install {

// The per-user data directory holds what the installer sets up:
//
//   EBOOT.ELF      the executable prepared from the player's disc image
//   disc.iso       the copied disc image (absent when the image is used in place)
//   settings.ini   where the disc image is, and the player's settings
//
// Save data is not here yet: ms0 stays where the host has always kept it.
inline constexpr const char *kExecutableFile = "EBOOT.ELF";
inline constexpr const char *kCopiedImageFile = "disc.iso";
inline constexpr const char *kSettingsFile = "settings.ini";

// MGA_DATA_DIR when set, otherwise SDL_GetPrefPath("MGAcid", "MGA")
// (or the same location computed by hand in a build without SDL). SDL creates
// the directory if it does not exist yet.
[[nodiscard]] std::filesystem::path user_data_directory();

// The three folders a player's own files live in. A release keeps them beside
// its executable so an installation is self-contained and can be moved to
// another drive, or carried on one, without losing saves or either pack.
enum class DataFolder { Saves, Textures, Music };

// The folder name under whichever root is in use: ms0, textures, music.
[[nodiscard]] const char *data_folder_name(DataFolder which) noexcept;

// Where `which` is being read and written. In order of preference:
//
//   1. the folder the player chose, if they have chosen one
//   2. beside the executable, once that folder exists
//   3. the same folder under the per-user data directory, which is where
//      installations made before this layout put it
//   4. beside the executable, created on the spot
//
// and, if the executable's own directory cannot be written -- an installation
// under Program Files, or a read-only mount -- the per-user data directory
// instead, so the game still runs.
[[nodiscard]] std::filesystem::path data_folder(DataFolder which);

// The player's choice, or an empty path to go back to the default. Settings
// calls this as it loads and whenever a folder row changes, which keeps the
// resolution here and the storage of it there: this layer must not depend on
// settings, because settings already depends on this one.
void set_chosen_folder(DataFolder which, const std::filesystem::path &folder);

// Creates all three where data_folder() says they belong. Called once at
// startup so a fresh installation has them even before anything is written.
void create_data_folders();

// Every key=value line of settings.ini. The installer owns disc_image; the
// player's settings (host/settings) keep their keys next to it, and writing
// one never drops the others.
using SettingsEntries = std::map<std::string, std::string>;

[[nodiscard]] SettingsEntries read_settings_file(const std::filesystem::path &data_dir);
// Replaces settings.ini with `entries`, creating data_dir if needed.
void write_settings_file(const std::filesystem::path &data_dir, const SettingsEntries &entries);

struct UserSettings {
    // Disc image to read. Relative paths are relative to the data directory.
    std::filesystem::path disc_image = kCopiedImageFile;
};

[[nodiscard]] UserSettings load_settings(const std::filesystem::path &data_dir);
void save_settings(const std::filesystem::path &data_dir, const UserSettings &settings);

struct Installation {
    std::filesystem::path executable;
    std::filesystem::path disc_image; // absolute
    bool image_copied{};              // disc image lives in the data directory
};

// The installation in data_dir, if the installer has completed there. The disc
// image is not checked for existence: callers report a missing image.
[[nodiscard]] std::optional<Installation> find_installation(const std::filesystem::path &data_dir);

// UTF-8 conversions for paths shown in dialogs, stored in settings or received
// from SDL.
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path &path);
[[nodiscard]] std::filesystem::path path_from_utf8(const std::string &text);

} // namespace mga::install
