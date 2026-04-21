// siv.c -- AES-SIV-CMAC-256 per RFC 5297.
//
// Design decisions worth calling out:
//
//   * The S2V loop is the authenticator. It absorbs each AD element
//     through CMAC and folds the result into a running accumulator D
//     via the GF(2^128) "dbl" (polynomial x^128 + x^7 + x^2 + x + 1,
//     i.e. 0x87) operation followed by XOR.
//
//   * Whether the plaintext is >= 16 bytes or shorter changes the
//     finalisation ("xorend" vs "dbl + pad" in RFC 5297 §2.4). We
//     implement both paths; the short path uses a 16-byte padded
//     buffer, the long path streams via the incremental CMAC API
//     so we don't have to allocate a full-length temporary.
//
//   * "Counter IV" masking: RFC 5297 §2.5 requires bits 31 and 63
//     (numbering from the right) of the SIV to be zeroed before use
//     as the CTR-mode starting counter, so that CTR increments never
//     wrap into the top halves of either 32-bit lane. On a 16-byte
//     big-endian array that means clearing the MSB of V[8] and V[12].
//
//   * The tag verification in Siv_Decrypt uses mbedtls_ct_memcmp()
//     to defeat timing-based forgery probes.
//
//   * No heap: all scratch lives on the stack. Contexts are zeroed
//     on exit with mbedtls_platform_zeroize.

#include "siv.h"

#include <string.h>

#include "mbedtls/cipher.h"
#include "mbedtls/cmac.h"
#include "mbedtls/aes.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/platform_util.h"

// GF(2^128) doubling used by S2V (RFC 5297 §2.3). Big-endian 16-byte
// block v is treated as a polynomial; this left-shifts by 1 and, if
// the top bit was set, XORs the reduction constant 0x87 into the
// low byte. This is the same "dbl" used by CMAC key derivation.
static void siv_dbl(uint8_t v[16])
{
    uint8_t carry = (uint8_t)(v[0] >> 7);
    for (int i = 0; i < 15; i++) {
        v[i] = (uint8_t)((v[i] << 1) | (v[i + 1] >> 7));
    }
    v[15] = (uint8_t)(v[15] << 1);
    if (carry) {
        v[15] ^= 0x87;
    }
}

static void siv_xor16(uint8_t *dst, const uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < 16; i++) {
        dst[i] = a[i] ^ b[i];
    }
}

// One-shot CMAC-AES-128. Wraps mbedtls_cipher_cmac() with a looked-up
// cipher_info. Returns 0 on success.
static int siv_cmac(const uint8_t *key16,
                    const uint8_t *in, size_t in_len,
                    uint8_t out[16])
{
    const mbedtls_cipher_info_t *info =
        mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB);
    if (info == NULL) {
        return -1;
    }
    return mbedtls_cipher_cmac(info, key16, 128, in, in_len, out);
}

// S2V -- the synthetic-IV construction from RFC 5297 §2.4. Absorbs
// nad AD components plus the plaintext "Sn" slot, returns a 16-byte
// authenticator that doubles as the CTR starting block.
static int siv_s2v(const uint8_t  *k1,
                   const SivSlice *ad, size_t nad,
                   const uint8_t  *pt, size_t pt_len,
                   uint8_t         out[16])
{
    static const uint8_t zero16[16] = { 0 };
    uint8_t d[16], cm[16];
    int rc;

    rc = siv_cmac(k1, zero16, 16, d);
    if (rc != 0) {
        return rc;
    }

    for (size_t i = 0; i < nad; i++) {
        rc = siv_cmac(k1, ad[i].p, ad[i].n, cm);
        if (rc != 0) {
            mbedtls_platform_zeroize(d, sizeof d);
            mbedtls_platform_zeroize(cm, sizeof cm);
            return rc;
        }
        siv_dbl(d);
        siv_xor16(d, d, cm);
    }

    if (pt_len >= 16) {
        // T = pt with its LAST 16 bytes XOR-ed with D; CMAC(K, T).
        // Stream the head (all bytes except last 16) through CMAC,
        // then the XOR-ed tail as the final 16 bytes.
        mbedtls_cipher_context_t ctx;
        mbedtls_cipher_init(&ctx);
        const mbedtls_cipher_info_t *info =
            mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB);
        if (info == NULL) {
            mbedtls_cipher_free(&ctx);
            rc = -1;
            goto done;
        }
        rc = mbedtls_cipher_setup(&ctx, info);
        if (rc == 0) rc = mbedtls_cipher_cmac_starts(&ctx, k1, 128);
        if (rc != 0) {
            mbedtls_cipher_free(&ctx);
            goto done;
        }
        size_t head = pt_len - 16;
        if (head > 0) {
            rc = mbedtls_cipher_cmac_update(&ctx, pt, head);
            if (rc != 0) {
                mbedtls_cipher_free(&ctx);
                goto done;
            }
        }
        uint8_t tail[16];
        siv_xor16(tail, pt + head, d);
        rc = mbedtls_cipher_cmac_update(&ctx, tail, 16);
        if (rc == 0) rc = mbedtls_cipher_cmac_finish(&ctx, out);
        mbedtls_platform_zeroize(tail, sizeof tail);
        mbedtls_cipher_free(&ctx);
    } else {
        // T = dbl(D) XOR pad(pt), where pad is pt || 0x80 || 0x00*.
        uint8_t padded[16] = { 0 };
        if (pt_len > 0) {
            memcpy(padded, pt, pt_len);
        }
        padded[pt_len] = 0x80;
        siv_dbl(d);
        uint8_t t[16];
        siv_xor16(t, d, padded);
        rc = siv_cmac(k1, t, 16, out);
        mbedtls_platform_zeroize(padded, sizeof padded);
        mbedtls_platform_zeroize(t, sizeof t);
    }

done:
    mbedtls_platform_zeroize(d, sizeof d);
    mbedtls_platform_zeroize(cm, sizeof cm);
    return rc;
}

// Run AES-128-CTR with the SIV as the initial counter, after masking
// bits 31 and 63 per RFC 5297 §2.5. siv_in may alias siv_out.
static int siv_ctr(const uint8_t  k2[16],
                   const uint8_t  siv_in[16],
                   const uint8_t *in,
                   uint8_t       *out,
                   size_t         len)
{
    uint8_t nonce_counter[16];
    memcpy(nonce_counter, siv_in, 16);
    nonce_counter[8]  &= 0x7f;     // bit 63 = MSB of V[8]  cleared
    nonce_counter[12] &= 0x7f;     // bit 31 = MSB of V[12] cleared

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int rc = mbedtls_aes_setkey_enc(&aes, k2, 128);
    if (rc == 0 && len > 0) {
        size_t nc_off = 0;
        uint8_t stream_block[16] = { 0 };
        rc = mbedtls_aes_crypt_ctr(&aes, len, &nc_off,
                                   nonce_counter, stream_block,
                                   in, out);
        mbedtls_platform_zeroize(stream_block, sizeof stream_block);
    }
    mbedtls_aes_free(&aes);
    mbedtls_platform_zeroize(nonce_counter, sizeof nonce_counter);
    return rc;
}

int Siv_Encrypt(const uint8_t   key[32],
                const SivSlice *ad, size_t nad,
                const uint8_t  *pt, size_t pt_len,
                uint8_t        *out)
{
    if (key == NULL || out == NULL) return -1;
    if (pt_len > 0 && pt == NULL)   return -1;
    if (nad > 0 && ad == NULL)      return -1;

    const uint8_t *k1 = key;
    const uint8_t *k2 = key + 16;

    uint8_t siv[16];
    int rc = siv_s2v(k1, ad, nad, pt, pt_len, siv);
    if (rc != 0) {
        mbedtls_platform_zeroize(siv, sizeof siv);
        return rc;
    }

    memcpy(out, siv, 16);
    if (pt_len > 0) {
        rc = siv_ctr(k2, siv, pt, out + 16, pt_len);
    }
    mbedtls_platform_zeroize(siv, sizeof siv);
    return rc;
}

int Siv_Decrypt(const uint8_t   key[32],
                const SivSlice *ad, size_t nad,
                const uint8_t  *ct, size_t ct_len,
                uint8_t        *out_pt)
{
    if (key == NULL || ct == NULL) return -1;
    if (ct_len < 16)               return -1;
    if (nad > 0 && ad == NULL)     return -1;

    size_t pt_len = ct_len - 16;
    if (pt_len > 0 && out_pt == NULL) return -1;

    const uint8_t *k1 = key;
    const uint8_t *k2 = key + 16;

    // Decrypt the ciphertext body under CTR, keyed by K2, using the
    // received SIV as the starting counter. We write the tentative
    // plaintext into out_pt and only expose it after verifying the
    // tag; on failure we zeroize it to avoid leaking unauthenticated
    // bytes to the caller.
    int rc = 0;
    if (pt_len > 0) {
        rc = siv_ctr(k2, ct, ct + 16, out_pt, pt_len);
        if (rc != 0) {
            mbedtls_platform_zeroize(out_pt, pt_len);
            return rc;
        }
    }

    uint8_t expected[16];
    rc = siv_s2v(k1, ad, nad, out_pt, pt_len, expected);
    if (rc != 0) {
        if (pt_len > 0) mbedtls_platform_zeroize(out_pt, pt_len);
        mbedtls_platform_zeroize(expected, sizeof expected);
        return rc;
    }

    int eq = mbedtls_ct_memcmp(expected, ct, 16);
    mbedtls_platform_zeroize(expected, sizeof expected);
    if (eq != 0) {
        if (pt_len > 0) mbedtls_platform_zeroize(out_pt, pt_len);
        return -1;
    }
    return 0;
}
