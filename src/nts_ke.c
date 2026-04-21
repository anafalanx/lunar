// nts_ke.c -- NTS-KE record serialisation/parsing (RFC 8915 §4).
//
// Safety-critical parsing posture:
//
//   * Every length field is compared against remaining-bytes BEFORE
//     being advanced. No arithmetic that could overflow size_t.
//   * Unknown records with the critical bit SET are a hard failure
//     (RFC 8915 §4: "A peer that receives a record type it does not
//     recognize with the Critical Bit set MUST terminate the
//     exchange"). Unknown records with the critical bit CLEAR are
//     silently ignored.
//   * Cookie count is capped at NTSKE_MAX_COOKIES; excess cookies are
//     discarded (not a parse error -- a server may grant more than we
//     can store, we just keep the first N).
//   * Any record body we don't fully understand (e.g. a protocol list
//     that does not include NTPv4) is fine as long as it is not
//     critical or actively signals rejection.
//   * Error records always fail the parse and propagate the code.
//
// We do NOT call into any network / TLS / heap / logging code here --
// the file is pure data in, pure data out, which makes it trivial to
// exercise with hand-crafted byte strings in the unit tests.

#include "nts_ke.h"
#include <string.h>

// --- Little helpers ---------------------------------------------------

static uint16_t rd_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void wr_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

// Emit one record: critical-bit-or-ed type (16), body-length (16), body.
// Returns bytes written or 0 on overflow.
static size_t emit_record(uint8_t *out, size_t cap, size_t pos,
                          uint16_t type_with_crit,
                          const uint8_t *body, size_t body_len)
{
    if (pos > cap) return 0;
    if (cap - pos < 4) return 0;
    if (body_len > 0xffff) return 0;
    if (cap - pos - 4 < body_len) return 0;

    wr_be16(out + pos,     type_with_crit);
    wr_be16(out + pos + 2, (uint16_t)body_len);
    if (body_len > 0) {
        memcpy(out + pos + 4, body, body_len);
    }
    return 4 + body_len;
}

// --- Client request builder ------------------------------------------

size_t NtsKe_BuildClientRequest(uint8_t *out, size_t out_cap)
{
    if (out == NULL || out_cap < 16) return 0;

    size_t pos = 0;
    size_t w;

    // Record 1: Next Protocol Negotiation = [NTPv4]. Critical.
    uint8_t body_np[2];
    wr_be16(body_np, NTSKE_NEXTPROTO_NTPV4);
    w = emit_record(out, out_cap, pos,
                    (uint16_t)(0x8000u | NTSKE_REC_NEXT_PROTOCOL),
                    body_np, sizeof body_np);
    if (w == 0) return 0;
    pos += w;

    // Record 2: AEAD Algorithm Negotiation = [AES-SIV-CMAC-256].
    // Spec says the AEAD record's critical bit is at the discretion
    // of the sender, but we request it critical so the server aborts
    // if it can't honour our only supported AEAD rather than silently
    // falling back to something weaker.
    uint8_t body_aead[2];
    wr_be16(body_aead, NTSKE_AEAD_AES_SIV_CMAC_256);
    w = emit_record(out, out_cap, pos,
                    (uint16_t)(0x8000u | NTSKE_REC_AEAD_ALGORITHM),
                    body_aead, sizeof body_aead);
    if (w == 0) return 0;
    pos += w;

    // Record 3: End of Message. Always critical, body empty.
    w = emit_record(out, out_cap, pos,
                    (uint16_t)(0x8000u | NTSKE_REC_END_OF_MESSAGE),
                    NULL, 0);
    if (w == 0) return 0;
    pos += w;

    return pos;  // == 16 for this fixed request
}

// --- Response parser --------------------------------------------------

int NtsKe_ParseResponse(const uint8_t *in, size_t len, NtsKeResponse *resp)
{
    if (resp == NULL) return 0;
    memset(resp, 0, sizeof *resp);

    if (in == NULL) return 0;

    size_t pos = 0;
    int saw_end = 0;

    while (pos + 4 <= len) {
        uint16_t th  = rd_be16(in + pos);
        uint16_t blen = rd_be16(in + pos + 2);
        pos += 4;

        int      critical = (th & 0x8000) != 0;
        uint16_t type     = (uint16_t)(th & 0x7fff);

        if (blen > len - pos) {
            return 0;             // truncated record body
        }
        const uint8_t *body = in + pos;
        pos += blen;

        switch (type) {
        case NTSKE_REC_END_OF_MESSAGE:
            // Body must be empty; any trailing data after this record
            // is a protocol violation.
            if (blen != 0) return 0;
            if (pos != len) return 0;
            saw_end = 1;
            break;

        case NTSKE_REC_NEXT_PROTOCOL:
            // Body is a list of u16 protocol IDs. We only care that
            // NTPv4 is in the list.
            if (blen % 2 != 0) return 0;
            for (size_t i = 0; i + 2 <= blen; i += 2) {
                if (rd_be16(body + i) == NTSKE_NEXTPROTO_NTPV4) {
                    resp->proto_ok = 1;
                }
            }
            break;

        case NTSKE_REC_ERROR:
            if (blen != 2) return 0;
            resp->error_code = rd_be16(body);
            return 0;              // server is telling us to give up

        case NTSKE_REC_WARNING:
            if (blen != 2) return 0;
            resp->warning_code = rd_be16(body);
            break;

        case NTSKE_REC_AEAD_ALGORITHM:
            if (blen % 2 != 0) return 0;
            for (size_t i = 0; i + 2 <= blen; i += 2) {
                if (rd_be16(body + i) == NTSKE_AEAD_AES_SIV_CMAC_256) {
                    resp->aead_ok = 1;
                }
            }
            break;

        case NTSKE_REC_NEW_COOKIE:
            if (blen == 0 || blen > NTSKE_MAX_COOKIE_LEN) {
                // Skip cookies that are empty (nonsense) or too big
                // for our static storage; not a protocol-level error.
                break;
            }
            if (resp->cookie_count < NTSKE_MAX_COOKIES) {
                size_t i = resp->cookie_count;
                memcpy(resp->cookies[i], body, blen);
                resp->cookie_len[i] = blen;
                resp->cookie_count = i + 1;
            }
            break;

        case NTSKE_REC_NTPV4_SERVER:
            if (blen == 0 || blen > NTSKE_MAX_NTP_HOST_LEN) {
                if (critical) return 0;
                break;
            }
            memcpy(resp->ntp_host, body, blen);
            resp->ntp_host[blen] = '\0';
            // A rudimentary hostname sanity check: RFC 1035 allows
            // letters, digits, '-' and '.'. Reject anything else so
            // later DNS code can't be confused by control chars.
            for (size_t i = 0; i < blen; i++) {
                uint8_t c = body[i];
                if (!((c >= '0' && c <= '9') ||
                      (c >= 'A' && c <= 'Z') ||
                      (c >= 'a' && c <= 'z') ||
                      c == '-' || c == '.')) {
                    resp->ntp_host[0] = '\0';
                    if (critical) return 0;
                    break;
                }
            }
            break;

        case NTSKE_REC_NTPV4_PORT:
            if (blen != 2) { if (critical) return 0; break; }
            resp->ntp_port = rd_be16(body);
            break;

        default:
            // Unknown record: abort iff the critical bit is set.
            if (critical) return 0;
            break;
        }
    }

    // After the loop, pos should have reached exactly len AND we
    // must have seen End-of-Message. Anything else is malformed.
    if (pos != len || !saw_end) return 0;
    if (!resp->proto_ok || !resp->aead_ok) return 0;
    if (resp->cookie_count == 0) return 0;

    resp->ok = 1;
    return 1;
}
