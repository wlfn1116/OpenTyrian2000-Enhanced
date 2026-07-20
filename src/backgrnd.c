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
#include "backgrnd.h"

#include "config.h"
#include "mtrand.h"
#include "opentyr.h"
#include "render_list.h"
#include "varz.h"
#include "video.h"

#include <assert.h>

// Map tiles are 24x24px. Rows draw two extra tiles beyond the playfield width so
// coverage is guaranteed at any horizontal scroll: a single extra tile left an
// uncovered strip on the right edge (intermittent black bars) when the scroll
// was negative.
#define BG_TILE_W 24
#define BG_TILE_COUNT (PLAYFIELD_WIDTH / BG_TILE_W + 2)

// Mirrored Layers only: extra tiles appended past the right end of a row (see bg_edge_px).
#define BG_EDGE_TILES 1

/*Special Background 2 and Background 3*/

/*Back Pos 3*/
JE_word backPos, backPos2, backPos3;
JE_word backMove, backMove2, backMove3;

// Endless SMOOTH scroll boost: the extra scroll (px) applied to each background layer this
// tick, computed once per tick in tyrian2.c via endlessScrollExtraPx() (fractional carry, so
// the boosted scroll advances by a near-constant px/tick instead of whole `backMove` lumps ->
// no velocity pulse). Layers 2/3 read these in draw_background_* here; the enemy scroll-track
// and rep_explosions read them in tyrian2.c so they ride the same smooth delta. 0 when off.
int endlessScrollExtraPx1 = 0, endlessScrollExtraPx2 = 0, endlessScrollExtraPx3 = 0;

// See backgrnd.h: true during the sim tick (advance scroll as normal), false
// during interpolated re-draws so the layer can be re-rendered without moving.
bool background_advance = true;

// See backgrnd.h. bgScrollDeltaY holds the true per-tick vertical scroll of each layer;
// bgMarginRows widens the interpolation's bottom margin under a speed modifier.
int bgScrollDeltaY[4] = { 0, 0, 0, 0 };
int bgMarginRows = 1;

// See backgrnd.h. Un-floored parallax offsets captured at each layer's draw site:
// bg_layer_dx = this tick's FLOAT scroll delta, bg_layer_frac = the floored-away fraction.
float mapXOfs_f, mapX2Ofs_f, mapX3Ofs_f;
// Un-floored mirrors of oldMapXOfs / oldMapX3Ofs (the previous tick's offsets, reused as
// the parallax anchor for some enemy groups). Set beside their integer versions so
// enemies on those anchors get the matching sub-pixel fraction (see tyrian2.c blit_enemy).
float oldMapXOfs_f, oldMapX3Ofs_f;
float bg_layer_dx[4]   = { 0.0f, 0.0f, 0.0f, 0.0f };  // index 1..3
float bg_layer_frac[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
float bg_layer_xofs[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
bool  bg_layer_xofs_valid[4] = { false, false, false, false };
static float bg_layer_ofs_prev[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

// VERTICAL counterparts of bg_layer_dx/frac: bg_layer_dy (FLOAT scroll rate) + bg_layer_yfrac
// (sub-pixel remainder), gated by bg_smooth_y_active. See backgrnd.h.
float bg_layer_dy[4]    = { 0.0f, 0.0f, 0.0f, 0.0f };
float bg_layer_yfrac[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
bool  bg_smooth_y_active = false;

// this-tick (non-lagged) vertical scroll rate + sub-pixel fraction per layer [1..3]. bg_layer_dy/
// bg_layer_yfrac above are lagged one tick to match all entities and the BACKGROUND rows of layers
// 1/2 (recorded PRE-advance). Background LAYER 3 instead needs the current values because
// draw_background_3 advances backPos3 before drawing. Set in tyrian2.c beside the publish; 0 when
// no modifier and on full-speed integer-rate layers.
float bg_layer_yfrac_now[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
float bg_layer_dy_now[4]    = { 0.0f, 0.0f, 0.0f, 0.0f };

// Record layer L's float scroll delta and fractional offset for this tick's draw. `cur_f`
// is the un-floored offset, `cur_int` the whole-pixel one the row was recorded at. A large
// jump (level load / warp) snaps the delta to 0, like the render list's own >40px guard.
static void bg_set_layer_dx(int layer, float cur_f, int cur_int)
{
	float d = cur_f - bg_layer_ofs_prev[layer];
	if (d > 12.0f || d < -12.0f)
		d = 0.0f;
	bg_layer_dx[layer] = d;
	bg_layer_frac[layer] = cur_f - (float)cur_int;
	bg_layer_xofs[layer] = cur_f;
	bg_layer_xofs_valid[layer] = true;
	bg_layer_ofs_prev[layer] = cur_f;
}

// Previous draw-time (mapY, backPos) per layer, used to derive bgScrollDeltaY. The
// absolute downward scroll between two consecutive draws is
//   (mapY_prev - mapY_now) * 28 + (backPos_now - backPos_prev)
// which recovers the whole-tile motion (mapY steps) that a bare screen-position diff
// loses. Works regardless of whether a layer advances before or after it draws, because
// it always compares the state at successive record points. mapY/backPos are unsigned
// (JE_word); snapshot them as int. A level load / BKwrap makes the diff wild -> snapped.
static int  bgPrevMapY[4]    = { 0, 0, 0, 0 };
static int  bgPrevBackPos[4] = { 0, 0, 0, 0 };
static bool bgPrevValid[4]   = { false, false, false, false };

static void bg_update_scroll_delta(int layer, int mapY_now, int backPos_now)
{
	if (bgPrevValid[layer])
	{
		int d = (bgPrevMapY[layer] - mapY_now) * 28 + (backPos_now - bgPrevBackPos[layer]);
		if (d < 0 || d > 300)  // level reset / map wrap: not a real scroll step -> snap
			d = 0;
		bgScrollDeltaY[layer] = d;
	}
	else
	{
		bgScrollDeltaY[layer] = 0;
	}
	bgPrevMapY[layer] = mapY_now;
	bgPrevBackPos[layer] = backPos_now;
	bgPrevValid[layer] = true;
}

/*Main Maps*/
JE_word mapX, mapY, mapX2, mapX3, mapY2, mapY3;
JE_byte **mapYPos, **mapY2Pos, **mapY3Pos;
JE_integer mapXPos, oldMapXOfs, mapXOfs, mapX2Ofs, mapX2Pos, mapX3Pos, oldMapX3Ofs, mapX3Ofs, tempMapXOfs;
intptr_t mapXbpPos, mapX2bpPos, mapX3bpPos;
JE_byte map1YDelay, map1YDelayMax, map2YDelay, map2YDelayMax;

JE_boolean  anySmoothies;
JE_byte     smoothie_data[9]; /* [1..9] */

void JE_darkenBackground(JE_word neat)  /* wild detail level */
{
	Uint8 *s = VGAScreen->pixels; /* screen pointer, 8-bit specific */
	int x, y;
	
	s += PLAYFIELD_LEFT;
	
	for (y = 184; y; y--)
	{
		for (x = PLAYFIELD_WIDTH; x; x--)
		{
			*s = ((((*s & 0x0f) << 4) - (*s & 0x0f) + ((((x - neat - y) >> 2) + *(s - 2) + (y == 184 ? 0 : *(s - (VGAScreen->pitch - 1)))) & 0x0f)) >> 4) | (*s & 0xf0);
			s++;
		}
		s += VGAScreen->pitch - PLAYFIELD_WIDTH;
	}
}

// Extra Parallax widens the horizontal pan (mainint.c parallax_span) enough that the mid/deep
// layers' read window slides past the side edge of their map rows. Out-of-row columns used to
// wrap into the adjacent map row (a visible content seam where the layer "ends"); now they
// re-read the row's edge columns in reflected order and render horizontally FLIPPED, so the
// layer continues past its edge as a pixel-exact mirror image. Activated per row batch by
// bg_mirror_setup (mirror_w = row width in tiles, 0 = off; col0 = map-column index of map[0]).
// notes.md §Extra Parallax edge mirror.
static Uint8 *bg_mirror_tile(Uint8 **map, int tile, int mirror_w, int col0, bool *flip)
{
	*flip = false;
	if (mirror_w == 0)
		return map[tile];
	const int c = col0 + tile;
	if (c >= 0 && c < mirror_w)
		return map[tile];
	*flip = true;
	return (map - col0)[c < 0 ? -1 - c : 2 * mirror_w - 1 - c];
}

// Mirrored Layers only: how much extra row to append past the right end of the nominal
// BG_TILE_COUNT tiles, in 1x px (0 = none; up to BG_EDGE_TILES tiles, clipped to the surface).
// A row is exactly BG_TILE_COUNT*24 = 336px -- the width of the near map -- so at the far-right
// pan extreme it ends flush with PLAYFIELD_RIGHT and nothing covers the columns beyond it. The
// lava and water smoothie filters SAMPLE up to 7px to the right of the pixel they write, so at
// the screen's right edge they read that black fill; their per-scanline waver is a triangle wave,
// which turns the miss into the sawtooth "black triangles" seen on EP1 ASSASSIN / EP4 LAVA RUN.
// The filters also feed back through the row above/below, so black beyond the read range still
// bleeds in over frames -- hence run the fill all the way to the surface edge rather than just
// past +7. (The smooth-motion replay independently shifts a row up to a tick's pan further left,
// which uncovers columns that ARE displayed.) bg_mirror_tile already resolves an out-of-row column
// as a flipped edge column, so the appended strip is just more of the layer. Clipped so the row can
// never run past surface->w and wrap onto the next scanline. Inert with Mirrored Layers off, where
// out-of-row columns have no defined content (they wrap into the next map row).
// notes.md §Extra Parallax edge mirror.
static int bg_edge_px(int mirror_w, int x, int surface_w, int scale)
{
	if (mirror_w == 0)
		return 0;
	const int room = (surface_w - x - BG_TILE_COUNT * BG_TILE_W * scale) / scale;
	if (room <= 0)
		return 0;
	return room < BG_EDGE_TILES * BG_TILE_W ? room : BG_EDGE_TILES * BG_TILE_W;
}

void blit_background_row(SDL_Surface *surface, int x, int y, Uint8 **map, int mirror_w, int col0)
{
	assert(surface->format->BitsPerPixel == 8);

	if (render_list_recording)
		rl_rec_bg_row(x, y, map, false, mirror_w, col0);

	Uint8 *pixels = (Uint8 *)surface->pixels + (y * surface->pitch) + x,
	      *pixels_ll = (Uint8 *)surface->pixels,  // lower limit
	      *pixels_ul = (Uint8 *)surface->pixels + (surface->h * surface->pitch);  // upper limit

	const int edge_px = bg_edge_px(mirror_w, x, surface->w, 1);
	const int tile_count = BG_TILE_COUNT + (edge_px > 0 ? 1 : 0);
	const int row_width = BG_TILE_COUNT * BG_TILE_W + edge_px;
	for (int y = 0; y < 28; y++)
	{
		// not drawing on screen yet; skip y
		if ((pixels + row_width) < pixels_ll)
		{
			pixels += surface->pitch;
			continue;
		}

		for (int tile = 0; tile < tile_count; tile++)
		{
			// the appended edge tile (bg_edge_px) is clipped to the surface width
			const int tile_w = (tile < BG_TILE_COUNT) ? BG_TILE_W : edge_px;

			bool flip;
			Uint8* data = bg_mirror_tile(map, tile, mirror_w, col0, &flip);

			// no tile; skip tile
			if (data == NULL)
			{
				pixels += tile_w;
				continue;
			}

			data += y * 24 + (flip ? 23 : 0);
			const int step = flip ? -1 : 1;

			for (int x = tile_w; x; x--)
			{
				if (pixels >= pixels_ul)
					return;
				if (pixels >= pixels_ll && *data != 0)
					*pixels = *data;

				pixels++;
				data += step;
			}
		}

		pixels += surface->pitch - row_width;
	}
}

void blit_background_row_blend(SDL_Surface *surface, int x, int y, Uint8 **map, int mirror_w, int col0)
{
	assert(surface->format->BitsPerPixel == 8);

	if (render_list_recording)
		rl_rec_bg_row(x, y, map, true, mirror_w, col0);

	Uint8 *pixels = (Uint8 *)surface->pixels + (y * surface->pitch) + x,
	      *pixels_ll = (Uint8 *)surface->pixels,  // lower limit
	      *pixels_ul = (Uint8 *)surface->pixels + (surface->h * surface->pitch);  // upper limit

	const int edge_px = bg_edge_px(mirror_w, x, surface->w, 1);
	const int tile_count = BG_TILE_COUNT + (edge_px > 0 ? 1 : 0);
	const int row_width = BG_TILE_COUNT * BG_TILE_W + edge_px;
	for (int y = 0; y < 28; y++)
	{
		// not drawing on screen yet; skip y
		if ((pixels + row_width) < pixels_ll)
		{
			pixels += surface->pitch;
			continue;
		}

		for (int tile = 0; tile < tile_count; tile++)
		{
			// the appended edge tile (bg_edge_px) is clipped to the surface width
			const int tile_w = (tile < BG_TILE_COUNT) ? BG_TILE_W : edge_px;

			bool flip;
			Uint8* data = bg_mirror_tile(map, tile, mirror_w, col0, &flip);

			// no tile; skip tile
			if (data == NULL)
			{
				pixels += tile_w;
				continue;
			}

			data += y * 24 + (flip ? 23 : 0);
			const int step = flip ? -1 : 1;

			for (int x = tile_w; x; x--)
			{
				if (pixels >= pixels_ul)
					return;
				if (pixels >= pixels_ll && *data != 0)
					*pixels = (*data & 0xf0) | (((*pixels & 0x0f) + (*data & 0x0f)) / 2);

				pixels++;
				data += step;
			}
		}

		pixels += surface->pitch - row_width;
	}
}

// Supersampled tile row (render-list replay only; see backgrnd.h). One loop serves
// both the copy and blend ops — the pixel math matches the 1x blitters above.
void blit_background_row_scaled(SDL_Surface *surface, int x, int y, Uint8 **map, int scale, bool blend, int mirror_w, int col0)
{
	assert(surface->format->BitsPerPixel == 8);

	const int edge_px = bg_edge_px(mirror_w, x, surface->w, scale);
	const int tile_count = BG_TILE_COUNT + (edge_px > 0 ? 1 : 0);

	for (int ty = 0; ty < 28; ++ty)
	{
		const int hy = y + ty * scale;
		if (hy >= surface->h)
			return;  // rows only grow downward

		int by0 = hy < 0 ? 0 : hy;
		int by1 = hy + scale;
		if (by1 > surface->h)
			by1 = surface->h;
		if (by1 <= by0)
			continue;  // this tile row is fully above the top edge

		int hx = x;
		for (int tile = 0; tile < tile_count; ++tile)
		{
			// the appended edge tile (bg_edge_px) is clipped to the surface width
			const int tile_w = (tile < BG_TILE_COUNT) ? BG_TILE_W : edge_px;

			bool flip;
			const Uint8 *data = bg_mirror_tile(map, tile, mirror_w, col0, &flip);

			if (data == NULL)  // no tile; skip
			{
				hx += tile_w * scale;
				continue;
			}

			data += ty * BG_TILE_W + (flip ? BG_TILE_W - 1 : 0);
			const int dstep = flip ? -1 : 1;

			for (int sx = 0; sx < tile_w; ++sx, data += dstep, hx += scale)
			{
				const Uint8 d = *data;
				if (d == 0)
					continue;  // transparent

				int bx0 = hx < 0 ? 0 : hx;
				int bx1 = hx + scale;
				if (bx1 > surface->w)
					bx1 = surface->w;
				if (bx1 <= bx0)
					continue;

				for (int yy = by0; yy < by1; ++yy)
				{
					Uint8 *p = (Uint8 *)surface->pixels + yy * surface->pitch + bx0;
					if (!blend)
					{
						for (int xx = bx0; xx < bx1; ++xx)
							*p++ = d;
					}
					else
					{
						for (int xx = bx0; xx < bx1; ++xx, ++p)
							*p = (d & 0xf0) | (((*p & 0x0f) + (d & 0x0f)) / 2);
					}
				}
			}
		}
	}
}

// Prepare a layer's row walk. Mirrored Layers off: stock pointers -- with Extra Parallax on,
// clamp the read pointer to the map base (the old bg_clamp_map out-of-bounds guard: uncovered
// edges show the adjacent-row wrap / repeated row 0); with it off this is the original draw
// byte-for-byte (including its harmless 1-element edge read). Mirrored Layers on works in
// EITHER parallax mode -- even the stock span uncovers ~12px of layer 3's left edge at
// far-left: enable edge mirroring for the batch; the walk keeps its column phase even when
// `map` starts before `base`, because bg_mirror_tile resolves out-of-row columns from inside
// the row -- which is the OOB dereference the clamp guarded against. If the first ROW itself
// starts before the map (level-end re-points mapYPos at row 0), fall back to that clamp: draw
// from row 0, mirroring inert. The draw loops only advance the pointer downward, so checking
// the initial (lowest) row covers every row drawn this call.
static Uint8 **bg_mirror_setup(Uint8 **map, Uint8 **base, int width, int col0, int *out_w, int *out_c0)
{
	if (!mirroredLayers)
		return (extraParallax && map < base) ? base : map;
	if (map - col0 < base)
	{
		map = base;
		col0 = 0;
	}
	*out_w = width;
	*out_c0 = col0;
	return map;
}

void draw_background_1(SDL_Surface *surface)
{
	SDL_FillRect(surface, NULL, 0);

	const int tile_count = BG_TILE_COUNT;
	Uint8** map = (Uint8**)mapYPos + mapXbpPos - tile_count;
	int mirror_w = 0, col0 = 0;
	map = bg_mirror_setup(map, (Uint8**)&megaData1.mainmap[0][0], 14, (int)mapXbpPos - 1, &mirror_w, &col0);

	bg_update_scroll_delta(1, (int)mapY, (int)backPos);

	rl_current_id = RL_ID_BG_BASE + 1;  // tag rows for cross-frame interpolation
	bg_set_layer_dx(1, mapXOfs_f, mapXOfs);  // float delta + frac for smooth horizontal pan
	for (int i = -1; i < 7; i++)
	{
		blit_background_row(surface, mapXPos + PLAYFIELD_X_SHIFT, (i * 28) + backPos, map, mirror_w, col0);

		map += 14;
	}
	// Extra off-screen row(s) so the interpolation shift (up to a full tick's scroll) can't
	// uncover the bottom; reuses the last row's tiles (map - 14) to avoid reading past the map.
	// bgMarginRows grows under a speed modifier (scroll can exceed one tile per tick).
	for (int m = 0; m < bgMarginRows; ++m)
		blit_background_row(surface, mapXPos + PLAYFIELD_X_SHIFT, ((7 + m) * 28) + backPos, map - 14, mirror_w, col0);
	rl_current_id = 0;
}

void draw_background_2(SDL_Surface *surface)
{
	if (background_advance && map2YDelayMax > 1 && backMove2 < 2)
		backMove2 = (map2YDelay == 1) ? 1 : 0;
	const int tile_count = BG_TILE_COUNT;
	if (background2 != 0)
	{
		// water effect combines background 1 and 2 by synchronizing the x coordinate
		int x = (smoothies[1] ? mapXPos : mapX2Pos) + PLAYFIELD_X_SHIFT;

		Uint8** map = (Uint8**)mapY2Pos + (smoothies[1] ? mapXbpPos : mapX2bpPos) - tile_count;
		int mirror_w = 0, col0 = 0;
		map = bg_mirror_setup(map, (Uint8**)&megaData2.mainmap[0][0], 14,
		                      (int)(smoothies[1] ? mapXbpPos : mapX2bpPos) - 1, &mirror_w, &col0);

		bg_update_scroll_delta(2, (int)mapY2, (int)backPos2);

		rl_current_id = RL_ID_BG_BASE + 2;
		// smoothies[1] syncs layer 2 to layer 1's X phase (see x above), so use that offset.
		bg_set_layer_dx(2, smoothies[1] ? mapXOfs_f : mapX2Ofs_f, smoothies[1] ? mapXOfs : mapX2Ofs);
		for (int i = -1; i < 7; i++)
		{
			blit_background_row(surface, x, (i * 28) + backPos2, map, mirror_w, col0);

			map += 14;
		}
		// Extra bottom margin row(s) for the interpolation (see draw_background_1).
		for (int m = 0; m < bgMarginRows; ++m)
			blit_background_row(surface, x, ((7 + m) * 28) + backPos2, map - 14, mirror_w, col0);
		rl_current_id = 0;
	}

	/*Set Movement of background*/
	if (background_advance && --map2YDelay == 0)
	{
		map2YDelay = map2YDelayMax;
		
		backPos2 += backMove2;
		
		if (backPos2 >  27)
		{
			backPos2 -= 28;
			mapY2--;
			mapY2Pos -= 14;  /*Map Width*/
		}
	}

	// Endless SMOOTH overclock: advance layer 2 by its fractional-carried extra px (computed once
	// per tick in tyrian2.c), matching layer 1/event pace with the parallax ratio preserved but no
	// velocity pulse. Ungated by the Y-delay to mirror layer 1; the wrap can cross several tiles.
	if (background_advance)
	{
		backPos2 += endlessScrollExtraPx2;
		while (backPos2 > 27)
		{
			backPos2 -= 28;
			mapY2--;
			mapY2Pos -= 14;
		}
	}
}

void draw_background_2_blend(SDL_Surface *surface)
{
	if (background_advance && map2YDelayMax > 1 && backMove2 < 2)
		backMove2 = (map2YDelay == 1) ? 1 : 0;
	
	const int tile_count = BG_TILE_COUNT;
	Uint8** map = (Uint8**)mapY2Pos + mapX2bpPos - tile_count;
	int mirror_w = 0, col0 = 0;
	map = bg_mirror_setup(map, (Uint8**)&megaData2.mainmap[0][0], 14, (int)mapX2bpPos - 1, &mirror_w, &col0);

	bg_update_scroll_delta(2, (int)mapY2, (int)backPos2);

	rl_current_id = RL_ID_BG_BASE + 2;
	bg_set_layer_dx(2, mapX2Ofs_f, mapX2Ofs);  // blend variant always draws at mapX2Pos
	for (int i = -1; i < 7; i++)
	{
		blit_background_row_blend(surface, mapX2Pos + PLAYFIELD_X_SHIFT, (i * 28) + backPos2, map, mirror_w, col0);

		map += 14;
	}
	// Extra bottom margin row(s) for the interpolation (see draw_background_1).
	for (int m = 0; m < bgMarginRows; ++m)
		blit_background_row_blend(surface, mapX2Pos + PLAYFIELD_X_SHIFT, ((7 + m) * 28) + backPos2, map - 14, mirror_w, col0);
	rl_current_id = 0;
	
	/*Set Movement of background*/
	if (background_advance && --map2YDelay == 0)
	{
		map2YDelay = map2YDelayMax;
		
		backPos2 += backMove2;
		
		if (backPos2 >  27)
		{
			backPos2 -= 28;
			mapY2--;
			mapY2Pos -= 14;  /*Map Width*/
		}
	}

	// Endless SMOOTH overclock: extra px to match layer 1 and the event pointer (see draw_background_2).
	if (background_advance)
	{
		backPos2 += endlessScrollExtraPx2;
		while (backPos2 > 27)
		{
			backPos2 -= 28;
			mapY2--;
			mapY2Pos -= 14;
		}
	}
}

void draw_background_3(SDL_Surface *surface)
{
	/* Movement of background */
	if (background_advance)
	{
		backPos3 += backMove3;

		if (backPos3 > 27)
		{
			backPos3 -= 28;
			mapY3--;
			mapY3Pos -= 15;   /*Map Width*/
		}

		// Endless SMOOTH overclock: extra px to match layer 1/2 and the event pointer (see above).
		backPos3 += endlessScrollExtraPx3;
		while (backPos3 > 27)
		{
			backPos3 -= 28;
			mapY3--;
			mapY3Pos -= 15;
		}
	}

	const int tile_count = BG_TILE_COUNT;
	Uint8** map = (Uint8**)mapY3Pos + mapX3bpPos - tile_count;
	// Layer 3 rows are 15 tiles wide and mapY3Pos already carries the -1 pointer bias,
	// so map[0] sits at column mapX3bpPos (no -1 like the 14-wide layers).
	int mirror_w = 0, col0 = 0;
	map = bg_mirror_setup(map, (Uint8**)&megaData3.mainmap[0][0], 15, (int)mapX3bpPos, &mirror_w, &col0);

	bg_update_scroll_delta(3, (int)mapY3, (int)backPos3);

	rl_current_id = RL_ID_BG_BASE + 3;
	// background3x1 welds this layer to layer 1 (mapX3Ofs = mapXOfs), but the two record on opposite
	// sides of the mid-tick parallax update, sampling that shared anchor a tick apart. Pan from the
	// one layer 1 recorded; the integer stays mapX3Ofs, which is what the rows below blit at.
	// notes.md §Sub-pixel parallax.
	const float x_anchor_f = (background3x1 && bg_layer_xofs_valid[1]) ? bg_layer_xofs[1] : mapX3Ofs_f;
	bg_set_layer_dx(3, x_anchor_f, mapX3Ofs);
	for (int i = -1; i < 7; i++)
	{
		// Layer 3 shares PLAYFIELD_X_SHIFT with layers 1/2; no per-layer correction.
		blit_background_row(surface, mapX3Pos + PLAYFIELD_X_SHIFT, (i * 28) + backPos3, map, mirror_w, col0);

		map += 15;
	}
	// Extra bottom margin row(s) for the interpolation (see draw_background_1; bg3 rows
	// are 15 map entries wide).
	for (int m = 0; m < bgMarginRows; ++m)
		blit_background_row(surface, mapX3Pos + PLAYFIELD_X_SHIFT, ((7 + m) * 28) + backPos3, map - 15, mirror_w, col0);
	rl_current_id = 0;
}

// Pixel-only body of JE_filterScreen: `col` recolours each playfield pixel's palette
// bank (high nibble), `int_` adjusts brightness (low nibble), -99 skips a component.
// No fade side effects, so the render list can replay it on interpolated frames.
void JE_filterScreenApply(SDL_Surface *surface, JE_shortint col, JE_shortint int_)
{
	Uint8 *s = NULL; /* screen pointer, 8-bit specific */
	int x, y;
	unsigned int temp;

	if (col != -99 && filtrationAvail)
	{
		s = surface->pixels;
		s += PLAYFIELD_LEFT;

		col <<= 4;

		for (y = 184; y; y--)
		{
			for (x = PLAYFIELD_WIDTH; x; x--)
			{
				*s = col | (*s & 0x0f);
				s++;
			}
			s += surface->pitch - PLAYFIELD_WIDTH;
		}
	}

	if (int_ != -99 && explosionTransparent)
	{
		s = surface->pixels;
		s += PLAYFIELD_LEFT;

		for (y = 184; y; y--)
		{
			for (x = PLAYFIELD_WIDTH; x; x--)
			{
				temp = (*s & 0x0f) + int_;
				*s = (*s & 0xf0) | (temp >= 0x1f ? 0 : (temp >= 0x0f ? 0x0f : temp));
				s++;
			}
			s += surface->pitch - PLAYFIELD_WIDTH;
		}
	}
}

// JE_filterScreenApply for an NxN supersampled surface: identical passes over the
// scaled playfield region.
void filter_screen_apply_scaled(SDL_Surface *surface, JE_shortint col, JE_shortint int_, int scale)
{
	const int left = PLAYFIELD_LEFT * scale;
	const int width = PLAYFIELD_WIDTH * scale;
	const int rows = 184 * scale;
	Uint8 *s = NULL;
	int x, y;
	unsigned int temp;

	if (col != -99 && filtrationAvail)
	{
		s = (Uint8 *)surface->pixels + left;

		col <<= 4;

		for (y = rows; y; y--)
		{
			for (x = width; x; x--)
			{
				*s = col | (*s & 0x0f);
				s++;
			}
			s += surface->pitch - width;
		}
	}

	if (int_ != -99 && explosionTransparent)
	{
		s = (Uint8 *)surface->pixels + left;

		for (y = rows; y; y--)
		{
			for (x = width; x; x--)
			{
				temp = (*s & 0x0f) + int_;
				*s = (*s & 0xf0) | (temp >= 0x1f ? 0 : (temp >= 0x0f ? 0x0f : temp));
				s++;
			}
			s += surface->pitch - width;
		}
	}
}

void JE_filterScreen(JE_shortint col, JE_shortint int_)
{
	// Advance the fade animation exactly once per sim tick; must not run on the
	// render list's replay path (see JE_filterScreenApply).
	if (filterFade)
	{
		levelBrightness += levelBrightnessChg;
		if ((filterFadeStart && levelBrightness < -14) || levelBrightness > 14)
		{
			levelBrightnessChg = -levelBrightnessChg;
			filterFadeStart = false;
			levelFilter = levelFilterNew;
		}
		if (!filterFadeStart && levelBrightness == 0)
		{
			filterFade = false;
			levelBrightness = -99;
		}
	}

	JE_filterScreenApply(VGAScreen, col, int_);
}

void JE_checkSmoothies(void)
{
	anySmoothies = (processorType > 2 && (smoothies[1-1] || smoothies[2-1])) || (processorType > 1 && (smoothies[3-1] || smoothies[4-1] || smoothies[5-1]));
}

void lava_filter(SDL_Surface *dst, SDL_Surface *src)
{
	assert(src->format->BitsPerPixel == 8 && dst->format->BitsPerPixel == 8);
	
	/* we don't need to check for over-reading the pixel surfaces since we only
		 * read from the top 185+1 scanlines, and the playfield width is vga_width */
	
	const int dst_pitch = dst->pitch;
	Uint8 *dst_pixel = (Uint8 *)dst->pixels + (185 * dst_pitch);
	const Uint8 * const dst_pixel_ll = (Uint8 *)dst->pixels;  // lower limit
	
	const int src_pitch = src->pitch;
	const Uint8 *src_pixel = (Uint8 *)src->pixels + (185 * src->pitch);
	const Uint8 * const src_pixel_ll = (Uint8 *)src->pixels;  // lower limit
	
	int w = vga_width * 185 - 1;
	
	for (int y = 185 - 1; y >= 0; --y)
	{
		dst_pixel -= (dst_pitch - vga_width);  // in case pitch differs
		src_pixel -= (src_pitch - vga_width);  // in case pitch differs

		for (int x = vga_width; x > 0; )
		{
			int waver = abs(((w >> 9) & 0x0f) - 8) - 1;
			w -= 8;

			int count = MIN(8, x);
			x -= count;

			for (int xi = 0; xi < count; ++xi)
			{
				--dst_pixel;
				--src_pixel;

				// value is average value of source pixel (2x), destination pixel above, and destination pixel below (all with waver)
				// hue is red
				Uint8 value = 0;

				if (src_pixel + waver >= src_pixel_ll)
					value += (*(src_pixel + waver) & 0x0f) * 2;
				value += *(dst_pixel + waver + dst_pitch) & 0x0f;
				if (dst_pixel + waver - dst_pitch >= dst_pixel_ll)
					value += *(dst_pixel + waver - dst_pitch) & 0x0f;

				*dst_pixel = (value / 4) | 0x70;
			}
		}
	}
}

void water_filter(SDL_Surface *dst, SDL_Surface *src)
{
	assert(src->format->BitsPerPixel == 8 && dst->format->BitsPerPixel == 8);
	
	Uint8 hue = smoothie_data[1] << 4;
	
	/* we don't need to check for over-reading the pixel surfaces since we only
		 * read from the top 185+1 scanlines, and the playfield width is vga_width */
	
	const int dst_pitch = dst->pitch;
	Uint8 *dst_pixel = (Uint8 *)dst->pixels + (185 * dst_pitch);
	
	const Uint8 *src_pixel = (Uint8 *)src->pixels + (185 * src->pitch);
	
	int w = vga_width * 185 - 1;
	
	for (int y = 185 - 1; y >= 0; --y)
	{
		dst_pixel -= (dst_pitch - vga_width);  // in case pitch differs
		src_pixel -= (src->pitch - vga_width);  // in case pitch differs

		for (int x = vga_width; x > 0; )
		{
			int waver = abs(((w >> 10) & 0x07) - 4) - 1;
			w -= 8;

			int count = MIN(8, x);
			x -= count;

			for (int xi = 0; xi < count; ++xi)
			{
				--dst_pixel;
				--src_pixel;

				// pixel is copied from source if not blue
				// otherwise, value is average of value of source pixel and destination pixel below (with waver)
				if ((*src_pixel & 0x30) == 0)
				{
					*dst_pixel = *src_pixel;
				}
				else
				{
					Uint8 value = *src_pixel & 0x0f;
					value += *(dst_pixel + waver + dst_pitch) & 0x0f;
					*dst_pixel = (value / 2) | hue;
				}
			}
		}
	}
}

void iced_blur_filter(SDL_Surface *dst, SDL_Surface *src)
{
	assert(src->format->BitsPerPixel == 8 && dst->format->BitsPerPixel == 8);
	
	Uint8 *dst_pixel = dst->pixels;
	const Uint8 *src_pixel = src->pixels;
	
	for (int y = 0; y < 184; ++y)
	{
		for (int x = 0; x < vga_width; ++x)
		{
			// value is average value of source pixel and destination pixel
			// hue is icy blue
			
			const Uint8 value = (*src_pixel & 0x0f) + (*dst_pixel & 0x0f);
			*dst_pixel = (value / 2) | 0x80;
			
			++dst_pixel;
			++src_pixel;
		}
		
		dst_pixel += (dst->pitch - vga_width);  // in case pitch differs
		src_pixel += (src->pitch - vga_width);  // in case pitch differs
	}
}

void blur_filter(SDL_Surface *dst, SDL_Surface *src)
{
	assert(src->format->BitsPerPixel == 8 && dst->format->BitsPerPixel == 8);
	
	Uint8 *dst_pixel = dst->pixels;
	const Uint8 *src_pixel = src->pixels;
	
	for (int y = 0; y < 184; ++y)
	{
		for (int x = 0; x < vga_width; ++x)
		{
			// value is average value of source pixel and destination pixel
			// hue is source pixel hue
			
			const Uint8 value = (*src_pixel & 0x0f) + (*dst_pixel & 0x0f);
			*dst_pixel = (value / 2) | (*src_pixel & 0xf0);
			
			++dst_pixel;
			++src_pixel;
		}
		
		dst_pixel += (dst->pitch - vga_width);  // in case pitch differs
		src_pixel += (src->pitch - vga_width);  // in case pitch differs
	}
}

/*
 * Supersampled smoothie filters — same pixel math as the 1x filters on an NxN
 * buffer; see backgrnd.h for the spatial-scale and stability notes. The 1x lava
 * and water filters scan bottom-up, so the "row below" read sees this frame's
 * value while the "row above" read sees the previous frame's — the scaled
 * versions keep that scan order (and therefore those dynamics) exactly.
 */
void lava_filter_scaled(SDL_Surface *dst, SDL_Surface *src, int scale)
{
	assert(src->format->BitsPerPixel == 8 && dst->format->BitsPerPixel == 8);

	const int W = vga_width * scale;
	const int H = 185 * scale;
	const int dst_pitch = dst->pitch;
	const int src_pitch = src->pitch;
	const int row_step = dst_pitch * scale;  // one 1x row's distance in the hi buffer
	Uint8 *const dst_px = (Uint8 *)dst->pixels;
	const Uint8 *const src_px = (const Uint8 *)src->pixels;

	for (int y = H - 1; y >= 0; --y)
	{
		Uint8 *const dp = dst_px + y * dst_pitch;
		const Uint8 *const sp = src_px + y * src_pitch;
		const int row1 = (y / scale) * vga_width;  // 1x linear index of this row

		for (int x = W - 1; x >= 0; --x)
		{
			// Waver from the 1x linear index, so the wobble pattern has the same
			// spatial frequency as the original.
			const int w1 = row1 + x / scale;
			const int waver = (abs(((w1 >> 9) & 0x0f) - 8) - 1) * scale;

			int xs = x + waver;
			if (xs < 0)
				xs = 0;
			else if (xs >= W)
				xs = W - 1;

			// Average of source (2x), the current frame's row below, and the
			// previous frame's row above (all wavered); hue forced red.
			int value = (sp[xs] & 0x0f) * 2;
			value += dp[xs + row_step] & 0x0f;
			if (y - scale >= 0)
				value += dp[xs - row_step] & 0x0f;

			dp[x] = (Uint8)((value / 4) | 0x70);
		}
	}
}

void water_filter_scaled(SDL_Surface *dst, SDL_Surface *src, int scale)
{
	assert(src->format->BitsPerPixel == 8 && dst->format->BitsPerPixel == 8);

	const Uint8 hue = smoothie_data[1] << 4;

	const int W = vga_width * scale;
	const int H = 185 * scale;
	const int dst_pitch = dst->pitch;
	const int src_pitch = src->pitch;
	const int row_step = dst_pitch * scale;  // one 1x row's distance in the hi buffer
	Uint8 *const dst_px = (Uint8 *)dst->pixels;
	const Uint8 *const src_px = (const Uint8 *)src->pixels;

	for (int y = H - 1; y >= 0; --y)
	{
		Uint8 *const dp = dst_px + y * dst_pitch;
		const Uint8 *const sp = src_px + y * src_pitch;
		const int row1 = (y / scale) * vga_width;

		for (int x = W - 1; x >= 0; --x)
		{
			// Pixel is copied from source if not blue; otherwise averaged with the
			// current frame's row below (wavered), recoloured to the level's hue.
			if ((sp[x] & 0x30) == 0)
			{
				dp[x] = sp[x];
			}
			else
			{
				const int w1 = row1 + x / scale;
				const int waver = (abs(((w1 >> 10) & 0x07) - 4) - 1) * scale;

				int xs = x + waver;
				if (xs < 0)
					xs = 0;
				else if (xs >= W)
					xs = W - 1;

				Uint8 value = sp[x] & 0x0f;
				value += dp[xs + row_step] & 0x0f;
				dp[x] = (Uint8)((value / 2) | hue);
			}
		}
	}
}

void iced_blur_filter_scaled(SDL_Surface *dst, SDL_Surface *src, int scale)
{
	assert(src->format->BitsPerPixel == 8 && dst->format->BitsPerPixel == 8);

	const int W = vga_width * scale;
	const int H = 184 * scale;

	Uint8 *dst_pixel = dst->pixels;
	const Uint8 *src_pixel = src->pixels;

	for (int y = 0; y < H; ++y)
	{
		for (int x = 0; x < W; ++x)
		{
			// Average of source and previous-frame destination; hue icy blue.
			const Uint8 value = (*src_pixel & 0x0f) + (*dst_pixel & 0x0f);
			*dst_pixel = (value / 2) | 0x80;

			++dst_pixel;
			++src_pixel;
		}

		dst_pixel += (dst->pitch - W);
		src_pixel += (src->pitch - W);
	}
}

void blur_filter_scaled(SDL_Surface *dst, SDL_Surface *src, int scale)
{
	assert(src->format->BitsPerPixel == 8 && dst->format->BitsPerPixel == 8);

	const int W = vga_width * scale;
	const int H = 184 * scale;

	Uint8 *dst_pixel = dst->pixels;
	const Uint8 *src_pixel = src->pixels;

	for (int y = 0; y < H; ++y)
	{
		for (int x = 0; x < W; ++x)
		{
			// Average of source and previous-frame destination; source hue kept.
			const Uint8 value = (*src_pixel & 0x0f) + (*dst_pixel & 0x0f);
			*dst_pixel = (value / 2) | (*src_pixel & 0xf0);

			++dst_pixel;
			++src_pixel;
		}

		dst_pixel += (dst->pitch - W);
		src_pixel += (src->pitch - W);
	}
}

/* Background Starfield. Each star is an (x column, float y row) point; only y is
 * advanced/interpolated — x stays fixed — so stars can never smear sideways. */
typedef struct
{
	int x;        // column (constant for a star's lifetime)
	float y;      // row (fractional, advances each tick, wraps at STARFIELD_WRAP)
	int speed;    // base rows of drift (scaled by STARFIELD_SPEED_SCALE)
	Uint8 color;
} StarfieldStar;

#define MAX_STARS 330  // sized so on-screen density stays constant with stars respawning above the top edge
#define STARFIELD_HUE 0x90
#define STARFIELD_WRAP    184  // rows; star recycles once it drifts past this (the playfield bottom edge)
#define STARFIELD_VISIBLE 184  // rows; stars draw above this. Matches the 184-row playfield so they
                               // fill it to the bottom (a shorter value leaves a black stripe on space levels).

// Recycled stars respawn in a band just ABOVE the visible top edge (negative rows)
// so they scroll smoothly into view instead of popping in at row 0. Respawn row is
// -(MIN + a rotating offset in [0, SPREAD)).
#define STARFIELD_SPAWN_MIN    4
#define STARFIELD_SPAWN_SPREAD 32

// rows/tick per unit of (speed + move_speed); 1.0 == the original game's speed.
#define STARFIELD_SPEED_SCALE 1.0f
static StarfieldStar starfield_stars[MAX_STARS];
int starfield_speed;
// Rotates the above-screen respawn height so consecutive recycles stagger across the
// spawn band instead of clustering at one height. Deterministic / RNG-free -- the
// per-tick starfield must never touch mt_rand (that would perturb the gameplay RNG
// stream the demos depend on).
static int starfield_spawn_phase;

void initialize_starfield(void)
{
	starfield_spawn_phase = 0;
	for (int i = MAX_STARS - 1; i >= 0; --i)
	{
		starfield_stars[i].x = mt_rand() % vga_width;
		starfield_stars[i].y = (float)(mt_rand() % STARFIELD_WRAP);
		starfield_stars[i].speed = mt_rand() % 3 + 2;
		starfield_stars[i].color = mt_rand() % 16 + STARFIELD_HUE;
	}
}

// Draw one star (center pixel plus dimmer halo); factored out for render-list replay.
// Bounds are checked per axis so halo pixels can't wrap into the neighbouring row.
void draw_starfield_star(SDL_Surface* surface, int x, int y, Uint8 color)
{
	if (x < 0 || x >= surface->w || y < 0 || y >= STARFIELD_VISIBLE)
		return;

	Uint8* p = (Uint8*)surface->pixels;
	const int pitch = surface->pitch;
	const int pos = y * pitch + x;

	if (p[pos] == 0)
		p[pos] = color;

	// If star is bright enough, draw surrounding pixels
	if (color - 4 >= STARFIELD_HUE)
	{
		if (x + 1 < surface->w && p[pos + 1] == 0)
			p[pos + 1] = color - 4;
		if (x - 1 >= 0 && p[pos - 1] == 0)
			p[pos - 1] = color - 4;
		if (y + 1 < surface->h && p[pos + pitch] == 0)
			p[pos + pitch] = color - 4;
		if (y - 1 >= 0 && p[pos - pitch] == 0)
			p[pos - pitch] = color - 4;
	}
}

// One scale x scale block of star light: written only over black pixels, like the
// per-pixel check in draw_starfield_star.
static void star_block(SDL_Surface *surface, int x, int y, Uint8 color, int scale)
{
	int x0 = x < 0 ? 0 : x;
	int y0 = y < 0 ? 0 : y;
	int x1 = x + scale, y1 = y + scale;
	if (x1 > surface->w)
		x1 = surface->w;
	if (y1 > surface->h)
		y1 = surface->h;

	for (int yy = y0; yy < y1; ++yy)
	{
		Uint8 *p = (Uint8 *)surface->pixels + yy * surface->pitch + x0;
		for (int xx = x0; xx < x1; ++xx, ++p)
		{
			if (*p == 0)
				*p = color;
		}
	}
}

void draw_starfield_star_scaled(SDL_Surface* surface, int x, int y, Uint8 color, int scale)
{
	// Same visibility rule as the 1x draw, in HI coordinates. The interpolated row
	// lands on the 1/scale-pixel grid, which is the whole point: slow stars glide.
	if (x < 0 || x >= surface->w || y < 0 || y >= STARFIELD_VISIBLE * scale)
		return;

	star_block(surface, x, y, color, scale);

	// If star is bright enough, draw the surrounding halo blocks
	if (color - 4 >= STARFIELD_HUE)
	{
		star_block(surface, x + scale, y, color - 4, scale);
		star_block(surface, x - scale, y, color - 4, scale);
		star_block(surface, x, y + scale, color - 4, scale);
		star_block(surface, x, y - scale, color - 4, scale);
	}
}

void update_and_draw_starfield(SDL_Surface* surface, int move_speed)
{
	for (int i = MAX_STARS-1; i >= 0; --i)
	{
		StarfieldStar* star = &starfield_stars[i];

		// Drift down by a (usually sub-pixel) amount; only y moves.
		const float dy = (star->speed + move_speed) * STARFIELD_SPEED_SCALE;
		star->y += dy;

		// Record this tick's row motion for interpolation; on a wrap pass 0 so the
		// replay snaps to the new position instead of streaking across the screen.
		float rec_dy = dy;
		if (star->y >= STARFIELD_WRAP)
		{
			// Respawn a little ABOVE the visible top edge (negative row) rather than at
			// row 0, so the star drifts smoothly into view instead of popping in and
			// holding there for the wrap tick. It stays invisible (y < 0) through the
			// snap and only crosses the top edge on a normal moving tick, so the
			// interpolator slides it in mid-motion -- no "frozen at the top" blip.
			star->y = -(float)(STARFIELD_SPAWN_MIN + (starfield_spawn_phase % STARFIELD_SPAWN_SPREAD));
			starfield_spawn_phase += 13;  // step coprime with SPREAD -> even coverage of the band
			rec_dy = 0.0f;                // snap on the wrap; the star is off-screen so nothing streaks
		}

		draw_starfield_star(surface, star->x, (int)(star->y + 0.5f), star->color);

		if (render_list_recording)
			rl_rec_star(star->x, star->y, rec_dy, star->color);
	}
}
