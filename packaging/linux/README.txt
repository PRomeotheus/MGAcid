MGAcid for Linux (x86-64)
=========================

A native port of Metal Gear Ac!d, built with PSPRecomp. MGAcid does not
include any game assets or original game files: no disc image, no copy of the
game's executable or data, and no textures, models, audio or video from the
game. You must provide the files from your own legally obtained copy of the
game (ULUS-10006): its disc image. The first start checks that copy and
accepts only the USA release.

Start it with

    ./mgacid

The first start asks for your disc image, copies it into MGAcid's data
directory (~/.local/share/MGAcid/MGA) and prepares the game from it. After
that the game starts straight away.

Your saves, and the folders for replacement textures and music, are kept
beside the mgacid program, so this folder can be moved or carried on a drive:
ms0/, textures/ and music/, each with a README saying what belongs in it. Any
of the three can be pointed elsewhere from the menu, under System > Folders.
Where the program sits in a read-only place -- the Flatpak, or a system-wide
install -- they go to the data directory above instead, and the game says so
once at startup.

    ./mgacid --install /path/to/image.iso   set up from a terminal instead
    ./mgacid --help                         all options

Needs: an x86-64 Linux with glibc 2.31 or newer, a Vulkan driver (Mesa on
AMD and Intel, or NVIDIA's), and Wayland or X11.

On a Steam Deck: copy this folder anywhere in your home directory, then add
./mgacid to Steam as a non-Steam game. Everything it needs is in this folder;
nothing has to be installed on SteamOS. Set up from Desktop Mode the first
time, since the setup asks for your disc image.

Third-party software and its licenses: licenses/THIRD_PARTY_NOTICES.md
