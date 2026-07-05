# Lunar

A trusted time reference for Windows, presented as a minimalist digital
dashboard. A hardened C (C23) engine under a statically-linked Tcl/Tk
shell — one self-contained, signed `.exe`.

Lunar keeps its own cryptographically-attested timescale — disciplined
by authenticated NTS consensus, never by the OS clock — and uses it to
tell you the true time, how certain it is, and **when your PC's own
clock is wrong and by how much**. It is fail-honest: it never silently
lies, and it never goes dark just because the network did.

Current version: **0.50** — a two-part `MAJOR.MINOR` string, single-sourced
from the top-level [`VERSION`](VERSION) file (the build injects it into the
exe's version resource and `build/version.h`).

## Build

The shipped product is the Tcl/Tk shell. Building it needs:

- **MSYS2 UCRT64** with `gcc`, `windres`, and Python:

      pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-python

- A **static Tcl/Tk 9** build (headers + static `.a`s). Lunar links it
  in so the exe has no external Tcl/Tk dependency. `tools/tasks.tcl`
  discovers it under the `zmal` layout (`r/tcltk/9.0.3`) or from the
  `ZMAL_TCLTK` environment variable.

From the project root, run the build task with the static `tclsh`:

    tclsh90.exe tools/tasks.tcl build

`scripts/build.py` is invoked by the build to compile the vendored
mbedTLS archive and generate `build/version.h`; it is no longer a
standalone exe builder. Other tasks: `check` (headless self-test),
`shot <png>` (occlusion-proof screenshot), `sign` (Authenticode via the
Certum flow), `repackage` (re-zip `lunar.tcl` onto the bare exe without
recompiling — fast for Tcl-only edits).

Output:

- `dist/lunar.exe` — the app binary, a single self-contained exe.

## Run

Double-click `lunar.exe`. No other files are needed: Tcl/Tk 9 is linked
in statically, the app and its script libraries ride along as an appended
zipfs image, and the C engine (libgcc + mbedTLS) is archived in.

## Project layout

    src/         # C engine (ntp/nts/dns/clock/pin_store/siv/tz/...) plus
                 #   the Tk shell glue: lunarx.c (::lunar::* commands),
                 #   lunar_main.c (entry point), cap.c (screenshot helper)
    lunar.tcl    # the Tk UI (digital dashboard, Settings, tray, menus)
    tools/       # Tcl build tooling (tasks/genres/package/shot/mkico)
    assets/      # icons, fonts
    scripts/     # Python helpers: build.py (mbedTLS archive + version.h),
                 #   gen_tz_embed.py, probe_nts.py
    third_party/ # vendored mbedTLS + IANA tzdata zoneinfo subset
                 #   + CLDR windowsZones (build-time only; see the
                 #   per-directory READMEs under third_party/)
    tests/       # C engine unit tests (run_tests.py) + live NTS probe

## Architecture

- Single static exe: **Tcl/Tk 9 linked in statically**, with the app and
  the Tcl/Tk script libraries appended as a self-mounting zipfs image
  (`TclZipfs_AppHook` + `Tk_Main`). No installer, no runtime deps.
- The **C engine is unchanged** from the native builds — it is exposed to
  the UI as `::lunar::*` commands (`engine_start`, `status`, `sources`,
  `localtime`, `syncnow`, ...) registered from `lunarx.c`. All time logic
  lives in C; Tcl/Tk only draws.
- The UI is a **digital time dashboard**: a large disciplined-time
  readout, the trust state, the honest error bound, a live sources pane,
  and the system-clock delta — with a menubar, Settings dialog, system
  tray, and Event Log viewer.
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
  holdover** (no live consensus at all — the readout keeps running on the
  disciplined oscillator with an honest, growing worst-case error bound,
  ~12 ms/min); **REACQUIRING** (suspend/resume or session handoff broke
  timing continuity — the last verified time is shown, greyed and frozen
  with its age, until an authenticated cycle re-anchors). The red INOP
  state is reserved for genuinely unrenderable states: no sync yet this
  run, or a local fault.
- **System-clock witness.** Lunar never *displays* the Windows clock,
  but with a disciplined reference in hand it *measures* it: the status
  bar shows "PC clock: +N.NN s vs Lunar", and every step in the OS clock
  (a w32time correction, a manual set, a VM time-sync) is logged with its
  magnitude. This is the one comparison the whole trust stack uniquely
  enables.
- **Lives in the tray.** Minimize-to-tray, a live time/zone/state
  tooltip, and an optional run-at-startup entry, so it is an all-day
  instrument rather than a window you reopen every boot.
- Time zones come from an IANA tzdata snapshot embedded at build time;
  the OS time-zone API is never consulted.

## License

MIT — see [LICENSE](LICENSE). Vendored third-party components keep
their own licenses: mbedTLS (Apache-2.0,
`third_party/mbedtls-3.6.6/LICENSE`), IANA tzdata (public domain,
`third_party/tzdata/README.md`), and CLDR windowsZones (Unicode License
v3, `third_party/cldr/LICENSE`). Overview in `third_party/README.md`.
Tcl/Tk is distributed under the BSD-style Tcl license.
