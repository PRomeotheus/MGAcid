#!/usr/bin/env bash
# Build a Windows release: a portable .zip.
#
#   release_windows.sh [--version VERSION] [--jobs N] [--sdl3 DIR] [--skip-build]
#
# Run it from Git Bash started from the x64 Native Tools prompt, so cl.exe and
# ninja are on PATH. From a checkout and the recompiled code generated from
# your own copy of the game (profiles/mga/generated, see generate.sh), this
# configures a release build, stages the program with the libraries it needs,
# packs the zip, checks that it contains no game data, and prints its SHA-256.
# Everything lands in out/release-windows; the artifact in its dist directory.
#
# A release build differs from a developer one in ways that matter here:
# MGA_RELEASE=ON drops the fallback to this checkout's game directory, so the
# executable carries no path from the machine it was built on, and builds for
# the GUI subsystem, so the game opens no console. What it would have printed
# goes to MGAcid.log beside the executable.
#
#   --version VERSION  name the artifact after VERSION instead of git describe
#   --jobs N           parallel compile jobs (default 4; each unit wants ~1 GB)
#   --sdl3 DIR         the SDL3 cmake directory, if it is not already on
#                      CMAKE_PREFIX_PATH or in MGA_SDL3
#   --skip-build       pack the staged build from a previous run as it is
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
profile_dir="$(cd "$script_dir/.." && pwd)"
repo_dir="$(cd "$profile_dir/../.." && pwd)"
# shellcheck source=no_game_data.sh
source "$script_dir/no_game_data.sh"

work="${MGA_WORK:-$repo_dir/out/release-windows}"
build="$work/build"
stage="$work/stage/MGAcid"
dist="$work/dist"

version=""
jobs="${MGA_JOBS:-4}"
sdl3="${MGA_SDL3:-}"
skip_build=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) version="${2:?--version needs a value}"; shift 2 ;;
        --jobs) jobs="${2:?--jobs needs a value}"; shift 2 ;;
        --sdl3) sdl3="${2:?--sdl3 needs a value}"; shift 2 ;;
        --skip-build) skip_build=1; shift ;;
        -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
if [[ -z "$version" ]]; then
    version="$(git -C "$repo_dir" describe --tags --always --dirty)"
    version="${version#v}"
fi
name="mgacid-$version-windows-x86_64"

fail() { echo "error: $*" >&2; exit 1; }
step() { printf '\n== %s\n' "$*"; }

if [[ $skip_build -eq 0 ]]; then
    step "MGAcid $version for Windows"
    # The recompiled code is made from the maintainer's own copy of the game and
    # is never committed, so a release cannot be built from a bare checkout.
    [[ -d "$profile_dir/generated" ]] ||
        fail "no recompiled code in $profile_dir/generated; run scripts/generate.sh first"
    command -v cmake >/dev/null || fail "cmake is not on PATH"
    command -v ninja >/dev/null || fail "ninja is not on PATH (start Git Bash from the x64 Native Tools prompt)"
    command -v python3 >/dev/null || fail "python3 is not on PATH"

    prefix_args=()
    [[ -n "$sdl3" ]] && prefix_args+=("-DCMAKE_PREFIX_PATH=$sdl3")

    step "Configure"
    cmake -S "$repo_dir" -B "$build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DPSPRECOMP_PROFILE=mga \
        -DMGA_RELEASE=ON \
        "${prefix_args[@]}"

    step "Build"
    cmake --build "$build" --target MGAcid -j "$jobs"

    step "Stage"
    rm -rf "$stage"
    mkdir -p "$stage/licenses"
    cp "$build/bin/MGAcid.exe" "$stage/"
    # The libraries the program loads. Copied by name rather than with a glob:
    # a glob would also sweep up whatever else a build directory happens to
    # hold, and the test executables live there too.
    for dll in SDL3.dll avcodec-61.dll avutil-59.dll swresample-5.dll; do
        [[ -f "$build/bin/$dll" ]] ||
            fail "missing $dll in $build/bin. The build copies SDL3 and the FFmpeg libraries there itself; if one is absent the build did not finish, or SDL3 was found as a static library"
        cp "$build/bin/$dll" "$stage/"
    done
    cp "$repo_dir/LICENSE" "$stage/licenses/MGAcid-LICENSE.txt"
    cp "$profile_dir/packaging/THIRD_PARTY_NOTICES.md" "$stage/licenses/"
    for f in FFmpeg-LICENSE.txt FFmpeg-SOURCE.txt; do
        [[ -f "$build/bin/$f" ]] && cp "$build/bin/$f" "$stage/licenses/"
    done
    install -m 644 "$profile_dir/packaging/windows/README.txt" "$stage/README.txt"
fi

[[ -f "$stage/MGAcid.exe" ]] || fail "nothing staged in $stage; run without --skip-build"

step "Check the staged build"
check_no_game_data "$stage" "the staged build"

step "Zip"
rm -rf "$dist"
mkdir -p "$dist"
# Written by python rather than by zip, which Git Bash does not ship, and in a
# fixed order with a fixed timestamp so two builds of the same commit give the
# same bytes.
python3 - "$stage" "$dist/$name.zip" "$name" <<'PY'
import os, sys, zipfile
stage, out, top = sys.argv[1], sys.argv[2], sys.argv[3]
files = []
for root, dirs, names in os.walk(stage):
    dirs.sort()
    for n in sorted(names):
        full = os.path.join(root, n)
        files.append((full, os.path.join(top, os.path.relpath(full, stage)).replace(os.sep, "/")))
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for full, arc in files:
        info = zipfile.ZipInfo(arc, date_time=(1980, 1, 1, 0, 0, 0))
        info.external_attr = 0o644 << 16
        info.compress_type = zipfile.ZIP_DEFLATED
        with open(full, "rb") as f:
            z.writestr(info, f.read())
print(f"{len(files)} files")
PY

step "Check what was actually packed"
check_dir="$work/check"
rm -rf "$check_dir"
mkdir -p "$check_dir"
python3 -c "import sys,zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$dist/$name.zip" "$check_dir"
check_no_game_data "$check_dir" "$name.zip"
rm -rf "$check_dir"

step "Build information"
{
    echo "MGAcid $version for Windows (x86-64)"
    echo "Commit:   $(git -C "$repo_dir" rev-parse HEAD)"
    echo "Built:    $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "Compiler: $(cl 2>&1 | head -1 | tr -d '\r')"
} > "$dist/BUILDINFO.txt"
cat "$dist/BUILDINFO.txt"

step "Checksums"
( cd "$dist" && sha256sum "$name.zip" BUILDINFO.txt > SHA256SUMS && cat SHA256SUMS )
printf '\nThe release is in %s\n' "$dist"
