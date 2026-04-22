// Lunar 0.2 -- Direct2D port.
//
// A minimalist Braun BN0032-style analog clock drawn with Direct2D and
// DirectWrite on a plain Win32 HWND. No persistent audio device. The
// window is event-driven: a 100 ms WM_TIMER invalidates the client
// area, which triggers WM_PAINT. Between ticks the process is idle.
//
// Persistence, NTP sync and system-volume query live in separate TUs
// (ntp.c, sysvol.c). The rest -- window, menu, paint, beep, layout --
// is here.

#define COBJMACROS
#define CINTERFACE
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <initguid.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>

// Older SDKs may not define these; they exist on Vista+ (thumbnails) and
// Windows 7+ (live-preview bitmap). We hard-code the message numbers so
// the build works on MinGW headers that predate them.
#ifndef WM_DWMSENDICONICTHUMBNAIL
#define WM_DWMSENDICONICTHUMBNAIL         0x0323
#endif
#ifndef WM_DWMSENDICONICLIVEPREVIEWBITMAP
#define WM_DWMSENDICONICLIVEPREVIEWBITMAP 0x0326
#endif

#include <time.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <direct.h>
#include <string.h>
#include <ctype.h>
#include <commctrl.h>

#include "ntp.h"
#include "clock.h"
#include "logbuf.h"
#include "tz.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define APP_TITLE        L"Lunar 0.2"
#define CLASS_NAME       L"LunarWin"
#define DEFAULT_W        600
#define DEFAULT_H        600
#define TICK_MS          200           // 5 fps sweep cadence
// Poll cadence: aggressive 5 s retry while INOP to minimize outage,
// and a gentle 60 s when all three sources concur. There is no
// degraded middle ground -- the trust state is binary.
#define NTP_INTERVAL_OK_MS      60000   // 60 s
#define NTP_INTERVAL_INOP_MS     5000   //  5 s

// System-menu command IDs. Must be in 1..0xEFFF (>= 0xF000 is reserved).
#define IDM_ALWAYS_ON_TOP 0x1001
#define IDM_TEST_BEEP     0x1002
#define IDM_SETTINGS      0x1003
#define IDM_ABOUT         0x1004
#define IDM_SYNC_NOW      0x1005   // deprecated, kept to preserve following IDs
#define IDM_LOG           0x1006

// Settings dialog + controls.
#define IDD_SETTINGS          100
#define IDC_CHK_CHIMES        1001
#define IDC_CHK_UNMIN         1002
#define IDC_CHK_CONFIRM_CLOSE 1003
#define IDC_CBO_TZ            1004
#define IDC_EDIT_TZ_FILTER    1005
#define IDC_STATIC_TZ_PREVIEW 1006
#define IDC_BTN_TEST_CHIME    1007
#define IDC_BTN_DEFAULTS      1008

// Dialog-scoped timer (dialogs own a separate timer namespace from
// the main window, so reusing IDT_REPAINT is safe).
#define IDT_SETTINGS_PREVIEW  2

#define IDT_REPAINT       1

#ifndef PI
#define PI 3.14159265358979323846f
#endif
#define DEG2RAD (PI / 180.0f)

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

typedef struct {
    D2D1_COLOR_F bg, face, ink, inkSoft, ring, accent;
} Palette;

static D2D1_COLOR_F RGB255(int r, int g, int b) {
    D2D1_COLOR_F c = { r / 255.0f, g / 255.0f, b / 255.0f, 1.0f };
    return c;
}

static Palette palette_dark(void) {
    Palette p = {
        RGB255( 26, 26, 26), RGB255( 22, 22, 22), RGB255(232,232,232),
        RGB255(160,160,160), RGB255( 60, 60, 60), RGB255(224, 65, 65)
    };
    return p;
}

static Palette palette_light(void) {
    Palette p = {
        RGB255(228,228,228), RGB255(236,236,236), RGB255( 26, 26, 26),
        RGB255( 90, 90, 90), RGB255(176,176,176), RGB255(200, 50, 50)
    };
    return p;
}

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------

static HWND                   g_hwnd;
static ID2D1Factory          *g_d2d;
// g_rt is the *current* render target used by the drawing code. It
// normally points at g_hwndRt (the window's HWND render target), but is
// temporarily redirected to a DC render target when we render iconic
// thumbnails/live-previews for the DWM taskbar.
static ID2D1RenderTarget     *g_rt;
static ID2D1HwndRenderTarget *g_hwndRt;
static ID2D1DCRenderTarget   *g_dcRt;        // lazy, used for DWM bitmaps
static ID2D1SolidColorBrush  *g_brush;       // reused, color reassigned
static IDWriteFactory        *g_dw;
static IDWriteTextFormat     *g_txtSys;      // for the "SYS" indicator
static int                    g_txtSysSize = 0;

static int                    g_alwaysOnTop  = 0;
static int                    g_theme        = 0; // 0 dark, 1 light
static int                    g_armed[12]    = { 0 };
static float                  g_prevMins     = -1.0f;
static char                   g_tzLabel[96]  = "";
static DWORD                  g_lastNtpKickMs = 0;

// User-configurable settings (persisted in %APPDATA%\Lunar\settings.dat).
static int                    g_chimesEnabled      = 1;
static int                    g_unminimizeOnChime  = 0;
static int                    g_confirmOnClose     = 0;
// Selected display time zone as an IANA name (e.g. "Europe/Paris").
// Empty string means UTC.  The clockwork itself is always UTC; this
// controls only how local wall-clock components are rendered.  Zones
// are resolved against tz_embed.c without any OS timezone call.
static char                   g_tzIana[64]         = "";
static TzId                   g_tzId               = TZ_ID_UTC;

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

// Wide-char path so users with non-ASCII profile names (ü, 漢, etc.)
// don't silently lose their armed state. APPDATA is always set on
// desktop Windows; we still fall back gracefully if it isn't.
static void ArmedPathW(wchar_t *out, size_t n) {
    const wchar_t *appdata = _wgetenv(L"APPDATA");
    if (!appdata || !*appdata) { out[0] = 0; return; }
    _snwprintf_s(out, n, _TRUNCATE, L"%ls\\Lunar", appdata);
    _wmkdir(out);
    _snwprintf_s(out, n, _TRUNCATE, L"%ls\\Lunar\\armed.dat", appdata);
}

static void LoadArmed(int armed[12]) {
    wchar_t path[MAX_PATH]; ArmedPathW(path, MAX_PATH);
    if (!path[0]) return;
    FILE *f = _wfopen(path, L"rb");
    if (!f) return;
    char buf[12] = { 0 };
    fread(buf, 1, 12, f);
    fclose(f);
    for (int i = 0; i < 12; i++) armed[i] = (buf[i] == '1') ? 1 : 0;
}

static void SaveArmed(const int armed[12]) {
    wchar_t path[MAX_PATH]; ArmedPathW(path, MAX_PATH);
    if (!path[0]) return;
    FILE *f = _wfopen(path, L"wb");
    if (!f) return;
    char buf[12];
    for (int i = 0; i < 12; i++) buf[i] = armed[i] ? '1' : '0';
    fwrite(buf, 1, 12, f);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Window state persistence (position, size, always-on-top, maximized).
//
// We store the RESTORED outer-window rect in virtual-screen coordinates
// (spanning all monitors). On a multi-monitor setup this Just Works:
// whatever monitor the window was on is the one it returns to, because
// the saved coordinates are absolute. If that monitor is no longer
// connected we detect the missing rect at load time and fall back to
// defaults.
// ---------------------------------------------------------------------------

typedef struct {
    int valid;
    int x, y, w, h;
    int alwaysOnTop;
    int maximized;
} WindowState;

static void WindowStatePathW(wchar_t *out, size_t n) {
    const wchar_t *appdata = _wgetenv(L"APPDATA");
    if (!appdata || !*appdata) { out[0] = 0; return; }
    _snwprintf_s(out, n, _TRUNCATE, L"%ls\\Lunar", appdata);
    _wmkdir(out);
    _snwprintf_s(out, n, _TRUNCATE, L"%ls\\Lunar\\window.dat", appdata);
}

static void LoadWindowState(WindowState *ws) {
    ws->valid = 0;
    wchar_t path[MAX_PATH]; WindowStatePathW(path, MAX_PATH);
    if (!path[0]) return;
    FILE *f = _wfopen(path, L"rb");
    if (!f) return;
    char buf[256] = { 0 };
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    int x, y, w, h, aot, maxd;
    if (sscanf(buf, "%d %d %d %d %d %d", &x, &y, &w, &h, &aot, &maxd) != 6)
        return;
    // Sanity-check sizes so a corrupted file can't produce a 1-pixel or
    // 40-screen-wide window.
    if (w < 120 || h < 120 || w > 16384 || h > 16384) return;

    // Confirm the rect still overlaps a connected monitor; if the saved
    // monitor has been unplugged, MonitorFromRect with
    // MONITOR_DEFAULTTONULL returns NULL and we fall back to defaults.
    RECT r = { x, y, x + w, y + h };
    HMONITOR hm = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
    if (!hm) return;

    ws->valid       = 1;
    ws->x           = x;
    ws->y           = y;
    ws->w           = w;
    ws->h           = h;
    ws->alwaysOnTop = aot ? 1 : 0;
    ws->maximized   = maxd ? 1 : 0;
}

static void SaveWindowState(HWND hwnd, int alwaysOnTop) {
    wchar_t path[MAX_PATH]; WindowStatePathW(path, MAX_PATH);
    if (!path[0]) return;

    // WINDOWPLACEMENT gives us the RESTORED rect even if the window is
    // currently minimized or maximized -- exactly what we want.
    WINDOWPLACEMENT wp = { .length = sizeof(wp) };
    if (!GetWindowPlacement(hwnd, &wp)) return;
    RECT r = wp.rcNormalPosition;
    int maximized = (wp.showCmd == SW_SHOWMAXIMIZED) ? 1 : 0;

    FILE *f = _wfopen(path, L"wb");
    if (!f) return;
    fprintf(f, "%ld %ld %ld %ld %d %d\n",
            (long)r.left, (long)r.top,
            (long)(r.right - r.left), (long)(r.bottom - r.top),
            alwaysOnTop ? 1 : 0, maximized);
    fclose(f);
}

// ---------------------------------------------------------------------------
// User settings persistence
// ---------------------------------------------------------------------------

static void SettingsPathW(wchar_t *out, size_t n) {
    const wchar_t *appdata = _wgetenv(L"APPDATA");
    if (!appdata || !*appdata) { out[0] = 0; return; }
    _snwprintf_s(out, n, _TRUNCATE, L"%ls\\Lunar", appdata);
    _wmkdir(out);
    _snwprintf_s(out, n, _TRUNCATE, L"%ls\\Lunar\\settings.dat", appdata);
}

static void LoadSettings(void) {
    wchar_t path[MAX_PATH]; SettingsPathW(path, MAX_PATH);
    if (!path[0]) return;
    FILE *f = _wfopen(path, L"rb");
    if (!f) return;
    char buf[2048] = { 0 };
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;

    // Legacy v1 format was a single line "%d %d %d" (chimes unmin
    // confirm) with no timezone field. Detect it by the absence of
    // '=' and parse accordingly.
    if (!strchr(buf, '=')) {
        int chimes = 1, unmin = 0, confirm = 0;
        if (sscanf(buf, "%d %d %d", &chimes, &unmin, &confirm) >= 1) {
            g_chimesEnabled     = chimes  ? 1 : 0;
            g_unminimizeOnChime = unmin   ? 1 : 0;
            g_confirmOnClose    = confirm ? 1 : 0;
        }
        return;
    }

    // v2 format: one key=value per line.
    char *save = NULL;
    char *line = strtok_s(buf, "\r\n", &save);
    while (line) {
        int v = 0;
        if      (sscanf(line, "chimes=%d",  &v) == 1) g_chimesEnabled     = v ? 1 : 0;
        else if (sscanf(line, "unmin=%d",   &v) == 1) g_unminimizeOnChime = v ? 1 : 0;
        else if (sscanf(line, "confirm=%d", &v) == 1) g_confirmOnClose    = v ? 1 : 0;
        else if (strncmp(line, "tz=", 3) == 0) {
            // IANA name (ASCII).  We copy it in raw; the resolver will
            // silently reject unknown values (including obsolete
            // Windows keys from pre-IANA builds).
            const char *v = line + 3;
            snprintf(g_tzIana, sizeof(g_tzIana), "%s", v);
        }
        line = strtok_s(NULL, "\r\n", &save);
    }
}

static void SaveSettings(void) {
    wchar_t path[MAX_PATH]; SettingsPathW(path, MAX_PATH);
    if (!path[0]) return;
    FILE *f = _wfopen(path, L"wb");
    if (!f) return;
    fprintf(f,
            "chimes=%d\n"
            "unmin=%d\n"
            "confirm=%d\n"
            "tz=%s\n",
            g_chimesEnabled, g_unminimizeOnChime, g_confirmOnClose,
            g_tzIana);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Audio (PlaySound + in-memory WAV, no persistent device)
// ---------------------------------------------------------------------------

extern float Sysvol_Get(void);

// 0.35 s of 16-bit mono PCM at 44,100 Hz: 15,435 samples * 2 B + 44 B header.
#define BEEP_SAMPLE_RATE 44100
#define BEEP_FRAMES      15435                         // == 44100 * 0.35
#define BEEP_FREQ_HZ     880.0f                        // A5; single chime tone
#define BEEP_DATA_BYTES  (BEEP_FRAMES * 2)
#define BEEP_BUF_BYTES   (44 + BEEP_DATA_BYTES)

// Single shared buffer rebuilt in place for each play. PlaySound with
// SND_ASYNC requires the buffer to remain valid while playing, but the
// next call to PlaySound (or sndPlaySound(NULL,0)) replaces the current
// playback. Since beeps don't overlap in this app this is safe.
static unsigned char g_wav[BEEP_BUF_BYTES];

static void WriteLE16(unsigned char *p, uint16_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }
static void WriteLE32(unsigned char *p, uint32_t v) {
    p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF;
}

// Render a sine-wave PCM16 clip with 10 ms attack + 30 ms release
// envelope, at the given peak amplitude (0..1), into g_wav.
static void BuildWav(float freqHz, float amplitude) {
    unsigned char *p = g_wav;
    // RIFF header
    memcpy(p, "RIFF", 4);                             p += 4;
    WriteLE32(p, 36 + BEEP_DATA_BYTES);               p += 4;
    memcpy(p, "WAVE", 4);                             p += 4;
    // fmt  chunk
    memcpy(p, "fmt ", 4);                             p += 4;
    WriteLE32(p, 16);                                 p += 4;   // PCM chunk size
    WriteLE16(p, 1);                                  p += 2;   // PCM
    WriteLE16(p, 1);                                  p += 2;   // mono
    WriteLE32(p, BEEP_SAMPLE_RATE);                   p += 4;
    WriteLE32(p, BEEP_SAMPLE_RATE * 2);               p += 4;   // byte rate
    WriteLE16(p, 2);                                  p += 2;   // block align
    WriteLE16(p, 16);                                 p += 2;   // bits
    // data chunk
    memcpy(p, "data", 4);                             p += 4;
    WriteLE32(p, BEEP_DATA_BYTES);                    p += 4;

    const float TAU            = 6.28318530717958647692f;
    const float attackFrames   = BEEP_SAMPLE_RATE * 0.010f;
    const float releaseFrames  = BEEP_SAMPLE_RATE * 0.030f;
    for (int i = 0; i < BEEP_FRAMES; i++) {
        float env = 1.0f;
        if (i < attackFrames)                 env = (float)i / attackFrames;
        else if (i > BEEP_FRAMES - releaseFrames) env = (BEEP_FRAMES - i) / releaseFrames;
        float s = sinf(TAU * freqHz * (float)i / BEEP_SAMPLE_RATE) * env * amplitude;
        int v = (int)(s * 32767.0f);
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        WriteLE16(p, (uint16_t)(int16_t)v);
        p += 2;
    }
}

// Play a single 880 Hz / 0.35 s chime. Applies the same volume-
// compensation: WASAPI scales our output linearly
// by the system master slider, so we pre-boost to keep perceived
// loudness roughly constant.
static void PlayBeep(void) {
    float v = Sysvol_Get();
    if (v <= 0.01f) return;                  // effectively muted
    const float TARGET = 0.50f;              // desired speaker-level amplitude
    float amp = TARGET / v;
    if (amp > 0.90f) amp = 0.90f;            // stay below clipping
    if (amp < 0.05f) amp = 0.05f;
    BuildWav(BEEP_FREQ_HZ, amp);
    // SND_MEMORY: buffer lives in memory. SND_ASYNC: return immediately.
    PlaySoundW((LPCWSTR)g_wav, NULL, SND_MEMORY | SND_ASYNC);
}

// ---------------------------------------------------------------------------
// Hit-testing
// ---------------------------------------------------------------------------

// Returns 0..11 (0 = 12 o'clock) or -1 on miss.
static int HitTestHour(float mx, float my, float cx, float cy, float S) {
    float dx = mx - cx, dy = my - cy;
    float r  = sqrtf(dx*dx + dy*dy);
    float margin = 0.04f * S;
    float radius = 0.5f * S - margin;
    float inner  = radius - 0.075f * S;
    float outer  = radius + 0.015f * S;
    if (r < inner || r > outer) return -1;
    float a = atan2f(dx, -dy) * (180.0f / PI);
    if (a < 0) a += 360.0f;
    float nearest = roundf(a / 30.0f);
    int idx = ((int)nearest) % 12;
    float err = fabsf(a - nearest * 30.0f);
    if (err > 12.0f) return -1;
    return idx;
}

// ---------------------------------------------------------------------------
// Timezone label + trust indicator -> window title
// ---------------------------------------------------------------------------
//
// Title format (U+2014 em-dashes separate the segments):
//
//   CET  -  Lunar 0.2  -  OK 3/3  +-5ms
//   UTC  -  Lunar 0.2  -  INOP 2/3
//   UTC  -  Lunar 0.2  -  INOP 3/3 sp=612ms     (three reached, disagreed)
//
// Called from Tick() every ~200 ms; SetWindowTextW is only invoked when
// the composed string actually changes, so the caption does not churn.

static void UpdateTitleBar(void) {
    static WCHAR s_last[192] = { 0 };

    WCHAR tz[32] = L"UTC";
    if (g_tzLabel[0])
        MultiByteToWideChar(CP_UTF8, 0, g_tzLabel, -1, tz, 32);

    WCHAR title[192];
    _snwprintf_s(title, 192, _TRUNCATE, L"%ls  \x2014  Lunar 0.2", tz);

    if (wcscmp(title, s_last) != 0) {
        SetWindowTextW(g_hwnd, title);
        wcsncpy(s_last, title, 192);
        s_last[191] = 0;
    }
}

// Resolve g_tzIana to an embedded-tzdata ID and refresh the short
// label used by the title bar ("CEST (UTC+2)" etc.).  Called once at
// startup and again whenever the user changes the zone in Settings.
// If g_tzIana is empty, or names a zone not present in the embedded
// index, we fall back to UTC and clear the stored name so the bad
// value never gets re-saved.
static void UpdateTimezone(void) {
    if (g_tzIana[0] == 0) {
        g_tzId = TZ_ID_UTC;
        snprintf(g_tzLabel, sizeof(g_tzLabel), "UTC");
        UpdateTitleBar();
        return;
    }

    TzId id = Tz_FindByName(g_tzIana);
    if (id == TZ_ID_INVALID) {
        // Selected IANA zone not present in the embedded index
        // (obsolete name, or leftover Windows key from a pre-IANA
        // build).  Fall back to UTC; do not clobber the saved file
        // here -- SaveSettings() will flush the empty string next
        // time the user touches Settings.
        Log_Append("tz: IANA name %.48s not in embedded index; falling back to UTC",
                   g_tzIana);
        g_tzIana[0] = 0;
        g_tzId = TZ_ID_UTC;
        snprintf(g_tzLabel, sizeof(g_tzLabel), "UTC");
        UpdateTitleBar();
        return;
    }
    g_tzId = id;

    // Compute the offset + abbreviation that apply RIGHT NOW.  We only
    // trust our disciplined clock for this; before the first good NTP
    // cycle there is no sensible "now" to ask about.  While we wait we
    // render just the IANA name.
    int64_t nowUtcMs = 0;
    if (!Clock_NowUtcMs(&nowUtcMs)) {
        snprintf(g_tzLabel, sizeof(g_tzLabel), "%s", g_tzIana);
        UpdateTitleBar();
        return;
    }

    TzifLocal tl;
    if (!Tz_LocalFromUtcMs(id, nowUtcMs, &tl)) {
        snprintf(g_tzLabel, sizeof(g_tzLabel), "%s", g_tzIana);
        UpdateTitleBar();
        return;
    }

    int offMin = tl.utcOffsetSec / 60;
    int offH   = offMin / 60;
    int offM   = offMin % 60;
    if (offM < 0) offM = -offM;
    if (offM == 0) {
        snprintf(g_tzLabel, sizeof(g_tzLabel), "%s (UTC%+d)",
                 tl.abbr[0] ? tl.abbr : g_tzIana, offH);
    } else {
        snprintf(g_tzLabel, sizeof(g_tzLabel), "%s (UTC%+d:%02d)",
                 tl.abbr[0] ? tl.abbr : g_tzIana, offH, offM);
    }
    UpdateTitleBar();
}

// Convert disciplined UTC milliseconds to a struct tm in the display
// zone, plus the sub-second ms part. Returns 1 on success, 0 on
// failure.  Uses ONLY the embedded IANA tzdata; no OS timezone call.
static int UtcMsToLocalTm(int64_t utcMs, struct tm *out, int *outMs) {
    if (!out) return 0;
    int ms = (int)(utcMs % 1000);
    if (ms < 0) { ms += 1000; utcMs -= 1000; }
    if (outMs) *outMs = ms;

    TzifLocal tl;
    if (!Tz_LocalFromUtcMs(g_tzId, utcMs, &tl)) return 0;

    out->tm_year = tl.year - 1900;
    out->tm_mon  = tl.month - 1;
    out->tm_mday = tl.mday;
    out->tm_hour = tl.hour;
    out->tm_min  = tl.minute;
    out->tm_sec  = tl.second;
    out->tm_wday = tl.wday;
    out->tm_yday = tl.yday;
    out->tm_isdst = tl.isDst;
    return 1;
}

// ---------------------------------------------------------------------------
// Direct2D resource creation
// ---------------------------------------------------------------------------

static HRESULT CreateDeviceResources(void) {
    if (g_hwndRt) return S_OK;

    RECT rc; GetClientRect(g_hwnd, &rc);
    D2D1_SIZE_U size = { (UINT32)(rc.right - rc.left),
                         (UINT32)(rc.bottom - rc.top) };
    if (size.width  == 0) size.width  = 1;
    if (size.height == 0) size.height = 1;

    D2D1_RENDER_TARGET_PROPERTIES rtp = {
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        { DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED },
        96.0f, 96.0f,
        D2D1_RENDER_TARGET_USAGE_NONE,
        D2D1_FEATURE_LEVEL_DEFAULT
    };
    D2D1_HWND_RENDER_TARGET_PROPERTIES hrtp = {
        g_hwnd, size, D2D1_PRESENT_OPTIONS_NONE
    };

    HRESULT hr = ID2D1Factory_CreateHwndRenderTarget(g_d2d, &rtp, &hrtp, &g_hwndRt);
    if (FAILED(hr)) return hr;
    g_rt = (ID2D1RenderTarget*)g_hwndRt;

    D2D1_COLOR_F black = { 0, 0, 0, 1 };
    hr = ID2D1RenderTarget_CreateSolidColorBrush(
        g_rt, &black, NULL, &g_brush);
    return hr;
}

static void DiscardDeviceResources(void) {
    if (g_brush)  { ID2D1SolidColorBrush_Release(g_brush);   g_brush  = NULL; }
    if (g_hwndRt) { ID2D1HwndRenderTarget_Release(g_hwndRt); g_hwndRt = NULL; }
    g_rt = NULL;
}

// ---------------------------------------------------------------------------
// Hand-geometry cache
// ---------------------------------------------------------------------------
// The tapered hour and minute hands are 4-point filled trapezoids whose
// shape depends only on `S` (the dial side). We build each one once at
// the current S, in "local" coordinates pointing straight up from the
// origin, and reuse them every frame by setting a rotate+translate
// transform on the render target. The only allocation cost is at
// startup + on resize.

static ID2D1PathGeometry *g_hourHand;
static ID2D1PathGeometry *g_minHand;
static float              g_handCacheS = 0.0f;

static ID2D1PathGeometry *BuildHand(float len, float baseW, float tipW) {
    // Hand points up (toward -y in D2D's y-down coordinate space), with
    // its pivot at the origin. A small tail (`backLen`) extends past the
    // pivot so the base reads as a flat edge, matching DrawTaperedHand.
    float backLen = baseW;
    ID2D1PathGeometry *g = NULL;
    if (FAILED(ID2D1Factory_CreatePathGeometry(g_d2d, &g))) return NULL;
    ID2D1GeometrySink *sink = NULL;
    if (FAILED(ID2D1PathGeometry_Open(g, &sink))) {
        ID2D1PathGeometry_Release(g); return NULL;
    }
    D2D1_POINT_2F p0 = { +baseW * 0.5f, +backLen };  // back-right
    D2D1_POINT_2F p1 = { +tipW  * 0.5f, -len     };  // tip-right
    D2D1_POINT_2F p2 = { -tipW  * 0.5f, -len     };  // tip-left
    D2D1_POINT_2F p3 = { -baseW * 0.5f, +backLen };  // back-left
    ID2D1GeometrySink_BeginFigure(sink, p0, D2D1_FIGURE_BEGIN_FILLED);
    D2D1_POINT_2F pts[3] = { p1, p2, p3 };
    ID2D1GeometrySink_AddLines(sink, pts, 3);
    ID2D1GeometrySink_EndFigure(sink, D2D1_FIGURE_END_CLOSED);
    ID2D1GeometrySink_Close(sink);
    ID2D1GeometrySink_Release(sink);
    return g;
}

static void ReleaseHandCache(void) {
    if (g_hourHand) { ID2D1PathGeometry_Release(g_hourHand); g_hourHand = NULL; }
    if (g_minHand)  { ID2D1PathGeometry_Release(g_minHand);  g_minHand  = NULL; }
    g_handCacheS = 0.0f;
}

static void EnsureHandCache(float S) {
    if (S == g_handCacheS && g_hourHand && g_minHand) return;
    ReleaseHandCache();
    g_hourHand = BuildHand(0.28f * S, 0.035f * S, 0.020f * S);
    g_minHand  = BuildHand(0.40f * S, 0.022f * S, 0.012f * S);
    g_handCacheS = S;
}

static void EnsureTextFormat(int pxSize) {
    if (g_txtSys && g_txtSysSize == pxSize) return;
    if (g_txtSys) { IDWriteTextFormat_Release(g_txtSys); g_txtSys = NULL; }
    HRESULT hr = IDWriteFactory_CreateTextFormat(
        g_dw, L"Segoe UI", NULL,
        DWRITE_FONT_WEIGHT_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        (FLOAT)pxSize,
        L"en-us",
        &g_txtSys);
    if (SUCCEEDED(hr)) g_txtSysSize = pxSize;
}

// ---------------------------------------------------------------------------
// Drawing primitives
// ---------------------------------------------------------------------------

static inline void SetBrush(D2D1_COLOR_F c) {
    ID2D1SolidColorBrush_SetColor(g_brush, &c);
}

static inline D2D1_POINT_2F PT(float x, float y) {
    D2D1_POINT_2F p = { x, y }; return p;
}

static inline D2D1_ELLIPSE EL(float cx, float cy, float r) {
    D2D1_ELLIPSE e = { { cx, cy }, r, r }; return e;
}

static D2D1_POINT_2F Polar(float cx, float cy, float r, float angleDeg) {
    float a = (angleDeg - 90.0f) * DEG2RAD;
    return PT(cx + r * cosf(a), cy + r * sinf(a));
}

// Draw a cached hand geometry by applying a rotate+translate transform.
// The geometry was built pointing up from the origin, so the combined
// matrix R(angle) * T(cx,cy) places the tip at (cx, cy) and rotates it
// clockwise by `angleDeg` (0 = 12 o'clock).
static void DrawCachedHand(ID2D1PathGeometry *g, float cx, float cy,
                           float angleDeg, D2D1_COLOR_F col) {
    if (!g) return;
    float r = angleDeg * DEG2RAD;
    float ca = cosf(r), sa = sinf(r);
    D2D1_MATRIX_3X2_F m;
    m._11 = ca;  m._12 = sa;
    m._21 = -sa; m._22 = ca;
    m._31 = cx;  m._32 = cy;
    ID2D1RenderTarget_SetTransform(g_rt, &m);
    SetBrush(col);
    ID2D1RenderTarget_FillGeometry(g_rt, (ID2D1Geometry*)g,
                                   (ID2D1Brush*)g_brush, NULL);
    D2D1_MATRIX_3X2_F I;
    I._11 = 1.0f; I._12 = 0.0f;
    I._21 = 0.0f; I._22 = 1.0f;
    I._31 = 0.0f; I._32 = 0.0f;
    ID2D1RenderTarget_SetTransform(g_rt, &I);
}

static void DrawSecondHand(float cx, float cy, float angleDeg, float len,
                           float tail, float width,
                           D2D1_COLOR_F accent, D2D1_COLOR_F face) {
    D2D1_POINT_2F tip     = Polar(cx, cy,  len,  angleDeg);
    D2D1_POINT_2F tailEnd = Polar(cx, cy, -tail, angleDeg);
    SetBrush(accent);
    ID2D1RenderTarget_DrawLine(g_rt, tailEnd, tip,
                               (ID2D1Brush*)g_brush, width, NULL);
    D2D1_ELLIPSE pivot = EL(cx, cy, width * 2.2f);
    ID2D1RenderTarget_FillEllipse(g_rt, &pivot, (ID2D1Brush*)g_brush);
    D2D1_ELLIPSE tipOuter = EL(tip.x, tip.y, width * 1.6f);
    ID2D1RenderTarget_FillEllipse(g_rt, &tipOuter, (ID2D1Brush*)g_brush);
    SetBrush(face);
    D2D1_ELLIPSE tipInner = EL(tip.x, tip.y, width * 0.9f);
    ID2D1RenderTarget_FillEllipse(g_rt, &tipInner, (ID2D1Brush*)g_brush);
}

// ---------------------------------------------------------------------------
// INOP indicator
// ---------------------------------------------------------------------------
//
// Safety-critical display: whenever the clockwork cannot deliver a
// time we trust (no successful NTP sync this run, or -- in later
// steps -- concurrence is lost), we stop rendering the dial entirely
// and paint a solid background with large red "INOP" letters. No
// hands, no numerals: an operator glancing at the window must never
// be able to misread an untrusted state as a normal time display.

static void DrawInop(float dw, float dh, const Palette *pal) {
    float S = (dw < dh ? dw : dh);
    // Use the face color as a flat background so INOP stands alone
    // on the same surface where the dial would normally appear.
    D2D1_RECT_F full = { 0, 0, dw, dh };
    SetBrush(pal->face);
    ID2D1RenderTarget_FillRectangle(g_rt, &full, (ID2D1Brush*)g_brush);

    int fs = (int)(S * 0.28f);
    if (fs < 24) fs = 24;
    EnsureTextFormat(fs);
    if (!g_txtSys) return;

    IDWriteTextFormat_SetTextAlignment(g_txtSys,
        DWRITE_TEXT_ALIGNMENT_CENTER);
    IDWriteTextFormat_SetParagraphAlignment(g_txtSys,
        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    D2D1_COLOR_F red = { 0.92f, 0.10f, 0.10f, 1.0f };
    SetBrush(red);
    ID2D1RenderTarget_DrawText(g_rt, L"INOP", 4,
        g_txtSys, &full, (ID2D1Brush*)g_brush,
        D2D1_DRAW_TEXT_OPTIONS_NONE,
        DWRITE_MEASURING_MODE_NATURAL);
}


// ---------------------------------------------------------------------------
// Dial
// ---------------------------------------------------------------------------

static void DrawDial(float cx, float cy, float S, const Palette *pal,
                     const struct tm *lt, int ms, const int armed[12]) {
    float margin = 0.04f * S;
    float radius = 0.5f * S - margin;

    // Ring + face.
    SetBrush(pal->ring);
    { D2D1_ELLIPSE e = EL(cx, cy, radius + 1.5f);
      ID2D1RenderTarget_FillEllipse(g_rt, &e, (ID2D1Brush*)g_brush); }
    SetBrush(pal->face);
    { D2D1_ELLIPSE e = EL(cx, cy, radius);
      ID2D1RenderTarget_FillEllipse(g_rt, &e, (ID2D1Brush*)g_brush); }

    // 60 minute ticks.
    float minInner = radius - 0.020f * S;
    SetBrush(pal->inkSoft);
    for (int m = 0; m < 60; m++) {
        if (m % 5 == 0) continue;
        float a = m * 6.0f;
        D2D1_POINT_2F p0 = Polar(cx, cy, minInner, a);
        D2D1_POINT_2F p1 = Polar(cx, cy, radius,   a);
        ID2D1RenderTarget_DrawLine(g_rt, p0, p1,
                                   (ID2D1Brush*)g_brush, 0.008f * S, NULL);
    }

    // 12 hour ticks (skip 0; drawn as the double-marker below).
    float hrInner = radius - 0.050f * S;
    for (int h = 1; h < 12; h++) {
        float a = h * 30.0f;
        SetBrush(armed[h] ? pal->accent : pal->ink);
        D2D1_POINT_2F p0 = Polar(cx, cy, hrInner, a);
        D2D1_POINT_2F p1 = Polar(cx, cy, radius,  a);
        ID2D1RenderTarget_DrawLine(g_rt, p0, p1,
                                   (ID2D1Brush*)g_brush, 0.014f * S, NULL);
    }

    // 12 o'clock double-marker.
    {
        float off = 0.020f * S;
        SetBrush(armed[0] ? pal->accent : pal->ink);
        D2D1_POINT_2F pIn  = Polar(cx, cy, hrInner, 0.0f);
        D2D1_POINT_2F pOut = Polar(cx, cy, radius,  0.0f);
        D2D1_POINT_2F a0 = PT(pIn.x  - off, pIn.y),  a1 = PT(pOut.x - off, pOut.y);
        D2D1_POINT_2F b0 = PT(pIn.x  + off, pIn.y),  b1 = PT(pOut.x + off, pOut.y);
        ID2D1RenderTarget_DrawLine(g_rt, a0, a1, (ID2D1Brush*)g_brush,
                                   0.014f * S, NULL);
        ID2D1RenderTarget_DrawLine(g_rt, b0, b1, (ID2D1Brush*)g_brush,
                                   0.014f * S, NULL);
    }

    float secs = (float)lt->tm_sec + ms / 1000.0f;
    float mins = (float)lt->tm_min + secs / 60.0f;
    float hrs  = (float)(lt->tm_hour % 12) + mins / 60.0f;

    EnsureHandCache(S);
    DrawCachedHand(g_hourHand, cx, cy, hrs  * 30.0f, pal->ink);
    DrawCachedHand(g_minHand,  cx, cy, mins * 6.0f,  pal->ink);
    DrawSecondHand (cx, cy, secs * 6.0f,  0.44f * S, 0.08f * S, 0.010f * S,
                    pal->accent, pal->face);
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

static void Paint(void) {
    if (FAILED(CreateDeviceResources())) return;

    // Read the disciplined clockwork time. Clock_NowUtcMs() returns 0
    // if we have not yet successfully synced THIS run -- the Windows
    // system clock is never used as a display source. In that case we
    // paint the INOP indicator and skip dial rendering entirely.
    int64_t displayMs = 0;
    int haveTime = Clock_NowUtcMs(&displayMs);

    Palette pal = (g_theme == 1) ? palette_light() : palette_dark();

    // Avoid ID2D1HwndRenderTarget_GetSize (its C binding's return-by-value
    // for D2D1_SIZE_F is ABI-fragile on MinGW); read the client size
    // directly. dpiX/dpiY were pinned to 96, so pixels == DIPs.
    RECT rc; GetClientRect(g_hwnd, &rc);
    float dw = (float)(rc.right - rc.left);
    float dh = (float)(rc.bottom - rc.top);
    float S  = (dw < dh ? dw : dh);
    float cx = dw * 0.5f, cy = dh * 0.5f;

    ID2D1RenderTarget_BeginDraw(g_rt);
    ID2D1RenderTarget_Clear(g_rt, &pal.bg);
    ID2D1RenderTarget_SetAntialiasMode(g_rt, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (!haveTime) {
        DrawInop(dw, dh, &pal);
    } else {
        struct tm lt = {0};
        int ms = 0;
        if (!UtcMsToLocalTm(displayMs, &lt, &ms)) {
            // A conversion failure from a supposedly disciplined
            // clock is a fault condition: show INOP rather than
            // silently skipping the frame.
            DrawInop(dw, dh, &pal);
        } else {
            if (S > 40.0f) DrawDial(cx, cy, S, &pal, &lt, ms, g_armed);
        }
    }

    // "SYS" indicator (lower-right of the window) when NTP is stale.
    // Suppressed while INOP is being shown -- INOP already conveys the
    // untrusted state and we don't want to overpaint it.
    if (haveTime && !Ntp_IsSynced()) {
        int fs = (int)(S * 0.035f);
        if (fs < 10) fs = 10;
        EnsureTextFormat(fs);
        if (g_txtSys) {
            IDWriteTextFormat_SetTextAlignment(g_txtSys,
                DWRITE_TEXT_ALIGNMENT_TRAILING);
            IDWriteTextFormat_SetParagraphAlignment(g_txtSys,
                DWRITE_PARAGRAPH_ALIGNMENT_FAR);
            float pad = fs * 0.5f;
            D2D1_RECT_F rect = { 0, 0, dw - pad, dh - pad };
            SetBrush(pal.accent);
            ID2D1RenderTarget_DrawText(g_rt, L"SYS", 3,
                g_txtSys, &rect, (ID2D1Brush*)g_brush,
                D2D1_DRAW_TEXT_OPTIONS_NONE,
                DWRITE_MEASURING_MODE_NATURAL);
        }
    }

    HRESULT hr = ID2D1RenderTarget_EndDraw(g_rt, NULL, NULL);
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
    }
}

// ---------------------------------------------------------------------------
// Iconic (taskbar thumbnail / peek) rendering
// ---------------------------------------------------------------------------
//
// When the window is minimized the HWND render target isn't painted, so
// the DWM-managed taskbar thumbnail would freeze on the last pre-minimize
// frame. We opt into custom iconic bitmaps via DwmSetWindowAttribute
// (DWMWA_HAS_ICONIC_BITMAP + DWMWA_FORCE_ICONIC_REPRESENTATION) and then
// respond to WM_DWMSENDICONICTHUMBNAIL / WM_DWMSENDICONICLIVEPREVIEWBITMAP
// by rendering the current clock face into a 32bpp top-down DIB through
// an ID2D1DCRenderTarget. Tick() calls DwmInvalidateIconicBitmaps() while
// iconic so the cached thumbnail is re-requested on a fresh hover.

// Render the current clock face into a newly-allocated 32bpp HBITMAP of
// size (w, h). Caller owns the bitmap and must DeleteObject it. Returns
// NULL on failure. Swaps g_rt temporarily; not thread-safe, but all our
// rendering is on the UI thread.
static HBITMAP RenderClockToBitmap(int w, int h) {
    if (w < 1 || h < 1) return NULL;

    if (!g_dcRt) {
        D2D1_RENDER_TARGET_PROPERTIES p = {
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED },
            96.0f, 96.0f,
            D2D1_RENDER_TARGET_USAGE_NONE,
            D2D1_FEATURE_LEVEL_DEFAULT
        };
        if (FAILED(ID2D1Factory_CreateDCRenderTarget(g_d2d, &p, &g_dcRt)))
            return NULL;
    }

    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;              // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hbm) return NULL;
    HDC hdc = CreateCompatibleDC(NULL);
    if (!hdc) { DeleteObject(hbm); return NULL; }
    HGDIOBJ old = SelectObject(hdc, hbm);

    RECT rc = { 0, 0, w, h };
    if (FAILED(ID2D1DCRenderTarget_BindDC(g_dcRt, hdc, &rc))) {
        SelectObject(hdc, old); DeleteDC(hdc); DeleteObject(hbm); return NULL;
    }

    // Redirect the drawing code at the DC render target. Path geometries
    // are device-independent (owned by the factory) so the hand cache is
    // still valid, but we must invalidate it because S is different here.
    ID2D1RenderTarget    *savedRt    = g_rt;
    ID2D1SolidColorBrush *savedBrush = g_brush;
    g_rt    = (ID2D1RenderTarget*)g_dcRt;
    g_brush = NULL;
    D2D1_COLOR_F black = { 0, 0, 0, 1 };
    HRESULT hr = ID2D1RenderTarget_CreateSolidColorBrush(
        g_rt, &black, NULL, &g_brush);
    if (SUCCEEDED(hr)) {
        // Current disciplined UTC (same source as Paint()). Returns 0
        // if not synced this run; in that case render INOP into the
        // thumbnail too -- the taskbar preview must never show a stale
        // or system-clock time.
        int64_t displayMs = 0;
        int haveTime = Clock_NowUtcMs(&displayMs);

        Palette pal = (g_theme == 1) ? palette_light() : palette_dark();
        float dw = (float)w, dh = (float)h;
        float S  = (dw < dh ? dw : dh);
        float cx = dw * 0.5f, cy = dh * 0.5f;

        ID2D1RenderTarget_BeginDraw(g_rt);
        ID2D1RenderTarget_Clear(g_rt, &pal.bg);
        ID2D1RenderTarget_SetAntialiasMode(g_rt, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        if (!haveTime) {
            DrawInop(dw, dh, &pal);
        } else {
            struct tm lt = {0};
            int ms = 0;
            if (UtcMsToLocalTm(displayMs, &lt, &ms)) {
                if (S > 40.0f) DrawDial(cx, cy, S, &pal, &lt, ms, g_armed);
            } else {
                DrawInop(dw, dh, &pal);
            }
        }
        ID2D1RenderTarget_EndDraw(g_rt, NULL, NULL);

        ID2D1SolidColorBrush_Release(g_brush);
    }

    g_brush = savedBrush;
    g_rt    = savedRt;

    SelectObject(hdc, old);
    DeleteDC(hdc);

    // The background is fully opaque but some D2D paths leave alpha=0 in
    // places where BI_RGB DIBs interpret the byte as "undefined". DWM
    // requires a proper alpha channel; force 0xFF across the board so
    // the thumbnail doesn't come out transparent.
    uint32_t *px = (uint32_t*)bits;
    int n = w * h;
    for (int i = 0; i < n; i++) px[i] |= 0xFF000000u;

    return hbm;
}

// ---------------------------------------------------------------------------
// System-menu installation
// ---------------------------------------------------------------------------

static void SyncAlwaysOnTopCheck(void) {
    HMENU sys = GetSystemMenu(g_hwnd, FALSE);
    if (!sys) return;
    CheckMenuItem(sys, IDM_ALWAYS_ON_TOP,
                  MF_BYCOMMAND | (g_alwaysOnTop ? MF_CHECKED : MF_UNCHECKED));
}

static void InstallSystemMenuItems(void) {
    HMENU sys = GetSystemMenu(g_hwnd, FALSE);
    if (!sys) return;
    InsertMenuW(sys, 0, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenuW(sys, 0, MF_BYPOSITION | MF_STRING, IDM_ABOUT,         L"&About Lunar");
    InsertMenuW(sys, 0, MF_BYPOSITION | MF_STRING, IDM_SETTINGS,      L"&Settings\x2026");
    InsertMenuW(sys, 0, MF_BYPOSITION | MF_STRING, IDM_LOG,           L"&Log\x2026");
    InsertMenuW(sys, 0, MF_BYPOSITION | MF_STRING, IDM_TEST_BEEP,     L"&Test beep");
    InsertMenuW(sys, 0, MF_BYPOSITION | MF_STRING, IDM_ALWAYS_ON_TOP, L"Always on &top");
    SyncAlwaysOnTopCheck();
}

static void ApplyAlwaysOnTop(int on) {
    SetWindowPos(g_hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void ShowAbout(void) {
    wchar_t sync[128];
    int64_t utc = Ntp_LastSyncUtcMs();
    if (utc == 0) {
        wcscpy_s(sync, 128, L"Last NTP sync: never");
    } else {
        // Display in the user's selected time zone (not the Windows
        // system TZ): UtcMsToLocalTm honours g_tzId / UTC default.
        struct tm lt = {0};
        int msDummy = 0;
        int okTm = UtcMsToLocalTm(utc, &lt, &msDummy);
        // "ago" measured against our disciplined clockwork. If no
        // trusted UTC is available yet, we simply omit the "ago"
        // portion rather than reach for the Windows system clock.
        int64_t nowUtc = 0;
        int haveNow = Clock_NowUtcMs(&nowUtc);
        if (!okTm) {
            wcscpy_s(sync, 128, L"Last NTP sync: (tm conversion failed)");
        } else if (!haveNow) {
            swprintf(sync, 128,
                     L"Last NTP sync: %02d:%02d:%02d",
                     lt.tm_hour, lt.tm_min, lt.tm_sec);
        } else {
            int64_t agoS = (nowUtc - utc) / 1000;
            if (agoS < 0) agoS = 0;
            const wchar_t *unit; int64_t n;
            if      (agoS < 60)    { n = agoS;         unit = (n == 1) ? L"second" : L"seconds"; }
            else if (agoS < 3600)  { n = agoS / 60;    unit = (n == 1) ? L"minute" : L"minutes"; }
            else if (agoS < 86400) { n = agoS / 3600;  unit = (n == 1) ? L"hour"   : L"hours"; }
            else                   { n = agoS / 86400; unit = (n == 1) ? L"day"    : L"days"; }
            swprintf(sync, 128,
                     L"Last NTP sync: %02d:%02d:%02d  (%lld %s ago)",
                     lt.tm_hour, lt.tm_min, lt.tm_sec, (long long)n, unit);
        }
    }
    wchar_t msg[512];
    int32_t ppm = Clock_RatePpm();
    wchar_t disciplineLine[160];
    if (!Clock_IsDisciplined()) {
        swprintf(disciplineLine, 160,
                 L"Clockwork: not yet disciplined (awaiting first sync)");
    } else if (ppm == 0) {
        swprintf(disciplineLine, 160,
                 L"Clockwork: disciplined (single sample, rate pending)");
    } else {
        swprintf(disciplineLine, 160,
                 L"Clockwork: %+d ppm  (QPC-anchored, system clock ignored)",
                 (int)ppm);
    }

    // Per-source snapshot from the most recent cycle.
    NtpSourceResult r[NTP_SOURCE_COUNT];
    int nok = Ntp_GetResults(r);
    int64_t spread = Ntp_LastSpreadMs();
    TrustState trust = Clock_Trust();

    wchar_t trustLine[160];
    swprintf(trustLine, 160,
             L"Trust: %ls  (%d of %d sources, spread %lld ms)",
             (trust == TRUST_OK) ? L"OK" : L"INOP",
             nok, NTP_SOURCE_COUNT, (long long)spread);

    wchar_t sourcesBlock[512] = { 0 };
    int sbPos = 0;
    for (int i = 0; i < NTP_SOURCE_COUNT; i++) {
        wchar_t label[32] = L"?";
        if (r[i].label) {
            MultiByteToWideChar(CP_UTF8, 0, r[i].label, -1, label, 32);
            label[31] = 0;
        }
        wchar_t line[128];
        if (r[i].ok) {
            swprintf(line, 128,
                     L"  %-18ls ok   off %+5lld ms   rtt %4u ms\n",
                     label, (long long)r[i].offsetMs,
                     (unsigned)r[i].rttMs);
        } else {
            swprintf(line, 128,
                     L"  %-18ls --   (no reply)\n",
                     label);
        }
        int len = (int)wcslen(line);
        if (sbPos + len < 511) {
            wcscpy(sourcesBlock + sbPos, line);
            sbPos += len;
        }
    }

    swprintf(msg, 512,
             L"Lunar 0.2.0\n\n"
             L"A minimalist analog clock.\n"
             L"Native Win32 + Direct2D.\n\n"
             L"%s\n"
             L"%s\n"
             L"%s\n"
             L"%s",
             disciplineLine,
             trustLine,
             sourcesBlock,
             sync);
    MessageBoxW(g_hwnd, msg, L"About Lunar", MB_ICONINFORMATION | MB_OK);
}

// ---------------------------------------------------------------------------
// Log viewer
// ---------------------------------------------------------------------------
//
// A top-level window hosting a read-only multiline edit control.
// Single-instance: if already open, we just re-focus it. The edit
// control contents are refreshed from logbuf on open and whenever
// the user clicks "Refresh". "Copy" places the whole log on the
// clipboard as CF_UNICODETEXT.

#define IDC_LOG_EDIT      2001
#define IDC_LOG_REFRESH   2002
#define IDC_LOG_COPY      2003
#define IDC_LOG_CLOSE     2004

static HWND  g_logWnd     = NULL;
static HWND  g_logEdit    = NULL;
static HFONT g_logFont    = NULL;

// Load the current log snapshot into the edit control.
static void LogViewer_RefreshText(void) {
    if (!g_logEdit) return;
    size_t need = Log_Snapshot(NULL, 0);
    if (need == 0) {
        SetWindowTextW(g_logEdit, L"(log is empty)");
        return;
    }
    // Allocate UTF-8 buffer then convert to UTF-16 for the control.
    char *u8 = (char *)malloc(need + 1);
    if (!u8) { SetWindowTextW(g_logEdit, L"(out of memory)"); return; }
    size_t wrote = Log_Snapshot(u8, need + 1);
    u8[wrote] = 0;

    int wide_len = MultiByteToWideChar(CP_UTF8, 0, u8, -1, NULL, 0);
    if (wide_len <= 0) { free(u8); return; }
    wchar_t *w = (wchar_t *)malloc((size_t)wide_len * sizeof(wchar_t));
    if (!w) { free(u8); return; }
    MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, wide_len);
    free(u8);

    SetWindowTextW(g_logEdit, w);
    // Scroll to bottom so the most recent line is visible.
    int n = GetWindowTextLengthW(g_logEdit);
    SendMessageW(g_logEdit, EM_SETSEL, (WPARAM)n, (LPARAM)n);
    SendMessageW(g_logEdit, EM_SCROLLCARET, 0, 0);

    free(w);
}

// Copy the edit-control contents to the clipboard as CF_UNICODETEXT.
static void LogViewer_CopyToClipboard(void) {
    if (!g_logEdit) return;
    int n = GetWindowTextLengthW(g_logEdit);
    if (n <= 0) return;
    // +1 for NUL.
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, ((size_t)n + 1) * sizeof(wchar_t));
    if (!h) return;
    wchar_t *p = (wchar_t *)GlobalLock(h);
    if (!p) { GlobalFree(h); return; }
    GetWindowTextW(g_logEdit, p, n + 1);
    GlobalUnlock(h);
    if (!OpenClipboard(g_logWnd)) { GlobalFree(h); return; }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, h)) {
        // Ownership only transfers on success; otherwise we must free.
        GlobalFree(h);
    }
    CloseClipboard();
    Log_Append("log: copied %d chars to clipboard", n);
}

static void LogViewer_LayoutChildren(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int pad = 10;
    int btnH = 32;
    int btnW = 120;
    int editBottom = rc.bottom - btnH - pad * 2;
    HWND edit    = GetDlgItem(hwnd, IDC_LOG_EDIT);
    HWND refresh = GetDlgItem(hwnd, IDC_LOG_REFRESH);
    HWND copy    = GetDlgItem(hwnd, IDC_LOG_COPY);
    HWND close   = GetDlgItem(hwnd, IDC_LOG_CLOSE);
    if (edit)
        MoveWindow(edit,    pad, pad, rc.right - 2 * pad, editBottom - pad, TRUE);
    int by = editBottom + pad;
    if (refresh) MoveWindow(refresh, pad,                             by, btnW, btnH, TRUE);
    if (copy)    MoveWindow(copy,    pad + btnW + pad,                by, btnW, btnH, TRUE);
    if (close)   MoveWindow(close,   rc.right - pad - btnW,           by, btnW, btnH, TRUE);
}

static LRESULT CALLBACK LogWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hi = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
        // Use a monospace font so columns line up.
        LOGFONTW lf = {0};
        lf.lfHeight = -20;
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
        wcscpy_s(lf.lfFaceName, 32, L"Consolas");
        // Release any font from a previous incarnation of this window
        // before replacing the handle, so a re-open can't leak GDI
        // objects.
        if (g_logFont) { DeleteObject(g_logFont); g_logFont = NULL; }
        HFONT font = CreateFontIndirectW(&lf);
        g_logFont = font;

        g_logEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL
            | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_LOG_EDIT, hi, NULL);
        if (g_logEdit && font)
            SendMessageW(g_logEdit, WM_SETFONT, (WPARAM)font, TRUE);

        CreateWindowExW(0, L"BUTTON", L"Refresh",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_LOG_REFRESH, hi, NULL);
        CreateWindowExW(0, L"BUTTON", L"Copy all",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_LOG_COPY, hi, NULL);
        CreateWindowExW(0, L"BUTTON", L"Close",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_LOG_CLOSE, hi, NULL);

        // Propagate the font to all buttons too.
        if (font) {
            SendMessageW(GetDlgItem(hwnd, IDC_LOG_REFRESH), WM_SETFONT, (WPARAM)font, TRUE);
            SendMessageW(GetDlgItem(hwnd, IDC_LOG_COPY),    WM_SETFONT, (WPARAM)font, TRUE);
            SendMessageW(GetDlgItem(hwnd, IDC_LOG_CLOSE),   WM_SETFONT, (WPARAM)font, TRUE);
        }

        LogViewer_LayoutChildren(hwnd);
        LogViewer_RefreshText();
        return 0;
    }

    case WM_SIZE:
        LogViewer_LayoutChildren(hwnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_LOG_REFRESH: LogViewer_RefreshText();       return 0;
        case IDC_LOG_COPY:    LogViewer_CopyToClipboard();   return 0;
        case IDC_LOG_CLOSE:   DestroyWindow(hwnd);           return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_logWnd  = NULL;
        g_logEdit = NULL;
        if (g_logFont) { DeleteObject(g_logFont); g_logFont = NULL; }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowLogViewer(void) {
    // Single-instance: re-focus an existing window.
    if (g_logWnd) {
        LogViewer_RefreshText();
        ShowWindow(g_logWnd, SW_SHOWNORMAL);
        SetForegroundWindow(g_logWnd);
        return;
    }
    static const wchar_t kClass[] = L"LunarLogWnd";
    static int s_registered = 0;
    HINSTANCE hi = GetModuleHandleW(NULL);
    if (!s_registered) {
        WNDCLASSEXW wc = {0};
        wc.cbSize        = sizeof wc;
        wc.lpfnWndProc   = LogWndProc;
        wc.hInstance     = hi;
        wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClass;
        wc.hIcon         = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1),
                                             IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
        wc.hIconSm       = wc.hIcon;
        RegisterClassExW(&wc);
        s_registered = 1;
    }
    g_logWnd = CreateWindowExW(0, kClass, L"Lunar \x2014 Log (last 24h)",
                               WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
                               g_hwnd, NULL, hi, NULL);
    if (g_logWnd) {
        ShowWindow(g_logWnd, SW_SHOWNORMAL);
        UpdateWindow(g_logWnd);
    }
    Log_Append("log: viewer opened");
}

static INT_PTR CALLBACK SettingsDlgProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp);

// Case-insensitive substring search over ASCII.  The tzdata index is
// pure ASCII ("America/Argentina/Buenos_Aires" etc.) so a byte-wise
// scan is both correct and fast enough to re-run on every keystroke
// against the 313-entry table.
static int ascii_istrstr(const char *haystack, const char *needle) {
    if (!needle || !*needle) return 1;
    size_t nlen = strlen(needle);
    for (const char *h = haystack; *h; h++) {
        size_t k = 0;
        while (k < nlen && h[k]) {
            int a = (unsigned char)h[k];
            int b = (unsigned char)needle[k];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            k++;
        }
        if (k == nlen) return 1;
    }
    return 0;
}

// Repopulate the zone combo honouring the current filter text.  The
// previously-selected TzId is preserved when still visible, otherwise
// falls back to the first entry (UTC is always present unless the
// filter excludes every row).  Returns the TzId that ended up
// selected, or TZ_ID_UTC if the list is empty.
static TzId settings_populate_tz_combo(HWND hdlg, TzId preferredId) {
    HWND cb = GetDlgItem(hdlg, IDC_CBO_TZ);
    if (!cb) return TZ_ID_UTC;

    char filter[64] = { 0 };
    GetDlgItemTextA(hdlg, IDC_EDIT_TZ_FILTER, filter, sizeof filter);

    // Suppress combo redraws while we rebuild the list; otherwise
    // each CB_ADDSTRING causes a flicker on every keystroke.
    SendMessageW(cb, WM_SETREDRAW, FALSE, 0);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);

    int selIdx   = -1;
    int firstTz  = -1;
    int n        = Tz_Count();
    for (int i = 0; i < n; i++) {
        const char *name = Tz_AtIndex(i);
        if (!name) continue;
        if (!ascii_istrstr(name, filter)) continue;

        wchar_t w[64];
        MultiByteToWideChar(CP_UTF8, 0, name, -1, w,
                            sizeof(w) / sizeof(w[0]));
        int idx = (int)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)w);
        if (idx == CB_ERR || idx == CB_ERRSPACE) continue;
        SendMessageW(cb, CB_SETITEMDATA, idx, (LPARAM)(INT_PTR)i);
        if (firstTz < 0) firstTz = i;
        if (i == preferredId) selIdx = idx;
    }

    if (selIdx < 0 && firstTz >= 0) {
        // Preferred zone filtered out; pick whatever's on top.
        int rows = (int)SendMessageW(cb, CB_GETCOUNT, 0, 0);
        for (int j = 0; j < rows; j++) {
            LPARAM data = SendMessageW(cb, CB_GETITEMDATA, j, 0);
            if ((TzId)(INT_PTR)data == firstTz) { selIdx = j; break; }
        }
    }
    SendMessageW(cb, CB_SETCURSEL, selIdx >= 0 ? selIdx : 0, 0);
    SendMessageW(cb, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(cb, NULL, TRUE);

    if (selIdx < 0) return TZ_ID_UTC;
    LPARAM data = SendMessageW(cb, CB_GETITEMDATA, selIdx, 0);
    return (TzId)(INT_PTR)data;
}

// Read the currently-selected TzId from the combo.  Returns
// TZ_ID_INVALID when the combo is empty.
static TzId settings_current_tz(HWND hdlg) {
    HWND cb = GetDlgItem(hdlg, IDC_CBO_TZ);
    int sel = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) return TZ_ID_INVALID;
    LPARAM data = SendMessageW(cb, CB_GETITEMDATA, sel, 0);
    return (TzId)(INT_PTR)data;
}

// Refresh the "Current time:" line under the zone combo.  Runs off
// Clock_NowUtcMs so before the first trusted sync we show an em-dash
// rather than a bogus epoch-ish time.
static void settings_refresh_preview(HWND hdlg) {
    HWND label = GetDlgItem(hdlg, IDC_STATIC_TZ_PREVIEW);
    if (!label) return;

    TzId id = settings_current_tz(hdlg);
    int64_t utcMs = 0;
    if (id == TZ_ID_INVALID || !Clock_NowUtcMs(&utcMs)) {
        SetWindowTextW(label, L"\x2014");       // em-dash
        return;
    }

    TzifLocal tl;
    if (!Tz_LocalFromUtcMs(id, utcMs, &tl)) {
        SetWindowTextW(label, L"\x2014");
        return;
    }

    int offMin = tl.utcOffsetSec / 60;
    int offH   = offMin / 60;
    int offM   = offMin % 60;
    if (offM < 0) offM = -offM;

    static const char *const kWday[7] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    const char *wday = (tl.wday >= 0 && tl.wday < 7) ? kWday[tl.wday] : "";

    char buf[128];
    if (offM == 0) {
        snprintf(buf, sizeof buf,
                 "%s %04d-%02d-%02d  %02d:%02d:%02d  %s (UTC%+d)",
                 wday, tl.year, tl.month, tl.mday,
                 tl.hour, tl.minute, tl.second,
                 tl.abbr[0] ? tl.abbr : "",
                 offH);
    } else {
        snprintf(buf, sizeof buf,
                 "%s %04d-%02d-%02d  %02d:%02d:%02d  %s (UTC%+d:%02d)",
                 wday, tl.year, tl.month, tl.mday,
                 tl.hour, tl.minute, tl.second,
                 tl.abbr[0] ? tl.abbr : "",
                 offH, offM);
    }
    wchar_t wbuf[160];
    MultiByteToWideChar(CP_UTF8, 0, buf, -1, wbuf,
                        sizeof(wbuf) / sizeof(wbuf[0]));
    SetWindowTextW(label, wbuf);
}

// Attach a tooltip to a child control.  Creates the tooltip window
// lazily on first call via the caller-supplied HWND* cache.
static void settings_add_tip(HWND *tipOut, HWND hdlg, int ctlId,
                             const wchar_t *text) {
    if (!tipOut) return;
    if (!*tipOut) {
        *tipOut = CreateWindowExW(
            WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT,
            CW_USEDEFAULT, CW_USEDEFAULT,
            hdlg, NULL, GetModuleHandleW(NULL), NULL);
        if (!*tipOut) return;
        SendMessageW(*tipOut, TTM_SETMAXTIPWIDTH, 0, 320);
        // Slow down the auto-pop so users have time to read.
        SendMessageW(*tipOut, TTM_SETDELAYTIME, TTDT_AUTOPOP,
                     MAKELPARAM(15000, 0));
    }
    HWND ctl = GetDlgItem(hdlg, ctlId);
    if (!ctl) return;
    TOOLINFOW ti = { 0 };
    ti.cbSize   = sizeof ti;
    ti.uFlags   = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd     = hdlg;
    ti.uId      = (UINT_PTR)ctl;
    ti.lpszText = (LPWSTR)text;
    SendMessageW(*tipOut, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

static INT_PTR CALLBACK SettingsDlgProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp) {
    // Cache the tooltip window across messages via GWLP_USERDATA.
    // We stash it as a single HWND; free on WM_DESTROY.
    switch (msg) {
    case WM_INITDIALOG: {
        (void)lp;
        CheckDlgButton(hdlg, IDC_CHK_CHIMES,
                       g_chimesEnabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_CHK_UNMIN,
                       g_unminimizeOnChime ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_CHK_CONFIRM_CLOSE,
                       g_confirmOnClose ? BST_CHECKED : BST_UNCHECKED);

        // Populate zone combo from the embedded IANA index.  No OS
        // timezone API is consulted.
        settings_populate_tz_combo(hdlg, g_tzId);

        // Placeholder hint for the filter edit.  EM_SETCUEBANNER is
        // themed-edit-only but gracefully no-ops on classic style.
        SendDlgItemMessageW(hdlg, IDC_EDIT_TZ_FILTER,
                            EM_SETCUEBANNER, TRUE,
                            (LPARAM)L"Type to filter\x2026");

        // Tooltips.  All strings are static; TTN_GETDISPINFO not needed.
        HWND tip = NULL;
        settings_add_tip(&tip, hdlg, IDC_CHK_CHIMES,
            L"Play a short audible beep at each 5-minute mark that is "
            L"armed on the clock face. Chimes never fire while the "
            L"clock is out of sync.");
        settings_add_tip(&tip, hdlg, IDC_CHK_UNMIN,
            L"Restore the window from the taskbar at each armed mark, "
            L"even if chimes are off. Useful as a silent pomodoro timer.");
        settings_add_tip(&tip, hdlg, IDC_BTN_TEST_CHIME,
            L"Play the chime sound now so you can check volume and tone.");
        settings_add_tip(&tip, hdlg, IDC_CHK_CONFIRM_CLOSE,
            L"Ask for confirmation before closing the window. Prevents "
            L"accidental shutdowns mid-session.");
        settings_add_tip(&tip, hdlg, IDC_EDIT_TZ_FILTER,
            L"Type to narrow the list below. Case-insensitive substring "
            L"match across the full IANA name, e.g. \"paris\" or \"new_york\".");
        settings_add_tip(&tip, hdlg, IDC_CBO_TZ,
            L"Display time zone. Drawn from the embedded IANA tzdata; no "
            L"network or registry lookup is performed.");
        settings_add_tip(&tip, hdlg, IDC_STATIC_TZ_PREVIEW,
            L"Live preview of the selected zone's wall-clock time, "
            L"updated from the disciplined NTP clock.");
        settings_add_tip(&tip, hdlg, IDC_BTN_DEFAULTS,
            L"Reset every setting on this page to its factory value. "
            L"Nothing is written until you press OK.");
        SetWindowLongPtrW(hdlg, GWLP_USERDATA, (LONG_PTR)tip);

        // Drive the live preview off a 500 ms dialog timer.  Seed an
        // immediate update so the label is populated before the first
        // tick.
        SetTimer(hdlg, IDT_SETTINGS_PREVIEW, 500, NULL);
        settings_refresh_preview(hdlg);
        return TRUE;
    }

    case WM_TIMER:
        if (wp == IDT_SETTINGS_PREVIEW) {
            settings_refresh_preview(hdlg);
        }
        return TRUE;

    case WM_COMMAND: {
        WORD code = HIWORD(wp);
        WORD id   = LOWORD(wp);
        switch (id) {
        case IDC_EDIT_TZ_FILTER:
            if (code == EN_CHANGE) {
                TzId keep = settings_current_tz(hdlg);
                settings_populate_tz_combo(hdlg, keep);
                settings_refresh_preview(hdlg);
                return TRUE;
            }
            break;

        case IDC_CBO_TZ:
            if (code == CBN_SELCHANGE) {
                settings_refresh_preview(hdlg);
                return TRUE;
            }
            break;

        case IDC_BTN_TEST_CHIME:
            PlayBeep();
            return TRUE;

        case IDC_BTN_DEFAULTS: {
            CheckDlgButton(hdlg, IDC_CHK_CHIMES,        BST_CHECKED);
            CheckDlgButton(hdlg, IDC_CHK_UNMIN,         BST_UNCHECKED);
            CheckDlgButton(hdlg, IDC_CHK_CONFIRM_CLOSE, BST_UNCHECKED);
            SetDlgItemTextW(hdlg, IDC_EDIT_TZ_FILTER, L"");
            settings_populate_tz_combo(hdlg, TZ_ID_UTC);
            settings_refresh_preview(hdlg);
            return TRUE;
        }

        case IDOK: {
            g_chimesEnabled     = IsDlgButtonChecked(hdlg, IDC_CHK_CHIMES)        == BST_CHECKED;
            g_unminimizeOnChime = IsDlgButtonChecked(hdlg, IDC_CHK_UNMIN)         == BST_CHECKED;
            g_confirmOnClose    = IsDlgButtonChecked(hdlg, IDC_CHK_CONFIRM_CLOSE) == BST_CHECKED;

            TzId id = settings_current_tz(hdlg);
            const char *nm = (id == TZ_ID_INVALID) ? NULL : Tz_Name(id);
            if (!nm || id == TZ_ID_UTC) {
                g_tzIana[0] = 0;
                g_tzId = TZ_ID_UTC;
            } else {
                snprintf(g_tzIana, sizeof(g_tzIana), "%s", nm);
                g_tzId = id;
            }

            SaveSettings();
            UpdateTimezone();
            InvalidateRect(g_hwnd, NULL, FALSE);

            EndDialog(hdlg, IDOK);
            return TRUE;
        }

        case IDCANCEL:
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        break;
    }

    case WM_DESTROY: {
        KillTimer(hdlg, IDT_SETTINGS_PREVIEW);
        HWND tip = (HWND)GetWindowLongPtrW(hdlg, GWLP_USERDATA);
        if (tip) DestroyWindow(tip);
        SetWindowLongPtrW(hdlg, GWLP_USERDATA, 0);
        return TRUE;
    }
    }
    return FALSE;
}

static void ShowSettings(void) {
    DialogBoxParamW(GetModuleHandleW(NULL),
                    MAKEINTRESOURCEW(IDD_SETTINGS),
                    g_hwnd, SettingsDlgProc, 0);
}

// ---------------------------------------------------------------------------
// Tick handler (fires 10x per second via WM_TIMER)
// ---------------------------------------------------------------------------

static void Tick(void) {
    // Periodic NTP re-sync. Binary cadence: 5 s while INOP (to recover
    // fast), 60 s once all three sources concur.
    DWORD nowMs = GetTickCount();
    DWORD interval = (Clock_Trust() == TRUST_OK)
        ? NTP_INTERVAL_OK_MS
        : NTP_INTERVAL_INOP_MS;
    if ((nowMs - g_lastNtpKickMs) >= interval) {
        Ntp_Start();
        g_lastNtpKickMs = nowMs;
    }

    // Minute-crossing beep detection. Skip entirely if the clockwork
    // is not synced this run -- chimes must never fire off an
    // untrusted time.
    int64_t displayMs = 0;
    if (!Clock_NowUtcMs(&displayMs)) {
        g_prevMins = -1.0f;   // reset so we don't cascade beeps on re-sync
        return;
    }
    struct tm lt = {0};
    int ms = 0;
    if (!UtcMsToLocalTm(displayMs, &lt, &ms)) return;
    float secs = (float)lt.tm_sec + ms / 1000.0f;
    float mins = (float)lt.tm_min + secs / 60.0f;

    //
    // Only honor a crossing when the apparent minute-step is 1 or 2.
    // Anything larger means the wall clock has jumped (DST transition,
    // large NTP correction, user setting the time manually, RDP session
    // resume), and must not trigger a cascade of beeps. A 2-minute
    // ceiling still tolerates a lagged tick on a loaded system.
    //
    if (g_prevMins >= 0.0f) {
        int prevFloor = (int)floorf(g_prevMins);
        int curFloor  = (int)floorf(mins);
        int delta     = curFloor - prevFloor;
        if (delta < 0) delta += 60;         // normal minute-60 rollover

        // Refresh the IANA-derived zone label whenever the local
        // minute advances.  Catches DST transitions within a minute
        // of the actual jump; on a steady-state zone the label
        // recomputes to the same string and UpdateTitleBar's
        // s_last cache suppresses the SetWindowTextW call.
        if (delta > 0) UpdateTimezone();

        if (delta >= 1 && delta <= 2) {
            for (int k = 1; k <= delta; k++) {
                int mm = (prevFloor + k) % 60;
                if (mm % 5 != 0) continue;
                int idx = mm / 5;
                if (!g_armed[idx]) continue;
                // Armed marker reached. Depending on settings:
                //   chimes on  + unmin on  -> beep AND restore window
                //   chimes on  + unmin off -> beep only
                //   chimes off + unmin on  -> restore window, no sound
                //   chimes off + unmin off -> nothing (both disabled)
                if (g_chimesEnabled) PlayBeep();
                if (g_unminimizeOnChime && IsIconic(g_hwnd))
                    ShowWindow(g_hwnd, SW_RESTORE);
            }
        }
    }
    g_prevMins = mins;

    // Refresh the title-bar trust indicator (no-op when unchanged).
    UpdateTitleBar();

    // When visible, repaint the window. When minimized, tell DWM that
    // our cached iconic bitmaps are stale; DWM will fire another
    // WM_DWMSENDICONICTHUMBNAIL the next time the taskbar needs one.
    if (IsIconic(g_hwnd)) {
        DwmInvalidateIconicBitmaps(g_hwnd);
    } else {
        InvalidateRect(g_hwnd, NULL, FALSE);
    }

    // Phase-lock the next tick to the wall-clock decisecond boundary.
    // Calling SetTimer with an existing (hwnd,id) resets the interval.
    // This keeps the once-per-second transitions on the second hand
    // close to the true second tick rather than drifting by up to
    // TICK_MS after hours of uptime. USER_TIMER_MINIMUM is ~10 ms, so
    // we clamp to that to avoid a pathological tight loop.
    int phase = ms % TICK_MS;
    UINT next = (UINT)(TICK_MS - phase);
    if (next < 10) next += TICK_MS;
    SetTimer(g_hwnd, IDT_REPAINT, next, NULL);
}

// ---------------------------------------------------------------------------
// WndProc
// ---------------------------------------------------------------------------

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        HICON ico = (HICON)LoadImageW(GetModuleHandleW(NULL),
                                      MAKEINTRESOURCEW(1),
                                      IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
        if (ico) {
            SendMessageW(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)ico);
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)ico);
        }
        InstallSystemMenuItems();
        UpdateTimezone();
        // Opt into DWM-driven iconic thumbnails + live previews so the
        // taskbar shows a current-time clock instead of a frozen
        // pre-minimize snapshot. Must be set after the HWND exists.
        {
            BOOL on = TRUE;
            DwmSetWindowAttribute(hwnd, DWMWA_FORCE_ICONIC_REPRESENTATION,
                                  &on, sizeof(on));
            DwmSetWindowAttribute(hwnd, DWMWA_HAS_ICONIC_BITMAP,
                                  &on, sizeof(on));
        }
        SetTimer(hwnd, IDT_REPAINT, TICK_MS, NULL);
        return 0;
    }

    case WM_TIMER:
        if (wp == IDT_REPAINT) Tick();
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        Paint();
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE: {
        UINT w = LOWORD(lp), h = HIWORD(lp);
        if (g_hwndRt && w > 0 && h > 0) {
            D2D1_SIZE_U sz = { w, h };
            ID2D1HwndRenderTarget_Resize(g_hwndRt, &sz);
        }
        return 0;
    }

    case WM_DWMSENDICONICTHUMBNAIL: {
        // HIWORD(lp) = max width, LOWORD(lp) = max height (per MSDN).
        // We render the clock square, inscribed in the smaller dim.
        int maxW = HIWORD(lp), maxH = LOWORD(lp);
        int s = (maxW < maxH) ? maxW : maxH;
        HBITMAP hbm = RenderClockToBitmap(s, s);
        if (hbm) {
            DwmSetIconicThumbnail(hwnd, hbm, 0);
            DeleteObject(hbm);
        }
        return 0;
    }

    case WM_DWMSENDICONICLIVEPREVIEWBITMAP: {
        // Aero Peek / task-switcher: render at the window's own client
        // size so the preview matches what the restored window looks like.
        RECT cr; GetClientRect(hwnd, &cr);
        int w = cr.right - cr.left, h = cr.bottom - cr.top;
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        HBITMAP hbm = RenderClockToBitmap(w, h);
        if (hbm) {
            DwmSetIconicLivePreviewBitmap(hwnd, hbm, NULL, 0);
            DeleteObject(hbm);
        }
        return 0;
    }

    case 0x02E0 /* WM_DPICHANGED */: {
        // Windows suggests a new window rect sized for the new DPI.
        RECT *r = (RECT*)lp;
        if (r) {
            SetWindowPos(hwnd, NULL, r->left, r->top,
                         r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }

    case WM_ERASEBKGND:
        // D2D owns the client area; refuse GDI erase to avoid flicker.
        return 1;

    case WM_LBUTTONUP: {
        RECT rc; GetClientRect(hwnd, &rc);
        float dw = (float)rc.right, dh = (float)rc.bottom;
        float S  = (dw < dh ? dw : dh);
        float cx = dw * 0.5f, cy = dh * 0.5f;
        if (S > 40.0f) {
            int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
            int idx = HitTestHour((float)mx, (float)my, cx, cy, S);
            if (idx >= 0) {
                g_armed[idx] = !g_armed[idx];
                SaveArmed(g_armed);
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    }

    case WM_SYSCOMMAND: {
        UINT raw = (UINT)wp;
        switch (raw) {
        case IDM_ALWAYS_ON_TOP:
            g_alwaysOnTop = !g_alwaysOnTop;
            ApplyAlwaysOnTop(g_alwaysOnTop);
            SyncAlwaysOnTopCheck();
            Log_Append("user: always-on-top -> %s",
                       g_alwaysOnTop ? "on" : "off");
            return 0;
        case IDM_TEST_BEEP: Log_Append("user: test beep"); PlayBeep(); return 0;
        case IDM_SETTINGS:  Log_Append("user: opened Settings"); ShowSettings();   return 0;
        case IDM_LOG:       ShowLogViewer(); return 0;
        case IDM_ABOUT:     Log_Append("user: opened About"); ShowAbout();      return 0;
        }
        break;
    }

    case WM_CLOSE:
        if (g_confirmOnClose) {
            int r = MessageBoxW(hwnd, L"Close Lunar?", L"Lunar",
                                MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
            if (r != IDOK) return 0;
        }
        Log_Append("app: shutting down");
        SaveWindowState(hwnd, g_alwaysOnTop);
        Clock_Shutdown();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_REPAINT);
        ReleaseHandCache();
        DiscardDeviceResources();
        if (g_dcRt)   { ID2D1DCRenderTarget_Release(g_dcRt); g_dcRt   = NULL; }
        if (g_txtSys) { IDWriteTextFormat_Release(g_txtSys); g_txtSys = NULL; }
        if (g_dw)     { IDWriteFactory_Release(g_dw);        g_dw     = NULL; }
        if (g_d2d)    { ID2D1Factory_Release(g_d2d);         g_d2d    = NULL; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------

#ifndef LUNAR_NO_MAIN
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nShow) {
    (void)hPrev; (void)cmdLine;

    // Enable Per-Monitor-V2 DPI awareness so the system menu, title bar,
    // and dialogs are rendered at native pixel density instead of being
    // bitmap-scaled (which strips antialiasing). Fall back to older
    // system-DPI awareness on pre-1703 Windows. We resolve the entry
    // points dynamically to keep the binary loadable on old systems.
    {
        HMODULE user = GetModuleHandleW(L"user32.dll");
        typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
        SetCtxFn setCtx = user ? (SetCtxFn)(void (*)(void))GetProcAddress(
            user, "SetProcessDpiAwarenessContext") : NULL;
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == ((HANDLE)-4)
        if (!setCtx || !setCtx((HANDLE)(INT_PTR)-4)) {
            SetProcessDPIAware();
        }
    }

    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   &IID_ID2D1Factory, NULL, (void**)&g_d2d);
    if (FAILED(hr)) return 1;
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                             &IID_IDWriteFactory, (IUnknown**)&g_dw);
    if (FAILED(hr)) return 1;

    // Register standard + tooltip common control classes.  Requires
    // the v6 comctl32 dependency declared in lunar.manifest.
    {
        INITCOMMONCONTROLSEX icc = { sizeof icc,
            ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
        InitCommonControlsEx(&icc);
    }

    LoadArmed(g_armed);

    WNDCLASSEXW wc = { 0 };
    wc.cbSize        = sizeof wc;
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = CLASS_NAME;
    wc.hIcon         = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1),
                                         IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    wc.hIconSm       = wc.hIcon;
    RegisterClassExW(&wc);

    // Scale the default window size by the primary monitor's DPI so the
    // clock keeps the same physical size it had before we became
    // DPI-aware (on a 150% display, DEFAULT_W*1.5 physical pixels).
    int winW = DEFAULT_W, winH = DEFAULT_H;
    {
        HDC hdc = GetDC(NULL);
        if (hdc) {
            int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
            ReleaseDC(NULL, hdc);
            if (dpi > 0) {
                winW = MulDiv(DEFAULT_W, dpi, 96);
                winH = MulDiv(DEFAULT_H, dpi, 96);
            }
        }
    }

    // Adjust so the CLIENT area (where the dial is drawn) is winW x winH;
    // otherwise title bar + borders eat one axis and the square dial
    // ends up inscribed in a rectangular client, wasting pixels.
    RECT adj = { 0, 0, winW, winH };
    AdjustWindowRectEx(&adj, WS_OVERLAPPEDWINDOW, FALSE, 0);
    int outerW = adj.right  - adj.left;
    int outerH = adj.bottom - adj.top;

    // Restore the previous session's window position, size, and
    // always-on-top state if we have them.
    WindowState ws = {0};
    LoadWindowState(&ws);
    LoadSettings();

    int posX = CW_USEDEFAULT, posY = CW_USEDEFAULT;
    if (ws.valid) {
        posX   = ws.x;
        posY   = ws.y;
        outerW = ws.w;
        outerH = ws.h;
        g_alwaysOnTop = ws.alwaysOnTop;
    }

    HWND hwnd = CreateWindowExW(0, CLASS_NAME, APP_TITLE,
        WS_OVERLAPPEDWINDOW,
        posX, posY, outerW, outerH,
        NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    if (g_alwaysOnTop) ApplyAlwaysOnTop(1);

    // Kick off the first NTP sync as soon as the window exists.
    Clock_Init();
    Log_Append("app: Lunar 0.2 started; initiating first NTP sync");
    Ntp_Start();
    g_lastNtpKickMs = GetTickCount();

    int showCmd = nShow ? nShow : SW_SHOWDEFAULT;
    if (ws.valid && ws.maximized) showCmd = SW_SHOWMAXIMIZED;
    ShowWindow(hwnd, showCmd);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
#endif /* LUNAR_NO_MAIN */
