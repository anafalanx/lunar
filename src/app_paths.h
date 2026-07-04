// app_paths.h -- shared application path helpers.

#ifndef LUNAR_APP_PATHS_H
#define LUNAR_APP_PATHS_H

#include <stddef.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

// Build %APPDATA%\Lunar\leaf_name into out and ensure %APPDATA%\Lunar
// exists. If leaf_name is NULL or empty, returns the directory path.
// Returns 1 on success; returns 0 and clears out on failure/truncation.
//
// If the LUNAR_DATA_DIR environment variable is set and non-empty, it
// replaces the %APPDATA%\Lunar base directory entirely (the directory
// is still auto-created). This exists so tests can redirect all
// persistence (settings.dat, window.dat, pins.dat, ...) to a per-run
// temp directory instead of the real user profile.
int Lunar_AppDataPathW(wchar_t *out, size_t out_len,
                       const wchar_t *leaf_name);

#ifdef __cplusplus
}
#endif

#endif