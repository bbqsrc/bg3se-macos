#!/bin/bash
# Auto-deploy libbg3se.dylib into the BG3 app bundle after build.
# Resolves the bundle via the harness config so detection (Steam/GOG/...) lives
# in one place.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DYLIB="$PROJECT_DIR/build/lib/libbg3se.dylib"

# Single source of truth: ask the Python config for the detected app bundle.
APP_BUNDLE="$(cd "$PROJECT_DIR" && PYTHONPATH=tools python3 -c \
    "from bg3se_harness.config import BG3_APP_BUNDLE; print(BG3_APP_BUNDLE)")"
# Deploy inside .app bundle where the dylib is loaded from (@loader_path)
TARGET_DYLIB="$APP_BUNDLE/Contents/MacOS/libbg3se.dylib"

if [[ ! -f "$BUILD_DYLIB" ]]; then
    echo "Error: Build dylib not found at $BUILD_DYLIB"
    exit 1
fi

if [[ -z "$APP_BUNDLE" || ! -d "$APP_BUNDLE" ]]; then
    echo "Error: BG3 app bundle not found (detected: '$APP_BUNDLE')"
    echo "       Set \$BG3SE_APP_BUNDLE if BG3 is installed in a custom location."
    exit 1
fi

# Skip if the deployed copy is already current (portable: bash -nt, no stat).
if [[ -f "$TARGET_DYLIB" && ! "$BUILD_DYLIB" -nt "$TARGET_DYLIB" ]]; then
    echo "Deployed dylib is up to date ($APP_BUNDLE)"
    exit 0
fi

cp "$BUILD_DYLIB" "$TARGET_DYLIB"
echo "Deployed to $APP_BUNDLE: $(ls -lh "$TARGET_DYLIB" | awk '{print $5, $6, $7, $8}')"
