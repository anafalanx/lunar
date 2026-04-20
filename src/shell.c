// shell.c -- Ultra-minimal Win32 chrome for Lunar. No status bar, no toolbar.
// All commands live in the system menu (click the window icon in the title
// bar, or right-click the title bar). Timezone abbreviation is shown in the
// window title before the app name.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "shell.h"

// System-menu command IDs. Must be in the range 1..0xEFFF (>=0xF000 is
// reserved for standard system commands like SC_CLOSE).
#define IDM_ALWAYS_ON_TOP 0x1001
#define IDM_TEST_BEEP     0x1002
#define IDM_SETTINGS      0x1003
#define IDM_ABOUT         0x1004

static HWND        g_hwnd;
static ShellState *g_state;
static WNDPROC     g_originalProc;
static char        g_tzLabel[32] = "";   // "RDT (UTC+2)" etc.

static void UpdateTitleBar(void) {
    if (!g_hwnd) return;
    WCHAR title[160];
    if (g_tzLabel[0]) {
        WCHAR wtz[32];
        MultiByteToWideChar(CP_UTF8, 0, g_tzLabel, -1, wtz, 32);
        _snwprintf(title, 160, L"%ls  \x2014  Lunar 0.2", wtz);
    } else {
        wcscpy(title, L"Lunar 0.2");
    }
    SetWindowTextW(g_hwnd, title);
}

static void SyncAlwaysOnTopCheck(void) {
    if (!g_hwnd) return;
    HMENU sys = GetSystemMenu(g_hwnd, FALSE);
    if (!sys) return;
    CheckMenuItem(sys, IDM_ALWAYS_ON_TOP,
                  MF_BYCOMMAND |
                  (g_state && g_state->alwaysOnTop ? MF_CHECKED : MF_UNCHECKED));
}

static void InstallSystemMenuItems(void) {
    HMENU sys = GetSystemMenu(g_hwnd, FALSE);
    if (!sys) return;
    // Insert our items at the top, with a separator between ours and the
    // standard Restore/Move/Size/.../Close block.
    InsertMenuW(sys, 0, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenuW(sys, 0, MF_BYPOSITION | MF_STRING, IDM_ABOUT,         L"&About Lunar");
    InsertMenuW(sys, 0, MF_BYPOSITION | MF_STRING, IDM_SETTINGS,      L"&Settings\x2026");
    InsertMenuW(sys, 0, MF_BYPOSITION | MF_STRING, IDM_TEST_BEEP,     L"&Test beep");
    InsertMenuW(sys, 0, MF_BYPOSITION | MF_STRING, IDM_ALWAYS_ON_TOP, L"Always on &top");
    SyncAlwaysOnTopCheck();
}

static LRESULT CALLBACK SubclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if (g_state) Shell_Layout(g_state);
        break;

    case WM_SYSCOMMAND: {
        UINT raw = (UINT)wp;
        switch (raw) {
        case IDM_ALWAYS_ON_TOP:
            if (g_state) {
                g_state->alwaysOnTop = !g_state->alwaysOnTop;
                Shell_ApplyAlwaysOnTop(g_state->alwaysOnTop);
                SyncAlwaysOnTopCheck();
            }
            return 0;
        case IDM_TEST_BEEP:
            if (g_state) g_state->bellTestRequested = 1;
            return 0;
        case IDM_SETTINGS:
            if (g_state) g_state->settingsRequested = 1;
            return 0;
        case IDM_ABOUT:
            Shell_ShowAbout();
            return 0;
        }
        break;
    }
    }
    return CallWindowProcW(g_originalProc, h, msg, wp, lp);
}

void Shell_Init(void *hwndVoid, ShellState *state) {
    g_hwnd  = (HWND)hwndVoid;
    g_state = state;

    g_originalProc = (WNDPROC)SetWindowLongPtrW(
        g_hwnd, GWLP_WNDPROC, (LONG_PTR)SubclassProc);

    HICON ico = (HICON)LoadImageW(GetModuleHandleW(NULL),
                                  MAKEINTRESOURCEW(1),
                                  IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    if (ico) {
        SendMessageW(g_hwnd, WM_SETICON, ICON_BIG,   (LPARAM)ico);
        SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)ico);
    }

    InstallSystemMenuItems();
    Shell_UpdateTimezone();
    Shell_Layout(state);
}

void Shell_Layout(ShellState *state) {
    if (!g_hwnd || !state) return;
    RECT rcCli;
    GetClientRect(g_hwnd, &rcCli);
    state->clientW    = rcCli.right;
    state->clientH    = rcCli.bottom;
    state->statusbarH = 0;
    state->dialW      = state->clientW;
    state->dialH      = state->clientH;
    if (state->dialH < 1) state->dialH = 1;
}

void Shell_UpdateTimezone(void) {
    TIME_ZONE_INFORMATION tzi;
    DWORD tzId = GetTimeZoneInformation(&tzi);
    LONG biasMin = tzi.Bias + ((tzId == TIME_ZONE_ID_DAYLIGHT) ?
                               tzi.DaylightBias : tzi.StandardBias);
    int utcOffsetH = -(int)biasMin / 60;

    const WCHAR *nameW = (tzId == TIME_ZONE_ID_DAYLIGHT)
                          ? tzi.DaylightName : tzi.StandardName;
    char name[64] = {0};
    WideCharToMultiByte(CP_UTF8, 0, nameW, -1, name, sizeof(name) - 1, NULL, NULL);

    // Abbreviate multi-word names ("Central European Summer Time" -> "CEST").
    char abbr[16] = {0};
    int ai = 0;
    int atStart = 1;
    for (int i = 0; name[i] && ai < (int)sizeof(abbr) - 1; i++) {
        if (name[i] == ' ' || name[i] == '\t') { atStart = 1; continue; }
        if (atStart) { abbr[ai++] = name[i]; atStart = 0; }
    }
    if (ai < 2) {
        size_t n = strlen(name);
        if (n >= sizeof(abbr)) n = sizeof(abbr) - 1;
        memcpy(abbr, name, n);
        abbr[n] = '\0';
    }

    snprintf(g_tzLabel, sizeof(g_tzLabel), "%s (UTC%+d)", abbr, utcOffsetH);
    UpdateTitleBar();
}

void Shell_ApplyAlwaysOnTop(int on) {
    if (!g_hwnd) return;
    SetWindowPos(g_hwnd,
                 on ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void Shell_ShowAbout(void) {
    MessageBoxA(g_hwnd,
                "Lunar 0.2.0\n\n"
                "A minimalist analog clock.\n"
                "Native Win32 + raylib.",
                "About Lunar",
                MB_ICONINFORMATION | MB_OK);
}

int Shell_ShowSettings(ShellState *state) {
    (void)state;
    MessageBoxA(g_hwnd, "Settings dialog coming soon.", "Lunar",
                MB_ICONINFORMATION);
    return 0;
}

void Shell_PumpMessages(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
