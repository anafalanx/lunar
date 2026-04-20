# Lunar

Minimalist analog clock for Windows. Version 0.2.0 (native C + raylib).

## Build

Requires:

- **MSYS2 UCRT64** with `gcc`, `windres`, and the
  `mingw-w64-ucrt-x86_64-raylib` package:

      pacman -S mingw-w64-ucrt-x86_64-raylib

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

Unzip and double-click `Lunar.exe`. `glfw3.dll` must sit next to it.

## Project layout

    src/         # C source (Win32 shell + raylib renderer)
    assets/      # icons, fonts
    scripts/     # Python build tooling (build.py)
    .venv/       # local Python environment (git-ignored)

## Architecture

- Single-process C program compiled with `gcc -mwindows`.
- Top-level window is created by raylib (via GLFW) with MSAA 4×.
- A native Win32 menu bar and status bar are attached to the raylib HWND.
- All drawing (dial, hands, icons) uses raylib primitives with anti-aliasing.
- Runtime dependencies: `glfw3.dll` (shipped) plus OS-shipped DLLs
  (`opengl32`, `gdi32`, `user32`, `comctl32`, `winmm`, `kernel32`).
