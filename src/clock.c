// clock.c -- NTP-disciplined monotonic clockwork. See clock.h for the
// design overview.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <direct.h>
#include <wchar.h>

#include "clock.h"

// --- Persistence path -----------------------------------------------------

static void DisciplinePathW(wchar_t *out, size_t n) {
    const wchar_t *appdata = _wgetenv(L"APPDATA");
    if (!appdata || !*appdata) { out[0] = 0; return; }
    _snwprintf(out, n, L"%ls\\Lunar", appdata);
    _wmkdir(out);
    _snwprintf(out, n, L"%ls\\Lunar\\discipline.dat", appdata);
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

#define RATE_CLAMP_PPM       500    // reject/clamp absurd rate updates
#define RATE_EMA_NUM         1      // new rate weight = 1/4 (EMA alpha 0.25)
#define RATE_EMA_DEN         4
#define SLEW_THRESHOLD_MS    2000   // residual above this snaps instead
#define SLEW_WINDOW_MS       60000  // slew small residuals over ~60 s

// Read the raw Windows system clock as UTC ms since 1970.
static int64_t RawSystemUtcMs(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    // 100-ns ticks since 1601 -> ms since 1970
    return (int64_t)(u.QuadPart / 10000LL) - 11644473600000LL;
}

int64_t Clock_Qpc(void) {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
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

    // Reject stale (> 30 days) or obviously out-of-range values. A
    // month-old rate is probably no longer representative of the
    // current hardware/temperature and anyway we'll re-verify on the
    // very first sync this run; worst case we just start at 0 ppm.
    int64_t nowUtc = RawSystemUtcMs();
    if (lastSync > 0 && nowUtc - lastSync > 30LL * 86400LL * 1000LL) return;
    if (rate >  RATE_CLAMP_PPM) rate =  RATE_CLAMP_PPM;
    if (rate < -RATE_CLAMP_PPM) rate = -RATE_CLAMP_PPM;
    g_loadedRatePpm = rate;
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
}

void Clock_Shutdown(void) {
    if (!g_haveRate) return;  // don't overwrite a good rate with a guess
    wchar_t path[MAX_PATH]; DisciplinePathW(path, MAX_PATH);
    if (!path[0]) return;
    FILE *f = _wfopen(path, L"wb");
    if (!f) return;
    fprintf(f, "%d %lld\n", (int)g_ratePpm, (long long)g_lastSyncUtcMs);
    fclose(f);
}

// Project a QPC tick onto disciplined UTC ms using current anchor + rate.
static int64_t ProjectLocked(int64_t qpc) {
    // elapsed_sec = (qpc - anchorQpc) / qpcFreq
    // elapsed_ms  = elapsed_sec * 1000
    // corrected   = elapsed_ms  * (1 + ratePpm/1e6)
    // anchorUtcMs + corrected
    int64_t dQ = qpc - g_anchorQpc;
    // Base elapsed ms with 64-bit precision:
    //   dMs_base = dQ * 1000 / freq
    // Keep this in int64 with rounding to nearest to avoid accumulating
    // a truncation bias.
    int64_t dMs_base;
    if (dQ >= 0) dMs_base = (dQ * 1000LL + g_qpcFreq / 2) / g_qpcFreq;
    else         dMs_base = (dQ * 1000LL - g_qpcFreq / 2) / g_qpcFreq;

    // Rate correction: ppm deviation. Integer ms result.
    //   rateMs = dMs_base * ratePpm / 1e6
    int64_t rateMs = (dMs_base * (int64_t)g_ratePpm) / 1000000LL;
    int64_t utcMs  = g_anchorUtcMs + dMs_base + rateMs;

    // Slew any pending residual: if we have to move the displayed time
    // by +/- S ms gradually over W ms since slewStartQpc, inject
    // S * progress(0..1) and retire the residual when progress reaches 1.
    if (g_slewTotalMs != 0 && g_slewDurationMs > 0) {
        int64_t sinceMs;
        int64_t dq2 = qpc - g_slewStartQpc;
        if (dq2 < 0) dq2 = 0;
        sinceMs = (dq2 * 1000LL + g_qpcFreq / 2) / g_qpcFreq;
        if (sinceMs >= g_slewDurationMs) {
            utcMs += g_slewTotalMs;
            // Bake finished slew into the anchor so subsequent reads
            // don't re-apply it.
            g_anchorUtcMs   += g_slewTotalMs;
            g_slewTotalMs    = 0;
            g_slewDurationMs = 0;
        } else {
            int64_t add = (g_slewTotalMs * sinceMs) / g_slewDurationMs;
            utcMs += add;
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

int64_t Clock_OffsetMs(void) {
    if (!g_haveSample) return 0;
    EnterCriticalSection(&g_cs);
    int64_t d = ProjectLocked(Clock_Qpc()) - RawSystemUtcMs();
    LeaveCriticalSection(&g_cs);
    return d;
}

// Called by ntp.c after a successful SNTP exchange. ntpUtcMs is the true
// UTC at the instant identified by localQpc (which is the QPC value
// recorded at t4, i.e. when we received the reply). We include the
// round-trip halving in ntp.c already.
void Clock_OnSyncedNtpUtc(int64_t ntpUtcMs, int64_t localQpc) {
    if (!g_csInit) Clock_Init();
    EnterCriticalSection(&g_cs);

    if (!g_haveSample) {
        // First sync THIS run. Anchor, apply persisted rate as a
        // bootstrap, but mark rate as "not yet measured" because we
        // have only one sample so far.
        g_anchorQpc     = localQpc;
        g_anchorUtcMs   = ntpUtcMs;
        g_ratePpm       = g_loadedRatePpm;   // bootstrap
        g_haveSample    = 1;
        // If we had a previously-persisted rate, consider the clock
        // already disciplined: we're using that rate until the next
        // sync re-verifies it.
        g_haveRate      = (g_loadedRatePpm != 0) ? 1 : 0;
        g_lastSyncUtcMs = ntpUtcMs;
        g_lastSyncQpc   = localQpc;
        g_slewTotalMs   = 0;
        g_slewDurationMs = 0;
    } else {
        // Subsequent sync. Compare server UTC to what WE thought the
        // time was at localQpc, given our current anchor+rate.
        int64_t predicted = ProjectLocked(localQpc);
        int64_t error     = ntpUtcMs - predicted;   // >0: we're behind

        // Measure the drift since the previous sync for the PLL update.
        int64_t dqpc   = localQpc - g_lastSyncQpc;
        int64_t elapMs = (dqpc * 1000LL + g_qpcFreq / 2) / g_qpcFreq;
        if (elapMs > 30000) {
            // newPpm deviation suggested by THIS interval:
            //   observed_rate = (error / elapMs)   (as fraction of 1)
            //   observed_ppm  = error * 1e6 / elapMs
            // Current rate was g_ratePpm; if we were perfectly right
            // then error would be zero. So the correction we need to
            // *apply* is observed_ppm.
            int64_t corr = (error * 1000000LL) / elapMs;
            if (corr >  RATE_CLAMP_PPM * 4) corr =  RATE_CLAMP_PPM * 4;
            if (corr < -RATE_CLAMP_PPM * 4) corr = -RATE_CLAMP_PPM * 4;
            int32_t target = (int32_t)(g_ratePpm + corr);
            if (target >  RATE_CLAMP_PPM) target =  RATE_CLAMP_PPM;
            if (target < -RATE_CLAMP_PPM) target = -RATE_CLAMP_PPM;

            // EMA: new = old + (target - old) * alpha
            int32_t old = g_ratePpm;
            int32_t delta = (int32_t)(((int64_t)(target - old)
                                       * RATE_EMA_NUM) / RATE_EMA_DEN);
            g_ratePpm = old + delta;
            g_haveRate = 1;
        }

        if (error > SLEW_THRESHOLD_MS || error < -SLEW_THRESHOLD_MS) {
            // Big jump (sleep-wake, user changed clock, first NTP
            // after a bad bootstrap). Snap.
            g_anchorQpc    = localQpc;
            g_anchorUtcMs  = ntpUtcMs;
            g_slewTotalMs  = 0;
            g_slewDurationMs = 0;
        } else {
            // Small residual: slew gradually so the second hand stays
            // smooth. Any currently-running slew is superseded.
            g_slewStartQpc   = localQpc;
            g_slewTotalMs    = error;
            g_slewDurationMs = SLEW_WINDOW_MS;
        }

        g_lastSyncUtcMs = ntpUtcMs;
        g_lastSyncQpc   = localQpc;
    }

    LeaveCriticalSection(&g_cs);
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

    if (state == TRUST_INOP) {
        EnterCriticalSection(&g_cs);
        g_trust = TRUST_INOP;
        LeaveCriticalSection(&g_cs);
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
        if (diff > 200) localFault = 1;
    }
    LeaveCriticalSection(&g_cs);

    if (localFault) {
        EnterCriticalSection(&g_cs);
        g_trust = TRUST_INOP;
        LeaveCriticalSection(&g_cs);
        return;
    }

    // Accept the sample: run the normal PLL/slew update.
    Clock_OnSyncedNtpUtc(bestUtcMs, bestQpc);

    EnterCriticalSection(&g_cs);
    g_trust = state;
    LeaveCriticalSection(&g_cs);
}

TrustState Clock_Trust(void) {
    if (!g_csInit) return TRUST_INOP;
    EnterCriticalSection(&g_cs);
    TrustState t = g_trust;
    LeaveCriticalSection(&g_cs);
    return t;
}
