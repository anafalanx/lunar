// nts.h -- NTS-KE transport (TLS 1.3 + ALPN + enrolled SPKI pins).
//
// This is the online half of NTS (RFC 8915): it performs the TCP
// handshake, the TLS 1.3 handshake, the NTS-KE record exchange, and
// the TLS exporter key derivation. The pure data codec (nts_ke.c)
// and the pure AEAD codec (siv.c + nts_ef.c) are composed here, but
// those modules remain unit-testable in isolation.
//
// Authentication model: first-run CA enrollment, then local SPKI pins.
// Lunar ships endpoint metadata but no provider cryptographic material.
// On first use / renewal, the endpoint is validated through the Windows
// Web PKI, its leaf SPKI is captured into the protected local pin store,
// and ordinary operation uses that local SPKI pin until renewal time.

#ifndef LUNAR_NTS_H
#define LUNAR_NTS_H

#include <stddef.h>
#include <stdint.h>

#include "nts_ke.h"

#ifdef __cplusplus
extern "C" {
#endif

// One provider in the NTS pool. host is a DNS name.
typedef struct {
    const char *host;              // e.g. "time.cloudflare.com"
    uint16_t    port;              // 0 => 4460 (the IANA NTS-KE port)
    const char *label;             // short log label
    const char *operator_family;   // independent operator grouping
} NtsProvider;

// Result of a completed NTS-KE exchange. Fields are only meaningful
// when ok == 1. Keys and cookies are kept in fixed-capacity buffers.
typedef struct {
    int      ok;
    uint8_t  c2s_key[32];          // from TLS exporter, C2S
    uint8_t  s2c_key[32];          // from TLS exporter, S2C
    size_t   cookie_count;
    size_t   cookie_len[NTSKE_MAX_COOKIES];
    uint8_t  cookies[NTSKE_MAX_COOKIES][NTSKE_MAX_COOKIE_LEN];
    // Optional SNTP endpoint override from the NTS-KE response.
    char     ntp_host[NTSKE_MAX_NTP_HOST_LEN + 1];   // empty => provider host
    uint16_t ntp_port;                                // 0 => 123
} NtsKeResult;

// Returns a pointer to the shipped provider metadata pool and its length.
const NtsProvider *Nts_Pool(size_t *out_len);

// Random pool selection using BCryptGenRandom for uniform selection.
const NtsProvider *Nts_PickProvider(void);

// Pick `n_want` DISTINCT providers at random. The picker prefers
// distinct operator families first, then fills any remaining slots from
// the rest of the pool. Intended for multi-slot NTS polling where the
// caller wants operator diversity within one cycle.
size_t Nts_PickProviders(const NtsProvider **out, size_t n_want);

// Perform a full NTS-KE exchange:
//   TCP connect -> TLS 1.3 handshake (ALPN "ntske/1") -> local SPKI
//   pin match or Windows CA enrollment -> NTS-KE records -> TLS
//   exporter -> graceful close.
//
// Returns 0 on success (out->ok == 1); non-zero on any failure,
// including SPKI pin mismatch, ALPN mismatch, server error record,
// socket timeout, or TLS failure. Safe to call from any thread (no
// shared state beyond lazy-initialised RNG).
int Nts_DoKe(const NtsProvider *p, NtsKeResult *out);

// Fetch one NTS-authenticated SNTP timing sample.
//
// Runs a full NTS-KE handshake against `p`, then issues exactly one
// SNTP v4 query to the resulting NTP endpoint using the first cookie
// and fresh random UID/nonce. The reply is authenticated with SIV
// (RFC 5297) using the S2C exporter key and validated the same way
// the unencrypted client validates a plain SNTP reply (LI / VN / Mode
// / stratum / non-zero timestamps).
//
// Timing is QPC-only; the Windows system clock is never consulted.
// The output is shaped the same way ntp.c's plain-SNTP query reports,
// so the aggregator can treat this as just another source:
//
//   *out_ntpUtcMs  : server-believed UTC at the moment *out_qpcAtT4
//                    was sampled (includes half-net-RTT compensation).
//   *out_qpcAtT4   : QPC tick at which the reply was received.
//   *out_rttMs     : measured round-trip time in milliseconds.
//
// Returns 1 on success, 0 on any failure (no pin populated, KE
// failure, DNS/socket failure, authentication failure, malformed
// reply, etc.). All key material is zeroised before return.
int Nts_FetchSample(const NtsProvider *p,
                    int64_t  *out_ntpUtcMs,
                    int64_t  *out_qpcAtT4,
                    uint32_t *out_rttMs);

#ifdef __cplusplus
}
#endif

#endif
