// nts_ef.c -- NTS SNTP extension fields (RFC 8915 §5).
//
// RFC 7822 extension-field wire format:
//
//    0                   1                   2                   3
//    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//   +-------------------------------+-------------------------------+
//   |          Field Type           |             Length            |
//   +-------------------------------+-------------------------------+
//   |                           Value + Padding                     |
//   +---------------------------------------------------------------+
//
// where Length is the total octet count of the extension (header +
// value + padding) and MUST be a multiple of 4.
//
// NTS Authenticator extension (type 0x0404) body layout:
//
//   +-------------------------------+-------------------------------+
//   |         Nonce Length          |       Ciphertext Length       |
//   +-------------------------------+-------------------------------+
//   :                           Nonce                               :
//   :                      Padding (as needed)                      :
//   +---------------------------------------------------------------+
//   :                         Ciphertext                            :
//   :                      Padding (as needed)                      :
//   +---------------------------------------------------------------+
//   :                     Additional Padding                        :
//   +---------------------------------------------------------------+
//
// The Nonce and Ciphertext fields are each rounded up to the next
// multiple of 4 with zero padding. The AEAD inputs are:
//   AD        = entire packet from byte 0 up to (not including) this
//               extension
//   Plaintext = zero or more encrypted extension fields
//   Ciphertext field = AEAD output (for SIV: tag(16) || enc(P))

#include "nts_ef.h"
#include "siv.h"
#include <string.h>

// --- Small helpers ----------------------------------------------------

static uint16_t rd_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}
static void wr_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}
static size_t align4(size_t n) { return (n + 3u) & ~(size_t)3u; }

// Emit one plain RFC 7822 extension (header + value + trailing pad).
// Returns bytes written or 0 on overflow.
static size_t emit_ef(uint8_t *buf, size_t cap, size_t pos,
                      uint16_t ef_type,
                      const uint8_t *value, size_t vlen)
{
    size_t total = align4(4 + vlen);      // header + value, padded
    if (total < 16) total = 16;           // RFC 5905 minimum for a single EF
    if (total > 0xffffu) return 0;
    if (pos > cap || cap - pos < total) return 0;

    wr_be16(buf + pos,     ef_type);
    wr_be16(buf + pos + 2, (uint16_t)total);
    if (vlen > 0) memcpy(buf + pos + 4, value, vlen);
    // Zero any padding bytes.
    for (size_t i = 4 + vlen; i < total; i++) buf[pos + i] = 0;
    return total;
}

// --- Request builder --------------------------------------------------

int NtsEf_BuildRequest(const uint8_t  ntp_header[48],
                       const uint8_t  unique_id[NTS_UNIQUE_ID_LEN],
                       const uint8_t  nonce[NTS_NONCE_LEN],
                       const uint8_t *cookie, size_t clen,
                       size_t         n_placeholder,
                       const uint8_t  c2s_key[32],
                       uint8_t       *out, size_t out_cap,
                       size_t        *out_len)
{
    if (!ntp_header || !unique_id || !nonce || !cookie ||
        !c2s_key || !out || !out_len) return -1;
    if (clen == 0 || clen > NTSKE_MAX_COOKIE_LEN) return -1;
    if (n_placeholder >= NTSKE_MAX_COOKIES) return -1;
    if (out_cap < 48) return -1;

    size_t pos = 48;
    memcpy(out, ntp_header, 48);

    size_t w;

    // 1) Unique Identifier
    w = emit_ef(out, out_cap, pos, NTS_EF_UNIQUE_IDENTIFIER,
                unique_id, NTS_UNIQUE_ID_LEN);
    if (w == 0) return -1;
    pos += w;

    // 2) NTS Cookie (one to spend).
    w = emit_ef(out, out_cap, pos, NTS_EF_COOKIE, cookie, clen);
    if (w == 0) return -1;
    pos += w;

    // 3) Placeholders (zeros the same size as the spent cookie) so the
    // server returns that many fresh cookies in the encrypted body.
    // We use a stack buffer capped at NTSKE_MAX_COOKIE_LEN.
    uint8_t zeros[NTSKE_MAX_COOKIE_LEN];
    memset(zeros, 0, clen);
    for (size_t i = 0; i < n_placeholder; i++) {
        w = emit_ef(out, out_cap, pos, NTS_EF_COOKIE_PLACEHOLDER,
                    zeros, clen);
        if (w == 0) return -1;
        pos += w;
    }

    // 4) Authenticator and Encrypted Extension Fields.
    // Body = Nonce Length(2) + Ciphertext Length(2) + Nonce(16) + CT(16)
    // CT = SIV tag over AD=out[0..pos], nonce=N, plaintext=empty.
    // => CT is exactly 16 bytes (tag only, no encrypted payload).
    const size_t nonce_len = NTS_NONCE_LEN;   // 16
    const size_t ct_len    = 16;              // SIV tag, empty plaintext
    const size_t nonce_pad = align4(nonce_len) - nonce_len;   // 0
    const size_t ct_pad    = align4(ct_len)    - ct_len;      // 0
    const size_t auth_body = 4 + nonce_len + nonce_pad + ct_len + ct_pad;
    const size_t auth_total = align4(4 + auth_body);          // 40
    if (out_cap - pos < auth_total) return -1;

    // Reserve the header now so AEAD AD = everything up to this EF.
    size_t auth_pos = pos;
    wr_be16(out + auth_pos,     NTS_EF_AUTHENTICATOR);
    wr_be16(out + auth_pos + 2, (uint16_t)auth_total);
    wr_be16(out + auth_pos + 4, (uint16_t)nonce_len);
    wr_be16(out + auth_pos + 6, (uint16_t)ct_len);
    memcpy(out + auth_pos + 8, nonce, nonce_len);

    // AD = bytes 0..auth_pos-1 (packet header + preceding extensions,
    // NOT including the Authenticator's own header).
    SivSlice ad_vec[2] = {
        { out,   auth_pos },
        { nonce, nonce_len },
    };
    uint8_t siv_out[16 + 0];     // tag only (no plaintext)
    int rc = Siv_Encrypt(c2s_key, ad_vec, 2, NULL, 0, siv_out);
    if (rc != 0) return rc;

    memcpy(out + auth_pos + 8 + nonce_len, siv_out, 16);
    // No additional padding needed (40 bytes is already aligned).

    pos = auth_pos + auth_total;
    *out_len = pos;
    return 0;
}

// --- Response parser --------------------------------------------------

// Walk extension fields starting at offset `start`. On each iteration
// yields the extension type, a pointer to its value (4 bytes after
// the start of the EF), and the value-plus-padding length (total-4).
// Returns 0 on clean end (pos == in_len), -1 on malformed.
//
// This is NOT used directly; the parser below iterates inline because
// it needs to compute the Authenticator's AD offset.

int NtsEf_ParseResponse(const uint8_t *in, size_t in_len,
                        const uint8_t  sent_unique_id[NTS_UNIQUE_ID_LEN],
                        const uint8_t  s2c_key[32],
                        uint8_t        out_new_cookies[NTSKE_MAX_COOKIES][NTSKE_MAX_COOKIE_LEN],
                        size_t         out_new_cookie_lens[NTSKE_MAX_COOKIES],
                        size_t        *out_new_count)
{
    if (!in || !sent_unique_id || !s2c_key ||
        !out_new_cookies || !out_new_cookie_lens || !out_new_count) return -1;

    *out_new_count = 0;
    if (in_len < 48) return -1;

    size_t pos        = 48;
    int    saw_uid    = 0;
    size_t auth_pos   = 0;
    int    saw_auth   = 0;

    // First pass: sanity-walk every extension, confirm Unique Identifier
    // matches, locate the Authenticator.
    while (pos + 4 <= in_len) {
        uint16_t type = rd_be16(in + pos);
        uint16_t len  = rd_be16(in + pos + 2);
        if (len < 4 || (len & 0x3) != 0) return -1;
        if (len > in_len - pos) return -1;

        if (type == NTS_EF_UNIQUE_IDENTIFIER) {
            // Value bytes: exactly 32. Constant-time compare against sent UID.
            if (len < 4 + NTS_UNIQUE_ID_LEN) return -1;
            // Padded value may be longer; the first 32 bytes carry the UID.
            uint8_t diff = 0;
            for (size_t i = 0; i < NTS_UNIQUE_ID_LEN; i++) {
                diff |= (uint8_t)(in[pos + 4 + i] ^ sent_unique_id[i]);
            }
            if (diff != 0) return -1;
            saw_uid = 1;
        } else if (type == NTS_EF_AUTHENTICATOR) {
            auth_pos = pos;
            saw_auth = 1;
            pos += len;
            break;      // per RFC 8915, this is the last extension we process
        }

        pos += len;
    }

    if (!saw_uid || !saw_auth) return -1;
    if (pos != in_len) {
        // Strictly: unencrypted extensions after the Authenticator are
        // forbidden (they would not be authenticated and could be
        // appended by an attacker to forge state). Reject.
        return -1;
    }

    // Parse the Authenticator body.
    //   off 0 : Field Type (2)
    //   off 2 : Length     (2)   [== total, already read]
    //   off 4 : Nonce Length     (2)
    //   off 6 : Ciphertext Length(2)
    //   off 8 : Nonce (padded to *4)
    //          Ciphertext (padded to *4)
    //          additional padding
    uint16_t auth_total = rd_be16(in + auth_pos + 2);
    if (auth_total < 12) return -1;
    if (auth_pos + auth_total != in_len) return -1;

    uint16_t nonce_len = rd_be16(in + auth_pos + 4);
    uint16_t ct_len    = rd_be16(in + auth_pos + 6);
    if (nonce_len < 16) return -1;              // RFC 8915 §5.7: >= 16
    if (ct_len    < 16) return -1;              // SIV tag alone = 16

    size_t nonce_padded = align4(nonce_len);
    size_t ct_padded    = align4(ct_len);
    // Body layout check.
    size_t need = 4 + nonce_padded + ct_padded;
    if (auth_total < 4 || need > (size_t)(auth_total - 4)) return -1;

    const uint8_t *nonce_p = in + auth_pos + 8;
    const uint8_t *ct_p    = nonce_p + nonce_padded;

    // AD = everything before this extension's header.
    SivSlice ad_vec[2] = {
        { in,      auth_pos },
        { nonce_p, nonce_len },
    };

    // Decrypt (and authenticate) the ciphertext.
    if (ct_len < 16) return -1;
    size_t pt_len = ct_len - 16;
    uint8_t pt_buf[1024];
    if (pt_len > sizeof pt_buf) return -1;
    int rc = Siv_Decrypt(s2c_key, ad_vec, 2, ct_p, ct_len, pt_buf);
    if (rc != 0) return -1;

    // Walk the decrypted plaintext as a stream of NTS extension fields
    // and harvest any New Cookie extensions (type 0x0204).
    size_t  p = 0;
    size_t  ncook = 0;
    while (p + 4 <= pt_len) {
        uint16_t t = rd_be16(pt_buf + p);
        uint16_t l = rd_be16(pt_buf + p + 2);
        if (l < 4 || (l & 0x3) != 0) return -1;
        if (l > pt_len - p) return -1;

        if (t == NTS_EF_COOKIE) {
            size_t clen = l - 4;
            // Strip trailing zero padding (RFC 7822): cookie bytes are
            // left-justified; padding is zeros to multiple of 4. But
            // cookies themselves can legitimately end in zero bytes,
            // so without knowing the true cookie length we preserve
            // the full padded body. The server uses cookies opaquely
            // and will accept whatever we feed back.
            if (clen > NTSKE_MAX_COOKIE_LEN) { p += l; continue; }
            if (ncook < NTSKE_MAX_COOKIES) {
                memcpy(out_new_cookies[ncook], pt_buf + p + 4, clen);
                out_new_cookie_lens[ncook] = clen;
                ncook++;
            }
        }
        p += l;
    }
    if (p != pt_len) return -1;

    *out_new_count = ncook;
    return 0;
}
