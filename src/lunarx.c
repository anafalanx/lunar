/* lunarx.c -- the Tcl command surface over Lunar's C engine.
 *
 * A stubs-based Tcl extension (statically linked into the exe and registered
 * in Lunar_AppInit) that exposes the kept engine -- clock discipline, the
 * NTP/NTS aggregator, the update check -- to the Tcl/Tk shell as ::lunar::*
 * commands. The UI polls these (all quick, mutex-protected reads); the
 * networking stays on the engine's own background threads.
 *
 * Follows els's extension pattern: Tcl_InitStubs, fully-qualified command
 * names created at Init, "" as the success sentinel.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>   /* Shell_NotifyIcon (system tray) */
#include <commctrl.h>   /* SetWindowSubclass (marshal the tray callback) */
#include <mmsystem.h>   /* PlaySoundW (chime) */
#include <tcl.h>

#include <string.h>
#include <stdint.h>
#include <math.h>

#include "clock.h"
#include "ntp.h"
#include "update_check.h"
#include "tz.h"
#include "tz_winmap.h"
#include "logbuf.h"
#include "version.h"

static const char *trust_name(TrustState s) {
    switch (s) {
        case TRUST_OK:          return "ok";
        case TRUST_DEGRADED:    return "degraded";
        case TRUST_HOLDOVER:    return "holdover";
        case TRUST_REACQUIRING: return "reacquiring";
        case TRUST_INOP:        default: return "inop";
    }
}

static const char *auth_name(NtpAuthMode m) {
    switch (m) {
        case NTP_AUTH_PLAIN_SNTP:   return "sntp";
        case NTP_AUTH_ENROLLED_PIN: return "nts";
        case NTP_AUTH_ROTATED_PIN:  return "nts-rotated";
        case NTP_AUTH_NONE:         default: return "none";
    }
}

#define PUT(d, k, v) Tcl_DictObjPut(ip, (d), Tcl_NewStringObj((k), -1), (v))

/* ---- chime (ported verbatim from the Win32 shell's PlayBeep) -----------
 * A single 880 Hz / 250 ms sine chime rendered to an in-memory WAV and
 * played async, volume-compensated against the system master slider. */
extern float Sysvol_Get(void);   /* sysvol.c (compiled into the engine) */

#define BEEP_SAMPLE_RATE 44100
#define BEEP_FRAMES      (BEEP_SAMPLE_RATE / 4)          /* 250 ms */
#define BEEP_FREQ_HZ     880.0f                          /* A5 */
#define BEEP_TARGET_SPEAKER_AMPLITUDE 0.276f
#define BEEP_DATA_BYTES  (BEEP_FRAMES * 2)
#define BEEP_BUF_BYTES   (44 + BEEP_DATA_BYTES)

static unsigned char g_wav[BEEP_BUF_BYTES];
static void WriteLE16(unsigned char *p, uint16_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }
static void WriteLE32(unsigned char *p, uint32_t v) {
    p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF;
}
static void BuildWav(float freqHz, float amplitude) {
    unsigned char *p = g_wav;
    memcpy(p, "RIFF", 4);               p += 4;
    WriteLE32(p, 36 + BEEP_DATA_BYTES); p += 4;
    memcpy(p, "WAVE", 4);               p += 4;
    memcpy(p, "fmt ", 4);               p += 4;
    WriteLE32(p, 16);                   p += 4;
    WriteLE16(p, 1);                    p += 2;
    WriteLE16(p, 1);                    p += 2;
    WriteLE32(p, BEEP_SAMPLE_RATE);     p += 4;
    WriteLE32(p, BEEP_SAMPLE_RATE * 2); p += 4;
    WriteLE16(p, 2);                    p += 2;
    WriteLE16(p, 16);                   p += 2;
    memcpy(p, "data", 4);               p += 4;
    WriteLE32(p, BEEP_DATA_BYTES);      p += 4;
    const float TAU = 6.28318530717958647692f;
    const float attackFrames  = BEEP_SAMPLE_RATE * 0.010f;
    const float releaseFrames = BEEP_SAMPLE_RATE * 0.030f;
    for (int i = 0; i < BEEP_FRAMES; i++) {
        float env = 1.0f;
        if (i < attackFrames)                     env = (float)i / attackFrames;
        else if (i > BEEP_FRAMES - releaseFrames) env = (BEEP_FRAMES - i) / releaseFrames;
        float s = sinf(TAU * freqHz * (float)i / BEEP_SAMPLE_RATE) * env * amplitude;
        int v = (int)(s * 32767.0f);
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        WriteLE16(p, (uint16_t)(int16_t)v);
        p += 2;
    }
}
static void PlayBeepImpl(void) {
    float v = Sysvol_Get();
    if (v <= 0.01f) return;
    float amp = BEEP_TARGET_SPEAKER_AMPLITUDE / v;
    if (amp > 0.90f) amp = 0.90f;
    if (amp < 0.05f) amp = 0.05f;
    BuildWav(BEEP_FREQ_HZ, amp);
    PlaySoundW((LPCWSTR)g_wav, NULL, SND_MEMORY | SND_ASYNC);
}
/* lunar::beep -- play the chime once. */
static int Beep_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                    int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }
    PlayBeepImpl();
    Tcl_SetObjResult(ip, Tcl_NewObj());
    return TCL_OK;
}

/* lunar::engine_start -- one-time engine bootstrap. */
static int EngineStart_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                           int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }
    static LONG started = 0;
    if (InterlockedCompareExchange(&started, 1, 0) == 0) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);   /* refcounted; safe if engine also inits */
        Clock_Init();
        UpdateCheck_Start();
        Ntp_Start();
    }
    Tcl_SetObjResult(ip, Tcl_NewObj());
    return TCL_OK;
}

/* lunar::syncnow -- kick a polling cycle (no-op if one is in flight). */
static int SyncNow_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                       int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }
    Ntp_Start();
    Tcl_SetObjResult(ip, Tcl_NewObj());
    return TCL_OK;
}

/* lunar::shutdown -- drain workers, persist rate + the diagnostic log
 * before the process exits (crash-survival parity with the Win32 shell). */
static int Shutdown_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                        int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }
    Ntp_Shutdown();
    Clock_Shutdown();
    Log_FlushToDisk(NULL);
    Tcl_SetObjResult(ip, Tcl_NewObj());
    return TCL_OK;
}

/* lunar::log_text -- the whole in-memory event log, oldest first, as one
 * \r\n-delimited string (what the Log viewer shows). */
static int LogText_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                       int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }
    size_t need = Log_Snapshot(NULL, 0);
    char *buf = (char *)Tcl_Alloc(need + 1);
    size_t n = Log_Snapshot(buf, need + 1);
    Tcl_SetObjResult(ip, Tcl_NewStringObj(buf, (Tcl_Size)n));
    Tcl_Free(buf);
    return TCL_OK;
}

/* lunar::about -- {version X tzdata Y} for the About box. */
static int About_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                     int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }
    Tcl_Obj *d = Tcl_NewDictObj();
    PUT(d, "version", Tcl_NewStringObj(LUNAR_VERSION_STR, -1));
    PUT(d, "tzdata",  Tcl_NewStringObj(Tz_Version(), -1));
    Tcl_SetObjResult(ip, d);
    return TCL_OK;
}

/* lunar::status -- a dict of everything the dashboard needs. */
static int Status_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                      int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }

    ClockDisplay disp;
    Clock_GetDisplay(&disp);

    int64_t sysDelta = 0;
    int sysValid = Clock_SystemDeltaMs(&sysDelta);

    Tcl_Obj *d = Tcl_NewDictObj();
    PUT(d, "state",         Tcl_NewStringObj(trust_name(disp.state), -1));
    PUT(d, "hasTime",       Tcl_NewIntObj(disp.state >= TRUST_HOLDOVER ? 1 : 0));
    PUT(d, "utcMs",         Tcl_NewWideIntObj((Tcl_WideInt)disp.utcMs));
    PUT(d, "boundMs",       Tcl_NewWideIntObj((Tcl_WideInt)disp.boundMs));
    PUT(d, "lastSyncUtcMs", Tcl_NewWideIntObj((Tcl_WideInt)disp.lastSyncUtcMs));
    PUT(d, "lastSyncAgeMs", Tcl_NewWideIntObj((Tcl_WideInt)disp.lastSyncAgeMs));
    PUT(d, "sysDeltaValid", Tcl_NewIntObj(sysValid ? 1 : 0));
    PUT(d, "sysDeltaMs",    Tcl_NewWideIntObj((Tcl_WideInt)sysDelta));
    PUT(d, "ratePpm",       Tcl_NewIntObj((int)Clock_RatePpm()));
    PUT(d, "spreadMs",      Tcl_NewWideIntObj((Tcl_WideInt)Ntp_LastSpreadMs()));
    PUT(d, "synced",        Tcl_NewIntObj(Ntp_IsSynced() ? 1 : 0));

    Tcl_SetObjResult(ip, d);
    return TCL_OK;
}

/* lunar::sources -- a list of per-source dicts from the last cycle. */
static int Sources_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                       int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }

    NtpSourceResult res[NTP_SOURCE_COUNT];
    Ntp_GetResults(res);

    Tcl_Obj *list = Tcl_NewListObj(0, NULL);
    for (int i = 0; i < NTP_SOURCE_COUNT; i++) {
        const NtpSourceResult *r = &res[i];
        Tcl_Obj *e = Tcl_NewDictObj();
        PUT(e, "slot",   Tcl_NewIntObj(i));
        PUT(e, "kind",   Tcl_NewStringObj(i >= NTP_FIRST_NTS_SLOT ? "nts" : "core", -1));
        PUT(e, "label",  Tcl_NewStringObj(r->label ? r->label : "", -1));
        PUT(e, "ok",     Tcl_NewIntObj(r->ok ? 1 : 0));
        PUT(e, "offsetMs", Tcl_NewWideIntObj((Tcl_WideInt)r->offsetMs));
        PUT(e, "rttMs",  Tcl_NewIntObj((int)r->rttMs));
        PUT(e, "auth",   Tcl_NewStringObj(auth_name(r->authMode), -1));
        PUT(e, "family", Tcl_NewStringObj(r->operatorFamily ? r->operatorFamily : "", -1));
        Tcl_ListObjAppendElement(ip, list, e);
    }
    Tcl_SetObjResult(ip, list);
    return TCL_OK;
}

/* One-entry TzId cache: the UI resolves the same zone name every tick.
 * Commands only ever run on the Tk thread, so no locking. */
static char g_tzCacheName[64];
static TzId g_tzCacheId = TZ_ID_INVALID;

static TzId tz_resolve(const char *name) {
    if (!name || !name[0]) return TZ_ID_INVALID;
    if (g_tzCacheId != TZ_ID_INVALID &&
        strcmp(name, g_tzCacheName) == 0) return g_tzCacheId;
    TzId id = Tz_FindByName(name);
    if (id != TZ_ID_INVALID && strlen(name) < sizeof g_tzCacheName) {
        strcpy(g_tzCacheName, name);
        g_tzCacheId = id;
    }
    return id;
}

/* lunar::localtime utcMs zone -- break a UTC instant into wall-clock
 * components for an embedded IANA zone (never the OS). Errors on a zone
 * missing from the embedded index so the caller can fall back. */
static int LocalTime_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                         int objc, Tcl_Obj *const objv[]) {
    if (objc != 3) { Tcl_WrongNumArgs(ip, 1, objv, "utcMs zone"); return TCL_ERROR; }
    Tcl_WideInt utcMs;
    if (Tcl_GetWideIntFromObj(ip, objv[1], &utcMs) != TCL_OK) return TCL_ERROR;
    const char *zone = Tcl_GetString(objv[2]);

    TzId id = tz_resolve(zone);
    if (id == TZ_ID_INVALID) {
        Tcl_SetObjResult(ip, Tcl_ObjPrintf("unknown zone \"%s\"", zone));
        return TCL_ERROR;
    }
    TzifLocal lt;
    if (!Tz_LocalFromUtcMs(id, (int64_t)utcMs, &lt)) {
        Tcl_SetObjResult(ip, Tcl_ObjPrintf("cannot resolve %s", zone));
        return TCL_ERROR;
    }
    Tcl_Obj *d = Tcl_NewDictObj();
    PUT(d, "year",   Tcl_NewIntObj(lt.year));
    PUT(d, "month",  Tcl_NewIntObj(lt.month));
    PUT(d, "day",    Tcl_NewIntObj(lt.mday));
    PUT(d, "hour",   Tcl_NewIntObj(lt.hour));
    PUT(d, "minute", Tcl_NewIntObj(lt.minute));
    PUT(d, "second", Tcl_NewIntObj(lt.second));
    PUT(d, "wday",   Tcl_NewIntObj(lt.wday));
    PUT(d, "isDst",  Tcl_NewIntObj(lt.isDst));
    PUT(d, "offSec", Tcl_NewIntObj(lt.utcOffsetSec));
    PUT(d, "abbr",   Tcl_NewStringObj(lt.abbr, -1));
    Tcl_SetObjResult(ip, d);
    return TCL_OK;
}

/* lunar::tz_list -- every embedded canonical zone name. */
static int TzList_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                      int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }
    Tcl_Obj *list = Tcl_NewListObj(0, NULL);
    int n = Tz_Count();
    for (int i = 0; i < n; i++) {
        const char *name = Tz_AtIndex(i);
        if (name) Tcl_ListObjAppendElement(ip, list, Tcl_NewStringObj(name, -1));
    }
    Tcl_SetObjResult(ip, list);
    return TCL_OK;
}

/* lunar::tz_version -- the embedded tzdata release ("2026b"). */
static int TzVersion_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                         int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }
    Tcl_SetObjResult(ip, Tcl_NewStringObj(Tz_Version(), -1));
    return TCL_OK;
}

/* lunar::tz_suggest -- the OS zone mapped to an embedded IANA name, or "".
 * Reading the zone NAME is not trusting the OS clock. */
static int TzSuggest_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                         int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }
    char sug[64] = "";
    if (!TzWinmap_CurrentIana(sug, sizeof sug)) sug[0] = 0;
    Tcl_SetObjResult(ip, Tcl_NewStringObj(sug, -1));
    return TCL_OK;
}

/* ---- system tray -------------------------------------------------------
 * The tray callback arrives in the Tk window's message dispatch (Tk pumps
 * the message queue on the Tcl thread), so -- exactly like els's windrop --
 * we never eval Tcl inline: we Tcl_QueueEvent and run ::lunar::tray_event at
 * a safe point in the event loop. */
#define LUNAR_TRAY_MSG (WM_APP + 17)
#define LUNAR_TRAY_UID 0x4C55       /* 'LU' */

static Tcl_Interp *g_uiInterp = NULL;   /* set at tray_add, used on same thread */
static int g_trayActive = 0;

typedef struct TrayEvent { Tcl_Event ev; char kind[16]; } TrayEvent;

static int TrayEventProc(Tcl_Event *evPtr, [[maybe_unused]] int flags) {
    TrayEvent *te = (TrayEvent *)evPtr;
    if (g_uiInterp) {
        Tcl_Obj *cmd = Tcl_NewListObj(0, NULL);
        Tcl_IncrRefCount(cmd);
        Tcl_ListObjAppendElement(g_uiInterp, cmd, Tcl_NewStringObj("::lunar::tray_event", -1));
        Tcl_ListObjAppendElement(g_uiInterp, cmd, Tcl_NewStringObj(te->kind, -1));
        if (Tcl_EvalObjEx(g_uiInterp, cmd, TCL_EVAL_GLOBAL) != TCL_OK) {
            Tcl_BackgroundException(g_uiInterp, TCL_ERROR);
        }
        Tcl_DecrRefCount(cmd);
    }
    return 1;
}
static void tray_queue(const char *kind) {
    TrayEvent *te = (TrayEvent *)Tcl_Alloc(sizeof(TrayEvent));
    te->ev.proc = TrayEventProc;
    te->ev.nextPtr = NULL;
    lstrcpynA(te->kind, kind, (int)sizeof te->kind);
    Tcl_QueueEvent((Tcl_Event *)te, TCL_QUEUE_TAIL);
}
static LRESULT CALLBACK TraySubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                     UINT_PTR uid, [[maybe_unused]] DWORD_PTR ref) {
    if (msg == LUNAR_TRAY_MSG) {
        switch (LOWORD(lp)) {
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK: tray_queue("activate"); break;
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:   tray_queue("menu");     break;
            default: break;
        }
        return 0;
    }
    if (msg == WM_ENDSESSION && wp) {
        /* OS shutdown/logoff bypasses the normal quit path: persist the
         * disciplined rate + the diagnostic log NOW (we run on the Tk
         * thread, so calling the engine directly is safe). No Ntp_Shutdown
         * -- its worker drain could eat the seconds Windows grants us. */
        Clock_Shutdown();
        Log_FlushToDisk(NULL);
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, TraySubclass, uid);
    return DefSubclassProc(hwnd, msg, wp, lp);
}
static void tray_fill(NOTIFYICONDATAW *nid, HWND hwnd, const char *tip) {
    memset(nid, 0, sizeof *nid);
    nid->cbSize = sizeof *nid;
    nid->hWnd = hwnd;
    nid->uID = LUNAR_TRAY_UID;
    nid->uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid->uCallbackMessage = LUNAR_TRAY_MSG;
    nid->hIcon = LoadIconW(GetModuleHandleW(NULL), L"lunar");
    if (!nid->hIcon) nid->hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    if (tip) MultiByteToWideChar(CP_UTF8, 0, tip, -1, nid->szTip,
                                 (int)(sizeof nid->szTip / sizeof nid->szTip[0]));
}

/* lunar::tray_add hwnd tooltip -- show/replace the tray icon. */
static int TrayAdd_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                       int objc, Tcl_Obj *const objv[]) {
    if (objc != 3) { Tcl_WrongNumArgs(ip, 1, objv, "hwnd tooltip"); return TCL_ERROR; }
    Tcl_WideInt h;
    if (Tcl_GetWideIntFromObj(ip, objv[1], &h) != TCL_OK) return TCL_ERROR;
    HWND hwnd = (HWND)(intptr_t)h;
    if (!IsWindow(hwnd)) { Tcl_SetObjResult(ip, Tcl_NewStringObj("not a window", -1)); return TCL_OK; }
    g_uiInterp = ip;
    SetWindowSubclass(hwnd, TraySubclass, LUNAR_TRAY_UID, 0);
    NOTIFYICONDATAW nid;
    tray_fill(&nid, hwnd, Tcl_GetString(objv[2]));
    Shell_NotifyIconW(g_trayActive ? NIM_MODIFY : NIM_ADD, &nid);
    g_trayActive = 1;
    Tcl_SetObjResult(ip, Tcl_NewObj());
    return TCL_OK;
}
/* lunar::tray_tip hwnd tooltip -- update the tooltip. */
static int TrayTip_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                       int objc, Tcl_Obj *const objv[]) {
    if (objc != 3) { Tcl_WrongNumArgs(ip, 1, objv, "hwnd tooltip"); return TCL_ERROR; }
    if (!g_trayActive) { Tcl_SetObjResult(ip, Tcl_NewObj()); return TCL_OK; }
    Tcl_WideInt h;
    if (Tcl_GetWideIntFromObj(ip, objv[1], &h) != TCL_OK) return TCL_ERROR;
    NOTIFYICONDATAW nid;
    tray_fill(&nid, (HWND)(intptr_t)h, Tcl_GetString(objv[2]));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    Tcl_SetObjResult(ip, Tcl_NewObj());
    return TCL_OK;
}
/* lunar::tray_remove hwnd -- remove the tray icon. */
static int TrayRemove_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                          int objc, Tcl_Obj *const objv[]) {
    if (objc != 2) { Tcl_WrongNumArgs(ip, 1, objv, "hwnd"); return TCL_ERROR; }
    if (g_trayActive) {
        Tcl_WideInt h;
        if (Tcl_GetWideIntFromObj(ip, objv[1], &h) != TCL_OK) return TCL_ERROR;
        NOTIFYICONDATAW nid;
        memset(&nid, 0, sizeof nid);
        nid.cbSize = sizeof nid; nid.hWnd = (HWND)(intptr_t)h; nid.uID = LUNAR_TRAY_UID;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        g_trayActive = 0;
    }
    Tcl_SetObjResult(ip, Tcl_NewObj());
    return TCL_OK;
}

/* lunar::run_at_startup ?0|1? -- query (no arg) or set the HKCU Run entry.
 * Returns 1/0 on query, "" on set. The Run key is the source of truth. */
static int RunAtStartup_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                            int objc, Tcl_Obj *const objv[]) {
    static const wchar_t *kSub = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    static const wchar_t *kVal = L"Lunar";
    if (objc == 1) {
        int on = 0; HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kSub, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
            if (RegQueryValueExW(k, kVal, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) on = 1;
            RegCloseKey(k);
        }
        Tcl_SetObjResult(ip, Tcl_NewIntObj(on));
        return TCL_OK;
    }
    if (objc != 2) { Tcl_WrongNumArgs(ip, 1, objv, "?0|1?"); return TCL_ERROR; }
    int enable;
    if (Tcl_GetBooleanFromObj(ip, objv[1], &enable) != TCL_OK) return TCL_ERROR;
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSub, 0, NULL, 0, KEY_SET_VALUE,
                        NULL, &k, NULL) != ERROR_SUCCESS) {
        Tcl_SetObjResult(ip, Tcl_NewStringObj("cannot open Run key", -1));
        return TCL_OK;
    }
    if (enable) {
        wchar_t exe[MAX_PATH]; DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            wchar_t q[MAX_PATH + 2];
            int qn = wsprintfW(q, L"\"%s\"", exe);
            RegSetValueExW(k, kVal, 0, REG_SZ, (const BYTE *)q,
                           (DWORD)((qn + 1) * (int)sizeof(wchar_t)));
        }
    } else {
        RegDeleteValueW(k, kVal);
    }
    RegCloseKey(k);
    Tcl_SetObjResult(ip, Tcl_NewObj());
    return TCL_OK;
}

/* lunar::update -- {available 0|1 version X.Y.Z}. */
static int Update_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                      int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }
    char ver[32] = "";
    int avail = UpdateCheck_Available(ver, sizeof ver);
    Tcl_Obj *d = Tcl_NewDictObj();
    PUT(d, "available", Tcl_NewIntObj(avail ? 1 : 0));
    PUT(d, "version",   Tcl_NewStringObj(ver, -1));
    Tcl_SetObjResult(ip, d);
    return TCL_OK;
}

int Lunarx_Init(Tcl_Interp *ip) {
    if (Tcl_InitStubs(ip, "9.0", 0) == nullptr) return TCL_ERROR;
    Tcl_CreateNamespace(ip, "::lunar", nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::engine_start", EngineStart_Cmd, nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::syncnow",      SyncNow_Cmd,     nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::shutdown",     Shutdown_Cmd,    nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::status",       Status_Cmd,      nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::sources",      Sources_Cmd,     nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::update_status", Update_Cmd,     nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::localtime",    LocalTime_Cmd,   nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::tz_list",      TzList_Cmd,      nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::tz_version",   TzVersion_Cmd,   nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::tz_suggest",   TzSuggest_Cmd,   nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::tray_add",     TrayAdd_Cmd,     nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::tray_tip",     TrayTip_Cmd,     nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::tray_remove",  TrayRemove_Cmd,  nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::run_at_startup", RunAtStartup_Cmd, nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::log_text",     LogText_Cmd,     nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::about",        About_Cmd,       nullptr, nullptr);
    Tcl_CreateObjCommand(ip, "::lunar::beep",         Beep_Cmd,        nullptr, nullptr);
    if (Tcl_PkgProvide(ip, "lunarx", "0.1") != TCL_OK) return TCL_ERROR;
    return TCL_OK;
}
