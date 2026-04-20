// ntp.c -- Minimal SNTP v4 client over UDP. Isolated from raylib.h because
// <windows.h>/<winsock2.h> collide with raylib's symbol names.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "ntp.h"

#define NTP_HOST             "pool.ntp.org"
#define NTP_PORT             "123"
#define NTP_TIMEOUT_MS       3000
#define NTP_EPOCH_DELTA_S    2208988800ULL   // seconds between 1900 and 1970
#define FT_EPOCH_DELTA_MS    11644473600000ULL // 1601 -> 1970 in ms
#define FRESH_WINDOW_MS      (2LL * 60LL * 60LL * 1000LL) // 2 hours

// Atomic-ish shared state. Aligned 64-bit loads/stores are atomic on x86_64.
static volatile LONG64 g_offsetMs        = 0;
static volatile LONG64 g_lastSuccessTick = 0; // GetTickCount64() at success
static volatile LONG64 g_lastSuccessUtc  = 0; // wall-clock UTC ms at success
static volatile LONG   g_running         = 0;

static int64_t FtToMs(FILETIME ft) {
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (int64_t)(u.QuadPart / 10000LL) - (int64_t)FT_EPOCH_DELTA_MS;
}

static int NtpQueryOnce(int64_t *outOffsetMs) {
    unsigned char pkt[48];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x23;  // LI=0, VN=4, Mode=3 (client)

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    if (getaddrinfo(NTP_HOST, NTP_PORT, &hints, &res) != 0 || !res) return 0;

    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return 0; }

    DWORD tmo = NTP_TIMEOUT_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof(tmo));

    FILETIME ft1; GetSystemTimeAsFileTime(&ft1);
    int sent = sendto(s, (const char *)pkt, (int)sizeof(pkt), 0,
                      res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);
    if (sent != (int)sizeof(pkt)) { closesocket(s); return 0; }

    int recvd = recv(s, (char *)pkt, (int)sizeof(pkt), 0);
    FILETIME ft4; GetSystemTimeAsFileTime(&ft4);
    closesocket(s);
    if (recvd != (int)sizeof(pkt)) return 0;

    // Validate the response header before trusting any timestamps:
    //   LI  (bits 7..6 of byte 0) must be 0, 1 or 2   (3 = alarm, unsynced)
    //   VN  (bits 5..3) must be 3 or 4
    //   Mode(bits 2..0) must be 4                     (server)
    //   Stratum (byte 1) must be 1..15                (0 = kiss-o'-death,
    //                                                  16 = unsynchronized)
    uint8_t li      = (pkt[0] >> 6) & 0x3;
    uint8_t vn      = (pkt[0] >> 3) & 0x7;
    uint8_t mode    =  pkt[0]       & 0x7;
    uint8_t stratum =  pkt[1];
    if (li == 3)                         return 0;
    if (vn != 3 && vn != 4)              return 0;
    if (mode != 4)                       return 0;
    if (stratum == 0 || stratum >= 16)   return 0;

    // Server receive timestamp at bytes 32..39, transmit at 40..47.
    uint32_t secBE, fracBE;
    memcpy(&secBE, pkt + 32, 4); memcpy(&fracBE, pkt + 36, 4);
    uint32_t t2_s    = ntohl(secBE);
    uint32_t t2_frac = ntohl(fracBE);
    memcpy(&secBE, pkt + 40, 4); memcpy(&fracBE, pkt + 44, 4);
    uint32_t t3_s    = ntohl(secBE);
    uint32_t t3_frac = ntohl(fracBE);
    if (t2_s == 0 || t3_s == 0) return 0;  // missing required timestamps

    int64_t t2_ms = ((int64_t)t2_s - (int64_t)NTP_EPOCH_DELTA_S) * 1000
                    + (int64_t)(((uint64_t)t2_frac * 1000ULL) >> 32);
    int64_t t3_ms = ((int64_t)t3_s - (int64_t)NTP_EPOCH_DELTA_S) * 1000
                    + (int64_t)(((uint64_t)t3_frac * 1000ULL) >> 32);
    int64_t t1_ms = FtToMs(ft1);
    int64_t t4_ms = FtToMs(ft4);

    // offset = ((t2 - t1) + (t3 - t4)) / 2
    *outOffsetMs = ((t2_ms - t1_ms) + (t3_ms - t4_ms)) / 2;
    return 1;
}

static DWORD WINAPI SyncThreadProc(LPVOID param) {
    (void)param;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
        int64_t off = 0;
        if (NtpQueryOnce(&off)) {
            FILETIME now; GetSystemTimeAsFileTime(&now);
            InterlockedExchange64(&g_offsetMs, off);
            InterlockedExchange64(&g_lastSuccessTick, (LONG64)GetTickCount64());
            InterlockedExchange64(&g_lastSuccessUtc, (LONG64)FtToMs(now));
        }
        WSACleanup();
    }
    InterlockedExchange(&g_running, 0);
    return 0;
}

void Ntp_Start(void) {
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) return;
    HANDLE th = CreateThread(NULL, 0, SyncThreadProc, NULL, 0, NULL);
    if (th) {
        CloseHandle(th);
    } else {
        InterlockedExchange(&g_running, 0);
    }
}

int Ntp_IsSynced(void) {
    LONG64 last = g_lastSuccessTick;
    if (last == 0) return 0;
    LONG64 now = (LONG64)GetTickCount64();
    return (now - last) <= FRESH_WINDOW_MS;
}

int64_t Ntp_OffsetMs(void) {
    return (int64_t)g_offsetMs;
}

int64_t Ntp_LastSyncUtcMs(void) {
    return (int64_t)g_lastSuccessUtc;
}
