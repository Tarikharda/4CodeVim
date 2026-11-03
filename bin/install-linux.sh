#!/bin/bash

# Builds an optimized 4coder and installs it for the current user:
#   $PREFIX (default ~/.local/share/4coder)  - binaries, fonts, themes, default config
#   $BIN_DIR/4coder (default ~/.local/bin)   - symlink to the executable
#   ~/.local/share/applications/4coder.desktop
# Personal settings belong in ~/.4coder/ (config.4coder, bindings.4coder, themes/),
# which 4coder searches before the install folder, so reinstalling never touches them.

# If any command errors, stop the script
set -e

# Set up directories

ME="$(readlink -f "$0")"
LOCATION="$(dirname "$ME")"
SRC_ROOT="$(dirname "$LOCATION")"
PROJECT_ROOT="$(dirname "$SRC_ROOT")"
BUILD_ROOT="$PROJECT_ROOT/build"

PREFIX="${PREFIX:-$HOME/.local/share/4coder}"
BIN_DIR="${BIN_DIR:-$HOME/.local/bin}"
APPS_DIR="$HOME/.local/share/applications"

# Build

BUILD_LOG="$(mktemp)"
"$SRC_ROOT/bin/build-linux.sh" -DOPT_BUILD 2>&1 | tee "$BUILD_LOG"
# NOTE: The build driver keeps going after compile errors, so check the log ourselves.
if grep -qE "error:|fatal error" "$BUILD_LOG"; then
    echo "install: build failed, nothing was installed"
    rm -f "$BUILD_LOG"
    exit 1
fi
rm -f "$BUILD_LOG"

# Install

mkdir -p "$PREFIX/fonts" "$PREFIX/themes" "$BIN_DIR" "$APPS_DIR"

# NOTE: Remove before copying so a running 4coder keeps its mapped binaries.
for FILE in 4ed 4ed_app.so custom_4coder.so; do
    rm -f "$PREFIX/$FILE"
    cp "$BUILD_ROOT/$FILE" "$PREFIX/$FILE"
done

cp "$SRC_ROOT/ship_files/config.4coder" "$PREFIX/config.4coder"
cp "$SRC_ROOT/ship_files/bindings.4coder" "$PREFIX/bindings.4coder"
rm -rf "$PREFIX/themes"
cp -r "$SRC_ROOT/ship_files/themes" "$PREFIX/themes"

# NOTE: The fonts normally come from 4coder-non-source; fall back to the system copies.
FOREIGN_FONTS="$PROJECT_ROOT/4coder-non-source/dist_files/fonts"
if [ -d "$FOREIGN_FONTS" ]; then
    cp "$FOREIGN_FONTS"/*.ttf "$PREFIX/fonts/"
fi
if [ ! -f "$PREFIX/fonts/liberation-mono.ttf" ]; then
    LIBERATION="$(fc-match -f '%{file}' 'Liberation Mono:style=Regular' 2>/dev/null || true)"
    if [ -z "$LIBERATION" ] || [ ! -f "$LIBERATION" ]; then
        echo "install: Liberation Mono not found, install it (e.g. 'sudo apt install fonts-liberation') and rerun"
        exit 1
    fi
    cp "$LIBERATION" "$PREFIX/fonts/liberation-mono.ttf"
fi
INCONSOLATA="$(fc-match -f '%{file}' 'Inconsolata:style=Regular' 2>/dev/null || true)"
if [ ! -f "$PREFIX/fonts/Inconsolata-Regular.ttf" ] && [[ "$INCONSOLATA" == *Inconsolata* ]]; then
    cp "$INCONSOLATA" "$PREFIX/fonts/Inconsolata-Regular.ttf"
fi

ln -sfn "$PREFIX/4ed" "$BIN_DIR/4coder"

cat > "$APPS_DIR/4coder.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=4coder
Comment=Code editor
Exec=$PREFIX/4ed %F
Icon=accessories-text-editor
Terminal=false
Categories=Development;TextEditor;
MimeType=text/plain;text/x-c;text/x-c++;text/x-java;
EOF

echo "install: 4coder installed to $PREFIX"
echo "install: run it with '4coder' (make sure $BIN_DIR is on your PATH)"
