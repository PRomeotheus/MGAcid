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
# MGA_STAGE_BASE is the address the corpus is generated for, and it matters:
# a corpus is only valid at its own address, and Ac!d does not load every stage
# at the same one. The boot sequence -- init, title, intermission -- loads while
# little else is allocated and lands at 0x09B38700; a gameplay stage loads with
# the game's pools in place and lands at 0x09B61100. Collect a dump at each with
# MGA_SWEEP_AFTER (see host/kernel/module_sweep.hpp) and run this once per base
# with the stages that use it. The module loader prints "(interpreted)" rather
# than "(recompiled)" for a stage whose corpus is at the wrong address, so a
# mistake here is visible in the first run rather than silent.
#
# Needs game/disc.iso (scripts/prepare_game.sh) and a framework build in
# build_dir (default out/build/$psp_preset). Output: modules/<prefix>/, which is
# derived from the game and ignored by Git. Reconfigure and rebuild afterwards.
#
# The load addresses are fixed because the module manager allocates in the same
# order on every boot. If a load address ever changes, the loader reports the
# mismatch and interprets that module until this script is run again.
set -euo pipefail

profile_dir="$(cd "$(dirname "$0")/.." && pwd)"
repo_dir="$profile_dir"
# framework_dir, psp_preset and build_default (scripts/framework.sh).
. "$repo_dir/scripts/framework.sh"
build_dir="${1:-$build_default}"
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
link="$framework_dir/host/tools/stage_link.py"

# generate <prefix> <prx> <module name> <load address> [file to generate from]
#
# Returns non-zero rather than leaving, and shows what psp_recomp said. Its
# output used to go to /dev/null and one failure ended the whole run, so a
# single awkward stage cost the other forty-three and took its reason with it.
failed=""
generate() {
    local prefix="$1" prx="$2" name="$3" base="$4" source="${5:-$2}"
    local out="$profile_dir/modules/$prefix"
    local log="$profile_dir/modules/$prefix.log"
    if ! "$build_dir/psp_recomp$exe" "$source" --auto "$out" "$base" --prefix "$prefix" > "$log" 2>&1; then
        echo "  $prefix: psp_recomp failed --" >&2
        tail -12 "$log" | sed 's/^/      /' >&2
        failed="$failed $prefix"
        return 1
    fi
    rm -f "$log"
    printf 'name=%s\nbase=%s\nprefix=%s\nhash=%s\n' "$name" "$base" "$prefix" \
        "$("$python" "$link" hash "$prx")" > "$out/module.txt"
    echo "  $prefix: $name at $base"
}

for entry in "${modules[@]}"; do
    read -r prefix path name base <<< "$entry"
    "$python" "$framework_dir/host/tools/extract_iso.py" "$iso" "$extract_dir" "$path" > /dev/null
    generate "$prefix" "$extract_dir/$path" "$name" "$base"
done

# Stages. The game links a stage after loading it (symbol table by hash), so a
# corpus can only be generated from the module as it is in memory afterwards.
# Play with MGA_DUMP_MODULES=<dir> to collect those dumps; each is named after
# the hash of the PRX it came from, which is how they are matched up here.
#
# A stage is named after the address its corpus is for as well as after itself,
# so the same stage can have one for each address the game loads it at. It does
# happen: intermission appears during the boot sequence, at the low address,
# and again between gameplay stages, at the high one. The module loader matches
# on name, hash and address together and takes whichever fits.
stage_base="${MGA_STAGE_BASE:-0x09B38700}"
base_tag=""
[[ "$stage_base" != "0x09B38700" ]] && base_tag="_$(printf '%x' $((stage_base)))"
stages="${MGA_STAGES:-all}"
dump_dir="${MGA_DUMP_DIR:-$profile_dir/game/dumps}"
if [[ "$stages" != "none" ]]; then
    "$python" "$framework_dir/host/tools/extract_iso.py" "$iso" "$extract_dir" PSP_GAME/USRDIR/stage/ > /dev/null
    missing=""
    for prx in "$extract_dir"/PSP_GAME/USRDIR/stage/*/*.prx; do
        stage="$(basename "$prx" .prx)"
        if [[ "$stages" != "all" && " $stages " != *" $stage "* ]]; then continue; fi
        hash="$("$python" "$link" hash "$prx")"
        dump="$dump_dir/mgp_stage_$hash.bin"
        rm -rf "$profile_dir/modules/stage_$stage$base_tag"
        if [[ ! -f "$dump" ]]; then
            missing="$missing $stage"
            continue
        fi
        linked="$extract_dir/$stage.linked.prx"
        # The stage's own archive carries <stage>.rlc: the relocations the game
        # applies after starting the module, against the executable rather than
        # against itself. A dump taken from a running game already has them; one
        # from MGA_SWEEP_MODULES does not, and stage_link.py tells the two apart
        # rather than guessing.
        zar="$(dirname "$prx")/_zar"
        if [[ -f "$zar" ]]; then
            "$python" "$link" fromdump "$prx" "$dump" "$linked" "$zar" "$stage"
        else
            "$python" "$link" fromdump "$prx" "$dump" "$linked" > /dev/null
        fi
        generate "stage_$stage$base_tag" "$prx" mgp_stage "$stage_base" "$linked" || true
    done
    if [[ -n "$missing" ]]; then
        echo "no dump yet for:$missing"
        echo "  play with MGA_DUMP_MODULES=$dump_dir until those stages have been loaded,"
        echo "  then run this script again."
    fi
fi
if [[ -n "$failed" ]]; then
    echo "these could not be generated and will be interpreted:$failed" >&2
fi
echo "module corpora in $profile_dir/modules; now run: cmake -S . -B out/build/$psp_preset && cmake --build out/build/$psp_preset --target MGAcid"
