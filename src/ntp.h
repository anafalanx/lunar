// ntp.h -- Parallel SNTP + NTS client.
//
// Per cycle we fill SIX result slots in parallel: FOUR unauthenticated
// stratum-1 SNTP slots drawn at random from a curated pool of national
// metrology / research-lab time servers, PLUS TWO NTS-authenticated
// SNTP slots drawn from an NTS provider metadata pool (see src/nts.c).
// If a slot's primary server fails to respond, the worker immediately
// tries a distinct replacement inside the same cycle. Results are
// aggregated into a single trust verdict via Ntp_Concur. Both NTS
// anchors must succeed, come from different operator families, and
// mutually agree; at least 3 of the 4 core sources must also concur
// for the cycle to produce TRUST_OK -- anything else is TRUST_INOP.
// There is no intermediate tier: an accepted cycle also carries its
// MEASURED anchor uncertainty, and the certainty interval simply grows
// from there until the next accepted cycle (see clock.h).
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

typedef enum {
    NTP_AUTH_NONE = 0,
    NTP_AUTH_PLAIN_SNTP = 1,
    NTP_AUTH_ENROLLED_PIN = 2,
    // TLS leaf matched no stored pin outside the renewal window but
    // passed full Windows CA + hostname validation (early/emergency key
    // rotation). Counts toward the 2-NTS gate ONLY alongside a
    // continuous ENROLLED_PIN peer from a different operator family;
    // the new pin is persisted only after such a cycle passes the gate.
    NTP_AUTH_ROTATED_PIN = 3,
} NtpAuthMode;

// One source's outcome from the most recent polling cycle. `label` is
// a short static string ("NIST" / "PTB" / ... / "NTS:<provider>")
// owned by ntp.c. When ok==0 every other field is meaningless.
typedef struct {
    int         ok;          // 1 if this source returned a valid reply
    int64_t     offsetMs;    // (this source - cycle consensus) in ms, display-only
    int64_t     ntpUtcMs;    // server-believed UTC at QPC capture moment
    int64_t     qpcAtT4;     // QPC tick at t4 (reply received)
    uint32_t    rttMs;       // round-trip time in ms
    uint32_t    rootErrMs;   // server-claimed root dispersion + rootDelay/2
                             // (ms), parsed from the (NTS: authenticated)
                             // NTP header; feeds the measured anchor error
    const char *label;       // short source name, e.g. "NIST"
    NtpAuthMode authMode;    // plain SNTP, enrolled NTS pin, or pending rotation
    const char *operatorFamily;
} NtpSourceResult;

// Kick off one parallel polling cycle against all sources. Safe to call
// at any time; no-ops if a cycle is already in flight.
void    Ntp_Start(void);

// Stop accepting new cycles and wait briefly for the current aggregator
// and any detached workers to finish before process shutdown.
void    Ntp_Shutdown(void);

// True iff the clockwork has been anchored at least once this run (the
// UI uses this to distinguish NO SIGNAL from never-synced ACQUIRING).
int     Ntp_IsSynced(void);

// Legacy accessors: the offset and wall-clock UTC of the most recent
// successful sample. Zero when never synced.
int64_t Ntp_OffsetMs(void);
int64_t Ntp_LastSyncUtcMs(void);

// Largest deviation (ms) among successful sources from the last
// cycle's consensus anchor (projected to a common QPC moment). Zero
// before the first cycle.
int64_t Ntp_LastSpreadMs(void);

// Largest mutual spread (ms) between the two authenticated NTS anchors on the
// most recent cycle that had both. The poll scheduler gates cadence-relaxation
// on this being small, so it only backs off when the trust anchors agree tightly.
int64_t Ntp_LastNtsSpreadMs(void);

// Copy the latest per-source results out of ntp.c under its lock.
// Returns the number of sources that ok'd in that cycle (0..NTP_SOURCE_COUNT).
int     Ntp_GetResults(NtpSourceResult out[NTP_SOURCE_COUNT]);

// Pure concurrence evaluator. Given a set of per-source results, returns
// the trust verdict and, on TRUST_OK, the consensus utcMs, its matching
// QPC tick, and the measured anchor uncertainty.
//
// The NTS slots are the authenticated trust anchor; the core sources
// corroborate. Two paths:
//
//   TRUST_OK -- both NTS slots ok (enrolled or rotated pins), different
//     operator families, mutual agreement within 200 ms (projected to a
//     common QPC), AND >= 3 of 4 core sources within 200 ms of the NTS
//     midpoint; the anchor is that midpoint. A ROTATED_PIN slot
//     (CA-validated leaf that matched no stored pin outside the renewal
//     window) may count ONLY when the other slot is a continuous
//     ENROLLED_PIN -- an attacker must defeat a still-pinned independent
//     operator to exploit a rotation. Two ROTATED_PIN slots return
//     TRUST_INOP.
//
//   TRUST_INOP -- anything else. There is no intermediate tier: trust is
//     the measured certainty interval, and unauthenticated sources can
//     only widen it (see Clock_OnCoreWitness).
//
// On TRUST_OK, *outAnchorErrMs carries the MEASURED anchor uncertainty:
//   base = pairSpread/2 + worstNtsRtt/2 + serverRootErr (floor 15 ms),
//   widened to the concurring cores' median deviation from the midpoint
// -- honest under one-sided asymmetry and under correlated NTS error.
//
// On TRUST_INOP, if >= 3 core sources still mutually agree within 100 ms,
// their cluster is reported via *outClusterUtcMs/*outClusterQpc and
// *outHaveCluster=1 so the caller can feed the widen-only watchdog
// (Clock_OnCoreWitness). The cluster carries no trust of its own.
//
// This function is pure: no persistent state beyond the published spread
// side-channels, no I/O. Exposed so the test harness can exercise the
// math directly. Out-params may be NULL.
TrustState Ntp_Concur(const NtpSourceResult results[NTP_SOURCE_COUNT],
                      int64_t *outBestUtcMs,
                      int64_t *outBestQpc,
                      int64_t *outAnchorErrMs,
                      int64_t *outClusterUtcMs,
                      int64_t *outClusterQpc,
                      int     *outHaveCluster);

#ifdef LUNAR_TESTING
// Kiss-o'-death bookkeeping hooks (see ntp.c).
void Ntp_TestMarkKissOfDeath(int poolIdx, const char *kiss);
int  Ntp_TestEligibleCoreCount(void);
void Ntp_TestClearKissOfDeath(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
