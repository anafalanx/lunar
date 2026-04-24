// nts.c -- TLS 1.3 + NTS-KE transport with SPKI pinning.
//
// Flow per call:
//
//   1. Resolve host -> IP (getaddrinfo, AF_UNSPEC so we accept v4/v6).
//   2. TCP connect with a 5 s timeout.
//   3. TLS 1.3 handshake (mbedTLS) with ALPN "ntske/1".
//   4. Verify SPKI pin against the provider's embedded SHA-256.
//   5. Send the fixed NTS-KE client request (NtsKe_BuildClientRequest).
//   6. Drain inbound TLS records until peer-close or full response is
//      in our accumulator; parse with NtsKe_ParseResponse.
//   7. Export C2S and S2C AEAD keys via the TLS exporter
//      (RFC 8446 §7.5, RFC 8915 §5.1).
//   8. Graceful TLS close_notify, close socket.
//
// We hold no long-lived state beyond a lazy-initialised CTR_DRBG and
// a single psa_crypto_init() -- both shared across calls and thread-
// safe after the one-shot init completes. Each exchange builds a
// fresh SSL config + context + socket.

// Winsock2 must come before <windows.h> -- see ntp.c for the same pattern.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/sha256.h"
#include "mbedtls/error.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/constant_time.h"
#include "psa/crypto.h"

#include "nts.h"
#include "nts_ke.h"
#include "nts_ef.h"
#include "clock.h"
#include "dns.h"

// ---------------------------------------------------------------------------
// Provider pool
// ---------------------------------------------------------------------------
//
// Each entry carries a 32-byte SHA-256 of the leaf cert's
// SubjectPublicKeyInfo. A pin of all zeros is taken as "not yet
// populated" and the entry is silently skipped by Nts_PickProvider.
//
// To populate: run scripts/fetch_nts_spki_pins.py from the repo root;
// it prints a C literal suitable to paste below. Re-run whenever a
// provider rotates their certificate (they usually publish rotation
// schedules; track the pinned SPKI, not the cert expiry).
//
// Source list is deliberately small and operator-curated. NTS is
// still a young protocol and not every public NTP pool runs it.

static const NtsProvider kProviders[] = {
    {
        .host = "time.cloudflare.com",
        .port = 0,
        .label = "cloudflare",
        /* spki_pin (leaf expires 2027-02-17) = */
        .spki_pin = {
            0x48, 0xf9, 0x3d, 0x4f, 0x1e, 0xca, 0xf8, 0xe2,
            0x32, 0x3b, 0xc2, 0xe5, 0xbe, 0x01, 0x5b, 0x33,
            0x1d, 0x65, 0xcb, 0x8e, 0x40, 0xa1, 0xb2, 0x8b,
            0xe5, 0x0e, 0xb4, 0xba, 0x92, 0x30, 0xe7, 0x89 },
    },
    {
        .host = "nts.netnod.se",
        .port = 0,
        .label = "netnod",
        /* spki_pin (leaf expires 2026-06-21) = */
        .spki_pin = {
            0xbd, 0x44, 0xc5, 0x5c, 0xfd, 0x57, 0xe3, 0x8d,
            0xa3, 0xa6, 0xae, 0xa8, 0x0b, 0xf6, 0x5f, 0x9c,
            0x44, 0x1b, 0x63, 0xfb, 0xd8, 0x66, 0x7e, 0x05,
            0x7b, 0x0f, 0x11, 0x2f, 0xc3, 0x21, 0x1e, 0xfa },
    },
    {
        .host = "sth1.nts.netnod.se",
        .port = 0,
        .label = "netnod-sth1",
        /* spki_pin (leaf expires 2026-06-22) = */
        .spki_pin = {
            0x10, 0xfe, 0xf1, 0xa0, 0x92, 0xd2, 0xf6, 0x8b,
            0x06, 0x31, 0x49, 0xc8, 0xdf, 0x34, 0xbc, 0xb1,
            0x5a, 0x94, 0x9c, 0x5d, 0xf0, 0x30, 0xb3, 0xc2,
            0x2c, 0x9c, 0x49, 0xa3, 0x3c, 0x15, 0x14, 0xed },
    },
    {
        .host = "sth2.nts.netnod.se",
        .port = 0,
        .label = "netnod-sth2",
        /* spki_pin (leaf expires 2026-06-22) = */
        .spki_pin = {
            0xb7, 0x06, 0x7a, 0xce, 0x17, 0x9c, 0x64, 0x7a,
            0xca, 0x14, 0x2f, 0x15, 0xcd, 0x35, 0xc0, 0xc3,
            0x3a, 0xdb, 0x2b, 0xf0, 0x08, 0x83, 0x54, 0x62,
            0xc1, 0x11, 0xcb, 0xc8, 0x9d, 0xe3, 0xdd, 0xa1 },
    },
    /* NOTE: ptbtime{1..4}.ptb.de KE + cookies work, but PTB silently
     * drops NTS-sized (>48 byte) UDP packets at the NTP endpoint, so
     * the authenticated round trip never completes. We keep PTB as a
     * plain-SNTP core source (see src/ntp.c) and use System76's US
     * nodes here for additional NTS diversity. Verified 2026-04-21 via
     * scripts/probe_nts.py on ptbtime{1..4}. */
    {
        .host = "ohio.time.system76.com",
        .port = 0,
        .label = "system76-ohio",
        /* spki_pin (leaf expires 2026-06-03) = */
        .spki_pin = {
            0x99, 0x82, 0xb5, 0x2b, 0x5e, 0x67, 0x4b, 0xe1,
            0x94, 0x17, 0xb3, 0x8b, 0x7c, 0x8f, 0xd4, 0x3f,
            0x67, 0x23, 0x01, 0xd8, 0x6f, 0xe0, 0x46, 0xe2,
            0x92, 0xf2, 0x15, 0xc4, 0x0e, 0x24, 0x39, 0xb7 },
    },
    {
        .host = "virginia.time.system76.com",
        .port = 0,
        .label = "system76-virginia",
        /* spki_pin (leaf expires 2026-05-31) = */
        .spki_pin = {
            0xb7, 0xe3, 0x66, 0x9c, 0x5a, 0x2e, 0x88, 0x88,
            0x0f, 0xef, 0x64, 0x67, 0xb5, 0x04, 0xa7, 0x04,
            0x99, 0xca, 0x47, 0x7c, 0x5b, 0x47, 0xbc, 0x4d,
            0x43, 0x6e, 0x71, 0x5f, 0x7d, 0xfc, 0xd6, 0x72 },
    },
    {
        .host = "oregon.time.system76.com",
        .port = 0,
        .label = "system76-oregon",
        /* spki_pin (leaf expires 2026-05-30) = */
        .spki_pin = {
            0xc0, 0xfe, 0x53, 0x64, 0xcd, 0x9d, 0xb5, 0x92,
            0xfb, 0xe0, 0x43, 0xed, 0xb7, 0xaa, 0x45, 0x9b,
            0x0a, 0x20, 0xfe, 0x09, 0x9f, 0xf0, 0x5a, 0x1d,
            0x88, 0x26, 0xe9, 0xdf, 0x15, 0x31, 0x91, 0xc9 },
    },
    {
        .host = "paris.time.system76.com",
        .port = 0,
        .label = "system76-paris",
        /* spki_pin (leaf expires 2026-05-31) = */
        .spki_pin = {
            0x5b, 0x0e, 0xb2, 0x79, 0xe0, 0xb0, 0x3d, 0xbf,
            0xba, 0x68, 0x0c, 0x15, 0x7d, 0xb3, 0xbe, 0x89,
            0xab, 0x17, 0xa5, 0x2e, 0xca, 0xa5, 0xb1, 0xcc,
            0x9b, 0x7c, 0x30, 0xff, 0x1a, 0x82, 0x20, 0x74 },
    },
    {
        .host = "nts.time.nl",
        .port = 0,
        .label = "sidn",
        /* spki_pin (leaf expires 2026-05-30) = */
        .spki_pin = {
            0x62, 0x13, 0x4a, 0x45, 0x9a, 0xd5, 0x3a, 0xb7,
            0x01, 0x55, 0x83, 0xb0, 0x78, 0x3b, 0xfd, 0x94,
            0x7e, 0xdd, 0x9e, 0xdf, 0xf1, 0x34, 0xd8, 0xc2,
            0xcf, 0xf7, 0x1a, 0x0c, 0x86, 0x93, 0x78, 0x43 },
    },
};

#define NTS_PROVIDER_COUNT (sizeof kProviders / sizeof kProviders[0])

static_assert(sizeof (((NtsProvider *)0)->spki_pin) == 32,
              "NTS SPKI pin must be a SHA-256 digest");
static_assert(NTS_PROVIDER_COUNT >= 2,
              "NTS provider pool must support distinct authenticated anchors");

const NtsProvider *Nts_Pool(size_t *out_len)
{
    if (out_len) *out_len = NTS_PROVIDER_COUNT;
    return kProviders;
}

static int pin_is_zero(const uint8_t pin[32])
{
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= pin[i];
    return acc == 0;
}

const NtsProvider *Nts_PickProvider(void)
{
    size_t n = NTS_PROVIDER_COUNT;
    size_t enabled[NTS_PROVIDER_COUNT];
    size_t n_enabled = 0;
    for (size_t i = 0; i < n; i++) {
        if (!pin_is_zero(kProviders[i].spki_pin)) enabled[n_enabled++] = i;
    }
    if (n_enabled == 0) return NULL;

    uint32_t r = 0;
    if (BCryptGenRandom(NULL, (PUCHAR)&r, sizeof r,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return NULL;
    }
    return &kProviders[enabled[r % n_enabled]];
}

size_t Nts_PickProviders(const NtsProvider **out, size_t n_want)
{
    if (!out || n_want == 0) return 0;
    size_t n = NTS_PROVIDER_COUNT;
    size_t enabled[NTS_PROVIDER_COUNT];
    size_t n_enabled = 0;
    for (size_t i = 0; i < n; i++) {
        if (!pin_is_zero(kProviders[i].spki_pin)) enabled[n_enabled++] = i;
    }
    if (n_enabled == 0) return 0;
    if (n_want > n_enabled) n_want = n_enabled;

    // Fisher-Yates partial shuffle over the enabled indices using
    // BCryptGenRandom for each draw. n is small (<= pool size), so
    // this is trivially constant-work and uniform.
    for (size_t i = 0; i < n_want; i++) {
        uint32_t r = 0;
        if (BCryptGenRandom(NULL, (PUCHAR)&r, sizeof r,
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            return i;   // partial result; caller decides what to do
        }
        size_t j = i + (size_t)(r % (uint32_t)(n_enabled - i));
        size_t tmp = enabled[i];
        enabled[i] = enabled[j];
        enabled[j] = tmp;
        out[i] = &kProviders[enabled[i]];
    }
    return n_want;
}

// ---------------------------------------------------------------------------
// One-shot RNG / PSA init
// ---------------------------------------------------------------------------

static CRITICAL_SECTION g_init_cs;
static volatile LONG    g_init_cs_ready = 0;
static int              g_init_done     = 0;
static mbedtls_entropy_context  g_entropy;
static mbedtls_ctr_drbg_context g_drbg;

static void ensure_cs(void)
{
    if (InterlockedCompareExchange(&g_init_cs_ready, 1, 0) == 0) {
        InitializeCriticalSection(&g_init_cs);
        g_init_cs_ready = 2;
    }
    while (g_init_cs_ready != 2) { Sleep(0); }
}

static int ensure_inited(void)
{
    ensure_cs();
    EnterCriticalSection(&g_init_cs);
    int rc = 0;
    if (!g_init_done) {
        if (psa_crypto_init() != PSA_SUCCESS) { rc = -1; goto out; }
        mbedtls_entropy_init(&g_entropy);
        mbedtls_ctr_drbg_init(&g_drbg);
        static const char pers[] = "lunar-nts";
        rc = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                                   (const unsigned char *)pers, sizeof pers - 1);
        if (rc != 0) goto out;
        g_init_done = 1;
    }
out:
    LeaveCriticalSection(&g_init_cs);
    return rc;
}

// ---------------------------------------------------------------------------
// Socket BIO
// ---------------------------------------------------------------------------

typedef struct {
    SOCKET s;
} SockCtx;

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
    if (n == 0) {
        // Peer closed the TCP connection. For TLS this is distinct
        // from a clean close_notify; surface as CONN_EOF so the caller
        // can decide whether that's legitimate (post-NTS-KE reply) or
        // an error (mid-handshake truncation).
        return MBEDTLS_ERR_SSL_CONN_EOF;
    }
    return n;
}

// ---------------------------------------------------------------------------
// TCP connect with a hard timeout
// ---------------------------------------------------------------------------

#define NTS_CONNECT_TIMEOUT_MS   5000
#define NTS_IO_TIMEOUT_MS        5000
#define NTS_MAX_REPLY_BYTES      16384

static SOCKET tcp_connect(const char *host, uint16_t port)
{
    // Resolve via DoH (pinned, 2-of-N agreement). No getaddrinfo
    // fallback: an attacker able to forge plain DNS could redirect
    // our NTS-KE TLS session to a host whose leaf they control.
    // SPKI pinning would still refuse that session, but failing
    // BEFORE we connect is both cheaper and clearer in logs.
    char ip[16];
    if (Dns_Resolve(host, ip) != 0) return INVALID_SOCKET;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) return INVALID_SOCKET;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    // Non-blocking connect with select() for a hard timeout.
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    int cr = connect(s, (struct sockaddr *)&sa, (int)sizeof sa);
    if (cr != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(s); return INVALID_SOCKET;
    }

    fd_set wfd; FD_ZERO(&wfd); FD_SET(s, &wfd);
    struct timeval tv;
    tv.tv_sec  =  NTS_CONNECT_TIMEOUT_MS / 1000;
    tv.tv_usec = (NTS_CONNECT_TIMEOUT_MS % 1000) * 1000;
    int sel = select(0, NULL, &wfd, NULL, &tv);
    if (sel <= 0) { closesocket(s); return INVALID_SOCKET; }

    int err = 0; int errlen = sizeof err;
    getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &errlen);
    if (err != 0) { closesocket(s); return INVALID_SOCKET; }

    nb = 0; ioctlsocket(s, FIONBIO, &nb);
    DWORD tmo = NTS_IO_TIMEOUT_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof tmo);
    return s;
}

// ---------------------------------------------------------------------------
// SPKI pin verification
// ---------------------------------------------------------------------------
//
// Compute SHA-256 over the DER-encoded SubjectPublicKeyInfo of the
// leaf certificate, compare constant-time against the pinned value.
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

// ---------------------------------------------------------------------------
// Nts_DoKe
// ---------------------------------------------------------------------------

int Nts_DoKe(const NtsProvider *p, NtsKeResult *out)
{
    if (p == NULL || out == NULL) return -1;
    memset(out, 0, sizeof *out);

    // A provider without a real SPKI pin is never contacted, so we
    // never even open a socket to a host whose key we cannot verify.
    if (pin_is_zero(p->spki_pin)) return -1;

    if (ensure_inited() != 0) return -1;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;

    int      rc = -1;
    SOCKET   s = INVALID_SOCKET;
    SockCtx  sc;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config  conf;
    int ssl_inited = 0, conf_inited = 0;

    uint16_t port = p->port ? p->port : 4460;
    s = tcp_connect(p->host, port);
    if (s == INVALID_SOCKET) goto cleanup;
    sc.s = s;

    mbedtls_ssl_init(&ssl);          ssl_inited = 1;
    mbedtls_ssl_config_init(&conf);  conf_inited = 1;

    if (mbedtls_ssl_config_defaults(&conf,
                                    MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) goto cleanup;

    // TLS 1.3 only.
    mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_3);

    // CA validation disabled -- we rely on the SPKI pin, which we
    // check manually post-handshake. The server still sends its
    // certificate chain; mbedTLS parses it and mbedtls_ssl_get_peer_cert
    // hands us the leaf.
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_ca_chain(&conf, NULL, NULL);

    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &g_drbg);

    // ALPN: exactly "ntske/1" (RFC 8915 §4).
    static const char *alpn_list[] = { "ntske/1", NULL };
    if (mbedtls_ssl_conf_alpn_protocols(&conf, alpn_list) != 0) goto cleanup;

    if (mbedtls_ssl_setup(&ssl, &conf) != 0) goto cleanup;
    if (mbedtls_ssl_set_hostname(&ssl, p->host) != 0) goto cleanup;   // SNI
    mbedtls_ssl_set_bio(&ssl, &sc, bio_send, bio_recv, NULL);

    // Handshake.
    for (;;) {
        int hr = mbedtls_ssl_handshake(&ssl);
        if (hr == 0) break;
        if (hr == MBEDTLS_ERR_SSL_WANT_READ || hr == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        goto cleanup;
    }

    // ALPN check.
    {
        const char *neg = mbedtls_ssl_get_alpn_protocol(&ssl);
        if (neg == NULL || strcmp(neg, "ntske/1") != 0) goto cleanup;
    }

    // SPKI pin.
    if (verify_spki_pin(&ssl, p->spki_pin) != 0) goto cleanup;

    // Send NTS-KE request.
    {
        uint8_t req[32];
        size_t  rlen = NtsKe_BuildClientRequest(req, sizeof req);
        if (rlen == 0) goto cleanup;
        size_t sent = 0;
        while (sent < rlen) {
            int wr = mbedtls_ssl_write(&ssl, req + sent, rlen - sent);
            if (wr == MBEDTLS_ERR_SSL_WANT_READ || wr == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (wr <= 0) goto cleanup;
            sent += (size_t)wr;
        }
    }

    // Drain reply.
    uint8_t reply[NTS_MAX_REPLY_BYTES];
    size_t  reply_len = 0;
    for (;;) {
        int rd = mbedtls_ssl_read(&ssl, reply + reply_len,
                                  sizeof reply - reply_len);
        if (rd == MBEDTLS_ERR_SSL_WANT_READ || rd == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (rd == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
        if (rd <= 0) {
            // Servers sometimes drop TCP after the reply without
            // sending close_notify; treat a clean CONN_EOF or 0 as
            // end-of-message if we already accumulated something.
            if ((rd == MBEDTLS_ERR_SSL_CONN_EOF || rd == 0) && reply_len > 0) break;
            goto cleanup;
        }
        reply_len += (size_t)rd;
        if (reply_len >= sizeof reply) goto cleanup;  // ceiling hit
    }

    // Parse.
    NtsKeResponse resp;
    if (NtsKe_ParseResponse(reply, reply_len, &resp) != 1) goto cleanup;

    // Export AEAD keys per RFC 8915 §5.1.
    static const char kExporterLabel[] = "EXPORTER-network-time-security";
    {
        // Context for C2S: [proto u16][aead u16][S-bit u8]
        uint8_t ctx_c2s[5] = { 0x00, 0x00, 0x00, 0x0f, 0x00 };
        uint8_t ctx_s2c[5] = { 0x00, 0x00, 0x00, 0x0f, 0x01 };
        if (mbedtls_ssl_export_keying_material(&ssl,
                out->c2s_key, 32,
                kExporterLabel, sizeof kExporterLabel - 1,
                ctx_c2s, sizeof ctx_c2s, 1) != 0) goto cleanup;
        if (mbedtls_ssl_export_keying_material(&ssl,
                out->s2c_key, 32,
                kExporterLabel, sizeof kExporterLabel - 1,
                ctx_s2c, sizeof ctx_s2c, 1) != 0) goto cleanup;
    }

    // Populate the result.
    out->cookie_count = resp.cookie_count;
    for (size_t i = 0; i < resp.cookie_count; i++) {
        out->cookie_len[i] = resp.cookie_len[i];
        memcpy(out->cookies[i], resp.cookies[i], resp.cookie_len[i]);
    }
    memcpy(out->ntp_host, resp.ntp_host, sizeof out->ntp_host);
    out->ntp_port = resp.ntp_port;
    out->ok = 1;
    rc = 0;

    // Graceful TLS shutdown -- ignore failures; we already have what
    // we came for.
    mbedtls_ssl_close_notify(&ssl);

cleanup:
    if (ssl_inited)  mbedtls_ssl_free(&ssl);
    if (conf_inited) mbedtls_ssl_config_free(&conf);
    if (s != INVALID_SOCKET) closesocket(s);
    WSACleanup();
    if (rc != 0) {
        // Wipe any key material we may have written before failing.
        mbedtls_platform_zeroize(out, sizeof *out);
    }
    return rc;
}

// ---------------------------------------------------------------------------
// Nts_FetchSample -- KE + one authenticated SNTP round trip
// ---------------------------------------------------------------------------

#define NTP_EPOCH_DELTA_S  2208988800ULL
#define NTS_UDP_TIMEOUT_MS 3000
#define NTS_UDP_MAX_PKT    1500

static int parse_sntp_reply(const uint8_t *pkt, size_t pkt_len,
                            int64_t *out_t2_ms, int64_t *out_t3_ms)
{
    if (pkt_len < 48) return 0;
    uint8_t li      = (pkt[0] >> 6) & 0x3;
    uint8_t vn      = (pkt[0] >> 3) & 0x7;
    uint8_t mode    =  pkt[0]       & 0x7;
    uint8_t stratum =  pkt[1];
    if (li == 3)                       return 0;
    if (vn != 3 && vn != 4)            return 0;
    if (mode != 4)                     return 0;
    if (stratum == 0 || stratum >= 16) return 0;

    uint32_t secBE, fracBE;
    memcpy(&secBE,  pkt + 32, 4); memcpy(&fracBE, pkt + 36, 4);
    uint32_t t2_s = ntohl(secBE), t2_frac = ntohl(fracBE);
    memcpy(&secBE,  pkt + 40, 4); memcpy(&fracBE, pkt + 44, 4);
    uint32_t t3_s = ntohl(secBE), t3_frac = ntohl(fracBE);
    if (t2_s == 0 || t3_s == 0) return 0;

    *out_t2_ms = ((int64_t)t2_s - (int64_t)NTP_EPOCH_DELTA_S) * 1000
                 + (int64_t)(((uint64_t)t2_frac * 1000ULL) >> 32);
    *out_t3_ms = ((int64_t)t3_s - (int64_t)NTP_EPOCH_DELTA_S) * 1000
                 + (int64_t)(((uint64_t)t3_frac * 1000ULL) >> 32);
    return 1;
}

int Nts_FetchSample(const NtsProvider *p,
                    int64_t  *out_ntpUtcMs,
                    int64_t  *out_qpcAtT4,
                    uint32_t *out_rttMs)
{
    if (p == NULL || out_ntpUtcMs == NULL || out_qpcAtT4 == NULL
        || out_rttMs == NULL) return 0;
    *out_ntpUtcMs = 0; *out_qpcAtT4 = 0; *out_rttMs = 0;

    // Phase 1: NTS-KE. Nts_DoKe runs its own WSAStartup/Cleanup pair
    // and clears `ke` on failure, so we just inherit its verdict.
    NtsKeResult ke;
    if (Nts_DoKe(p, &ke) != 0 || !ke.ok || ke.cookie_count == 0) return 0;

    const char *host = ke.ntp_host[0] ? ke.ntp_host : p->host;
    uint16_t    port = ke.ntp_port    ? ke.ntp_port : 123;

    // Phase 2: build the authenticated SNTP request.
    uint8_t hdr[48];
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 0x23;   // LI=0, VN=4, Mode=3 (client)

    uint8_t uid[NTS_UNIQUE_ID_LEN], nonce[NTS_NONCE_LEN];
    if (BCryptGenRandom(NULL, uid, sizeof uid,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) goto fail;
    if (BCryptGenRandom(NULL, nonce, sizeof nonce,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) goto fail;

    uint8_t pkt[NTS_UDP_MAX_PKT];
    size_t  pkt_len = 0;
    if (NtsEf_BuildRequest(hdr, uid, nonce,
                           ke.cookies[0], ke.cookie_len[0],
                           0 /* no placeholder cookies */,
                           ke.c2s_key,
                           pkt, sizeof pkt, &pkt_len) != 0) goto fail;

    // Phase 3: UDP exchange with QPC-bracketed timing.
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) goto fail;

    int           rc = 0;
    SOCKET        s  = INVALID_SOCKET;

    // DoH resolution: same no-fallback policy as tcp_connect above.
    // The host here is either the NTS-KE host or an override the
    // server sent via NTSKE_REC_NTPV4_SERVER. Either way, we MUST NOT
    // trust OS DNS to tell us where to send our authenticated SNTP
    // packet.
    char ip[16];
    if (Dns_Resolve(host, ip) != 0) goto udp_done;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) goto udp_done;

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) goto udp_done;

    DWORD tmo = NTS_UDP_TIMEOUT_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof tmo);

    int64_t qpcT1 = Clock_Qpc();
    int sent = sendto(s, (const char *)pkt, (int)pkt_len, 0,
                      (struct sockaddr *)&sa, (int)sizeof sa);
    if (sent != (int)pkt_len) goto udp_done;

    uint8_t reply[NTS_UDP_MAX_PKT];
    int recvd = recv(s, (char *)reply, (int)sizeof reply, 0);
    int64_t qpcT4 = Clock_Qpc();
    if (recvd <= 0) goto udp_done;

    // Phase 4: authenticate + parse. ParseResponse does the SIV check,
    // enforces the Authenticator is the final extension, and matches
    // our UID in constant time. If anything's off we drop the sample.
    uint8_t new_cookies[NTSKE_MAX_COOKIES][NTSKE_MAX_COOKIE_LEN];
    size_t  new_lens[NTSKE_MAX_COOKIES];
    size_t  new_cnt = 0;
    if (NtsEf_ParseResponse(reply, (size_t)recvd, uid, ke.s2c_key,
                            new_cookies, new_lens, &new_cnt) != 0) goto udp_done;

    int64_t t2_ms = 0, t3_ms = 0;
    if (!parse_sntp_reply(reply, (size_t)recvd, &t2_ms, &t3_ms)) goto udp_done;

    int64_t qpcFreq = Clock_QpcFreq();
    if (qpcFreq <= 0) goto udp_done;
    int64_t rtt = ((qpcT4 - qpcT1) * 1000LL + qpcFreq / 2) / qpcFreq;
    if (rtt < 0) rtt = 0;
    int64_t serverProc = t3_ms - t2_ms;
    if (serverProc < 0) serverProc = 0;
    int64_t netRtt = rtt - serverProc;
    if (netRtt < 0) netRtt = 0;

    *out_ntpUtcMs = t3_ms + netRtt / 2;
    *out_qpcAtT4  = qpcT4;
    *out_rttMs    = (uint32_t)(rtt > 0x7fffffff ? 0x7fffffff : rtt);
    rc = 1;

    // Newly harvested cookies are discarded in this stateless model;
    // the next cycle performs a fresh KE and gets new ones. See
    // nts.h commentary. Zeroise any key material we've touched.
    mbedtls_platform_zeroize(new_cookies, sizeof new_cookies);

udp_done:
    if (s != INVALID_SOCKET) closesocket(s);
    WSACleanup();
    mbedtls_platform_zeroize(uid,   sizeof uid);
    mbedtls_platform_zeroize(nonce, sizeof nonce);
    mbedtls_platform_zeroize(pkt,   sizeof pkt);
    mbedtls_platform_zeroize(&ke,   sizeof ke);
    return rc;

fail:
    mbedtls_platform_zeroize(&ke, sizeof ke);
    return 0;
}
