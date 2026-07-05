/*
 * lunar_main.c -- custom Windows entry point for Lunar's Tcl/Tk shell.
 *
 * A minimal fork of Tk's win/winMain.c (the source of wish90s.exe): a GUI
 * (no-console) WinMain that lets TclZipfs_AppHook self-mount the zipfs
 * archive appended to THIS executable -- which carries main.tcl (= lunar.tcl),
 * the Tcl and Tk script libraries, and resources/ -- then hands off to
 * Tk_Main, which runs main.tcl as the startup script. Lunar's C engine is
 * exposed to the Tcl side by the statically-linked lunarx extension,
 * registered in the app-init below; there are no loadable DLLs.
 *
 * Build requirements (mirrors els; see docs):
 *   -DUNICODE -D_UNICODE -municode  : TclZipfs_AppHook uses the WCHAR
 *                                     signature; without UNICODE the
 *                                     self-mount silently no-ops and
 *                                     main.tcl is never found.
 *   -DSTATIC_BUILD=1                : tcl.h/tk.h must not mark symbols
 *                                     dllimport.
 *   USE_TCL_STUBS undefined         : with stubs, Tk_Main becomes a TIP-596
 *                                     thunk that LoadLibrary's tcl90.dll and
 *                                     crashes in a static exe with no such DLL.
 *   -mwindows                       : GUI subsystem (no console window).
 * Define LUNAR_STATIC_LUNARX (and link lunarx.o + the engine objects) to
 * compile the engine's Tcl command surface straight in.
 */

#undef USE_TCL_STUBS
#include "tk.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN
#include <locale.h>
#include <stdlib.h>
#include <stdio.h>
#include <tchar.h>

#include "app_paths.h"   /* Lunar_AppDataPathW */
#include "logbuf.h"      /* Log_Append, Log_FlushToDisk */

#if defined(__GNUC__)
int _CRT_glob = 0;          /* keep the mingw CRT from glob-expanding argv */
#endif

/* ---- crash minidump (ported from the Win32 shell's CrashHandler) --------
 * A native C fault (in the engine or the Tcl/Tk core) otherwise vanishes
 * into WerFault, taking the 24 h in-memory event ring with it. This last-
 * resort filter writes %APPDATA%\Lunar\crash\lunar-<pid>-<tick>.dmp (a
 * MiniDumpNormal snapshot) plus a snapshot of the event log, then lets the
 * process die. dbghelp is loaded inside the handler; every step is optional
 * and failure just falls through to process death. Installed in _tWinMain
 * before Tk_Main. Complements the Tcl-level bgerror routing, which cannot
 * see a C fault. */
typedef BOOL (WINAPI *MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, int,
                                           void *, void *, void *);

typedef struct {
    MiniDumpWriteDumpFn fn;
    HANDLE              file;
    DWORD               pid;
    BOOL                ok;
    DWORD               err;
} LunarDumpCtx;

/* Write the dump from a dedicated thread. MiniDumpNormal captures every
 * thread's stack (including the faulting one). We deliberately do NOT pass a
 * MINIDUMP_EXCEPTION_INFORMATION stream: threading the caller's exception
 * pointers through a helper thread trips ERROR_NOACCESS in dbghelp here,
 * whereas a plain all-threads dump succeeds -- and the exception CODE is
 * already recorded in the event-log snapshot written alongside. */
static DWORD WINAPI Lunar_DumpThread(LPVOID arg) {
    LunarDumpCtx *c = (LunarDumpCtx *)arg;
    HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                            FALSE, c->pid);
    c->ok  = c->fn(hp ? hp : GetCurrentProcess(), c->pid, c->file,
                   0 /* MiniDumpNormal */, NULL, NULL, NULL);
    c->err = GetLastError();
    if (hp) CloseHandle(hp);
    return 0;
}

static LONG WINAPI Lunar_CrashHandler(EXCEPTION_POINTERS *ep) {
    wchar_t dir[MAX_PATH];
    if (Lunar_AppDataPathW(dir, MAX_PATH, L"crash")) {
        CreateDirectoryW(dir, NULL);   /* ok if it already exists */

        DWORD pid  = GetCurrentProcessId();
        DWORD tick = GetTickCount();
        wchar_t path[MAX_PATH];

        _snwprintf_s(path, MAX_PATH, _TRUNCATE,
                     L"%ls\\lunar-%lu-%lu.dmp", dir,
                     (unsigned long)pid, (unsigned long)tick);
        HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
        if (dbghelp) {
            MiniDumpWriteDumpFn writeDump =
                (MiniDumpWriteDumpFn)(void (*)(void))GetProcAddress(
                    dbghelp, "MiniDumpWriteDump");
            if (writeDump) {
                HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                       NULL);
                if (f != INVALID_HANDLE_VALUE) {
                    LunarDumpCtx c = { writeDump, f, pid, FALSE, 0 };
                    HANDLE th = CreateThread(NULL, 0, Lunar_DumpThread, &c, 0, NULL);
                    if (th) { WaitForSingleObject(th, INFINITE); CloseHandle(th); }
                    LARGE_INTEGER sz = {0}; GetFileSizeEx(f, &sz);
                    CloseHandle(f);
                    Log_Append("app: crash dump %s (%lld bytes)",
                               c.ok ? "written" : "failed", (long long)sz.QuadPart);
                }
            }
        }

        wchar_t leaf[64];
        _snwprintf_s(leaf, 64, _TRUNCATE, L"crash\\lunar-%lu-%lu.log",
                     (unsigned long)pid, (unsigned long)tick);
        Log_Append("app: unhandled exception 0x%08lX; writing crash dump",
                   ep && ep->ExceptionRecord
                       ? (unsigned long)ep->ExceptionRecord->ExceptionCode
                       : 0UL);
        Log_FlushToDisk(leaf);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

#ifdef LUNAR_STATIC_LUNARX
extern int Lunarx_Init(Tcl_Interp *interp);   /* src/lunarx.c -- ::lunar::* commands */
#endif

static int Lunar_AppInit(Tcl_Interp *interp);

/*
 * _tWinMain -- entry point from Windows (GUI subsystem). Mirrors winMain.c
 * but with the console path removed: Lunar is a clock, never a shell.
 */
int APIENTRY
_tWinMain(
    HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    LPTSTR lpszCmdLine,
    int nCmdShow)
{
    TCHAR **argv;
    int argc;
    TCHAR *p;
    (void) hInstance;
    (void) hPrevInstance;
    (void) lpszCmdLine;
    (void) nCmdShow;

    /* Last-resort crash dump before anything else can fault. */
    SetUnhandledExceptionFilter(Lunar_CrashHandler);

    /* Standard "C" locale so Tcl parses numbers/paths predictably. */
    setlocale(LC_ALL, "C");

    argc = __argc;
    argv = __targv;

    for (p = argv[0]; *p != '\0'; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }

#if defined(UNICODE)
    TclZipfs_AppHook(&argc, &argv);
#endif

    Tk_Main(argc, argv, Lunar_AppInit);
    return 0;                   /* Tk_Main does not return; silences a warning. */
}

static int
Lunar_AppInit(
    Tcl_Interp *interp)
{
    if (Tcl_Init(interp) == TCL_ERROR) {
        return TCL_ERROR;
    }
    if (Tk_Init(interp) == TCL_ERROR) {
        return TCL_ERROR;
    }
    Tcl_StaticLibrary(interp, "Tk", Tk_Init, Tk_SafeInit);

#ifdef LUNAR_STATIC_LUNARX
    if (Lunarx_Init(interp) == TCL_ERROR) {
        return TCL_ERROR;
    }
    Tcl_StaticLibrary(interp, "Lunarx", Lunarx_Init, NULL);
#endif

    return TCL_OK;
}
