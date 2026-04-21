// nts_ke.h -- NTS-KE (Network Time Security Key Establishment) record
// layer per RFC 8915, section 4.
//
// Scope of this file:
//   * Record type constants.
//   * Pure serialize / parse of the wire format (length-prefixed TLVs
//     with a "critical" bit). No TLS, no sockets, no side effects.
//   * Higher-level helpers for the client's specific request
//     (NextProtocol=NTPv4, AEAD=AES-SIV-CMAC-256, EndOfMessage) and
//     for interpreting the server's response into cookies + optional
//     server/port overrides.
//
// The TLS transport that carries these records lives in a separate
// translation unit so this one can be unit-tested against hand-
// crafted byte strings without requiring mbedTLS on the test path.
//
// Wire format (RFC 8915 §4):
//
//     0                   1                   2                   3
//     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//    +-+---------------------------+-------------------------------+
//    |C|        Record Type        |          Body Length          |
//    +-+---------------------------+-------------------------------+
//    |                                                             |
//    :                       Record Body                           :
//    |                                                             |
//    +-------------------------------------------------------------+
//
// C = Critical bit: MSB of the first octet. Any record with C set
// that the peer does not understand MUST abort the exchange.

#ifndef LUNAR_NTS_KE_H
#define LUNAR_NTS_KE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Record-type registry (RFC 8915 §7.6 and IANA registry) -----------

#define NTSKE_REC_END_OF_MESSAGE        0x0000    // critical
#define NTSKE_REC_NEXT_PROTOCOL         0x0001    // critical
#define NTSKE_REC_ERROR                 0x0002    // critical
#define NTSKE_REC_WARNING               0x0003    // critical
#define NTSKE_REC_AEAD_ALGORITHM        0x0004    // critical
#define NTSKE_REC_NEW_COOKIE            0x0005
#define NTSKE_REC_NTPV4_SERVER          0x0006
#define NTSKE_REC_NTPV4_PORT            0x0007

// Next Protocol IDs (RFC 8915).
#define NTSKE_NEXTPROTO_NTPV4           0x0000

// AEAD algorithm IDs (IANA AEAD registry; RFC 5297 = 15).
#define NTSKE_AEAD_AES_SIV_CMAC_256     15

// --- Limits -------------------------------------------------------------

// Practical ceilings we impose on the parser. A malicious or malformed
// peer cannot force us to allocate more than these.
#define NTSKE_MAX_COOKIES               8        // per KE exchange
#define NTSKE_MAX_COOKIE_LEN            256      // per RFC 8915 practice
#define NTSKE_MAX_NTP_HOST_LEN          255      // DNS name ceiling

// --- Request builder ---------------------------------------------------

// Build the fixed client request: {NextProtocol=[NTPv4], AEAD=[SIV],
// EndOfMessage}. All three records have the critical bit set.
// Returns bytes written on success (always 16 for this fixed set),
// or 0 if `out_cap` is too small.
size_t NtsKe_BuildClientRequest(uint8_t *out, size_t out_cap);

// --- Response parser ---------------------------------------------------

// Parsed NTS-KE response. All fields written only on success. Cookies
// are copied into the embedded buffers; no heap.
typedef struct {
    int      ok;                         // 1 on a successful exchange, 0 otherwise
    int      aead_ok;                    // 1 if server accepted AES-SIV-CMAC-256
    int      proto_ok;                   // 1 if server accepted NTPv4
    uint16_t error_code;                 // valid if !ok and server sent NTSKE_REC_ERROR
    uint16_t warning_code;               // 0 if none
    size_t   cookie_count;
    size_t   cookie_len[NTSKE_MAX_COOKIES];
    uint8_t  cookies[NTSKE_MAX_COOKIES][NTSKE_MAX_COOKIE_LEN];
    // Optional server/port override. Empty host means "use the KE host".
    char     ntp_host[NTSKE_MAX_NTP_HOST_LEN + 1];
    uint16_t ntp_port;                   // 0 means "default (123)"
} NtsKeResponse;

// Parse `in` (len bytes) as a sequence of NTS-KE records. Returns 1
// if the response ends with an End-of-Message record and all critical
// records were recognised; 0 on any parse error, unknown critical
// record, or peer-signalled Error record. Always sets resp->ok to
// match the return value so callers can trust resp->ok alone.
int NtsKe_ParseResponse(const uint8_t *in, size_t len,
                        NtsKeResponse *resp);

#ifdef __cplusplus
}
#endif

#endif
