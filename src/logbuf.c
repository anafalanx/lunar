// logbuf.c -- rolling in-memory 24h event log.
//
// See logbuf.h for the contract.

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "logbuf.h"
#include "clock.h"

// --- Per-entry record ----------------------------------------------------
//
// We store each entry with:
//   - tickMs   : GetTickCount64() at the moment of Log_Append. Used
//                exclusively for retention-age comparisons, so that
//                an untrusted clock cannot affect eviction.
//   - utcMs    : Clock_NowUtcMs() result, or 0 if the clock wasn't
//                disciplined at append time. Used for the displayed
//                stamp in Log_Snapshot (falls back to T+relative).
//   - trusted  : 1 if utcMs came from the disciplined clockwork, 0
//                if we had to use the relative stamp at display.
//   - msg      : NUL-terminated payload.

typedef struct {
    uint64_t tickMs;
    int64_t  utcMs;
    int      trusted;
    char     msg[LOGBUF_MSG_MAX];
} LogEntry;

static LogEntry          g_ring[LOGBUF_CAP];
static int               g_head = 0;    // oldest valid index
static int               g_count = 0;   // number of valid entries
static uint64_t          g_procStartTick = 0;
static CRITICAL_SECTION  g_cs;
static int               g_csInit = 0;
static volatile LONG     g_initGuard = 0;

static void EnsureInit(void) {
    // Double-checked init. InterlockedCompareExchange gives us a
    // cheap fast path once initialised; the first-through caller
    // performs the actual initialisation.
    if (g_csInit) return;
    if (InterlockedCompareExchange(&g_initGuard, 1, 0) == 0) {
        InitializeCriticalSection(&g_cs);
        g_procStartTick = GetTickCount64();
        g_csInit = 1;
    } else {
        // Another thread is initialising; spin briefly.
        while (!g_csInit) Sleep(0);
    }
}

// --- Formatting helpers --------------------------------------------------

static void FormatIsoUtcLine(int64_t utcMs, char *out, size_t n) {
    time_t secs = (time_t)(utcMs / 1000);
    int    msec = (int)(utcMs % 1000);
    if (msec < 0) { msec += 1000; secs -= 1; }
    struct tm tm = { 0 };
    gmtime_s(&tm, &secs);
    _snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
              tm.tm_hour, tm.tm_min, tm.tm_sec, msec);
}

static void FormatRelativeLine(uint64_t tickMs, char *out, size_t n) {
    uint64_t dt = (tickMs >= g_procStartTick) ? (tickMs - g_procStartTick) : 0;
    uint64_t s  = dt / 1000;
    uint64_t ms = dt % 1000;
    _snprintf(out, n, "T+%08llu.%03llus",
              (unsigned long long)s, (unsigned long long)ms);
}

// --- Eviction ------------------------------------------------------------

static void EvictOld(uint64_t nowTick) {
    // Evict from head while the oldest entry is older than the
    // retention window. Ring holds entries in append order, so all
    // entries after the first non-stale are guaranteed fresher.
    while (g_count > 0) {
        const LogEntry *e = &g_ring[g_head];
        uint64_t age = (nowTick >= e->tickMs) ? (nowTick - e->tickMs) : 0;
        if ((int64_t)age < LOGBUF_RETENTION_MS) break;
        g_head = (g_head + 1) % LOGBUF_CAP;
        g_count--;
    }
}

// --- Public API ----------------------------------------------------------

void Log_Append(const char *fmt, ...) {
    if (fmt == NULL) return;
    EnsureInit();

    // Format the message first, outside the lock, into a stack buffer.
    // Keeps the critical section short and avoids running printf
    // handlers while other threads wait.
    char buf[LOGBUF_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    int nw = _vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (nw < 0 || (size_t)nw >= sizeof buf) {
        // Output was truncated; make sure it's NUL-terminated so the
        // snapshot reader can never walk off the end.
        buf[sizeof buf - 1] = 0;
    }

    uint64_t nowTick = GetTickCount64();
    int64_t  utcMs = 0;
    int      trusted = Clock_NowUtcMs(&utcMs) ? 1 : 0;

    EnterCriticalSection(&g_cs);
    EvictOld(nowTick);

    int idx;
    if (g_count < LOGBUF_CAP) {
        idx = (g_head + g_count) % LOGBUF_CAP;
        g_count++;
    } else {
        // Ring full -- overwrite the oldest entry. In normal
        // operation EvictOld above would have freed room; this
        // branch is the safety ceiling for burst traffic.
        idx = g_head;
        g_head = (g_head + 1) % LOGBUF_CAP;
    }

    LogEntry *e = &g_ring[idx];
    e->tickMs  = nowTick;
    e->utcMs   = utcMs;
    e->trusted = trusted;
    // Copy with explicit NUL termination.
    size_t msgLen = strlen(buf);
    if (msgLen >= sizeof e->msg) msgLen = sizeof e->msg - 1;
    memcpy(e->msg, buf, msgLen);
    e->msg[msgLen] = 0;

    LeaveCriticalSection(&g_cs);
}

size_t Log_Snapshot(char *out, size_t out_cap) {
    EnsureInit();

    size_t written = 0;

    EnterCriticalSection(&g_cs);
    EvictOld(GetTickCount64());

    for (int i = 0; i < g_count; i++) {
        const LogEntry *e = &g_ring[(g_head + i) % LOGBUF_CAP];

        char stamp[40];
        if (e->trusted && e->utcMs != 0) {
            FormatIsoUtcLine(e->utcMs, stamp, sizeof stamp);
        } else {
            FormatRelativeLine(e->tickMs, stamp, sizeof stamp);
        }

        // Line = "<stamp>[~]  <msg>\r\n". '~' marks untrusted stamps.
        char line[LOGBUF_MSG_MAX + 64];
        int  ln = _snprintf(line, sizeof line,
                            "%s%c  %s\r\n",
                            stamp,
                            e->trusted ? ' ' : '~',
                            e->msg);
        if (ln < 0) continue;
        size_t need = (size_t)ln;
        if (need >= sizeof line) need = sizeof line - 1;

        if (out == NULL || out_cap == 0) {
            written += need;
            continue;
        }
        // Leave room for NUL.
        size_t remain = (out_cap - 1 > written) ? (out_cap - 1 - written) : 0;
        if (remain == 0) break;
        size_t copy = (need < remain) ? need : remain;
        memcpy(out + written, line, copy);
        written += copy;
    }
    LeaveCriticalSection(&g_cs);

    if (out && out_cap > 0) {
        size_t term = (written < out_cap) ? written : out_cap - 1;
        out[term] = 0;
    }
    return written;
}
