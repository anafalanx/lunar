"""Probe an arbitrary NTS-KE host to see if it responds to KE + NTS-SNTP."""
import sys, ssl, socket, struct, secrets, time

def ntske_exchange(host, port=4460):
    ctx = ssl.create_default_context()
    ctx.set_alpn_protocols(["ntske/1"])
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    with socket.create_connection((host, port), timeout=6) as raw:
        with ctx.wrap_socket(raw, server_hostname=host) as s:
            # request: next_proto=0 (NTPv4) + aead=15 + EOM, all critical
            req = b"\x80\x01\x00\x02\x00\x00"  # next_proto NTPv4
            req += b"\x80\x04\x00\x02\x00\x0f"  # AEAD SIV-CMAC-256
            req += b"\x80\x00\x00\x00"          # EOM
            s.sendall(req)
            buf = b""
            while True:
                chunk = s.recv(4096)
                if not chunk: break
                buf += chunk
                if len(buf) > 65536: break
                # stop after we see a plausible EOM
                if b"\x80\x00\x00\x00" in buf and len(buf) > 16: break
    return buf, None, None

def parse_records(buf):
    recs = []; i = 0
    while i + 4 <= len(buf):
        t = int.from_bytes(buf[i:i+2], "big") & 0x7fff
        c = buf[i] & 0x80
        L = int.from_bytes(buf[i+2:i+4], "big")
        body = buf[i+4:i+4+L]
        recs.append((t, bool(c), body))
        i += 4 + L
        if t == 0: break
    return recs

def probe(host):
    print(f"=== {host} ===")
    try:
        buf, c2s, s2c = ntske_exchange(host)
    except Exception as e:
        print(f"  KE failed: {e}"); return
    cookies = 0; srv = None; port = None; proto = None; aead = None
    for t, crit, body in parse_records(buf):
        if   t == 1: proto = int.from_bytes(body[:2], "big")
        elif t == 4: aead  = int.from_bytes(body[:2], "big")
        elif t == 5: cookies += 1
        elif t == 6: srv   = body.decode("ascii", "replace")
        elif t == 7: port  = int.from_bytes(body[:2], "big")
    print(f"  KE ok proto={proto} aead={aead} cookies={cookies} srv={srv} port={port}")
    first_cookie = next((b for t, _, b in parse_records(buf) if t == 5), None)
    if first_cookie is None: return
    # Build NTS-protected SNTP with single cookie, no placeholders
    ntp_host = srv or host
    ntp_port = port or 123
    print(f"  probing udp {ntp_host}:{ntp_port} (len varies)")
    for test_label, n_placeholder in [("minimal(cookie only)", 0)]:
        uid = secrets.token_bytes(32)
        # naive NTS EF encoder (UID + Cookie only, no auth -- PTB will reject,
        # but we want to know whether they ACK any NTS-shaped packet).
        # Use full authenticated build to match real client:
        pass
    # Skip — we've already got that with the C client. This script just
    # confirms the KE side is healthy. The real question is UDP reachability.
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(4.0)
        s.connect((ntp_host, ntp_port))
        # plain SNTP
        p = bytearray(48); p[0] = 0x23
        s.sendall(p)
        r = s.recv(1500)
        print(f"  plain SNTP reply len={len(r)} LI/VN/Mode=0x{r[0]:02x}")
        # padded to 228 bytes (simulating NTS packet size)
        padded = bytes(p) + b"\x01\x04\x00\x24" + b"\0"*32 + b"\x02\x04\x00\x68" + b"\0"*100 + b"\x04\x04\x00\x38" + b"\0"*52
        print(f"  padded probe len={len(padded)}")
        s.sendall(padded)
        try:
            r2 = s.recv(1500); print(f"  padded SNTP reply len={len(r2)}")
        except socket.timeout:
            print(f"  padded SNTP TIMED OUT (firewall drops large NTP?)")
    except Exception as e:
        print(f"  udp probe failed: {e}")

for h in sys.argv[1:] or ["ptbtime1.ptb.de", "ptbtime2.ptb.de",
                          "ptbtime3.ptb.de", "ptbtime4.ptb.de"]:
    probe(h)
