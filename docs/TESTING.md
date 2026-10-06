# Testing

## Automated

- **Framework tests** run with `ctest --test-dir out/framework` and need no game data.
- **Builds on every platform** in CI — planned in [#15](https://github.com/PRomeotheus/MGAcid/issues/15).
- **Regression tests on your own copy of the game**, replaying recorded input and comparing frames against reference images — planned in [#16](https://github.com/PRomeotheus/MGAcid/issues/16).

Until those exist, changes are checked by playing, with the smoke test below.

## Smoke test

About fifteen minutes. It walks through the parts of the game known to work, so a regression in one of them shows up. Start from a fresh profile — rename `profiles/mga/game/ms0` aside — so earlier state cannot hide a problem.

Before you start, write down the commit you are testing: `git rev-parse --short HEAD`. A result is only useful with it.

A released build is tested the same way. Note its version and which download it is (Flatpak or tarball) instead of the commit, start it through its launcher (`flatpak run io.github.promeotheus.MGAcid` or `./mgacid`, from a terminal to see the console), and start from a fresh data directory: for the Flatpak, move `~/.var/app/io.github.promeotheus.MGAcid` aside; for the tarball, `~/.local/share/MGAcid`. The first start then runs the setup from your disc image, which is part of the test. [`LINUX.md`](LINUX.md) says where a release keeps its saves.

| # | Step | Expected |
| --- | --- | --- |
| 1 | Start `out/mga/bin/MGAcid` | A window opens; the console lists the renderer, the audio device, and the modules it loads (`KCEJ_FS`, `KCEJ_SOUND`, `ZLIB`, the stage) |
| 2 | Wait through the logos | The title screen appears, with its music |
| 3 | Start a new game | The first stage loads and is drawn, with its text legible |
| 4 | Play far enough to be offered a save, and save to a slot | The console logs `[savedata] saved … ULUS10006… (encrypted)`, and a matching folder appears under `ms0/PSP/SAVEDATA` |
| 5 | Open the menu (Esc, or L3+R3) | The game pauses behind it and the settings are there |
| 6 | Change Texture scaling, and the Interface art row beside it | The picture changes without a restart; the console reports the textures being dropped and rebuilt |
| 7 | With a music pack in the `music` folder, toggle **Sound → Replacement music** while a track plays | The track changes over without restarting the game |
| 8 | System → Folders | Each row shows the folder in use, marked `(default)` when it is the automatic one |
| 9 | System → Saves → **Back up saves…** | Your save is listed as `Save 1 (ULUS10006001)` and copies to the folder you choose |
| 10 | Move the camera, if you have a gamepad | It turns and stops when the stick is released |
| 11 | Close the window and start the game again | The console logs `[savedata] loaded … (decrypted)`, and the title screen offers the save from step 4 |
| 12 | Load it | The game continues from where it was saved |

Steps 3 and 4 are deliberately vague about where the game offers a save: that
depends on how far the first stage runs, which has not been written down yet.
Replace them with the real beats once somebody has played through.

### What to watch for throughout

- Any line reading `[interpreter] no recompiled function at …` — that code runs about twenty times slower. Note the address.
- Missing, torn or flickering geometry, and black or white patches where effects should be.
- The console's last lines if the game stops or crashes.

## Reporting

Open a **Test report** issue with the platform, hardware, commit and the steps you reached.

Useful settings while testing — all described in [the profile README](../profiles/mga/README.md#configuration):

- `MGA_SCREENSHOT_DIR` and `MGA_SCREENSHOT_EVERY` capture frames to attach to a report.
- `MGA_TRACE_PAD=1` shows whether input is reaching the game.
- `MGA_TRACE_AUDIO=1` shows audio levels and dropped frames.
- `PSPRECOMP_HLE_HISTOGRAM=1` prints which system calls the game made.
