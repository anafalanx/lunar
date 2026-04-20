// clock.h -- NTP-disciplined monotonic clock ("the clockwork").
//
// Every read of UTC in the app goes through Clock_NowUtcMs(). It is
// driven by QueryPerformanceCounter (monotonic, unaffected by DST,
// Windows Time Service, sleeps, or user time changes), projected onto
// UTC through an anchor point (anchorUtcMs, anchorQpc) and a disciplined
// rate (rate_ppm: parts per million deviation from 1.0).
//
// The rate is updated on each successful NTP sample via a first-order
// PLL (EMA on the fractional drift since the previous anchor). Small
// residual errors (<= 2 s) are slewed smoothly over ~60 s; larger
// errors (cold boot, sleep-wake, user set-time) are snapped by moving
// the anchor.
//
// The rate is persisted to %APPDATA%\Lunar\discipline.dat so a fresh
// start can run accurately even before the first NTP reply lands, but
// every run re-verifies the rate on its first sync.
//
// Before any sync this run AND with no trustworthy persisted state,
// Clock_NowUtcMs() returns the raw Windows system clock.

#ifndef LUNAR_CLOCK_H
#define LUNAR_CLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called once at startup. Reads persisted rate.
void    Clock_Init(void);

// Called at shutdown (WM_CLOSE). Persists the current rate to disk.
void    Clock_Shutdown(void);

// The canonical time readout. Returns UTC milliseconds since Unix epoch,
// disciplined whenever possible, raw system time otherwise.
int64_t Clock_NowUtcMs(void);

// True once we have at least one NTP sample anchoring the clockwork in
// THIS run. A loaded rate from disk alone does not count.
int     Clock_IsDisciplined(void);

// Current rate deviation from nominal, in parts per million. Positive
// means our clockwork is running FASTER than the true second. Zero
// before the second sync of a session (one sample can't yet measure a
// rate).
int32_t Clock_RatePpm(void);

// Offset from the raw Windows system clock, in milliseconds
// (disciplined time minus system time). Zero before the first sync.
int64_t Clock_OffsetMs(void);

// Fed by ntp.c after a validated SNTP reply: ntpUtcMs is what the NTP
// server believes UTC was "now" as of this call. localQpc is the QPC
// value captured when t4 was measured, so we don't charge the network
// round-trip twice.
void    Clock_OnSyncedNtpUtc(int64_t ntpUtcMs, int64_t localQpc);

// Convenience for ntp.c: read the QPC at a defined moment.
int64_t Clock_Qpc(void);

#ifdef __cplusplus
}
#endif

#endif
