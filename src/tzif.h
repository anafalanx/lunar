// tzif.h -- tiny TZif v2/v3 parser (RFC 8536) used by Lunar's
// OS-independent timezone resolver.
//
// Given a raw TZif blob (as emitted by `zic` and embedded by
// scripts/gen_tz_embed.py) and a UTC moment in seconds since the Unix
// epoch, tzif_resolve() returns the local wall-clock breakdown plus
// the timezone abbreviation and UTC offset that applied at that
// moment.  No OS timezone machinery is consulted.
//
// The parser supports the 64-bit (version >= '2') data block only.
// Version '1' blobs that pre-date the 64-bit table are rejected, as
// modern tzdata always writes at least version 2.  The POSIX TZ footer
// string IS parsed and is LOAD-BEARING: the embedded blobs are slim
// (transition tables end around the build year), so every recurring
// future DST transition comes from evaluating the footer rule. See
// tzif.c's footer section.
//
// All fields in `TzifLocal` use the familiar conventions:
//     month  1..12
//     mday   1..31
//     wday   0..6   (Sun=0)
//     yday   0..365
//     utcOffsetSec east-of-UTC (+3600 for CET, -18000 for EST, ...)
//
// abbr is ASCII, NUL-terminated, at most TZIF_ABBR_CAP - 1 chars.

#ifndef LUNAR_TZIF_H
#define LUNAR_TZIF_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TZIF_ABBR_CAP 8

typedef struct {
    int year;          // full year, e.g. 2026
    int month;         // 1..12
    int mday;          // 1..31
    int hour;          // 0..23
    int minute;        // 0..59
    int second;        // 0..60
    int wday;          // 0=Sun
    int yday;          // 0..365
    int isDst;
    int utcOffsetSec;
    char abbr[TZIF_ABBR_CAP];
} TzifLocal;

// Returns 1 on success, 0 on malformed blob or out-of-range input.
int tzif_resolve(const uint8_t *blob, size_t blobLen,
                 int64_t utcSec, TzifLocal *out);

#ifdef __cplusplus
}
#endif
#endif
