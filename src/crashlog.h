/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 *
 * Crash / diagnostic logger. Writes a rich, (symbolised, if the .pdb is present)
 * report to opentyrian_log.log next to the executable whenever the game dies hard,
 * so hard-to-reproduce failures can be pinned to an exact function / file:line.
 * The report carries a timestamp, build version, the decoded fault (exception name,
 * read/write address), a register dump, the call stack, and the loaded-module map.
 *
 * install_crash_handler() covers unhandled structured exceptions (access violation,
 * divide-by-zero, ...) plus CRT-level fatals that raise no exception: abort()/failed
 * asserts, invalid-parameter, and pure-call. Call it once, early in startup.
 * Windows only; a no-op elsewhere.
 *
 * Every report also carries a game-state snapshot (mode, difficulty, both players'
 * loadout, custom-weapon / endless-run state, active cheats, and how full each on-screen
 * object pool is) so a bare stack trace becomes a reproducible scenario -- see
 * crashlog_write_game_state() in crashlog_state.c.
 */
#ifndef CRASHLOG_H
#define CRASHLOG_H

#include <stdio.h>

void install_crash_handler(void);

// Append the live game-state snapshot to an open crash report. Defined in crashlog_state.c
// (its own <windows.h>-free TU) and invoked by the crash/hang/CRT-fatal paths. Reads only
// static globals and never faults on a corrupt process; safe to call from a fault handler.
void crashlog_write_game_state(FILE *f);

// "Current phase" breadcrumb. Game code calls crashlog_set_phase() at coarse phase boundaries
// (title, shop, loading a level, in gameplay, cutscene); the crash report prints the last one
// set, giving a bare/unsymbolised stack trace context. The pointer must be a string literal or
// otherwise long-lived string -- it is read from the fault handler. Both are defined in
// crashlog_state.c; set is a trivial global store, safe and cheap to call from anywhere.
void crashlog_set_phase(const char *phase);
const char *crashlog_get_phase(void);

// Hang watchdog: catches a hard main-thread hang (infinite loop / deadlock) that the crash
// handler can't (no exception fires). Call watchdog_init() once at startup ON THE MAIN THREAD,
// and watchdog_heartbeat() from a spot the main loop hits every iteration (service_SDL_events).
// If the heartbeat stalls, the watchdog writes the stuck main-thread stack to opentyrian_log.log.
// Windows only; a no-op elsewhere.
void watchdog_init(void);
void watchdog_heartbeat(void);

// The watchdog's stall threshold in seconds: how long the main thread must make NO progress
// before a HANG report is written. Adjustable at runtime (debug menu) and persisted in config, so
// you can lower it toward the minimum to catch a brief freeze -- but note that while it's low, a
// legitimate multi-second main-thread block (level load on a slow disk, dragging/resizing the
// window) can log a false hang, overwriting whatever was in opentyrian_log.log. Default 5s. The
// value lives in crashlog_state.c so it exists on every platform, even though only the Windows
// watchdog reads it.
#define CRASHLOG_HANG_TIMEOUT_MIN     1     // 1s granularity floor (the watchdog polls every ~1s)
#define CRASHLOG_HANG_TIMEOUT_MAX     9999  // matches the 4-digit debug-menu input field; ~2.7h = "off"
#define CRASHLOG_HANG_TIMEOUT_DEFAULT 5
void crashlog_set_hang_timeout(int seconds);  // clamps to [MIN, MAX]
int  crashlog_get_hang_timeout(void);

#endif // CRASHLOG_H
