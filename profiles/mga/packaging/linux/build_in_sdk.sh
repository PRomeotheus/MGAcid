#!/usr/bin/env bash
# Builds everything a Linux release contains, inside the Steam Runtime 3
# "sniper" SDK container that scripts/release_linux.sh starts:
#
#   1. SDL3 from the source pinned in sources.sh
#   2. the recompiled executable (generate.sh, then MGAcid), with the
#      LGPL-only FFmpeg the build bundles (cmake/FFmpeg.cmake)
#   3. a staging tree with the executable, lib/, fonts/ and licenses/, the
#      part both the tarball and the Flatpak ship
#
# There are no overlay libraries. The MHP3rd profile this was copied from
# builds 355 of them; Metal Gear Ac!d loads relocatable stage modules through
# ModuleMgr instead, kOverlaySlots holds only its end marker, and this profile
# has no build_overlays.sh to call.
#
# Every step is resumable: finished dependencies are kept and ccache holds the
# compiled code.
#
# Environment: MGA_WORK (required) is the work directory; MGA_JOBS the
# number of parallel jobs (default 4).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
profile_dir="$(cd "$here/../.." && pwd)"
repo_dir="$(cd "$profile_dir/../.." && pwd)"
work="${MGA_WORK:?set MGA_WORK to the work directory}"
jobs="${MGA_JOBS:-4}"
# shellcheck source=sources.sh
source "$here/sources.sh"

sources="$work/sources"
deps="$work/deps"
deps_build="$work/deps-build"
build="$work/build"
stage="$work/stage/mgacid"

export CC=gcc-14 CXX=g++-14
export CCACHE_DIR="$work/ccache" CCACHE_MAXSIZE=20G
export PKG_CONFIG_PATH="$deps/lib/pkgconfig"
mkdir -p "$sources" "$deps" "$deps_build"

step() { printf '\n=== %s\n' "$*"; }

# fetch <file name> <url> <sha256>
fetch() {
    local target="$sources/$1"
    if [[ -f "$target" ]] && echo "$3  $target" | sha256sum -c --status; then return; fi
    curl -fsSL -o "$target.part" "$2"
    if ! echo "$3  $target.part" | sha256sum -c --status; then
        echo "error: $2 does not match its pinned SHA-256" >&2
        exit 1
    fi
    mv "$target.part" "$target"
}

# A dependency is rebuilt when its version or configuration changes.
stamp_matches() { [[ -f "$deps/.$1.stamp" && "$(cat "$deps/.$1.stamp")" == "$2" ]]; }

step "Fetching pinned sources"
fetch "SDL3-$SDL3_VERSION.tar.gz" "$SDL3_URL" "$SDL3_SHA256"
fetch NotoSansCJKjp-Regular.otf "$NOTO_CJK_URL" "$NOTO_CJK_SHA256"
fetch NotoSansCJK-LICENSE.txt "$NOTO_CJK_LICENSE_URL" "$NOTO_CJK_LICENSE_SHA256"

sdl_stamp="$SDL3_VERSION $SDL3_SHA256"
if ! stamp_matches sdl3 "$sdl_stamp"; then
    step "Building SDL3 $SDL3_VERSION"
    rm -rf "$deps_build/SDL3-$SDL3_VERSION"
    tar -xzf "$sources/SDL3-$SDL3_VERSION.tar.gz" -C "$deps_build"
    cmake -S "$deps_build/SDL3-$SDL3_VERSION" -B "$deps_build/sdl3-build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$deps" -DCMAKE_INSTALL_LIBDIR=lib \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
    cmake --build "$deps_build/sdl3-build" -j "$jobs"
    cmake --install "$deps_build/sdl3-build"
    echo "$sdl_stamp" > "$deps/.sdl3.stamp"
fi

step "Configuring MGAcid (release)"
cmake -S "$repo_dir" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DPSPRECOMP_PROFILE=mga \
    -DMGA_RELEASE=ON \
    -DMGA_FFMPEG=bundled \
    -DMGA_FFMPEG_DOWNLOAD_DIR="$sources" \
    -DCMAKE_PREFIX_PATH="$deps" \
    -DPSPRECOMP_GENERATED_JOBS="$jobs" | tee "$work/configure.log"
# A release without the renderer or without music would configure fine; refuse it.
for feature in "mga: Vulkan renderer enabled" "mga: bundled FFmpeg"; do
    if ! grep -q "$feature" "$work/configure.log"; then
        echo "error: configure did not report '$feature'" >&2
        exit 1
    fi
done

step "Generating the recompiled code"
"$profile_dir/scripts/generate.sh" "$build"

step "Building MGAcid"
cmake --build "$build" -j "$jobs" --target MGAcid mga_savedata_tests
"$build/bin/mga_savedata_tests"

step "Staging"
rm -rf "$stage"
mkdir -p "$stage/lib" "$stage/fonts" "$stage/licenses"
install -m 755 "$build/bin/MGAcid" "$stage/MGAcid"
strip --strip-unneeded "$stage/MGAcid"

# The libraries built above that the executable needs, directly or through
# each other, under the names the loader looks for.
# SDL3 comes from the dependency prefix, FFmpeg from the build's bin/lib.
needed() { objdump -p "$1" | awk '$1 == "NEEDED" { print $2 }'; }
pending=("$stage/MGAcid")
while [[ ${#pending[@]} -gt 0 ]]; do
    current="${pending[0]}"
    pending=("${pending[@]:1}")
    for soname in $(needed "$current"); do
        [[ -e "$stage/lib/$soname" ]] && continue
        for dir in "$build/bin/lib" "$deps/lib"; do
            [[ -e "$dir/$soname" ]] || continue
            cp -L "$dir/$soname" "$stage/lib/$soname"
            chmod 644 "$stage/lib/$soname"
            strip --strip-unneeded "$stage/lib/$soname"
            pending+=("$stage/lib/$soname")
            break
        done
    done
done

cp "$sources/NotoSansCJKjp-Regular.otf" "$stage/fonts/"
cp "$repo_dir/LICENSE" "$stage/licenses/MGAcid-LICENSE.txt"
cp "$here/../THIRD_PARTY_NOTICES.md" "$stage/licenses/THIRD_PARTY_NOTICES.md"
cp "$deps_build/SDL3-$SDL3_VERSION/LICENSE.txt" "$stage/licenses/SDL3-LICENSE.txt"
# The FFmpeg build leaves its licence and a note of its source and configure
# line next to the libraries.
cp "$build/bin/lib/FFmpeg-COPYING.LGPLv2.1.txt" "$build/bin/lib/FFmpeg-SOURCE.txt" "$stage/licenses/"
cp "$profile_dir/third_party/imgui/LICENSE.txt" "$stage/licenses/DearImGui-LICENSE.txt"
cp "$profile_dir/third_party/tiny_aes/UNLICENSE" "$stage/licenses/tiny-AES-c-UNLICENSE.txt"
cp "$sources/NotoSansCJK-LICENSE.txt" "$stage/licenses/NotoSansCJK-OFL.txt"

step "Checking the staged program"
# Everything must resolve from lib/ or from libraries every desktop has.
missing="$(LD_LIBRARY_PATH='' ldd "$stage/MGAcid" | grep 'not found' || true)"
if [[ -n "$missing" ]]; then
    echo "error: unresolved libraries:" >&2
    echo "$missing" >&2
    exit 1
fi
ldd "$stage/MGAcid" | grep "$stage/lib" || { echo "error: bundled libraries not used" >&2; exit 1; }
# No libstdc++ from the build machine is needed or exported.
if objdump -p "$stage/MGAcid" | grep -q 'NEEDED.*libstdc++'; then
    echo "error: a staged binary needs libstdc++.so" >&2
    exit 1
fi
glibc_floor="$(objdump -T "$stage/MGAcid" "$stage/lib/"*.so* |
    grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
echo "Newest glibc symbol version needed: $glibc_floor"
echo "$glibc_floor" > "$work/stage/glibc-floor.txt"
echo "Staged MGAcid and $(find "$stage/lib" -name '*.so*' | wc -l) libraries in $stage"
