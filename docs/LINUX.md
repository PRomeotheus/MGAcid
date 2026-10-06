# Playing on Linux and the Steam Deck

This guide is for players using a prebuilt release. To build MGAcid yourself instead, follow [`profiles/mga/README.md`](../profiles/mga/README.md); building from source stays fully supported.

A release does not include any game assets or original game files: no disc image, no copy of the game's executable or data, and no textures, models, audio or video from the game. You must provide the files from your own legally obtained copy of **Metal Gear Ac!d** (`ULUS-10006`): its disc image, as an uncompressed `.iso` file. The first start checks that copy, accepts only the original release, and sets the game up from it.

Each [release](https://github.com/PRomeotheus/MGAcid/releases) has two Linux downloads:

| File | For |
| --- | --- |
| `mgacid-<version>-linux-x86_64.flatpak` | The Steam Deck, and any distribution with Flatpak. Recommended. |
| `mgacid-<version>-linux-x86_64.tar.gz` | Any x86-64 distribution, without installing anything |

`SHA256SUMS` on the same page lists the checksum of each file; `sha256sum -c SHA256SUMS --ignore-missing` in the download folder checks them.

## Steam Deck and Flatpak

### Install

1. Switch to Desktop Mode (hold the power button, then *Switch to Desktop*).
2. Download the `.flatpak` file from the release page into Downloads.
3. Open Konsole and run:

   ```bash
   flatpak install --user ~/Downloads/mgacid-*-linux-x86_64.flatpak
   ```

   Answer `y`. Flatpak also downloads the Freedesktop runtime (`org.freedesktop.Platform` 25.08) from Flathub if it is not installed yet; SteamOS usually has it already.

MGAcid then appears in the application menu under *Games*.

### First start

Start MGAcid from the application menu, or with `flatpak run io.github.promeotheus.MGAcid`. The setup runs in the game's window and works with the Deck's controls alone. The buttons each screen uses are shown at its bottom; which face button confirms follows the button layout set in MGAcid's menu.

1. The welcome screen says what is needed. Confirm to go on.
2. Pick your disc image in MGAcid's file browser. The row of places at the top holds Home, Downloads and every SD card or USB drive (on the Deck, the SD card shows under its label). Confirm opens a folder or picks the file, back goes up a folder, and Ⓨ switches between `.iso` files and all files. *System dialog…* opens the desktop's own file dialog instead; it is not offered in Game Mode.
3. MGAcid checks the image. A different release or region, a modified image or a compressed `.cso` gets a screen that says so.
4. Choose whether to **copy** the image (the default, about 1.3 GB in your home folder) or **use it where it is**. Copying keeps the game working if the original is moved or its SD card is removed; using it in place saves the space, but the image has to be there every time you play.
5. MGAcid copies the image and prepares the game from it, then starts the game.

Later starts go straight to the game. Esc, or L3+R3 (both sticks pressed in), opens MGAcid's menu with the settings; *Set up game data again…* there repeats the setup.

### Game Mode

Add MGAcid to Steam once, in Desktop Mode:

1. Open Steam, then *Games* → *Add a Non-Steam Game to My Library…*
2. Tick **MGAcid** in the list and click *Add Selected Programs*. Steam launches it through Flatpak by itself.
3. Return to Game Mode. MGAcid is in the library under *Non-Steam*.

In Game Mode Steam presents the Deck's controls to the game as a gamepad; if a button does something unexpected, open the controller settings for MGAcid and choose the *Gamepad* layout. The first setup also works in Game Mode, through MGAcid's own file browser.

To change the name or artwork Steam shows, open the shortcut's *Properties* in Steam.

### Where your data lives

Everything MGAcid keeps is in the Flatpak's own data directory:

```text
~/.var/app/io.github.promeotheus.MGAcid/data/MGAcid/MGA/
    EBOOT.ELF      the game's executable, prepared from your disc image
    disc.iso       the copy of your disc image (absent when you use it in place)
    settings.ini   where the image is, and the settings from MGAcid's menu
    ms0/           the memory stick
        PSP/SAVEDATA/ULUS10006001/   your saves, one folder per slot
```

It survives updates and is removed only if you ask for it (see [Uninstall](#uninstall)). To keep your saves safe, use *Back up saves…* in MGAcid's menu (System section), which copies them to `save-backups` in this directory or to a folder you choose, or back up `ms0` yourself.

### Import a save from a PSP

Saves use the PSP's own format, so a save from a PSP's memory stick or from PPSSPP works unchanged. MGAcid's menu imports it, with a gamepad:

1. Put the save where MGAcid can see it: connect the memory stick or SD card, or copy the folder into your home folder. On a PSP's memory stick *Metal Gear Ac!d* keeps one folder per save slot under `PSP/SAVEDATA`, named `ULUS10006000`, `ULUS10006001` and so on.
2. In the game, open the menu (L3+R3, or Esc) and go to **System → Import save…**.
3. Open the save folder, or choose *Import from this folder* on a folder that holds several, such as the memory stick's `PSP/SAVEDATA`. Removable drives are in the row of places at the top.
4. Check what is shown, with the save it replaces, and confirm. The replaced save is kept in `ms0/PSP/SAVEDATA/.backup/`, not deleted.
5. Choose **Restart now**: the game reads saves at the title screen, which then offers the imported one.

**System → Back up saves…** copies your saves to `save-backups` in the data directory above, and **Open the saves folder** and **Open the backups folder** show where they are. The Flatpak reads your home folder and removable drives, but it writes only to its own data directory and to your **Downloads** folder. So *Export save…* and backups work when you pick Downloads or the backups folder; any other folder fails. The [profile README](../profiles/mga/README.md#importing-a-save-from-a-psp) describes the checks and the backups.

By hand, with MGAcid closed, copy the folder into `~/.var/app/io.github.promeotheus.MGAcid/data/MGAcid/MGA/ms0/PSP/SAVEDATA/`, keeping a copy of any `ULUS10006*` folders already there first.

```bash
mkdir -p ~/.var/app/io.github.promeotheus.MGAcid/data/MGAcid/MGA/ms0/PSP/SAVEDATA
cp -r /path/to/memory-stick/PSP/SAVEDATA/ULUS10006* ~/.var/app/io.github.promeotheus.MGAcid/data/MGAcid/MGA/ms0/PSP/SAVEDATA/
```

In Dolphin, the file manager, Ctrl+H shows hidden folders such as `.var` in your home folder.

### Update

Download the new `.flatpak` and install it the same way:

```bash
flatpak install --user ~/Downloads/mgacid-<new version>-linux-x86_64.flatpak
```

It replaces the old version. Your data, settings and saves stay, and the Steam shortcut keeps working.

### Uninstall

```bash
flatpak uninstall --user io.github.promeotheus.MGAcid
```

This keeps your data. To remove it as well, **including your saves and the copied disc image**, add `--delete-data`, or delete `~/.var/app/io.github.promeotheus.MGAcid`. `flatpak uninstall --unused` then removes a runtime nothing else uses. Remove the Steam shortcut from the library as with any game.

### Multiplayer

Ad hoc multiplayer works in the Flatpak as it does elsewhere: open **Network** in MGAcid's menu (Esc, or L3+R3) to host a session or join one, on your home network, over a VPN, or through an ad hoc server. The [profile README](../profiles/mga/README.md#multiplayer-ad-hoc) explains how. SteamOS has no firewall by default. On a desktop with one, allow TCP 27312 and 27313 to host, and UDP 27314 for sessions on your network to show up. On a VPN without broadcast, such as Tailscale, type the host's address instead of waiting for it to be listed.

### Permissions

The Flatpak asks for as little as the game needs:

| Permission | Why |
| --- | --- |
| Wayland, with X11 as a fallback; X11 shared memory | The game window |
| GPU (`dri`) | Vulkan rendering |
| Input devices | Gamepads, including the Deck's built-in controls and Steam Input in Game Mode |
| PulseAudio (PipeWire serves it on current systems) | Sound |
| Network | Ad hoc multiplayer: joining a server, hosting a session with the built-in server, and finding sessions on the local network |
| Home folder, read-only | Finding your disc image with the gamepad file browser. The desktop's file dialog needs no permission, but it is not available in Game Mode. |
| `/run/media` and `/media`, read-only | Disc images on SD cards and USB drives |
| Downloads folder, read and write | Where *Export save…* and *Back up saves…* can write outside the app's own data directory |

MGAcid writes only to its own data directory. It uses the network only for multiplayer, when you host or join a session. If your image is somewhere else, for example on a second drive mounted under `/mnt`, give MGAcid read access to it:

```bash
flatpak override --user --filesystem=/mnt/games:ro io.github.promeotheus.MGAcid
```

## Other distributions: the tarball

The tarball runs without installing anything. It needs:

- x86-64 Linux with glibc 2.29 or newer: Ubuntu 20.04, Debian 11, Fedora 31, SteamOS 3 or anything newer
- A Vulkan driver and the Vulkan loader (`libvulkan.so.1`): Mesa on AMD and Intel, or NVIDIA's driver
- Wayland or X11, and PipeWire, PulseAudio or ALSA for sound

SDL3, FFmpeg and a Japanese font come with it.

```bash
tar -xzf mgacid-<version>-linux-x86_64.tar.gz
cd mgacid-<version>-linux-x86_64
./mgacid
```

Always start it through `./mgacid`, which gives the game the 64 MiB stack it needs. The setup is the same as in the Flatpak. Its data, settings and saves are in `~/.local/share/MGAcid/MGA/` (under `$XDG_DATA_HOME` if you set it), with the saves in `ms0/PSP/SAVEDATA/`; import a PSP save there as described above.

To set up from a terminal instead of the setup screens:

```bash
./mgacid --install /path/to/your.iso              # copy the image
./mgacid --install /path/to/your.iso --in-place   # use it where it is
```

**Update:** unpack the new version and delete the old folder. Your data stays in `~/.local/share/MGAcid`.
**Uninstall:** delete the folder, and `~/.local/share/MGAcid` to remove your data and saves too.

To add the tarball to Steam, choose *Browse…* in *Add a Non-Steam Game* and pick the `mgacid` script.

## Troubleshooting

- **Messages from the game.** Start it from a terminal (`flatpak run io.github.promeotheus.MGAcid`, or `./mgacid`) to see what it prints.
- **No gamepad in the Flatpak** on a system with Flatpak older than 1.15.6, which does not know the input-device permission: `flatpak override --user --device=all io.github.promeotheus.MGAcid`.
- **The game's text is missing.** MGAcid draws it with a Japanese font from the system and falls back to the one it ships. `MGA_FONT=/path/to/font.ttf` picks another; in the Flatpak, set it with `flatpak override --user --env=MGA_FONT=... io.github.promeotheus.MGAcid`.
- Every other setting is described in the [profile README](../profiles/mga/README.md#configuration). In the Flatpak, pass environment variables with `flatpak run --env=NAME=value io.github.promeotheus.MGAcid`.

Report problems with the [test report form](https://github.com/PRomeotheus/MGAcid/issues/new/choose); [`TESTING.md`](TESTING.md) describes what to check.
