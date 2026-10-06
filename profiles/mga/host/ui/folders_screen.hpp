#pragma once

// The System page's folder rows: where the player's saves, texture pack and
// replacement music are kept. A release keeps all three beside the executable
// so an installation can be moved or carried on a drive; these rows point any
// of them somewhere else instead. The resolution itself is in
// install/user_data.hpp -- this is only the choosing of it.

namespace mga::ui {

// The rows: the three folders and a row that puts them all back to default.
void folder_rows();

// Whether a folder is being chosen.
[[nodiscard]] bool folder_screen_open();

// Draws the browser in place of the page. `back`: the back button was pressed
// this frame. True while the browser is open.
bool folder_screen(bool back);

} // namespace mga::ui
