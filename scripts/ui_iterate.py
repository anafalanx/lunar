"""UI iteration harness for Lunar.

Workflow:
    python scripts/ui_iterate.py [--no-build] [--name NAME] [--wait MS]

Steps:
    1. (Optional) Build + deploy via build.py.
    2. Kill any running Lunar.exe.
    3. Launch build/Lunar.exe.
    4. Wait for the window to appear, stabilise for `--wait` ms.
    5. Screenshot the full window (including title bar + status bar) to
       build/ui/NAME.png. If NAME is omitted, use a timestamp.
    6. Close the app.

Uses only .venv packages (Pillow) + ctypes Win32 bindings -- no pywin32.
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import pathlib
import subprocess
import sys
import time


ROOT = pathlib.Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "Lunar.exe"
OUT_DIR = ROOT / "build" / "ui"

user32   = ctypes.WinDLL("user32",   use_last_error=True)
gdi32    = ctypes.WinDLL("gdi32",    use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

SRCCOPY    = 0x00CC0020
SW_RESTORE = 9

# --- win32 prototypes -------------------------------------------------------
user32.FindWindowW.argtypes           = [wt.LPCWSTR, wt.LPCWSTR]
user32.FindWindowW.restype            = wt.HWND
user32.IsWindowVisible.argtypes       = [wt.HWND]
user32.IsWindowVisible.restype        = wt.BOOL
user32.GetWindowRect.argtypes         = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.GetWindowRect.restype          = wt.BOOL
user32.GetClientRect.argtypes         = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.GetClientRect.restype          = wt.BOOL
user32.GetWindowDC.argtypes           = [wt.HWND]
user32.GetWindowDC.restype            = wt.HDC
user32.ReleaseDC.argtypes             = [wt.HWND, wt.HDC]
user32.ReleaseDC.restype              = ctypes.c_int
user32.ShowWindow.argtypes            = [wt.HWND, ctypes.c_int]
user32.ShowWindow.restype             = wt.BOOL
user32.SetForegroundWindow.argtypes   = [wt.HWND]
user32.SetForegroundWindow.restype    = wt.BOOL
user32.PostMessageW.argtypes          = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.PostMessageW.restype           = wt.BOOL
user32.SetProcessDPIAware.argtypes    = []
user32.SetProcessDPIAware.restype     = wt.BOOL

gdi32.CreateCompatibleDC.argtypes     = [wt.HDC]
gdi32.CreateCompatibleDC.restype      = wt.HDC
gdi32.CreateCompatibleBitmap.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int]
gdi32.CreateCompatibleBitmap.restype  = wt.HBITMAP
gdi32.SelectObject.argtypes           = [wt.HDC, wt.HGDIOBJ]
gdi32.SelectObject.restype            = wt.HGDIOBJ
gdi32.BitBlt.argtypes                 = [wt.HDC, ctypes.c_int, ctypes.c_int,
                                         ctypes.c_int, ctypes.c_int,
                                         wt.HDC, ctypes.c_int, ctypes.c_int,
                                         wt.DWORD]
gdi32.BitBlt.restype                  = wt.BOOL
gdi32.DeleteObject.argtypes           = [wt.HGDIOBJ]
gdi32.DeleteObject.restype            = wt.BOOL
gdi32.DeleteDC.argtypes               = [wt.HDC]
gdi32.DeleteDC.restype                = wt.BOOL

WM_CLOSE = 0x0010


def log(msg: str) -> None:
    print(f"[ui] {msg}", flush=True)


def build() -> None:
    log("Building")
    r = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "build.py")],
        cwd=ROOT,
    )
    if r.returncode != 0:
        raise SystemExit(f"build failed (exit {r.returncode})")


def kill_running() -> None:
    subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "Get-Process Lunar -ErrorAction SilentlyContinue | Stop-Process -Force"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )


def find_window(title: str = "Lunar 0.2", timeout_s: float = 5.0):
    # Match as substring to tolerate the title-bar timezone prefix
    # (e.g. "RDT (UTC+2)  --  Lunar 0.2").
    EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
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
            if title in buf.value:
                found.append(h)
                return False
            return True
        user32.EnumWindows(EnumWindowsProc(cb), 0)
        if found:
            return found[0]
        time.sleep(0.1)
    return None


def screenshot(hwnd, out_path: pathlib.Path) -> tuple[int, int]:
    from PIL import Image

    # Full window bounds (includes title bar).
    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    if w <= 0 or h <= 0:
        raise RuntimeError(f"bad window size {w}x{h}")

    hdc_win = user32.GetWindowDC(hwnd)
    hdc_mem = gdi32.CreateCompatibleDC(hdc_win)
    hbmp    = gdi32.CreateCompatibleBitmap(hdc_win, w, h)
    gdi32.SelectObject(hdc_mem, hbmp)

    if not gdi32.BitBlt(hdc_mem, 0, 0, w, h, hdc_win, 0, 0, SRCCOPY):
        raise RuntimeError(f"BitBlt failed, err={ctypes.get_last_error()}")

    # Pull bits via GetDIBits into a BGRA buffer.
    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [
            ("biSize",          wt.DWORD),
            ("biWidth",         wt.LONG),
            ("biHeight",        wt.LONG),
            ("biPlanes",        wt.WORD),
            ("biBitCount",      wt.WORD),
            ("biCompression",   wt.DWORD),
            ("biSizeImage",     wt.DWORD),
            ("biXPelsPerMeter", wt.LONG),
            ("biYPelsPerMeter", wt.LONG),
            ("biClrUsed",       wt.DWORD),
            ("biClrImportant",  wt.DWORD),
        ]

    class BITMAPINFO(ctypes.Structure):
        _fields_ = [("bmiHeader", BITMAPINFOHEADER),
                    ("bmiColors", wt.DWORD * 3)]

    bi = BITMAPINFO()
    bi.bmiHeader.biSize        = ctypes.sizeof(BITMAPINFOHEADER)
    bi.bmiHeader.biWidth       = w
    bi.bmiHeader.biHeight      = -h   # top-down
    bi.bmiHeader.biPlanes      = 1
    bi.bmiHeader.biBitCount    = 32
    bi.bmiHeader.biCompression = 0

    buf = (ctypes.c_ubyte * (w * h * 4))()
    gdi32.GetDIBits = gdi32.GetDIBits  # ensure attr exists
    gdi32.GetDIBits.argtypes = [
        wt.HDC, wt.HBITMAP, wt.UINT, wt.UINT,
        ctypes.c_void_p, ctypes.POINTER(BITMAPINFO), wt.UINT,
    ]
    gdi32.GetDIBits.restype = ctypes.c_int
    if gdi32.GetDIBits(hdc_mem, hbmp, 0, h, buf, ctypes.byref(bi), 0) == 0:
        raise RuntimeError("GetDIBits failed")

    img = Image.frombuffer("RGBA", (w, h), bytes(buf), "raw", "BGRA", 0, 1)
    img = img.convert("RGB")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    img.save(out_path, "PNG", optimize=True)

    # Cleanup.
    gdi32.DeleteObject(hbmp)
    gdi32.DeleteDC(hdc_mem)
    user32.ReleaseDC(hwnd, hdc_win)
    return w, h


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-build", action="store_true",
                    help="Skip build step (use existing build/Lunar.exe).")
    ap.add_argument("--name", default=None,
                    help="Output PNG name (without extension). "
                         "Default: a timestamp.")
    ap.add_argument("--wait", type=int, default=600,
                    help="ms to wait after window appears before snapping.")
    ap.add_argument("--keep", action="store_true",
                    help="Leave Lunar.exe running after screenshot.")
    ap.add_argument("--click", type=int, action="append", default=[],
                    help="Send BN_CLICKED WM_COMMAND to child control with "
                         "this dialog ID before screenshotting. Repeatable.")
    args = ap.parse_args()

    user32.SetProcessDPIAware()

    if not args.no_build:
        build()

    if not EXE.exists():
        raise SystemExit(f"missing {EXE}")

    kill_running()
    time.sleep(0.2)

    log(f"Launching {EXE}")
    proc = subprocess.Popen([str(EXE)], cwd=EXE.parent)

    hwnd = find_window()
    if not hwnd:
        proc.kill()
        raise SystemExit("Lunar window did not appear")

    user32.ShowWindow(hwnd, SW_RESTORE)
    user32.SetForegroundWindow(hwnd)
    time.sleep(args.wait / 1000.0)

    if args.click:
        user32.GetDlgItem = user32.GetDlgItem
        user32.GetDlgItem.argtypes = [wt.HWND, ctypes.c_int]
        user32.GetDlgItem.restype  = wt.HWND
        user32.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
        user32.SendMessageW.restype  = ctypes.c_long
        WM_COMMAND = 0x0111
        BN_CLICKED = 0
        for cid in args.click:
            child = user32.GetDlgItem(hwnd, cid)
            log(f"Clicking control id={cid} (hwnd={child})")
            user32.SendMessageW(hwnd, WM_COMMAND,
                                (BN_CLICKED << 16) | cid, child)
        time.sleep(0.3)

    name = args.name or time.strftime("%Y%m%d-%H%M%S")
    out = OUT_DIR / f"{name}.png"
    w, h = screenshot(hwnd, out)
    log(f"Saved {out}  ({w}x{h})")

    if not args.keep:
        user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()

    print(str(out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
