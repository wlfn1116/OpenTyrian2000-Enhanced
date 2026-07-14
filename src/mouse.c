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
#include "mouse.h"

#include "keyboard.h"
#include "nortvars.h"
#include "sprite.h"
#include "video.h"
#include "vga256d.h"

#if defined(TARGET_GP2X) || defined(TARGET_DINGUX)
bool has_mouse = false;
#else
bool has_mouse = true;
#endif

bool mouseInactive = true;
JE_byte mouseCursor;
JE_word mouseX, mouseY, mouseButton;
JE_word mouseXB, mouseYB;

// On the consoles (Switch / Vita) the pointer is the touchscreen; an on-screen cursor
// sprite misleads (it's absolute in menus and irrelevant to the relative "trackpad" ship
// control), so the sprite is suppressed there. Pixel grab/restore still runs, so nothing smears.
#if defined(__SWITCH__) || defined(__vita__)
#define MOUSE_CURSOR_HIDDEN 1
#else
#define MOUSE_CURSOR_HIDDEN 0
#endif

static JE_word mouseGrabX, mouseGrabY;
static JE_byte mouseGrabShape[24 * 28];

// True when JE_mouseStart/Filter grabbed the pixels under the cursor on VGAScreen, so
// JE_mouseReplace must restore them. False in centered-menu mode.
static bool mouseGrabbed = false;

// Set in centered-menu mode: the next JE_showVGA composites the cursor onto the
// pillarboxed frame (JE_drawMouseToMenuScreen). filter 0 means unfiltered.
static bool cursorPresentPending = false;
static bool cursorPresentFiltered = false;
static Uint8 cursorPresentFilter = 0;

static void JE_drawShapeTypeOne(JE_word x, JE_word y, JE_byte *shape)
{
	JE_word xloop = 0, yloop = 0;
	JE_byte *p = shape; /* shape pointer */
	Uint8 *s;   /* screen pointer, 8-bit specific */
	Uint8 *s_limit; /* buffer boundary */

	s = (Uint8 *)VGAScreen->pixels;
	s += y * VGAScreen->pitch + x;

	s_limit = (Uint8 *)VGAScreen->pixels;
	s_limit += VGAScreen->h * VGAScreen->pitch;

	for (yloop = 0; yloop < 28; yloop++)
	{
		for (xloop = 0; xloop < 24; xloop++)
		{
			if (s >= s_limit)
				return;
			*s = *p;
			s++; p++;
		}
		s -= 24;
		s += VGAScreen->pitch;
	}
}

static void JE_grabShapeTypeOne(JE_word x, JE_word y, JE_byte *shape)
{
	JE_word xloop = 0, yloop = 0;
	JE_byte *p = shape; /* shape pointer */
	Uint8 *s;   /* screen pointer, 8-bit specific */
	Uint8 *s_limit; /* buffer boundary */

	s = (Uint8 *)VGAScreen->pixels;
	s += y * VGAScreen->pitch + x;

	s_limit = (Uint8 *)VGAScreen->pixels;
	s_limit += VGAScreen->h * VGAScreen->pitch;

	for (yloop = 0; yloop < 28; yloop++)
	{
		for (xloop = 0; xloop < 24; xloop++)
		{
			if (s >= s_limit)
				return;
			*p = *s;
			s++; p++;
		}
		s -= 24;
		s += VGAScreen->pitch;
	}
}

typedef struct
{
	Uint16 index;
	Uint8 x;
	Uint8 y;
	Uint8 w;
	Uint8 h;
	Uint8 fx;
	Uint8 fy;
} MousePointerSpriteInfo;

static const MousePointerSpriteInfo mousePointerSprites[] = // fka mouseCursorGr
{
	{ 273, 0, 0, 11, 16,  0,  0 },
	{ 275, 0, 0, 21, 16, 10,  8 },
	{ 277, 0, 0, 21, 16, 10,  7 },
	{ 279, 0, 0, 16, 21,  8, 10 },
	{ 281, 8, 0, 16, 21,  7, 10 },
};

void JE_mouseStart(void)
{
	if (has_mouse)
	{
		service_SDL_events(false);

		mouseButton = mousedown ? lastmouse_but : 0; /* incorrect, possibly unimportant */

		// Pillarboxed menus build a side gradient from the frame's edge columns; a cursor
		// drawn into VGAScreen here would smear into it near an edge. So defer: JE_showVGA
		// composites the cursor after the gradient is built from clean content.
		if (video_get_menu_x_offset() != 0)
		{
			mouseGrabbed = false;
			cursorPresentPending = true;
			cursorPresentFiltered = false;
			return;
		}

		const MousePointerSpriteInfo *spriteInfo = &mousePointerSprites[mouseCursor];

		mouseGrabX = MIN(MAX(spriteInfo->fx, mouse_x), vga_width - (spriteInfo->w - spriteInfo->fx)) - spriteInfo->fx;
		mouseGrabY = MIN(MAX(spriteInfo->fy, mouse_y), vga_height - (spriteInfo->h - spriteInfo->fy)) - spriteInfo->fy;

		JE_grabShapeTypeOne(mouseGrabX, mouseGrabY, mouseGrabShape);
		mouseGrabbed = true;

		if (!mouseInactive && !MOUSE_CURSOR_HIDDEN)
		{
			const Sint32 x = mouse_x - spriteInfo->x - spriteInfo->fx;
			const Sint32 y = mouse_y - spriteInfo->y - spriteInfo->fy;
			blit_sprite2x2_clip(VGAScreen, x, y, shopSpriteSheet, spriteInfo->index);
		}
	 }
}

// Composite the cursor onto the final pillarboxed menu frame (dst) at the live pointer
// position shifted by the centering offset. Called by JE_showVGA after the side gradient
// is built, so the cursor draws cleanly over the full width.
void JE_drawMouseToMenuScreen(SDL_Surface *dst, int x_offset)
{
	if (!has_mouse || !cursorPresentPending)
		return;
	cursorPresentPending = false;

	if (mouseInactive || MOUSE_CURSOR_HIDDEN)
		return;

	const MousePointerSpriteInfo *spriteInfo = &mousePointerSprites[mouseCursor];
	const Sint32 x = mouse_x - spriteInfo->x - spriteInfo->fx + x_offset;
	const Sint32 y = mouse_y - spriteInfo->y - spriteInfo->fy;
	if (cursorPresentFiltered)
		blit_sprite2x2_filter_clip(dst, x, y, shopSpriteSheet, spriteInfo->index, cursorPresentFilter);
	else
		blit_sprite2x2_clip(dst, x, y, shopSpriteSheet, spriteInfo->index);
}

void JE_mouseStartFilter(Uint8 filter)
{
	if (has_mouse)
	{
		mouseButton = mousedown ? lastmouse_but : 0; /* incorrect, possibly unimportant */

		// Same pillarbox deferral as JE_mouseStart, carrying the darken filter
		// (the title screen uses this variant).
		if (video_get_menu_x_offset() != 0)
		{
			mouseGrabbed = false;
			cursorPresentPending = true;
			cursorPresentFiltered = true;
			cursorPresentFilter = filter;
			return;
		}

		const MousePointerSpriteInfo *spriteInfo = &mousePointerSprites[mouseCursor];

		mouseGrabX = MIN(MAX(spriteInfo->fx, mouse_x), vga_width - (spriteInfo->w - spriteInfo->fx)) - spriteInfo->fx;
		mouseGrabY = MIN(MAX(spriteInfo->fy, mouse_y), vga_height - (spriteInfo->h - spriteInfo->fy)) - spriteInfo->fy;

		JE_grabShapeTypeOne(mouseGrabX, mouseGrabY, mouseGrabShape);
		mouseGrabbed = true;

		if (!mouseInactive && !MOUSE_CURSOR_HIDDEN)
		{
			const Sint32 x = mouse_x - spriteInfo->x - spriteInfo->fx;
			const Sint32 y = mouse_y - spriteInfo->y - spriteInfo->fy;
			blit_sprite2x2_filter_clip(VGAScreen, x, y, shopSpriteSheet, spriteInfo->index, filter);
		}
	}
}

void JE_mouseReplace(void)
{
	// Only restore when JE_mouseStart/Filter grabbed on VGAScreen; in centered-menu
	// mode the cursor lives on the composited frame instead.
	if (has_mouse && mouseGrabbed)
	{
		JE_drawShapeTypeOne(mouseGrabX, mouseGrabY, mouseGrabShape);
		mouseGrabbed = false;
	}
}
