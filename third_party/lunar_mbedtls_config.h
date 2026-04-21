/* ========================================================================
 * Lunar's mbedTLS configuration                                           *
 * ========================================================================*
 *
 * This file is passed to mbedTLS via -DMBEDTLS_CONFIG_FILE="..." on the
 * gcc command line (see scripts/build.py). It takes the default config
 * shipped with mbedtls-3.6.6 as a starting point, then subtracts every
 * feature that a safety-critical TLS 1.3 NTS-KE client does not need.
 *
 * The defensive discipline here is "fewer features = smaller attack
 * surface". Each removal below is annotated with the reason. If you
 * enable something, write down why.
 *
 * What we keep:
 *   - TLS 1.3 client, ALPN
 *   - X.509 CRT parse + full certificate verification
 *   - Hashes: SHA-256, SHA-384, SHA-512 (for cert signatures)
 *   - AEADs: AES-GCM and ChaCha20-Poly1305 (TLS 1.3 mandatory)
 *   - Signature verify: RSA-PSS, ECDSA P-256 / P-384
 *   - Key exchange: X25519, P-256, P-384 (TLS 1.3 groups)
 *   - AES-CMAC and AES-CTR primitives (needed for NTS AES-SIV-CMAC-256)
 *   - PSA crypto (TLS 1.3 in 3.6.x uses PSA internally)
 *   - CTR_DRBG + platform entropy (Windows CryptGenRandom)
 *
 * What we throw away:
 *   - Any and all server code
 *   - TLS 1.2, DTLS, SSLv3, TLS 1.0, TLS 1.1
 *   - PSK / anonymous / static DH key exchanges
 *   - File-system I/O (all trusted certs are compile-time constants)
 *   - Session ticket persistence
 *   - RSA / EC keypair generation, signing, key export, PEM write
 *   - Weak curves (secp192, secp224, brainpool)
 *   - MD5, SHA-1, RIPEMD160 (not used in TLS 1.3 cert chains we accept)
 *   - Self-test harness, error strings, debug printing
 *   - Deprecated compat layers
 *
 * ------------------------------------------------------------------------
 * Start from the upstream default and poke holes in it.
 * ------------------------------------------------------------------------*/

#ifndef LUNAR_MBEDTLS_CONFIG_H
#define LUNAR_MBEDTLS_CONFIG_H

/* Take the upstream default config as a baseline. This is the
 * mbedtls/mbedtls_config.h shipped in the 3.6.6 tarball. Using
 * MBEDTLS_CONFIG_FILE already overrode it, so we must pull it in
 * explicitly. */
#include "mbedtls/mbedtls_config.h"

/* ------------------------------------------------------------------------
 * Protocol surface -- TLS 1.3 client only.
 * ------------------------------------------------------------------------*/

/* Kill TLS 1.2. NTS-KE servers are 1.3-only (RFC 8915 Sec.3). Removing
 * 1.2 eliminates the largest chunk of protocol state machine code and
 * every historical 1.2-era vulnerability class. */
#undef MBEDTLS_SSL_PROTO_TLS1_2
#undef MBEDTLS_SSL_ENCRYPT_THEN_MAC
#undef MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#undef MBEDTLS_SSL_RENEGOTIATION

/* TLS 1.3 is on by default in 3.6.x; be explicit. */
#define MBEDTLS_SSL_PROTO_TLS1_3

/* TLS 1.3 optional features we don't use. */
#undef MBEDTLS_SSL_EARLY_DATA            /* No 0-RTT. */
#undef MBEDTLS_SSL_RECORD_SIZE_LIMIT     /* Default 16KB is fine. */
#undef MBEDTLS_SSL_SESSION_TICKETS       /* Fresh handshake each cycle. */
#undef MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE  /* No middleboxes expected. */

/* Kill server. We are only ever a client. */
#undef MBEDTLS_SSL_SRV_C

/* Kill DTLS in all its forms. */
#undef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_DTLS_SRTP

/* Kill every non-(EC)DHE key exchange. TLS 1.3 only supports (EC)DHE
 * + PSK anyway, but TLS 1.2 left droppings all over the config. */
#undef MBEDTLS_KEY_EXCHANGE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_DHE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDHE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_RSA_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_DHE_RSA_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDH_ECDSA_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDH_RSA_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED

/* TLS 1.3-specific key exchange: ephemeral all round. */
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#undef  MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_ENABLED
#undef  MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED

/* ------------------------------------------------------------------------
 * File-system I/O: none. The only trusted roots are the ones we
 * compile into the binary. No on-disk loading, ever.
 * ------------------------------------------------------------------------*/
#undef MBEDTLS_FS_IO

/* PEM parse pulls in filesystem & base64. We embed DER, so drop it. */
#undef MBEDTLS_PEM_PARSE_C
#undef MBEDTLS_PEM_WRITE_C
#undef MBEDTLS_BASE64_C

/* ------------------------------------------------------------------------
 * Networking: we provide our own BIO on top of our existing SOCKET.
 * mbedTLS never touches the socket directly.
 * ------------------------------------------------------------------------*/
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C

/* ------------------------------------------------------------------------
 * X.509 surface: read and verify CRTs. Nothing else.
 * ------------------------------------------------------------------------*/
#undef MBEDTLS_X509_CRT_WRITE_C
#undef MBEDTLS_X509_CSR_WRITE_C
#undef MBEDTLS_X509_CSR_PARSE_C          /* We never parse CSRs. */
#undef MBEDTLS_X509_CREATE_C
#undef MBEDTLS_X509_CRL_PARSE_C          /* No CRL fetching; we pin SPKIs. */

/* ------------------------------------------------------------------------
 * PK (public key) surface: parse + verify. No generation, no signing,
 * no writing.
 * ------------------------------------------------------------------------*/
#undef MBEDTLS_PK_WRITE_C
#undef MBEDTLS_PKCS5_C
#undef MBEDTLS_PKCS7_C
#undef MBEDTLS_PKCS12_C

/* ------------------------------------------------------------------------
 * Hashes: keep only what TLS 1.3 and modern cert chains use.
 * ------------------------------------------------------------------------*/
#undef MBEDTLS_MD5_C                     /* Cert-signing MD5 died years ago. */
#undef MBEDTLS_SHA1_C                    /* Same story. */
#undef MBEDTLS_RIPEMD160_C
/* SHA-224/SHA-256 are in one compilation unit; leave SHA256 on (kept). */
#undef MBEDTLS_SHA3_C                    /* Not used by any NTS provider. */
#undef MBEDTLS_SHA512_USE_A64_CRYPTO_IF_PRESENT  /* x86 build. */
#undef MBEDTLS_SHA512_USE_A64_CRYPTO_ONLY

/* ------------------------------------------------------------------------
 * Ciphers: keep AES (GCM, CTR, CMAC) and ChaCha20-Poly1305. Nothing else.
 * ------------------------------------------------------------------------*/
#undef MBEDTLS_ARIA_C
#undef MBEDTLS_CAMELLIA_C
#undef MBEDTLS_DES_C
#undef MBEDTLS_BLOWFISH_C
#undef MBEDTLS_CHACHA20_C_disabled       /* keep on -- used by ChaCha20-Poly */
#undef MBEDTLS_CCM_C                     /* TLS 1.3 permits but we prefer GCM/ChaChaPoly. */
#undef MBEDTLS_CIPHER_MODE_CBC
#undef MBEDTLS_CIPHER_MODE_CFB
#undef MBEDTLS_CIPHER_MODE_OFB
#undef MBEDTLS_CIPHER_MODE_XTS
#undef MBEDTLS_CIPHER_PADDING_PKCS7
#undef MBEDTLS_CIPHER_PADDING_ONE_AND_ZEROS
#undef MBEDTLS_CIPHER_PADDING_ZEROS_AND_LEN
#undef MBEDTLS_CIPHER_PADDING_ZEROS
#undef MBEDTLS_NIST_KW_C

/* We DO need CMAC (for AES-SIV-CMAC-256 in NTS) and GCM (for TLS 1.3 AEAD). */
#define MBEDTLS_CMAC_C
#define MBEDTLS_CIPHER_MODE_CTR          /* Inner CTR of AES-SIV. */

/* ------------------------------------------------------------------------
 * Elliptic curves: keep only the ones modern CAs use AND the TLS 1.3
 * named groups. Everything else is a CVE waiting to happen.
 * ------------------------------------------------------------------------*/
#undef MBEDTLS_ECP_DP_SECP192R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP224R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP521R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP192K1_ENABLED
#undef MBEDTLS_ECP_DP_SECP224K1_ENABLED
#undef MBEDTLS_ECP_DP_SECP256K1_ENABLED
#undef MBEDTLS_ECP_DP_BP256R1_ENABLED
#undef MBEDTLS_ECP_DP_BP384R1_ENABLED
#undef MBEDTLS_ECP_DP_BP512R1_ENABLED
#undef MBEDTLS_ECP_DP_CURVE448_ENABLED
/* Kept: SECP256R1, SECP384R1, CURVE25519 -- all three are in the
 * upstream defaults already. */

/* Finite-field DH is not used in TLS 1.3. Drop the entire module. */
#undef MBEDTLS_DHM_C

/* ------------------------------------------------------------------------
 * PSA entry points and storage. TLS 1.3 in 3.6.x uses PSA internally,
 * so we keep MBEDTLS_PSA_CRYPTO_C and MBEDTLS_USE_PSA_CRYPTO enabled.
 * We disable PSA STORAGE and keys-on-disk.
 * ------------------------------------------------------------------------*/
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_SE_C
#undef MBEDTLS_PSA_INJECT_ENTROPY

/* ------------------------------------------------------------------------
 * Hygiene: no deprecated APIs, no weird debug, no self tests.
 * ------------------------------------------------------------------------*/
#undef MBEDTLS_DEPRECATED_WARNING
#define MBEDTLS_DEPRECATED_REMOVED       /* Hard-remove anything deprecated. */
#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_ERROR_C                   /* Saves ~14KB; error codes are
                                            still returned as integers. */
#undef MBEDTLS_VERSION_FEATURES          /* Saves a table; MBEDTLS_VERSION_C
                                            is still on for runtime checks. */

/* We do NOT allow weak cert-verification-without-hostname fallback.
 * The upstream 3.6.3 advisory specifically warns against this. */
#undef MBEDTLS_SSL_CLI_ALLOW_WEAK_CERTIFICATE_VERIFICATION_WITHOUT_HOSTNAME

/* ------------------------------------------------------------------------
 * Platform: MinGW/UCRT64 on Windows. Entropy comes from CryptGenRandom
 * (library/entropy_poll.c auto-detects _WIN32).
 * ------------------------------------------------------------------------*/
#define MBEDTLS_HAVE_TIME                /* We provide mbedtls_time via Clock_NowUtcMs. */
#define MBEDTLS_PLATFORM_MS_TIME_ALT     /* We supply our own ms-resolution time. */
#undef MBEDTLS_HAVE_TIME_DATE            /* No gmtime() calls inside mbedTLS;
                                            we validate cert notBefore/notAfter
                                            using our own disciplined UTC. */

#endif /* LUNAR_MBEDTLS_CONFIG_H */
