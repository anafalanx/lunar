// cert_verify_win.c -- Windows Web-PKI validation for CA-enrolled pins.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cert_verify_win.h"

#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"

void CertVerifyWin_ResultInit(CertVerifyWinResult *out)
{
    if (out) memset(out, 0, sizeof *out);
}

void CertVerifyWin_Hex32(const uint8_t in[32], char out_hex[65])
{
    static const char hexdig[] = "0123456789abcdef";
    if (!out_hex) return;
    if (!in) {
        out_hex[0] = 0;
        return;
    }
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2]     = hexdig[in[i] >> 4];
        out_hex[i * 2 + 1] = hexdig[in[i] & 0x0f];
    }
    out_hex[64] = 0;
}

static int64_t FileTimeToUnixMs(const FILETIME *ft)
{
    if (!ft) return 0;
    ULARGE_INTEGER u;
    u.LowPart = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    if (u.QuadPart < 116444736000000000ULL) return 0;
    return (int64_t)((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

static int64_t FileTimeToUnixSeconds(const FILETIME *ft)
{
    return FileTimeToUnixMs(ft) / 1000;
}

static void FormatFileTimeUtc(const FILETIME *ft, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    out[0] = 0;
    SYSTEMTIME st;
    if (!ft || !FileTimeToSystemTime(ft, &st)) return;
    _snprintf(out, out_len, "%04u-%02u-%02uT%02u:%02u:%02uZ",
              (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
              (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
    out[out_len - 1] = 0;
}

static void CertNameToUtf8(PCCERT_CONTEXT cert, DWORD which,
                           char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    out[0] = 0;
    if (!cert || !cert->pCertInfo) return;
    CERT_NAME_BLOB *blob = (which == CERT_NAME_ISSUER_FLAG)
        ? &cert->pCertInfo->Issuer
        : &cert->pCertInfo->Subject;
    DWORD got = CertNameToStrA(X509_ASN_ENCODING, blob,
                               CERT_X500_NAME_STR | CERT_NAME_STR_REVERSE_FLAG,
                               out, (DWORD)out_len);
    if (got == 0) out[0] = 0;
    else out[out_len - 1] = 0;
}

static int HostToWide(const char *host, wchar_t *out, size_t out_len)
{
    if (!host || !*host || !out || out_len == 0 || out_len > INT_MAX) return 0;
    int got = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                  host, -1, out, (int)out_len);
    if (got <= 0) {
        got = MultiByteToWideChar(CP_ACP, 0, host, -1, out, (int)out_len);
    }
    if (got <= 0 || (size_t)got > out_len) {
        out[0] = 0;
        return 0;
    }
    return 1;
}

static int BuildWindowsChain(PCCERT_CONTEXT leaf,
                             HCERTSTORE additional,
                             DWORD flags,
                             PCCERT_CHAIN_CONTEXT *out_chain)
{
    if (!leaf || !out_chain) return 0;
    *out_chain = NULL;

    LPSTR server_auth[] = { szOID_PKIX_KP_SERVER_AUTH };
    CERT_CHAIN_PARA para;
    memset(&para, 0, sizeof para);
    para.cbSize = sizeof para;
    para.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
    para.RequestedUsage.Usage.cUsageIdentifier = 1;
    para.RequestedUsage.Usage.rgpszUsageIdentifier = server_auth;

    return CertGetCertificateChain(NULL, leaf, NULL, additional,
                                   &para, flags, NULL, out_chain) ? 1 : 0;
}

int CertVerifyWin_ValidateMbedtlsPeer(mbedtls_ssl_context *ssl,
                                      const char *hostname,
                                      CertVerifyWinResult *out)
{
    CertVerifyWin_ResultInit(out);
    if (!ssl || !hostname || !*hostname || !out) return 0;

    const mbedtls_x509_crt *chain = mbedtls_ssl_get_peer_cert(ssl);
    if (!chain || !chain->raw.p || chain->raw.len == 0 ||
        !chain->pk_raw.p || chain->pk_raw.len == 0) {
        return 0;
    }

    if (mbedtls_sha256(chain->pk_raw.p, chain->pk_raw.len,
                       out->spki_sha256, 0) != 0) {
        return 0;
    }
    CertVerifyWin_Hex32(out->spki_sha256, out->spki_hex);

    PCCERT_CONTEXT leaf = CertCreateCertificateContext(
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
        chain->raw.p, (DWORD)chain->raw.len);
    if (!leaf) return 0;

    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0,
                                     CERT_STORE_CREATE_NEW_FLAG, NULL);
    if (!store) {
        CertFreeCertificateContext(leaf);
        return 0;
    }

    for (const mbedtls_x509_crt *c = chain->next; c; c = c->next) {
        if (!c->raw.p || c->raw.len == 0) continue;
        PCCERT_CONTEXT ctx = CertCreateCertificateContext(
            X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
            c->raw.p, (DWORD)c->raw.len);
        if (ctx) {
            CertAddCertificateContextToStore(store, ctx,
                                             CERT_STORE_ADD_REPLACE_EXISTING,
                                             NULL);
            CertFreeCertificateContext(ctx);
        }
    }

    CertNameToUtf8(leaf, 0, out->subject, sizeof out->subject);
    CertNameToUtf8(leaf, CERT_NAME_ISSUER_FLAG, out->issuer, sizeof out->issuer);
    FormatFileTimeUtc(&leaf->pCertInfo->NotBefore,
                      out->not_before, sizeof out->not_before);
    FormatFileTimeUtc(&leaf->pCertInfo->NotAfter,
                      out->not_after, sizeof out->not_after);
    out->not_before_unix = FileTimeToUnixSeconds(&leaf->pCertInfo->NotBefore);
    out->not_after_unix  = FileTimeToUnixSeconds(&leaf->pCertInfo->NotAfter);

    PCCERT_CHAIN_CONTEXT chain_ctx = NULL;
    DWORD rev_flags = CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
    if (!BuildWindowsChain(leaf, store, rev_flags, &chain_ctx)) {
        out->policy_error = GetLastError();
        CertCloseStore(store, 0);
        CertFreeCertificateContext(leaf);
        return 0;
    }

    out->revocation_checked = 1;
    out->chain_error_status = chain_ctx->TrustStatus.dwErrorStatus;
    out->chain_info_status  = chain_ctx->TrustStatus.dwInfoStatus;

    DWORD revocation_bits = CERT_TRUST_REVOCATION_STATUS_UNKNOWN |
                            CERT_TRUST_IS_OFFLINE_REVOCATION;
    DWORD hard_chain_errors = out->chain_error_status & ~revocation_bits;
    if (hard_chain_errors == 0 && (out->chain_error_status & revocation_bits)) {
        out->revocation_offline = 1;
        CertFreeCertificateChain(chain_ctx);
        chain_ctx = NULL;
        if (!BuildWindowsChain(leaf, store, 0, &chain_ctx)) {
            out->policy_error = GetLastError();
            CertCloseStore(store, 0);
            CertFreeCertificateContext(leaf);
            return 0;
        }
        out->chain_error_status = chain_ctx->TrustStatus.dwErrorStatus;
        out->chain_info_status  = chain_ctx->TrustStatus.dwInfoStatus;
    }

    wchar_t hostW[256];
    if (!HostToWide(hostname, hostW, sizeof hostW / sizeof hostW[0])) {
        CertFreeCertificateChain(chain_ctx);
        CertCloseStore(store, 0);
        CertFreeCertificateContext(leaf);
        return 0;
    }

    SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl_para;
    memset(&ssl_para, 0, sizeof ssl_para);
    ssl_para.cbSize = sizeof ssl_para;
    ssl_para.dwAuthType = AUTHTYPE_SERVER;
    ssl_para.pwszServerName = hostW;

    CERT_CHAIN_POLICY_PARA policy_para;
    memset(&policy_para, 0, sizeof policy_para);
    policy_para.cbSize = sizeof policy_para;
    policy_para.pvExtraPolicyPara = &ssl_para;

    CERT_CHAIN_POLICY_STATUS policy_status;
    memset(&policy_status, 0, sizeof policy_status);
    policy_status.cbSize = sizeof policy_status;

    if (!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL,
                                          chain_ctx,
                                          &policy_para,
                                          &policy_status)) {
        out->policy_error = GetLastError();
    } else {
        out->policy_error = policy_status.dwError;
    }

    out->ok = (out->chain_error_status == 0 && out->policy_error == 0) ? 1 : 0;

    CertFreeCertificateChain(chain_ctx);
    CertCloseStore(store, 0);
    CertFreeCertificateContext(leaf);
    return out->ok;
}