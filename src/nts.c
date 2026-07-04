// nts.c -- TLS 1.3 + NTS-KE transport with local SPKI enrollment.
//
// Flow per call:
//
//   1. Resolve host -> IPv4 through the enrolled DoH resolver.
//   2. TCP connect with a 5 s timeout.
//   3. TLS 1.3 handshake (shared pinned_tls helper) with ALPN "ntske/1".
//   4. Match the leaf against the endpoint's enrolled SPKI set, or
//      perform Windows CA enrollment/renewal. A CA-valid leaf that
//      matches no stored pin outside the renewal window completes the
//      exchange as a PENDING ROTATION (not persisted here; see ntp.c).
//   5. Send the fixed NTS-KE client request (NtsKe_BuildClientRequest).
//   6. Drain inbound TLS records until peer-close or full response is
//      in our accumulator; parse with NtsKe_ParseResponse.
//   7. Export C2S and S2C AEAD keys via the TLS exporter
//      (RFC 8446 §7.5, RFC 8915 §5.1).
//   8. Graceful TLS close_notify, close socket.
//
// We hold no long-lived TLS RNG state. pinned_tls.c feeds mbedTLS through
// a BCryptGenRandom callback, so parallel exchanges do not share a
// mutable DRBG. Each exchange builds a fresh SSL config + context + socket.

// Winsock2 must come before <windows.h> -- see ntp.c for the same pattern.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "mbedtls/ssl.h"
#include "mbedtls/error.h"
#include "mbedtls/platform_util.h"

#include "nts.h"
#include "nts_ke.h"
#include "nts_ef.h"
#include "clock.h"
#include "dns.h"
#include "netutil.h"
#include "logbuf.h"
#include "pinned_tls.h"
#include "pin_store.h"

// ---------------------------------------------------------------------------
// Provider pool
// ---------------------------------------------------------------------------
//
// Lunar ships only endpoint metadata here: no provider SPKI pins, no
// signing keys, and no other provider cryptographic trust material.
// First-run enrollment and renewal capture SPKI pins into the protected
// local pin store after Windows/Web-PKI validation succeeds.
//
// Source list is deliberately small and operator-curated. NTS is
// still a young protocol and not every public NTP pool runs it.

static const NtsProvider kProviders[] = {
    { .host = "time.cloudflare.com",        .port = 0, .label = "cloudflare",        .operator_family = "cloudflare" },
    { .host = "nts.netnod.se",              .port = 0, .label = "netnod",            .operator_family = "netnod" },
    { .host = "sth1.nts.netnod.se",         .port = 0, .label = "netnod-sth1",       .operator_family = "netnod" },
    { .host = "sth2.nts.netnod.se",         .port = 0, .label = "netnod-sth2",       .operator_family = "netnod" },
    /* NOTE: ptbtime{1..4}.ptb.de KE + cookies work, but PTB silently
     * drops NTS-sized (>48 byte) UDP packets at the NTP endpoint, so
     * the authenticated round trip never completes. We keep PTB as a
     * plain-SNTP core source (see src/ntp.c) and use System76's US
     * nodes here for additional NTS diversity. Verified 2026-04-21 via
     * scripts/probe_nts.py on ptbtime{1..4}. */
    { .host = "ohio.time.system76.com",     .port = 0, .label = "system76-ohio",     .operator_family = "system76" },
    { .host = "virginia.time.system76.com", .port = 0, .label = "system76-virginia", .operator_family = "system76" },
    { .host = "oregon.time.system76.com",   .port = 0, .label = "system76-oregon",   .operator_family = "system76" },
    { .host = "paris.time.system76.com",    .port = 0, .label = "system76-paris",    .operator_family = "system76" },
    { .host = "nts.time.nl",                .port = 0, .label = "sidn",              .operator_family = "sidn" },
};

#define NTS_PROVIDER_COUNT (sizeof kProviders / sizeof kProviders[0])

static_assert(NTS_PROVIDER_COUNT >= 2,
              "NTS provider pool must support distinct authenticated anchors");

const NtsProvider *Nts_Pool(size_t *out_len)
{
    if (out_len) *out_len = NTS_PROVIDER_COUNT;
    return kProviders;
}

const NtsProvider *Nts_PickProvider(void)
{
    uint32_t r = 0;
    if (BCryptGenRandom(NULL, (PUCHAR)&r, sizeof r,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return NULL;
    }
    return &kProviders[r % NTS_PROVIDER_COUNT];
}

size_t Nts_PickProviders(const NtsProvider **out, size_t n_want)
{
    if (!out || n_want == 0) return 0;
    size_t enabled[NTS_PROVIDER_COUNT];
    size_t n_enabled = NTS_PROVIDER_COUNT;
    for (size_t i = 0; i < n_enabled; i++) enabled[i] = i;
    if (n_want > n_enabled) n_want = n_enabled;

    // Fisher-Yates shuffle over the metadata indices using
    // BCryptGenRandom for each draw. n is small (<= pool size), so
    // this is trivially constant-work and uniform.
    for (size_t i = 0; i < n_enabled; i++) {
        uint32_t r = 0;
        if (BCryptGenRandom(NULL, (PUCHAR)&r, sizeof r,
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            return 0;
        }
        size_t j = i + (size_t)(r % (uint32_t)(n_enabled - i));
        size_t tmp = enabled[i];
        enabled[i] = enabled[j];
        enabled[j] = tmp;
    }

    for (size_t i = 0; i < n_want; i++) out[i] = NULL;

    size_t written = 0;
    for (size_t i = 0; i < n_enabled && written < n_want; i++) {
        const NtsProvider *p = &kProviders[enabled[i]];
        const char *fam = p->operator_family ? p->operator_family : "";
        int duplicate_family = 0;
        for (size_t j = 0; j < written; j++) {
            const char *old = out[j] && out[j]->operator_family
                ? out[j]->operator_family : "";
            if (strcmp(fam, old) == 0) {
                duplicate_family = 1;
                break;
            }
        }
        if (!duplicate_family) out[written++] = p;
    }
    for (size_t i = 0; i < n_enabled && written < n_want; i++) {
        const NtsProvider *p = &kProviders[enabled[i]];
        int already = 0;
        for (size_t j = 0; j < written; j++) {
            if (out[j] == p) {
                already = 1;
                break;
            }
        }
        if (!already) out[written++] = p;
    }
    return written;
}

// ---------------------------------------------------------------------------
// TCP connect with a hard timeout
// ---------------------------------------------------------------------------

#define NTS_CONNECT_TIMEOUT_MS   5000
#define NTS_IO_TIMEOUT_MS        5000
#define NTS_MAX_REPLY_BYTES      16384

static SOCKET tcp_connect(const char *host, uint16_t port)
{
    // Resolve via pinned DoH first (dual-stack A/AAAA). If every pinned
    // resolver is blocked -- common on corporate networks that force
    // their own DNS and firewall 443 to public resolvers -- fall back
    // to the OS resolver. That fallback is safe HERE and only here: the
    // NTS-KE session below is authenticated by a locally enrolled SPKI
    // pin, so a forged system-DNS answer cannot redirect us to a host
    // whose leaf an attacker controls -- it fails at TLS. (Core SNTP,
    // which is unauthenticated, must never do this.)
    char ip[NET_IP_STRLEN];
    int  fam = AF_UNSPEC;
    if (Dns_ResolveEx(host, ip, &fam) != 0) {
        if (Dns_ResolveSystem(host, ip, &fam) != 0) return INVALID_SOCKET;
    }

    struct sockaddr_storage sa;
    int salen = 0;
    if (Net_ParseIp(ip, port, &sa, &salen) == AF_UNSPEC) return INVALID_SOCKET;

    return Net_ConnectStream(&sa, salen,
                             NTS_CONNECT_TIMEOUT_MS, NTS_IO_TIMEOUT_MS);
}

// ---------------------------------------------------------------------------
// Nts_DoKe
// ---------------------------------------------------------------------------

int Nts_DoKe(const NtsProvider *p, NtsKeResult *out)
{
    return Nts_DoKeEx(p, out, NULL);
}

int Nts_DoKeEx(const NtsProvider *p, NtsKeResult *out,
               NtsRotationPending *rot)
{
    if (rot) memset(rot, 0, sizeof *rot);
    if (p == NULL || out == NULL) return -1;
    memset(out, 0, sizeof *out);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;

    int      rc = -1;
    PinnedTls tls;
    PinnedTls_Init(&tls);

    uint16_t port = p->port ? p->port : 4460;
    SOCKET s = tcp_connect(p->host, port);
    if (s == INVALID_SOCKET) goto cleanup;

    // ALPN: exactly "ntske/1" (RFC 8915 §4).
    static const char *alpn_list[] = { "ntske/1", NULL };
    PinRecord pin;
    int have_pin = PinStore_GetPin(PIN_ENDPOINT_NTS, p->label, p->host,
                                   port, &pin);
    // The endpoint's usable pins: every stored, un-expired SPKI. A leaf
    // matching ANY of them authenticates as an enrolled pin.
    uint8_t pin_set[PIN_STORE_MAX_SPKIS][32];
    size_t n_pins = have_pin ? PinStore_CollectValidSpkis(&pin, pin_set) : 0;
    int usable = n_pins > 0;
    int renew_due = usable ? PinStore_ShouldRenew(&pin) : 1;
    if (!have_pin) {
        Log_Append("nts: %s first-run enrollment required for %s:%u",
                   p->label, p->host, (unsigned)port);
    } else if (!usable) {
        Log_Append("nts: %s all %u local pin(s) expired; CA revalidation required (newest valid=%s..%s nextCa=%s)",
                   p->label, (unsigned)pin.spki_count,
                   pin.not_before, pin.not_after,
                   pin.renewal_due[0] ? pin.renewal_due : "unknown");
    } else if (renew_due) {
        Log_Append("nts: %s scheduled CA renewal due (pins=%u newest valid=%s..%s nextCa=%s nextCaUnix=%lld)",
                   p->label, (unsigned)n_pins,
                   pin.not_before, pin.not_after,
                   pin.renewal_due[0] ? pin.renewal_due : "unknown",
                   (long long)pin.renewal_due_unix);
    }

    // CA fallback stays enabled even for an out-of-window mismatch:
    // instead of hard-rejecting an early/emergency key rotation, the
    // leaf is CA-validated and, if valid, flagged as a pending rotation
    // for the aggregator to corroborate (never persisted here).
    PinnedTlsOpenResult openInfo;
    if (PinnedTls_OpenEnrolledSet(&tls, s, p->host, alpn_list,
                                  usable ? (const uint8_t (*)[32])pin_set : NULL,
                                  n_pins, 1 /* allow CA */,
                                  usable && renew_due, &openInfo) != 0) {
        if (openInfo.ca_attempted) {
            Log_Append("nts: %s CA validation rejected host=%s%s chain=0x%lx policy=0x%lx revocation=%s subject=\"%s\" issuer=\"%s\" spki=%s",
                       p->label, p->host,
                       openInfo.pin_mismatched
                           ? " (pin mismatch; unvalidated rotation refused)" : "",
                       (unsigned long)openInfo.cert.chain_error_status,
                       (unsigned long)openInfo.cert.policy_error,
                       openInfo.cert.revocation_offline ? "offline" :
                       (openInfo.cert.revocation_checked ? "checked" : "not-checked"),
                       openInfo.cert.subject, openInfo.cert.issuer,
                       openInfo.cert.spki_hex);
        }
        goto cleanup;
    }
    mbedtls_ssl_context *ssl = PinnedTls_Ssl(&tls);

    // ALPN check.
    {
        const char *neg = PinnedTls_NegotiatedAlpn(&tls);
        if (neg == NULL || strcmp(neg, "ntske/1") != 0) goto cleanup;
        if (openInfo.ca_attempted) {
            Log_Append("nts: %s CA validation %s host=%s alpn=%s subject=\"%s\" issuer=\"%s\" notBefore=%s notAfter=%s spki=%s revocation=%s chain=0x%lx policy=0x%lx",
                       p->label,
                       openInfo.ca_valid ? "accepted" : "failed-but-pin-still-valid",
                       p->host, neg,
                       openInfo.cert.subject, openInfo.cert.issuer,
                       openInfo.cert.not_before, openInfo.cert.not_after,
                       openInfo.cert.spki_hex,
                       openInfo.cert.revocation_offline ? "offline" :
                       (openInfo.cert.revocation_checked ? "checked" : "not-checked"),
                       (unsigned long)openInfo.cert.chain_error_status,
                       (unsigned long)openInfo.cert.policy_error);
            if (openInfo.ca_valid) {
                if (usable && !openInfo.pin_matched && !renew_due) {
                    // Early/emergency rotation: CA-valid leaf, no pin
                    // match, outside the renewal window. Complete the
                    // exchange but DO NOT persist; the aggregator
                    // promotes the pin only after an operator-diverse,
                    // still-pinned peer corroborates the cycle.
                    if (rot) {
                        rot->pending = 1;
                        rot->port = port;
                        memcpy(rot->spki, openInfo.cert.spki_sha256, 32);
                        memcpy(rot->spki_hex, openInfo.cert.spki_hex,
                               sizeof rot->spki_hex);
                        memcpy(rot->old_spki_hex, pin.spki_hex,
                               sizeof rot->old_spki_hex);
                        memcpy(rot->not_before, openInfo.cert.not_before,
                               sizeof rot->not_before);
                        memcpy(rot->not_after, openInfo.cert.not_after,
                               sizeof rot->not_after);
                        rot->not_before_unix = openInfo.cert.not_before_unix;
                        rot->not_after_unix  = openInfo.cert.not_after_unix;
                    }
                    Log_Append("nts: %s pin ROTATION observed outside renewal window host=%s newSpki=%s storedNewest=%s pins=%u; enrollment deferred pending operator-diverse corroboration",
                               p->label, p->host, openInfo.cert.spki_hex,
                               pin.spki_hex, (unsigned)n_pins);
                } else {
                    const char *status = !usable
                        ? (have_pin ? "expired-renewal" : "first-run-enrollment")
                        : (openInfo.pin_matched ? "scheduled-renewal"
                                                : "pin-rotation");
                    PinStore_SavePin(PIN_ENDPOINT_NTS, p->label, p->host, port,
                                     p->operator_family,
                                     openInfo.cert.spki_sha256,
                                     openInfo.cert.spki_hex,
                                     openInfo.cert.not_before,
                                     openInfo.cert.not_after,
                                     openInfo.cert.not_before_unix,
                                     openInfo.cert.not_after_unix,
                                     status);
                }
            }
        } else if (openInfo.pin_matched) {
            Log_Append("nts: %s local pin match host=%s spki=%s (1 of %u enrolled) newest valid=%s..%s nextCa=%s",
                       p->label, p->host, openInfo.peer_spki_hex,
                       (unsigned)n_pins,
                       pin.not_before, pin.not_after,
                       pin.renewal_due[0] ? pin.renewal_due : "unknown");
        }
    }

    // Send NTS-KE request.
    {
        uint8_t req[32];
        size_t  rlen = NtsKe_BuildClientRequest(req, sizeof req);
        if (rlen == 0) goto cleanup;
        size_t sent = 0;
        while (sent < rlen) {
            int wr = mbedtls_ssl_write(ssl, req + sent, rlen - sent);
            if (wr == MBEDTLS_ERR_SSL_WANT_READ || wr == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (wr <= 0) goto cleanup;
            sent += (size_t)wr;
        }
    }

    // Drain reply.
    uint8_t reply[NTS_MAX_REPLY_BYTES];
    size_t  reply_len = 0;
    for (;;) {
        int rd = mbedtls_ssl_read(ssl, reply + reply_len,
                                  sizeof reply - reply_len);
        if (rd == MBEDTLS_ERR_SSL_WANT_READ || rd == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (rd == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
        if (rd <= 0) {
            // Servers sometimes drop TCP after the reply without
            // sending close_notify; treat a clean CONN_EOF or 0 as
            // end-of-message if we already accumulated something.
            if ((rd == MBEDTLS_ERR_SSL_CONN_EOF || rd == 0) && reply_len > 0) break;
            goto cleanup;
        }
        reply_len += (size_t)rd;
        if (reply_len >= sizeof reply) goto cleanup;  // ceiling hit
    }

    // Parse.
    NtsKeResponse resp;
    if (NtsKe_ParseResponse(reply, reply_len, &resp) != 1) goto cleanup;

    // Export AEAD keys per RFC 8915 §5.1.
    static const char kExporterLabel[] = "EXPORTER-network-time-security";
    {
        // Context for C2S: [proto u16][aead u16][S-bit u8]
        uint8_t ctx_c2s[5] = { 0x00, 0x00, 0x00, 0x0f, 0x00 };
        uint8_t ctx_s2c[5] = { 0x00, 0x00, 0x00, 0x0f, 0x01 };
        if (mbedtls_ssl_export_keying_material(ssl,
                out->c2s_key, 32,
                kExporterLabel, sizeof kExporterLabel - 1,
                ctx_c2s, sizeof ctx_c2s, 1) != 0) goto cleanup;
        if (mbedtls_ssl_export_keying_material(ssl,
                out->s2c_key, 32,
                kExporterLabel, sizeof kExporterLabel - 1,
                ctx_s2c, sizeof ctx_s2c, 1) != 0) goto cleanup;
    }

    // Populate the result.
    out->cookie_count = resp.cookie_count;
    for (size_t i = 0; i < resp.cookie_count; i++) {
        out->cookie_len[i] = resp.cookie_len[i];
        memcpy(out->cookies[i], resp.cookies[i], resp.cookie_len[i]);
    }
    memcpy(out->ntp_host, resp.ntp_host, sizeof out->ntp_host);
    out->ntp_port = resp.ntp_port;
    out->ok = 1;
    rc = 0;

    // Graceful TLS shutdown -- ignore failures; we already have what
    // we came for.
    PinnedTls_CloseNotify(&tls);

cleanup:
    PinnedTls_Free(&tls);
    WSACleanup();
    if (rc != 0) {
        // Wipe any key material we may have written before failing,
        // and drop rotation evidence from a failed exchange.
        mbedtls_platform_zeroize(out, sizeof *out);
        if (rot) memset(rot, 0, sizeof *rot);
    }
    return rc;
}

// ---------------------------------------------------------------------------
// Nts_FetchSample -- KE + one authenticated SNTP round trip
// ---------------------------------------------------------------------------

#define NTP_EPOCH_DELTA_S  2208988800ULL
#define NTS_UDP_TIMEOUT_MS 3000
#define NTS_UDP_MAX_PKT    1500

static int parse_sntp_reply(const uint8_t *pkt, size_t pkt_len,
                            int64_t *out_t2_ms, int64_t *out_t3_ms)
{
    if (pkt_len < 48) return 0;
    uint8_t li      = (pkt[0] >> 6) & 0x3;
    uint8_t vn      = (pkt[0] >> 3) & 0x7;
    uint8_t mode    =  pkt[0]       & 0x7;
    uint8_t stratum =  pkt[1];
    if (li == 3)                       return 0;
    if (vn != 3 && vn != 4)            return 0;
    if (mode != 4)                     return 0;
    if (stratum == 0 || stratum >= 16) return 0;

    uint32_t secBE, fracBE;
    memcpy(&secBE,  pkt + 32, 4); memcpy(&fracBE, pkt + 36, 4);
    uint32_t t2_s = ntohl(secBE), t2_frac = ntohl(fracBE);
    memcpy(&secBE,  pkt + 40, 4); memcpy(&fracBE, pkt + 44, 4);
    uint32_t t3_s = ntohl(secBE), t3_frac = ntohl(fracBE);
    if (t2_s == 0 || t3_s == 0) return 0;

    *out_t2_ms = ((int64_t)t2_s - (int64_t)NTP_EPOCH_DELTA_S) * 1000
                 + (int64_t)(((uint64_t)t2_frac * 1000ULL) >> 32);
    *out_t3_ms = ((int64_t)t3_s - (int64_t)NTP_EPOCH_DELTA_S) * 1000
                 + (int64_t)(((uint64_t)t3_frac * 1000ULL) >> 32);
    return 1;
}

int Nts_FetchSample(const NtsProvider *p,
                    int64_t  *out_ntpUtcMs,
                    int64_t  *out_qpcAtT4,
                    uint32_t *out_rttMs)
{
    return Nts_FetchSampleEx(p, out_ntpUtcMs, out_qpcAtT4, out_rttMs, NULL);
}

int Nts_FetchSampleEx(const NtsProvider *p,
                      int64_t  *out_ntpUtcMs,
                      int64_t  *out_qpcAtT4,
                      uint32_t *out_rttMs,
                      NtsRotationPending *rot)
{
    if (rot) memset(rot, 0, sizeof *rot);
    if (p == NULL || out_ntpUtcMs == NULL || out_qpcAtT4 == NULL
        || out_rttMs == NULL) return 0;
    *out_ntpUtcMs = 0; *out_qpcAtT4 = 0; *out_rttMs = 0;

    // Phase 1: NTS-KE. Nts_DoKeEx runs its own WSAStartup/Cleanup pair
    // and clears `ke` (and `rot`) on failure, so we just inherit its
    // verdict.
    NtsKeResult ke;
    if (Nts_DoKeEx(p, &ke, rot) != 0 || !ke.ok || ke.cookie_count == 0) {
        if (rot) memset(rot, 0, sizeof *rot);
        return 0;
    }

    const char *host = ke.ntp_host[0] ? ke.ntp_host : p->host;
    uint16_t    port = ke.ntp_port    ? ke.ntp_port : 123;

    // Phase 2: build the authenticated SNTP request.
    uint8_t hdr[48];
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 0x23;   // LI=0, VN=4, Mode=3 (client)

    uint8_t uid[NTS_UNIQUE_ID_LEN], nonce[NTS_NONCE_LEN];
    if (BCryptGenRandom(NULL, uid, sizeof uid,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) goto fail;
    if (BCryptGenRandom(NULL, nonce, sizeof nonce,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) goto fail;

    uint8_t pkt[NTS_UDP_MAX_PKT];
    size_t  pkt_len = 0;
    if (NtsEf_BuildRequest(hdr, uid, nonce,
                           ke.cookies[0], ke.cookie_len[0],
                           0 /* no placeholder cookies */,
                           ke.c2s_key,
                           pkt, sizeof pkt, &pkt_len) != 0) goto fail;

    // Phase 3: UDP exchange with QPC-bracketed timing.
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) goto fail;

    int           rc = 0;
    SOCKET        s  = INVALID_SOCKET;

    // Dual-stack DoH, with the same NTS-only system-DNS fallback as
    // tcp_connect. The host here is the NTS-KE host or an override the
    // server sent via NTSKE_REC_NTPV4_SERVER; either way the SNTP
    // exchange is AEAD-authenticated with c2s/s2c keys, so a forged DNS
    // answer that redirects us cannot forge a valid sample -- the
    // authenticator check drops it. Safe to fall back to OS DNS when
    // pinned DoH is blocked.
    char ip[NET_IP_STRLEN];
    int  fam = AF_UNSPEC;
    if (Dns_ResolveEx(host, ip, &fam) != 0) {
        if (Dns_ResolveSystem(host, ip, &fam) != 0) goto udp_done;
    }

    struct sockaddr_storage sa;
    int salen = 0;
    if (Net_ParseIp(ip, port, &sa, &salen) == AF_UNSPEC) goto udp_done;

    s = Net_DgramSocket(fam, NTS_UDP_TIMEOUT_MS);
    if (s == INVALID_SOCKET) goto udp_done;

    int64_t qpcT1 = Clock_Qpc();
    int sent = sendto(s, (const char *)pkt, (int)pkt_len, 0,
                      (struct sockaddr *)&sa, salen);
    if (sent != (int)pkt_len) goto udp_done;

    uint8_t reply[NTS_UDP_MAX_PKT];
    int recvd = recv(s, (char *)reply, (int)sizeof reply, 0);
    int64_t qpcT4 = Clock_Qpc();
    if (recvd <= 0) goto udp_done;

    // Phase 4: authenticate + parse. ParseResponse does the SIV check,
    // enforces the Authenticator is the final extension, and matches
    // our UID in constant time. If anything's off we drop the sample.
    uint8_t new_cookies[NTSKE_MAX_COOKIES][NTSKE_MAX_COOKIE_LEN];
    size_t  new_lens[NTSKE_MAX_COOKIES];
    size_t  new_cnt = 0;
    if (NtsEf_ParseResponse(reply, (size_t)recvd, uid, ke.s2c_key,
                            new_cookies, new_lens, &new_cnt) != 0) goto udp_done;

    int64_t t2_ms = 0, t3_ms = 0;
    if (!parse_sntp_reply(reply, (size_t)recvd, &t2_ms, &t3_ms)) goto udp_done;

    int64_t qpcFreq = Clock_QpcFreq();
    if (qpcFreq <= 0) goto udp_done;
    int64_t rtt = ((qpcT4 - qpcT1) * 1000LL + qpcFreq / 2) / qpcFreq;
    if (rtt < 0) rtt = 0;
    int64_t serverProc = t3_ms - t2_ms;
    if (serverProc < 0) serverProc = 0;
    int64_t netRtt = rtt - serverProc;
    if (netRtt < 0) netRtt = 0;

    *out_ntpUtcMs = t3_ms + netRtt / 2;
    *out_qpcAtT4  = qpcT4;
    *out_rttMs    = (uint32_t)(rtt > 0x7fffffff ? 0x7fffffff : rtt);
    rc = 1;

    // Newly harvested cookies are discarded in this stateless model;
    // the next cycle performs a fresh KE and gets new ones. See
    // nts.h commentary. Zeroise any key material we've touched.
    mbedtls_platform_zeroize(new_cookies, sizeof new_cookies);

udp_done:
    if (s != INVALID_SOCKET) closesocket(s);
    WSACleanup();
    mbedtls_platform_zeroize(uid,   sizeof uid);
    mbedtls_platform_zeroize(nonce, sizeof nonce);
    mbedtls_platform_zeroize(pkt,   sizeof pkt);
    mbedtls_platform_zeroize(&ke,   sizeof ke);
    // Rotation evidence from the KE is only meaningful when the whole
    // authenticated sample succeeded.
    if (rc == 0 && rot) memset(rot, 0, sizeof *rot);
    return rc;

fail:
    mbedtls_platform_zeroize(&ke, sizeof ke);
    if (rot) memset(rot, 0, sizeof *rot);
    return 0;
}
