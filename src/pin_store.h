// pin_store.h -- protected local SPKI enrollment cache.

#ifndef LUNAR_PIN_STORE_H
#define LUNAR_PIN_STORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PIN_ENDPOINT_DOH = 1,
    PIN_ENDPOINT_NTS = 2,
} PinEndpointKind;

typedef struct {
    int      present;
    uint8_t  spki[32];
    char     spki_hex[65];
    char     not_before[32];
    char     not_after[32];
    int64_t  not_before_unix;
    int64_t  not_after_unix;
    int64_t  renewal_due_unix;
    char     renewal_due[32];
    char     last_status[96];
} PinRecord;

int  PinStore_Init(void);
void PinStore_Shutdown(void);

int  PinStore_GetPin(PinEndpointKind kind,
                     const char *label,
                     const char *hostname,
                     uint16_t port,
                     PinRecord *out);

int  PinStore_ShouldRenew(const PinRecord *rec);
int  PinStore_IsExpired(const PinRecord *rec);

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