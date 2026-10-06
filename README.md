# MGAcid

A native port of **Metal Gear Ac!d** (USA, `ULUS-10006`) made by static recompilation: the game's PSP code is translated ahead of time into C++ and compiled for your machine, then run on a reimplementation of the PSP system software. It is not an emulator — there is no interpreter or JIT at the heart of it — and it is not a decompilation.

> **This project does not include any game assets.** You must provide the files from your own legally obtained copy of Metal Gear Ac!d (`ULUS-10006`) to install or build MGAcid.

## Legal disclaimer

**MGAcid** is an independent, open-source project and is not affiliated with, authorized by, sponsored by, or endorsed by Konami, Sony, or any of their affiliates.

Metal Gear, Metal Gear Ac!d, KONAMI, PlayStation, PSP, and all related trademarks, game assets, artwork, audio, characters, and other intellectual property belong to their respective owners.

**MGAcid** does not include any game assets or original game files: no disc image, no copy of the game's executable or data, and no textures, models, audio or video from the game. You must provide the files from your own legally obtained copy of Metal Gear Ac!d to install or build **MGAcid**; the installer checks that copy and accepts only the original release.

Users are solely responsible for obtaining, dumping, extracting, and using their game copy in accordance with the laws applicable in their jurisdiction.

**MGAcid** does not support, provide, link to, or encourage the use of unauthorized or pirated copies of the game.

Any references to the original game or its trademarks are made solely for identification, compatibility, and interoperability purposes.

Screenshots and other depictions of the original game may be used solely to document or demonstrate **MGAcid's** functionality. All depicted third-party game content remains the property of its respective rights holders.

The license covering **MGAcid** applies only to the project's own original code and materials and does not grant any rights to third-party intellectual property.

**MGAcid** provides the software, not the game. You must provide your own legally obtained copy.

## Status: in development

The game boots, plays and saves. The parts below are what has actually been exercised; this is not a finished compatibility report, and anything not listed has not been verified rather than been found broken.

| Works | Rough or unverified |
| --- | --- |
| Booting, the title screen, menus and the stages played so far | The game has not been played end to end, and not every card has been exercised, but no issue has been found so far |
| The game's own save data, in the PSP's format, with the save dialogs the game opens | Importing, exporting and backing up saves target Ac!d's own slots, but no real transfer has been tried yet |
| 3D models, animation, textures, transparency and lighting | Bezier and spline patches are not drawn; nothing yet shows that Ac!d asks for any |
| Sound effects and streamed ATRAC3plus music | |
| Replacement music: your own recordings in place of the game's cues, switchable while a track plays | |
| A replacement texture pack, with optional 2x–4x upscaling and an edge-preserving filter for art drawn texel by texel | |
| Keyboard and gamepad, with an on-screen keyboard for names | |
| An in-game menu (Esc, or L3+R3) for video, audio, controls, saves and folders | |

Multiplayer is **not** supported: the ad hoc networking in the tree comes from the upstream profile and is specific to that game.

## Requirements

Building from source is currently the way to play. It needs:

- Your own copy of the game (see above)
- CMake 3.20 or newer, Ninja and a C++20 compiler
- Python 3
- SDL3, Vulkan and `glslangValidator`
- `make` and a C compiler on macOS and Linux: the build makes its own FFmpeg for the music and the movies. Nothing to install for it on Windows
- A few gigabytes of free memory for the build; the recompiled code is large

## Getting started

```bash
# 1. Configure and build the bootstrap
cmake -S . -B out/mga -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSPRECOMP_PROFILE=mga \
      -DCMAKE_PREFIX_PATH="/path/to/SDL3"
cmake --build out/mga --target MGAcid

# 2. Game data: extracts BOOT.BIN as EBOOT.ELF and links the image
profiles/mga/scripts/prepare_game.sh "/path/to/Metal Gear Acid (USA).iso"

# 3. Generate the recompiled executable and build it
profiles/mga/scripts/generate.sh
cmake -S . -B out/mga && cmake --build out/mga --target MGAcid

# 4. Recompile the game's own libraries and rebuild
profiles/mga/scripts/generate_modules.sh
cmake -S . -B out/mga && cmake --build out/mga --target MGAcid

# 5. Run
out/mga/bin/MGAcid
```

The full instructions, every environment variable and how the stages are recompiled are in [`profiles/mga/README.md`](profiles/mga/README.md). Building on each platform is in [`docs/BUILDING.md`](docs/BUILDING.md).

## Your files

MGAcid keeps three folders beside the executable, so an installation is self-contained and can be moved or carried on a drive:

```text
ms0/        the game's memory stick: its saves
textures/   replacement textures, with a README listing what is expected
music/      replacement music, with a README naming the cue each file replaces
```

Any of the three can be pointed somewhere else from the menu, under System → Folders. An installation that already keeps them in the per-user data directory goes on using it, and says so once at startup.

## Controls

On a gamepad the buttons are where you expect them, and the right stick drives the camera. On a keyboard the arrow keys are the D-pad, I/J/K/L the analog stick, X and Z are ○ and ✕, A and S are □ and △, Q and W are L and R, and Enter is START.

Esc, or both sticks pressed together (L3+R3), opens MGAcid's own menu: it pauses the game and holds the settings, which are kept between runs.

## How it works

The executable is analyzed and every instruction of its code is emitted as C++, which is compiled into the program. The game also loads its own PRX modules — the file system, the sound driver, zlib and every stage — at run time; each is recompiled into its own corpus, and an interpreter covers any code the recompiled set does not reach, so nothing stops the game, it only runs slower there.

Around that code sits a reimplementation of the PSP system: a kernel with threads, semaphores, event flags and timers; disc I/O read straight from the image; a Vulkan renderer for the PSP's graphics engine; software voice mixing for audio; and input from SDL3.

[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) describes the execution model. [`docs/TESTING.md`](docs/TESTING.md) has the smoke test.

## Repository layout

```text
include/psprecomp/   Framework interfaces: runtime, memory, Allegrex state
src/                 Framework: ELF loading, decoder, runtime, interpreter
tools/               Framework: analyzer and C++ code generator
tests/               Framework regression tests
profiles/mga/        Everything specific to Metal Gear Ac!d: host, kernel,
                     renderer, audio, input, configuration and build scripts
docs/                Architecture, archive format, profile guide, source rules
```

The recompiled code itself is generated locally from your copy of the game and is never committed.

## Built on PSPRecomp

**MGAcid** is built on [PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp), a static recompilation framework for PSP software, and started from [Yakumo](https://github.com/TeamGDB/Yakumo), its Monster Hunter Portable 3rd HD port, whose host layer this one was derived from. That port is a separate project; none of its code for its own game is in this repository.

## Credits

- [PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp) — the recompilation framework this project builds on
- [Yakumo](https://github.com/TeamGDB/Yakumo) — the host layer this profile was derived from
- [SDL3](https://www.libsdl.org/) — windowing, input and audio output
- [FFmpeg](https://ffmpeg.org/) — music and movie decoding
- [Vulkan](https://www.vulkan.org/) — rendering
- [stb_truetype](https://github.com/nothings/stb) — font rasterization

## License

The repository is distributed under the MIT License; see [`LICENSE`](LICENSE). Third-party files keep their own notices beside them.
