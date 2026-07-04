// pin_store.h -- protected local SPKI enrollment cache.

#ifndef LUNAR_PIN_STORE_H
#define LUNAR_PIN_STORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PIN_ENDPOINT_DOH = 1,
    PIN_ENDPOINT_NTS = 2,
} PinEndpointKind;

// Maximum enrolled SPKIs retained per endpoint. Anycast / multi-POP
// providers legitimately present different leaf keys per POP, so a
// roaming client would flap between "pinned" and "mismatch" with a
// single stored key. Keeping a small set of recently observed,
// CA-validated keys absorbs that; the oldest entry is evicted when the
// set is full.
#define PIN_STORE_MAX_SPKIS 4

// One enrolled SPKI observation: a leaf public key plus the validity
// metadata of the certificate it was observed in.
typedef struct {
    uint8_t  spki[32];
    char     spki_hex[65];
    char     not_before[32];
    char     not_after[32];
    int64_t  not_before_unix;
    int64_t  not_after_unix;
    int64_t  renewal_due_unix;
    char     renewal_due[32];
    char     last_status[96];
} PinSpki;

typedef struct {
    int      present;
    // Newest enrolled SPKI, mirrored here for single-pin callers and
    // for record-level renewal/expiry decisions. Always identical to
    // spkis[spki_count - 1] when present != 0.
    uint8_t  spki[32];
    char     spki_hex[65];
    char     not_before[32];
    char     not_after[32];
    int64_t  not_before_unix;
    int64_t  not_after_unix;
    int64_t  renewal_due_unix;
    char     renewal_due[32];
    char     last_status[96];
    // Full enrolled set, ordered oldest -> newest.
    size_t   spki_count;
    PinSpki  spkis[PIN_STORE_MAX_SPKIS];
} PinRecord;

int  PinStore_Init(void);
void PinStore_Shutdown(void);

int  PinStore_GetPin(PinEndpointKind kind,
                     const char *label,
                     const char *hostname,
                     uint16_t port,
                     PinRecord *out);

// Record-level helpers, keyed on the NEWEST enrolled SPKI (the mirror
// fields above). Older set members may expire earlier or later; use
// PinStore_CollectValidSpkis for per-key expiry.
int  PinStore_ShouldRenew(const PinRecord *rec);
int  PinStore_IsExpired(const PinRecord *rec);

// Adaptive renewal margin in seconds: min(30 days, validity / 3) where
// validity = not_after - not_before of the observed leaf. Short-lived
// leaves (47-day, 6-day) therefore renew after two thirds of their
// life instead of being permanently "renewal due". Unknown or garbled
// validity metadata falls back to the fixed 30-day cap. Pure.
int64_t PinStore_RenewalMarginSeconds(int64_t not_before_unix,
                                      int64_t not_after_unix);

// Copy the un-expired SPKI hashes of a fetched record into `out`,
// oldest -> newest. Returns the number written (0..PIN_STORE_MAX_SPKIS).
size_t PinStore_CollectValidSpkis(const PinRecord *rec,
                                  uint8_t out[PIN_STORE_MAX_SPKIS][32]);

// Append (or refresh) one CA-validated SPKI observation for the
// endpoint. A key already in the set has its validity/status updated
// and becomes the newest member; a new key appends, evicting the
// oldest member when the set is full.
int  PinStore_SavePin(PinEndpointKind kind,
                      const char *label,
                      const char *hostname,
                      uint16_t port,
                      const char *operator_family,
                      const uint8_t spki[32],
                      const char spki_hex[65],
                      const char *not_before,
                      const char *not_after,
                      int64_t not_before_unix,
                      int64_t not_after_unix,
                      const char *status);

#ifdef LUNAR_TESTING
void PinStore_TestReset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
