// ntp.h -- Parallel SNTP + NTS client. Queries three independent
// unauthenticated stratum-1 SNTP sources (NIST USA, PTB Germany,
// NICT Japan) plus, in slot 3, one NTS-authenticated SNTP source
// drawn per-cycle from a rotating pool (see src/nts.c). Exposes
// per-source results for concurrence checking.
#ifndef LUNAR_NTP_H
#define LUNAR_NTP_H

#include <stdint.h>
#include "clock.h"   // for TrustState (used by Ntp_Concur)

#ifdef __cplusplus
extern "C" {
#endif

// Four slots: 0..2 are the three core SNTP sources, slot 3 is the
// NTS-authenticated rotating source. The NTS slot's `label` is filled
// in at runtime from the provider picked for that cycle, or "NTS--"
// if no provider is available (e.g. no pins populated in this build).
#define NTP_SOURCE_COUNT     4
#define NTP_CORE_COUNT       3
#define NTP_NTS_SLOT         3

// One source's outcome from the most recent polling cycle. `label` is a
// short static string ("NIST" / "PTB" / "NICT" / "NTS:<provider>"
// owned by ntp.c). When ok==0 every other field is meaningless.
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

// Largest deviation (ms) among successful sources from the last cycle's
// consensus (projected to a common QPC moment). Zero before the first
// cycle.
int64_t Ntp_LastSpreadMs(void);

// Copy the latest per-source results out of ntp.c under its lock.
// Returns the number of sources that ok'd in that cycle (0..NTP_SOURCE_COUNT).
int     Ntp_GetResults(NtpSourceResult out[NTP_SOURCE_COUNT]);

// Pure concurrence evaluator. Given a set of per-source results,
// returns the trust verdict and, when the verdict is TRUST_OK, the
// best utcMs and its matching QPC tick. maxSpreadMs receives the
// largest absolute deviation of a source from the consensus anchor
// (0 if fewer than 2 sources succeeded).
//
// Verdict rules (binary; no degraded middle ground). NTS slot is the
// trust anchor: if NTS fails, the verdict is INOP regardless of what
// the core trio reports. The NTS sample is cryptographically
// authenticated, so an adversary spoofing plain SNTP cannot shift the
// anchor without also defeating TLS + the SPKI pin.
//
//   NTS slot not ok                                    -> TRUST_INOP
//   NTS ok, fewer than 2 core sources agree (<= 200 ms
//     projected offset from the NTS anchor)            -> TRUST_INOP
//   NTS ok, >= 2 of 3 core sources agree               -> TRUST_OK,
//     anchor = NTS (utcMs, qpcAtT4).
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
