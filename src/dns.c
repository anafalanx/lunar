// dns.c -- DNS-over-HTTPS resolver with local SPKI enrollment and
// randomized enrolled-resolver failover. See dns.h for design rationale.

// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
// clang-format on

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>

#include "dns.h"
#include "logbuf.h"
#include "clock.h"
#include "pinned_tls.h"
#include "pin_store.h"

#include "mbedtls/ssl.h"

// ---------------------------------------------------------------------------
// Pinned resolver table
// ---------------------------------------------------------------------------
//
// Each resolver has hardcoded anycast IP(s) so bootstrap needs no DNS.
// These addresses are reachability hints, not trust anchors: first-run
// enrollment validates the resolver certificate through Windows/Web PKI
// and stores the leaf SPKI in the protected local pin store.

static const DnsResolver kResolvers[] = {
    {
        .hostname = "cloudflare-dns.com",
        .ip_primary = "1.1.1.1",
        .ip_secondary = "1.0.0.1",
        .label = "cloudflare",
        .operator_family = "cloudflare",
    },

    {
        .hostname = "dns.quad9.net",
        .ip_primary = "9.9.9.9",
        .ip_secondary = "149.112.112.112",
        .label = "quad9",
        .operator_family = "quad9",
    },

    {
        .hostname = "dns.google",
        .ip_primary = "8.8.8.8",
        .ip_secondary = "8.8.4.4",
        .label = "google",
        .operator_family = "google",
    },

    {
        .hostname = "dns.nextdns.io",
        .ip_primary = "45.90.28.0",
        .ip_secondary = NULL,
        .label = "nextdns",
        .operator_family = "nextdns",
    },

    {
        .hostname = "dns.mullvad.net",
        .ip_primary = "194.242.2.2",
        .ip_secondary = NULL,
        .label = "mullvad",
        .operator_family = "mullvad",
    },
};

#define DNS_POOL_SIZE (sizeof kResolvers / sizeof kResolvers[0])

static_assert(DNS_POOL_SIZE >= 2,
              "DoH resolver pool must keep random-provider diversity");

const DnsResolver *Dns_Pool(size_t *out_len)
{
    if (out_len) *out_len = DNS_POOL_SIZE;
    return kResolvers;
}

size_t Dns_PickResolvers(const DnsResolver **out, size_t n_want)
{
    if (!out || n_want == 0) return 0;
    size_t enabled[DNS_POOL_SIZE];
    size_t n_enabled = DNS_POOL_SIZE;
    for (size_t i = 0; i < DNS_POOL_SIZE; i++) enabled[i] = i;
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
// DoH transport
// ===========================================================================

#define DOH_CONNECT_TIMEOUT_MS   4000
#define DOH_IO_TIMEOUT_MS        5000
#define DOH_MAX_REPLY_BYTES      65536

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
    PinnedTls tls;
    uint8_t *reply = NULL;
    PinnedTls_Init(&tls);

    SOCKET s = tcp_connect_ip(ip_str, 443);
    if (s == INVALID_SOCKET) return -1;

    PinRecord pin;
    int have_pin = PinStore_GetPin(PIN_ENDPOINT_DOH, r->label, r->hostname,
                                   443, &pin);
    int renew_due = have_pin ? PinStore_ShouldRenew(&pin) : 1;
    int expired = have_pin ? PinStore_IsExpired(&pin) : 1;
    int allow_ca = !have_pin || renew_due || expired;
    if (!have_pin) {
        Log_Append("dns: %s first-run enrollment required for %s:%u",
                   r->label, r->hostname, 443u);
    } else if (expired) {
        Log_Append("dns: %s local pin expired (notAfter=%s); CA revalidation required",
                   r->label, pin.not_after);
    } else if (renew_due) {
        Log_Append("dns: %s scheduled CA renewal due (notAfter=%s, renewalDue=%lld)",
                   r->label, pin.not_after, (long long)pin.renewal_due_unix);
    }

    static const char *alpn_list[] = { "http/1.1", NULL };
    PinnedTlsOpenResult openInfo;
    if (PinnedTls_OpenEnrolled(&tls, s, r->hostname, alpn_list,
                               (have_pin && !expired) ? pin.spki : NULL,
                               allow_ca, renew_due && !expired, &openInfo) != 0) {
        if (openInfo.pin_mismatched && !allow_ca) {
            Log_Append("dns: %s pin mismatch before renewal window; endpoint rejected without CA refresh (peer_spki=%s stored_spki=%s)",
                       r->label, openInfo.peer_spki_hex, pin.spki_hex);
        } else if (openInfo.ca_attempted) {
            Log_Append("dns: %s CA validation rejected host=%s chain=0x%lx policy=0x%lx revocation=%s subject=\"%s\" issuer=\"%s\" spki=%s",
                       r->label, r->hostname,
                       (unsigned long)openInfo.cert.chain_error_status,
                       (unsigned long)openInfo.cert.policy_error,
                       openInfo.cert.revocation_offline ? "offline" :
                       (openInfo.cert.revocation_checked ? "checked" : "not-checked"),
                       openInfo.cert.subject, openInfo.cert.issuer,
                       openInfo.cert.spki_hex);
        }
        goto cleanup;
    }
    const char *neg = PinnedTls_NegotiatedAlpn(&tls);
    if (openInfo.ca_attempted) {
        Log_Append("dns: %s CA validation %s host=%s alpn=%s subject=\"%s\" issuer=\"%s\" notBefore=%s notAfter=%s spki=%s revocation=%s chain=0x%lx policy=0x%lx",
                   r->label, openInfo.ca_valid ? "accepted" : "failed-but-pin-still-valid",
                   r->hostname, neg ? neg : "(none)",
                   openInfo.cert.subject, openInfo.cert.issuer,
                   openInfo.cert.not_before, openInfo.cert.not_after,
                   openInfo.cert.spki_hex,
                   openInfo.cert.revocation_offline ? "offline" :
                   (openInfo.cert.revocation_checked ? "checked" : "not-checked"),
                   (unsigned long)openInfo.cert.chain_error_status,
                   (unsigned long)openInfo.cert.policy_error);
        if (openInfo.ca_valid) {
            const char *status = have_pin
                ? (expired ? "expired-renewal" :
                   (openInfo.pin_matched ? "scheduled-renewal" : "pin-rotation"))
                : "first-run-enrollment";
            PinStore_SavePin(PIN_ENDPOINT_DOH, r->label, r->hostname, 443,
                             r->operator_family,
                             openInfo.cert.spki_sha256,
                             openInfo.cert.spki_hex,
                             openInfo.cert.not_before,
                             openInfo.cert.not_after,
                             openInfo.cert.not_before_unix,
                             openInfo.cert.not_after_unix,
                             status);
        }
    } else if (openInfo.pin_matched) {
        Log_Append("dns: %s local pin match host=%s spki=%s notAfter=%s",
                   r->label, r->hostname, pin.spki_hex, pin.not_after);
    }
    mbedtls_ssl_context *ssl = PinnedTls_Ssl(&tls);

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
            int wr = mbedtls_ssl_write(ssl, (const unsigned char *)req_hdr + sent,
                                       (size_t)hlen - sent);
            if (wr == MBEDTLS_ERR_SSL_WANT_READ || wr == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (wr <= 0) goto cleanup;
            sent += (size_t)wr;
        }
        sent = 0;
        while (sent < dns_q_len) {
            int wr = mbedtls_ssl_write(ssl, dns_q + sent, dns_q_len - sent);
            if (wr == MBEDTLS_ERR_SSL_WANT_READ || wr == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (wr <= 0) goto cleanup;
            sent += (size_t)wr;
        }
    }

    // Drain reply.
    reply = (uint8_t *)malloc(DOH_MAX_REPLY_BYTES);
    if (!reply) goto cleanup;
    size_t reply_len = 0;
    for (;;) {
        int rd = mbedtls_ssl_read(ssl, reply + reply_len,
                                  DOH_MAX_REPLY_BYTES - reply_len);
        if (rd == MBEDTLS_ERR_SSL_WANT_READ || rd == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (rd == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
        if (rd <= 0) {
            if ((rd == MBEDTLS_ERR_SSL_CONN_EOF || rd == 0) && reply_len > 0) break;
            goto cleanup;
        }
        reply_len += (size_t)rd;
        if (reply_len >= DOH_MAX_REPLY_BYTES) goto cleanup;
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

    PinnedTls_CloseNotify(&tls);

cleanup:
    PinnedTls_Free(&tls);
    free(reply);
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

    // Shuffle the enabled pinned resolver pool and try it in that
    // random order. We still never fall back to plain DNS. If one
    // pinned DoH resolver is down, blocked, or has a transient
    // TLS/pin/query failure, we fall through to the next pinned
    // resolver in the shuffled list. A resolver that returns a
    // syntactically valid but malicious A set is still handled by the
    // downstream NTS SPKI pins, AEAD SNTP, and NTP concurrence gate.
    const DnsResolver *picked[DNS_POOL_SIZE] = { NULL };
    size_t got = Dns_PickResolvers(picked, DNS_POOL_SIZE);
    if (got < 1) {
        Log_Append("dns: cannot resolve %s -- no enabled DoH resolvers", host);
        return -1;
    }

    char     ips[DNS_ANSWERS_MAX][16];
    for (size_t i = 0; i < got; i++) {
        // Fresh QID per DoH query. DoH doesn't require it (the TLS
        // channel already defeats off-path spoofing) but it costs
        // nothing and lets us reject cross-wired responses from a
        // buggy resolver.
        uint16_t qid = 0;
        BCryptGenRandom(NULL, (PUCHAR)&qid, sizeof qid,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG);

        size_t   n_ips = 0;
        uint32_t ttl   = 0;
        if (doh_query_resolver(picked[i], host, qid,
                               ips, DNS_ANSWERS_MAX, &n_ips, &ttl) == 0 &&
            n_ips > 0) {
            // Take the first A record. Order is resolver-defined; for
            // geo-DNS it's typically already latency-sorted for our
            // vantage.
            _snprintf(out_ip, 16, "%s", ips[0]);
            cache_insert(host, ips[0], ttl);
            Log_Append("dns: resolve %s -> %s  (via %s, ttl=%us)",
                       host, ips[0], picked[i]->label, (unsigned)ttl);
            return 0;
        }

        Log_Append("dns: resolve %s failed via %s%s",
                   host, picked[i]->label,
                   (i + 1 < got) ? "; trying next pinned DoH resolver" : "");
    }

    Log_Append("dns: resolve %s FAIL (all %u pinned DoH resolvers failed)",
               host, (unsigned)got);
    return -1;
}
