# Lunar

Minimalist analog clock for Windows. Version 0.2.0, native C + Direct2D.

## Build

Requires:

- **MSYS2 UCRT64** with `gcc` and `windres`:

      pacman -S mingw-w64-ucrt-x86_64-gcc

- **Python 3.12+** (for the build tooling). A local virtual environment is
  expected at `.venv/`:

      py -3 -m venv .venv

From the project root:

    .venv\Scripts\python.exe scripts/build.py

Options:

    --no-desktop       don't copy the exe to the desktop
    --dest <path>      copy the exe to <path> instead of the desktop

Output:

- `build/Lunar.exe` — the app binary (copied to the desktop by default).

## Run

Double-click `Lunar.exe`. No other files are needed: the binary is
fully static (libgcc/libstdc++ linked in, mbedTLS archived in).

## Project layout

    src/         # C source (Win32 shell + Direct2D renderer)
    assets/      # icons, fonts
    scripts/     # Python build tooling (build.py, gen_tz_embed.py)
    third_party/ # vendored mbedTLS + tzdata
    tests/       # C unit tests + Python smoke tests
    .venv/       # local Python environment (git-ignored)

## Architecture

- Single-process C program compiled with `gcc -mwindows`.
- Top-level window is a plain Win32 `HWND` created by `CreateWindowExW`.
- All drawing (dial, hands, icons) uses **Direct2D** primitives; text is
  rendered with **DirectWrite**.
- Menus live entirely in the window's system menu (click the icon in
  the title bar, or right-click the title bar).
- Time is disciplined against NIST/PTB/NICT (SNTPv4) and an NTS anchor
  (RFC 8915, TLS 1.3, SPKI-pinned), with a 2-of-3 concurrence gate.
- Time zones come from an IANA tzdata snapshot embedded at build time;
  the OS time-zone API is never consulted.
- Runtime dependencies: only OS-shipped DLLs (`d2d1`, `dwrite`,
  `user32`, `gdi32`, `comctl32`, `winmm`, `kernel32`, `ws2_32`,
  `bcrypt`, `ole32`, `uxtheme`, `dwmapi`, `advapi32`, `shell32`).
