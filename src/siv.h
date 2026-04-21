// siv.h -- AES-SIV-CMAC-256 (RFC 5297), the AEAD mandated by NTS
// (RFC 8915, §5.1) for authenticating SNTP packets.
//
// SIV is a nonce-misuse-resistant AEAD: reusing a nonce with the same
// key does not leak the plaintext beyond revealing that the messages
// were identical (deterministic "IV re-use is catastrophic" problem
// that GCM has does not apply here). This makes SIV well-suited to
// the NTS setting where a client may occasionally re-send a query.
//
// "256" in the name is the *total* key material (32 bytes), split
// into K1 = key[0..16] used for CMAC / S2V, and K2 = key[16..32]
// used for AES-CTR encryption. The underlying block cipher is AES-128
// for both halves.
//
// This module depends only on mbedTLS (CMAC + AES-CTR primitives).
// It does not touch the network, the clock, or any global state.

#ifndef LUNAR_SIV_H
#define LUNAR_SIV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One component of the associated-data vector. For NTS, the AD vector
// contains a single element: the entire NTP packet header up to (but
// not including) the NTS Authenticator extension field.
typedef struct {
    const uint8_t *p;
    size_t         n;
} SivSlice;

// AES-SIV-CMAC-256 encryption.
//
//   key      32-byte key (K1 || K2).
//   ad       array of associated-data components. Per RFC 5297 §2.6,
//            when used as a nonce-based AEAD, the *last* AD element is
//            the nonce. For NTS the caller supplies a single AD element
//            (the NTP header) and no explicit nonce -- the SIV tag
//            itself becomes the nonce, and RFC 8915 §5.7 requires the
//            nonce field in the NTS Authenticator to carry it.
//   nad      number of AD components (may be 0).
//   pt       plaintext bytes (may be NULL iff pt_len == 0).
//   pt_len   plaintext length.
//   out      destination buffer, MUST be at least (pt_len + 16) bytes.
//            On success: out[0..16]   = 16-byte SIV tag (== synthetic IV).
//                        out[16..16+pt_len] = CTR-encrypted plaintext.
//
// Returns 0 on success, non-zero on failure (e.g. mbedTLS internal error).
int Siv_Encrypt(const uint8_t  key[32],
                const SivSlice *ad, size_t nad,
                const uint8_t  *pt, size_t pt_len,
                uint8_t        *out);

// AES-SIV-CMAC-256 decryption.
//
//   key      32-byte key (K1 || K2).
//   ad       same AD vector that was used for encryption.
//   nad      ditto.
//   ct       tag||ciphertext. First 16 bytes are the SIV; remaining
//            (ct_len - 16) bytes are the CTR-encrypted payload.
//   ct_len   total length of ct, MUST be >= 16.
//   out_pt   destination buffer for decrypted plaintext, size
//            (ct_len - 16) bytes. Must not alias ct.
//
// Returns 0 on authentic decryption, -1 on authentication failure
// (wrong key, tampered AD, tampered ciphertext, or short input).
// The comparison against the recomputed tag is constant-time.
int Siv_Decrypt(const uint8_t  key[32],
                const SivSlice *ad, size_t nad,
                const uint8_t  *ct, size_t ct_len,
                uint8_t        *out_pt);

#ifdef __cplusplus
}
#endif

#endif
