// logbuf.h -- rolling in-memory event log (24 hour retention).
//
// Every major action or result in Lunar is recorded here as a single
// timestamped line. The buffer is bounded in two ways: any entry
// older than LOGBUF_RETENTION_MS is evicted on the next append, and
// the whole ring is capped at LOGBUF_CAP slots as a hard safety
// ceiling in case a burst of appends outruns the time-based pruner.
//
// Timestamps on each entry prefer Clock_NowUtcMs(); when the clock is
// not yet disciplined, a "T+SSSS.mmm" relative-since-process-start
// stamp is used instead and marked with a trailing '~'.
//
// The log is read via Log_Snapshot(), which copies the current
// contents into a caller-supplied buffer as a single \r\n-delimited
// UTF-8 string, oldest first. This is exactly what the Log viewer
// dialog pastes into its edit control.
//
// All entry points are thread-safe. First call auto-initialises the
// internal CRITICAL_SECTION (idempotent).

#ifndef LUNAR_LOGBUF_H
#define LUNAR_LOGBUF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Per-entry message ceiling (excl. the "YYYY-MM-DDTHH:MM:SS.mmmZ  "
// prefix the reader builds when formatting). Short enough that even
// a full ring takes well under 2 MiB, long enough to carry a full
// per-cycle NTP summary on one line.
#define LOGBUF_MSG_MAX    240

// Ring capacity (hard ceiling). At Lunar's cadence (60 s on OK, 5 s
// on INOP, plus occasional user actions) 24 h fits in a few hundred
// entries; the ceiling is insurance against a pathological burst.
#define LOGBUF_CAP        2048

// Retention window in milliseconds of process time (GetTickCount64).
// Independent of wall-clock trust state -- entries expire strictly
// by their age on the local monotonic clock.
#define LOGBUF_RETENTION_MS  (24LL * 60 * 60 * 1000)  // 24 hours

// Append one line to the log. Free-form printf-style formatting.
// The caller should NOT include a trailing newline; Log_Snapshot
// adds them when serialising.
void Log_Append(const char *fmt, ...);

// Raw entry as handed to structured readers (the Tcl-side event
// store). `seq` is a process-lifetime monotonic id (first entry = 1)
// that survives ring eviction, so an incremental reader can ask
// "everything after the last seq I saw" and never miss or duplicate
// an entry that is still in the ring.
typedef struct {
    uint64_t seq;
    uint64_t tickMs;    // GetTickCount64() at append (monotonic)
    int64_t  utcMs;     // disciplined UTC ms, or 0 if untrusted
    int      trusted;
    char     msg[LOGBUF_MSG_MAX];
} LogRawEntry;

// Copy every ring entry with seq > sinceSeq into `out` (oldest
// first), up to `cap` entries. Returns the number copied. Entries
// evicted before the call are gone -- the caller sees a gap in seq
// numbers, which is honest (the ring is the burst buffer; disk is
// the retention authority). Pass sinceSeq=0 for everything held.
size_t Log_CollectSince(uint64_t sinceSeq, LogRawEntry *out, size_t cap);

// Copy the whole log (oldest first) into `out`, up to `out_cap-1`
// bytes, NUL-terminating. Lines are separated by "\r\n" so the
// result is ready for pasting into a Win32 EDIT control. Returns
// the number of bytes actually written (excluding the terminator).
// Safe to call with out=NULL / out_cap=0 to query the required size.
size_t Log_Snapshot(char *out, size_t out_cap);

// Persist the current ring to %APPDATA%\Lunar\<leaf_name> (atomically;
// leaf_name NULL uses "last-session.log"). Called on shutdown, session
// end, and from the crash handler, so the diagnostic history the trust
// machinery produced survives the failure it explains. Returns 1 on
// success.
int Log_FlushToDisk(const wchar_t *leaf_name);

#ifdef LUNAR_TESTING
// Test-only: drop all buffered entries so a test can assert on log
// content in isolation from whatever logging ran before it.
void Log_Reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
