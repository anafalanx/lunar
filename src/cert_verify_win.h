// cert_verify_win.h -- Windows certificate-chain validation for enrolled pins.

#ifndef LUNAR_CERT_VERIFY_WIN_H
#define LUNAR_CERT_VERIFY_WIN_H

#include <stdint.h>

#include "mbedtls/ssl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      ok;
    int      revocation_checked;
    int      revocation_offline;
    uint32_t chain_error_status;
    uint32_t chain_info_status;
    uint32_t policy_error;
    uint8_t  spki_sha256[32];
    char     spki_hex[65];
    char     subject[256];
    char     issuer[256];
    char     not_before[32];
    char     not_after[32];
    int64_t  not_before_unix;
    int64_t  not_after_unix;
} CertVerifyWinResult;

void CertVerifyWin_ResultInit(CertVerifyWinResult *out);
void CertVerifyWin_Hex32(const uint8_t in[32], char out_hex[65]);

// Validate the peer certificate chain from an mbedTLS TLS session using
// the Windows certificate store and SSL hostname policy. Also extracts
// the leaf SPKI SHA-256 and certificate validity metadata.
int CertVerifyWin_ValidateMbedtlsPeer(mbedtls_ssl_context *ssl,
                                      const char *hostname,
                                      CertVerifyWinResult *out);

#ifdef __cplusplus
}
#endif

#endif