"""Capture a screenshot of the Lunar settings dialog.

Launches Lunar, opens the Settings dialog via WM_SYSCOMMAND, waits for
it to appear, screenshots it, then closes everything.  Output lands in
`build/ui/settings.png`.
"""
from __future__ import annotations
import ctypes
import ctypes.wintypes as wt
import pathlib
import subprocess
import sys
import time

import ui_iterate as U   # reuses screenshot() + find_window() + kill_running()

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXE  = ROOT / "build" / "Lunar.exe"
OUT  = ROOT / "build" / "ui" / "settings.png"

WM_SYSCOMMAND = 0x0112
IDM_SETTINGS  = 0x1003   # must match src/lunar.c

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.FindWindowExW.argtypes = [wt.HWND, wt.HWND, wt.LPCWSTR, wt.LPCWSTR]
user32.FindWindowExW.restype  = wt.HWND


def find_dialog(parent_hwnd, timeout_s: float = 3.0):
    """Poll for a popup whose owner is parent_hwnd and whose caption
    contains 'Lunar Settings'."""
    EnumProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        found = []

        def cb(h, _):
            if not user32.IsWindowVisible(h):
                return True
            n = user32.GetWindowTextLengthW(h)
            if n <= 0:
                return True
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(h, buf, n + 1)
            if "Lunar Settings" in buf.value:
                found.append(h)
                return False
            return True

        user32.EnumWindows(EnumProc(cb), 0)
        if found:
            return found[0]
        time.sleep(0.05)
    return None


def main() -> int:
    user32.SetProcessDPIAware()
    U.kill_running()
    time.sleep(0.2)
    proc = subprocess.Popen([str(EXE)], cwd=EXE.parent)

    try:
        hwnd = U.find_window()
        if not hwnd:
            raise SystemExit("Lunar main window did not appear")
        user32.ShowWindow(hwnd, 9)       # SW_RESTORE
        user32.SetForegroundWindow(hwnd)

        # Wait long enough for the first NTP cycle to land so the live
        # preview shows an actual time rather than an em-dash.
        time.sleep(8.0)

        # Open settings.
        user32.PostMessageW(hwnd, WM_SYSCOMMAND, IDM_SETTINGS, 0)
        dlg = find_dialog(hwnd)
        if not dlg:
            raise SystemExit("Settings dialog did not appear")
        time.sleep(1.2)   # let live preview render at least one frame

        w, h = U.screenshot(dlg, OUT)
        print(f"Saved {OUT}  ({w}x{h})")
    finally:
        try:
            user32.PostMessageW(proc.pid and proc.pid or 0, 0x0010, 0, 0)
        except Exception:
            pass
        proc.kill()

    return 0


if __name__ == "__main__":
    sys.exit(main())
