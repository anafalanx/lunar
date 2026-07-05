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
// for the cycle to produce TRUST_OK. If NTS is unavailable but >= 3 core
// sources still agree within a tighter 100 ms gate and a full OK occurred
// within the last eight hours, the cycle produces the unauthenticated
// TRUST_DEGRADED state instead (see clock.h).
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

// True iff the clockwork has been anchored within the fresh window.
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
// the trust verdict and, on TRUST_OK / TRUST_DEGRADED, the consensus
// utcMs and its matching QPC tick. maxSpreadMs receives the largest
// absolute deviation of a corroborating source from the consensus.
//
// The NTS slots are the authenticated trust anchor; the core sources
// corroborate. Two paths:
//
//   Path 1 -- both NTS slots ok (enrolled or rotated pins): the cycle must
//     reach a full OK or hard-fail. TRUST_OK requires different operator
//     families AND mutual agreement within 200 ms (projected to a common
//     QPC) AND >= 3 of 4 core sources within 200 ms of the NTS midpoint;
//     the anchor is that midpoint. A ROTATED_PIN slot (CA-validated leaf
//     that matched no stored pin outside the renewal window) may count
//     ONLY when the other slot is a continuous ENROLLED_PIN -- an attacker
//     must defeat a still-pinned independent operator to exploit a
//     rotation. Two ROTATED_PIN slots return TRUST_INOP. A same-family,
//     disagreeing, or under-corroborated NTS pair returns TRUST_INOP -- a
//     conflicting authenticated layer is never downgraded to DEGRADED.
//
//   Path 2 -- fewer than two ok NTS slots (NTS unavailable): if >= 3 of 4
//     core sources mutually agree within the tighter 100 ms gate, returns
//     TRUST_DEGRADED with the core consensus in the out-params. The caller
//     gates this on the last TRUST_OK being recent and uses the consensus
//     only to cross-check the held anchor. Otherwise TRUST_INOP.
//
// This function is pure: no globals, no I/O. The freshness window for
// DEGRADED is applied by the caller, not here. Exposed so the test
// harness can exercise the math directly.
TrustState Ntp_Concur(const NtpSourceResult results[NTP_SOURCE_COUNT],
                      int64_t *outBestUtcMs,
                      int64_t *outBestQpc,
                      int64_t *outMaxSpreadMs);

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
