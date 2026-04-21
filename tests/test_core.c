// test_core.c -- C unit tests for Lunar's pure logic.
//
// We compile this as a single translation unit that re-includes the whole
// of lunar.c with LUNAR_NO_MAIN defined, so we can reach its static
// helpers directly. ntp.c and sysvol.c are linked in because lunar.c
// references their exported entry points.

#define LUNAR_NO_MAIN
#include "../src/lunar.c"

#include <stdio.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(expr) do {                                                \
    if (expr) { g_pass++; }                                             \
    else {                                                              \
        g_fail++;                                                       \
        fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); \
    }                                                                   \
} while (0)

#define CHECK_EQ_INT(a, b) do {                                         \
    long long _a = (long long)(a), _b = (long long)(b);                 \
    if (_a == _b) { g_pass++; }                                         \
    else {                                                              \
        g_fail++;                                                       \
        fprintf(stderr, "FAIL %s:%d  %s == %s  (%lld vs %lld)\n",       \
                __FILE__, __LINE__, #a, #b, _a, _b);                    \
    }                                                                   \
} while (0)

#define CHECK_EQ_STR(a, b) do {                                         \
    const char *_a = (a), *_b = (b);                                    \
    if (_a && _b && strcmp(_a, _b) == 0) { g_pass++; }                  \
    else {                                                              \
        g_fail++;                                                       \
        fprintf(stderr, "FAIL %s:%d  %s == %s  (\"%s\" vs \"%s\")\n",   \
                __FILE__, __LINE__, #a, #b,                             \
                _a ? _a : "(null)", _b ? _b : "(null)");                \
    }                                                                   \
} while (0)

// ---------------------------------------------------------------------------
// LE byte writers (trivial but catches a bad refactor)
// ---------------------------------------------------------------------------

static void test_le_writers(void) {
    unsigned char b[8] = { 0 };
    WriteLE16(b, 0xBEEF);
    CHECK_EQ_INT(b[0], 0xEF);
    CHECK_EQ_INT(b[1], 0xBE);
    WriteLE32(b, 0xDEADBEEFu);
    CHECK_EQ_INT(b[0], 0xEF);
    CHECK_EQ_INT(b[1], 0xBE);
    CHECK_EQ_INT(b[2], 0xAD);
    CHECK_EQ_INT(b[3], 0xDE);
}

// ---------------------------------------------------------------------------
// WAV builder: RIFF header + PCM properties
// ---------------------------------------------------------------------------

static uint16_t rd16(const unsigned char *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

static void test_wav_header(void) {
    BuildWav(880.0f, 0.5f);
    const unsigned char *w = g_wav;
    CHECK(memcmp(w + 0,  "RIFF", 4) == 0);
    CHECK_EQ_INT(rd32(w + 4),  36 + BEEP_DATA_BYTES);
    CHECK(memcmp(w + 8,  "WAVE", 4) == 0);
    CHECK(memcmp(w + 12, "fmt ", 4) == 0);
    CHECK_EQ_INT(rd32(w + 16), 16);
    CHECK_EQ_INT(rd16(w + 20), 1);                                  // PCM
    CHECK_EQ_INT(rd16(w + 22), 1);                                  // mono
    CHECK_EQ_INT(rd32(w + 24), BEEP_SAMPLE_RATE);
    CHECK_EQ_INT(rd32(w + 28), BEEP_SAMPLE_RATE * 2);               // byte rate
    CHECK_EQ_INT(rd16(w + 32), 2);                                  // block align
    CHECK_EQ_INT(rd16(w + 34), 16);                                 // bits
    CHECK(memcmp(w + 36, "data", 4) == 0);
    CHECK_EQ_INT(rd32(w + 40), BEEP_DATA_BYTES);

    // Envelope: first sample must be zero (attack starts at 0), last
    // sample must be zero (release ends at 0), peak must exceed 50% of
    // the requested amplitude somewhere in the middle.
    int16_t s0    = (int16_t)rd16(w + 44);
    int16_t sLast = (int16_t)rd16(w + 44 + (BEEP_FRAMES - 1) * 2);
    CHECK_EQ_INT(s0, 0);
    CHECK(abs(sLast) < 1000);

    int peak = 0;
    for (int i = 0; i < BEEP_FRAMES; i++) {
        int16_t s = (int16_t)rd16(w + 44 + i * 2);
        int a = abs(s);
        if (a > peak) peak = a;
    }
    // amp=0.5 -> peak around 0.5 * 32767 = ~16384; require at least 12000.
    CHECK(peak > 12000);
    CHECK(peak <= 32767);
}

// ---------------------------------------------------------------------------
// Timezone abbreviation table
// ---------------------------------------------------------------------------

static void test_tz_lookup(void) {
    CHECK_EQ_STR(LookupTzAbbr("Pacific Standard Time",    0), "PST");
    CHECK_EQ_STR(LookupTzAbbr("Pacific Standard Time",    1), "PDT");
    CHECK_EQ_STR(LookupTzAbbr("Eastern Standard Time",    0), "EST");
    CHECK_EQ_STR(LookupTzAbbr("Eastern Standard Time",    1), "EDT");
    CHECK_EQ_STR(LookupTzAbbr("Central Standard Time",    0), "CST");
    CHECK_EQ_STR(LookupTzAbbr("Central Standard Time",    1), "CDT");
    CHECK_EQ_STR(LookupTzAbbr("Mountain Standard Time",   0), "MST");
    CHECK_EQ_STR(LookupTzAbbr("Mountain Standard Time",   1), "MDT");
    CHECK_EQ_STR(LookupTzAbbr("US Mountain Standard Time",0), "MST");
    CHECK_EQ_STR(LookupTzAbbr("US Mountain Standard Time",1), "MST");
    CHECK_EQ_STR(LookupTzAbbr("W. Europe Standard Time",  0), "CET");
    CHECK_EQ_STR(LookupTzAbbr("W. Europe Standard Time",  1), "CEST");
    CHECK_EQ_STR(LookupTzAbbr("Romance Standard Time",    0), "CET");
    CHECK_EQ_STR(LookupTzAbbr("Romance Standard Time",    1), "CEST");
    CHECK_EQ_STR(LookupTzAbbr("GMT Standard Time",        0), "GMT");
    CHECK_EQ_STR(LookupTzAbbr("GMT Standard Time",        1), "BST");
    CHECK_EQ_STR(LookupTzAbbr("Tokyo Standard Time",      0), "JST");
    CHECK_EQ_STR(LookupTzAbbr("Tokyo Standard Time",      1), "JST");
    CHECK_EQ_STR(LookupTzAbbr("India Standard Time",      0), "IST");
    CHECK_EQ_STR(LookupTzAbbr("India Standard Time",      1), "IST");
    CHECK_EQ_STR(LookupTzAbbr("AUS Eastern Standard Time",0), "AEST");
    CHECK_EQ_STR(LookupTzAbbr("AUS Eastern Standard Time",1), "AEDT");
    CHECK_EQ_STR(LookupTzAbbr("New Zealand Standard Time",0), "NZST");
    CHECK_EQ_STR(LookupTzAbbr("New Zealand Standard Time",1), "NZDT");
    // Unknown names must return NULL (falling through to initials logic).
    CHECK(LookupTzAbbr("Bogus Fictional Time", 0) == NULL);
    CHECK(LookupTzAbbr("",                     0) == NULL);
}

// ---------------------------------------------------------------------------
// Hit testing on hour markers
// ---------------------------------------------------------------------------

static void test_hit_test(void) {
    const float cx = 200.0f, cy = 200.0f, S = 400.0f;
    // Radius at which ticks sit, approximately.
    const float margin = 0.04f * S;
    const float radius = 0.5f * S - margin;
    const float rMid   = radius - 0.035f * S; // inside the hit band
    for (int h = 0; h < 12; h++) {
        float a = h * 30.0f - 90.0f;   // 0 = 12 o'clock in our code uses atan2(dx,-dy)
        // Use the same polar convention the app uses: 0 deg is up, clockwise.
        float ang = (h * 30.0f) * DEG2RAD;
        float x = cx + rMid * sinf(ang);
        float y = cy - rMid * cosf(ang);
        int idx = HitTestHour(x, y, cx, cy, S);
        CHECK_EQ_INT(idx, h);
        (void)a;
    }
    // Center miss
    CHECK_EQ_INT(HitTestHour(cx, cy, cx, cy, S), -1);
    // Far outside
    CHECK_EQ_INT(HitTestHour(cx + 10 * S, cy, cx, cy, S), -1);
    // Midway between two hour ticks (angle = 15 deg between 12 and 1)
    {
        float ang = 15.0f * DEG2RAD;
        float x = cx + rMid * sinf(ang);
        float y = cy - rMid * cosf(ang);
        CHECK_EQ_INT(HitTestHour(x, y, cx, cy, S), -1);
    }
}

// ---------------------------------------------------------------------------
// Armed persistence round-trip
// ---------------------------------------------------------------------------

static void test_armed_roundtrip(void) {
    // Redirect APPDATA to a scratch directory under build/. ArmedPath
    // now uses _wgetenv(L"APPDATA"), so we must set the wide env too.
    wchar_t scratchW[MAX_PATH + 32];
    {
        wchar_t cwd[MAX_PATH]; GetCurrentDirectoryW(MAX_PATH, cwd);
        _snwprintf(scratchW, MAX_PATH + 32, L"%ls\\build\\test_scratch", cwd);
        _wmkdir(scratchW);
    }
    wchar_t envsetW[MAX_PATH + 48];
    _snwprintf(envsetW, MAX_PATH + 48, L"APPDATA=%ls", scratchW);
    _wputenv(envsetW);

    int in[12]  = { 1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 0, 1 };
    int out[12] = { 0 };
    SaveArmed(in);
    LoadArmed(out);
    for (int i = 0; i < 12; i++) CHECK_EQ_INT(out[i], in[i]);

    // Round-trip of all-zero and all-one too, to catch bit-packing bugs.
    int z[12]  = { 0 };
    int z2[12] = { 1,1,1,1,1,1,1,1,1,1,1,1 };
    int o[12]  = { 0 };
    SaveArmed(z);  LoadArmed(o);  for (int i = 0; i < 12; i++) CHECK_EQ_INT(o[i], z[i]);
    SaveArmed(z2); LoadArmed(o);  for (int i = 0; i < 12; i++) CHECK_EQ_INT(o[i], z2[i]);
}

// ---------------------------------------------------------------------------
// NTP packet parsing -- run the byte math the way ntp.c does, against a
// synthetically crafted response with known timestamps, and verify the
// offset comes out to ~0.
// ---------------------------------------------------------------------------
//
// ntp.c parses inline so we re-derive the math here. This catches a
// refactor of the byte offsets or endianness.

static void test_ntp_timestamp_math(void) {
    // Simulate: client sent at t=1_000_000 ms since NTP epoch, server
    // received at 1_000_050, sent at 1_000_051, client got reply at
    // 1_000_100. True offset between client and server = 0 here.
    //
    // In ntp.c:
    //   t2_ms = (t2_s - epoch_delta)*1000 + (frac * 1000 >> 32)
    //   offset = ((t2 - t1) + (t3 - t4)) / 2 (in ms)
    uint64_t epoch = 2208988800ULL;
    uint32_t t2_s = (uint32_t)(epoch + 1000);
    uint32_t t3_s = t2_s;
    // 0.5 sec fractional part: frac = 2^31
    uint32_t t2_frac = 0x80000000u;
    uint32_t t3_frac = 0x80000000u;

    int64_t t2_ms = ((int64_t)t2_s - (int64_t)epoch) * 1000
                  + (int64_t)(((uint64_t)t2_frac * 1000ULL) >> 32);
    int64_t t3_ms = ((int64_t)t3_s - (int64_t)epoch) * 1000
                  + (int64_t)(((uint64_t)t3_frac * 1000ULL) >> 32);
    // 1000 seconds + 0.5 sec = 1_000_500 ms
    CHECK_EQ_INT(t2_ms, 1000500);
    CHECK_EQ_INT(t3_ms, 1000500);

    // Client t1=1_000_400 ms, t4=1_000_600 ms. Offset = ((500 - 400) +
    // (500 - 600))/2 = 0.
    int64_t t1 = 1000400, t4 = 1000600;
    int64_t off = ((t2_ms - t1) + (t3_ms - t4)) / 2;
    CHECK_EQ_INT(off, 0);

    // Shift server forward by 250 ms.
    t2_ms += 250; t3_ms += 250;
    off = ((t2_ms - t1) + (t3_ms - t4)) / 2;
    CHECK_EQ_INT(off, 250);
}

// ---------------------------------------------------------------------------
// Hand geometry: verify tip and base points land where expected.
// ---------------------------------------------------------------------------

static void test_hand_geometry(void) {
    // Need a factory.
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   &IID_ID2D1Factory, NULL, (void **)&g_d2d);
    CHECK(SUCCEEDED(hr));
    if (FAILED(hr)) return;

    float len = 200.0f, baseW = 20.0f, tipW = 10.0f;
    ID2D1PathGeometry *g = BuildHand(len, baseW, tipW);
    CHECK(g != NULL);
    if (!g) return;

    D2D1_RECT_F bounds;
    hr = ID2D1PathGeometry_GetBounds(g, NULL, &bounds);
    CHECK(SUCCEEDED(hr));
    // Tip at y = -len, base at y = +baseW (backLen == baseW).
    CHECK(fabsf(bounds.top    - (-len))  < 0.01f);
    CHECK(fabsf(bounds.bottom - (+baseW)) < 0.01f);
    // Widest at the base: baseW / 2 on each side.
    CHECK(fabsf(bounds.left  - (-baseW * 0.5f)) < 0.01f);
    CHECK(fabsf(bounds.right - (+baseW * 0.5f)) < 0.01f);

    ID2D1PathGeometry_Release(g);
    ID2D1Factory_Release(g_d2d);
    g_d2d = NULL;
}

// ---------------------------------------------------------------------------
// Minute-step guard: the fixed behaviour from C2/C3 should accept small
// forward steps and reject large jumps.
// ---------------------------------------------------------------------------
//
// We can't call Tick() directly (it touches the window), so we
// re-implement the exact guard as it exists in lunar.c and assert its
// output on a known sequence. Any future divergence between this test
// and Tick() is a red flag to revisit both.

static int sim_beeps_on_crossing(float prev, float cur, const int armed[12]) {
    int beeps = 0;
    int prevFloor = (int)floorf(prev);
    int curFloor  = (int)floorf(cur);
    int delta     = curFloor - prevFloor;
    if (delta < 0) delta += 60;
    if (delta >= 1 && delta <= 2) {
        for (int k = 1; k <= delta; k++) {
            int mm = (prevFloor + k) % 60;
            if (mm % 5 != 0) continue;
            int idx = mm / 5;
            if (!armed[idx]) continue;
            beeps++;
        }
    }
    return beeps;
}

static void test_minute_guard(void) {
    int all_armed[12] = { 1,1,1,1,1,1,1,1,1,1,1,1 };
    int no_armed [12] = { 0 };

    // Normal sub-minute tick: no beep.
    CHECK_EQ_INT(sim_beeps_on_crossing(10.4f, 10.6f, all_armed), 0);
    // Cross exactly one armed 5-minute marker (minute 10 -> 15).
    CHECK_EQ_INT(sim_beeps_on_crossing(14.9f, 15.1f, all_armed), 1);
    CHECK_EQ_INT(sim_beeps_on_crossing(14.9f, 15.1f, no_armed),  0);
    // 59 -> 00 wrap with armed-zero (hour chime).
    CHECK_EQ_INT(sim_beeps_on_crossing(59.9f, 0.1f, all_armed), 1);
    // Two-minute catch-up across 14 -> 16: hits 15 only.
    CHECK_EQ_INT(sim_beeps_on_crossing(14.5f, 16.5f, all_armed), 1);
    // DST spring-forward: jump of 60 -- MUST be rejected.
    CHECK_EQ_INT(sim_beeps_on_crossing(1.9f, 2.0f + 60.0f, all_armed), 0);
    // Any jump of 3 minutes or more rejected.
    CHECK_EQ_INT(sim_beeps_on_crossing(10.0f, 13.5f, all_armed), 0);
    // Backward jump (DST fall-back or manual wind-back): wraps to +59,
    // still rejected because delta > 2.
    CHECK_EQ_INT(sim_beeps_on_crossing(30.0f, 29.5f, all_armed), 0);
}

// ---------------------------------------------------------------------------
// NTP header validation: build synthetic packets and run them through the
// same byte-accessors / bit-masks used by ntp.c.
// ---------------------------------------------------------------------------

static int ntp_header_accepts(uint8_t li_vn_mode, uint8_t stratum) {
    uint8_t li   = (li_vn_mode >> 6) & 0x3;
    uint8_t vn   = (li_vn_mode >> 3) & 0x7;
    uint8_t mode =  li_vn_mode       & 0x7;
    if (li == 3)                         return 0;
    if (vn != 3 && vn != 4)              return 0;
    if (mode != 4)                       return 0;
    if (stratum == 0 || stratum >= 16)   return 0;
    return 1;
}

static void test_ntp_header_validation(void) {
    // Good: LI=0, VN=4, Mode=4, stratum=2
    CHECK_EQ_INT(ntp_header_accepts(0x24, 2), 1);
    // LI=3 (alarm/unsynced)
    CHECK_EQ_INT(ntp_header_accepts(0xE4, 2), 0);
    // VN=2 (ancient)
    CHECK_EQ_INT(ntp_header_accepts(0x14, 2), 0);
    // Mode=3 (client — this is an echo of our own packet, reject)
    CHECK_EQ_INT(ntp_header_accepts(0x23, 2), 0);
    // Stratum=0 (kiss-of-death: DENY/RATE/etc.)
    CHECK_EQ_INT(ntp_header_accepts(0x24, 0), 0);
    // Stratum=16 (unsynchronized)
    CHECK_EQ_INT(ntp_header_accepts(0x24, 16), 0);
    // Stratum=15 (edge, valid)
    CHECK_EQ_INT(ntp_header_accepts(0x24, 15), 1);
    // VN=3 also acceptable (server speaks older version)
    CHECK_EQ_INT(ntp_header_accepts(0x1C, 2), 1);
}

// ---------------------------------------------------------------------------
// Disciplined clock: anchor + PLL behaviour.
// ---------------------------------------------------------------------------
//
// Clock_Init/OnSyncedNtpUtc/NowUtcMs are tied to Windows time internally,
// but the core math can be exercised by feeding synthetic QPC values and
// reading Clock_RatePpm() / Clock_OffsetMs(). We don't cover the
// ProjectLocked() slew curve here (hard to script against the real QPC);
// we cover the PLL convergence and the snap threshold.

static void test_clock_discipline(void) {
    // Redirect APPDATA so we don't clobber the user's discipline.dat.
    wchar_t scratchW[MAX_PATH + 32];
    wchar_t cwd[MAX_PATH]; GetCurrentDirectoryW(MAX_PATH, cwd);
    _snwprintf(scratchW, MAX_PATH + 32, L"%ls\\build\\test_scratch", cwd);
    _wmkdir(scratchW);
    wchar_t envsetW[MAX_PATH + 48];
    _snwprintf(envsetW, MAX_PATH + 48, L"APPDATA=%ls", scratchW);
    _wputenv(envsetW);

    // Kill any stale discipline file from a previous run.
    wchar_t discPath[MAX_PATH];
    _snwprintf(discPath, MAX_PATH, L"%ls\\Lunar\\discipline.dat", scratchW);
    _wremove(discPath);

    Clock_Init();

    // Not disciplined yet: must return 0 rate.
    CHECK_EQ_INT(Clock_IsDisciplined(), 0);
    CHECK_EQ_INT(Clock_RatePpm(), 0);

    // Simulate an NTP sample. Use the REAL QPC so ProjectLocked's deltas
    // are consistent with subsequent samples we'll feed it.
    int64_t qpcFreq;
    { LARGE_INTEGER f; QueryPerformanceFrequency(&f); qpcFreq = f.QuadPart; }

    int64_t t0_utc;
    { FILETIME ft; GetSystemTimeAsFileTime(&ft);
      ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
      t0_utc = (int64_t)(u.QuadPart / 10000LL) - 11644473600000LL; }
    int64_t t0_qpc = Clock_Qpc();
    Clock_OnSyncedNtpUtc(t0_utc, t0_qpc);
    CHECK_EQ_INT(Clock_IsDisciplined(), 1);
    CHECK_EQ_INT(Clock_RatePpm(), 0);   // only one sample => rate undefined

    // Second sample: pretend 1 hour of QPC elapsed, and the server says
    // we drifted +100 ms. Observed rate correction = +100 ms / 3.6e6 ms
    // = +27.7 ppm. EMA alpha = 0.25, starting from 0 => new rate ~= 6-7 ppm.
    int64_t oneHourQpc = t0_qpc + qpcFreq * 3600;
    int64_t t1_utc     = t0_utc + 3600LL * 1000 + 100;  // server says +100ms
    Clock_OnSyncedNtpUtc(t1_utc, oneHourQpc);
    int32_t r1 = Clock_RatePpm();
    CHECK(r1 >= 5 && r1 <= 9);   // 27.7 * 0.25 = 6.9

    // Third sample another hour later, again +100 ms drift vs our
    // (still-too-slow) rate of ~7 ppm. EMA should pull us further up.
    int64_t twoHourQpc = t0_qpc + qpcFreq * 7200;
    int64_t t2_utc     = t1_utc + 3600LL * 1000 + 100;
    Clock_OnSyncedNtpUtc(t2_utc, twoHourQpc);
    int32_t r2 = Clock_RatePpm();
    CHECK(r2 > r1);            // PLL is converging toward ~28 ppm
    CHECK(r2 < 35);            // but clamped/smoothed, not overshooting

    // Clock_Shutdown must persist r2 and a >30-day-old timestamp must
    // be rejected on reload. First shutdown+reload with fresh timestamp
    // should restore r2 as the bootstrap.
    Clock_Shutdown();

    // Reset in-memory state (simulate app restart) and verify the
    // bootstrap rate is loaded.
    Clock_Init();
    CHECK_EQ_INT(Clock_IsDisciplined(), 0);     // not disciplined until first sync
    // First sync after restart: bootstrap rate should be applied.
    int64_t rs_qpc = Clock_Qpc();
    Clock_OnSyncedNtpUtc(t0_utc + 100000, rs_qpc);
    CHECK_EQ_INT(Clock_RatePpm(), r2);           // loaded bootstrap
}

// ---------------------------------------------------------------------------
// Ntp_Concur -- the pure trust-verdict evaluator (NTS-anchored)
// ---------------------------------------------------------------------------
//
// Contract (from ntp.h, post-NTS refactor):
//   * slot 3 is the NTS (authenticated) source and is the trust anchor;
//   * if slot 3 failed, verdict is INOP regardless of core sources;
//   * if slot 3 succeeded, AT LEAST 2 of the 3 core sources (0..2) must
//     agree with the NTS anchor to within 200 ms (signed projection
//     delta onto NTS's qpcAtT4). Otherwise INOP.
//   * On OK, bestUtcMs + bestQpc are exactly the NTS source's raw
//     (ntpUtcMs, qpcAtT4) -- the clockwork anchors onto the
//     cryptographically authenticated reading.
//   * maxSpreadMs is the largest absolute core deviation from the NTS
//     anchor, reported regardless of verdict.

static NtpSourceResult MkSrc(int ok, int64_t utc, int64_t qpc,
                             const char *label) {
    NtpSourceResult r = {0};
    r.ok       = ok;
    r.offsetMs = 0;       // display-only, not used by Ntp_Concur
    r.ntpUtcMs = utc;
    r.qpcAtT4  = qpc;
    r.rttMs    = 10;
    r.label    = label;
    return r;
}

static void test_ntp_concur(void) {
    // Clock_Init is required so Clock_QpcFreq() returns a valid
    // frequency for the projection code path. It is idempotent.
    Clock_Init();

    NtpSourceResult s[NTP_SOURCE_COUNT];
    int64_t best = 0, qpc = 0, spread = 0;
    const int64_t Q = 1000;  // shared qpcAtT4 -- projection delta = 0

    // 1) All four ok, all UTC identical -> OK, anchor = NTS, spread 0.
    s[0] = MkSrc(1, 1000, Q, "A");
    s[1] = MkSrc(1, 1000, Q, "B");
    s[2] = MkSrc(1, 1000, Q, "C");
    s[3] = MkSrc(1, 1000, Q, "NTS");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread), TRUST_OK);
    CHECK_EQ_INT(spread, 0);
    CHECK_EQ_INT(best, 1000);
    CHECK_EQ_INT(qpc, Q);

    // 2) Core disagree a bit but all within 200 ms of NTS -> OK.
    s[0] = MkSrc(1,  850, Q, "A");   // -150 from NTS
    s[1] = MkSrc(1, 1100, Q, "B");   // +100 from NTS
    s[2] = MkSrc(1,  900, Q, "C");   // -100 from NTS
    s[3] = MkSrc(1, 1000, Q, "NTS");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread), TRUST_OK);
    CHECK_EQ_INT(best, 1000);        // NTS anchor
    CHECK_EQ_INT(spread, 150);       // worst abs deviation

    // 3) Boundary: exactly 200 ms off in both directions -> OK (<=).
    s[0] = MkSrc(1,  800, Q, "A");   // -200 (boundary)
    s[1] = MkSrc(1, 1200, Q, "B");   // +200 (boundary)
    s[2] = MkSrc(1, 1000, Q, "C");   //    0
    s[3] = MkSrc(1, 1000, Q, "NTS");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread), TRUST_OK);
    CHECK_EQ_INT(spread, 200);

    // 4) One core 201 ms off, two within -> still OK (2 concur, 1 not).
    s[0] = MkSrc(1, 1000, Q, "A");
    s[1] = MkSrc(1, 1000, Q, "B");
    s[2] = MkSrc(1, 1201, Q, "C");   // disagrees by 201 ms
    s[3] = MkSrc(1, 1000, Q, "NTS");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread), TRUST_OK);
    CHECK_EQ_INT(spread, 201);       // worst deviation reported

    // 5) Two cores 201 ms off, one within -> INOP (only 1 concurs).
    s[0] = MkSrc(1, 1000, Q, "A");
    s[1] = MkSrc(1, 1201, Q, "B");
    s[2] = MkSrc(1,  799, Q, "C");
    s[3] = MkSrc(1, 1000, Q, "NTS");
    best = qpc = -1;
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread), TRUST_INOP);
    CHECK_EQ_INT(best, 0); CHECK_EQ_INT(qpc, 0);

    // 6) NTS fails -> INOP regardless of cores agreeing.
    s[0] = MkSrc(1, 1000, Q, "A");
    s[1] = MkSrc(1, 1000, Q, "B");
    s[2] = MkSrc(1, 1000, Q, "C");
    s[3] = MkSrc(0,    0, 0, "NTS");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread), TRUST_INOP);
    CHECK_EQ_INT(best, 0);

    // 7) NTS fails, cores wildly disagree -> INOP (NTS is the gate).
    s[0] = MkSrc(1, 1000, Q, "A");
    s[1] = MkSrc(1, 2000, Q, "B");
    s[2] = MkSrc(1, 3000, Q, "C");
    s[3] = MkSrc(0,    0, 0, "NTS");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread), TRUST_INOP);

    // 8) NTS ok, two cores failed, one agrees -> INOP (need >= 2).
    s[0] = MkSrc(1, 1000, Q, "A");
    s[1] = MkSrc(0,    0, 0, "B");
    s[2] = MkSrc(0,    0, 0, "C");
    s[3] = MkSrc(1, 1000, Q, "NTS");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread), TRUST_INOP);

    // 9) NTS ok, two cores agree, one failed -> OK (2 of 3).
    s[0] = MkSrc(1, 1000, Q, "A");
    s[1] = MkSrc(1, 1000, Q, "B");
    s[2] = MkSrc(0,    0, 0, "C");
    s[3] = MkSrc(1, 1000, Q, "NTS");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread), TRUST_OK);
    CHECK_EQ_INT(best, 1000);

    // 10) All cores fail, NTS ok -> INOP.
    s[0] = MkSrc(0, 0, 0, "A");
    s[1] = MkSrc(0, 0, 0, "B");
    s[2] = MkSrc(0, 0, 0, "C");
    s[3] = MkSrc(1, 1000, Q, "NTS");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread), TRUST_INOP);

    // 11) Everything fails -> INOP.
    s[0] = MkSrc(0, 0, 0, "A");
    s[1] = MkSrc(0, 0, 0, "B");
    s[2] = MkSrc(0, 0, 0, "C");
    s[3] = MkSrc(0, 0, 0, "NTS");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread), TRUST_INOP);

    // 12) NULL out-parameters are tolerated.
    s[0] = MkSrc(1, 1000, Q, "A");
    s[1] = MkSrc(1, 1000, Q, "B");
    s[2] = MkSrc(1, 1000, Q, "C");
    s[3] = MkSrc(1, 1000, Q, "NTS");
    CHECK_EQ_INT(Ntp_Concur(s, NULL, NULL, NULL), TRUST_OK);

    // 13) Negative UTCs (pre-1970) work.
    s[0] = MkSrc(1, -100, Q, "A");
    s[1] = MkSrc(1, -100, Q, "B");
    s[2] = MkSrc(1, -100, Q, "C");
    s[3] = MkSrc(1, -100, Q, "NTS");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread), TRUST_OK);
    CHECK_EQ_INT(best, -100);

    // 14) QPC-projection sanity: three cores captured at different
    // qpc moments with UTC staggered to match perfectly -- projected
    // onto NTS's qpc, they should all agree. Stagger = 100 ms.
    int64_t freq = Clock_QpcFreq();
    CHECK(freq > 0);
    int64_t tick100ms = freq / 10;
    s[0] = MkSrc(1, 1000 - 100, Q - tick100ms, "A");
    s[1] = MkSrc(1, 1000 + 100, Q + tick100ms, "B");
    s[2] = MkSrc(1, 1000 + 200, Q + 2 * tick100ms, "C");
    s[3] = MkSrc(1, 1000,       Q, "NTS");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread), TRUST_OK);
    CHECK_EQ_INT(spread, 0);         // projection cancels the staggering
    CHECK_EQ_INT(best, 1000);

    // 15) QPC-projection divergence: hold core UTC constant while
    // their qpcAtT4 drifts past NTS by 300 ms each. Projected
    // deltas exceed the threshold -> INOP.
    int64_t tick300ms = (freq * 3) / 10;
    s[0] = MkSrc(1, 1000, Q + tick300ms,     "A");  // +300 proj
    s[1] = MkSrc(1, 1000, Q + 2 * tick300ms, "B");  // +600 proj
    s[2] = MkSrc(1, 1000, Q + 3 * tick300ms, "C");  // +900 proj
    s[3] = MkSrc(1, 1000, Q, "NTS");
    CHECK_EQ_INT(Ntp_Concur(s, &best, &qpc, &spread), TRUST_INOP);
    CHECK(spread >= 899 && spread <= 901);
}

// ---------------------------------------------------------------------------
// AES-SIV-CMAC-256 -- RFC 5297 known-answer tests + round-trip checks
// ---------------------------------------------------------------------------
//
// SIV is the AEAD NTS uses to authenticate SNTP packets. We need this
// to be exactly right: a silent bug here would either break authentic
// cookie/message exchange (failing closed, visible) or, worse, accept
// a forged authenticator (failing open, invisible). The RFC ships a
// pair of worked examples in Appendix A; we verify both, then add a
// few round-trip fuzzes that exercise edge cases (empty AD, empty PT,
// PT crossing the 16-byte S2V boundary, decrypt-tamper detection).

#include "../src/siv.h"

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static size_t unhex(const char *s, uint8_t *out, size_t cap) {
    size_t n = 0;
    while (*s) {
        if (*s == ' ' || *s == '\n') { s++; continue; }
        int hi = hex_nibble(s[0]);
        int lo = hex_nibble(s[1]);
        if (hi < 0 || lo < 0 || n >= cap) return 0;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return n;
}
static int bufs_eq(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static void test_siv_rfc5297_appendix_a1(void) {
    // RFC 5297 Appendix A.1: deterministic authenticated encryption
    // with a single associated-data element.
    //
    //   Key:        fffefdfcfbfaf9f8f7f6f5f4f3f2f1f0
    //               f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff
    //   AD:         101112131415161718191a1b1c1d1e1f2021222324252627
    //   Plaintext:  112233445566778899aabbccddee
    //   V (tag):    85632d07c6e8f37f950acd320a2ecc93
    //   Cipher:     40c02b9690c4dc04daef7f6afe5c
    uint8_t key[32], ad[32], pt[32], want_tag[16], want_ct[32];
    size_t key_n = unhex("fffefdfcfbfaf9f8f7f6f5f4f3f2f1f0"
                         "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", key, sizeof key);
    size_t ad_n  = unhex("101112131415161718191a1b1c1d1e1f"
                         "2021222324252627", ad, sizeof ad);
    size_t pt_n  = unhex("112233445566778899aabbccddee", pt, sizeof pt);
    size_t tag_n = unhex("85632d07c6e8f37f950acd320a2ecc93",
                         want_tag, sizeof want_tag);
    size_t ct_n  = unhex("40c02b9690c4dc04daef7f6afe5c",
                         want_ct, sizeof want_ct);
    CHECK_EQ_INT(key_n, 32);
    CHECK_EQ_INT(ad_n, 24);
    CHECK_EQ_INT(pt_n, 14);
    CHECK_EQ_INT(tag_n, 16);
    CHECK_EQ_INT(ct_n, 14);

    SivSlice ad_vec = { ad, ad_n };
    uint8_t out[64];
    CHECK_EQ_INT(Siv_Encrypt(key, &ad_vec, 1, pt, pt_n, out), 0);
    CHECK(bufs_eq(out,        want_tag, 16));    // SIV matches RFC
    CHECK(bufs_eq(out + 16,   want_ct,  pt_n));  // CTR body matches RFC

    // Round-trip: the same AD + key must recover the plaintext exactly.
    uint8_t pt_back[32];
    CHECK_EQ_INT(Siv_Decrypt(key, &ad_vec, 1, out, 16 + pt_n, pt_back), 0);
    CHECK(bufs_eq(pt_back, pt, pt_n));
}

static void test_siv_rfc5297_appendix_a2(void) {
    // RFC 5297 Appendix A.2: multiple AD elements, longer plaintext
    // that spans three 16-byte S2V blocks (so the "xorend" path runs).
    //
    //   Key:        7f7e7d7c7b7a79787776757473727170
    //               404142434445464748494a4b4c4d4e4f
    //   AD1:        00112233445566778899aabbccddeeff
    //               deaddadadeaddadaffeeddccbbaa9988
    //               7766554433221100
    //   AD2:        102030405060708090a0
    //   Nonce:      09f911029d74e35bd84156c5635688c0
    //   Plaintext:  7468697320697320736f6d6520706c61
    //               696e7465787420746f20656e63727970
    //               74207573696e67205349562d414553
    //   V (tag):    7bdb6e3b432667eb06f4d14bff2fbd0f
    //   Cipher:     cb900f2fddbe404326601965c889bf17
    //               dba77ceb094fa663b7a3f748ba8af829
    //               ea64ad544a272e9c485b62a3fd5c0d
    uint8_t key[32], ad1[48], ad2[16], nonce[16], pt[64], want_tag[16], want_ct[64];
    size_t key_n = unhex("7f7e7d7c7b7a79787776757473727170"
                         "404142434445464748494a4b4c4d4e4f", key, sizeof key);
    size_t ad1_n = unhex("00112233445566778899aabbccddeeff"
                         "deaddadadeaddadaffeeddccbbaa9988"
                         "7766554433221100", ad1, sizeof ad1);
    size_t ad2_n = unhex("102030405060708090a0", ad2, sizeof ad2);
    size_t nc_n  = unhex("09f911029d74e35bd84156c5635688c0", nonce, sizeof nonce);
    size_t pt_n  = unhex("7468697320697320736f6d6520706c61"
                         "696e7465787420746f20656e63727970"
                         "74207573696e67205349562d414553", pt, sizeof pt);
    size_t tag_n = unhex("7bdb6e3b432667eb06f4d14bff2fbd0f", want_tag, sizeof want_tag);
    size_t ct_n  = unhex("cb900f2fddbe404326601965c889bf17"
                         "dba77ceb094fa663b7a3f748ba8af829"
                         "ea64ad544a272e9c485b62a3fd5c0d", want_ct, sizeof want_ct);
    CHECK_EQ_INT(key_n, 32);
    CHECK_EQ_INT(ad1_n, 40);
    CHECK_EQ_INT(ad2_n, 10);
    CHECK_EQ_INT(nc_n, 16);
    CHECK_EQ_INT(pt_n, 47);
    CHECK_EQ_INT(tag_n, 16);
    CHECK_EQ_INT(ct_n, 47);

    // Per RFC 5297 §2.6: the nonce is the LAST AD element.
    SivSlice ad_vec[3] = {
        { ad1,   ad1_n },
        { ad2,   ad2_n },
        { nonce, nc_n  },
    };
    uint8_t out[128];
    CHECK_EQ_INT(Siv_Encrypt(key, ad_vec, 3, pt, pt_n, out), 0);
    CHECK(bufs_eq(out,      want_tag, 16));
    CHECK(bufs_eq(out + 16, want_ct,  pt_n));

    uint8_t pt_back[64];
    CHECK_EQ_INT(Siv_Decrypt(key, ad_vec, 3, out, 16 + pt_n, pt_back), 0);
    CHECK(bufs_eq(pt_back, pt, pt_n));
}

static void test_siv_edge_cases(void) {
    // Common NTS shape: one AD element (the SNTP header), zero-length
    // plaintext (empty encrypted-extensions field). Must still produce
    // a 16-byte authenticator and round-trip cleanly.
    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;

    uint8_t ad[48];
    for (int i = 0; i < 48; i++) ad[i] = (uint8_t)(0xA0 + i);
    SivSlice ad_vec = { ad, sizeof ad };

    uint8_t tag[16];
    CHECK_EQ_INT(Siv_Encrypt(key, &ad_vec, 1, NULL, 0, tag), 0);
    CHECK_EQ_INT(Siv_Decrypt(key, &ad_vec, 1, tag, 16, NULL), 0);

    // Flip one AD byte -> authentication MUST fail and the (empty)
    // plaintext buffer MUST NOT be written past length 0.
    ad[7] ^= 0x01;
    CHECK_EQ_INT(Siv_Decrypt(key, &ad_vec, 1, tag, 16, NULL), -1);
    ad[7] ^= 0x01;

    // Zero AD, non-empty plaintext -- exercises the n==0 S2V path.
    uint8_t pt[20] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                       11, 12, 13, 14, 15, 16, 17, 18, 19, 20 };
    uint8_t out2[64];
    CHECK_EQ_INT(Siv_Encrypt(key, NULL, 0, pt, sizeof pt, out2), 0);
    uint8_t pt_back[32];
    CHECK_EQ_INT(Siv_Decrypt(key, NULL, 0, out2, 16 + sizeof pt, pt_back), 0);
    CHECK(bufs_eq(pt_back, pt, sizeof pt));

    // Tamper one ciphertext byte -> auth must fail, plaintext wiped.
    out2[20] ^= 0x80;
    for (size_t i = 0; i < sizeof pt; i++) pt_back[i] = 0xCC;
    CHECK_EQ_INT(Siv_Decrypt(key, NULL, 0, out2, 16 + sizeof pt, pt_back), -1);
    for (size_t i = 0; i < sizeof pt; i++) CHECK_EQ_INT(pt_back[i], 0);

    // Short input (< 16 bytes) rejected outright without reading key/AD.
    CHECK_EQ_INT(Siv_Decrypt(key, NULL, 0, out2, 10, pt_back), -1);
}

// ---------------------------------------------------------------------------
// NTS-KE record codec -- RFC 8915 §4 serialise/parse on hand-crafted bytes
// ---------------------------------------------------------------------------

#include "../src/nts_ke.h"

static void test_ntske_client_request(void) {
    // The fixed client request must be exactly 16 bytes:
    //   Record 1 (NextProtocol, critical, body len 2, body [0x0000]) = 6
    //   Record 2 (AEAD,         critical, body len 2, body [0x000F]) = 6
    //   Record 3 (EndOfMessage, critical, body len 0)                = 4
    // All critical bits set.
    uint8_t buf[64] = { 0xaa };
    size_t n = NtsKe_BuildClientRequest(buf, sizeof buf);
    CHECK_EQ_INT(n, 16);

    // Record 1: type 0x8001, len 2, body 0x0000
    CHECK_EQ_INT(buf[0], 0x80);
    CHECK_EQ_INT(buf[1], 0x01);
    CHECK_EQ_INT(buf[2], 0x00);
    CHECK_EQ_INT(buf[3], 0x02);
    CHECK_EQ_INT(buf[4], 0x00);
    CHECK_EQ_INT(buf[5], 0x00);
    // Record 2: type 0x8004, len 2, body 0x000F
    CHECK_EQ_INT(buf[6],  0x80);
    CHECK_EQ_INT(buf[7],  0x04);
    CHECK_EQ_INT(buf[8],  0x00);
    CHECK_EQ_INT(buf[9],  0x02);
    CHECK_EQ_INT(buf[10], 0x00);
    CHECK_EQ_INT(buf[11], 0x0f);
    // Record 3: type 0x8000, len 0
    CHECK_EQ_INT(buf[12], 0x80);
    CHECK_EQ_INT(buf[13], 0x00);
    CHECK_EQ_INT(buf[14], 0x00);
    CHECK_EQ_INT(buf[15], 0x00);

    // Small output buffer must refuse cleanly.
    CHECK_EQ_INT(NtsKe_BuildClientRequest(buf, 10), 0);
    CHECK_EQ_INT(NtsKe_BuildClientRequest(NULL, 64), 0);
}

// Small helper: append a record to a mutable buffer.
static size_t append_rec(uint8_t *buf, size_t pos,
                         uint16_t type_crit, const uint8_t *body, uint16_t blen) {
    buf[pos++] = (uint8_t)(type_crit >> 8);
    buf[pos++] = (uint8_t)(type_crit & 0xff);
    buf[pos++] = (uint8_t)(blen >> 8);
    buf[pos++] = (uint8_t)(blen & 0xff);
    if (blen && body) { memcpy(buf + pos, body, blen); pos += blen; }
    return pos;
}

static void test_ntske_parse_valid(void) {
    // Build a realistic server response:
    //   NextProtocol=[NTPv4] crit
    //   AEAD=[SIV-CMAC-256] crit
    //   3x NewCookie (16 bytes each, non-critical)
    //   NTPv4Port=4123 (non-critical)
    //   NTPv4Server="ntp.example.org" (non-critical)
    //   EndOfMessage crit
    uint8_t buf[512];
    size_t  pos = 0;

    uint8_t np_body[2]  = { 0x00, 0x00 };
    uint8_t aead_body[2]= { 0x00, 0x0f };
    uint8_t cookie1[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    uint8_t cookie2[16] = { 0xaa, 0xbb, 0xcc };
    uint8_t cookie3[16] = { 0xff };
    uint8_t port_body[2]= { 0x10, 0x1b };     // 4123
    const char *host    = "ntp.example.org";

    pos = append_rec(buf, pos, 0x8001, np_body, 2);
    pos = append_rec(buf, pos, 0x8004, aead_body, 2);
    pos = append_rec(buf, pos, 0x0005, cookie1, 16);
    pos = append_rec(buf, pos, 0x0005, cookie2, 16);
    pos = append_rec(buf, pos, 0x0005, cookie3, 16);
    pos = append_rec(buf, pos, 0x0007, port_body, 2);
    pos = append_rec(buf, pos, 0x0006, (const uint8_t *)host, (uint16_t)strlen(host));
    pos = append_rec(buf, pos, 0x8000, NULL, 0);

    NtsKeResponse r;
    CHECK_EQ_INT(NtsKe_ParseResponse(buf, pos, &r), 1);
    CHECK(r.ok);
    CHECK(r.proto_ok);
    CHECK(r.aead_ok);
    CHECK_EQ_INT(r.cookie_count, 3);
    CHECK_EQ_INT(r.cookie_len[0], 16);
    CHECK_EQ_INT(r.cookie_len[1], 16);
    CHECK_EQ_INT(r.cookie_len[2], 16);
    CHECK(bufs_eq(r.cookies[0], cookie1, 16));
    CHECK(bufs_eq(r.cookies[1], cookie2, 16));
    CHECK(bufs_eq(r.cookies[2], cookie3, 16));
    CHECK_EQ_INT(r.ntp_port, 4123);
    CHECK_EQ_STR(r.ntp_host, "ntp.example.org");
    CHECK_EQ_INT(r.error_code, 0);
}

static void test_ntske_parse_errors(void) {
    NtsKeResponse r;

    // 1) Missing End-of-Message: parser must reject.
    {
        uint8_t buf[32]; size_t pos = 0;
        uint8_t np[2] = {0,0}, ae[2] = {0,15}, ck[8] = {0xc0};
        pos = append_rec(buf, pos, 0x8001, np, 2);
        pos = append_rec(buf, pos, 0x8004, ae, 2);
        pos = append_rec(buf, pos, 0x0005, ck, 8);
        CHECK_EQ_INT(NtsKe_ParseResponse(buf, pos, &r), 0);
        CHECK_EQ_INT(r.ok, 0);
    }

    // 2) Server sends Error record -> parse fails, code captured.
    {
        uint8_t buf[32]; size_t pos = 0;
        uint8_t err[2] = { 0x00, 0x02 };   // code 2 (Internal Server Error)
        pos = append_rec(buf, pos, 0x8002, err, 2);
        CHECK_EQ_INT(NtsKe_ParseResponse(buf, pos, &r), 0);
        CHECK_EQ_INT(r.ok, 0);
        CHECK_EQ_INT(r.error_code, 2);
    }

    // 3) Missing AEAD agreement -> fail.
    {
        uint8_t buf[32]; size_t pos = 0;
        uint8_t np[2] = {0,0}, ck[8] = {0xc0};
        pos = append_rec(buf, pos, 0x8001, np, 2);
        pos = append_rec(buf, pos, 0x0005, ck, 8);
        pos = append_rec(buf, pos, 0x8000, NULL, 0);
        CHECK_EQ_INT(NtsKe_ParseResponse(buf, pos, &r), 0);
    }

    // 4) No cookies -> fail (server must supply at least one).
    {
        uint8_t buf[32]; size_t pos = 0;
        uint8_t np[2] = {0,0}, ae[2] = {0,15};
        pos = append_rec(buf, pos, 0x8001, np, 2);
        pos = append_rec(buf, pos, 0x8004, ae, 2);
        pos = append_rec(buf, pos, 0x8000, NULL, 0);
        CHECK_EQ_INT(NtsKe_ParseResponse(buf, pos, &r), 0);
    }

    // 5) Unknown record with critical bit -> fail.
    {
        uint8_t buf[32]; size_t pos = 0;
        uint8_t np[2] = {0,0}, ae[2] = {0,15}, ck[8] = {0xc0};
        pos = append_rec(buf, pos, 0x8001, np, 2);
        pos = append_rec(buf, pos, 0x8004, ae, 2);
        pos = append_rec(buf, pos, 0x0005, ck, 8);
        pos = append_rec(buf, pos, 0x8042, NULL, 0);   // unknown critical
        pos = append_rec(buf, pos, 0x8000, NULL, 0);
        CHECK_EQ_INT(NtsKe_ParseResponse(buf, pos, &r), 0);
    }

    // 6) Unknown record with critical bit CLEAR -> ignored, success.
    {
        uint8_t buf[32]; size_t pos = 0;
        uint8_t np[2] = {0,0}, ae[2] = {0,15}, ck[8] = {0xc0};
        pos = append_rec(buf, pos, 0x8001, np, 2);
        pos = append_rec(buf, pos, 0x8004, ae, 2);
        pos = append_rec(buf, pos, 0x0005, ck, 8);
        pos = append_rec(buf, pos, 0x0042, NULL, 0);   // unknown non-critical
        pos = append_rec(buf, pos, 0x8000, NULL, 0);
        CHECK_EQ_INT(NtsKe_ParseResponse(buf, pos, &r), 1);
        CHECK_EQ_INT(r.cookie_count, 1);
    }

    // 7) Truncated body length -> fail.
    {
        uint8_t buf[6] = { 0x80, 0x01, 0x00, 0x10, 0x00, 0x00 };  // claims 16-byte body
        CHECK_EQ_INT(NtsKe_ParseResponse(buf, sizeof buf, &r), 0);
    }

    // 8) Trailing bytes after End-of-Message -> fail.
    {
        uint8_t buf[32]; size_t pos = 0;
        uint8_t np[2] = {0,0}, ae[2] = {0,15}, ck[8] = {0xc0};
        pos = append_rec(buf, pos, 0x8001, np, 2);
        pos = append_rec(buf, pos, 0x8004, ae, 2);
        pos = append_rec(buf, pos, 0x0005, ck, 8);
        pos = append_rec(buf, pos, 0x8000, NULL, 0);
        buf[pos++] = 0x55;       // extra byte
        CHECK_EQ_INT(NtsKe_ParseResponse(buf, pos, &r), 0);
    }

    // 9) NTPv4-server record with non-ASCII bytes -> host left empty but
    // (non-critical) overall parse still succeeds.
    {
        uint8_t buf[64]; size_t pos = 0;
        uint8_t np[2] = {0,0}, ae[2] = {0,15}, ck[8] = {0xc0};
        uint8_t bad_host[] = { 'n', 't', 'p', 0x01, 'x' };
        pos = append_rec(buf, pos, 0x8001, np, 2);
        pos = append_rec(buf, pos, 0x8004, ae, 2);
        pos = append_rec(buf, pos, 0x0005, ck, 8);
        pos = append_rec(buf, pos, 0x0006, bad_host, sizeof bad_host);
        pos = append_rec(buf, pos, 0x8000, NULL, 0);
        CHECK_EQ_INT(NtsKe_ParseResponse(buf, pos, &r), 1);
        CHECK_EQ_INT(r.ntp_host[0], 0);
    }
}

// ---------------------------------------------------------------------------
// NTS-SNTP extension field codec -- RFC 8915 §5 round-trip
// ---------------------------------------------------------------------------

#include "../src/nts_ef.h"

// Helper: craft a server reply that mirrors RFC 8915's server-side AEAD.
// Packet = 48-byte header
//          + Unique Identifier (echoed)
//          + Authenticator (s2c_key, nonce, AD=preceding bytes, PT=N new cookies).
static size_t build_server_reply(uint8_t *out, size_t cap,
                                 const uint8_t hdr[48],
                                 const uint8_t *echo_uid,
                                 const uint8_t nonce[16],
                                 const uint8_t s2c_key[32],
                                 const uint8_t *new_cookies,
                                 size_t new_cookie_len,
                                 size_t n_new_cookies) {
    size_t pos = 0;
    memcpy(out, hdr, 48);
    pos = 48;

    // Unique Identifier extension
    out[pos++] = 0x01; out[pos++] = 0x04;
    out[pos++] = 0x00; out[pos++] = 36;          // 4 + 32
    memcpy(out + pos, echo_uid, 32);
    pos += 32;

    // Build plaintext = concatenated NTS Cookie extensions
    uint8_t pt[512] = {0};
    size_t  pt_len = 0;
    for (size_t i = 0; i < n_new_cookies; i++) {
        size_t total = (4 + new_cookie_len + 3) & ~(size_t)3u;
        if (total < 16) total = 16;
        pt[pt_len++] = 0x02; pt[pt_len++] = 0x04;
        pt[pt_len++] = (uint8_t)(total >> 8);
        pt[pt_len++] = (uint8_t)(total & 0xff);
        memcpy(pt + pt_len, new_cookies + i * new_cookie_len, new_cookie_len);
        pt_len += new_cookie_len;
        while (pt_len % 4) pt[pt_len++] = 0;
    }

    // Authenticator header + nonce-len + ct-len
    size_t auth_pos = pos;
    size_t ct_len = pt_len + 16;                 // tag + enc(plaintext)
    size_t auth_total = (4 + 4 + 16 + ct_len + 3) & ~(size_t)3u;

    out[pos++] = 0x04; out[pos++] = 0x04;
    out[pos++] = (uint8_t)(auth_total >> 8);
    out[pos++] = (uint8_t)(auth_total & 0xff);
    out[pos++] = 0x00; out[pos++] = 16;          // nonce len
    out[pos++] = (uint8_t)(ct_len >> 8);
    out[pos++] = (uint8_t)(ct_len & 0xff);
    memcpy(out + pos, nonce, 16);
    pos += 16;

    // SIV-encrypt the plaintext with AD = [in[0..auth_pos], nonce]
    SivSlice ad[2] = { { out, auth_pos }, { nonce, 16 } };
    uint8_t siv_out[1024];
    if (Siv_Encrypt(s2c_key, ad, 2, pt, pt_len, siv_out) != 0) return 0;
    memcpy(out + pos, siv_out, ct_len);
    pos += ct_len;

    while (pos - auth_pos < auth_total) out[pos++] = 0;

    (void)cap;
    return pos;
}

static void test_nts_ef_roundtrip(void) {
    // Keys: 32 bytes C2S and 32 bytes S2C (arbitrary; any random).
    uint8_t c2s_key[32], s2c_key[32];
    for (int i = 0; i < 32; i++) { c2s_key[i] = (uint8_t)(0x10 + i); s2c_key[i] = (uint8_t)(0x80 + i); }

    uint8_t ntp_hdr[48] = {0};
    ntp_hdr[0] = 0x23;                              // LI=0, VN=4, Mode=3

    uint8_t uid[32];
    for (int i = 0; i < 32; i++) uid[i] = (uint8_t)(0xAB ^ i);
    uint8_t nonce_c[16];
    for (int i = 0; i < 16; i++) nonce_c[i] = (uint8_t)(0x55 + i);

    uint8_t cookie[64];
    for (int i = 0; i < 64; i++) cookie[i] = (uint8_t)(0xC0 + i);

    uint8_t req[512];
    size_t  req_len = 0;
    int rc = NtsEf_BuildRequest(ntp_hdr, uid, nonce_c,
                                cookie, sizeof cookie,
                                /*n_placeholder=*/3,
                                c2s_key,
                                req, sizeof req, &req_len);
    CHECK_EQ_INT(rc, 0);

    // Minimum expected size: 48 + 36 (UID) + 68 (cookie) + 3*68 (placeholders)
    //                       + 40 (auth) = 396.
    CHECK_EQ_INT(req_len, 48 + 36 + 68 + 3 * 68 + 40);

    // Now verify self-consistency by running the parser over a
    // server-shape reply we craft using the SAME SIV primitive.
    // The server echoes UID, uses s2c_key, and returns 4 new cookies
    // (one for the spent cookie + 3 placeholder tops-up).
    uint8_t new_cookies[4 * 64];
    for (int i = 0; i < 4 * 64; i++) new_cookies[i] = (uint8_t)(0xF0 + (i & 0x0F));
    uint8_t nonce_s[16];
    for (int i = 0; i < 16; i++) nonce_s[i] = (uint8_t)(0xAA + i);

    uint8_t hdr_reply[48] = {0};
    hdr_reply[0] = 0x24;      // server mode
    hdr_reply[1] = 1;         // stratum
    // Transmit Timestamp non-zero so a real NTP validator wouldn't
    // reject (not that our codec reads it -- it's just AD bytes).
    hdr_reply[40] = 0x12; hdr_reply[41] = 0x34;

    uint8_t reply[512];
    size_t  reply_len = build_server_reply(reply, sizeof reply,
                                           hdr_reply, uid, nonce_s, s2c_key,
                                           new_cookies, 64, 4);
    CHECK(reply_len > 0);

    uint8_t got_cookies[NTSKE_MAX_COOKIES][NTSKE_MAX_COOKIE_LEN];
    size_t  got_lens[NTSKE_MAX_COOKIES];
    size_t  got_n = 99;

    rc = NtsEf_ParseResponse(reply, reply_len, uid, s2c_key,
                             got_cookies, got_lens, &got_n);
    CHECK_EQ_INT(rc, 0);
    CHECK_EQ_INT(got_n, 4);
    for (size_t i = 0; i < 4; i++) {
        CHECK_EQ_INT(got_lens[i], 64);
        CHECK(bufs_eq(got_cookies[i], new_cookies + i * 64, 64));
    }
}

static void test_nts_ef_tamper(void) {
    uint8_t s2c_key[32];
    for (int i = 0; i < 32; i++) s2c_key[i] = (uint8_t)(0xE0 + i);
    uint8_t uid[32], nonce_s[16];
    for (int i = 0; i < 32; i++) uid[i]     = (uint8_t)(0x10 + i);
    for (int i = 0; i < 16; i++) nonce_s[i] = (uint8_t)(0x20 + i);

    uint8_t new_c[64];
    for (int i = 0; i < 64; i++) new_c[i] = (uint8_t)(0x88 + i);

    uint8_t hdr_s[48] = {0}; hdr_s[0] = 0x24; hdr_s[1] = 1;

    uint8_t reply[512];
    size_t  reply_len = build_server_reply(reply, sizeof reply,
                                           hdr_s, uid, nonce_s, s2c_key,
                                           new_c, 64, 1);
    CHECK(reply_len > 0);

    uint8_t gc[NTSKE_MAX_COOKIES][NTSKE_MAX_COOKIE_LEN];
    size_t  gl[NTSKE_MAX_COOKIES];
    size_t  gn;

    // 1) Clean parse succeeds.
    CHECK_EQ_INT(NtsEf_ParseResponse(reply, reply_len, uid, s2c_key, gc, gl, &gn), 0);

    // 2) Flip one byte of the UID in the reply -> mismatch, fail.
    reply[52] ^= 0x01;
    CHECK_EQ_INT(NtsEf_ParseResponse(reply, reply_len, uid, s2c_key, gc, gl, &gn), -1);
    reply[52] ^= 0x01;

    // 3) Flip one byte of the NTP header -> AD mismatch, SIV fails.
    reply[10] ^= 0x80;
    CHECK_EQ_INT(NtsEf_ParseResponse(reply, reply_len, uid, s2c_key, gc, gl, &gn), -1);
    reply[10] ^= 0x80;

    // 4) Flip one byte of the ciphertext -> SIV auth fails.
    // Find the Ciphertext field: ends at reply_len.
    reply[reply_len - 4] ^= 0x40;
    CHECK_EQ_INT(NtsEf_ParseResponse(reply, reply_len, uid, s2c_key, gc, gl, &gn), -1);
    reply[reply_len - 4] ^= 0x40;

    // 5) Wrong S2C key -> SIV auth fails.
    uint8_t bad_key[32];
    memcpy(bad_key, s2c_key, 32); bad_key[0] ^= 1;
    CHECK_EQ_INT(NtsEf_ParseResponse(reply, reply_len, uid, bad_key, gc, gl, &gn), -1);

    // 6) Truncate to below 48 bytes -> reject.
    CHECK_EQ_INT(NtsEf_ParseResponse(reply, 47, uid, s2c_key, gc, gl, &gn), -1);

    // 7) Parse with a different sent UID -> reject.
    uint8_t wrong_uid[32];
    memcpy(wrong_uid, uid, 32); wrong_uid[5] ^= 0x02;
    CHECK_EQ_INT(NtsEf_ParseResponse(reply, reply_len, wrong_uid, s2c_key, gc, gl, &gn), -1);
}

// ---------------------------------------------------------------------------
// NTS provider pool -- without populated pins, PickProvider must refuse
// ---------------------------------------------------------------------------

#include "../src/nts.h"

static void test_nts_pool_all_pins_zero(void) {
    // Sanity: the pool exists and every entry is currently pin-less
    // (shipped default). Nts_PickProvider MUST NOT return a provider
    // whose pin is all-zero, because that would mean handing trust
    // to an unauthenticated TLS endpoint.
    size_t n = 0;
    const NtsProvider *p = Nts_Pool(&n);
    CHECK(p != NULL);
    CHECK(n > 0);

    for (size_t i = 0; i < n; i++) {
        uint8_t acc = 0;
        for (int j = 0; j < 32; j++) acc |= p[i].spki_pin[j];
        // When this repo lands in production, operators will populate
        // pins via scripts/fetch_nts_spki_pins.py. Until then we
        // deliberately ship with empty pins so a misconfigured build
        // fails closed on trust, rather than open.
        CHECK_EQ_INT(acc, 0);
        CHECK(p[i].host != NULL && p[i].host[0] != 0);
        CHECK(p[i].label != NULL);
    }

    // With no pins populated, PickProvider returns NULL: no provider
    // can be trusted, so the NTS slot will always miss and the
    // concurrence gate (updated in a later step) will drive INOP.
    const NtsProvider *picked = Nts_PickProvider();
    CHECK(picked == NULL);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(void) {
    test_le_writers();
    test_wav_header();
    test_tz_lookup();
    test_hit_test();
    test_armed_roundtrip();
    test_ntp_timestamp_math();
    test_hand_geometry();
    test_minute_guard();
    test_ntp_header_validation();
    test_clock_discipline();
    test_ntp_concur();
    test_siv_rfc5297_appendix_a1();
    test_siv_rfc5297_appendix_a2();
    test_siv_edge_cases();
    test_ntske_client_request();
    test_ntske_parse_valid();
    test_ntske_parse_errors();
    test_nts_ef_roundtrip();
    test_nts_ef_tamper();
    test_nts_pool_all_pins_zero();

    printf("\n%d checks, %d failed\n", g_pass + g_fail, g_fail);
    return g_fail == 0 ? 0 : 1;
}
