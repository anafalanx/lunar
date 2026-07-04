// clock.h -- NTP-disciplined monotonic clock ("the clockwork").
//
// Every read of UTC in the app goes through Clock_GetDisplay() (or the
// Clock_NowUtcMs() convenience wrapper). It is driven by
// QueryPerformanceCounter (monotonic while the machine stays awake),
// projected onto UTC through an anchor point (anchorUtcMs, anchorQpc)
// and a disciplined rate (rate_ppm: parts per million deviation from
// 1.0).
//
// The rate is updated on each accepted NTP cycle by a clamped integral
// controller on the residual drift since the previous anchor. Accepted
// residuals snap immediately to the newest trusted anchor; the UI never
// knowingly displays a cosmetically-slewed time that differs from the
// freshest trusted reading.
//
// The rate is persisted to %APPDATA%\Lunar\discipline.dat so a fresh
// start can run accurately even before the first NTP reply lands, but
// every run re-verifies the rate on its first sync.
//
// Display policy is FAIL-HONEST: once anchored, the clock keeps
// displaying a projected time for as long as the projection is
// physically defensible, together with an honest worst-case error
// bound that grows while no cycle corroborates it. The display only
// refuses a running time when the projection itself is invalid: before
// the first sync of a run (TRUST_INOP) or after a suspend/resume or
// session handoff broke QPC continuity (TRUST_REACQUIRING, which shows
// the last verified time plus its age instead). The Windows system
// clock is never used as a display source.

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

// --- Display states --------------------------------------------------------
//
// Ordered by confidence. TRUST_HOLDOVER and above carry a running
// projected time; TRUST_REACQUIRING carries only the last verified time
// plus its age; TRUST_INOP carries nothing.
typedef enum {
    TRUST_INOP        = 0,  // no renderable time: never anchored this run,
                            // or a local conversion fault was tripped
    TRUST_REACQUIRING = 1,  // anchor exists but QPC continuity was broken
                            // (suspend/resume, session handoff): show the
                            // last verified time + age, not a running dial;
                            // exits only via a full authenticated cycle
    TRUST_HOLDOVER    = 2,  // running projection with no live corroboration
                            // (offline, gate failing, or polling stalled):
                            // the error bound grows ~12 ms/min worst case
    TRUST_DEGRADED    = 3,  // NTS unavailable, but >= 3 unauthenticated core
                            // sources corroborate the held anchor within
                            // 100 ms and the last authenticated cycle is
                            // within the freshness window
    TRUST_OK          = 4,  // full authenticated consensus
} TrustState;

// Everything a painter or badge needs, captured atomically.
typedef struct {
    TrustState state;         // derived display state (includes staleness:
                              // an OK/DEGRADED older than the corroboration
                              // window is reported as TRUST_HOLDOVER)
    int64_t    utcMs;         // projected UTC ms; valid when
                              // state >= TRUST_HOLDOVER, else 0
    int64_t    boundMs;       // honest worst-case display error bound (ms);
                              // valid when state >= TRUST_HOLDOVER, else 0
    int64_t    lastSyncUtcMs; // UTC of the last accepted authenticated
                              // anchor (0 if never anchored this run)
    int64_t    lastSyncAgeMs; // wall age of that anchor, measured with
                              // GetTickCount64 so it keeps counting across
                              // sleep (drives the REACQUIRING/holdover age
                              // readouts)
    uint64_t   generation;    // display generation token, see below
} ClockDisplay;

// The canonical readout. Always fills *out (state may be TRUST_INOP).
void    Clock_GetDisplay(ClockDisplay *out);

// Convenience wrapper: writes the projected UTC ms and returns 1 when a
// running time exists (state >= TRUST_HOLDOVER); returns 0 otherwise and
// leaves *outMs untouched. The Windows system clock is never used as a
// display fallback.
int     Clock_NowUtcMs(int64_t *outMs);

// Same as Clock_NowUtcMs plus the generation token. Painters can verify
// the token immediately before presenting a dial; if it is stale, they
// must repaint from fresh state instead.
int     Clock_ReadDisplayTime(int64_t *outMs, uint64_t *outGeneration);

// True iff `generation` is still the current display generation. The
// clock lock is taken and released internally, so the lock is never held
// across the caller's (potentially slow, GPU-flushing) present. A painter
// validates the generation immediately BEFORE presenting a dial, and
// again immediately AFTER the present returns: if the post-present check
// fails, the dial it just presented is stale and the painter must repaint
// (the live window) or hand back a fresh bitmap (the taskbar thumbnail).
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

// --- Poll-cycle intake -----------------------------------------------------
//
// After each parallel NTP polling cycle, ntp.c calls Clock_OnPollCycle()
// with the concurrence verdict computed by Ntp_Concur. Cycle verdicts
// only ever use three of the enum values:
//   TRUST_OK       -- two operator-diverse NTS anchors and the required
//                     core SNTP super-majority concur within 200 ms.
//                     bestUtcMs / bestQpc update the anchor and rate,
//                     and restore QPC continuity after a resume.
//   TRUST_DEGRADED -- NTS is unavailable, but >= 3 core sources still
//                     corroborate within the tighter 100 ms gate and the
//                     last authenticated cycle is inside the freshness
//                     window. bestUtcMs / bestQpc are NOT used to
//                     re-anchor; they only cross-check the held
//                     projection. Corroboration keeps the claimed error
//                     bound tight but never steers the clockwork.
//   TRUST_INOP     -- the gate failed or no sources answered. The anchor
//                     is untouched; the display state derives to
//                     TRUST_HOLDOVER (anchored, continuity intact),
//                     TRUST_REACQUIRING (continuity broken) or TRUST_INOP
//                     (never anchored).
//
// A gate-passing cycle whose consensus disagrees with our running
// projection by more than 200 ms inflates the published error bound to
// cover the disagreement and reports TRUST_HOLDOVER; after
// LOCAL_FAULT_ESCAPE_N consecutive such cycles the consensus wins and
// the clockwork snaps to it with a prominent TIME STEP log entry (our
// anchor/rate are the broken party, not the network).
// maxPairSpreadMs is the largest pairwise offset difference from the
// cycle (used for audit / UI).
void    Clock_OnPollCycle(TrustState state,
                          int64_t bestUtcMs,
                          int64_t bestQpc,
                          int64_t maxPairSpreadMs);

// QPC continuity was broken or become untrustworthy: system
// suspend/resume, RDP session reconnect, or a timer gap long enough to
// suggest a missed suspend. The projection can no longer be defended,
// so the display drops to TRUST_REACQUIRING (last verified time + age)
// until the next full authenticated cycle re-anchors the clockwork.
void    Clock_OnContinuityBroken(const char *reason);

// Force the display state to hard INOP (no time at all). Reserved for
// genuinely unrenderable faults, e.g. local time conversion failure.
void    Clock_TripInop(const char *reason);

// Current derived display state (same value Clock_GetDisplay reports).
TrustState Clock_Trust(void);

#ifdef LUNAR_TESTING
// Age the last corroborating cycle / the last accepted anchor by ageMs,
// so staleness derivation and bound growth can be tested without
// sleeping.
void    Clock_TestAgeLastCycle(int64_t ageMs);
void    Clock_TestAgeAnchor(int64_t ageMs);
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
