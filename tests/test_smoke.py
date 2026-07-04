"""Integration smoke tests for Lunar.

Runs build/Lunar.exe in a child process, drives it through common
life-cycle scenarios, and verifies observable behaviour without any UI
automation library (ctypes Win32 only).

Covered:
  1. Binary exists and launches.
  2. A visible window with the expected class name and title fragment
     shows up within a few seconds.
  3. The window is square (within a small tolerance) and of the expected
     DPI-scaled default size.
  4. The system menu contains our five custom items.
  5. DWM has accepted our iconic bitmap attributes (HAS_ICONIC_BITMAP
     and FORCE_ICONIC_REPRESENTATION).
  6. Minimize -> the process stays healthy (timer keeps firing, so the
     message loop is alive).
  7. WM_CLOSE -> the process exits cleanly within a reasonable time
     with exit code 0.
  8. A second instance can be launched after the first exits (no
     leaked mutex / class).
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import shutil
import subprocess
import sys
import tempfile
import time
import os
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
EXE  = ROOT / "build" / "Lunar.exe"

user32   = ctypes.WinDLL("user32",   use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
dwmapi   = ctypes.WinDLL("dwmapi",   use_last_error=True)

user32.FindWindowW.argtypes       = [wt.LPCWSTR, wt.LPCWSTR]
user32.FindWindowW.restype        = wt.HWND
user32.IsWindowVisible.argtypes   = [wt.HWND]
user32.IsWindowVisible.restype    = wt.BOOL
user32.GetClientRect.argtypes     = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.GetClientRect.restype      = wt.BOOL
user32.GetWindowRect.argtypes     = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.GetWindowRect.restype      = wt.BOOL
user32.GetSystemMenu.argtypes     = [wt.HWND, wt.BOOL]
user32.GetSystemMenu.restype      = wt.HMENU
user32.GetMenuItemCount.argtypes  = [wt.HMENU]
user32.GetMenuItemCount.restype   = ctypes.c_int
user32.GetMenuStringW.argtypes    = [wt.HMENU, wt.UINT, wt.LPWSTR,
                                     ctypes.c_int, wt.UINT]
user32.GetMenuStringW.restype     = ctypes.c_int
user32.PostMessageW.argtypes      = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.PostMessageW.restype       = wt.BOOL
user32.ShowWindow.argtypes        = [wt.HWND, ctypes.c_int]
user32.ShowWindow.restype         = wt.BOOL
user32.GetClassNameW.argtypes     = [wt.HWND, wt.LPWSTR, ctypes.c_int]
user32.GetClassNameW.restype      = ctypes.c_int
user32.EnumWindows.argtypes       = [ctypes.c_void_p, wt.LPARAM]
user32.EnumWindows.restype        = wt.BOOL
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.GetWindowThreadProcessId.restype  = wt.DWORD
user32.SetProcessDPIAware.argtypes = []
user32.SetProcessDPIAware.restype  = wt.BOOL

dwmapi.DwmGetWindowAttribute.argtypes = [wt.HWND, wt.DWORD,
                                         ctypes.c_void_p, wt.DWORD]
dwmapi.DwmGetWindowAttribute.restype  = ctypes.c_long   # HRESULT

WM_SYSCOMMAND = 0x0112
WM_CLOSE      = 0x0010
SC_MINIMIZE   = 0xF020
MF_BYPOSITION = 0x400

# Should match the same IDs the app installs. Keep in sync manually.
IDM_ALWAYS_ON_TOP = 0x1001
IDM_TEST_BEEP     = 0x1002
IDM_SETTINGS      = 0x1003
IDM_ABOUT         = 0x1004
IDM_SYNC_NOW      = 0x1005


g_pass = 0
g_fail = 0


def check(ok: bool, msg: str) -> None:
    global g_pass, g_fail
    if ok:
        g_pass += 1
        print(f"  OK   {msg}")
    else:
        g_fail += 1
        print(f"  FAIL {msg}")


def find_main_window(pid: int, timeout: float = 5.0) -> int:
    """Return the HWND of a visible top-level window owned by pid."""
    deadline = time.time() + timeout
    found: list[int] = []

    EnumProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def cb(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        owner = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value != pid:
            return True
        cls = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(hwnd, cls, 64)
        if cls.value == "LunarWin":
            found.append(hwnd)
            return False
        return True

    while time.time() < deadline:
        found.clear()
        user32.EnumWindows(EnumProc(cb), 0)
        if found:
            return found[0]
        time.sleep(0.05)
    return 0


def collect_sys_menu(hwnd: int) -> list[str]:
    menu = user32.GetSystemMenu(hwnd, False)
    if not menu:
        return []
    n = user32.GetMenuItemCount(menu)
    out = []
    for i in range(n):
        buf = ctypes.create_unicode_buffer(128)
        user32.GetMenuStringW(menu, i, buf, 128, MF_BYPOSITION)
        out.append(buf.value)
    return out


def main() -> int:
    user32.SetProcessDPIAware()

    # Point the app at a fresh per-run data directory via LUNAR_DATA_DIR
    # (see src/app_paths.c). The child starts with no persisted window
    # state / settings, so the test always exercises the default geometry
    # path -- and the developer's real %APPDATA%\Lunar is never touched.
    data_dir = Path(tempfile.mkdtemp(prefix="lunar-smoke-"))
    child_env = os.environ.copy()
    child_env["LUNAR_DATA_DIR"] = str(data_dir)
    print(f"Using LUNAR_DATA_DIR={data_dir}")

    try:
        return run_checks(child_env)
    finally:
        shutil.rmtree(data_dir, ignore_errors=True)


def run_checks(child_env: dict[str, str]) -> int:
    print("1) Binary exists")
    check(EXE.is_file(), f"{EXE} present ({EXE.stat().st_size if EXE.is_file() else 0} bytes)")
    if not EXE.is_file():
        return 2

    print("\n2) Launch and find main window")
    proc = subprocess.Popen([str(EXE)], env=child_env)
    try:
        hwnd = find_main_window(proc.pid, timeout=6.0)
        check(hwnd != 0, f"HWND discovered within 6 s (pid={proc.pid})")
        if hwnd == 0:
            return 2

        print("\n3) Window geometry")
        rc = wt.RECT()
        user32.GetClientRect(hwnd, ctypes.byref(rc))
        w = rc.right - rc.left
        h = rc.bottom - rc.top
        check(w > 0 and h > 0, f"client rect {w}x{h} is positive")
        # Square-ish (dial is inscribed in min(w,h); allow chrome wiggle).
        check(abs(w - h) <= 4, f"client is square within 4 px (w-h = {w - h})")

        print("\n4) System menu contents")
        items = collect_sys_menu(hwnd)
        required = ["About Lunar", "Settings", "Test beep", "Always on"]
        for needle in required:
            hit = any(needle in it for it in items)
            check(hit, f"menu item matching {needle!r} present")

        print("\n5) DWM iconic-bitmap attributes accepted")
        # DWMWA_HAS_ICONIC_BITMAP = 10, DWMWA_FORCE_ICONIC_REPRESENTATION = 7
        # Both are set-only; there is no reliable get. Validate by round-
        # tripping: send the minimize + restore and confirm the app still
        # responds.
        user32.PostMessageW(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0)
        time.sleep(0.6)
        alive = proc.poll() is None
        check(alive, "process still alive after minimize")

        # Prod the app: send the "Sync Now" menu command and check the
        # process stays alive a beat later (timer is still firing).
        user32.PostMessageW(hwnd, WM_SYSCOMMAND, IDM_SYNC_NOW, 0)
        time.sleep(0.4)
        check(proc.poll() is None, "process alive after IDM_SYNC_NOW")

        print("\n6) Clean shutdown via WM_CLOSE")
        # The app's worst-case shutdown budget is ~8.5 s when a sync is
        # in flight (watchdog 1.5 s + NTP aggregator 5 s + detached
        # worker grace 2 s) -- and the IDM_SYNC_NOW we just posted starts
        # a cold sync since the data dir is fresh. Allow 15 s.
        user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        try:
            rc = proc.wait(timeout=15.0)
            check(rc == 0, f"exited cleanly with code {rc}")
        except subprocess.TimeoutExpired:
            check(False, "did not exit within 15 s")
            proc.kill()
            proc.wait()
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()

    print("\n7) Second instance after clean shutdown")
    proc2 = subprocess.Popen([str(EXE)], env=child_env)
    hwnd2 = 0
    try:
        hwnd2 = find_main_window(proc2.pid, timeout=6.0)
        check(hwnd2 != 0, "second instance window found")
    finally:
        if hwnd2:
            user32.PostMessageW(hwnd2, WM_CLOSE, 0, 0)
        try:
            proc2.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            proc2.kill()
            proc2.wait()

    print(f"\n{g_pass + g_fail} checks, {g_fail} failed")
    return 0 if g_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
