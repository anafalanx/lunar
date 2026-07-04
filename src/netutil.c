// netutil.c -- family-agnostic socket helpers. See netutil.h.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string.h>

#include "netutil.h"

int Net_ParseIp(const char *ip, uint16_t port,
                struct sockaddr_storage *out, int *out_len) {
    if (!ip || !out || !out_len) return AF_UNSPEC;
    memset(out, 0, sizeof *out);

    // Try IPv6 first (a bare ':' can only be v6), then IPv4.
    struct in6_addr a6;
    if (inet_pton(AF_INET6, ip, &a6) == 1) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)out;
        s6->sin6_family = AF_INET6;
        s6->sin6_port   = htons(port);
        s6->sin6_addr   = a6;
        *out_len = (int)sizeof *s6;
        return AF_INET6;
    }
    struct in_addr a4;
    if (inet_pton(AF_INET, ip, &a4) == 1) {
        struct sockaddr_in *s4 = (struct sockaddr_in *)out;
        s4->sin_family = AF_INET;
        s4->sin_port   = htons(port);
        s4->sin_addr   = a4;
        *out_len = (int)sizeof *s4;
        return AF_INET;
    }
    return AF_UNSPEC;
}

SOCKET Net_ConnectStream(const struct sockaddr_storage *sa, int salen,
                         int connect_timeout_ms, int io_timeout_ms) {
    if (!sa || salen <= 0) return INVALID_SOCKET;

    SOCKET s = socket(sa->ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    // Non-blocking connect + select() so we get a hard timeout instead
    // of the OS default (which can be tens of seconds).
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    int cr = connect(s, (const struct sockaddr *)sa, salen);
    if (cr != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    fd_set wfd; FD_ZERO(&wfd); FD_SET(s, &wfd);
    struct timeval tv;
    tv.tv_sec  =  connect_timeout_ms / 1000;
    tv.tv_usec = (connect_timeout_ms % 1000) * 1000;
    int sel = select(0, NULL, &wfd, NULL, &tv);
    if (sel <= 0) { closesocket(s); return INVALID_SOCKET; }

    int err = 0; int errlen = (int)sizeof err;
    getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &errlen);
    if (err != 0) { closesocket(s); return INVALID_SOCKET; }

    nb = 0; ioctlsocket(s, FIONBIO, &nb);
    DWORD tmo = (DWORD)io_timeout_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof tmo);
    return s;
}

SOCKET Net_DgramSocket(int family, int io_timeout_ms) {
    SOCKET s = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    DWORD tmo = (DWORD)io_timeout_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof tmo);
    return s;
}
