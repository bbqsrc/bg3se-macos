#!/bin/bash
# Build a double-clickable .app that launches Baldur's Gate 3 with the BG3SE
# dylib injected via DYLD_INSERT_LIBRARIES — no game-binary modification.
#
# Usage: scripts/make_launcher_app.sh [output.app]
#   default output: /Applications/BG3 Script Extender.app
#   override game location with BG3_APP=/path/to/Baldur's Gate 3.app
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
TEMPLATE="$HERE/launcher_template"
OUT="${1:-/Applications/BG3 Script Extender.app}"
# Default the game path in a plain assignment — an apostrophe inside a
# ${var:-default} word is quote-active even within double quotes and breaks the
# parse, but a plain double-quoted string treats it literally.
BG3_APP="${BG3_APP:-}"
[ -n "$BG3_APP" ] || BG3_APP="/Applications/Baldur's Gate 3.app"

if [ ! -d "$BG3_APP" ]; then
    echo "error: Baldur's Gate 3.app not found at: $BG3_APP" >&2
    echo "       set BG3_APP=/path/to/Baldur's Gate 3.app and re-run" >&2
    exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT/Contents/MacOS" "$OUT/Contents/Resources"
cp "$TEMPLATE/Info.plist" "$OUT/Contents/Info.plist"
cp "$TEMPLATE/launch" "$OUT/Contents/MacOS/launch"

# Reuse BG3's own icon (copied from the install, not committed — it's Larian's
# art). If it isn't found, drop the icon reference so the bundle still validates.
ICON_SRC="$BG3_APP/Contents/Resources/BG3Icon.icns"
if [ -f "$ICON_SRC" ]; then
    cp "$ICON_SRC" "$OUT/Contents/Resources/BG3Icon.icns"
else
    echo "warning: $ICON_SRC not found — launcher will use the default icon" >&2
    /usr/bin/plutil -remove CFBundleIconFile "$OUT/Contents/Info.plist" >/dev/null 2>&1 || true
fi

# Point the launcher at this install's game bundle (default is /Applications).
/usr/bin/sed -i '' "s|^BG3_APP=.*|BG3_APP=\"$BG3_APP\"|" "$OUT/Contents/MacOS/launch"
chmod +x "$OUT/Contents/MacOS/launch"

# Ad-hoc sign so the bundle is self-consistent. Locally-created apps are not
# quarantined, so Gatekeeper does not block them.
codesign --force --sign - "$OUT" >/dev/null 2>&1 || true

# Nudge Finder/LaunchServices to pick up the icon for an existing bundle path.
touch "$OUT" 2>/dev/null || true

echo "created: $OUT"
