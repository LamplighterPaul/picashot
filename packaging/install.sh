#!/bin/sh
# Installs the prebuilt Picashot from a release tarball into your home directory (no root needed).
#   ./install.sh            install into ~/.local   (set PREFIX to choose another place)
#   ./install.sh --remove   remove it again
set -eu
prefix=${PREFIX:-$HOME/.local}
here=$(dirname "$0")
bin=$prefix/bin/picashot
desktop=$prefix/share/applications/picashot.desktop
icon=$prefix/share/icons/hicolor/scalable/apps/picashot.svg
licence=$prefix/share/licenses/picashot/LICENSE

if [ "${1:-}" = "--remove" ]; then
    rm -f "$bin" "$desktop" "$icon" "$licence"
    rmdir "$(dirname "$licence")" 2>/dev/null || true
    echo "Picashot removed from $prefix"
    exit 0
fi

install -Dm755 "$here/picashot" "$bin"
install -Dm644 "$here/picashot.desktop" "$desktop"
install -Dm644 "$here/picashot.svg" "$icon"
install -Dm644 "$here/LICENSE" "$licence"
update-desktop-database "$prefix/share/applications" 2>/dev/null || true
echo "Picashot installed in $prefix. Start it from your launcher, or run: $bin"
case ":$PATH:" in *":$prefix/bin:"*) ;; *) echo "Note: $prefix/bin is not on your PATH." ;; esac
missing=$(ldd "$bin" 2>/dev/null | grep 'not found' || true)
[ -z "$missing" ] || { echo "These libraries are missing; install SDL3 (3.4 or newer) and libjpeg-turbo:"; echo "$missing"; }
