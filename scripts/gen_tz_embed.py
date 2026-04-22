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
import datetime as _dt
import hashlib
import re
import struct
import sys
from pathlib import Path


DEFAULT_ZI = Path(r"C:\msys64\usr\share\zoneinfo")

# Lunar only displays "now".  Everything strictly before this cutoff
# gets discarded from each zone's transition table -- the saved space
# is substantial (raw tzdata is ~200 KB of mostly pre-WWII history).
# Any time >= CUTOFF resolves correctly via the trimmed table + POSIX
# footer.  Regenerate yearly (or whenever tzdata updates) to advance
# the cutoff.
CUTOFF_EPOCH = int(_dt.datetime(2026, 1, 1,
                                tzinfo=_dt.timezone.utc).timestamp())


def _u32(b: bytes, o: int) -> int: return struct.unpack(">I", b[o:o + 4])[0]
def _i32(b: bytes, o: int) -> int: return struct.unpack(">i", b[o:o + 4])[0]
def _i64(b: bytes, o: int) -> int: return struct.unpack(">q", b[o:o + 8])[0]


def trim_tzif(data: bytes, cutoff_epoch: int) -> bytes:
    """Rebuild a TZif file with only transitions >= cutoff_epoch.

    The result keeps one synthetic "initial" transition exactly at
    cutoff_epoch so that any query at or after the cutoff resolves
    correctly, and preserves the POSIX TZ footer verbatim for the
    recurring-rule tail.  Types and abbreviations are minimised."""
    if data[:4] != b"TZif":
        return data  # not a TZif; leave alone

    # --- Parse v1 header to know where v2 starts. ---
    tti_ut = _u32(data, 20); tti_st = _u32(data, 24); leap = _u32(data, 28)
    tcnt1  = _u32(data, 32); typec1 = _u32(data, 36); charc1 = _u32(data, 40)
    v1_size = (tcnt1 * 4 + tcnt1 + typec1 * 6 + charc1
               + leap * 8 + tti_st + tti_ut)
    v1_end = 44 + v1_size

    # --- Parse v2 header + data. ---
    h2 = data[v1_end:v1_end + 44]
    if h2[:4] != b"TZif":
        return data
    tti_ut = _u32(h2, 20); tti_st = _u32(h2, 24); leap = _u32(h2, 28)
    tcnt   = _u32(h2, 32); typec  = _u32(h2, 36); charc = _u32(h2, 40)

    p = v1_end + 44
    trans    = [_i64(data, p + i * 8) for i in range(tcnt)]
    p += tcnt * 8
    type_idx = list(data[p:p + tcnt])
    p += tcnt
    ttinfos = []
    for i in range(typec):
        off = p + i * 6
        ttinfos.append((_i32(data, off), data[off + 4], data[off + 5]))
    p += typec * 6
    abbrs = data[p:p + charc]
    p += charc
    # skip leap, std, utc arrays
    p += leap * 12 + tti_st + tti_ut

    # --- POSIX TZ footer ("\n<tz>\n"). ---
    footer = b""
    if p < len(data) and data[p:p + 1] == b"\n":
        end = data.find(b"\n", p + 1)
        if end > 0:
            footer = data[p:end + 1]

    # --- Determine the type in effect at cutoff_epoch. ---
    initial_type = 0
    for i, t in enumerate(trans):
        if t <= cutoff_epoch:
            initial_type = type_idx[i]
        else:
            break

    # --- Keep only future transitions, prepend a synthetic one. ---
    new_trans    = [cutoff_epoch]
    new_type_idx = [initial_type]
    for i, t in enumerate(trans):
        if t > cutoff_epoch:
            new_trans.append(t)
            new_type_idx.append(type_idx[i])

    # --- Remap types to only what's referenced. ---
    used = []
    seen = {}
    for ti in new_type_idx:
        if ti not in seen:
            seen[ti] = len(used)
            used.append(ti)
    new_type_idx = [seen[ti] for ti in new_type_idx]

    # Rebuild abbreviation table, de-duplicating strings.
    new_ttinfos: list[tuple[int, int, int]] = []
    new_abbrs = bytearray()
    abbr_idx: dict[bytes, int] = {}
    for old in used:
        gmtoff, isdst, aidx = ttinfos[old]
        end = abbrs.find(b"\x00", aidx)
        if end < 0:
            end = len(abbrs)
        abbr_str = bytes(abbrs[aidx:end])
        if abbr_str in abbr_idx:
            new_aidx = abbr_idx[abbr_str]
        else:
            new_aidx = len(new_abbrs)
            new_abbrs += abbr_str + b"\x00"
            abbr_idx[abbr_str] = new_aidx
        new_ttinfos.append((gmtoff, 1 if isdst else 0, new_aidx))

    # --- Encode. ---
    def hdr(tcnt_, typec_, charc_) -> bytes:
        # magic + ver + 15 reserved + 6 counts (isutcnt, isstdcnt, leap,
        # timecnt, typecnt, charcnt).
        return (b"TZif2" + b"\x00" * 15
                + struct.pack(">IIIIII", 0, 0, 0, tcnt_, typec_, charc_))

    out = bytearray()

    # v1 stub (mandatory to satisfy parsers that scan the v1 block).
    # tcnt=0, typec=1, charc=1 (single zero ttinfo + "\0" abbr).
    out += hdr(0, 1, 1)
    out += struct.pack(">ibB", 0, 0, 0)   # one ttinfo (6 bytes)
    out += b"\x00"                        # one abbr char

    # v2 real data.
    nt = len(new_trans)
    ny = len(new_ttinfos)
    nc = len(new_abbrs)
    out += hdr(nt, ny, nc)
    for t in new_trans:
        out += struct.pack(">q", t)
    out += bytes(new_type_idx)
    for gmtoff, isdst, aidx in new_ttinfos:
        out += struct.pack(">ibB", gmtoff, isdst, aidx)
    out += bytes(new_abbrs)

    # Footer verbatim (or empty newline pair if absent).
    if footer:
        out += footer
    else:
        out += b"\n\n"
    return bytes(out)


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

        # Trim to "now and later" -- see CUTOFF_EPOCH.
        data = trim_tzif(data, CUTOFF_EPOCH)

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
        out.write(f"// Cutoff: transitions < "
                  f"{_dt.datetime.fromtimestamp(CUTOFF_EPOCH, _dt.timezone.utc).isoformat()}"
                  " dropped\n")
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
