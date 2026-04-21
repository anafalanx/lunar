// tests/live_nts.c -- live end-to-end NTS verification. Not part of the
// regular test suite (it hits the real network). Compiled on demand via
// `py tests/run_live_nts.py` -- see that script for the build line.
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include "../src/clock.h"
#include "../src/nts.h"

int main(void) {
    Clock_Init();
    size_t n = 0;
    const NtsProvider *pool = Nts_Pool(&n);
    int any_ok = 0, all_ok = 1;
    for (size_t i = 0; i < n; i++) {
        const NtsProvider *p = &pool[i];
        int64_t utc = 0, qpc = 0;
        uint32_t rtt = 0;
        int ok = Nts_FetchSample(p, &utc, &qpc, &rtt);
        printf("%-16s %-24s : ", p->label, p->host);
        if (ok) {
            time_t s = (time_t)(utc / 1000);
            struct tm tm; gmtime_s(&tm, &s);
            char buf[40];
            snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(utc % 1000));
            printf("OK   %s  rtt=%4ums\n", buf, (unsigned)rtt);
            any_ok = 1;
        } else {
            printf("FAIL\n");
            all_ok = 0;
        }
    }
    if (!any_ok) return 1;
    return all_ok ? 0 : 2;
}
