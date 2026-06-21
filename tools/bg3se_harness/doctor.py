"""Prerequisite verifier for bg3se-harness.

Checks that all required paths, permissions, and tools are available.
Reports actionable diagnostics as JSON.

Usage:
    bg3se-harness doctor
"""

from __future__ import annotations

import json
import os
import plistlib
import re
import subprocess
import sys
from pathlib import Path

import stat

from .config import (
    BG3_APP_BUNDLE, BG3_EXEC, DEPLOYED_DYLIB, HARNESS_CONFIG_DIR,
    INSERT_DYLIB, MOD_CRASH_SANITY_CHECK_DIR, MODS_DIR, MODSETTINGS_PATH,
    SAVES_DIR, SOCKET_PATH, PROJECT_ROOT,
)
from . import launch as launch_mod
from . import patch as patch_mod


def _check(name, passed, detail=None, fix=None):
    """Build a check result dict."""
    result = {"name": name, "passed": passed}
    if detail:
        result["detail"] = detail
    if fix and not passed:
        result["fix"] = fix
    return result


def _bundle_version():
    """Read CFBundleShortVersionString from the detected BG3 bundle, or None."""
    plist = BG3_APP_BUNDLE / "Contents/Info.plist"
    try:
        with open(plist, "rb") as f:
            data = plistlib.load(f)
        return data.get("CFBundleShortVersionString") or data.get("CFBundleVersion")
    except (OSError, plistlib.InvalidFileException):
        return None


def _known_good_version():
    """Parse BG3_KNOWN_VERSION from src/core/version_detect.h (single source)."""
    header = PROJECT_ROOT / "src/core/version_detect.h"
    try:
        m = re.search(r'#define\s+BG3_KNOWN_VERSION\s+"([^"]+)"', header.read_text())
        return m.group(1) if m else None
    except OSError:
        return None


def run_doctor():
    """Run all diagnostic checks. Returns dict with checks array and summary."""
    checks = []

    # 1. BG3 app bundle
    checks.append(_check(
        "bg3_app_bundle",
        BG3_APP_BUNDLE.exists(),
        detail=str(BG3_APP_BUNDLE),
        fix="Install BG3 (Steam or GOG), or set $BG3SE_APP_BUNDLE to its path",
    ))

    # 2. BG3 binary
    checks.append(_check(
        "bg3_binary",
        BG3_EXEC.exists(),
        detail=str(BG3_EXEC),
    ))

    # 2b. Game version vs. the offsets' known-good build. A newer build (any
    # store) doesn't fail — version_detect gracefully gates address-baked
    # features — but we surface the delta so it's not a surprise in-game.
    detected = _bundle_version()
    known = _known_good_version()
    if detected and known:
        if detected == known:
            ver_passed, ver_detail = True, f"{detected} (matches known-good)"
        elif detected.split(".")[:3] == known.split(".")[:3]:
            ver_passed = True
            ver_detail = (
                f"{detected} — newer build than known-good {known}; "
                "address-baked features (Stats, prototype managers) may degrade"
            )
        else:
            ver_passed = False
            ver_detail = (
                f"{detected} differs from known-good {known}; "
                "address-dependent features likely disabled"
            )
    else:
        ver_passed = False
        ver_detail = f"detected={detected or '?'} known-good={known or '?'}"
    checks.append(_check(
        "game_version",
        ver_passed,
        detail=ver_detail,
        fix="Offsets were verified for the known-good build; newer patches may "
            "need offset updates for full feature coverage",
    ))

    # 3. SE dylib built
    dylib_built = (PROJECT_ROOT / "build/lib/libbg3se.dylib").exists()
    checks.append(_check(
        "se_dylib_built",
        dylib_built,
        fix="Run: bg3se-harness build",
    ))

    # 4. SE dylib deployed
    checks.append(_check(
        "se_dylib_deployed",
        DEPLOYED_DYLIB.exists(),
        detail=str(DEPLOYED_DYLIB),
        fix="Run: bg3se-harness build (auto-deploys)",
    ))

    # 5. Binary patched
    patched = False
    try:
        patched = patch_mod.is_patched()
    except Exception:
        pass
    checks.append(_check(
        "binary_patched",
        patched,
        fix="Run: bg3se-harness patch — or use 'launch --inject dyld' to skip "
            "patching (GOG/non-Steam installs without Hardened Runtime)",
    ))

    # 6. insert_dylib available
    checks.append(_check(
        "insert_dylib",
        INSERT_DYLIB.exists(),
        detail=str(INSERT_DYLIB),
        fix="Build insert_dylib from tools/vendor/insert_dylib/",
    ))

    # 7. Mods directory
    checks.append(_check(
        "mods_directory",
        MODS_DIR.exists(),
        detail=str(MODS_DIR),
        fix="Launch BG3 at least once to create Larian directories",
    ))

    # 8. modsettings.lsx
    modsettings_ok = MODSETTINGS_PATH.exists()
    checks.append(_check(
        "modsettings_lsx",
        modsettings_ok,
        detail=str(MODSETTINGS_PATH),
        fix="Launch BG3 at least once",
    ))

    # 9. Save directory
    checks.append(_check(
        "save_directory",
        SAVES_DIR.exists(),
        detail=str(SAVES_DIR),
    ))

    # 10. Harness config dir writable
    try:
        HARNESS_CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        test_file = HARNESS_CONFIG_DIR / ".doctor_test"
        test_file.write_text("ok")
        test_file.unlink()
        config_ok = True
    except OSError:
        config_ok = False
    checks.append(_check(
        "harness_config_writable",
        config_ok,
        detail=str(HARNESS_CONFIG_DIR),
    ))

    # 11. Game running?
    game_running = launch_mod.is_running()
    checks.append(_check(
        "game_running",
        game_running,
        detail="BG3 process detected" if game_running else "BG3 not running",
    ))

    # 12. Socket alive?
    socket_alive = launch_mod.socket_alive()
    checks.append(_check(
        "se_socket",
        socket_alive,
        detail=SOCKET_PATH,
    ))

    # 13. Accessibility permission (for menu automation)
    accessibility_ok = False
    try:
        result = subprocess.run(
            ["osascript", "-e",
             'tell application "System Events" to get name of first process'],
            capture_output=True, text=True, timeout=5,
        )
        accessibility_ok = result.returncode == 0
    except (subprocess.TimeoutExpired, OSError):
        pass
    checks.append(_check(
        "accessibility_permission",
        accessibility_ok,
        fix="System Settings > Privacy & Security > Accessibility > enable terminal app",
    ))

    # 14. BG3MacModManager installed?
    mmgr_installed = False
    mmgr_detail = "Not found"
    for app_dir in [Path.home() / "Applications", Path("/Applications")]:
        mmgr_path = app_dir / "BG3 Mac Mod Manager.app"
        if mmgr_path.exists():
            mmgr_installed = True
            mmgr_detail = str(mmgr_path)
            break
    checks.append(_check(
        "bg3macmodmanager",
        mmgr_installed,
        detail=mmgr_detail,
        fix="Optional: https://github.com/ShaiLaric/BG3MacModManager",
    ))

    # 15. NoLauncher defaults set?
    nolauncher = False
    try:
        result = subprocess.run(
            ["defaults", "read", "com.larian.bg3", "NoLauncher"],
            capture_output=True, text=True,
        )
        nolauncher = result.stdout.strip() == "1"
    except OSError:
        pass
    checks.append(_check(
        "no_launcher_bypass",
        nolauncher,
        fix="Run: defaults write com.larian.bg3 NoLauncher 1",
    ))

    # 16. ModCrashSanityCheck directory (Patch 8+ footgun)
    sanity_exists = MOD_CRASH_SANITY_CHECK_DIR.exists()
    checks.append(_check(
        "mod_crash_sanity_check",
        not sanity_exists,
        detail="Not present (good)" if not sanity_exists else str(MOD_CRASH_SANITY_CHECK_DIR),
        fix=(
            "Delete this directory — since Patch 8, BG3 deactivates externally-managed "
            f"mods when it exists: rm -rf \"{MOD_CRASH_SANITY_CHECK_DIR}\""
        ),
    ))

    # 17. modsettings.lsx file locking (chflags uchg)
    modsettings_locked = False
    if MODSETTINGS_PATH.exists():
        try:
            modsettings_locked = bool(os.stat(MODSETTINGS_PATH).st_flags & stat.UF_IMMUTABLE)
        except (OSError, AttributeError):
            pass
    checks.append(_check(
        "modsettings_unlocked",
        not modsettings_locked,
        detail="Locked (chflags uchg)" if modsettings_locked else "Writable",
        fix=f'Unlock: chflags nouchg "{MODSETTINGS_PATH}"',
    ))

    # Summary
    passed = sum(1 for c in checks if c["passed"])
    total = len(checks)

    return {
        "checks": checks,
        "passed": passed,
        "total": total,
        "all_passed": passed == total,
    }


def cmd_doctor(args):
    """CLI handler for doctor command."""
    result = run_doctor()
    print(json.dumps(result, indent=2))

    # Also print human-readable summary to stderr
    for check in result["checks"]:
        icon = "OK" if check["passed"] else "FAIL"
        line = f"  [{icon}] {check['name']}"
        if "detail" in check:
            line += f" — {check['detail']}"
        print(line, file=sys.stderr)
        if not check["passed"] and "fix" in check:
            print(f"         Fix: {check['fix']}", file=sys.stderr)

    print(f"\n  {result['passed']}/{result['total']} checks passed", file=sys.stderr)
    return 0 if result.get("all_passed") else 1
