// tz_winmap.h -- map the OS's Windows time-zone key to an IANA name.
//
// Reading the *name* of the user's zone is not the same as trusting the
// OS *clock*: Lunar still disciplines time from the network. This exists
// only so a fresh install can pre-select the user's likely display zone
// instead of silently defaulting to UTC. The table (tz_winmap_gen.c) is
// generated from CLDR windowsZones, filtered to zones Lunar embeds.

#ifndef LUNAR_TZ_WINMAP_H
#define LUNAR_TZ_WINMAP_H

#include <stddef.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const wchar_t *win;    // Windows time-zone key ("W. Europe Standard Time")
    const char    *iana;   // canonical IANA name present in Lunar's tz index
} TzWinMapEntry;

// Generated table (sorted by `win`, ASCII) and its length.
extern const TzWinMapEntry g_tz_winmap[];
extern const int           g_tz_winmap_count;

// Map a Windows time-zone key to an IANA name, or nullptr if unknown.
// Binary search over the generated table; exact (case-sensitive) match,
// which is correct because Windows keys are canonical.
const char *TzWinmap_IanaFromWindows(const wchar_t *winKey);

// Read the machine's current Windows time-zone key and map it to an
// IANA name, writing it (NUL-terminated) into out. Returns 1 on success,
// 0 if the zone can't be read or has no embedded mapping. Consults only
// the zone NAME via GetDynamicTimeZoneInformation, never the OS clock.
int TzWinmap_CurrentIana(char *out, size_t cap);

// Entry count, for tests.
int TzWinmap_Count(void);

#ifdef __cplusplus
}
#endif

#endif
