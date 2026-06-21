from __future__ import annotations

import json as _json
import os
import re
import signal
import shutil
import socket
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

from .config import (
    BG3_EXEC, DYLIB_OUTPUT, GRAPHIC_SETTINGS_PATH, HEALTH_TIMEOUT,
    HEALTH_TIMEOUT_CONTINUE, HARNESS_CONFIG_DIR, HEALTH_FILE, PID_FILE,
    PROJECT_ROOT, SOCKET_PATH,
)
from .flags import build_flag_args


HEADLESS_GRAPHICS_RESTORE_PATH = HARNESS_CONFIG_DIR / "graphic_settings_headless_restore.json"
HEADLESS_GRAPHICS_ENTRIES = {
    "Fullscreen": 0,
    "FakeFullscreenEnabled": 0,
    "FakeFullscreen": 0,
    "ScreenWidth": 1280,
    "ScreenHeight": 720,
}


def _read_pid_file():
    """Read harness-owned PID from file. Returns int or None."""
    try:
        data = _json.loads(PID_FILE.read_text())
        pid = data.get("pid")
        if pid and _pid_is_bg3(pid):
            return pid
    except (FileNotFoundError, ValueError, _json.JSONDecodeError):
        pass
    return None


def _write_pid_file(pid):
    PID_FILE.write_text(_json.dumps({"pid": pid, "launched_at": time.time()}))


def _clear_pid_file():
    try:
        PID_FILE.unlink()
    except FileNotFoundError:
        pass


def _pid_is_bg3(pid):
    """Check if a PID belongs to a BG3 process."""
    try:
        r = subprocess.run(
            ["ps", "-p", str(pid), "-o", "comm="],
            capture_output=True, text=True,
        )
        return "Baldur" in r.stdout
    except OSError:
        return False


def kill_existing(force_all=False):
    """Kill harness-owned BG3 process. Only blanket-kills if force_all."""
    pid = _read_pid_file()
    if pid:
        try:
            os.kill(pid, signal.SIGTERM)
            time.sleep(1)
        except ProcessLookupError:
            pass
        _clear_pid_file()
        return

    if force_all:
        subprocess.run(
            ["pkill", "-f", "Baldur's Gate 3"],
            capture_output=True,
        )
        time.sleep(1)


def clean_socket():
    try:
        os.unlink(SOCKET_PATH)
    except FileNotFoundError:
        pass


def ensure_no_launcher():
    """Set com.larian.bg3 NoLauncher=1 to bypass the Larian WebKit launcher."""
    subprocess.run(
        ["defaults", "write", "com.larian.bg3", "NoLauncher", "1"],
        capture_output=True,
    )


def ensure_skip_videos():
    """Set SkipVideo + SkipSplashScreen via defaults and graphicSettings.lsx.

    Best-effort — never blocks launch on failure. This is the legacy
    approach; prepare_video_skip() + -mediaPath is preferred.
    """
    try:
        # Layer 1: macOS UserDefaults (mirrors ensure_no_launcher pattern)
        subprocess.run(
            ["defaults", "write", "com.larian.bg3", "SkipVideo", "-bool", "true"],
            capture_output=True,
        )

        # Layer 2: graphicSettings.lsx ConfigEntry injection
        _upsert_graphic_settings({"SkipVideo": 1, "SkipSplashScreen": 1})
    except Exception as exc:
        print(f"Warning: skip-videos setup failed: {exc}", file=sys.stderr)


def _load_graphic_settings_tree():
    """Return (tree, config_children) for graphicSettings.lsx, or (None, None)."""
    path = GRAPHIC_SETTINGS_PATH
    if not path.exists():
        return None, None

    try:
        tree = ET.parse(path)
    except (ET.ParseError, OSError) as exc:
        print(f"Warning: could not parse {path}: {exc}", file=sys.stderr)
        return None, None

    root = tree.getroot()

    # Find the <children> node that holds ConfigEntry nodes
    config_children = None
    for children in root.iter("children"):
        for node in children.findall("node"):
            if node.get("id") == "ConfigEntry":
                config_children = children
                break
        if config_children is not None:
            break

    return tree, config_children


def _entry_map_key(node):
    for attr in node.findall("attribute"):
        if attr.get("id") == "MapKey":
            return attr.get("value")
    return None


def _index_graphic_settings(config_children):
    """Return a dict mapping MapKey value to ConfigEntry element."""
    existing = {}
    if config_children is None:
        return existing
    for node in config_children.findall("node"):
        if node.get("id") != "ConfigEntry":
            continue
        key = _entry_map_key(node)
        if key is not None:
            existing[key] = node
    return existing


def _find_config_attribute(entry, attr_id):
    for attr in entry.findall("attribute"):
        if attr.get("id") == attr_id:
            return attr
    return None


def _set_config_attribute(entry, attr_id, attr_type, value):
    attr = _find_config_attribute(entry, attr_id)
    changed = False
    if attr is None:
        attr = ET.SubElement(entry, "attribute", id=attr_id)
        changed = True
    if attr.get("type") != attr_type:
        attr.set("type", attr_type)
        changed = True
    if attr.get("value") != str(value):
        attr.set("value", str(value))
        changed = True
    return changed


def _snapshot_graphic_settings(keys):
    """Return JSON-serializable original state for keys."""
    tree, config_children = _load_graphic_settings_tree()
    if tree is None or config_children is None:
        return None

    existing = _index_graphic_settings(config_children)
    nodes = list(config_children.findall("node"))
    snapshot = {
        "path": str(GRAPHIC_SETTINGS_PATH),
        "entries": {},
    }
    for key in keys:
        node = existing.get(key)
        if node is None:
            snapshot["entries"][key] = {"existed": False}
            continue
        snapshot["entries"][key] = {
            "existed": True,
            "index": nodes.index(node) if node in nodes else None,
            "node_attrib": dict(node.attrib),
            "attributes": [dict(attr.attrib) for attr in node.findall("attribute")],
        }
    return snapshot


def _write_graphic_settings_tree(tree):
    path = GRAPHIC_SETTINGS_PATH
    bak = path.with_suffix(".lsx.bak")
    if not bak.exists():
        shutil.copy2(path, bak)
    tree.write(path, xml_declaration=True, encoding="unicode")


def _write_int_graphic_settings(entries):
    """Insert or update integer ConfigEntry values."""
    tree, config_children = _load_graphic_settings_tree()
    if tree is None or config_children is None:
        return {"success": False, "error": f"could not load {GRAPHIC_SETTINGS_PATH}"}

    existing = _index_graphic_settings(config_children)

    changed = False
    for key, value in entries.items():
        if key in existing:
            entry = existing[key]
            changed = _set_config_attribute(entry, "Type", "int32", 0) or changed
            changed = _set_config_attribute(entry, "Value", "int32", value) or changed
        else:
            # Create new ConfigEntry node
            entry = ET.SubElement(config_children, "node", id="ConfigEntry")
            ET.SubElement(entry, "attribute", id="MapKey", type="FixedString", value=key)
            ET.SubElement(entry, "attribute", id="Type", type="int32", value="0")
            ET.SubElement(entry, "attribute", id="Value", type="int32", value=str(value))
            changed = True

    if changed:
        _write_graphic_settings_tree(tree)

    return {"success": True, "changed": changed, "path": str(GRAPHIC_SETTINGS_PATH)}


def _restore_graphic_settings_snapshot(snapshot):
    """Restore existing entries and remove entries that were originally missing."""
    tree, config_children = _load_graphic_settings_tree()
    if tree is None or config_children is None:
        return {"success": False, "error": f"could not load {GRAPHIC_SETTINGS_PATH}"}

    existing = _index_graphic_settings(config_children)
    changed = False
    restored = []
    removed = []

    for key, state in snapshot.get("entries", {}).items():
        node = existing.get(key)
        if not state.get("existed", False):
            if node is not None:
                config_children.remove(node)
                changed = True
                removed.append(key)
            continue

        node_attrib = dict(state.get("node_attrib") or {"id": "ConfigEntry"})
        attributes = list(state.get("attributes") or [])

        if node is None:
            node = ET.Element("node", node_attrib)
            for attr_state in attributes:
                ET.SubElement(node, "attribute", attr_state)
            index = state.get("index")
            children = list(config_children)
            if isinstance(index, int) and 0 <= index < len(children):
                config_children.insert(index, node)
            else:
                config_children.append(node)
            changed = True
            restored.append(key)
            continue

        if dict(node.attrib) != node_attrib:
            node.attrib.clear()
            node.attrib.update(node_attrib)
            changed = True

        current_attributes = [dict(attr.attrib) for attr in node.findall("attribute")]
        if current_attributes != attributes:
            for child in list(node):
                node.remove(child)
            for attr_state in attributes:
                ET.SubElement(node, "attribute", attr_state)
            changed = True
        restored.append(key)

    if changed:
        _write_graphic_settings_tree(tree)

    return {
        "success": True,
        "changed": changed,
        "restored": restored,
        "removed": removed,
        "path": str(GRAPHIC_SETTINGS_PATH),
    }


def _upsert_graphic_settings(entries: dict[str, int]) -> None:
    """Insert or update ConfigEntry nodes in graphicSettings.lsx."""
    result = _write_int_graphic_settings(entries)
    if not result.get("success"):
        print(f"Warning: {result.get('error')}", file=sys.stderr)


def prepare_headless_graphics():
    """Temporarily force BG3 graphics settings to normal windowed mode."""
    result = {
        "success": False,
        "path": str(GRAPHIC_SETTINGS_PATH),
        "restore_path": str(HEADLESS_GRAPHICS_RESTORE_PATH),
        "entries": HEADLESS_GRAPHICS_ENTRIES,
        "snapshot_created": False,
    }

    try:
        HARNESS_CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        if not HEADLESS_GRAPHICS_RESTORE_PATH.exists():
            snapshot = _snapshot_graphic_settings(HEADLESS_GRAPHICS_ENTRIES.keys())
            if snapshot is None:
                result["error"] = f"could not snapshot {GRAPHIC_SETTINGS_PATH}"
                return result
            HEADLESS_GRAPHICS_RESTORE_PATH.write_text(
                _json.dumps(snapshot, indent=2),
            )
            result["snapshot_created"] = True

        write_result = _write_int_graphic_settings(HEADLESS_GRAPHICS_ENTRIES)
        result.update(write_result)
        result["restore_path"] = str(HEADLESS_GRAPHICS_RESTORE_PATH)
        result["entries"] = HEADLESS_GRAPHICS_ENTRIES
        return result
    except (OSError, ET.ParseError) as exc:
        result["error"] = str(exc)
        return result


def restore_headless_graphics(reason=""):
    """Restore graphics settings saved by prepare_headless_graphics()."""
    result = {
        "success": True,
        "path": str(GRAPHIC_SETTINGS_PATH),
        "restore_path": str(HEADLESS_GRAPHICS_RESTORE_PATH),
        "reason": reason,
        "restored": [],
        "removed": [],
    }

    if not HEADLESS_GRAPHICS_RESTORE_PATH.exists():
        result["restored"] = False
        result["noop"] = True
        return result

    try:
        snapshot = _json.loads(HEADLESS_GRAPHICS_RESTORE_PATH.read_text())
        restore_result = _restore_graphic_settings_snapshot(snapshot)
        result.update(restore_result)
        result["restore_path"] = str(HEADLESS_GRAPHICS_RESTORE_PATH)
        result["reason"] = reason
        if restore_result.get("success"):
            HEADLESS_GRAPHICS_RESTORE_PATH.unlink()
        return result
    except (_json.JSONDecodeError, OSError, ET.ParseError) as exc:
        result["success"] = False
        result["error"] = str(exc)
        return result


def launch(continue_game=False, load_save=None, extra_flags=None,
           skip_videos=True, auto_dismiss=True, headless=False, inject="patch"):
    kill_existing(force_all=True)
    clean_socket()
    ensure_no_launcher()
    headless_graphics = None

    if headless:
        headless_graphics = prepare_headless_graphics()
        if not headless_graphics.get("success"):
            print(
                f"Warning: headless graphics setup failed: {headless_graphics.get('error', 'unknown')}",
                file=sys.stderr,
            )

    if skip_videos:
        ensure_skip_videos()

    if inject == "dyld":
        # No binary patch: load the dylib via DYLD_INSERT_LIBRARIES instead.
        # Works on installs without Hardened Runtime / library validation (e.g.
        # GOG launched directly). `arch -e` injects the var into the game's own
        # spawn — `arch` itself is a platform binary so dyld would strip an
        # inherited DYLD_* var, but -e sets it on the (unrestricted) child, and
        # -arm64 still applies to the game (unlike piping through `env`, which
        # leaves the game to default to Rosetta/x86_64).
        if not DYLIB_OUTPUT.exists():
            raise FileNotFoundError(
                f"dyld injection needs a built dylib at {DYLIB_OUTPUT} — run the build first"
            )
        cmd = ["arch", "-arm64", "-e",
               f"DYLD_INSERT_LIBRARIES={DYLIB_OUTPUT}", str(BG3_EXEC)]
    else:
        cmd = ["arch", "-arm64", str(BG3_EXEC)]

    if continue_game:
        cmd.append("-continueGame")
    elif load_save:
        cmd.extend(["-loadSaveGame", load_save])

    if extra_flags:
        cmd.extend(build_flag_args(extra_flags))

    env = os.environ.copy()
    if skip_videos:
        env["BG3SE_SKIP_VIDEOS"] = "1"
    if auto_dismiss:
        env["BG3SE_AUTO_DISMISS_SPLASH"] = "1"
    if headless:
        env["BG3SE_MUTE_AUDIO"] = "1"

    print(f"[harness] phase: process_launching", file=sys.stderr)
    proc = subprocess.Popen(
        cmd,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
        env=env,
    )
    proc.bg3se_headless_graphics = headless_graphics
    _write_pid_file(proc.pid)
    flags_desc = " ".join(cmd[3:]) if len(cmd) > 3 else "(no extra flags)"
    print(f"[harness] phase: process_launched (pid {proc.pid}) [{flags_desc}]",
          file=sys.stderr)
    if auto_dismiss:
        print(f"[harness] BG3SE_AUTO_DISMISS_SPLASH=1 (in-process splash dismiss)",
              file=sys.stderr)

    if headless:
        print("[harness] headless-after-boot: BG3 visible during boot, will hide after socket ready",
              file=sys.stderr)

    return proc


def default_timeout(continue_game=False, load_save=None):
    """Return appropriate timeout — loading a save takes longer."""
    if continue_game or load_save:
        return HEALTH_TIMEOUT_CONTINUE
    return HEALTH_TIMEOUT


_MAX_DISMISS_ATTEMPTS = 20
_MENU_STALL_FIRST_ACTION_S = 20.0
_MENU_STALL_RETRY_INTERVAL_S = 12.0
_MENU_STALL_MAX_ACTIONS = 6
_MENU_STALL_ABORT_S = 120.0
BOOT_RETRYABLE_STAGES = {"timeout", "menu_stalled"}
_CONTINUE_CLICK_FRACTIONS = [
    # Measured from .screenshots/boot-stall-1778937630.png after watching
    # the cursor land right/below the button. Fractions are relative to the
    # full System Events window bounds, including the macOS title bar.
    {"x": 0.293, "y": 0.380, "label": "continue_button_center"},
    {"x": 0.293, "y": 0.370, "label": "continue_button_upper_center"},
    {"x": 0.293, "y": 0.390, "label": "continue_button_lower_center"},
]
_SPLASH_CLICK_FRACTION = {"x": 0.5, "y": 0.5, "label": "splash_center"}
_MOD_VERIFICATION_START_FRACTIONS = [
    # "Mod Verification" can appear after Continue when BG3 sees local mod
    # changes. Fractions are relative to the full System Events window bounds.
    {"x": 0.443, "y": 0.889, "label": "mod_verification_start_game_center"},
    {"x": 0.443, "y": 0.899, "label": "mod_verification_start_game_lower_center"},
]
_MOD_VERIFICATION_CHECKBOX_FRACTIONS = [
    # Right-column enable/acknowledge boxes in the Mod Verification list.
    # Do not include the "New Mods Detected" row that BG3 already checks.
    {"x": 0.783, "y": 0.306, "label": "mod_verification_checkbox_1"},
    {"x": 0.783, "y": 0.381, "label": "mod_verification_checkbox_2"},
    {"x": 0.783, "y": 0.456, "label": "mod_verification_checkbox_3"},
    {"x": 0.783, "y": 0.531, "label": "mod_verification_checkbox_4"},
    {"x": 0.783, "y": 0.606, "label": "mod_verification_checkbox_5"},
    {"x": 0.783, "y": 0.681, "label": "mod_verification_checkbox_6"},
]
_WATCHDOG_CLICK_SEQUENCE = [
    ("dismiss_splash", _SPLASH_CLICK_FRACTION),
    ("click_continue", _CONTINUE_CLICK_FRACTIONS[0]),
    ("check_mod_verification_boxes", _MOD_VERIFICATION_CHECKBOX_FRACTIONS),
    ("click_mod_verification_start_game", _MOD_VERIFICATION_START_FRACTIONS[0]),
    ("check_mod_verification_boxes", _MOD_VERIFICATION_CHECKBOX_FRACTIONS),
    ("click_mod_verification_start_game", _MOD_VERIFICATION_START_FRACTIONS[1]),
]


def _watchdog_target_for_attempt(attempt):
    index = min(max(attempt - 1, 0), len(_WATCHDOG_CLICK_SEQUENCE) - 1)
    return _WATCHDOG_CLICK_SEQUENCE[index]


def wait_for_socket(timeout=HEALTH_TIMEOUT, dismiss_splash=False, process=None):
    """Wait for the SE socket to respond to Lua commands.

    When dismiss_splash is True, periodically sends Escape+Space via
    System Events key code to dismiss the BG3 splash screen while waiting.
    Dismissal stops as soon as the socket accepts a connection (splash
    is gone at that point) or after _MAX_DISMISS_ATTEMPTS, whichever
    comes first. Only returns success when the socket responds to a
    command, not merely when it accepts a connection.

    If process is provided, polls process.poll() each iteration and
    returns early with stage="process_exited" if BG3 dies during boot.

    The returned dict includes a "phases" list of {t, phase} dicts
    recording each stage transition with elapsed time in seconds.
    """
    start = time.monotonic()
    interval = 0.5
    dismiss_delay = 5.0
    dismiss_interval = 3.0
    ocr_interval = 5.0
    last_dismiss = 0.0
    last_ocr = 0.0
    dismiss_count = 0
    socket_ever_connected = False
    log_file_detected = False
    menu_detected = False
    continue_sent = False
    socket_listening_at = None
    menu_watchdog_actions = []
    phases = []

    def _phase(name, detail=""):
        elapsed = time.monotonic() - start
        entry = {"t": round(elapsed, 1), "phase": name}
        if detail:
            entry["detail"] = detail
        phases.append(entry)
        msg = f"[harness] phase: {name}"
        if detail:
            msg += f" ({detail})"
        else:
            msg += f" ({elapsed:.1f}s)"
        print(msg, file=sys.stderr)

    _phase("waiting_for_socket", f"timeout={timeout}s")

    while (time.monotonic() - start) < timeout:
        if process is not None and process.poll() is not None:
            elapsed_ms = int((time.monotonic() - start) * 1000)
            _phase("process_exited", f"code={process.returncode}, {elapsed_ms}ms")
            return {
                "socket_connected": False,
                "stage": "process_exited",
                "exitcode": process.returncode,
                "elapsed_ms": elapsed_ms,
                "phases": phases,
                "diagnostics": _collect_boot_diagnostics(),
            }

        elapsed = time.monotonic() - start

        if not log_file_detected:
            log_dir = os.path.expanduser(
                "~/Library/Application Support/BG3SE/logs")
            latest = os.path.join(log_dir, "latest.log")
            if os.path.exists(latest):
                log_file_detected = True
                _phase("dylib_loaded")

        bg3_pid = process.pid if process else None

        if socket_ever_connected:
            stalled_for = elapsed - (socket_listening_at or elapsed)
            should_act = (
                dismiss_splash
                and bg3_pid
                and stalled_for >= _MENU_STALL_FIRST_ACTION_S
                and len(menu_watchdog_actions) < _MENU_STALL_MAX_ACTIONS
                and (
                    not menu_watchdog_actions
                    or elapsed - menu_watchdog_actions[-1].get("t", 0) >= _MENU_STALL_RETRY_INTERVAL_S
                )
            )
            if should_act:
                action = _try_watchdog_continue(bg3_pid, len(menu_watchdog_actions) + 1)
                action["t"] = round(elapsed, 1)
                action["stalled_for_s"] = round(stalled_for, 1)
                menu_watchdog_actions.append(action)
                _phase(
                    "menu_watchdog_action",
                    f"#{len(menu_watchdog_actions)} {action.get('method', 'unknown')}",
                )
                if action.get("menu_detected") and not menu_detected:
                    menu_detected = True
                    _phase("menu_detected", f"watchdog={action.get('buttons', [])}")

            if (
                dismiss_splash
                and socket_ever_connected
                and len(menu_watchdog_actions) >= _MENU_STALL_MAX_ACTIONS
                and stalled_for >= _MENU_STALL_ABORT_S
            ):
                elapsed_ms = int((time.monotonic() - start) * 1000)
                _phase("menu_stalled", f"{elapsed_ms}ms")
                return {
                    "socket_connected": False,
                    "stage": "menu_stalled",
                    "elapsed_ms": elapsed_ms,
                    "dismiss_attempts": dismiss_count,
                    "phases": phases,
                    "watchdog_actions": menu_watchdog_actions,
                    "diagnostics": _collect_boot_diagnostics(),
                }

        # State machine: menu_detected → continue_clicked → socket_waiting
        # Splash dismiss (Esc+Space) is handled in-process by focusless_input.m
        # via BG3SE_AUTO_DISMISS_SPLASH=1. The Python side only does OCR to
        # detect the main menu, then sends Enter to click Continue.
        if dismiss_splash and not menu_detected:
            if elapsed >= dismiss_delay and (elapsed - last_ocr) >= ocr_interval:
                last_ocr = elapsed
                found, buttons = _detect_main_menu()
                if found:
                    menu_detected = True
                    _phase("menu_detected", f"buttons={buttons}")

        if menu_detected and not continue_sent:
            # In-process CGEventPostToPid/NSApp sendEvent can't reach
            # BG3's custom Metal UI. System Events key code via
            # Accessibility is the only method that works. Safe now
            # that the C-side HID tap leak is removed.
            if bg3_pid:
                _try_click_continue(bg3_pid)
            continue_sent = True
            _phase("continue_clicked")

        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.settimeout(2)
            sock.connect(SOCKET_PATH)

            if not socket_ever_connected:
                _phase("socket_listening")
                socket_listening_at = time.monotonic() - start
            socket_ever_connected = True

            time.sleep(0.3)
            try:
                sock.recv(4096)
            except socket.timeout:
                pass

            sock.sendall(b"Ext.GetVersion()\n")
            time.sleep(0.5)

            response = b""
            try:
                while True:
                    chunk = sock.recv(4096)
                    if not chunk:
                        break
                    response += chunk
            except socket.timeout:
                pass

            sock.close()

            if response:
                text = re.sub(rb'\033\[[0-9;]*m', b'', response).decode(
                    "utf-8", errors="replace").strip()
                elapsed_ms = int((time.monotonic() - start) * 1000)
                _phase("socket_responded", f"{elapsed_ms}ms")
                return {
                    "socket_connected": True,
                    "se_version": text if text else "connected",
                    "elapsed_ms": elapsed_ms,
                    "dismiss_attempts": dismiss_count,
                    "phases": phases,
                }

        except (ConnectionRefusedError, FileNotFoundError, OSError):
            pass

        time.sleep(interval)

    elapsed_ms = int((time.monotonic() - start) * 1000)
    _phase("timeout", f"{elapsed_ms}ms")
    return {
        "socket_connected": False,
        "stage": "timeout",
        "elapsed_ms": elapsed_ms,
        "dismiss_attempts": dismiss_count,
        "phases": phases,
        "watchdog_actions": menu_watchdog_actions,
        "diagnostics": _collect_boot_diagnostics(),
    }


def move_window_offscreen():
    """Move the BG3 window off-screen so it renders without being visible.

    Unlike ``visible=false`` or AXMinimized, moving off-screen does NOT
    stall Metal — the GPU still renders to the framebuffer. This makes
    the boot sequence invisible to the user while Metal initializes.
    After socket connects, ``hide_window()`` fully hides it in the Dock.
    """
    script = r'''
tell application "System Events"
  if exists process "Baldur's Gate 3" then
    tell process "Baldur's Gate 3"
      repeat with w in windows
        set position of w to {-10000, -10000}
      end repeat
    end tell
  end if
end tell
'''
    try:
        r = subprocess.run(
            ["osascript", "-e", script],
            capture_output=True, text=True, timeout=5,
        )
        if r.returncode == 0:
            # Activate terminal so user stays in their workflow
            for app in ("Ghostty", "Terminal", "iTerm2"):
                ra = subprocess.run(
                    ["osascript", "-e", f'tell application "{app}" to activate'],
                    capture_output=True, text=True, timeout=3,
                )
                if ra.returncode == 0:
                    break
            print("BG3 window moved off-screen (headless boot)", file=sys.stderr)
            return {"success": True, "method": "position_offscreen"}
        return {"success": False, "error": r.stderr.strip()}
    except (subprocess.TimeoutExpired, OSError) as exc:
        return {"success": False, "error": str(exc)}


def hide_window():
    """Hide BG3 window and bring terminal to front.

    Uses only ``visible=false`` (hide in Dock). AXMinimized is avoided
    because minimizing during boot prevents Metal from creating a
    drawable, stalling the game indefinitely.
    """
    try:
        r = subprocess.run(
            ["osascript", "-e",
             'tell application "System Events" to set visible of process "Baldur\'s Gate 3" to false'],
            capture_output=True, text=True, timeout=5,
        )
        if r.returncode != 0:
            return {"success": False, "error": r.stderr.strip(), "returncode": r.returncode}
        # Activate terminal to escape BG3's windowed app
        for app in ("Ghostty", "Terminal", "iTerm2"):
            ra = subprocess.run(
                ["osascript", "-e", f'tell application "{app}" to activate'],
                capture_output=True, text=True, timeout=3,
            )
            if ra.returncode == 0:
                break
        print("BG3 window hidden (headless mode)", file=sys.stderr)
        return {"success": True, "method": "system_events_visible_false"}
    except (subprocess.TimeoutExpired, OSError) as exc:
        return {"success": False, "error": str(exc)}


def show_window():
    """Bring BG3 window back to foreground."""
    graphics_restore = restore_headless_graphics(reason="show_window")
    script = r'''
tell application "System Events"
  if exists process "Baldur's Gate 3" then
    set visible of process "Baldur's Gate 3" to true
  else
    error "Baldur's Gate 3 process not found"
  end if
end tell
tell application "Baldur's Gate 3" to activate
'''
    try:
        r = subprocess.run(
            ["osascript", "-e", script],
            capture_output=True, text=True, timeout=5,
        )
        if r.returncode == 0:
            return {
                "success": True,
                "method": "system_events_visible_true_activate",
                "graphics_restore": graphics_restore,
            }
        return {
            "success": False,
            "method": "system_events_visible_true_activate",
            "graphics_restore": graphics_restore,
            "error": r.stderr.strip(),
            "returncode": r.returncode,
        }
    except (subprocess.TimeoutExpired, OSError) as exc:
        return {
            "success": False,
            "method": "system_events_visible_true_activate",
            "graphics_restore": graphics_restore,
            "error": str(exc),
        }


def _tail_file(path, lines=80):
    try:
        text = Path(path).read_text(errors="replace")
    except OSError:
        return None
    return "\n".join(text.splitlines()[-lines:])


def _analyze_log_tail(text):
    """Return compact problem markers from a boot log tail."""
    if not text:
        return {"has_problem_markers": False, "problem_lines": []}

    problem_re = re.compile(
        r"\b(error|fail(?:ed|ure)?|fatal|crash|exception|assert|panic|"
        r"signal|timeout|refused|denied|missing|invalid)\b",
        re.IGNORECASE,
    )
    problem_lines = []
    for line in text.splitlines():
        if problem_re.search(line):
            problem_lines.append(line[-500:])

    return {
        "has_problem_markers": bool(problem_lines),
        "problem_lines": problem_lines[-20:],
    }


def _collect_boot_diagnostics():
    """Collect lightweight diagnostics when boot stalls."""
    diagnostics = {}
    latest = Path.home() / "Library/Application Support/BG3SE/logs/latest.log"
    if latest.exists():
        diagnostics["latest_log"] = str(latest.resolve())
        log_tail = _tail_file(latest, lines=80)
        diagnostics["latest_log_tail"] = log_tail
        diagnostics["latest_log_analysis"] = _analyze_log_tail(log_tail)

    try:
        from .menu import collect_geometry, detect_menu
        diagnostics["geometry"] = collect_geometry(capture=True)
        debug_dir = PROJECT_ROOT / ".screenshots"
        debug_dir.mkdir(parents=True, exist_ok=True)
        debug_image = debug_dir / f"boot-stall-{int(time.time())}.png"
        detection = detect_menu(debug_image=str(debug_image))
        diagnostics["menu_detect"] = {
            "buttons": [b.get("text") for b in detection.get("buttons", [])],
            "raw_ocr": detection.get("raw_ocr", []),
            "error": detection.get("error"),
            "geometry": detection.get("geometry"),
            "debug_image": detection.get("debug_image") or str(debug_image),
        }
    except Exception as exc:
        diagnostics["menu_error"] = str(exc)
    return diagnostics


def should_retry_boot(health):
    """Return True when a failed boot is likely transient/retryable."""
    if health.get("socket_connected"):
        return False
    return health.get("stage") in BOOT_RETRYABLE_STAGES


def _try_watchdog_continue(pid, attempt):
    """Retry menu progress with a measured foreground mouse click."""
    action = {
        "attempt": attempt,
        "method": "foreground_physical_left_click",
        "success": False,
    }
    try:
        from .menu import click_fraction, detect_menu

        detection = detect_menu()
        buttons = [b.get("text") for b in detection.get("buttons", [])]
        action["buttons"] = buttons
        action["menu_detected"] = bool({"Continue", "New Game", "Load Game", "Multiplayer"} & set(buttons))
        action["ocr_error"] = detection.get("error")
        purpose, fraction = _watchdog_target_for_attempt(attempt)
        if action["menu_detected"] and purpose == "dismiss_splash":
            purpose = "click_continue"
            fraction = _CONTINUE_CLICK_FRACTIONS[0]
        action["purpose"] = purpose

        action["target_fraction"] = fraction

        if isinstance(fraction, list):
            clicks = []
            geometry = detection.get("geometry")
            for item in fraction:
                click_result = click_fraction(
                    item["x"],
                    item["y"],
                    method="both",
                    activate=True,
                )
                click_result["target_fraction"] = item
                clicks.append(click_result)
                geometry = click_result.get("geometry") or geometry
                time.sleep(0.25)
            action["clicks"] = clicks
            action["geometry"] = geometry
            action["success"] = any(bool(c.get("success")) for c in clicks)
        else:
            click_result = click_fraction(
                fraction["x"],
                fraction["y"],
                method="both",
                activate=True,
            )
            action["click"] = click_result
            action["geometry"] = click_result.get("geometry") or detection.get("geometry")
            action["success"] = bool(click_result.get("success"))
    except Exception as exc:
        action["error"] = str(exc)
    return action


_kVK_Escape = 53
_kVK_Space = 49
_kVK_Return = 36


def _try_dismiss_splash(attempt, pid=None):
    """Send Escape+Space to dismiss splash via System Events key code.

    Routes through the Accessibility framework so keys never touch the
    HID event stream and never leak to the terminal. Falls back to the
    aggressive method only if no PID is available.
    """
    try:
        if pid:
            from .menu import cg_key_to_pid
            cg_key_to_pid(pid, _kVK_Escape)
            time.sleep(0.2)
            cg_key_to_pid(pid, _kVK_Space)
            print(f"Splash dismiss #{attempt} (SystemEvents)", file=sys.stderr)
        else:
            from .menu import dismiss_splash_aggressive
            result = dismiss_splash_aggressive()
            if result.get("success"):
                print(f"Splash dismiss #{attempt} ({result.get('method', 'aggressive')})",
                      file=sys.stderr)
    except Exception:
        pass


def _try_click_continue(pid):
    """Send Enter to click the highlighted Continue button."""
    try:
        from .menu import cg_key_to_pid
        cg_key_to_pid(pid, _kVK_Return)
        print("Sent Enter to click Continue", file=sys.stderr)
    except Exception:
        pass


def _detect_main_menu():
    """Check if the BG3 main menu is visible via Vision OCR."""
    try:
        from .menu import detect_menu
        result = detect_menu()
        buttons = [b["text"] for b in result.get("buttons", [])]
        menu_labels = {"Continue", "New Game", "Load Game", "Multiplayer"}
        if menu_labels & set(buttons):
            return True, buttons
        return False, buttons
    except Exception:
        return False, []


def is_running():
    result = subprocess.run(
        ["pgrep", "-f", "Baldur's Gate 3"],
        capture_output=True, text=True,
    )
    return result.returncode == 0


def quit_game(force=False):
    """Quit BG3. Tries graceful AppleScript first, falls back to SIGTERM."""
    if not is_running():
        _clear_pid_file()
        return {"success": True, "method": "not_running"}

    if not force:
        result = subprocess.run(
            ["osascript", "-e", 'quit app "Baldur\'s Gate 3"'],
            capture_output=True, text=True,
        )
        for _ in range(10):
            time.sleep(1)
            if not is_running():
                _clear_pid_file()
                return {"success": True, "method": "graceful"}

    kill_existing(force_all=True)
    if not is_running():
        _clear_pid_file()
        return {"success": True, "method": "force"}
    return {"success": False, "method": "failed"}


def socket_alive():
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(2)
        sock.connect(SOCKET_PATH)
        sock.close()
        return True
    except (ConnectionRefusedError, FileNotFoundError, OSError):
        return False


def read_health_file():
    """Read the background monitor health file. Returns dict or None."""
    try:
        return _json.loads(HEALTH_FILE.read_text())
    except (FileNotFoundError, _json.JSONDecodeError):
        return None
