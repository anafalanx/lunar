// ntp.c -- Parallel SNTP v4 client against three fixed national-metrology
// sources: NIST (USA), PTB (Germany), NICT (Japan). One UDP socket per
// source, one worker thread per source, all fired in parallel. Results
// are collected per-source so higher layers can apply concurrence rules.
//
// This translation unit is isolated from raylib.h / D2D headers because
// <winsock2.h> + <windows.h> collide with their symbol names.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ntp.h"
#include "clock.h"

#define NTP_PORT             "123"
#define NTP_TIMEOUT_MS       3000
#define NTP_EPOCH_DELTA_S    2208988800ULL        // seconds between 1900 and 1970
#define FRESH_WINDOW_MS      (2LL * 60LL * 60LL * 1000LL) // 2 hours

// --- Source list ---------------------------------------------------------

// Three independent national metrology institutes on three continents,
// each running stratum-1 caesium / hydrogen maser references. Diverse
// operators, geographies, and upstream routing make a coordinated spoof
// or outage much less likely than a single-pool query.
typedef struct {
    const char *host;
    const char *label;   // short display / log label
} NtpSource;

static const NtpSource kSources[NTP_SOURCE_COUNT] = {
    { "time.nist.gov",    "NIST" },    // USA      (Boulder / Gaithersburg)
    { "ptbtime1.ptb.de",  "PTB"  },    // Germany  (Braunschweig)
    { "ntp.nict.jp",      "NICT" },    // Japan    (Tokyo)
};

// --- Shared state --------------------------------------------------------

static CRITICAL_SECTION g_cs;
static int              g_csInit = 0;

// Results from the most recent polling cycle. Written by the aggregator
// thread under g_cs; read by Ntp_GetResults().
static NtpSourceResult  g_results[NTP_SOURCE_COUNT];
static volatile LONG64  g_offsetMs        = 0;   // legacy accessor
static volatile LONG64  g_lastSuccessTick = 0;   // GetTickCount64() at any-ok
static volatile LONG64  g_lastSuccessUtc  = 0;   // UTC ms at any-ok
static volatile LONG64  g_lastSpreadMs    = 0;   // last cycle's spread
static volatile LONG    g_running         = 0;

// --- Single-source query -------------------------------------------------

// Run one SNTP exchange against a specific host. Writes the result fields
// on success; returns 1 on success, 0 on any error (DNS, socket, timeout,
// invalid header, zero timestamps).
//
// QPC-only timing: we do NOT read the Windows system clock. T1 and T4
// are taken from QueryPerformanceCounter; only the server's reply
// timestamps (T2, T3 in NTP epoch) supply real UTC. The result's
// ntpUtcMs is "the real UTC at the moment qpcAtT4 was sampled":
//
//   rtt_ms         = (qpcT4 - qpcT1) * 1000 / qpcFreq
//   server_proc_ms = t3_ms - t2_ms
//   net_rtt_ms     = max(0, rtt_ms - server_proc_ms)
//   half_net_ms    = net_rtt_ms / 2
//   ntpUtcMs       = t3_ms + half_net_ms    // UTC at our local qpcT4
//
// outOffsetMs is kept for API compatibility but repurposed: it now
// carries ntpUtcMs (absolute UTC at qpcAtT4). The aggregator projects
// those values onto a common QPC moment before computing concurrence.
static int NtpQueryHost(const char *host,
                        int64_t *outOffsetMs,
                        int64_t *outNtpUtcMs,
                        int64_t *outQpcAtT4,
                        uint32_t *outRttMs) {
    unsigned char pkt[48];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x23;  // LI=0, VN=4, Mode=3 (client)

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    if (getaddrinfo(host, NTP_PORT, &hints, &res) != 0 || !res) return 0;

    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return 0; }

    DWORD tmo = NTP_TIMEOUT_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof(tmo));

    int64_t qpcT1 = Clock_Qpc();
    int sent = sendto(s, (const char *)pkt, (int)sizeof(pkt), 0,
                      res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);
    if (sent != (int)sizeof(pkt)) { closesocket(s); return 0; }

    int recvd = recv(s, (char *)pkt, (int)sizeof(pkt), 0);
    int64_t qpcT4 = Clock_Qpc();
    closesocket(s);
    if (recvd != (int)sizeof(pkt)) return 0;

    // Validate the response header before trusting any timestamps:
    //   LI  (bits 7..6 of byte 0) must be 0, 1 or 2 (3 = alarm, unsynced)
    //   VN  (bits 5..3) must be 3 or 4
    //   Mode(bits 2..0) must be 4                   (server)
    //   Stratum (byte 1) must be 1..15              (0 = kiss-o'-death,
    //                                                16 = unsynchronized)
    uint8_t li      = (pkt[0] >> 6) & 0x3;
    uint8_t vn      = (pkt[0] >> 3) & 0x7;
    uint8_t mode    =  pkt[0]       & 0x7;
    uint8_t stratum =  pkt[1];
    if (li == 3)                         return 0;
    if (vn != 3 && vn != 4)              return 0;
    if (mode != 4)                       return 0;
    if (stratum == 0 || stratum >= 16)   return 0;

    uint32_t secBE, fracBE;
    memcpy(&secBE, pkt + 32, 4); memcpy(&fracBE, pkt + 36, 4);
    uint32_t t2_s    = ntohl(secBE);
    uint32_t t2_frac = ntohl(fracBE);
    memcpy(&secBE, pkt + 40, 4); memcpy(&fracBE, pkt + 44, 4);
    uint32_t t3_s    = ntohl(secBE);
    uint32_t t3_frac = ntohl(fracBE);
    if (t2_s == 0 || t3_s == 0) return 0;

    int64_t t2_ms = ((int64_t)t2_s - (int64_t)NTP_EPOCH_DELTA_S) * 1000
                    + (int64_t)(((uint64_t)t2_frac * 1000ULL) >> 32);
    int64_t t3_ms = ((int64_t)t3_s - (int64_t)NTP_EPOCH_DELTA_S) * 1000
                    + (int64_t)(((uint64_t)t3_frac * 1000ULL) >> 32);

    int64_t qpcFreq = Clock_QpcFreq();
    if (qpcFreq <= 0) return 0;
    int64_t rtt = ((qpcT4 - qpcT1) * 1000LL + qpcFreq / 2) / qpcFreq;
    if (rtt < 0) rtt = 0;

    int64_t serverProc = t3_ms - t2_ms;
    if (serverProc < 0) serverProc = 0;
    int64_t netRtt = rtt - serverProc;
    if (netRtt < 0) netRtt = 0;

    *outNtpUtcMs = t3_ms + netRtt / 2;
    *outQpcAtT4  = qpcT4;
    *outRttMs    = (uint32_t)(rtt > 0x7fffffff ? 0x7fffffff : rtt);
    // offsetMs carries absolute UTC at qpcAtT4 (legacy field reused).
    *outOffsetMs = *outNtpUtcMs;
    return 1;
}

// --- Per-source worker thread --------------------------------------------

typedef struct {
    int              index;    // 0..NTP_SOURCE_COUNT-1
    NtpSourceResult  out;      // written by worker, read by aggregator
} WorkerCtx;

static DWORD WINAPI WorkerProc(LPVOID param) {
    WorkerCtx *ctx = (WorkerCtx *)param;
    const NtpSource *src = &kSources[ctx->index];

    NtpSourceResult *r = &ctx->out;
    memset(r, 0, sizeof(*r));
    r->label = src->label;

    int64_t off = 0, utc = 0, qpc = 0;
    uint32_t rtt = 0;
    if (NtpQueryHost(src->host, &off, &utc, &qpc, &rtt)) {
        r->ok        = 1;
        r->offsetMs  = off;
        r->ntpUtcMs  = utc;
        r->qpcAtT4   = qpc;
        r->rttMs     = rtt;
    } else {
        r->ok = 0;
    }
    return 0;
}

// --- Aggregator thread ---------------------------------------------------
//
// Spawns three worker threads in parallel, waits for all of them to
// complete (or to time out at the socket level inside NtpQueryHost),
// publishes per-source results, and picks the median of the successful
// samples to feed into the disciplined clockwork.
//
// The concurrence / INOP gating that decides whether the clockwork is
// allowed to update will be layered on top of this in the next step.
// For now the clock is fed the median of any successful samples; the
// legacy Ntp_IsSynced() / Ntp_OffsetMs() accessors continue to work the
// same way as before.

static int CmpI64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a;
    int64_t y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

// ---------------------------------------------------------------------------
// Audit log
// ---------------------------------------------------------------------------
//
// Every polling cycle appends one plain-text line to
// %APPDATA%\Lunar\audit.log. The file rotates to audit.log.1 at
// AUDIT_MAX_BYTES so the log can't grow unbounded. The format is line-
// oriented and greppable:
//
//   2026-04-21T12:34:56.789Z  OK    spread=  42ms  NIST:ok off= -12ms rtt= 85ms  PTB:ok off=  30ms rtt= 42ms  NICT:ok off=  10ms rtt=110ms
//   2026-04-21T12:35:01.000Z~ INOP  spread=   0ms  NIST:-- off=    - rtt=   -  PTB:ok off=  30ms rtt= 42ms  NICT:-- off=    - rtt=   -
//
// A trailing '~' on the timestamp marks a fallback stamp taken from
// the Windows system clock (used only when our own trusted clockwork
// cannot supply a time -- i.e. on an INOP cycle before the first ever
// concurrence). Stamps without '~' are from the disciplined clockwork.
//
// The logger never blocks the aggregator on I/O errors: a failed write
// is silently ignored. Losing a log line is preferable to stalling a
// polling cycle in a safety context.

#define AUDIT_MAX_BYTES (1 * 1024 * 1024)   // 1 MiB

static void AuditDir(wchar_t *out, size_t n) {
    wchar_t appdata[MAX_PATH] = { 0 };
    DWORD got = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (got == 0 || got >= MAX_PATH) { out[0] = 0; return; }
    _snwprintf(out, n, L"%ls\\Lunar", appdata);
}

static void AuditPath(wchar_t *out, size_t n) {
    wchar_t dir[MAX_PATH] = { 0 };
    AuditDir(dir, MAX_PATH);
    if (dir[0] == 0) { out[0] = 0; return; }
    _snwprintf(out, n, L"%ls\\audit.log", dir);
}

static void AuditRotateIfNeeded(void) {
    wchar_t path[MAX_PATH];
    AuditPath(path, MAX_PATH);
    if (path[0] == 0) return;

    WIN32_FILE_ATTRIBUTE_DATA fad = { 0 };
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return;

    uint64_t sz = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    if (sz < AUDIT_MAX_BYTES) return;

    wchar_t rotated[MAX_PATH];
    _snwprintf(rotated, MAX_PATH, L"%ls.1", path);
    DeleteFileW(rotated);       // ignore failure (may not exist)
    MoveFileW(path, rotated);   // ignore failure (next write will create fresh)
}

// Produce "YYYY-MM-DDTHH:MM:SS.mmmZ" from UTC ms since epoch.
// Buffer must be at least 25 bytes.
static void FormatIsoUtc(int64_t utcMs, char *out, size_t n) {
    time_t secs = (time_t)(utcMs / 1000);
    int    msec = (int)(utcMs % 1000);
    if (msec < 0) { msec += 1000; secs -= 1; }
    struct tm tm = { 0 };
    // gmtime_s is pure arithmetic: Unix epoch seconds to Y/M/D/h/m/s.
    // It does NOT read the system clock. Safe to use while remaining
    // independent of the Windows wall clock.
    gmtime_s(&tm, &secs);
    _snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
              tm.tm_hour, tm.tm_min, tm.tm_sec, msec);
}

// Timestamp helper for audit-log lines when no trusted UTC is yet
// available. We refuse to read the Windows system clock, so use the
// monotonic QPC counter since process start instead. The line will be
// marked with a '~' prefix so readers know this is a relative stamp,
// not real UTC.
//
//   T+000012.345s    -- 12.345 seconds since the clockwork was created
static void FormatRelativeStamp(char *out, size_t n) {
    static int64_t s_anchorQpc = 0;
    static int     s_anchored  = 0;
    if (!s_anchored) {
        s_anchorQpc = Clock_Qpc();
        s_anchored  = 1;
    }
    int64_t freq = Clock_QpcFreq();
    if (freq <= 0) freq = 1;
    int64_t ticks = Clock_Qpc() - s_anchorQpc;
    int64_t totalMs = (ticks * 1000LL + freq / 2) / freq;
    if (totalMs < 0) totalMs = 0;
    int64_t secs = totalMs / 1000;
    int     ms   = (int)(totalMs % 1000);
    _snprintf(out, n, "T+%06lld.%03ds", (long long)secs, ms);
}

// Grab the best timestamp we have: disciplined clock if OK, otherwise
// a QPC-relative "T+seconds since process start" marker. Returns 1 if
// the stamp is trusted wall-clock UTC.
static int BestLogStamp(char *out, size_t n) {
    int64_t utcMs = 0;
    if (Clock_NowUtcMs(&utcMs)) {
        FormatIsoUtc(utcMs, out, n);
        return 1;
    }
    FormatRelativeStamp(out, n);
    return 0;
}

static void AuditWrite(TrustState trust,
                       int64_t maxSpreadMs,
                       const NtpSourceResult results[NTP_SOURCE_COUNT]) {
    wchar_t dir[MAX_PATH];
    AuditDir(dir, MAX_PATH);
    if (dir[0] == 0) return;
    CreateDirectoryW(dir, NULL);  // no-op if exists

    AuditRotateIfNeeded();

    wchar_t path[MAX_PATH];
    AuditPath(path, MAX_PATH);
    if (path[0] == 0) return;

    HANDLE h = CreateFileW(path,
                           FILE_APPEND_DATA,
                           FILE_SHARE_READ,
                           NULL,
                           OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    char stamp[32];
    int trusted = BestLogStamp(stamp, sizeof stamp);

    char line[512];
    int  pos = 0;
    pos += _snprintf(line + pos, sizeof(line) - (size_t)pos,
                     "%s%c %-4s  spread=%5lldms",
                     stamp,
                     trusted ? ' ' : '~',
                     (trust == TRUST_OK) ? "OK" : "INOP",
                     (long long)maxSpreadMs);

    for (int i = 0; i < NTP_SOURCE_COUNT && pos < (int)sizeof(line) - 64; i++) {
        const NtpSourceResult *r = &results[i];
        const char *label = r->label ? r->label : "?";
        if (r->ok) {
            pos += _snprintf(line + pos, sizeof(line) - (size_t)pos,
                             "  %s:ok off=%5lldms rtt=%4ums",
                             label,
                             (long long)r->offsetMs,
                             (unsigned)r->rttMs);
        } else {
            pos += _snprintf(line + pos, sizeof(line) - (size_t)pos,
                             "  %s:-- off=    - rtt=   -",
                             label);
        }
    }
    if (pos < (int)sizeof(line) - 1) {
        line[pos++] = '\r';
        line[pos++] = '\n';
    } else {
        line[sizeof(line) - 2] = '\r';
        line[sizeof(line) - 1] = '\n';
        pos = sizeof(line);
    }

    DWORD written = 0;
    WriteFile(h, line, (DWORD)pos, &written, NULL);
    CloseHandle(h);
}

static DWORD WINAPI AggregatorProc(LPVOID param) {
    (void)param;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        InterlockedExchange(&g_running, 0);
        return 0;
    }

    WorkerCtx ctx[NTP_SOURCE_COUNT];
    HANDLE    threads[NTP_SOURCE_COUNT] = { 0 };
    int       spawned = 0;

    for (int i = 0; i < NTP_SOURCE_COUNT; i++) {
        ctx[i].index = i;
        memset(&ctx[i].out, 0, sizeof(ctx[i].out));
        ctx[i].out.label = kSources[i].label;
        threads[i] = CreateThread(NULL, 0, WorkerProc, &ctx[i], 0, NULL);
        if (threads[i]) spawned++;
    }

    // Cap total wall time at 2x the socket timeout so a stuck DNS
    // lookup or a dead server cannot keep us from publishing results.
    if (spawned > 0) {
        // WaitForMultipleObjects can't take NULL handles, so compact.
        HANDLE hs[NTP_SOURCE_COUNT];
        int    nh = 0;
        for (int i = 0; i < NTP_SOURCE_COUNT; i++)
            if (threads[i]) hs[nh++] = threads[i];
        WaitForMultipleObjects((DWORD)nh, hs, TRUE, NTP_TIMEOUT_MS * 2);
        for (int i = 0; i < NTP_SOURCE_COUNT; i++)
            if (threads[i]) CloseHandle(threads[i]);
    }

    // Collect successful results and compute the concurrence verdict.
    //
    //   3 ok + max pairwise spread <= 200 ms   -> TRUST_OK     (median)
    //   anything else                          -> TRUST_INOP
    //
    // There is no degraded middle ground: safety-critical operation
    // requires unanimous agreement among all three national-metrology
    // sources. 2-of-3 is not good enough; 0/1 reached is not good
    // enough; 3 reached but any pair disagrees > 200 ms is not good
    // enough. In all those cases we refuse to provide a reading and
    // the UI shows INOP.
    //
    // An extra cross-check against the running clockwork projection
    // lives in clock.c -- a trusted-looking sample that disagrees with
    // our own projection by > 200 ms trips INOP there too.

    NtpSourceResult snapshot[NTP_SOURCE_COUNT];
    for (int i = 0; i < NTP_SOURCE_COUNT; i++) snapshot[i] = ctx[i].out;

    int64_t    bestUtcMs = 0;
    int64_t    bestQpc   = 0;
    int64_t    maxSpread = 0;
    TrustState trust = Ntp_Concur(snapshot, &bestUtcMs, &bestQpc, &maxSpread);

    // Rewrite snapshot[i].offsetMs to the MEANINGFUL display value:
    // this source's deviation from the cycle consensus, projected to
    // a common QPC moment. That's what the audit log and About dialog
    // want to show (per-source agreement with the trio).
    {
        int64_t qpcFreq = Clock_QpcFreq();
        if (qpcFreq <= 0) qpcFreq = 1;
        // Consensus reference: the OK cycle's bestQpc if we have it,
        // otherwise an arbitrary successful source's qpcAtT4. If no
        // source succeeded we just zero all offsetMs.
        int64_t refQpc = bestQpc;
        int64_t refUtc = bestUtcMs;
        if (trust != TRUST_OK) {
            int picked = -1;
            for (int i = 0; i < NTP_SOURCE_COUNT; i++) {
                if (snapshot[i].ok) { picked = i; break; }
            }
            if (picked >= 0) {
                refQpc = snapshot[picked].qpcAtT4;
                refUtc = snapshot[picked].ntpUtcMs;
            }
        }
        for (int i = 0; i < NTP_SOURCE_COUNT; i++) {
            if (!snapshot[i].ok) { snapshot[i].offsetMs = 0; continue; }
            int64_t dq = refQpc - snapshot[i].qpcAtT4;
            int64_t dms = (dq >= 0)
                ? (dq * 1000LL + qpcFreq / 2) / qpcFreq
                : (dq * 1000LL - qpcFreq / 2) / qpcFreq;
            int64_t projected = snapshot[i].ntpUtcMs + dms;
            snapshot[i].offsetMs = projected - refUtc;
        }
    }

    // Publish shared state.
    if (g_csInit) EnterCriticalSection(&g_cs);
    for (int i = 0; i < NTP_SOURCE_COUNT; i++) g_results[i] = snapshot[i];
    if (g_csInit) LeaveCriticalSection(&g_cs);

    // Inform the clockwork. If state is INOP the anchor is not updated
    // and Clock_NowUtcMs() will refuse to return a time (callers render
    // the big red INOP).
    Clock_OnPollCycle(trust, bestUtcMs, bestQpc, maxSpread);
    InterlockedExchange64(&g_lastSpreadMs, (LONG64)maxSpread);

    // One line to the audit log per cycle. Done after Clock_OnPollCycle
    // so the timestamp reflects the clockwork's *post-cycle* state
    // (disciplined if this cycle was OK, untrusted-fallback otherwise).
    AuditWrite(trust, maxSpread, snapshot);

    // Legacy accessors (About dialog etc.) only record on OK cycles.
    if (trust == TRUST_OK) {
        // g_offsetMs legacy meaning ("median offset") no longer
        // applies -- we don't read the system clock. Publish 0; the
        // per-source offsetMs values on the snapshot (deviation from
        // consensus) are what the About dialog displays now.
        InterlockedExchange64(&g_offsetMs,        0);
        InterlockedExchange64(&g_lastSuccessTick, (LONG64)GetTickCount64());
        InterlockedExchange64(&g_lastSuccessUtc,  (LONG64)bestUtcMs);
    }

    WSACleanup();
    InterlockedExchange(&g_running, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// Pure concurrence evaluator (exported; unit-tested in tests/test_core.c)
// ---------------------------------------------------------------------------
//
// Given per-source results, compute the trust verdict. See ntp.h for
// contract. No globals, no I/O -- safe to call from anywhere.
//
// Each source captured its qpcAtT4 at a slightly different moment (the
// three workers run in parallel but reply at different times). We
// therefore cannot compare ntpUtcMs values directly -- a naive diff
// would include the local elapsed time between captures. Instead we
// project every source's UTC estimate onto a common QPC moment (the
// median qpcAtT4) using the local QPC frequency, then measure the
// pairwise spread on those projected values. This is a purely
// arithmetic operation on QPC (monotonic tick counter) and NTP-server
// timestamps; the Windows system clock is never consulted.
TrustState Ntp_Concur(const NtpSourceResult results[NTP_SOURCE_COUNT],
                      int64_t *outBestUtcMs,
                      int64_t *outBestQpc,
                      int64_t *outMaxSpreadMs) {
    #define CONCUR_THRESHOLD_MS 200

    if (outBestUtcMs)   *outBestUtcMs   = 0;
    if (outBestQpc)     *outBestQpc     = 0;
    if (outMaxSpreadMs) *outMaxSpreadMs = 0;

    int     nok = 0;
    int64_t utcs[NTP_SOURCE_COUNT];
    int64_t qpcs[NTP_SOURCE_COUNT];
    for (int i = 0; i < NTP_SOURCE_COUNT; i++) {
        if (results[i].ok) {
            utcs[nok] = results[i].ntpUtcMs;
            qpcs[nok] = results[i].qpcAtT4;
            nok++;
        }
    }

    // Below-unanimity is INOP regardless of agreement -- no degraded
    // middle ground. Still compute a spread so the audit log has
    // something informative.
    int64_t qpcFreq = Clock_QpcFreq();
    if (qpcFreq <= 0) qpcFreq = 1;

    if (nok != NTP_SOURCE_COUNT) {
        if (nok >= 2) {
            // Project to the first source's qpc as an arbitrary reference.
            int64_t ref = qpcs[0];
            int64_t proj[NTP_SOURCE_COUNT];
            for (int i = 0; i < nok; i++) {
                int64_t dq = ref - qpcs[i];
                int64_t dms = (dq >= 0)
                    ? (dq * 1000LL + qpcFreq / 2) / qpcFreq
                    : (dq * 1000LL - qpcFreq / 2) / qpcFreq;
                proj[i] = utcs[i] + dms;
            }
            qsort(proj, (size_t)nok, sizeof(int64_t), CmpI64);
            if (outMaxSpreadMs) *outMaxSpreadMs = proj[nok - 1] - proj[0];
        }
        return TRUST_INOP;
    }

    // Three successes. Project every sample to the median qpc.
    int64_t sortedQpc[3] = { qpcs[0], qpcs[1], qpcs[2] };
    qsort(sortedQpc, 3, sizeof(int64_t), CmpI64);
    int64_t refQpc = sortedQpc[1];

    int64_t proj[3];
    for (int i = 0; i < 3; i++) {
        int64_t dq = refQpc - qpcs[i];
        int64_t dms = (dq >= 0)
            ? (dq * 1000LL + qpcFreq / 2) / qpcFreq
            : (dq * 1000LL - qpcFreq / 2) / qpcFreq;
        proj[i] = utcs[i] + dms;
    }
    int64_t sortedProj[3] = { proj[0], proj[1], proj[2] };
    qsort(sortedProj, 3, sizeof(int64_t), CmpI64);
    int64_t spread = sortedProj[2] - sortedProj[0];
    if (outMaxSpreadMs) *outMaxSpreadMs = spread;

    if (spread > CONCUR_THRESHOLD_MS) return TRUST_INOP;

    // Feed the source whose projected UTC is the median. Return its
    // original ntpUtcMs and qpcAtT4 so the clockwork anchor is exact.
    int64_t medianProj = sortedProj[1];
    for (int k = 0; k < 3; k++) {
        if (proj[k] == medianProj) {
            if (outBestUtcMs) *outBestUtcMs = utcs[k];
            if (outBestQpc)   *outBestQpc   = qpcs[k];
            break;
        }
    }
    return TRUST_OK;
}

// --- Public API ----------------------------------------------------------

void Ntp_Start(void) {
    if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = 1; }
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) return;
    HANDLE th = CreateThread(NULL, 0, AggregatorProc, NULL, 0, NULL);
    if (th) {
        CloseHandle(th);
    } else {
        InterlockedExchange(&g_running, 0);
    }
}

int Ntp_IsSynced(void) {
    LONG64 last = g_lastSuccessTick;
    if (last == 0) return 0;
    LONG64 now = (LONG64)GetTickCount64();
    return (now - last) <= FRESH_WINDOW_MS;
}

int64_t Ntp_OffsetMs(void) {
    return (int64_t)g_offsetMs;
}

int64_t Ntp_LastSyncUtcMs(void) {
    return (int64_t)g_lastSuccessUtc;
}

int64_t Ntp_LastSpreadMs(void) {
    return (int64_t)g_lastSpreadMs;
}

int Ntp_GetResults(NtpSourceResult out[NTP_SOURCE_COUNT]) {
    if (!out) return 0;
    if (!g_csInit) {
        memset(out, 0, sizeof(NtpSourceResult) * NTP_SOURCE_COUNT);
        return 0;
    }
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < NTP_SOURCE_COUNT; i++) out[i] = g_results[i];
    LeaveCriticalSection(&g_cs);
    int nok = 0;
    for (int i = 0; i < NTP_SOURCE_COUNT; i++) if (out[i].ok) nok++;
    return nok;
}
