#!/usr/bin/env bash
# Builds the Windows executables from Linux: out/win/build/sanctuary-probe.exe,
# sanctuary-gpu-capabilities.exe and sanctuary-play.exe. The host clang compiles against an MSYS2
# CLANG64 sysroot that this script downloads once into out/win/root (packaging/windows/msys2_fetch.py),
# so nothing is installed on the system and the Linux build (out/, out/gpu) is not touched.
# Needs: clang, lld, llvm-windres, cmake, ninja, pkg-config, glslang, zstd, bsdtar, python3.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
win=out/win
root=$win/root
packages=(crt-git headers-git winpthreads libc++ libunwind compiler-rt
          vulkan-headers vulkan-loader vulkan-memory-allocator glslang
          sdl3 fmt boost robin-map xxhash ffmpeg zydis spirv-tools spirv-headers spirv-cross)
for tool in clang lld llvm-windres cmake ninja pkg-config zstd bsdtar python3; do
    command -v "$tool" >/dev/null || { echo "Need $tool" >&2; exit 1; }
done
if [[ ! -f $root/clang64/include/windows.h ]]; then
    mkdir -p "$win"
    curl -sSfL -o "$win/clang64.db" https://repo.msys2.org/mingw/clang64/clang64.db
    python3 -I packaging/windows/msys2_fetch.py "$win/clang64.db" "$win/cache" "$root" "${packages[@]}"
fi
if [[ ! -f gpu/third_party/fsr-vulkan/CMakeLists.txt || ! -f gpu/third_party/imgui/imgui.h ||
      ! -f gpu/third_party/xbyak/xbyak/xbyak.h ]]; then
    git submodule update --init --recursive
fi
for patch in gpu/patches/fsr-vulkan/*.patch; do
    if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
        git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch"
    fi
done
if [[ ! -f $win/build/build.ninja ]]; then
    cmake -S gpu -B "$win/build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSANCTUARY_PGO=off \
        -DSANCTUARY_LTO="${SANCTUARY_LTO:-OFF}" -DCMAKE_TOOLCHAIN_FILE="$PWD/packaging/windows/cross-toolchain.cmake" \
        -DSANCTUARY_WIN_ROOT="$PWD/$root" >/dev/null
fi
if ! ninja -C "$win/build" sanctuary-probe sanctuary-gpu-capabilities sanctuary-play > "$win/build.log" 2>&1; then
    grep -v '^\[' "$win/build.log" | tail -40 >&2
    echo "Windows build failed (full log: $win/build.log)" >&2
    exit 1
fi
echo "Built $PWD/$win/build/sanctuary-probe.exe"
