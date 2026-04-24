// ntp.h -- Parallel SNTP + NTS client.
//
// Per cycle we query SIX sources in parallel: FOUR unauthenticated
// stratum-1 SNTP sources drawn at random from a curated pool of
// national metrology / research-lab time servers, PLUS TWO NTS-
// authenticated SNTP sources drawn from a pinned NTS provider pool
// (see src/nts.c). Results are aggregated into a single trust verdict
// via Ntp_Concur. Both NTS anchors must succeed and mutually agree,
// and at least 3 of the 4 core sources must concur with them, for the
// cycle to produce TRUST_OK.
#ifndef LUNAR_NTP_H
#define LUNAR_NTP_H

#include <stdint.h>
#include "clock.h"   // for TrustState (used by Ntp_Concur)

#ifdef __cplusplus
extern "C" {
#endif

// Slot layout: 0..3 are the four core (plain-SNTP) sources; 4..5 are
// the two NTS-authenticated sources. Each NTS slot's `label` is filled
// in at runtime from the provider picked for that cycle, or
// "NTS--" / "NTS:?" if no provider is available (e.g. no pins
// populated in this build).
#define NTP_SOURCE_COUNT      6
#define NTP_CORE_COUNT        4
#define NTP_NTS_COUNT         2
#define NTP_FIRST_NTS_SLOT    NTP_CORE_COUNT   /* = 4 */

// One source's outcome from the most recent polling cycle. `label` is
// a short static string ("NIST" / "PTB" / ... / "NTS:<provider>")
// owned by ntp.c. When ok==0 every other field is meaningless.
typedef struct {
    int         ok;          // 1 if this source returned a valid reply
    int64_t     offsetMs;    // (this source - cycle consensus) in ms, display-only
    int64_t     ntpUtcMs;    // server-believed UTC at QPC capture moment
    int64_t     qpcAtT4;     // QPC tick at t4 (reply received)
    uint32_t    rttMs;       // round-trip time in ms
    const char *label;       // short source name, e.g. "NIST"
} NtpSourceResult;

// Kick off one parallel polling cycle against all sources. Safe to call
// at any time; no-ops if a cycle is already in flight.
void    Ntp_Start(void);

// True iff the clockwork has been anchored within the fresh window.
// Unchanged contract from the 3-source era.
int     Ntp_IsSynced(void);

// Legacy accessors: the offset and wall-clock UTC of the most recent
// successful sample. Zero when never synced.
int64_t Ntp_OffsetMs(void);
int64_t Ntp_LastSyncUtcMs(void);

// Largest deviation (ms) among successful sources from the last
// cycle's consensus anchor (projected to a common QPC moment). Zero
// before the first cycle.
int64_t Ntp_LastSpreadMs(void);

// Copy the latest per-source results out of ntp.c under its lock.
// Returns the number of sources that ok'd in that cycle (0..NTP_SOURCE_COUNT).
int     Ntp_GetResults(NtpSourceResult out[NTP_SOURCE_COUNT]);

// Pure concurrence evaluator. Given a set of per-source results,
// returns the trust verdict and, when the verdict is TRUST_OK, the
// best utcMs and its matching QPC tick. maxSpreadMs receives the
// largest absolute deviation of a core source from the NTS anchor
// (0 if fewer than one core succeeded).
//
// Verdict rules (binary; no degraded middle ground). The NTS slots
// are the trust anchor; the core sources corroborate.
//
//   Both NTS slots ok AND they mutually agree to within 200 ms
//     (projected to a common QPC) AND >= 3 of 4 core sources agree
//     with the NTS midpoint to within 200 ms        -> TRUST_OK,
//     anchor = midpoint of the two NTS samples.
//
//   Exactly one NTS slot ok AND ALL FOUR core sources agree with it
//     to within 200 ms                              -> TRUST_OK,
//     anchor = the surviving NTS sample. This is a strict fallback
//     so that a single-NTS-outage cycle can still discipline the
//     clock, but only when the core trio-plus-one unanimously
//     corroborates the authenticated reading.
//
//   Both NTS slots fail                             -> TRUST_INOP
//   NTS slots disagree by > 200 ms                  -> TRUST_INOP
//   Too few core sources concur with the NTS anchor -> TRUST_INOP
//
// This function is pure: no globals, no I/O. Exposed here so the test
// harness can exercise the math directly.
TrustState Ntp_Concur(const NtpSourceResult results[NTP_SOURCE_COUNT],
                      int64_t *outBestUtcMs,
                      int64_t *outBestQpc,
                      int64_t *outMaxSpreadMs);

#ifdef __cplusplus
}
#endif

#endif
