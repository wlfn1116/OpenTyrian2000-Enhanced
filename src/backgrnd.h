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
#ifndef BACKGRND_H
#define BACKGRND_H

#include "opentyr.h"

#include "SDL.h"

#include <stdint.h>

extern JE_word backPos, backPos2, backPos3;
extern JE_word backMove, backMove2, backMove3;
extern int endlessScrollExtraThisTick;  // extra scroll sub-steps this tick (endless overclock); see backgrnd.c
// Endless SMOOTH scroll boost: per-layer extra scroll px this tick (fractional carry). Set
// once/tick in tyrian2.c; makes the boosted scroll advance smoothly instead of in whole
// backMove lumps. See backgrnd.c and endlessScrollExtraPx() (endless.c).
extern int endlessScrollExtraPx1, endlessScrollExtraPx2, endlessScrollExtraPx3;

// TRUE per-tick vertical scroll (px) of each background layer [1..3], computed at draw
// time in backgrnd.c/tyrian2.c. Unlike a screen-position diff (which only sees the
// sub-tile remainder, mod 28), this carries the whole-tile component, so the render-list
// interpolation stays continuous when a speed modifier pushes the scroll past 28px/tick.
extern int bgScrollDeltaY[4];

// Bottom off-screen margin rows drawn by each background layer (default 1). Widened per
// tick in tyrian2.c while a scroll-speed modifier is active, so the larger interpolation
// up-shift can't uncover a black strip below the playfield. Kept STABLE across ticks
// (set from the modifier being active, not the fractional per-tick step count) so the
// recorded row count matches frame-to-frame and the layer doesn't snap.
extern int bgMarginRows;

// Un-floored parallax offsets (mainint.c) and, per layer, bg_layer_dx (FLOAT scroll delta) +
// bg_layer_frac (floored-away fraction). Interpolated sub-pixel-smooth, tick-locked to the
// layer's anchored enemies. notes.md §Sub-pixel parallax.
extern float mapXOfs_f, mapX2Ofs_f, mapX3Ofs_f;
extern float oldMapXOfs_f, oldMapX3Ofs_f;  // un-floored mirrors of oldMapXOfs / oldMapX3Ofs
extern float bg_layer_dx[4], bg_layer_frac[4];

// Vertical scroll smoothing: bg_layer_dy (FLOAT average scroll rate) + bg_layer_yfrac (sub-pixel
// remainder) per layer, gated by bg_smooth_y_active. notes.md §Slow-scroll smoothing.
extern float bg_layer_dy[4], bg_layer_yfrac[4];
extern bool bg_smooth_y_active;
// this-tick (non-lagged) scroll rate + sub-pixel fraction. Used by things recorded AFTER their
// scroll advance: scroll-tracked ENTITIES (enemies + HP bars), AND background LAYER 3, which
// (unlike layers 1/2) advances backPos3 before it records its rows (draw_background_3). The lagged
// bg_layer_dy/bg_layer_yfrac match the pre-advance layers 1/2; layer 3 needs these this-tick values
// or its rows drift one tick out of phase and jitter. See backgrnd.c / tyrian2.c.
extern float bg_layer_yfrac_now[4], bg_layer_dy_now[4];

extern JE_word mapX, mapY, mapX2, mapX3, mapY2, mapY3;
extern JE_byte **mapYPos, **mapY2Pos, **mapY3Pos;
extern JE_integer mapXPos, oldMapXOfs, mapXOfs, mapX2Ofs, mapX2Pos, mapX3Pos, oldMapX3Ofs, mapX3Ofs, tempMapXOfs;
extern intptr_t mapXbpPos, mapX2bpPos, mapX3bpPos;
extern JE_byte map1YDelay, map1YDelayMax, map2YDelay, map2YDelayMax;
extern JE_boolean anySmoothies;  // if yes, I want one :D
extern JE_byte smoothie_data[9];

extern int starfield_speed;

// When false, draw_background_* render at the current scroll position without
// advancing it (interpolated re-draws between ticks); the sim tick leaves it true.
extern bool background_advance;

void JE_darkenBackground(JE_word neat);

void blit_background_row(SDL_Surface *surface, int x, int y, Uint8 **map);
void blit_background_row_blend(SDL_Surface *surface, int x, int y, Uint8 **map);
// Supersampled variant (render-list replay only): x,y are HI-buffer coordinates;
// each tile pixel is drawn as a scale x scale block, fully clipped, never recorded.
void blit_background_row_scaled(SDL_Surface *surface, int x, int y, Uint8 **map, int scale, bool blend);

void draw_background_1(SDL_Surface *surface);
void draw_background_2(SDL_Surface *surface);
void draw_background_2_blend(SDL_Surface *surface);
void draw_background_3(SDL_Surface *surface);

void JE_filterScreen(JE_shortint col, JE_shortint generic_int);
void JE_filterScreenApply(SDL_Surface *surface, JE_shortint col, JE_shortint generic_int);
// JE_filterScreenApply on an NxN supersampled surface (playfield region x scale).
void filter_screen_apply_scaled(SDL_Surface *surface, JE_shortint col, JE_shortint generic_int, int scale);

void JE_checkSmoothies(void);
void lava_filter(SDL_Surface *dst, SDL_Surface *src);
void water_filter(SDL_Surface *dst, SDL_Surface *src);
void iced_blur_filter(SDL_Surface *dst, SDL_Surface *src);
void blur_filter(SDL_Surface *dst, SDL_Surface *src);
/*smoothies #5 is used for 3*/
/*smoothies #9 is a vertical flip*/

// Supersampled smoothie filters: identical pixel math on NxN buffers. The waver
// pattern and the feedback neighbour reads are kept at the ORIGINAL spatial scale
// (offsets multiplied by `scale`), so the plasma looks the same, just rendered on a
// 1/scale-pixel grid. All four stay contractive toward src, so the hi-res feedback
// is as stable as the 1x one.
void lava_filter_scaled(SDL_Surface *dst, SDL_Surface *src, int scale);
void water_filter_scaled(SDL_Surface *dst, SDL_Surface *src, int scale);
void iced_blur_filter_scaled(SDL_Surface *dst, SDL_Surface *src, int scale);
void blur_filter_scaled(SDL_Surface *dst, SDL_Surface *src, int scale);

void initialize_starfield(void);
void update_and_draw_starfield(SDL_Surface* surface, int move_speed);
void draw_starfield_star(SDL_Surface* surface, int x, int y, Uint8 color);
// Star at HI-buffer coordinates: centre + halo drawn as scale x scale blocks.
void draw_starfield_star_scaled(SDL_Surface* surface, int x, int y, Uint8 color, int scale);

#endif /* BACKGRND_H */
