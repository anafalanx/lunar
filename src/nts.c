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
    { "time.cloudflare.com", 0, "cloudflare",
      /* spki_pin = */ { 0 } },
    { "nts.netnod.se",       0, "netnod",
      /* spki_pin = */ { 0 } },
    { "ptbtime1.ptb.de",     0, "ptb1",
      /* spki_pin = */ { 0 } },
    { "nts.time.nl",         0, "sidn",
      /* spki_pin = */ { 0 } },
};

const NtsProvider *Nts_Pool(size_t *out_len)
{
    if (out_len) *out_len = sizeof kProviders / sizeof kProviders[0];
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
    size_t n = sizeof kProviders / sizeof kProviders[0];
    size_t enabled[sizeof kProviders / sizeof kProviders[0]];
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
    char portbuf[8];
    _snprintf(portbuf, sizeof portbuf, "%u", (unsigned)port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host, portbuf, &hints, &res) != 0 || res == NULL) {
        return INVALID_SOCKET;
    }

    SOCKET s = INVALID_SOCKET;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        // Non-blocking connect with select() for a hard timeout.
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        int cr = connect(s, ai->ai_addr, (int)ai->ai_addrlen);
        if (cr != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(s); s = INVALID_SOCKET; continue;
        }

        fd_set wfd; FD_ZERO(&wfd); FD_SET(s, &wfd);
        struct timeval tv;
        tv.tv_sec  =  NTS_CONNECT_TIMEOUT_MS / 1000;
        tv.tv_usec = (NTS_CONNECT_TIMEOUT_MS % 1000) * 1000;
        int sel = select(0, NULL, &wfd, NULL, &tv);
        if (sel <= 0) {
            closesocket(s); s = INVALID_SOCKET; continue;
        }

        // Check SO_ERROR for a completed-but-failed connect.
        int err = 0; int errlen = sizeof err;
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &errlen);
        if (err != 0) { closesocket(s); s = INVALID_SOCKET; continue; }

        // Restore blocking + set I/O timeouts.
        nb = 0; ioctlsocket(s, FIONBIO, &nb);
        DWORD tmo = NTS_IO_TIMEOUT_MS;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof tmo);
        break;
    }
    freeaddrinfo(res);
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
