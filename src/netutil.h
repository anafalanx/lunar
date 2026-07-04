// netutil.h -- family-agnostic socket helpers (IPv4 + IPv6).
//
// The SNTP, NTS-KE, NTS-NTP, and DoH-bootstrap paths all need the same
// two things: turn an IP literal into a sockaddr, and open a socket to
// it with a hard timeout. Centralizing that here is what lets every
// upstream path speak IPv4 or IPv6 without each call site branching on
// address family.

#ifndef LUNAR_NETUTIL_H
#define LUNAR_NETUTIL_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Longest textual IP the resolvers hand back: an IPv6 address with an
// embedded scope id fits in INET6_ADDRSTRLEN (46). We never carry a
// scope id (all addresses are global), so 46 is ample.
#define NET_IP_STRLEN 46

// Parse an IPv4 or IPv6 literal into *out with `port`. Returns the
// address family (AF_INET / AF_INET6) and sets *out_len, or AF_UNSPEC
// on a parse failure (out untouched).
int Net_ParseIp(const char *ip, uint16_t port,
                struct sockaddr_storage *out, int *out_len);

// Non-blocking connect() to `sa` with a hard timeout, returning a
// connected *blocking* SOCKET with rcv/snd timeouts applied, or
// INVALID_SOCKET. The socket family is taken from `sa`, so this works
// for IPv4 and IPv6 identically.
SOCKET Net_ConnectStream(const struct sockaddr_storage *sa, int salen,
                         int connect_timeout_ms, int io_timeout_ms);

// A datagram socket of the given family (AF_INET / AF_INET6) with
// rcv/snd timeouts applied, or INVALID_SOCKET. Not connected; the
// caller sendto()s the sockaddr it parsed.
SOCKET Net_DgramSocket(int family, int io_timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
