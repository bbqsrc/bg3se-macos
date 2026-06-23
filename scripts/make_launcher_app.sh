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
mkdir -p "$OUT/Contents/MacOS"
cp "$TEMPLATE/Info.plist" "$OUT/Contents/Info.plist"
cp "$TEMPLATE/launch" "$OUT/Contents/MacOS/launch"

# Point the launcher at this install's game bundle (default is /Applications).
/usr/bin/sed -i '' "s|^BG3_APP=.*|BG3_APP=\"$BG3_APP\"|" "$OUT/Contents/MacOS/launch"
chmod +x "$OUT/Contents/MacOS/launch"

# Ad-hoc sign so the bundle is self-consistent. Locally-created apps are not
# quarantined, so Gatekeeper does not block them.
codesign --force --sign - "$OUT" >/dev/null 2>&1 || true

echo "created: $OUT"
