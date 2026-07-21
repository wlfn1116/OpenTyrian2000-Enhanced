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
#include "tyrian2.h"

#include "animlib.h"
#include "backgrnd.h"
#include "config.h"
#include "crashlog.h"
#include "endless.h"
#include "episodes.h"
#include "file.h"
#include "font.h"
#include "fonthand.h"
#include "game_menu.h"
#include "joystick.h"
#include "keyboard.h"
#include "lds_play.h"
#include "loudness.h"
#include "lvllib.h"
#include "menus.h"
#include "mainint.h"
#include "mouse.h"
#include "mtrand.h"
#include "network.h"
#include "nortsong.h"
#include "nortvars.h"
#include "opentyr.h"
#include "params.h"
#include "pcxload.h"
#include "pcxmast.h"
#include "picload.h"
#include "render_list.h"
#include "shots.h"
#include "sprite.h"
#include "vga256d.h"
#include "video.h"

#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Render-list verification harness: replay the captured list each clean frame
// and report any pixels that differ from the real frame. Gated off; kept for debugging.
#define RL_SELFTEST 0

inline static void blit_enemy(SDL_Surface *surface, unsigned int i, signed int x_offset, signed int y_offset, signed int sprite_offset);
static void draw_enemy_health_bars(void);

boss_bar_t boss_bar[2];

/* Level Event Data */
JE_boolean quit, loadLevelOk;

struct JE_EventRecType eventRec[EVENT_MAXIMUM]; /* [1..eventMaximum] */
JE_word levelEnemyMax;
JE_word levelEnemyFrequency;
JE_word levelEnemy[40]; /* [1..40] */

char tempStr[31];

/* Data used for ItemScreen procedure to indicate items available */
JE_byte itemAvail[9][10]; /* [1..9, 1..10] */
JE_byte itemAvailMax[9]; /* [1..9] */

// Render-rate ship movement: the displayed ship extrapolates its last per-tick velocity each
// frame and is reconciled to the 35Hz sim via the ship override. notes.md §Smooth motion.
static int ship_tick_x[2], ship_tick_y[2];   // ship position captured at the last tick
static int ship_vel_x[2], ship_vel_y[2];       // per-tick movement (cur - prev tick)
static bool ship_pred_have_tick = false;

// Fixed-timestep accumulator: each display frame presents at alpha = accumulator/period, the
// sim ticks once per full period. Perf-counter timing, not SDL_GetTicks — notes.md §Smooth motion.
static float sim_accumulator = 0.0f;
static Uint64 sim_last_counter = 0;
static Uint64 sim_perf_freq = 0;
static bool sim_timing_init = false;

float debug_interp_alpha = 0.0f;  // last presented interpolation fraction (perf overlay)

// Smoothie levels present in two passes: render_gs = persistent background plasma (per tick),
// smoothie_frame = per-frame display buffer composited on top. notes.md §Smoothie levels.
static SDL_Surface *render_gs = NULL;
static SDL_Surface *smoothie_frame = NULL;

// (Re)create a lazily-allocated 8-bit surface at scale x the logical size. A factor
// change discards the old content — fine for the plasma base: the contractive filters
// rebuild it from black within a couple of frames (masked by any level fade).
static SDL_Surface *ensure_scaled_surface(SDL_Surface **surf, int scale)
{
	const int w = vga_width * scale, h = vga_height * scale;
	if (*surf != NULL && ((*surf)->w != w || (*surf)->h != h))
	{
		SDL_FreeSurface(*surf);
		*surf = NULL;
	}
	if (*surf == NULL)
		*surf = SDL_CreateRGBSurface(0, w, h, 8, 0, 0, 0, 0);
	return *surf;
}

static SDL_Surface *get_render_gs(int scale)
{
	return ensure_scaled_surface(&render_gs, scale);
}

static SDL_Surface *get_smoothie_frame(int scale)
{
	return ensure_scaled_surface(&smoothie_frame, scale);
}

// Supersampled present path: the interpolated playfield renders NxN into pf_hi, composites into
// vga_hi with the 1x HUD block-expanded on top, presents via present_hi(). notes.md §Supersampling & video.
static SDL_Surface *pf_hi = NULL;   // NxN playfield replay target (normal levels)
static SDL_Surface *vga_hi = NULL;  // NxN final frame (playfield composite + HUD)

static bool ensure_hi_buffers(int scale)
{
	return ensure_scaled_surface(&pf_hi, scale) != NULL
	    && ensure_scaled_surface(&vga_hi, scale) != NULL;
}

static void ship_pred_on_tick(void)
{
	const int players = twoPlayerMode ? 2 : 1;
	for (int p = 0; p < players; ++p)
	{
		if (ship_pred_have_tick)
		{
			ship_vel_x[p] = player[p].x - ship_tick_x[p];
			ship_vel_y[p] = player[p].y - ship_tick_y[p];
		}
		ship_tick_x[p] = player[p].x;
		ship_tick_y[p] = player[p].y;
	}
	ship_pred_have_tick = true;
}

static int round_signed(float v)
{
	return (int)(v + (v >= 0.0f ? 0.5f : -0.5f));
}

static void update_ship_override(float alpha)
{
	// Extrapolate each ship's actual per-tick velocity (a fixed step would fight the sim). The
	// offset also carries the shadow and charge meter; sidekicks interpolate on their own.
	const int players = twoPlayerMode ? 2 : 1;
	for (int p = 0; p < players; ++p)
	{
		int vx = ship_vel_x[p], vy = ship_vel_y[p];
		// A large jump (warp, dragonwing spawn, level start) isn't real velocity;
		// snap rather than fling the ship along a bogus extrapolation.
		if (vx > 40 || vx < -40 || vy > 40 || vy < -40)
		{
			vx = 0;
			vy = 0;
		}
		// Float offset: the replay rounds it at the render scale, so a supersampled
		// ship glides on the sub-pixel grid instead of stepping whole pixels.
		rl_set_ship_override(p, vx * alpha, vy * alpha);
	}
}

// Variable-timestep (VT) player ship: the ship alone is simulated at the render rate with
// real dt while the world stays on the fixed 35Hz tick; the integrator owns it. notes.md §VT ship.
bool vt_ship = true;

#define VT_ACCEL      1.0f  // velocity gained per tick while a direction is held (orig accelXC: +1/tick)
#define VT_DIRECT     1.0f  // immediate px/tick while a direction is held (orig CURRENT_KEY_SPEED; kills momentum lag)
#define VT_FRICTION_X 1.0f  // velocity bled per tick with no x input (orig ~1/tick)
#define VT_FRICTION_Y 0.5f  // orig y friction fires every 2nd tick => half rate
#define VT_VMAX       4.0f  // velocity clamp/axis (orig: vel clamp 4); +VT_DIRECT gives ~5 total like the original
// VT_MOUSE_SENS lives in tyrian2.h: the classic per-tick mouse path shares it

static float vt_x[2], vt_y[2], vt_vx[2], vt_vy[2];
static int vt_wrote_vx[2], vt_wrote_vy[2];  // last velocity VT wrote to player[]
static bool vt_seeded[2] = { false, false };

// Raw (un-inverted) mouse motion accumulated by vt_ship_step since the last twiddle
// read; consumed by vt_ship_twiddle_dir (rationale there).
static float vt_twiddle_mx[2], vt_twiddle_my[2];

bool vt_ship_owns(void)
{
	return vt_ship
	    && smoothMotion                            // user's Graphics-menu toggle
	    && smoothScroll != 0 && frameCountMax > 0  // the render-rate present loop must run
	    && !play_demo && !record_demo && !isNetworkGame  // determinism-sensitive
	    && !twoPlayerMode && !endLevel;
}

static void vt_seed_player(int p)
{
	vt_x[p] = (float)player[p].x;
	vt_y[p] = (float)player[p].y;
	vt_vx[p] = (float)player[p].x_velocity;
	vt_vy[p] = (float)player[p].y_velocity;
	vt_wrote_vx[p] = player[p].x_velocity;
	vt_wrote_vy[p] = player[p].y_velocity;
	vt_seeded[p] = true;
}

static float vt_friction(float v, float f)
{
	if (v > 0.0f) { v -= f; if (v < 0.0f) v = 0.0f; }
	else if (v < 0.0f) { v += f; if (v > 0.0f) v = 0.0f; }
	return v;
}

void vt_ship_step(float dt)  // dt = this frame's fraction of a 35Hz tick
{
	if (dt <= 0.0f)
		return;

	const int p = 0;  // single-player slice

	if (!player[p].is_alive)
	{
		// Don't drive a dead/exploding ship; let it sit at the sim position.
		vt_seeded[p] = false;
		rl_clear_ship_override();
		return;
	}

	// (Re)seed if uninitialised or the sim moved the ship out from under us
	// (respawn, warp, link, or VT was just toggled on).
	if (!vt_seeded[p]
	    || abs(player[p].x - (int)lrintf(vt_x[p])) > 8
	    || abs(player[p].y - (int)lrintf(vt_y[p])) > 8)
		vt_seed_player(p);

	// --- Input ---
	// Directional input (keyboard/d-pad/stick) feeds momentum; mouse relative motion
	// applies directly. "Inverted controls" levels (smoothies[8]) flip the Y axis.
	const bool invert_y = smoothies[9 - 1];
	float ix = 0.0f, iy = 0.0f;   // directional input -> momentum
	float mdx = 0.0f, mdy = 0.0f; // mouse relative motion -> direct position

	if (keysactive[keySettings[KEY_SETTING_UP]])    iy -= 1.0f;
	if (keysactive[keySettings[KEY_SETTING_DOWN]])  iy += 1.0f;
	if (keysactive[keySettings[KEY_SETTING_LEFT]])  ix -= 1.0f;
	if (keysactive[keySettings[KEY_SETTING_RIGHT]]) ix += 1.0f;

	if (joysticks > 0)
	{
		poll_joystick(0);

		// Menu/pause/change-fire are edge-triggered (action_pressed) and this render-rate
		// poll consumes the edge before tick-rate JE_playerMovement can read it, so catch
		// (latch) them here. Without this the discrete "change fire" is eaten most presses.
		if (joystick[0].action_pressed[4]) ingamemenu_pressed = true;  // "menu"
		if (joystick[0].action_pressed[5]) pause_pressed = true;        // "pause"
		if (joystick[0].action_pressed[1]) changefire_pressed = true;   // "change fire"

		if (joystick[0].analog)
		{
			// Stick deflection -> proportional momentum input (clamped ~[-1,1]).
			float ax = joystick_axis_reduce(0, joystick[0].x) / 32.0f;
			float ay = joystick_axis_reduce(0, joystick[0].y) / 32.0f;
			ix += (ax < -1.0f) ? -1.0f : (ax > 1.0f) ? 1.0f : ax;
			iy += (ay < -1.0f) ? -1.0f : (ay > 1.0f) ? 1.0f : ay;
		}

		// D-pad / digital directions, read in both modes so the d-pad works even when
		// the stick is configured analog. (The combined input is clamped below.)
		if (joystick[0].direction[0]) iy -= 1.0f;  // up
		if (joystick[0].direction[2]) iy += 1.0f;  // down
		if (joystick[0].direction[3]) ix -= 1.0f;  // left
		if (joystick[0].direction[1]) ix += 1.0f;  // right
	}

	if (has_mouse)
	{
		// Float relative motion since our last frame (per-frame sampling would round
		// small/diagonal motion away); returns 0 when relative mode is off.
		float mxr = 0.0f, myr = 0.0f;
		mouseGetRelativeMotionF(&mxr, &myr);
		mdx = mxr * VT_MOUSE_SENS;
		mdy = myr * VT_MOUSE_SENS;

		// Stash the raw mouse direction for the twiddle-code detector, un-inverted —
		// the detector applies the "inverted controls" flip itself, like keyboard.
		vt_twiddle_mx[p] += mxr;
		vt_twiddle_my[p] += myr;
	}

	if (invert_y) { iy = -iy; mdy = -mdy; }

	// Clamp combined directional input (keyboard + analog stick + d-pad) to full
	// deflection so overlapping sources can't push past 1.
	if (ix < -1.0f) ix = -1.0f; else if (ix > 1.0f) ix = 1.0f;
	if (iy < -1.0f) iy = -1.0f; else if (iy > 1.0f) iy = 1.0f;

	// --- Integrate momentum (everything dt-scaled; dt==1 == one old tick) ---
	if (ix != 0.0f)
		vt_vx[p] += ix * VT_ACCEL * dt;
	else
		vt_vx[p] = vt_friction(vt_vx[p], VT_FRICTION_X * dt);

	if (iy != 0.0f)
		vt_vy[p] += iy * VT_ACCEL * dt;
	else
		vt_vy[p] = vt_friction(vt_vy[p], VT_FRICTION_Y * dt);

	if (vt_vx[p] >  VT_VMAX) vt_vx[p] =  VT_VMAX;
	if (vt_vx[p] < -VT_VMAX) vt_vx[p] = -VT_VMAX;
	if (vt_vy[p] >  VT_VMAX) vt_vy[p] =  VT_VMAX;
	if (vt_vy[p] < -VT_VMAX) vt_vy[p] = -VT_VMAX;

	// Momentum step + immediate DIRECT component + direct mouse delta. The direct term is not
	// folded into vt_vx: releasing input stops it at once, only the momentum glides out.
	// Endless SLUGGISH scales this committed displacement -- keyboard/stick momentum, the direct
	// term, AND the mouse/touch delta (touch rides mdx/mdy) all slow together. Exactly 1.0 (a no-op,
	// bit-for-bit) whenever the modifier is off, so normal play is unchanged.
	const float mscale = endlessMoveScale();
	vt_x[p] += ((vt_vx[p] + ix * VT_DIRECT) * dt + mdx) * mscale;
	vt_y[p] += ((vt_vy[p] + iy * VT_DIRECT) * dt + mdy) * mscale;

	// Endless GRAVITY WELL: a steady drag (dt-scaled, so render-rate independent). A plain well pulls
	// straight down; an omnidirectional well pulls along a fixed random heading, so it has an X
	// component too. The playfield-bounds clamp below catches any axis, so a sideways/up pull just
	// pins the ship at that edge (no phantom momentum, same as the down case).
	vt_x[p] += endlessGravityDriftX() * dt;
	vt_y[p] += endlessGravityDriftY() * dt;

	// Same playfield bounds the sim enforces (mainint.c). Stop velocity at walls
	// so we don't accumulate phantom momentum while held against an edge.
	if (vt_x[p] > PLAYFIELD_WIDTH - SHIP_RIGHT_MARGIN) { vt_x[p] = PLAYFIELD_WIDTH - SHIP_RIGHT_MARGIN; if (vt_vx[p] > 0) vt_vx[p] = 0; }
	if (vt_x[p] < SHIP_LEFT_MARGIN)                    { vt_x[p] = SHIP_LEFT_MARGIN;                    if (vt_vx[p] < 0) vt_vx[p] = 0; }
	if (vt_y[p] > SHIP_BOTTOM_MARGIN)                  { vt_y[p] = SHIP_BOTTOM_MARGIN;                  if (vt_vy[p] > 0) vt_vy[p] = 0; }
	if (vt_y[p] < SHIP_TOP_MARGIN)                     { vt_y[p] = SHIP_TOP_MARGIN;                     if (vt_vy[p] < 0) vt_vy[p] = 0; }

	// Write back so the next 35Hz tick (collisions, firing, homing) sees the
	// current position/velocity.
	player[p].x = (int)lrintf(vt_x[p]);
	player[p].y = (int)lrintf(vt_y[p]);
	player[p].x_velocity = (int)lrintf(vt_vx[p]);
	player[p].y_velocity = (int)lrintf(vt_vy[p]);
	vt_wrote_vx[p] = player[p].x_velocity;
	vt_wrote_vy[p] = player[p].y_velocity;

	// Display the recorded (tick-time) ship sprite at its new continuous position
	// via the override channel the interpolator already understands. Pass the FLOAT
	// integrator position, not the rounded player.x: under supersampling the ship
	// (and everything attached to it) then moves on the sub-pixel grid.
	rl_set_ship_override(p, vt_x[p] - (float)ship_tick_x[p], vt_y[p] - (float)ship_tick_y[p]);
}

void vt_ship_tick(void)  // once per 35Hz tick, before ship_pred_on_tick()
{
	if (!vt_ship_owns())
		return;

	const int p = 0;
	if (!vt_seeded[p] || !player[p].is_alive)
		return;

	// Hard reposition (respawn/warp/link): re-seed instead of folding.
	if (abs(player[p].x - (int)lrintf(vt_x[p])) > 8
	    || abs(player[p].y - (int)lrintf(vt_y[p])) > 8)
	{
		vt_seed_player(p);
		return;
	}

	// Fold external velocity impulses (magnet fields, knockback) into VT: they modify
	// player.x_velocity during the tick and JE_playerMovement's integration is skipped.
	vt_vx[p] += (float)(player[p].x_velocity - vt_wrote_vx[p]);
	vt_vy[p] += (float)(player[p].y_velocity - vt_wrote_vy[p]);

	// Refresh the ship position history (trailing sidekicks read old_x/old_y):
	// vanilla derives it from an intra-tick delta that VT leaves at zero.
	if (player[p].x != ship_tick_x[p] || player[p].y != ship_tick_y[p])
	{
		for (unsigned int i = 1; i < COUNTOF(player[p].old_x); ++i)
		{
			player[p].old_x[i - 1] = player[p].old_x[i];
			player[p].old_y[i - 1] = player[p].old_y[i];
		}
		player[p].old_x[COUNTOF(player[p].old_x) - 1] = player[p].x;
		player[p].old_y[COUNTOF(player[p].old_x) - 1] = player[p].y;
	}
}

// Per-tick ship movement for shots that track the ship (delta_x_shot_move, e.g. the
// laser). VT moves the ship BETWEEN ticks, so vanilla's intra-tick delta reads ~0;
// supply current pos vs the previous tick snapshot instead. (ship_tick_x/y still hold
// the previous tick here — ship_pred_on_tick updates them after JE_playerMovement.)
void vt_ship_shot_delta(int player_index, int *out_dx, int *out_dy)
{
	const int p = (player_index <= 0) ? 0 : 1;
	if (!ship_pred_have_tick)
	{
		*out_dx = 0;
		*out_dy = 0;
		return;
	}
	*out_dx = player[p].x - ship_tick_x[p];
	*out_dy = player[p].y - ship_tick_y[p];
}

// Hand the twiddle-code detector the mouse direction since the last call as a -1/0/+1
// per axis (raw / un-inverted), then reset the accumulator. vt_ship_step drains the
// mouse at render rate, so the once-per-tick detector in JE_playerMovement would
// otherwise never see a mouse direction.
void vt_ship_twiddle_dir(int player_index, int *out_dx, int *out_dy)
{
	const int p = (player_index <= 0) ? 0 : 1;
	const float deadzone = 1.0f;  // screen px of motion before a direction counts

	const float ax = vt_twiddle_mx[p], ay = vt_twiddle_my[p];
	vt_twiddle_mx[p] = 0.0f;
	vt_twiddle_my[p] = 0.0f;

	*out_dx = 0;
	*out_dy = 0;

	// Twiddle codes are cardinal sequences (diagonals are ignored), so collapse the
	// flick to its dominant axis — a hand flick is never perfectly straight, and its
	// off-axis drift would read as a diagonal. Movement itself stays in vt_ship_step.
	if (fabsf(ax) >= fabsf(ay))
	{
		if (ax > deadzone)       *out_dx =  1;
		else if (ax < -deadzone) *out_dx = -1;
	}
	else
	{
		if (ay > deadzone)       *out_dy =  1;
		else if (ay < -deadzone) *out_dy = -1;
	}
}

// Copy the freshly-drawn playfield into VGAScreenSeg, applying the special
// vertical-flip / lighting composites when active; interpolated in-between frames
// re-composite through here too.
static void composite_playfield(SDL_Surface *playfield)
{
	JE_byte *src;
	Uint8 *s = VGAScreenSeg->pixels;

	int x, y, lightx, lighty, lightdist;

	src = playfield->pixels;
	src += PLAYFIELD_LEFT;  // crop off the off-screen entry margin; see video.h

	if (starShowVGASpecialCode == 1)
	{
		src += playfield->pitch * 183;
		for (y = 0; y < 184; y++)
		{
			memmove(s, src, PLAYFIELD_WIDTH);
			s += VGAScreenSeg->pitch;
			src -= playfield->pitch;
		}
	}
	else if (starShowVGASpecialCode == 2 && processorType >= 2)
	{
		lighty = 172 - player[0].y;
		lightx = (PLAYFIELD_WIDTH - PLAYFIELD_X_SHIFT + 5) - player[0].x;

		for (y = 184; y; y--)
		{
			if (lighty > y)
			{
				for (x = PLAYFIELD_WIDTH; x; x--)
				{
					*s = (*src & 0xf0) | ((*src >> 2) & 0x03);
					s++;
					src++;
				}
			}
			else
			{
				for (x = PLAYFIELD_WIDTH; x; x--)
				{
					lightdist = abs(lightx - x) + lighty;
					if (lightdist < y)
						*s = *src;
					else if (lightdist - y <= 5)
						*s = (*src & 0xf0) | (((*src & 0x0f) + (3 * (5 - (lightdist - y)))) / 4);
					else
						*s = (*src & 0xf0) | ((*src & 0x0f) >> 2);
					s++;
					src++;
				}
			}
			s += VGAScreenSeg->pitch - PLAYFIELD_WIDTH;
			src += playfield->pitch - PLAYFIELD_WIDTH;
		}
	}
	else
	{
		for (y = 0; y < 184; y++)
		{
			memmove(s, src, PLAYFIELD_WIDTH);
			s += VGAScreenSeg->pitch;
			src += playfield->pitch;
		}
	}
}

// composite_playfield at NxN: same three modes (normal copy, vertical flip,
// spotlight) on the supersampled playfield, writing into vga_hi's playfield
// region. The spotlight math runs in HI units (every distance multiplied by
// scale), so the light circle is the same size on screen, just smoother.
static void composite_playfield_hi(SDL_Surface *playfield, SDL_Surface *out, int scale)
{
	const int width = PLAYFIELD_WIDTH * scale;
	const int rows = 184 * scale;
	const JE_byte *src = (const JE_byte *)playfield->pixels + PLAYFIELD_LEFT * scale;
	Uint8 *s = (Uint8 *)out->pixels;

	if (starShowVGASpecialCode == 1)
	{
		src += (size_t)playfield->pitch * (rows - 1);
		for (int y = 0; y < rows; y++)
		{
			memmove(s, src, width);
			s += out->pitch;
			src -= playfield->pitch;
		}
	}
	else if (starShowVGASpecialCode == 2 && processorType >= 2)
	{
		const int lighty = (172 - player[0].y) * scale;
		const int lightx = ((PLAYFIELD_WIDTH - PLAYFIELD_X_SHIFT + 5) - player[0].x) * scale;
		const int band = 5 * scale;

		for (int y = rows; y; y--)
		{
			if (lighty > y)
			{
				for (int x = width; x; x--)
				{
					*s = (*src & 0xf0) | ((*src >> 2) & 0x03);
					s++;
					src++;
				}
			}
			else
			{
				for (int x = width; x; x--)
				{
					const int lightdist = abs(lightx - x) + lighty;
					if (lightdist < y)
						*s = *src;
					else if (lightdist - y <= band)
						*s = (*src & 0xf0) | (((*src & 0x0f) + (3 * (band - (lightdist - y))) / scale) / 4);
					else
						*s = (*src & 0xf0) | ((*src & 0x0f) >> 2);
					s++;
					src++;
				}
			}
			s += out->pitch - width;
			src += playfield->pitch - width;
		}
	}
	else
	{
		for (int y = 0; y < rows; y++)
		{
			memmove(s, src, width);
			s += out->pitch;
			src += playfield->pitch;
		}
	}
}

// Block-expand the 1x HUD onto the hi frame: the right-hand HUD column for the
// playfield rows, and the full width below the playfield. The HUD is tick-drawn
// 1x art (plus the per-frame power gauge), so expanding is exact — it looks
// identical to the classic path.
static void expand_hud_to_hi(SDL_Surface *src, SDL_Surface *hi, int scale)
{
	for (int y = 0; y < vga_height; ++y)
	{
		const int x_start = (y < 184) ? PLAYFIELD_WIDTH : 0;
		const Uint8 *sp = (const Uint8 *)src->pixels + y * src->pitch + x_start;
		Uint8 *const d0 = (Uint8 *)hi->pixels + (y * scale) * hi->pitch + x_start * scale;

		Uint8 *d = d0;
		for (int x = x_start; x < vga_width; ++x, ++sp, d += scale)
			memset(d, *sp, scale);

		const int row_bytes = (vga_width - x_start) * scale;
		for (int k = 1; k < scale; ++k)
			memcpy(d0 + k * hi->pitch, d0, row_bytes);
	}
}

// Soul of Zinglon light pillar, drawn at display rate from the per-tick request (zinglonPillar*).
// cx is in HI units (already scaled); temp is 1x half-width. notes.md §Other render-rate presents.
static void draw_zinglon_pillar(SDL_Surface *surface, int cx, int temp, int scale)
{
	const int bottom = 184 * scale;
	int x0 = cx - temp * scale;
	int x1 = cx + temp * scale + (scale - 1);
	if (x0 < 0) x0 = 0;
	JE_barBright(surface, x0, 0, x1, bottom);
	x0 = cx - (temp + 2) * scale;
	x1 = cx + (temp + 2) * scale + (scale - 1);
	if (x0 < 0) x0 = 0;
	JE_barBright(surface, x0, 0, x1, bottom);
}

// Generator power bar render state: a HUD overlay on VGAScreenSeg, redrawn every presented
// frame at an interpolated level with a sub-pixel anti-aliased top edge.
static bool power_gauge_active = false;
static int power_render_prev = 0, power_render_cur = 0;  // `power` (0..900) at the prev/cur tick

static void draw_power_gauge(float power_value)
{
	enum { Y_BOTTOM = 104, BAR_MAX = 93, BASE = 113, POWER_MAX = 900 };
	// 9 pixels wide (x1..x2). The classic art drew this gauge 1px narrower than the
	// shield/armor bars; extend it right by one so all three gauges match at 9px.
	const int x1 = HUD_X(269), x2 = HUD_X(277);

	// power (0..POWER_MAX) -> bar height in pixels (0..BAR_MAX). BAR_MAX drives the full
	// height, so the bar rescales with it (classic was power/10 with a 90px BAR_MAX).
	float level = power_value * BAR_MAX / (float)POWER_MAX;
	if (level < 0.0f)
		level = 0.0f;
	else if (level > BAR_MAX)
		level = BAR_MAX;

	const int full = (int)level;          // solid pixel rows
	const float frac = level - full;      // sub-pixel remainder for the top edge
	const int dir = gaugeGradGenerator;   // GaugeGradientDir

	// Kill-fire BOON window: main-gun fire is power-free, so recolour the gauge under the same
	// condition that gates the free power. notes.md §Course generation & danger labels.
	int base = BASE;
	if (endlessMode && endlessTurbodriveActive() && !endlessKillFireIsEvil())
		base = ENDLESS_FREE_POWER_GAUGE_BASE;
	const int darkEnd = base & ~0x0F;     // bank floor: the AA top edge blends up from here

	// Clear the bar slot (its background is black, like the original shrink fill).
	fill_rectangle_xy(VGAScreenSeg, x1, Y_BOTTOM - BAR_MAX, x2, Y_BOTTOM, 0);

	if (dir == GAUGE_GRAD_LEFT || dir == GAUGE_GRAD_RIGHT)
	{
		// Horizontal gradient: each column is a fixed shade stepping across the width,
		// with the sub-pixel anti-aliased top edge applied per column. Lifted +2 shades to
		// match the slightly-brighter horizontal ramp on the shield/armor bars (in-family).
		for (int j = 0; j <= x2 - x1; j++)
		{
			const int off = (dir == GAUGE_GRAD_RIGHT) ? j : (x2 - x1 - j);
			const int shade = base + 2 + off;
			if (full >= 1)
				fill_rectangle_xy(VGAScreenSeg, x1 + j, Y_BOTTOM - full + 1, x1 + j, Y_BOTTOM, (Uint8)shade);
			if (full < BAR_MAX && frac > 0.04f)
			{
				int edgeCol = darkEnd + (int)(frac * (shade - darkEnd) + 0.5f);
				if (edgeCol > shade)
					edgeCol = shade;
				JE_pix(VGAScreenSeg, x1 + j, Y_BOTTOM - full, (Uint8)edgeCol);
			}
		}
		return;
	}

	// Vertical gradient, drawn bottom-up in same-shade bands. Up = classic (shade
	// BASE + h/7, darkest at the bottom); Down mirrors the gradient within the fill.
	for (int h = 1; h <= full; )
	{
		const int shade = (dir == GAUGE_GRAD_DOWN) ? (full - h) / 7 : h / 7;
		int h2 = h;
		while (h2 + 1 <= full &&
		       ((dir == GAUGE_GRAD_DOWN) ? (full - (h2 + 1)) / 7 : (h2 + 1) / 7) == shade)
			++h2;
		fill_rectangle_xy(VGAScreenSeg, x1, Y_BOTTOM - h2 + 1, x2, Y_BOTTOM - h + 1, (Uint8)(base + shade));
		h = h2 + 1;
	}

	// Anti-aliased leading row: dimmed toward the darkest shade by frac so the top
	// edge appears to move at sub-pixel resolution as the bar fills. In Down the top
	// band is the darkest (BASE); in Up it is the current top shade.
	if (full < BAR_MAX && frac > 0.04f)
	{
		const int barCol = (dir == GAUGE_GRAD_DOWN) ? base : (base + full / 7);
		int edgeCol = darkEnd + (int)(frac * (barCol - darkEnd) + 0.5f);
		if (edgeCol > barCol)
			edgeCol = barCol;
		fill_rectangle_xy(VGAScreenSeg, x1, Y_BOTTOM - full, x2, Y_BOTTOM - full, (Uint8)edgeCol);
	}
}

static void draw_boss_bar_present(SDL_Surface *dst, int scale, float alpha);

void JE_starShowVGA(void)
{
	if (!playerEndLevel && !skipStarShowVGA)
	{
		// Zinglon pillar at the tick position: baseline for the non-interpolated
		// present paths; the interp loop below redraws it shifted after replay.
		if (zinglonPillarActive)
			draw_zinglon_pillar(game_screen, zinglonPillarCX, zinglonPillarTemp, 1);

		composite_playfield(game_screen);

		if (smoothScroll != 0)
		{
			// Interpolation needs a real tick period. Smoothie levels present in two passes
			// (notes.md §Smoothie levels); normal levels interpolate straight into game_screen.
			const bool can_interp = frameCountMax > 0 && smoothMotion;

			// Supersample factor for this present pass (Auto follows the scaler; see
			// video.h). The hi path needs its buffers; on any allocation failure it
			// degrades to the classic 1x path — never to a missing frame.
			int rss = can_interp ? effective_supersample() : 1;
			const bool use_hi = rss > 1 && ensure_hi_buffers(rss);
			if (!use_hi)
				rss = 1;

			SDL_Surface *const interp_buf  = anySmoothies ? get_smoothie_frame(rss)
			                               : (use_hi ? pf_hi : game_screen);
			SDL_Surface *const bg_feedback = anySmoothies ? get_render_gs(rss) : NULL;

			if (can_interp && interp_buf != NULL && (!anySmoothies || bg_feedback != NULL))
			{
				// Present every display frame at alpha = accumulator/period (real
				// elapsed time); break to run the next sim tick once a full period has
				// elapsed. Leftover time carries over, keeping the sim rate exact.
				const float period = (float)frameCountMax * get_delay_period();

				if (!sim_timing_init)
				{
					sim_perf_freq = SDL_GetPerformanceFrequency();
					sim_last_counter = SDL_GetPerformanceCounter();
					sim_accumulator = 0.0f;
					sim_timing_init = true;
				}

				const float counter_to_ms = 1000.0f / (float)sim_perf_freq;

				for (;;)
				{
					const Uint64 now = SDL_GetPerformanceCounter();
					float elapsed = (float)(now - sim_last_counter) * counter_to_ms;
					sim_last_counter = now;
					if (elapsed > period * 4.0f)
						elapsed = period;  // spiral guard (lag spike / resume from pause)
					sim_accumulator += elapsed;

					// Advance the VT ship by the REAL elapsed time before the break check:
					// stepping only on rendered frames discards the elapsed time of the
					// iteration that triggers a sim tick, which reads as visible stutter
					// even at a solid 60 fps (notes.md §VT ship).
					const bool vt_owns = vt_ship_owns();
					if (vt_owns)
						vt_ship_step(elapsed / period);

					if (sim_accumulator >= period)
					{
						sim_accumulator -= period;
						if (sim_accumulator > period)
							sim_accumulator = period;  // clamp backlog: at most one tick behind
						break;  // time for the next simulation tick
					}

					const float alpha = sim_accumulator / period;
					debug_interp_alpha = alpha;  // expose for the perf overlay
					if (!vt_owns)
						update_ship_override(alpha);  // extrapolate the ship for this frame (non-VT path)

					if (anySmoothies)
					{
						// Pass 1: derive this frame's background by filtering a COPY of
						// the fixed plasma base with the interpolated backgrounds.
						memcpy(interp_buf->pixels, bg_feedback->pixels, (size_t)bg_feedback->h * bg_feedback->pitch);
						rl_replay_bg(interp_buf, alpha, rss);
						// Pass 2: composite the interpolated entities + overlays on top.
						rl_replay_fg(interp_buf, alpha, rss);
					}
					else
					{
						rl_replay_interp(interp_buf, alpha, false, rss);
					}

					// Zinglon pillar onto the freshly-interpolated frame, centred on the
					// ship's render-rate position so it glides rather than snapping.
					if (zinglonPillarActive)
						draw_zinglon_pillar(interp_buf,
						                    round_signed(((float)zinglonPillarCX + rl_get_ship_override_dx(0)) * rss),
						                    zinglonPillarTemp, rss);

					draw_boss_bar_present(interp_buf, rss, alpha);

					if (use_hi)
					{
						// NxN composite + block-expanded 1x HUD, presented through the
						// dedicated hi path (same on-screen rect as the classic path).
						composite_playfield_hi(interp_buf, vga_hi, rss);
						if (power_gauge_active)
							draw_power_gauge((float)power_render_prev + (power_render_cur - power_render_prev) * alpha);
						gauge_flash_present(alpha);
						expand_hud_to_hi(VGAScreenSeg, vga_hi, rss);
						present_hi(vga_hi);
					}
					else
					{
						composite_playfield(interp_buf);

						// Power bar at the interpolated level: rises smoothly instead of per-tick steps.
						if (power_gauge_active)
							draw_power_gauge((float)power_render_prev + (power_render_cur - power_render_prev) * alpha);
						gauge_flash_present(alpha);

						JE_showVGA();
					}

					if (!output_vsync)
						limit_render_fps();
					service_SDL_events(false);
				}
				setDelay(frameCountMax);  // keep `target` current for other timing readers
			}
			else
			{
				JE_showVGA();
				service_wait_delay();
				setDelay(frameCountMax);
			}

			// Advance the persistent plasma base one filter step to this tick's plasma,
			// the base for the next tick's interpolated frames. This is the only place
			// the smoothie feedback accumulates — exactly once per tick, like the sim.
			if (anySmoothies && bg_feedback != NULL)
				rl_replay_bg(bg_feedback, 1.0f, rss);
		}
		else
		{
			JE_showVGA();
		}
	}

	quitRequested = false;
	skipStarShowVGA = false;
}

// Expand the bottom-right playfield edge to the HUD by copying a
// 16-pixel-tall column horizontally for the additional playfield width.
static void extend_playfield_right_column(SDL_Surface* surface)
{
	const int src_x = 262;  // last column of the original playfield
	const int src_y = 184;
	const int height = 16;
	const int copy_width = surface->w - HUD_WIDTH - 263;  // pixels to extend

	Uint8* row = (Uint8*)surface->pixels + src_y * surface->pitch + src_x;
	for (int y = 0; y < height; ++y)
	{
		const Uint8 pixel = row[0];
		memset(row + 1, pixel, copy_width);
		row += surface->pitch;
	}
}

static void copy_screen_to_buffer(Uint8* buffer)
{
	Uint8* src = VGAScreen->pixels;
	for (int y = 0; y < vga_height; ++y)
	{
		memcpy(buffer, src, VGAScreen->pitch);
		buffer += VGAScreen->pitch;
		src += VGAScreen->pitch;
	}
}

static void copy_buffer_to_screen(const Uint8* buffer)
{
	Uint8* dst = VGAScreen->pixels;
	for (int y = 0; y < vga_height; ++y)
	{
		memcpy(dst, buffer, VGAScreen->pitch);
		buffer += VGAScreen->pitch;
		dst += VGAScreen->pitch;
	}
}

// Sub-pixel fraction of tempMapXOfs, set beside every tempMapXOfs assignment so enemies float
// their parallax onto the background layer's sub-pixel offset. notes.md §Sub-pixel parallax.
static float tempMapXOfs_frac = 0.0f;
// Background layer whose horizontal anchor tempMapXOfs represents (1..3). The render list uses
// this to resolve legacy draw-order cases where the entity and layer straddle the parallax update.
static int tempMapXOfs_layer = 0;

// Vertical presentation phase of the layer the current enemy scroll-tracks. Sprites and HP bars
// can be recorded on opposite sides of the integer ey advance, so each gets an explicit whole-
// pixel phase correction plus the layer's fractional phase. Keeping them separate prevents the
// scale-1 .5 rounding mismatch between negative background rows and positive enemy coordinates.
static int   tempScrollYBase = 0, tempScrollYBaseBar = 0;
static float tempScrollYfrac = 0.0f, tempScrollYfracBar = 0.0f;
static int   tempScrollYLayer = 0;
// Normal layer step/delay behind the current batch. Full-speed fixedmovey scripts can be
// scaled from the layer's exact integer delta; delay-gated scripts use a percentage carry.
static int   tempScrollBaseStep = 0, tempScrollDelayMax = 1;

// This batch's endless smooth-overclock extra scroll px (the endlessScrollExtraPxN of the layer
// these enemies ride), set beside tempBackMove so the enemy scrolls at its layer's true pace. The
// channel is tagged EXPLICITLY here rather than matched by value (tempBackMove == backMove?) --
// value-matching mis-picks when two layers momentarily share a backMove (e.g. EP1 TYRIAN's slowdown
// makes backMove == backMove3, which sent layer-3 enemies onto layer 1's much smaller delay-gated
// extra px, so they drifted off the terrain). notes.md §Endless scroll boost / §Slow-scroll smoothing.
static int tempScrollExtraPx = 0;

// Sky-bank (slots 0..24) enemies carry tempBackMove == 0: any scroll ride is authored in the
// enemy's own eyc. One moving at exactly the layer-2 step is attached scenery (EP2 GYGES's
// glass structures), detected per enemy in JE_drawEnemy so it can take the scroll modifier's
// extra layer-2 pixels and the layer's canonical sub-pixel clock instead of drifting off its
// terrain. Read by blit_enemy for the sprite's vertical binding stamps.
static bool skyGlueThisEnemy = false;

// Event records are keyed to layer-1's absolute scroll coordinate (curLoc). A boosted layer can
// cross two or more event coordinates in one simulation tick, so some records are first processed
// at curLoc > eventtime. Remember the interval crossed by the previous tick: newly-created bound
// enemies can then be advanced through just the missed fraction instead of spawning one or more
// pixels behind their terrain. The layer/base fields also let fixedmovey cancellation participate
// in that partial advance (BRAINIAC's fixed -1 shootables must remain screen-stationary).
static bool eventScrollCatchupValid = false;
static int eventScrollFrom = 0, eventScrollTo = 0;
static int eventScrollLayerDelta[4] = { 0, 0, 0, 0 };
static int eventScrollBaseStep[4] = { 0, 0, 0, 0 };
static int eventScrollDelayMax[4] = { 1, 1, 1, 1 };
static int eventScrollBoost = 0;
// Sky-glue spawns ride a DIFFERENT layer than the event clock, and the two layers quantize
// their boosted fractional rates through independent carries. Anchoring those spawns needs
// the stock glass-px-per-event-px ratio (the boost cancels out of it) and the current
// cross-layer carry phase, captured in hundredths at the end of each tick's scroll block.
static bool eventScrollSkyValid = false;
static int eventScrollSkyRatio100 = 0;
static int eventScrollSkyPhase100 = 0;

// Only the part of fixedmovey left after it cancels an opposing eyc is scroll-relative. BRAINIAC,
// for example, pairs fixed=-1 with eyc=+1 to make a zero local velocity; scaling fixed alone would
// turn that exact cancellation into a speed-dependent drift. Any excess fixed component still
// modifies the layer advance and is therefore the part that must scale with it.
static int enemy_scalable_fixed_y(int fixed_move, int own_move)
{
	if (fixed_move < 0 && own_move > 0)
		return fixed_move + own_move < 0 ? fixed_move + own_move : 0;
	if (fixed_move > 0 && own_move < 0)
		return fixed_move + own_move > 0 ? fixed_move + own_move : 0;
	return fixed_move;
}

// Scale a layer-bound fixed-motion residual in the SAME integer phase as its layer. This matters
// for the common residual == -baseStep case: those shootable/map pieces cancel the normal layer
// advance and must cancel every boosted pixel too, rather than merely averaging out later. Sky
// enemies are not layer-bound and retain stock motion. Delay-gated sections have no single
// per-tick base divisor, so they use an exact signed percentage carry instead.
static int enemy_fixed_move_y(unsigned int i)
{
	struct JE_SingleEnemyType *const e = &enemy[i];
	const int move = e->fixedmovey;
	const int scalable = enemy_scalable_fixed_y(move, e->eyc);
	const int boost = endlessScrollBoostPercent();

	if (scalable == 0 || tempScrollYLayer == 0 || boost == 0 || tempScrollBaseStep <= 0)
	{
		e->fixedmovey_carry = 0;
		e->fixedmovey_carry_base = 0;
		e->fixedmovey_carry_move = 0;
		return move;
	}

	int divisor, carry_base, numerator;
	if (tempScrollDelayMax == 1)
	{
		// Scale by the layer's ACTUAL base+extra delta this tick. For move == -baseStep,
		// integer division is exact and the sprite remains pixel-locked to the terrain.
		divisor = tempScrollBaseStep;
		carry_base = divisor;
		numerator = scalable * (tempBackMove + tempScrollExtraPx);
	}
	else
	{
		// Stock fixedmovey runs every tick even when the layer's delay gate is closed.
		// Preserve that behavior while scaling its average by the modifier exactly.
		divisor = 100;
		carry_base = -100;  // distinct from a full-speed layer whose step happens to be 100
		numerator = scalable * (100 + boost);
	}

	if (e->fixedmovey_carry_base != carry_base || e->fixedmovey_carry_move != scalable)
	{
		e->fixedmovey_carry = 0;
		e->fixedmovey_carry_base = carry_base;
		e->fixedmovey_carry_move = scalable;
	}

	numerator += e->fixedmovey_carry;
	const int scaled = numerator / divisor;  // signed truncation keeps carry bounded around zero
	e->fixedmovey_carry = numerator - scaled * divisor;
	return (move - scalable) + scaled;
}

inline static void blit_enemy(SDL_Surface *surface, unsigned int i, signed int x_offset, signed int y_offset, signed int sprite_offset)
{
	if (enemy[i].sprite2s == NULL)
	{
		fprintf(stderr, "warning: enemy %d sprite missing\n", i);
		return;
	}
	
	const int x = enemy[i].ex + x_offset + tempMapXOfs,
	          y = enemy[i].ey + y_offset;

	// enemycycle indexes egr[] 1-based; skip anything that doesn't name a real in-sheet sprite
	// instead of underflowing into a wild read in blit_sprite2 (notes.md §General pitfalls).
	const unsigned int cycle = enemy[i].enemycycle;
	if (cycle < 1 || cycle > 20)
		return;
	const unsigned int index = enemy[i].egr[cycle - 1] + sprite_offset;
	if (index == 0 || (size_t)index * sizeof(Uint16) > enemy[i].sprite2s->size)
		return;

	rl_current_id = RL_ID_ENEMY_BASE + (int)i;  // tag for cross-frame interpolation
	rl_current_par_frac = tempMapXOfs_frac;     // float the parallax to match the background
	rl_current_par_layer = tempMapXOfs_layer;
	rl_current_par_anchor = (float)(tempMapXOfs - PLAYFIELD_X_SHIFT) + tempMapXOfs_frac;
	rl_current_par_ybase = tempScrollYBase;
	if (skyGlueThisEnemy)
	{
		// Attached sky scenery: bind to layer 2's lagged clock so replay places it with the
		// exact transform the glass rows use (pre-advance phase, batch base is already 0).
		rl_current_par_yfrac = bg_layer_yfrac[2];
		rl_current_par_ylayer = 2;
	}
	else
	{
		rl_current_par_yfrac = tempScrollYfrac;
		rl_current_par_ylayer = tempScrollYLayer;
	}
	if (enemy[i].filter != 0)
		blit_sprite2_filter(surface, x, y, *enemy[i].sprite2s, index, enemy[i].filter);
	else
		blit_sprite2(surface, x, y, *enemy[i].sprite2s, index);
	rl_current_id = 0;
	rl_current_par_frac = 0.0f;
	rl_current_par_layer = 0;
	rl_current_par_anchor = 0.0f;
	rl_current_par_ybase = 0;
	rl_current_par_yfrac = 0.0f;
	rl_current_par_ylayer = 0;
}

// Extra off-screen margin for the enemy-draw visibility tests: the interpolated position lags
// the tick position by up to one tick of motion. Equals the interpolation snap threshold.
enum { ENEMY_DRAW_MARGIN = 40 };

// True if this live enemy is stuck above the top of the screen with no way to ever leave:
// beyond shot reach (ey <= -58) and vertically frozen. HORIZONTAL state is deliberately
// ignored (HARVEST's anchor carries a sideways sway). notes.md §Map-stop softlock watchdog.
static bool enemy_stuck_above_screen(unsigned int i)
{
	return enemy[i].ey   <= -58 &&
	       enemy[i].eyc  <= 0 &&
	       enemy[i].eycc <= 0 &&
	       enemy[i].fixedmovey <= 0;
}

// Count live enemies stuck above the screen: a dedicated full-pool scan, deliberately not the
// draw loop's on-screen census (notes.md §Map-stop softlock watchdog).
static unsigned int count_stuck_above_screen(void)
{
	unsigned int n = 0;
	for (int i = 0; i < 100; i++)
		if (enemyAvail[i] != 1 && enemy_stuck_above_screen(i))
			++n;
	return n;
}

// Sim ticks (~6s) a stuck-above stall is left alone before the watchdog culls it (notes.md §Map-stop softlock watchdog).
enum { MAP_STOP_STALL_LIMIT = 210 };

void JE_drawEnemy(int enemyOffset) // actually does a whole lot more than just drawing
{
	// JE_drawEnemy(25) is only ever the sky bank (slots 0..24), the one batch whose layer-2
	// ride lives in eyc rather than tempBackMove -- the same structural identity the event
	// spawner uses (its case 0 adjusts sky spawns by backMove2).
	const bool skyBank = (enemyOffset == 25);

	player[0].x -= 25;

	for (int i = enemyOffset - 25; i < enemyOffset; i++)
	{
		if (enemyAvail[i] != 1)
		{
			// Attached sky scenery rides at exactly the layer-2 step. The ride can be
			// authored in eyc (GYGES's later glass structures) OR in fixedmovey with the
			// eycc oscillator swinging eyc symmetrically around 0 on top (GYGES's first
			// chain structure: fixed=2, eyc 0 +/- eyrev), so sum the ride components and
			// skip the oscillator's transient eyc. Homing (yaccel) marks a free flyer.
			const int skyRide = (int)enemy[i].fixedmovey +
			                    (enemy[i].eycc != 0 ? 0 : (int)enemy[i].eyc);
			skyGlueThisEnemy = skyBank && backMove2 > 0 &&
			                   skyRide == (int)backMove2 && enemy[i].yaccel == 0;

			enemy[i].mapoffset = tempMapXOfs;
			enemy[i].mapoffset_frac = tempMapXOfs_frac;  // for the health bar's smooth-H match
			if (skyGlueThisEnemy)
			{
				// The bar records post-advance; pull back the scroll part of this tick's
				// advance (eyc == backMove2 plus the modifier's extra layer-2 pixels).
				enemy[i].scroll_ybase = -((int)backMove2 + endlessScrollExtraPx2);
				enemy[i].scroll_yfrac = bg_layer_yfrac[2];
				enemy[i].scroll_ylayer = 2;
			}
			else
			{
				enemy[i].scroll_ybase = tempScrollYBaseBar;
				enemy[i].scroll_yfrac = tempScrollYfracBar;
				enemy[i].scroll_ylayer = (JE_byte)tempScrollYLayer;
			}

			// Endless: decide once (per linkgroup) this enemy's tier -- 0 undecided, 1 normal,
			// 2 elite, 3 champion; score pickups and invincible (255-armor) enemies excluded.
			if (endlessMode && enemy[i].eliteState == 0)
			{
				if (!enemy[i].scoreitem && enemy[i].armorleft > 0 && enemy[i].armorleft < 255)
					enemy[i].eliteState = (JE_byte)endlessRollEliteTier(enemy[i].linknum);
				else
					enemy[i].eliteState = 1;
			}

			if (enemy[i].xaccel && enemy[i].xaccel - 89u > mt_rand() % 11)
			{
				if (player[0].x > enemy[i].ex)
				{
					if (enemy[i].exc < enemy[i].xaccel - 89)
						enemy[i].exc++;
				}
				else
				{
					if (enemy[i].exc >= 0 || -enemy[i].exc < enemy[i].xaccel - 89)
						enemy[i].exc--;
				}
			}

			if (enemy[i].yaccel && enemy[i].yaccel - 89u > mt_rand() % 11)
			{
				if (player[0].y > enemy[i].ey)
				{
					if (enemy[i].eyc < enemy[i].yaccel - 89)
						enemy[i].eyc++;
				}
				else
				{
					if (enemy[i].eyc >= 0 || -enemy[i].eyc < enemy[i].yaccel - 89)
						enemy[i].eyc--;
				}
			}

			// Draw/animate gate vs the VISIBLE window [PLAYFIELD_LEFT, PLAYFIELD_RIGHT] (vanilla:
			// 0..262, gate -29..300). Right: +38 so a 2x2 (spans ex-6..ex+17) is fully hidden
			// before it stops drawing -- the old +29-off-PLAYFIELD_WIDTH bound cut it with pixels
			// still visible at the right edge. Left: -28, not -29, because blit_sprite2 doesn't
			// clip X and a left tile at blit x -34 row-wraps its first column onto the previous
			// row's last VISIBLE column (322); one less keeps the wrap inside the cropped margin.
			if (enemy[i].ex + tempMapXOfs > -28 && enemy[i].ex + tempMapXOfs < PLAYFIELD_RIGHT + 38)
			{
				if (enemy[i].aniactive == 1)
				{
					enemy[i].enemycycle++;

					if (enemy[i].enemycycle == enemy[i].animax)
						enemy[i].aniactive = enemy[i].aniwhenfire;
					else if (enemy[i].enemycycle > enemy[i].ani)
						enemy[i].enemycycle = enemy[i].animin;
				}

				if (enemy[i].enemycycle >= 1 && enemy[i].enemycycle <= 20 &&
				    enemy[i].egr[enemy[i].enemycycle - 1] == 999)
					goto enemy_gone;

				// Elite/champion tint: filter is zeroed every frame (below), so re-apply it
				// here each frame. Skip if something already set filter (e.g. a hit flash).
				if (enemy[i].eliteState >= 2 && enemy[i].filter == 0)
					enemy[i].filter = (enemy[i].eliteState == 3) ? ENDLESS_CHAMPION_FILTER : ENDLESS_ELITE_FILTER;

				if (enemy[i].size == 1) // 2x2 enemy
				{
					if (enemy[i].ey > -13 - ENEMY_DRAW_MARGIN)
					{
						blit_enemy(VGAScreen, i, -6, -7, 0);
						blit_enemy(VGAScreen, i,  6, -7, 1);
					}
					if (enemy[i].ey > -26 - ENEMY_DRAW_MARGIN && enemy[i].ey < 182 + ENEMY_DRAW_MARGIN)
					{
						blit_enemy(VGAScreen, i, -6,  7, 19);
						blit_enemy(VGAScreen, i,  6,  7, 20);
					}
				}
				else
				{
					if (enemy[i].ey > -13 - ENEMY_DRAW_MARGIN)
						blit_enemy(VGAScreen, i, 0, 0, 0);
				}

				enemy[i].filter = 0;
			}

			if (enemy[i].excc)
			{
				if (--enemy[i].exccw <= 0)
				{
					if (enemy[i].exc == enemy[i].exrev)
					{
						enemy[i].excc = -enemy[i].excc;
						enemy[i].exrev = -enemy[i].exrev;
						enemy[i].exccadd = -enemy[i].exccadd;
					}
					else
					{
						enemy[i].exc += enemy[i].exccadd;
						enemy[i].exccw = enemy[i].exccwmax;
						if (enemy[i].exc == enemy[i].exrev)
						{
							enemy[i].excc = -enemy[i].excc;
							enemy[i].exrev = -enemy[i].exrev;
							enemy[i].exccadd = -enemy[i].exccadd;
						}
					}
				}
			}

			if (enemy[i].eycc)
			{
				if (--enemy[i].eyccw <= 0)
				{
					if (enemy[i].eyc == enemy[i].eyrev)
					{
						enemy[i].eycc = -enemy[i].eycc;
						enemy[i].eyrev = -enemy[i].eyrev;
						enemy[i].eyccadd = -enemy[i].eyccadd;
					}
					else
					{
						enemy[i].eyc += enemy[i].eyccadd;
						enemy[i].eyccw = enemy[i].eyccwmax;
						if (enemy[i].eyc == enemy[i].eyrev)
						{
							enemy[i].eycc = -enemy[i].eycc;
							enemy[i].eyrev = -enemy[i].eyrev;
							enemy[i].eyccadd = -enemy[i].eyccadd;
						}
					}
				}
			}

			// Fixed movement has mixed level-script semantics: sky values are local motion, while
			// layer-bound values often cancel/modify the normal layer advance. Scale only the latter,
			// directly from this batch's exact layer delta (never from an independent boost pulse).
			enemy[i].ey += enemy_fixed_move_y(i);

			enemy[i].ex += enemy[i].exc;
			if (enemy[i].ex < -80 || enemy[i].ex > vga_width + 20)
				goto enemy_gone;

			enemy[i].ey += enemy[i].eyc;
			if (enemy[i].ey < -112 || enemy[i].ey > 190)
				goto enemy_gone;

			goto enemy_still_exists;

enemy_gone:
			/* enemy[i].egr[10] &= 0x00ff; <MXD> madness? */
			enemyAvail[i] = 1;
			goto draw_enemy_end;

enemy_still_exists:

			/*X bounce*/
			if (enemy[i].ex <= enemy[i].xminbounce || enemy[i].ex >= enemy[i].xmaxbounce)
				enemy[i].exc = -enemy[i].exc;

			/*Y bounce*/
			if (enemy[i].ey <= enemy[i].yminbounce || enemy[i].ey >= enemy[i].ymaxbounce)
				enemy[i].eyc = -enemy[i].eyc;

			/* Evalue != 0 - score item at boundary */
			// Keep pickups inside the VISIBLE window (vanilla: -5..245 vs window 0..262).
			// The old -5/PLAYFIELD_WIDTH pair let items park fully hidden in the cropped
			// left margin, and stopped them ~24px short of the widened right edge.
			if (enemy[i].scoreitem)
			{
				if (enemy[i].ex < PLAYFIELD_LEFT - 5)
					enemy[i].ex++;
				if (enemy[i].ex > PLAYFIELD_RIGHT - 17)
					enemy[i].ex--;
			}

			// Scroll-track at overclock pace using the SMOOTH per-tick px of whichever layer this
			// enemy rides (tagged per batch beside tempBackMove), so ground enemies glide with the
			// terrain instead of drifting when two layers share a backMove. Attached sky scenery
			// rides layer 2 through its own eyc, so it takes that layer's extra pixels here.
			// notes.md §Endless scroll boost.
			enemy[i].ey += tempBackMove + tempScrollExtraPx +
			               (skyGlueThisEnemy ? endlessScrollExtraPx2 : 0);

			if (enemy[i].ex <= -24 || enemy[i].ex >= vga_width - 24)
				goto draw_enemy_end;

			JE_integer tempX = enemy[i].ex;
			JE_integer tempY = enemy[i].ey;

			temp = enemy[i].enemytype;

			/* Enemy Shots */
			if (enemy[i].edamaged == 1)
				goto draw_enemy_end;

			enemyOnScreen++;

			if (enemy[i].iced)
			{
				enemy[i].iced--;
				if (enemy[i].enemyground != 0)
				{
					enemy[i].filter = 0x09;
				}
				goto draw_enemy_end;
			}

			for (int j = 3; j > 0; j--)
			{
				if (enemy[i].freq[j-1])
				{
					temp3 = enemy[i].tur[j-1];

					if (--enemy[i].eshotwait[j-1] == 0 && temp3)
					{
						enemy[i].eshotwait[j-1] = enemy[i].freq[j-1];
						if (difficultyLevel > DIFFICULTY_NORMAL)
						{
							enemy[i].eshotwait[j-1] = (enemy[i].eshotwait[j-1] / 2) + 1;
							if (difficultyLevel > DIFFICULTY_MANIACAL)
								enemy[i].eshotwait[j-1] = (enemy[i].eshotwait[j-1] / 2) + 1;
						}

						if (endlessMode)
						{
							// Endless: enemies fire faster with depth (lower cooldown = faster);
							// champions fire faster still.
							int fd = endlessFireDelayPercent();
							if (enemy[i].eliteState == 3)
								fd = fd * endlessChampionFireDelayPercent() / 100;
							enemy[i].eshotwait[j-1] = enemy[i].eshotwait[j-1] * fd / 100;
							if (enemy[i].eshotwait[j-1] < 1)
								enemy[i].eshotwait[j-1] = 1;
						}

						if (galagaMode && (enemy[i].eyc == 0 || (mt_rand() % 400) >= galagaShotFreq))
							goto draw_enemy_end;

						switch (temp3)
						{
						case 252: /* Savara Boss DualMissile */
							if (enemy[i].ey > 20)
							{
								JE_setupExplosion(tempX - 8 + tempMapXOfs, tempY - 20 - backMove * 8, -2, 6, false, false);
								JE_setupExplosion(tempX + 4 + tempMapXOfs, tempY - 20 - backMove * 8, -2, 6, false, false);
							}
							break;
						case 251:; /* Suck-O-Magnet */
							const JE_integer attraction = 4 - (abs(player[0].x - tempX) + abs(player[0].y - tempY)) / 100;
							if (attraction > 0)
								player[0].x_velocity += (player[0].x > tempX) ? -attraction : attraction;
							break;
						case 253: /* Left ShortRange Magnet */
							if (abs(player[0].x + 25 - 14 - tempX) < 24 && abs(player[0].y - tempY) < 28)
							{
								player[0].x_velocity += 2;
							}
							if (twoPlayerMode &&
							   (abs(player[1].x - 14 - tempX) < 24 && abs(player[1].y - tempY) < 28))
							{
								player[1].x_velocity += 2;
							}
							break;
						case 254: /* Left ShortRange Magnet */
							if (abs(player[0].x + 25 - 14 - tempX) < 24 && abs(player[0].y - tempY) < 28)
							{
								player[0].x_velocity -= 2;
							}
							if (twoPlayerMode &&
							   (abs(player[1].x - 14 - tempX) < 24 && abs(player[1].y - tempY) < 28))
							{
								player[1].x_velocity -= 2;
							}
							break;
						case 255: /* Magneto RePulse!! */
							if (difficultyLevel != DIFFICULTY_EASY) /*DIF*/
							{
								if (j == 3)
								{
									enemy[i].filter = 0x70;
								}
								else
								{
									const JE_integer repulsion = 4 - (abs(player[0].x - tempX) + abs(player[0].y - tempY)) / 20;
									if (repulsion > 0)
										player[0].x_velocity += (player[0].x > tempX) ? repulsion : -repulsion;
								}
							}
							break;
						default:
						/*Rot*/
							if (cheatNoEnemyFire)  // debug: enemies behave but don't shoot
								break;
							// Endless "rising tide": once faster-fire has saturated, enemies add EXTRA
							// shots per volley (fanned out below) rather than firing quicker -- bullet
							// count is the one difficulty axis with no engine ceiling. Zero early on.
							int endlessBaseMulti = weapons[temp3].multi;
							int endlessVolley = endlessBaseMulti + (endlessMode ? endlessExtraEnemyShots() : 0);
							// Only endless draws on the enlarged enemy-shot pool; normal levels keep the
							// original 60-slot cap so they play exactly as before.
							const int enemyShotCap = endlessMode ? ENEMY_SHOT_MAX : ENEMY_SHOT_NORMAL;
							for (int shotNum = 0; shotNum < endlessVolley; shotNum++)
							{
								for (b = 0; b < enemyShotCap; b++)
								{
									if (enemyShotAvail[b] == 1)
										break;
								}
								if (b == enemyShotCap)
									goto draw_enemy_end;

								enemyShotAvail[b] = !enemyShotAvail[b];

								if (weapons[temp3].sound > 0 && shotNum < endlessBaseMulti)
								{
									do
									{
										temp = mt_rand() % 8;
									} while (temp == 3);
									soundQueue[temp] = weapons[temp3].sound;
								}

								if (enemy[i].aniactive == 2)
									enemy[i].aniactive = 1;

								if (++enemy[i].eshotmultipos[j-1] > weapons[temp3].max)
									enemy[i].eshotmultipos[j-1] = 1;

								int tempPos = enemy[i].eshotmultipos[j-1] - 1;

								if (j == 1)
									temp2 = 4;

								enemyShot[b].sx = tempX + weapons[temp3].bx[tempPos] + tempMapXOfs;
								enemyShot[b].sy = tempY + weapons[temp3].by[tempPos];
								enemyShot[b].sdmg = weapons[temp3].attack[tempPos];
								enemyShot[b].tx = weapons[temp3].tx;
								enemyShot[b].ty = weapons[temp3].ty;
								enemyShot[b].duration = weapons[temp3].del[tempPos];
								enemyShot[b].animate = 0;
								enemyShot[b].animax = weapons[temp3].weapani;

								enemyShot[b].sgr = weapons[temp3].sg[tempPos];
								switch (j)
								{
								case 1:
									enemyShot[b].syc = weapons[temp3].acceleration;
									enemyShot[b].sxc = weapons[temp3].accelerationx;

									enemyShot[b].sxm = weapons[temp3].sx[tempPos];
									enemyShot[b].sym = weapons[temp3].sy[tempPos];
									break;
								case 3:
									enemyShot[b].sxc = -weapons[temp3].acceleration;
									enemyShot[b].syc = weapons[temp3].accelerationx;

									enemyShot[b].sxm = -weapons[temp3].sy[tempPos];
									enemyShot[b].sym = -weapons[temp3].sx[tempPos];
									break;
								case 2:
									enemyShot[b].sxc = weapons[temp3].acceleration;
									enemyShot[b].syc = -weapons[temp3].acceleration;

									enemyShot[b].sxm = weapons[temp3].sy[tempPos];
									enemyShot[b].sym = -weapons[temp3].sx[tempPos];
									break;
								}

								if (weapons[temp3].aim > 0)
								{
									JE_byte aim = weapons[temp3].aim;

									/*DIF*/
									if (difficultyLevel > DIFFICULTY_NORMAL)
										aim += difficultyLevel - 2;

									JE_word targetX = player[0].x;
									JE_word targetY = player[0].y;

									if (twoPlayerMode)
									{
										// fire at live player(s)
										if (player[0].is_alive && !player[1].is_alive)
											temp = 0;
										else if (player[1].is_alive && !player[0].is_alive)
											temp = 1;
										else
											temp = mt_rand() % 2;

										if (temp == 1)
										{
											targetX = player[1].x - 25;
											targetY = player[1].y;
										}
									}

									JE_integer aimX = (targetX + 25) - tempX - tempMapXOfs - 4;
									if (aimX == 0)
										aimX = 1;
									JE_integer aimY = targetY - tempY;
									if (aimY == 0)
										aimY = 1;
									const JE_integer maxMagAim = MAX(abs(aimX), abs(aimY));
									enemyShot[b].sxm = roundf((float)aimX / maxMagAim * aim);
									enemyShot[b].sym = roundf((float)aimY / maxMagAim * aim);
								}

								if (endlessMode)
								{
									// Endless: enemy projectiles get faster with depth. Scale both
									// velocity components (set above for fixed-direction and aimed
									// shots alike); round away from zero so slow shots still speed up.
									int spct = endlessShotSpeedPercent();
									enemyShot[b].sxm = (enemyShot[b].sxm * spct + (enemyShot[b].sxm >= 0 ? 50 : -50)) / 100;
									enemyShot[b].sym = (enemyShot[b].sym * spct + (enemyShot[b].sym >= 0 ? 50 : -50)) / 100;

									// ...and hit harder with depth (DEVASTATING sector adds more; champion
									// shooters add more still). sdmg is a byte fed straight to
									// JE_playerDamage; round to nearest and clamp so it can't wrap past 255.
									int dpct = endlessShotDamagePercent();
									if (enemy[i].eliteState == 3)
										dpct = dpct * endlessChampionShotDamagePercent() / 100;
									int dmg = (enemyShot[b].sdmg * dpct + 50) / 100;
									enemyShot[b].sdmg = (JE_byte)(dmg > 255 ? 255 : dmg);

									// Tide fan: the EXTRA shots (beyond the weapon's own volley) get a small
									// alternating angular offset, so they spread into a readable fan instead
									// of stacking on the base trajectory. Done after the speed scale, so the
									// larger velocity rotates with some resolution.
									if (shotNum >= endlessBaseMulti)
									{
										int fanK = shotNum - endlessBaseMulti;   // 0, 1, 2, ... per extra shot
										float fanAng = ((fanK & 1) ? -1.0f : 1.0f) * (fanK / 2 + 1) * 0.20f;
										float fc = cosf(fanAng), fs = sinf(fanAng);
										int ox = enemyShot[b].sxm, oy = enemyShot[b].sym;
										enemyShot[b].sxm = roundf(ox * fc - oy * fs);
										enemyShot[b].sym = roundf(ox * fs + oy * fc);
										if (enemyShot[b].sxm == 0 && enemyShot[b].sym == 0)  // rounding zeroed a tiny vector
										{
											enemyShot[b].sxm = ox;
											enemyShot[b].sym = oy;
										}
									}
								}
							}
							break;
						}
					}
				}
			}

			/* Enemy Launch Routine */
			if (enemy[i].launchfreq)
			{
				if (--enemy[i].launchwait == 0)
				{
					enemy[i].launchwait = enemy[i].launchfreq;

					if (enemy[i].launchspecial != 0)
					{
						/*Type  1 : Must be inline with player*/
						if (abs(enemy[i].ey - player[0].y) > 5)
							goto draw_enemy_end;
					}

					if (enemy[i].aniactive == 2)
					{
						enemy[i].aniactive = 1;
					}

					if (enemy[i].launchtype == 0)
						goto draw_enemy_end;

					tempW = enemy[i].launchtype;
					b = JE_newEnemy(enemyOffset == 50 ? 75 : enemyOffset - 25, tempW, 0);

					/*Launch Enemy Placement*/
					if (b > 0)
					{
						struct JE_SingleEnemyType* e = &enemy[b-1];

						e->ex = tempX;
						e->ey = tempY + enemyDat[e->enemytype].startyc;
						if (e->size == 0)
							e->ey -= 7;

						if (e->launchtype > 0 && e->launchfreq == 0)
						{
							if (e->launchtype > 90)
							{
								e->ex += mt_rand() % ((e->launchtype - 90) * 4) - (e->launchtype - 90) * 2;
							}
							else
							{
								JE_integer aimX = (player[0].x + 25) - tempX - tempMapXOfs - 4;
								if (aimX == 0)
									aimX = 1;
								JE_integer aimY = player[0].y - tempY;
								if (aimY == 0)
									aimY = 1;
								const JE_integer maxMagAim = MAX(abs(aimX), abs(aimY));
								e->exc = roundf((float)aimX / maxMagAim * e->launchtype);
								e->eyc = roundf((float)aimY / maxMagAim * e->launchtype);
							}
						}

						do
						{
							temp = mt_rand() % 8;
						} while (temp == 3);
						soundQueue[temp] = randomEnemyLaunchSounds[(mt_rand() % 3)];

						if (enemy[i].launchspecial == 1 &&
						    enemy[i].linknum < 100)
						{
							e->linknum = enemy[i].linknum;
						}
					}
				}
			}
		}
draw_enemy_end:
		;
	}

	player[0].x += 25;
}

void JE_main(void)
{
	char buffer[256];

	int lastEnemyOnScreen;

	/* NOTE: BEGIN MAIN PROGRAM HERE after LOADING A GAME OR STARTING A NEW ONE */

	/* ----------- GAME ROUTINES ------------------------------------- */
	/* We need to jump to the beginning to make space for the routines */
	/* --------------------------------------------------------------- */
	if (endlessMode)
	{
		// First level of the run: run the starting shop before level 1 (the normal between-
		// level flow only runs after a level clears). The shop's Start Level submenu IS the
		// course choice -- it sets mainLevel + the mutators and launches.
		endlessBetweenLevels();

		// Player chose Quit Game in the shop instead of charting a course: end the run and
		// return to the title. On quit the shop leaves mainLevel == 0 (and has already faded out).
		if (mainLevel == 0)
		{
			endlessEndRunToTitle();  // hardcore shows the Run Over summary first
			return;
		}
	}

	goto start_level_first;

	/*------------------------------GAME LOOP-----------------------------------*/

	/* Startlevel is called after a previous level is over.  If the first level
	   is started for a gaming session, startlevelfirst is called instead and
	   this code is skipped.  The code here finishes the level and prepares for
	   the loadmap function. */

start_level:

	mouseSetRelative(false);

	if (galagaMode)
		twoPlayerMode = false;

	JE_clearKeyboard();

	free_sprite2s(&enemySpriteSheets[0]);
	free_sprite2s(&enemySpriteSheets[1]);
	free_sprite2s(&enemySpriteSheets[2]);
	free_sprite2s(&enemySpriteSheets[3]);

	/* Normal speed */
	if (fastPlay != 0)
	{
		smoothScroll = true;
		Uint16 speed = 0x4300;
		setDelaySpeed(speed);
	}

	if (play_demo || record_demo)
	{
		if (demo_file)
		{
			fclose(demo_file);
			demo_file = NULL;
		}

		if (play_demo)
		{
			moveTyrianLogoUp = true;
			stop_song();
			fade_black(10);

			wait_noinput(true, true, true);
		}
	}

	difficultyLevel = oldDifficultyLevel;   /*Return difficulty to normal*/

	if (!play_demo)
	{
		// Endless "Quit Level": don't end the run. Revert to the launch-time snapshot and reopen the
		// buy/sell menu LOCKED to those choices -- relaunch the same level, or save/load, or quit the
		// run. No depth++ and no level-clear screen: this is a retry, not a completed zone.
		if (endlessMode && endlessQuitToOutpost && endlessSortieValid())
		{
			endlessQuitToOutpost = false;
			fade_song();
			fade_black(10);
			endlessRestoreSortie();   // revert player loadout + endless state; arms the locked outpost
			endlessBetweenLevels();   // reopens the shop (locked); sets mainLevel on relaunch, 0 on quit-run
			if (mainLevel == 0)          // player chose Quit Game in the locked outpost: end the run and
			{                            // return to the title (hardcore shows the Run Over summary first,
				endlessEndRunToTitle();  // since a hardcore quit is as final as a death).
				return;
			}
			goto start_level_first;   // re-run the same level (endlessCaptureSortie re-snapshots + clears the lock)
		}

		// Was the level that just ran picked straight out of the debug level browser? Consume the
		// flag whatever the answer, so it can never carry over to a campaign-reached level.
		const bool fromDebugBrowser = debugLevelJumpTake();

		// ENGAGE mini-games (** ALE **, TIME WAR, SQUADRON) are campaign dead ends: clearing one
		// falls into the episode's END GAME section, dying reloads the "LAST LEVEL" backup save.
		// Neither exists behind a browser jump, so both just restart the game at level 1 of the
		// next episode. Show the level-complete screen if it was cleared, then give the player
		// back the outpost they launched from instead of running the ending.
		if (fromDebugBrowser && engageMode)
		{
			const bool cleared = (!all_players_dead() || normalBonusLevelCurrent || bonusLevelCurrent) && !playerEndLevel;
			if (cleared)
				JE_endLevelAni();
			fade_song();
			if (!cleared)
				fade_black(10);

			debugLevelJumpReturn();
			goto start_level_first;
		}

		if ((!all_players_dead() || normalBonusLevelCurrent || bonusLevelCurrent) && !playerEndLevel)
		{
			if (endlessMode)
				endlessRunDepth++;
			else
				mainLevel = nextLevel;

			JE_endLevelAni();  // level-complete screen first...

			fade_song();

			if (endlessMode)
			{
				endlessBetweenLevels();  // ...then the between-level shop, whose Start Level submenu is the
				                         // course choice: it sets mainLevel + the mutators and launches.

				// Player chose Quit Game instead of a course: end the run, back to the title.
				if (mainLevel == 0)
				{
					endlessEndRunToTitle();  // hardcore shows the Run Over summary first
					return;
				}
			}
		}
		else
		{
			fade_song();
			fade_black(10);

			if (endlessMode)
			{
				endlessOnRunEnd();
				endlessMode = false;
				mainLevel = 0;
				return;
			}

			if (timedBattleMode)
			{
				mainLevel = 0;
				return;
			}

			JE_loadGame(twoPlayerMode ? 22 : 11);
			if (doNotSaveBackup)
			{
				superTyrian = false;
				onePlayerAction = false;
				player[0].items.super_arcade_mode = SA_NONE;
			}
			if (bonusLevelCurrent && !playerEndLevel)
			{
				mainLevel = nextLevel;
			}
		}
	}
	doNotSaveBackup = false;

	if (play_demo)
		return;

start_level_first:

	set_volume(tyrMusicVolume, fxVolume);

	endLevel = false;
	reallyEndLevel = false;
	playerEndLevel = false;
	extraGame = false;

	doNotSaveBackup = false;
	JE_loadMap();

	if (mainLevel == 0)  // if quit itemscreen
		return;          // back to titlescreen

	// An endless run just loaded from within a shop resumes at its OUTPOST, not by dropping
	// straight into the loaded level. (Title-screen loads already ran the outpost at JE_main's
	// entry; this covers the buy/sell-menu load, where JE_loadMap bailed out above.)
	if (endlessMode && endlessResumePending())
	{
		gameLoaded = false;      // consumed -- don't let the shop see it and close instantly
		endlessBetweenLevels();  // reopens the SAVED outpost snapshot (no reroll)
		if (mainLevel == 0)      // player quit the outpost
		{
			endlessEndRunToTitle();  // hardcore shows Run Over first (a resumed run is never hardcore, but keep it uniform)
			return;
		}
		goto start_level_first;
	}

	if (endlessMode)
	{
		endlessRegenerateLevel();
		endlessCaptureSortie();  // snapshot the launch-time loadout + committed level for a possible Quit Level retry
	}

	crashlog_set_phase("playing level");

	if (!play_demo)
		mouseSetRelative(true);

	fade_song();

	for (uint i = 0; i < COUNTOF(player); ++i)
		player[i].is_alive = true;

	oldDifficultyLevel = difficultyLevel;
	if (episodeNum == EPISODE_AVAILABLE)
		difficultyLevel--;
	if (difficultyLevel < DIFFICULTY_EASY)
		difficultyLevel = DIFFICULTY_EASY;

	player[0].x = 100;
	player[0].y = 180;

	player[1].x = 190;
	player[1].y = 180;

	assert(COUNTOF(player->old_x) == COUNTOF(player->old_y));

	for (uint i = 0; i < COUNTOF(player); ++i)
	{
		for (uint j = 0; j < COUNTOF(player->old_x); ++j)
		{
			player[i].old_x[j] = player[i].x - (19 - j);
			player[i].old_y[j] = player[i].y - 18;
		}
		
		player[i].last_x_shot_move = player[i].x;
		player[i].last_y_shot_move = player[i].y;
	}
	
	JE_loadPic(VGAScreen, twoPlayerMode ? 6 : 3, false);

	// Relocate the HUD to the new right edge when the playfield is wider
	const int hud_shift = vga_width - LEGACY_WIDTH;
	if (hud_shift > 0)
	{
		for (int y = 0; y < vga_height; ++y)
		{
			Uint8* row = (Uint8*)VGAScreen->pixels + y * VGAScreen->pitch;
			memmove(row + PLAYFIELD_WIDTH, row + LEGACY_WIDTH - HUD_WIDTH, HUD_WIDTH);
			memset(row + LEGACY_WIDTH - HUD_WIDTH, 0, hud_shift);
		}
	}

	JE_drawOptions();

	JE_outText(VGAScreen, HUD_X(268), twoPlayerMode ? 76 : 118, levelName, 12, 4);

	// Ensure the widened playfield blends into the HUD before the fade-in
	// so no remnants of the old HUD position appear during level start.
	extend_playfield_right_column(VGAScreen);

	JE_showVGA();
	JE_gammaCorrect(&colors, gammaCorrection);
	fade_palette(colors, 50, 0, 255);

	if (explosionSpriteSheet.data == NULL)
		JE_loadCompShapes(&explosionSpriteSheet, '6');

	/* MAPX will already be set correctly */
	mapY = 300 - 8;
	mapY2 = 600 - 8;
	mapY3 = 600 - 8;
	mapYPos = &megaData1.mainmap[mapY][0] - 1;
	mapY2Pos = &megaData2.mainmap[mapY2][0] - 1;
	mapY3Pos = &megaData3.mainmap[mapY3][0] - 1;
	mapXPos = 0;
	mapXOfs = 0;
	mapX2Pos = 0;
	mapX3Pos = 0;
	mapX3Ofs = 0;
	mapXbpPos = 0;
	mapX2bpPos = 0;
	mapX3bpPos = 0;

	map1YDelay = 1;
	map1YDelayMax = 1;
	map2YDelay = 1;
	map2YDelayMax = 1;

	musicFade = false;

	backPos = 0;
	backPos2 = 0;
	backPos3 = 0;
	power = 0;
	starfield_speed = 1;

	/* Setup player ship graphics */
	JE_getShipInfo();

	for (uint i = 0; i < COUNTOF(player); ++i)
	{
		player[i].x_velocity = 0;
		player[i].y_velocity = 0;

		player[i].invulnerable_ticks = 100;
	}

	newkey = newmouse = false;

	/* Initialize Level Data and Debug Mode */
	levelEnd = 255;
	levelEndWarp = -4;
	levelEndFxWait = 0;
	warningCol = 120;
	warningColChange = 1;
	warningSoundDelay = 0;
	armorShipDelay = 50;

	bonusLevel = false;
	readyToEndLevel = false;
	firstGameOver = true;
	eventLoc = 1;
	curLoc = 0;
	eventScrollCatchupValid = false;
	eventScrollSkyValid = false;
	backMove = 1;
	backMove2 = 2;
	backMove3 = 3;
	explodeMove = 2;
	enemiesActive = true;
	for (temp = 0; temp < 3; temp++)
	{
		button[temp] = false;
	}
	stopBackgrounds = false;
	stopBackgroundNum = 0;
	mapStopStallTicks = 0;
	background3x1   = false;
	background3x1b  = false;
	background3over = 0;
	background2over = 1;
	topEnemyOver = false;
	skyEnemyOverAll = false;
	smallEnemyAdjust = false;
	starActive = true;
	enemyContinualDamage = false;
	levelEnemyFrequency = 96;
	quitRequested = false;

	for (unsigned int i = 0; i < COUNTOF(boss_bar); i++)
		boss_bar[i].link_num = 0;

	forceEvents = false;  /*Force events to continue if background movement = 0*/

	superEnemy254Jump = 0;   /*When Enemy with PL 254 dies*/

	/* Filter Status */
	filterActive = true;
	filterFade = true;
	filterFadeStart = false;
	levelFilter = -99;
	levelBrightness = -14;
	levelBrightnessChg = 1;

	background2notTransparent = false;

	uint old_weapon_bar[2] = { 0, 0 };  // only redrawn when they change

	/* Initially erase power bars */
	lastPower = power / 10;

	/* Initial Text */
	JE_drawTextWindow(miscText[20]);

	/* Setup Armor/Shield Data */
	shieldWait = 1;
	shieldT    = shields[player[0].items.shield].tpwr * 20;

	// Endless SHIELDLESS/DEADGEN sectors never recharge the shield, so hand it over fully charged:
	// you get the whole buffer up front, then fly on armor once it's spent (no way to earn it back).
	const bool startShieldFull = endlessShieldRegenOff();

	for (uint i = 0; i < COUNTOF(player); ++i)
	{
		player[i].shield     = shields[player[i].items.shield].mpwr;
		player[i].shield_max = player[i].shield * 2;
		if (startShieldFull)
			player[i].shield = player[i].shield_max;
		shieldGaugeFlash[i] = armorGaugeFlash[i] = 0;
	}

	JE_drawShield();
	JE_drawArmor();

	// Endless keeps its bought bombs across levels (like cash/armor/perks); campaign resets each level.
	if (!endlessMode)
		for (uint i = 0; i < COUNTOF(player); ++i)
			player[i].superbombs = 0;

	/* Set cubes to 0 */
	cubeMax = 0;

	/* Secret Level Display */
	flash = 0;
	flashChange = 1;
	displayTime = 0;

	play_song(levelSong - 1);

	JE_drawPortConfigButtons();

	/* --- MAIN LOOP --- */

	newkey = false;

#ifdef WITH_NETWORK
	if (isNetworkGame)
	{
		JE_clearSpecialRequests();
		mt_srand(32402394);
	}
#endif

	initialize_starfield();

	JE_setNewGameSpeed();

	set_volume(tyrMusicVolume, fxVolume);

	/*Save backup game*/
	// Skip this mid-level autosave point for endless: its continue-slot autosave lives at the
	// OUTPOST instead (endlessBetweenLevels), the one coherent resume point. notes.md §Save / resume.
	if (!play_demo && !doNotSaveBackup && !timedBattleMode && !endlessMode)
	{
		temp = twoPlayerMode ? 22 : 11;
		JE_saveGame(temp, "LAST LEVEL    ");
		endlessSaveSlot(temp);  // not in endless mode: drops any stale endless sidecar record for this slot
	}

	if (!play_demo && record_demo)
	{
		Uint8 new_demo_num = 0;

		do
		{
			sprintf(tempStr, "demorec.%d", new_demo_num++);
		} while (dir_file_exists(get_user_directory(), tempStr)); // until file doesn't exist

		demo_file = dir_fopen_warn(get_user_directory(), tempStr, "wb");
		if (!demo_file)
			exit(1);

		fwrite_u8_die(&episodeNum, 1, demo_file);

		// Pad string buffer with NULs.
		for (size_t i = 1; i < 10; ++i)
			if (levelName[i - 1] == '\0')
				levelName[i] = '\0';
		fwrite_u8_die((Uint8 *)levelName, 10, demo_file);

		fwrite_u8_die(&lvlFileNum, 1, demo_file);

		fwrite_u8_die(&player[0].items.weapon[FRONT_WEAPON].id,  1, demo_file);
		fwrite_u8_die(&player[0].items.weapon[REAR_WEAPON].id,   1, demo_file);
		fwrite_u8_die(&player[0].items.super_arcade_mode,        1, demo_file);
		fwrite_u8_die(&player[0].items.sidekick[LEFT_SIDEKICK],  1, demo_file);
		fwrite_u8_die(&player[0].items.sidekick[RIGHT_SIDEKICK], 1, demo_file);
		fwrite_u8_die(&player[0].items.generator,                1, demo_file);

		fwrite_u8_die(&player[0].items.sidekick_level,           1, demo_file);
		fwrite_u8_die(&player[0].items.sidekick_series,          1, demo_file);

		fwrite_u8_die(&initial_episode_num,                      1, demo_file);

		fwrite_u8_die(&player[0].items.shield,                   1, demo_file);
		fwrite_u8_die(&player[0].items.special,                  1, demo_file);
		fwrite_u8_die(&player[0].items.ship,                     1, demo_file);

		for (uint i = 0; i < 2; ++i)
			fwrite_u8_die(&player[0].items.weapon[i].power,      1, demo_file);

		Uint8 unused[3] = { 0, 0, 0 };
		fwrite_u8_die(unused, 3, demo_file);

		fwrite_u8_die(&levelSong, 1, demo_file);

		demo_keys = 0;
		demo_keys_wait = 0;
	}

	twoPlayerLinked = false;
	linkGunDirec = M_PI;

	for (uint i = 0; i < COUNTOF(player); ++i)
		calc_purple_balls_needed(&player[i]);

	damageRate = 2;  /*Normal Rate for Collision Damage*/

	chargeWait   = 5;
	chargeLevel  = 0;
	chargeMax    = 5;
	chargeGr     = 0;
	chargeGrWait = 3;

	portConfigChange = false;

	/*Destruction Ratio*/
	totalEnemy = 0;
	enemyKilled = 0;

	astralDuration = 0;

	superArcadePowerUp = 1;

	yourInGameMenuRequest = false;

	constantLastX = -1;

	for (uint i = 0; i < COUNTOF(player); ++i)
		player[i].exploding_ticks = 0;

	if (isNetworkGame)
	{
		JE_loadItemDat();
	}

	memset(enemyAvail,       1, sizeof(enemyAvail));
	for (uint i = 0; i < COUNTOF(enemyShotAvail); i++)
		enemyShotAvail[i] = 1;

	/*Initialize Shots*/
	memset(playerShotData,   0, sizeof(playerShotData));
	memset(shotAvail,        0, sizeof(shotAvail));
	memset(shotMultiPos,     0, sizeof(shotMultiPos));
	memset(shotRepeat,       1, sizeof(shotRepeat));

	memset(button,           0, sizeof(button));

	memset(globalFlags,      0, sizeof(globalFlags));

	memset(explosions,       0, sizeof(explosions));
	memset(rep_explosions,   0, sizeof(rep_explosions));

	/* --- Clear Sound Queue --- */
	memset(soundQueue,       0, sizeof(soundQueue));
	soundQueue[3] = V_GOOD_LUCK;

	memset(enemySpriteSheetIds, 0, sizeof(enemySpriteSheetIds));
	memset(enemy,               0, sizeof(enemy));

	if (endlessMode)
		endlessPreloadBanks();  // load starting sprite banks now so early spawns aren't invisible

	memset(SFCurrentCode,    0, sizeof(SFCurrentCode));
	memset(SFExecuted,       0, sizeof(SFExecuted));

	zinglonDuration = 0;
	specialWait = 0;
	nextSpecialWait = 0;
	for (uint i = 0; i < 2; i++)  /*Launch the Attachments!*/
	{
		optionAttachmentMove[i]   = 0;
		optionAttachmentLinked[i] = true;
		optionAttachmentReturn[i] = false;
	}

	editShip1 = false;
	editShip2 = false;

	memset(smoothies, 0, sizeof(smoothies));

	levelTimer = false;
	randomExplosions = false;

	last_superpixel = 0;
	memset(superpixels, 0, sizeof(superpixels));

	returnActive = false;

	galagaShotFreq = 0;

	if (galagaMode)
	{
		difficultyLevel = DIFFICULTY_NORMAL;
	}
	galagaLife = 10000;

	JE_drawOptionLevel();

	// keeps map from scrolling past the top
	BKwrap1 = BKwrap1to = &megaData1.mainmap[1][0];
	BKwrap2 = BKwrap2to = &megaData2.mainmap[1][0];
	BKwrap3 = BKwrap3to = &megaData3.mainmap[1][0];

level_loop:

	//tempScreenSeg = game_screen; /* side-effect of game_screen */

	if (isNetworkGame)
	{
		smoothies[9-1] = false;
		smoothies[6-1] = false;
	}
	else
	{
		starShowVGASpecialCode = smoothies[9-1] + (smoothies[6-1] << 1);

		// Endless decouples the light-cone spotlight (special code 2) from the level's own script:
		// strip a spotlight any level would set by default, and show it only for the zones that
		// rolled the seeded 1-in-10 chance (endlessRegenerateLevel). Inverted-control levels (code
		// 1, or the rare code 3) are left alone so their flipped display and controls stay in sync.
		if (endlessMode)
		{
			if (starShowVGASpecialCode == 2)
				starShowVGASpecialCode = 0;
			if (starShowVGASpecialCode == 0 && endlessLightConeActive())
				starShowVGASpecialCode = 2;

			// TOPSY-TURVY modifier: flip the playfield like a screen-flip boss. Boss-STYLE means we
			// set the same smoothies[9-1] the bosses use, so the vertical controls invert WITH the
			// view (up on the stick still moves the ship up ON SCREEN) -- disorienting but fair. It
			// overrides any spotlight above; the two never share a zone (TOPSY is a standalone theme).
			if (endlessActiveMods & ENDLESS_MOD_TOPSY)
			{
				smoothies[9 - 1] = true;
				starShowVGASpecialCode = 1;
			}
		}
	}

	/*Background Wrapping*/
	// A boosted scroll can cross a wrap threshold more than one whole map row deep in a single
	// tick; carry those rows past the wrap target so a looping layer doesn't slip a tile against
	// the scroll clock and its glued enemies. Stock strides (< 28 px) land within one row of the
	// threshold, so the carry is 0 and the plain reset is unchanged. A stop wrap (to == from,
	// the "don't scroll past the top" default) keeps the plain pin.
	if (mapYPos <= BKwrap1)
		mapYPos = (BKwrap1to > BKwrap1) ? BKwrap1to - (BKwrap1 - mapYPos) / 14 * 14 : BKwrap1to;
	if (mapY2Pos <= BKwrap2)
		mapY2Pos = (BKwrap2to > BKwrap2) ? BKwrap2to - (BKwrap2 - mapY2Pos) / 14 * 14 : BKwrap2to;
	if (mapY3Pos <= BKwrap3)
		mapY3Pos = (BKwrap3to > BKwrap3) ? BKwrap3to - (BKwrap3 - mapY3Pos) / 15 * 15 : BKwrap3to;

	allPlayersGone = all_players_dead() &&
	                 ((*player[0].lives == 1 && player[0].exploding_ticks == 0) || (!onePlayerAction && !twoPlayerMode)) &&
	                 ((*player[1].lives == 1 && player[1].exploding_ticks == 0) || !twoPlayerMode);

	/*-----MUSIC FADE------*/
	if (musicFade)
	{
		if (tempVolume > 10)
		{
			tempVolume--;
			set_volume(tempVolume, fxVolume);
		}
		else
		{
			musicFade = false;
		}
	}

	if (!allPlayersGone && levelEnd > 0 && endLevel)
	{
		play_song(9);
		musicFade = false;
	}
	else if (!playing && firstGameOver)
	{
		// Endless rolls a random per-zone track; one-shot songs otherwise just stop. Force the
		// rolled track to loop when it ends (play_song is idempotent for the current song, so it
		// won't restart it). Event 35 moves song_playing off levelSong, so its songs stay vanilla.
		if (endlessMode && song_playing == (unsigned int)(levelSong - 1))
			restart_song();
		else
			play_song(levelSong - 1);
	}

	if (!endLevel) // draw HUD
	{
		VGAScreen = VGAScreenSeg; /* side-effect of game_screen */

		extend_playfield_right_column(VGAScreenSeg);

		/*-----------------------Message Bar------------------------*/
		if (textErase > 0 && --textErase == 0)
			blit_sprite(VGAScreenSeg, 16, 189, OPTION_SHAPES, 36);  // in-game message area

		/*------------------------Shield Gen-------------------------*/
		if (galagaMode)
		{
			for (uint i = 0; i < COUNTOF(player); ++i)
				player[i].shield = 0;

			// spawned dragonwing died :(
			if (*player[1].lives == 0 || player[1].armor == 0)
				twoPlayerMode = false;

			if (player[0].cash >= (unsigned)galagaLife)
			{
				soundQueue[6] = S_EXPLOSION_11;
				soundQueue[7] = S_SOUL_OF_ZINGLON;

				if (*player[0].lives < 11)
					++(*player[0].lives);
				else
					player[0].cash += 1000;

				if (galagaLife == 10000)
					galagaLife = 20000;
				else
					galagaLife += 25000;
			}
		}
		else // not galagaMode
		{
			if (twoPlayerMode)
			{
				if (--shieldWait == 0)
				{
					shieldWait = endlessPerkShieldWait(15);  // Shield Matrix perk shortens this in endless (no-op otherwise)

					for (uint i = 0; i < COUNTOF(player); ++i)
					{
						if (player[i].shield < player[i].shield_max && player[i].is_alive)
							++player[i].shield;
					}

					JE_drawShield();
				}
			}
			else if (player[0].is_alive && player[0].shield < player[0].shield_max && power > shieldT
			         && !endlessShieldRegenOff())  // endless SHIELDLESS / DEADGEN: shields never recharge
			{
				if (--shieldWait == 0)
				{
					shieldWait = endlessPerkShieldWait(15);  // Shield Matrix perk shortens this in endless (no-op otherwise)

					power -= shieldT;

					++player[0].shield;
					if (player[1].shield < player[0].shield_max)
						++player[1].shield;

					JE_drawShield();
				}
			}
		}

		/*---------------------Weapon Display-------------------------*/
		for (uint i = 0; i < 2; ++i)
		{
			uint item_power = player[twoPlayerMode ? i : 0].items.weapon[i].power;

			if (old_weapon_bar[i] != item_power)
			{
				old_weapon_bar[i] = item_power;

				int x = HUD_X(twoPlayerMode ? 286 : 289),
					y = (i == 0) ? (twoPlayerMode ? 6 : 17) : (twoPlayerMode ? 100 : 38);

				fill_rectangle_xy(VGAScreenSeg, x, y, x + 1 + 10 * 2, y + 2, 0);

				for (uint j = 1; j <= item_power; ++j)
				{
					JE_rectangle(VGAScreen, x, y, x + 1, y + 2, 115 + j); /* SEGa000 */
					x += 2;
				}
			}
		}

		/*------------------------Power Bar-------------------------*/
		if (twoPlayerMode || onePlayerAction)
		{
			power = 900;
			power_gauge_active = false;
		}
		else
		{
			power += endlessGeneratorPowerAdd(powerAdd);  // endless DEADGEN throttles the generator to a trickle (normal rate otherwise)
			if (power > 900)
				power = 900;

			// Track prev/cur tick levels for the present loop to interpolate between;
			// draw now so the non-interpolated path still updates each tick.
			power_render_prev = power_render_cur;
			power_render_cur = (int)power;
			power_gauge_active = true;
			lastPower = power / 10;  // keep the legacy counter consistent

			draw_power_gauge((float)power);
		}

		oldMapX3Ofs = mapX3Ofs;
		oldMapX3Ofs_f = mapX3Ofs_f;  // matching un-floored mirror (see backgrnd.c)

		enemyOnScreen = 0;
	}

	/* use game_screen for all the generic drawing functions */
	VGAScreen = game_screen;

	// Begin capturing this tick's playfield draws into the render list so they
	// can be replayed (interpolated) for in-between frames at the display rate.
	rl_begin_record();

	// Cleared each tick; JE_doSpecialShot re-sets it while the Zinglon blast is live.
	zinglonPillarActive = false;

	/*---------------------------EVENTS-------------------------*/
	while (eventRec[eventLoc-1].eventtime <= curLoc && eventLoc <= maxEvent)
		JE_eventSystem();

	if (isNetworkGame && reallyEndLevel)
		goto start_level;

	/* SMOOTHIES! */
	JE_checkSmoothies();
	if (anySmoothies)
		VGAScreen = VGAScreen2;  // this makes things complicated, but we do it anyway :(

	/* --- BACKGROUNDS --- */
	/* --- BACKGROUND 1 --- */

	// A boosted scroll can advance more than one tile (28px) per tick, so widen the render
	// list's bottom interpolation margin or its up-shift uncovers a strip below the playfield.
	// Set once per tick (before any layer draws) so all three layers agree; 3 rows cover ~96px/tick.
	bgMarginRows = (endlessMode && endlessScrollBoostActive()) ? 3 : 1;

	if (forceEvents && !backMove)
		curLoc++;
	// Any forceEvents-only increment is a script-timeline step, not terrain movement. Start the
	// catch-up interval after it so a later spawn is never shifted by a stationary background.
	const int eventScrollStartThisTick = (int)curLoc;

	if (map1YDelayMax > 1 && backMove < 2)
		backMove = (map1YDelay == 1) ? 1 : 0;

	/*Draw background*/
	if (astralDuration == 0)
		draw_background_1(VGAScreen);
	else
		JE_clr256(VGAScreen);

	/*Set Movement of background 1*/
	// base1ScrollPx: px layer 1 (and the event pointer) actually scrolled this tick -- 0 on a
	// delay-gated "off" tick (map1YDelayMax > 1). Feeds the smooth overclock boost below so it
	// fills the off-ticks instead of pulsing with the (0-on-off-ticks) instantaneous backMove.
	int base1ScrollPx = 0;
	if (--map1YDelay == 0)
	{
		map1YDelay = map1YDelayMax;

		curLoc += backMove;

		backPos += backMove;
		base1ScrollPx = backMove;

		if (backPos > 27)
		{
			backPos -= 28;
			mapY--;
			mapYPos -= 14;  /*Map Width*/
		}
	}

	// Publish the PREVIOUS tick's smooth vertical scroll rate + sub-pixel fraction for the render
	// list (the present loop shows this tick's list at its pre-advance position, so the data lags
	// one tick, matching bgScrollDeltaY). notes.md §Slow-scroll smoothing.
	static float bgSmoothRatePend[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	static float bgSmoothFracPend[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	static bool  bgSmoothActivePend  = false;
	for (int L = 1; L <= 3; ++L)
	{
		bg_layer_dy[L]    = bgSmoothRatePend[L];
		bg_layer_yfrac[L] = bgSmoothFracPend[L];
	}
	bg_smooth_y_active = bgSmoothActivePend;

	// Smooth every layer to its true average scroll rate so a delay-gated slow section (event 3:
	// layer 1 1px/3 ticks, layer 2 1px/2 ticks) slides sub-pixel instead of freezing then jumping.
	// Runs on EVERY level; a scroll modifier additionally emits extra px. fireN = per-fire step (1
	// while delay-gated, else backMoveN); baseN = px that actually moved this tick.
	{
		int fire1 = (map1YDelayMax > 1 && backMove < 2) ? 1 : (int)backMove;
		endlessScrollExtraPx1 = endlessScrollExtraPx(0, fire1, map1YDelayMax, base1ScrollPx,
		                                             &bgSmoothRatePend[1], &bgSmoothFracPend[1]);
		eventScrollBaseStep[1] = fire1;
		eventScrollDelayMax[1] = map1YDelayMax;

		int fire2 = (map2YDelayMax > 1 && backMove2 < 2) ? 1 : (int)backMove2;
		int base2 = (map2YDelay == 1) ? fire2 : 0;
		endlessScrollExtraPx2 = endlessScrollExtraPx(1, fire2, map2YDelayMax, base2,
		                                             &bgSmoothRatePend[2], &bgSmoothFracPend[2]);

		// Sky-glue spawn anchor: stock layer ratio + the layers' current carry phase (the
		// fracs are these exact integer hundredths). Captured post-update, so events at the
		// START of the next tick see the state their crossed interval ended on.
		eventScrollSkyValid = fire1 > 0 && fire2 > 0;
		if (eventScrollSkyValid)
		{
			eventScrollSkyRatio100 = 100 * fire2 * map1YDelayMax / (fire1 * map2YDelayMax);
			const int c1 = (int)lroundf(bgSmoothFracPend[1] * 100.0f);
			const int c2 = (int)lroundf(bgSmoothFracPend[2] * 100.0f);
			eventScrollSkyPhase100 = eventScrollSkyRatio100 * c1 / 100 - c2;
		}

		endlessScrollExtraPx3 = endlessScrollExtraPx(2, (int)backMove3, 1, (int)backMove3,
		                                             &bgSmoothRatePend[3], &bgSmoothFracPend[3]);
		eventScrollBaseStep[3] = (int)backMove3;
		eventScrollDelayMax[3] = 1;
	}
	bgSmoothActivePend = true;

	// Publish this tick's (non-lagged) rate + fraction for background layer 3, which advances before
	// it records its rows (unlike layers 1/2). Enemy banks preserve their common pre-advance phase
	// and use the lagged bg_layer_dy/bg_layer_yfrac values even when bound to layer 3.
	// notes.md §Sub-pixel parallax / §Slow-scroll smoothing.
	for (int L = 1; L <= 3; ++L)
	{
		bg_layer_yfrac_now[L] = bgSmoothFracPend[L];
		bg_layer_dy_now[L]    = bgSmoothRatePend[L];
	}

	// Layer 1 (+ the event pointer, which must ride the identical delta so scripted stops stay
	// aligned to the terrain). One combined advance; the wrap can cross more than one tile.
	curLoc += endlessScrollExtraPx1;
	eventScrollFrom = eventScrollStartThisTick;
	eventScrollTo = (int)curLoc;
	eventScrollLayerDelta[1] = eventScrollTo - eventScrollFrom;
	eventScrollLayerDelta[3] = (int)backMove3 + endlessScrollExtraPx3;
	eventScrollBoost = endlessScrollBoostPercent();
	eventScrollCatchupValid = eventScrollBoost > 0 && eventScrollTo > eventScrollFrom;
	backPos += endlessScrollExtraPx1;
	while (backPos > 27)
	{
		backPos -= 28;
		mapY--;
		mapYPos -= 14;
	}

	if (starActive || astralDuration > 0)
		update_and_draw_starfield(VGAScreen, starfield_speed);

	if (processorType > 1 && smoothies[5-1])
	{
		if (render_list_recording)
			rl_rec_smoothie_filter(RC_ICED_BLUR);
		iced_blur_filter(game_screen, VGAScreen);
		VGAScreen = game_screen;
	}

	/*-----------------------BACKGROUNDS------------------------*/
	/*-----------------------BACKGROUND 2------------------------*/
	if (background2over == 3)
	{
		draw_background_2(VGAScreen);
		background2 = true;
	}

	if (background2over == 0)
	{
		if (!(smoothies[2-1] && processorType < 4) && !(smoothies[1-1] && processorType == 3))
		{
			if (wild && !background2notTransparent)
				draw_background_2_blend(VGAScreen);
			else
				draw_background_2(VGAScreen);
		}
	}

	if (smoothies[0] && processorType > 2 && smoothie_data[0] == 0)
	{
		if (render_list_recording)
			rl_rec_smoothie_filter(RC_LAVA_FILTER);
		lava_filter(game_screen, VGAScreen);
		VGAScreen = game_screen;
	}
	if (smoothies[2-1] && processorType > 2)
	{
		if (render_list_recording)
			rl_rec_smoothie_filter(RC_WATER_FILTER);
		water_filter(game_screen, VGAScreen);
		VGAScreen = game_screen;
	}

	/*-----------------------Ground Enemy------------------------*/
	lastEnemyOnScreen = enemyOnScreen;

	tempMapXOfs = mapXOfs + PLAYFIELD_X_SHIFT;
	tempMapXOfs_frac = mapXOfs_f - mapXOfs;
	tempMapXOfs_layer = 1;
	tempBackMove = backMove;
	tempScrollExtraPx  = endlessScrollExtraPx1;  // this batch rides layer 1
	tempScrollYLayer   = 1;
	tempScrollBaseStep = (map1YDelayMax > 1 && backMove < 2) ? 1 : (int)backMove;
	tempScrollDelayMax = map1YDelayMax;
	// Layer 1 records both terrain and enemy sprites before the ey advance, so their
	// fractional phases match. Health bars are drawn after the advance; pull their
	// integer anchor back by the same amount and retain the pre-advance phase.
	tempScrollYBase    = 0;
	tempScrollYfrac    = bg_layer_yfrac[1];
	tempScrollYBaseBar = -(tempBackMove + tempScrollExtraPx);
	tempScrollYfracBar = bg_layer_yfrac[1];
	JE_drawEnemy(50);
	JE_drawEnemy(100);

	if (enemyOnScreen == 0 || enemyOnScreen == lastEnemyOnScreen)
	{
		if (stopBackgroundNum == 1)
			stopBackgroundNum = 9;
	}

	if (smoothies[0] && processorType > 2 && smoothie_data[0] > 0)
	{
		if (render_list_recording)
			rl_rec_smoothie_filter(RC_LAVA_FILTER);
		lava_filter(game_screen, VGAScreen);
		VGAScreen = game_screen;
	}

	if (superWild)
	{
		neat += 3;
		JE_darkenBackground(neat);
	}

	/*-----------------------BACKGROUNDS------------------------*/
	/*-----------------------BACKGROUND 2------------------------*/
	if (!(smoothies[2-1] && processorType < 4) &&
	    !(smoothies[1-1] && processorType == 3))
	{
		if (background2over == 1)
		{
			if (wild && !background2notTransparent)
				draw_background_2_blend(VGAScreen);
			else
				draw_background_2(VGAScreen);
		}
	}

	if (superWild)
	{
		neat++;
		JE_darkenBackground(neat);
	}

	if (background3over == 2)
		draw_background_3(VGAScreen);

	/* New Enemy */
	if (enemiesActive && levelEnemyMax > 0 && mt_rand() % 100 > levelEnemyFrequency)
	{
		tempW = levelEnemy[mt_rand() % levelEnemyMax];
		if (tempW == 2)
			soundQueue[3] = S_WEAPON_7;
		b = JE_newEnemy(0, tempW, 0);
	}

	if (processorType > 1 && smoothies[3-1])
	{
		if (render_list_recording)
			rl_rec_smoothie_filter(RC_ICED_BLUR);
		iced_blur_filter(game_screen, VGAScreen);
		VGAScreen = game_screen;
	}
	if (processorType > 1 && smoothies[4-1])
	{
		if (render_list_recording)
			rl_rec_smoothie_filter(RC_BLUR);
		blur_filter(game_screen, VGAScreen);
		VGAScreen = game_screen;
	}

	/* Draw Sky Enemy */
	if (!skyEnemyOverAll)
	{
		lastEnemyOnScreen = enemyOnScreen;

		tempMapXOfs = mapX2Ofs + PLAYFIELD_X_SHIFT;
		tempMapXOfs_frac = mapX2Ofs_f - mapX2Ofs;
		tempMapXOfs_layer = 2;
		tempBackMove = 0;
		tempScrollExtraPx  = 0;     // layer-2 anchor: any ride is per-enemy via skyGlueThisEnemy
		tempScrollYLayer   = 0;
		tempScrollBaseStep = 0;
		tempScrollDelayMax = 1;
		tempScrollYBase = tempScrollYBaseBar = 0;
		tempScrollYfrac = tempScrollYfracBar = 0.0f;
		JE_drawEnemy(25);

		if (enemyOnScreen == lastEnemyOnScreen)
		{
			if (stopBackgroundNum == 2)
				stopBackgroundNum = 9;
		}
	}

	if (background3over == 0)
		draw_background_3(VGAScreen);

	/* Draw Top Enemy */
	if (!topEnemyOver)
	{
		tempMapXOfs = ((background3x1 == 0) ? oldMapX3Ofs : mapXOfs) + PLAYFIELD_X_SHIFT;
		tempMapXOfs_frac = (background3x1 == 0) ? (oldMapX3Ofs_f - oldMapX3Ofs) : (mapXOfs_f - mapXOfs);
		tempMapXOfs_layer = 3;
		tempBackMove = backMove3;
		tempScrollExtraPx  = endlessScrollExtraPx3;  // this batch rides layer 3
		tempScrollYLayer   = 3;
		tempScrollBaseStep = (int)backMove3;
		tempScrollDelayMax = 1;
		// All enemies are authored and recorded at their pre-advance ey phase. Layer 3's
		// terrain is post-advance, but shifting only this bank's sprites by the current
		// delta breaks cross-bank structures (BRAINIAC) by exactly 1px normally and 2px
		// on a Slipstream pulse. Preserve the entity phase; pull its post-move bar back.
		tempScrollYBase    = 0;
		tempScrollYfrac    = bg_layer_yfrac[3];
		tempScrollYBaseBar = -(tempBackMove + tempScrollExtraPx);
		tempScrollYfracBar = bg_layer_yfrac[3];
		JE_drawEnemy(75);
	}

	/* Player Shot Images */
	for (int z = 0; z < MAX_PWEAPON; z++)
	{
		if (shotAvail[z] != 0)
		{
			bool is_special = false;
			int tempShotX = 0, tempShotY = 0;
			JE_byte chain;
			JE_byte playerNum;
			JE_word tempX2, tempY2;
			JE_integer damage;
			
			if (!player_shot_move_and_draw(z, &is_special, &tempShotX, &tempShotY, &damage, &temp2, &chain, &playerNum, &tempX2, &tempY2))
			{
				goto draw_player_shot_loop_end;
			}

			// OVERCHARGE / Overdrive / Heavy Rounds perk (endless): your weapons hit harder.
			// Gate on the computed percent so any damage source (sector mod or run perk) applies.
			if (endlessMode && endlessPlayerDamagePercent() != 100)
				damage = damage * endlessPlayerDamagePercent() / 100;

			for (b = 0; b < 100; b++)
			{
				if (enemyAvail[b] == 0)
				{
					bool collided;

					if (z == MAX_PWEAPON - 1)
					{
						temp = 25 - abs(zinglonDuration - 25);
						collided = abs(enemy[b].ex + enemy[b].mapoffset - (player[0].x + 7)) < temp;
						temp2 = 9;
						chain = 0;
						damage = 10;
					}
					else if (is_special)
					{
						collided = ((enemy[b].enemycycle == 0) &&
						            (abs(enemy[b].ex + enemy[b].mapoffset - tempShotX - tempX2) < (25 + tempX2)) &&
						            (abs(enemy[b].ey - tempShotY - 12 - tempY2)                 < (29 + tempY2))) ||
						           ((enemy[b].enemycycle > 0) &&
						            (abs(enemy[b].ex + enemy[b].mapoffset - tempShotX - tempX2) < (13 + tempX2)) &&
						            (abs(enemy[b].ey - tempShotY - 6 - tempY2)                  < (15 + tempY2)));
					}
					else
					{
						collided = ((enemy[b].enemycycle == 0) &&
						            (abs(enemy[b].ex + enemy[b].mapoffset - tempShotX) < 25) && (abs(enemy[b].ey - tempShotY - 12) < 29)) ||
						           ((enemy[b].enemycycle > 0) &&
						            (abs(enemy[b].ex + enemy[b].mapoffset - tempShotX) < 13) && (abs(enemy[b].ey - tempShotY - 6) < 15));
					}

					if (collided)
					{
						if (chain > 0)
						{
							shotMultiPos[SHOT_MISC] = 0;
							b = player_shot_create(0, SHOT_MISC, tempShotX, tempShotY, mouseX, mouseY, chain, playerNum);
							shotAvail[z] = 0;
							goto draw_player_shot_loop_end;
						}

						infiniteShot = false;

						if (damage == 99)
						{
							damage = 0;
							doIced = 40;
							enemy[b].iced = 40;
						}
						else
						{
							doIced = 0;
							if (damage >= 250)
							{
								damage = damage - 250;
								infiniteShot = true;
							}
						}

						int armorleft = enemy[b].armorleft;

						bool has_boss_bar = false;
						for (unsigned int i = 0; i < COUNTOF(boss_bar); i++)
							if (enemy[b].linknum == boss_bar[i].link_num)
								has_boss_bar = true;

						// Nx boss HP (expert-mode and/or endless-depth). Both use the same
						// damage accumulator: spend 1 armor per N damage dealt, so the boss
						// effectively has N times its HP. The two multipliers combine.
						int bossHpMult = 1;
						if (expertMode)
							bossHpMult *= expertBossHpMult;
						if (endlessMode)
							bossHpMult *= endlessBossHpMult();
						// Combined divisor: boss depth-scaling and/or endless elite/champion
						// tier (elites use the accumulator too; an elite boss gets a capped bump).
						int hpMult = endlessMode ? endlessEnemyHpMult(has_boss_bar, bossHpMult, enemy[b].eliteState)
						                         : (has_boss_bar ? bossHpMult : 1);
						if (hpMult > 1)
						{
							enemy[b].damageAccum += damage;
							damage = enemy[b].damageAccum / hpMult;
							enemy[b].damageAccum -= damage * hpMult;
						}

						temp = enemy[b].linknum;
						if (temp == 0)
							temp = 255;

						if (enemy[b].armorleft < 255)
						{
							for (unsigned int i = 0; i < COUNTOF(boss_bar); i++)
								if (temp == boss_bar[i].link_num)
									boss_bar[i].color = 6;

							if (enemy[b].enemyground)
								enemy[b].filter = temp2;

							for (unsigned int e = 0; e < COUNTOF(enemy); e++)
							{
								if (enemy[e].linknum == temp &&
								    enemyAvail[e] != 1 &&
								    enemy[e].enemyground != 0)
								{
									if (doIced)
										enemy[e].iced = doIced;
									enemy[e].filter = temp2;
								}
							}
						}

						if (armorleft > damage)
						{
							if (z != MAX_PWEAPON - 1)
							{
								if (enemy[b].armorleft != 255)
								{
									if (!enemy[b].healthbar_seen)
									{
										// latch the pre-hit armor as the bar's "full" value
										enemy[b].healthbar_seen = true;
										enemy[b].healthbar_max = armorleft;
									}
									enemy[b].armorleft -= damage;
									JE_setupExplosion(tempShotX, tempShotY, 0, 0, false, false);
								}
								else
								{
									JE_doSP(tempShotX + 6, tempShotY + 6, damage / 2 + 3, damage / 4 + 2, temp2, false);
								}
							}

							soundQueue[5] = S_ENEMY_HIT;

							if ((armorleft - damage <= enemy[b].edlevel) &&
							    ((!enemy[b].edamaged) ^ (enemy[b].edani < 0)))
							{

								for (temp3 = 0; temp3 < 100; temp3++)
								{
									if (enemyAvail[temp3] != 1)
									{
										int linknum = enemy[temp3].linknum;
										if (
										     (temp3 == b) ||
										     (
										       (temp != 255) &&
										       (
										         ((enemy[temp3].edlevel > 0) && (linknum == temp)) ||
										         (
										           (enemyContinualDamage && (temp - 100 == linknum)) ||
										           ((linknum > 40) && (linknum / 20 == temp / 20) && (linknum <= temp))
										         )
										       )
										     )
										   )
										{
											enemy[temp3].enemycycle = 1;

											enemy[temp3].edamaged = !enemy[temp3].edamaged;

											if (enemy[temp3].edani != 0)
											{
												enemy[temp3].ani = abs(enemy[temp3].edani);
												enemy[temp3].aniactive = 1;
												enemy[temp3].animax = 0;
												enemy[temp3].animin = enemy[temp3].edgr;
												enemy[temp3].enemycycle = enemy[temp3].animin - 1;

											}
											else if (enemy[temp3].edgr > 0)
											{
												enemy[temp3].egr[1-1] = enemy[temp3].edgr;
												enemy[temp3].ani = 1;
												enemy[temp3].aniactive = 0;
												enemy[temp3].animax = 0;
												enemy[temp3].animin = 1;
											}
											else
											{
												enemyAvail[temp3] = 1;
												enemyKilled++;
												endlessCountKill(enemy[temp3].linknum);
												if (endlessMode && enemy[temp3].eliteState >= 2)
													endlessAwardEliteKill(enemy[temp3].eliteState);
											}

											enemy[temp3].aniwhenfire = 0;

											if (enemy[temp3].armorleft > (unsigned char)enemy[temp3].edlevel)
												enemy[temp3].armorleft = enemy[temp3].edlevel;

											JE_integer tempX = enemy[temp3].ex + enemy[temp3].mapoffset;
											JE_integer tempY = enemy[temp3].ey;

											if (enemyDat[enemy[temp3].enemytype].esize != 1)
												JE_setupExplosion(tempX, tempY - 6, 0, 1, false, false);
											else
												JE_setupExplosionLarge(enemy[temp3].enemyground, enemy[temp3].explonum / 2, tempX, tempY);
										}
									}
								}
							}
						}
						else
						{

							if ((temp == 254) && (superEnemy254Jump > 0))
								JE_eventJump(superEnemy254Jump);

							for (temp2 = 0; temp2 < 100; temp2++)
							{
								if (enemyAvail[temp2] != 1)
								{
									temp3 = enemy[temp2].linknum;
									if ((temp2 == b) || (temp == 254) ||
									    ((temp != 255) && ((temp == temp3) || (temp - 100 == temp3) ||
									                       ((temp3 > 40) && (temp3 / 20 == temp / 20) && (temp3 <= temp)))))
									{

										int enemy_screen_x = enemy[temp2].ex + enemy[temp2].mapoffset;

										if (enemy[temp2].special)
										{
											assert((unsigned int) enemy[temp2].flagnum-1 < COUNTOF(globalFlags));
											globalFlags[enemy[temp2].flagnum-1] = enemy[temp2].setto;
										}

										if ((enemy[temp2].enemydie > 0) &&
										    !((superArcadeMode != SA_NONE) &&
										      (enemyDat[enemy[temp2].enemydie].value == 30000)))
										{
											int temp_b = b;
											tempW = enemy[temp2].enemydie;
											int enemy_offset = temp2 - (temp2 % 25);
											if (enemyDat[tempW].value > 30000)
											{
												enemy_offset = 0;
											}
											b = JE_newEnemy(enemy_offset, tempW, 0);
											if (b != 0)
											{
												if ((superArcadeMode != SA_NONE) && (enemy[b-1].evalue > 30000))
												{
													superArcadePowerUp++;
													if (superArcadePowerUp > 5)
														superArcadePowerUp = 1;
													enemy[b-1].egr[1-1] = 5 + superArcadePowerUp * 2;
													enemy[b-1].evalue = 30000 + superArcadePowerUp;
												}

												if (enemy[b-1].evalue != 0)
													enemy[b-1].scoreitem = true;
												else
													enemy[b-1].scoreitem = false;

												enemy[b-1].ex = enemy[temp2].ex;
												enemy[b-1].ey = enemy[temp2].ey;
											}
											b = temp_b;
										}

										if ((enemy[temp2].evalue > 0) && (enemy[temp2].evalue < 10000))
										{
											if (enemy[temp2].evalue == 1)
											{
												if (endlessMode)  // datacube -> random special weapon in endless (no cube archive)
													endlessGrantSpecial();
												else
													cubeMax++;
											}
											else
											{
												// in galaga mode player 2 is sidekick, so give cash to player 1
												player[galagaMode ? 0 : playerNum - 1].cash += enemy[temp2].evalue;
											}
										}

										if ((enemy[temp2].edlevel == -1) && (temp == temp3))
										{
											enemy[temp2].edlevel = 0;
											enemyAvail[temp2] = 2;
											enemy[temp2].egr[1-1] = enemy[temp2].edgr;
											enemy[temp2].ani = 1;
											enemy[temp2].aniactive = 0;
											enemy[temp2].animax = 0;
											enemy[temp2].animin = 1;
											enemy[temp2].edamaged = true;
											enemy[temp2].enemycycle = 1;
										}
										else
										{
											enemyAvail[temp2] = 1;
											enemyKilled++;
											endlessCountKill(enemy[temp2].linknum);
											if (endlessMode && enemy[temp2].eliteState >= 2)
												endlessAwardEliteKill(enemy[temp2].eliteState);
										}

										if (enemyDat[enemy[temp2].enemytype].esize == 1)
										{
											JE_setupExplosionLarge(enemy[temp2].enemyground, enemy[temp2].explonum, enemy_screen_x, enemy[temp2].ey);
											soundQueue[6] = S_EXPLOSION_9;
										}
										else
										{
											JE_setupExplosion(enemy_screen_x, enemy[temp2].ey, 0, 1, false, false);
											soundQueue[6] = S_EXPLOSION_8;
										}
									}
								}
							}
						}

						if (infiniteShot)
						{
							damage += 250;
						}
						else if (z != MAX_PWEAPON - 1)
						{
							if (damage <= armorleft)
							{
								shotAvail[z] = 0;
								goto draw_player_shot_loop_end;
							}
							else
							{
								playerShotData[z].shotDmg -= armorleft;
							}
						}
					}
				}
			}

draw_player_shot_loop_end:
			;
		}
	}

	/* Player movement indicators for shots that track your ship */
	for (uint i = 0; i < COUNTOF(player); ++i)
	{
		player[i].last_x_shot_move = player[i].x;
		player[i].last_y_shot_move = player[i].y;
	}
	
	/*=================================*/
	/*=======Collisions Detection======*/
	/*=================================*/
	
	for (uint i = 0; i < (twoPlayerMode ? 2 : 1); ++i)
		if (player[i].is_alive && !endLevel)
			JE_playerCollide(&player[i], i + 1);
	
	if (firstGameOver)
		JE_mainGamePlayerFunctions();      /*--------PLAYER DRAW+MOVEMENT---------*/

	if (!endLevel)
	{    /*MAIN DRAWING IS STOPPED STARTING HERE*/

		/* Draw Enemy Shots */
		for (int z = 0; z < ENEMY_SHOT_MAX; z++)
		{
			if (enemyShotAvail[z] == 0)
			{
				enemyShot[z].sxm += enemyShot[z].sxc;
				enemyShot[z].sx += enemyShot[z].sxm;

				if (enemyShot[z].tx != 0)
				{
					if (enemyShot[z].sx > player[0].x)
					{
						if (enemyShot[z].sxm > -enemyShot[z].tx)
							enemyShot[z].sxm--;
					}
					else
					{
						if (enemyShot[z].sxm < enemyShot[z].tx)
							enemyShot[z].sxm++;
					}
				}

				enemyShot[z].sym += enemyShot[z].syc;
				enemyShot[z].sy += enemyShot[z].sym;

				if (enemyShot[z].ty != 0)
				{
					if (enemyShot[z].sy > player[0].y)
					{
						if (enemyShot[z].sym > -enemyShot[z].ty)
							enemyShot[z].sym--;
					}
					else
					{
						if (enemyShot[z].sym < enemyShot[z].ty)
							enemyShot[z].sym++;
					}
				}

				// X cull is against the VISIBLE window: [PLAYFIELD_LEFT, PLAYFIELD_RIGHT] after
				// the composite crop (vanilla: 0..262, cull at >275/<=0). Right: 13 past the last
				// visible column, fully hidden and row-wrap-safe (blit_sprite2 doesn't clip X).
				// Left: <=0 is already 12+ px past hidden thanks to the crop margin.
				if (enemyShot[z].duration-- == 0 || enemyShot[z].sy > 190 || enemyShot[z].sy <= -14 || enemyShot[z].sx > PLAYFIELD_RIGHT + 13 || enemyShot[z].sx <= 0)
				{
					enemyShotAvail[z] = true;
				}
				else  // check if shot collided with player
				{
					for (uint i = 0; i < (twoPlayerMode ? 2 : 1); ++i)
					{
						if (player[i].is_alive &&
						    enemyShot[z].sx > player[i].x - (signed)player[i].shot_hit_area_x &&
						    enemyShot[z].sx < player[i].x + (signed)player[i].shot_hit_area_x &&
						    enemyShot[z].sy > player[i].y - (signed)player[i].shot_hit_area_y &&
						    enemyShot[z].sy < player[i].y + (signed)player[i].shot_hit_area_y)
						{
							JE_integer tempX = enemyShot[z].sx;
							JE_integer tempY = enemyShot[z].sy;
							temp = enemyShot[z].sdmg;

							enemyShotAvail[z] = true;

							JE_setupExplosion(tempX, tempY, 0, 0, false, false);

							if (player[i].invulnerable_ticks == 0)
							{
								if ((temp = JE_playerDamage(temp, &player[i])) > 0)
								{
									player[i].x_velocity += (enemyShot[z].sxm * temp) / 2;
									player[i].y_velocity += (enemyShot[z].sym * temp) / 2;
								}
							}

							break;
						}
					}

					if (enemyShotAvail[z] == false)
					{
						if (enemyShot[z].animax != 0)
						{
							if (++enemyShot[z].animate >= enemyShot[z].animax)
								enemyShot[z].animate = 0;
						}

						rl_current_id = RL_ID_ESHOT_BASE + z;
						// Record the shot's real per-tick velocity AND acceleration so the
						// render list can extrapolate it smoothly (past the generic snap
						// threshold, and without a decelerating shot appearing to reverse).
						rl_current_vel_x = enemyShot[z].sxm;
						rl_current_vel_y = enemyShot[z].sym;
						rl_current_acc_x = enemyShot[z].sxc;
						rl_current_acc_y = enemyShot[z].syc;
						if (enemyShot[z].sgr >= 500)
							blit_sprite2(VGAScreen, enemyShot[z].sx, enemyShot[z].sy, spriteSheet12, enemyShot[z].sgr + enemyShot[z].animate - 500);
						else
							blit_sprite2(VGAScreen, enemyShot[z].sx, enemyShot[z].sy, spriteSheet8, enemyShot[z].sgr + enemyShot[z].animate);
						rl_current_id = 0;
						rl_current_vel_x = 0;
						rl_current_vel_y = 0;
						rl_current_acc_x = 0;
						rl_current_acc_y = 0;
					}
				}

			}
		}
	}

	if (background3over == 1)
		draw_background_3(VGAScreen);

	/* Draw Top Enemy */
	if (topEnemyOver)
	{
		// This bank and layer 3 are both drawn after JE_mainGamePlayerFunctions updates
		// parallax. Use that same current anchor so foreground-mounted enemies (notably
		// EP1 DELIANI) do not trail the terrain by the player's variable per-tick pan.
		tempMapXOfs = ((background3x1 == 0) ? mapX3Ofs : mapXOfs) + PLAYFIELD_X_SHIFT;
		tempMapXOfs_frac = (background3x1 == 0) ? (mapX3Ofs_f - mapX3Ofs) : (mapXOfs_f - mapXOfs);
		tempMapXOfs_layer = 3;
		tempBackMove = backMove3;
		tempScrollExtraPx  = endlessScrollExtraPx3;  // this batch rides layer 3
		tempScrollYLayer   = 3;
		tempScrollBaseStep = (int)backMove3;
		tempScrollDelayMax = 1;
		// Keep every enemy bank at the common pre-advance entity phase; see the matching
		// !topEnemyOver path above. The bar is recorded after JE_drawEnemy advances ey.
		tempScrollYBase    = 0;
		tempScrollYfrac    = bg_layer_yfrac[3];
		tempScrollYBaseBar = -(tempBackMove + tempScrollExtraPx);
		tempScrollYfracBar = bg_layer_yfrac[3];
		JE_drawEnemy(75);
	}

	/* Draw Sky Enemy */
	if (skyEnemyOverAll)
	{
		lastEnemyOnScreen = enemyOnScreen;

		tempMapXOfs = mapX2Ofs + PLAYFIELD_X_SHIFT;
		tempMapXOfs_frac = mapX2Ofs_f - mapX2Ofs;
		tempMapXOfs_layer = 2;
		tempBackMove = 0;
		tempScrollExtraPx  = 0;     // layer-2 anchor: any ride is per-enemy via skyGlueThisEnemy
		tempScrollYLayer   = 0;
		tempScrollBaseStep = 0;
		tempScrollDelayMax = 1;
		tempScrollYBase = tempScrollYBaseBar = 0;
		tempScrollYfrac = tempScrollYfracBar = 0.0f;
		JE_drawEnemy(25);

		if (enemyOnScreen == lastEnemyOnScreen)
		{
			if (stopBackgroundNum == 2)
				stopBackgroundNum = 9;
		}
	}

	/*-------------------------- Sequenced Explosions -------------------------*/
	enemyStillExploding = false;
	for (int i = 0; i < MAX_REPEATING_EXPLOSIONS; i++)
	{
		if (rep_explosions[i].ttl != 0)
		{
			enemyStillExploding = true;

			if (rep_explosions[i].delay > 0)
			{
				rep_explosions[i].delay--;
				continue;
			}

			rep_explosions[i].y += backMove2 + endlessScrollExtraPx2 + 1;  // scroll-track layer 2 at smooth overclock pace; +1 fall speed unchanged
			JE_integer tempX = rep_explosions[i].x + (mt_rand() % 24) - 12;
			JE_integer tempY = rep_explosions[i].y + (mt_rand() % 27) - 24;

			if (rep_explosions[i].big)
			{
				JE_setupExplosionLarge(false, 2, tempX, tempY);

				if (rep_explosions[i].ttl == 1 || mt_rand() % 5 == 1)
					soundQueue[7] = S_EXPLOSION_11;
				else
					soundQueue[6] = S_EXPLOSION_9;

				rep_explosions[i].delay = 4 + (mt_rand() % 3);
			}
			else
			{
				JE_setupExplosion(tempX, tempY, 0, 1, false, false);

				soundQueue[5] = S_EXPLOSION_4;

				rep_explosions[i].delay = 3;
			}

			rep_explosions[i].ttl--;
		}
	}

	/*---------------------------- Draw Explosions ----------------------------*/
	for (int j = 0; j < MAX_EXPLOSIONS; j++)
	{
		if (explosions[j].ttl != 0)
		{
			if (!explosions[j].fixedPosition)
			{
				explosions[j].sprite++;
				explosions[j].y += explodeMove;
			}
			else if (explosions[j].followPlayer)
			{
				explosions[j].x += explosionFollowAmountX;
				explosions[j].y += explosionFollowAmountY;
			}
			explosions[j].y += explosions[j].deltaY;

			if (explosions[j].y > vga_height - 14)
			{
				explosions[j].ttl = 0;
			}
			else
			{
				// X guard (display only; ttl still runs): the blitters don't clip X, so a
				// far-offscreen explosion (e.g. a kill just past the left cull margin) would
				// row-wrap its pixels onto the opposite edge -- which the widescreen crop
				// makes VISIBLE (vanilla hid that region under the HUD). Both bounds are
				// well past fully-hidden, so nothing on-screen is lost.
				if (explosions[j].x > -12 && explosions[j].x < 344)
				{
					// Stable per-instance id: puff churn recycles slots, and a plain slot id
					// would mis-pair a recycled slot with its previous occupant. Fold in the
					// per-slot generation (4 values disambiguate consecutive reuses); j*4 + 3
					// stays within the EXPL id range (MAX_EXPLOSIONS*4 < 1000).
					rl_current_id = RL_ID_EXPL_BASE + j * 4 + (explosions[j].id_gen & 3);
					if (explosionTransparent)
						blit_sprite2_blend(VGAScreen, explosions[j].x, explosions[j].y, explosionSpriteSheet, explosions[j].sprite + 1);
					else
						blit_sprite2(VGAScreen, explosions[j].x, explosions[j].y, explosionSpriteSheet, explosions[j].sprite + 1);
					rl_current_id = 0;
				}

				explosions[j].ttl--;
			}
		}
	}

	if (!portConfigChange)
		portConfigDone = true;

	/*-----------------------BACKGROUNDS------------------------*/
	/*-----------------------BACKGROUND 2------------------------*/
	if (!(smoothies[2-1] && processorType < 4) &&
	    !(smoothies[1-1] && processorType == 3))
	{
		if (background2over == 2)
		{
			if (wild && !background2notTransparent)
				draw_background_2_blend(VGAScreen);
			else
				draw_background_2(VGAScreen);
		}
	}

	// Explosion sparks (superpixels): drawn AND recorded here, before the residual
	// snapshot below, so they're part of its baseline and interpolate via the replay
	// instead of snapping through the residual.
	JE_drawSP();

	// Smoothie levels: draw+record the enemy health bars before the residual snapshot
	// (like the sparks above) so they interpolate with their enemy and diff out of
	// rl_capture_residual_delta. Normal levels draw them later, after the filter, to keep
	// that path's z-order (bars above the level fade).
	if (anySmoothies)
		draw_enemy_health_bars();

	// Smoothie levels: snapshot the frame here -- after the last recorded blit (sparks/bars above
	// diff out), before the non-blit overlays (WARNING bars, fades, boss bar, HUD) so those are
	// captured as the per-frame residual and don't freeze at 35fps. notes.md §Smoothie levels.
	if (anySmoothies)
		memcpy(VGAScreen2->pixels, game_screen->pixels, (size_t)game_screen->h * game_screen->pitch);

	/*-------------------------Warning---------------------------*/
	if ((player[0].is_alive && player[0].armor < 6) ||
	    (twoPlayerMode && !galagaMode && player[1].is_alive && player[1].armor < 6))
	{
		int armor_amount = (player[0].is_alive && player[0].armor < 6) ? player[0].armor : player[1].armor;

		if (armorShipDelay > 0)
		{
			armorShipDelay--;
		}
		else
		{
			tempW = 560;
			b = JE_newEnemy(50, tempW, 0);
			if (b > 0)
			{
				enemy[b-1].enemydie = 560 + (mt_rand() % 3) + 1;
				enemy[b-1].eyc -= backMove3;
				enemy[b-1].armorleft = 4;
			}
			armorShipDelay = 500;
		}

		if ((player[0].is_alive && player[0].armor < 6 && (!isNetworkGame || thisPlayerNum == 1)) ||
		    (twoPlayerMode && player[1].is_alive && player[1].armor < 6 && (!isNetworkGame || thisPlayerNum == 2)))
		{

			tempW = armor_amount * 4 + 8;
			if (warningSoundDelay > tempW)
				warningSoundDelay = tempW;

			if (warningSoundDelay > 1)
			{
				warningSoundDelay--;
			}
			else
			{
				soundQueue[7] = S_WARNING;
				warningSoundDelay = tempW;
			}

			warningCol += warningColChange;
			if (warningCol > 113 + (14 - (armor_amount * 2)))
			{
				warningColChange = -warningColChange;
				warningCol = 113 + (14 - (armor_amount * 2));
			}
			else if (warningCol < 113)
			{
				warningColChange = -warningColChange;
			}
			const int playfield_left = PLAYFIELD_LEFT;
			const int playfield_right = playfield_left + PLAYFIELD_WIDTH - 1;
			const char warning_text[] = "WARNING";
			const int warning_text_width = JE_textWidth(warning_text, TINY_FONT);
			const int warning_x = playfield_left + (PLAYFIELD_WIDTH - warning_text_width) / 2;
			const int gap_margin = 1;
			fill_rectangle_xy(VGAScreen, playfield_left, 181, warning_x - gap_margin - 1, 183, warningCol);
			fill_rectangle_xy(VGAScreen, warning_x + warning_text_width + gap_margin, 181, playfield_right, 183, warningCol);
			fill_rectangle_xy(VGAScreen, playfield_left, 0, playfield_right, 3, warningCol);

			JE_outText(VGAScreen, warning_x, 178, warning_text, 7, (warningCol % 16) / 2);

		}
	}

	/*------- Random Explosions --------*/
	// Full visible playfield: 280 was the pre-widescreen width, so explosions used to
	// stop short of the widened right edge (see composite_playfield / video.h);
	// 184 = full playfield height (vanilla stopped at 180).
	if (randomExplosions && mt_rand() % 10 == 1)
		JE_setupExplosionLarge(false, 20, PLAYFIELD_LEFT + mt_rand() % PLAYFIELD_WIDTH, mt_rand() % 184);

	/*=================================*/
	/*=======The Sound Routine=========*/
	/*=================================*/
	if (firstGameOver)
	{
		temp = 0;
		for (temp2 = 0; temp2 < COUNTOF(soundQueue); temp2++)
		{
			if (soundQueue[temp2] != S_NONE)
			{
				temp = soundQueue[temp2];
				if (temp2 == 3)
					temp3 = fxPlayVol;
				else if (temp == 15)
					temp3 = fxPlayVol / 4;
				else   /*Lightning*/
					temp3 = fxPlayVol / 2;

				multiSamplePlay(soundSamples[temp-1], soundSampleCount[temp-1], temp2, temp3);

				soundQueue[temp2] = S_NONE;
			}
		}
	}

	if (returnActive && enemyOnScreen == 0)
	{
		JE_eventJump(65535);
		returnActive = false;
	}

	/*-------      DEbug      ---------*/
	debugTime = SDL_GetTicks();
	tempW = lastmouse_but;

	if (debug)
	{
		for (size_t i = 0; i < 9; i++)
		{
			tempStr[i] = '0' + smoothies[i];
		}
		tempStr[9] = '\0';
		sprintf(buffer, "SM = %s", tempStr);
		JE_outText(VGAScreen, 30, 70, buffer, 4, 0);

		sprintf(buffer, "Memory left = %d", -1);
		JE_outText(VGAScreen, 30, 80, buffer, 4, 0);
		sprintf(buffer, "Enemies onscreen = %d", enemyOnScreen);
		JE_outText(VGAScreen, 30, 90, buffer, 6, 0);

		debugHist = debugHist + abs((JE_longint)debugTime - (JE_longint)lastDebugTime);
		debugHistCount++;
		sprintf(tempStr, "%2.3f", 1000.0f / roundf(debugHist / debugHistCount));
		sprintf(buffer, "X:%d Y:%-5d  %s FPS  %d %d %d %d", (mapX - 1) * 12 + player[0].x, curLoc, tempStr, player[0].x_velocity, player[0].y_velocity, player[0].x, player[0].y);
		JE_outText(VGAScreen, 45, 175, buffer, 15, 3);
		lastDebugTime = debugTime;
	}

	/*Pentium Speed Mode?*/
	if (pentiumMode)
	{
		frameCountMax = (frameCountMax == 2) ? 3 : 2;
	}

	/*--------  Level Timer    ---------*/
	if (levelTimer && levelTimerCountdown > 0)
	{
		levelTimerCountdown--;
		if (levelTimerCountdown == 0)
			JE_eventJump(levelTimerJumpTo);

		if (timedBattleMode)
		{
			// No-op; play no sound effects
		}
		else if (levelTimerCountdown > 200)
		{
			if (levelTimerCountdown % 100 == 0)
				soundQueue[7] = S_WARNING;

			if (levelTimerCountdown % 10 == 0)
				soundQueue[6] = S_CLICK;
		}
		else if (levelTimerCountdown % 20 == 0)
		{
			soundQueue[7] = S_WARNING;
		}

		// Don't use floats due to rounding.
		sprintf(buffer, "%d.%d", levelTimerCountdown / 100, (levelTimerCountdown / 10) % 10);

		// Lay the countdown number and its "TIME" label out as one group and centre
		// that group on the playfield.
		const int label_w = JE_textWidth(miscText[66], TINY_FONT);
		const int counter_w = JE_textWidth(buffer, SMALL_FONT_SHAPES);
		const int group_w = counter_w + 3 + label_w;
		const int counter_x = PLAYFIELD_CENTER_X(group_w);
		const int label_x = counter_x + counter_w + 3;

		JE_textShade(VGAScreen, label_x, 6, miscText[66], 7, (levelTimerCountdown % 20) / 3, FULL_SHADE);
		JE_dString(VGAScreen, counter_x, 2, buffer, SMALL_FONT_SHAPES);
	}

	/*GAME OVER*/
	if (!constantPlay && !constantDie)
	{
		if (allPlayersGone)
		{
			if (player[0].exploding_ticks > 0 || player[1].exploding_ticks > 0)
			{
				if (galagaMode)
					player[1].exploding_ticks = 0;

				musicFade = true;
			}
			else
			{
				if (play_demo || normalBonusLevelCurrent || bonusLevelCurrent)
					reallyEndLevel = true;
				else
				{
					const int playfield_left = PLAYFIELD_LEFT;
					const int game_over_width = JE_textWidth(miscText[21], FONT_SHAPES);
					const int game_over_x = playfield_left + (PLAYFIELD_WIDTH - game_over_width) / 2;
					JE_dString(VGAScreen, game_over_x, 60, miscText[21], FONT_SHAPES); // game over
				}

				if (firstGameOver)
				{
					if (!play_demo)
					{
						play_song(SONG_GAMEOVER);
						set_volume(tyrMusicVolume, fxVolume);
					}
					// Drop any input still held/queued from the moment of death, so
					// GAME OVER doesn't dismiss itself instantly — require a fresh press.
					newkey = newmouse = false;
					firstGameOver = false;
				}

				if (!play_demo)
				{
					push_joysticks_as_keyboard();
					// Poll without clearing: the smooth-present loop in JE_starShowVGA
					// already consumed inter-tick SDL events into newkey/newmouse, so
					// clearing here would discard the press and GAME OVER would never respond.
					service_SDL_events(false);
					if ((newkey || button[0] || button[1] || button[2]) || newmouse)
					{
						reallyEndLevel = true;
					}
				}

				if (isNetworkGame)
					reallyEndLevel = true;
			}
		}
	}

	if (play_demo) // input kills demo
	{
		push_joysticks_as_keyboard();
		service_SDL_events(false);

		if (newkey || newmouse)
		{
			reallyEndLevel = true;

			stopped_demo = true;
		}
	}
	else // input handling for pausing, menu, cheats
	{
		service_SDL_events(false);

		if (newkey)
		{
			skipStarShowVGA = false;
			JE_mainKeyboardInput();
			newkey = false;
			if (skipStarShowVGA)
				goto level_loop;
		}

		if (pause_pressed || !windowHasFocus)
		{
			pause_pressed = false;

			if (isNetworkGame)
				pauseRequest = true;
			else
				JE_pauseGame();
		}

		if (ingamemenu_pressed)
		{
			ingamemenu_pressed = false;

			if (isNetworkGame)
			{
				inGameMenuRequest = true;
			}
			else
			{
				yourInGameMenuRequest = true;
				JE_doInGameSetup();
				skipStarShowVGA = true;
			}
		}
	}

	/*Network Update*/
#ifdef WITH_NETWORK
	if (isNetworkGame)
	{
		if (!reallyEndLevel)
		{
			Uint16 requests = (pauseRequest == true) |
			                  (inGameMenuRequest == true) << 1 |
			                  (skipLevelRequest == true) << 2 |
			                  (nortShipRequest == true) << 3;
			SDLNet_Write16(requests,        &packet_state_out[0]->data[14]);

			SDLNet_Write16(difficultyLevel, &packet_state_out[0]->data[16]);
			SDLNet_Write16(player[0].x,     &packet_state_out[0]->data[18]);
			SDLNet_Write16(player[1].x,     &packet_state_out[0]->data[20]);
			SDLNet_Write16(player[0].y,     &packet_state_out[0]->data[22]);
			SDLNet_Write16(player[1].y,     &packet_state_out[0]->data[24]);
			SDLNet_Write16(curLoc,          &packet_state_out[0]->data[26]);

			network_state_send();

			if (network_state_update())
			{
				assert(SDLNet_Read16(&packet_state_in[0]->data[26]) == SDLNet_Read16(&packet_state_out[network_delay]->data[26]));

				requests = SDLNet_Read16(&packet_state_in[0]->data[14]) ^ SDLNet_Read16(&packet_state_out[network_delay]->data[14]);
				if (requests & 1)
				{
					JE_pauseGame();
				}
				if (requests & 2)
				{
					yourInGameMenuRequest = SDLNet_Read16(&packet_state_out[network_delay]->data[14]) & 2;
					JE_doInGameSetup();
					yourInGameMenuRequest = false;
					if (haltGame)
						reallyEndLevel = true;
				}
				if (requests & 4)
				{
					levelTimer = true;
					levelTimerCountdown = 0;
					endLevel = true;
					levelEnd = 40;
				}
				if (requests & 8) // nortship
				{
					player[0].items.ship = 12;                     // Nort Ship
					player[0].items.special = 13;                  // Astral Zone
					player[0].items.weapon[FRONT_WEAPON].id = 36;  // NortShip Super Pulse
					player[0].items.weapon[REAR_WEAPON].id = 37;   // NortShip Spreader
					shipGr = 1;
				}

				for (int i = 0; i < 2; i++)
				{
					if (SDLNet_Read16(&packet_state_in[0]->data[18 + i * 2]) != SDLNet_Read16(&packet_state_out[network_delay]->data[18 + i * 2]) || SDLNet_Read16(&packet_state_in[0]->data[20 + i * 2]) != SDLNet_Read16(&packet_state_out[network_delay]->data[20 + i * 2]))
					{
						char temp[64];
						sprintf(temp, "Player %d is unsynchronized!", i + 1);

						JE_textShade(game_screen, 40, 110 + i * 10, temp, 9, 2, FULL_SHADE);
					}
				}
			}
		}

		JE_clearSpecialRequests();
	}
#endif

	/*Filtration*/
	if (filterActive)
	{
		if (render_list_recording)
			rl_rec_filter_screen(levelFilter, levelBrightness);

		// Smoothie residual: any full-screen grade -- colour flare (levelFilter != -99) or
		// brightness-only flash (levelBrightness != -99) -- must hit the pre-overlay snapshot too,
		// or it bakes into the residual and freezes the playfield. notes.md §Smoothie levels.
		if (anySmoothies)
			JE_filterScreenApply(VGAScreen2, levelFilter, levelBrightness);

		JE_filterScreen(levelFilter, levelBrightness);
	}

	// Smoothie levels already drew+recorded the enemy bars before the residual
	// snapshot (above) so they interpolate; here we draw them for normal levels only.
	if (!anySmoothies)
		draw_enemy_health_bars();
	draw_boss_bar();
	JE_updateGaugeFlash();

	JE_inGameDisplays();

	// Render-list capture for this tick ends here (everything below composites
	// or presents; it does not draw into the playfield).
	rl_end_record();
	rl_finalize();  // match against previous frame -> per-command motion deltas
	if (!anySmoothies)
		rl_capture_residual(game_screen, VGAScreen2);  // non-blit pixels (superpixels, boss bar)
	else
		rl_capture_residual_delta(VGAScreen2, game_screen);  // overlay-only (WARNING bars, boss bar, HUD) -> re-applied unfiltered on the display frame
	vt_ship_tick();       // fold external forces / repositions into the variable-dt ship
	ship_pred_on_tick();  // snapshot authoritative ship pos for render-rate prediction
	// This tick's ship velocity lets the render list interpolate ship-attached shots
	// (the orbiting killer's circle); matches the c->dx just computed by rl_finalize.
	for (int p = 0; p < (twoPlayerMode ? 2 : 1); ++p)
		rl_set_ship_vel(p, ship_vel_x[p], ship_vel_y[p]);
#if RL_SELFTEST
	{
		static int seen = 0;
		if (seen < 5)
		{
			++seen;
			fprintf(stderr, "RL: in-game frame %d, anySmoothies=%d filterActive=%d cmds=%zu\n",
			        seen, (int)anySmoothies, (int)filterActive, rl_count());
		}
	}
	// Without smoothie per-pixel effects the replayed list (including the recorded
	// full-screen colour filter) must reproduce game_screen exactly.
	if (!anySmoothies)
	{
		const size_t mism = rl_replay_and_compare(VGAScreen2, game_screen);
		static int clean_frames = 0, bad_frames = 0;
		++clean_frames;
		if (mism != 0)
		{
			++bad_frames;
			if (bad_frames <= 20)
				fprintf(stderr, "RL selftest: %zu mismatched bytes (clean frame %d, %zu cmds)\n",
				        mism, clean_frames, rl_count());
		}
		else if (clean_frames % 120 == 1)
		{
			fprintf(stderr, "RL selftest: clean frame %d OK (%zu cmds)\n", clean_frames, rl_count());
		}
	}
#endif

	VGAScreen = VGAScreenSeg; /* side-effect of game_screen */

	JE_starShowVGA();

	/*Start backgrounds if no enemies on screen
	  End level if number of enemies left to kill equals 0.*/
	if (stopBackgroundNum == 9 && backMove == 0 && !enemyStillExploding)
	{
		backMove = 1;
		backMove2 = 2;
		backMove3 = 3;
		explodeMove = 2;
		stopBackgroundNum = 0;
		stopBackgrounds = false;
		if (waitToEndLevel)
		{
			endLevel = true;
			levelEnd = 40;
		}
		if (allPlayersGone)
		{
			reallyEndLevel = true;
		}
	}

	if (!endLevel && enemyOnScreen == 0)
	{
		if (readyToEndLevel && !enemyStillExploding)
		{
			if (levelTimerCountdown > 0)
			{
				levelTimer = false;
			}
			readyToEndLevel = false;
			endLevel = true;
			levelEnd = 40;
			if (allPlayersGone)
			{
				reallyEndLevel = true;
			}
		}
		if (stopBackgrounds)
		{
			stopBackgrounds = false;
			backMove = 1;
			backMove2 = 2;
			backMove3 = 3;
			explodeMove = 2;
		}
	}

	// Map-stop softlock watchdog: a boss killed before its script finishes staging the fight can
	// orphan a group member frozen above the screen -- unreachable, and with the event clock
	// stopped unmovable -- so it holds enemyOnScreen != 0 forever. After a stop is held long enough
	// with one present, cull it like an off-playfield enemy and the level resumes. notes.md §Level scripting.
	enemyParkedAbove = count_stuck_above_screen();
	if (!endLevel && stopBackgrounds && !forceEvents && enemyParkedAbove != 0)
	{
		if (++mapStopStallTicks >= MAP_STOP_STALL_LIMIT)
		{
			for (int i = 0; i < 100; i++)
				if (enemyAvail[i] != 1 && enemy_stuck_above_screen(i))
					enemyAvail[i] = 1;
			mapStopStallTicks = 0;
		}
	}
	else
		mapStopStallTicks = 0;

	/*Other Network Functions*/
	JE_handleChat();

	if (reallyEndLevel)
	{
		goto start_level;
	}
	goto level_loop;
}

// Full-screen picture-wipe styles used by the level-script commands U / V / R.
typedef enum
{
	WIPE_U,  // vertical wipe (new picture slides in over the old)
	WIPE_V,  // vertical wipe (new picture revealed from the opposite edge)
	WIPE_R,  // horizontal wipe
} WipeKind;

// Composite one frame of a picture wipe at boundary position z: the old image
// (VGAScreen2) and the new image (pic_buffer) meet at row/column z. These are the
// original per-step inner loops verbatim -- only the outer pacing changes, in
// animate_picture_wipe below.
static void compose_wipe_frame(WipeKind kind, int z, const Uint8 *pic_buffer)
{
	const int pitch = VGAScreen->pitch;
	Uint8 *vga = VGAScreen->pixels;
	Uint8 *vga2 = VGAScreen2->pixels;
	const Uint8 *pic;

	switch (kind)
	{
	case WIPE_U:
		pic = pic_buffer + (vga_height - 1 - z) * pitch;
		for (int y = 0; y < vga_height; y++)
		{
			if (y <= z)
			{
				memcpy(vga, pic, pitch);
				pic += pitch;
			}
			else
			{
				memcpy(vga, vga2, pitch);
				vga2 += pitch;
			}
			vga += pitch;
		}
		break;

	case WIPE_V:
		pic = pic_buffer;
		for (int y = 0; y < vga_height; y++)
		{
			if (y <= vga_height - 1 - z)
			{
				memcpy(vga, vga2, pitch);
				vga2 += pitch;
			}
			else
			{
				memcpy(vga, pic, pitch);
				pic += pitch;
			}
			vga += pitch;
		}
		break;

	case WIPE_R:
		pic = pic_buffer;
		for (int y = 0; y < vga_height; y++)
		{
			memcpy(vga, vga2 + z, pitch - 1 - z);
			vga += pitch - z;
			vga2 += VGAScreen2->pitch;
			memcpy(vga, pic, z + 1);
			vga += z;
			pic += pitch;
		}
		break;
	}
}

// Animate a full-screen picture wipe. The classic path advances the wipe boundary
// one row/column per sim tick (setDelay(1) + wait); the smooth path advances it by
// real elapsed time and presents every display frame (vsync-aligned), so the wipe
// glides at the monitor's refresh instead of the ~35Hz-paced SDL_Delay cadence.
// Same total duration and end image; a key press skips to the end, as before.
static void animate_picture_wipe(WipeKind kind, const Uint8 *pic_buffer)
{
	const int steps = (kind == WIPE_R) ? (VGAScreen->pitch - 1) : vga_height;

	if (smoothMotion)
	{
		const float duration = steps * get_delay_period();  // ms; == classic (steps x one delay unit)
		const Uint64 freq = SDL_GetPerformanceFrequency();
		const Uint64 begin = SDL_GetPerformanceCounter();
		const float counter_to_ms = 1000.0f / (float)freq;

		for (;;)
		{
			if (newkey)
				break;

			const float t = (float)(SDL_GetPerformanceCounter() - begin) * counter_to_ms / duration;
			const bool done = t >= 1.0f;
			int z = done ? steps - 1 : (int)(t * steps);
			if (z > steps - 1)
				z = steps - 1;

			compose_wipe_frame(kind, z, pic_buffer);
			JE_showVGA();

			if (done)
				break;

			if (!output_vsync)
				limit_render_fps();
			service_SDL_events(false);
		}
	}
	else
	{
		for (int z = 0; z < steps; z++)
		{
			if (!newkey)
			{
				setDelay(1);
				compose_wipe_frame(kind, z, pic_buffer);
				JE_showVGA();
				service_wait_delay();
			}
		}
	}
}

/* --- Load Level/Map Data --- */
void JE_loadMap(void)
{
	JE_DanCShape shape;

	JE_word x, y;
	JE_integer yy;
	JE_word mapSh[3][128]; /* [1..3, 0..127] */
	JE_byte *ref[3][128]; /* [1..3, 0..127] */
	char s[256];

	JE_byte mapBuf[15 * 600]; /* [1..15 * 600] */
	JE_word bufLoc;

	char buffer[256];
	int i;
	Uint8 pic_buffer[vga_width * vga_height]; /* screen buffer, 8-bit specific */

	crashlog_set_phase("loading level map");

	lastCubeMax = cubeMax;

	/*Defaults*/
	songBuy = DEFAULT_SONG_BUY;  /*Item Screen default song*/

	/* Load LEVELS.DAT - Section = MAINLEVEL */
	saveLevel = mainLevel;

new_game:
	set_menu_centered(true);
	galagaMode = false;
	useLastBank = false;
	extraGame = false;
	engageMode = false;
	haltGame = false;

	gameLoaded = false;

	if (!play_demo)
	{
		do
		{
			FILE *ep_f = dir_fopen_die(data_dir(), episode_file, "rb");
			const long ep_end = ftell_eof(ep_f);  // guard the scans below against reading past EOF

			jumpSection = false;
			loadLevelOk = false;

			/* Seek Section # Mainlevel. An out-of-range mainLevel -- a desynced save (episode
			 * switched to a shorter episode while the level index stayed high) or a bad next-level
			 * pointer -- used to run this scan off the end of the file, where read_encrypted_pascal_string
			 * hits fread_die -> exit() and killed the game with NO crash log. Bound the scan by EOF and
			 * recover to the title instead. See notes.md / crashlog. */
			int x = 0;
			while (x < mainLevel)
			{
				if (ftell(ep_f) >= ep_end)
				{
					char detail[192];
					snprintf(detail, sizeof(detail),
					         "episode %d has no section %d (file holds only %d); out-of-range save/level -- returning to title",
					         (int)episodeNum, (int)mainLevel, x);
					fprintf(stderr, "error: %s\n", detail);
					crashlog_note("RECOVERED (JE_loadMap: level section out of range)", detail);
					fclose(ep_f);
					mainLevel = 0;   // caller (JE_main) sees mainLevel == 0 and returns to the title screen
					return;
				}
				read_encrypted_pascal_string(s, sizeof(s), ep_f);
				if (s[0] == '*')
				{
					x++;
					s[0] = ' ';
				}
			}

			ESCPressed = false;

			do
			{
				if (gameLoaded)
				{
					fclose(ep_f);

					if (mainLevel == 0)  // if quit itemscreen
						return;          // back to title screen
					else if (endlessMode)  // endless save loaded from within this shop: bail out so JE_main
						return;             // resumes at its outpost (endlessBetweenLevels), not a campaign reload
					else
						goto new_game;
				}

				// Same EOF guard as the section seek above: a section that ends without a ']L' or a
				// jump (truncated/garbled data) would read past EOF into fread_die -> exit(). Recover.
				if (ftell(ep_f) >= ep_end)
				{
					char detail[192];
					snprintf(detail, sizeof(detail),
					         "episode %d section %d ended with no level to load (truncated/garbled data) -- returning to title",
					         (int)episodeNum, (int)mainLevel);
					fprintf(stderr, "error: %s\n", detail);
					crashlog_note("RECOVERED (JE_loadMap: section had no loadable level)", detail);
					fclose(ep_f);
					mainLevel = 0;
					return;
				}

				strcpy(s, " ");
				read_encrypted_pascal_string(s, sizeof(s), ep_f);

				if (s[0] == ']')
				{
					switch (s[1])
					{
					case 'A':
						JE_playAnim("tyrend.anm", 0, 7);
						break;

					case 'G':
						mapOrigin = atoi(s + 4);
						mapPNum   = atoi(s + 7);
						for (i = 0; i < mapPNum; i++)
						{
							mapPlanet[i] = atoi(s + 1 + (i + 1) * 8);
							mapSection[i] = atoi(s + 4 + (i + 1) * 8);
						}
						break;

					case '?':
						temp = atoi(s + 4);
						for (i = 0; i < temp; i++)
						{
							cubeList[i] = atoi(s + 3 + (i + 1) * 4);
						}
						if (cubeMax > temp)
							cubeMax = temp;
						break;

					case '!':
						cubeMax = atoi(s + 4);    /*Auto set CubeMax*/
						break;

					case '+':
						temp = atoi(s + 4);
						cubeMax += temp;
						if (cubeMax > 4)
							cubeMax = 4;
						break;

					case 'g':
						galagaMode = true;   /*GALAGA mode*/

						player[1].items = player[0].items;
						player[1].items.weapon[REAR_WEAPON].id = 15;  // Vulcan Cannon
						for (uint i = 0; i < COUNTOF(player[1].items.sidekick); ++i)
							player[1].items.sidekick[i] = 0;          // None
						break;

					case 'x':
						extraGame = true;
						break;

					case 'e': // ENGAGE mode, used for mini-games
						engageMode = true;
						doNotSaveBackup = true;
						constantDie = false;
						onePlayerAction = true;
						superTyrian = true;
						twoPlayerMode = false;

						player[0].cash = 0;

						player[0].items.ship = 13;                     // The Stalker 21.126
						player[0].items.weapon[FRONT_WEAPON].id = 39;  // Atomic RailGun
						player[0].items.weapon[REAR_WEAPON].id = 0;    // None
						for (uint i = 0; i < COUNTOF(player[0].items.sidekick); ++i)
							player[0].items.sidekick[i] = 0;           // None
						player[0].items.generator = 2;                 // Advanced MR-12
						player[0].items.shield = 4;                    // Advanced Integrity Field
						player[0].items.special = 0;                   // None

						player[0].items.weapon[FRONT_WEAPON].power = 3;
						player[0].items.weapon[REAR_WEAPON].power = 1;
						break;

					case 'J':  // section jump
						temp = atoi(s + 3);
						mainLevel = temp;
						jumpSection = true;
						break;

					case '2':  // two-player section jump
						temp = atoi(s + 3);
						if (twoPlayerMode || onePlayerAction)
						{
							mainLevel = temp;
							jumpSection = true;
						}
						break;

					case 'w':  // Stalker 21.126 section jump
						temp = atoi(s + 3);   /*Allowed to go to Time War?*/
						if (player[0].items.ship == 13)
						{
							mainLevel = temp;
							jumpSection = true;
						}
						break;

					case 't':
						temp = atoi(s + 3);
						if (levelTimer && levelTimerCountdown == 0)
						{
							mainLevel = temp;
							jumpSection = true;
						}
						break;

					case 'l':
						temp = atoi(s + 3);
						if (!all_players_alive())
						{
							mainLevel = temp;
							jumpSection = true;
						}
						break;

					case 's':
						saveLevel = mainLevel;
						break; /*store savepoint*/

					case 'b':
						if (twoPlayerMode)
							temp = 22;
						else
							temp = 11;
						if (!endlessMode)  // mid-level savepoint: unstable for endless; it autosaves at the outpost instead (endlessBetweenLevels)
						{
							JE_saveGame(11, "LAST LEVEL    ");
							endlessSaveSlot(11);  // keep the endless sidecar in sync: drop any stale record so this campaign save isn't read back as endless
						}
						break;

					case 'i':
						temp = atoi(s + 3);
						songBuy = temp - 1;
						break;

					case 'I': /*Load Items Available Information*/
						memset(&itemAvail, 0, sizeof(itemAvail));

						for (int i = 0; i < 9; ++i)
						{
							read_encrypted_pascal_string(s, sizeof(s), ep_f);

							char buf[256];
							strncpy(buf, (strlen(s) > 8) ? s + 8 : "", sizeof(buf));

							int j = 0, temp;
							while (str_pop_int(buf, &temp))
								itemAvail[i][j++] = temp;
							itemAvailMax[i] = j;
						}

						// Re-offer the DOS Charge-Laser Cannon in its original shops. Tyrian
						// 2000 reused its option slot 16 for the Mint-O-Ship, so it can't ride
						// the stock shop data; inject its re-added slot into the Opt1/Opt2
						// sidekick lists (per originaldostyriandata/LEVELS{2,3}.DAT).
						if (chargeLaserSlot > 0 &&
						    ((episodeNum == 2 && (mainLevel == 3 || mainLevel == 11 || mainLevel == 16)) ||
						     (episodeNum == 3 && mainLevel == 16)))
						{
							if (itemAvailMax[5] < 10) itemAvail[5][itemAvailMax[5]++] = chargeLaserSlot;
							if (itemAvailMax[6] < 10) itemAvail[6][itemAvailMax[6]++] = chargeLaserSlot;
						}

						if (!endlessMode)
							JE_itemScreen();
						break;

					case 'L':
						nextLevel = atoi(s + 9);
						SDL_strlcpy(levelName, s + 13, 10);
						levelSong = atoi(s + 22);
						if (nextLevel == 0)
						{
							nextLevel = mainLevel + 1;
						}
						lvlFileNum = atoi(s + 25);
						loadLevelOk = true;
						bonusLevelCurrent = (strlen(s) > 28) & (s[28] == '$');
						normalBonusLevelCurrent = (strlen(s) > 27) & (s[27] == '$');
						gameJustLoaded = false;
						break;

					case '@':
						useLastBank = !useLastBank;
						break;

					case 'Q':
						ESCPressed = false;
						temp = secretHint + (mt_rand() % 3) * 3;

						if (twoPlayerMode)
						{
							for (uint i = 0; i < 2; ++i)
								snprintf(levelWarningText[i], sizeof(*levelWarningText), "%s %lu", miscText[40 + i], player[i].cash);
							strcpy(levelWarningText[2], "");
							levelWarningLines = 3;
						}
						else
						{
							sprintf(levelWarningText[0], "%s %lu", miscText[37], JE_totalScore(&player[0]));
							strcpy(levelWarningText[1], "");
							levelWarningLines = 2;
						}

						for (x = 0; x < temp - 1; x++)
						{
							do
							{
								read_encrypted_pascal_string(s, sizeof(s), ep_f);
							} while (s[0] != '#');
						}

						do
						{
							read_encrypted_pascal_string(s, sizeof(s), ep_f);
							strcpy(levelWarningText[levelWarningLines], s);
							levelWarningLines++;
						} while (s[0] != '#');
						levelWarningLines--;

						JE_wipeKey();
						frameCountMax = 4;
						if (!constantPlay)
							JE_displayText();

						fade_black(15);

						JE_nextEpisode();

						if (jumpBackToEpisode1 && !twoPlayerMode)
						{
							JE_loadPic(VGAScreen, 1, false); // huh?
							JE_clr256(VGAScreen);

							if (superTyrian)
							{
								// if completed Zinglon's Revenge, show SuperTyrian and Destruct codes
								// if completed SuperTyrian, show Nort-Ship Z code
								superArcadeMode = (initialDifficulty == DIFFICULTY_LORD_OF_GAME) ? 8 : 1;
							}

							if (superArcadeMode < SA_ENGAGE)
							{
								if (SANextShip[superArcadeMode] == SA_ENGAGE)
								{
									sprintf(buffer, "%s %s", miscTextB[4], pName[0]);
									JE_dString(VGAScreen, JE_fontCenter(buffer, FONT_SHAPES), 100, buffer, FONT_SHAPES);

									sprintf(buffer, "Or play... %s", specialName[SA_DESTRUCT - 1]);
									JE_dString(VGAScreen, 80, 180, buffer, SMALL_FONT_SHAPES);
								}
								else
								{
									JE_dString(VGAScreen, JE_fontCenter(superShips[0], FONT_SHAPES), 30, superShips[0], FONT_SHAPES);
									JE_dString(VGAScreen, JE_fontCenter(superShips[SANextShip[superArcadeMode]], SMALL_FONT_SHAPES), 100, superShips[SANextShip[superArcadeMode]], SMALL_FONT_SHAPES);
								}

								if (SANextShip[superArcadeMode] < SA_NORTSHIPZ)
									blit_sprite2x2(VGAScreen, 148, 70, spriteSheet9, ships[SAShip[SANextShip[superArcadeMode]-1]].shipgraphic);
								else if (SANextShip[superArcadeMode] == SA_NORTSHIPZ)
									trentWin = true;

								sprintf(buffer, "Type %s at Title", specialName[SANextShip[superArcadeMode]-1]);
								JE_dString(VGAScreen, JE_fontCenter(buffer, SMALL_FONT_SHAPES), 160, buffer, SMALL_FONT_SHAPES);
								JE_showVGA();

								fade_palette(colors, 50, 0, 255);

								if (!constantPlay)
									wait_input(true, true, true);
							}

							jumpSection = true;

							if (isNetworkGame)
								JE_readTextSync();

							if (superTyrian)
							{
								fade_black(10);

								// back to titlescreen
								mainLevel = 0;
								return;
							}
						}
						break;

					case 'P':
						if (!constantPlay)
						{
							JE_word tempX = atoi(s + 3);
							if (tempX > 900)
							{
								memcpy(colors, palettes[pcxpal[tempX-1 - 900]], sizeof(colors));
								JE_clr256(VGAScreen);
								JE_showVGA();
								fade_palette(colors, 1, 0, 255);
							}
							else
							{
								if (tempX == 0)
									JE_loadPCX("tshp2.pcx");
								else
									JE_loadPic(VGAScreen, tempX, false);

								JE_showVGA();
								fade_palette(colors, 10, 0, 255);
							}
						}
						break;

					case 'U':
						if (!constantPlay)
						{
							memcpy(VGAScreen2->pixels, VGAScreen->pixels, VGAScreen2->pitch * VGAScreen2->h);

							JE_word tempX = atoi(s + 3);
							JE_loadPic(VGAScreen, tempX, false);
							copy_screen_to_buffer(pic_buffer);

							service_SDL_events(true);

							animate_picture_wipe(WIPE_U, pic_buffer);

							copy_buffer_to_screen(pic_buffer);
						}
						break;

					case 'V':
						if (!constantPlay)
						{
							/* TODO: NETWORK */
							memcpy(VGAScreen2->pixels, VGAScreen->pixels, VGAScreen2->pitch * VGAScreen2->h);

							JE_word tempX = atoi(s + 3);
							JE_loadPic(VGAScreen, tempX, false);
							copy_screen_to_buffer(pic_buffer);

							service_SDL_events(true);

							animate_picture_wipe(WIPE_V, pic_buffer);

							copy_buffer_to_screen(pic_buffer);
						}
						break;

					case 'R':
						if (!constantPlay)
						{
							/* TODO: NETWORK */
							memcpy(VGAScreen2->pixels, VGAScreen->pixels, VGAScreen2->pitch * VGAScreen2->h);

							JE_word tempX = atoi(s + 3);
							JE_loadPic(VGAScreen, tempX, false);
							copy_screen_to_buffer(pic_buffer);

							service_SDL_events(true);

							animate_picture_wipe(WIPE_R, pic_buffer);

							copy_buffer_to_screen(pic_buffer);
						}
						break;

					case 'C':
						if (!isNetworkGame)
						{
							fade_black(10);
						}
						JE_clr256(VGAScreen);
						JE_showVGA();
						memcpy(colors, palettes[7], sizeof(colors));
						set_palette(colors, 0, 255);
						break;

					case 'B':
						if (!isNetworkGame)
						{
							fade_black(10);
						}
						break;
					case 'F':
						if (!isNetworkGame)
						{
							fade_white(100);
							fade_black(30);
						}
						JE_clr256(VGAScreen);
						JE_showVGA();
						break;

					case 'W':
						if (!constantPlay)
						{
							if (!ESCPressed)
							{
								JE_wipeKey();
								warningCol = 14 * 16 + 5;
								warningColChange = 1;
								warningSoundDelay = 0;
								levelWarningDisplay = (s[2] == 'y');
								levelWarningLines = 0;
								frameCountMax = atoi(s + 4);
								setDelay2(6);
								warningRed = frameCountMax / 10;
								frameCountMax = frameCountMax % 10;

								do
								{
									read_encrypted_pascal_string(s, sizeof(s), ep_f);

									if (s[0] != '#')
									{
										strcpy(levelWarningText[levelWarningLines], s);
										levelWarningLines++;
									}
								} while (!(s[0] == '#'));

								JE_displayText();
								newkey = false;
							}
						}
						break;

					case 'H':
						if (initialDifficulty < DIFFICULTY_HARD)
						{
							mainLevel = atoi(s + 4);
							jumpSection = true;
						}
						break;

					case 'h':
						if (initialDifficulty > DIFFICULTY_NORMAL)
						{
							read_encrypted_pascal_string(s, sizeof(s), ep_f);
						}
						break;

					case 'S':
						if (isNetworkGame)
						{
							JE_readTextSync();
						}
						break;

					case 'n':
						ESCPressed = false;
						break;

					case 'M':
						temp = atoi(s + 3);
						play_song(temp - 1);
						break;

					case 'T':
						if (timedBattleMode)
						{
							// ]T[ 43 44 45 46 47 -- Episode 1
							// ]T[ 03 03 04 05 06 -- Episode 5
							mainLevel = atoi(s + (timeBattleSelection * 3));
							jumpSection = true;
						}
						break;

					case 'q':
						if (timedBattleMode)
						{
							JE_highScoreCheck();
							mainLevel = 0;
							return;
						}
						break;
					}
				}

			} while (!(loadLevelOk || jumpSection));

			fclose(ep_f);

		} while (!loadLevelOk);
	}

	if (play_demo)
		load_next_demo();
	else
		fade_black(50);

	/* Return the display to the normal gameplay offset after the fade */
	set_menu_centered(false);

	// The scan above set lvlFileNum from the section's first ']L', but a caller may need a LATER
	// ']L' in the same section (Episode 1 section 3's second TYRIAN cut) -- reachable via the endless
	// pool or the debug/level-select menu. select_level and the endless commit paths stash that in
	// forcedLvlFileNum; apply it here, then consume it so normal campaign progression is untouched.
	if (forcedLvlFileNum != 0)
	{
		if (JE_levelFileNumValid(forcedLvlFileNum))
			lvlFileNum = forcedLvlFileNum;
		else
			fprintf(stderr, "warning: ignoring missing level file %u in episode %u\n",
			        (unsigned int)forcedLvlFileNum, (unsigned int)episodeNum);
		forcedLvlFileNum = 0;
	}

	if (!JE_levelFileNumValid(lvlFileNum))
	{
		fprintf(stderr, "error: episode %u references missing level file %u (available: 1-%u)\n",
		        (unsigned int)episodeNum, (unsigned int)lvlFileNum, (unsigned int)(lvlNum / 2));
		JE_tyrianHalt(1);
		return;
	}

	FILE* level_f = dir_fopen_die(data_dir(), levelFile, "rb");
	if (fseek(level_f, lvlPos[(lvlFileNum - 1) * 2], SEEK_SET) != 0)
	{
		fprintf(stderr, "error: failed to seek to episode %u level file %u\n",
		        (unsigned int)episodeNum, (unsigned int)lvlFileNum);
		fclose(level_f);
		JE_tyrianHalt(1);
		return;
	}

	JE_char char_mapFile;
	JE_char char_shapeFile;
	fread_die(&char_mapFile,   1, 1, level_f);
	fread_die(&char_shapeFile, 1, 1, level_f);
	fread_u16_die(&mapX,  1, level_f);
	fread_u16_die(&mapX2, 1, level_f);
	fread_u16_die(&mapX3, 1, level_f);

	fread_u16_die(&levelEnemyMax, 1, level_f);
	if (levelEnemyMax > COUNTOF(levelEnemy))
	{
		fprintf(stderr, "error: episode %u level file %u has too many random enemies (%u)\n",
		        (unsigned int)episodeNum, (unsigned int)lvlFileNum, (unsigned int)levelEnemyMax);
		fclose(level_f);
		JE_tyrianHalt(1);
		return;
	}
	fread_u16_die(levelEnemy, levelEnemyMax, level_f);

	fread_u16_die(&maxEvent, 1, level_f);
	if (maxEvent >= COUNTOF(eventRec))
	{
		fprintf(stderr, "error: episode %u level file %u has too many events (%u)\n",
		        (unsigned int)episodeNum, (unsigned int)lvlFileNum, (unsigned int)maxEvent);
		fclose(level_f);
		JE_tyrianHalt(1);
		return;
	}
	for (x = 0; x < maxEvent; x++)
	{
		fread_u16_die(&eventRec[x].eventtime, 1, level_f);
		fread_u8_die( &eventRec[x].eventtype, 1, level_f);
		fread_s16_die(&eventRec[x].eventdat,  1, level_f);
		fread_s16_die(&eventRec[x].eventdat2, 1, level_f);
		fread_s8_die( &eventRec[x].eventdat3, 1, level_f);
		fread_s8_die( &eventRec[x].eventdat5, 1, level_f);
		fread_s8_die( &eventRec[x].eventdat6, 1, level_f);
		fread_u8_die( &eventRec[x].eventdat4, 1, level_f);
	}
	eventRec[x].eventtime = 65500;  /*Not needed but just in case*/

	/*debuginfo('Level loaded.');*/

	/*debuginfo('Loading Map');*/

	/* MAP SHAPE LOOKUP TABLE - Each map is directly after level */
	for (temp = 0; temp < 3; temp++)
	{
		fread_u16_die(mapSh[temp], sizeof(*mapSh) / sizeof(JE_word), level_f);
		for (temp2 = 0; temp2 < 128; temp2++)
		{
			mapSh[temp][temp2] = SDL_Swap16(mapSh[temp][temp2]);
		}
	}

	/* Read Shapes.DAT */
	sprintf(tempStr, "shapes%c.dat", tolower((unsigned char)char_shapeFile));
	FILE *shpFile = dir_fopen_die(data_dir(), tempStr, "rb");

	for (int z = 0; z < 600; z++)
	{
		JE_boolean shapeBlank;
		fread_bool_die(&shapeBlank, shpFile);

		if (shapeBlank)
			memset(shape, 0, sizeof(shape));
		else
			fread_u8_die(shape, sizeof(shape), shpFile);

		/* Match 1 */
		for (int x = 0; x <= 71; ++x)
		{
			if (mapSh[0][x] == z+1)
			{
				memcpy(megaData1.shapes[x].sh, shape, sizeof(JE_DanCShape));

				ref[0][x] = megaData1.shapes[x].sh;
			}
		}

		/* Match 2 */
		for (int x = 0; x <= 71; ++x)
		{
			if (mapSh[1][x] == z+1)
			{
				if (x != 71 && !shapeBlank)
				{
					memcpy(megaData2.shapes[x].sh, shape, sizeof(JE_DanCShape));

					y = 1;
					for (yy = 0; yy < (24 * 28) >> 1; yy++)
						if (shape[yy] == 0)
							y = 0;

					megaData2.shapes[x].fill = y;
					ref[1][x] = megaData2.shapes[x].sh;
				}
				else
				{
					ref[1][x] = NULL;
				}
			}
		}

		/*Match 3*/
		for (int x = 0; x <= 71; ++x)
		{
			if (mapSh[2][x] == z+1)
			{
				if (x < 70 && !shapeBlank)
				{
					memcpy(megaData3.shapes[x].sh, shape, sizeof(JE_DanCShape));

					y = 1;
					for (yy = 0; yy < (24 * 28) >> 1; yy++)
						if (shape[yy] == 0)
							y = 0;

					megaData3.shapes[x].fill = y;
					ref[2][x] = megaData3.shapes[x].sh;
				}
				else
				{
					ref[2][x] = NULL;
				}
			}
		}
	}

	fclose(shpFile);

	fread_u8_die(mapBuf, 14 * 300, level_f);
	bufLoc = 0;              /* MAP NUMBER 1 */
	for (y = 0; y < 300; y++)
	{
		for (x = 0; x < 14; x++)
		{
			megaData1.mainmap[y][x] = ref[0][mapBuf[bufLoc]];
			bufLoc++;
		}
	}

	fread_u8_die(mapBuf, 14 * 600, level_f);
	bufLoc = 0;              /* MAP NUMBER 2 */
	for (y = 0; y < 600; y++)
	{
		for (x = 0; x < 14; x++)
		{
			megaData2.mainmap[y][x] = ref[1][mapBuf[bufLoc]];
			bufLoc++;
		}
	}

	fread_u8_die(mapBuf, 15 * 600, level_f);
	bufLoc = 0;              /* MAP NUMBER 3 */
	for (y = 0; y < 600; y++)
	{
		for (x = 0; x < 15; x++)
		{
			megaData3.mainmap[y][x] = ref[2][mapBuf[bufLoc]];
			bufLoc++;
		}
	}

	fclose(level_f);

	/* Note: The map data is automatically calculated with the correct mapsh
	value and then the pointer is calculated using the formula (MAPSH-1)*168.
	Then, we'll automatically add S2Ofs to get the exact offset location into
	the shape table! This makes it VERY FAST! */

	/*debuginfo('Map file done.');*/
	/* End of find loop for LEVEL??.DAT */
}

#ifdef WITH_NETWORK
void networkStartScreen(void)
{
	JE_loadPic(VGAScreen, 2, false);
	memcpy(VGAScreen2->pixels, VGAScreen->pixels, VGAScreen2->pitch * VGAScreen2->h);
	JE_dString(VGAScreen, JE_fontCenter("Waiting for other player.", SMALL_FONT_SHAPES), 140, "Waiting for other player.", SMALL_FONT_SHAPES);
	JE_showVGA();
	fade_palette(colors, 10, 0, 255);

	network_connect();

	twoPlayerMode = true;
	if (thisPlayerNum == 1)
	{
		fade_black(10);

		if (episodeSelect() && difficultySelect())
		{
			initialDifficulty = difficultyLevel;

			difficultyLevel++;  /*Make it one step harder for 2-player mode!*/

			network_prepare(PACKET_DETAILS);
			SDLNet_Write16(episodeNum, &packet_out_temp->data[4]);
			SDLNet_Write16(difficultyLevel, &packet_out_temp->data[6]);
			network_send(8);  // PACKET_DETAILS
		}
		else
		{
			network_prepare(PACKET_QUIT);
			network_send(4);  // PACKET QUIT

			network_tyrian_halt(0, true);
		}
	}
	else
	{
		memcpy(VGAScreen->pixels, VGAScreen2->pixels, VGAScreen->pitch * VGAScreen->h);
		JE_dString(VGAScreen, JE_fontCenter(networkText[4 - 1], SMALL_FONT_SHAPES), 140, networkText[4 - 1], SMALL_FONT_SHAPES);
		JE_showVGA();

		// until opponent sends details packet
		while (true)
		{
			service_SDL_events(false);
			JE_showVGA();

			if (packet_in[0] && SDLNet_Read16(&packet_in[0]->data[0]) == PACKET_DETAILS)
				break;

			network_update();
			network_check();

			SDL_Delay(16);
		}

		JE_initEpisode(SDLNet_Read16(&packet_in[0]->data[4]));
		difficultyLevel = SDLNet_Read16(&packet_in[0]->data[6]);
		initialDifficulty = difficultyLevel - 1;
		fade_black(10);

		network_update();
	}

	for (uint i = 0; i < COUNTOF(player); ++i)
		player[i].cash = 0;

	player[0].items.ship = 11;  // Silver Ship

	while (!network_is_sync())
	{
		service_SDL_events(false);
		JE_showVGA();

		network_check();
		SDL_Delay(16);
	}
}
#endif /* WITH_NETWORK */

bool titleScreen(void)
{
	enum MenuItemIndex
	{
		MENU_ITEM_NEW_GAME = 0,
		MENU_ITEM_LOAD_GAME,
		MENU_ITEM_HIGH_SCORES,
		MENU_ITEM_INSTRUCTIONS,
		MENU_ITEM_SETUP,
		MENU_ITEM_EXTRA,
		MENU_ITEM_DEMO,
		MENU_ITEM_QUIT,
	};

	if (shopSpriteSheet.data == NULL)
		JE_loadCompShapes(&shopSpriteSheet, '1');  // need mouse pointer sprites

	bool restart = true;

	size_t selectedIndex = MENU_ITEM_NEW_GAME;
	size_t specialNameProgress[SA_ENGAGE] = { 0 };

	// Title menu labels: the 7 data-file entries (menuText[]) with "Extra" inserted
	// after Setup. Keep in sync with enum MenuItemIndex above.
	const char *const titleLabels[] =
	{
		menuText[0],  // Start New Game
		menuText[1],  // Load Game
		menuText[2],  // High Scores
		menuText[3],  // Instructions
		menuText[4],  // Setup
		"Extra",
		menuText[5],  // Demo
		menuText[6],  // Quit
	};

	const int xCenter = LEGACY_WIDTH / 2;
	const int yMenuItems = 98;
	const int hMenuItem = 12;
	int wMenuItem[COUNTOF(titleLabels)] = { 0 };

	for (; ; )
	{
		if (restart)
		{
			play_song(SONG_TITLE);

			JE_loadPic(VGAScreen, 4, false);

			draw_font_hv_shadow(VGAScreen, 2, 192, opentyrian_version, small_font, left_aligned, 15, 0, false, 1);

			if (moveTyrianLogoUp)
			{
				memcpy(VGAScreen2->pixels, VGAScreen->pixels, VGAScreen2->pitch * VGAScreen2->h);

				blit_sprite(VGAScreenSeg, 11, 62, PLANET_SHAPES, 146); // tyrian logo
				blit_sprite(VGAScreenSeg, 155, 41, PLANET_SHAPES, 151); // 2000(tm)
				fade_palette(colors, 10, 0, 255 - 16);

				if (smoothMotion)
				{
					// Slide the logo up by real elapsed time, presented at the display
					// rate (the original stepped 2 px per 35Hz tick). yLogo runs 60->4
					// and y2K runs 45->73 over the slide.
					const Uint32 slideStart = SDL_GetTicks();
					const Uint32 slideMs = 800;  // ~matches the original stepped slide

					for (;;)
					{
						float t = (float)(SDL_GetTicks() - slideStart) / (float)slideMs;
						if (t > 1.0f)
							t = 1.0f;

						const int yLogo = (int)(60.0f - 56.0f * t + 0.5f);
						const int y2K   = (int)(45.0f + 28.0f * t + 0.5f);

						memcpy(VGAScreen->pixels, VGAScreen2->pixels, VGAScreen->pitch * VGAScreen->h);
						blit_sprite(VGAScreenSeg, 11, yLogo, PLANET_SHAPES, 146); // tyrian logo
						blit_sprite(VGAScreenSeg, 155, y2K, PLANET_SHAPES, 151); // 2000(tm)
						JE_showVGA();

						if (t >= 1.0f)
							break;

						service_SDL_events(false);
						if (!output_vsync)
							limit_render_fps();
					}
				}
				else
				{
					for (int yLogo = 60, y2K = 45; yLogo >= 4; yLogo -= 2, ++y2K)
					{
						setDelay(2);

						memcpy(VGAScreen->pixels, VGAScreen2->pixels, VGAScreen->pitch * VGAScreen->h);
						blit_sprite(VGAScreenSeg, 11, yLogo, PLANET_SHAPES, 146); // tyrian logo
						blit_sprite(VGAScreenSeg, 155, y2K, PLANET_SHAPES, 151); // 2000(tm)
						JE_showVGA();

						service_wait_delay();
					}
				}
				moveTyrianLogoUp = false;
			}
			else
			{
				blit_sprite(VGAScreenSeg, 11, 4, PLANET_SHAPES, 146); // tyrian logo
				blit_sprite(VGAScreenSeg, 155, 73, PLANET_SHAPES, 151); // 2000(tm)
				fade_palette(colors, 10, 0, 255 - 16);
			}

			// Draw menu items.
			for (size_t i = 0; i < COUNTOF(titleLabels); ++i)
			{
				const char *const text = titleLabels[i];

				wMenuItem[i] = JE_textWidth(text, normal_font);
				const int x = xCenter - wMenuItem[i] / 2;
				const int y = yMenuItems + hMenuItem * i;

				draw_font_hv(VGAScreen, x - 1, y - 1, text, normal_font, left_aligned, 15, -10);
				draw_font_hv(VGAScreen, x + 1, y + 1, text, normal_font, left_aligned, 15, -10);
				draw_font_hv(VGAScreen, x + 1, y - 1, text, normal_font, left_aligned, 15, -10);
				draw_font_hv(VGAScreen, x - 1, y + 1, text, normal_font, left_aligned, 15, -10);
				draw_font_hv(VGAScreen, x,     y,     text, normal_font, left_aligned, 15, -3);
			}

			memcpy(VGAScreen2->pixels, VGAScreen->pixels, VGAScreen2->pitch * VGAScreen2->h);

			mouseCursor = MOUSE_POINTER_NORMAL;

			// Fade in menu items.
			fade_palette(colors, 20, 255 - 16 + 1, 255);

			restart = false;
		}

		memcpy(VGAScreen->pixels, VGAScreen2->pixels, VGAScreen->pitch * VGAScreen->h);

		// Highlight selected menu item.
		draw_font_hv(VGAScreen, xCenter, yMenuItems + hMenuItem * selectedIndex, titleLabels[selectedIndex], normal_font, centered, 15, -1);

		service_SDL_events(true);

		JE_mouseStartFilter(0xF0);
		JE_showVGA();
		JE_mouseReplace();
		if (!output_vsync)
			limit_render_fps();  // pace the cursor redraw to the render-fps cap

		const Uint32 idleStartTick = SDL_GetTicks();

		// Poll finely instead of sleeping 16 ms so the outer loop (and mouse cursor)
		// redraws at the display's refresh rate; a still cursor yields the CPU.
		const Uint16 startMouseX = mouse_x;
		const Uint16 startMouseY = mouse_y;
		bool mouseMoved = false;
		for (;;)
		{
			// Play demo after idle for 30 seconds.
			if (SDL_GetTicks() - idleStartTick > 30000)
			{
				fade_black(15);

				play_demo = true;
				return true;
			}

			push_joysticks_as_keyboard();
			service_SDL_events(false);

			mouseMoved = mouse_x != startMouseX || mouse_y != startMouseY;
			if (newkey || new_text || newmouse || mouseMoved)
				break;

			SDL_Delay(1);  // brief idle poll; a still cursor doesn't need redrawing
		}

		// Handle interaction.

		bool action = false;
		bool done = false;

		if (mouseMoved || newmouse)
		{
			// Find menu item that was hovered or clicked.
			for (size_t i = 0; i < COUNTOF(titleLabels); ++i)
			{
				const int xMenuItem = xCenter - wMenuItem[i] / 2;
				if (mouse_x >= xMenuItem && mouse_x < xMenuItem + wMenuItem[i])
				{
					const int yMenuItem = yMenuItems + hMenuItem * i;
					if (mouse_y >= yMenuItem && mouse_y < yMenuItem + hMenuItem)
					{
						if (selectedIndex != i)
						{
							JE_playSampleNum(S_CURSOR);

							selectedIndex = i;
						}

						if (newmouse && lastmouse_but == SDL_BUTTON_LEFT &&
						    lastmouse_x >= xMenuItem && lastmouse_x < xMenuItem + wMenuItem[i] &&
						    lastmouse_y >= yMenuItem && lastmouse_y < yMenuItem + hMenuItem)
						{
							action = true;
						}

						break;
					}
				}
			}
		}

		if (newmouse)
		{
			if (lastmouse_but == SDL_BUTTON_RIGHT)
			{
				JE_playSampleNum(S_SPRING);

				done = true;
			}
		}
		else if (newkey)
		{
			switch (lastkey_scan)
			{
			case SDL_SCANCODE_UP:
			{
				JE_playSampleNum(S_CURSOR);

				selectedIndex = selectedIndex == 0
					? COUNTOF(titleLabels) - 1
					: selectedIndex - 1;
				break;
			}
			case SDL_SCANCODE_DOWN:
			{
				JE_playSampleNum(S_CURSOR);

				selectedIndex = selectedIndex == COUNTOF(titleLabels) - 1
					? 0
					: selectedIndex + 1;
				break;
			}
			case SDL_SCANCODE_SPACE:
			case SDL_SCANCODE_RETURN:
			{
				action = true;
				break;
			}
			case SDL_SCANCODE_ESCAPE:
			{
				JE_playSampleNum(S_SPRING);

				done = true;
			}
			default:
				break;
			}
		}

		if (new_text)
		{
			for (size_t ti = 0U; last_text[ti] != '\0'; ++ti)
			{
				const char c = toupper(last_text[ti]);

				for (size_t i = 0; i < SA_ENGAGE; i++)
				{
					if (specialNameProgress[i] >= COUNTOF(specialName[i]) - 1 ||
						c != specialName[i][specialNameProgress[i]])
					{
						specialNameProgress[i] = 0;
						continue;
					}

					specialNameProgress[i]++;

					if (specialName[i][specialNameProgress[i]] == '\0')
					{
						if (i + 1 == SA_DESTRUCT)
						{
							fade_black(10);

							loadDestruct = true;
							return true;
						}
						else if (i + 1 == SA_ENGAGE)
						{
							JE_playSampleNum(V_DANGER);

							JE_whoa();
							set_colors((SDL_Color) { 0, 0, 0 }, 0, 255);

							if (newSuperTyrianGame())
								return true;

							restart = true;
						}
						else
						{
							fade_black(10);

							if (newSuperArcadeGame(i))
								return true;

							restart = true;
						}
					}
				}
			}
		}

		if (action)
		{
			JE_playSampleNum(S_SELECT);

			switch (selectedIndex)
			{
			case MENU_ITEM_NEW_GAME:
			{
				fade_black(15);

				if (newGame())
					return true;

				restart = true;
				break;
			}
			case MENU_ITEM_LOAD_GAME:
			{
				fade_black(15);

				if (JE_loadScreen())
					return true;

				restart = true;
				break;
			}
			case MENU_ITEM_HIGH_SCORES:
			{
				fade_black(15);

				JE_highScoreScreen();

				restart = true;
				break;
			}
			case MENU_ITEM_INSTRUCTIONS:
			{
				fade_black(15);

				JE_helpSystem(1);

				restart = true;
				break;
			}
			case MENU_ITEM_SETUP:
			{
				fade_black(15);

				setupMenu();

				restart = true;
				break;
			}
			case MENU_ITEM_EXTRA:
			{
				fade_black(15);

				if (extraMenu())  // launched a game (SuperTyrian / Super Arcade)?
					return true;

				restart = true;
				break;
			}
			case MENU_ITEM_DEMO:
			{
				fade_black(15);

				play_demo = true;
				return true;
			}
			case MENU_ITEM_QUIT:
			{
				fade_black(15);

				return false;
			}
			default:
				break;
			}
		}

		if (done)
		{
			fade_black(15);

			return false;
		}
	}
}

bool newGame(void)
{
	if (gameplaySelect())
	{
		// Endless was picked in the mode menu: newEndlessGame does its own difficulty select
		// and full setup (episode 1, starting cash, flags) and resets endlessMode on cancel.
		if (endlessMode)
			return newEndlessGame();

		if (timedBattleMode)
		{
			onePlayerAction = true;
			if (timedBattleSelect() && difficultySelect())
				gameLoaded = true;
		}
		else if (episodeSelect() && difficultySelect())
			gameLoaded = true;

		initialDifficulty = difficultyLevel;

		if (onePlayerAction)
		{
			player[0].cash = 0;

			player[0].items.ship = 8;  // Stalker
		}
		else if (twoPlayerMode)
		{
			for (uint i = 0; i < COUNTOF(player); ++i)
				player[i].cash = 0;

			player[0].items.ship = 11;  // Silver Ship

			difficultyLevel++;

			inputDevice[0] = 1;
			inputDevice[1] = 2;
		}
		else if (richMode)
		{
			player[0].cash = 1000000;
		}
		else if (gameLoaded)
		{
			// allows player to smuggle arcade/super-arcade ships into full game

			const ulong initial_cash[] = { 10000, 15000, 20000, 30000, 20000 };

			assert(episodeNum >= 1 && episodeNum <= EPISODE_AVAILABLE);
			player[0].cash = initial_cash[episodeNum - 1];
		}
	}

	return gameLoaded;
}

bool newSuperArcadeGame(unsigned int i)
{
	player[0].items.ship = SAShip[i];

	if (episodeSelect() && difficultySelect())
	{
		/* Start special mode! */
		JE_loadPic(VGAScreen, 1, false);
		JE_clr256(VGAScreen);
		JE_dString(VGAScreen, JE_fontCenter(superShips[0], FONT_SHAPES), 30, superShips[0], FONT_SHAPES);
		JE_dString(VGAScreen, JE_fontCenter(superShips[i + 1], SMALL_FONT_SHAPES), 100, superShips[i + 1], SMALL_FONT_SHAPES);
		tempW = ships[player[0].items.ship].shipgraphic;
		if (tempW > 500)
			blit_sprite2x2(VGAScreen, 148, 70, spriteSheetT2000, tempW - 500);
		else if (tempW == 1)
		{
			// Nort Ship: shipgraphic 1 is a sentinel (see JE_playerMovement / JE_drawItem), so draw
			// its two-piece hull here instead of skipping it (previously left blank).
			blit_sprite2x2(VGAScreen, 148, 70, spriteSheet9, 220);
			blit_sprite2x2(VGAScreen, 172, 70, spriteSheet9, 222);
		}
		else
			blit_sprite2x2(VGAScreen, 148, 70, spriteSheet9, tempW);

		JE_showVGA();
		fade_palette(colors, 50, 0, 255);

		wait_input(true, true, true);

		twoPlayerMode = false;
		onePlayerAction = true;
		superArcadeMode = i + 1;
		timedBattleMode = false;
		gameLoaded = true;
		initialDifficulty = ++difficultyLevel;

		player[0].cash = 0;

		player[0].items.weapon[FRONT_WEAPON].id = SAWeapon[i][0];
		player[0].items.special = SASpecialWeapon[i];
		if (superArcadeMode == SA_NORTSHIPZ)
		{
			for (uint i = 0; i < COUNTOF(player[0].items.sidekick); ++i)
				player[0].items.sidekick[i] = 24;  // Companion Ship Quicksilver
		}

		fade_black(10);
	}

	return gameLoaded;
}

bool newSuperTyrianGame(void)
{
	/* SuperTyrian */

	initialDifficulty = keysactive[SDL_SCANCODE_SCROLLLOCK] ? DIFFICULTY_SUICIDE : DIFFICULTY_LORD_OF_GAME;

	JE_clr256(VGAScreen);
	JE_outText(VGAScreen, 10, 10, superTyrianText[0], 15, 4);
	if (initialDifficulty == DIFFICULTY_LORD_OF_GAME)
		JE_outText(VGAScreen, 10, 20, superTyrianText[1], 15, 4);
	else
		JE_outText(VGAScreen, 10, 20, superTyrianText[2], 15, 4);
	JE_outText(VGAScreen, 10, 30, superTyrianText[3], 15, 4);
	if (initialDifficulty == DIFFICULTY_LORD_OF_GAME)
		JE_outText(VGAScreen, 10, 40, superTyrianText[4], 15, 4);
	JE_outText(VGAScreen, 10, 60, superTyrianText[5], 15, 4);

	char buf[10 + 1 + 15 + 1];
	snprintf(buf, sizeof(buf), "%s %s", miscTextB[4], pName[0]);
	JE_dString(VGAScreen, JE_fontCenter(buf, FONT_SHAPES), 110, buf, FONT_SHAPES);

	play_song(16);
	JE_playSampleNum(V_GOOD_LUCK);

	JE_showVGA();
	fade_palette(colors, 10, 0, 255);

	wait_noinput(true, true, true);
	wait_input(true, true, true);

	fade_black(1);
	if (episodeSelect()) // T2000 let you choose the starting episode
	{
		constantDie = false;
		superTyrian = true;
		onePlayerAction = true;
		timedBattleMode = false;
		gameLoaded = true;
		difficultyLevel = initialDifficulty;

		player[0].cash = 0;

		player[0].items.ship = 13;                     // The Stalker 21.126
		player[0].items.weapon[FRONT_WEAPON].id = 39;  // Atomic RailGun

		fade_black(10);
		return true;
	}
	else
	{
		play_song(SONG_TITLE);
		return false;
	}

}

bool newEndlessGame(void)
{
	/* Endless roguelite mode. */

	// Absorb the menu-selection keypress/click so it doesn't fall straight through
	// into the difficulty picker (which would auto-select the default and skip it).
	wait_noinput(true, true, true);

	// Endless always starts at episode 1; the run traverses episodes as it deepens.
	JE_initEpisode(1);
	initial_episode_num = episodeNum;

	// Choose the run seed (random or typed) and the Hardcore toggle before the difficulty picker.
	// Cancelling here backs all the way out to the title, exactly like cancelling difficulty.
	char seedbuf[ENDLESS_SEED_MAXLEN];
	bool hardcore = false;
	if (!endlessSeedSelect(seedbuf, sizeof(seedbuf), &hardcore))
	{
		endlessMode = false;
		play_song(SONG_TITLE);
		return false;
	}

	if (!difficultySelect())
	{
		endlessMode = false;  // cancelled: don't leave the mode flag set for the next new game
		play_song(SONG_TITLE);
		return false;
	}
	initialDifficulty = difficultyLevel;

	endlessResetRun();
	endlessSetSeed(seedbuf);  // establish the run's seeded structural RNG (endlessResetRun blanked it)
	endlessHardcore = hardcore;  // apply the seed screen's Hardcore choice (endlessResetRun cleared it)

	endlessMode = true;
	onePlayerAction = false;  // full game: cash economy + between-level shops, NOT arcade orb drops
	timedBattleMode = false;
	twoPlayerMode = false;
	superTyrian = false;
	superArcadeMode = SA_NONE;
	gameLoaded = true;
	difficultyLevel = initialDifficulty;

	player[0].cash = endlessStartingCash();  // difficulty-based starting cash for the first shop

	fade_black(10);
	return true;
}

void intro_logos(void)
{
	moveTyrianLogoUp = true;

	SDL_FillRect(VGAScreen, NULL, 0);

	fade_white(25);

	JE_loadPic(VGAScreen, 10, false);
	JE_showVGA();

	fade_palette(colors, 25, 0, 255);

	setDelay(200);
	wait_delayorinput();

	fade_black(10);

	JE_loadPic(VGAScreen, 12, false);
	JE_showVGA();

	fade_palette(colors, 10, 0, 255);

	setDelay(200);
	wait_delayorinput();

	fade_black(10);
}

void JE_readTextSync(void)
{
#if 0  // this function seems to be unnecessary
	JE_clr256(VGAScreen);
	JE_showVGA();
	JE_loadPic(VGAScreen, 1, true);

	JE_barShade(VGAScreen, 3, 3, 316, 196);
	JE_barShade(VGAScreen, 1, 1, 318, 198);
	JE_dString(VGAScreen, 10, 160, "Waiting for other player.", SMALL_FONT_SHAPES);
	JE_showVGA();

	/* TODO: NETWORK */

	do
	{
		setjasondelay(2);

		/* TODO: NETWORK */

		wait_delay();

	} while (0 /* TODO: NETWORK */);
#endif
}

void JE_displayText(void)
{
	/* Display Warning Text */
	JE_word tempY = 55;
	if (warningRed)
	{
		tempY = 2;
	}
	for (temp = 0; temp < levelWarningLines; temp++)
	{
		if (!ESCPressed)
		{
			JE_outCharGlow(10, tempY, levelWarningText[temp]);

			if (haltGame)
			{
				JE_tyrianHalt(5);
			}

			tempY += 10;
		}
	}
	if (frameCountMax != 0)
	{
		frameCountMax = 6;
		temp = 1;
	}
	else
	{
		temp = 0;
	}
	textGlowFont = TINY_FONT;
	tempW = 184;
	if (warningRed)
		tempW = 7 * 16 + 6;

	JE_outCharGlow(JE_fontCenter(miscText[4], TINY_FONT), tempW, miscText[4]);

	do
	{
		if (levelWarningDisplay)
			JE_updateWarning(VGAScreen);

		setDelay(1);

		NETWORK_KEEP_ALIVE();

		wait_delay();

	} while (!(JE_anyButton() || (frameCountMax == 0 && temp == 1) || ESCPressed));
	levelWarningDisplay = false;
}

Sint16 JE_newEnemy(int enemyOffset, Uint16 eDatI, Sint16 uniqueShapeTableI)
{
	for (int i = enemyOffset; i < enemyOffset + 25; ++i)
	{
		if (enemyAvail[i] == 1)
		{
			enemyAvail[i] = JE_makeEnemy(&enemy[i], eDatI, uniqueShapeTableI);
			return i + 1;
		}
	}
	
	return 0;
}

uint JE_makeEnemy(struct JE_SingleEnemyType *enemy, Uint16 eDatI, Sint16 uniqueShapeTableI)
{
	uint avail;

	JE_byte shapeTableI;

	if (superArcadeMode != SA_NONE && eDatI == 534)
		eDatI = 533;

	if (uniqueShapeTableI > 0)
	{
		shapeTableI = uniqueShapeTableI;
	}
	else
	{
		shapeTableI = enemyDat[eDatI].shapebank;
	}

	Sprite2_array *sprite2s = NULL;
	if (shapeTableI == 21)
	{
		sprite2s = &spriteSheet11;  // Coins&Gems
	}
	else if (shapeTableI == 26)
	{
		sprite2s = &spriteSheet10;  // Two-Player Stuff
	}
	else
	{
		for (size_t i = 0; i < COUNTOF(enemySpriteSheetIds); ++i)
			if (shapeTableI == enemySpriteSheetIds[i])
				sprite2s = &enemySpriteSheets[i];
	}
	
	if (sprite2s != NULL)
		enemy->sprite2s = sprite2s;
	else
		// Use shape table value from previous enemy that occupied the enemy slot. (Ex. APPROACH.)
		fprintf(stderr, "warning: ignoring sprite from unloaded shape table %d\n", shapeTableI);

	enemy->enemydatofs = &enemyDat[eDatI];

	enemy->mapoffset = 0;
	enemy->mapoffset_frac = 0.0f;
	enemy->scroll_ybase = 0;
	enemy->scroll_yfrac = 0.0f;
	enemy->scroll_ylayer = 0;

	for (uint i = 0; i < 3; ++i)
	{
		enemy->eshotmultipos[i] = 0;
	}

	enemy->enemyground = (enemyDat[eDatI].explosiontype & 1) == 0;
	enemy->explonum = enemyDat[eDatI].explosiontype >> 1;

	enemy->launchfreq = enemyDat[eDatI].elaunchfreq;
	enemy->launchwait = enemyDat[eDatI].elaunchfreq;

	// T2000 ... Account for the second enemy bank only if we're creating something from it
	if (eDatI > 1000)
	{
		enemy->launchtype = enemyDat[eDatI].elaunchtype;
		enemy->launchspecial = 0;
	}
	else
	{
		enemy->launchtype = enemyDat[eDatI].elaunchtype % 1000;
		enemy->launchspecial = enemyDat[eDatI].elaunchtype / 1000;
	}

	enemy->xaccel = enemyDat[eDatI].xaccel;
	enemy->yaccel = enemyDat[eDatI].yaccel;

	// RAMPAGE / KAMIKAZE / HOMING (endless): force a minimum tracking accel so enemies home in on you.
	// The accel maps to tracking strength as (accel - 89). Three tiers, hardest first:
	//   RAMPAGE  96 -> strength 7, and ALSO rams for extra collision damage (see mainint.c) -- the
	//            original brutal Kamikaze, now a super-rare gamble-only mod.
	//   KAMIKAZE 92 -> strength 3, no ram -- the moderate sector tier (what HOMING used to be).
	//   HOMING   90 -> strength 1, no ram -- the gentlest sector tier (barely leans toward you).
	// Only ever RAISE a weak enemy to the floor; an enemy that already tracks harder keeps its accel.
	if (endlessMode)
	{
		const int trackFloor = (endlessActiveMods & ENDLESS_MOD_RAMPAGE)  ? 96
		                     : (endlessActiveMods & ENDLESS_MOD_KAMIKAZE) ? 92
		                     : (endlessActiveMods & ENDLESS_MOD_HOMING)   ? 90
		                     : 0;
		if (trackFloor)
		{
			if (enemy->xaccel < trackFloor) enemy->xaccel = trackFloor;
			if (enemy->yaccel < trackFloor) enemy->yaccel = trackFloor;
		}
	}

	enemy->xminbounce = -10000;
	enemy->xmaxbounce = 10000;
	enemy->yminbounce = -10000;
	enemy->ymaxbounce = 10000;
	/*Far enough away to be impossible to reach*/

	for (uint i = 0; i < 3; ++i)
	{
		enemy->tur[i] = enemyDat[eDatI].tur[i];
	}

	enemy->ani = enemyDat[eDatI].ani;
	enemy->animin = 1;

	switch (enemyDat[eDatI].animate)
	{
	case 0:
		enemy->enemycycle = 1;
		enemy->aniactive = 0;
		enemy->animax = 0;
		enemy->aniwhenfire = 0;
		break;
	case 1:
		enemy->enemycycle = 0;
		enemy->aniactive = 1;
		enemy->animax = 0;
		enemy->aniwhenfire = 0;
		break;
	case 2:
		enemy->enemycycle = 1;
		enemy->aniactive = 2;
		enemy->animax = enemy->ani;
		enemy->aniwhenfire = 2;
		break;
	}

	if (enemyDat[eDatI].startxc != 0)
		enemy->ex = enemyDat[eDatI].startx + (mt_rand() % (enemyDat[eDatI].startxc * 2)) - enemyDat[eDatI].startxc + 1;
	else
		enemy->ex = enemyDat[eDatI].startx + 1;

	if (enemyDat[eDatI].startyc != 0)
		enemy->ey = enemyDat[eDatI].starty + (mt_rand() % (enemyDat[eDatI].startyc * 2)) - enemyDat[eDatI].startyc + 1;
	else
		enemy->ey = enemyDat[eDatI].starty + 1;

	enemy->exc = enemyDat[eDatI].xmove;
	enemy->eyc = enemyDat[eDatI].ymove;
	enemy->excc = enemyDat[eDatI].xcaccel;
	enemy->eycc = enemyDat[eDatI].ycaccel;
	enemy->exccw = abs(enemy->excc);
	enemy->exccwmax = enemy->exccw;
	enemy->eyccw = abs(enemy->eycc);
	enemy->eyccwmax = enemy->eyccw;
	enemy->exccadd = (enemy->excc > 0) ? 1 : -1;
	enemy->eyccadd = (enemy->eycc > 0) ? 1 : -1;
	enemy->special = false;
	enemy->iced = 0;

	if (enemyDat[eDatI].xrev == 0)
		enemy->exrev = 100;
	else if (enemyDat[eDatI].xrev == -99)
		enemy->exrev = 0;
	else
		enemy->exrev = enemyDat[eDatI].xrev;

	if (enemyDat[eDatI].yrev == 0)
		enemy->eyrev = 100;
	else if (enemyDat[eDatI].yrev == -99)
		enemy->eyrev = 0;
	else
		enemy->eyrev = enemyDat[eDatI].yrev;

	enemy->exca = (enemy->xaccel > 0) ? 1 : -1;
	enemy->eyca = (enemy->yaccel > 0) ? 1 : -1;

	enemy->enemytype = eDatI;

	for (uint i = 0; i < 3; ++i)
	{
		if (enemy->tur[i] == 252)
			enemy->eshotwait[i] = 1;
		else if (enemy->tur[i] > 0)
			enemy->eshotwait[i] = 20;
		else
			enemy->eshotwait[i] = 255;
	}
	for (uint i = 0; i < 20; ++i)
		enemy->egr[i] = enemyDat[eDatI].egraphic[i];
	enemy->size = enemyDat[eDatI].esize;
	enemy->linknum = 0;
	enemy->edamaged = enemyDat[eDatI].dani < 0;
	enemy->enemydie = enemyDat[eDatI].eenemydie;

	enemy->freq[1-1] = enemyDat[eDatI].freq[1-1];
	enemy->freq[2-1] = enemyDat[eDatI].freq[2-1];
	enemy->freq[3-1] = enemyDat[eDatI].freq[3-1];

	enemy->edani   = enemyDat[eDatI].dani;
	enemy->edgr    = enemyDat[eDatI].dgr;
	enemy->edlevel = enemyDat[eDatI].dlevel;

	enemy->fixedmovey = 0;
	enemy->fixedmovey_carry = 0;
	enemy->fixedmovey_carry_base = 0;
	enemy->fixedmovey_carry_move = 0;

	enemy->filter = 0x00;

	int tempValue = 0;
	if (enemyDat[eDatI].value > 1 && enemyDat[eDatI].value < 10000)
	{
		switch (difficultyLevel)
		{
		case -1:
		case DIFFICULTY_WIMP:
			tempValue = enemyDat[eDatI].value * 0.75f;
			break;
		case DIFFICULTY_EASY:
		case DIFFICULTY_NORMAL:
			tempValue = enemyDat[eDatI].value;
			break;
		case DIFFICULTY_HARD:
			tempValue = enemyDat[eDatI].value * 1.125f;
			break;
		case DIFFICULTY_IMPOSSIBLE:
			tempValue = enemyDat[eDatI].value * 1.5f;
			break;
		case DIFFICULTY_INSANITY:
			tempValue = enemyDat[eDatI].value * 2;
			break;
		case DIFFICULTY_SUICIDE:
			tempValue = enemyDat[eDatI].value * 2.5f;
			break;
		case DIFFICULTY_MANIACAL:
		case DIFFICULTY_LORD_OF_GAME:
			tempValue = enemyDat[eDatI].value * 4;
			break;
		case DIFFICULTY_NORTANEOUS:
		case DIFFICULTY_10:
			tempValue = enemyDat[eDatI].value * 8;
			break;
		}
		if (expertMode)  // expert-mode cash bonus to offset the harsher economy
			tempValue = tempValue * expertScorePct / 100;
		if (tempValue > 10000)
			tempValue = 10000;
		enemy->evalue = tempValue;
	}
	else
	{
		enemy->evalue = enemyDat[eDatI].value;
	}

	int tempArmor = 1;
	if (enemyDat[eDatI].armor > 0)
	{
		if (enemyDat[eDatI].armor != 255)
		{
			switch (difficultyLevel)
			{
			case -1:
			case DIFFICULTY_WIMP:
				tempArmor = enemyDat[eDatI].armor * 0.5f + 1;
				break;
			case DIFFICULTY_EASY:
				tempArmor = enemyDat[eDatI].armor * 0.75f + 1;
				break;
			case DIFFICULTY_NORMAL:
				tempArmor = enemyDat[eDatI].armor;
				break;
			case DIFFICULTY_HARD:
				tempArmor = enemyDat[eDatI].armor * 1.2f;
				break;
			case DIFFICULTY_IMPOSSIBLE:
				tempArmor = enemyDat[eDatI].armor * 1.5f;
				break;
			case DIFFICULTY_INSANITY:
				tempArmor = enemyDat[eDatI].armor * 1.8f;
				break;
			case DIFFICULTY_SUICIDE:
				tempArmor = enemyDat[eDatI].armor * 2;
				break;
			case DIFFICULTY_MANIACAL:
				tempArmor = enemyDat[eDatI].armor * 3;
				break;
			case DIFFICULTY_LORD_OF_GAME:
				tempArmor = enemyDat[eDatI].armor * 4;
				break;
			case DIFFICULTY_NORTANEOUS:
			case DIFFICULTY_10:
				tempArmor = enemyDat[eDatI].armor * 8;
				break;
			}

			if (endlessMode)
				tempArmor = tempArmor * endlessArmorPercent() / 100;

			// Expert mode toughens every enemy; bosses sit near the 254 cap already
			// and get their extra HP from expertBossHpMult instead.
			if (expertMode)
				tempArmor = tempArmor * expertEnemyArmorPct / 100;

			if (tempArmor > 254)
			{
				tempArmor = 254;
			}
		}
		else
		{
			tempArmor = 255;
		}

		enemy->armorleft = tempArmor;

		avail = 0;
		enemy->scoreitem = false;
	}
	else
	{
		avail = 2;
		enemy->armorleft = 255;
		if (enemy->evalue != 0)
			enemy->scoreitem = true;
	}

	enemy->damageAccum = 0;  // reset expert-mode boss-HP accumulator on (re)spawn
	enemy->healthbar_seen = false;  // no enemy HP bar until this slot takes damage
	enemy->healthbar_max = 0;
	enemy->eliteState = 0;  // endless: elite undecided until first processed (see JE_drawEnemy)

	if (!enemy->scoreitem)
	{
		totalEnemy++;  /*Destruction ratio*/
	}

	/* indicates what to set ENEMYAVAIL to */
	return avail;
}

// Signed round-to-nearest division. Spawn catch-up values are small, but keeping the negative
// path symmetric matters for fixedmovey/eyc combinations whose net motion is upward.
static int event_scroll_round_div(int numerator, int denominator)
{
	if (denominator <= 0)
		return 0;
	return numerator >= 0
	       ? (numerator + denominator / 2) / denominator
	       : -((-numerator + denominator / 2) / denominator);
}

// Advance a freshly-created layer-bound enemy through the part of the previous boosted tick that
// lay after its event coordinate. This is deliberately derived from the previous tick's ACTUAL
// layer delta, not merely from the modifier percentage: the same code therefore covers fractional
// carry pulses, stronger modifiers, and layer 3's independent rate. fixedmovey is folded into the
// missed full-tick motion before it is prorated, so fixed=-baseStep scenery does not get shifted.
static int event_enemy_scroll_catchup(JE_word enemyOffset, const struct JE_SingleEnemyType *e)
{
	int layer = 0;
	if (enemyOffset == 25 || enemyOffset == 75)
		layer = 1;
	else if (enemyOffset == 50)
		layer = 3;
	else if (enemyOffset == 0 && backMove2 > 0 && e->yaccel == 0 &&
	         (int)e->fixedmovey + (e->eycc != 0 ? 0 : (int)e->eyc) == (int)backMove2)
		layer = 2;  // attached sky scenery rides layer 2 through eyc and/or fixedmovey (see JE_drawEnemy)
	else
		return 0;  // free-flying sky enemies are not vertically layer-bound

	const int eventTime = (int)eventRec[eventLoc - 1].eventtime;
	const int span = eventScrollTo - eventScrollFrom;
	if (!eventScrollCatchupValid || (int)curLoc != eventScrollTo || span <= 0 ||
	    eventTime <= eventScrollFrom || eventTime > eventScrollTo)
		return 0;  // exact-time event, level/event jump, or a non-scroll forceEvents interval

	const int late = eventScrollTo - eventTime;

	if (layer == 2)
	{
		// The glass and the event clock quantize their boosted fractional rates through
		// independent carries, so the integer identity "glass == ratio x curLoc" that stock
		// keeps exact wanders +/-1px with the relative carry phase. Pieces of one structure
		// spawn on different ticks and would inherit different phases -- a permanent 1px seam
		// inside the structure (GYGES's chain machine). Anchor every spawn to the same ideal
		// line instead: late whole event-px at the stock layer ratio plus the current
		// cross-layer phase. Applies even at late == 0 (the phase can be nonzero on an
		// on-time tick). Local motion beyond the ride is prorated like the other banks;
		// sky fixedmovey itself never scales (local-motion semantics).
		if (!eventScrollSkyValid || late < 0)
			return 0;
		int catchup = event_scroll_round_div(eventScrollSkyRatio100 * late +
		                                     eventScrollSkyPhase100, 100);
		const int surplus = ((int)e->fixedmovey + (e->eycc != 0 ? 0 : (int)e->eyc)) -
		                    (int)backMove2;
		if (surplus != 0)
			catchup += event_scroll_round_div(surplus * late, span);
		return catchup;
	}

	if (late <= 0)
		return 0;

	const int fixedMoveRaw = e->fixedmovey;
	const int scalable = enemy_scalable_fixed_y(fixedMoveRaw, e->eyc);
	int fixedMoveScaled = scalable;
	const int baseStep = eventScrollBaseStep[layer];
	if (scalable != 0 && eventScrollBoost > 0 && baseStep > 0)
	{
		if (eventScrollDelayMax[layer] == 1)
			fixedMoveScaled = scalable * eventScrollLayerDelta[layer] / baseStep;
		else
			fixedMoveScaled = scalable * (100 + eventScrollBoost) / 100;
	}
	const int fixedMove = (fixedMoveRaw - scalable) + fixedMoveScaled;

	// An existing enemy would have received these three terms during the full previous tick.
	// Apply only the fraction after this event's coordinate; for ordinary fixed-0 scenery on
	// layer 1 this reduces exactly to `late`, with no rounding at all.
	const int fullMove = eventScrollLayerDelta[layer] + fixedMove + e->eyc;
	return event_scroll_round_div(fullMove * late, span);
}

void JE_createNewEventEnemy(JE_byte enemyTypeOfs, JE_word enemyOffset, Sint16 uniqueShapeTableI)
{
	int i;

	b = 0;

	for (i = enemyOffset; i < enemyOffset + 25; i++)
	{
		if (enemyAvail[i] == 1)
		{
			b = i + 1;
			break;
		}
	}

	if (b == 0)
		return;

	tempW = eventRec[eventLoc-1].eventdat + enemyTypeOfs;

	enemyAvail[b-1] = JE_makeEnemy(&enemy[b-1], tempW, uniqueShapeTableI);

	// When T2000 gives an X position of -200, what it actually wants is a random X position...
	if (eventRec[eventLoc-1].eventdat2 == -200)
	{
		// Ranged 24 - 231
		eventRec[eventLoc-1].eventdat2 = (mt_rand() % 208) + 24;
	}

	if (eventRec[eventLoc-1].eventdat2 != -99)
	{
		switch (enemyOffset)
		{
		case 0:
			enemy[b - 1].ex = eventRec[eventLoc - 1].eventdat2 - (mapX - 3) * 24;
			enemy[b - 1].ey -= backMove2;
			break;
		case 25:
		case 75:
			enemy[b - 1].ex = eventRec[eventLoc - 1].eventdat2 - (mapX - 3) * 24 - 12;
			enemy[b - 1].ey -= backMove;
			break;
		case 50:
			if (background3x1)
				enemy[b - 1].ex = eventRec[eventLoc - 1].eventdat2 - (mapX - 3) * 24 - 12;
			else
				enemy[b - 1].ex = eventRec[eventLoc - 1].eventdat2 - mapX3 * 24 + 6;
			enemy[b - 1].ey -= backMove3;

			if (background3x1b)
				enemy[b-1].ex -= 6;
			break;
		}
		enemy[b-1].ey = -28;
		if (background3x1b && enemyOffset == 50)
			enemy[b-1].ey += 4;
	}

	if (smallEnemyAdjust && enemy[b-1].size == 0)
	{
		enemy[b-1].ex -= 10;
		enemy[b-1].ey -= 7;
	}

	enemy[b - 1].ey += eventRec[eventLoc - 1].eventdat5;
	enemy[b - 1].eyc += eventRec[eventLoc - 1].eventdat3;
	enemy[b - 1].linknum = eventRec[eventLoc - 1].eventdat4;
	enemy[b - 1].fixedmovey = eventRec[eventLoc - 1].eventdat6;
	enemy[b - 1].fixedmovey_carry = 0;
	enemy[b - 1].fixedmovey_carry_base = 0;
	enemy[b - 1].fixedmovey_carry_move = 0;
	enemy[b - 1].ey += event_enemy_scroll_catchup(enemyOffset, &enemy[b - 1]);
}

void JE_eventJump(JE_word jump)
{
	JE_word tempW;
	JE_word target;

	if (jump == 65535)
	{
		curLoc = returnLoc;
		target = returnLoc;
	}
	else
	{
		returnLoc = curLoc + 1;
		// Endless scroll boost: the tick that reached this jump advanced the clock and every
		// scrolling layer in lockstep by base + extra px, overrunning the jump record by up to
		// endlessScrollExtraPx1 more than the stock stride level authors laid their maps out
		// around. That overrun terrain is already consumed and cannot be rewound, so re-base
		// the clock the same distance PAST the target to keep post-jump event times matched to
		// the map (ICESECRET's ramp-in jump otherwise lands every clock-timed building ~a tile
		// off its foundation under Warp Speed). Zero extra px -> stock byte-identical.
		int excess = (int)curLoc - (int)eventRec[eventLoc - 1].eventtime;
		if (excess > endlessScrollExtraPx1)
			excess = endlessScrollExtraPx1;
		if (excess < 0)
			excess = 0;
		curLoc = (JE_word)(jump + excess);
		target = jump;
	}
	// Rescan against the author's target, not the re-based clock: records inside the excess
	// window must still fire (this pass), merely late, like any other boost-overrun record.
	tempW = 0;
	do
	{
		tempW++;
	} while (!(eventRec[tempW-1].eventtime >= target));
	eventLoc = tempW - 1;
}

bool JE_searchFor/*enemy*/(JE_byte PLType, JE_byte* out_index)
{
	int found_id = -1;

	for (int i = 0; i < 100; i++)
	{
		if (enemyAvail[i] == 0 && enemy[i].linknum == PLType)
		{
			found_id = i;
			if (galagaMode)
				enemy[i].evalue += enemy[i].evalue;
		}
	}

	if (found_id != -1)
	{
		if (out_index)
			*out_index = found_id;
		return true;
	}
	else
	{
		return false;
	}
}

void JE_eventSystem(void)
{
	switch (eventRec[eventLoc-1].eventtype)
	{
	case 1:
		starfield_speed = eventRec[eventLoc-1].eventdat;
		break;

	case 2:
		map1YDelay = 1;
		map1YDelayMax = 1;
		map2YDelay = 1;
		map2YDelayMax = 1;

		backMove = eventRec[eventLoc-1].eventdat;
		backMove2 = eventRec[eventLoc-1].eventdat2;

		if (backMove2 > 0)
			explodeMove = backMove2;
		else
			explodeMove = backMove;

		backMove3 = eventRec[eventLoc-1].eventdat3;

		if (backMove > 0)
			stopBackgroundNum = 0;
		break;

	case 3:
		backMove = 1;
		map1YDelay = 3;
		map1YDelayMax = 3;
		backMove2 = 1;
		map2YDelay = 2;
		map2YDelayMax = 2;
		backMove3 = 1;
		break;

	case 4: // Map stop
	case 83: // T2000: Also a map stop 
		stopBackgrounds = true;
		switch (eventRec[eventLoc-1].eventdat)
		{
		case 0:
		case 1:
			stopBackgroundNum = 1;
			break;
		case 2:
			stopBackgroundNum = 2;
			break;
		case 3:
			stopBackgroundNum = 3;
			break;
		}
		break;

	case 5:  // load enemy shape banks
		{
			const Uint8 newEnemyShapeTables[] =
			{
				eventRec[eventLoc-1].eventdat > 0 ? eventRec[eventLoc-1].eventdat : 0,
				eventRec[eventLoc-1].eventdat2 > 0 ? eventRec[eventLoc-1].eventdat2 : 0,
				eventRec[eventLoc-1].eventdat3 > 0 ? eventRec[eventLoc-1].eventdat3 : 0,
				eventRec[eventLoc-1].eventdat4 > 0 ? eventRec[eventLoc-1].eventdat4 : 0,
			};
			
			for (unsigned int i = 0; i < COUNTOF(newEnemyShapeTables); ++i)
			{
				if (enemySpriteSheetIds[i] != newEnemyShapeTables[i])
				{
					if (newEnemyShapeTables[i] > 0)
					{
						assert(newEnemyShapeTables[i] <= COUNTOF(shapeFile));
						JE_loadCompShapes(&enemySpriteSheets[i], shapeFile[newEnemyShapeTables[i] - 1]);
					}
					else
						free_sprite2s(&enemySpriteSheets[i]);

					enemySpriteSheetIds[i] = newEnemyShapeTables[i];
				}
			}
		}
		break;

	case 6: /* Ground Enemy */
		JE_createNewEventEnemy(0, 25, 0);
		break;

	case 7: /* Top Enemy */
		JE_createNewEventEnemy(0, 50, 0);
		break;

	case 8:
		starActive = false;
		break;

	case 9:
		starActive = true;
		break;

	case 10: /* Ground Enemy 2 */
		JE_createNewEventEnemy(0, 75, 0);
		break;

	case 11:
		if (allPlayersGone || eventRec[eventLoc-1].eventdat == 1)
		{
			reallyEndLevel = true;
		}
		else if (!endLevel)
		{
			readyToEndLevel = false;
			endLevel = true;
			levelEnd = 40;
		}
		break;

	case 12: /* Custom 4x4 Ground Enemy */
		{
			uint temp = 0;
			switch (eventRec[eventLoc-1].eventdat6)
			{
			case 0:
			case 1:
				temp = 25;
				break;
			case 2:
				temp = 0;
				break;
			case 3:
				temp = 50;
				break;
			case 4:
				temp = 75;
				break;
			}
			eventRec[eventLoc-1].eventdat6 = 0;   /* We use EVENTDAT6 for the background */
			JE_createNewEventEnemy(0, temp, 0);
			JE_createNewEventEnemy(1, temp, 0);
			if (b > 0)
				enemy[b-1].ex += 24;
			JE_createNewEventEnemy(2, temp, 0);
			if (b > 0)
				enemy[b-1].ey -= 28;
			JE_createNewEventEnemy(3, temp, 0);
			if (b > 0)
			{
				enemy[b-1].ex += 24;
				enemy[b-1].ey -= 28;
			}
			break;
		}
	case 13:
		enemiesActive = false;
		break;

	case 14:
		enemiesActive = true;
		break;

	case 15: /* Sky Enemy */
		JE_createNewEventEnemy(0, 0, 0);
		break;

	case 16:
		if (eventRec[eventLoc-1].eventdat > 9)
		{
			fprintf(stderr, "warning: event 16: bad event data\n");
		}
		else
		{
			JE_drawTextWindow(outputs[eventRec[eventLoc-1].eventdat-1]);
			soundQueue[3] = windowTextSamples[eventRec[eventLoc-1].eventdat-1];
		}
		break;

	case 17: /* Ground Bottom */
		JE_createNewEventEnemy(0, 25, 0);
		if (b > 0)
		{
			enemy[b-1].ey = 190 + eventRec[eventLoc-1].eventdat5;
			enemy[b-1].ey += event_enemy_scroll_catchup(25, &enemy[b-1]);
		}
		break;

	case 18: /* Sky Enemy on Bottom */
		JE_createNewEventEnemy(0, 0, 0);
		if (b > 0)
			enemy[b-1].ey = 190 + eventRec[eventLoc-1].eventdat5;
		break;

	case 19: /* Enemy Global Move */
	{
		int initial_i = 0, max_i = 0;
		bool all_enemies = false;

		if (eventRec[eventLoc-1].eventdat3 > 79 && eventRec[eventLoc-1].eventdat3 < 90)
		{
			initial_i = 0;
			max_i = 100;
			all_enemies = false;
			eventRec[eventLoc-1].eventdat4 = newPL[eventRec[eventLoc-1].eventdat3 - 80];
		}
		else
		{
			switch (eventRec[eventLoc-1].eventdat3)
			{
			case 0:
				initial_i = 0;
				max_i = 100;
				all_enemies = false;
				break;
			case 2:
				initial_i = 0;
				max_i = 25;
				all_enemies = true;
				break;
			case 1:
				initial_i = 25;
				max_i = 50;
				all_enemies = true;
				break;
			case 3:
				initial_i = 50;
				max_i = 75;
				all_enemies = true;
				break;
			case 99:
				initial_i = 0;
				max_i = 100;
				all_enemies = true;
				break;
			}
		}

		for (int i = initial_i; i < max_i; i++)
		{
			if (all_enemies || enemy[i].linknum == eventRec[eventLoc-1].eventdat4)
			{
				if (eventRec[eventLoc-1].eventdat != -99)
					enemy[i].exc = eventRec[eventLoc-1].eventdat;

				if (eventRec[eventLoc-1].eventdat2 != -99)
					enemy[i].eyc = eventRec[eventLoc-1].eventdat2;

				if (eventRec[eventLoc-1].eventdat6 != 0)
				{
					enemy[i].fixedmovey = (eventRec[eventLoc-1].eventdat6 == -99)
					                       ? 0 : eventRec[eventLoc-1].eventdat6;
					enemy[i].fixedmovey_carry = 0;
					enemy[i].fixedmovey_carry_base = 0;
					enemy[i].fixedmovey_carry_move = 0;
				}

				if (eventRec[eventLoc-1].eventdat5 > 0)
					enemy[i].enemycycle = eventRec[eventLoc-1].eventdat5;
			}
		}
		break;
	}

	case 20: /* Enemy Global Accel */
		if (eventRec[eventLoc-1].eventdat3 > 79 && eventRec[eventLoc-1].eventdat3 < 90)
			eventRec[eventLoc-1].eventdat4 = newPL[eventRec[eventLoc-1].eventdat3 - 80];

		for (temp = 0; temp < 100; temp++)
		{
			if (enemyAvail[temp] != 1 &&
			    (enemy[temp].linknum == eventRec[eventLoc-1].eventdat4 || eventRec[eventLoc-1].eventdat4 == 0))
			{
				if (eventRec[eventLoc-1].eventdat != -99)
				{
					enemy[temp].excc = eventRec[eventLoc-1].eventdat;
					enemy[temp].exccw = abs(eventRec[eventLoc-1].eventdat);
					enemy[temp].exccwmax = abs(eventRec[eventLoc-1].eventdat);
					if (eventRec[eventLoc-1].eventdat > 0)
						enemy[temp].exccadd = 1;
					else
						enemy[temp].exccadd = -1;
				}

				if (eventRec[eventLoc-1].eventdat2 != -99)
				{
					enemy[temp].eycc = eventRec[eventLoc-1].eventdat2;
					enemy[temp].eyccw = abs(eventRec[eventLoc-1].eventdat2);
					enemy[temp].eyccwmax = abs(eventRec[eventLoc-1].eventdat2);
					if (eventRec[eventLoc-1].eventdat2 > 0)
						enemy[temp].eyccadd = 1;
					else
						enemy[temp].eyccadd = -1;
				}

				if (eventRec[eventLoc-1].eventdat5 > 0)
				{
					enemy[temp].enemycycle = eventRec[eventLoc-1].eventdat5;
				}
				if (eventRec[eventLoc-1].eventdat6 > 0)
				{
					enemy[temp].ani = eventRec[eventLoc-1].eventdat6;
					enemy[temp].animin = eventRec[eventLoc-1].eventdat5;
					enemy[temp].animax = 0;
					enemy[temp].aniactive = 1;
				}
			}
		}
		break;

	case 21:
		background3over = 1;
		break;

	case 22:
		background3over = 0;
		break;

	case 23: /* Sky Enemy on Bottom */
		JE_createNewEventEnemy(0, 50, 0);
		if (b > 0)
		{
			enemy[b-1].ey = 180 + eventRec[eventLoc-1].eventdat5;
			enemy[b-1].ey += event_enemy_scroll_catchup(50, &enemy[b-1]);
		}
		break;

	case 24: /* Enemy Global Animate */
		for (temp = 0; temp < 100; temp++)
		{
			if (enemy[temp].linknum == eventRec[eventLoc-1].eventdat4)
			{
				enemy[temp].aniactive = 1;
				enemy[temp].aniwhenfire = 0;
				if (eventRec[eventLoc-1].eventdat2 > 0)
				{
					enemy[temp].enemycycle = eventRec[eventLoc-1].eventdat2;
					enemy[temp].animin = enemy[temp].enemycycle;
				}
				else
				{
					enemy[temp].enemycycle = 0;
				}

				if (eventRec[eventLoc-1].eventdat > 0)
					enemy[temp].ani = eventRec[eventLoc-1].eventdat;

				if (eventRec[eventLoc-1].eventdat3 == 1)
				{
					enemy[temp].animax = enemy[temp].ani;
				}
				else if (eventRec[eventLoc-1].eventdat3 == 2)
				{
					enemy[temp].aniactive = 2;
					enemy[temp].animax = enemy[temp].ani;
					enemy[temp].aniwhenfire = 2;
				}
			}
		}
		break;

	case 25: /* Enemy Global Damage change */
		for (temp = 0; temp < 100; temp++)
		{
			if (eventRec[eventLoc-1].eventdat4 == 0 || enemy[temp].linknum == eventRec[eventLoc-1].eventdat4)
			{
				if (galagaMode)
					enemy[temp].armorleft = roundf(eventRec[eventLoc-1].eventdat * (difficultyLevel / 2));
				else
					enemy[temp].armorleft = eventRec[eventLoc-1].eventdat;
			}
		}
		break;

	case 26:
		smallEnemyAdjust = eventRec[eventLoc-1].eventdat;
		break;

	case 27: /* Enemy Global AccelRev */
		if (eventRec[eventLoc-1].eventdat3 > 79 && eventRec[eventLoc-1].eventdat3 < 90)
			eventRec[eventLoc-1].eventdat4 = newPL[eventRec[eventLoc-1].eventdat3 - 80];

		for (temp = 0; temp < 100; temp++)
		{
			if (eventRec[eventLoc-1].eventdat4 == 0 || enemy[temp].linknum == eventRec[eventLoc-1].eventdat4)
			{
				if (eventRec[eventLoc-1].eventdat != -99)
					enemy[temp].exrev = eventRec[eventLoc-1].eventdat;
				if (eventRec[eventLoc-1].eventdat2 != -99)
					enemy[temp].eyrev = eventRec[eventLoc-1].eventdat2;
				if (eventRec[eventLoc-1].eventdat3 != 0 && eventRec[eventLoc-1].eventdat3 < 17)
					enemy[temp].filter = eventRec[eventLoc-1].eventdat3;
			}
		}
		break;

	case 28:
		topEnemyOver = false;
		break;

	case 29:
		topEnemyOver = true;
		break;

	case 30:
		map1YDelay = 1;
		map1YDelayMax = 1;
		map2YDelay = 1;
		map2YDelayMax = 1;

		backMove = eventRec[eventLoc-1].eventdat;
		backMove2 = eventRec[eventLoc-1].eventdat2;
		explodeMove = backMove2;
		backMove3 = eventRec[eventLoc-1].eventdat3;
		break;

	case 31: /* Enemy Fire Override */
		for (temp = 0; temp < 100; temp++)
		{
			if (eventRec[eventLoc-1].eventdat4 == 99 || enemy[temp].linknum == eventRec[eventLoc-1].eventdat4)
			{
				enemy[temp].freq[1-1] = eventRec[eventLoc-1].eventdat ;
				enemy[temp].freq[2-1] = eventRec[eventLoc-1].eventdat2;
				enemy[temp].freq[3-1] = eventRec[eventLoc-1].eventdat3;
				for (temp2 = 0; temp2 < 3; temp2++)
				{
					enemy[temp].eshotwait[temp2] = 1;
				}
				if (enemy[temp].launchtype > 0)
				{
					enemy[temp].launchfreq = eventRec[eventLoc-1].eventdat5;
					enemy[temp].launchwait = 1;
				}
			}
		}
		break;

	case 32:  // create enemy
		JE_createNewEventEnemy(0, 50, 0);
		if (b > 0)
		{
			enemy[b-1].ey = 190;
			enemy[b-1].ey += event_enemy_scroll_catchup(50, &enemy[b-1]);
		}
		break;

	case 33: /* Enemy From other Enemies */
		if (!((eventRec[eventLoc-1].eventdat == 512 || eventRec[eventLoc-1].eventdat == 513) && (twoPlayerMode || onePlayerAction || superTyrian)))
		{
			if (superArcadeMode != SA_NONE)
			{
				if (eventRec[eventLoc-1].eventdat == 534)
					eventRec[eventLoc-1].eventdat = 827;
			}
			else if (!superTyrian)
			{
				const Uint8 lives = *player[0].lives;

				if (eventRec[eventLoc-1].eventdat == 533 && (lives == 11 || (mt_rand() % 15) < lives))
				{
					// enemy will drop random special weapon
					eventRec[eventLoc-1].eventdat = 829 + (mt_rand() % 6);
				}
			}
			if (eventRec[eventLoc-1].eventdat == 534 && superTyrian)
				eventRec[eventLoc-1].eventdat = 828 + superTyrianSpecials[mt_rand() % 4];

			for (temp = 0; temp < 100; temp++)
			{
				if (enemy[temp].linknum == eventRec[eventLoc-1].eventdat4)
					enemy[temp].enemydie = eventRec[eventLoc-1].eventdat;
			}
		}
		break;

	case 34: /* Start Music Fade */
		if (firstGameOver)
		{
			musicFade = true;
			tempVolume = tyrMusicVolume;
		}
		break;

	case 35: /* Play new song */
		if (firstGameOver)
		{
			play_song(eventRec[eventLoc-1].eventdat - 1);
			set_volume(tyrMusicVolume, fxVolume);
		}
		musicFade = false;
		break;

	case 36:
		readyToEndLevel = true;
		break;

	case 37:
		levelEnemyFrequency = eventRec[eventLoc-1].eventdat;
		break;

	case 38:
		curLoc = eventRec[eventLoc-1].eventdat;
		int new_event_loc = 1;
		for (tempW = 0; tempW < maxEvent; tempW++)
		{
			if (eventRec[tempW].eventtime <= curLoc)
				new_event_loc = tempW+1 - 1;
		}
		eventLoc = new_event_loc;
		break;

	case 39: /* Enemy Global Linknum Change */
		for (temp = 0; temp < 100; temp++)
		{
			if (enemy[temp].linknum == eventRec[eventLoc-1].eventdat)
				enemy[temp].linknum = eventRec[eventLoc-1].eventdat2;
		}
		break;

	case 40: /* Enemy Continual Damage */
		enemyContinualDamage = true;
		break;

	case 41:
		if (eventRec[eventLoc-1].eventdat == 0)
		{
			memset(enemyAvail, 1, sizeof(enemyAvail));
		}
		else
		{
			for (x = 0; x <= 24; x++)
				enemyAvail[x] = 1;
		}
		break;

	case 42:
		background3over = 2;
		break;

	case 43:
		background2over = eventRec[eventLoc-1].eventdat;
		break;

	case 44:
		filterActive       = (eventRec[eventLoc-1].eventdat > 0);
		filterFade         = (eventRec[eventLoc-1].eventdat == 2);
		levelFilter        = eventRec[eventLoc-1].eventdat2;
		levelBrightness    = eventRec[eventLoc-1].eventdat3;
		levelFilterNew     = eventRec[eventLoc-1].eventdat4;
		levelBrightnessChg = eventRec[eventLoc-1].eventdat5;
		filterFadeStart    = (eventRec[eventLoc-1].eventdat6 == 0);
		break;

	case 45: /* arcade-only enemy from other enemies */
		if (!superTyrian)
		{
			const Uint8 lives = *player[0].lives;

			if (eventRec[eventLoc-1].eventdat == 533 && (lives == 11 || (mt_rand() % 15) < lives))
			{
				eventRec[eventLoc-1].eventdat = 829 + (mt_rand() % 6);
			}
			if (twoPlayerMode || onePlayerAction)
			{
				for (temp = 0; temp < 100; temp++)
				{
					if (enemy[temp].linknum == eventRec[eventLoc-1].eventdat4)
						enemy[temp].enemydie = eventRec[eventLoc-1].eventdat;
				}
			}
		}
		break;

	case 46:  // change difficulty
		if (eventRec[eventLoc-1].eventdat3 != 0)
			damageRate = eventRec[eventLoc-1].eventdat3;

		if (eventRec[eventLoc-1].eventdat2 == 0 || twoPlayerMode || onePlayerAction)
		{
			difficultyLevel += eventRec[eventLoc-1].eventdat;
			if (difficultyLevel < DIFFICULTY_EASY)
				difficultyLevel = DIFFICULTY_EASY;
			if (difficultyLevel > DIFFICULTY_10)
				difficultyLevel = DIFFICULTY_10;
		}
		break;

	case 47: /* Enemy Global AccelRev */
		for (temp = 0; temp < 100; temp++)
		{
			if (eventRec[eventLoc-1].eventdat4 == 0 || enemy[temp].linknum == eventRec[eventLoc-1].eventdat4)
				enemy[temp].armorleft = eventRec[eventLoc-1].eventdat;
		}
		break;

	case 48: /* Background 2 Cannot be Transparent */
		background2notTransparent = true;
		break;

	case 49:
	case 50:
	case 51:
	case 52:
		tempDat2 = eventRec[eventLoc-1].eventdat;
		eventRec[eventLoc-1].eventdat = 0;
		tempDat = eventRec[eventLoc-1].eventdat3;
		eventRec[eventLoc-1].eventdat3 = 0;
		tempDat3 = eventRec[eventLoc-1].eventdat6;
		eventRec[eventLoc-1].eventdat6 = 0;
		enemyDat[0].armor = tempDat3;
		enemyDat[0].egraphic[1-1] = tempDat2;
		switch (eventRec[eventLoc-1].eventtype - 48)
		{
		case 1:
			temp = 25;
			break;
		case 2:
			temp = 0;
			break;
		case 3:
			temp = 50;
			break;
		case 4:
			temp = 75;
			break;
		}
		JE_createNewEventEnemy(0, temp, tempDat);
		eventRec[eventLoc-1].eventdat = tempDat2;
		eventRec[eventLoc-1].eventdat3 = tempDat;
		eventRec[eventLoc-1].eventdat6 = tempDat3;
		break;

	case 53:
		forceEvents = (eventRec[eventLoc-1].eventdat != 99);
		break;

	case 54:
		JE_eventJump(eventRec[eventLoc-1].eventdat);
		break;

	case 55: /* Enemy Global AccelRev */
		if (eventRec[eventLoc-1].eventdat3 > 79 && eventRec[eventLoc-1].eventdat3 < 90)
			eventRec[eventLoc-1].eventdat4 = newPL[eventRec[eventLoc-1].eventdat3 - 80];

		for (temp = 0; temp < 100; temp++)
		{
			if (eventRec[eventLoc-1].eventdat4 == 0 || enemy[temp].linknum == eventRec[eventLoc-1].eventdat4)
			{
				if (eventRec[eventLoc-1].eventdat != -99)
					enemy[temp].xaccel = eventRec[eventLoc-1].eventdat;
				if (eventRec[eventLoc-1].eventdat2 != -99)
					enemy[temp].yaccel = eventRec[eventLoc-1].eventdat2;
			}
		}
		break;

	case 56: /* Ground2 Bottom */
		JE_createNewEventEnemy(0, 75, 0);
		if (b > 0)
		{
			enemy[b-1].ey = 190;
			enemy[b-1].ey += event_enemy_scroll_catchup(75, &enemy[b-1]);
		}
		break;

	case 57:
		superEnemy254Jump = eventRec[eventLoc-1].eventdat;
		break;

	case 58: // Set enemy launch
		// This implementation comes from ArcTyr, and may not be 100% accurate to Tyrian 2000
		for (temp = 0; temp < 100; temp++)
		{
			if (eventRec[eventLoc-1].eventdat4 == 99 || enemy[temp].linknum == eventRec[eventLoc-1].eventdat4)
				enemy[temp].launchtype = eventRec[eventLoc-1].eventdat;
		}
		break;

	case 59: // Replace enemy
	case 68: // Note: random explosions got moved to event 99 in T2000
		// This implementation comes from ArcTyr, and may not be 100% accurate to Tyrian 2000
		{
			Uint16 eDatI = eventRec[eventLoc-1].eventdat;

			for (temp = 0; temp < 100; temp++)
			{
				if (!(eventRec[eventLoc-1].eventdat4 == 0 || enemy[temp].linknum == eventRec[eventLoc-1].eventdat4))
					continue;

				const int enemy_offset = temp - (temp % 25);
				b = JE_newEnemy(enemy_offset, eDatI, 0);
				if (b != 0)
				{
					enemy[b-1].ex = enemy[temp].ex;
					enemy[b-1].ey = enemy[temp].ey;
				}

				enemyAvail[temp] = 1;
			}			
		}
		break;
		break;

	case 60: /*Assign Special Enemy*/
		for (temp = 0; temp < 100; temp++)
		{
			if (enemy[temp].linknum == eventRec[eventLoc-1].eventdat4)
			{
				enemy[temp].special = true;
				enemy[temp].flagnum = eventRec[eventLoc-1].eventdat;
				enemy[temp].setto  = (eventRec[eventLoc-1].eventdat2 == 1);
			}
		}
		break;

	case 61:  // if specific flag set to specific value, skip events
		if (globalFlags[eventRec[eventLoc-1].eventdat-1] == eventRec[eventLoc-1].eventdat2)
			eventLoc += eventRec[eventLoc-1].eventdat3;
		break;

	case 62: /*Play sound effect*/
		soundQueue[3] = eventRec[eventLoc-1].eventdat;
		break;

	case 63:  // skip events if not in 2-player mode
		if (!twoPlayerMode && !onePlayerAction)
			eventLoc += eventRec[eventLoc-1].eventdat;
		break;

	case 64:
		if (!(eventRec[eventLoc-1].eventdat == 6 && twoPlayerMode && difficultyLevel > DIFFICULTY_NORMAL))
		{
			smoothies[eventRec[eventLoc-1].eventdat-1] = eventRec[eventLoc-1].eventdat2;
			temp = eventRec[eventLoc-1].eventdat;
			if (temp == 5)
				temp = 3;
			smoothie_data[temp-1] = eventRec[eventLoc-1].eventdat3;
		}
		break;

	case 65:
		background3x1 = (eventRec[eventLoc-1].eventdat == 0);
		break;

	case 66: /*If not on this difficulty level or higher then...*/
		if (initialDifficulty <= eventRec[eventLoc-1].eventdat)
			eventLoc += eventRec[eventLoc-1].eventdat2;
		break;

	case 67:
		levelTimer = (eventRec[eventLoc-1].eventdat == 1);
		levelTimerCountdown = eventRec[eventLoc-1].eventdat3 * 100;
		levelTimerJumpTo   = eventRec[eventLoc-1].eventdat2;
		break;

	case 69:
		for (uint i = 0; i < COUNTOF(player); ++i)
			player[i].invulnerable_ticks = eventRec[eventLoc-1].eventdat;
		break;

	case 70:
		if (eventRec[eventLoc-1].eventdat2 == 0)
		{  /*1-10*/
			bool found = false;

			for (temp = 1; temp <= 19; temp++)
				found = found || JE_searchFor(temp, NULL);

			if (!found)
				JE_eventJump(eventRec[eventLoc-1].eventdat);
		}
		else if (!JE_searchFor(eventRec[eventLoc-1].eventdat2, NULL) &&
		         (eventRec[eventLoc-1].eventdat3 == 0 || !JE_searchFor(eventRec[eventLoc-1].eventdat3, NULL)) &&
		         (eventRec[eventLoc-1].eventdat4 == 0 || !JE_searchFor(eventRec[eventLoc-1].eventdat4, NULL)))
		{
			JE_eventJump(eventRec[eventLoc-1].eventdat);
		}
		break;

	case 71:
		if (((((intptr_t)mapYPos - (intptr_t)&megaData1.mainmap) / sizeof(JE_byte *)) * 2) <= (unsigned)eventRec[eventLoc-1].eventdat2)
			JE_eventJump(eventRec[eventLoc-1].eventdat);
		break;

	case 72:
		background3x1b = (eventRec[eventLoc-1].eventdat == 1);
		break;

	case 73:
		skyEnemyOverAll = (eventRec[eventLoc-1].eventdat == 1);
		break;

	case 74: /* Enemy Global BounceParams */
		for (temp = 0; temp < 100; temp++)
		{
			if (eventRec[eventLoc-1].eventdat4 == 0 || enemy[temp].linknum == eventRec[eventLoc-1].eventdat4)
			{
				if (eventRec[eventLoc-1].eventdat5 != -99)
					enemy[temp].xminbounce = eventRec[eventLoc-1].eventdat5;

				if (eventRec[eventLoc-1].eventdat6 != -99)
					enemy[temp].yminbounce = eventRec[eventLoc-1].eventdat6;

				if (eventRec[eventLoc-1].eventdat != -99)
					// Bounce data was authored for the 320px field; shift the right bound
					// out by the widescreen extension so enemies sweep the full playfield.
					enemy[temp].xmaxbounce = eventRec[eventLoc-1].eventdat + (vga_width - LEGACY_WIDTH);

				if (eventRec[eventLoc-1].eventdat2 != -99)
					enemy[temp].ymaxbounce = eventRec[eventLoc-1].eventdat2;
			}
		}
		break;

	case 75:;
		bool temp_no_clue = false; // TODO: figure out what this is doing

		for (temp = 0; temp < 100; temp++)
		{
			if (enemyAvail[temp] == 0 &&
			    enemy[temp].eyc == 0 &&
			    enemy[temp].linknum >= eventRec[eventLoc-1].eventdat &&
			    enemy[temp].linknum <= eventRec[eventLoc-1].eventdat2)
			{
				temp_no_clue = true;
			}
		}

		if (temp_no_clue)
		{
			JE_byte enemy_i;
			do
			{
				temp = (mt_rand() % (eventRec[eventLoc-1].eventdat2 + 1 - eventRec[eventLoc-1].eventdat)) + eventRec[eventLoc-1].eventdat;
			} while (!(JE_searchFor(temp, &enemy_i) && enemy[enemy_i].eyc == 0));

			newPL[eventRec[eventLoc-1].eventdat3 - 80] = temp;
		}
		else
		{
			newPL[eventRec[eventLoc-1].eventdat3 - 80] = 255;
			if (eventRec[eventLoc-1].eventdat4 > 0)
			{ /*Skip*/
				curLoc = eventRec[eventLoc-1 + eventRec[eventLoc-1].eventdat4].eventtime - 1;
				eventLoc += eventRec[eventLoc-1].eventdat4 - 1;
			}
		}

		break;

	case 76:
		returnActive = true;
		break;

	case 77:
		mapYPos = &megaData1.mainmap[0][0];
		mapYPos += eventRec[eventLoc-1].eventdat / 2;
		if (eventRec[eventLoc-1].eventdat2 > 0)
		{
			mapY2Pos = &megaData2.mainmap[0][0];
			mapY2Pos += eventRec[eventLoc-1].eventdat2 / 2;
		}
		else
		{
			mapY2Pos = &megaData2.mainmap[0][0];
			mapY2Pos += eventRec[eventLoc-1].eventdat / 2;
		}
		break;

	case 78:
		if (galagaShotFreq < 10)
			galagaShotFreq++;
		break;

	case 79:
		boss_bar[0].link_num = eventRec[eventLoc - 1].eventdat;
		boss_bar[1].link_num = eventRec[eventLoc - 1].eventdat2;
		break;

	case 80:  // skip events if in 2-player mode
		if (twoPlayerMode)
			eventLoc += eventRec[eventLoc-1].eventdat;
		break;

	case 81: /*WRAP2*/
		BKwrap2   = &megaData2.mainmap[0][0];
		BKwrap2   += eventRec[eventLoc-1].eventdat / 2;
		BKwrap2to = &megaData2.mainmap[0][0];
		BKwrap2to += eventRec[eventLoc-1].eventdat2 / 2;
		break;

	case 82: /*Give SPECIAL WEAPON*/
		player[0].items.special = eventRec[eventLoc-1].eventdat;
		shotMultiPos[SHOT_SPECIAL] = 0;
		shotRepeat[SHOT_SPECIAL] = 0;
		shotMultiPos[SHOT_SPECIAL2] = 0;
		shotRepeat[SHOT_SPECIAL2] = 0;
		break;

	case 84: // timed battle level timer
		if (!timedBattleMode)
			break;

		// note: a copy of event 67
		levelTimer = (eventRec[eventLoc-1].eventdat == 1);
		levelTimerCountdown = eventRec[eventLoc-1].eventdat3 * 100;
		levelTimerJumpTo   = eventRec[eventLoc-1].eventdat2;
		break;

	case 85: // timed battle enemy from other enemies
		if (timedBattleMode)
		{
			for (temp = 0; temp < 100; temp++)
			{
				if (enemy[temp].linknum == eventRec[eventLoc-1].eventdat4)
					enemy[temp].enemydie = eventRec[eventLoc-1].eventdat;
			}
		}
		break;


	case 99:
		randomExplosions = (eventRec[eventLoc-1].eventdat == 1);
		break;

	default:
		fprintf(stderr, "warning: ignoring unknown event %d\n", eventRec[eventLoc-1].eventtype);
		break;
	}

	eventLoc++;
}

void JE_whoa(void)
{
	unsigned int i, j, color, offset, timer;
	unsigned int screenSize, topBorder, bottomBorder;
	Uint8 * TempScreen1, * TempScreen2, * TempScreenSwap;

	/* 'whoa' gets us that nifty screen fade used when you type in
	 * 'engage'.  We need two temporary screen buffers (char arrays can
	 * work too, but these screens already exist) for our effect.
	 * This could probably be a lot more efficient (there's probably a
	 * way to get vgascreen as one of the temp buffers), but it's only called
	 * once so don't worry about it. */

	TempScreen1  = game_screen->pixels;
	TempScreen2  = VGAScreen2->pixels;

	screenSize   = VGAScreenSeg->h * VGAScreenSeg->pitch;
	topBorder    = VGAScreenSeg->pitch * 4; /* Seems an arbitrary number of lines */
	bottomBorder = VGAScreenSeg->pitch * 7;

	/* Okay, one disadvantage to using other screens as temp buffers: they
	 * need to be the right size.  I doubt they'll ever be anything but 320x200,
	 * but just in case, these asserts will clue in whoever stumbles across
	 * the problem.  You can fix it with the stack or malloc. */
	assert((unsigned)VGAScreen2->h  * VGAScreen2->pitch >= screenSize &&
	       (unsigned)game_screen->h * game_screen->pitch >= screenSize);

	/* Clear the top and bottom borders.  We don't want to process
	 * them and we don't want to draw them. */
	memset((Uint8 *)VGAScreenSeg->pixels, 0, topBorder);
	memset((Uint8 *)VGAScreenSeg->pixels + screenSize - bottomBorder, 0, bottomBorder);

	/* Copy our test subject to one of the temporary buffers.  Blank the other */
	memset(TempScreen1, 0, screenSize);
	memcpy(TempScreen2, VGAScreenSeg->pixels, VGAScreenSeg->h * VGAScreenSeg->pitch);

	service_SDL_events(true);
	timer = 300; /* About 300 rounds is enough to make the screen mostly black */

	do
	{
		setDelay(1);

		/* This gets us our 'whoa' effect with pixel bleeding magic.
		 * I'm willing to bet the guy who originally wrote the asm was goofing
		 * around on acid and thought this looked good enough to use. */
		for (i = screenSize - bottomBorder, j = topBorder / 2; i > 0; i--, j++)
		{
			offset = j + i/8192 - 4;
			color = (TempScreen2[offset                    ] * 12 +
			         TempScreen1[offset-VGAScreenSeg->pitch]      +
			         TempScreen1[offset-1                  ]      +
			         TempScreen1[offset+1                  ]      +
			         TempScreen1[offset+VGAScreenSeg->pitch]) / 16;

			TempScreen1[j] = color;
		}

		/* Now copy that mess to the buffer. */
		memcpy((Uint8 *)VGAScreenSeg->pixels + topBorder, TempScreen1 + topBorder, screenSize - bottomBorder);

		JE_showVGA();

		timer--;
		wait_delay();

		/* Flip the buffer. */
		TempScreenSwap = TempScreen1;
		TempScreen1    = TempScreen2;
		TempScreen2    = TempScreenSwap;

	} while (!(timer == 0 || JE_anyButton()));

	levelWarningLines = 4;
}

static void JE_barX(JE_word x1, JE_word y1, JE_word x2, JE_word y2, JE_byte col)
{
	fill_rectangle_xy(VGAScreen, x1, y1,     x2, y1,     col + 1);
	fill_rectangle_xy(VGAScreen, x1, y1 + 1, x2, y2 - 1, col    );
	fill_rectangle_xy(VGAScreen, x1, y2,     x2, y2,     col - 1);
}

// Palette-bank base for a boss bar, matching the boss's endless tier tint (elite / champion)
// so the bar reads as part of that boss; ordinary and campaign bosses keep bank 7.
static int boss_bar_tint_base(JE_byte link_num)
{
	int tier = 0;  // highest tier among the boss's live parts
	if (endlessMode && link_num != 0)
		for (unsigned int e = 0; e < COUNTOF(enemy); e++)
			if (enemyAvail[e] != 1 && enemy[e].linknum == link_num && enemy[e].eliteState > tier)
				tier = enemy[e].eliteState;
	if (tier == 3) return ENDLESS_CHAMPION_FILTER;
	if (tier == 2) return ENDLESS_ELITE_FILTER;
	return 112;  // palette bank 7 (default)
}

static void bbfill(SDL_Surface *dst, int x0, int y0, int x1, int y1, int scale, Uint8 color)
{
	fill_rectangle_xy(dst, x0 * scale, y0 * scale, (x1 + 1) * scale - 1, (y1 + 1) * scale - 1, color);
}

// One enhanced boss bar: a framed, recessed track with a glossy gradient fill. (gx,gy)/gw/gh are
// the outer frame; horizontal fills left->right, vertical bottom->up; fraction is 0..1; flash
// brightens on a hit. Colours stay within the one palette bank passed as base. notes.md §Boss & enemy health bars.
static void draw_boss_bar_gauge(SDL_Surface *dst, int scale, int gx, int gy, int gw, int gh,
                                bool horizontal, float fraction, int flash, int base)
{
	if (gw < 4 || gh < 4)
		return;
	if (fraction < 0.0f)
		fraction = 0.0f;
	else if (fraction > 1.0f)
		fraction = 1.0f;

	const int BASE  = base;        // palette bank 7 normally; elite/champion tint in endless
	const int FRAME = BASE + 6;    // visible outline
	const int TRACK = BASE + 2;    // dark empty groove

	const int ix = gx + 1, iy = gy + 1;     // inner track origin
	const int iw = gw - 2, ih = gh - 2;     // inner track size
	const int cross = horizontal ? ih : iw; // bar thickness (across the fill)
	const int along = horizontal ? iw : ih; // bar length (along the fill)

	// Outline + recessed empty groove (always drawn, so a depleted bar still reads).
	bbfill(dst, gx, gy, gx + gw - 1, gy + gh - 1, scale, (Uint8)FRAME);
	bbfill(dst, ix, iy, ix + iw - 1, iy + ih - 1, scale, (Uint8)TRACK);

	const int fillLen = (int)(along * fraction + 0.5f);
	if (fillLen <= 0)
		return;  // boss at (near) zero: just the empty groove shows

	// Glossy fill: a brightness gradient across the thickness (highlight near the
	// first edge, darker at the far edge), lifted by the hit-flash amount.
	for (int c = 0; c < cross; ++c)
	{
		int shade = (cross <= 1) ? 13 : 15 - (c * 6) / (cross - 1);  // 127 -> ~121
		int col = BASE + shade + flash;
		if (col > BASE + 15)
			col = BASE + 15;

		if (horizontal)
			bbfill(dst, ix, iy + c, ix + fillLen - 1, iy + c, scale, (Uint8)col);
		else
			bbfill(dst, ix + c, iy + ih - fillLen, ix + c, iy + ih - 1, scale, (Uint8)col);
	}

	// Bright leading-edge cap at the tip of the fill for a crisp, readable edge.
	if (horizontal)
		bbfill(dst, ix + fillLen - 1, iy, ix + fillLen - 1, iy + ih - 1, scale, (Uint8)(BASE + 15));
	else
		bbfill(dst, ix, iy + ih - fillLen, ix + iw - 1, iy + ih - fillLen, scale, (Uint8)(BASE + 15));
}

// Shared by draw_boss_bars_enhanced and boss_bar_right_edge_x (below), so the two can never
// drift apart: the enhanced gauge's thickness and the gap between two grouped bars.
static const int BOSS_BAR_THICK = 7;
static const int BOSS_BAR_GAP   = 4;

static int boss_flash_render(int color, float alpha)
{
	if (color <= 0)
		return 0;
	int f = (int)(color + 1.0f - alpha + 0.5f);
	if (f < 0)
		f = 0;
	return f;
}

// Lay out and draw the enhanced boss bars per the player's Enhancements
// settings (bossBarLayout / bossBarTwoMode). barCount is 1 or 2. notes.md §Boss & enemy health bars.
static void draw_boss_bars_enhanced(SDL_Surface *dst, int scale, float flashAlpha, bool decrement, unsigned int barCount)
{
	// Bars draw into game_screen (playfield space); JE_inGameDisplays draws the corner HUD
	// indicators in the same space, so keep bars centred on the playfield and clear of them.
	// notes.md §Widescreen, §Boss & enemy health bars.
	const int PF_L  = PLAYFIELD_LEFT;        // 24: left visible edge
	const int PF_R  = PLAYFIELD_RIGHT;       // 322: right visible edge, just before the HUD
	const int PF_CX = PF_L + PLAYFIELD_WIDTH / 2;    // 173: playfield centre
	const bool two  = (barCount == 2);

	const int THICK = BOSS_BAR_THICK;   // bar thickness
	const int GAP   = BOSS_BAR_GAP;     // spacing between two grouped bars

	const bool vertical  = (bossBarLayout == BOSS_BAR_LEFT || bossBarLayout == BOSS_BAR_RIGHT);
	const bool splitMode = (bossBarTwoMode == BOSS_BAR_TWO_SPLIT);
	const bool stackMode = (bossBarTwoMode == BOSS_BAR_TWO_STACKED);

	if (!vertical)
	{
		// ----- Horizontal bars (Top / Bottom) -----
		const bool top = (bossBarLayout == BOSS_BAR_TOP);
		const bool sideBySide = two && splitMode;  // halves on one row; else stacked rows

		// Longest centred span that clears the corner indicators: at the TOP the
		// special-weapon icon ends ~x49 (and player 2's indicators start ~x208 in
		// 2-player); near the BOTTOM the playfield edge is the limit.
		const int leftClear  = top ? 64 : PF_L + 2;
		const int rightClear = (top && twoPlayerMode) ? 208 : PF_R;
		const int leftHalf   = PF_CX - leftClear;
		const int rightHalf  = rightClear - PF_CX;
		const int half       = (leftHalf < rightHalf) ? leftHalf : rightHalf;
		const int fullL      = PF_CX - half;
		const int fullR      = PF_CX + half;
		const int fullW      = fullR - fullL + 1;

		// Top drops below the level timer when it shows; bottom sits above the score
		// row (y175+) and the low-armor WARNING (y178+).
		const int topAnchor = levelTimer ? 18 : 6;
		const int botAnchor = 174;

		for (unsigned int b = 0; b < barCount; b++)
		{
			int bx = fullL, bw = fullW, by;

			if (sideBySide)
			{
				bw = (fullW - GAP) / 2;
				bx = (b == 0) ? fullL : fullR - bw + 1;
			}

			if (top)
				by = topAnchor + ((two && !sideBySide) ? (int)b * (THICK + GAP) : 0);
			else  // bottom: stacked grows upward, bar 0 on top
				by = botAnchor - THICK + 1
				   - ((two && !sideBySide) ? (int)(barCount - 1 - b) * (THICK + GAP) : 0);

			draw_boss_bar_gauge(dst, scale, bx, by, bw, THICK, true,
			                    boss_bar[b].armor / 254.0f, boss_flash_render(boss_bar[b].color, flashAlpha),
			                    boss_bar_tint_base(boss_bar[b].link_num));

			if (decrement && boss_bar[b].color > 0)
				boss_bar[b].color--;
		}
	}
	else
	{
		// ----- Vertical bars (Left / Right) -----
		// Bars hug the side edges: the left edge carries player-1 corner indicators
		// (use the clear middle band); the right is full-height in 1-player but
		// banded in 2-player. Two-bar modes: Split = one per side, Together =
		// parallel on the chosen side, Stacked = one above the other on that side.
		const int edgeL    = PF_L + 2;            // left bar's left edge
		const int edgeR    = PF_R - 1;            // right bar's right edge
		const int clearTop = 48, clearBot = 158;  // between the top & bottom corner HUD
		const int fullTop  = 8,  fullBot  = 176;  // clear of the top/bottom WARNING strips

		for (unsigned int b = 0; b < barCount; b++)
		{
			const bool onLeft = (two && splitMode)
				? (b == 0)                              // one bar on each side
				: (bossBarLayout == BOSS_BAR_LEFT);     // single, or both on chosen side

			// Parallel ("Together") offsets the second bar inward; split/stacked share a column.
			const int slot = (two && !splitMode && !stackMode) ? (int)b : 0;

			int bx = onLeft
				? edgeL + slot * (THICK + GAP)
				: edgeR - THICK + 1 - slot * (THICK + GAP);

			// Full height only on a HUD-free side; split forces both sides to the
			// clear band so the pair matches.
			const bool clearColumn = (two && splitMode) ? true : (onLeft || twoPlayerMode);
			int vTop = clearColumn ? clearTop : fullTop;
			int vBot = clearColumn ? clearBot : fullBot;

			// Stacked: split this side's span into a top half and a bottom half.
			if (two && stackMode)
			{
				const int mid = (vTop + vBot) / 2;
				if (b == 0)
					vBot = mid - GAP / 2;          // upper bar
				else
					vTop = mid + GAP / 2 + 1;      // lower bar
			}

			draw_boss_bar_gauge(dst, scale, bx, vTop, THICK, vBot - vTop + 1, false,
			                    boss_bar[b].armor / 254.0f, boss_flash_render(boss_bar[b].color, flashAlpha),
			                    boss_bar_tint_base(boss_bar[b].link_num));

			if (decrement && boss_bar[b].color > 0)
				boss_bar[b].color--;
		}
	}
}

// Original compact double-sided boss bar (kept for the "Classic" setting).
static void draw_boss_bars_classic(unsigned int bars)
{
	const int playfield_left = PLAYFIELD_LEFT;
	const int center_x = playfield_left + PLAYFIELD_WIDTH / 2;

	for (unsigned int b = 0; b < bars; b++)
	{
		unsigned int x;

		if (bars == 2)
			x = center_x + ((b == 0) ? -30 : 30);
		else
			x = center_x + ((levelTimer) ? 95 : 0);  // level timer and boss bar would overlap

		unsigned int y = (levelTimer) ? 15 : 7;

		const int base = boss_bar_tint_base(boss_bar[b].link_num);  // bank 7, or elite/champion tint
		JE_barX(x - 25, y, x + 25, y + 5, base + 3);
		JE_barX(x - (boss_bar[b].armor / 10), y, x + (boss_bar[b].armor + 5) / 10, y + 5, base + 6 + boss_bar[b].color);

		if (boss_bar[b].color > 0)
			boss_bar[b].color--;
	}
}

// Tunables for the tiny per-enemy health bars.
enum
{
	ENEMY_BAR_MIN_HP  = 1,   // show a bar on every enemy that survives a hit (incl. low-HP trash)
	ENEMY_BAR_MAX_LEN = 48,  // cap so a spread-out linkgroup can't paint a screen-long bar
	ENEMY_BAR_MIN_LEN = 3,   // shorter than this and the gauge can't be read; skip it
	ENEMY_BAR_THICK   = 2,   // bar thickness across the fill (groove row/col + shadow)
};

// Lay out and draw one enemy's thin health bar from the group's on-screen bounding box
// [boxL..boxR] x [boxT..boxB-1] (boxB is one row past the sprites). Orientation,
// placement and opacity come from the Enemy Bars settings. frac fills the bar
// (left->right horizontal, bottom->up vertical); the fill colour tracks health on the
// bank-7 (112..127) ramp, so it reads on every level palette.
static void draw_enemy_hp_bar(int id, int boxL, int boxR, int boxT, int boxB, float frac,
                              int barBase, float par_frac, int par_layer, float par_anchor,
                              int par_ybase, float par_yfrac, int par_ylayer)
{
	if (frac < 0.0f) frac = 0.0f; else if (frac > 1.0f) frac = 1.0f;

	const Uint8 opacity = (Uint8)(enemyBarOpacity * 255 / 100);
	if (opacity == 0)
		return;  // fully transparent: nothing to draw or record

	const bool vertical  = (enemyBarLayout == ENEMY_BAR_VERTICAL);
	const int  boxBottom = boxB - 1;                 // inclusive bottom sprite row
	const int  cx = (boxL + boxR) / 2;               // enemy centre
	const int  cy = (boxT + boxBottom) / 2;
	const int  T  = ENEMY_BAR_THICK;

	int along, x, y;

	if (!vertical)
	{
		// Horizontal bar: length spans the enemy width, inset 1px each end so
		// enemies packed into a row keep a visible gap between their bars.
		int xl = boxL + 1, xr = boxR - 1;
		along = xr - xl + 1;
		if (along < ENEMY_BAR_MIN_LEN)
			return;
		if (along > ENEMY_BAR_MAX_LEN)
		{
			xl = cx - ENEMY_BAR_MAX_LEN / 2;
			along = ENEMY_BAR_MAX_LEN;
		}
		x = xl;

		switch (enemyBarPosition)
		{
		case ENEMY_BAR_POS_TOP:    y = boxT - T - 1;                     break;  // above the enemy
		case ENEMY_BAR_POS_CENTER: y = cy - T / 2;                       break;  // over the enemy's centre
		case ENEMY_BAR_POS_LEFT:   x = boxL - along - 1; y = cy - T / 2; break;  // left of the enemy
		case ENEMY_BAR_POS_RIGHT:  x = boxR + 2;         y = cy - T / 2; break;  // right of the enemy
		case ENEMY_BAR_POS_BOTTOM:
		default:                   y = boxB + 1;                         break;  // below (original)
		}
	}
	else
	{
		// Vertical bar: length spans the enemy height, inset 1px each end.
		int yt = boxT + 1, yb = boxBottom - 1;
		along = yb - yt + 1;
		if (along < ENEMY_BAR_MIN_LEN)
			return;
		if (along > ENEMY_BAR_MAX_LEN)
		{
			yt = cy - ENEMY_BAR_MAX_LEN / 2;
			along = ENEMY_BAR_MAX_LEN;
		}
		y = yt;

		switch (enemyBarPosition)
		{
		case ENEMY_BAR_POS_LEFT:   x = boxL - T - 1;                     break;  // left of the enemy
		case ENEMY_BAR_POS_CENTER: x = cx - T / 2;                       break;  // over the enemy's centre
		case ENEMY_BAR_POS_TOP:    y = boxT - along - 1; x = cx - T / 2; break;  // above the enemy
		case ENEMY_BAR_POS_BOTTOM: y = boxB + 1;         x = cx - T / 2; break;  // below the enemy
		case ENEMY_BAR_POS_RIGHT:
		default:                   x = boxR + 2;                         break;  // right of the enemy
		}
	}

	const int fill = (int)(along * frac + 0.5f);
	// Fill colour tracks remaining health within the bar's palette bank: full -> +15
	// (bright), near-empty -> +5 (dark). barBase is bank 7 (112) normally, or the elite /
	// champion tint bank so a special enemy's bar matches its tint.
	const int col = (fill > 0) ? barBase + 5 + (int)(frac * 10.0f + 0.5f) : barBase;

	// Draw into the authoritative tick frame.
	rl_draw_hp_bar(VGAScreen, x, y, along, fill, (Uint8)col, vertical, opacity);

	// Record the bar so the replay reproduces AND interpolates it with its enemy
	// (id = RL_ID_ENEMYBAR_BASE + slot). It stays out of the residual via recording
	// (normal levels) / pre-snapshot call order (smoothie levels) — see the call sites.
	if (render_list_recording)
	{
		rl_current_id = id;
		rl_current_par_frac = par_frac;    // float the bar's parallax to match its enemy
		rl_current_par_layer = par_layer;
		rl_current_par_anchor = par_anchor;
		rl_current_par_ybase = par_ybase;
		rl_current_par_yfrac = par_yfrac;  // float the bar's vertical scroll to match its enemy
		rl_current_par_ylayer = par_ylayer;
		rl_rec_hp_bar(x, y, along, fill, (Uint8)col, vertical, opacity);
		rl_current_id = 0;
		rl_current_par_frac = 0.0f;
		rl_current_par_layer = 0;
		rl_current_par_anchor = 0.0f;
		rl_current_par_ybase = 0;
		rl_current_par_yfrac = 0.0f;
		rl_current_par_ylayer = 0;
	}
}

// Tiny per-enemy health bars: one bar per linknum group, spanning the group and showing its
// most-damaged part. Shown once an enemy has taken damage (healthbar_seen latch); boss-linked
// groups are skipped; only active, damageable slots qualify. notes.md §Boss & enemy health bars.
static void draw_enemy_health_bars(void)
{
	if (!enemyBars)
		return;

	bool done[100] = { false };

	for (unsigned int e = 0; e < 100; e++)
	{
		if (enemyAvail[e] != 0 || done[e])
			continue;

		const int link = enemy[e].linknum;

		// Groups that already own a boss bar are handled by draw_boss_bar().
		if (link != 0 && (link == boss_bar[0].link_num || link == boss_bar[1].link_num))
		{
			done[e] = true;
			continue;
		}

		// Accumulate the group's on-screen bounding box and most-damaged fraction.
		bool shown = false;
		float frac = 1.0f;
		int left = 99999, right = -99999, top = 99999, bottom = -99999;

		for (unsigned int f = e; f < 100; f++)
		{
			// Skip freed (1) and lingering non-damageable (2) slots; this also shrinks
			// a linkgroup's bar to just its surviving parts.
			if (enemyAvail[f] != 0)
				continue;
			if (link == 0 ? (f != e) : (enemy[f].linknum != (JE_byte)link))
				continue;

			done[f] = true;

			// Sprite footprint: a normal enemy is one 12x14 cell at (ex+mapoffset, ey);
			// a "2x2" enemy (size==1) is four cells around that point (+-6 x, +-7 y),
			// i.e. 24x28.
			const bool big = (enemy[f].size == 1);
			const int sx = enemy[f].ex + enemy[f].mapoffset + (big ? -6 : 0);
			const int sy = enemy[f].ey + (big ? -7 : 0);
			const int sw = big ? 24 : 12;
			const int sh = big ? 28 : 14;

			if (sx < left)             left = sx;
			if (sx + sw - 1 > right)   right = sx + sw - 1;
			if (sy < top)              top = sy;
			if (sy + sh > bottom)      bottom = sy + sh;

			// Bar only while alive AND damageable: armorleft == 0 is dead/dying, 255 is the
			// "invincible" sentinel. A level-script event (types 25/47) can raise an
			// already-damaged enemy's armor to 255 mid-fight; it can then never lose armor
			// again, so its bar would hang over an unkillable enemy forever.
			if (enemy[f].healthbar_seen && enemy[f].armorleft > 0 && enemy[f].armorleft < 255 &&
			    enemy[f].healthbar_max >= ENEMY_BAR_MIN_HP)
			{
				shown = true;
				const float f2 = (float)enemy[f].armorleft / (float)enemy[f].healthbar_max;
				if (f2 < frac)
					frac = f2;
			}
		}

		if (shown)
		{
			// Endless special enemies get a bar in their tint bank (elite / champion) so the
			// bar reads as part of the enemy; ordinary enemies keep the bank-7 yellow ramp.
			int barBase = 112;  // palette bank 7
			if (endlessMode && enemy[e].eliteState == 2)
				barBase = ENDLESS_ELITE_FILTER;
			else if (endlessMode && enemy[e].eliteState == 3)
				barBase = ENDLESS_CHAMPION_FILTER;
			// Slot banks 0/25/50/75 use horizontal anchors 2/1/3/1 respectively (the same
			// batches configured around JE_drawEnemy above). Preserve the representative enemy's
			// absolute anchor so finalize can apply the same draw-order correction to its bar.
			const int par_layer = (e < 25) ? 2 : (e < 50) ? 1 : (e < 75) ? 3 : 1;
			const float par_anchor = (float)(enemy[e].mapoffset - PLAYFIELD_X_SHIFT) + enemy[e].mapoffset_frac;
			draw_enemy_hp_bar(RL_ID_ENEMYBAR_BASE + (int)e, left, right, top, bottom, frac,
			                  barBase, enemy[e].mapoffset_frac, par_layer, par_anchor,
			                  enemy[e].scroll_ybase, enemy[e].scroll_yfrac,
			                  enemy[e].scroll_ylayer);
		}
	}
}

void draw_boss_bar(void)
{
	for (unsigned int b = 0; b < COUNTOF(boss_bar); b++)
	{
		if (boss_bar[b].link_num == 0)
			continue;

		unsigned int armor = 256;  // higher than armor max

		for (unsigned int e = 0; e < COUNTOF(enemy); e++)  // find most damaged
		{
			if (enemyAvail[e] != 1 && enemy[e].linknum == boss_bar[b].link_num)
				if (enemy[e].armorleft < armor)
					armor = enemy[e].armorleft;
		}

		if (armor > 255 || armor == 0)  // boss dead?
		{
			boss_bar[b].link_num = 0;
			// Tally "Bosses slain" here -- the one definitive moment a boss that actually
			// had a health bar is destroyed. Each such boss counts exactly once (the bar is
			// skipped once link_num is 0); high-armor mini-bosses that never spawn a bar
			// are never counted.
			if (endlessMode)
				++endlessRunBossKills;
		}
		else
			boss_bar[b].armor = (armor == 255) ? 254 : armor;  // 255 would make the bar too long
	}

	unsigned int bars = (boss_bar[0].link_num != 0 ? 1 : 0)
	                  + (boss_bar[1].link_num != 0 ? 1 : 0);

	// if only one bar left, make it the first one
	if (bars == 1 && boss_bar[0].link_num == 0)
	{
		memcpy(&boss_bar[0], &boss_bar[1], sizeof(boss_bar_t));
		boss_bar[1].link_num = 0;
	}

	if (bars == 0)
		return;

	if (bossBarStyle == BOSS_BAR_ENHANCED)
		draw_boss_bars_enhanced(VGAScreen, 1, 1.0f, true, bars);
	else
		draw_boss_bars_classic(bars);
}

// Per-frame redraw of the enhanced boss bars at an interpolated hit flash. notes.md §Boss & enemy health bars.
static void draw_boss_bar_present(SDL_Surface *dst, int scale, float alpha)
{
	if (bossBarStyle != BOSS_BAR_ENHANCED)
		return;

	bool flashing = false;
	for (unsigned int b = 0; b < COUNTOF(boss_bar); b++)
		if (boss_bar[b].link_num != 0 && boss_bar[b].color > 0)
			flashing = true;
	if (!flashing)
		return;

	const unsigned int bars = (boss_bar[0].link_num != 0 ? 1 : 0)
	                        + (boss_bar[1].link_num != 0 ? 1 : 0);
	if (bars == 0)
		return;

	draw_boss_bars_enhanced(dst, scale, alpha, false, bars);
}

// How far LEFT the endless kill-fire HUD (bottom-right of the playfield, right-aligned to
// hudRightX) must shift to clear a currently-shown RIGHT-side vertical boss bar, or 0 if none is
// in the way. Classic style and non-right layouts never occupy that column. Mirrors the bx
// geometry in draw_boss_bars_enhanced exactly (same BOSS_BAR_THICK/GAP) so the two can't drift.
int boss_bar_hud_left_shift(int hudRightX)
{
	const unsigned int bars = (boss_bar[0].link_num != 0 ? 1 : 0)
	                         + (boss_bar[1].link_num != 0 ? 1 : 0);
	if (bars == 0 || bossBarStyle != BOSS_BAR_ENHANCED)
		return 0;
	if (bossBarLayout != BOSS_BAR_LEFT && bossBarLayout != BOSS_BAR_RIGHT)
		return 0;  // horizontal layouts never occupy the right edge column

	const bool splitMode = (bossBarTwoMode == BOSS_BAR_TWO_SPLIT);
	const bool onRight = (bossBarLayout == BOSS_BAR_RIGHT) || (bars == 2 && splitMode);
	if (!onRight)
		return 0;  // vertical bar(s) confined to the left edge

	// Two bars side-by-side ("Together") widen the occupied column; Stacked/Split/single share
	// just one THICK-wide column (see the bx formula in draw_boss_bars_enhanced).
	const bool together = (bars == 2 && !splitMode && bossBarTwoMode == BOSS_BAR_TWO_TOGETHER);
	const int slots = together ? 1 : 0;
	const int leftmostX = (PLAYFIELD_RIGHT - 1) - BOSS_BAR_THICK + 1 - slots * (BOSS_BAR_THICK + BOSS_BAR_GAP);

	return (hudRightX >= leftmostX) ? (hudRightX - leftmostX + 4) : 0;  // +4px clearance
}

// True while an Enhanced BOTTOM horizontal boss bar is shown -- it can span near the full
// playfield width and sits close to the bottom edge, unlike TOP which never reaches there.
bool boss_bar_hud_needs_up_shift(void)
{
	const unsigned int bars = (boss_bar[0].link_num != 0 ? 1 : 0)
	                        + (boss_bar[1].link_num != 0 ? 1 : 0);
	return bars > 0 && bossBarStyle == BOSS_BAR_ENHANCED && bossBarLayout == BOSS_BAR_BOTTOM;
}
