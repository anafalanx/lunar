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
import hashlib
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


# ---- paths -----------------------------------------------------------------

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
BUILD = ROOT / "build"
UCRT_BIN = Path(r"C:\msys64\ucrt64\bin")

# Vendored mbedTLS 3.6.6 LTS. Produced from an upstream tarball whose
# SHA-256 we checked against the release page before committing the
# source tree (see third_party/lunar_mbedtls_config.h for rationale
# and /memories/session/mbedtls_vendor.md for the audit trail).
MBEDTLS_DIR = ROOT / "third_party" / "mbedtls-3.6.6"
MBEDTLS_CONFIG = ROOT / "third_party" / "lunar_mbedtls_config.h"


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


# ---- mbedTLS vendored build -------------------------------------------------
#
# We compile every .c in third_party/mbedtls-3.6.6/library/ into a static
# archive (libmbedtls_lunar.a) and link it into Lunar. The archive is
# hash-cached: if the config, headers, and sources haven't changed, we
# skip recompilation. First build takes ~30 s; subsequent builds are
# instant.
#
# Security note: our config (third_party/lunar_mbedtls_config.h) disables
# every mbedTLS module Lunar does not need -- the disabled .c files still
# compile, but they compile to nothing because their module-level guards
# are undefined. This is the pattern recommended by the mbedTLS team.

def _mbedtls_cache_key() -> str:
    """Hash every file that affects the mbedTLS library build.

    If ANY of these change -- config, headers, or sources -- the archive
    is invalidated and rebuilt. Cheap to compute (a few MB of SHA-256).
    """
    h = hashlib.sha256()
    inputs = sorted([
        MBEDTLS_CONFIG,
        *(MBEDTLS_DIR / "include").rglob("*.h"),
        *(MBEDTLS_DIR / "library").glob("*.c"),
        *(MBEDTLS_DIR / "library").glob("*.h"),
    ])
    for f in inputs:
        h.update(f.as_posix().encode("utf-8"))
        h.update(b"\0")
        h.update(f.read_bytes())
        h.update(b"\0")
    return h.hexdigest()[:16]


def build_mbedtls_archive(gcc: Path) -> Path:
    """Return path to libmbedtls_lunar.a, (re)building if stale."""
    key = _mbedtls_cache_key()
    cache_dir = BUILD / "mbedtls" / key
    archive = cache_dir / "libmbedtls_lunar.a"
    stamp = cache_dir / ".ok"
    if stamp.is_file() and archive.is_file():
        log(f"mbedTLS archive cache hit ({key})  -> {archive}")
        return archive

    # Clean any other cache directories -- keep only the current one.
    cache_root = BUILD / "mbedtls"
    if cache_root.is_dir():
        for d in cache_root.iterdir():
            if d.is_dir() and d.name != key:
                shutil.rmtree(d, ignore_errors=True)
    cache_dir.mkdir(parents=True, exist_ok=True)
    obj_dir = cache_dir / "obj"
    obj_dir.mkdir(exist_ok=True)

    log(f"Building mbedTLS 3.6.6 archive ({key})")

    # The MBEDTLS_CONFIG_FILE macro is pasted verbatim into an #include
    # directive inside mbedTLS's build_info.h. We pass it with angle
    # brackets so the preprocessor searches the include path; the file
    # lives in third_party/ which we add with -I below.
    config_flag = "-DMBEDTLS_CONFIG_FILE=<lunar_mbedtls_config.h>"

    includes = [
        f"-I{MBEDTLS_DIR / 'include'}",
        f"-I{MBEDTLS_DIR / 'library'}",
        f"-I{ROOT / 'third_party'}",
    ]

    # -O2 for speed (the TLS handshake dominates each NTP cycle's budget
    # if we're lazy about crypto). -Wall -Wextra so we catch integration
    # warnings. -Wno-unused-function because many mbedTLS helpers compile
    # to unused when their module is disabled -- that's not a bug, it's
    # the whole point of the minimal config.
    common_flags = [
        "-O2", "-g",
        "-Wall", "-Wextra",
        "-Wno-unused-function",
        "-Wno-unused-parameter",
        "-Wno-unused-variable",
        "-Wno-unused-but-set-variable",
        "-std=c11",
        "-DWIN32_LEAN_AND_MEAN",
        "-D_WIN32_WINNT=0x0601",
        config_flag,
    ] + includes

    srcs = sorted((MBEDTLS_DIR / "library").glob("*.c"))

    # Parallel compile -- up to CPU count.
    def compile_one(src: Path) -> tuple[Path, int, str]:
        obj = obj_dir / (src.stem + ".o")
        cmd = [str(gcc), *common_flags, "-c", str(src), "-o", str(obj)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        return obj, proc.returncode, proc.stderr

    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        results = list(pool.map(compile_one, srcs))

    failed = [(s, rc, err) for (s, rc, err) in zip(srcs, [r[1] for r in results],
              [r[2] for r in results]) if rc != 0]
    if failed:
        for src, rc, err in failed:
            print(f"---- FAILED: {src.name} (rc {rc}) ----", file=sys.stderr)
            print(err, file=sys.stderr)
        die(f"{len(failed)} mbedTLS file(s) failed to compile")

    # Any non-silent warnings worth flagging? Collect stderr with content.
    for src, (_, _, err) in zip(srcs, results):
        if err.strip():
            # Not fatal -- show once so the human sees them.
            print(f"[mbedtls warn] {src.name}:", file=sys.stderr)
            print(err, file=sys.stderr)

    # Pack into archive.
    ar = find_tool("ar")
    objs = [str(r[0]) for r in results]
    archive.unlink(missing_ok=True)
    # Use @response-file to dodge the Windows 32K cmdline limit -- 100+
    # object paths can overflow in extreme cases.
    rsp = cache_dir / "ar.rsp"
    rsp.write_text(" ".join(f'"{o}"' for o in objs), encoding="utf-8")
    run(ar, "rcs", archive, f"@{rsp}")
    rsp.unlink(missing_ok=True)
    stamp.write_text(key, encoding="utf-8")
    log(f"mbedTLS archive ready  -> {archive}  ({archive.stat().st_size / 1024:.0f} KB)")
    return archive


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

    # Build the vendored mbedTLS 3.6.6 archive first (cached on a hash
    # of config + sources; subsequent builds are instant).
    mbedtls_archive = build_mbedtls_archive(gcc)

    log("Compiling resource")
    run(windres, SRC / "lunar.rc", "-O", "coff", "-o", res)

    log("Compiling + linking Lunar.exe")
    run(
        gcc,
        "-O2", "-Wall", "-Wextra", "-std=c11",
        "-mwindows", "-static-libgcc", "-static",
        # -ffunction-sections + -fdata-sections + -Wl,--gc-sections
        # make the linker strip unused functions/data, so pulling in
        # mbedTLS's ~3 MB static archive only costs us the parts we
        # actually reference. TLS 1.3 client + X.509 verify + AES-GCM
        # + ChaCha20-Poly1305 + SHA-256 should land around ~400 KB.
        "-ffunction-sections", "-fdata-sections",
        # mbedTLS include paths so our src/*.c can #include <mbedtls/...>.
        # We do NOT set WIN32_LEAN_AND_MEAN or _WIN32_WINNT globally --
        # lunar.c manages its own Win32 header defines and needs the
        # default mingw-w64 _WIN32_WINNT (Win10) for Dynamic TZ APIs.
        f"-I{MBEDTLS_DIR / 'include'}",
        f"-I{ROOT / 'third_party'}",
        "-DMBEDTLS_CONFIG_FILE=<lunar_mbedtls_config.h>",
        "-o", str(exe),
        str(SRC / "lunar.c"),
        str(SRC / "sysvol.c"),
        str(SRC / "ntp.c"),
        str(SRC / "clock.c"),
        str(SRC / "siv.c"),
        str(SRC / "nts_ke.c"),
        str(SRC / "nts_ef.c"),
        str(res),
        # Static archive goes AFTER the objects so the linker sees the
        # undefined symbols first; standard GCC link-order rule.
        str(mbedtls_archive),
        "-Wl,--gc-sections",
        # Direct2D + DirectWrite for rendering; winmm for PlaySound;
        # ole32 for COM plumbing that sysvol.c and D2D rely on;
        # ws2_32 for SNTP; uxtheme for native title-bar theming;
        # advapi32 for mbedTLS's entropy source (CryptGenRandom).
        "-ld2d1", "-ldwrite", "-lwinmm",
        "-luser32", "-lkernel32", "-lgdi32", "-lcomctl32", "-lshell32",
        "-luxtheme", "-lole32", "-lws2_32", "-ldwmapi", "-ladvapi32",
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
