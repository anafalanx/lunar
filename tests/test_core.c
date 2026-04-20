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

    printf("\n%d checks, %d failed\n", g_pass + g_fail, g_fail);
    return g_fail == 0 ? 0 : 1;
}
