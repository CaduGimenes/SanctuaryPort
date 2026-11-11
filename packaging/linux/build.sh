#!/usr/bin/env bash
# Builds dist/SanctuaryPort-linux-x86_64.AppImage from the Linux build (run `bash build.sh` in the repo
# root first; the launcher is built here). Libraries are bundled with their dependencies, except the
# ones that must come from the host (glibc, Vulkan loader and drivers, X11/Wayland, GL). The game
# starts from run.sh like a source checkout, with SANCTUARY_PREBUILT=1: players need python3 (every
# desktop distro and SteamOS have it). Needs: ldd, patchelf, curl (appimagetool), cargo.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
[[ -f out/sanctuary-probe && -f out/gpu/libsanctuarygpu.so ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }
# From launcher-rust/, so rustup uses the version its rust-toolchain.toml pins.
(cd launcher-rust && cargo build --release)

appdir=out/AppDir
rm -rf "$appdir"
share=$appdir/usr/share/sanctuary
mkdir -p "$share/bin/gpu" "$appdir/usr/lib" "$share/launcher" "$share/assets"

install -m755 out/sanctuary-probe out/sanctuary-gpu-capabilities "$share/bin/"
install -m755 out/gpu/libsanctuarygpu.so "$share/bin/gpu/"
install -m755 launcher-rust/target/release/sanctuaryport-launcher "$share/sanctuaryport-launcher"
strip "$share/sanctuaryport-launcher"
install -m755 run.sh "$share/run.sh"
cp run.py LICENSE README.md COMO_JOGAR.md "$share/"
cp -r scripts patches "$share/"
cp -r assets/glyphs "$share/assets/"
# launcher/sanctuaryport.png marks the port folder for the launcher (paths::default_port_dir).
for file in sanctuaryport.png sanctuary.png Cinzel-Black.ttf OFL-Cinzel.txt; do
    install -m644 "launcher/$file" "$share/launcher/$file"
done
icon=launcher/sanctuary.png
find "$share" -name __pycache__ -prune -exec rm -r {} +
install -m755 packaging/linux/AppRun "$appdir/AppRun"
install -m644 packaging/linux/sanctuary.desktop "$appdir/sanctuary.desktop"
# RecompLauncher reads this at the root of the AppImage.
install -m644 recomplauncher.json "$appdir/recomplauncher.json"
install -m644 "$icon" "$appdir/sanctuary.png"

# Host-provided libraries: bundling any of these breaks drivers or the display connection.
excluded='^(ld-linux.*|libc|libm|libdl|librt|libpthread|libresolv|libutil|libnsl|libanl|libvulkan|libGL|libGLX|libGLdispatch|libEGL|libOpenGL|libgbm|libdrm.*|libX11|libX11-xcb|libxcb.*|libXau|libXdmcp|libXext|libXfixes|libXrender|libXi|libXcursor|libXrandr|libwayland-.*|libasound|libudev|libpulse.*|libfontconfig|libfreetype|libOpenCL|libva.*|libvdpau|libvpl|libnvidia-.*|libcuda|libz|libstdc\+\+|libgcc_s)\.so'
declare -A seen
bundle() {
    local elf=$1 line lib name
    while read -r line; do
        lib=$(awk '/=> \//{print $3}' <<<"$line")
        [[ -n $lib ]] || continue
        name=$(basename -- "$lib")
        [[ ${seen[$name]:-} ]] && continue
        seen[$name]=1
        [[ $name =~ $excluded ]] && continue
        install -m755 "$lib" "$appdir/usr/lib/$name"
        bundle "$lib"
    done < <(ldd "$elf")
}
bundle out/gpu/libsanctuarygpu.so
bundle out/sanctuary-probe
bundle "$share/sanctuaryport-launcher"
# The launcher loads the keyboard libraries at run time (dlopen), so ldd does not list them: bundle
# them, as minimal desktops and containers can lack libxkbcommon-x11.
for name in libxkbcommon.so.0 libxkbcommon-x11.so.0; do
    lib=$(ldconfig -p | awk -v n="$name" '$1 == n && /x86-64/ && !found {print $NF; found = 1}')
    [[ -n $lib ]] || { echo "Need $name (libxkbcommon-x11-dev)" >&2; exit 1; }
    install -m755 "$lib" "$appdir/usr/lib/$name"
done
# libstdc++/libgcc/libz come from the build host: the same glibc generation as the one that runs
# the AppImage is assumed, and a newer libstdc++ than Mesa's is the safe direction.
for name in libstdc++.so.6 libgcc_s.so.1; do
    install -m755 "$(ldd out/gpu/libsanctuarygpu.so | awk -v n="$name" '$1==n{print $3}')" "$appdir/usr/lib/$name"
done
# $ORIGIN/gpu is already the probe's RUNPATH; the libraries it needs sit in usr/lib via AppRun.
find "$appdir/usr/lib" -name '*.so*' -exec strip --strip-unneeded {} + 2>/dev/null || true

# The newest glibc symbol in the bundle is the oldest glibc players can have. Built on a rolling
# distro it follows the host's and the AppImage stops starting on SteamOS, Debian and Fedora (#291):
# release builds come from .github/workflows/release.yml (Ubuntu 24.04). SANCTUARY_MAX_GLIBC
# raises the limit for local builds.
max_glibc=${SANCTUARY_MAX_GLIBC:-2.39}
need=$(find "$appdir" -type f \( -name '*.so*' -o -perm -u+x \) -exec objdump -T {} + 2>/dev/null |
       grep -o 'GLIBC_2\.[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1 || true)  # objdump fails on scripts
if [[ $(printf '%s\n' "$need" "$max_glibc" | sort -V | tail -1) != "$max_glibc" ]]; then
    echo "The bundle needs glibc $need, newer than $max_glibc: build on Ubuntu 24.04 (packaging/linux/ci-deps.sh) or set SANCTUARY_MAX_GLIBC" >&2
    exit 1
fi
echo "Minimum glibc: $need"

forbidden=$(find "$appdir" \( -iname eboot.bin -o -iname sce_module -o -iname sce_sys -o -iname savedata \
    -o -iname '*.pkg' -o -iname '*.sprx' -o -iname '*.prx' -o -iname sanctuary.ini -o -iname settings.json \) -print)
if [[ -n $forbidden ]]; then
    echo "Package holds files that must not ship:" >&2
    echo "$forbidden" >&2
    exit 1
fi

tool=${APPIMAGETOOL:-$(command -v appimagetool || true)}
if [[ -z $tool ]]; then
    tool=out/appimagetool-x86_64.AppImage
    [[ -x $tool ]] || {
        curl -fL -o "$tool" https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
        chmod +x "$tool"
    }
fi
mkdir -p dist
# Written aside and moved over: replacing a running AppImage in place fails ("Text file busy").
ARCH=x86_64 "$tool" --appimage-extract-and-run "$appdir" dist/SanctuaryPort-linux-x86_64.AppImage.new
mv -f dist/SanctuaryPort-linux-x86_64.AppImage.new dist/SanctuaryPort-linux-x86_64.AppImage
ls -lh dist/SanctuaryPort-linux-x86_64.AppImage
