/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 *
 * Sony PlayStation Vita (VitaSDK / arm-vita-eabi) platform glue.
 *
 * The whole implementation is compiled out unless __vita__ is defined (which the
 * VitaSDK gcc sets automatically), so this header and its translation unit are
 * inert on every other target and safe to leave in the shared source tree. This
 * mirrors switch_platform.h -- see console_platform.h for the neutral names the
 * shared game code actually calls.
 */
#ifndef VITA_PLATFORM_H
#define VITA_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __vita__

// Writable data directory on the memory card: holds tyrian.cfg, save files, and any
// user-supplied data files. ux0:data always exists; vita_platform_init() creates the
// subfolder. Kept in one place so file.c and config.c agree.
#define VITA_USER_DIR   "ux0:data/opentyrian2000"

// Read-only game data bundled inside the VPK, auto-mounted at app0: by the loader.
#define VITA_DATA_DIR   "app0:data"

// Ensure the ux0: user directory exists and preload the IME sysmodule used for text
// entry. Call once, as early as possible in main() (before any file access).
void vita_platform_init(void);

// Show the on-screen keyboard (there is no physical keyboard). Writes the entered text
// into out (NUL-terminated). `initial` pre-fills it; `guide`/`numeric` are accepted for
// call-site parity with switch_swkbd but the SDL Vita IME has no matching option, so
// they are currently ignored. `max_len` caps the entry length (0 = out_size-1). Returns
// true if the user confirmed, false if they cancelled. Blocks (modal) while up.
bool vita_swkbd(char *out, size_t out_size, size_t max_len,
                const char *initial, const char *guide, bool numeric);

// Native output resolution of the Vita LCD: a fixed 960x544. Either pointer may be NULL.
// (Mirrors switch_get_output_size, which varies with the dock state; the Vita panel is
// a single fixed size, so this is constant.)
void vita_get_output_size(int *w, int *h);

#endif // __vita__

#endif // VITA_PLATFORM_H
