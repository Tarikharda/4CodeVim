#!/bin/bash

# Builds an optimized 4coder and installs it for the current user on macOS:
#   $APP_DIR/My Editor.app (default ~/Applications) - binaries, fonts, themes, default config
#   $BIN_DIR/4coder (default ~/.local/bin)          - symlink to the executable, for terminals
# Personal settings belong in ~/.4coder/ (config.4coder, bindings.4coder, themes/),
# which 4coder searches before the app's folder, so reinstalling never touches them.
#
# Needs the Xcode command line tools (xcode-select --install) and Homebrew's freetype and
# pkg-config (brew install freetype pkg-config). The editor also uses rg, git, lazygit,
# Ghostty and jdtls when they are installed (brew install ripgrep lazygit jdtls,
# brew install --cask ghostty).

# If any command errors, stop the script
set -e

# Set up directories

ME="$(realpath "$0")"
LOCATION="$(dirname "$ME")"
SRC_ROOT="$(dirname "$LOCATION")"
PROJECT_ROOT="$(dirname "$SRC_ROOT")"
BUILD_ROOT="$PROJECT_ROOT/build"

APP_NAME="My Editor"
APP_DIR="${APP_DIR:-$HOME/Applications}"
BIN_DIR="${BIN_DIR:-$HOME/.local/bin}"
BUNDLE="$APP_DIR/$APP_NAME.app"
PREFIX="$BUNDLE/Contents/MacOS"

# Check the build dependencies

if ! command -v clang++ > /dev/null; then
    echo "install: clang++ not found, install the command line tools: xcode-select --install"
    exit 1
fi
if ! command -v pkg-config > /dev/null || ! pkg-config --exists freetype2; then
    echo "install: FreeType not found, install it with: brew install freetype pkg-config"
    exit 1
fi

# Build

BUILD_LOG="$(mktemp)"
"$SRC_ROOT/bin/build-mac.sh" -DOPT_BUILD 2>&1 | tee "$BUILD_LOG"
# NOTE: The build driver keeps going after compile errors, so check the log ourselves.
if grep -qE "error:|fatal error" "$BUILD_LOG"; then
    echo "install: build failed, nothing was installed"
    rm -f "$BUILD_LOG"
    exit 1
fi
rm -f "$BUILD_LOG"

# Install

mkdir -p "$PREFIX/fonts" "$BUNDLE/Contents/Resources" "$BIN_DIR"

# NOTE: Remove before copying so a running 4coder keeps its mapped binaries.
for FILE in 4ed 4ed_app.so custom_4coder.so; do
    rm -f "$PREFIX/$FILE"
    cp "$BUILD_ROOT/$FILE" "$PREFIX/$FILE"
done

cp "$SRC_ROOT/ship_files/config.4coder" "$PREFIX/config.4coder"
cp "$SRC_ROOT/ship_files/bindings.4coder" "$PREFIX/bindings.4coder"
cp "$SRC_ROOT/ship_files/mac-bindings.4coder" "$PREFIX/mac-bindings.4coder"
rm -rf "$PREFIX/themes"
cp -r "$SRC_ROOT/ship_files/themes" "$PREFIX/themes"

# NOTE: The fonts normally come from 4coder-non-source. Otherwise use Liberation Mono from
# the user's or the system's fonts (brew install --cask font-liberation), else Menlo,
# which ships with macOS (FreeType reads the first face of the .ttc).
FOREIGN_FONTS="$PROJECT_ROOT/4coder-non-source/dist_files/fonts"
if [ -d "$FOREIGN_FONTS" ]; then
    cp "$FOREIGN_FONTS"/*.ttf "$PREFIX/fonts/"
fi
if [ ! -f "$PREFIX/fonts/liberation-mono.ttf" ]; then
    LIBERATION=""
    for CANDIDATE in "$HOME/Library/Fonts/LiberationMono-Regular.ttf" \
                     "/Library/Fonts/LiberationMono-Regular.ttf" \
                     "/opt/homebrew/share/fonts/LiberationMono-Regular.ttf" \
                     "/usr/local/share/fonts/LiberationMono-Regular.ttf"; do
        if [ -f "$CANDIDATE" ]; then
            LIBERATION="$CANDIDATE"
            break
        fi
    done
    if [ -n "$LIBERATION" ]; then
        cp "$LIBERATION" "$PREFIX/fonts/liberation-mono.ttf"
    else
        echo "install: Liberation Mono not found (brew install --cask font-liberation), using Menlo"
        cp "/System/Library/Fonts/Menlo.ttc" "$PREFIX/fonts/liberation-mono.ttf"
    fi
fi

cat > "$BUNDLE/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>
    <string>$APP_NAME</string>
    <key>CFBundleDisplayName</key>
    <string>$APP_NAME</string>
    <key>CFBundleIdentifier</key>
    <string>local.myeditor</string>
    <key>CFBundleExecutable</key>
    <string>4ed</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>4.1.8</string>
    <key>CFBundleVersion</key>
    <string>4.1.8</string>
    <key>LSMinimumSystemVersion</key>
    <string>11.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
EOF

# NOTE: Holding hjkl repeats them instead of opening the accent picker.
defaults write local.myeditor ApplePressAndHoldEnabled -bool false

# NOTE: Let Spotlight and the Dock see the new bundle right away.
touch "$BUNDLE"

ln -sfn "$PREFIX/4ed" "$BIN_DIR/4coder"

# NOTE: Tools the editor runs; missing ones only disable their features.
for TOOL in rg git lazygit jdtls; do
    if ! command -v "$TOOL" > /dev/null; then
        echo "install: optional tool '$TOOL' not found (brew install $([ "$TOOL" = rg ] && echo ripgrep || echo "$TOOL"))"
    fi
done
if [ ! -d "/Applications/Ghostty.app" ] && [ ! -d "$HOME/Applications/Ghostty.app" ]; then
    echo "install: Ghostty not found, lazygit and the terminal keys need it (brew install --cask ghostty)"
fi

echo "install: $APP_NAME installed to $BUNDLE"
echo "install: open it from Spotlight, or run '4coder' (make sure $BIN_DIR is on your PATH)"
