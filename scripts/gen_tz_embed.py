#!/usr/bin/env python3
"""Generate src/tz_embed.c: an IANA tzdata blob and canonical zone index.

Input : a zoneinfo directory (TZif files) such as MSYS2's
        C:\\msys64\\usr\\share\\zoneinfo, plus its `zone1970.tab` file.

Output: src/tz_embed.c -- a C translation unit that publishes

            const char * const g_tz_names[];
            const uint32_t     g_tz_blob_index[];
            const uint32_t     g_tz_blob_offset[];  // per unique blob
            const uint32_t     g_tz_blob_length[];  // per unique blob
            const uint8_t      g_tz_blob[];
            const unsigned     g_tz_name_count;
            const unsigned     g_tz_blob_count;
            const char *       g_tz_version;

Canonical zones are taken from `zone1970.tab` (the modern list).  We also
inject the synthetic "UTC" entry at index 0, even if the distro's file
doesn't list it -- the app's default is UTC and we want a stable ID.

Content is de-duplicated by SHA-256 of the raw TZif bytes so that alias
zones (tzdb "Link" lines) share storage.  The generator does not look
at any operating-system state; the only inputs are the two files above.
"""

from __future__ import annotations
import argparse
import hashlib
import re
import sys
from pathlib import Path


DEFAULT_ZI = Path(r"C:\msys64\usr\share\zoneinfo")


def read_zone_list(zi: Path) -> list[str]:
    tab = zi / "zone1970.tab"
    names: list[str] = []
    with tab.open("r", encoding="utf-8") as f:
        for line in f:
            if not line.strip() or line.startswith("#"):
                continue
            # country-codes \t coords \t TZ [\t comments]
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 3 and parts[2]:
                names.append(parts[2])
    return names


def read_version(zi: Path) -> str:
    # tzdata.zi starts with "# version 2026a"
    zi_file = zi / "tzdata.zi"
    if zi_file.exists():
        with zi_file.open("r", encoding="utf-8") as f:
            first = f.readline().strip()
            m = re.search(r"version\s+(\S+)", first)
            if m:
                return m.group(1).replace("-dirty", "")
    return "unknown"


def emit_byte_array(name: str, data: bytes, out) -> None:
    out.write(f"const uint8_t {name}[] = {{\n")
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        out.write("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
    out.write("};\n")


def emit_u32_array(name: str, values: list[int], out) -> None:
    out.write(f"const uint32_t {name}[] = {{\n")
    for i in range(0, len(values), 8):
        chunk = values[i : i + 8]
        out.write("    " + ", ".join(f"{v}u" for v in chunk) + ",\n")
    out.write("};\n")


def emit_string_table(name: str, names: list[str], out) -> None:
    out.write(f"const char * const {name}[] = {{\n")
    for n in names:
        esc = n.replace("\\", "\\\\").replace('"', '\\"')
        out.write(f'    "{esc}",\n')
    out.write("};\n")


def build(zi: Path, out_path: Path) -> None:
    version = read_version(zi)
    names = read_zone_list(zi)

    # Make sure UTC is present and is index 0.  zone1970.tab sometimes
    # omits it because it is implicit in Etc/; we always want it as the
    # stable default.
    if "UTC" in names:
        names.remove("UTC")
    names = ["UTC"] + sorted(names)

    # De-dup by SHA-256 of the file bytes.
    blob_bytes = bytearray()
    blob_offsets: list[int] = []
    blob_lengths: list[int] = []
    hash_to_blob: dict[str, int] = {}
    name_to_blob: list[int] = []

    missing: list[str] = []
    for n in names:
        # UTC and a handful of others can live at the root OR in Etc/.
        # MSYS2 ships both; prefer the canonical location.
        candidates = [zi / n]
        if n == "UTC":
            candidates = [zi / "UTC", zi / "Etc" / "UTC"]
        path = None
        for c in candidates:
            if c.is_file():
                path = c
                break
        if path is None:
            missing.append(n)
            continue

        with path.open("rb") as f:
            data = f.read()
        # Sanity check: TZif header magic.
        if data[:4] != b"TZif":
            print(f"warning: {n} does not start with TZif magic, skipped",
                  file=sys.stderr)
            missing.append(n)
            continue

        h = hashlib.sha256(data).hexdigest()
        if h in hash_to_blob:
            name_to_blob.append(hash_to_blob[h])
        else:
            idx = len(blob_offsets)
            hash_to_blob[h] = idx
            blob_offsets.append(len(blob_bytes))
            blob_lengths.append(len(data))
            blob_bytes.extend(data)
            name_to_blob.append(idx)

    # If some zones were missing, drop them from the name list in
    # parallel so the indices stay aligned.
    if missing:
        print(f"note: {len(missing)} zones missing from {zi}, dropping",
              file=sys.stderr)
        good_names: list[str] = []
        good_idx: list[int] = []
        k = 0
        for n in names:
            if n in missing:
                continue
            good_names.append(n)
            good_idx.append(name_to_blob[k])
            k += 1
        # But we already advanced k only when not missing above; fix:
        # simpler to rebuild: walk names again and skip.
        good_names = []
        good_idx = []
        # Re-resolve deterministically.
        cursor = 0
        filtered = [n for n in names if n not in missing]
        assert len(filtered) == len(name_to_blob)
        good_names, good_idx = filtered, name_to_blob
        names = good_names
        name_to_blob = good_idx

    # Emit.
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", encoding="utf-8", newline="\n") as out:
        out.write("// tz_embed.c -- generated by scripts/gen_tz_embed.py.\n")
        out.write("// DO NOT EDIT BY HAND.  Regenerate with:\n")
        out.write("//     py scripts\\\\gen_tz_embed.py\n")
        out.write(f"// Source: IANA tzdata {version}\n")
        out.write(f"// Zones : {len(names)} canonical\n")
        out.write(f"// Blobs : {len(blob_offsets)} unique "
                  f"({len(blob_bytes)} bytes)\n")
        out.write("\n")
        out.write("#include <stdint.h>\n\n")
        out.write(f'const char *g_tz_version = "{version}";\n')
        out.write(f"const unsigned g_tz_name_count = {len(names)}u;\n")
        out.write(f"const unsigned g_tz_blob_count = {len(blob_offsets)}u;\n\n")

        emit_string_table("g_tz_names", names, out)
        out.write("\n")
        emit_u32_array("g_tz_blob_index",  name_to_blob, out)
        out.write("\n")
        emit_u32_array("g_tz_blob_offset", blob_offsets, out)
        out.write("\n")
        emit_u32_array("g_tz_blob_length", blob_lengths, out)
        out.write("\n")
        emit_byte_array("g_tz_blob", bytes(blob_bytes), out)

    print(f"wrote {out_path}")
    print(f"  tzdata {version}, {len(names)} zones, "
          f"{len(blob_offsets)} unique blobs, "
          f"{len(blob_bytes)} blob bytes")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--zoneinfo", type=Path, default=DEFAULT_ZI,
                    help=f"Path to the zoneinfo dir (default: {DEFAULT_ZI})")
    ap.add_argument("--out", type=Path,
                    default=Path(__file__).resolve().parents[1] / "src" / "tz_embed.c",
                    help="Output .c path (default: src/tz_embed.c)")
    args = ap.parse_args()

    if not args.zoneinfo.is_dir():
        print(f"error: {args.zoneinfo} is not a directory", file=sys.stderr)
        return 2
    build(args.zoneinfo, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
