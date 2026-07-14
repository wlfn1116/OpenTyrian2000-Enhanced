/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 * Copyright (C) 2007-2009  The OpenTyrian Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include "keyboard.h"

#include "config.h"
#include "crashlog.h"
#include "joystick.h"
#include "mouse.h"
#include "network.h"
#include "opentyr.h"
#include "console_platform.h"
#include "video.h"
#include "video_scale.h"

#include "SDL.h"

#include <stdio.h>

#if defined(__SWITCH__) || defined(__vita__)
// Touch-drag -> ship travel multiplier. The base 4.0 cancels VT_MOUSE_SENS (0.25) so at the
// slider's middle the ship tracks the finger 1:1; the Touch Sensitivity slider scales it
// linearly around TOUCH_SENS_DEFAULT. notes.md §Console ports.
#define SWITCH_TOUCH_SHIP_SENS_BASE 4.0f
#endif

// Defined on every platform so the shared menu code (opentyr.c setup, mainint.c pause) links;
// only the Switch touch handler below reads it. See keyboard.h.
int touch_sensitivity = TOUCH_SENS_DEFAULT;

JE_boolean ESCPressed;

JE_boolean newkey, newmouse, keydown, mousedown;
SDL_Scancode lastkey_scan;
SDL_Keymod lastkey_mod;
Uint8 lastmouse_but;
Sint32 lastmouse_x, lastmouse_y;
JE_boolean mouse_pressed[4] = {false, false, false, false};
Sint32 mouse_x, mouse_y;
Sint32 mouse_scroll;  // accumulated wheel delta since the last clear_new poll

bool windowHasFocus;

Uint8 keysactive[SDL_NUM_SCANCODES];

bool new_text;
char last_text[SDL_TEXTINPUTEVENT_TEXT_SIZE];

static bool mouseRelativeEnabled;

// Relative mouse position in window coordinates.
static Sint32 mouseWindowXRelative;
static Sint32 mouseWindowYRelative;

void flush_events_buffer(void)
{
	SDL_Event ev;

	while (SDL_PollEvent(&ev));
}

void wait_input(JE_boolean keyboard, JE_boolean mouse, JE_boolean joystick)
{
	service_SDL_events(false);
	while (!((keyboard && keydown) || (mouse && mousedown) || (joystick && joydown)))
	{
		SDL_Delay(SDL_POLL_INTERVAL);
		push_joysticks_as_keyboard();
		service_SDL_events(false);

#ifdef WITH_NETWORK
		if (isNetworkGame)
			network_check();
#endif
	}
}

void wait_noinput(JE_boolean keyboard, JE_boolean mouse, JE_boolean joystick)
{
	service_SDL_events(false);
	while ((keyboard && keydown) || (mouse && mousedown) || (joystick && joydown))
	{
		SDL_Delay(SDL_POLL_INTERVAL);
		poll_joysticks();
		service_SDL_events(false);

#ifdef WITH_NETWORK
		if (isNetworkGame)
			network_check();
#endif
	}
}

void init_keyboard(void)
{
	//SDL_EnableKeyRepeat(500, 60); TODO Find if SDL2 has an equivalent.

	newkey = newmouse = false;
	keydown = mousedown = false;

	SDL_ShowCursor(SDL_FALSE);

#if SDL_VERSION_ATLEAST(2, 26, 0)
	SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "1");
#endif

#if defined(__SWITCH__) || defined(__vita__)
	// Handle touch explicitly (see service_SDL_events); don't also let SDL synthesise
	// mouse events from it, which would double-count and teleport on each re-touch.
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
}

bool mouseGetRelative(void)
{
	return mouseRelativeEnabled;
}

void mouseSetRelative(bool enable)
{
	SDL_SetRelativeMouseMode(enable && windowHasFocus);

	mouseRelativeEnabled = enable;

	mouseWindowXRelative = 0;
	mouseWindowYRelative = 0;
}

JE_word JE_mousePosition(JE_word *mouseX, JE_word *mouseY)
{
	service_SDL_events(false);
	*mouseX = mouse_x;
	*mouseY = mouse_y;
	return mousedown ? lastmouse_but : 0;
}

void mouseGetRelativePosition(Sint32 *const out_x, Sint32 *const out_y)
{
	service_SDL_events(false);

	scaleWindowDistanceToScreen(&mouseWindowXRelative, &mouseWindowYRelative);
	*out_x = mouseWindowXRelative;
	*out_y = mouseWindowYRelative;

	mouseWindowXRelative = 0;
	mouseWindowYRelative = 0;
}

// Float variant of mouseGetRelativePosition: no rounding, so it can be sampled per
// render frame (e.g. the variable-timestep ship) without losing sub-pixel/diagonal motion.
void mouseGetRelativeMotionF(float *const out_x, float *const out_y)
{
	service_SDL_events(false);

	float x = (float)mouseWindowXRelative;
	float y = (float)mouseWindowYRelative;
	scaleWindowDistanceToScreenF(&x, &y);
	*out_x = x;
	*out_y = y;

	mouseWindowXRelative = 0;
	mouseWindowYRelative = 0;
}

void service_SDL_events(JE_boolean clear_new)
{
	SDL_Event ev;

	watchdog_heartbeat();  // main-loop progress marker; a stall here trips the hang watchdog

	// Recover from a resolution change (fullscreen toggle, scaler resize, Switch
	// dock/undock) that the current game state won't redraw for on its own: every
	// input-wait loop pumps events through here, so re-presenting the frame when the
	// window size changed stops those screens sitting frozen/black until a keypress.
	// A no-op (one size query) unless the size actually changed.
	video_repaint_if_stale(false);

	if (clear_new)
	{
		newkey = false;
		newmouse = false;
		new_text = false;
		mouse_scroll = 0;
	}

	while (SDL_PollEvent(&ev))
	{
		switch (ev.type)
		{
			case SDL_WINDOWEVENT:
				switch (ev.window.event)
				{
				case SDL_WINDOWEVENT_FOCUS_LOST:
					windowHasFocus = false;

					mouseSetRelative(mouseRelativeEnabled);
					break;

				case SDL_WINDOWEVENT_FOCUS_GAINED:
					windowHasFocus = true;

					mouseSetRelative(mouseRelativeEnabled);
					break;

				case SDL_WINDOWEVENT_RESIZED:
					video_on_win_resize();
					break;

				case SDL_WINDOWEVENT_EXPOSED:
				case SDL_WINDOWEVENT_RESTORED:
					// Window uncovered or un-minimised: contents may be lost even
					// though the size is unchanged, so force a re-present.
					video_repaint_if_stale(true);
					break;
				}
				break;

			// The renderer's backbuffer / all textures were reset (GPU context loss,
			// which some drivers do during a fullscreen transition). Re-present so the
			// window isn't left black on a state that won't redraw itself.
			case SDL_RENDER_TARGETS_RESET:
			case SDL_RENDER_DEVICE_RESET:
				video_repaint_if_stale(true);
				break;

			case SDL_KEYDOWN:
				/* <alt><enter> toggle fullscreen */
				if (ev.key.keysym.mod & KMOD_ALT && ev.key.keysym.scancode == SDL_SCANCODE_RETURN)
				{
					toggle_fullscreen();
					break;
				}

				keysactive[ev.key.keysym.scancode] = 1;

				newkey = true;
				lastkey_scan = ev.key.keysym.scancode;
				lastkey_mod = ev.key.keysym.mod;
				keydown = true;

				mouseInactive = true;
				return;

			case SDL_KEYUP:
				keysactive[ev.key.keysym.scancode] = 0;
				keydown = false;
				return;

			case SDL_MOUSEMOTION:
				mouse_x = ev.motion.x;
				mouse_y = ev.motion.y;
				mapWindowPointToScreen(&mouse_x, &mouse_y);

				if (mouseRelativeEnabled && windowHasFocus)
				{
					mouseWindowXRelative += ev.motion.xrel;
					mouseWindowYRelative += ev.motion.yrel;
				}

				// Show the OS cursor only outside the rendered frame. A pillarboxed menu
				// spans mouse_x in [-offset, vga_width - offset), so widen the bounds by the
				// centering offset, else the OS cursor reappears over the side gradients.
				{
					const int menuXOffset = video_get_menu_x_offset();
					SDL_ShowCursor(mouse_x < -menuXOffset || mouse_x >= vga_width - menuXOffset ||
					               mouse_y < 0 || mouse_y >= vga_height ? SDL_TRUE : SDL_FALSE);
				}

				if (ev.motion.xrel != 0 || ev.motion.yrel != 0)
					mouseInactive = false;
				break;

			case SDL_MOUSEBUTTONDOWN:
				mouseInactive = false;

				// fall through
			case SDL_MOUSEBUTTONUP:
				mapWindowPointToScreen(&ev.button.x, &ev.button.y);
				if (ev.type == SDL_MOUSEBUTTONDOWN)
				{
					newmouse = true;
					lastmouse_but = ev.button.button;
					lastmouse_x = ev.button.x;
					lastmouse_y = ev.button.y;
					mousedown = true;
				}
				else
				{
					mousedown = false;
				}

				int whichMB = -1;
				switch (ev.button.button)
				{
					case SDL_BUTTON_LEFT:   whichMB = 0; break;
					case SDL_BUTTON_RIGHT:  whichMB = 1; break;
					case SDL_BUTTON_MIDDLE: whichMB = 2; break;
				}
				if (whichMB < 0)
					break;

				switch (mouseSettings[whichMB])
				{
					case 1: // Fire Main Weapons
						mouse_pressed[0] = mousedown;
						break;
					case 2: // Fire Left Sidekick
						mouse_pressed[1] = mousedown;
						break;
					case 3: // Fire Right Sidekick
						mouse_pressed[2] = mousedown;
						break;
					case 4: // Fire BOTH Sidekicks
						mouse_pressed[1] = mousedown;
						mouse_pressed[2] = mousedown;
						break;
					case 5: // Change Rear Mode
						mouse_pressed[3] = mousedown;
						break;
				}
				break;

#if defined(__SWITCH__) || defined(__vita__)
			// Touchscreen. In menus (absolute mouse mode) a touch is a tap-to-click at the
			// touched point. During gameplay (relative mouse mode, set by mouseSetRelative)
			// a drag steers the ship RELATIVELY -- fed through the same relative-motion
			// channel the render-rate ship reads -- so circling a thumb anywhere circles
			// the ship, like a trackpad. tfinger coords are normalised [0,1] to the window.
			case SDL_FINGERDOWN:
			case SDL_FINGERMOTION:
			case SDL_FINGERUP:
			{
#ifdef __vita__
				// The Vita has two touch panels: front (touch device 0) and rear (device 1).
				// Ignore the rear panel entirely -- it's very easy to brush accidentally while
				// holding the console. Only the front screen drives the pointer / ship.
				if (SDL_GetNumTouchDevices() >= 2 && ev.tfinger.touchId == SDL_GetTouchDevice(1))
					break;
#endif
				int ww = 0, wh = 0;
				SDL_GetWindowSize(main_window, &ww, &wh);
				if (ww <= 0 || wh <= 0)
					break;

				if (mouseRelativeEnabled)
				{
					// Gameplay: relative "trackpad" motion. Only the drag matters; a fresh
					// touch (down) or lift (up) must not jump the ship.
					if (ev.type == SDL_FINGERMOTION && windowHasFocus)
					{
						// Scale the 1:1 base by the slider; TOUCH_SENS_DEFAULT reproduces 1:1.
						const float sens = SWITCH_TOUCH_SHIP_SENS_BASE * (float)touch_sensitivity / (float)TOUCH_SENS_DEFAULT;
						mouseWindowXRelative += (Sint32)(ev.tfinger.dx * (float)ww * sens);
						mouseWindowYRelative += (Sint32)(ev.tfinger.dy * (float)wh * sens);
					}

					// Auto-fire the main weapon while a finger is controlling the ship.
					if (ev.type == SDL_FINGERDOWN)
						mouse_pressed[0] = true;
					else if (ev.type == SDL_FINGERUP)
						mouse_pressed[0] = false;
				}
				else
				{
					// Menus: absolute tap-to-click at the touched point.
					mouse_x = (Sint32)(ev.tfinger.x * (float)ww);
					mouse_y = (Sint32)(ev.tfinger.y * (float)wh);
					mapWindowPointToScreen(&mouse_x, &mouse_y);
					mouseInactive = false;

					if (ev.type == SDL_FINGERDOWN)
					{
						newmouse = true;
						lastmouse_but = SDL_BUTTON_LEFT;
						lastmouse_x = mouse_x;
						lastmouse_y = mouse_y;
						mousedown = true;
					}
					else if (ev.type == SDL_FINGERUP)
					{
						mousedown = false;
					}
				}
				break;
			}
#endif

			case SDL_MOUSEWHEEL:
			{
				int dy = ev.wheel.y;
				if (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
					dy = -dy;
				mouse_scroll += dy;
				if (dy != 0)
					mouseInactive = false;
				break;
			}

			case SDL_TEXTINPUT:
				SDL_strlcpy(last_text, ev.text.text, COUNTOF(last_text));
				new_text = true;
				break;

			case SDL_TEXTEDITING:
				break;

			case SDL_QUIT:
				/* TODO: Call the cleanup code here. */
				exit(0);
				break;
		}
	}
}

void JE_clearKeyboard(void)
{
	// /!\ Doesn't seems important. I think. D:
}
