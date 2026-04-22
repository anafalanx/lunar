// tz.c -- IANA time-zone resolver.  Thin wrapper over the embedded
// tzdata tables emitted by scripts/gen_tz_embed.py and the TZif v2/v3
// parser in tzif.c.

#include "tz.h"

#include <stdint.h>
#include <string.h>

// ---- Symbols published by tz_embed.c (the generated blob) ----
extern const char   *g_tz_version;
extern const unsigned g_tz_name_count;
extern const unsigned g_tz_blob_count;
extern const char * const g_tz_names[];
extern const uint32_t g_tz_blob_index[];
extern const uint32_t g_tz_blob_offset[];
extern const uint32_t g_tz_blob_length[];
extern const uint8_t  g_tz_blob[];

const char *Tz_Version(void) { return g_tz_version; }

int Tz_Count(void) { return (int)g_tz_name_count; }

const char *Tz_AtIndex(int index) {
    if (index < 0 || (unsigned)index >= g_tz_name_count) return NULL;
    return g_tz_names[index];
}

const char *Tz_Name(TzId id) {
    if (id < 0 || (unsigned)id >= g_tz_name_count) return NULL;
    return g_tz_names[id];
}

// The table is sorted (UTC first, then locale-independent ASCII
// lexicographic ascending), so a binary search is enough.  Callers
// that request rare aliases will miss; Lunar only ever surfaces names
// from `g_tz_names` itself so this is fine.
TzId Tz_FindByName(const char *iana) {
    if (!iana || !*iana) return TZ_ID_INVALID;
    // UTC is always index 0 (see gen_tz_embed.py).
    if (strcmp(iana, "UTC") == 0) return TZ_ID_UTC;

    int lo = 1, hi = (int)g_tz_name_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int cmp = strcmp(iana, g_tz_names[mid]);
        if (cmp == 0) return (TzId)mid;
        if (cmp < 0)  hi = mid - 1;
        else          lo = mid + 1;
    }
    return TZ_ID_INVALID;
}

int Tz_LocalFromUtcMs(TzId id, int64_t utcMs, TzifLocal *out) {
    if (!out) return 0;
    if (id < 0 || (unsigned)id >= g_tz_name_count) return 0;

    uint32_t bidx   = g_tz_blob_index[id];
    if (bidx >= g_tz_blob_count) return 0;
    uint32_t offset = g_tz_blob_offset[bidx];
    uint32_t length = g_tz_blob_length[bidx];

    int64_t utcSec = utcMs / 1000;
    if (utcMs < 0 && utcMs % 1000 != 0) utcSec -= 1;    // floor

    return tzif_resolve(g_tz_blob + offset, (size_t)length, utcSec, out);
}
