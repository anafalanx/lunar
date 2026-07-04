// pinned_tls.c -- shared TLS 1.3 over SOCKET helper with SPKI pinning.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>

#include <stdint.h>
#include <string.h>

#include "pinned_tls.h"
#include "cert_verify_win.h"

#include "psa/crypto.h"
#include "mbedtls/sha256.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/constant_time.h"

static INIT_ONCE g_tls_init_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK tls_init_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    return psa_crypto_init() == PSA_SUCCESS;
}

static int tls_ensure_inited(void)
{
    return InitOnceExecuteOnce(&g_tls_init_once, tls_init_once, NULL, NULL) ? 0 : -1;
}

static int bcrypt_rng(void *ctx, unsigned char *out, size_t out_len)
{
    (void)ctx;
    while (out_len > 0) {
        ULONG chunk = (out_len > 1048576u) ? 1048576u : (ULONG)out_len;
        if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, out, chunk,
                                            BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
            return -1;
        }
        out += chunk;
        out_len -= chunk;
    }
    return 0;
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    PinnedTlsBio *bio = (PinnedTlsBio *)ctx;
    int n = send(bio->s, (const char *)buf, (int)len, 0);
    if (n < 0) {
        int e = WSAGetLastError();
        if (e == WSAETIMEDOUT) return MBEDTLS_ERR_SSL_TIMEOUT;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    PinnedTlsBio *bio = (PinnedTlsBio *)ctx;
    int n = recv(bio->s, (char *)buf, (int)len, 0);
    if (n < 0) {
        int e = WSAGetLastError();
        if (e == WSAETIMEDOUT) return MBEDTLS_ERR_SSL_TIMEOUT;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    if (n == 0) return MBEDTLS_ERR_SSL_CONN_EOF;
    return n;
}

int PinnedTls_PinIsZero(const uint8_t pin[32])
{
    if (!pin) return 1;
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= pin[i];
    return acc == 0;
}

void PinnedTls_Init(PinnedTls *tls)
{
    if (!tls) return;
    memset(tls, 0, sizeof *tls);
    tls->socket = INVALID_SOCKET;
    tls->bio.s  = INVALID_SOCKET;
}

static int peer_spki_sha256(mbedtls_ssl_context *ssl, uint8_t out[32])
{
    const mbedtls_x509_crt *crt = mbedtls_ssl_get_peer_cert(ssl);
    if (crt == NULL) return -1;
    if (crt->pk_raw.p == NULL || crt->pk_raw.len == 0) return -1;

    return mbedtls_sha256(crt->pk_raw.p, crt->pk_raw.len, out, 0) == 0 ? 0 : -1;
}

// Constant-time membership test of `got` against a set of pins. Every
// set member is compared regardless of earlier matches so the timing
// does not reveal which (or whether an early) pin matched.
static int spki_in_pin_set(const uint8_t got[32],
                           const uint8_t (*pins)[32], size_t pin_count)
{
    int matched = 0;
    for (size_t i = 0; i < pin_count; i++) {
        if (PinnedTls_PinIsZero(pins[i])) continue;
        matched |= (mbedtls_ct_memcmp(got, pins[i], 32) == 0);
    }
    return matched;
}

static void open_result_init(PinnedTlsOpenResult *result)
{
    if (!result) return;
    memset(result, 0, sizeof *result);
    CertVerifyWin_ResultInit(&result->cert);
}

static int do_handshake(PinnedTls *tls, SOCKET socket,
                        const char *hostname, const char **alpn)
{
    PinnedTls_Init(tls);
    tls->socket = socket;
    tls->bio.s  = socket;

    if (socket == INVALID_SOCKET || !hostname || !*hostname ||
        tls_ensure_inited() != 0) {
        PinnedTls_Free(tls);
        return -1;
    }

    mbedtls_ssl_init(&tls->ssl);          tls->ssl_inited = 1;
    mbedtls_ssl_config_init(&tls->conf);  tls->conf_inited = 1;

    if (mbedtls_ssl_config_defaults(&tls->conf,
                                    MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) goto fail;
    mbedtls_ssl_conf_min_tls_version(&tls->conf, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&tls->conf, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_ca_chain(&tls->conf, NULL, NULL);
    mbedtls_ssl_conf_rng(&tls->conf, bcrypt_rng, NULL);

    if (alpn && mbedtls_ssl_conf_alpn_protocols(&tls->conf, alpn) != 0) goto fail;
    if (mbedtls_ssl_setup(&tls->ssl, &tls->conf) != 0) goto fail;
    if (mbedtls_ssl_set_hostname(&tls->ssl, hostname) != 0) goto fail;
    mbedtls_ssl_set_bio(&tls->ssl, &tls->bio, bio_send, bio_recv, NULL);

    for (;;) {
        int hr = mbedtls_ssl_handshake(&tls->ssl);
        if (hr == 0) break;
        if (hr == MBEDTLS_ERR_SSL_WANT_READ || hr == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        goto fail;
    }

    return 0;

fail:
    PinnedTls_Free(tls);
    return -1;
}

int PinnedTls_Open(PinnedTls *tls, SOCKET socket,
                   const char *hostname,
                   const char **alpn,
                   const uint8_t spki_pin[32])
{
    PinnedTlsOpenResult result;
    return PinnedTls_OpenEnrolled(tls, socket, hostname, alpn, spki_pin,
                                  0, 0, &result);
}

int PinnedTls_OpenEnrolled(PinnedTls *tls, SOCKET socket,
                           const char *hostname,
                           const char **alpn,
                           const uint8_t spki_pin[32],
                           int allow_ca_enrollment,
                           int force_ca_refresh,
                           PinnedTlsOpenResult *result)
{
    const uint8_t (*pins)[32] = NULL;
    size_t pin_count = 0;
    if (spki_pin && !PinnedTls_PinIsZero(spki_pin)) {
        pins = (const uint8_t (*)[32])spki_pin;
        pin_count = 1;
    }
    return PinnedTls_OpenEnrolledSet(tls, socket, hostname, alpn,
                                     pins, pin_count,
                                     allow_ca_enrollment, force_ca_refresh,
                                     result);
}

int PinnedTls_OpenEnrolledSet(PinnedTls *tls, SOCKET socket,
                              const char *hostname,
                              const char **alpn,
                              const uint8_t (*spki_pins)[32],
                              size_t pin_count,
                              int allow_ca_enrollment,
                              int force_ca_refresh,
                              PinnedTlsOpenResult *result)
{
    if (!tls) {
        if (socket != INVALID_SOCKET) closesocket(socket);
        return -1;
    }
    open_result_init(result);
    if (do_handshake(tls, socket, hostname, alpn) != 0) return -1;

    uint8_t got[32];
    if (peer_spki_sha256(&tls->ssl, got) != 0) goto fail;
    if (result) {
        memcpy(result->peer_spki, got, 32);
        CertVerifyWin_Hex32(got, result->peer_spki_hex);
    }

    int pin_present = (spki_pins != NULL && pin_count > 0);
    int pin_matched = 0;
    if (result) result->pin_present = pin_present;
    if (pin_present && spki_in_pin_set(got, spki_pins, pin_count)) {
        pin_matched = 1;
        if (result) {
            result->pin_matched = 1;
            result->auth_mode = PINNED_TLS_AUTH_PIN;
        }
    } else if (pin_present && result) {
        result->pin_mismatched = 1;
    }

    if (pin_matched && !force_ca_refresh) {
        mbedtls_platform_zeroize(got, sizeof got);
        return 0;
    }

    if ((pin_matched && force_ca_refresh) || (!pin_matched && allow_ca_enrollment)) {
        CertVerifyWinResult cert;
        CertVerifyWin_ResultInit(&cert);
        if (result) result->ca_attempted = 1;
        if (CertVerifyWin_ValidateMbedtlsPeer(&tls->ssl, hostname, &cert)) {
            if (result) {
                result->ca_valid = 1;
                result->cert = cert;
                result->auth_mode = pin_matched ? PINNED_TLS_AUTH_PIN
                                                : PINNED_TLS_AUTH_CA;
            }
            mbedtls_platform_zeroize(got, sizeof got);
            return 0;
        }
        if (result) result->cert = cert;
        // A scheduled refresh failure does not break a still-matching pin.
        if (pin_matched) {
            mbedtls_platform_zeroize(got, sizeof got);
            return 0;
        }
    }

fail:
    mbedtls_platform_zeroize(got, sizeof got);
    PinnedTls_Free(tls);
    return -1;
}

mbedtls_ssl_context *PinnedTls_Ssl(PinnedTls *tls)
{
    return tls ? &tls->ssl : NULL;
}

const char *PinnedTls_NegotiatedAlpn(PinnedTls *tls)
{
    if (!tls || !tls->ssl_inited) return NULL;
    return mbedtls_ssl_get_alpn_protocol(&tls->ssl);
}

void PinnedTls_CloseNotify(PinnedTls *tls)
{
    if (tls && tls->ssl_inited) mbedtls_ssl_close_notify(&tls->ssl);
}

void PinnedTls_Free(PinnedTls *tls)
{
    if (!tls) return;
    if (tls->ssl_inited) {
        mbedtls_ssl_free(&tls->ssl);
        tls->ssl_inited = 0;
    }
    if (tls->conf_inited) {
        mbedtls_ssl_config_free(&tls->conf);
        tls->conf_inited = 0;
    }
    if (tls->socket != INVALID_SOCKET) {
        closesocket(tls->socket);
        tls->socket = INVALID_SOCKET;
        tls->bio.s = INVALID_SOCKET;
    }
}