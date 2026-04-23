// clock.c -- NTP-disciplined monotonic clockwork. See clock.h for the
// design overview.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <direct.h>
#include <wchar.h>

#include "clock.h"
#include "logbuf.h"

// NOTE: Log_Append() internally reads Clock_NowUtcMs(), which also
// takes g_cs. Every Log_Append call site in this file therefore runs
// AFTER the CS has been released, never while it's held.

// --- Persistence path -----------------------------------------------------

static void DisciplinePathW(wchar_t *out, size_t n) {
    const wchar_t *appdata = _wgetenv(L"APPDATA");
    if (!appdata || !*appdata) { out[0] = 0; return; }
    _snwprintf_s(out, n, _TRUNCATE, L"%ls\\Lunar", appdata);
    _wmkdir(out);
    _snwprintf_s(out, n, _TRUNCATE, L"%ls\\Lunar\\discipline.dat", appdata);
}

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
static int64_t    g_slewStartQpc  = 0;   // while slewing, start of window
static int64_t    g_slewTotalMs   = 0;   // residual to absorb
static int64_t    g_slewDurationMs = 0;  // over how long (currently 60000)

// Trust state published by ntp.c after each polling cycle. Starts at
// TRUST_INOP and stays there until the first cycle achieves concurrence.
static TrustState g_trust         = TRUST_INOP;

// Counts consecutive local-oscillator-fault rejections. If the NTP
// cycle concurs (≥2 core + NTS agree) but our running projection
// disagrees by >200 ms for several cycles in a row, our anchor/rate
// are clearly wrong -- an automatic escape hatch forces a snap after
// LOCAL_FAULT_ESCAPE_N faults to recover without user intervention.
static int        g_consecutiveLocalFaults = 0;
#define LOCAL_FAULT_ESCAPE_N  3

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
// server's reading on every sync (the anchor always represents truth).
// The visible-slew mechanism in ProjectLocked() is a pure cosmetic
// decay that keeps the second hand smooth across a re-anchor; it is
// NOT baked into the anchor and therefore cannot contaminate the next
// cycle's residual measurement -- a previous design that did bake the
// slew produced a self-reinforcing limit cycle (see audit 2026-04-23).
#define RATE_CLAMP_PPM              200    // realistic quartz: ±50 ppm typ
#define PER_CYCLE_RATE_CLAMP_PPM    20     // max rate swing per single cycle
#define KI_NUM                      1      // integral gain = 1/8 (gentle)
#define KI_DEN                      8
#define SLEW_THRESHOLD_MS           2000   // residual above this snaps instead
#define SLEW_WINDOW_MS              60000  // cosmetic decay window (~60 s)

int64_t Clock_Qpc(void) {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}

int64_t Clock_QpcFreq(void) {
    return g_qpcFreq;
}

static void LoadDiscipline(void) {
    g_loadedRatePpm = 0;
    wchar_t path[MAX_PATH]; DisciplinePathW(path, MAX_PATH);
    if (!path[0]) return;
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
    wchar_t path[MAX_PATH]; DisciplinePathW(path, MAX_PATH);
    if (!path[0]) return;
    FILE *f = _wfopen(path, L"wb");
    if (!f) return;
    fprintf(f, "%d %lld\n", (int)g_ratePpm, (long long)g_lastSyncUtcMs);
    fclose(f);
    Log_Append("clock: shutdown \xe2\x80\x94" " persisted rate=%+d ppm",
               (int)g_ratePpm);
}

// Project a QPC tick onto disciplined UTC ms using current anchor + rate.
//
// The anchor (g_anchorQpc, g_anchorUtcMs) always represents our best
// belief of truth at that QPC moment. Between syncs we project forward
// using g_ratePpm. On top of that we may add a cosmetic VISUAL slew
// that decays from g_slewTotalMs to zero over g_slewDurationMs: its
// sole purpose is to smooth the transition when a sync re-anchors to a
// slightly different UTC than we were projecting, so the second hand
// does not jump. The cosmetic slew is NEVER baked into the anchor,
// because the anchor already represents truth -- baking the visual
// residual back in was the source of a self-reinforcing limit cycle
// in the pre-2026-04-23 design.
static int64_t ProjectLocked(int64_t qpc) {
    int64_t dQ = qpc - g_anchorQpc;
    // Base elapsed ms with 64-bit precision and round-to-nearest.
    int64_t dMs_base;
    if (dQ >= 0) dMs_base = (dQ * 1000LL + g_qpcFreq / 2) / g_qpcFreq;
    else         dMs_base = (dQ * 1000LL - g_qpcFreq / 2) / g_qpcFreq;

    int64_t rateMs = (dMs_base * (int64_t)g_ratePpm) / 1000000LL;
    int64_t utcMs  = g_anchorUtcMs + dMs_base + rateMs;

    // Cosmetic visual slew: linearly decay the residual from its
    // initial value to zero over g_slewDurationMs. Initial value is
    // (displayed_before_reanchor - anchor_utc), so at slew start the
    // displayed time equals what the user saw immediately before the
    // re-anchor. It then ramps to the (now-authoritative) anchor-based
    // projection.
    if (g_slewTotalMs != 0 && g_slewDurationMs > 0) {
        int64_t dq2 = qpc - g_slewStartQpc;
        if (dq2 < 0) dq2 = 0;
        int64_t sinceMs = (dq2 * 1000LL + g_qpcFreq / 2) / g_qpcFreq;
        if (sinceMs >= g_slewDurationMs) {
            // Cosmetic slew has fully decayed to zero: clear state.
            // Crucially we do NOT modify the anchor here -- the anchor
            // was already truth when the slew was installed.
            g_slewTotalMs    = 0;
            g_slewDurationMs = 0;
        } else {
            int64_t remaining =
                (g_slewTotalMs * (g_slewDurationMs - sinceMs))
                / g_slewDurationMs;
            utcMs += remaining;
        }
    }
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

int Clock_NowUtcMs(int64_t *outMs) {
    if (!g_csInit) return 0;
    EnterCriticalSection(&g_cs);
    int ok = 0;
    // Clock must be anchored AND the last polling cycle must have
    // concurred (OK or DEGRADED). TRUST_INOP -- even with a stale
    // anchor -- must not return a time: we have no trusted reading
    // right now, and the UI must render INOP.
    if (g_haveSample && g_trust != TRUST_INOP) {
        if (outMs) *outMs = ProjectLocked(Clock_Qpc());
        ok = 1;
    }
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
    enum { EV_FIRST, EV_FIRST_STALE, EV_SUBSEQ } ev = EV_SUBSEQ;
    int32_t oldRate = 0, newRate = 0;
    int64_t errorMs = 0;
    int     didSnap = 0;
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
        g_slewTotalMs   = 0;
        g_slewDurationMs = 0;
        newRate = g_ratePpm;
    } else {
        // Subsequent sync. Measure clean drift error relative to the
        // anchor-only projection (no cosmetic slew component) so the
        // residual reflects true oscillator drift, not the visual
        // decay still in flight from the previous cycle.
        int64_t dQ = localQpc - g_anchorQpc;
        int64_t dMs_base = (dQ >= 0)
            ? (dQ * 1000LL + g_qpcFreq / 2) / g_qpcFreq
            : (dQ * 1000LL - g_qpcFreq / 2) / g_qpcFreq;
        int64_t rateMs   = (dMs_base * (int64_t)g_ratePpm) / 1000000LL;
        int64_t projectionPure = g_anchorUtcMs + dMs_base + rateMs;
        int64_t error          = ntpUtcMs - projectionPure;    // >0: we're behind
        errorMs = error;
        oldRate = g_ratePpm;

        // What the user is currently SEEING (anchor projection plus any
        // still-decaying cosmetic slew). We use this only to set up the
        // next cosmetic slew so the transition is seamless.
        int64_t displayedNow = ProjectLocked(localQpc);

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

        if (error > SLEW_THRESHOLD_MS || error < -SLEW_THRESHOLD_MS) {
            // Big jump (sleep-wake, user changed clock, bad bootstrap
            // replaced by first good sync). No cosmetic smoothing --
            // smoothing a 2+ s error would be worse than the jump.
            g_slewTotalMs    = 0;
            g_slewDurationMs = 0;
            didSnap = 1;
        } else {
            // Small error: install a cosmetic visual slew so the display
            // does not jump at the moment of re-anchor. The slew starts
            // at (displayedNow - ntpUtcMs) and decays linearly to 0 over
            // SLEW_WINDOW_MS, bringing the user smoothly to the new truth.
            g_slewStartQpc   = localQpc;
            g_slewTotalMs    = displayedNow - ntpUtcMs;
            g_slewDurationMs = SLEW_WINDOW_MS;
            didSnap = 0;
        }

        g_lastSyncUtcMs = ntpUtcMs;
        g_lastSyncQpc   = localQpc;
    }

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
        const char *adj = didSnap ? "snap" : "slew over 60s";
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

// Called by ntp.c at the end of every polling cycle. The concurrence
// verdict has already been computed by the caller; this function just
// either updates the anchor (OK / DEGRADED) or trips INOP and leaves
// the anchor alone. One extra safety check: even when the caller
// believes it has a valid sample, we cross-check against our own
// running projection. If we already have an anchor and the agreed
// time disagrees with our projection by > 200 ms, that is a local
// oscillator fault: trip INOP rather than accept the sample.
void Clock_OnPollCycle(TrustState state,
                       int64_t bestUtcMs,
                       int64_t bestQpc,
                       int64_t maxPairSpreadMs) {
    (void)maxPairSpreadMs;   // reserved for future audit-log use
    if (!g_csInit) Clock_Init();

    TrustState prev;
    int64_t    faultDiffMs = 0;

    if (state == TRUST_INOP) {
        EnterCriticalSection(&g_cs);
        prev = g_trust;
        g_trust = TRUST_INOP;
        LeaveCriticalSection(&g_cs);
        if (prev != TRUST_INOP) {
            Log_Append("clock: trust OK \xe2\x86\x92"
                       " INOP (NTP cycle failed concurrence gate)");
        }
        return;
    }

    // Additional guard: cross-check agreed time against projection.
    // Only meaningful once we already have an anchor from a previous
    // cycle; the first concurrence-valid cycle is accepted as-is.
    int localFault = 0;
    EnterCriticalSection(&g_cs);
    if (g_haveSample) {
        int64_t predicted = ProjectLocked(bestQpc);
        int64_t diff = bestUtcMs - predicted;
        if (diff < 0) diff = -diff;
        if (diff > 200) {
            localFault  = 1;
            faultDiffMs = bestUtcMs - predicted;
        }
    }
    LeaveCriticalSection(&g_cs);

    if (localFault) {
        int faultCount;
        EnterCriticalSection(&g_cs);
        g_consecutiveLocalFaults++;
        faultCount = g_consecutiveLocalFaults;
        LeaveCriticalSection(&g_cs);

        if (faultCount >= LOCAL_FAULT_ESCAPE_N) {
            // Escape hatch: N consecutive cycles where all gating
            // sources concurred but we rejected them means OUR state
            // (anchor/rate) is the broken party, not the network.
            // Force-accept this sample as a snap to recover without
            // user intervention, and reset the counter.
            EnterCriticalSection(&g_cs);
            g_consecutiveLocalFaults = 0;
            LeaveCriticalSection(&g_cs);
            Log_Append("clock: escape hatch \xe2\x80\x94"
                       " %d consecutive local-oscillator faults "
                       "(diff %+lldms); forcing snap to recover",
                       faultCount, (long long)faultDiffMs);
            Clock_OnSyncedNtpUtc(bestUtcMs, bestQpc);

            EnterCriticalSection(&g_cs);
            prev = g_trust;
            g_trust = state;
            LeaveCriticalSection(&g_cs);
            if (prev != state && state == TRUST_OK) {
                Log_Append("clock: trust INOP \xe2\x86\x92"
                           " OK (recovered via escape hatch)");
            }
            return;
        }

        EnterCriticalSection(&g_cs);
        prev = g_trust;
        g_trust = TRUST_INOP;
        LeaveCriticalSection(&g_cs);
        Log_Append("clock: local-oscillator fault \xe2\x80\x94"
                   " 3/3 servers concurred but our projection differs "
                   "by %+lldms (>200ms); tripping INOP [%d/%d before escape]",
                   (long long)faultDiffMs,
                   faultCount, LOCAL_FAULT_ESCAPE_N);
        if (prev != TRUST_INOP) {
            Log_Append("clock: trust OK \xe2\x86\x92 INOP");
        }
        return;
    }

    // Sample accepted (no local fault). Reset fault counter.
    EnterCriticalSection(&g_cs);
    g_consecutiveLocalFaults = 0;
    LeaveCriticalSection(&g_cs);

    // Accept the sample: run the normal PLL/slew update.
    // (Clock_OnSyncedNtpUtc emits its own log line.)
    Clock_OnSyncedNtpUtc(bestUtcMs, bestQpc);

    EnterCriticalSection(&g_cs);
    prev = g_trust;
    g_trust = state;
    LeaveCriticalSection(&g_cs);
    if (prev != state && state == TRUST_OK) {
        Log_Append("clock: trust INOP \xe2\x86\x92"
                   " OK (3/3 sources concurred)");
    }
}

TrustState Clock_Trust(void) {
    if (!g_csInit) return TRUST_INOP;
    EnterCriticalSection(&g_cs);
    TrustState t = g_trust;
    LeaveCriticalSection(&g_cs);
    return t;
}
