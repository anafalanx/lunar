// clock.h -- NTP-disciplined monotonic clock ("the clockwork").
//
// Every read of UTC in the app goes through Clock_NowUtcMs(). It is
// driven by QueryPerformanceCounter (monotonic, unaffected by DST,
// Windows Time Service, sleeps, or user time changes), projected onto
// UTC through an anchor point (anchorUtcMs, anchorQpc) and a disciplined
// rate (rate_ppm: parts per million deviation from 1.0).
//
// The rate is updated on each successful NTP sample via a first-order
// PLL (EMA on the fractional drift since the previous anchor). Accepted
// residuals snap immediately to the newest trusted anchor; the UI never
// knowingly displays a cosmetically-slewed time that differs from the
// freshest trusted reading.
//
// The rate is persisted to %APPDATA%\Lunar\discipline.dat so a fresh
// start can run accurately even before the first NTP reply lands, but
// every run re-verifies the rate on its first sync.
//
// Before any successful sync THIS run, or after the trusted display
// lease expires, Clock_NowUtcMs() refuses to return a time (returns 0).
// The Windows system clock is never used as a display source; callers
// must render an "INOP" indicator in that case.

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

// The canonical time readout. On success writes disciplined UTC
// milliseconds since Unix epoch into *outMs and returns 1. If the
// clockwork has NOT been anchored by a successful NTP sync this run, or
// if the display lease from the last trusted cycle has expired, returns
// 0 and leaves *outMs untouched. Callers MUST check the return value --
// the Windows system clock is never used as a display fallback.
int     Clock_NowUtcMs(int64_t *outMs);

// Same canonical display readout, plus a generation token that remains
// valid only while the display trust state and anchor/rate backing the
// dial are unchanged. Painters can verify the token immediately before
// presenting a dial; if it is stale, they must render INOP instead.
int     Clock_ReadDisplayTime(int64_t *outMs, uint64_t *outGeneration);

// True iff `generation` is still the current trusted display generation.
// Also fails closed if the display lease expired while checking. The
// clock lock is taken and released internally, so the lock is never held
// across the caller's (potentially slow, GPU-flushing) present. A painter
// validates the generation immediately BEFORE presenting a dial, and
// again immediately AFTER the present returns: if the post-present check
// fails, the dial it just presented is stale and the painter must repaint
// (the live window) or hand back an INOP bitmap (the taskbar thumbnail).
int     Clock_DisplayGenerationIsCurrent(uint64_t generation);

// True once we have at least one NTP sample anchoring the clockwork in
// THIS run. A loaded rate from disk alone does not count.
int     Clock_IsDisciplined(void);

// Current rate deviation from nominal, in parts per million. Positive
// means our clockwork is running FASTER than the true second. Zero
// before the second sync of a session (one sample can't yet measure a
// rate).
int32_t Clock_RatePpm(void);

// Fed by ntp.c after a validated SNTP reply: ntpUtcMs is what the NTP
// server believes UTC was "now" as of this call. localQpc is the QPC
// value captured when t4 was measured, so we don't charge the network
// round-trip twice.
void    Clock_OnSyncedNtpUtc(int64_t ntpUtcMs, int64_t localQpc);

// --- Trust state machine -------------------------------------------------
//
// After each parallel NTP polling cycle, ntp.c calls Clock_OnPollCycle()
// with the binary concurrence verdict computed by Ntp_Concur:
//   TRUST_OK   -- authenticated NTS anchor(s) and the required core
//                 SNTP super-majority concur within 200 ms.
//   TRUST_INOP -- the cycle failed the NTP concurrence gate, or the
//                 agreed time disagrees with our running projection by
//                 more than 200 ms (suggests local oscillator fault).
//
// While in TRUST_INOP, Clock_NowUtcMs() returns 0 even if a valid anchor
// was previously established: higher layers must render INOP. Only
// a successful NTS-anchored concurrence verdict counts as trusted;
// there is no degraded middle ground.
typedef enum {
    TRUST_INOP = 0,
    TRUST_OK   = 1,
} TrustState;

// Called by ntp.c at the end of every polling cycle. If state is OK,
// bestUtcMs / bestQpc are used to update the anchor (same semantics
// as Clock_OnSyncedNtpUtc). If state is INOP, the anchor is NOT
// updated and Clock_NowUtcMs() will refuse to return a time.
// maxPairSpreadMs is the largest pairwise offset difference from the
// cycle (used for audit / UI).
void    Clock_OnPollCycle(TrustState state,
                          int64_t bestUtcMs,
                          int64_t bestQpc,
                          int64_t maxPairSpreadMs);

// Force the display trust state to INOP immediately. Used by the UI for
// safety events such as resume/timer gaps or local-time conversion
// failures. The old anchor is retained for diagnostics but will not be
// returned by Clock_NowUtcMs() until a new trusted poll cycle succeeds.
void    Clock_TripInop(const char *reason);

// Current trust state. TRUST_INOP before the first concurrence-valid
// cycle and whenever the last cycle failed to concur.
TrustState Clock_Trust(void);

#ifdef LUNAR_TESTING
void    Clock_TestExpireDisplayLease(void);
#endif

// Convenience for ntp.c: read the QPC at a defined moment.
int64_t Clock_Qpc(void);

// QPC tick frequency (ticks per second). Needed by callers that
// measure intervals themselves (e.g. the SNTP worker timing its
// round-trip in QPC ticks).
int64_t Clock_QpcFreq(void);

#ifdef __cplusplus
}
#endif

#endif
