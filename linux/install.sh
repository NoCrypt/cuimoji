#!/bin/sh
# cuimoji installer: per-user install from a GitHub release, no build tools.
set -eu

REPO="NoCrypt/cuimoji"
DEFAULT_VERSION="0.2"
PREFIX="${CUIMOJI_PREFIX:-$HOME/.local}"
LIB_DIR="$PREFIX/lib/cuimoji"
BIN_DIR="$PREFIX/bin"
APPS_DIR="$PREFIX/share/applications"
ICON_DIR="$PREFIX/share/icons/hicolor/256x256/apps"

die() { printf 'cuimoji: %s\n' "$*" >&2; exit 1; }

remove_files() {
    rm -f "$BIN_DIR/cuimoji" "$APPS_DIR/cuimoji.desktop" "$ICON_DIR/cuimoji.png"
    rm -rf "$LIB_DIR"
}

if [ "${1:-}" = "--uninstall" ]; then
    remove_files
    echo "cuimoji removed."
    exit 0
fi
if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    cat <<'HELP'
cuimoji installer

  curl -fsSL https://raw.githubusercontent.com/NoCrypt/cuimoji/main/linux/install.sh | sh

Installs for the current user in ~/.local (override with CUIMOJI_PREFIX),
adds the cuimoji command and an application-menu entry.
Uninstall with: ~/.local/lib/cuimoji/install.sh --uninstall
HELP
    exit 0
fi

[ "$(uname -s)" = "Linux" ] || die "this installer is for Linux."
case "$(uname -m)" in
    x86_64|amd64) ARCH=x86_64 ;;
    *) die "no prebuilt binary for $(uname -m) yet." ;;
esac
command -v curl >/dev/null 2>&1 || die "curl is required."
command -v tar >/dev/null 2>&1 || die "tar is required."

VERSION="${CUIMOJI_VERSION:-}"
if [ -z "$VERSION" ]; then
    VERSION=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" 2>/dev/null \
        | sed -n 's/.*"tag_name": *"v\{0,1\}\([^"]*\)".*/\1/p' | head -n 1) || true
fi
[ -n "$VERSION" ] || VERSION="$DEFAULT_VERSION"
VERSION=${VERSION#v}

if [ -e "$LIB_DIR/cuimoji" ] || [ -L "$BIN_DIR/cuimoji" ] || [ -f "$APPS_DIR/cuimoji.desktop" ]; then
    installed=$("$LIB_DIR/cuimoji" --version 2>/dev/null | sed -n 's/^cuimoji //p' | head -n 1) || true
    [ -n "$installed" ] || installed="unknown"
    answer="u"
    if [ -n "${CUIMOJI_YES:-}" ]; then
        :
    elif [ -r /dev/tty ]; then
        printf '\ncuimoji %s is already installed in %s.\n' "$installed" "$LIB_DIR" >/dev/tty
        printf '  [u] Upgrade to %s\n  [r] Remove cuimoji\n  [c] Cancel\n' "$VERSION" >/dev/tty
        printf 'Choose [u/r/c] (u): ' >/dev/tty
        IFS= read -r answer </dev/tty || answer="u"
        [ -n "$answer" ] || answer="u"
    else
        printf 'cuimoji %s is already installed; upgrading to %s (set CUIMOJI_YES=1 to skip this notice).\n' "$installed" "$VERSION"
    fi
    case "$answer" in
        r|R|remove|uninstall)
            remove_files
            echo "cuimoji removed."
            exit 0 ;;
        c|C|cancel)
            echo "Cancelled."
            exit 0 ;;
    esac
fi

TARBALL="cuimoji-$VERSION-linux-$ARCH.tar.gz"
URL="https://github.com/$REPO/releases/download/v$VERSION/$TARBALL"

TMP=$(mktemp -d "${TMPDIR:-/tmp}/cuimoji.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

echo "Downloading cuimoji $VERSION..."
curl -fL --retry 3 --connect-timeout 15 -o "$TMP/$TARBALL" "$URL" \
    || die "could not download $URL"
if command -v sha256sum >/dev/null 2>&1 \
    && curl -fsSL --connect-timeout 15 -o "$TMP/$TARBALL.sha256" "$URL.sha256" 2>/dev/null; then
    (cd "$TMP" && sha256sum -c "$TARBALL.sha256") >/dev/null \
        || die "download checksum failed."
fi
tar -xzf "$TMP/$TARBALL" -C "$TMP" || die "could not extract the archive."
[ -f "$TMP/cuimoji" ] || die "archive is missing the cuimoji binary."

# The archive links SDL2, Wayland, X11 and Vulkan. Check the four hard ones and
# offer to install the distro packages when they are missing.
missing=""
for lib in libX11.so.6 libSDL2-2.0.so.0 libwayland-client.so.0 libvulkan.so.1; do
    ldconfig -p 2>/dev/null | grep -q "$lib" || missing="$missing $lib"
done

have_wl_copy=1
command -v wl-copy >/dev/null 2>&1 || have_wl_copy=0

if [ -n "$missing" ] || [ "$have_wl_copy" = 0 ]; then
    if [ "$(id -u)" -eq 0 ]; then SUDO=""
    elif command -v sudo >/dev/null 2>&1; then SUDO="sudo"
    else die "missing libraries:$missing. Re-run as root or install the packages manually."
    fi

    if command -v apt-get >/dev/null 2>&1; then
        PKGS="libsdl2-2.0-0 libwayland-client0 libx11-6 libvulkan1 wl-clipboard"
        $SUDO apt-get update
        $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y $PKGS
    elif command -v dnf >/dev/null 2>&1; then
        PKGS="SDL2 wayland-libs-client libX11 libvulkan wl-clipboard"
        $SUDO dnf install -y $PKGS
    elif command -v pacman >/dev/null 2>&1; then
        PKGS="sdl2 wayland libx11 vulkan-icd-loader wl-clipboard"
        $SUDO pacman -S --needed --noconfirm $PKGS
    elif command -v zypper >/dev/null 2>&1; then
        PKGS="libSDL2-2_0-0 libwayland-client0 libX11-6 libvulkan1 wl-clipboard"
        $SUDO zypper --non-interactive install $PKGS
    else
        die "missing libraries:$missing. Install them with your package manager."
    fi
fi

mkdir -p "$LIB_DIR" "$BIN_DIR" "$APPS_DIR" "$ICON_DIR"
cp "$TMP/cuimoji" "$LIB_DIR/cuimoji"
chmod 755 "$LIB_DIR/cuimoji"
if [ -f "$TMP/lucide-cuimoji.ttf" ]; then
    cp "$TMP/lucide-cuimoji.ttf" "$LIB_DIR/lucide-cuimoji.ttf"
fi
if [ -f "$TMP/icon.png" ]; then
    cp "$TMP/icon.png" "$ICON_DIR/cuimoji.png"
fi
if [ -f "$TMP/install.sh" ]; then
    cp "$TMP/install.sh" "$LIB_DIR/install.sh"
    chmod 755 "$LIB_DIR/install.sh"
fi
ln -sf "$LIB_DIR/cuimoji" "$BIN_DIR/cuimoji"

cat > "$APPS_DIR/cuimoji.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=cuimoji
GenericName=Emoji Picker
Comment=Search and paste emoji
Exec="$LIB_DIR/cuimoji"
TryExec=$LIB_DIR/cuimoji
Icon=$ICON_DIR/cuimoji.png
Terminal=false
Categories=Utility;
Keywords=emoji;picker;smileys;
StartupNotify=false
DESKTOP

if ! "$LIB_DIR/cuimoji" --version >/dev/null 2>&1; then
    "$LIB_DIR/cuimoji" --version 2>&1 | head -n 3 >&2 || true
    die "the downloaded binary cannot run on this system (it needs glibc 2.35 or newer)."
fi

echo
echo "cuimoji $VERSION installed."
echo "  command: $BIN_DIR/cuimoji"
echo "  files:   $LIB_DIR"
echo "  menu:    $APPS_DIR/cuimoji.desktop"
case ":$PATH:" in
    *":$BIN_DIR:"*) ;;
    *) echo
       echo "Add $BIN_DIR to your PATH:"; echo "  export PATH=\"$BIN_DIR:\$PATH\"" ;;
esac
echo
echo "For automatic paste, allow your user to open /dev/uinput (usually the input group)"
echo "and install ydotool as a fallback. To uninstall:"
echo "  $LIB_DIR/install.sh --uninstall"
