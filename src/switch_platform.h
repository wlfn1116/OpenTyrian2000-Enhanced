/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 *
 * Nintendo Switch (libnx / devkitA64) platform glue.
 *
 * The whole implementation is compiled out unless __SWITCH__ is defined (which
 * devkitA64 sets automatically), so this header and its translation unit are
 * inert on every other target and safe to leave in the shared source tree.
 */
#ifndef SWITCH_PLATFORM_H
#define SWITCH_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __SWITCH__

// Writable data directory on the SD card: holds tyrian.cfg, save files, and any
// user-supplied data files. Kept in one place so file.c and config.c agree.
#define SWITCH_USER_DIR   "sdmc:/switch/opentyrian2000"

// Read-only data bundled inside the .nro, mounted by romfsInit().
#define SWITCH_ROMFS_DIR  "romfs:"

// Mount romfs, ensure the SD-card user directory exists, and register cleanup.
// Call once, as early as possible in main() (before any file access).
void switch_platform_init(void);

// Show the Switch software keyboard (there is no physical keyboard). Writes the entered
// text into out (NUL-terminated). `initial` pre-fills it; `guide` is a hint shown on the
// keyboard; `max_len` caps the entry length (0 = out_size-1); `numeric` picks the number
// pad. Returns true if the user confirmed, false if they cancelled. swkbdShow blocks
// (modal) while the keyboard is up.
bool switch_swkbd(char *out, size_t out_size, size_t max_len,
                  const char *initial, const char *guide, bool numeric);

// Native output resolution for the console's current operation mode:
// 1280x720 in handheld, 1920x1080 when docked (TV). This is the size the video
// layer scans out, so the SDL window/buffer should match it 1:1 to fill the panel
// crisply without cropping. Either pointer may be NULL. Safe to call any time.
void switch_get_output_size(int *w, int *h);

#endif // __SWITCH__

#endif // SWITCH_PLATFORM_H
