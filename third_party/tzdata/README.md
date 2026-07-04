# Vendored IANA tzdata (compiled zoneinfo)

- Version:     **2026b** (MSYS2 `tzdata` package reports `2026b-dirty`;
  the `-dirty` suffix comes from MSYS2's packaging patches and is
  stripped by `scripts/gen_tz_embed.py`)
- Source:      `C:\msys64\usr\share\zoneinfo` (MSYS2 `tzdata` package)
- Vendored on: 2026-07-04
- Size:        ~830 KB, 319 files
- Licence:     public domain (IANA time zone database)

## Why this exists

`scripts/gen_tz_embed.py` compiles a zoneinfo tree into `src/tz_embed.c`
(the tzdata snapshot embedded in `Lunar.exe`). Reading whatever tree a
build machine happens to have at `C:\msys64\usr\share\zoneinfo` is not
hermetic: two machines with different `tzdata` package versions produce
different binaries from the same commit. This vendored tree is the
generator's default input, so the embedded tzdata is pinned by the repo.

## What is vendored

Only what `gen_tz_embed.py` consumes (not the full ~4.7 MB tree):

- every compiled TZif zone listed in `zone1970.tab` (the canonical
  modern zone list the generator indexes), plus `UTC` / `Etc/UTC`
- the tab files: `zone1970.tab`, `zone.tab`, `iso3166.tab`,
  `zonenow.tab`
- `tzdata.zi` (first line carries the tzdata version string)

## Refresh procedure (release-time, when tzdata updates)

1. Update the MSYS2 package:

       pacman -S tzdata

2. Re-copy the consumed subset into this directory. From an MSYS2
   shell at the repo root:

       DEST=$(pwd)/third_party/tzdata/zoneinfo
       rm -rf "$DEST" && mkdir -p "$DEST"
       cd /c/msys64/usr/share/zoneinfo
       cp --parents $(awk -F'\t' '!/^#/ && NF>=3 {print $3}' zone1970.tab | sort -u) \
          UTC Etc/UTC zone1970.tab zone.tab iso3166.tab zonenow.tab tzdata.zi \
          "$DEST/"

   (Or copy the whole tree and prune; the generator ignores files it
   doesn't consume, but keeping the subset small keeps review easy.)

3. Regenerate the embedded snapshot:

       C:\msys64\ucrt64\bin\python.exe scripts\gen_tz_embed.py

4. Update the version and date at the top of this file, rebuild, run
   the tests, and commit the vendored tree + regenerated
   `src/tz_embed.c` together as one commit ("Bump tzdata to 20XXx").
