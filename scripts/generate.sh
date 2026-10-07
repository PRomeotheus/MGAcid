#!/usr/bin/env bash
# Analyze Metal Gear Ac!d's main executable and regenerate the AOT corpus.
#
#   generate.sh [build_dir]
#
# Requires a framework build (psp_analyze/psp_recomp) in build_dir, default
# out/build/$psp_preset. Output: analysis and generated.
set -euo pipefail

profile_dir="$(cd "$(dirname "$0")/.." && pwd)"
repo_dir="$profile_dir"
# framework_dir, psp_preset and build_default (scripts/framework.sh).
. "$repo_dir/scripts/framework.sh"
build_dir="${1:-$build_default}"
elf="$profile_dir/game/EBOOT.ELF"

if [[ ! -f "$elf" ]]; then
    echo "missing $elf; run scripts/prepare_game.sh first" >&2
    exit 1
fi

exe=""
[[ "${OS:-}" == "Windows_NT" ]] && exe=".exe"

cmake --build "$build_dir" --target psp_analyze psp_recomp
mkdir -p "$profile_dir/analysis"
"$build_dir/psp_analyze$exe" "$elf" "$profile_dir/analysis/report.json"
# Regenerate in place: psp_recomp rewrites only units whose text changed and
# removes units that no longer exist.
"$build_dir/psp_recomp$exe" "$elf" --auto "$profile_dir/generated"
