#!/bin/sh
# Builds didj-x86_64.AppImage from a build directory.
#
#   scripts/package-appimage.sh BUILD_DIR [OUT]
#
# The build should use SDL built from source (-DCMAKE_DISABLE_FIND_PACKAGE_SDL2=ON)
# so the binaries need nothing beyond glibc and libstdc++; SDL loads X11,
# Wayland, PulseAudio and the rest from the host at run time.
set -eu
build=$1
out=${2:-didj-x86_64.AppImage}
src=$(cd "$(dirname "$0")/.." && pwd)
appdir=$(mktemp -d)
trap 'rm -rf "$appdir"' EXIT

mkdir -p "$appdir/usr/bin"
for b in didj didj-fetch didj-run; do
    install -m 755 "$build/$b" "$appdir/usr/bin/$b"
    strip "$appdir/usr/bin/$b"
done
install -m 755 "$src/packaging/linux/AppRun" "$appdir/AppRun"
install -m 644 "$src/packaging/linux/didj.desktop" "$appdir/didj.desktop"
install -m 644 "$src/packaging/linux/didj.svg" "$appdir/didj.svg"

# Anything outside glibc and the C++ runtime would have to be bundled.
extra=$(ldd "$appdir/usr/bin/didj" | awk '{print $1}' |
        grep -Ev '^(linux-vdso\.so|/lib64/ld-linux|(libc|libm|libstdc\+\+|libgcc_s|libpthread|libdl|librt)\.so)' || true)
if [ -n "$extra" ]; then
    echo "didj links libraries the AppImage does not carry:" >&2
    echo "$extra" >&2
    exit 1
fi

tool=${APPIMAGETOOL:-appimagetool}
APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 "$tool" --no-appstream "$appdir" "$out"
