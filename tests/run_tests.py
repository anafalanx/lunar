"""Build and run the Lunar C unit tests.

Usage (from project root):

    .venv\\Scripts\\python.exe tests/run_tests.py

Compiles tests/test_core.c against src/*.c in a separate object set
(without -mwindows so we have a console main) and runs the resulting
binary. Returns non-zero on any check failure.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
TESTS = ROOT / "tests"
BUILD = ROOT / "build" / "tests"
UCRT_BIN = Path(r"C:\msys64\ucrt64\bin")


def find(name: str) -> Path:
    p = shutil.which(name)
    if p:
        return Path(p)
    for ext in (".exe", ""):
        c = UCRT_BIN / f"{name}{ext}"
        if c.is_file():
            os.environ["PATH"] = f"{UCRT_BIN};{os.environ.get('PATH', '')}"
            return c
    raise SystemExit(f"tool not found: {name}")


def main() -> int:
    BUILD.mkdir(parents=True, exist_ok=True)
    gcc = find("gcc")

    exe = BUILD / "test_core.exe"
    src = [
        TESTS / "test_core.c",   # #includes ../src/lunar.c with LUNAR_NO_MAIN
        SRC  / "sysvol.c",
        SRC  / "ntp.c",
        SRC  / "clock.c",
    ]
    # No -mwindows: we want a console main(). -Werror to catch new warnings.
    cmd = [
        str(gcc),
        "-O0", "-g",
        "-Wall", "-Wextra", "-Wno-unused-function", "-Werror",
        "-std=c11",
        "-static-libgcc", "-static",
        "-o", str(exe),
        *[str(p) for p in src],
        "-ld2d1", "-ldwrite", "-lwinmm",
        "-luser32", "-lkernel32", "-lgdi32", "-lcomctl32", "-lshell32",
        "-luxtheme", "-lole32", "-lws2_32", "-ldwmapi",
    ]
    print("==> Compiling tests")
    print("   ", " ".join(cmd))
    r = subprocess.run(cmd)
    if r.returncode != 0:
        print(f"compile failed (exit {r.returncode})", file=sys.stderr)
        return 2

    print(f"==> Running {exe}")
    r = subprocess.run([str(exe)], cwd=ROOT)
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
