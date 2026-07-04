// clock.c -- NTP-disciplined monotonic clockwork. See clock.h for the
// design overview.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

#include <string.h>

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
static int64_t    g_lastSyncTick64 = 0;  // GetTickCount64 at last accepted
                                         // anchor; unlike QPC-derived ages
                                         // this keeps counting across sleep
static int64_t    g_lastCycleTick64 = 0; // GetTickCount64 at last
                                         // corroborating cycle (OK accept or
                                         // DEGRADED hold); drives staleness
static uint64_t   g_displayGeneration = 1; // bumps on every display-state change

// Trust tier published by the last poll cycle: TRUST_OK, TRUST_DEGRADED
// or TRUST_HOLDOVER once anchored. The externally visible display state
// is DERIVED from this plus the flags below (see DeriveDisplayLocked).
static TrustState g_trust         = TRUST_INOP;

// 1 while the QPC timescale has run uninterrupted since the last anchor.
// Cleared by Clock_OnContinuityBroken (suspend/resume, session handoff,
// suspicious timer gap); restored by the next accepted authenticated
// cycle, which lays down a fresh anchor.
static int        g_qpcContinuityOk = 1;

// Hard-INOP latch for genuinely unrenderable faults (local time
// conversion failure). Cleared by the next corroborating cycle.
static int        g_hardInop        = 0;

// When a gate-passing consensus disagreed with our projection, the
// magnitude of that disagreement (ms). Folded into the published error
// bound so the display honestly covers "the network says we are off by
// this much". Cleared on the next accepted cycle.
static int64_t    g_faultInflateMs  = 0;

// Last display state observed by a reader; used to log derived
// transitions (e.g. OK -> HOLDOVER via staleness) exactly once.
static TrustState g_lastObservedState = TRUST_INOP;

// Counts consecutive local-oscillator-fault rejections. If the NTP
// cycle passes the concurrence gate (two operator-diverse NTS anchors
// agree, plus a core super-majority) but our running projection
// disagrees by >200 ms for several cycles in a row, our anchor/rate
// are clearly wrong -- the consensus wins after LOCAL_FAULT_ESCAPE_N
// faults and the clockwork snaps to it (prominent TIME STEP log).
static int        g_consecutiveLocalFaults = 0;
#define LOCAL_FAULT_ESCAPE_N  3

// --- Display-derivation constants ------------------------------------------
//
// BOUND_BASE_MS: uncertainty of a freshly accepted anchor. The OK gate
// requires mutual NTS agreement within 200 ms, so the midpoint anchor is
// honest to about that width (RTT asymmetry included in practice).
//
// BOUND_DEGRADED_CAP_MS: while live core sources corroborate the held
// projection within the 100 ms degraded gate, the claimed bound need not
// grow past corroboration width + base.
//
// CYCLE_STALE_AFTER_MS: a corroborating cycle older than this no longer
// keeps the OK/DEGRADED claim alive and the display derives to HOLDOVER.
// Chosen above the worst-case healthy renewal path (60 s poll interval
// + 40 s cycle budget = 100 s) so a slow-but-successful cycle never
// causes a false alarm.
#define BOUND_BASE_MS            200
#define BOUND_DEGRADED_CAP_MS    300
#define CYCLE_STALE_AFTER_MS  150000

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

static void BumpGenerationLocked(void) {
    g_displayGeneration++;
    if (g_displayGeneration == 0) g_displayGeneration = 1;
}

static int64_t QpcTicksToMs(int64_t ticks) {
    return (ticks >= 0)
        ? (ticks * 1000LL + g_qpcFreq / 2) / g_qpcFreq
        : (ticks * 1000LL - g_qpcFreq / 2) / g_qpcFreq;
}

// Forward: projection helper defined below.
static int64_t ProjectLocked(int64_t qpc);

// Derive the externally visible display state and error bound from the
// published trust tier plus the continuity / staleness / fault flags.
// Must be called with g_cs held. Fills every field of *out.
static void DeriveDisplayLocked(int64_t nowQpc, int64_t nowTick64,
                                ClockDisplay *out) {
    out->state         = TRUST_INOP;
    out->utcMs         = 0;
    out->boundMs       = 0;
    out->lastSyncUtcMs = g_haveSample ? g_lastSyncUtcMs : 0;
    out->lastSyncAgeMs = g_haveSample ? (nowTick64 - g_lastSyncTick64) : 0;
    out->generation    = g_displayGeneration;

    if (!g_haveSample || g_hardInop) return;                 // TRUST_INOP

    if (!g_qpcContinuityOk) {                                // REACQUIRING
        out->state = TRUST_REACQUIRING;
        return;
    }

    // Anchored, continuity intact: the display is never below holdover.
    // The OK/DEGRADED claim is only as fresh as its last corroborating
    // cycle; past the staleness window the display is honest holdover.
    TrustState st = g_trust;
    if (st < TRUST_HOLDOVER) st = TRUST_HOLDOVER;
    if (st > TRUST_HOLDOVER &&
        nowTick64 - g_lastCycleTick64 > CYCLE_STALE_AFTER_MS) {
        st = TRUST_HOLDOVER;
    }

    // Worst-case bound: anchor base + oscillator drift at the rate clamp
    // since the last accepted anchor, inflated by any network-reported
    // disagreement, capped while live corroboration is current.
    int64_t elapsedMs  = QpcTicksToMs(nowQpc - g_lastSyncQpc);
    if (elapsedMs < 0) elapsedMs = 0;
    int64_t bound = BOUND_BASE_MS
                  + (elapsedMs * RATE_CLAMP_PPM) / 1000000LL;
    if (g_faultInflateMs > 0 && bound < g_faultInflateMs + BOUND_BASE_MS) {
        bound = g_faultInflateMs + BOUND_BASE_MS;
    }
    if (st == TRUST_DEGRADED && bound > BOUND_DEGRADED_CAP_MS) {
        bound = BOUND_DEGRADED_CAP_MS;
    }

    out->state   = st;
    out->utcMs   = ProjectLocked(nowQpc);
    out->boundMs = bound;
}

static const char *TrustName(TrustState t);

// Record a derived-state transition for one-shot logging. Returns the
// previous observed state; the caller logs OUTSIDE the lock when the
// value differs from the new state.
static TrustState ObserveStateLocked(TrustState now) {
    TrustState prev = g_lastObservedState;
    g_lastObservedState = now;
    return prev;
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
    // run). Until then the display state is TRUST_INOP and no time is
    // returned -- the Windows system clock is never a display source.
    g_ratePpm    = 0;
    g_haveSample = 0;
    g_haveRate   = 0;
    g_lastSyncUtcMs = 0;
    g_lastSyncQpc = 0;
    g_lastSyncTick64 = 0;
    g_lastCycleTick64 = 0;
    g_trust = TRUST_INOP;
    g_qpcContinuityOk = 1;
    g_hardInop = 0;
    g_faultInflateMs = 0;
    g_lastObservedState = TRUST_INOP;
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
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%d %lld\n",
                     (int)g_ratePpm, (long long)g_lastSyncUtcMs);
    if (n <= 0 || !Lunar_WriteFileAtomicW(path, buf, (size_t)n)) return;
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

void Clock_GetDisplay(ClockDisplay *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!g_csInit) return;

    int64_t nowQpc   = Clock_Qpc();
    int64_t nowTick  = (int64_t)GetTickCount64();
    TrustState prevObserved, nowState;
    int64_t staleAgeMs = 0;

    EnterCriticalSection(&g_cs);
    DeriveDisplayLocked(nowQpc, nowTick, out);
    nowState = out->state;
    prevObserved = ObserveStateLocked(nowState);
    if (nowState == TRUST_HOLDOVER && prevObserved > TRUST_HOLDOVER) {
        staleAgeMs = nowTick - g_lastCycleTick64;
    }
    LeaveCriticalSection(&g_cs);

    // One-shot log for transitions readers derive (e.g. staleness).
    if (prevObserved != nowState && nowState == TRUST_HOLDOVER &&
        prevObserved > TRUST_HOLDOVER) {
        Log_Append("clock: display %s \xe2\x86\x92 HOLDOVER "
                   "(no corroborating cycle for %llds; bound now grows)",
                   TrustName(prevObserved),
                   (long long)(staleAgeMs / 1000));
    }
}

int Clock_ReadDisplayTime(int64_t *outMs, uint64_t *outGeneration) {
    ClockDisplay d;
    Clock_GetDisplay(&d);
    if (d.state < TRUST_HOLDOVER) return 0;
    if (outMs) *outMs = d.utcMs;
    if (outGeneration) *outGeneration = d.generation;
    return 1;
}

int Clock_NowUtcMs(int64_t *outMs) {
    return Clock_ReadDisplayTime(outMs, NULL);
}

int Clock_DisplayGenerationIsCurrent(uint64_t generation) {
    if (!g_csInit || generation == 0) return 0;
    ClockDisplay d;
    int64_t nowQpc  = Clock_Qpc();
    int64_t nowTick = (int64_t)GetTickCount64();
    EnterCriticalSection(&g_cs);
    DeriveDisplayLocked(nowQpc, nowTick, &d);
    int ok = d.state >= TRUST_HOLDOVER && d.generation == generation;
    LeaveCriticalSection(&g_cs);
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
    enum { EV_FIRST, EV_FIRST_STALE, EV_SUBSEQ, EV_REANCHOR } ev = EV_SUBSEQ;
    int32_t oldRate = 0, newRate = 0;
    int64_t errorMs = 0;
    int     rateMeasured = 0;       // did this cycle refresh the rate?
    int64_t staleAgeDays = 0;       // only meaningful when ev==EV_FIRST_STALE
    int64_t nowTick64 = (int64_t)GetTickCount64();

    EnterCriticalSection(&g_cs);

    if (g_haveSample && !g_qpcContinuityOk) {
        // Re-anchor after a continuity break (suspend/resume, session
        // handoff). The residual against the pre-break projection is
        // meaningless -- QPC may have paused or jumped -- so the rate is
        // held as-is and no PI update runs; the fresh anchor restores a
        // valid projection basis.
        g_anchorQpc     = localQpc;
        g_anchorUtcMs   = ntpUtcMs;
        g_lastSyncUtcMs = ntpUtcMs;
        g_lastSyncQpc   = localQpc;
        g_qpcContinuityOk = 1;
        newRate = g_ratePpm;
        ev = EV_REANCHOR;
    } else if (!g_haveSample) {
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
        g_qpcContinuityOk = 1;   // fresh anchor defines a fresh timescale
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

    // An accepted authenticated sample refreshes every claim: the anchor
    // age, the corroboration freshness, and it clears the unrenderable /
    // network-disagreement latches.
    g_lastSyncTick64  = nowTick64;
    g_lastCycleTick64 = nowTick64;
    g_hardInop        = 0;
    g_faultInflateMs  = 0;
    BumpGenerationLocked();

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
    case EV_REANCHOR:
        Log_Append("clock: re-anchored after continuity break \xe2\x80\x94"
                   " rate %+d ppm held (residual across the break is not "
                   "measurable)", (int)newRate);
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
    case TRUST_OK:          return "OK";
    case TRUST_DEGRADED:    return "DEGRADED";
    case TRUST_HOLDOVER:    return "HOLDOVER";
    case TRUST_REACQUIRING: return "REACQUIRING";
    default:                return "INOP";
    }
}

// Called by ntp.c at the end of every polling cycle. The concurrence
// verdict has already been computed by the caller. TRUST_OK re-anchors
// from (bestUtcMs, bestQpc); TRUST_DEGRADED holds the existing anchor and
// rate, using (bestUtcMs, bestQpc) only to cross-check the held projection
// against the core consensus; TRUST_INOP leaves everything alone (the
// display derives to holdover on the held anchor). A gate-passing
// consensus that disagrees with our projection by more than 200 ms
// inflates the published bound and drops the tier to HOLDOVER; after
// LOCAL_FAULT_ESCAPE_N consecutive such cycles the consensus wins and the
// clockwork snaps to it (our anchor/rate are the broken party).
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
        CYCLE_ACCEPT,         // OK verdict -> re-anchor, publish OK
        CYCLE_ESCAPE,         // Nth corroborated disagreement -> forced snap
        CYCLE_FAULT_HOLD,     // OK verdict disagrees with our projection ->
                              // holdover with the bound inflated to cover it
        CYCLE_DEGRADED_HOLD,  // DEGRADED verdict, projection corroborated
        CYCLE_DEGRADED_FAULT, // DEGRADED verdict disagrees -> holdover
        CYCLE_NO_CONSENSUS,   // gate failed -> holdover on the held anchor
        CYCLE_IGNORED,        // nothing usable: no anchor yet, or continuity
                              // is broken and the verdict cannot re-anchor
    } verdict;
    TrustState prevPublished;
    int64_t    faultDiffMs = 0;
    int        faultCount  = 0;
    int64_t    nowTick64   = (int64_t)GetTickCount64();

    EnterCriticalSection(&g_cs);
    prevPublished = g_trust;
    if (state == TRUST_OK) {
        if (!g_haveSample || !g_qpcContinuityOk) {
            // First anchor of the run, or the authenticated re-anchor that
            // ends a REACQUIRING episode. Nothing to cross-check against.
            g_consecutiveLocalFaults = 0;
            verdict = CYCLE_ACCEPT;
        } else {
            int64_t predicted = ProjectLocked(bestQpc);
            int64_t diff      = bestUtcMs - predicted;
            int64_t absDiff   = diff < 0 ? -diff : diff;
            if (absDiff > 200) {
                faultDiffMs = diff;
                faultCount  = ++g_consecutiveLocalFaults;
                if (faultCount >= LOCAL_FAULT_ESCAPE_N) {
                    // N consecutive cycles where all gating sources
                    // concurred but we rejected them: OUR state
                    // (anchor/rate) is the broken party, not the network.
                    g_consecutiveLocalFaults = 0;
                    verdict = CYCLE_ESCAPE;
                } else {
                    // Keep displaying, but publish HOLDOVER with the bound
                    // inflated to honestly cover the disagreement.
                    g_faultInflateMs = absDiff;
                    if (g_trust != TRUST_HOLDOVER) {
                        g_trust = TRUST_HOLDOVER;
                        BumpGenerationLocked();
                    }
                    verdict = CYCLE_FAULT_HOLD;
                }
            } else {
                g_consecutiveLocalFaults = 0;
                verdict = CYCLE_ACCEPT;
            }
        }
    } else if (state == TRUST_DEGRADED) {
        // NTS unavailable; core sources corroborate. We never re-anchor or
        // update the rate from unauthenticated sources -- corroboration
        // only refreshes the claimed bound on the held projection. It can
        // neither create an anchor nor repair broken QPC continuity.
        if (!g_haveSample || !g_qpcContinuityOk) {
            verdict = CYCLE_IGNORED;
        } else {
            int64_t predicted = ProjectLocked(bestQpc);
            int64_t diff      = bestUtcMs - predicted;
            int64_t absDiff   = diff < 0 ? -diff : diff;
            if (absDiff > 200) {
                faultDiffMs = diff;
                g_faultInflateMs = absDiff;
                if (g_trust != TRUST_HOLDOVER) {
                    g_trust = TRUST_HOLDOVER;
                    BumpGenerationLocked();
                }
                verdict = CYCLE_DEGRADED_FAULT;
            } else {
                g_consecutiveLocalFaults = 0;
                g_lastCycleTick64 = nowTick64;   // live corroboration
                g_hardInop        = 0;
                g_faultInflateMs  = 0;
                if (g_trust != TRUST_DEGRADED) {
                    g_trust = TRUST_DEGRADED;
                    BumpGenerationLocked();
                }
                verdict = CYCLE_DEGRADED_HOLD;
            }
        }
    } else {
        // Gate failed / no usable sources this cycle. The anchor is left
        // untouched; an anchored, continuity-intact clock keeps running as
        // honest holdover. (Fail-honest: the display never goes dark just
        // because the network went away.)
        if (g_haveSample && g_qpcContinuityOk) {
            if (g_trust != TRUST_HOLDOVER) {
                g_trust = TRUST_HOLDOVER;
                BumpGenerationLocked();
            }
            verdict = CYCLE_NO_CONSENSUS;
        } else {
            verdict = CYCLE_IGNORED;
        }
    }
    LeaveCriticalSection(&g_cs);

    // Phase 2 -- act on the verdict outside the lock.
    switch (verdict) {
    case CYCLE_IGNORED:
        return;
    case CYCLE_NO_CONSENSUS:
        if (prevPublished != TRUST_HOLDOVER) {
            Log_Append("clock: trust %s \xe2\x86\x92 HOLDOVER "
                       "(cycle failed the concurrence gate; projecting the "
                       "held anchor, error bound growing)",
                       TrustName(prevPublished));
        }
        return;
    case CYCLE_FAULT_HOLD:
        Log_Append("clock: gate-passing consensus disagrees with our "
                   "projection by %+lldms (>200ms) [%d/%d before forced "
                   "re-anchor]; publishing HOLDOVER with inflated bound",
                   (long long)faultDiffMs,
                   faultCount, LOCAL_FAULT_ESCAPE_N);
        return;
    case CYCLE_DEGRADED_FAULT:
        Log_Append("clock: core consensus differs from the held anchor "
                   "by %+lldms (>200ms); holding over with inflated bound "
                   "(unauthenticated sources cannot re-anchor)",
                   (long long)faultDiffMs);
        return;
    case CYCLE_DEGRADED_HOLD:
        if (prevPublished != TRUST_DEGRADED) {
            Log_Append("clock: trust %s \xe2\x86\x92"
                       " DEGRADED (NTS unavailable; core sources corroborate "
                       "the held anchor, running unauthenticated)",
                       TrustName(prevPublished));
        }
        return;
    case CYCLE_ESCAPE:
    case CYCLE_ACCEPT:
        break;
    }

    // CYCLE_ESCAPE and CYCLE_ACCEPT both accept the sample and re-anchor.
    // The escape path logs its prominent step banner first; both then run
    // the normal discipline update (which emits its own per-sync log line)
    // and publish OK.
    if (verdict == CYCLE_ESCAPE) {
        Log_Append("clock: *** TIME STEP %+lldms *** %d consecutive "
                   "gate-passing cycles disagreed with the local "
                   "projection; the authenticated consensus wins and the "
                   "clockwork snaps to it",
                   (long long)faultDiffMs, faultCount);
    }
    Clock_OnSyncedNtpUtc(bestUtcMs, bestQpc);

    TrustState prevAtPublish;
    EnterCriticalSection(&g_cs);
    prevAtPublish = g_trust;
    if (g_trust != TRUST_OK) {
        g_trust = TRUST_OK;
        BumpGenerationLocked();
    }
    LeaveCriticalSection(&g_cs);
    if (prevAtPublish != TRUST_OK) {
        Log_Append((verdict == CYCLE_ESCAPE)
                   ? "clock: trust %s \xe2\x86\x92 OK (recovered via forced re-anchor)"
                   : "clock: trust %s \xe2\x86\x92 OK (concurrence gate passed)",
                   TrustName(prevAtPublish));
    }
}

void Clock_TripInop(const char *reason) {
    if (!g_csInit) Clock_Init();

    int changed = 0;
    EnterCriticalSection(&g_cs);
    if (!g_hardInop) {
        g_hardInop = 1;
        changed = 1;
        BumpGenerationLocked();
    }
    LeaveCriticalSection(&g_cs);

    if (changed) {
        Log_Append("clock: display INOP latched (%s); no time shown until "
                   "a corroborating cycle clears the fault",
                   (reason && *reason) ? reason : "unrenderable fault");
    }
}

void Clock_OnContinuityBroken(const char *reason) {
    if (!g_csInit) Clock_Init();

    int changed = 0;
    EnterCriticalSection(&g_cs);
    g_consecutiveLocalFaults = 0;
    if (g_qpcContinuityOk) {
        g_qpcContinuityOk = 0;
        changed = 1;
        BumpGenerationLocked();
    }
    LeaveCriticalSection(&g_cs);

    if (changed) {
        Log_Append("clock: QPC continuity broken (%s); showing the last "
                   "verified time until an authenticated cycle re-anchors",
                   (reason && *reason) ? reason : "continuity event");
    }
}

TrustState Clock_Trust(void) {
    if (!g_csInit) return TRUST_INOP;
    ClockDisplay d;
    Clock_GetDisplay(&d);
    return d.state;
}

#ifdef LUNAR_TESTING
void Clock_TestAgeLastCycle(int64_t ageMs) {
    if (!g_csInit) Clock_Init();
    EnterCriticalSection(&g_cs);
    g_lastCycleTick64 -= ageMs;
    LeaveCriticalSection(&g_cs);
}

void Clock_TestAgeAnchor(int64_t ageMs) {
    if (!g_csInit) Clock_Init();
    EnterCriticalSection(&g_cs);
    g_lastSyncQpc    -= (ageMs * g_qpcFreq + 999LL) / 1000LL;
    g_lastSyncTick64 -= ageMs;
    LeaveCriticalSection(&g_cs);
}
#endif
