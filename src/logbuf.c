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
#include "app_paths.h"

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
//
// All line stamps are normalised to a fixed 12-character column so
// that message text aligns cleanly. Trusted UTC stamps are shown as
// "HH:MM:SS.mmm"; untrusted stamps (taken before the first successful
// sync this run) are shown as "T+NNNNN.mmms" relative to process
// start. The date is NOT repeated on every line -- the snapshot
// writer emits a "-- YYYY-MM-DD UTC --" header at the top and again
// whenever the UTC date rolls over across adjacent trusted entries.

static void FormatUtcTimeOnly(int64_t utcMs, char *out, size_t n) {
    time_t secs = (time_t)(utcMs / 1000);
    int    msec = (int)(utcMs % 1000);
    if (msec < 0) { msec += 1000; secs -= 1; }
    struct tm tm = { 0 };
    gmtime_s(&tm, &secs);
    _snprintf(out, n, "%02d:%02d:%02d.%03d",
              tm.tm_hour, tm.tm_min, tm.tm_sec, msec);
}

static void FormatUtcDate(int64_t utcMs, char *out, size_t n) {
    time_t secs = (time_t)(utcMs / 1000);
    struct tm tm = { 0 };
    gmtime_s(&tm, &secs);
    _snprintf(out, n, "%04d-%02d-%02d",
              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

// UTC "day number" for rollover detection. floor(utcMs / 86 400 000)
// with correct handling of negative values.
static int64_t UtcDayNumber(int64_t utcMs) {
    int64_t day = utcMs / 86400000LL;
    if (utcMs < 0 && utcMs % 86400000LL != 0) day -= 1;
    return day;
}

static void FormatRelativeLine(uint64_t tickMs, char *out, size_t n) {
    uint64_t dt = (tickMs >= g_procStartTick) ? (tickMs - g_procStartTick) : 0;
    uint64_t s  = dt / 1000;
    uint64_t ms = dt % 1000;
    // "T+NNNNN.mmms" -- 12 chars for any s < 100000 (~27 h, safely
    // larger than the 24 h retention window).
    _snprintf(out, n, "T+%05llu.%03llus",
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

    size_t  written = 0;
    int64_t lastDay = INT64_MIN;   // sentinel: no trusted entry seen yet

    EnterCriticalSection(&g_cs);
    EvictOld(GetTickCount64());

    for (int i = 0; i < g_count; i++) {
        const LogEntry *e = &g_ring[(g_head + i) % LOGBUF_CAP];

        // On the first trusted entry, and on every UTC date rollover
        // between adjacent trusted entries, emit a standalone header
        // line "-- YYYY-MM-DD UTC --". Untrusted (T+relative) entries
        // do not trigger headers and do not advance lastDay.
        if (e->trusted && e->utcMs != 0) {
            int64_t day = UtcDayNumber(e->utcMs);
            if (day != lastDay) {
                char date[16];
                FormatUtcDate(e->utcMs, date, sizeof date);
                char hdr[64];
                int hl = _snprintf(hdr, sizeof hdr,
                                   "-- %s UTC --\r\n", date);
                if (hl > 0) {
                    size_t need = (size_t)hl;
                    if (need >= sizeof hdr) need = sizeof hdr - 1;
                    if (out != NULL && out_cap > 0) {
                        size_t remain = (out_cap - 1 > written)
                                            ? (out_cap - 1 - written) : 0;
                        size_t copy = (need < remain) ? need : remain;
                        memcpy(out + written, hdr, copy);
                        written += copy;
                    } else {
                        written += need;
                    }
                }
                lastDay = day;
            }
        }

        char stamp[16];
        if (e->trusted && e->utcMs != 0) {
            FormatUtcTimeOnly(e->utcMs, stamp, sizeof stamp);
        } else {
            FormatRelativeLine(e->tickMs, stamp, sizeof stamp);
        }

        // Line = "<12-char stamp>  <msg>\r\n".
        char line[LOGBUF_MSG_MAX + 32];
        int  ln = _snprintf(line, sizeof line,
                            "%s  %s\r\n", stamp, e->msg);
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

int Log_FlushToDisk(const wchar_t *leaf_name) {
    EnsureInit();

    wchar_t path[MAX_PATH];
    if (!Lunar_AppDataPathW(path, MAX_PATH,
                            leaf_name ? leaf_name : L"last-session.log")) {
        return 0;
    }

    // This runs from shutdown paths and the crash handler, where the
    // CRT heap may be corrupt: VirtualAlloc goes straight to the
    // kernel and touches no heap locks. The pointer is kept for reuse
    // (a crash-time flush may be the first and only call).
    static char *s_flushBuf;
    static const size_t kFlushCap = 512 * 1024;
    if (!s_flushBuf) {
        s_flushBuf = (char *)VirtualAlloc(NULL, kFlushCap,
                                          MEM_COMMIT | MEM_RESERVE,
                                          PAGE_READWRITE);
        if (!s_flushBuf) return 0;
    }
    size_t n = Log_Snapshot(s_flushBuf, kFlushCap);
    if (n == 0) return 0;
    return Lunar_WriteFileAtomicW(path, s_flushBuf, n);
}

#ifdef LUNAR_TESTING
// Test-only: drop all buffered entries so a test can assert on log
// content in isolation from whatever logging ran before it.
void Log_Reset(void) {
    EnsureInit();
    EnterCriticalSection(&g_cs);
    g_head  = 0;
    g_count = 0;
    LeaveCriticalSection(&g_cs);
}
#endif
