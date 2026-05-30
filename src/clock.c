// clock.c -- NTP-disciplined monotonic clockwork. See clock.h for the
// design overview.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

#include "clock.h"
#include "logbuf.h"
#include "app_paths.h"

// NOTE: Log_Append() internally reads Clock_NowUtcMs(), which also
// takes g_cs. Every Log_Append call site in this file therefore runs
// AFTER the CS has been released, never while it's held.

// --- Shared state ---------------------------------------------------------

// Updates to the anchor set are serialized behind a CS so Paint() (UI
// thread) and SyncThreadProc (NTP worker) can't get a torn read.
static CRITICAL_SECTION g_cs;
static int        g_csInit       = 0;

static int64_t    g_qpcFreq      = 1;    // ticks per second
static int64_t    g_anchorQpc    = 0;    // QPC at anchor
static int64_t    g_anchorUtcMs  = 0;    // disciplined UTC at anchor (ms)
static int32_t    g_ratePpm      = 0;    // parts per million offset from 1.0
static int32_t    g_loadedRatePpm = 0;   // rate read from disk at startup
static int64_t    g_loadedLastSync = 0;  // UTC at save time (for staleness chk)
static int        g_haveSample   = 0;    // at least one NTP sample THIS run
static int        g_haveRate     = 0;    // two+ samples: rate is measured
static int64_t    g_lastSyncUtcMs = 0;   // UTC at last successful sync
static int64_t    g_lastSyncQpc   = 0;   // QPC at last successful sync
static int64_t    g_displayLeaseUntilQpc = 0; // QPC deadline for dial display
static uint64_t   g_displayGeneration = 1; // bumps on every display-state change

// Trust state published by ntp.c after each polling cycle. Starts at
// TRUST_INOP and stays there until the first cycle achieves concurrence.
static TrustState g_trust         = TRUST_INOP;

// Counts consecutive local-oscillator-fault rejections. If the NTP
// cycle passes the concurrence gate (two operator-diverse NTS anchors
// agree, plus a core super-majority) but our running projection
// disagrees by >200 ms for several cycles in a row, our anchor/rate
// are clearly wrong -- an automatic escape hatch forces a snap after
// LOCAL_FAULT_ESCAPE_N faults to recover without user intervention.
static int        g_consecutiveLocalFaults = 0;
#define LOCAL_FAULT_ESCAPE_N  3

// A trusted poll grants the UI a short display lease. If that lease is
// not renewed by another concurrence-valid cycle, Clock_NowUtcMs()
// fails closed even though the old anchor can still be projected.
#define CLOCK_DISPLAY_LEASE_MS  90000

// --- Discipline-loop constants --------------------------------------------
//
// Rate control is a PI loop where the integral term is the per-cycle
// rate correction:
//
//   observed_ppm  = residual * 1e6 / interval_ms
//   delta_ppm     = observed_ppm * KI_NUM / KI_DEN       -- integral step
//   delta_ppm     = clamp(delta_ppm, ±PER_CYCLE_RATE_CLAMP_PPM)
//   new_rate_ppm  = clamp(old + delta, ±RATE_CLAMP_PPM)
//
// Phase correction is absorbed by re-anchoring the clockwork to the
// server's reading on every sync. The display snaps immediately to
// the accepted anchor; Lunar does not knowingly show a smoothed value
// that differs from the freshest trusted reading.
#define RATE_CLAMP_PPM              200    // realistic quartz: ±50 ppm typ
#define PER_CYCLE_RATE_CLAMP_PPM    20     // max rate swing per single cycle
#define KI_NUM                      1      // integral gain = 1/8 (gentle)
#define KI_DEN                      8

int64_t Clock_Qpc(void) {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}

int64_t Clock_QpcFreq(void) {
    return g_qpcFreq;
}

static int64_t MsToQpcTicks(int64_t ms) {
    return (ms * g_qpcFreq + 999LL) / 1000LL;
}

static void Clock_GrantDisplayLeaseLocked(void) {
    g_displayLeaseUntilQpc = Clock_Qpc() + MsToQpcTicks(CLOCK_DISPLAY_LEASE_MS);
    g_displayGeneration++;
    if (g_displayGeneration == 0) g_displayGeneration = 1;
}

static void Clock_TripInopLocked(void) {
    int changed = (g_trust != TRUST_INOP || g_displayLeaseUntilQpc != 0);
    g_trust = TRUST_INOP;
    g_displayLeaseUntilQpc = 0;
    if (changed) {
        g_displayGeneration++;
        if (g_displayGeneration == 0) g_displayGeneration = 1;
    }
}

static int Clock_DisplayLeaseExpiredLocked(int64_t nowQpc) {
    return g_haveSample && g_trust != TRUST_INOP &&
           (g_displayLeaseUntilQpc <= 0 || nowQpc > g_displayLeaseUntilQpc);
}

static void LoadDiscipline(void) {
    g_loadedRatePpm = 0;
    wchar_t path[MAX_PATH];
    if (!Lunar_AppDataPathW(path, MAX_PATH, L"discipline.dat")) return;
    FILE *f = _wfopen(path, L"rb");
    if (!f) return;
    char buf[128] = {0};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    int rate = 0;
    long long lastSync = 0;
    if (sscanf(buf, "%d %lld", &rate, &lastSync) < 1) return;

    // Staleness: we USED to reject rates older than 30 days here via
    // the Windows system clock. That would reintroduce a system-clock
    // dependency, so we defer the check: load the rate tentatively,
    // and re-evaluate staleness against our own disciplined UTC after
    // the first successful NTP cycle (see Clock_OnSyncedNtpUtc).
    // Worst-case the value is stale -- in which case the first sync's
    // residual will be large; the PLL absorbs it into a snap.
    (void)lastSync;
    if (rate >  RATE_CLAMP_PPM) rate =  RATE_CLAMP_PPM;
    if (rate < -RATE_CLAMP_PPM) rate = -RATE_CLAMP_PPM;
    g_loadedRatePpm   = rate;
    g_loadedLastSync  = lastSync;
}

void Clock_Init(void) {
    if (!g_csInit) {
        InitializeCriticalSection(&g_cs);
        g_csInit = 1;
    }
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    g_qpcFreq = f.QuadPart ? f.QuadPart : 1;

    LoadDiscipline();
    // Persisted rate is a bootstrap hint only. It won't be applied
    // until we have our first anchor (first successful NTP sync this
    // run). Until then NowUtcMs() returns the raw system clock.
    g_ratePpm    = 0;
    g_haveSample = 0;
    g_haveRate   = 0;
    g_lastSyncUtcMs = 0;
    g_lastSyncQpc = 0;
    g_displayLeaseUntilQpc = 0;
    g_trust = TRUST_INOP;
    g_consecutiveLocalFaults = 0;
    g_displayGeneration++;
    if (g_displayGeneration == 0) g_displayGeneration = 1;

    if (g_loadedRatePpm != 0) {
        Log_Append("clock: persisted rate %+d ppm loaded from disk "
                   "(bootstrap; will be re-verified on first sync)",
                   (int)g_loadedRatePpm);
    } else {
        Log_Append("clock: no persisted rate on disk (fresh discipline)");
    }
}

void Clock_Shutdown(void) {
    if (!g_haveRate) {
        Log_Append("clock: shutdown \xe2\x80\x94" " rate not persisted "
                   "(only one sample this run)");
        return;
    }
    wchar_t path[MAX_PATH];
    if (!Lunar_AppDataPathW(path, MAX_PATH, L"discipline.dat")) return;
    FILE *f = _wfopen(path, L"wb");
    if (!f) return;
    fprintf(f, "%d %lld\n", (int)g_ratePpm, (long long)g_lastSyncUtcMs);
    fclose(f);
    Log_Append("clock: shutdown \xe2\x80\x94" " persisted rate=%+d ppm",
               (int)g_ratePpm);
}

// Project a QPC tick onto disciplined UTC ms using current anchor + rate.
// The anchor (g_anchorQpc, g_anchorUtcMs) always represents our best
// belief of truth at that QPC moment. Between syncs we project forward
// using g_ratePpm.
static int64_t ProjectLocked(int64_t qpc) {
    int64_t dQ = qpc - g_anchorQpc;
    // Base elapsed ms with 64-bit precision and round-to-nearest.
    int64_t dMs_base;
    if (dQ >= 0) dMs_base = (dQ * 1000LL + g_qpcFreq / 2) / g_qpcFreq;
    else         dMs_base = (dQ * 1000LL - g_qpcFreq / 2) / g_qpcFreq;

    int64_t rateMs = (dMs_base * (int64_t)g_ratePpm) / 1000000LL;
    int64_t utcMs  = g_anchorUtcMs + dMs_base + rateMs;

    return utcMs;
}

int64_t Clock_NowUtcMs_Internal(void) {
    // Internal read used only for audit/diagnostic purposes. Never
    // used for display. Returns 0 if not disciplined this run.
    if (!g_csInit) return 0;
    EnterCriticalSection(&g_cs);
    int64_t r = g_haveSample ? ProjectLocked(Clock_Qpc()) : 0;
    LeaveCriticalSection(&g_cs);
    return r;
}

int Clock_ReadDisplayTime(int64_t *outMs, uint64_t *outGeneration) {
    if (!g_csInit) return 0;
    int64_t nowQpc = Clock_Qpc();
    int expired = 0;
    EnterCriticalSection(&g_cs);
    int ok = 0;
    int64_t utcMs = 0;
    uint64_t generation = 0;
    if (Clock_DisplayLeaseExpiredLocked(nowQpc)) {
        Clock_TripInopLocked();
        expired = 1;
    }

    // Clock must be anchored, the last polling cycle must have
    // concurred, and the display lease from that cycle must still be
    // fresh. TRUST_INOP -- even with a projectable old anchor -- must
    // not return a time: callers must render INOP.
    if (g_haveSample && g_trust != TRUST_INOP) {
        utcMs = ProjectLocked(nowQpc);
        generation = g_displayGeneration;
        ok = 1;
    }
    LeaveCriticalSection(&g_cs);
    if (ok) {
        if (outMs) *outMs = utcMs;
        if (outGeneration) *outGeneration = generation;
    }
    if (expired) {
        Log_Append("clock: trust OK \xe2\x86\x92"
                   " INOP (trusted display lease expired)");
    }
    return ok;
}

int Clock_NowUtcMs(int64_t *outMs) {
    return Clock_ReadDisplayTime(outMs, NULL);
}

int Clock_DisplayGenerationIsCurrent(uint64_t generation) {
    if (!g_csInit || generation == 0) return 0;
    int64_t nowQpc = Clock_Qpc();
    int expired = 0;
    EnterCriticalSection(&g_cs);
    if (Clock_DisplayLeaseExpiredLocked(nowQpc)) {
        Clock_TripInopLocked();
        expired = 1;
    }
    int ok = g_haveSample && g_trust != TRUST_INOP &&
             g_displayGeneration == generation;
    LeaveCriticalSection(&g_cs);
    if (expired) {
        Log_Append("clock: trust OK \xe2\x86\x92"
                   " INOP (trusted display lease expired)");
    }
    return ok;
}

int Clock_IsDisciplined(void) { return g_haveSample ? 1 : 0; }
int32_t Clock_RatePpm(void)   { return g_haveRate ? g_ratePpm : 0; }

// Called by ntp.c after a successful SNTP exchange. ntpUtcMs is the true
// UTC at the instant identified by localQpc (which is the QPC value
// recorded at t4, i.e. when we received the reply). We include the
// round-trip halving in ntp.c already.
void Clock_OnSyncedNtpUtc(int64_t ntpUtcMs, int64_t localQpc) {
    if (!g_csInit) Clock_Init();

    // We build a log record INSIDE the CS (cheap local scalars only),
    // then emit it AFTER LeaveCriticalSection to avoid the re-entrant
    // Log_Append -> Clock_NowUtcMs -> g_cs deadlock.
    enum { EV_FIRST, EV_FIRST_STALE, EV_SUBSEQ } ev = EV_SUBSEQ;
    int32_t oldRate = 0, newRate = 0;
    int64_t errorMs = 0;
    int     rateMeasured = 0;       // did this cycle refresh the rate?
    int64_t staleAgeDays = 0;       // only meaningful when ev==EV_FIRST_STALE

    EnterCriticalSection(&g_cs);

    if (!g_haveSample) {
        // First sync THIS run. Deferred staleness check: if the
        // persisted rate was saved more than 30 days ago (measured
        // against the trusted NTP time we just received -- NOT the
        // Windows system clock), discard it.
        if (g_loadedLastSync > 0
            && ntpUtcMs - g_loadedLastSync > 30LL * 86400LL * 1000LL) {
            staleAgeDays = (ntpUtcMs - g_loadedLastSync) / 86400000LL;
            g_loadedRatePpm = 0;
            ev = EV_FIRST_STALE;
        } else {
            ev = EV_FIRST;
        }

        // Anchor, apply persisted rate as a bootstrap, but mark rate
        // as "not yet measured" because we have only one sample so far.
        g_anchorQpc     = localQpc;
        g_anchorUtcMs   = ntpUtcMs;
        g_ratePpm       = g_loadedRatePpm;   // bootstrap (or 0 if stale)
        g_haveSample    = 1;
        // If we had a previously-persisted rate, consider the clock
        // already disciplined: we're using that rate until the next
        // sync re-verifies it.
        g_haveRate      = (g_loadedRatePpm != 0) ? 1 : 0;
        g_lastSyncUtcMs = ntpUtcMs;
        g_lastSyncQpc   = localQpc;
        newRate = g_ratePpm;
    } else {
        // Subsequent sync. Measure clean drift error relative to the
        // anchor projection so the residual reflects true oscillator
        // drift.
        int64_t dQ = localQpc - g_anchorQpc;
        int64_t dMs_base = (dQ >= 0)
            ? (dQ * 1000LL + g_qpcFreq / 2) / g_qpcFreq
            : (dQ * 1000LL - g_qpcFreq / 2) / g_qpcFreq;
        int64_t rateMs   = (dMs_base * (int64_t)g_ratePpm) / 1000000LL;
        int64_t projectionPure = g_anchorUtcMs + dMs_base + rateMs;
        int64_t error          = ntpUtcMs - projectionPure;    // >0: we're behind
        errorMs = error;
        oldRate = g_ratePpm;

        // PI rate update: integral term on residual-rate.
        int64_t dqpc   = localQpc - g_lastSyncQpc;
        int64_t elapMs = (dqpc * 1000LL + g_qpcFreq / 2) / g_qpcFreq;
        if (elapMs > 30000) {
            // observed_ppm = residual rate error (X - R_old) in ppm.
            int64_t observed_ppm = (error * 1000000LL) / elapMs;
            // Integral step: gentle gain (Ki = 1/8).
            int64_t delta = (observed_ppm * KI_NUM) / KI_DEN;
            // Per-cycle change clamp: a single measurement can never
            // swing the rate by more than PER_CYCLE_RATE_CLAMP_PPM.
            // This is the anti-oscillation safety net.
            if (delta >  PER_CYCLE_RATE_CLAMP_PPM) delta =  PER_CYCLE_RATE_CLAMP_PPM;
            if (delta < -PER_CYCLE_RATE_CLAMP_PPM) delta = -PER_CYCLE_RATE_CLAMP_PPM;
            int32_t target = (int32_t)(g_ratePpm + delta);
            // Absolute rate clamp.
            if (target >  RATE_CLAMP_PPM) target =  RATE_CLAMP_PPM;
            if (target < -RATE_CLAMP_PPM) target = -RATE_CLAMP_PPM;
            g_ratePpm  = target;
            g_haveRate = 1;
            rateMeasured = 1;
        }
        newRate = g_ratePpm;

        // Phase correction: ALWAYS re-anchor to the server's reading.
        // The anchor now represents truth; subsequent residual
        // measurements are clean.
        g_anchorQpc   = localQpc;
        g_anchorUtcMs = ntpUtcMs;
        // Safety display policy: accepted samples snap immediately.
        // We do not knowingly display a cosmetically-slewed time that
        // differs from the freshest trusted anchor.

        g_lastSyncUtcMs = ntpUtcMs;
        g_lastSyncQpc   = localQpc;
    }

    Clock_GrantDisplayLeaseLocked();

    LeaveCriticalSection(&g_cs);

    // --- Emit the log line (outside the CS). ---
    switch (ev) {
    case EV_FIRST:
        if (newRate != 0) {
            Log_Append("clock: first anchor acquired \xe2\x80\x94"
                       " applying persisted rate %+d ppm as bootstrap",
                       (int)newRate);
        } else {
            Log_Append("clock: first anchor acquired \xe2\x80\x94"
                       " no rate yet (needs a second sync to measure drift)");
        }
        break;
    case EV_FIRST_STALE:
        Log_Append("clock: first anchor acquired \xe2\x80\x94"
                   " discarding persisted rate (saved %lld days ago, "
                   "exceeds 30-day staleness window)",
                   (long long)staleAgeDays);
        break;
    case EV_SUBSEQ: {
        const char *adj = "snap";
        if (rateMeasured) {
            Log_Append("clock: sync \xe2\x80\x94"
                       " residual %+lldms  adj=%s  rate %+d\xe2\x86\x92"
                       "%+d ppm (\xce\x94%+d, PI Ki=1/8, clamp \xc2\xb1%d/cycle)",
                       (long long)errorMs, adj,
                       (int)oldRate, (int)newRate,
                       (int)(newRate - oldRate),
                       PER_CYCLE_RATE_CLAMP_PPM);
        } else {
            Log_Append("clock: sync \xe2\x80\x94"
                       " residual %+lldms  adj=%s  rate %+d ppm "
                       "(interval too short to re-measure)",
                       (long long)errorMs, adj, (int)newRate);
        }
        break;
    }
    }
}

static const char *TrustName(TrustState t) {
    switch (t) {
    case TRUST_OK:       return "OK";
    case TRUST_DEGRADED: return "DEGRADED";
    default:             return "INOP";
    }
}

// Called by ntp.c at the end of every polling cycle. The concurrence
// verdict has already been computed by the caller. TRUST_OK re-anchors
// from (bestUtcMs, bestQpc); TRUST_DEGRADED holds the existing anchor and
// rate, using (bestUtcMs, bestQpc) only to cross-check the held projection
// against the core consensus; TRUST_INOP leaves the anchor alone. In every
// non-INOP case we additionally cross-check the agreed time against our own
// running projection: a disagreement of more than 200 ms is a
// local-oscillator fault and trips INOP (the OK path adds an escape hatch
// after LOCAL_FAULT_ESCAPE_N consecutive faults).
void Clock_OnPollCycle(TrustState state,
                       int64_t bestUtcMs,
                       int64_t bestQpc,
                       int64_t maxPairSpreadMs) {
    (void)maxPairSpreadMs;   // reserved for future audit-log use
    if (!g_csInit) Clock_Init();

    // Phase 1 -- classify the cycle under a single lock acquisition,
    // capturing everything needed for logging into locals. We must not
    // call Log_Append or Clock_OnSyncedNtpUtc while g_cs is held (both run
    // their own locking / logging and assume the lock is released -- see
    // the file header note), so logging and anchor updates are deferred
    // until after LeaveCriticalSection.
    enum {
        CYCLE_GATE_INOP,      // gate failed -> INOP
        CYCLE_FAULT_INOP,     // OK verdict but our projection drifted -> INOP
        CYCLE_ESCAPE,         // Nth consecutive OK-fault -> force snap to OK
        CYCLE_ACCEPT,         // OK verdict -> re-anchor, publish OK
        CYCLE_DEGRADED_HOLD,  // DEGRADED verdict, projection still agrees
        CYCLE_DEGRADED_INOP,  // DEGRADED verdict but no anchor / drift -> INOP
    } verdict;
    TrustState prevAtClassify;
    int64_t    faultDiffMs = 0;
    int        faultCount  = 0;

    EnterCriticalSection(&g_cs);
    prevAtClassify = g_trust;
    if (state == TRUST_INOP) {
        Clock_TripInopLocked();
        verdict = CYCLE_GATE_INOP;
    } else if (state == TRUST_DEGRADED) {
        // NTS unavailable; core sources corroborate. We never re-anchor or
        // update the rate from unauthenticated sources -- we only verify
        // that our held (last-OK) projection still agrees with the core
        // consensus, then keep displaying with an UNAUTHENTICATED badge.
        if (!g_haveSample) {
            Clock_TripInopLocked();
            verdict = CYCLE_DEGRADED_INOP;
        } else {
            int64_t predicted = ProjectLocked(bestQpc);
            int64_t diff      = bestUtcMs - predicted;
            int64_t absDiff   = diff < 0 ? -diff : diff;
            if (absDiff > 200) {
                faultDiffMs = diff;
                Clock_TripInopLocked();
                verdict = CYCLE_DEGRADED_INOP;
            } else {
                g_consecutiveLocalFaults = 0;
                Clock_GrantDisplayLeaseLocked();   // keep the dial alive
                verdict = CYCLE_DEGRADED_HOLD;
            }
        }
    } else if (g_haveSample) {
        // TRUST_OK with an existing anchor: cross-check the agreed time
        // against our running projection before accepting it.
        int64_t predicted = ProjectLocked(bestQpc);
        int64_t diff      = bestUtcMs - predicted;
        int64_t absDiff   = diff < 0 ? -diff : diff;
        if (absDiff > 200) {
            faultDiffMs = diff;
            faultCount  = ++g_consecutiveLocalFaults;
            if (faultCount >= LOCAL_FAULT_ESCAPE_N) {
                // Escape hatch: N consecutive cycles where all gating
                // sources concurred but we rejected them means OUR state
                // (anchor/rate) is the broken party, not the network.
                // Force-accept this sample as a snap and reset the counter.
                g_consecutiveLocalFaults = 0;
                verdict = CYCLE_ESCAPE;
            } else {
                Clock_TripInopLocked();
                verdict = CYCLE_FAULT_INOP;
            }
        } else {
            g_consecutiveLocalFaults = 0;
            verdict = CYCLE_ACCEPT;
        }
    } else {
        // First concurrence-valid cycle: no prior anchor to cross-check.
        g_consecutiveLocalFaults = 0;
        verdict = CYCLE_ACCEPT;
    }
    LeaveCriticalSection(&g_cs);

    // Phase 2 -- act on the verdict outside the lock.
    if (verdict == CYCLE_GATE_INOP) {
        if (prevAtClassify != TRUST_INOP) {
            Log_Append("clock: trust %s \xe2\x86\x92"
                       " INOP (NTP cycle failed concurrence gate)",
                       TrustName(prevAtClassify));
        }
        return;
    }
    if (verdict == CYCLE_DEGRADED_INOP) {
        if (faultDiffMs != 0) {
            Log_Append("clock: degraded projection drift \xe2\x80\x94"
                       " core sources corroborate but our held anchor differs "
                       "by %+lldms (>200ms); tripping INOP",
                       (long long)faultDiffMs);
        }
        if (prevAtClassify != TRUST_INOP) {
            Log_Append("clock: trust %s \xe2\x86\x92 INOP",
                       TrustName(prevAtClassify));
        }
        return;
    }
    if (verdict == CYCLE_FAULT_INOP) {
        Log_Append("clock: local-oscillator fault \xe2\x80\x94"
                   " concurrence gate passed but our projection differs "
                   "by %+lldms (>200ms); tripping INOP [%d/%d before escape]",
                   (long long)faultDiffMs,
                   faultCount, LOCAL_FAULT_ESCAPE_N);
        if (prevAtClassify != TRUST_INOP) {
            Log_Append("clock: trust %s \xe2\x86\x92 INOP",
                       TrustName(prevAtClassify));
        }
        return;
    }
    if (verdict == CYCLE_DEGRADED_HOLD) {
        // Anchor and rate held; only the trust state and the display lease
        // (granted above) change. Publish DEGRADED.
        TrustState prevPub;
        EnterCriticalSection(&g_cs);
        prevPub = g_trust;
        if (g_trust != TRUST_DEGRADED) {
            g_trust = TRUST_DEGRADED;
            g_displayGeneration++;
            if (g_displayGeneration == 0) g_displayGeneration = 1;
        }
        LeaveCriticalSection(&g_cs);
        if (prevPub != TRUST_DEGRADED) {
            Log_Append("clock: trust %s \xe2\x86\x92"
                       " DEGRADED (NTS unavailable; core sources corroborate "
                       "the held anchor, running unauthenticated)",
                       TrustName(prevPub));
        }
        return;
    }

    // CYCLE_ESCAPE and CYCLE_ACCEPT both accept the sample and re-anchor.
    // The escape path logs its recovery banner first; both then run the
    // normal PLL update (which emits its own per-sync log line) and
    // publish the state.
    if (verdict == CYCLE_ESCAPE) {
        Log_Append("clock: escape hatch \xe2\x80\x94"
                   " %d consecutive local-oscillator faults "
                   "(diff %+lldms); forcing snap to recover",
                   faultCount, (long long)faultDiffMs);
    }
    Clock_OnSyncedNtpUtc(bestUtcMs, bestQpc);

    TrustState prevAtPublish;
    EnterCriticalSection(&g_cs);
    prevAtPublish = g_trust;
    if (g_trust != state) {
        g_trust = state;
        g_displayGeneration++;
        if (g_displayGeneration == 0) g_displayGeneration = 1;
    }
    LeaveCriticalSection(&g_cs);
    if (prevAtPublish != state && state == TRUST_OK) {
        Log_Append((verdict == CYCLE_ESCAPE)
                   ? "clock: trust %s \xe2\x86\x92 OK (recovered via escape hatch)"
                   : "clock: trust %s \xe2\x86\x92 OK (concurrence gate passed)",
                   TrustName(prevAtPublish));
    }
}

void Clock_TripInop(const char *reason) {
    if (!g_csInit) Clock_Init();

    TrustState prev;
    EnterCriticalSection(&g_cs);
    prev = g_trust;
    Clock_TripInopLocked();
    LeaveCriticalSection(&g_cs);

    if (prev != TRUST_INOP) {
        Log_Append("clock: trust OK \xe2\x86\x92 INOP (%s)",
                   (reason && *reason) ? reason : "display safety trip");
    }
}

TrustState Clock_Trust(void) {
    if (!g_csInit) return TRUST_INOP;
    int64_t nowQpc = Clock_Qpc();
    int expired = 0;
    EnterCriticalSection(&g_cs);
    if (Clock_DisplayLeaseExpiredLocked(nowQpc)) {
        Clock_TripInopLocked();
        expired = 1;
    }
    TrustState t = g_trust;
    LeaveCriticalSection(&g_cs);
    if (expired) {
        Log_Append("clock: trust OK \xe2\x86\x92"
                   " INOP (trusted display lease expired)");
    }
    return t;
}

#ifdef LUNAR_TESTING
void Clock_TestExpireDisplayLease(void) {
    if (!g_csInit) Clock_Init();
    EnterCriticalSection(&g_cs);
    g_displayLeaseUntilQpc = Clock_Qpc() - 1;
    LeaveCriticalSection(&g_cs);
}
#endif
