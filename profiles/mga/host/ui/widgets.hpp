#pragma once

#include "imgui.h"

#include <initializer_list>
#include <string>

// The look of the interface and the controls its screens are built from.
// Everything is sized from the current text size, so it scales with the
// window, and every control works with a gamepad, a keyboard and a mouse.
namespace mga::ui {

// Colours from the project's logo: dark brown, bronze and gold.
// Metal Gear's palette, not Monster Hunter's.
//
// The amber and brown these replace came from the profile this port was forked
// from, where they suited the game. Metal Gear's interfaces are a dark
// desaturated green with near-white text on them -- the colour of a display
// rather than of parchment.
//
// Sampled from a frame of Metal Gear Solid 4's shop screen rather than chosen
// by eye, which is why the numbers are not round: the panel reads (30, 41, 29)
// there, its text (251, 254, 248) -- white with just enough green left in it to
// belong to the panel -- and the labels between them (159, 174, 146). Green is
// the highest channel everywhere, including in the greys, and that is what
// stops the result looking merely dark rather than looking like a screen.
namespace colors {
inline constexpr ImU32 kBackdrop = IM_COL32(9, 13, 9, 185);
inline constexpr ImU32 kPanel = IM_COL32(22, 31, 21, 246);
inline constexpr ImU32 kPanelEdge = IM_COL32(154, 170, 146, 255);
inline constexpr ImU32 kRow = IM_COL32(232, 245, 225, 10);
inline constexpr ImU32 kRowHover = IM_COL32(206, 228, 196, 30);
inline constexpr ImU32 kRowFocus = IM_COL32(226, 244, 216, 66);
inline constexpr ImU32 kAccent = IM_COL32(176, 202, 160, 255);
inline constexpr ImU32 kAccentBright = IM_COL32(246, 253, 235, 255);
inline constexpr ImU32 kText = IM_COL32(233, 243, 228, 255);
inline constexpr ImU32 kTextDim = IM_COL32(150, 168, 142, 255);
inline constexpr ImU32 kTextDisabled = IM_COL32(97, 111, 93, 255);
// Kept warm on purpose: a warning has to stop reading as part of the panel,
// and in a green interface the one thing that does that is not being green.
inline constexpr ImU32 kDanger = IM_COL32(211, 118, 88, 255);
inline constexpr ImU32 kGood = IM_COL32(150, 206, 132, 255);
inline constexpr ImU32 kTrack = IM_COL32(232, 245, 225, 36);
// The selected row, inverted: a near-white bar with dark ink on it, which is
// what Metal Gear Solid 4 does and the last thing that separated this from the
// screenshot. Sampled from the same frame -- the bar reads (251, 254, 249) and
// its text (33, 40, 32) -- and everything a row draws needs a dark counterpart,
// because a row is a label, a value, arrows, a switch and a slider, and any one
// of them left pale would vanish into the bar.
inline constexpr ImU32 kRowSelected = IM_COL32(243, 249, 238, 255);
inline constexpr ImU32 kOnSelected = IM_COL32(28, 36, 27, 255);
inline constexpr ImU32 kOnSelectedDim = IM_COL32(74, 88, 70, 255);
inline constexpr ImU32 kOnSelectedFaint = IM_COL32(148, 160, 142, 255);
inline constexpr ImU32 kOnSelectedTrack = IM_COL32(28, 36, 27, 56);
inline constexpr ImU32 kDangerOnSelected = IM_COL32(138, 54, 34, 255);
inline constexpr ImU32 kGoodOnSelected = IM_COL32(44, 96, 40, 255);
} // namespace colors

ImGuiStyle make_style(float scale, float font_size);

// The panel every screen sits in, centred in the window: title and optional
// subtitle, then fixed parts such as tabs, then the scrolling content from
// begin_content(), then a footer with the focused row's description and the
// button hints:
//
//   begin_panel(...); tab_bar(...); begin_content(); rows...;
//   begin_footer(); hints(...); end_panel();
void begin_panel(const char *id, const std::string &title, const std::string &subtitle, bool dim_game);
void begin_content();
void begin_footer();
void end_panel();

// Tabs switched with L1/R1 (Q/W on the keyboard) or the mouse. Returns true
// when `selected` changed.
bool tab_bar(const char *const *labels, int count, int &selected);

struct RowOptions {
    bool disabled{};
    std::string note;         // shown dimmed next to the value, e.g. who decides it
    std::string description;  // shown in the footer while the row is focused
};

// A setting with a few values, changed with left/right or by activating it.
// Returns -1, 0 or +1.
int choice_row(const char *label, const std::string &value, const RowOptions &options = {});
// An on/off setting. Returns true when toggled.
bool toggle_row(const char *label, bool value, const RowOptions &options = {});
// A number between minimum and maximum, changed in steps with left/right or
// dragged with the mouse. Returns true when `value` changed.
bool slider_row(const char *label, int &value, int minimum, int maximum, int step, const char *format,
                const RowOptions &options = {});
// An action. Returns true when activated.
bool button_row(const char *label, const RowOptions &options = {}, ImU32 color = colors::kText);
// A setting chosen on a screen of its own: its value, and a chevron that
// opens that screen. Returns true when activated.
bool value_row(const char *label, const std::string &value, const RowOptions &options = {});
// An entry of a list such as the file browser's: an icon, a name and a
// detail on the right. `id` keeps rows with equal names apart.
enum class ListIcon { None, Folder, ParentFolder, File, Disc, Drive };
bool list_row(const char *id, const std::string &name, const std::string &detail, ListIcon icon,
              bool highlight = false);
// A line of information, focusable so a gamepad can scroll to it.
void info_row(const char *label, const std::string &value);
void section(const char *title);

// Focuses the next row, e.g. the first row after switching tabs.
void focus_next_row();

// A wide button for the setup screens. Returns true when activated.
bool big_button(const char *label, float width, bool primary = false, bool disabled = false);
void progress_bar(float fraction, const std::string &overlay);

// Text wrapped to the available width.
void paragraph(const std::string &text, ImU32 color = colors::kText);
void heading(const std::string &text);

// Button hints for the footer, drawn with the glyphs of the pad in use or the
// keys of the keyboard. Toggle, Shift, Space and Symbols exist on the pad only
// and are skipped for the keyboard.
enum class Control {
    Confirm,
    Back,
    Tabs,
    Change,
    Menu,
    Start,
    Toggle,   // the top face button
    Delete,   // the back face button; Backspace
    Shift,    // the left face button
    Space,    // the top face button
    Symbols,  // Select, Share, Create or View, as the pad names it
    Cursor,   // the shoulder buttons; the arrow keys
};
struct Hint {
    Control control;
    const char *text;
};
void hints(std::initializer_list<Hint> list);

} // namespace mga::ui
