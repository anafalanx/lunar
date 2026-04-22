// tzif.c -- TZif v2/v3 parser with POSIX TZ footer support.
//
// Modern zic emits "slim" TZif files: the explicit transition table
// covers only historical dates (typically up to ~2007) and future
// transitions must be computed from the POSIX TZ string stored in the
// file's footer.  This parser honours the footer so zones still return
// the correct DST state for "now" without consulting the OS.
//
// Reference: RFC 8536 (TZif) + IEEE Std 1003.1 (POSIX TZ).

#include "tzif.h"

#include <string.h>

// ---------------------------------------------------------------------
// Big-endian readers.
// ---------------------------------------------------------------------

static uint32_t rd_u32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}
static int32_t rd_i32be(const uint8_t *p) {
    return (int32_t)rd_u32be(p);
}
static int64_t rd_i64be(const uint8_t *p) {
    uint64_t hi = rd_u32be(p);
    uint64_t lo = rd_u32be(p + 4);
    return (int64_t)((hi << 32) | lo);
}

// ---------------------------------------------------------------------
// Calendar helpers.  No OS calls; pure arithmetic.
// ---------------------------------------------------------------------

static int is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static const int kMdays[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };

// Days since 1970-01-01 for Jan 1 of year `y` (y >= 1).  Uses the
// Gregorian leap rule throughout (proleptic).
static int64_t days_before_year(int y) {
    int64_t yy = (int64_t)y - 1;
    int64_t leaps    = yy   / 4 - yy   / 100 + yy   / 400;
    int64_t leaps69  = 1969 / 4 - 1969 / 100 + 1969 / 400;
    return yy * 365 + leaps - (1969 * 365 + leaps69);
}

// Break down a seconds-since-1970-01-01 value (already shifted by the
// zone's UTC offset) into calendar components.
static void breakdown(int64_t localSec, TzifLocal *out) {
    int64_t days = localSec / 86400;
    int64_t rem  = localSec % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }
    int sod = (int)rem;

    out->hour   = sod / 3600;
    out->minute = (sod / 60) % 60;
    out->second = sod % 60;

    int w = (int)((days % 7 + 4) % 7);    // 1970-01-01 was Thursday (4)
    if (w < 0) w += 7;
    out->wday = w;

    // Shift epoch to 2000-03-01 (start of 400-year cycle).
    int64_t d = days - 11017;
    int64_t era = (d >= 0 ? d : d - 146096) / 146097;
    unsigned doe = (unsigned)(d - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)(yoe) + (int)(era) * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp  = (5 * doy + 2) / 153;
    unsigned d_  = doy - (153 * mp + 2) / 5 + 1;
    unsigned m   = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2 ? 1 : 0);
    y += 2000;

    out->year  = y;
    out->month = (int)m;
    out->mday  = (int)d_;

    int leap = is_leap(y);
    int yd = 0;
    for (int i = 1; i < (int)m; i++) {
        yd += kMdays[i - 1];
        if (i == 2 && leap) yd += 1;
    }
    yd += (int)d_ - 1;
    out->yday = yd;
}

// ---------------------------------------------------------------------
// POSIX TZ footer parser.
// ---------------------------------------------------------------------

typedef struct {
    int mode;       // 0=Mm.w.d, 1=Jn (no Feb 29), 2=n (with Feb 29)
    int m, w, d;    // for mode 0
    int n;          // for mode 1/2
    int timeSec;    // local time-of-day, default 02:00:00
} PosixRule;

typedef struct {
    int   hasDst;
    int   hasRules;
    int   stdOff;               // east-of-UTC seconds
    int   dstOff;               // east-of-UTC seconds
    char  stdAbbr[TZIF_ABBR_CAP];
    char  dstAbbr[TZIF_ABBR_CAP];
    PosixRule startRule;        // std->dst
    PosixRule endRule;          // dst->std
} PosixTz;

static int is_alpha_c(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}
static int is_digit_c(int c) { return c >= '0' && c <= '9'; }

static const char *parse_abbr(const char *s, char abbr[TZIF_ABBR_CAP]) {
    int k = 0;
    if (*s == '<') {
        s++;
        while (*s && *s != '>' && k + 1 < TZIF_ABBR_CAP) abbr[k++] = *s++;
        if (*s == '>') s++;
    } else {
        while (is_alpha_c((unsigned char)*s) && k + 1 < TZIF_ABBR_CAP)
            abbr[k++] = *s++;
    }
    abbr[k] = 0;
    return s;
}

// Consume up to `maxDigits` ASCII digits, accumulating into *v with
// saturation at INT_MAX.  Caps runaway inputs like "M99999...9" so a
// malformed or hostile footer can neither overflow nor burn CPU.
static const char *take_digits(const char *s, int *v, int maxDigits) {
    int x = 0;
    int k = 0;
    while (is_digit_c((unsigned char)*s) && k < maxDigits) {
        if (x > 214748364) x = 2147483647;    // saturate near INT_MAX
        else x = x * 10 + (*s - '0');
        s++;
        k++;
    }
    // Skip any excess digits without incorporating them so callers
    // still advance past the full numeric token.
    while (is_digit_c((unsigned char)*s)) s++;
    *v = x;
    return s;
}

// POSIX offsets are "time to add to local to get UTC" -- the sign is
// the opposite of gmtoff.  Caller negates to get east-of-UTC.
static const char *parse_posix_offset(const char *s, int *rawSec) {
    int sign = 1;
    if (*s == '+') s++;
    else if (*s == '-') { sign = -1; s++; }
    int hh = 0, mm = 0, ss = 0;
    s = take_digits(s, &hh, 3);     // POSIX: max 3 hour digits
    if (*s == ':') {
        s++;
        s = take_digits(s, &mm, 2);
    }
    if (*s == ':') {
        s++;
        s = take_digits(s, &ss, 2);
    }
    if (hh > 24)  hh = 24;
    if (mm > 59)  mm = 59;
    if (ss > 59)  ss = 59;
    *rawSec = sign * (hh * 3600 + mm * 60 + ss);
    return s;
}

static const char *parse_rule(const char *s, PosixRule *r) {
    r->mode = 0; r->m = r->w = r->d = r->n = 0;
    r->timeSec = 2 * 3600;
    if (*s == 'M') {
        s++; r->mode = 0;
        s = take_digits(s, &r->m, 2);
        if (*s == '.') s++;
        s = take_digits(s, &r->w, 1);
        if (*s == '.') s++;
        s = take_digits(s, &r->d, 1);
        if (r->m < 1 || r->m > 12) r->m = 1;
        if (r->w < 1 || r->w > 5)  r->w = 1;
        if (r->d < 0 || r->d > 6)  r->d = 0;
    } else if (*s == 'J') {
        s++; r->mode = 1;
        s = take_digits(s, &r->n, 3);
        if (r->n < 1)   r->n = 1;
        if (r->n > 365) r->n = 365;
    } else {
        r->mode = 2;
        s = take_digits(s, &r->n, 3);
        if (r->n < 0)   r->n = 0;
        if (r->n > 365) r->n = 365;
    }
    if (*s == '/') {
        s++;
        int t = 0;
        s = parse_posix_offset(s, &t);
        r->timeSec = t;
    }
    return s;
}

static int parse_posix(const char *s, PosixTz *p) {
    memset(p, 0, sizeof(*p));
    if (!s || !*s) return 0;
    int stdRaw = 0;
    s = parse_abbr(s, p->stdAbbr);
    if (!p->stdAbbr[0]) return 0;
    s = parse_posix_offset(s, &stdRaw);
    p->stdOff = -stdRaw;
    if (*s == 0) { p->hasDst = 0; return 1; }

    s = parse_abbr(s, p->dstAbbr);
    if (!p->dstAbbr[0]) return 0;
    p->hasDst = 1;
    if (*s == ',' || *s == 0) {
        p->dstOff = p->stdOff + 3600;    // default: +1 hour
    } else {
        int dstRaw = 0;
        s = parse_posix_offset(s, &dstRaw);
        p->dstOff = -dstRaw;
    }
    if (*s != ',') return 0;
    s++;
    s = parse_rule(s, &p->startRule);
    if (*s != ',') return 0;
    s++;
    s = parse_rule(s, &p->endRule);
    p->hasRules = 1;
    return 1;
}

// Local-wall seconds (in the offset implied by the caller) for a rule
// in year Y.  Caller subtracts the appropriate offset to reach UTC.
static int64_t rule_local_sec(const PosixRule *r, int Y) {
    int64_t yearDays = days_before_year(Y);
    int leap = is_leap(Y);
    int doy = 0;

    if (r->mode == 0) {
        int doyFirst = 0;
        for (int i = 1; i < r->m; i++) {
            doyFirst += kMdays[i - 1];
            if (i == 2 && leap) doyFirst += 1;
        }
        int64_t epochDays = yearDays + doyFirst;
        int dow = (int)((epochDays % 7 + 4) % 7);
        if (dow < 0) dow += 7;
        int firstOccur = (r->d - dow + 7) % 7;
        int dayNum = firstOccur + (r->w - 1) * 7;
        int dim = kMdays[r->m - 1];
        if (r->m == 2 && leap) dim += 1;
        if (dayNum >= dim) dayNum -= 7;   // clamp (especially w == 5)
        doy = doyFirst + dayNum;
    } else if (r->mode == 1) {
        int n = r->n;
        if (n < 1) n = 1;
        if (n > 365) n = 365;
        if (leap && n > 59) doy = n;      // Jn skips Feb 29
        else                doy = n - 1;
    } else {
        int n = r->n;
        if (n < 0) n = 0;
        if (n > 365) n = 365;
        doy = n;
    }
    return (yearDays + doy) * 86400 + r->timeSec;
}

static int posix_resolve(const PosixTz *p, int64_t utcSec,
                         int *gmtoff, int *isdst,
                         const char **abbr) {
    if (!p->hasDst) {
        *gmtoff = p->stdOff;
        *isdst  = 0;
        *abbr   = p->stdAbbr;
        return 1;
    }
    if (!p->hasRules) return 0;

    TzifLocal tmp;
    breakdown(utcSec, &tmp);
    int Y = tmp.year;

    int64_t cands[6];
    int     cDst [6];
    int     n = 0;
    for (int dy = -1; dy <= 1; dy++) {
        int y = Y + dy;
        int64_t sLocal = rule_local_sec(&p->startRule, y);
        int64_t eLocal = rule_local_sec(&p->endRule,   y);
        cands[n] = sLocal - p->stdOff; cDst[n] = 1; n++;
        cands[n] = eLocal - p->dstOff; cDst[n] = 0; n++;
    }

    int64_t bestT = 0;
    int     bestDst = 0;
    int     found = 0;
    for (int i = 0; i < n; i++) {
        if (cands[i] <= utcSec) {
            if (!found || cands[i] > bestT) {
                bestT = cands[i];
                bestDst = cDst[i];
                found = 1;
            }
        }
    }
    if (!found) {
        *gmtoff = p->stdOff;
        *isdst  = 0;
        *abbr   = p->stdAbbr;
        return 1;
    }
    if (bestDst) {
        *gmtoff = p->dstOff;
        *isdst  = 1;
        *abbr   = p->dstAbbr;
    } else {
        *gmtoff = p->stdOff;
        *isdst  = 0;
        *abbr   = p->stdAbbr;
    }
    return 1;
}

// ---------------------------------------------------------------------
// Main entry point.
// ---------------------------------------------------------------------

int tzif_resolve(const uint8_t *blob, size_t blobLen,
                 int64_t utcSec, TzifLocal *out) {
    if (!blob || !out || blobLen < 44) return 0;
    if (blob[0] != 'T' || blob[1] != 'Z' || blob[2] != 'i' || blob[3] != 'f')
        return 0;
    char ver = (char)blob[4];
    if (ver != '2' && ver != '3' && ver != '4') return 0;

    // ---- Skip v1 data block. ----
    const uint8_t *h1 = blob + 20;
    uint32_t tti_ut = rd_u32be(h1 +  0);
    uint32_t tti_st = rd_u32be(h1 +  4);
    uint32_t leap   = rd_u32be(h1 +  8);
    uint32_t tcnt   = rd_u32be(h1 + 12);
    uint32_t typec  = rd_u32be(h1 + 16);
    uint32_t charc  = rd_u32be(h1 + 20);

    size_t v1_size = (size_t)tcnt * 4
                   + (size_t)tcnt * 1
                   + (size_t)typec * 6
                   + (size_t)charc
                   + (size_t)leap * 8
                   + (size_t)tti_st
                   + (size_t)tti_ut;
    size_t v1_end = 44 + v1_size;
    if (v1_end + 44 > blobLen) return 0;

    // ---- V2 header. ----
    const uint8_t *h2 = blob + v1_end;
    if (h2[0] != 'T' || h2[1] != 'Z' || h2[2] != 'i' || h2[3] != 'f')
        return 0;
    tti_ut = rd_u32be(h2 + 20 +  0);
    tti_st = rd_u32be(h2 + 20 +  4);
    leap   = rd_u32be(h2 + 20 +  8);
    tcnt   = rd_u32be(h2 + 20 + 12);
    typec  = rd_u32be(h2 + 20 + 16);
    charc  = rd_u32be(h2 + 20 + 20);

    size_t need2 = 44
                 + (size_t)tcnt  * 8
                 + (size_t)tcnt  * 1
                 + (size_t)typec * 6
                 + (size_t)charc
                 + (size_t)leap  * 12
                 + (size_t)tti_st
                 + (size_t)tti_ut;
    if (v1_end + need2 > blobLen) return 0;
    if (typec == 0) return 0;

    const uint8_t *p       = h2 + 44;
    const uint8_t *ptrans  = p;
    const uint8_t *ptypes  = ptrans  + (size_t)tcnt * 8;
    const uint8_t *pttinfo = ptypes  + (size_t)tcnt;
    const uint8_t *pchars  = pttinfo + (size_t)typec * 6;

    // ---- POSIX TZ footer (delimited by newlines). ----
    PosixTz pz;
    int hasFooter = 0;
    size_t footerStart = v1_end + need2;
    if (footerStart < blobLen && blob[footerStart] == '\n') {
        const char *fs = (const char *)(blob + footerStart + 1);
        const char *fe = (const char *)(blob + blobLen);
        char buf[128];
        size_t k = 0;
        while (fs < fe && *fs != '\n' && k + 1 < sizeof(buf)) {
            buf[k++] = *fs++;
        }
        buf[k] = 0;
        if (k > 0) hasFooter = parse_posix(buf, &pz);
    }

    // ---- Binary-search transitions. ----
    int typeIdx = 0;
    if (tcnt > 0) {
        int lo = 0, hi = (int)tcnt - 1, found = -1;
        while (lo <= hi) {
            int mid = (lo + hi) >> 1;
            int64_t t = rd_i64be(ptrans + (size_t)mid * 8);
            if (t <= utcSec) { found = mid; lo = mid + 1; }
            else             { hi = mid - 1; }
        }
        if (found >= 0) typeIdx = ptypes[found];
    }

    int32_t     gmtoff = 0;
    int         isdst  = 0;
    const char *abbrP  = NULL;
    char        abbrStatic[TZIF_ABBR_CAP];
    abbrStatic[0] = 0;

    // Delegate to POSIX footer when utcSec is beyond the explicit
    // table, or when the table is empty (pure POSIX zones like UTC).
    int useFooter = 0;
    if (hasFooter) {
        if (tcnt == 0) useFooter = 1;
        else {
            int64_t tblLast = rd_i64be(ptrans + (size_t)(tcnt - 1) * 8);
            if (utcSec > tblLast) useFooter = 1;
        }
    }

    if (useFooter) {
        int g = 0, d = 0;
        if (!posix_resolve(&pz, utcSec, &g, &d, &abbrP)) return 0;
        gmtoff = (int32_t)g;
        isdst  = d;
    } else {
        if ((uint32_t)typeIdx >= typec) return 0;
        const uint8_t *ti = pttinfo + (size_t)typeIdx * 6;
        gmtoff = rd_i32be(ti + 0);
        isdst  = ti[4] ? 1 : 0;
        uint8_t abbridx = ti[5];
        if (abbridx >= charc) return 0;
        const char *ab = (const char *)(pchars + abbridx);
        size_t k = 0;
        while (ab < (const char *)(pchars + charc)
               && *ab
               && k + 1 < TZIF_ABBR_CAP) {
            abbrStatic[k++] = *ab++;
        }
        abbrStatic[k] = 0;
        abbrP = abbrStatic;
    }

    out->utcOffsetSec = (int)gmtoff;
    out->isDst        = isdst;
    size_t k = 0;
    if (abbrP) {
        while (abbrP[k] && k + 1 < TZIF_ABBR_CAP) {
            out->abbr[k] = abbrP[k];
            k++;
        }
    }
    out->abbr[k] = 0;

    breakdown(utcSec + gmtoff, out);
    return 1;
}
