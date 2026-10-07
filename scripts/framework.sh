# Sourced by this repository's scripts after they set repo_dir: finds the
# PSPRecomp framework and the build directory the scripts use by default.
#
#   framework_dir  $PSPRECOMP_DIR, else the workspace's ../../framework,
#                  else the submodule external/PSPRecomp
#   psp_preset     $PSP_PRESET, else win-amd64 in Git Bash, linux-amd64 elsewhere
#   build_default  out/build/<psp_preset>, where `cmake --preset` builds
if [ -n "${PSPRECOMP_DIR:-}" ]; then
    framework_dir="$PSPRECOMP_DIR"
elif [ -f "$repo_dir/../../framework/cmake/PSPRecomp.cmake" ]; then
    framework_dir="$(cd "$repo_dir/../../framework" && pwd)"
else
    framework_dir="$repo_dir/external/PSPRecomp"
fi
if [ ! -f "$framework_dir/cmake/PSPRecomp.cmake" ]; then
    echo "The PSPRecomp framework was not found: run 'git submodule update --init', or set PSPRECOMP_DIR." >&2
    exit 1
fi
case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) psp_preset="${PSP_PRESET:-win-amd64}" ;;
    *) psp_preset="${PSP_PRESET:-linux-amd64}" ;;
esac
build_default="$repo_dir/out/build/$psp_preset"
