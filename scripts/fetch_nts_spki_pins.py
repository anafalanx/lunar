#!/usr/bin/env python3
# fetch_nts_spki_pins.py -- extract SHA-256 SPKI pins for NTS providers.
#
# Run from the repo root:
#
#     py scripts/fetch_nts_spki_pins.py
#
# For each provider listed below, this script:
#   1. Opens a TLS 1.2+ connection to <host>:4460 with ALPN "ntske/1".
#   2. Retrieves the leaf certificate (DER).
#   3. Parses the SubjectPublicKeyInfo field and hashes it SHA-256.
#   4. Prints the hash as a C byte-array literal suitable for pasting
#      into the kProviders[] table in src/nts.c.
#
# NOTE: cert rotations invalidate the pin. Re-run whenever NTS auth
# starts failing after a provider rotates. The printed output includes
# the cert expiry as a hint of how soon you'll need to do that again.
#
# Dependencies: Python 3.10+ stdlib only (ssl, socket, hashlib, struct).

from __future__ import annotations

import hashlib
import socket
import ssl
import struct
import sys
from datetime import datetime, timezone

PROVIDERS = [
    ("time.cloudflare.com",        4460, "cloudflare"),
    ("nts.netnod.se",              4460, "netnod"),
    ("sth1.nts.netnod.se",         4460, "netnod-sth1"),
    ("sth2.nts.netnod.se",         4460, "netnod-sth2"),
    ("ohio.time.system76.com",     4460, "system76-ohio"),
    ("virginia.time.system76.com", 4460, "system76-virginia"),
    ("oregon.time.system76.com",   4460, "system76-oregon"),
    ("paris.time.system76.com",    4460, "system76-paris"),
    ("nts.time.nl",                4460, "sidn"),
]


def fetch_leaf_der(host: str, port: int) -> bytes:
    ctx = ssl.create_default_context()
    # We want the cert, we don't want to reject it if the pin will
    # later be the sole trust anchor. But we still let default CA
    # validation happen for extra assurance during pin capture.
    ctx.set_alpn_protocols(["ntske/1"])
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    with socket.create_connection((host, port), timeout=10) as raw:
        with ctx.wrap_socket(raw, server_hostname=host) as tls:
            der = tls.getpeercert(binary_form=True)
            if not der:
                raise RuntimeError(f"{host}: server did not present a certificate")
            alpn = tls.selected_alpn_protocol()
            if alpn != "ntske/1":
                print(f"warning: {host} negotiated ALPN={alpn!r} (expected ntske/1)",
                      file=sys.stderr)
            return der


# Minimal DER walker to extract the SPKI field from an X.509 certificate.
#
# Certificate  ::= SEQUENCE {
#     tbsCertificate      TBSCertificate,       <-- we enter this
#     signatureAlgorithm  AlgorithmIdentifier,
#     signatureValue      BIT STRING
# }
# TBSCertificate ::= SEQUENCE {
#     [0] version           EXPLICIT INTEGER,    <-- optional
#     serialNumber          INTEGER,
#     signature             AlgorithmIdentifier,
#     issuer                Name,
#     validity              Validity,            <-- used only for hint
#     subject               Name,
#     subjectPublicKeyInfo  SubjectPublicKeyInfo <-- this is our target
#     ... optional fields we don't care about
# }


def _read_tlv(buf: bytes, i: int) -> tuple[int, int, int, int]:
    """Return (tag, hdr_start, content_start, content_end)."""
    tag = buf[i]
    j = i + 1
    length = buf[j]
    j += 1
    if length & 0x80:
        n = length & 0x7F
        length = int.from_bytes(buf[j:j + n], "big")
        j += n
    return tag, i, j, j + length


def extract_spki_and_expiry(cert_der: bytes) -> tuple[bytes, datetime]:
    # outer SEQUENCE (Certificate)
    tag, _, cs, ce = _read_tlv(cert_der, 0)
    assert tag == 0x30
    # first inner field: TBSCertificate SEQUENCE
    tag, _, ts, te = _read_tlv(cert_der, cs)
    assert tag == 0x30
    p = ts

    # [0] EXPLICIT version (optional)
    if cert_der[p] == 0xA0:
        _, _, _, p_end = _read_tlv(cert_der, p)
        p = p_end

    # serialNumber INTEGER
    _, _, _, p = _read_tlv(cert_der, p)
    # signature AlgorithmIdentifier SEQUENCE
    _, _, _, p = _read_tlv(cert_der, p)
    # issuer Name SEQUENCE
    _, _, _, p = _read_tlv(cert_der, p)
    # validity SEQUENCE { notBefore, notAfter }
    _, _, vs, ve = _read_tlv(cert_der, p)
    # notBefore
    _, _, _, nb_end = _read_tlv(cert_der, vs)
    # notAfter
    na_tag, na_hdr, na_cs, na_ce = _read_tlv(cert_der, nb_end)
    na_bytes = cert_der[na_cs:na_ce]
    if na_tag == 0x17:  # UTCTime YYMMDDHHMMSSZ
        s = na_bytes.decode("ascii")
        y = int(s[0:2]); y += 2000 if y < 50 else 1900
        expiry = datetime(y, int(s[2:4]), int(s[4:6]),
                          int(s[6:8]), int(s[8:10]), int(s[10:12]),
                          tzinfo=timezone.utc)
    else:  # GeneralizedTime YYYYMMDDHHMMSSZ
        s = na_bytes.decode("ascii")
        expiry = datetime(int(s[0:4]), int(s[4:6]), int(s[6:8]),
                          int(s[8:10]), int(s[10:12]), int(s[12:14]),
                          tzinfo=timezone.utc)
    p = ve

    # subject Name SEQUENCE
    _, _, _, p = _read_tlv(cert_der, p)
    # subjectPublicKeyInfo SEQUENCE -- the pin target
    _, hdr, cs_spki, ce_spki = _read_tlv(cert_der, p)
    spki_der = cert_der[hdr:ce_spki]
    return spki_der, expiry


def main() -> int:
    print("// Generated by scripts/fetch_nts_spki_pins.py")
    print("// Paste each line into the matching entry of kProviders[] in src/nts.c.\n")
    rc = 0
    for host, port, label in PROVIDERS:
        try:
            der = fetch_leaf_der(host, port)
            spki, expiry = extract_spki_and_expiry(der)
            pin = hashlib.sha256(spki).digest()
            lit = ", ".join(f"0x{b:02x}" for b in pin)
            print(f"// {label}: {host}  (leaf expires {expiry.isoformat()})")
            print(f"// spki_pin = {{ {lit} }}\n")
        except Exception as e:  # pragma: no cover -- network errors
            print(f"// {label}: {host}  FAILED: {e}\n")
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
