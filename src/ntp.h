// ntp.h -- Parallel SNTP client. Queries three independent national
// metrology institutes (NIST USA, PTB Germany, NICT Japan) in parallel
// and exposes per-source results for concurrence checking.
#ifndef LUNAR_NTP_H
#define LUNAR_NTP_H

#include <stdint.h>
#include "clock.h"   // for TrustState (used by Ntp_Concur)

#ifdef __cplusplus
extern "C" {
#endif

#define NTP_SOURCE_COUNT 3

// One source's outcome from the most recent polling cycle. `label` is a
// static string ("NIST" / "PTB" / "NICT") owned by ntp.c. When ok==0
// every other field is meaningless.
typedef struct {
    int         ok;          // 1 if this source returned a valid reply
    int64_t     offsetMs;    // (NTP - local system) in ms
    int64_t     ntpUtcMs;    // server-believed UTC at QPC capture moment
    int64_t     qpcAtT4;     // QPC tick at t4 (reply received)
    uint32_t    rttMs;       // round-trip time in ms
    const char *label;       // short source name, e.g. "NIST"
} NtpSourceResult;

// Kick off one parallel polling cycle against all sources. Safe to call
// at any time; no-ops if a cycle is already in flight.
void    Ntp_Start(void);

// True iff ANY source has succeeded within the fresh window (2 h).
// The concurrence-based trust state lives in a separate module
// (added in the next step); this accessor stays behavior-compatible.
int     Ntp_IsSynced(void);

// Legacy accessors: the offset and wall-clock UTC of the most recent
// successful sample (median pick of the trio). Zero when never synced.
int64_t Ntp_OffsetMs(void);
int64_t Ntp_LastSyncUtcMs(void);

// Largest pairwise offset spread from the most recent polling cycle,
// in milliseconds. Zero before the first cycle. Updated regardless
// of trust verdict (useful in the title bar / audit readout).
int64_t Ntp_LastSpreadMs(void);

// Copy the latest per-source results out of ntp.c under its lock.
// Returns the number of sources that ok'd in that cycle (0..NTP_SOURCE_COUNT).
int     Ntp_GetResults(NtpSourceResult out[NTP_SOURCE_COUNT]);

// Pure concurrence evaluator. Given a set of per-source results,
// returns the trust verdict and, when the verdict is TRUST_OK, the
// best (median) utcMs and its matching QPC tick. maxSpreadMs receives
// the largest pairwise offset spread among successful sources (0 if
// fewer than 2 succeeded).
//
// Verdict rules (binary, no degraded middle ground):
//   3 ok + max pairwise spread <= 200 ms  -> TRUST_OK (median feeds)
//   anything else                         -> TRUST_INOP
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
