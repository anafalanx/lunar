// tz.h -- IANA time-zone resolver.  Fully OS-independent: all zone
// data is embedded at build time from tzdata by gen_tz_embed.py.
//
// Two layers:
//
//   Index layer (this header):
//     Zone names and blob lookup tables live in tz_embed.c.
//     Tz_FindByName()   resolves an IANA string to a small integer ID.
//     Tz_Name(id)       returns the canonical IANA string.
//     Tz_Count()        number of zones (including UTC at index 0).
//     Tz_AtIndex(i)     the i-th canonical zone.
//
//   Resolution layer:
//     Tz_LocalFromUtcMs(id, utcMs, &out) breaks a UTC millisecond
//     instant down into local wall-clock components for the given
//     zone.  Does not consult the OS.
//
// The zero-valued TzId (TZ_ID_UTC) is always valid and always names
// "UTC", even on builds where tz_embed.c might be minimal.

#ifndef LUNAR_TZ_H
#define LUNAR_TZ_H

#include <stdint.h>
#include <stddef.h>
#include "tzif.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t TzId;
#define TZ_ID_INVALID ((TzId)-1)
#define TZ_ID_UTC     ((TzId) 0)

// Returns the tzdata release string ("2026a", "unknown", ...).
const char *Tz_Version(void);

// Returns number of canonical zone names, always >= 1 (UTC).
int Tz_Count(void);

// Returns the IANA name at a given canonical index, or NULL if OOR.
// Index 0 is always "UTC".
const char *Tz_AtIndex(int index);

// Returns the canonical name for an id, or NULL if invalid.
const char *Tz_Name(TzId id);

// Case-sensitive lookup of an IANA string.  Returns TZ_ID_INVALID if
// not present in the embedded index.
TzId Tz_FindByName(const char *iana);

// Resolve a UTC millisecond moment to local components in the given
// zone.  Returns 1 on success, 0 on invalid id or malformed data.
int Tz_LocalFromUtcMs(TzId id, int64_t utcMs, TzifLocal *out);

#ifdef __cplusplus
}
#endif
#endif
