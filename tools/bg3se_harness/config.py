import os
import plistlib
import subprocess
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent

# Steam's standard install location — also used as the not-found fallback so
# doctor can report a sensible path.
_STEAM_BUNDLE = (
    Path.home()
    / "Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app"
)


def detect_bg3_app_bundle() -> Path:
    """Locate the BG3 .app bundle across stores (Steam, GOG, ...).

    BG3 stores all user data under ~/Documents/Larian Studios and keys off the
    com.larian.bg3 bundle id regardless of store, so only the install location
    differs. Returns the first existing candidate; falls back to the Steam path.
    """
    # 1. Explicit override for unusual installs.
    override = os.environ.get("BG3SE_APP_BUNDLE")
    if override:
        return Path(override).expanduser()

    home = Path.home()
    candidates = [
        _STEAM_BUNDLE,                                              # Steam
        Path("/Applications/Baldur's Gate 3.app"),                 # GOG default
        home / "Applications/Baldur's Gate 3.app",
        home / "GOG Games/Baldur's Gate 3/Baldur's Gate 3.app",
        home / "GOG Games/Baldurs Gate 3/Baldur's Gate 3.app",
    ]
    for c in candidates:
        if c.exists():
            return c

    # 2. Last resort: ask Spotlight for the bundle by id. Skip the GOG Galaxy
    #    launcher stub ("GOG Galaxy - Baldur's Gate 3.app"), which is not the game.
    try:
        out = subprocess.run(
            ["mdfind", "kMDItemCFBundleIdentifier == 'com.larian.bg3'"],
            capture_output=True, text=True, timeout=5,
        ).stdout
        for line in out.splitlines():
            line = line.strip()
            if line.endswith("Baldur's Gate 3.app") and "GOG Galaxy" not in line:
                return Path(line)
    except (OSError, subprocess.SubprocessError):
        pass

    return _STEAM_BUNDLE


def detect_bg3_executable(bundle: Path) -> Path:
    """Locate the real game binary inside the bundle.

    GOG ships a tiny launcher stub ('Baldur's Gate 3', ~200KB) that execs the
    real game ('Baldur's Gate 3 GOG', ~480MB); on Steam the main executable IS
    the game. Launching the launcher leaves the real game to default to Rosetta
    (x86_64) — fatal for our arm64 offsets — so target the real binary directly.
    Heuristic: the largest Mach-O in Contents/MacOS (launcher stubs are tiny),
    excluding our own dylib and any patch backup. Falls back to CFBundleExecutable.
    """
    macos = bundle / "Contents/MacOS"
    fallback = macos / "Baldur's Gate 3"
    try:
        with open(bundle / "Contents/Info.plist", "rb") as f:
            exe = plistlib.load(f).get("CFBundleExecutable")
        if exe:
            fallback = macos / exe
    except (OSError, plistlib.InvalidFileException):
        pass

    try:
        candidates = [
            p for p in macos.iterdir()
            if p.is_file()
            and p.name != "libbg3se.dylib"
            and not p.name.endswith((".dylib", ".bg3se-original"))
            and not p.name.startswith(".")
        ]
        if candidates:
            # Largest file = the real game; the launcher stub is orders smaller.
            return max(candidates, key=lambda p: p.stat().st_size)
    except OSError:
        pass
    return fallback


BG3_APP_BUNDLE = detect_bg3_app_bundle()
BG3_EXEC = detect_bg3_executable(BG3_APP_BUNDLE)
DYLIB_OUTPUT = PROJECT_ROOT / "build/lib/libbg3se.dylib"
DEPLOYED_DYLIB = BG3_APP_BUNDLE / "Contents/MacOS/libbg3se.dylib"
SOCKET_PATH = "/tmp/bg3se.sock"
SENTINEL_PATH = "/tmp/bg3se_loaded.txt"
HEALTH_TIMEOUT = 30
HEALTH_TIMEOUT_CONTINUE = 180  # Save loading takes 30-60s+ on top of launch
BACKUP_SUFFIX = ".bg3se-original"
HASH_FILE = BG3_APP_BUNDLE / "Contents/MacOS/.bg3se-patch-hash"
INSERT_DYLIB = PROJECT_ROOT / "tools/vendor/insert_dylib/insert_dylib_bin"
DYLIB_INSTALL_NAME = "@loader_path/libbg3se.dylib"

# Harness process tracking
PID_FILE = Path("/tmp/bg3se_harness.pid")
HEALTH_FILE = Path("/tmp/bg3se_health.json")
MONITOR_LOG = Path("/tmp/bg3se_monitor.log")

# Mod management paths
LARIAN_LOCAL = Path.home() / "Documents/Larian Studios/Baldur's Gate 3"
MODS_DIR = LARIAN_LOCAL / "Mods"
MODSETTINGS_PATH = LARIAN_LOCAL / "PlayerProfiles/Public/modsettings.lsx"
GRAPHIC_SETTINGS_PATH = LARIAN_LOCAL / "graphicSettings.lsx"
SAVES_DIR = LARIAN_LOCAL / "PlayerProfiles/Public/Savegames/Story"
MOD_CRASH_SANITY_CHECK_DIR = LARIAN_LOCAL / "ModCrashSanityCheck"

# Harness data paths
HARNESS_CONFIG_DIR = Path.home() / ".config/bg3se-harness"
MOD_REGISTRY_PATH = HARNESS_CONFIG_DIR / "mod_registry.json"
SAVE_FIXTURES_DIR = HARNESS_CONFIG_DIR / "save_fixtures"
REPORTS_DIR = PROJECT_ROOT / ".reports"

# Catalog paths (shipped with repo)
CATALOG_DIR = Path(__file__).resolve().parent / "catalog"
SCENARIOS_DIR = Path(__file__).resolve().parent / "scenarios"

# GustavX invariant — must always be at position 0 in modsettings.lsx
# Match by name, not UUID — the UUID changes between game versions
GUSTAVX_NAME = "GustavX"
