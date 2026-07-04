# Lunar

A trusted time reference for Windows, drawn as a minimalist analog
clock. Native C (C23) + Direct2D.

Lunar keeps its own cryptographically-attested timescale — disciplined
by authenticated NTS consensus, never by the OS clock — and uses it to
tell you the true time, how certain it is, and **when your PC's own
clock is wrong and by how much**. It is fail-honest: it never silently
lies, and it never goes dark just because the network did.

Current version: **0.20.0** — single-sourced from the top-level
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
                 # + CLDR windowsZones (build-time only; see the
                 # per-directory READMEs under third_party/)
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
  core sources to concur.
- The display policy is **fail-honest**: the clock never silently lies,
  but it also never goes dark just because the network did. In
  descending confidence: full trust; **UNAUTHENTICATED** (NTS down, but
  3+ core sources corroborate the held anchor within 100 ms, allowed
  for up to 8 h after the last authenticated cycle); **UNSYNCED
  holdover** (no live consensus at all — the dial keeps running on the
  disciplined oscillator with an honest, growing worst-case error bound
  badge, ~12 ms/min); **REACQUIRING** (suspend/resume or session
  handoff broke timing continuity — the face shows the last verified
  time, greyed and frozen with its age, until an authenticated cycle
  re-anchors). The red INOP face is reserved for genuinely unrenderable
  states: no sync yet this run, or a local fault.
- **System-clock witness.** Lunar never *displays* the Windows clock,
  but with a disciplined reference in hand it *measures* it: the About
  dialog shows "System clock: +N.NN s vs Lunar", the dial badges the PC
  clock when it drifts past half a second, and every step in the OS
  clock (a w32time correction, a manual set, a VM time-sync) is logged
  with its magnitude. This is the one comparison the whole trust stack
  uniquely enables.
- **Lives in the tray.** Minimize-to-tray, a live time/zone/state
  tooltip, and an optional run-at-startup entry, so it is an all-day
  instrument rather than a window you reopen every boot.
- Time zones come from an IANA tzdata snapshot embedded at build time;
  the OS time-zone API is never consulted.
- Runtime dependencies: only OS-shipped DLLs (`d2d1`, `dwrite`,
  `user32`, `gdi32`, `comctl32`, `winmm`, `kernel32`, `ws2_32`,
  `bcrypt`, `ole32`, `uxtheme`, `dwmapi`, `advapi32`, `shell32`).

## License

MIT — see [LICENSE](LICENSE). Vendored third-party components keep
their own licenses: mbedTLS (Apache-2.0,
`third_party/mbedtls-3.6.6/LICENSE`), IANA tzdata (public domain,
`third_party/tzdata/README.md`), and CLDR windowsZones (Unicode License
v3, `third_party/cldr/LICENSE`). Overview in `third_party/README.md`.
