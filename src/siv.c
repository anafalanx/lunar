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
#include <assert.h>

#include "mbedtls/cipher.h"
#include "mbedtls/cmac.h"
#include "mbedtls/aes.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/platform_util.h"

enum {
    SIV_KEY_LEN = 32,
    SIV_HALF_KEY_LEN = 16,
    SIV_BLOCK_LEN = 16,
    SIV_AES_KEY_BITS = 128,
};

static_assert(SIV_KEY_LEN == 2 * SIV_HALF_KEY_LEN,
              "AES-SIV-CMAC-256 key must split into K1 || K2");
static_assert(SIV_HALF_KEY_LEN == SIV_BLOCK_LEN,
              "AES-128 SIV half-key length must match the block length");

// GF(2^128) doubling used by S2V (RFC 5297 §2.3). Big-endian 16-byte
// block v is treated as a polynomial; this left-shifts by 1 and, if
// the top bit was set, XORs the reduction constant 0x87 into the
// low byte. This is the same "dbl" used by CMAC key derivation.
static void siv_dbl(uint8_t v[SIV_BLOCK_LEN])
{
    uint8_t carry = (uint8_t)(v[0] >> 7);
    for (int i = 0; i < SIV_BLOCK_LEN - 1; i++) {
        v[i] = (uint8_t)((v[i] << 1) | (v[i + 1] >> 7));
    }
    v[SIV_BLOCK_LEN - 1] = (uint8_t)(v[SIV_BLOCK_LEN - 1] << 1);
    if (carry) {
        v[SIV_BLOCK_LEN - 1] ^= 0x87;
    }
}

static void siv_xor16(uint8_t *dst, const uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < SIV_BLOCK_LEN; i++) {
        dst[i] = a[i] ^ b[i];
    }
}

// One-shot CMAC-AES-128. Wraps mbedtls_cipher_cmac() with a looked-up
// cipher_info. Returns 0 on success.
static int siv_cmac(const uint8_t *key16,
                    const uint8_t *in, size_t in_len,
                    uint8_t out[SIV_BLOCK_LEN])
{
    const mbedtls_cipher_info_t *info =
        mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB);
    if (info == NULL) {
        return -1;
    }
    return mbedtls_cipher_cmac(info, key16, SIV_AES_KEY_BITS, in, in_len, out);
}

// S2V -- the synthetic-IV construction from RFC 5297 §2.4. Absorbs
// nad AD components plus the plaintext "Sn" slot, returns a 16-byte
// authenticator that doubles as the CTR starting block.
static int siv_s2v(const uint8_t  *k1,
                   const SivSlice *ad, size_t nad,
                   const uint8_t  *pt, size_t pt_len,
                   uint8_t         out[SIV_BLOCK_LEN])
{
    static const uint8_t zero16[SIV_BLOCK_LEN] = { 0 };
    uint8_t d[SIV_BLOCK_LEN], cm[SIV_BLOCK_LEN];
    int rc;

    rc = siv_cmac(k1, zero16, SIV_BLOCK_LEN, d);
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

    if (pt_len >= SIV_BLOCK_LEN) {
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
        if (rc == 0) rc = mbedtls_cipher_cmac_starts(&ctx, k1, SIV_AES_KEY_BITS);
        if (rc != 0) {
            mbedtls_cipher_free(&ctx);
            goto done;
        }
        size_t head = pt_len - SIV_BLOCK_LEN;
        if (head > 0) {
            rc = mbedtls_cipher_cmac_update(&ctx, pt, head);
            if (rc != 0) {
                mbedtls_cipher_free(&ctx);
                goto done;
            }
        }
        uint8_t tail[SIV_BLOCK_LEN];
        siv_xor16(tail, pt + head, d);
        rc = mbedtls_cipher_cmac_update(&ctx, tail, SIV_BLOCK_LEN);
        if (rc == 0) rc = mbedtls_cipher_cmac_finish(&ctx, out);
        mbedtls_platform_zeroize(tail, sizeof tail);
        mbedtls_cipher_free(&ctx);
    } else {
        // T = dbl(D) XOR pad(pt), where pad is pt || 0x80 || 0x00*.
        uint8_t padded[SIV_BLOCK_LEN] = { 0 };
        if (pt_len > 0) {
            memcpy(padded, pt, pt_len);
        }
        padded[pt_len] = 0x80;
        siv_dbl(d);
        uint8_t t[SIV_BLOCK_LEN];
        siv_xor16(t, d, padded);
        rc = siv_cmac(k1, t, SIV_BLOCK_LEN, out);
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
static int siv_ctr(const uint8_t  k2[SIV_HALF_KEY_LEN],
                   const uint8_t  siv_in[SIV_BLOCK_LEN],
                   const uint8_t *in,
                   uint8_t       *out,
                   size_t         len)
{
    uint8_t nonce_counter[SIV_BLOCK_LEN];
    memcpy(nonce_counter, siv_in, SIV_BLOCK_LEN);
    nonce_counter[8]  &= 0x7f;     // bit 63 = MSB of V[8]  cleared
    nonce_counter[12] &= 0x7f;     // bit 31 = MSB of V[12] cleared

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int rc = mbedtls_aes_setkey_enc(&aes, k2, SIV_AES_KEY_BITS);
    if (rc == 0 && len > 0) {
        size_t nc_off = 0;
        uint8_t stream_block[SIV_BLOCK_LEN] = { 0 };
        rc = mbedtls_aes_crypt_ctr(&aes, len, &nc_off,
                                   nonce_counter, stream_block,
                                   in, out);
        mbedtls_platform_zeroize(stream_block, sizeof stream_block);
    }
    mbedtls_aes_free(&aes);
    mbedtls_platform_zeroize(nonce_counter, sizeof nonce_counter);
    return rc;
}

int Siv_Encrypt(const uint8_t   key[SIV_KEY_LEN],
                const SivSlice *ad, size_t nad,
                const uint8_t  *pt, size_t pt_len,
                uint8_t        *out)
{
    if (key == NULL || out == NULL) return -1;
    if (pt_len > 0 && pt == NULL)   return -1;
    if (nad > 0 && ad == NULL)      return -1;

    const uint8_t *k1 = key;
    const uint8_t *k2 = key + SIV_HALF_KEY_LEN;

    uint8_t siv[SIV_BLOCK_LEN];
    int rc = siv_s2v(k1, ad, nad, pt, pt_len, siv);
    if (rc != 0) {
        mbedtls_platform_zeroize(siv, sizeof siv);
        return rc;
    }

    memcpy(out, siv, SIV_BLOCK_LEN);
    if (pt_len > 0) {
        rc = siv_ctr(k2, siv, pt, out + SIV_BLOCK_LEN, pt_len);
    }
    mbedtls_platform_zeroize(siv, sizeof siv);
    return rc;
}

int Siv_Decrypt(const uint8_t   key[SIV_KEY_LEN],
                const SivSlice *ad, size_t nad,
                const uint8_t  *ct, size_t ct_len,
                uint8_t        *out_pt)
{
    if (key == NULL || ct == NULL) return -1;
    if (ct_len < SIV_BLOCK_LEN)    return -1;
    if (nad > 0 && ad == NULL)     return -1;

    size_t pt_len = ct_len - SIV_BLOCK_LEN;
    if (pt_len > 0 && out_pt == NULL) return -1;

    const uint8_t *k1 = key;
    const uint8_t *k2 = key + SIV_HALF_KEY_LEN;

    // Decrypt the ciphertext body under CTR, keyed by K2, using the
    // received SIV as the starting counter. We write the tentative
    // plaintext into out_pt and only expose it after verifying the
    // tag; on failure we zeroize it to avoid leaking unauthenticated
    // bytes to the caller.
    int rc = 0;
    if (pt_len > 0) {
        rc = siv_ctr(k2, ct, ct + SIV_BLOCK_LEN, out_pt, pt_len);
        if (rc != 0) {
            mbedtls_platform_zeroize(out_pt, pt_len);
            return rc;
        }
    }

    uint8_t expected[SIV_BLOCK_LEN];
    rc = siv_s2v(k1, ad, nad, out_pt, pt_len, expected);
    if (rc != 0) {
        if (pt_len > 0) mbedtls_platform_zeroize(out_pt, pt_len);
        mbedtls_platform_zeroize(expected, sizeof expected);
        return rc;
    }

    int eq = mbedtls_ct_memcmp(expected, ct, SIV_BLOCK_LEN);
    mbedtls_platform_zeroize(expected, sizeof expected);
    if (eq != 0) {
        if (pt_len > 0) mbedtls_platform_zeroize(out_pt, pt_len);
        return -1;
    }
    return 0;
}
