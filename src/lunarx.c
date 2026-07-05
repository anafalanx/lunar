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
#include <tcl.h>

#include <string.h>

#include "clock.h"
#include "ntp.h"
#include "update_check.h"
#include "tz.h"
#include "tz_winmap.h"

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

/* lunar::shutdown -- drain workers + persist rate before the process exits. */
static int Shutdown_Cmd([[maybe_unused]] void *cd, Tcl_Interp *ip,
                        int objc, Tcl_Obj *const objv[]) {
    if (objc != 1) { Tcl_WrongNumArgs(ip, 1, objv, ""); return TCL_ERROR; }
    Ntp_Shutdown();
    Clock_Shutdown();
    Tcl_SetObjResult(ip, Tcl_NewObj());
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
    if (Tcl_PkgProvide(ip, "lunarx", "0.1") != TCL_OK) return TCL_ERROR;
    return TCL_OK;
}
