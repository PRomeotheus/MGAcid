MGAcid for Windows (x86-64)
===========================

A native port of Metal Gear Ac!d, built with PSPRecomp. MGAcid does not
include any game assets or original game files: no disc image, no copy of the
game's executable or data, and no textures, models, audio or video from the
game. You must provide the files from your own legally obtained copy of the
game (ULUS-10006): its disc image. The first start checks that copy and
accepts only the USA release.

Start it by running MGAcid.exe.

The first start asks for your disc image, copies it into MGAcid's data
directory (%APPDATA%\MGAcid\MGA) and prepares the game from it. After that
the game starts straight away.

Your own files are kept beside MGAcid.exe, so this folder can be moved to
another drive or carried on one without losing them:

    ms0\        your saves
    textures\   replacement textures; the README inside lists what is expected
    music\      replacement music; the README inside names the cue each
                file replaces, and the soundtrack track it came from

Any of the three can be pointed somewhere else from the menu, under
System > Folders.

Open the menu with Esc, or with both sticks pressed together (L3+R3) on a
gamepad. It pauses the game and holds the settings, which are kept between
runs.

    MGAcid.exe --install path\to\image.iso   set up from a terminal instead
    MGAcid.exe --help                        all options

The game opens no console window. Anything it would have printed goes to
MGAcid.log beside the executable, replaced each run; send that file with a
bug report.

Needs: 64-bit Windows 10 or newer, and a Vulkan driver (the normal driver for
your graphics card has one).

Third-party software and its licenses: licenses\THIRD_PARTY_NOTICES.md
