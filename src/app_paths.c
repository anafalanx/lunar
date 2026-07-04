// app_paths.c -- shared application path helpers.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>

#include "app_paths.h"

int Lunar_AppDataPathW(wchar_t *out, size_t out_len,
                       const wchar_t *leaf_name)
{
    if (!out || out_len == 0) return 0;
    out[0] = 0;

    wchar_t dir[MAX_PATH] = { 0 };

    // LUNAR_DATA_DIR, when set and non-empty, replaces the default
    // %APPDATA%\Lunar base directory entirely. Tests use this to
    // redirect persistence away from the real user profile.
    DWORD got = GetEnvironmentVariableW(L"LUNAR_DATA_DIR", dir, MAX_PATH);
    if (got >= MAX_PATH) return 0;   // set but too long: fail, don't fall back
    if (got == 0) {
        wchar_t appdata[MAX_PATH] = { 0 };
        got = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
        if (got == 0 || got >= MAX_PATH) return 0;

        if (_snwprintf_s(dir, MAX_PATH, _TRUNCATE, L"%ls\\Lunar", appdata) < 0) {
            return 0;
        }
    }

    DWORD attr = GetFileAttributesW(dir);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryW(dir, NULL)) {
            if (GetLastError() != ERROR_ALREADY_EXISTS) return 0;
            attr = GetFileAttributesW(dir);
            if (attr == INVALID_FILE_ATTRIBUTES ||
                (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) return 0;
        }
    } else if ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return 0;
    }

    if (!leaf_name || !*leaf_name) {
        if (_snwprintf_s(out, out_len, _TRUNCATE, L"%ls", dir) < 0) {
            out[0] = 0;
            return 0;
        }
        return 1;
    }

    if (_snwprintf_s(out, out_len, _TRUNCATE, L"%ls\\%ls", dir, leaf_name) < 0) {
        out[0] = 0;
        return 0;
    }
    return 1;
}

int Lunar_WriteFileAtomicW(const wchar_t *path, const void *data, size_t len)
{
    if (!path || !path[0]) return 0;

    wchar_t tmp[MAX_PATH + 8];
    if (_snwprintf_s(tmp, MAX_PATH + 8, _TRUNCATE, L"%ls.tmp", path) < 0) {
        return 0;
    }

    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    DWORD written = 0;
    BOOL ok = TRUE;
    if (len > 0) {
        ok = WriteFile(h, data, (DWORD)len, &written, NULL) &&
             written == (DWORD)len;
    }
    if (ok) ok = FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp);
        return 0;
    }

    if (!MoveFileExW(tmp, path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp);
        return 0;
    }
    return 1;
}