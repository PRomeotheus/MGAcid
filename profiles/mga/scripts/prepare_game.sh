#!/usr/bin/env bash
# Populate profiles/mga/game from the user's own disc image of Metal Gear Ac!d
# (ULUS-10006).
#
#   prepare_game.sh <image.iso>
#
# The disc's PSP_GAME/SYSDIR/BOOT.BIN is the game's executable as a plain ELF,
# so it is extracted as EBOOT.ELF with no decryption step.
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <image.iso>" >&2
    exit 2
fi

profile_dir="$(cd "$(dirname "$0")/.." && pwd)"
game_dir="$profile_dir/game"
iso="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
python=python3
command -v python3 > /dev/null || python=python

mkdir -p "$game_dir/ms0"
extract_dir="$game_dir/.extract"
"$python" "$profile_dir/tools/extract_iso.py" "$iso" "$extract_dir" PSP_GAME/SYSDIR/BOOT.BIN
mv -f "$extract_dir/PSP_GAME/SYSDIR/BOOT.BIN" "$game_dir/EBOOT.ELF"
rm -rf "$extract_dir"

expected_sha256="f0886cc094dcab4001383fdae5c347cf0365be1deddb88008c1ee66ea6299c29"
if command -v sha256sum > /dev/null; then
    actual_sha256="$(sha256sum "$game_dir/EBOOT.ELF" | cut -d' ' -f1)"
else
    actual_sha256="$(shasum -a 256 "$game_dir/EBOOT.ELF" | cut -d' ' -f1)"
fi
if [[ "$actual_sha256" != "$expected_sha256" ]]; then
    echo "warning: EBOOT.ELF sha256 $actual_sha256 does not match the supported executable" >&2
fi

# The image is read in place. A symbolic link where the platform makes one,
# otherwise (Git Bash without Developer Mode) a copy.
ln -sf "$iso" "$game_dir/disc.iso"
echo "game data prepared in $game_dir"
