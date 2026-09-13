#!/usr/bin/env bash
# Builds stable-diffusion.cpp with the Vulkan backend and installs the results
# into third_party/stable-diffusion/{include,bin}.
#
# The library is loaded at run time, never linked (D20), and is built outside
# the project tree so that its vendored libwebp/libwebm cannot collide with the
# MSYS2 ones we use for image I/O, and so that a normal `cmake --build build`
# stays fast (D21). Re-run this only when bumping SD_COMMIT below.
#
# Windows/MSYS2 prerequisites:
#   pacman -S mingw-w64-x86_64-{shaderc,glslang,spirv-tools,spirv-headers}
#   pacman -S mingw-w64-x86_64-vulkan-{headers,loader}
# and MSYS2 must come first on PATH:
#   export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"

set -euo pipefail

# GCC puts intermediates in $TMPDIR / $TMP / $TEMP. If none of them is exported
# into the environment, MinGW falls back to C:\WINDOWS and every single compile
# dies with "Cannot create temporary file ...: Permission denied" - which looks
# like a broken compiler rather than a missing variable. Pick the first
# writable candidate and export all three.
pick_tmpdir() {
    local candidate
    for candidate in "${TMPDIR:-}" "${TMP:-}" "${TEMP:-}" /tmp; do
        [ -n "$candidate" ] || continue
        [ -d "$candidate" ] || continue
        if touch "$candidate/.pastiche_tmp_probe.$$" 2>/dev/null; then
            rm -f "$candidate/.pastiche_tmp_probe.$$"
            printf '%s' "$candidate"
            return 0
        fi
    done
    return 1
}
if tmp_dir="$(pick_tmpdir)"; then
    export TMPDIR="$tmp_dir" TMP="$tmp_dir" TEMP="$tmp_dir"
else
    echo "ERROR: no writable temporary directory (tried TMPDIR, TMP, TEMP, /tmp)" >&2
    exit 1
fi

# Pinned upstream version. Bump deliberately; re-measure the VRAM constants of
# the diffusion algorithm afterwards, the same rule as for ONNX Runtime.
SD_REPO="https://github.com/leejet/stable-diffusion.cpp.git"
SD_COMMIT="7f410a3793c5bba8eb198e962ce7a3d6095f9d89"   # master-859, 2026-09-12

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src_dir="$root_dir/third_party/sdcpp-src"
build_dir="$src_dir/build-vulkan"
out_dir="$root_dir/third_party/stable-diffusion"

jobs="$(nproc 2>/dev/null || echo 4)"

echo "== stable-diffusion.cpp $SD_COMMIT -> $out_dir"

if [ ! -d "$src_dir/.git" ]; then
    echo "-- cloning"
    git clone "$SD_REPO" "$src_dir"
fi

echo "-- checking out pinned commit"
git -C "$src_dir" fetch --tags origin
git -C "$src_dir" checkout --quiet "$SD_COMMIT"
# The server frontend submodule is a web app we never build; skip it.
git -C "$src_dir" submodule update --init --recursive ggml thirdparty/libwebm thirdparty/libwebp

echo "-- configuring"
# SD_BUILD_SHARED_LIBS: one DLL, loaded at run time (D20).
# GGML_NATIVE=OFF:      no -march=native, or the release only runs on machines
#                       resembling the build host (D20 pkt 3).
# SD_WEBP/SD_WEBM=OFF:  we do our own image I/O with the MSYS2 libraries (D6).
cmake -S "$src_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DSD_VULKAN=ON \
    -DSD_BUILD_SHARED_LIBS=ON \
    -DSD_BUILD_EXAMPLES=OFF \
    -DSD_WEBP=OFF \
    -DSD_WEBM=OFF \
    -DGGML_NATIVE=OFF \
    -DCMAKE_PREFIX_PATH="${CMAKE_PREFIX_PATH:-/c/msys64/mingw64}"

echo "-- building (this compiles a few hundred Vulkan shaders, be patient)"
cmake --build "$build_dir" -j "$jobs"

echo "-- installing"
mkdir -p "$out_dir/include" "$out_dir/bin"
cp "$src_dir/include/stable-diffusion.h" "$out_dir/include/"

copied=0
for lib in libstable-diffusion.dll stable-diffusion.dll libstable-diffusion.so; do
    if [ -f "$build_dir/bin/$lib" ]; then
        cp "$build_dir/bin/$lib" "$out_dir/bin/"
        echo "   $lib ($(du -h "$out_dir/bin/$lib" | cut -f1))"
        copied=1
    fi
done
if [ "$copied" -eq 0 ]; then
    echo "ERROR: no shared library found under $build_dir/bin" >&2
    exit 1
fi

echo "== done. Re-run cmake so it picks up the header."
