/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 *
 * Unified entry points for the console (Nintendo Switch / Sony PS Vita) platform
 * back-ends. Each console has a small glue TU -- switch_platform.c / vita_platform.c --
 * that provides the same three services (early init, on-screen keyboard, native output
 * size). This header maps the neutral console_* names the shared game code calls onto
 * whichever back-end is being built.
 *
 * Inert on desktop: neither branch is taken, so nothing is declared and the console_*
 * names never appear (their call sites are all guarded by the same platform test).
 *
 * Shared seams in the game code use the explicit test
 *     #if defined(__SWITCH__) || defined(__vita__)
 * rather than a single umbrella macro, so a missing include can never silently drop a
 * seam on both consoles at once.
 */
#ifndef CONSOLE_PLATFORM_H
#define CONSOLE_PLATFORM_H

#if defined(__SWITCH__)

  #include "switch_platform.h"

  #define console_platform_init    switch_platform_init
  #define console_swkbd            switch_swkbd
  #define console_get_output_size  switch_get_output_size

#elif defined(__vita__)

  #include "vita_platform.h"

  #define console_platform_init    vita_platform_init
  #define console_swkbd            vita_swkbd
  #define console_get_output_size  vita_get_output_size

#endif

#endif // CONSOLE_PLATFORM_H
