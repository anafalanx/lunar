// nts_ef.h -- NTS extension fields for SNTP packets (RFC 8915 §5).
//
// Scope:
//   * Build an NTS-authenticated client request given a 48-byte NTP
//     header, a cookie, a C2S AEAD key, and a caller-supplied random
//     Unique Identifier. The builder appends:
//         Unique Identifier        (type 0x0104)
//         NTS Cookie               (type 0x0204)
//         NTS Cookie Placeholder*  (type 0x0304) * n_placeholder
//         NTS Authenticator and Encrypted Extension Fields (0x0404)
//     where the Authenticator's AEAD input is:
//         AD        = entire packet up to this extension
//         Nonce     = 16 caller-supplied random bytes
//         Plaintext = empty  (no client-encrypted extensions)
//
//   * Parse + authenticate a server reply. Verifies the Unique
//     Identifier matches the one we sent, locates the Authenticator,
//     decrypts it with the S2C key, and extracts any New Cookie
//     extensions from the decrypted-plaintext field.
//
// No network, no heap, no TLS. Exercised by hand-crafted test vectors.

#ifndef LUNAR_NTS_EF_H
#define LUNAR_NTS_EF_H

#include <stddef.h>
#include <stdint.h>

#include "nts_ke.h"   // for NTSKE_MAX_COOKIES / NTSKE_MAX_COOKIE_LEN

#ifdef __cplusplus
extern "C" {
#endif

#define NTS_EF_UNIQUE_IDENTIFIER    0x0104
#define NTS_EF_COOKIE               0x0204
#define NTS_EF_COOKIE_PLACEHOLDER   0x0304
#define NTS_EF_AUTHENTICATOR        0x0404

#define NTS_UNIQUE_ID_LEN           32
#define NTS_NONCE_LEN               16   // what we use; RFC allows >= 16

// Build an NTS-authenticated SNTP client request.
//
// Inputs:
//   ntp_header[48]   Caller-composed NTP header (LI/VN/Mode, stratum,
//                    poll, precision, root delay / dispersion, refid,
//                    ref/originate/receive timestamps, and the
//                    Transmit Timestamp the caller wants echoed). This
//                    block is copied verbatim and is part of the AEAD
//                    associated data.
//   unique_id[32]    Caller-generated random identifier used as the
//                    Unique Identifier extension value and, on reply,
//                    for replay-rejection matching.
//   nonce[16]        Caller-generated random nonce placed into the
//                    Authenticator's Nonce field and fed to SIV as
//                    the last AD element.
//   cookie / clen    Current cookie to spend.
//   n_placeholder    Number of placeholder cookies to request (0..
//                    NTSKE_MAX_COOKIES-1). Each placeholder causes the
//                    server to return one additional fresh cookie.
//   c2s_key[32]      C2S AEAD key from the TLS exporter.
// Output:
//   out / out_cap    Destination buffer.
//   *out_len         Bytes written on success.
//
// Returns 0 on success, non-zero on any failure (bad args, short
// output buffer, SIV error).
int NtsEf_BuildRequest(const uint8_t  ntp_header[48],
                       const uint8_t  unique_id[NTS_UNIQUE_ID_LEN],
                       const uint8_t  nonce[NTS_NONCE_LEN],
                       const uint8_t *cookie, size_t clen,
                       size_t         n_placeholder,
                       const uint8_t  c2s_key[32],
                       uint8_t       *out, size_t out_cap,
                       size_t        *out_len);

// Parse + authenticate a server's NTS-protected SNTP reply.
//
// Inputs:
//   in / in_len             Received packet (48-byte header + extensions).
//   sent_unique_id[32]      The UID we sent; must appear in the reply.
//   s2c_key[32]             S2C AEAD key from the TLS exporter.
// Outputs (only written on success):
//   out_new_cookies / out_new_cookie_lens
//       Cookies extracted from the decrypted plaintext; up to
//       NTSKE_MAX_COOKIES. Individual entries capped at
//       NTSKE_MAX_COOKIE_LEN.
//   *out_new_count          Number of new cookies written.
//
// Returns 0 on a successfully authenticated reply matching our UID;
// non-zero on any failure (short input, malformed extension, missing
// or mismatched UID, missing authenticator, SIV auth failure, etc.).
int NtsEf_ParseResponse(const uint8_t *in, size_t in_len,
                        const uint8_t  sent_unique_id[NTS_UNIQUE_ID_LEN],
                        const uint8_t  s2c_key[32],
                        uint8_t        out_new_cookies[NTSKE_MAX_COOKIES][NTSKE_MAX_COOKIE_LEN],
                        size_t         out_new_cookie_lens[NTSKE_MAX_COOKIES],
                        size_t        *out_new_count);

#ifdef __cplusplus
}
#endif

#endif
