// dns.c -- DNS-over-HTTPS resolver with SPKI-pinned leaves + 2-of-N
// cross-resolver agreement gate. See dns.h for design rationale.

// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
// clang-format on

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "dns.h"
#include "logbuf.h"
#include "clock.h"

#include "psa/crypto.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/sha256.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/constant_time.h"

// ---------------------------------------------------------------------------
// Pinned resolver table
// ---------------------------------------------------------------------------
//
// Each resolver has (a) hardcoded anycast IP(s) so bootstrap needs no
// DNS, and (b) an SPKI pin captured by scripts/fetch_doh_spki_pins.py.
// Leaf rotations require a pin refresh + rebuild; see docs/pins.md.
// An all-zero pin disables the entry (tests use this to shrink the
// pool without removing entries).

static const DnsResolver kResolvers[] = {
    { "cloudflare-dns.com", "1.1.1.1",       "1.0.0.1",           "cloudflare",
      /* spki_pin (leaf expires 2026-12-21) = */ {
          0x96, 0xd4, 0x3a, 0x69, 0x7c, 0xb7, 0xb6, 0xaa,
          0x4d, 0x64, 0xa2, 0x5d, 0x9d, 0xeb, 0xcc, 0x0f,
          0xba, 0x11, 0xf8, 0x8b, 0x08, 0xe6, 0xb3, 0x56,
          0x6c, 0xeb, 0x2c, 0x14, 0x3a, 0xe5, 0xf8, 0x4c } },

    { "dns.quad9.net",      "9.9.9.9",       "149.112.112.112",   "quad9",
      /* spki_pin (leaf expires 2026-07-27) = */ {
          0x8b, 0x69, 0x0e, 0x6d, 0xfc, 0xf4, 0xa8, 0x82,
          0x82, 0x18, 0xd5, 0xad, 0xec, 0xc8, 0xc1, 0x51,
          0xe4, 0xab, 0x87, 0x40, 0xf2, 0x8d, 0xbd, 0x3f,
          0xcd, 0x62, 0x0d, 0x22, 0x66, 0x44, 0x4b, 0xe2 } },

    { "dns.google",         "8.8.8.8",       "8.8.4.4",           "google",
      /* spki_pin (leaf expires 2026-06-22) = */ {
          0x6a, 0x48, 0x59, 0x8c, 0x97, 0x8e, 0x53, 0xe1,
          0x57, 0xec, 0xb6, 0x3b, 0xb1, 0x09, 0x3e, 0xcc,
          0xde, 0x3a, 0xd9, 0x3d, 0x9f, 0xb7, 0xa3, 0x85,
          0x43, 0xe8, 0xd2, 0xc9, 0xac, 0xee, 0xa5, 0xed } },

    { "dns.nextdns.io",     "45.90.28.0",    NULL,                "nextdns",
      /* spki_pin (leaf expires 2026-05-26) = */ {
          0xc9, 0xdc, 0x3d, 0x6f, 0x37, 0x02, 0x33, 0xac,
          0x4a, 0xc3, 0x36, 0x8d, 0xa7, 0x11, 0xa6, 0x74,
          0x5e, 0x68, 0x26, 0x50, 0xd0, 0x6c, 0xa7, 0xdb,
          0xe4, 0xd9, 0xcc, 0xae, 0xc2, 0x87, 0x2a, 0x3b } },

    { "dns.mullvad.net",    "194.242.2.2",   NULL,                "mullvad",
      /* spki_pin (leaf expires 2026-06-21) = */ {
          0xc8, 0x44, 0x8c, 0x68, 0x3d, 0x14, 0x2b, 0x43,
          0x6e, 0x55, 0xa3, 0x86, 0xa1, 0x0b, 0xb6, 0xa2,
          0x59, 0xc6, 0x1c, 0x82, 0x2a, 0xfb, 0x7d, 0xd0,
          0x90, 0xeb, 0x46, 0xd4, 0x73, 0x53, 0x0e, 0x96 } },
};

#define DNS_POOL_SIZE (sizeof kResolvers / sizeof kResolvers[0])

const DnsResolver *Dns_Pool(size_t *out_len)
{
    if (out_len) *out_len = DNS_POOL_SIZE;
    return kResolvers;
}

static int pin_is_zero(const uint8_t pin[32])
{
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= pin[i];
    return acc == 0;
}

size_t Dns_PickResolvers(const DnsResolver **out, size_t n_want)
{
    if (!out || n_want == 0) return 0;
    size_t enabled[DNS_POOL_SIZE];
    size_t n_enabled = 0;
    for (size_t i = 0; i < DNS_POOL_SIZE; i++) {
        if (!pin_is_zero(kResolvers[i].spki_pin)) enabled[n_enabled++] = i;
    }
    if (n_enabled == 0) return 0;
    if (n_want > n_enabled) n_want = n_enabled;

    for (size_t i = 0; i < n_want; i++) {
        uint32_t r = 0;
        if (BCryptGenRandom(NULL, (PUCHAR)&r, sizeof r,
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            return i;   // partial; caller decides
        }
        size_t j = i + (size_t)(r % (uint32_t)(n_enabled - i));
        size_t tmp = enabled[i];
        enabled[i] = enabled[j];
        enabled[j] = tmp;
        out[i] = &kResolvers[enabled[i]];
    }
    return n_want;
}

// ===========================================================================
// DNS wire format -- RFC 1035
// ===========================================================================

#define DNS_TYPE_A      1
#define DNS_TYPE_CNAME  5
#define DNS_CLASS_IN    1
#define DNS_HDR_LEN     12
#define DNS_MAX_NAME    255

// Encode a hostname as a length-prefixed label sequence into buf.
// Returns bytes written (including the terminating 0), or 0 on error.
static size_t encode_qname(const char *host, uint8_t *buf, size_t cap)
{
    if (!host || !*host) return 0;
    size_t  out  = 0;
    size_t  llen = 0;
    size_t  llp  = 0;    // position of current label-length byte
    if (cap < 1) return 0;
    buf[out++] = 0;      // placeholder for first length

    for (const char *p = host; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '.') {
            if (llen == 0) return 0;             // empty label
            if (llen > 63) return 0;             // label too long
            buf[llp] = (uint8_t)llen;
            if (out >= cap) return 0;
            llp = out;
            buf[out++] = 0;
            llen = 0;
        } else {
            // Valid DNS label chars: letters, digits, hyphen, underscore.
            // Reject anything else to avoid injection tricks.
            if (!(isalnum(c) || c == '-' || c == '_')) return 0;
            if (llen >= 63) return 0;
            if (out >= cap) return 0;
            buf[out++] = (uint8_t)c;
            llen++;
        }
    }
    // Trailing dot is optional; if host didn't end in one, close the
    // final label.
    if (llen > 0) {
        if (llen > 63) return 0;
        buf[llp] = (uint8_t)llen;
        if (out >= cap) return 0;
        buf[out++] = 0;     // root label
    } else {
        // already terminated by the '.' handler; the placeholder slot
        // from the loop IS the terminating 0.
    }
    if (out > DNS_MAX_NAME) return 0;
    return out;
}

int Dns_BuildQueryA(const char *host, uint16_t id,
                    uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!host || !out || !out_len) return -1;
    if (out_cap < DNS_HDR_LEN + 5) return -1;

    // Header: ID, flags (RD=1), QDCOUNT=1.
    out[0] = (uint8_t)(id >> 8);
    out[1] = (uint8_t)(id & 0xff);
    out[2] = 0x01;      // RD
    out[3] = 0x00;
    out[4] = 0x00; out[5] = 0x01;   // QDCOUNT = 1
    out[6] = 0x00; out[7] = 0x00;   // ANCOUNT
    out[8] = 0x00; out[9] = 0x00;   // NSCOUNT
    out[10]= 0x00; out[11]= 0x00;   // ARCOUNT

    size_t qn = encode_qname(host, out + DNS_HDR_LEN,
                             out_cap - DNS_HDR_LEN - 4);
    if (qn == 0) return -1;

    size_t p = DNS_HDR_LEN + qn;
    out[p++] = 0x00; out[p++] = DNS_TYPE_A;
    out[p++] = 0x00; out[p++] = DNS_CLASS_IN;

    *out_len = p;
    return 0;
}

// Decode a (possibly compressed) domain name starting at `off` into
// `out_name` (a NUL-terminated lowercased dotted string). Returns the
// offset JUST AFTER the name on success (or for compressed names,
// after the 2-byte pointer in the source). On error returns 0.
// `out_name` may be NULL if the caller only wants to advance past the
// name without decoding it.
static size_t decode_name(const uint8_t *in, size_t len, size_t off,
                          char *out_name, size_t out_cap)
{
    if (off >= len) return 0;
    size_t op        = 0;
    size_t cursor    = off;
    int    followed  = 0;
    size_t end_after = 0;    // offset just after the first label sequence
    int    depth     = 0;

    for (;;) {
        if (cursor >= len) return 0;
        if (depth++ > 32) return 0;   // anti-loop ceiling
        uint8_t b = in[cursor];
        if ((b & 0xC0) == 0xC0) {
            // Pointer: two-byte big-endian, lower 14 bits is offset.
            if (cursor + 1 >= len) return 0;
            size_t ptr = ((size_t)(b & 0x3F) << 8) | in[cursor + 1];
            if (!followed) {
                end_after = cursor + 2;
                followed  = 1;
            }
            if (ptr >= cursor) return 0;  // forward/self pointer -> reject
            cursor = ptr;
            continue;
        }
        if (b & 0xC0) return 0;          // reserved labels
        if (b == 0) {
            // End of name.
            if (!followed) end_after = cursor + 1;
            if (out_name && op < out_cap) out_name[op] = 0;
            else if (out_name) return 0;
            return end_after;
        }
        size_t llen = b;
        cursor++;
        if (cursor + llen > len) return 0;
        if (llen > 63) return 0;
        if (out_name) {
            if (op > 0) {
                if (op + 1 >= out_cap) return 0;
                out_name[op++] = '.';
            }
            if (op + llen >= out_cap) return 0;
            for (size_t i = 0; i < llen; i++) {
                unsigned char c = in[cursor + i];
                // Lowercase ASCII for case-insensitive compare.
                if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
                out_name[op++] = (char)c;
            }
        }
        cursor += llen;
    }
}

// Case-insensitive ASCII domain compare. Tolerates a trailing dot on
// either side.
static int domain_equal_ci(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return 0;
    }
    // Tolerate trailing dots
    if (*a == '.' && !*(a + 1)) a++;
    if (*b == '.' && !*(b + 1)) b++;
    return *a == 0 && *b == 0;
}

int Dns_ParseResponseA(const uint8_t *in, size_t in_len,
                       uint16_t expect_id, const char *expect_host,
                       char (*out_ips)[16], size_t ips_cap,
                       size_t *out_count,
                       uint32_t *out_min_ttl)
{
    if (!in || !out_ips || !out_count || !expect_host) return -1;
    if (in_len < DNS_HDR_LEN) return -1;

    uint16_t id      = ((uint16_t)in[0] << 8) | in[1];
    uint16_t flags   = ((uint16_t)in[2] << 8) | in[3];
    uint16_t qdcount = ((uint16_t)in[4] << 8) | in[5];
    uint16_t ancount = ((uint16_t)in[6] << 8) | in[7];

    if (id != expect_id)  return -3;
    if ((flags & 0x8000) == 0) return -1;   // not a response
    int rcode = flags & 0x000F;
    if (rcode != 0) return -2;
    if (qdcount != 1) return -1;

    // Decode the question name and verify it matches expect_host.
    char  qname[DNS_MAX_NAME + 1];
    size_t off = DNS_HDR_LEN;
    size_t after_name = decode_name(in, in_len, off,
                                    qname, sizeof qname);
    if (after_name == 0) return -1;
    if (after_name + 4 > in_len) return -1;
    uint16_t qtype  = ((uint16_t)in[after_name] << 8) | in[after_name + 1];
    uint16_t qclass = ((uint16_t)in[after_name + 2] << 8) | in[after_name + 3];
    if (qtype != DNS_TYPE_A || qclass != DNS_CLASS_IN) return -1;
    if (!domain_equal_ci(qname, expect_host)) return -3;
    off = after_name + 4;

    size_t   n = 0;
    uint32_t min_ttl = UINT32_MAX;
    for (uint16_t i = 0; i < ancount && n < ips_cap; i++) {
        char rname[DNS_MAX_NAME + 1];
        size_t after_rname = decode_name(in, in_len, off,
                                         rname, sizeof rname);
        if (after_rname == 0) return -1;
        if (after_rname + 10 > in_len) return -1;
        uint16_t rtype  = ((uint16_t)in[after_rname]     << 8) | in[after_rname + 1];
        uint16_t rclass = ((uint16_t)in[after_rname + 2] << 8) | in[after_rname + 3];
        uint32_t ttl    = ((uint32_t)in[after_rname + 4] << 24)
                        | ((uint32_t)in[after_rname + 5] << 16)
                        | ((uint32_t)in[after_rname + 6] << 8)
                        |  (uint32_t)in[after_rname + 7];
        uint16_t rdlen  = ((uint16_t)in[after_rname + 8] << 8) | in[after_rname + 9];
        size_t   rdoff  = after_rname + 10;
        if (rdoff + rdlen > in_len) return -1;

        if (rtype == DNS_TYPE_A && rclass == DNS_CLASS_IN && rdlen == 4) {
            _snprintf(out_ips[n], 16, "%u.%u.%u.%u",
                      in[rdoff], in[rdoff + 1],
                      in[rdoff + 2], in[rdoff + 3]);
            if (ttl < min_ttl) min_ttl = ttl;
            n++;
        }
        // CNAMEs we just skip; a normal resolver follows them and
        // includes the target A records in the same answer section,
        // which is what we care about. If there are ONLY CNAMEs and
        // no terminal As, n stays 0 and we'll report failure.
        off = rdoff + rdlen;
    }

    *out_count = n;
    if (out_min_ttl) *out_min_ttl = (min_ttl == UINT32_MAX) ? 0 : min_ttl;
    return 0;
}

int Dns_Intersect(const char (*set_a)[16], size_t n_a,
                  const char (*set_b)[16], size_t n_b,
                  char out_ip[16])
{
    for (size_t i = 0; i < n_a; i++) {
        for (size_t j = 0; j < n_b; j++) {
            if (strcmp(set_a[i], set_b[j]) == 0) {
                _snprintf(out_ip, 16, "%s", set_a[i]);
                return 0;
            }
        }
    }
    return -1;
}

// ===========================================================================
// Cache
// ===========================================================================
//
// Small fixed array indexed linearly. 32 entries easily covers the
// 6-sources-per-cycle traffic with room for KE-server override
// hostnames. Eviction: oldest expiring slot wins on insert collision.

#define DNS_CACHE_N     32
#define DNS_TTL_FLOOR   60U
#define DNS_TTL_CEIL    3600U

typedef struct {
    char     host[256];
    char     ip[16];
    uint64_t expires_ms_tickcount;  // GetTickCount64 basis
} DnsCacheEntry;

static CRITICAL_SECTION g_cache_cs;
static volatile LONG    g_cache_cs_ready = 0;
static DnsCacheEntry    g_cache[DNS_CACHE_N];

static void cache_cs_ensure(void)
{
    if (InterlockedCompareExchange(&g_cache_cs_ready, 1, 0) == 0) {
        InitializeCriticalSection(&g_cache_cs);
        g_cache_cs_ready = 2;
    }
    while (g_cache_cs_ready != 2) { Sleep(0); }
}

static int cache_lookup(const char *host, char out_ip[16])
{
    cache_cs_ensure();
    EnterCriticalSection(&g_cache_cs);
    int found = 0;
    uint64_t now = GetTickCount64();
    for (int i = 0; i < DNS_CACHE_N; i++) {
        if (g_cache[i].host[0] == 0) continue;
        if (now >= g_cache[i].expires_ms_tickcount) {
            // Lazy expiry: wipe and keep scanning.
            g_cache[i].host[0] = 0;
            continue;
        }
        if (strcmp(g_cache[i].host, host) == 0) {
            _snprintf(out_ip, 16, "%s", g_cache[i].ip);
            found = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_cache_cs);
    return found;
}

static void cache_insert(const char *host, const char *ip, uint32_t ttl)
{
    if (ttl < DNS_TTL_FLOOR) ttl = DNS_TTL_FLOOR;
    if (ttl > DNS_TTL_CEIL)  ttl = DNS_TTL_CEIL;

    cache_cs_ensure();
    EnterCriticalSection(&g_cache_cs);
    uint64_t now = GetTickCount64();
    int slot = -1;
    int updated = 0;
    uint64_t oldest = UINT64_MAX;
    int oldest_slot = 0;
    for (int i = 0; i < DNS_CACHE_N; i++) {
        if (g_cache[i].host[0] && strcmp(g_cache[i].host, host) == 0) {
            slot = i; updated = 1; break;
        }
        uint64_t exp = g_cache[i].host[0] ? g_cache[i].expires_ms_tickcount : 0;
        if (exp < oldest) { oldest = exp; oldest_slot = i; }
    }
    if (slot < 0) slot = oldest_slot;
    _snprintf(g_cache[slot].host, sizeof g_cache[slot].host, "%s", host);
    _snprintf(g_cache[slot].ip,   sizeof g_cache[slot].ip,   "%s", ip);
    g_cache[slot].expires_ms_tickcount = now + (uint64_t)ttl * 1000ULL;
    (void)updated;
    LeaveCriticalSection(&g_cache_cs);
}

void Dns_CacheClear(void)
{
    cache_cs_ensure();
    EnterCriticalSection(&g_cache_cs);
    memset(g_cache, 0, sizeof g_cache);
    LeaveCriticalSection(&g_cache_cs);
}

// ===========================================================================
// TLS / DoH transport
// ===========================================================================

#define DOH_CONNECT_TIMEOUT_MS   4000
#define DOH_IO_TIMEOUT_MS        5000
#define DOH_MAX_REPLY_BYTES      65536

static CRITICAL_SECTION g_tls_init_cs;
static volatile LONG    g_tls_init_cs_ready = 0;
static int              g_tls_init_done     = 0;
static mbedtls_entropy_context  g_entropy;
static mbedtls_ctr_drbg_context g_drbg;

static void tls_cs_ensure(void)
{
    if (InterlockedCompareExchange(&g_tls_init_cs_ready, 1, 0) == 0) {
        InitializeCriticalSection(&g_tls_init_cs);
        g_tls_init_cs_ready = 2;
    }
    while (g_tls_init_cs_ready != 2) { Sleep(0); }
}

static int tls_ensure_inited(void)
{
    tls_cs_ensure();
    EnterCriticalSection(&g_tls_init_cs);
    int rc = 0;
    if (!g_tls_init_done) {
        // psa_crypto_init is idempotent after first success; nts.c may
        // have already called it. We tolerate either outcome.
        (void)psa_crypto_init();
        mbedtls_entropy_init(&g_entropy);
        mbedtls_ctr_drbg_init(&g_drbg);
        static const char pers[] = "lunar-dns";
        rc = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                                   (const unsigned char *)pers, sizeof pers - 1);
        if (rc == 0) g_tls_init_done = 1;
    }
    LeaveCriticalSection(&g_tls_init_cs);
    return rc;
}

typedef struct { SOCKET s; } SockCtx;

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    SockCtx *c = (SockCtx *)ctx;
    int n = send(c->s, (const char *)buf, (int)len, 0);
    if (n < 0) {
        int e = WSAGetLastError();
        if (e == WSAETIMEDOUT) return MBEDTLS_ERR_SSL_TIMEOUT;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    SockCtx *c = (SockCtx *)ctx;
    int n = recv(c->s, (char *)buf, (int)len, 0);
    if (n < 0) {
        int e = WSAGetLastError();
        if (e == WSAETIMEDOUT) return MBEDTLS_ERR_SSL_TIMEOUT;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    if (n == 0) return MBEDTLS_ERR_SSL_CONN_EOF;
    return n;
}

// TCP connect to a literal IPv4:port with a hard timeout. DoH
// bootstrap needs this so we never depend on the OS resolver.
static SOCKET tcp_connect_ip(const char *ip_str, uint16_t port)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip_str, &sa.sin_addr) != 1) return INVALID_SOCKET;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    int cr = connect(s, (struct sockaddr *)&sa, (int)sizeof sa);
    if (cr != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(s); return INVALID_SOCKET;
    }
    fd_set wfd; FD_ZERO(&wfd); FD_SET(s, &wfd);
    struct timeval tv;
    tv.tv_sec  =  DOH_CONNECT_TIMEOUT_MS / 1000;
    tv.tv_usec = (DOH_CONNECT_TIMEOUT_MS % 1000) * 1000;
    int sel = select(0, NULL, &wfd, NULL, &tv);
    if (sel <= 0) { closesocket(s); return INVALID_SOCKET; }
    int err = 0; int errlen = sizeof err;
    getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &errlen);
    if (err != 0) { closesocket(s); return INVALID_SOCKET; }

    nb = 0; ioctlsocket(s, FIONBIO, &nb);
    DWORD tmo = DOH_IO_TIMEOUT_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof tmo);
    return s;
}

static int verify_spki_pin(mbedtls_ssl_context *ssl, const uint8_t pin[32])
{
    const mbedtls_x509_crt *crt = mbedtls_ssl_get_peer_cert(ssl);
    if (crt == NULL) return -1;
    if (crt->pk_raw.p == NULL || crt->pk_raw.len == 0) return -1;
    uint8_t got[32];
    if (mbedtls_sha256(crt->pk_raw.p, crt->pk_raw.len, got, 0) != 0) return -1;
    int diff = mbedtls_ct_memcmp(got, pin, 32);
    mbedtls_platform_zeroize(got, sizeof got);
    return diff == 0 ? 0 : -1;
}

// Execute a single DoH POST /dns-query against one pinned resolver IP.
// Returns 0 on success with up to ips_cap A records in out_ips and
// the smallest TTL in out_min_ttl. On any failure (connect, TLS, pin
// mismatch, HTTP error, DNS error), returns -1.
static int doh_query_one(const DnsResolver *r, const char *ip_str,
                         const char *host, uint16_t qid,
                         char (*out_ips)[16], size_t ips_cap,
                         size_t *out_count, uint32_t *out_min_ttl)
{
    int      rc = -1;
    SOCKET   s  = INVALID_SOCKET;
    SockCtx  sc;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config  conf;
    int ssl_inited = 0, conf_inited = 0;

    if (tls_ensure_inited() != 0) return -1;

    s = tcp_connect_ip(ip_str, 443);
    if (s == INVALID_SOCKET) return -1;
    sc.s = s;

    mbedtls_ssl_init(&ssl);          ssl_inited = 1;
    mbedtls_ssl_config_init(&conf);  conf_inited = 1;

    if (mbedtls_ssl_config_defaults(&conf,
                                    MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) goto cleanup;
    mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);  // pin is the trust
    mbedtls_ssl_conf_ca_chain(&conf, NULL, NULL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &g_drbg);

    static const char *alpn_list[] = { "http/1.1", NULL };
    if (mbedtls_ssl_conf_alpn_protocols(&conf, alpn_list) != 0) goto cleanup;

    if (mbedtls_ssl_setup(&ssl, &conf) != 0) goto cleanup;
    if (mbedtls_ssl_set_hostname(&ssl, r->hostname) != 0) goto cleanup;
    mbedtls_ssl_set_bio(&ssl, &sc, bio_send, bio_recv, NULL);

    for (;;) {
        int hr = mbedtls_ssl_handshake(&ssl);
        if (hr == 0) break;
        if (hr == MBEDTLS_ERR_SSL_WANT_READ || hr == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        goto cleanup;
    }
    if (verify_spki_pin(&ssl, r->spki_pin) != 0) goto cleanup;

    // Build DNS query.
    uint8_t dns_q[512];
    size_t  dns_q_len = 0;
    if (Dns_BuildQueryA(host, qid, dns_q, sizeof dns_q, &dns_q_len) != 0) goto cleanup;

    // HTTP POST /dns-query HTTP/1.1
    char req_hdr[512];
    int  hlen = _snprintf(req_hdr, sizeof req_hdr,
        "POST /dns-query HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Accept: application/dns-message\r\n"
        "Content-Type: application/dns-message\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "\r\n",
        r->hostname, (unsigned)dns_q_len);
    if (hlen <= 0 || (size_t)hlen >= sizeof req_hdr) goto cleanup;

    {
        size_t sent = 0;
        while (sent < (size_t)hlen) {
            int wr = mbedtls_ssl_write(&ssl, (const unsigned char *)req_hdr + sent,
                                       (size_t)hlen - sent);
            if (wr == MBEDTLS_ERR_SSL_WANT_READ || wr == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (wr <= 0) goto cleanup;
            sent += (size_t)wr;
        }
        sent = 0;
        while (sent < dns_q_len) {
            int wr = mbedtls_ssl_write(&ssl, dns_q + sent, dns_q_len - sent);
            if (wr == MBEDTLS_ERR_SSL_WANT_READ || wr == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (wr <= 0) goto cleanup;
            sent += (size_t)wr;
        }
    }

    // Drain reply.
    static uint8_t reply[DOH_MAX_REPLY_BYTES];
    size_t         reply_len = 0;
    for (;;) {
        int rd = mbedtls_ssl_read(&ssl, reply + reply_len,
                                  sizeof reply - reply_len);
        if (rd == MBEDTLS_ERR_SSL_WANT_READ || rd == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (rd == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
        if (rd <= 0) {
            if ((rd == MBEDTLS_ERR_SSL_CONN_EOF || rd == 0) && reply_len > 0) break;
            goto cleanup;
        }
        reply_len += (size_t)rd;
        if (reply_len >= sizeof reply) goto cleanup;
    }

    // Parse HTTP: status line, headers, body. We need Content-Length
    // or Connection:close + EOF; most DoH servers do the latter so we
    // simply locate the end of headers and take everything after.
    if (reply_len < 16) goto cleanup;
    if (memcmp(reply, "HTTP/1.1 200", 12) != 0 &&
        memcmp(reply, "HTTP/1.0 200", 12) != 0) goto cleanup;

    // Hand-roll a search for "\r\n\r\n"; memmem isn't in msvcrt.
    size_t hdr_end = 0;
    for (size_t i = 0; i + 3 < reply_len; i++) {
        if (reply[i]=='\r' && reply[i+1]=='\n' &&
            reply[i+2]=='\r' && reply[i+3]=='\n') {
            hdr_end = i + 4; break;
        }
    }
    if (hdr_end == 0) goto cleanup;
    const uint8_t *body = reply + hdr_end;
    size_t body_len = reply_len - hdr_end;
    if (body_len < DNS_HDR_LEN) goto cleanup;

    size_t   n = 0;
    uint32_t min_ttl = 0;
    int pr = Dns_ParseResponseA(body, body_len, qid, host,
                                out_ips, ips_cap, &n, &min_ttl);
    if (pr != 0 || n == 0) goto cleanup;

    *out_count   = n;
    if (out_min_ttl) *out_min_ttl = min_ttl;
    rc = 0;

    mbedtls_ssl_close_notify(&ssl);

cleanup:
    if (ssl_inited)  mbedtls_ssl_free(&ssl);
    if (conf_inited) mbedtls_ssl_config_free(&conf);
    if (s != INVALID_SOCKET) closesocket(s);
    return rc;
}

// Try the resolver's primary IP; on failure, try its secondary (if
// any). Same SPKI pin covers both IPs because they share the same
// leaf cert (anycast).
static int doh_query_resolver(const DnsResolver *r, const char *host,
                              uint16_t qid,
                              char (*out_ips)[16], size_t ips_cap,
                              size_t *out_count, uint32_t *out_min_ttl)
{
    if (doh_query_one(r, r->ip_primary, host, qid,
                      out_ips, ips_cap, out_count, out_min_ttl) == 0) {
        return 0;
    }
    if (r->ip_secondary &&
        doh_query_one(r, r->ip_secondary, host, qid,
                      out_ips, ips_cap, out_count, out_min_ttl) == 0) {
        return 0;
    }
    return -1;
}

// ===========================================================================
// Dns_Resolve -- public orchestrator
// ===========================================================================

#define DNS_ANSWERS_MAX 16

int Dns_Resolve(const char *host, char out_ip[16])
{
    if (!host || !out_ip) return -1;
    size_t hlen = strlen(host);
    if (hlen == 0 || hlen > 255) return -1;

    if (cache_lookup(host, out_ip)) return 0;

    const DnsResolver *picked[2] = { NULL, NULL };
    size_t got = Dns_PickResolvers(picked, 2);
    if (got < 2) {
        // Cannot satisfy the 2-of-N agreement requirement with < 2
        // enabled resolvers. Fail closed; there is no safe fallback.
        Log_Append("dns: cannot resolve %s -- need 2 enabled DoH resolvers, have %zu",
                   host, got);
        return -1;
    }

    // A fresh transaction ID per query, per resolver. DoH doesn't
    // technically require it (the TLS channel already defeats off-
    // path spoofing), but it helps us reject cross-wired responses.
    uint16_t qid_a = 0, qid_b = 0;
    BCryptGenRandom(NULL, (PUCHAR)&qid_a, sizeof qid_a,
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    BCryptGenRandom(NULL, (PUCHAR)&qid_b, sizeof qid_b,
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG);

    char  ips_a[DNS_ANSWERS_MAX][16];
    char  ips_b[DNS_ANSWERS_MAX][16];
    size_t n_a = 0, n_b = 0;
    uint32_t ttl_a = 0, ttl_b = 0;

    int ra = doh_query_resolver(picked[0], host, qid_a,
                                ips_a, DNS_ANSWERS_MAX, &n_a, &ttl_a);
    int rb = doh_query_resolver(picked[1], host, qid_b,
                                ips_b, DNS_ANSWERS_MAX, &n_b, &ttl_b);
    if (ra != 0 || rb != 0 || n_a == 0 || n_b == 0) {
        Log_Append("dns: resolve %s FAIL (%s=%s, %s=%s)", host,
                   picked[0]->label, ra == 0 ? "ok" : "fail",
                   picked[1]->label, rb == 0 ? "ok" : "fail");
        return -1;
    }

    char agreed[16];
    if (Dns_Intersect(ips_a, n_a, ips_b, n_b, agreed) != 0) {
        // Both resolvers answered, but with disjoint A-record sets.
        // Exactly the 1-compromised-resolver case we designed the
        // agreement gate to detect. Fail closed; the cycle will INOP.
        Log_Append("dns: resolve %s DISAGREE (%s vs %s) -- refusing to trust",
                   host, picked[0]->label, picked[1]->label);
        return -1;
    }

    _snprintf(out_ip, 16, "%s", agreed);
    uint32_t ttl = (ttl_a < ttl_b) ? ttl_a : ttl_b;
    cache_insert(host, agreed, ttl);
    Log_Append("dns: resolve %s -> %s  (via %s+%s, ttl=%us)",
               host, agreed, picked[0]->label, picked[1]->label,
               (unsigned)ttl);
    return 0;
}
