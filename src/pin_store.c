// pin_store.c -- DPAPI-protected local SPKI enrollment cache.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <aclapi.h>
#include <accctrl.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pin_store.h"
#include "app_paths.h"
#include "cert_verify_win.h"
#include "logbuf.h"

#define PINSTORE_VERSION 1
#define PINSTORE_MAX_ENTRIES 64
#define PINSTORE_RENEWAL_MARGIN_SECONDS (30LL * 24LL * 60LL * 60LL)

typedef struct {
    PinEndpointKind kind;
    char label[48];
    char hostname[128];
    uint16_t port;
    char operator_family[48];
    PinRecord rec;
} PinEntry;

static CRITICAL_SECTION g_cs;
static int g_cs_init;
static int g_loaded;
static PinEntry g_entries[PINSTORE_MAX_ENTRIES];
static size_t g_entry_count;

static void EnsureCs(void)
{
    if (!g_cs_init) {
        InitializeCriticalSection(&g_cs);
        g_cs_init = 1;
    }
}

static int64_t NowUnixSeconds(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    if (u.QuadPart < 116444736000000000ULL) return 0;
    return (int64_t)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

static const char *KindName(PinEndpointKind kind)
{
    return kind == PIN_ENDPOINT_DOH ? "doh" :
           kind == PIN_ENDPOINT_NTS ? "nts" : "?";
}

static PinEndpointKind ParseKind(const char *s)
{
    if (strcmp(s, "doh") == 0) return PIN_ENDPOINT_DOH;
    if (strcmp(s, "nts") == 0) return PIN_ENDPOINT_NTS;
    return 0;
}

static int SafeCopy(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0 || !src) return 0;
    if (strchr(src, '|') || strchr(src, '\n') || strchr(src, '\r')) return 0;
    size_t n = strlen(src);
    if (n >= dst_len) return 0;
    memcpy(dst, src, n + 1);
    return 1;
}

static int HexVal(int ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int HexToBytes32(const char *hex, uint8_t out[32])
{
    if (!hex || strlen(hex) != 64 || !out) return 0;
    for (int i = 0; i < 32; i++) {
        int hi = HexVal(hex[i * 2]);
        int lo = HexVal(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

static int64_t ComputeRenewalDue(int64_t not_before, int64_t not_after)
{
    (void)not_before;
    if (not_after <= 0) return 0;
    int64_t due = not_after - PINSTORE_RENEWAL_MARGIN_SECONDS;
    if (due < 0) due = 0;
    return due;
}

static int EntryMatches(const PinEntry *e, PinEndpointKind kind,
                        const char *label, const char *hostname,
                        uint16_t port)
{
    return e && e->kind == kind && e->port == port &&
           strcmp(e->label, label ? label : "") == 0 &&
           strcmp(e->hostname, hostname ? hostname : "") == 0;
}

static PinEntry *FindEntryLocked(PinEndpointKind kind,
                                 const char *label,
                                 const char *hostname,
                                 uint16_t port)
{
    for (size_t i = 0; i < g_entry_count; i++) {
        if (EntryMatches(&g_entries[i], kind, label, hostname, port)) {
            return &g_entries[i];
        }
    }
    return NULL;
}

static int ReadWholeFile(const wchar_t *path, uint8_t **out, DWORD *out_len)
{
    if (!out || !out_len) return 0;
    *out = NULL;
    *out_len = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 1024 * 1024) {
        CloseHandle(h);
        return 0;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz.QuadPart);
    if (!buf) {
        CloseHandle(h);
        return 0;
    }
    DWORD got = 0;
    int ok = ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL) && got == (DWORD)sz.QuadPart;
    CloseHandle(h);
    if (!ok) {
        free(buf);
        return 0;
    }
    *out = buf;
    *out_len = got;
    return 1;
}

static int ProtectBlob(const uint8_t *plain, DWORD plain_len,
                       DATA_BLOB *out)
{
    if (!plain || plain_len == 0 || !out) return 0;
    DATA_BLOB in;
    in.pbData = (BYTE *)plain;
    in.cbData = plain_len;
    memset(out, 0, sizeof *out);
    return CryptProtectData(&in, L"Lunar enrolled pin cache", NULL, NULL, NULL,
                            CRYPTPROTECT_UI_FORBIDDEN, out) ? 1 : 0;
}

static int UnprotectBlob(const uint8_t *cipher, DWORD cipher_len,
                         DATA_BLOB *out)
{
    if (!cipher || cipher_len == 0 || !out) return 0;
    DATA_BLOB in;
    in.pbData = (BYTE *)cipher;
    in.cbData = cipher_len;
    memset(out, 0, sizeof *out);
    return CryptUnprotectData(&in, NULL, NULL, NULL, NULL,
                              CRYPTPROTECT_UI_FORBIDDEN, out) ? 1 : 0;
}

static void ApplyStrictAcl(const wchar_t *path)
{
    HANDLE tok = NULL;
    TOKEN_USER *tu = NULL;
    DWORD need = 0;
    PSID system_sid = NULL;
    PACL acl = NULL;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) goto done;
    GetTokenInformation(tok, TokenUser, NULL, 0, &need);
    tu = (TOKEN_USER *)malloc(need);
    if (!tu) goto done;
    if (!GetTokenInformation(tok, TokenUser, tu, need, &need)) goto done;

    SID_IDENTIFIER_AUTHORITY nt_auth = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(&nt_auth, 1, SECURITY_LOCAL_SYSTEM_RID,
                                  0, 0, 0, 0, 0, 0, 0, &system_sid)) {
        system_sid = NULL;
    }

    EXPLICIT_ACCESSW ea[2];
    memset(ea, 0, sizeof ea);
    ea[0].grfAccessPermissions = GENERIC_READ | GENERIC_WRITE | DELETE | READ_CONTROL;
    ea[0].grfAccessMode = SET_ACCESS;
    ea[0].grfInheritance = NO_INHERITANCE;
    ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea[0].Trustee.TrusteeType = TRUSTEE_IS_USER;
    ea[0].Trustee.ptstrName = (LPWSTR)tu->User.Sid;

    DWORD count = 1;
    if (system_sid) {
        ea[1].grfAccessPermissions = GENERIC_ALL;
        ea[1].grfAccessMode = SET_ACCESS;
        ea[1].grfInheritance = NO_INHERITANCE;
        ea[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea[1].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        ea[1].Trustee.ptstrName = (LPWSTR)system_sid;
        count = 2;
    }
    if (SetEntriesInAclW(count, ea, NULL, &acl) == ERROR_SUCCESS) {
        DWORD rc = SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            NULL, NULL, acl, NULL);
        if (rc != ERROR_SUCCESS) {
            Log_Append("pinstore: strict ACL apply failed (err=%lu)",
                       (unsigned long)rc);
        }
    }

done:
    if (acl) LocalFree(acl);
    if (system_sid) FreeSid(system_sid);
    free(tu);
    if (tok) CloseHandle(tok);
}

static int WriteProtectedFile(const wchar_t *path, const uint8_t *buf, DWORD len)
{
    wchar_t tmp[MAX_PATH];
    if (_snwprintf_s(tmp, MAX_PATH, _TRUNCATE, L"%ls.tmp", path) < 0) return 0;
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD wrote = 0;
    int ok = WriteFile(h, buf, len, &wrote, NULL) && wrote == len;
    if (ok) ok = FlushFileBuffers(h) ? 1 : 0;
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp);
        return 0;
    }
    ApplyStrictAcl(tmp);
    if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp);
        return 0;
    }
    ApplyStrictAcl(path);
    return 1;
}

static int SerializeLocked(char **out_text, DWORD *out_len)
{
    size_t cap = 4096 + g_entry_count * 512;
    char *buf = (char *)malloc(cap);
    if (!buf) return 0;
    size_t pos = 0;
    int n = _snprintf(buf + pos, cap - pos, "LUNAR_PINSTORE|%d\n", PINSTORE_VERSION);
    if (n < 0 || (size_t)n >= cap - pos) { free(buf); return 0; }
    pos += (size_t)n;
    for (size_t i = 0; i < g_entry_count; i++) {
        PinEntry *e = &g_entries[i];
        n = _snprintf(buf + pos, cap - pos,
            "E|%s|%s|%s|%u|%s|%s|%s|%s|%lld|%lld|%lld|%s\n",
            KindName(e->kind), e->label, e->hostname, (unsigned)e->port,
            e->operator_family, e->rec.spki_hex,
            e->rec.not_before, e->rec.not_after,
            (long long)e->rec.not_before_unix,
            (long long)e->rec.not_after_unix,
            (long long)e->rec.renewal_due_unix,
            e->rec.last_status);
        if (n < 0 || (size_t)n >= cap - pos) { free(buf); return 0; }
        pos += (size_t)n;
    }
    *out_text = buf;
    *out_len = (DWORD)pos;
    return 1;
}

static int ParseLine(char *line)
{
    if (strncmp(line, "E|", 2) != 0) return 1;
    char *fields[12];
    size_t n = 0;
    char *p = line + 2;
    while (n < 12) {
        fields[n++] = p;
        char *sep = strchr(p, '|');
        if (!sep) break;
        *sep = 0;
        p = sep + 1;
    }
    if (n != 12) return 0;
    if (g_entry_count >= PINSTORE_MAX_ENTRIES) return 0;

    PinEntry e;
    memset(&e, 0, sizeof e);
    e.kind = ParseKind(fields[0]);
    if (!e.kind) return 0;
    if (!SafeCopy(e.label, sizeof e.label, fields[1])) return 0;
    if (!SafeCopy(e.hostname, sizeof e.hostname, fields[2])) return 0;
    int port = atoi(fields[3]);
    if (port <= 0 || port > 65535) return 0;
    e.port = (uint16_t)port;
    if (!SafeCopy(e.operator_family, sizeof e.operator_family, fields[4])) return 0;
    if (!HexToBytes32(fields[5], e.rec.spki)) return 0;
    if (!SafeCopy(e.rec.spki_hex, sizeof e.rec.spki_hex, fields[5])) return 0;
    if (!SafeCopy(e.rec.not_before, sizeof e.rec.not_before, fields[6])) return 0;
    if (!SafeCopy(e.rec.not_after, sizeof e.rec.not_after, fields[7])) return 0;
    e.rec.not_before_unix = _strtoi64(fields[8], NULL, 10);
    e.rec.not_after_unix = _strtoi64(fields[9], NULL, 10);
    e.rec.renewal_due_unix = _strtoi64(fields[10], NULL, 10);
    if (!SafeCopy(e.rec.last_status, sizeof e.rec.last_status, fields[11])) return 0;
    e.rec.present = 1;

    g_entries[g_entry_count++] = e;
    return 1;
}

static int ParsePlaintextLocked(char *text, DWORD len)
{
    if (!text || len == 0) return 0;
    g_entry_count = 0;
    const char header[] = "LUNAR_PINSTORE|1\n";
    size_t header_len = sizeof header - 1;
    if (len < header_len || strncmp(text, header, header_len) != 0) return 0;
    char *p = text + header_len;
    while (*p) {
        char *line = p;
        char *nl = strchr(line, '\n');
        if (!nl) return 0;
        *nl = 0;
        if (*line && !ParseLine(line)) return 0;
        p = nl + 1;
    }
    return 1;
}

static int PersistLocked(void)
{
    wchar_t path[MAX_PATH];
    if (!Lunar_AppDataPathW(path, MAX_PATH, L"pins.dat")) {
        Log_Append("pinstore: cannot build cache path; pins not persisted");
        return 0;
    }

    char *plain = NULL;
    DWORD plain_len = 0;
    if (!SerializeLocked(&plain, &plain_len)) {
        Log_Append("pinstore: serialize failed; pins not persisted");
        return 0;
    }
    DATA_BLOB protected_blob;
    if (!ProtectBlob((const uint8_t *)plain, plain_len, &protected_blob)) {
        Log_Append("pinstore: DPAPI protect failed (err=%lu)",
                   (unsigned long)GetLastError());
        free(plain);
        return 0;
    }
    free(plain);

    Log_Append("pinstore: atomic write start (%u protected bytes)",
               (unsigned)protected_blob.cbData);
    int ok = WriteProtectedFile(path, protected_blob.pbData, protected_blob.cbData);
    if (ok) Log_Append("pinstore: atomic write success (%u endpoints)",
                       (unsigned)g_entry_count);
    else Log_Append("pinstore: atomic write failed (err=%lu)",
                    (unsigned long)GetLastError());
    LocalFree(protected_blob.pbData);
    return ok;
}

int PinStore_Init(void)
{
    EnsureCs();
    EnterCriticalSection(&g_cs);
    if (g_loaded) {
        LeaveCriticalSection(&g_cs);
        return 1;
    }
    g_entry_count = 0;

    wchar_t path[MAX_PATH];
    if (!Lunar_AppDataPathW(path, MAX_PATH, L"pins.dat")) {
        Log_Append("pinstore: cache path unavailable; enrollment required");
        g_loaded = 1;
        LeaveCriticalSection(&g_cs);
        return 1;
    }

    uint8_t *cipher = NULL;
    DWORD cipher_len = 0;
    if (!ReadWholeFile(path, &cipher, &cipher_len)) {
        Log_Append("pinstore: cache missing; first-run enrollment required");
        g_loaded = 1;
        LeaveCriticalSection(&g_cs);
        return 1;
    }

    DATA_BLOB plain;
    if (!UnprotectBlob(cipher, cipher_len, &plain)) {
        Log_Append("pinstore: DPAPI decrypt failed (err=%lu); ignoring cache",
                   (unsigned long)GetLastError());
        free(cipher);
        g_loaded = 1;
        LeaveCriticalSection(&g_cs);
        return 1;
    }
    free(cipher);

    char *text = (char *)malloc(plain.cbData + 1);
    if (!text) {
        LocalFree(plain.pbData);
        g_loaded = 1;
        LeaveCriticalSection(&g_cs);
        return 1;
    }
    memcpy(text, plain.pbData, plain.cbData);
    text[plain.cbData] = 0;
    int ok = ParsePlaintextLocked(text, plain.cbData);
    free(text);
    LocalFree(plain.pbData);

    if (ok) {
        Log_Append("pinstore: decrypt/parse success (%u endpoints)",
                   (unsigned)g_entry_count);
    } else {
        g_entry_count = 0;
        Log_Append("pinstore: parse/schema failure; ignoring cache and re-enrolling");
    }
    g_loaded = 1;
    LeaveCriticalSection(&g_cs);
    return 1;
}

void PinStore_Shutdown(void)
{
}

int PinStore_GetPin(PinEndpointKind kind,
                    const char *label,
                    const char *hostname,
                    uint16_t port,
                    PinRecord *out)
{
    if (out) memset(out, 0, sizeof *out);
    if (!label || !hostname || !port) return 0;
    PinStore_Init();
    EnterCriticalSection(&g_cs);
    PinEntry *e = FindEntryLocked(kind, label, hostname, port);
    if (e && out) *out = e->rec;
    int present = e && e->rec.present;
    LeaveCriticalSection(&g_cs);
    return present ? 1 : 0;
}

int PinStore_ShouldRenew(const PinRecord *rec)
{
    if (!rec || !rec->present) return 1;
    if (rec->renewal_due_unix <= 0) return 1;
    int64_t now = NowUnixSeconds();
    return now <= 0 || now >= rec->renewal_due_unix;
}

int PinStore_IsExpired(const PinRecord *rec)
{
    if (!rec || !rec->present) return 1;
    if (rec->not_after_unix <= 0) return 1;
    int64_t now = NowUnixSeconds();
    return now > 0 && now >= rec->not_after_unix;
}

int PinStore_SavePin(PinEndpointKind kind,
                     const char *label,
                     const char *hostname,
                     uint16_t port,
                     const char *operator_family,
                     const uint8_t spki[32],
                     const char spki_hex[65],
                     const char *not_before,
                     const char *not_after,
                     int64_t not_before_unix,
                     int64_t not_after_unix,
                     const char *status)
{
    if (!label || !hostname || !port || !spki || !spki_hex ||
        !not_before || !not_after || !status) {
        return 0;
    }
    PinStore_Init();
    EnterCriticalSection(&g_cs);
    PinEntry *e = FindEntryLocked(kind, label, hostname, port);
    if (!e) {
        if (g_entry_count >= PINSTORE_MAX_ENTRIES) {
            LeaveCriticalSection(&g_cs);
            Log_Append("pinstore: cannot save %s:%s; cache full",
                       KindName(kind), label);
            return 0;
        }
        e = &g_entries[g_entry_count++];
        memset(e, 0, sizeof *e);
        e->kind = kind;
        e->port = port;
        SafeCopy(e->label, sizeof e->label, label);
        SafeCopy(e->hostname, sizeof e->hostname, hostname);
        SafeCopy(e->operator_family, sizeof e->operator_family,
                 operator_family ? operator_family : "unknown");
    }

    e->rec.present = 1;
    memcpy(e->rec.spki, spki, 32);
    SafeCopy(e->rec.spki_hex, sizeof e->rec.spki_hex, spki_hex);
    SafeCopy(e->rec.not_before, sizeof e->rec.not_before, not_before);
    SafeCopy(e->rec.not_after, sizeof e->rec.not_after, not_after);
    e->rec.not_before_unix = not_before_unix;
    e->rec.not_after_unix = not_after_unix;
    e->rec.renewal_due_unix = ComputeRenewalDue(not_before_unix, not_after_unix);
    SafeCopy(e->rec.last_status, sizeof e->rec.last_status, status);

    Log_Append("pinstore: save %s:%s host=%s port=%u family=%s notAfter=%s renewalDue=%lld status=%s spki=%s",
               KindName(kind), e->label, e->hostname, (unsigned)e->port,
               e->operator_family, e->rec.not_after,
               (long long)e->rec.renewal_due_unix,
               e->rec.last_status, e->rec.spki_hex);
    int ok = PersistLocked();
    LeaveCriticalSection(&g_cs);
    return ok;
}

#ifdef LUNAR_TESTING
void PinStore_TestReset(void)
{
    EnsureCs();
    EnterCriticalSection(&g_cs);
    memset(g_entries, 0, sizeof g_entries);
    g_entry_count = 0;
    g_loaded = 0;
    LeaveCriticalSection(&g_cs);
}
#endif