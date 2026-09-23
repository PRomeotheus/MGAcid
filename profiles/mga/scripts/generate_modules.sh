#!/usr/bin/env bash
# Recompile the game's own PRX libraries at the addresses the host's module
# loader places them, so they run as native code instead of in the interpreter.
#
#   generate_modules.sh [build_dir]
#
# MGA_STAGES selects the stage modules (PSP_GAME/USRDIR/stage/<name>/<name>.prx)
# to recompile as well: "all" (the default), "none", or names such as
# "title prologue st01". Stages are generated from their code as the game
# patches it after loading (tools/stage_link.py).
#
# Needs game/disc.iso (scripts/prepare_game.sh) and a framework build in
# build_dir (default out/mga). Output: profiles/mga/modules/<prefix>/, which is
# derived from the game and ignored by Git. Reconfigure and rebuild afterwards.
#
# The load addresses are fixed because the module manager allocates in the same
# order on every boot. If a load address ever changes, the loader reports the
# mismatch and interprets that module until this script is run again.
set -euo pipefail

profile_dir="$(cd "$(dirname "$0")/.." && pwd)"
repo_dir="$(cd "$profile_dir/../.." && pwd)"
build_dir="${1:-$repo_dir/out/mga}"
iso="$profile_dir/game/disc.iso"
exe=""
[[ "${OS:-}" == "Windows_NT" ]] && exe=".exe"
python=python3
command -v python3 > /dev/null || python=python

if [[ ! -e "$iso" ]]; then
    echo "missing $iso; run scripts/prepare_game.sh first" >&2
    exit 1
fi

# prefix  file on the disc                        module name  load address
modules=(
    "kjfs   PSP_GAME/USRDIR/modules/kjfs.prx     KCEJ_FS     0x09AF6000"
    "zlib   PSP_GAME/USRDIR/modules/zlibdec.prx  ZLIB        0x09B05A00"
    "sound  PSP_GAME/USRDIR/modules/sound.prx    KCEJ_SOUND  0x09B09B00"
)

cmake --build "$build_dir" --target psp_recomp
extract_dir="$profile_dir/game/modules"
mkdir -p "$extract_dir"
link="$profile_dir/tools/stage_link.py"

# generate <prefix> <prx> <module name> <load address> [file to generate from]
generate() {
    local prefix="$1" prx="$2" name="$3" base="$4" source="${5:-$2}"
    local out="$profile_dir/modules/$prefix"
    "$build_dir/psp_recomp$exe" "$source" --auto "$out" "$base" --prefix "$prefix" > /dev/null 2>&1 || { echo "psp_recomp failed for $prefix" >&2; exit 1; }
    printf 'name=%s\nbase=%s\nprefix=%s\nhash=%s\n' "$name" "$base" "$prefix" \
        "$("$python" "$link" hash "$prx")" > "$out/module.txt"
    echo "  $prefix: $name at $base"
}

for entry in "${modules[@]}"; do
    read -r prefix path name base <<< "$entry"
    "$python" "$profile_dir/tools/extract_iso.py" "$iso" "$extract_dir" "$path" > /dev/null
    generate "$prefix" "$extract_dir/$path" "$name" "$base"
done

# Stages. The game links a stage after loading it (symbol table by hash), so a
# corpus can only be generated from the module as it is in memory afterwards.
# Play with MGA_DUMP_MODULES=<dir> to collect those dumps; each is named after
# the hash of the PRX it came from, which is how they are matched up here.
# Every stage loads at the same address, once the one before it is unloaded.
stage_base=0x09B38700
stages="${MGA_STAGES:-all}"
dump_dir="${MGA_DUMP_DIR:-$profile_dir/game/dumps}"
if [[ "$stages" != "none" ]]; then
    "$python" "$profile_dir/tools/extract_iso.py" "$iso" "$extract_dir" PSP_GAME/USRDIR/stage/ > /dev/null
    missing=""
    for prx in "$extract_dir"/PSP_GAME/USRDIR/stage/*/*.prx; do
        stage="$(basename "$prx" .prx)"
        if [[ "$stages" != "all" && " $stages " != *" $stage "* ]]; then continue; fi
        hash="$("$python" "$link" hash "$prx")"
        dump="$dump_dir/mgp_stage_$hash.bin"
        rm -rf "$profile_dir/modules/stage_$stage"
        if [[ ! -f "$dump" ]]; then
            missing="$missing $stage"
            continue
        fi
        linked="$extract_dir/$stage.linked.prx"
        "$python" "$link" fromdump "$prx" "$dump" "$linked" > /dev/null
        generate "stage_$stage" "$prx" mgp_stage "$stage_base" "$linked"
    done
    if [[ -n "$missing" ]]; then
        echo "no dump yet for:$missing"
        echo "  play with MGA_DUMP_MODULES=$dump_dir until those stages have been loaded,"
        echo "  then run this script again."
    fi
fi
echo "module corpora in $profile_dir/modules; now run: cmake -S . -B out/mga && cmake --build out/mga --target MGAcid"
