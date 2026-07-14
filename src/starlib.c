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
#include "starlib.h"

#include "keyboard.h"
#include "mtrand.h"
#include "opentyr.h"
#include "video.h"

#include <ctype.h>

#define starlib_MAX_STARS 2500  // warp-star field density for intros/interludes
#define MAX_TYPES 14

struct JE_StarType
{
	JE_integer spX, spY;
	JE_real spZ;  // fractional so movement can be time-scaled per rendered frame
	JE_integer lastX, lastY;
};

static int tempX, tempY;
static JE_boolean run;
static struct JE_StarType star[starlib_MAX_STARS];

static JE_byte setup;
static JE_word stepCounter;

static JE_word nsp2;
static JE_shortint nspVar2Inc;

/* JE: new sprite pointer */
static JE_real nsp;
static JE_real nspVarInc;
static JE_real nspVarVarInc;

static JE_word changeTime;
static JE_boolean doChange;

static JE_boolean grayB;

static JE_integer starlib_speed;
static JE_shortint speedChange;

static JE_byte pColor;

// Fill a scale x scale block at hi-buffer pixel (hx, hy) with `col`, clipped to
// the surface. Used to draw supersampled stars: each classic 1-pixel cell of the
// star cross becomes an NxN block so the star keeps its on-screen size.
static void star_fill_block(Uint8 *pixels, int pitch, int w, int h, int hx, int hy, int n, Uint8 col)
{
	int x0 = hx < 0 ? 0 : hx;
	int y0 = hy < 0 ? 0 : hy;
	int x1 = hx + n > w ? w : hx + n;
	int y1 = hy + n > h ? h : hy + n;
	for (int y = y0; y < y1; ++y)
	{
		Uint8 *row = pixels + y * pitch;
		for (int x = x0; x < x1; ++x)
			row[x] = col;
	}
}

// Draw the classic star cross (centre + 2-pixel arms) supersampled by `scale`,
// centred on the sub-pixel screen position (fx, fy). round(f*scale) places the
// whole cross in 1/scale-pixel steps, so present_hi()'s downscale antialiases the
// motion instead of stepping whole pixels. Colours match the 1x path: the centre
// is `c0`, the inner arms c0+72, the outer arms c0+144 (Uint8 wrap, as before).
static void draw_star_scaled(SDL_Surface *target, float fx, float fy, Uint8 c0, int scale)
{
	Uint8 *pixels = target->pixels;
	const int pitch = target->pitch, w = target->w, h = target->h;
	const int hx = (int)(fx * scale + 0.5f);
	const int hy = (int)(fy * scale + 0.5f);
	const Uint8 c1 = c0 + 72;
	const Uint8 c2 = c0 + 144;

	star_fill_block(pixels, pitch, w, h, hx, hy, scale, c0);

	star_fill_block(pixels, pitch, w, h, hx - scale, hy, scale, c1);
	star_fill_block(pixels, pitch, w, h, hx + scale, hy, scale, c1);
	star_fill_block(pixels, pitch, w, h, hx, hy - scale, scale, c1);
	star_fill_block(pixels, pitch, w, h, hx, hy + scale, scale, c1);

	star_fill_block(pixels, pitch, w, h, hx - 2 * scale, hy, scale, c2);
	star_fill_block(pixels, pitch, w, h, hx + 2 * scale, hy, scale, c2);
	star_fill_block(pixels, pitch, w, h, hx, hy - 2 * scale, scale, c2);
	star_fill_block(pixels, pitch, w, h, hx, hy + 2 * scale, scale, c2);
}

void JE_starlib_main(float step, SDL_Surface *target, int scale)  // step = fraction of a classic ~70Hz tick elapsed
{
	int off;
	JE_word i;
	JE_real tempZ;
	JE_byte tempCol;
	struct JE_StarType *stars;
	Uint8 *surf = target->pixels;

	// Housekeeping that must stay at the classic tick rate; movement below is
	// scaled by `step` directly so it stays smooth at any render rate.
	static float tick_acc = 0.0f;
	tick_acc += step;
	for (; tick_acc >= 1.0f; tick_acc -= 1.0f)
	{
		starlib_speed += speedChange;

		if (doChange)
		{
			stepCounter++;
			if (stepCounter > changeTime)
			{
				JE_changeSetup(0);
			}
		}

		if ((mt_rand() % 1000) == 1)
		{
			nspVarVarInc = mt_rand_1() * 0.01f - 0.005f;
		}

		nspVarInc += nspVarVarInc;
	}

	JE_wackyCol();

	grayB = false;

	for (stars = star, i = starlib_MAX_STARS; i > 0; stars++, i--)
	{
		/* We don't want trails in our star field.  Erase the old graphic.
		 * (Supersampled frames are drawn into a buffer the caller clears each
		 * frame, so there is nothing to erase per-star.) */
		if (scale == 1)
		{
			off = (stars->lastX) + (stars->lastY) * vga_width;
			if (off >= vga_width * 2 && off < (vga_width * vga_height) - vga_width * 2)
			{
				surf[off] = 0; /* Shade Level 0 */

				surf[off - 1] = 0; /* Shade Level 1, 2 */
				surf[off + 1] = 0;
				surf[off - 2] = 0;
				surf[off + 2] = 0;

				surf[off - vga_width] = 0;
				surf[off + vga_width] = 0;
				surf[off - vga_width * 2] = 0;
				surf[off + vga_width * 2] = 0;
			}
		}

		/* Move star */
		tempZ = stars->spZ;
		tempX = (int)(stars->spX / tempZ) + vga_width / 2;
		tempY = (int)(stars->spY / tempZ) + vga_height / 2;

		// Sub-pixel screen position (same projection, not truncated) for the
		// supersampled draw; must use the pre-decrement z, like tempX/tempY.
		const float fx = stars->spX / tempZ + vga_width / 2.0f;
		const float fy = stars->spY / tempZ + vga_height / 2.0f;

		tempZ -= starlib_speed * step;

		/* If star is out of range, make a new one */
		if (tempZ <= 0 ||
			tempY == 0 || tempY > vga_height - 2 ||
			tempX > vga_width - 2 || tempX < 1)
		{
			stars->spZ = 500;

			JE_newStar();

			stars->spX = tempX;
			stars->spY = tempY;
		}
		else /* Otherwise, update & draw it */
		{
			stars->lastX = tempX;
			stars->lastY = tempY;
			stars->spZ = tempZ;

			if (grayB)
				tempCol = (int)tempZ >> 1;
			else
				tempCol = pColor+(((int)tempZ >> 4) & 31);

			if (scale != 1)
			{
				draw_star_scaled(target, fx, fy, tempCol, scale);
			}
			else
			{
				off = tempX + tempY * vga_width;

				/* Draw the pixel! */
				if (off >= vga_width * 2 && off < (vga_width * vga_height) - vga_width * 2)
				{
					surf[off] = tempCol;

					tempCol += 72;
					surf[off - 1] = tempCol;
					surf[off + 1] = tempCol;
					surf[off - vga_width] = tempCol;
					surf[off + vga_width] = tempCol;

					tempCol += 72;
					surf[off - 2] = tempCol;
					surf[off + 2] = tempCol;
					surf[off - vga_width * 2] = tempCol;
					surf[off + vga_width * 2] = tempCol;
				}
			}
		}
	}

	if (newkey)
	{
		char key = 0;

		if ((lastkey_mod & (KMOD_CTRL | KMOD_SHIFT | KMOD_ALT | KMOD_GUI)) == KMOD_NONE)
		{
			switch (lastkey_scan)
			{
			case SDL_SCANCODE_C: key = 'c'; break;
			case SDL_SCANCODE_P: key = 'p'; break;
			case SDL_SCANCODE_S: key = 's'; break;
			case SDL_SCANCODE_X: key = 'x'; break;
			case SDL_SCANCODE_1: key = '1'; break;
			case SDL_SCANCODE_2: key = '2'; break;
			case SDL_SCANCODE_3: key = '3'; break;
			case SDL_SCANCODE_4: key = '4'; break;
			case SDL_SCANCODE_5: key = '5'; break;
			case SDL_SCANCODE_6: key = '6'; break;
			case SDL_SCANCODE_7: key = '7'; break;
			case SDL_SCANCODE_8: key = '8'; break;
			case SDL_SCANCODE_9: key = '9'; break;
			case SDL_SCANCODE_0: key = '0'; break;
			case SDL_SCANCODE_ESCAPE: key = 27; break;
			case SDL_SCANCODE_MINUS: key = '-'; break;
			case SDL_SCANCODE_LEFTBRACKET: key = '['; break;
			case SDL_SCANCODE_RIGHTBRACKET: key = ']'; break;
			case SDL_SCANCODE_GRAVE: key = '`'; break;
			case SDL_SCANCODE_KP_MINUS: key = '-'; break;
			case SDL_SCANCODE_KP_PLUS: key = '+'; break;
			case SDL_SCANCODE_KP_1: key = '1'; break;
			case SDL_SCANCODE_KP_2: key = '2'; break;
			case SDL_SCANCODE_KP_3: key = '3'; break;
			case SDL_SCANCODE_KP_4: key = '4'; break;
			case SDL_SCANCODE_KP_5: key = '5'; break;
			case SDL_SCANCODE_KP_6: key = '6'; break;
			case SDL_SCANCODE_KP_7: key = '7'; break;
			case SDL_SCANCODE_KP_8: key = '8'; break;
			case SDL_SCANCODE_KP_9: key = '9'; break;
			case SDL_SCANCODE_KP_0: key = '0'; break;
			default: break;
			}
		}
		else if ((lastkey_mod & KMOD_SHIFT) != KMOD_NONE &&
		         (lastkey_mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) == KMOD_NONE)
		{
			switch (lastkey_scan)
			{
			case SDL_SCANCODE_C: key = 'C'; break;
			case SDL_SCANCODE_P: key = 'P'; break;
			case SDL_SCANCODE_S: key = 'S'; break;
			case SDL_SCANCODE_X: key = 'X'; break;
			case SDL_SCANCODE_1: key = '!'; break;
			case SDL_SCANCODE_2: key = '@'; break;
			case SDL_SCANCODE_3: key = '#'; break;
			case SDL_SCANCODE_4: key = '$'; break;
			case SDL_SCANCODE_EQUALS: key = '+'; break;
			case SDL_SCANCODE_LEFTBRACKET: key = '{'; break;
			case SDL_SCANCODE_RIGHTBRACKET: key = '}'; break;
			default: break;
			}
		}

		switch (toupper(key))
		{
			case '+':
				starlib_speed++;
				speedChange = 0;
				break;
			case '-':
				starlib_speed--;
				speedChange = 0;
				break;
			case '1':
				JE_changeSetup(1);
				break;
			case '2':
				JE_changeSetup(2);
				break;
			case '3':
				JE_changeSetup(3);
				break;
			case '4':
				JE_changeSetup(4);
				break;
			case '5':
				JE_changeSetup(5);
				break;
			case '6':
				JE_changeSetup(6);
				break;
			case '7':
				JE_changeSetup(7);
				break;
			case '8':
				JE_changeSetup(8);
				break;
			case '9':
				JE_changeSetup(9);
				break;
			case '0':
				JE_changeSetup(10);
				break;
			case '!':
				JE_changeSetup(11);
				break;
			case '@':
				JE_changeSetup(12);
				break;
			case '#':
				JE_changeSetup(13);
				break;
			case '$':
				JE_changeSetup(14);
				break;

			case 'C':
				JE_resetValues();
				break;
			case 'S':
				nspVarVarInc = mt_rand_1() * 0.01f - 0.005f;
				break;
			case 'X':
			case 27:
				run = false;
				break;
			case '[':
				pColor--;
				break;
			case ']':
				pColor++;
				break;
			case '{':
				pColor -= 72;
				break;
			case '}':
				pColor += 72;
				break;
			case '`':
				doChange = !doChange;
				break;
			case 'P':
				wait_noinput(true, false, false);
				wait_input(true, false, false);
				break;
			default:
				break;
		}
	}
}

void JE_wackyCol(void)
{
	/* YKS: Does nothing */
}

void JE_starlib_init(void)
{
	static JE_boolean initialized = false;

	if (!initialized)
	{
		initialized = true;

		JE_resetValues();
		JE_changeSetup(2);
		doChange = true;

		/* RANDOMIZE; */
		for (int x = 0; x < starlib_MAX_STARS; x++)
		{
			star[x].spX = (mt_rand() % 64000) - 32000;
			star[x].spY = (mt_rand() % 40000) - 20000;
			star[x].spZ = x+1;
		}
	}
}

void JE_resetValues(void)
{
	nsp2 = 1;
	nspVar2Inc = 1;
	nspVarInc = 0.1f;
	nspVarVarInc = 0.0001f;
	nsp = 0;
	pColor = 32;
	starlib_speed = 2;
	speedChange = 0;
}

void JE_changeSetup(JE_byte setupType)
{
	stepCounter = 0;
	changeTime = (mt_rand() % 1000);

	if (setupType > 0)
		setup = setupType;
	else
		setup = mt_rand() % (MAX_TYPES + 1);

	if (setup == 1)
		nspVarInc = 0.1f;
	if (nspVarInc > 2.2f)
		nspVarInc = 0.1f;
}

void JE_newStar(void)
{
	if (setup == 0)
	{
		tempX = (mt_rand() % 64000) - 32000;
		tempY = (mt_rand() % 40000) - 20000;
	}
	else
	{
		nsp = nsp + nspVarInc; /* YKS: < lol */
		switch (setup)
		{
			case 1:
				tempX = (int)(sinf(nsp / 30) * 20000);
				tempY = (mt_rand() % 40000) - 20000;
				break;
			case 2:
				tempX = (int)(cosf(nsp) * 20000);
				tempY = (int)(sinf(nsp) * 20000);
				break;
			case 3:
				tempX = (int)(cosf(nsp * 15) * 100) * ((int)(nsp / 6) % 200);
				tempY = (int)(sinf(nsp * 15) * 100) * ((int)(nsp / 6) % 200);
				break;
			case 4:
				tempX = (int)(sinf(nsp / 60) * 20000);
				tempY = (int)(cosf(nsp) * (int)(sinf(nsp / 200) * 300) * 100);
				break;
			case 5:
				tempX = (int)(sinf(nsp / 2) * 20000);
				tempY = (int)(cosf(nsp) * (int)(sinf(nsp / 200) * 300) * 100);
				break;
			case 6:
				tempX = (int)(sinf(nsp) * 40000);
				tempY = (int)(cosf(nsp) * 20000);
				break;
			case 8:
				tempX = (int)(sinf(nsp / 2) * 40000);
				tempY = (int)(cosf(nsp) * 20000);
				break;
			case 7:
				tempX = mt_rand() % 65535;
				if ((mt_rand() % 2) == 0)
					tempY = (int)(cosf(nsp / 80) * 10000) + 15000;
				else
					tempY = 50000 - (int)(cosf(nsp / 80) * 13000);
				break;
			case 9:
				nsp2 += nspVar2Inc;
				if ((nsp2 == 65535) || (nsp2 == 0))
					nspVar2Inc = -nspVar2Inc;
				tempX = (int)(cosf(sinf(nsp2 / 10.0f) + (nsp / 500)) * 32000);
				tempY = (int)(sinf(cosf(nsp2 / 10.0f) + (nsp / 500)) * 30000);
				break;
			case 10:
				nsp2 += nspVar2Inc;
				if ((nsp2 == 65535) || (nsp2 == 0))
					nspVar2Inc = -nspVar2Inc;
				tempX = (int)(cosf(sinf(nsp2 / 5.0f) + (nsp / 100)) * 32000);
				tempY = (int)(sinf(cosf(nsp2 / 5.0f) + (nsp / 100)) * 30000);
				break;;
			case 11:
				nsp2 += nspVar2Inc;
				if ((nsp2 == 65535) || (nsp2 == 0))
					nspVar2Inc = -nspVar2Inc;
				tempX = (int)(cosf(sinf(nsp2 / 1000.0f) + (nsp / 2)) * 32000);
				tempY = (int)(sinf(cosf(nsp2 / 1000.0f) + (nsp / 2)) * 30000);
				break;
			case 12:
				if (nsp != 0)
				{
					nsp2 += nspVar2Inc;
					if ((nsp2 == 65535) || (nsp2 == 0))
						nspVar2Inc = -nspVar2Inc;
					tempX = (int)(cosf(sinf(nsp2 / 2.0f) / (sqrtf(fabsf(nsp)) / 10.0f + 1) + (nsp2 / 100.0f)) * 32000);
					tempY = (int)(sinf(cosf(nsp2 / 2.0f) / (sqrtf(fabsf(nsp)) / 10.0f + 1) + (nsp2 / 100.0f)) * 30000);
				}
				break;
			case 13:
				if (nsp != 0)
				{
					nsp2 += nspVar2Inc;
					if ((nsp2 == 65535) || (nsp2 == 0))
						nspVar2Inc = -nspVar2Inc;
					tempX = (int)(cosf(sinf(nsp2 / 10.0f) / 2 + (nsp / 20)) * 32000);
					tempY = (int)(sinf(sinf(nsp2 / 11.0f) / 2 + (nsp / 20)) * 30000);
				}
				break;
			case 14:
				nsp2 += nspVar2Inc;
				tempX = (int)((sinf(nsp) + cosf(nsp2 / 1000.0f) * 3) * 12000);
				tempY = (int)(cosf(nsp) * 10000) + nsp2;
				break;
		}
	}
}
