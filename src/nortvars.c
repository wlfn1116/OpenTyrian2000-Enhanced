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
#include "nortvars.h"

#include "config.h"
#include "file.h"
#include "joystick.h"
#include "keyboard.h"
#include "opentyr.h"
#include "vga256d.h"
#include "video.h"

#include <assert.h>
#include <ctype.h>

JE_boolean inputDetected;

JE_boolean JE_anyButton(void)
{
	poll_joysticks();
	service_SDL_events(true);
	return newkey || mousedown || joydown;
}

// Classic vertical shade progression for the 9x2 bands, indexed by band z (0 = bottom):
// 0,0,0,1,1,2,2,3,3,... i.e. the darkest shade for the first three bands then +1 every two.
// Matches the original zWait counter exactly; the Down direction mirrors it within the bar.
static int dbar_voffset(int z)
{
	return z < 1 ? 0 : (z - 1) / 2;
}

// Draw one of the vertical HUD gauges: 9 pixels wide (x..x+8), num+1 stacked 2px bands rising
// from y, shaded as a gradient. dir (GaugeGradientDir) picks the gradient axis/direction:
//   Up    - classic, brightest at the top of the bar
//   Down  - vertical, brightest at the bottom (Up mirrored within the current fill)
//   Left  - horizontal, brightest at the left column
//   Right - horizontal, brightest at the right column
// The Left/Right modes paint the same rows the vertical bars cover (bar top .. y), so the
// empty area above is still cleared the usual way by JE_wipeShieldArmorBars.
void JE_dBar3(SDL_Surface *surface, JE_integer x,  JE_integer y,  JE_integer num,  JE_integer col,  JE_integer dir)
{
	col += 2;

	if (num < 0)
		return;

	if (dir == GAUGE_GRAD_LEFT || dir == GAUGE_GRAD_RIGHT)
	{
		// Horizontal gradient: nine 1px-wide, full-height stripes whose shade steps across
		// the width. Same vertical extent as the stacked bands (bottom row y, top row y-2*num-1).
		// Lifted +2 shades so the horizontal ramp reads a touch brighter (still in-family; the
		// vertical bar's upper bands reach higher still).
		const int yBot = y;
		const int yTop = y - (2 * num + 1);
		for (int j = 0; j <= 8; j++)
		{
			const int off = (dir == GAUGE_GRAD_RIGHT) ? j : (8 - j);
			fill_rectangle_xy(surface, x + j, yTop, x + j, yBot, (Uint8)(col + 2 + off));
		}
		return;
	}

	// Vertical gradient: Up = classic; Down = the same shading mirrored top-to-bottom.
	for (JE_integer z = 0; z <= num; z++)
	{
		const int off = (dir == GAUGE_GRAD_DOWN) ? dbar_voffset(num - z) : dbar_voffset(z);
		JE_rectangle(surface, x, y - 1, x + 8, y, (Uint8)(col + off)); /* <MXD> SEGa000 */
		y -= 2;
	}
}

void JE_barDrawShadow(SDL_Surface *surface, JE_word x, JE_word y, JE_word res, JE_word col, JE_word amt, JE_word xsize, JE_word ysize)
{
	xsize--;
	ysize--;

	for (int z = 1; z <= amt / res; z++)
	{
		JE_barShade(surface, x+2, y+2, x+xsize+2, y+ysize+2);
		fill_rectangle_xy(surface, x, y, x+xsize, y+ysize, col+12);
		fill_rectangle_xy(surface, x, y, x+xsize, y, col+13);
		JE_pix(surface, x, y, col+15);
		fill_rectangle_xy(surface, x, y+ysize, x+xsize, y+ysize, col+11);
		x += xsize + 2;
	}

	amt %= res;
	if (amt > 0)
	{
		JE_barShade(surface, x+2, y+2, x+xsize+2, y+ysize+2);
		fill_rectangle_xy(surface, x,y, x+xsize, y+ysize, col+(12 / res * amt));
	}
}

// Recolour a single bar of a JE_barDrawShadow slider, at 1-based slot `mark`, using colour
// bank `col`. Mirrors JE_barDrawShadow's per-bar geometry (same xsize/ysize) and step, so it
// lands exactly on the mark-th bar. Used to flag a fixed reference value -- the middle/default
// of the touch-sensitivity sliders -- in a contrasting colour, drawn whether or not that slot
// currently holds a filled bar. No JE_barShade here: when the slot is filled the underlying
// JE_barDrawShadow bar already cast its drop-shadow (re-shading would darken the next bar); when
// it's empty a flat marker still reads clearly.
void JE_barDrawMark(SDL_Surface *surface, JE_word x, JE_word y, JE_word col, JE_word mark, JE_word xsize, JE_word ysize)
{
	if (mark == 0)
		return;

	xsize--;
	ysize--;

	x += (mark - 1) * (xsize + 2);  // JE_barDrawShadow advances x by xsize+2 per bar

	fill_rectangle_xy(surface, x, y, x+xsize, y+ysize, col+12);
	fill_rectangle_xy(surface, x, y, x+xsize, y, col+13);
	JE_pix(surface, x, y, col+15);
	fill_rectangle_xy(surface, x, y+ysize, x+xsize, y+ysize, col+11);
}

void JE_wipeKey(void)
{
	// /!\ Doesn't seems to affect anything.
}
