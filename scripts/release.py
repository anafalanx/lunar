"""Build a release distributable for Lunar.

Produces:
    dist/Lunar-<version>-win-x64/
        Lunar.exe
        README.txt
        LICENSE       (if present)
    dist/Lunar-<version>-win-x64.zip

The version is read from the VERSION file at the project root.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
DIST = ROOT / "dist"
EXE = BUILD / "Lunar.exe"
VERSION_FILE = ROOT / "VERSION"


def log(msg: str) -> None:
    print(f"==> {msg}", flush=True)


def read_version() -> str:
    v = VERSION_FILE.read_text(encoding="utf-8").strip()
    if not v:
        sys.exit("VERSION file is empty")
    return v


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def sign_exe(exe: Path) -> None:
    """Authenticode-sign exe if LUNAR_SIGN_CMD is set; warn loudly if not.

    LUNAR_SIGN_CMD is a full command line; the literal placeholder {exe}
    is replaced with the path of the exe to sign (the path is appended if
    the placeholder is absent). Example (Certum via SimplySign, what we
    use -- connect SimplySign Desktop first so /a finds the cloud cert):

        set LUNAR_SIGN_CMD=signtool sign /a /tr http://time.certum.pl /td sha256 /fd sha256 /v {exe}

    See docs/distribution.md for the full Certum Open Source Code Signing
    setup.
    """
    template = os.environ.get("LUNAR_SIGN_CMD", "").strip()
    if not template:
        print()
        print("*" * 68)
        print("*  WARNING: LUNAR_SIGN_CMD is not set.                             *")
        print("*  This release is UNSIGNED. Windows SmartScreen will warn users.  *")
        print("*  See docs/distribution.md for signing setup.                     *")
        print("*" * 68)
        print()
        return

    if "{exe}" in template:
        cmd = template.replace("{exe}", str(exe))
    else:
        cmd = f'{template} "{exe}"'

    log(f"Signing: {cmd}")
    rc = subprocess.call(cmd, shell=True)
    if rc != 0:
        sys.exit(f"signing command failed (exit {rc})")

    # Verify the signature actually landed (Get-AuthenticodeSignature
    # equivalent). signtool verify /pa uses the Default Authenticode
    # policy, which is what end users' machines apply.
    signtool = shutil.which("signtool")
    if signtool:
        log("Verifying signature (signtool verify /pa)")
        rc = subprocess.call([signtool, "verify", "/pa", str(exe)])
        if rc != 0:
            sys.exit(f"signature verification failed (exit {rc})")
        log("Signature verified")
    else:
        print("NOTE: signtool not on PATH; skipping signature verification. "
              "Verify manually with: signtool verify /pa <exe> or "
              "Get-AuthenticodeSignature <exe>")


README_TEMPLATE = """\
Lunar {version}
================

A minimalist analog clock for Windows.

Running
-------
Double-click Lunar.exe.  No installer, no runtime, no dependencies beyond
the Windows system DLLs.  Runs on Windows 10 and Windows 11 (x64).

Controls
--------
  * Click the window icon (top-left of the title bar) to open the
    system menu.  It contains:
      - About Lunar
      - Settings...       (chimes, time zone, window behavior)
      - Test beep
      - Always on top     (toggle)
      - Sync clock now
      - View log...
  * Drag any edge .............................. resize
  * Alt+F4 ..................................... quit

Files
-----
  Lunar.exe   single-file executable (static C runtime + mbedTLS)
  README.txt  this file

SHA-256
-------
  {sha}  Lunar.exe
"""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-build", action="store_true",
                    help="Use the existing build/Lunar.exe (skip build.py).")
    ap.add_argument("--open", action="store_true",
                    help="Open the dist folder in Explorer when done.")
    args = ap.parse_args()

    if not args.no_build:
        log("Building release exe")
        rc = subprocess.call(
            [sys.executable, str(ROOT / "scripts" / "build.py"),
             "--no-desktop"],
            cwd=ROOT,
        )
        if rc != 0:
            sys.exit(f"build failed (exit {rc})")

    if not EXE.exists():
        sys.exit(f"missing {EXE}; run build.py first")

    version = read_version()
    bundle_name = f"Lunar-{version}-win-x64"
    bundle_dir = DIST / bundle_name
    zip_path   = DIST / f"{bundle_name}.zip"

    # Fresh dist tree.
    if bundle_dir.exists():
        shutil.rmtree(bundle_dir)
    if zip_path.exists():
        zip_path.unlink()
    bundle_dir.mkdir(parents=True)

    # Stage files.
    staged_exe = bundle_dir / "Lunar.exe"
    shutil.copy2(EXE, staged_exe)
    log(f"Copied {EXE.name} ({staged_exe.stat().st_size / 1048576:.2f} MB)")

    # Sign the staged copy (before hashing/zipping, so the digest in
    # README.txt and the zipped exe both reflect the signed binary).
    sign_exe(staged_exe)

    digest = sha256(staged_exe)
    readme = bundle_dir / "README.txt"
    readme.write_text(
        README_TEMPLATE.format(version=version, sha=digest),
        encoding="utf-8",
        newline="\r\n",
    )
    log(f"Wrote {readme.name}")

    # Include LICENSE (required for distribution; warn if missing so a
    # release without it doesn't slip through silently).
    for name in ("LICENSE", "LICENSE.txt", "LICENSE.md"):
        src = ROOT / name
        if src.is_file():
            shutil.copy2(src, bundle_dir / name)
            log(f"Copied {name}")
            break
    else:
        print("WARNING: no LICENSE file found at repo root; "
              "the zip will not contain a license.")

    # Zip it.
    log(f"Creating {zip_path.name}")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for p in sorted(bundle_dir.rglob("*")):
            if p.is_file():
                zf.write(p, arcname=str(p.relative_to(bundle_dir)))

    log(f"{zip_path}  ({zip_path.stat().st_size / 1024:.1f} KB)")
    print()
    print(f"version:  {version}")
    print(f"exe:      {staged_exe}")
    print(f"bundle:   {bundle_dir}")
    print(f"zip:      {zip_path}")
    print(f"sha256:   {digest}")

    if args.open:
        os.startfile(DIST)  # noqa: S606
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
