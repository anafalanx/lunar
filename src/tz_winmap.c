// tz_winmap.c -- lookup + OS-zone reader for the Windows->IANA table.
// The table itself lives in the generated tz_winmap_gen.c.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>

#include "tz_winmap.h"

int TzWinmap_Count(void) { return g_tz_winmap_count; }

const char *TzWinmap_IanaFromWindows(const wchar_t *winKey) {
    if (!winKey) return nullptr;
    int lo = 0, hi = g_tz_winmap_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int c = wcscmp(winKey, g_tz_winmap[mid].win);
        if (c == 0)      return g_tz_winmap[mid].iana;
        else if (c < 0)  hi = mid - 1;
        else             lo = mid + 1;
    }
    return nullptr;
}

int TzWinmap_CurrentIana(char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = 0;

    // GetDynamicTimeZoneInformation gives the registry key name
    // ("W. Europe Standard Time") directly, without the localization
    // that GetTimeZoneInformation's names carry. This reads the zone
    // NAME only -- never the current time.
    DYNAMIC_TIME_ZONE_INFORMATION dtzi;
    DWORD r = GetDynamicTimeZoneInformation(&dtzi);
    if (r == TIME_ZONE_ID_INVALID) return 0;
    if (dtzi.TimeZoneKeyName[0] == 0) return 0;

    const char *iana = TzWinmap_IanaFromWindows(dtzi.TimeZoneKeyName);
    if (!iana) return 0;

    size_t n = strlen(iana);
    if (n + 1 > cap) return 0;
    memcpy(out, iana, n + 1);
    return 1;
}
