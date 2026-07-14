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
#ifndef VIDEO_H
#define VIDEO_H

#include "opentyr.h"

#include "SDL.h"

#define vga_width 356
#define vga_height 200

 /*
  * Original Tyrian rendered 320x200 with the rightmost columns as HUD.  Widened
  * to 356x200 -- exactly 16:9 at square pixels -- for true-widescreen gameplay,
  * HUD width intact and pinned to the right.  356 is the practical ceiling: the
  * background tile maps are only 14 columns (336px) wide.  LEGACY_WIDTH is the
  * original 320px the menu/shop/HUD art is still authored against.
  */
#define LEGACY_WIDTH 320
#define HUD_WIDTH 57
#define PLAYFIELD_WIDTH (vga_width - HUD_WIDTH)
#define PLAYFIELD_X_SHIFT (-12)
#define HUD_X(x) ((x) + (vga_width - LEGACY_WIDTH))

/*
 * PLAYFIELD_LEFT must equal composite_playfield()'s crop offset, and is
 * deliberately NOT derived from PLAYFIELD_X_SHIFT (an unrelated tile phase).
 * notes.md §Widescreen.
 */
#define PLAYFIELD_LEFT   24
#define PLAYFIELD_RIGHT  (PLAYFIELD_LEFT + PLAYFIELD_WIDTH - 1)
#define PLAYFIELD_CENTER_X(w)  (PLAYFIELD_LEFT + (PLAYFIELD_WIDTH - (int)(w)) / 2)

// Pillarbox margin (per side) when a legacy 320px screen is centred in the
// widescreen buffer; also sizes the gradient fade table in video.c.
#define MENU_X_OFFSET ((vga_width - LEGACY_WIDTH) / 2)

// Clamp on the ship's reference position (this_player->x/y or vt_x/vt_y, not the
// sprite edge): how close it may get to the playfield edges. Enforced by both
// JE_playerMovement (mainint.c) and the VT ship integrator (tyrian2.c) -- re-tune here.
// blit_sprite2/2x2 draw ~17-31px past this position without clipping at the surface
// edge, so the margins can't shrink to 0. Originals: 40/8 (x), 10/160 (y).
#define SHIP_LEFT_MARGIN   29
#define SHIP_RIGHT_MARGIN  -4
#define SHIP_TOP_MARGIN    7
#define SHIP_BOTTOM_MARGIN 162

typedef enum {
	SCALE_CENTER,
	SCALE_INTEGER,
	SCALE_WIDESCREEN,   // fit the buffer at its own pixel ratio (square pixels = true 16:9)
	SCALE_CLASSIC_PAR,  // fit at the original DOS pixel aspect (taller pixels, ~3:2 overall)
	ScalingMode_MAX
} ScalingMode;

extern const char *const scaling_mode_names[ScalingMode_MAX];

extern int fullscreen_display; // -1 means windowed
extern bool output_vsync;      // present in sync with the display refresh rate
extern ScalingMode scaling_mode;

/*
 * Sub-pixel supersampling into an NxN buffer so motion lands on 1/N-pixel
 * positions.  0 = Auto (match the scaler), 1 = off, 2..8 = fixed factor.
 * notes.md §Supersampling & video.
 */
#define RENDER_SUPERSAMPLE_MAX 8
extern int render_supersample;
int effective_supersample(void);

/*
 * How the supersampled frame fits a larger output: Sharp / Smooth / None.
 * Values persist in the config: keep Sharp=0/Smooth=1 and append.
 * notes.md §Supersampling & video.
 */
enum
{
	SS_FILTER_SHARP = 0,
	SS_FILTER_SMOOTH = 1,
	SS_FILTER_NONE = 2,
};
extern int render_supersample_filter;

extern bool show_fps;          // draw the FPS counter during gameplay
extern int current_fps;        // presented frames during the last sampled second

extern SDL_Surface *VGAScreen, *VGAScreenSeg;
extern SDL_Surface *game_screen;
extern SDL_Surface *VGAScreen2;

extern SDL_Window *main_window;
extern SDL_PixelFormat *main_window_tex_format;

void init_video(void);

void video_on_win_resize(void);
void reinit_fullscreen(int new_display);
void toggle_fullscreen(void);
bool init_scaler(unsigned int new_scaler);
bool set_scaling_mode_by_name(const char *name);

void deinit_video(void);

void JE_clr256(SDL_Surface *);
void JE_showVGA(void);

// Re-present the last composed frame (no software scaling). Keeps the display refreshing
// while a modal system overlay (the Vita IME) is up so the compositor keeps drawing it.
void video_repeat_last_present(void);
// Present a supersampled 8-bit frame (vga_width*N x vga_height*N): palette-converted
// 1:1 into a texture, filtered per render_supersample_filter, and fitted into the
// same on-screen rectangle the classic path would use, so supersampling never
// changes the window/output size.
void present_hi(SDL_Surface *hi);
void set_vsync(bool enabled);

// Recover the window contents after a resolution change / expose. video_repaint()
// re-presents the current frame unconditionally; video_repaint_if_stale() does so only
// when the window size changed since the last present (or `force` for expose / render
// reset). The event pump calls the latter so input-wait screens don't freeze on a resize.
void video_repaint(void);
void video_repaint_if_stale(bool force);

void set_menu_centered(bool centered);
int video_get_menu_x_offset(void);

void mapScreenPointToWindow(Sint32 *inout_x, Sint32 *inout_y);
void mapWindowPointToScreen(Sint32 *inout_x, Sint32 *inout_y);
void scaleWindowDistanceToScreen(Sint32 *inout_x, Sint32 *inout_y);
void scaleWindowDistanceToScreenF(float *inout_x, float *inout_y);  // float, no rounding loss

#endif /* VIDEO_H */
