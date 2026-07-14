/*
 * High-refresh render interpolation: every playfield draw of a 35Hz tick is recorded
 * and re-drawn at interpolated positions once per displayed frame (re-running the
 * game's draw code isn't safe). notes.md §Smooth motion (render list).
 */
#ifndef RENDER_LIST_H
#define RENDER_LIST_H

#include "opentyr.h"
#include "sprite.h"

#include "SDL.h"

#include <stdbool.h>

typedef enum
{
	RC_SPRITE2 = 0,
	RC_SPRITE2_CLIP,
	RC_SPRITE2_BLEND,
	RC_SPRITE2_DARKEN,
	RC_SPRITE2_FILTER,
	RC_SPRITE2_FILTER_CLIP,
	RC_SPRITE,
	RC_SPRITE_BLEND,
	RC_SPRITE_HV,
	RC_SPRITE_HV_BLEND,
	RC_SPRITE_HV_UNSAFE,
	RC_SPRITE_DARK,
	RC_BG_ROW,
	RC_BG_ROW_BLEND,
	RC_STAR,
	RC_FILTER_SCREEN,   // full-screen colour filter (JE_filterScreen)
	RC_ICED_BLUR,       // smoothie feedback filters: dst = filter(dst, src), src = the
	RC_LAVA_FILTER,     // buffer named by `surface`. These read the previous frame's
	RC_WATER_FILTER,    // main buffer (trails/plasma), so it persists across frames.
	RC_BLUR,            // like RC_ICED_BLUR but preserves the source pixel hue
	RC_SUPERPIXEL,      // explosion spark (JE_drawSP): 5-pixel additive blend, foreground
	RC_HP_BAR,          // enemy health bar; interpolates with its enemy (same id matching)
} RenderCmdKind;

typedef struct
{
	Uint8 kind;
	int x, y;

	// Target/source buffer: 0 = main playfield buffer, 1 = background scratch
	// (VGAScreen2). Smoothie levels draw backgrounds to the scratch, then a filter
	// blends it into the main buffer. Always 0 on non-smoothie levels.
	Uint8 surface;

	// Identity for cross-frame matching, and this tick's motion (cur - prev):
	// interpolated replay draws at (x,y) - (dx,dy)*(1-alpha). EXTRAPOLATED ids
	// (shots, see rl_id_extrapolates) instead hold their own recorded per-tick
	// velocity (rl_current_vel_*) and replay forward at (x,y) + (dx+acc,dy+acc)*alpha.
	int id;
	int dx, dy;

	// Per-tick acceleration (shots only; 0 for everything else). Extrapolation leads
	// by velocity + acceleration = the predicted NEXT displacement, so a decelerating
	// shot lands on its next tick position instead of overshooting and snapping back
	// each boundary (which reads as the shot briefly reversing).
	int acc_x, acc_y;

	// sprite2 family
	Sprite2_array sheet;
	unsigned int index;

	// sprite (table) family
	unsigned int table;

	// background row
	Uint8 **map;

	// star: column (constant), float row, and this tick's row motion (for interp)
	int star_x;
	float star_y;
	float star_dy;
	Uint8 star_color;

	// superpixel (explosion spark): per-tick motion, brightness, colour. Velocity is
	// constant, so the motion is self-contained (no cross-frame matching, like a star).
	int sp_dx, sp_dy;
	Uint8 sp_z, sp_color;

	// enemy health bar (RC_HP_BAR): length along the fill axis, filled-pixel count,
	// fill colour. Top-left in x,y, interpolating by (dx,dy) like the enemy.
	// bar_vertical: 0 fills left->right, 1 fills bottom->up; bar_opacity = blend
	// alpha (0..255, 255 = solid).
	int bar_w, bar_fill;
	Uint8 bar_col;
	Uint8 bar_vertical;
	Uint8 bar_opacity;

	// full-screen filter: colour bank + brightness, plus this tick's brightness
	// motion (cur - prev) so the flash/fade ramp interpolates across frames.
	int filt_col;
	int filt_bright;
	int filt_dbright;

	// modifiers
	Uint8 hue;
	Sint8 value;
	bool black;
	Uint8 filter;

	// Per-axis ship attachment for tracking shots (bit0 = X, bit1 = Y, bit2 = player
	// index). An attached axis is drawn at the ship's render-rate position instead of
	// interpolated, so the laser/main-pulse base stays on the gun. 0 = not attached.
	Uint8 ship_attach;

	// Sub-pixel parallax correction for entities anchored to a background layer (enemies:
	// drawn at ex + tempMapXOfs, a whole-pixel offset). par_frac = the fraction the integer
	// offset dropped this tick; par_frac_dx = its per-tick change (finalize). The display
	// replay adds (par_frac - par_frac_dx*inv) so the entity floats its parallax onto the
	// same sub-pixel offset as its layer (kept glued instead of stepping). 0 = not anchored.
	float par_frac, par_frac_dx;

	// VERTICAL counterpart for scroll-tracked entities: the sub-pixel Y offset of the
	// background layer this entity rides (bg_layer_yfrac), so it floats onto the layer's
	// smooth sub-pixel scroll instead of stepping by the integer per-tick delta. Without it,
	// a boosted (fractional-rate) scroll makes the smooth background slide against the
	// integer-stepping enemies. par_yfrac_dy = its per-tick change (finalize). 0 = not tracked.
	float par_yfrac, par_yfrac_dy;
}
RenderCmd;

// When true, the leaf blit functions append a command to the active list.
// Replay turns this off so re-issued blits are not recorded again.
extern bool render_list_recording;

// Id stamped onto subsequent recorded commands; the game sets it before drawing a
// logical entity (enemy slot, ship, background layer, ...) so the entity can be
// matched across frames. 0 = untagged/static (never interpolated).
extern int rl_current_id;

// Per-axis ship attachment for the next recorded command(s) (see ship_attach in
// RenderCmd). Shots set this around their blit; 0 otherwise.
extern int rl_shot_attach;

// Sub-pixel parallax fraction stamped onto the next recorded command(s) (see par_frac in
// RenderCmd). blit_enemy sets this around its blit to the fraction its whole-pixel
// tempMapXOfs dropped (tempMapXOfs_frac); 0 otherwise.
extern float rl_current_par_frac;

// Vertical scroll sub-pixel fraction stamped onto the next recorded command(s) (see
// par_yfrac in RenderCmd). blit_enemy sets this to the bg_layer_yfrac of the layer the
// enemy scroll-tracks, so it stays glued to the smoothly-scrolling background; 0 otherwise.
extern float rl_current_par_yfrac;

// Per-tick velocity (px/tick) stamped onto the next recorded command(s): a shot's
// real motion (sxm/sym), set around its blit, 0 otherwise. Drives forward
// extrapolation (see the dx/dy note in RenderCmd).
extern int rl_current_vel_x, rl_current_vel_y;

// Per-tick acceleration (px/tick^2) stamped onto the next recorded command(s): a
// shot's sxc/syc (shotXC/shotYC), set around its blit, 0 otherwise. Extrapolation
// leads by velocity + acceleration so a decelerating shot doesn't overshoot and snap
// back (see acc_x/acc_y in RenderCmd).
extern int rl_current_acc_x, rl_current_acc_y;

// Identity ranges for rl_current_id (kept < RL_ID_MAX). The ranges must not overlap: each base
// plus its largest "+ slot" has to stay below the next base. RL_ID_PSHOT_BASE + slot spans the
// whole player-shot pool (MAX_PWEAPON, shots.h), so the ranges after it were pushed up to give
// the enlarged pool room (player shots now occupy 3000..3000+MAX_PWEAPON-1 = ..10999).
enum
{
	RL_ID_FILTER = 8,        // full-screen colour filter (one per tick; brightness interpolates)
	RL_ID_BG_BASE = 16,      // + layer (1..3)
	RL_ID_ENEMY_BASE = 2000, // + slot
	RL_ID_ENEMYBAR_BASE = 2500, // + slot (enemy health bar; interpolates with its enemy)
	RL_ID_PSHOT_BASE = 3000, // + slot (0 .. MAX_PWEAPON-1; reaches ~10999 at MAX_PWEAPON = 8000)
	RL_ID_ESHOT_BASE = 12000, // + slot (0 .. ENEMY_SHOT_MAX-1)
	RL_ID_EXPL_BASE = 13000, // + slot (0 .. MAX_EXPLOSIONS-1); also the upper bound of the "shot" id range
	RL_ID_SHIP_BASE = 14000, // + player
	RL_ID_SIDEKICK_BASE = 15000, // + player*2 + slot
	RL_ID_MAX = 16384,
};

// Begin/finish recording the current tick's playfield draws.
void rl_begin_record(void);
void rl_end_record(void);

// Number of commands captured for the current frame.
size_t rl_count(void);

// Match the just-recorded frame against the previous one and compute each
// command's per-tick motion (dx,dy). Call once after rl_end_record.
void rl_finalize(void);

// Re-draw every captured command into dst at its recorded position (alpha=1).
void rl_replay(SDL_Surface *dst);

// Smoothie levels present in two passes (full rationale at the definitions):
//   rl_replay_bg: backgrounds interpolated + the smoothie filter, once, full strength
//     (per displayed frame on a COPY of the plasma base; per tick advancing the base).
//   rl_replay_fg: entities interpolated + full-screen grade + residual, onto pass 1.
// `scale` >= 2 replays into an NxN supersampled dst (vga_width*scale wide) with all
// positions on the 1/scale-pixel grid; 1 reproduces the classic path byte-for-byte.
void rl_replay_bg(SDL_Surface *dst, float alpha, int scale);
void rl_replay_fg(SDL_Surface *dst, float alpha, int scale);

// Re-draw every captured command into dst at an interpolated position
// (x,y) - (dx,dy)*(1-alpha); alpha in [0,1], 1 reproduces the exact frame. Also
// re-applies the captured residual. feedback=false clears dst first (normal levels);
// feedback=true does not, so smoothie trails persist — caller seeds dst from the tick
// frame. `scale` as in rl_replay_bg (dst must be scale x the logical size).
void rl_replay_interp(SDL_Surface *dst, float alpha, bool feedback, int scale);

// Capture the residual: pixels in `reference` (the authoritative frame) that a
// blit-only replay doesn't reproduce — non-blit draws like superpixels and boss-
// health bars. `scratch` is a same-size 8-bit work surface. Call after the tick draws.
void rl_capture_residual(SDL_Surface *reference, SDL_Surface *scratch);

// Capture residual from a before/after diff of the authoritative frame. Used on
// feedback (smoothie) levels to grab only the overlays drawn after the per-pixel
// filters (boss bar, in-game displays) — a blit-only replay can't rebuild the
// evolved plasma, so the full capture would wrongly flag the filtered playfield.
void rl_capture_residual_delta(SDL_Surface *before, SDL_Surface *after);

// Drop captured residual (so rl_replay_interp applies none). For callers whose
// frame is fully reproduced by recorded blits.
void rl_clear_residual(void);

// Ship override: during interpolated replay, the hull/shadow/charge of player
// `player` (0 or 1) draw at their recorded position PLUS (dx,dy) instead of being
// time-interpolated, driving each ship at the render rate. Sidekicks are excluded
// by id range and interpolate by their own motion. dx/dy are FLOAT: the replay
// rounds them at the render scale, so a supersampled ship moves on the sub-pixel
// grid instead of snapping whole pixels.
void rl_set_ship_override(int player, float dx, float dy);
void rl_clear_ship_override(void);
float rl_get_ship_override_dx(int player);

// The ship's authoritative per-tick velocity (player 0/1), set once per tick. A
// ship-attached shot that also moves relative to the ship (orbiting asteroid killer)
// records ship-move + own-move; subtracting this recovers the own-move so it can be
// interpolated (smooth orbit).
void rl_set_ship_vel(int player, int vx, int vy);

// Completeness gate: clear scratch, replay the captured list into it, and return the
// number of bytes differing from reference (0 = the list fully reproduces the frame).
size_t rl_replay_and_compare(SDL_Surface *scratch, SDL_Surface *reference);

// Recorder helpers, called from the leaf blit functions when recording.
void rl_rec_sprite2(int x, int y, Sprite2_array sheet, unsigned int index, RenderCmdKind kind);
void rl_rec_sprite2_filter(int x, int y, Sprite2_array sheet, unsigned int index, Uint8 filter, bool clip);
void rl_rec_sprite(int x, int y, unsigned int table, unsigned int index, RenderCmdKind kind, Uint8 hue, Sint8 value, bool black);
void rl_rec_bg_row(int x, int y, Uint8 **map, bool blend);
void rl_rec_star(int x, float y, float dy, Uint8 color);
void rl_rec_superpixel(int x, int y, int dx, int dy, Uint8 z, Uint8 color);
void rl_rec_hp_bar(int x, int y, int along, int fill, Uint8 col, bool vertical, Uint8 opacity);
// Draw an enemy health bar (shared by the authoritative tick draw and the
// interpolated replay so they produce identical pixels).
void rl_draw_hp_bar(SDL_Surface *dst, int x, int y, int along, int fill, Uint8 col, bool vertical, Uint8 opacity);
void rl_rec_filter_screen(int col, int brightness);
void rl_rec_smoothie_filter(RenderCmdKind kind);  // RC_ICED_BLUR / RC_LAVA_FILTER / RC_WATER_FILTER / RC_BLUR

#endif /* RENDER_LIST_H */
