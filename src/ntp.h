// ntp.h -- Tiny SNTP client. Kicks off a background sync and exposes the
// offset (NTP time minus system time) in milliseconds.
#ifndef LUNAR_NTP_H
#define LUNAR_NTP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Kick off a one-shot background sync. Safe to call at any time; no-ops if
// a sync is already in flight.
void    Ntp_Start(void);

// True iff we have a successful sync that is still considered fresh
// (currently: completed within the last 2 hours).
int     Ntp_IsSynced(void);

// Offset in milliseconds to add to the system clock to obtain NTP time.
// Zero when never synced.
int64_t Ntp_OffsetMs(void);

// Wall-clock UTC time (ms since Unix epoch) at which the last successful
// sync completed. Zero when never synced.
int64_t Ntp_LastSyncUtcMs(void);

#ifdef __cplusplus
}
#endif

#endif
