#!/usr/bin/env bash
# Builds dist/SanctuaryPort-windows/ and dist/SanctuaryPort-windows.zip: SanctuaryPort.exe (the
# Rust launcher), "Play SanctuaryPort.exe", bin/ (sanctuary-probe.exe and the DLLs it loads),
# python/ (python.org's embeddable Python with Pillow and iced-x86, which runs run.py: players
# install nothing), run.py, scripts/, patches/, assets/ and launcher/.
#
# From Linux: cross-compiles first (packaging/windows/cross-build.sh) and needs the Rust target
# x86_64-pc-windows-gnu (rustup target add x86_64-pc-windows-gnu) and x86_64-w64-mingw32-gcc.
# From an MSYS2 CLANG64 shell: after `bash build.sh`, with cargo on PATH.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
name=SanctuaryPort-windows

case $(uname -s) in
MINGW*|MSYS*)
    source ./msys2-env.sh
    [[ -f out/sanctuary-probe.exe && -f out/sanctuary-gpu-capabilities.exe && -f out/sanctuary-play.exe ]] ||
        { echo 'Build first: bash build.sh' >&2; exit 1; }
    exes=out
    dlls=${MSYS2_ROOT:-/c/msys64}/clang64/bin
    (cd launcher-rust && cargo build --release)
    launcher=launcher-rust/target/release/sanctuaryport-launcher.exe
    ;;
*)
    bash packaging/windows/cross-build.sh
    exes=out/win/build
    dlls=out/win/root/clang64/bin
    # The builder's home folder stays out of the launcher's panic paths.
    CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=${CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER:-x86_64-w64-mingw32-gcc} \
    CARGO_TARGET_X86_64_PC_WINDOWS_GNU_RUSTFLAGS="--remap-path-prefix=$HOME=~" \
        bash -c "cd launcher-rust && cargo build --release --target x86_64-pc-windows-gnu"
    launcher=launcher-rust/target/x86_64-pc-windows-gnu/release/sanctuaryport-launcher.exe
    ;;
esac

# The package is assembled in a fresh staging folder and zipped from there; dist/$name (a
# playable copy that may hold saves and settings) is only refreshed afterwards.
dest=out/stage/$name
rm -rf -- out/stage
mkdir -p "$dest/bin"
llvm-strip -o "$dest/SanctuaryPort.exe" "$launcher"
llvm-strip -o "$dest/Play SanctuaryPort.exe" "$exes/sanctuary-play.exe"
# The executables without debug information (the build folder keeps the symbols).
for exe in sanctuary-probe.exe sanctuary-gpu-capabilities.exe; do
    llvm-strip --strip-debug -o "$dest/bin/$exe" "$exes/$exe"
done
# Every DLL the executables load from the CLANG64 tree (SDL3, FFmpeg, Vulkan loader...), found
# through their import tables, recursively.
declare -A seen
queue=("$dest/bin/sanctuary-probe.exe" "$dest/bin/sanctuary-gpu-capabilities.exe")
while ((${#queue[@]})); do
    file=${queue[0]}
    queue=("${queue[@]:1}")
    while read -r dll; do
        key=${dll,,}
        [[ -n ${seen[$key]:-} ]] && continue
        seen[$key]=1
        found=$(find "$dlls" -maxdepth 1 -iname "$dll" -print -quit)
        if [[ -n $found ]]; then
            cp "$found" "$dest/bin/"
            queue+=("$found")
        fi
    done < <(llvm-objdump -p "$file" | awk '/DLL Name:/ {print $3}')
done
python3 -I packaging/windows/fetch_python.py out/win/cache "$dest/python"
mkdir -p "$dest/launcher" "$dest/assets"
cp launcher/sanctuaryport.png launcher/sanctuary.png launcher/sanctuary.ico launcher/Cinzel-Black.ttf \
    launcher/OFL-Cinzel.txt "$dest/launcher/"
cp -r assets/glyphs assets/trophies assets/cosmetics "$dest/assets/"
cp -r scripts patches "$dest/"
cp run.py LICENSE README.md COMO_JOGAR.md packaging/windows/README-Windows.txt "$dest/"
# RecompLauncher reads this next to the executable.
cp recomplauncher.json "$dest/"
find "$dest" -name __pycache__ -prune -exec rm -r {} +
# No game files and nothing a player made (saves, shader caches built from the game's shaders,
# settings, logs) may ship: stop when one is found.
forbidden=$(find "$dest" \( -iname eboot.bin -o -iname sce_module -o -iname sce_sys -o -iname savedata \
    -o -iname user -o -iname cache -o -iname '*.log' -o -iname sanctuary.ini -o -iname settings.json \
    -o -iname mods.json -o -iname patches.json -o -iname fsr4_shaders -o -iname 'nvngx_dlss*.dll' \
    -o -iname '*.spv' -o -iname '*.pkg' -o -iname '*.sprx' -o -iname '*.prx' \) -print)
if [[ -n $forbidden ]]; then
    echo "Package holds files that must not ship:" >&2
    echo "$forbidden" >&2
    exit 1
fi
mkdir -p dist
rm -f "dist/$name.zip"
python3 -I - "out/stage" "$name" "dist/$name.zip" <<'EOF'
import sys, zipfile
from pathlib import Path
stage, name, out = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    for path in sorted((stage / name).rglob('*')):
        archive.write(path, path.relative_to(stage))
EOF

# Refresh dist/$name, keeping what players create there (saves, settings, mods).
play=dist/$name
mkdir -p "$play"
find "$play" -mindepth 1 -maxdepth 1 ! -name user ! -name mods ! -name sanctuary.ini \
    ! -name settings.json ! -name mods.json ! -name patches.json -exec rm -rf {} +
cp -r "$dest/." "$play/"
du -sh "$dest" "dist/$name.zip"
