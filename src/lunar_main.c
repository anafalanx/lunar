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
#include <tchar.h>

#if defined(__GNUC__)
int _CRT_glob = 0;          /* keep the mingw CRT from glob-expanding argv */
#endif

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
