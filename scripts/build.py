"""Build a single-file Lunar.exe and copy it to the desktop.

Usage (from project root):

    python scripts/build.py
    python scripts/build.py --no-desktop
    python scripts/build.py --dest <path>

Requires MSYS2 UCRT64 with `gcc` and `windres`. Renders with Direct2D +
DirectWrite on a plain Win32 HWND; the resulting exe has no third-party
runtime dependencies.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


# ---- paths -----------------------------------------------------------------

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
BUILD = ROOT / "build"
UCRT_BIN = Path(r"C:\msys64\ucrt64\bin")


def log(msg: str) -> None:
    print(f"==> {msg}", flush=True)


def die(msg: str) -> "None":
    print(f"build.py: {msg}", file=sys.stderr)
    sys.exit(1)


def run(*args: str | os.PathLike) -> None:
    """Run a command, streaming output; die on non-zero exit."""
    cmd = [str(a) for a in args]
    log(" ".join(cmd))
    rc = subprocess.call(cmd)
    if rc != 0:
        die(f"command failed (exit {rc}): {cmd[0]}")


def find_tool(name: str) -> Path:
    """Locate a tool on PATH or in the MSYS2 UCRT64 bin dir."""
    found = shutil.which(name)
    if found:
        return Path(found)
    for ext in (".exe", ""):
        candidate = UCRT_BIN / f"{name}{ext}"
        if candidate.is_file():
            # Prepend UCRT bin so child processes can find it too.
            os.environ["PATH"] = f"{UCRT_BIN};{os.environ.get('PATH', '')}"
            return candidate
    die(f"{name} not found on PATH or in {UCRT_BIN}")
    raise SystemExit  # unreachable; keeps type checkers happy


def desktop_dir() -> Path | None:
    profile = os.environ.get("USERPROFILE")
    if not profile:
        return None
    for rel in ("OneDrive/Desktop", "Desktop"):
        d = Path(profile, *rel.split("/"))
        if d.is_dir():
            return d
    return None


# ---- main ------------------------------------------------------------------


def main() -> None:
    ap = argparse.ArgumentParser(description="Build Lunar.exe.")
    ap.add_argument(
        "--no-desktop", action="store_true",
        help="Do not copy the exe to the desktop.",
    )
    ap.add_argument(
        "--dest", type=Path, default=None,
        help="Copy the exe to this directory instead of the desktop.",
    )
    args = ap.parse_args()

    BUILD.mkdir(exist_ok=True)

    exe = BUILD / "Lunar.exe"
    res = BUILD / "lunar.res"

    gcc = find_tool("gcc")
    windres = find_tool("windres")

    log("Compiling resource")
    run(windres, SRC / "lunar.rc", "-O", "coff", "-o", res)

    log("Compiling + linking Lunar.exe")
    run(
        gcc,
        "-O2", "-Wall", "-Wextra", "-std=c11",
        "-mwindows", "-static-libgcc", "-static",
        "-o", str(exe),
        str(SRC / "lunar.c"),
        str(SRC / "sysvol.c"),
        str(SRC / "ntp.c"),
        str(SRC / "clock.c"),
        str(res),
        # Direct2D + DirectWrite for rendering; winmm for PlaySound;
        # ole32 for COM plumbing that sysvol.c and D2D rely on;
        # ws2_32 for SNTP; uxtheme for native title-bar theming.
        "-ld2d1", "-ldwrite", "-lwinmm",
        "-luser32", "-lkernel32", "-lgdi32", "-lcomctl32", "-lshell32",
        "-luxtheme", "-lole32", "-lws2_32", "-ldwmapi",
    )
    log(f"{exe}  ({exe.stat().st_size / 1048576:.2f} MB)")

    # ---- deploy ------------------------------------------------------------
    if args.no_desktop:
        return
    target = args.dest if args.dest else desktop_dir()
    if target is None or not target.is_dir():
        die("desktop directory not found; try --dest <path>")
    final = target / exe.name
    shutil.copy2(exe, final)
    log(f"Copied to {final}")


if __name__ == "__main__":
    main()
