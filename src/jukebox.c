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
#include "jukebox.h"

#include "crashlog.h"
#include "font.h"
#include "joystick.h"
#include "keyboard.h"
#include "lds_play.h"
#include "loudness.h"
#include "mtrand.h"
#include "nortsong.h"
#include "opentyr.h"
#include "palette.h"
#include "sprite.h"
#include "starlib.h"
#include "vga_palette.h"
#include "video.h"

#include <stdio.h>

// Overlay the 1x text layer (only its non-zero glyph pixels) onto the hi-res star
// buffer as scale x scale blocks, so the small-font text stays crisp on top of the
// supersampled stars. Mirrors the game's HUD block-expand into the hi frame.
static void juke_overlay_text(SDL_Surface *text1x, SDL_Surface *hi, int scale)
{
	for (int y = 0; y < text1x->h; ++y)
	{
		const Uint8 *sp = (const Uint8 *)text1x->pixels + y * text1x->pitch;
		Uint8 *drow = (Uint8 *)hi->pixels + (y * scale) * hi->pitch;
		for (int x = 0; x < text1x->w; ++x)
		{
			const Uint8 c = sp[x];
			if (c == 0)
				continue;
			Uint8 *d = drow + x * scale;
			for (int ky = 0; ky < scale; ++ky, d += hi->pitch)
				for (int kx = 0; kx < scale; ++kx)
					d[kx] = c;
		}
	}
}

void jukebox(void)  // FKA Setup.jukeboxGo
{
	bool trigger_quit = false,  // true when user wants to quit
	     quitting = false;

	bool hide_text = false;

	crashlog_set_phase("jukebox");

	bool fade_looped_songs = true, fading_song = false;
	bool stopped = false;

	bool fx = false;
	int fx_num = 0;

	int palette_fade_steps = 15;

	int diff[256][3];
	init_step_fade_palette(diff, vga_palette, 0, 255);

	JE_starlib_init();

	int fade_volume = tyrMusicVolume;

	// Render at the display rate: star movement is scaled to fractional classic
	// ~70Hz ticks, while per-tick logic (fades) runs on accumulated whole ticks.
	const Uint64 perf_freq = SDL_GetPerformanceFrequency();
	Uint64 last_frame = SDL_GetPerformanceCounter();
	float tick_acc = 0.0f;

	// If sub-pixel supersampling is on, render the starfield into a hi-res buffer
	// and present it through the same downscaling path the game uses, so the flying
	// stars/sparks glide smoothly instead of stepping whole pixels. The video scaler
	// can't change from inside the jukebox, so the factor is fixed for the session.
	SDL_Surface *juke_hi = NULL;
	{
		const int ss = effective_supersample();
		if (ss > 1)
			juke_hi = SDL_CreateRGBSurface(0, vga_width * ss, vga_height * ss, 8, 0, 0, 0, 0);
	}
	const int scale = juke_hi != NULL ? juke_hi->w / vga_width : 1;

	for (; ; )
	{
		Uint64 now = SDL_GetPerformanceCounter();
		float step = (float)((double)(now - last_frame) * 1000.0 / perf_freq) / get_delay_period();
		last_frame = now;
		if (step > 4.0f)
			step = 4.0f;  // don't jump after a stall (window drag, starlib pause)

		tick_acc += step;
		int whole_ticks = (int)tick_acc;
		tick_acc -= whole_ticks;

		if (!stopped && !audio_disabled)
		{
			if (songlooped && fade_looped_songs)
				fading_song = true;

			if (fading_song)
			{
				for (int t = 0; t < whole_ticks && fading_song; ++t)
				{
					if (fade_volume > 5)
					{
						fade_volume -= 2;
					}
					else
					{
						fade_volume = tyrMusicVolume;

						fading_song = false;
					}
				}

				set_volume(fade_volume, fxVolume);
			}

			if (!playing || (songlooped && fade_looped_songs && !fading_song))
				play_song(mt_rand() % MUSIC_NUM);
		}

		SDL_FillRect(VGAScreenSeg, NULL, 0);
		if (scale > 1)
			SDL_FillRect(juke_hi, NULL, 0);

		// starlib input needs to be rewritten
		JE_starlib_main(step, scale > 1 ? juke_hi : VGAScreen, scale);

		push_joysticks_as_keyboard();
		service_SDL_events(true);

#if defined(__SWITCH__) || defined(__vita__)
		// Y (Switch) / Square (Vita) toggles the text overlay, mirroring F/SPACE.
		// Raw button read with local edge state, because a controller only feeds
		// confirm/cancel/directions into the jukebox (push_joysticks_as_keyboard)
		// and this button is bound to none of those. poll_joysticks (inside
		// push_joysticks_as_keyboard, just above) already ran SDL_JoystickUpdate
		// this tick. Both consoles happen to use raw id 3 (switch-sdl2: 3 = Y;
		// Vita: 3 = Square).
		{
			static bool hide_btn_was;
			const bool down = joysticks > 0 && joystick[0].handle != NULL &&
			                  SDL_JoystickGetButton(joystick[0].handle, 3) != 0;
			if (down && !hide_btn_was)
				hide_text = !hide_text;
			hide_btn_was = down;
		}
#endif

		if (!hide_text)
		{
			char buffer[60];
			
			if (fx)
				snprintf(buffer, sizeof(buffer), "%d %s", fx_num + 1, soundTitle[fx_num]);
			else
				snprintf(buffer, sizeof(buffer), "%d %s", song_playing + 1, musicTitle[song_playing]);
			
			const int x = VGAScreen->w / 2;
			
			draw_font_hv(VGAScreen, x, 170, "Press ESC to quit the jukebox.",           small_font, centered, 1, 0);
			draw_font_hv(VGAScreen, x, 180, "Arrow keys change the song being played.", small_font, centered, 1, 0);
			draw_font_hv(VGAScreen, x, 190, buffer,                                     small_font, centered, 1, 4);
		}

		for (int t = 0; t < whole_ticks && palette_fade_steps > 0; ++t)
			step_fade_palette(diff, palette_fade_steps--, 0, 255);

		if (scale > 1)
		{
			// Stars are already in juke_hi; lay the crisp 1x text on top, then let
			// present_hi() palette-convert and downscale the supersampled frame.
			juke_overlay_text(VGAScreen, juke_hi, scale);
			present_hi(juke_hi);
		}
		else
		{
			JE_showVGA();
		}

		if (!output_vsync)
			limit_render_fps();

		// quit on mouse click
		Uint16 x, y;
		if (JE_mousePosition(&x, &y) > 0)
			trigger_quit = true;

		if (newkey)
		{
			switch (lastkey_scan)
			{
			case SDL_SCANCODE_ESCAPE: // quit jukebox
			case SDL_SCANCODE_Q:
				trigger_quit = true;
				break;

			case SDL_SCANCODE_SPACE:
			case SDL_SCANCODE_F:
				hide_text = !hide_text;
				break;

			case SDL_SCANCODE_V: // toggle song fade (was F, which now hides the text)
				fading_song = !fading_song;
				break;
			case SDL_SCANCODE_N:
				fade_looped_songs = !fade_looped_songs;
				break;

			case SDL_SCANCODE_SLASH: // switch to sfx mode
				fx = !fx;
				break;
			case SDL_SCANCODE_COMMA:
				if (fx && --fx_num < 0)
					fx_num = SOUND_COUNT - 1;
				break;
			case SDL_SCANCODE_PERIOD:
				if (fx && ++fx_num >= SOUND_COUNT)
					fx_num = 0;
				break;
			case SDL_SCANCODE_SEMICOLON:
				if (fx)
					JE_playSampleNum(fx_num + 1);
				break;

			case SDL_SCANCODE_LEFT:
			case SDL_SCANCODE_UP:
				play_song((song_playing > 0 ? song_playing : MUSIC_NUM) - 1);
				stopped = false;
				break;
			case SDL_SCANCODE_RETURN:
			case SDL_SCANCODE_RIGHT:
			case SDL_SCANCODE_DOWN:
				play_song((song_playing + 1) % MUSIC_NUM);
				stopped = false;
				break;
			case SDL_SCANCODE_S: // stop song
				stop_song();
				stopped = true;
				break;
			case SDL_SCANCODE_R: // restart song
				restart_song();
				stopped = false;
				break;

			default:
				break;
			}
		}
		
		// user wants to quit, start fade-out
		if (trigger_quit && !quitting)
		{
			palette_fade_steps = 15;
			
			SDL_Color black = { 0, 0, 0 };
			init_step_fade_solid(diff, black, 0, 255);
			
			quitting = true;
		}
		
		// if fade-out finished, we can finally quit
		if (quitting && palette_fade_steps == 0)
			break;
	}

	set_volume(tyrMusicVolume, fxVolume);

	if (juke_hi != NULL)
		SDL_FreeSurface(juke_hi);
}
