# Releasing

A release gives players the program ready to run: the executable with the recompiled game code built in and the 355 overlay libraries. Like the repository, it does not include any game assets or original game files: no disc image, no copy of the game's executable or data, and no textures, models, audio or video from the game. On first start it asks for the files from the player's own legally obtained copy, the disc image, checks it, accepts only the original release, and prepares the game from it (see [Installer](../profiles/mga/README.md#installer)).

Releases are built by maintainers, not by CI: the recompiled code is generated from the game's executable, so a build needs a copy of the game. The game data stays on the maintainer's machine. Every artifact is checked for it before it is published.

So far there are Linux builds. macOS and Windows follow in [#29](https://github.com/PRomeotheus/MGAcid/issues/29).

## Linux

`profiles/mga/scripts/release_linux.sh` builds both Linux artifacts from a checkout:

| Artifact | Contents |
| --- | --- |
| `mgacid-<version>-linux-x86_64.flatpak` | Flatpak bundle, app ID `io.github.promeotheus.MGAcid`, on the `org.freedesktop.Platform` 25.08 runtime. The main download, and the one for the Steam Deck. |
| `mgacid-<version>-linux-x86_64.tar.gz` | Portable tarball: the `mgacid` launcher, `MGAcid`, `overlays/`, `lib/` (SDL3, FFmpeg), `fonts/`, `licenses/` |
| `ffmpeg-<version>.tar.xz` | The unmodified source of the FFmpeg both artifacts contain, published next to them as the LGPL asks |
| `SHA256SUMS` | Checksums of the three files above |
| `BUILDINFO.txt` | Commit, build environment, glibc requirement and the FFmpeg configure line, for the release notes |

Flathub cannot host the Flatpak: its builders compile everything from source, and this build needs the game's executable.

### What the script does

1. Checks the game data a source build uses: `profiles/mga/game/EBOOT.ELF` (the SHA-256 in the profile README) and `profiles/mga/game/disc.iso`. `prepare_game.sh` sets both up; `MGAcid --install` produces the executable from the image.
2. Starts the Steam Runtime 3 "sniper" SDK container (Debian 11, glibc 2.31), pinned by digest in `packaging/linux/sources.sh`, and runs `packaging/linux/build_in_sdk.sh` in it, which
   - builds SDL3 from a source archive pinned by version and SHA-256 in `sources.sh`;
   - configures MGAcid with `-DMGA_RELEASE=ON`, `-DMGA_FFMPEG=bundled` and GCC 14. The bundled FFmpeg is built by `cmake/FFmpeg.cmake` with only `libavcodec` and `libavutil` and the ATRAC3, ATRAC3plus and H.264 decoders, and configuring stops unless FFmpeg reports the LGPL with no GPL or non-free parts. The build then generates the recompiled code, builds `MGAcid` and runs the save-data self-tests;
   - builds all overlay libraries with `build_overlays.sh`;
   - stages the program with the libraries it needs, the fallback Japanese font and the license texts, strips it, and checks that every library resolves, that no binary needs `libstdc++.so`, and which glibc version it needs.
3. Packs the tarball from the staged tree, with the launcher, in a reproducible order and with fixed timestamps.
4. Builds the Flatpak with `flatpak-builder` (the `org.flatpak.Builder` Flatpak) from `packaging/linux/io.github.promeotheus.MGAcid.yml`, which packs the same staged tree, and exports it as a single-file bundle.
5. Unpacks both artifacts again and refuses them if any file is named like game data (`EBOOT*`, `*.iso`, `*.cso`, `*.bin`, `*.prx`, `*.pbp`, `*.elf`, `DATA.BIN`, `PARAM.SFO`, `SAVEDATA`, `ms0`, …) or looks like it by content: a MIPS ELF, a PBP, an encrypted PSP module, a `PARAM.SFO` or an ISO 9660 image.
6. Prints the checksums.

### Requirements

- An x86-64 Linux machine, a Steam Deck in Desktop Mode included, with `podman` (or `docker`), `flatpak` with the Flathub remote, `curl` and `ostree`. The script installs `org.flatpak.Builder` and the Freedesktop 25.08 SDK for the user.
- The game data, as above.
- About 25 GB of free space. The first build takes one to two hours; later ones reuse the compiler cache, the built dependencies and the finished overlay libraries.

### Build

```bash
profiles/mga/scripts/release_linux.sh --version 0.2.0
```

Without `--version`, the artifacts are named after `git describe`. `--jobs N` sets the number of parallel compiles (default 4; each generated unit needs over a gigabyte of memory). `--skip-build` packs the staged build of a previous run again, `--no-flatpak` and `--no-tarball` leave one artifact out. Everything is kept in `out/release-linux/`, the artifacts in `out/release-linux/dist/`. Build from a clean checkout of the commit you release: the version shown in MGAcid's menu comes from `git describe` of the checkout.

On a Steam Deck, keep it plugged in and stop it from sleeping during the build, which runs the CPU flat out for hours: a Deck that suspends or runs flat in the middle leaves the build to be resumed, and a hard power-off can damage the half-written files, so after one delete `out/release-linux/build` and let the compiler cache catch up. Over SSH, start the build as a user service so that it also survives the connection closing, and use three jobs to keep the temperature down:

```bash
systemd-run --user --unit=mgacid-release --collect \
    systemd-inhibit --what=sleep:idle --why="MGAcid release build" \
    "$PWD/profiles/mga/scripts/release_linux.sh" --jobs 3
journalctl --user -fu mgacid-release     # follow it
```

### Check

On a machine with the game, before publishing.

**Never touch a player's own installation or data.** `~/.var/app/io.github.promeotheus.MGAcid` is shared by every installation of the Flatpak, and `~/.local/share/MGAcid` by every tarball: they hold the player's settings, copied disc image and saves. So on a machine where someone plays:

- never uninstall, reinstall or replace an installed MGAcid, and never use `--delete-data`;
- never delete or overwrite anything in those directories, or in Steam's files;
- run every check with its data in a throwaway directory of your own, and clean up only what you created there.

```bash
check=$(mktemp -d)

# The tarball: set up from the disc image alone into a temporary data
# directory, then boot without a window.
tar -xzf out/release-linux/dist/mgacid-*-linux-x86_64.tar.gz -C "$check"
export MGA_DATA_DIR="$check/data"
"$check"/mgacid-*/mgacid --install /path/to/your.iso
MGA_NO_RENDER=1 MGA_NO_AUDIO=1 timeout 120 "$check"/mgacid-*/mgacid
unset MGA_DATA_DIR

# The Flatpak, without installing it: run the committed tree from the build's
# repository with the same runtime, pointing its data at the temporary
# directory. This leaves any installed MGAcid and its data alone.
ostree --repo=out/release-linux/flatpak/repo checkout --user-mode \
    app/io.github.promeotheus.MGAcid/x86_64/stable "$check/app"
flatpak run --command=bash --filesystem="$check" --filesystem=/path/to/iso-folder:ro \
    --env=MGA_DATA_DIR="$check/flatpak-data" --env=MGA_NO_RENDER=1 --env=MGA_NO_AUDIO=1 \
    org.freedesktop.Platform//25.08 -c \
    "$check/app/files/lib/mgacid/mgacid --install /path/to/your.iso &&
     timeout 120 $check/app/files/lib/mgacid/mgacid"

rm -rf "$check"
```

This checks the Flatpak's files on its runtime, not its sandbox permissions. Check those on a machine where MGAcid is not installed, with `flatpak install --user` of the bundle and `flatpak run` using `--env=MGA_DATA_DIR=` pointing at a temporary directory. Uninstall it afterwards without `--delete-data`, and delete only that temporary directory.

The boot should print the executable's SHA-256, `Functions:` with a non-zero count, and overlays being installed as the game loads them; the run then ends at the timeout. Then play the release once with a window, a gamepad, a save and a reload, following [`TESTING.md`](TESTING.md), on a Steam Deck in Game Mode as well.

### Publish

Create the release on GitHub with the tag of the commit that was built, and attach every file from `out/release-linux/dist/` except `BUILDINFO.txt`, whose contents go into the release notes. The FFmpeg source archive must stay attached to every release whose artifacts contain that FFmpeg: that is the source offer in [`THIRD_PARTY_NOTICES.md`](../profiles/mga/packaging/THIRD_PARTY_NOTICES.md).

### Updating a bundled component

Change its version and SHA-256 where it is pinned, `packaging/linux/sources.sh` for SDL3 and the font or `cmake/FFmpeg.cmake` for FFmpeg, and the matching entry in `THIRD_PARTY_NOTICES.md`; the script refuses to pack when they disagree. A new FFmpeg configure option goes into both as well. To move to a newer SDK, update its digest in `sources.sh`; to move to a newer Flatpak runtime, update `FLATPAK_RUNTIME_VERSION` and `runtime-version` in the manifest together.
