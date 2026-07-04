// pinned_tls.h -- shared TLS 1.3 over SOCKET helper with SPKI pinning.

#ifndef LUNAR_PINNED_TLS_H
#define LUNAR_PINNED_TLS_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <stddef.h>
#include <stdint.h>

#include "mbedtls/ssl.h"
#include "cert_verify_win.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    SOCKET s;
} PinnedTlsBio;

typedef struct {
    SOCKET              socket;
    PinnedTlsBio        bio;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config  conf;
    int                 ssl_inited;
    int                 conf_inited;
} PinnedTls;

typedef enum {
    PINNED_TLS_AUTH_NONE = 0,
    PINNED_TLS_AUTH_PIN  = 1,
    PINNED_TLS_AUTH_CA   = 2,
} PinnedTlsAuthMode;

typedef struct {
    PinnedTlsAuthMode auth_mode;
    int               pin_present;
    int               pin_matched;
    int               pin_mismatched;
    int               ca_attempted;
    int               ca_valid;
    uint8_t           peer_spki[32];
    char              peer_spki_hex[65];
    CertVerifyWinResult cert;
} PinnedTlsOpenResult;

int  PinnedTls_PinIsZero(const uint8_t pin[32]);
void PinnedTls_Init(PinnedTls *tls);

// Takes ownership of socket whether the TLS setup succeeds or fails.
// ALPN may be NULL or a NULL-terminated mbedTLS ALPN list.
int  PinnedTls_Open(PinnedTls *tls, SOCKET socket,
                    const char *hostname,
                    const char **alpn,
                    const uint8_t spki_pin[32]);

// Variant used by first-run/renewal enrollment. If `spki_pin` is present
// and matches, the connection succeeds without CA validation unless
// `force_ca_refresh` is nonzero. If the pin is missing/mismatched and
// `allow_ca_enrollment` is nonzero, the peer is accepted only after
// Windows certificate-chain + hostname validation succeeds.
int  PinnedTls_OpenEnrolled(PinnedTls *tls, SOCKET socket,
                            const char *hostname,
                            const char **alpn,
                            const uint8_t spki_pin[32],
                            int allow_ca_enrollment,
                            int force_ca_refresh,
                            PinnedTlsOpenResult *result);

// As PinnedTls_OpenEnrolled, but the peer leaf SPKI is accepted when it
// matches ANY of `pin_count` enrolled pins (multi-POP providers present
// different leaf keys per POP). `spki_pins` may be NULL when pin_count
// is 0. result->pin_matched reports a match against any set member.
int  PinnedTls_OpenEnrolledSet(PinnedTls *tls, SOCKET socket,
                               const char *hostname,
                               const char **alpn,
                               const uint8_t (*spki_pins)[32],
                               size_t pin_count,
                               int allow_ca_enrollment,
                               int force_ca_refresh,
                               PinnedTlsOpenResult *result);

mbedtls_ssl_context *PinnedTls_Ssl(PinnedTls *tls);
const char          *PinnedTls_NegotiatedAlpn(PinnedTls *tls);
void                 PinnedTls_CloseNotify(PinnedTls *tls);
void                 PinnedTls_Free(PinnedTls *tls);

#ifdef __cplusplus
}
#endif

#endif