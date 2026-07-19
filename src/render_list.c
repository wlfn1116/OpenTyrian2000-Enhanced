/*
 * OpenTyrian 2000 Engaged: render-list capture & replay (see render_list.h).
 */
#include "render_list.h"

#include "backgrnd.h"
#include "sprite.h"
#include "vga256d.h"
#include "video.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

bool render_list_recording = false;
int rl_current_id = 0;
int rl_shot_attach = 0;
float rl_current_par_frac = 0.0f;
int rl_current_par_layer = 0;
float rl_current_par_anchor = 0.0f;
float rl_current_par_yfrac = 0.0f;
int rl_current_par_ybase = 0;
int rl_current_par_ylayer = 0;
int rl_current_vel_x = 0, rl_current_vel_y = 0;
int rl_current_acc_x = 0, rl_current_acc_y = 0;

// Forward decl: rl_finalize must preserve extrapolating ids' recorded dx/dy.
static bool rl_id_extrapolates(int id);

// Double-buffered command lists: one for the current tick, one for the previous
// (used to derive per-command motion for interpolation).
static RenderCmd *bufs[2] = { NULL, NULL };
static size_t counts[2] = { 0, 0 };
static size_t caps[2] = { 0, 0 };
static int cur_buf = 0;

static RenderCmd *rl_push(void)
{
	size_t *cap = &caps[cur_buf];
	if (counts[cur_buf] == *cap)
	{
		size_t ncap = *cap ? *cap * 2 : 4096;
		RenderCmd *n = realloc(bufs[cur_buf], ncap * sizeof(*n));
		if (n == NULL)
			return NULL;  // out of memory: drop this command
		bufs[cur_buf] = n;
		*cap = ncap;
	}
	RenderCmd *c = &bufs[cur_buf][counts[cur_buf]++];
	c->id = rl_current_id;
	c->ship_attach = (Uint8)rl_shot_attach;
	// Parallax sub-pixel fraction (enemies); finalize fills par_frac_dx from the prev match.
	c->par_frac = rl_current_par_frac;
	c->par_frac_dx = 0.0f;
	c->par_layer = (Uint8)rl_current_par_layer;
	c->par_anchor = rl_current_par_anchor;
	// Vertical background binding; finalize fills only the entity-local displacement.
	c->par_ybase = rl_current_par_ybase;
	c->par_yfrac = rl_current_par_yfrac;
	c->par_yown100 = 0;
	c->par_ylayer = (Uint8)rl_current_par_ylayer;
	// Seed dx/dy from the recorded velocity: rl_finalize keeps it for extrapolating
	// ids (shots) and overwrites it with the prev/cur diff for the rest.
	c->dx = rl_current_vel_x;
	c->dy = rl_current_vel_y;
	// Acceleration (shots only); finalize never touches it, so it survives for the
	// extrapolating ids that read it and stays 0 (unused) for everything else.
	c->acc_x = rl_current_acc_x;
	c->acc_y = rl_current_acc_y;
	// On smoothie levels the playfield draw ping-pongs between game_screen and
	// VGAScreen2; capture which buffer this draw targeted so replay can route it.
	c->surface = (VGAScreen == VGAScreen2) ? 1 : 0;
	return c;
}

void rl_begin_record(void)
{
	cur_buf ^= 1;             // previous current becomes prev; record into the other
	counts[cur_buf] = 0;
	rl_current_id = 0;
	rl_current_par_frac = 0.0f;
	rl_current_par_layer = 0;
	rl_current_par_anchor = 0.0f;
	rl_current_par_yfrac = 0.0f;
	rl_current_par_ybase = 0;
	rl_current_par_ylayer = 0;
	rl_current_vel_x = 0;
	rl_current_vel_y = 0;
	rl_current_acc_x = 0;
	rl_current_acc_y = 0;
	for (int layer = 1; layer <= 3; ++layer)
		bg_layer_xofs_valid[layer] = false;
	render_list_recording = true;
}

void rl_end_record(void)
{
	render_list_recording = false;
}

size_t rl_count(void)
{
	return counts[cur_buf];
}

void rl_rec_sprite2(int x, int y, Sprite2_array sheet, unsigned int index, RenderCmdKind kind)
{
	RenderCmd *c = rl_push();
	if (c == NULL)
		return;
	c->kind = kind;
	c->x = x;
	c->y = y;
	c->sheet = sheet;
	c->index = index;
}

void rl_rec_sprite2_filter(int x, int y, Sprite2_array sheet, unsigned int index, Uint8 filter, bool clip)
{
	RenderCmd *c = rl_push();
	if (c == NULL)
		return;
	c->kind = clip ? RC_SPRITE2_FILTER_CLIP : RC_SPRITE2_FILTER;
	c->x = x;
	c->y = y;
	c->sheet = sheet;
	c->index = index;
	c->filter = filter;
}

void rl_rec_sprite(int x, int y, unsigned int table, unsigned int index, RenderCmdKind kind, Uint8 hue, Sint8 value, bool black)
{
	RenderCmd *c = rl_push();
	if (c == NULL)
		return;
	c->kind = kind;
	c->x = x;
	c->y = y;
	c->table = table;
	c->index = index;
	c->hue = hue;
	c->value = value;
	c->black = black;
}

void rl_rec_bg_row(int x, int y, Uint8 **map, bool blend)
{
	RenderCmd *c = rl_push();
	if (c == NULL)
		return;
	c->kind = blend ? RC_BG_ROW_BLEND : RC_BG_ROW;
	c->x = x;
	c->y = y;
	c->map = map;
}

void rl_rec_star(int x, float y, float dy, Uint8 color)
{
	RenderCmd *c = rl_push();
	if (c == NULL)
		return;
	c->kind = RC_STAR;
	c->star_x = x;
	c->star_y = y;
	c->star_dy = dy;
	c->star_color = color;
}

void rl_rec_superpixel(int x, int y, int dx, int dy, Uint8 z, Uint8 color)
{
	RenderCmd *c = rl_push();
	if (c == NULL)
		return;
	c->kind = RC_SUPERPIXEL;
	c->x = x;
	c->y = y;
	c->sp_dx = dx;
	c->sp_dy = dy;
	c->sp_z = z;
	c->sp_color = color;
}

void rl_rec_hp_bar(int x, int y, int along, int fill, Uint8 col, bool vertical, Uint8 opacity)
{
	RenderCmd *c = rl_push();
	if (c == NULL)
		return;
	c->kind = RC_HP_BAR;
	c->x = x;
	c->y = y;
	c->bar_w = along;
	c->bar_fill = fill;
	c->bar_col = col;
	c->bar_vertical = vertical ? 1 : 0;
	c->bar_opacity = opacity;
}

void rl_rec_filter_screen(int col, int brightness)
{
	RenderCmd *c = rl_push();
	if (c == NULL)
		return;
	c->kind = RC_FILTER_SCREEN;
	c->id = RL_ID_FILTER;   // one per tick; lets rl_finalize match it for brightness interpolation
	c->x = 0;
	c->y = 0;
	c->filt_col = col;
	c->filt_bright = brightness;
	c->filt_dbright = 0;
}

void rl_rec_smoothie_filter(RenderCmdKind kind)
{
	RenderCmd *c = rl_push();
	if (c == NULL)
		return;
	c->kind = kind;
	// rl_push already stamped c->surface = (VGAScreen == VGAScreen2), which is the
	// filter's SOURCE buffer; the destination is always the main buffer.
}

// Lazily-allocated background scratch (the "VGAScreen2" role) for replaying the
// smoothie two-buffer ping-pong without disturbing the live surfaces. Sized to the
// replay scale; reallocated only when the supersample factor changes (rare — the
// scale is constant within a present loop).
static SDL_Surface *rl_scratch_b = NULL;

static SDL_Surface *rl_get_scratch_b(int scale)
{
	const int w = vga_width * scale, h = vga_height * scale;
	if (rl_scratch_b != NULL && (rl_scratch_b->w != w || rl_scratch_b->h != h))
	{
		SDL_FreeSurface(rl_scratch_b);
		rl_scratch_b = NULL;
	}
	if (rl_scratch_b == NULL)
		rl_scratch_b = SDL_CreateRGBSurface(0, w, h, 8, 0, 0, 0, 0);
	return rl_scratch_b;
}

// Round-half-away-from-zero, the rounding the 1x replay always used; shared by every
// scaled position computation so scale==1 reproduces the old integer path exactly.
static inline int rl_iround(float v)
{
	return (int)(v + (v >= 0.0f ? 0.5f : -0.5f));
}

// Round a fractional POSITION offset to the nearest pixel, with exact integer-translation
// invariance. Unlike round-half-away-from-zero, floor(x + .5) gives the same result after any
// integer translation; a negative background row and positive enemy therefore cannot separate
// by 1px merely because the shared fast-scroll phase lands exactly on a half pixel.
static inline int rl_round_offset(double v)
{
	return (int)floor(v + 0.5);
}

// The one canonical vertical presentation transform for a background layer. Background rows
// and every bound entity call this same helper, so their shared component cannot diverge through
// stale command state, different rounding, or an unmatched interpolation id. Layer 3 is RECORDED
// after its integer advance; replay preserves its authored base step but removes any modifier-added
// part, then uses the shared lagged fractional clock. `now` remains available for callers that
// genuinely need the current phase.
//
// own100 is the bound entity's own per-tick displacement (par_yown100; background rows pass 0),
// folded into the SAME rounded value. Rounding the layer and own offsets separately makes their
// staircases interleave whenever the entity moves against the scroll (own opposing the rate):
// the sum then steps down-up-down inside one tick -- a 1px sawtooth at scale 1 (CORAL's upward-
// swimming launched fish). One combined round keeps own100 == 0 bit-identical to the background
// rows, holds a scroll-cancelling boss (rate + own == 0) perfectly still at every alpha, and
// tracks a mover's true position within half a pixel, monotonically. notes.md §Sub-pixel parallax.
static inline int rl_layer_y_offset(int layer, bool now, float inv, int scale, int own100)
{
	const float rate = now ? bg_layer_dy_now[layer] : bg_layer_dy[layer];
	const float frac = now ? bg_layer_yfrac_now[layer] : bg_layer_yfrac[layer];
	// endlessScrollExtraPx publishes both values in exact hundredths. Recover that fixed-point
	// representation before subtracting: doing (frac - rate) in float can turn an exact -N.5
	// endpoint into -N.500000004 under a fast rate, which half-up then rounds one pixel backward.
	const int rate100 = rl_iround(rate * 100.0f);
	const int frac100 = rl_iround(frac * 100.0f);
	const double offset = ((double)frac100 - (double)(rate100 + own100) * (double)inv) *
	                      (double)scale / 100.0;
	return rl_round_offset(offset);
}

// Wrap a delta into [-m/2, m/2) so background rows interpolate smoothly across the
// 24px/28px tile wrap instead of snapping. For either-way axes (horizontal scroll).
static int wrap_delta(int d, int m)
{
	int r = d % m;
	if (r < 0)
		r += m;
	if (r >= m / 2)
		r -= m;
	return r;
}

// Wrap a delta into [0, m): resolve the tile wrap DOWNWARD. Vertical scroll is always
// downward (backMove >= 0) and can be fast; the symmetric wrap_delta would map a
// >= m/2 px/tick scroll to a negative delta, interpolating the field the wrong way.
static int wrap_delta_down(int d, int m)
{
	int r = d % m;
	if (r < 0)
		r += m;
	return r;
}

void rl_finalize(void)
{
	RenderCmd *const cur = bufs[cur_buf];
	const size_t ncur = counts[cur_buf];
	RenderCmd *const prev = bufs[cur_buf ^ 1];
	const size_t nprev = counts[cur_buf ^ 1];

	// Per-id forward-linked lists over the previous frame, plus per-id blit counts
	// for both frames (to detect a changed sub-blit set — see the snap below).
	static int head[RL_ID_MAX];
	static int prevN[RL_ID_MAX], curN[RL_ID_MAX];
	static int *link = NULL;
	static size_t link_cap = 0;

	for (int i = 0; i < RL_ID_MAX; ++i)
	{
		head[i] = -1;
		prevN[i] = 0;
		curN[i] = 0;
	}

	if (link_cap < nprev)
	{
		int *n = realloc(link, nprev * sizeof(int));
		if (n == NULL)
			return;  // on OOM, skip matching: every command stays snapped (dx=dy=0)
		link = n;
		link_cap = nprev;
	}

	for (size_t i = nprev; i-- > 0; )
	{
		int id = prev[i].id;
		if (id <= 0 || id >= RL_ID_MAX)
			continue;
		link[i] = head[id];
		head[id] = (int)i;
		++prevN[id];
	}

	// Count this frame's blits per id (a pre-pass, since the matching loop below
	// needs each id's full current count before it decides the first blit).
	for (size_t i = 0; i < ncur; ++i)
	{
		int id = cur[i].id;
		if (id > 0 && id < RL_ID_MAX)
			++curN[id];
	}

	for (size_t i = 0; i < ncur; ++i)
	{
		RenderCmd *const c = &cur[i];
		// The player/parallax update occurs halfway through the legacy draw order. Depending on
		// the z-order flags, a bound enemy can therefore be recorded with the previous anchor
		// while its background uses the new one (or vice versa). Normalize the display-only
		// fraction to the anchor the layer actually recorded; x and simulation coordinates stay
		// untouched, and exact/residual replay ignores this correction.
		if (c->par_layer >= 1 && c->par_layer <= 3 && bg_layer_xofs_valid[c->par_layer])
			c->par_frac += bg_layer_xofs[c->par_layer] - c->par_anchor;

		const int id = c->id;

		// Extrapolating ids already carry their own velocity in dx/dy; keep it (see
		// rl_id_extrapolates) — no large-jump snap, no recycled-slot streak.
		if (rl_id_extrapolates(id))
			continue;

		c->dx = 0;
		c->dy = 0;
		c->par_yown100 = 0;

		if (id <= 0 || id >= RL_ID_MAX)
			continue;  // static / untagged: never interpolate

		// A changed per-id blit count means the sub-blit SET changed this tick (multi-
		// sprite enemy crossing a screen edge, shadow toggling, recycled slot). The
		// pairing below is positional (k-th cur <-> k-th prev), so survivors would
		// mis-pair and wobble; snap the whole id (dx/dy already 0) — invisible, and
		// self-limited to the one tick the count differs.
		if (prevN[id] != curN[id])
			continue;

		const int pi = head[id];
		if (pi < 0)
			continue;  // no match (newly spawned): snap
		head[id] = link[pi];

		int dx = c->x - prev[pi].x;
		int dy = c->y - prev[pi].y;

		// Parallax sub-pixel: the entity's own frac change this tick. Both fracs are the
		// same anchor's (an enemy keeps its layer), so this stays small; the integer part
		// of the parallax move is already in dx above, so their sum floats the parallax.
		c->par_frac_dx = c->par_frac - prev[pi].par_frac;

		// Recover only the entity-local displacement from the phase-corrected endpoints.
		// The canonical layer rate is deliberately NOT stored in the command: replay applies
		// it independently, so an unmatched/new/clipped bound sprite still follows its layer.
		int par_yown100 = 0;
		bool par_ybound_match = false;
		if (c->par_ylayer >= 1 && c->par_ylayer <= 3 &&
		    c->par_ylayer == prev[pi].par_ylayer)
		{
			par_ybound_match = true;
			const int endpoint100 =
			    ((c->y + c->par_ybase) -
			     (prev[pi].y + prev[pi].par_ybase)) * 100 +
			    rl_iround(c->par_yfrac * 100.0f) -
			    rl_iround(prev[pi].par_yfrac * 100.0f);
			const int L = c->par_ylayer;
			const float layer_rate = bg_layer_dy[L];
			par_yown100 = endpoint100 - rl_iround(layer_rate * 100.0f);
		}

		if (c->kind == RC_BG_ROW || c->kind == RC_BG_ROW_BLEND)
		{
			dx = wrap_delta(dx, 24);       // horizontal scroll: either direction
			// Vertical scroll is always downward and, under an endless speed modifier, can
			// exceed the 28px tile height. A screen-position diff only recovers the sub-tile
			// remainder (mod 28), so the whole-tile part would snap at every tick boundary
			// -> vertical jitter. Use the layer's TRUE per-tick scroll (whole tiles included),
			// tracked in backgrnd.c. In the normal (<28px/tick) case this equals the old
			// wrap_delta_down result exactly, so slow levels are unchanged.
			const int layer = id - RL_ID_BG_BASE;
			dy = (layer >= 1 && layer <= 3) ? bgScrollDeltaY[layer] : wrap_delta_down(dy, 28);
		}
		else if (c->kind == RC_FILTER_SCREEN)
		{
			// The flash/fade ramps brightness by ±levelBrightnessChg per tick; smooth it
			// across displayed frames. Snap across the -99 "no filter" sentinel and
			// colour-bank swaps — discontinuities, not ramps (a bank swap happens at
			// peak wash-out, so the snap is invisible).
			int db = c->filt_bright - prev[pi].filt_bright;
			if (c->filt_bright == -99 || prev[pi].filt_bright == -99 ||
			    c->filt_col != prev[pi].filt_col || db > 14 || db < -14)
				db = 0;
			c->filt_dbright = db;
		}
		else if (dx > 40 || dx < -40 ||
		         (par_ybound_match
		              ? (par_yown100 > 4000 || par_yown100 < -4000)
		              : (dy > 40 || dy < -40)))
		{
			// Large enemy-own jump => recycled slot or teleport; snap rather than streak.
			// A bound layer may itself legitimately move more than 40px under a speed
			// modifier, so exclude its canonical rate from this test.
			dx = 0;
			dy = 0;
			par_yown100 = 0;
		}

		c->dx = dx;
		c->dy = dy;
		c->par_yown100 = par_yown100;
	}
}

// Draw one explosion spark (superpixel): a 5-pixel additive blend, matching
// JE_drawSP (varz.c) so an exact (alpha=0) replay reproduces it pixel-for-pixel.
static void rl_draw_superpixel(SDL_Surface *dst, int x, int y, Uint8 z, Uint8 color)
{
	if (x < 0 || y < 0 || x >= dst->w || y >= dst->h)
		return;
	const int pitch = dst->pitch;
	Uint8 *const s = (Uint8 *)dst->pixels + y * pitch + x;
	*s = (((*s & 0x0f) + z) >> 1) + color;
	if (x > 0)            *(s - 1)     = (((*(s - 1)     & 0x0f) + (z >> 1)) >> 1) + color;
	if (x < dst->w - 1)   *(s + 1)     = (((*(s + 1)     & 0x0f) + (z >> 1)) >> 1) + color;
	if (y > 0)            *(s - pitch) = (((*(s - pitch) & 0x0f) + (z >> 1)) >> 1) + color;
	if (y < dst->h - 1)   *(s + pitch) = (((*(s + pitch) & 0x0f) + (z >> 1)) >> 1) + color;
}

// One scale x scale block of superpixel light (additive-ish blend matching
// rl_draw_superpixel's per-pixel math), clipped.
static void rl_superpixel_block(SDL_Surface *dst, int x, int y, int scale, Uint8 z, Uint8 color)
{
	int x0 = x < 0 ? 0 : x;
	int y0 = y < 0 ? 0 : y;
	int x1 = x + scale, y1 = y + scale;
	if (x1 > dst->w)
		x1 = dst->w;
	if (y1 > dst->h)
		y1 = dst->h;

	for (int yy = y0; yy < y1; ++yy)
	{
		Uint8 *p = (Uint8 *)dst->pixels + yy * dst->pitch + x0;
		for (int xx = x0; xx < x1; ++xx, ++p)
			*p = (((*p & 0x0f) + z) >> 1) + color;
	}
}

// Supersampled explosion spark: the same 5-tap pattern as rl_draw_superpixel with
// each tap a scale x scale block (halo taps one whole 1x pixel = `scale` away).
static void rl_draw_superpixel_scaled(SDL_Surface *dst, int x, int y, Uint8 z, Uint8 color, int scale)
{
	if (x < -(scale - 1) || y < -(scale - 1) || x >= dst->w || y >= dst->h)
		return;
	rl_superpixel_block(dst, x, y, scale, z, color);
	rl_superpixel_block(dst, x - scale, y, scale, z >> 1, color);
	rl_superpixel_block(dst, x + scale, y, scale, z >> 1, color);
	rl_superpixel_block(dst, x, y - scale, scale, z >> 1, color);
	rl_superpixel_block(dst, x, y + scale, scale, z >> 1, color);
}

// Plot one bar pixel, clipped. opacity < 255 alpha-blends like the engine's
// translucent sprites (blit_sprite_blend): the bar's colour bank is kept, only the
// brightness nibble mixes with the background's. Reading the destination makes the
// draw background-dependent — safe, because the residual capture replays the bar
// over the reconstructed background (see rl_capture_residual).
static inline void rl_hp_plot(SDL_Surface *dst, int x, int y, Uint8 col, Uint8 opacity)
{
	if (x < 0 || y < 0 || x >= dst->w || y >= dst->h)
		return;
	Uint8 *const p = &((Uint8 *)dst->pixels)[y * dst->pitch + x];
	if (opacity >= 255)
	{
		*p = col;
		return;
	}
	// Mix brightness: fg at `opacity`, background at the remainder. Keep the bar's
	// bank (col & 0xf0) so the fade stays inside the health-bar colour ramp.
	const int fg = col & 0x0f, bg = *p & 0x0f;
	int lo = (fg * opacity + bg * (255 - opacity) + 127) / 255;
	if (lo > 15)
		lo = 15;
	*p = (Uint8)((col & 0xf0) | lo);
}

// Draw one enemy health bar (see draw_enemy_health_bars in tyrian2.c). Shared by the
// authoritative tick draw and the interpolated replay so the two match pixel-for-pixel
// (required for the residual diff to cancel the bar out). 2px thick, `along` px long:
// horizontal fills left->right with a shadow row below, vertical fills bottom->up with
// a shadow column beside. All writes clip, so any interpolated position is safe.
void rl_draw_hp_bar(SDL_Surface *dst, int x, int y, int along, int fill, Uint8 col, bool vertical, Uint8 opacity)
{
	if (along < 1 || opacity == 0)
		return;
	if (fill > along) fill = along;
	if (fill < 0)     fill = 0;

	// Track (groove) and shadow live in the fill's own palette bank (col & 0xf0), not a
	// hardcoded bank 7, so an elite/champion bar is tinted blue/purple top-to-bottom
	// instead of only on the fill row/column. edge clamps within the bank's brightest.
	const int   bank   = col & 0xf0;
	const Uint8 groove = (Uint8)(bank + 2);
	const Uint8 shadow = (Uint8)(bank + 0);
	const Uint8 edge   = ((col & 0x0f) < 15) ? (Uint8)(col + 1) : col;

	if (!vertical)
	{
		for (int i = 0; i < along; ++i)          // empty groove
			rl_hp_plot(dst, x + i, y, groove, opacity);
		for (int i = 0; i < along; ++i)          // dark base shadow
			rl_hp_plot(dst, x + i, y + 1, shadow, opacity);
		for (int i = 0; i < fill; ++i)           // remaining health
			rl_hp_plot(dst, x + i, y, col, opacity);
		if (fill > 0)                            // glossy leading edge
			rl_hp_plot(dst, x + fill - 1, y, edge, opacity);
	}
	else
	{
		for (int i = 0; i < along; ++i)          // empty groove
			rl_hp_plot(dst, x, y + i, groove, opacity);
		for (int i = 0; i < along; ++i)          // dark base shadow
			rl_hp_plot(dst, x + 1, y + i, shadow, opacity);
		for (int i = 0; i < fill; ++i)           // remaining health, from the bottom up
			rl_hp_plot(dst, x, y + along - 1 - i, col, opacity);
		if (fill > 0)                            // glossy leading edge
			rl_hp_plot(dst, x, y + along - fill, edge, opacity);
	}
}

// One scale x scale block of health-bar pixel, clipped (see rl_hp_plot).
static void rl_hp_plot_block(SDL_Surface *dst, int x, int y, Uint8 col, Uint8 opacity, int scale)
{
	int x0 = x < 0 ? 0 : x;
	int y0 = y < 0 ? 0 : y;
	int x1 = x + scale, y1 = y + scale;
	if (x1 > dst->w)
		x1 = dst->w;
	if (y1 > dst->h)
		y1 = dst->h;

	for (int yy = y0; yy < y1; ++yy)
		for (int xx = x0; xx < x1; ++xx)
			rl_hp_plot(dst, xx, yy, col, opacity);
}

// Supersampled enemy health bar: same geometry as rl_draw_hp_bar with every 1x
// pixel a scale x scale block; x,y are HI coordinates (already interpolated on the
// sub-pixel grid), so the bar glides with its enemy.
static void rl_draw_hp_bar_scaled(SDL_Surface *dst, int x, int y, int along, int fill, Uint8 col, bool vertical, Uint8 opacity, int scale)
{
	if (along < 1 || opacity == 0)
		return;
	if (fill > along) fill = along;
	if (fill < 0)     fill = 0;

	// Same bank-derived track/shadow/edge as rl_draw_hp_bar (elite/champion tinting).
	const int   bank   = col & 0xf0;
	const Uint8 groove = (Uint8)(bank + 2);
	const Uint8 shadow = (Uint8)(bank + 0);
	const Uint8 edge   = ((col & 0x0f) < 15) ? (Uint8)(col + 1) : col;

	if (!vertical)
	{
		for (int i = 0; i < along; ++i)          // empty groove
			rl_hp_plot_block(dst, x + i * scale, y, groove, opacity, scale);
		for (int i = 0; i < along; ++i)          // dark base shadow
			rl_hp_plot_block(dst, x + i * scale, y + scale, shadow, opacity, scale);
		for (int i = 0; i < fill; ++i)           // remaining health
			rl_hp_plot_block(dst, x + i * scale, y, col, opacity, scale);
		if (fill > 0)                            // glossy leading edge
			rl_hp_plot_block(dst, x + (fill - 1) * scale, y, edge, opacity, scale);
	}
	else
	{
		for (int i = 0; i < along; ++i)          // empty groove
			rl_hp_plot_block(dst, x, y + i * scale, groove, opacity, scale);
		for (int i = 0; i < along; ++i)          // dark base shadow
			rl_hp_plot_block(dst, x + scale, y + i * scale, shadow, opacity, scale);
		for (int i = 0; i < fill; ++i)           // remaining health, from the bottom up
			rl_hp_plot_block(dst, x, y + (along - 1 - i) * scale, col, opacity, scale);
		if (fill > 0)                            // glossy leading edge
			rl_hp_plot_block(dst, x, y + (along - fill) * scale, edge, opacity, scale);
	}
}

static void rl_draw_cmd(SDL_Surface *dst, const RenderCmd *c, int x, int y)
{
	switch (c->kind)
	{
	case RC_HP_BAR:              rl_draw_hp_bar(dst, x, y, c->bar_w, c->bar_fill, c->bar_col, c->bar_vertical, c->bar_opacity); break;
	// Clip on X even for the nominally-unclipped kinds: an extrapolating id (player /
	// enemy shot, rl_id_extrapolates) is drawn at cur + vel*alpha, which leads a fast
	// edge-exiting shot a pixel or two past the surface bound. The non-clipping blits
	// wrap that overshoot onto the adjacent row (a shot leaving the left flashing on the
	// right). Clipping is a no-op for in-bounds sprites, so exact (alpha=0) replays stay
	// byte-identical in the visible playfield. The scaled path already clips (blit2_block).
	case RC_SPRITE2:             blit_sprite2_clip(dst, x, y, c->sheet, c->index); break;
	case RC_SPRITE2_CLIP:        blit_sprite2_clip(dst, x, y, c->sheet, c->index); break;
	case RC_SPRITE2_BLEND:       blit_sprite2_blend_clip(dst, x, y, c->sheet, c->index); break;
	case RC_SPRITE2_DARKEN:      blit_sprite2_darken_clip(dst, x, y, c->sheet, c->index); break;
	case RC_SPRITE2_FILTER:      blit_sprite2_filter(dst, x, y, c->sheet, c->index, c->filter); break;
	case RC_SPRITE2_FILTER_CLIP: blit_sprite2_filter_clip(dst, x, y, c->sheet, c->index, c->filter); break;
	case RC_SPRITE:              blit_sprite(dst, x, y, c->table, c->index); break;
	case RC_SPRITE_BLEND:        blit_sprite_blend(dst, x, y, c->table, c->index); break;
	case RC_SPRITE_HV:           blit_sprite_hv(dst, x, y, c->table, c->index, c->hue, c->value); break;
	case RC_SPRITE_HV_BLEND:     blit_sprite_hv_blend(dst, x, y, c->table, c->index, c->hue, c->value); break;
	case RC_SPRITE_HV_UNSAFE:    blit_sprite_hv_unsafe(dst, x, y, c->table, c->index, c->hue, c->value); break;
	case RC_SPRITE_DARK:         blit_sprite_dark(dst, x, y, c->table, c->index, c->black); break;
	case RC_BG_ROW:              blit_background_row(dst, x, y, c->map); break;
	case RC_BG_ROW_BLEND:        blit_background_row_blend(dst, x, y, c->map); break;
	case RC_STAR:                draw_starfield_star(dst, c->star_x, (int)(c->star_y + 0.5f), c->star_color); break;
	case RC_FILTER_SCREEN:       JE_filterScreenApply(dst, (JE_shortint)c->filt_col, (JE_shortint)c->filt_bright); break;
	}
}

// Supersampled dispatch: x,y are HI coordinates. Every kind routes to its scaled
// drawer; the clip-variant sprite kinds share the scaled blitter (it always clips).
// RC_STAR / RC_SUPERPIXEL / RC_FILTER_SCREEN are positioned specially and handled
// directly in rl_replay_common, like in the 1x path.
static void rl_draw_cmd_scaled(SDL_Surface *dst, const RenderCmd *c, int x, int y, int scale)
{
	switch (c->kind)
	{
	case RC_HP_BAR:              rl_draw_hp_bar_scaled(dst, x, y, c->bar_w, c->bar_fill, c->bar_col, c->bar_vertical, c->bar_opacity, scale); break;
	case RC_SPRITE2:             blit_sprite2_scaled(dst, x, y, c->sheet, c->index, scale, BLIT2_COPY, 0); break;
	case RC_SPRITE2_CLIP:        blit_sprite2_scaled(dst, x, y, c->sheet, c->index, scale, BLIT2_COPY, 0); break;
	case RC_SPRITE2_BLEND:       blit_sprite2_scaled(dst, x, y, c->sheet, c->index, scale, BLIT2_BLEND, 0); break;
	case RC_SPRITE2_DARKEN:      blit_sprite2_scaled(dst, x, y, c->sheet, c->index, scale, BLIT2_DARKEN, 0); break;
	case RC_SPRITE2_FILTER:      blit_sprite2_scaled(dst, x, y, c->sheet, c->index, scale, BLIT2_FILTER, c->filter); break;
	case RC_SPRITE2_FILTER_CLIP: blit_sprite2_scaled(dst, x, y, c->sheet, c->index, scale, BLIT2_FILTER, c->filter); break;
	case RC_SPRITE:              blit_sprite_table_scaled(dst, x, y, c->table, c->index, scale, BLITT_COPY, 0, 0, false); break;
	case RC_SPRITE_BLEND:        blit_sprite_table_scaled(dst, x, y, c->table, c->index, scale, BLITT_BLEND, 0, 0, false); break;
	case RC_SPRITE_HV:           blit_sprite_table_scaled(dst, x, y, c->table, c->index, scale, BLITT_HV, c->hue, c->value, false); break;
	case RC_SPRITE_HV_BLEND:     blit_sprite_table_scaled(dst, x, y, c->table, c->index, scale, BLITT_HV_BLEND, c->hue, c->value, false); break;
	case RC_SPRITE_HV_UNSAFE:    blit_sprite_table_scaled(dst, x, y, c->table, c->index, scale, BLITT_HV_UNSAFE, c->hue, c->value, false); break;
	case RC_SPRITE_DARK:         blit_sprite_table_scaled(dst, x, y, c->table, c->index, scale, BLITT_DARK, 0, 0, c->black); break;
	case RC_BG_ROW:              blit_background_row_scaled(dst, x, y, c->map, scale, false); break;
	case RC_BG_ROW_BLEND:        blit_background_row_scaled(dst, x, y, c->map, scale, true); break;
	default:                     break;
	}
}

// Residual = pixels the captured blit list does not reproduce: non-blit playfield
// draws (superpixels, boss-health bars, ...), diffed each tick against a blit-only
// replay and re-applied on every interpolated frame so those effects don't vanish
// between ticks (they snap rather than interpolate).
static int *res_off = NULL;
static Uint8 *res_val = NULL;
static size_t res_count = 0, res_cap = 0;
// Geometry of the 1x reference the residual was captured against, so a supersampled
// replay can decode each offset back to (x,y) and re-apply it as a scale x scale block.
static int res_ref_pitch = 0;

// Ship render-rate override: per-player offset applied to that ship's
// hull/shadow/charge (id in [RL_ID_SHIP_BASE, RL_ID_SIDEKICK_BASE)). Kept as FLOAT
// and rounded at the render scale, so a supersampled ship moves sub-pixel.
static bool ship_override_active = false;
static float ship_override_dx[2] = { 0, 0 }, ship_override_dy[2] = { 0, 0 };

// The ship's authoritative per-tick velocity: lets the replay separate a ship-
// attached shot's own motion (orbit) from its ship-tracking component (see below).
static int ship_tick_vel_x[2] = { 0, 0 }, ship_tick_vel_y[2] = { 0, 0 };

void rl_set_ship_override(int player, float dx, float dy)
{
	if (player < 0 || player > 1)
		return;
	ship_override_active = true;
	ship_override_dx[player] = dx;
	ship_override_dy[player] = dy;
}

void rl_clear_ship_override(void)
{
	ship_override_active = false;
}

// Current render-rate x offset applied to a ship this frame (0 if inactive); overlays
// that track the smooth ship (Soul of Zinglon pillar) add it to their tick position.
float rl_get_ship_override_dx(int player)
{
	if (player < 0 || player > 1 || !ship_override_active)
		return 0.0f;
	return ship_override_dx[player];
}

void rl_set_ship_vel(int player, int vx, int vy)
{
	if (player < 0 || player > 1)
		return;
	ship_tick_vel_x[player] = vx;
	ship_tick_vel_y[player] = vy;
}

// Ids drawn extrapolated (forward, at the render rate) instead of interpolated (a
// tick behind), so they share the render-rate ship's clock. Their dx/dy hold the
// shot's own recorded per-tick velocity (rl_current_vel_*), so cur + dx*alpha is
// exact even for fast bullets and immune to slot recycling (rl_finalize keeps it).
static bool rl_id_extrapolates(int id)
{
	// Player + enemy shots (rl_current_vel_* stamped around the blit in shots.c);
	// a fresh shot leads from the gun with no muzzle gap, and fast free shots don't
	// lag behind the ship and jitter. Ship-tracking shots (laser, main pulse) instead
	// follow the ship via ship_attach, which wins in replay.
	return id >= RL_ID_PSHOT_BASE && id < RL_ID_EXPL_BASE;  // player + enemy shots
}

// Which slice of the render list a replay pass draws. Smoothie levels split the
// frame into two passes so the background feedback can evolve continuously (smooth)
// while entities are composited fresh on top (also smooth) without polluting the
// feedback. Normal levels use ALL (one self-contained pass).
typedef enum
{
	RL_PHASE_ALL = 0,  // backgrounds + filters + entities + grade (normal levels)
	RL_PHASE_BG,       // backgrounds + smoothie filters only (the persistent plasma)
	RL_PHASE_FG,       // entities + full-screen grade + residual only (onto a plasma copy)
}
rl_phase;

static void rl_replay_common(SDL_Surface *dst, float inv, float alpha, bool apply_residual, bool use_override, bool feedback, rl_phase phase, int scale)
{
	const bool was_recording = render_list_recording;
	render_list_recording = false;  // re-issued blits must not record themselves

	// A = main playfield buffer; B = background scratch (smoothie ping-pong).
	// At scale > 1 both are supersampled (dst comes in scaled; B is sized to match).
	SDL_Surface *const A = dst;
	SDL_Surface *const B = rl_get_scratch_b(scale);

	// The leaf blitters step rows using the global VGAScreen's pitch; point it
	// at dst so they write coherently (all 8-bit surfaces share a pitch anyway).
	SDL_Surface *const saved = VGAScreen;
	VGAScreen = A;

	// B is rebuilt each frame (the FG phase draws no backgrounds, so it skips B). A
	// is cleared only for the self-contained ALL pass on normal levels: the BG pass's
	// A is the persistent plasma (must carry across frames) and the FG pass's A is a
	// fresh copy of it (already populated), so neither may be cleared.
	if (B != NULL && phase != RL_PHASE_FG)
		JE_clr256(B);
	if (!feedback && phase == RL_PHASE_ALL)
		JE_clr256(A);

	RenderCmd *const cur = bufs[cur_buf];
	const size_t n = counts[cur_buf];
	for (size_t i = 0; i < n; ++i)
	{
		const RenderCmd *const c = &cur[i];

		const bool is_filter = (c->kind == RC_ICED_BLUR || c->kind == RC_LAVA_FILTER || c->kind == RC_WATER_FILTER || c->kind == RC_BLUR);
		const bool is_bg = (c->kind == RC_BG_ROW || c->kind == RC_BG_ROW_BLEND || c->kind == RC_STAR);
		if (phase == RL_PHASE_BG && !(is_bg || is_filter))
			continue;  // entities and the full-screen grade belong to the FG pass
		if (phase == RL_PHASE_FG && (is_bg || is_filter))
			continue;  // backgrounds and filters are already baked into the plasma copy

		// In the FG pass, entities draw straight onto the display buffer (the plasma
		// copy); the B/A ping-pong source only matters while evolving the plasma.
		SDL_Surface *const src = (phase != RL_PHASE_FG && c->surface && B != NULL) ? B : A;

		if (is_filter)
		{
			if (scale == 1)
			{
				switch (c->kind)
				{
				case RC_ICED_BLUR:    iced_blur_filter(A, src); break;
				case RC_LAVA_FILTER:  lava_filter(A, src);      break;
				case RC_BLUR:         blur_filter(A, src);      break;
				default:              water_filter(A, src);     break;  // RC_WATER_FILTER
				}
			}
			else
			{
				switch (c->kind)
				{
				case RC_ICED_BLUR:    iced_blur_filter_scaled(A, src, scale); break;
				case RC_LAVA_FILTER:  lava_filter_scaled(A, src, scale);      break;
				case RC_BLUR:         blur_filter_scaled(A, src, scale);      break;
				default:              water_filter_scaled(A, src, scale);     break;
				}
			}
			continue;
		}

		if (c->kind == RC_FILTER_SCREEN)
		{
			// Full-screen flash/fade: interpolate the brightness across the tick so
			// the ramp is smooth at any refresh (filt_dbright = 0 => snap). Applied
			// side-effect-free onto the composited playfield (A), matching the tick's
			// own JE_filterScreen which runs after all entities.
			int bright = c->filt_bright;
			if (inv != 0.0f && c->filt_dbright != 0)
				bright -= rl_iround(c->filt_dbright * inv);
			if (scale == 1)
				JE_filterScreenApply(A, (JE_shortint)c->filt_col, (JE_shortint)bright);
			else
				filter_screen_apply_scaled(A, (JE_shortint)c->filt_col, (JE_shortint)bright, scale);
			continue;
		}

		if (c->kind == RC_STAR)
		{
			// Interpolate only the row (x is fixed): the star slides from its
			// previous row to the recorded one across the tick. star_dy is 0 on a
			// wrap tick, so a wrapped star simply snaps to the top. At scale > 1 the
			// float row lands on the 1/scale-pixel grid — slow drifts glide.
			const float sy = c->star_y - c->star_dy * inv;
			if (scale == 1)
				draw_starfield_star(src, c->star_x, (int)(sy + 0.5f), c->star_color);
			else
				draw_starfield_star_scaled(src, c->star_x * scale, (int)(sy * scale + 0.5f), c->star_color, scale);
			continue;
		}

		if (c->kind == RC_SUPERPIXEL)
		{
			// Explosion spark at its interpolated position (constant velocity, so the
			// recorded per-tick delta is self-contained — no cross-frame matching).
			const int sx = c->x * scale - rl_iround(c->sp_dx * inv * scale);
			const int sy = c->y * scale - rl_iround(c->sp_dy * inv * scale);
			if (scale == 1)
				rl_draw_superpixel(src, sx, sy, c->sp_z, c->sp_color);
			else
				rl_draw_superpixel_scaled(src, sx, sy, c->sp_z, c->sp_color, scale);
			continue;
		}

		// Position: exact scaled integer base plus a rounded displacement. Vertical background
		// bindings also keep their phase correction split into integer + fractional pieces; this
		// is required for relative alignment when one sprite is above Y=0 and another below it.
		int x = c->x * scale, y = c->y * scale;
		const bool is_ship_id = c->id >= RL_ID_SHIP_BASE && c->id < RL_ID_SIDEKICK_BASE;
		if (use_override && ship_override_active && is_ship_id)
		{
			// Ship hull/shadow/charge: render-rate driven, not time-interpolated.
			// Sidekicks are EXCLUDED — trailing companions (e.g. Gerund) follow the
			// ship's past path, not its velocity; the ship offset would jitter them.
			// They interpolate by their own motion instead (the branch below).
			// id = RL_ID_SHIP_BASE + playerNum (1 or 2) => player index 0/1.
			int p = c->id - RL_ID_SHIP_BASE - 1;
			if (p < 0) p = 0; else if (p > 1) p = 1;
			x += rl_iround(ship_override_dx[p] * scale);
			y += rl_iround(ship_override_dy[p] * scale);
		}
		else
		{
			// Per-axis placement. An axis that tracks the ship (ship_attach) is
			// drawn at the ship's render-rate position, so attached shots (laser,
			// main pulse) stay on the gun during strafes; otherwise the axis
			// extrapolates (enemy shots) or interpolates (everything else).
			const bool ovr = use_override && ship_override_active;
			const int sp = (c->ship_attach >> 2) & 1;  // player index
			const bool extrap = rl_id_extrapolates(c->id);

			// Background rows: on a display replay (use_override) pan the horizontal parallax
			// sub-pixel-smooth as recorded x plus (frac - dx*inv), from the un-floored float
			// offsets (backgrnd.c). The exact/residual replay keeps the whole-pixel c->dx so
			// recorded frames reproduce byte-exact. notes.md §Sub-pixel parallax.
			const bool bg_row = (c->kind == RC_BG_ROW || c->kind == RC_BG_ROW_BLEND)
			    && c->id >= RL_ID_BG_BASE + 1 && c->id <= RL_ID_BG_BASE + 3;

			if (bg_row)
			{
				const int L = c->id - RL_ID_BG_BASE;
				if (use_override)
					x = c->x * scale + rl_iround((bg_layer_frac[L] - bg_layer_dx[L] * inv) * scale);
				else if (c->dx && inv != 0.0f)
					x -= rl_iround(c->dx * inv * scale);  // exact / smoothie: classic whole-pixel
			}
			else if ((c->ship_attach & 1) && ovr)
			{
				x += rl_iround(ship_override_dx[sp] * scale);  // X tracks the render-rate ship
				// An attached shot can also move relative to the ship (orbiting asteroid
				// killer, weapon 104). c->dx is the total delta (ship move + own motion);
				// subtracting the ship's velocity leaves the own motion to interpolate
				// (smooth orbit). A pure tracker (laser, main pulse) has own == 0 and stays
				// glued. The >40 guard snaps on a warp tick (the ship override snaps too).
				const int own = c->dx - ship_tick_vel_x[sp];
				if (inv != 0.0f && own && own <= 40 && own >= -40)
					x -= rl_iround(own * inv * scale);
			}
			else if (extrap)
			{
				// Forward extrapolation leads by the predicted next displacement (velocity +
				// acceleration). Leaving out acceleration makes a decelerating shot overshoot
				// each tick and snap back at the boundary; adding it lands exactly on the next
				// tick position.
				const int vext = c->dx + c->acc_x;
				if (vext)
					x += rl_iround(vext * alpha * scale);
			}
			else if (use_override && (c->par_frac != 0.0f || c->par_frac_dx != 0.0f))
			{
				// Parallax-anchored entity (enemy / HP bar): fold the integer parallax + own X
				// (c->dx) and the sub-pixel fraction into one rounded displacement, so the <1px
				// fraction survives at scale 1 instead of rounding to 0 (mirrors the vertical
				// par_yfrac path below). c->dx also carries any own horizontal motion, so a moving
				// enemy still interpolates right. notes.md §Sub-pixel parallax.
				x = c->x * scale + rl_iround((c->par_frac - (c->dx + c->par_frac_dx) * inv) * scale);
			}
			else if (c->dx && inv != 0.0f)
			{
				x -= rl_iround(c->dx * inv * scale);
			}

			if ((c->ship_attach & 2) && ovr)
			{
				y += rl_iround(ship_override_dy[sp] * scale);  // Y tracks the render-rate ship
				const int own = c->dy - ship_tick_vel_y[sp];  // own (orbit) motion; see X
				if (inv != 0.0f && own && own <= 40 && own >= -40)
					y -= rl_iround(own * inv * scale);
			}
			else if (extrap)
			{
				const int vext = c->dy + c->acc_y;  // velocity + acceleration; see X
				if (vext)
					y += rl_iround(vext * alpha * scale);
			}
			else if (bg_row && bg_smooth_y_active && use_override)
			{
				// Vertical scroll at the true float rate (constant velocity) instead of the
				// integer per-tick pulse of c->dy (bgScrollDeltaY), which freezes on delay-gated
				// slow sections then jumps. Mirrors the horizontal parallax above; a byte-exact
				// no-op (frac 0, integer rate) on full-speed layers. notes.md §Slow-scroll smoothing.
				//
				// The integer row and fractional phase are rounded separately with rl_round_offset,
				// whose half-up rule is integer-translation-invariant. Thus tile-wrap pulses remain
				// continuous without making negative rows round differently from positive entities.
				//
				// Layer 3 is recorded AFTER its integer advance, unlike layers 1/2. The stock 1px
				// base advance is part of the level's authored placement; only a scroll modifier's
				// EXTRA pixels are unaccounted for. Remove those extras, then use the same lagged
				// fractional clock as the bound enemy. Removing nothing leaves BRAINIAC's shootables
				// behind; removing the complete step puts them 1px ahead. Removing exactly the extra
				// preserves stock placement at every modifier speed.
				const int L = c->id - RL_ID_BG_BASE;
				const int phase_base = (L == 3) ? -endlessScrollExtraPx3 : 0;
				y = (c->y + phase_base) * scale +
				    rl_layer_y_offset(L, false, inv, scale, 0);
			}
			else if (use_override && c->par_ylayer != 0)
			{
				// Scroll-tracked entity (enemy / HP bar): the canonical layer transform with
				// the entity-local displacement folded into the same single round (see
				// rl_layer_y_offset). A resting entity (own100 == 0) stays bit-identical to
				// the background's offset even for a new command, a changed multi-blit count,
				// or a snapped slot; an entity opposing the scroll no longer saws 1px inside
				// the tick from two interleaved rounding staircases.
				const int L = c->par_ylayer;
				y = (c->y + c->par_ybase) * scale +
				    rl_layer_y_offset(L, false, inv, scale, c->par_yown100);
			}
			else if (c->dy && inv != 0.0f)
			{
				y -= rl_iround(c->dy * inv * scale);
			}
		}
		if (scale == 1)
			rl_draw_cmd(src, c, x, y);  // backgrounds -> B, entities/filters-out -> A
		else
			rl_draw_cmd_scaled(src, c, x, y, scale);
	}

	VGAScreen = saved;
	render_list_recording = was_recording;

	if (apply_residual)
	{
		if (scale == 1)
		{
			Uint8 *const p = (Uint8 *)A->pixels;
			for (size_t i = 0; i < res_count; ++i)
				p[res_off[i]] = res_val[i];
		}
		else if (res_ref_pitch > 0)
		{
			// Residual pixels were captured against the 1x reference; re-apply each
			// as a scale x scale block (overlays like the boss bar simply appear at
			// classic resolution — correct, just not supersampled).
			for (size_t i = 0; i < res_count; ++i)
			{
				const int rx = res_off[i] % res_ref_pitch;
				const int ry = res_off[i] / res_ref_pitch;
				if (rx >= vga_width || ry >= vga_height)
					continue;  // offset landed in the 1x pitch padding
				const Uint8 v = res_val[i];
				Uint8 *row = (Uint8 *)A->pixels + (ry * scale) * A->pitch + rx * scale;
				for (int yy = 0; yy < scale; ++yy)
				{
					memset(row, v, scale);
					row += A->pitch;
				}
			}
		}
	}
}

void rl_replay(SDL_Surface *dst)
{
	rl_replay_common(dst, 0.0f, 0.0f, false, false, false, RL_PHASE_ALL, 1);  // exact positions (inv=0, alpha=0)
}

void rl_replay_interp(SDL_Surface *dst, float alpha, bool feedback, int scale)
{
	if (alpha < 0.0f)
		alpha = 0.0f;
	else if (alpha > 1.0f)
		alpha = 1.0f;

	// Normal (non-smoothie) levels: one self-contained pass into dst (cleared first),
	// entities interpolated, residual (superpixels, boss bar, HUD) on top; smoothie
	// levels use the two passes below instead.
	rl_replay_common(dst, 1.0f - alpha, alpha, true, true, feedback, RL_PHASE_ALL, scale);
}

// Smoothie pass 1 (background): apply the filter once, FULL strength, feedback on, entities
// skipped (RL_PHASE_BG). Two call sites: per frame (dst = fresh copy of render_gs, frame
// alpha) and per tick (dst = render_gs, alpha 1, advancing the base one step). use_override
// on so bg rows get the same sub-pixel parallax as pass 2; both call sites share identical
// float positions, so base and copies agree at the tick boundary (no seam). notes.md §Smoothie levels.
void rl_replay_bg(SDL_Surface *dst, float alpha, int scale)
{
	if (alpha < 0.0f)
		alpha = 0.0f;
	else if (alpha > 1.0f)
		alpha = 1.0f;
	rl_replay_common(dst, 1.0f - alpha, alpha, false, true, true, RL_PHASE_BG, scale);
}

// Smoothie pass 2 (foreground): onto pass 1's background frame, draw the entities at
// interpolated / ship-override positions plus the full-screen grade, then re-apply
// the residual overlays (WARNING bars, superpixels, boss bar, HUD). dst not cleared.
void rl_replay_fg(SDL_Surface *dst, float alpha, int scale)
{
	if (alpha < 0.0f)
		alpha = 0.0f;
	else if (alpha > 1.0f)
		alpha = 1.0f;
	rl_replay_common(dst, 1.0f - alpha, alpha, true, true, false, RL_PHASE_FG, scale);
}

// See render_list.h: callers fully reproduced by recorded blits must not inherit
// gameplay's residual pixels.
void rl_clear_residual(void)
{
	res_count = 0;
}

// Append one residual pixel (offset + value). Returns false if growth failed
// (out of memory) so the caller can stop; the residual captured so far is kept.
static bool rl_res_push(int off, Uint8 val)
{
	if (res_count == res_cap)
	{
		size_t ncap = res_cap ? res_cap * 2 : 1024;
		int *no = realloc(res_off, ncap * sizeof(*no));
		Uint8 *nv = realloc(res_val, ncap * sizeof(*nv));
		if (no == NULL || nv == NULL)
		{
			if (no != NULL) res_off = no;
			if (nv != NULL) res_val = nv;
			return false;  // OOM: keep what we have
		}
		res_off = no;
		res_val = nv;
		res_cap = ncap;
	}

	res_off[res_count] = off;
	res_val[res_count] = val;
	++res_count;
	return true;
}

void rl_capture_residual(SDL_Surface *reference, SDL_Surface *scratch)
{
	JE_clr256(scratch);
	rl_replay(scratch);  // blit-only reproduction at recorded positions

	res_count = 0;
	res_ref_pitch = reference->pitch;

	const size_t n = (size_t)reference->h * reference->pitch;
	const Uint8 *const ref = (const Uint8 *)reference->pixels;
	const Uint8 *const sc = (const Uint8 *)scratch->pixels;
	for (size_t i = 0; i < n; ++i)
	{
		if (ref[i] == sc[i])
			continue;
		if (!rl_res_push((int)i, ref[i]))
			break;
	}
}

// See render_list.h. `before` = the frame just before the post-filter overlays,
// `after` = the finished frame; the filtered playfield is identical in both, so
// only overlay pixels are caught.
void rl_capture_residual_delta(SDL_Surface *before, SDL_Surface *after)
{
	res_count = 0;
	res_ref_pitch = after->pitch;

	const size_t n = (size_t)after->h * after->pitch;
	const Uint8 *const b = (const Uint8 *)before->pixels;
	const Uint8 *const a = (const Uint8 *)after->pixels;
	for (size_t i = 0; i < n; ++i)
	{
		if (a[i] == b[i])
			continue;
		if (!rl_res_push((int)i, a[i]))
			break;
	}
}

size_t rl_replay_and_compare(SDL_Surface *scratch, SDL_Surface *reference)
{
	JE_clr256(scratch);  // the frame normally starts with a full clear to 0
	rl_replay(scratch);

	const size_t n = (size_t)reference->h * reference->pitch;
	const Uint8 *a = (const Uint8 *)scratch->pixels;
	const Uint8 *b = (const Uint8 *)reference->pixels;
	size_t mismatch = 0;
	for (size_t i = 0; i < n; ++i)
		if (a[i] != b[i])
			++mismatch;
	return mismatch;
}
