# Lunar

Minimalist analog clock for Windows. Native C + Direct2D.

Current version: **0.4.0** — single-sourced from the top-level
[`VERSION`](VERSION) file (the build injects it into the exe's version
resource and `build/version.h`).

## Build

Requires **MSYS2 UCRT64** with `gcc`, `windres`, and Python:

    pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-python

From the project root:

    C:\msys64\ucrt64\bin\python.exe scripts/build.py

Any Python 3.12+ works for the build tooling; a local virtual
environment (`py -3 -m venv .venv`, then
`.venv\Scripts\python.exe scripts/build.py`) is fine too, but the
MSYS2 UCRT64 interpreter is the recommended default since MSYS2 is
required anyway for the toolchain.

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
    third_party/ # vendored mbedTLS + IANA tzdata zoneinfo subset
                 # (see third_party/tzdata/README.md)
    tests/       # C unit tests + Python smoke tests
    .venv/       # optional local Python environment (git-ignored)

## Architecture

- Single-process C program compiled with `gcc -mwindows`.
- Top-level window is a plain Win32 `HWND` created by `CreateWindowExW`.
- All drawing (dial, hands, icons) uses **Direct2D** primitives; text is
  rendered with **DirectWrite**.
- Menus live entirely in the window's system menu (click the icon in
  the title bar, or right-click the title bar).
- Time is disciplined by six parallel sources: four plain-SNTP
  national-metrology / research-lab servers and two NTS-authenticated
  anchors (RFC 8915, TLS 1.3, local enrolled SPKI pins). The trust gate
  requires two operator-diverse NTS anchors to agree and at least 3 of 4
  core sources to concur. If NTS is unavailable but at least 3 core
  sources still corroborate the held anchor within a tighter 100 ms gate
  and the last authenticated sync was under two hours ago, the clock keeps
  running in a degraded, rate-frozen state and badges the time
  UNAUTHENTICATED rather than going dark; anything weaker renders the red
  INOP state.
- Time zones come from an IANA tzdata snapshot embedded at build time;
  the OS time-zone API is never consulted.
- Runtime dependencies: only OS-shipped DLLs (`d2d1`, `dwrite`,
  `user32`, `gdi32`, `comctl32`, `winmm`, `kernel32`, `ws2_32`,
  `bcrypt`, `ole32`, `uxtheme`, `dwmapi`, `advapi32`, `shell32`).

## License

MIT — see [LICENSE](LICENSE). Vendored third-party components keep
their own licenses (see `third_party/README.md` and
`third_party/tzdata/README.md`).
