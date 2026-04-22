// ntp.c -- Parallel SNTP+NTS client. Queries four sources in parallel
// every cycle:
//
//    slot 0: NIST (time.nist.gov)      -- USA
//    slot 1: PTB  (ptbtime1.ptb.de)    -- Germany
//    slot 2: NICT (ntp.nict.jp)        -- Japan
//    slot 3: NTS  (rotating)           -- per-cycle random pick from a
//                                         short pool of NTS providers
//                                         with SPKI-pinned TLS 1.3
//                                         authentication (src/nts.c)
//
// One UDP socket per core source + one TCP+UDP pair for the NTS slot,
// one worker thread per source, all fired in parallel. Results are
// collected per-source and a single concurrence verdict is computed:
// NTS is the trust anchor, and at least 2 of the 3 core sources must
// agree with it within 200 ms (after QPC projection) for the cycle to
// be OK; otherwise the clockwork goes INOP.
//
// This translation unit is isolated from D2D headers because
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
#include "nts.h"
#include "logbuf.h"

#define NTP_PORT             "123"
#define NTP_TIMEOUT_MS       6000      // core slot UDP recv timeout
#define NTS_SLOT_TIMEOUT_MS  20000     // KE (TLS 1.3 + handshake) + authenticated UDP
#define NTP_EPOCH_DELTA_S    2208988800ULL        // seconds between 1900 and 1970
#define FRESH_WINDOW_MS      (2LL * 60LL * 60LL * 1000LL) // 2 hours
#define CONCUR_THRESHOLD_MS  200

// --- Source list (core only; NTS slot handled separately) ---------------

// Three independent national metrology institutes on three continents,
// each running stratum-1 caesium / hydrogen maser references. Diverse
// operators, geographies, and upstream routing make a coordinated spoof
// or outage much less likely than a single-pool query.
typedef struct {
    const char *host;
    const char *label;   // short display / log label
} NtpSource;

static const NtpSource kSources[NTP_CORE_COUNT] = {
    { "time.nist.gov",    "NIST" },    // USA      (Boulder / Gaithersburg)
    { "ptbtime1.ptb.de",  "PTB"  },    // Germany  (Braunschweig)
    { "ntp.nict.jp",      "NICT" },    // Japan    (Tokyo)
};

// Placeholder label used for the NTS slot when no provider is available
// (e.g. the shipped build has no SPKI pins populated yet).
static const char kNtsNoPinLabel[] = "NTS--";

// Published label buffer for the NTS slot. Pointed to by
// g_results[NTP_NTS_SLOT].label, so it MUST outlive the aggregator
// thread that filled it -- which is exactly what file-scope storage
// gives us. Guarded by g_cs on write and read.
static char g_ntsLabelPublished[32] = { 0 };

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
    int              index;    // 0..NTP_CORE_COUNT-1 for core workers
    NtpSourceResult  out;      // written by worker, read by aggregator
} WorkerCtx;

// Core (unauthenticated SNTP) worker. Only used for slots 0..2.
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

// NTS (authenticated) worker. Picks a provider at random from the
// SPKI-pinned pool, performs a full KE, then one authenticated SNTP
// round trip. Because the pool is small and the pick is per-cycle,
// this slot ALSO provides geographic and operator diversity against
// the core trio -- an adversary would need to simultaneously defeat
// the authenticated NTS provider PLUS two of the three core sources.
typedef struct {
    NtpSourceResult out;
    const char     *pickedLabel;   // filled in by worker with provider label
    char            labelBuf[32];  // "NTS:<provider>", owned by this struct
} NtsCtx;

static DWORD WINAPI NtsWorkerProc(LPVOID param) {
    NtsCtx *ctx = (NtsCtx *)param;
    NtpSourceResult *r = &ctx->out;
    memset(r, 0, sizeof(*r));
    r->label = kNtsNoPinLabel;

    const NtsProvider *p = Nts_PickProvider();
    if (p == NULL) {
        // No provider enabled (no pins populated) -- this slot fails
        // unconditionally, which means the concurrence gate will
        // drive INOP until the operator runs fetch_nts_spki_pins.py.
        ctx->pickedLabel = NULL;
        r->ok = 0;
        return 0;
    }

    // Copy the provider label into our owned buffer so the aggregator
    // can reference it after the worker returns.
    _snprintf(ctx->labelBuf, sizeof ctx->labelBuf, "NTS:%s",
              p->label ? p->label : "?");
    ctx->labelBuf[sizeof ctx->labelBuf - 1] = 0;
    r->label = ctx->labelBuf;
    ctx->pickedLabel = ctx->labelBuf;

    int64_t utc = 0, qpc = 0;
    uint32_t rtt = 0;
    if (Nts_FetchSample(p, &utc, &qpc, &rtt)) {
        r->ok       = 1;
        r->ntpUtcMs = utc;
        r->qpcAtT4  = qpc;
        r->rttMs    = rtt;
        r->offsetMs = utc;   // legacy field (overwritten with display value below)
    } else {
        r->ok = 0;
    }
    return 0;
}

// --- Aggregator thread ---------------------------------------------------
//
// Spawns four worker threads in parallel (three core SNTP + one NTS),
// waits for all of them to complete (or to time out at the socket /
// TLS level inside their respective query helpers), publishes per-
// source results, and delegates the trust verdict to Ntp_Concur.

// ---------------------------------------------------------------------------
// Audit log
// ---------------------------------------------------------------------------
//
// Every polling cycle appends one plain-text line to
// %APPDATA%\Lunar\audit.log. The file rotates to audit.log.1 at
// AUDIT_MAX_BYTES so the log can't grow unbounded. The format is line-
// oriented and greppable:
//
//   2026-04-21T12:34:56.789Z  OK    spread=  42ms  NIST:ok off= -12ms rtt= 85ms  PTB:ok off=  30ms rtt= 42ms  NICT:ok off=  10ms rtt=110ms  NTS:cloudflare:ok off=  0ms rtt=120ms
//   2026-04-21T12:35:01.000Z~ INOP  spread=   0ms  NIST:-- ...  PTB:ok ...  NICT:-- ...  NTS--:--
//
// "spread" here is the worst absolute deviation of any core source
// from the NTS anchor (in ms). On INOP cycles where NTS itself failed,
// spread is reported as 0.
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

    // Three core workers (slots 0..2) plus one NTS worker (slot 3).
    WorkerCtx core_ctx[NTP_CORE_COUNT];
    NtsCtx    nts_ctx;
    HANDLE    threads[NTP_SOURCE_COUNT] = { 0 };
    int       spawned = 0;

    for (int i = 0; i < NTP_CORE_COUNT; i++) {
        core_ctx[i].index = i;
        memset(&core_ctx[i].out, 0, sizeof(core_ctx[i].out));
        core_ctx[i].out.label = kSources[i].label;
        threads[i] = CreateThread(NULL, 0, WorkerProc, &core_ctx[i], 0, NULL);
        if (threads[i]) spawned++;
    }
    memset(&nts_ctx, 0, sizeof nts_ctx);
    threads[NTP_NTS_SLOT] = CreateThread(NULL, 0, NtsWorkerProc, &nts_ctx, 0, NULL);
    if (threads[NTP_NTS_SLOT]) spawned++;

    // Cap total wall time at the NTS slot's budget: the KE phase alone
    // can take ~5 s on a cold connect, plus ~3 s for the SNTP round
    // trip, plus slack for TLS handshake variability. Core SNTP
    // workers finish in ~NTP_TIMEOUT_MS; they just wait for the slow
    // slot to catch up.
    if (spawned > 0) {
        HANDLE hs[NTP_SOURCE_COUNT];
        int    nh = 0;
        for (int i = 0; i < NTP_SOURCE_COUNT; i++)
            if (threads[i]) hs[nh++] = threads[i];
        WaitForMultipleObjects((DWORD)nh, hs, TRUE, NTS_SLOT_TIMEOUT_MS);
        for (int i = 0; i < NTP_SOURCE_COUNT; i++)
            if (threads[i]) CloseHandle(threads[i]);
    }

    NtpSourceResult snapshot[NTP_SOURCE_COUNT];
    for (int i = 0; i < NTP_CORE_COUNT; i++) snapshot[i] = core_ctx[i].out;
    snapshot[NTP_NTS_SLOT] = nts_ctx.out;

    int64_t    bestUtcMs = 0;
    int64_t    bestQpc   = 0;
    int64_t    maxSpread = 0;
    TrustState trust = Ntp_Concur(snapshot, &bestUtcMs, &bestQpc, &maxSpread);

    // Rewrite snapshot[i].offsetMs to the MEANINGFUL display value:
    // this source's deviation from the cycle's trust anchor, projected
    // to a common QPC moment. NTS is the anchor when it succeeded;
    // otherwise fall back to the first successful source so the audit
    // log still carries informative per-source numbers.
    {
        int64_t qpcFreq = Clock_QpcFreq();
        if (qpcFreq <= 0) qpcFreq = 1;
        int64_t refQpc = 0, refUtc = 0;
        int have_ref = 0;
        if (snapshot[NTP_NTS_SLOT].ok) {
            refQpc = snapshot[NTP_NTS_SLOT].qpcAtT4;
            refUtc = snapshot[NTP_NTS_SLOT].ntpUtcMs;
            have_ref = 1;
        } else {
            for (int i = 0; i < NTP_SOURCE_COUNT; i++) {
                if (snapshot[i].ok) {
                    refQpc = snapshot[i].qpcAtT4;
                    refUtc = snapshot[i].ntpUtcMs;
                    have_ref = 1;
                    break;
                }
            }
        }
        for (int i = 0; i < NTP_SOURCE_COUNT; i++) {
            if (!snapshot[i].ok || !have_ref) { snapshot[i].offsetMs = 0; continue; }
            int64_t dq = refQpc - snapshot[i].qpcAtT4;
            int64_t dms = (dq >= 0)
                ? (dq * 1000LL + qpcFreq / 2) / qpcFreq
                : (dq * 1000LL - qpcFreq / 2) / qpcFreq;
            int64_t projected = snapshot[i].ntpUtcMs + dms;
            snapshot[i].offsetMs = projected - refUtc;
        }
    }

    // Publish shared state. The NTS slot's label points into
    // `nts_ctx.labelBuf`, which is an AggregatorProc local and will
    // cease to exist when this function returns; copy it into the
    // file-scope g_ntsLabelPublished buffer and rewrite the pointer
    // so About-dialog / audit-log readers don't chase a dangling
    // pointer.
    if (g_csInit) EnterCriticalSection(&g_cs);
    {
        const char *lbl = snapshot[NTP_NTS_SLOT].label
                          ? snapshot[NTP_NTS_SLOT].label : kNtsNoPinLabel;
        _snprintf(g_ntsLabelPublished, sizeof g_ntsLabelPublished, "%s", lbl);
        g_ntsLabelPublished[sizeof g_ntsLabelPublished - 1] = 0;
        snapshot[NTP_NTS_SLOT].label = g_ntsLabelPublished;
    }
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

    // Mirror a detailed summary into the rolling in-memory log so the
    // "Log" menu item can show recent cycles without hitting disk.
    // One header line + one line per slot carrying offset (vs. cycle
    // consensus), RTT, and server-believed UTC so operators can see
    // exactly what each source returned.
    {
        int okCount = 0;
        for (int i = 0; i < NTP_SOURCE_COUNT; i++)
            if (snapshot[i].ok) okCount++;
        const char *ntsLabel = snapshot[NTP_NTS_SLOT].label
                               ? snapshot[NTP_NTS_SLOT].label : "NTS--";
        Log_Append("ntp: cycle %s  ok=%d/%d  spread=%lldms  anchor=%s",
                   (trust == TRUST_OK) ? "OK" : "INOP",
                   okCount, NTP_SOURCE_COUNT,
                   (long long)maxSpread,
                   snapshot[NTP_NTS_SLOT].ok ? ntsLabel : "(NTS failed)");
        for (int i = 0; i < NTP_SOURCE_COUNT; i++) {
            const char *lbl = snapshot[i].label ? snapshot[i].label : "?";
            if (snapshot[i].ok) {
                Log_Append("  [%d] %-18s ok    offset=%+lldms  rtt=%ums  utc=%lld",
                           i, lbl,
                           (long long)snapshot[i].offsetMs,
                           (unsigned)snapshot[i].rttMs,
                           (long long)snapshot[i].ntpUtcMs);
            } else {
                Log_Append("  [%d] %-18s FAIL", i, lbl);
            }
        }
    }

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
// Per ntp.h: NTS (slot 3) is the trust anchor. The three core SNTP
// sources (slots 0..2) must corroborate it: at least 2 of 3 must agree
// to within CONCUR_THRESHOLD_MS after projection to a common QPC
// moment. Otherwise the verdict is INOP.
//
// Rationale: the NTS reply is cryptographically authenticated, so an
// on-path adversary cannot forge it without defeating TLS 1.3 plus our
// SPKI pin. Plain SNTP packets, in contrast, are forgeable. Using NTS
// as the anchor elevates authentication above geographic consensus:
// even if all three core sources were hijacked, the verdict would still
// be INOP because they would not match the authenticated NTS reading.
//
// Each source captured its qpcAtT4 at a slightly different moment, so
// we cannot compare ntpUtcMs values directly (the raw diff would
// include elapsed local time between captures). Instead we project
// every core source's UTC estimate onto the NTS source's qpc moment
// using the local QPC frequency, then threshold on the signed delta.
// This is pure arithmetic on monotonic QPC + server timestamps; the
// Windows system clock is never consulted.
//
// maxSpreadMs carries the largest absolute core deviation from the
// NTS anchor for logging, independent of the verdict.
TrustState Ntp_Concur(const NtpSourceResult results[NTP_SOURCE_COUNT],
                      int64_t *outBestUtcMs,
                      int64_t *outBestQpc,
                      int64_t *outMaxSpreadMs) {
    if (outBestUtcMs)   *outBestUtcMs   = 0;
    if (outBestQpc)     *outBestQpc     = 0;
    if (outMaxSpreadMs) *outMaxSpreadMs = 0;

    const NtpSourceResult *nts = &results[NTP_NTS_SLOT];
    if (!nts->ok) return TRUST_INOP;

    int64_t qpcFreq = Clock_QpcFreq();
    if (qpcFreq <= 0) return TRUST_INOP;

    int64_t refQpc = nts->qpcAtT4;
    int64_t refUtc = nts->ntpUtcMs;

    int concurring = 0;
    int64_t worstAbs = 0;
    for (int i = 0; i < NTP_CORE_COUNT; i++) {
        const NtpSourceResult *r = &results[i];
        if (!r->ok) continue;
        int64_t dq = refQpc - r->qpcAtT4;
        int64_t dms = (dq >= 0)
            ? (dq * 1000LL + qpcFreq / 2) / qpcFreq
            : (dq * 1000LL - qpcFreq / 2) / qpcFreq;
        int64_t projected = r->ntpUtcMs + dms;
        int64_t delta     = projected - refUtc;
        int64_t absDelta  = delta < 0 ? -delta : delta;
        if (absDelta > worstAbs) worstAbs = absDelta;
        if (absDelta <= CONCUR_THRESHOLD_MS) concurring++;
    }

    if (outMaxSpreadMs) *outMaxSpreadMs = worstAbs;

    if (concurring < 2) return TRUST_INOP;

    // NTS is the anchor: return its exact (utcMs, qpcAtT4) so the
    // clockwork anchor is sub-millisecond accurate regardless of
    // which core sources happened to agree.
    if (outBestUtcMs) *outBestUtcMs = nts->ntpUtcMs;
    if (outBestQpc)   *outBestQpc   = nts->qpcAtT4;
    return TRUST_OK;
}

// --- Public API ----------------------------------------------------------

void Ntp_Start(void) {
    if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = 1; }
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) {
        Log_Append("ntp: sync requested but a cycle is already in flight");
        return;
    }
    Log_Append("ntp: cycle start (4 slots: NIST, PTB, NICT, NTS)");
    HANDLE th = CreateThread(NULL, 0, AggregatorProc, NULL, 0, NULL);
    if (th) {
        CloseHandle(th);
    } else {
        InterlockedExchange(&g_running, 0);
        Log_Append("ntp: CreateThread failed (err=%lu); cycle aborted",
                   (unsigned long)GetLastError());
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
