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
#include "mainint.h"

#include "backgrnd.h"
#include "config.h"
#include "crashlog.h"
#include "editship.h"
#include "endless.h"
#include "episodes.h"
#include "file.h"
#include "font.h"
#include "fonthand.h"
#include "game_menu.h"
#include "helptext.h"
#include "joystick.h"
#include "keyboard.h"
#include "lds_play.h"
#include "loudness.h"
#include "menus.h"
#include "mouse.h"
#include "mtrand.h"
#include "musmast.h"
#include "network.h"
#include "nortsong.h"
#include "nortvars.h"
#include "opentyr.h"
#include "palette.h"
#include "params.h"
#include "pcxmast.h"
#include "picload.h"
#include "player.h"
#include "render_list.h"
#include "shots.h"
#include "sndmast.h"
#include "sprite.h"
#include "console_platform.h"
#include "tyrian2.h"
#include "varz.h"
#include "vga256d.h"
#include "video.h"
#include "lvlmast.h"

#include <assert.h>
#include <ctype.h>
#include <string.h>
#include <limits.h>

bool button[4];

#define MAX_PAGE 8
#define TOPICS 6
const JE_byte topicStart[TOPICS] = { 0, 1, 2, 3, 7, 255 };

JE_shortint constantLastX;
JE_word textErase;
ulong upgradeCost;
ulong downgradeCost;
JE_boolean performSave;
JE_boolean jumpSection;
JE_boolean useLastBank; /* See if I want to use the last 16 colors for DisplayText */

bool pause_pressed = false, ingamemenu_pressed = false, changefire_pressed = false;

/* debug submenu dimensions for in-game setup */
#define DEBUG_MENU_WIDTH  255
/* Centre the panel horizontally in the playfield (left of the HUD) so it stays
 * centred under the widescreen layout. */
#define DEBUG_MENU_X      ((PLAYFIELD_WIDTH - DEBUG_MENU_WIDTH) / 2)
#define DEBUG_MENU_Y      5
/* total height of debug menu area */
#define DEBUG_MENU_HEIGHT (vga_height - 5 - DEBUG_MENU_Y + 1)

static Uint8 debug_menu_backup[DEBUG_MENU_WIDTH * DEBUG_MENU_HEIGHT];

/* Draws a message at the bottom text window on the playing screen */
void JE_drawTextWindow(const char *text)
{
	if (textErase > 0) // erase current text
		blit_sprite(VGAScreenSeg, 16, vga_height - 11, OPTION_SHAPES, 36);  // in-game text area

	textErase = 100;
	JE_outText(VGAScreenSeg, 20, vga_height - 10, text, 0, 4);
}

// As JE_drawTextWindow, but splits the line: `left` stays left-aligned in the normal x=20 slot,
// while `right` is right-aligned so its rightmost pixel lands on right_x (x = right_x - textWidth,
// matching the game's other right-aligned HUD readouts). The shared message-bar background
// (OPTION_SHAPES 36 at x=16) spans well past the usable right_x range, so the same erase path
// clears both. Endless uses this for the elite/champion kill line (label left, bounty right).
void JE_drawTextWindowSplit(const char *left, const char *right, int right_x)
{
	if (textErase > 0) // erase current text
		blit_sprite(VGAScreenSeg, 16, vga_height - 11, OPTION_SHAPES, 36);  // in-game text area

	textErase = 100;
	JE_outText(VGAScreenSeg, 20, vga_height - 10, left, 0, 4);
	JE_outText(VGAScreenSeg, right_x - JE_textWidth(right, TINY_FONT), vga_height - 10, right, 0, 4);
}

void JE_outCharGlow(JE_word x, JE_word y, const char *s)
{
	JE_integer maxloc, loc, z;
	JE_shortint glowcol[60]; /* [1..60] */
	JE_shortint glowcolc[60]; /* [1..60] */
	JE_word textloc[60]; /* [1..60] */
	JE_byte bank;

	setDelay2(1);

	bank = (warningRed) ? 7 : ((useLastBank) ? 15 : 14);

	if (s[0] == '\0')
		return;

	if (frameCountMax == 0)
	{
		JE_textShade(VGAScreen, x, y, s, bank, 0, PART_SHADE);
		JE_showVGA();
	}
	else
	{
		maxloc = strlen(s);
		for (z = 0; z < 60; z++)
		{
			glowcol[z] = -8;
			glowcolc[z] = 1;
		}

		loc = x;
		for (z = 0; z < maxloc; z++)
		{
			textloc[z] = loc;

			int sprite_id = font_ascii[(unsigned char)s[z]];

			if (s[z] == ' ')
				loc += 6;
			else if (sprite_id != -1)
				loc += sprite(TINY_FONT, sprite_id)->width + 1;
		}

		for (loc = 0; (unsigned)loc < strlen(s) + 28; loc++)
		{
			if (!ESCPressed)
			{
				setDelay(frameCountMax);

				NETWORK_KEEP_ALIVE();

				int sprite_id = -1;

				for (z = loc - 28; z <= loc; z++)
				{
					if (z >= 0 && z < maxloc)
					{
						sprite_id = font_ascii[(unsigned char)s[z]];

						if (sprite_id != -1)
						{
							blit_sprite_hv(VGAScreen, textloc[z], y, TINY_FONT, sprite_id, bank, glowcol[z]);

							glowcol[z] += glowcolc[z];
							if (glowcol[z] > 9)
								glowcolc[z] = -1;
						}
					}
				}
				if (sprite_id != -1 && --z < maxloc)
					blit_sprite_dark(VGAScreen, textloc[z] + 1, y + 1, TINY_FONT, sprite_id, true);

				if (JE_anyButton())
					frameCountMax = 0;

				do
				{
					if (levelWarningDisplay)
						JE_updateWarning(VGAScreen);

					SDL_Delay(16);
				} while (!(getDelayTicks() == 0 || ESCPressed));

				JE_showVGA();
			}
		}
	}
}

void JE_drawPortConfigButtons(void) // rear weapon pattern indicator
{
	if (twoPlayerMode)
		return;

	const int x_lit = HUD_X(285);
	const int x_unlit = HUD_X(302);
	if (player[0].weapon_mode == 1)
	{
		blit_sprite(VGAScreenSeg, x_lit, 44, OPTION_SHAPES, 18);  // lit
		blit_sprite(VGAScreenSeg, x_unlit, 44, OPTION_SHAPES, 19);  // unlit
	}
	else // == 2
	{
		blit_sprite(VGAScreenSeg, x_lit, 44, OPTION_SHAPES, 19);  // unlit
		blit_sprite(VGAScreenSeg, x_unlit, 44, OPTION_SHAPES, 18);  // lit
	}
}

static bool helpSystemPage(Uint8 *topic, bool *restart);

void JE_helpSystem(JE_byte startTopic)
{
	if (shopSpriteSheet.data == NULL)
		JE_loadCompShapes(&shopSpriteSheet, '1');  // need mouse pointer sprites

	Uint8 topic = startTopic;

	bool restart = true;

	const size_t menuItemsCount = COUNTOF(topicName) - 1;
	size_t selectedIndex = 0;

	/* Menus render on a 320px virtual screen centered in the wider VGA buffer; center on
	 * LEGACY_WIDTH, not vga_width, or text drifts when the menu is blitted over. */
	const int xCenter = LEGACY_WIDTH / 2;
	const int yMenuHeader = 30;
	const int yMenuItems = 60;
	/* reduce spacing to fit new Debug option */
	const int dyMenuItems = 17;
	const int hMenuItem = 13;
	int wMenuItem[COUNTOF(topicName) - 1] = { 0 };

	for (; ; )
	{
		if (restart)
		{
			play_song(SONG_MAPVIEW);

			JE_loadPic(VGAScreen2, 2, false);
		}

		if (topic > 1)
		{
			if (!helpSystemPage(&topic, &restart))
				return;

			selectedIndex = (size_t)topic - 1;
			topic = 1;
			continue;
		}

		// Restore background.
		memcpy(VGAScreen->pixels, VGAScreen2->pixels, (size_t)VGAScreen->pitch * VGAScreen->h);

		// Draw header.
		draw_font_hv_shadow(VGAScreen, xCenter, yMenuHeader, topicName[0], large_font, centered, 15, -3, false, 2);

		// Draw menu items.
		for (size_t i = 0; i < menuItemsCount; ++i)
		{
			const char *const text = topicName[i + 1];

			wMenuItem[i] = JE_textWidth(text, normal_font);
			const int y = yMenuItems + dyMenuItems * i;

			const bool selected = i == selectedIndex;

			draw_font_hv_shadow(VGAScreen, xCenter, y, text, normal_font, centered, 15, -3 + (selected ? 2 : 0), false, 2);
		}

		mouseCursor = MOUSE_POINTER_NORMAL;

		if (restart)
		{
			fade_palette(colors, 10, 0, 255);

			restart = false;
		}

		service_SDL_events(true);

		JE_mouseStart();
		JE_showVGA();
		JE_mouseReplace();

		bool mouseMoved = false;
		do
		{
			SDL_Delay(1);  // fine poll so the cursor redraws at display rate on motion

			Uint16 oldMouseX = mouse_x;
			Uint16 oldMouseY = mouse_y;

			push_joysticks_as_keyboard();
			service_SDL_events(false);

			mouseMoved = mouse_x != oldMouseX || mouse_y != oldMouseY;
		} while (!(newkey || newmouse || mouseMoved));

		// Handle interaction.

		bool action = false;
		bool done = false;

		if (mouseMoved || newmouse)
		{
			// Find menu item that was hovered or clicked.
			for (size_t i = 0; i < menuItemsCount; ++i)
			{
				const int xMenuItem = xCenter - wMenuItem[i] / 2;
				if (mouse_x >= xMenuItem && mouse_x < xMenuItem + wMenuItem[i])
				{
					const int yMenuItem = yMenuItems + dyMenuItems * i;
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
					? menuItemsCount - 1
					: selectedIndex - 1;
				break;
			}
			case SDL_SCANCODE_DOWN:
			{
				JE_playSampleNum(S_CURSOR);

				selectedIndex = selectedIndex == menuItemsCount - 1
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
				break;
			}
			default:
				break;
			}
		}

		if (action)
		{
			JE_playSampleNum(S_SELECT);

			topic = selectedIndex + 2;

			if (selectedIndex == menuItemsCount - 1)
				done = true;
		}

		if (done)
		{
			fade_black(15);

			return;
		}
	}
}

static bool helpSystemPage(Uint8 *topic, bool *restart)
{
	Uint8 page = topicStart[*topic - 1];

	/* See comment in JE_helpSystem regarding the virtual screen width. */
	const int xCenter = LEGACY_WIDTH / 2;

	for (; ; )
	{
		if (page == 0)
		{
			*topic = 1;
			return true;
		}
		else if (page > MAX_PAGE)
		{
			*topic = COUNTOF(topicName) - 1;
			return true;
		}

		for (Uint8 temp = 0; temp < COUNTOF(topicName); ++temp)
		{
			if (topicStart[temp] <= page)
				*topic = temp + 1;
			else
				break;
		}

		// Restore background.
		memcpy(VGAScreen->pixels, VGAScreen2->pixels, (size_t)VGAScreen->pitch * VGAScreen->h);

		fill_rectangle_wh(VGAScreen, 0, vga_height - 8, vga_width, 8, 0);

		const char *const text = topicName[*topic - 1];

		// Draw header.
		draw_font_hv_shadow(VGAScreen, xCenter, 1, text, normal_font, centered, 15, -3, false, 2);

		// Draw footer.
		JE_char buffer[128];

		snprintf(buffer, sizeof buffer, "%s %d", miscText[24], page - topicStart[*topic - 1] + 1);
		draw_font_hv(VGAScreen, 10, vga_height - 8, buffer, small_font, left_aligned, 13, 5);

		snprintf(buffer, sizeof buffer, "%s %d of %d", miscText[25], page, MAX_PAGE);
		draw_font_hv(VGAScreen, vga_width - 10, vga_height - 8, buffer, small_font, right_aligned, 13, 5);

		// Draw text.

		helpBoxBrightness = 3;
		verticalHeight = 8;

		switch (page)
		{
		case 1: /* One-Player Menu */
			JE_HBox(VGAScreen, 10,  20,  2, 60);
			JE_HBox(VGAScreen, 10,  50,  5, 60);
			JE_HBox(VGAScreen, 10,  80, 21, 60);
			JE_HBox(VGAScreen, 10, 110,  1, 60);
			JE_HBox(VGAScreen, 10, 140, 28, 60);
			break;
		case 2: /* Two-Player Menu */
			JE_HBox(VGAScreen, 10,  20,  1, 60);
			JE_HBox(VGAScreen, 10,  60,  2, 60);
			JE_HBox(VGAScreen, 10, 100, 21, 60);
			JE_HBox(VGAScreen, 10, 140, 28, 60);
			break;
		case 3: /* Upgrade Ship */
			JE_HBox(VGAScreen, 10,  20,  5, 60);
			JE_HBox(VGAScreen, 10,  70,  6, 60);
			JE_HBox(VGAScreen, 10, 110,  7, 60);
			break;
		case 4:
			JE_HBox(VGAScreen, 10,  20,  8, 60);
			JE_HBox(VGAScreen, 10,  55,  9, 60);
			JE_HBox(VGAScreen, 10,  87, 10, 60);
			JE_HBox(VGAScreen, 10, 120, 11, 60);
			JE_HBox(VGAScreen, 10, 170, 13, 60);
			break;
		case 5:
			JE_HBox(VGAScreen, 10,  20, 14, 60);
			JE_HBox(VGAScreen, 10,  80, 15, 60);
			JE_HBox(VGAScreen, 10, 120, 16, 60);
			break;
		case 6:
			JE_HBox(VGAScreen, 10,  20, 17, 60);
			JE_HBox(VGAScreen, 10,  40, 18, 60);
			JE_HBox(VGAScreen, 10, 130, 20, 60);
			break;
		case 7: /* Options */
			JE_HBox(VGAScreen, 10,  20, 21, 60);
			JE_HBox(VGAScreen, 10,  70, 22, 60);
			JE_HBox(VGAScreen, 10, 110, 23, 60);
			JE_HBox(VGAScreen, 10, 140, 24, 60);
			break;
		case 8:
			JE_HBox(VGAScreen, 10,  20, 25, 60);
			JE_HBox(VGAScreen, 10,  60, 26, 60);
			JE_HBox(VGAScreen, 10, 100, 27, 60);
			JE_HBox(VGAScreen, 10, 140, 28, 60);
			JE_HBox(VGAScreen, 10, 170, 29, 60);
			break;
		}

		helpBoxBrightness = 1;
		verticalHeight = 7;

		if (*restart)
		{
			fade_palette(colors, 10, 0, 255);

			*restart = false;
		}

		do
		{
			mouseCursor = mouse_x < xCenter ? MOUSE_POINTER_LEFT : MOUSE_POINTER_RIGHT;

			service_SDL_events(true);

			JE_mouseStart();
			JE_showVGA();
			JE_mouseReplace();

			// Present at display rate for a smooth cursor; vsync-on paces via showVGA.
			if (!output_vsync)
				limit_render_fps();

			push_joysticks_as_keyboard();
			service_SDL_events(false);
		} while (!(newkey || newmouse));

		// Handle interaction.

		bool done = false;

		if (newmouse)
		{
			switch (lastmouse_but)
			{
			case SDL_BUTTON_LEFT:
			{
				JE_playSampleNum(S_CURSOR);

				if (mouse_x < xCenter)
					page -= 1;
				else
					page += 1;
				break;
			}
			case SDL_BUTTON_RIGHT:
			{
				JE_playSampleNum(S_SPRING);

				done = true;
				break;
			}
			default:
				break;
			}
		}
		else if (newkey)
		{
			switch (lastkey_scan)
			{
			case SDL_SCANCODE_LEFT:
			{
				JE_playSampleNum(S_CURSOR);

				page -= 1;
				break;
			}
			case SDL_SCANCODE_RIGHT:
			case SDL_SCANCODE_SPACE:
			case SDL_SCANCODE_RETURN:
			{
				JE_playSampleNum(S_CURSOR);

				page += 1;
				break;
			}
			case SDL_SCANCODE_ESCAPE:
			{
				JE_playSampleNum(S_SPRING);

				done = true;
				break;
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

// cost to upgrade a weapon power from power-1 (where power == 0 indicates an unupgraded weapon)
long weapon_upgrade_cost(long base_cost, unsigned int power)
{
	assert(power <= 11);

	unsigned int temp = 0;

	// 0 1 3 6 10 15 21 29 ...
	for (; power > 0; power--)
		temp += power;

	return base_cost * temp;
}

ulong JE_getCost(JE_byte itemType, JE_word itemNum)
{
	long cost = 0;

	switch (itemType)
	{
	case 2:
		cost = (itemNum > 90) ? 100 : ships[itemNum].cost;
		break;
	case 3:
	case 4:
		cost = weaponPort[itemNum].cost;

		const uint port = itemType - 3,
			item_power = player[0].items.weapon[port].power - 1;

		downgradeCost = weapon_upgrade_cost(cost, item_power);
		upgradeCost = weapon_upgrade_cost(cost, item_power + 1);
		break;
	case 5:
		cost = shields[itemNum].cost;
		break;
	case 6:
		cost = powerSys[itemNum].cost;
		break;
	case 7:
	case 8:
		cost = options[itemNum].cost;
		break;
	}

	if (expertMode)
	{
		// purchase price scales with the shop knob; power upgrades with their own knob
		if (cost > LONG_MAX / expertShopCostMult)
			cost = LONG_MAX;
		else
			cost = cost * expertShopCostMult;

		if (itemType == 3 || itemType == 4)
		{
			downgradeCost = (downgradeCost > ULONG_MAX / (ulong)expertUpgradeCostMult) ? ULONG_MAX : downgradeCost * expertUpgradeCostMult;
			upgradeCost = (upgradeCost > ULONG_MAX / (ulong)expertUpgradeCostMult) ? ULONG_MAX : upgradeCost * expertUpgradeCostMult;
		}
	}

	if (endlessMode)
	{
		// Endless: shop prices inflate with depth (+19%/level, capped 100x) since income also
		// scales; first 5 zones ramp at half-slope to keep early game gentle. notes.md §Economy & perk plumbing.
		int pct;
		if (endlessRunDepth < 5)
			pct = 100 + endlessRunDepth * 19 / 2;                 // first 5 zones: gentle half-slope
		else
			pct = 100 + 4 * 19 / 2 + (endlessRunDepth - 4) * 19;  // then full +19%/depth (continuous at zone 5)
		if (pct > 10000)
			pct = 10000;                            // cap at 100x
		pct += endlessShopTaxPercent();             // Loan Shark: a permanent debt tax on top of the depth cap
		if (endlessActiveMods & ENDLESS_MOD_FAVOR)  // Merchant's Favor: the outpost slashes prices
			pct = pct * 65 / 100;

		cost = (cost > LONG_MAX / pct) ? LONG_MAX : cost * pct / 100;

		if (itemType == 3 || itemType == 4)
		{
			downgradeCost = (downgradeCost > ULONG_MAX / (ulong)pct) ? ULONG_MAX : downgradeCost * (ulong)pct / 100;
			upgradeCost   = (upgradeCost   > ULONG_MAX / (ulong)pct) ? ULONG_MAX : upgradeCost   * (ulong)pct / 100;
		}
	}

	return cost;
}


bool JE_loadScreen(void)
{
	set_menu_centered(true);

	if (shopSpriteSheet.data == NULL)
		JE_loadCompShapes(&shopSpriteSheet, '1');  // need mouse pointer and arrow sprites

	bool restart = true;

	size_t playersIndex = 0;
	const size_t menuItemsCount = 12;
	size_t selectedIndex = 0;

	const int xCenter = 160; // center of 320px menu field
	const int yMenuHeader = 5;
	const int xMenuItem = 10;
	const int xMenuItemName = xMenuItem;
	const int xMenuItemLastLevel = 120;
	const int xMenuItemEpisode = 250;
	const int wMenuItem = 300;
	const int yMenuItems = 30;
	const int dyMenuItems = 13;
	const int hMenuItem = 8;
	const int xLeftControl = 83;
	const int xRightControl = 213;
	const int wControl = 24;
	const int yControls = vga_height - 21;

	for (; ; )
	{
		if (restart)
		{
			JE_loadPic(VGAScreen2, 2, false);
			fill_rectangle_wh(VGAScreen2, 0, vga_height - 8, vga_width, 8, 0);
		}

		// Restore background.
		memcpy(VGAScreen->pixels, VGAScreen2->pixels, (size_t)VGAScreen->pitch * VGAScreen->h);

		// Draw header.
		draw_font_hv_shadow(VGAScreen, xCenter, yMenuHeader, miscText[38 + playersIndex], large_font, centered, 15, -3, false, 2);

		// Draw menu items.

		for (size_t i = 0; i < menuItemsCount; ++i)
		{
			const int y = yMenuItems + dyMenuItems * i;

			const bool selected = i == selectedIndex;

			if (i == menuItemsCount - 1)
			{
				JE_textShade(VGAScreen, xMenuItemName, y, miscText[33], 13, selected ? 6 : 2, FULL_SHADE);
				continue;
			}

			const JE_SaveFileType *const saveFile = &saveFiles[playersIndex * 11 + i];

			const bool disabled = saveFile->level == 0;

			char buffer[22];

			if (disabled)
			{
				JE_textShade(VGAScreen, xMenuItemName, y, miscText[2], 13, selected ? 6 : 0, FULL_SHADE);

				snprintf(buffer, sizeof buffer, "%s -----", miscTextB[2]);
				JE_textShade(VGAScreen, xMenuItemLastLevel, y, buffer, 5, selected ? 6 : 0, FULL_SHADE);
			}
			else
			{
				JE_textShade(VGAScreen, xMenuItemName, y, saveFile->name, 13, selected ? 6 : 2, FULL_SHADE);

				snprintf(buffer, sizeof buffer, "%s %s", miscTextB[2], saveFile->levelName);
				JE_textShade(VGAScreen, xMenuItemLastLevel, y, buffer, 5, selected ? 6 : 2, FULL_SHADE);

				snprintf(buffer, sizeof buffer, "%s %u", miscTextB[1], saveFile->episode);
				JE_textShade(VGAScreen, xMenuItemEpisode, y, buffer, 5, selected ? 6 : 2, FULL_SHADE);
			}
		}

		// Draw paging controls.

		const bool leftControlVisible = playersIndex > 0;
		const bool rightControlVisible = playersIndex < 1;

		if (leftControlVisible)
			blit_sprite2x2(VGAScreen, xLeftControl, yControls, shopSpriteSheet, 279);

		if (rightControlVisible)
			blit_sprite2x2(VGAScreen, xRightControl, yControls, shopSpriteSheet, 281);

		helpBoxColor = 15;
		JE_helpBox(VGAScreen, 103, vga_height - 18, miscText[55], 25);

		if (restart)
		{
			mouseCursor = MOUSE_POINTER_NORMAL;

			fade_palette(colors, 10, 0, 255);

			restart = false;
		}

		service_SDL_events(true);

		JE_mouseStart();
		JE_showVGA();
		JE_mouseReplace();

		bool mouseMoved = false;
		do
		{
			SDL_Delay(1);  // fine poll so the cursor redraws at display rate on motion

			Uint16 oldMouseX = mouse_x;
			Uint16 oldMouseY = mouse_y;

			push_joysticks_as_keyboard();
			service_SDL_events(false);

			mouseMoved = mouse_x != oldMouseX || mouse_y != oldMouseY;
		} while (!(newkey || newmouse || mouseMoved));

		// Handle interaction.

		bool leftAction = false;
		bool rightAction = false;
		bool action = false;
		bool done = false;

		if (mouseMoved || newmouse)
		{
			if (leftControlVisible &&
			    mouse_y >= yControls &&
			    mouse_x >= xLeftControl &&
			    mouse_x < xLeftControl + wControl)
			{
				if (newmouse && lastmouse_but == SDL_BUTTON_LEFT)
				{
					JE_playSampleNum(S_CURSOR);

					leftAction = true;
				}
			}
			else if (rightControlVisible &&
			         mouse_y >= yControls &&
			         mouse_x >= xRightControl &&
			         mouse_x < xRightControl + wControl)
			{
				if (newmouse && lastmouse_but == SDL_BUTTON_LEFT)
				{
					JE_playSampleNum(S_CURSOR);

					rightAction = true;
				}
			}
			else
			{
				// Find menu item that was hovered or clicked.
				if (mouse_x >= xMenuItem && mouse_x < xMenuItem + wMenuItem)
				{
					for (size_t i = 0; i < menuItemsCount; ++i)
					{
						const int yMenuItem = yMenuItems + dyMenuItems * i;
						if (mouse_y >= yMenuItem && mouse_y < yMenuItem + hMenuItem)
						{
							if (selectedIndex != i)
							{
								JE_playSampleNum(S_CURSOR);

								selectedIndex = i;
							}

							if (newmouse && lastmouse_but == SDL_BUTTON_LEFT &&
							    lastmouse_x >= xMenuItem && lastmouse_x < xMenuItem + wMenuItem &&
							    lastmouse_y >= yMenuItem && lastmouse_y < yMenuItem + hMenuItem)
							{
								action = true;
							}

							break;
						}
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
			case SDL_SCANCODE_LEFT:
			{
				JE_playSampleNum(S_CURSOR);

				leftAction = true;
				break;
			}
			case SDL_SCANCODE_RIGHT:
			{
				JE_playSampleNum(S_CURSOR);

				rightAction = true;
				break;
			}
			case SDL_SCANCODE_UP:
			{
				JE_playSampleNum(S_CURSOR);

				selectedIndex = selectedIndex == 0
					? menuItemsCount - 1
					: selectedIndex - 1;
				break;
			}
			case SDL_SCANCODE_DOWN:
			{
				JE_playSampleNum(S_CURSOR);

				selectedIndex = selectedIndex == menuItemsCount - 1
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
				break;
			}
			default:
				break;
			}
		}

		if (leftAction)
		{
			playersIndex = playersIndex == 0 ? 1 : 0;
		}
		else if (rightAction)
		{
			playersIndex = playersIndex == 1 ? 0 : 1;
		}
		else if (action)
		{
			if (selectedIndex == menuItemsCount - 1)  // "Exit to Main Menu"
			{
				JE_playSampleNum(S_SELECT);

				done = true;
			}
			else
			{
				const size_t saveFileIndex = playersIndex * 11 + selectedIndex;

				if (saveFiles[saveFileIndex].level == 0)  // "EMPTY SLOT"
				{
					JE_playSampleNum(S_CLINK);
				}
				else
				{
					JE_playSampleNum(S_SELECT);

					performSave = false;
					JE_operation(saveFileIndex + 1);

					fade_black(15);

					return gameLoaded;
				}
			}
		}

		if (done)
		{
			fade_black(15);

			return false;
		}
	}
}

ulong JE_totalScore(const Player *this_player)
{
	ulong temp = this_player->cash;

	temp += JE_getValue(2, this_player->items.ship);
	temp += JE_getValue(3, this_player->items.weapon[FRONT_WEAPON].id);
	temp += JE_getValue(4, this_player->items.weapon[REAR_WEAPON].id);
	temp += JE_getValue(5, this_player->items.shield);
	temp += JE_getValue(6, this_player->items.generator);
	temp += JE_getValue(7, this_player->items.sidekick[LEFT_SIDEKICK]);
	temp += JE_getValue(8, this_player->items.sidekick[RIGHT_SIDEKICK]);

	return temp;
}

JE_longint JE_getValue(JE_byte itemType, JE_word itemNum)
{
	long value = 0;

	switch (itemType)
	{
	case 2:
		value = ships[itemNum].cost;
		break;
	case 3:
	case 4:;
		const long base_value = weaponPort[itemNum].cost;

		// if two-player, use first player's front and second player's rear weapon
		const uint port = itemType - 3;
		const uint item_power = player[twoPlayerMode ? port : 0].items.weapon[port].power - 1;

		value = base_value;
		for (unsigned int i = 1; i <= item_power; ++i)
			value += weapon_upgrade_cost(base_value, i);
		break;
	case 5:
		value = shields[itemNum].cost;
		break;
	case 6:
		value = powerSys[itemNum].cost;
		break;
	case 7:
	case 8:
		value = options[itemNum].cost;
		break;
	}

	return value;
}

void JE_nextEpisode(void)
{
	strcpy(lastLevelName, "Completed");

	if (episodeNum == initial_episode_num && !gameHasRepeated && !isNetworkGame && !constantPlay && !endlessMode)
	{
		JE_highScoreCheck();
	}

	unsigned int newEpisode = JE_findNextEpisode();

	if (jumpBackToEpisode1)
	{
		if (episodeNum > 2 &&
			!constantPlay && !endlessMode)
		{
			JE_playCredits();
		}

		// randomly give player the SuperCarrot
		if ((mt_rand() % 6) == 0)
		{
			player[0].items.ship = 2;                      // SuperCarrot
			player[0].items.weapon[FRONT_WEAPON].id = 23;  // Banana Blast
			player[0].items.weapon[REAR_WEAPON].id = 24;   // Banana Blast Rear

			for (uint i = 0; i < COUNTOF(player[0].items.weapon); ++i)
				player[0].items.weapon[i].power = 1;

			player[1].items.weapon[REAR_WEAPON].id = 24;   // Banana Blast Rear

			player[0].last_items = player[0].items;
		}
	}

	if (newEpisode != episodeNum)
		JE_initEpisode(newEpisode);

	gameLoaded = true;
	mainLevel = FIRST_LEVEL;
	saveLevel = FIRST_LEVEL;

	play_song(26);

	JE_clr256(VGAScreen);
	memcpy(colors, palettes[6-1], sizeof(colors));

	JE_dString(VGAScreen, JE_fontCenter(episode_name[episodeNum], SMALL_FONT_SHAPES), 130, episode_name[episodeNum], SMALL_FONT_SHAPES);
	JE_dString(VGAScreen, JE_fontCenter(miscText[5-1], SMALL_FONT_SHAPES), 185, miscText[5-1], SMALL_FONT_SHAPES);

	JE_showVGA();
	fade_palette(colors, 15, 0, 255);

	JE_wipeKey();
	if (!constantPlay)
	{
		do
		{
			NETWORK_KEEP_ALIVE();

			SDL_Delay(16);
		} while (!JE_anyButton());
	}

	fade_black(15);
}

void JE_initPlayerData(void)
{
	/* JE: New Game Items/Data */

	player[0].items.ship = 1;                     // USP Talon
	player[0].items.weapon[FRONT_WEAPON].id = 1;  // Pulse Cannon
	player[0].items.weapon[REAR_WEAPON].id = 0;   // None
	player[0].items.shield = 4;                   // Gencore High Energy Shield
	player[0].items.generator = 2;                // Advanced MR-12
	for (uint i = 0; i < COUNTOF(player[0].items.sidekick); ++i)
		player[0].items.sidekick[i] = 0;          // None
	player[0].items.special = 0;                  // None

	player[0].last_items = player[0].items;

	player[1].items = player[0].items;
	player[1].items.weapon[REAR_WEAPON].id = 15;  // Vulcan Cannon
	player[1].items.sidekick_level = 101;         // 101, 102, 103
	player[1].items.sidekick_series = 0;          // None

	gameHasRepeated = false;
	onePlayerAction = false;
	superArcadeMode = SA_NONE;
	superTyrian = false;
	twoPlayerMode = false;
	timedBattleMode = false;
	endlessMode = false;

	secretHint = (mt_rand() % 3) + 1;

	for (uint p = 0; p < COUNTOF(player); ++p)
	{
		for (uint i = 0; i < COUNTOF(player->items.weapon); ++i)
		{
			player[p].items.weapon[i].power = 1;
		}

		player[p].weapon_mode = 1;
		player[p].armor = ships[player[p].items.ship].dmg;

		player[p].is_dragonwing = (p == 1);
		player[p].lives = &player[p].items.weapon[p].power;

	}

	mainLevel = FIRST_LEVEL;
	saveLevel = FIRST_LEVEL;

	strcpy(lastLevelName, miscText[19]);
}

void JE_sortHighScores(void)
{
	T2KHighScoreType tempHiScore;
	for (int table = 0; table < 20; ++table)
	{
		if (t2kHighScores[table][1].score > t2kHighScores[table][0].score)
		{
			memcpy(&tempHiScore,             &t2kHighScores[table][0], sizeof(T2KHighScoreType));
			memcpy(&t2kHighScores[table][0], &t2kHighScores[table][1], sizeof(T2KHighScoreType));
			memcpy(&t2kHighScores[table][1], &tempHiScore,             sizeof(T2KHighScoreType));
		}
		if (t2kHighScores[table][2].score > t2kHighScores[table][1].score)
		{
			memcpy(&tempHiScore,             &t2kHighScores[table][1], sizeof(T2KHighScoreType));
			memcpy(&t2kHighScores[table][1], &t2kHighScores[table][2], sizeof(T2KHighScoreType));
			memcpy(&t2kHighScores[table][2], &tempHiScore,             sizeof(T2KHighScoreType));
		}
		if (t2kHighScores[table][1].score > t2kHighScores[table][0].score)
		{
			memcpy(&tempHiScore,             &t2kHighScores[table][0], sizeof(T2KHighScoreType));
			memcpy(&t2kHighScores[table][0], &t2kHighScores[table][1], sizeof(T2KHighScoreType));
			memcpy(&t2kHighScores[table][1], &tempHiScore,             sizeof(T2KHighScoreType));
		}
	}
}

void JE_highScoreScreen(void)
{
	set_menu_centered(true);

	if (shopSpriteSheet.data == NULL)
		JE_loadCompShapes(&shopSpriteSheet, '1');  // need mouse pointer and arrow sprites

	bool restart = true;

	size_t episodeIndex = 0;
	// Five episodes, three timed battles
	const size_t episodeCount = 8;

	const int xCenter = 160; // center of 320px menu field
	const int yMenuHeader = 3;
	const int yEpisodeHeader = 30;
	const int xLeftControl = 83;
	const int xRightControl = 213;
	const int wControl = 24;
	const int yControls = vga_height - 21;

	char buffer[64];
	int boardOnePlayer, boardTwoPlayer;

	for (; ; )
	{
		if (restart)
		{
			JE_loadPic(VGAScreen2, 2, false);
			fill_rectangle_wh(VGAScreen2, 0, vga_height - 8, vga_width, 8, 0);

			// Draw header.
			draw_font_hv_shadow(VGAScreen2, xCenter, yMenuHeader, miscText[50], large_font, centered, 15, -3, false, 2);
		}

		// Restore background and header.
		memcpy(VGAScreen->pixels, VGAScreen2->pixels, (size_t)VGAScreen->pitch * VGAScreen->h);

		if (episodeIndex < 5)
		{
			snprintf(buffer, sizeof(buffer), "%s", episode_name[episodeIndex + 1]);

			// Regular episode boards
			boardOnePlayer = 10 + (episodeIndex * 2);
			boardTwoPlayer = 11 + (episodeIndex * 2);
		}
		else
		{
			snprintf(buffer, sizeof(buffer), "%s %s", timed_battle_name[0], timed_battle_name[episodeIndex - 4]);

			// Timed Battle boards
			boardOnePlayer = episodeIndex - 5;
			boardTwoPlayer = -1;
		}

		// Draw episode header.
		draw_font_hv_shadow(VGAScreen, xCenter, yEpisodeHeader, buffer, normal_font, centered, 15, -3, false, 2);

		// Draw 1-player scores.
		draw_font_hv_shadow(VGAScreen, xCenter, 55, miscText[46], normal_font, centered, 15, -3, false, 2);

		for (Uint8 i = 0; i < 3; ++i)
		{
			const int y = 75 + 10 * i;

			if (t2kHighScores[boardOnePlayer][i].difficulty > 9)
				t2kHighScores[boardOnePlayer][i].difficulty = 0;

			const int rank = t2kHighScores[boardOnePlayer][i].difficulty;
			const int score = t2kHighScores[boardOnePlayer][i].score;
			const char *playerName = t2kHighScores[boardOnePlayer][i].playerName;

			snprintf(buffer, sizeof buffer, "~#%d:~  %d", i + 1, score);
			JE_textShade(VGAScreen, 20, y, buffer, 15, 0, FULL_SHADE);
			JE_textShade(VGAScreen, 110, y, playerName, 15, 2, FULL_SHADE);
			JE_textShade(VGAScreen, 250, y, difficultyNameB[rank], 15, rank + (rank == 0 ? 0 : -1), FULL_SHADE);
		}

		// Draw 2-player scores.
		if (boardTwoPlayer >= 0)
		{
			draw_font_hv_shadow(VGAScreen, xCenter, 120, miscText[47], normal_font, centered, 15, -3, false, 2);

			for (Uint8 i = 0; i < 3; ++i)
			{
				const int y = 135 + 10 * i;

				if (t2kHighScores[boardTwoPlayer][i].difficulty > 9)
					t2kHighScores[boardTwoPlayer][i].difficulty = 0;

				const int rank = t2kHighScores[boardTwoPlayer][i].difficulty;
				const int score = t2kHighScores[boardTwoPlayer][i].score;
				const char *teamName = t2kHighScores[boardTwoPlayer][i].playerName;

				snprintf(buffer, sizeof buffer, "~#%d:~  %d", i + 1, score);
				JE_textShade(VGAScreen, 20, y, buffer, 15, 0, FULL_SHADE);
				JE_textShade(VGAScreen, 110, y, teamName, 15, 2, FULL_SHADE);
				JE_textShade(VGAScreen, 250, y, difficultyNameB[rank], 15, rank + (rank == 0 ? 0 : -1), FULL_SHADE);
			}			
		}

		// Draw paging controls.

		const bool leftControlVisible = episodeIndex > 0;
		const bool rightControlVisible = episodeIndex < episodeCount - 1;

		if (leftControlVisible)
			blit_sprite2x2(VGAScreen, xLeftControl, yControls, shopSpriteSheet, 279);

		if (rightControlVisible)
			blit_sprite2x2(VGAScreen, xRightControl, yControls, shopSpriteSheet, 281);

		helpBoxColor = 15;
		JE_helpBox(VGAScreen, 103, vga_height - 18, miscText[56], 25);

		if (restart)
		{
			mouseCursor = MOUSE_POINTER_NORMAL;

			fade_palette(colors, 10, 0, 255);

			restart = false;
		}

		do
		{
			service_SDL_events(true);

			JE_mouseStart();
			JE_showVGA();
			JE_mouseReplace();

			// Present at display rate for a smooth cursor; vsync-on paces via showVGA.
			if (!output_vsync)
				limit_render_fps();

			push_joysticks_as_keyboard();
			service_SDL_events(false);
		} while (!(newkey || newmouse));

		// Handle interaction.

		bool leftAction = false;
		bool rightAction = false;
		bool done = false;

		if (newmouse)
		{
			switch (lastmouse_but)
			{
			case SDL_BUTTON_LEFT:
			{
				if (leftControlVisible &&
				    mouse_y >= yControls &&
				    mouse_x >= xLeftControl &&
				    mouse_x < xLeftControl + wControl)
				{
					JE_playSampleNum(S_CURSOR);

					leftAction = true;
				}
				else if (rightControlVisible &&
				         mouse_y >= yControls &&
				         mouse_x >= xRightControl &&
				         mouse_x < xRightControl + wControl)
				{
					JE_playSampleNum(S_CURSOR);

					rightAction = true;
				}
				break;
			}
			case SDL_BUTTON_RIGHT:
			{
				JE_playSampleNum(S_SPRING);

				done = true;
				break;
			}
			default:
				break;
			}
		}
		else if (newkey)
		{
			switch (lastkey_scan)
			{
			case SDL_SCANCODE_LEFT:
			{
				JE_playSampleNum(S_CURSOR);

				leftAction = true;
				break;
			}
			case SDL_SCANCODE_RIGHT:
			{
				JE_playSampleNum(S_CURSOR);

				rightAction = true;
				break;
			}
			case SDL_SCANCODE_SPACE:
			case SDL_SCANCODE_RETURN:
			case SDL_SCANCODE_ESCAPE:
			{
				JE_playSampleNum(S_SPRING);

				done = true;
				break;
			}
			default:
				break;
			}
		}

		if (leftAction)
		{
			episodeIndex = episodeIndex == 0
				? episodeCount - 1
				: episodeIndex - 1;
		}
		else if (rightAction)
		{
			episodeIndex = episodeIndex == episodeCount - 1
				? 0
				: episodeIndex + 1;
		}

		if (done)
		{
			fade_black(15);

			return;
		}
	}
}

void JE_gammaCorrect_func(JE_byte *col, JE_real r)
{
	int temp = roundf(*col * r);
	if (temp > 255)
	{
		temp = 255;
	}
	*col = temp;
}

void JE_gammaCorrect(Palette *colorBuffer, JE_byte gamma)
{
	int x;
	JE_real r = 1 + (JE_real)gamma / 10;

	for (x = 0; x < 256; x++)
	{
		JE_gammaCorrect_func(&(*colorBuffer)[x].r, r);
		JE_gammaCorrect_func(&(*colorBuffer)[x].g, r);
		JE_gammaCorrect_func(&(*colorBuffer)[x].b, r);
	}
}

JE_boolean JE_gammaCheck(void)
{
	bool temp = keysactive[SDL_SCANCODE_F11] != 0;
	if (temp)
	{
		keysactive[SDL_SCANCODE_F11] = false;
		newkey = false;
		gammaCorrection = (gammaCorrection + 1) % 4;
		memcpy(colors, palettes[pcxpal[3-1]], sizeof(colors));
		JE_gammaCorrect(&colors, gammaCorrection);
		set_palette(colors, 0, 255);
	}
	return temp;
}

void JE_doInGameSetup(void)
{
	// These menus present their own frames inside the gameplay tick's recording
	// window (rl_begin_record..rl_end_record in JE_main). Left on, their per-frame
	// draws flood the command buffer (runaway memory, multi-second replay), so
	// suspend recording for the duration.
	const bool rl_was_recording = render_list_recording;
	render_list_recording = false;

	mouseSetRelative(false);

	haltGame = false;

#ifdef WITH_NETWORK
	if (isNetworkGame)
	{
		network_prepare(PACKET_GAME_MENU);
		network_send(4);  // PACKET_GAME_MENU

		while (true)
		{
			service_SDL_events(false);

			if (packet_in[0] && SDLNet_Read16(&packet_in[0]->data[0]) == PACKET_GAME_MENU)
			{
				network_update();
				break;
			}

			network_update();
			network_check();

			SDL_Delay(16);
		}
	}
#endif

	if (yourInGameMenuRequest)
	{
		if (JE_inGameSetup())
		{
			reallyEndLevel = true;
			playerEndLevel = true;
		}
		quitRequested = false;

		keysactive[SDL_SCANCODE_ESCAPE] = false;

#ifdef WITH_NETWORK
		if (isNetworkGame)
		{
			if (!playerEndLevel)
			{
				network_prepare(PACKET_WAITING);
				network_send(4);  // PACKET_WAITING
			}
			else
			{
				network_prepare(PACKET_GAME_QUIT);
				network_send(4);  // PACKET_GAMEQUIT
			}
		}
#endif
	}

#ifdef WITH_NETWORK
	if (isNetworkGame)
	{
		SDL_Surface *temp_surface = VGAScreen;
		VGAScreen = VGAScreenSeg; /* side-effect of game_screen */

		if (!yourInGameMenuRequest)
		{
			JE_barShade(VGAScreen, 3, 60, 257, 80); /*Help Box*/
			JE_barShade(VGAScreen, 5, 62, 255, 78);
			JE_dString(VGAScreen, 10, 65, "Other player in options menu.", SMALL_FONT_SHAPES);
			JE_showVGA();

			while (true)
			{
				service_SDL_events(false);
				JE_showVGA();

				if (packet_in[0])
				{
					if (SDLNet_Read16(&packet_in[0]->data[0]) == PACKET_WAITING)
					{
						network_check();
						break;
					}
					else if (SDLNet_Read16(&packet_in[0]->data[0]) == PACKET_GAME_QUIT)
					{
						reallyEndLevel = true;
						playerEndLevel = true;;

						network_check();
						break;
					}
				}

				network_update();
				network_check();

				SDL_Delay(16);
			}
		}
		else
		{
			/*
				   JE_barShade(3, vga_height - 20, 257, vga_height); /-*Help Box*-/
						JE_barShade(5, vga_height - 18, 255, vga_height - 2);
			tempScreenSeg = VGAScreen;
			JE_dString(VGAScreen, 10, 165, "Waiting for other player.", SMALL_FONT_SHAPES);
			JE_showVGA();
			*/
		}

		while (!network_is_sync())
		{
			service_SDL_events(false);

			network_check();
			SDL_Delay(16);
		}

		VGAScreen = temp_surface; /* side-effect of game_screen */
	}
#endif

	yourInGameMenuRequest = false;

	//skipStarShowVGA = true;

	mouseSetRelative(true);

	render_list_recording = rl_was_recording;
}

// In-game "Extra" menu, opened from the pause menu. A small opaque panel (styled
// like the debug menu) with three pages: a root with an Invincibility toggle and
// two submenus, one triggering the F-key cheat combos and one the Backspace debug
// combos. The selected row's key combo is shown in the footer help line.
// Returns true if a triggered cheat wants control handed back to the game
// (so the caller closes the pause menu too).
bool JE_extraMenu(void)
{
	enum { PAGE_ROOT, PAGE_CHEATS, PAGE_DEBUG };

	// The cheat combos (and invincibility) are only valid in a normal solo game,
	// matching the guards on the key handlers in JE_mainKeyboardInput.
	const bool cheatsAllowed = !isNetworkGame && !twoPlayerMode && !superTyrian && superArcadeMode == SA_NONE;

	// Footer help is drawn as two short lines (description + key combo) so long
	// combos never overflow the panel.
	static const char *const rootLabels[] = { "Invincibility", "Cheat Codes...", "Debug Codes...", "Return" };
	static const char *const rootDesc[] = {
		"Don't die when armor runs out.",
		"Trigger the F-key cheat combos.",
		"The Backspace debug-key combos.",
		"Return to the pause menu.",
	};
	static const char *const rootCombo[] = { "", "", "", "" };

	static const char *const cheatLabels[] = { "Nort Ship", "Self-Destruct", "Skip Level", "Return" };
	static const char *const cheatDesc[] = {
		"Switch to the Nort Ship.",
		"Zero your ship's armor.",
		"Jump to the next level.",
		"Back to Extra.",
	};
	static const char *const cheatCombo[] = { "F2+F4+F6+F7+F9+\\+/", "F2+F3+F4", "F2+F6+F7", "" };

	static const char *const debugLabels[] = { "Debug Overlay", "Hyper-Speed", "Level Filter", "Random Music", "Screenshot Pause", "Return" };
	static const char *const debugDesc[] = {
		"Show the debug info overlay.",
		"Fast-forward gameplay.",
		"Tint the level colors.",
		"Play a random music track.",
		"Freeze; hides the pause text.",
		"Back to Extra.",
	};
	static const char *const debugCombo[] = { "F10+Backspace", "Backspace+1", "Backspace+minus", "Backspace+ScrollLock", "Backspace+NumLock", "" };

	SDL_Surface *temp_surface = VGAScreen;
	VGAScreen = VGAScreenSeg;

	const bool wasRelative = mouseGetRelative();
	mouseSetRelative(false);

	if (shopSpriteSheet.data == NULL)
		JE_loadCompShapes(&shopSpriteSheet, '1');  // mouse pointer sprites

	// The panel is opaque and covers only a sub-region; the pause menu's own loop
	// repaints the full background from VGAScreen2 when we return, so there's no
	// need to save/restore the screen here.

	wait_noinput(false, false, true);
	newkey = newmouse = false;  // don't let the click/key that opened us leak in

	// Panel geometry: reuse the debug menu's horizontal span, compact and centered.
	const int px0 = DEBUG_MENU_X;
	const int px1 = DEBUG_MENU_X + DEBUG_MENU_WIDTH - 1;
	const int title_h = 15;
	const int row_h = 16;
	const int footer_h = 26;  // two footer lines (description + key combo)
	const int maxRows = 6;
	const int panel_h = title_h + 4 + maxRows * row_h + footer_h;
	int py0 = DEBUG_MENU_Y + (DEBUG_MENU_HEIGHT - panel_h) / 2;
	if (py0 < DEBUG_MENU_Y)
		py0 = DEBUG_MENU_Y;
	const int py1 = py0 + panel_h;
	const int items_top = py0 + title_h + 4;
	const int mid_x = (px0 + px1) / 2;

	enum {
		C_PANEL_BG = 0xF1, C_TITLE_BG = 0xF3, C_DIVIDER = 0xF6,
		C_EDGE_HI = 0xFB, C_EDGE_LO = 0xF4, C_SEL_BAR = 0xF5
	};

	int page = PAGE_ROOT;
	size_t selected = 0;
	int prev_mx = mouse_x, prev_my = mouse_y;

	bool closeMenu = false;
	bool returnToGame = false;

	while (!closeMenu)
	{
		// Rows for the current page.
		const char *const *labels;
		const char *const *descs;
		const char *const *combos;
		int count;
		const char *title;
		switch (page)
		{
		case PAGE_CHEATS: labels = cheatLabels; descs = cheatDesc; combos = cheatCombo; count = 4; title = "CHEAT  CODES"; break;
		case PAGE_DEBUG:  labels = debugLabels; descs = debugDesc; combos = debugCombo; count = 6; title = "DEBUG  CODES"; break;
		default:          labels = rootLabels;  descs = rootDesc;  combos = rootCombo;  count = 4; title = "EXTRA"; break;
		}

		if ((int)selected >= count)
			selected = count - 1;

		// Opaque panel, fully repainted every frame.
		fill_rectangle_xy(VGAScreen, px0, py0, px1, py1, C_PANEL_BG);
		fill_rectangle_xy(VGAScreen, px0, py0, px1, py0, C_EDGE_HI);
		fill_rectangle_xy(VGAScreen, px0, py0, px0, py1, C_EDGE_HI);
		fill_rectangle_xy(VGAScreen, px0, py1, px1, py1, C_EDGE_LO);
		fill_rectangle_xy(VGAScreen, px1, py0, px1, py1, C_EDGE_LO);
		fill_rectangle_xy(VGAScreen, px0 + 1, py0 + 1, px1 - 1, py0 + title_h - 1, C_TITLE_BG);
		fill_rectangle_xy(VGAScreen, px0 + 1, py0 + title_h, px1 - 1, py0 + title_h, C_DIVIDER);
		draw_font_hv_shadow(VGAScreen, mid_x, py0 + 3, title, normal_font, centered, 15, 4, true, 1);

		for (int i = 0; i < count; ++i)
		{
			const int ry = items_top + i * row_h;
			const bool sel = ((int)selected == i);

			// Grey out cheat rows (and the paths to them) when cheats aren't valid.
			bool enabled = true;
			if (page == PAGE_ROOT && (i == 0 || i == 1))
				enabled = cheatsAllowed;
			else if (page == PAGE_CHEATS && i < 3)
				enabled = cheatsAllowed;

			if (sel)
				fill_rectangle_xy(VGAScreen, px0 + 3, ry - 1, px1 - 3, ry + row_h - 3, C_SEL_BAR);

			const int bright = enabled ? (sel ? 5 : -1) : -6;
			draw_font_hv_shadow(VGAScreen, px0 + 14, ry, labels[i], small_font, left_aligned, 15, bright, true, 1);

			// Value column (toggles / cyclers).
			char valbuf[16];
			const char *value = NULL;
			if (page == PAGE_ROOT && i == 0)
				value = youAreCheating ? "On" : "Off";
			else if (page == PAGE_DEBUG && i == 0)
				value = debug ? "On" : "Off";
			else if (page == PAGE_DEBUG && i == 1)
				value = fastPlay == 0 ? "Off" : (fastPlay == 1 ? "On" : "Max");
			else if (page == PAGE_DEBUG && i == 2)
			{
				if (levelFilter == -99)
					value = "Off";
				else
				{
					snprintf(valbuf, sizeof(valbuf), "%d", levelFilter);
					value = valbuf;
				}
			}
			else if (page == PAGE_DEBUG && i == 4)
				value = superPause ? "On" : "Off";

			if (value != NULL)
				draw_font_hv_shadow(VGAScreen, px1 - 12, ry, value, small_font, right_aligned, 15, bright, true, 1);

			if (sel)
				draw_font_hv(VGAScreen, px0 + 5, ry, ">", small_font, left_aligned, 15, 6);
		}

		// Footer (two lines so long key combos don't overflow the panel): the
		// selected row's description, then its key combo.
		fill_rectangle_xy(VGAScreen, px0 + 1, py1 - footer_h, px1 - 1, py1 - footer_h, C_DIVIDER);
		draw_font_hv(VGAScreen, px0 + 6, py1 - footer_h + 4, descs[selected], small_font, left_aligned, 15, -3);
		if (combos[selected][0] != '\0')
		{
			char keyline[40];
			snprintf(keyline, sizeof(keyline), "Keys: %s", combos[selected]);
			draw_font_hv(VGAScreen, px0 + 6, py1 - footer_h + 15, keyline, small_font, left_aligned, 15, -3);
		}

		mouseCursor = MOUSE_POINTER_NORMAL;
		JE_mouseStart();
		JE_showVGA();
		JE_mouseReplace();

		push_joysticks_as_keyboard();
		service_SDL_events(true);

		// Mouse: hover to select, wheel to move, left-click to activate, right-click back.
		bool activate = false, goBack = false;
		int adjustDir = 0;  // -1/+1 from Left/Right on a toggle/cycler row

		if (mouse_scroll != 0)
		{
			int ns = (int)selected - mouse_scroll;
			ns = MAX(0, MIN(count - 1, ns));
			selected = (size_t)ns;
			mouse_scroll = 0;
		}
		if (mouse_x != prev_mx || mouse_y != prev_my)
		{
			prev_mx = mouse_x;
			prev_my = mouse_y;
			if (mouse_x >= px0 && mouse_x <= px1 && mouse_y >= items_top)
			{
				const int vis = (mouse_y - items_top) / row_h;
				if (vis >= 0 && vis < count)
					selected = (size_t)vis;
			}
		}
		if (newmouse)
		{
			if (lastmouse_but == SDL_BUTTON_LEFT && lastmouse_x >= px0 && lastmouse_x <= px1 && lastmouse_y >= items_top)
			{
				const int vis = (lastmouse_y - items_top) / row_h;
				if (vis >= 0 && vis < count)
				{
					selected = (size_t)vis;
					activate = true;
				}
			}
			else if (lastmouse_but == SDL_BUTTON_RIGHT)
			{
				goBack = true;
			}
		}
		else if (newkey)
		{
			switch (lastkey_scan)
			{
			case SDL_SCANCODE_UP:
				JE_playSampleNum(S_CURSOR);
				selected = selected == 0 ? (size_t)(count - 1) : selected - 1;
				break;
			case SDL_SCANCODE_DOWN:
				JE_playSampleNum(S_CURSOR);
				selected = (int)selected == count - 1 ? 0 : selected + 1;
				break;
			case SDL_SCANCODE_LEFT:
				adjustDir = -1;
				break;
			case SDL_SCANCODE_RIGHT:
				adjustDir = +1;
				break;
			case SDL_SCANCODE_RETURN:
			case SDL_SCANCODE_SPACE:
				activate = true;
				break;
			case SDL_SCANCODE_ESCAPE:
				goBack = true;
				break;
			default:
				break;
			}
		}

		if (goBack)
		{
			JE_playSampleNum(S_SPRING);
			if (page == PAGE_ROOT)
				closeMenu = true;
			else
			{
				selected = (page == PAGE_CHEATS) ? 1 : 2;  // land back on the row that opened it
				page = PAGE_ROOT;
			}
			continue;
		}

		// Left/Right adjusts the toggles and cyclers in place.
		if (adjustDir != 0)
		{
			if (page == PAGE_ROOT && selected == 0 && cheatsAllowed)
			{
				youAreCheating = !youAreCheating;
				JE_playSampleNum(S_CURSOR);
			}
			else if (page == PAGE_DEBUG && selected == 0)
			{
				debug = !debug;
				debugHist = 1; debugHistCount = 1; lastDebugTime = SDL_GetTicks();
				JE_playSampleNum(S_CURSOR);
			}
			else if (page == PAGE_DEBUG && selected == 1)
			{
				fastPlay = (fastPlay + (adjustDir > 0 ? 1 : 2)) % 3;
				JE_setNewGameSpeed();
				JE_playSampleNum(S_CURSOR);
			}
			else if (page == PAGE_DEBUG && selected == 2)
			{
				if (adjustDir > 0)
				{
					if (levelFilter == -99) levelFilter = 0;
					else { levelFilter++; if (levelFilter == 16) levelFilter = -99; }
				}
				else
				{
					if (levelFilter == -99) levelFilter = 15;
					else if (levelFilter == 0) levelFilter = -99;
					else levelFilter--;
				}
				JE_playSampleNum(S_CURSOR);
			}
			else if (page == PAGE_DEBUG && selected == 4)
			{
				superPause = !superPause;
				JE_playSampleNum(S_CURSOR);
			}
			continue;
		}

		if (!activate)
			continue;

		// Activate the selected row.
		if (page == PAGE_ROOT)
		{
			switch (selected)
			{
			case 0:  // Invincibility
				if (cheatsAllowed) { youAreCheating = !youAreCheating; JE_playSampleNum(S_SELECT); }
				else JE_playSampleNum(S_SPRING);
				break;
			case 1:  // Cheat Codes...
				if (cheatsAllowed) { page = PAGE_CHEATS; selected = 0; JE_playSampleNum(S_SELECT); }
				else JE_playSampleNum(S_SPRING);
				break;
			case 2:  // Debug Codes...
				page = PAGE_DEBUG; selected = 0; JE_playSampleNum(S_SELECT);
				break;
			default:  // Return
				JE_playSampleNum(S_SELECT);
				closeMenu = true;
				break;
			}
		}
		else if (page == PAGE_CHEATS)
		{
			if (selected == 3)  // Return
			{
				JE_playSampleNum(S_SELECT);
				page = PAGE_ROOT; selected = 1;
			}
			else if (!cheatsAllowed)
			{
				JE_playSampleNum(S_SPRING);
			}
			else
			{
				JE_playSampleNum(S_SELECT);
				switch (selected)
				{
				case 0:  // Nort Ship
					player[0].items.ship = 12;
					player[0].items.special = 13;
					player[0].items.weapon[FRONT_WEAPON].id = 36;
					player[0].items.weapon[REAR_WEAPON].id = 37;
					shipGr = 1;
					break;
				case 1:  // Self-Destruct (zero armor)
					youAreCheating = false;
					for (uint i = 0; i < COUNTOF(player); ++i)
						player[i].armor = 0;
					break;
				case 2:  // Skip Level
					levelTimer = true;
					levelTimerCountdown = 0;
					endLevel = true;
					levelEnd = 40;
					break;
				default:
					break;
				}
				returnToGame = true;
				closeMenu = true;
			}
		}
		else  // PAGE_DEBUG
		{
			JE_playSampleNum(S_SELECT);
			switch (selected)
			{
			case 0:  // Debug Overlay
				debug = !debug;
				debugHist = 1; debugHistCount = 1; lastDebugTime = SDL_GetTicks();
				break;
			case 1:  // Hyper-Speed
				fastPlay = (fastPlay + 1) % 3;
				JE_setNewGameSpeed();
				break;
			case 2:  // Level Filter
				if (levelFilter == -99) levelFilter = 0;
				else { levelFilter++; if (levelFilter == 16) levelFilter = -99; }
				break;
			case 3:  // Random Music
				play_song(mt_rand() % MUSIC_NUM);
				break;
			case 4:  // Screenshot Pause
				superPause = !superPause;
				break;
			default:  // Return
				page = PAGE_ROOT; selected = 2;
				break;
			}
		}
	}

	// Restore input/mouse state; the caller repaints the pause menu.
	newkey = newmouse = false;
	mouseSetRelative(wasRelative);
	VGAScreen = temp_surface;

	return returnToGame;
}

JE_boolean JE_inGameSetup(void)
{
	bool result = false;

	SDL_Surface *temp_surface = VGAScreen;
	VGAScreen = VGAScreenSeg; /* side-effect of game_screen */

	enum MenuItemIndex
	{
		MENU_ITEM_MUSIC_VOLUME = 0,
		MENU_ITEM_EFFECTS_VOLUME,
		MENU_ITEM_TOUCH_SENS,        // Switch only; skipped on other platforms (see below)
		MENU_ITEM_DETAIL_LEVEL,
		MENU_ITEM_GAME_SPEED,
		MENU_ITEM_EXTRA,
		MENU_ITEM_DEBUG,
		MENU_ITEM_RETURN_TO_GAME,
		MENU_ITEM_QUIT,
	};

	// Indexed by id (help for Touch/Extra/Debug/Return/Quit is overridden below).
	const size_t helpIndexes[] = { 14, 14, 14, 27, 28, 26, 26, 26, 26 };

	if (shopSpriteSheet.data == NULL)
		JE_loadCompShapes(&shopSpriteSheet, '1');  // need mouse pointer sprites

	bool restart = true;

	/* Indexed by MenuItemIndex (id), not by visible row. */
	const char* const menuNames[] =
	{
			inGameText[0],
			inGameText[1],
			"Touch",
			inGameText[2],
			inGameText[3],
			"Extra",
			"Debug Menu",
			inGameText[4],
			inGameText[5],
	};

	/* Visible rows: the Debug Menu row only appears when Debug Mode is enabled
	 * in the Enhancements menu. */
	enum MenuItemIndex items[COUNTOF(menuNames)];
	size_t menuItemsCount = 0;
	for (size_t i = 0; i < COUNTOF(menuNames); ++i)
	{
		if (i == MENU_ITEM_DEBUG && !debugMode)
			continue;
#if !defined(__SWITCH__) && !defined(__vita__)
		// Touch sensitivity is only meaningful on the consoles' touchscreen.
		if (i == MENU_ITEM_TOUCH_SENS)
			continue;
#endif
		items[menuItemsCount++] = (enum MenuItemIndex)i;
	}

	size_t selectedIndex = 0;

	const int yMenuItems = 18;
	/* Extra always adds a row (7 rows, or 8 with the Debug row). Tighten the
	 * pitch when the Debug row is present so the last row clears the help box.
	 * On the consoles the Touch Sensitivity row adds one more, so squish further. */
#if defined(__SWITCH__) || defined(__vita__)
	const int dyMenuItems = debugMode ? 14 : 16;
#else
	const int dyMenuItems = debugMode ? 16 : 18;
#endif
	const int xMenuItem = 10;
	const int xMenuItemName = xMenuItem;
	const int wMenuItemName = 110;
	const int xMenuItemValue = xMenuItemName + wMenuItemName;
	const int wMenuItemValue = 90;
	const int wMenuItem = wMenuItemName + wMenuItemValue;
	const int hMenuItem = 13;

	for (bool done = false; !done; )
	{
		if (restart)
		{
			// Main box (extended down a little to fit the extra Extra row)
			JE_barShade(VGAScreen, 3, 13, 217, 148);
			JE_barShade(VGAScreen, 5, 15, 215, 146);

			// Help box
			JE_barShade(VGAScreen, 3, 152, 257, 168);
			JE_barShade(VGAScreen, 5, 154, 255, 166);

			memcpy(VGAScreen2->pixels, VGAScreen->pixels, VGAScreen2->pitch * VGAScreen2->h);

			mouseCursor = MOUSE_POINTER_NORMAL;

			restart = false;
		}

		// Restore background.
		memcpy(VGAScreen->pixels, VGAScreen2->pixels, (size_t)VGAScreen->pitch * VGAScreen->h);

		// Draw menu items.
		for (size_t i = 0; i < menuItemsCount; ++i)
		{
			const int y = yMenuItems + dyMenuItems * i;

			const enum MenuItemIndex itemId = items[i];

			const char* const name = menuNames[itemId];

			const bool selected = i == selectedIndex;

			draw_font_hv_shadow(VGAScreen, xMenuItemName, y, name, normal_font, left_aligned, 15, -4 + (selected ? 2 : 0), false, 2);

			switch (itemId)
			{
			case MENU_ITEM_MUSIC_VOLUME:
			{
				JE_barDrawShadow(VGAScreen, xMenuItemValue, y, 1, music_disabled ? 12 : 16, (tyrMusicVolume + 6) / 12, 3, 13);
				break;
			}
			case MENU_ITEM_EFFECTS_VOLUME:
			{
				JE_barDrawShadow(VGAScreen, xMenuItemValue, y, 1, samples_disabled ? 12 : 16, (fxVolume + 6) / 12, 3, 13);
				break;
			}
			case MENU_ITEM_TOUCH_SENS:
			{
				// Same bar style as the volume sliders; middle == the classic touch feel. The marker
				// slot goes bright once the fill reaches it -- compare drawn bar counts (amt vs mark),
				// not the raw value, so it flips exactly on the middle bar.
				const int amt = (touch_sensitivity + 6) / 12;
				const int mark = (TOUCH_SENS_DEFAULT + 6) / 12;
				JE_barDrawShadow(VGAScreen, xMenuItemValue, y, 1, 16, amt, 3, 13);
				JE_barDrawMark(VGAScreen, xMenuItemValue, y,
				               amt >= mark ? TOUCH_SENS_MARK_COL : TOUCH_SENS_MARK_COL_DIM, mark, 3, 13);
				break;
			}
			case MENU_ITEM_DETAIL_LEVEL:
			{
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, detailLevel[processorType-1], normal_font, left_aligned, 15, -4 + (selected ? 2 : 0), false, 2);
				break;
			}
			case MENU_ITEM_GAME_SPEED:
			{
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, gameSpeedText[gameSpeed - 1], normal_font, left_aligned, 15, -4 + (selected ? 2 : 0), false, 2);
				break;
			}
			case MENU_ITEM_DEBUG:
			{
				break;
			}
			default:
				break;
			}
		}

		// Draw help text.
		const enum MenuItemIndex selectedId = items[selectedIndex];
		const char* pause_help = mainMenuHelp[helpIndexes[selectedId]];
		if (selectedId == MENU_ITEM_EXTRA)
			pause_help = "Cheats and bonus options.";
		else if (selectedId == MENU_ITEM_DEBUG)
			pause_help = "Open debug menu.";
		else if (selectedId == MENU_ITEM_RETURN_TO_GAME)
			pause_help = "Return to game.";
		else if (selectedId == MENU_ITEM_QUIT)
			pause_help = endlessMode ? "Give up the level; return to the outpost." : "Quit playing the level.";
		else if (selectedId == MENU_ITEM_TOUCH_SENS)
			pause_help = "Touchscreen ship control sensitivity.";
		JE_outTextAdjust(VGAScreen, 10, 156, pause_help, 14, 6, TINY_FONT, true);

		service_SDL_events(true);

		JE_mouseStart();
		JE_showVGA();
		JE_mouseReplace();

		bool mouseMoved = false;
		do
		{
			SDL_Delay(1);  // fine poll so the cursor redraws at display rate on motion

			Uint16 oldMouseX = mouse_x;
			Uint16 oldMouseY = mouse_y;

			push_joysticks_as_keyboard();
			service_SDL_events(false);

			NETWORK_KEEP_ALIVE();

			mouseMoved = mouse_x != oldMouseX || mouse_y != oldMouseY;
		} while (!(newkey || newmouse || mouseMoved));

		// Handle interaction.

		bool action = false;
		bool leftAction = false;
		bool rightAction = false;

		if (mouseMoved || newmouse)
		{
			// Find menu item that was hovered or clicked.
			if (mouse_x >= xMenuItem && mouse_x < xMenuItem + wMenuItem)
			{
				for (size_t i = 0; i < menuItemsCount; ++i)
				{
					const int yMenuItem = yMenuItems + dyMenuItems * i;
					if (mouse_y >= yMenuItem && mouse_y < yMenuItem + hMenuItem)
					{
						if (selectedIndex != i)
						{
							JE_playSampleNum(S_CURSOR);

							selectedIndex = i;
						}

						if (newmouse && lastmouse_but == SDL_BUTTON_LEFT &&
						    lastmouse_x >= xMenuItem && lastmouse_x < xMenuItem + wMenuItem &&
						    lastmouse_y >= yMenuItem && lastmouse_y < yMenuItem + hMenuItem)
						{
							// Act on menu item via name.
							if (lastmouse_x >= xMenuItemName && lastmouse_x < xMenuItemName + wMenuItemName)
							{
								action = true;
							}

							// Act on menu item via value.
							else if (lastmouse_x >= xMenuItemValue && lastmouse_x < xMenuItemValue + wMenuItemValue)
							{
								switch (items[i])
								{
								case MENU_ITEM_MUSIC_VOLUME:
								{
									JE_playSampleNum(S_CURSOR);

									const int w = ((255 + 6) / 12) * (3 + 1) - 1;

									int value = (lastmouse_x - xMenuItemValue) * 255 / (w - 1);
									tyrMusicVolume = MIN(MAX(0, value), 255);

									set_volume(tyrMusicVolume, fxVolume);
									break;
								}
								case MENU_ITEM_EFFECTS_VOLUME:
								{
									const int w = ((255 + 6) / 12) * (3 + 1) - 1;

									int value = (lastmouse_x - xMenuItemValue) * 255 / (w - 1);
									fxVolume = MIN(MAX(0, value), 255);

									set_volume(tyrMusicVolume, fxVolume);

									JE_playSampleNum(S_CURSOR);
									break;
								}
								case MENU_ITEM_TOUCH_SENS:
								{
									const int w = ((TOUCH_SENS_MAX + 6) / 12) * (3 + 1) - 1;

									int value = (lastmouse_x - xMenuItemValue) * TOUCH_SENS_MAX / (w - 1);
									touch_sensitivity = MIN(MAX(0, value), TOUCH_SENS_MAX);

									JE_playSampleNum(S_CURSOR);
									break;
								}
								case MENU_ITEM_DETAIL_LEVEL:
								case MENU_ITEM_GAME_SPEED:
								{
									rightAction = true;
									break;
								}
								default:
									break;
								}
							}
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
					? menuItemsCount - 1
					: selectedIndex - 1;
				break;
			}
			case SDL_SCANCODE_DOWN:
			{
				JE_playSampleNum(S_CURSOR);

				selectedIndex = selectedIndex == menuItemsCount - 1
					? 0
					: selectedIndex + 1;
				break;
			}
			case SDL_SCANCODE_LEFT:
			{
				leftAction = true;
				break;
			}
			case SDL_SCANCODE_RIGHT:
			{
				rightAction = true;
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
				break;
			}
			case SDL_SCANCODE_W:
			{
				if (items[selectedIndex] == MENU_ITEM_DETAIL_LEVEL)
				{
					processorType = 6;
					JE_initProcessorType();
				}
				break;
			}
			default:
				break;
			}
		}

		if (action)
		{
			switch (items[selectedIndex])
			{
			case MENU_ITEM_MUSIC_VOLUME:
			{
				JE_playSampleNum(S_SELECT);

				set_music_disabled(!music_disabled);
				break;
			}
			case MENU_ITEM_EFFECTS_VOLUME:
			{
				samples_disabled = !samples_disabled;

				JE_playSampleNum(S_SELECT);
				break;
			}
			case MENU_ITEM_EXTRA:
			{
				JE_playSampleNum(S_SELECT);

				// JE_extraMenu draws its own opaque panel and restores the screen
				// on exit; it returns true if a triggered cheat wants the game back.
				if (JE_extraMenu())
					done = true;
				break;
			}
			case MENU_ITEM_DEBUG:
			{
				JE_playSampleNum(S_SELECT);

				/* capture debug menu area */
				for (int yy = 0; yy < DEBUG_MENU_HEIGHT; ++yy)
				{
					memcpy(&debug_menu_backup[yy * DEBUG_MENU_WIDTH],
						(Uint8*)VGAScreen2->pixels +
						(DEBUG_MENU_Y + yy) * VGAScreen2->pitch + DEBUG_MENU_X,
						DEBUG_MENU_WIDTH);
				}

				JE_debugMenu(false);

				/* restore debug menu area */
				for (int yy = 0; yy < DEBUG_MENU_HEIGHT; ++yy)
				{
					memcpy((Uint8*)VGAScreen->pixels +
						(DEBUG_MENU_Y + yy) * VGAScreen->pitch + DEBUG_MENU_X,
						&debug_menu_backup[yy * DEBUG_MENU_WIDTH],
						DEBUG_MENU_WIDTH);
				}

				restart = true;
				continue; /* redraw menu after exiting debug */
			}
			case MENU_ITEM_RETURN_TO_GAME:
			{
				JE_playSampleNum(S_SELECT);

				done = true;
				break;
			}
			case MENU_ITEM_QUIT:
			{
				JE_playSampleNum(S_SELECT);

				if (constantPlay)
					JE_tyrianHalt(0);

				if (isNetworkGame)
				{
					/*Tell other computer to exit*/
					haltGame = true;
					playerEndLevel = true;
				}

				// Endless: don't end the run -- flag the game loop to revert this level and reopen
				// the outpost LOCKED to the launch-time choices (see tyrian2.c JE_main). The level
				// still ends here (result/reallyEndLevel); the loop decides what happens next.
				if (endlessMode)
					endlessQuitToOutpost = true;

				result = true;
				done = true;
				break;
			}
			default:
				break;
			}
		}
		else if (leftAction || rightAction)
		{
			const int dir = leftAction ? -1 : 1;

			switch (items[selectedIndex])
			{
			case MENU_ITEM_MUSIC_VOLUME:
			{
				JE_playSampleNum(S_CURSOR);

				JE_changeVolume(&tyrMusicVolume, dir * 12, &fxVolume, 0);
				break;
			}
			case MENU_ITEM_EFFECTS_VOLUME:
			{
				JE_changeVolume(&tyrMusicVolume, 0, &fxVolume, dir * 12);

				JE_playSampleNum(S_CURSOR);
				break;
			}
			case MENU_ITEM_TOUCH_SENS:
			{
				touch_sensitivity = MIN(MAX(0, touch_sensitivity + dir * 12), TOUCH_SENS_MAX);

				JE_playSampleNum(S_CURSOR);
				break;
			}
			case MENU_ITEM_DETAIL_LEVEL:
			{
				JE_playSampleNum(S_CURSOR);

				if (dir > 0)
					processorType = processorType < 4 ? processorType + 1 : 1;
				else
					processorType = processorType > 1 ? processorType - 1 : 4;
				JE_initProcessorType();
				JE_setNewGameSpeed();
				break;
			}
			case MENU_ITEM_GAME_SPEED:
			{
				JE_playSampleNum(S_CURSOR);

				if (dir > 0)
					gameSpeed = gameSpeed < 5 ? gameSpeed + 1 : 1;
				else
					gameSpeed = gameSpeed > 1 ? gameSpeed - 1 : 5;
				JE_initProcessorType();
				JE_setNewGameSpeed();
				break;
			}
			default:
				break;
			}
		}
	}

	VGAScreen = temp_surface; /* side-effect of game_screen */

	return result;
}

/* Map a screen point to the absolute row index of a scrolled panel list, or -1
 * if the point isn't over a row. Shared by the debug menu and its submenus. */
static int panel_row_at(int mx, int my, int px0, int px1, int items_top,
                        int row_h, int visibleRows, int scrollTop, int count)
{
	if (mx < px0 || mx > px1 || my < items_top)
		return -1;
	const int vis = (my - items_top) / row_h;
	if (vis < 0 || vis >= visibleRows)
		return -1;
	const int idx = scrollTop + vis;
	return (idx >= 0 && idx < count) ? idx : -1;
}

/* Expert-mode tuning submenu of the debug menu; rows come from the shared
 * expertSettings table plus a synthetic final "Reset Defaults" row. */
static void JE_expertSettingsMenu(int off_x, int off_y)
{
	const size_t resetRow = (size_t)expertSettingsCount;  // synthetic last row
	const size_t rowCount = resetRow + 1;
	size_t selected = 0;
	int prev_mx = mouse_x, prev_my = mouse_y;  // for motion-based hover

	wait_noinput(false, false, true);
	newkey = newmouse = false;  // don't let the click/key that opened us leak in

	const int px0 = DEBUG_MENU_X + off_x, py0 = DEBUG_MENU_Y + off_y;
	const int px1 = DEBUG_MENU_X + DEBUG_MENU_WIDTH - 1 + off_x, py1 = vga_height - 5 + off_y;
	const int title_h = 15;
	const int items_top = py0 + title_h + 3;
	const int items_bottom = py1 - 9;
	const int row_h = MAX(1, (items_bottom - items_top) / (int)rowCount);  // >= 1: divisor in panel_row_at
	const int mid_x = (px0 + px1) / 2;

	enum {
		C_PANEL_BG = 0xF1, C_TITLE_BG = 0xF3, C_DIVIDER = 0xF6,
		C_EDGE_HI  = 0xFB, C_EDGE_LO  = 0xF4, C_SEL_BAR = 0xF5
	};

	bool done = false;
	while (!done)
	{
		fill_rectangle_xy(VGAScreen, px0, py0, px1, py1, C_PANEL_BG);

		fill_rectangle_xy(VGAScreen, px0, py0, px1, py0, C_EDGE_HI);
		fill_rectangle_xy(VGAScreen, px0, py0, px0, py1, C_EDGE_HI);
		fill_rectangle_xy(VGAScreen, px0, py1, px1, py1, C_EDGE_LO);
		fill_rectangle_xy(VGAScreen, px1, py0, px1, py1, C_EDGE_LO);

		fill_rectangle_xy(VGAScreen, px0 + 1, py0 + 1, px1 - 1, py0 + title_h - 1, C_TITLE_BG);
		fill_rectangle_xy(VGAScreen, px0 + 1, py0 + title_h, px1 - 1, py0 + title_h, C_DIVIDER);
		draw_font_hv_shadow(VGAScreen, mid_x, py0 + 3, "EXPERT  SETTINGS", normal_font, centered, 15, 4, true, 1);

		for (size_t i = 0; i < rowCount; ++i)
		{
			int ry = items_top + (int)i * row_h;
			bool sel = (i == selected);

			const char* label;
			char buf[24];
			if (i == resetRow)
			{
				label = "Reset Defaults";
				buf[0] = '\0';
			}
			else
			{
				const ExpertSetting* s = &expertSettings[i];
				label = s->label;
				if (s->fmt == 'x')
					snprintf(buf, sizeof(buf), "x%d", *s->value);
				else
					snprintf(buf, sizeof(buf), "%d%%", *s->value);
			}

			if (sel)
				fill_rectangle_xy(VGAScreen, px0 + 3, ry - 1, px1 - 3, ry + row_h - 2, C_SEL_BAR);

			draw_font_hv_shadow(VGAScreen, px0 + 12, ry, label, small_font, left_aligned,
			                    15, sel ? 5 : -1, true, 1);
			draw_font_hv_shadow(VGAScreen, px1 - 9, ry, buf, small_font, right_aligned,
			                    15, sel ? 5 : 0, true, 1);

			if (sel)
				draw_font_hv(VGAScreen, px0 + 5, ry, ">", small_font, left_aligned, 15, 6);
		}

		draw_font_hv(VGAScreen, mid_x, py1 - 8, "Left/Right: change    Esc: back",
		             small_font, centered, 15, -3);

		mouseCursor = MOUSE_POINTER_NORMAL;
		JE_mouseStart();
		JE_showVGA();
		JE_mouseReplace();

		push_joysticks_as_keyboard();
		service_SDL_events(true);

		/* wheel moves the selection; hover highlights on pointer motion; left-click
		 * advances a value (Enter on the Reset row), right-click decreases it */
		{
			if (mouse_scroll != 0)
			{
				int ns = (int)selected - mouse_scroll;
				if (ns < 0)
					ns = 0;
				if (ns > (int)rowCount - 1)
					ns = (int)rowCount - 1;
				selected = (size_t)ns;
				mouse_scroll = 0;
			}
			if (mouse_x != prev_mx || mouse_y != prev_my)
			{
				const int hov = panel_row_at(mouse_x, mouse_y, px0, px1, items_top,
				                             row_h, (int)rowCount, 0, (int)rowCount);
				if (hov >= 0)
					selected = (size_t)hov;
			}
			prev_mx = mouse_x;
			prev_my = mouse_y;
			if (newmouse)
			{
				const int r = panel_row_at(lastmouse_x, lastmouse_y, px0, px1, items_top,
				                           row_h, (int)rowCount, 0, (int)rowCount);
				if (r >= 0)
				{
					selected = (size_t)r;
					newkey = true;
					lastkey_scan = (lastmouse_but == SDL_BUTTON_RIGHT) ? SDL_SCANCODE_LEFT
					             : ((size_t)r == resetRow ? SDL_SCANCODE_RETURN : SDL_SCANCODE_RIGHT);
				}
				newmouse = false;
			}
		}

		if (newkey)
		{
			ExpertSetting* s = (selected == resetRow) ? NULL : &expertSettings[selected];
			switch (lastkey_scan)
			{
			case SDL_SCANCODE_UP:
				selected = (selected == 0) ? rowCount - 1 : selected - 1;
				break;
			case SDL_SCANCODE_DOWN:
				selected = (selected + 1) % rowCount;
				break;
			case SDL_SCANCODE_LEFT:
				if (s != NULL && *s->value - s->step >= s->lo)
					*s->value -= s->step;
				break;
			case SDL_SCANCODE_RIGHT:
				if (s != NULL && *s->value + s->step <= s->hi)
					*s->value += s->step;
				break;
			case SDL_SCANCODE_RETURN:
			case SDL_SCANCODE_SPACE:
				if (s == NULL)  // Reset Defaults
					for (int j = 0; j < expertSettingsCount; ++j)
						*expertSettings[j].value = expertSettings[j].def;
				break;
			case SDL_SCANCODE_ESCAPE:
				done = true;
				break;
			default:
				break;
			}

			newkey = false;
		}
	}

	wait_noinput(false, false, true);
}

/* God Mode: a single 4-state debug-menu option backed by the two independent
 * infinite-shield / infinite-armor cheat flags the engine already honours. */
enum { GOD_OFF = 0, GOD_ON, GOD_ARMOR_ONLY, GOD_SHIELD_ONLY };
static const char *const debug_god_mode_names[] = { "OFF", "ON", "Armor Only", "Shield Only" };

static int debug_god_mode_get(void)
{
	if (cheatInfiniteShields)
		return cheatInfiniteArmor ? GOD_ON : GOD_SHIELD_ONLY;
	return cheatInfiniteArmor ? GOD_ARMOR_ONLY : GOD_OFF;
}
static void debug_god_mode_set(int g)
{
	cheatInfiniteShields = (g == GOD_ON || g == GOD_SHIELD_ONLY);
	cheatInfiniteArmor   = (g == GOD_ON || g == GOD_ARMOR_ONLY);
}

/* Draw a main-table sprite magnified to scale*scale blocks, writing only inside
 * the inclusive clip box (blit_sprite() can neither clip horizontally nor magnify). */
static void draw_sprite_obj_scaled_clip(SDL_Surface *s, int x, int y, const Sprite *sp, int scale,
                                        int cx0, int cy0, int cx1, int cy1)
{
	if (sp == NULL || sp->data == NULL || scale < 1)
		return;

	const Uint8 *data = sp->data;
	const Uint8 *const data_ul = data + sp->size;
	const int width = (int)sp->width;

	if (cx0 < 0) cx0 = 0;
	if (cy0 < 0) cy0 = 0;
	if (cx1 > s->w - 1) cx1 = s->w - 1;
	if (cy1 > s->h - 1) cy1 = s->h - 1;

	Uint8 *const pixels = (Uint8 *)s->pixels;
	int col = 0, row = 0;
	for (; data < data_ul; ++data)
	{
		switch (*data)
		{
		case 255:  // run of transparent pixels
			++data;
			col += *data;
			break;
		case 254:  // next row
			col = width;
			break;
		case 253:  // one transparent pixel
			++col;
			break;
		default:   // opaque pixel -> scale*scale block
		{
			const int bx = x + col * scale, by = y + row * scale;
			for (int dy = 0; dy < scale; ++dy)
			{
				const int py = by + dy;
				if (py < cy0 || py > cy1)
					continue;
				Uint8 *const prow = pixels + py * s->pitch;
				for (int dx = 0; dx < scale; ++dx)
				{
					const int px = bx + dx;
					if (px >= cx0 && px <= cx1)
						prow[px] = *data;
				}
			}
			++col;
			break;
		}
		}
		if (col >= width)
		{
			col = 0;
			++row;
		}
	}
}

/* Convenience wrapper for a sprite addressed by main-table index. */
static void draw_sprite_scaled_clip(SDL_Surface *s, int x, int y, unsigned int table,
                                    unsigned int index, int scale,
                                    int cx0, int cy0, int cx1, int cy1)
{
	if (table >= SPRITE_TABLES_MAX || !sprite_exists(table, index))
		return;
	draw_sprite_obj_scaled_clip(s, x, y, sprite(table, index), scale, cx0, cy0, cx1, cy1);
}

/* Load a Sprite_array .shp (u16 count, then per sprite a "populated" flag and,
 * when set, u16 width/height/size + size bytes). Fails safely on a missing file
 * or implausible values; *out must be freed with free_local_sprite_array. */
static void free_local_sprite_array(Sprite_array *a)
{
	for (unsigned int i = 0; i < a->count && i < SPRITES_PER_TABLE_MAX; ++i)
	{
		free(a->sprite[i].data);
		a->sprite[i].data = NULL;
	}
	a->count = 0;
}
static bool load_sprite_array_file(const char *filename, Sprite_array *out)
{
	memset(out, 0, sizeof(*out));

	FILE *f = dir_fopen(data_dir(), filename, "rb");
	if (f == NULL)
		return false;

	Uint16 count = 0;
	if (fread(&count, sizeof(count), 1, f) != 1)
	{
		fclose(f);
		return false;
	}
	count = SDL_SwapLE16(count);
	if (count == 0 || count > SPRITES_PER_TABLE_MAX)  // not this format / empty
	{
		fclose(f);
		return false;
	}

	out->count = count;
	bool any = false;
	for (unsigned int i = 0; i < count; ++i)
	{
		Sprite *const sp = &out->sprite[i];
		Uint8 populated = 0;
		if (fread(&populated, 1, 1, f) != 1)
			break;
		if (!populated)
			continue;

		Uint16 whs[3];
		if (fread(whs, sizeof(whs[0]), 3, f) != 3)
			break;
		sp->width  = SDL_SwapLE16(whs[0]);
		sp->height = SDL_SwapLE16(whs[1]);
		sp->size   = SDL_SwapLE16(whs[2]);
		if (sp->size == 0 || sp->size > 64u * 1024u)  // sanity guard
			break;

		sp->data = malloc(sp->size);
		if (sp->data == NULL || fread(sp->data, 1, sp->size, f) != sp->size)
		{
			free(sp->data);
			sp->data = NULL;
			break;
		}
		any = true;
	}
	fclose(f);

	if (!any)
	{
		free_local_sprite_array(out);
		return false;
	}
	return true;
}

static const char *const spriteTableNames[SPRITE_TABLES_MAX] = {
	"Font", "Small Font", "Tiny Font", "Planets",
	"Faces", "Options / Help", "Weapons", "Extra / Endings"
};

/* ---- compiled (Sprite2) sheet helpers -------------------------------------
 * 12px-wide RLE sprites preceded by a table of 16-bit byte offsets: count is
 * the first offset / 2 and 1-based sprite N starts at offsets[N-1]. */
static int sprite2_count(const Sprite2_array *a)
{
	if (a->data == NULL || a->size < 2)
		return 0;
	return SDL_SwapLE16(((const Uint16 *)a->data)[0]) / 2;
}
static int sprite2_height(const Sprite2_array *a, int index1)
{
	if (index1 < 1 || index1 > sprite2_count(a))
		return 0;
	const size_t off = SDL_SwapLE16(((const Uint16 *)a->data)[index1 - 1]);
	if (off >= a->size)
		return 0;
	const Uint8 *data = a->data + off;
	const Uint8 *const end = a->data + a->size;  // never read past the buffer
	int rows = 1;
	for (; data < end && *data != 0x0f; ++data)
	{
		const int count = (*data & 0xf0) >> 4;
		if (count == 0)
			++rows;
		else
			data += count;  // step past the opaque pixel bytes
	}
	return rows;
}
static void draw_sprite2_scaled_clip(SDL_Surface *s, int x, int y, const Sprite2_array *a,
                                     int index1, int scale, int cx0, int cy0, int cx1, int cy1)
{
	if (a->data == NULL || index1 < 1 || index1 > sprite2_count(a) || scale < 1)
		return;
	if (cx0 < 0) cx0 = 0;
	if (cy0 < 0) cy0 = 0;
	if (cx1 > s->w - 1) cx1 = s->w - 1;
	if (cy1 > s->h - 1) cy1 = s->h - 1;

	const size_t off = SDL_SwapLE16(((const Uint16 *)a->data)[index1 - 1]);
	if (off >= a->size)
		return;
	Uint8 *const pixels = (Uint8 *)s->pixels;
	const Uint8 *data = a->data + off;
	const Uint8 *const end = a->data + a->size;  // never read past the buffer
	int col = 0, row = 0;
	for (; data < end && *data != 0x0f; ++data)
	{
		col += *data & 0x0f;  // transparent skip
		int count = (*data & 0xf0) >> 4;
		if (count == 0)
		{
			++row;
			col = 0;
		}
		else
		{
			while (count-- && data + 1 < end)
			{
				++data;
				const int bx = x + col * scale, by = y + row * scale;
				for (int dy = 0; dy < scale; ++dy)
				{
					const int py = by + dy;
					if (py < cy0 || py > cy1) continue;
					Uint8 *const prow = pixels + py * s->pitch;
					for (int dx = 0; dx < scale; ++dx)
					{
						const int px = bx + dx;
						if (px >= cx0 && px <= cx1)
							prow[px] = *data;
					}
				}
				++col;
			}
		}
	}
}

/* ---- background tile helper (raw 24x28, one byte per pixel) ---------------- */
#define TILE_W 24
#define TILE_H 28
static void draw_tile_scaled_clip(SDL_Surface *s, int x, int y, const Uint8 *tile,
                                  int scale, int cx0, int cy0, int cx1, int cy1)
{
	if (tile == NULL || scale < 1)
		return;
	if (cx0 < 0) cx0 = 0;
	if (cy0 < 0) cy0 = 0;
	if (cx1 > s->w - 1) cx1 = s->w - 1;
	if (cy1 > s->h - 1) cy1 = s->h - 1;

	Uint8 *const pixels = (Uint8 *)s->pixels;
	for (int r = 0; r < TILE_H; ++r)
		for (int c = 0; c < TILE_W; ++c)
		{
			const Uint8 v = tile[r * TILE_W + c];  // tiles are opaque; draw every pixel
			const int bx = x + c * scale, by = y + r * scale;
			for (int dy = 0; dy < scale; ++dy)
			{
				const int py = by + dy;
				if (py < cy0 || py > cy1) continue;
				Uint8 *const prow = pixels + py * s->pitch;
				for (int dx = 0; dx < scale; ++dx)
				{
					const int px = bx + dx;
					if (px >= cx0 && px <= cx1)
						prow[px] = v;
				}
			}
		}
}

/* ---- unified sprite source model ------------------------------------------
 * Main shape tables, compiled (Sprite2) sheets and background tile banks
 * behind one count / dims / exists / draw interface for the viewer. */
typedef enum { VS_MAIN, VS_SPRITE2, VS_TILE, VS_SPRITE_ARRAY } VSKind;
typedef struct
{
	const char *name;
	VSKind kind;
	int count;
	unsigned int table;          // VS_MAIN
	const Sprite2_array *sheet;  // VS_SPRITE2
	const Uint8 *tileBase;       // VS_TILE: pointer to first tile's pixels
	size_t tileStride;           // VS_TILE: bytes between successive tiles
	const Sprite_array *localArr;// VS_SPRITE_ARRAY: a freshly-loaded shape table
} VSource;

static int vsrc_add(VSource *list, int n, const char *name, VSKind kind, int count,
                    unsigned int table, const Sprite2_array *sheet,
                    const Uint8 *tileBase, size_t tileStride)
{
	list[n].name = name;
	list[n].kind = kind;
	list[n].count = count;
	list[n].table = table;
	list[n].sheet = sheet;
	list[n].tileBase = tileBase;
	list[n].tileStride = tileStride;
	list[n].localArr = NULL;
	return n + 1;
}
static bool vsrc_exists(const VSource *s, int i)
{
	if (i < 0 || i >= s->count)
		return false;
	if (s->kind == VS_MAIN)
		return sprite_exists(s->table, i);
	if (s->kind == VS_SPRITE_ARRAY)
		return i < (int)s->localArr->count && s->localArr->sprite[i].data != NULL;
	return true;  // sprite2 / tile banks have no gaps
}
static void vsrc_dims(const VSource *s, int i, int *w, int *h)
{
	switch (s->kind)
	{
	case VS_MAIN:    *w = get_sprite_width(s->table, i); *h = get_sprite_height(s->table, i); break;
	case VS_SPRITE2: *w = 12; *h = sprite2_height(s->sheet, i + 1); break;
	case VS_TILE:    *w = TILE_W; *h = TILE_H; break;
	case VS_SPRITE_ARRAY: *w = s->localArr->sprite[i].width; *h = s->localArr->sprite[i].height; break;
	}
}
static void vsrc_draw(SDL_Surface *surf, const VSource *s, int i, int x, int y, int scale,
                      int cx0, int cy0, int cx1, int cy1)
{
	switch (s->kind)
	{
	case VS_MAIN:    draw_sprite_scaled_clip(surf, x, y, s->table, i, scale, cx0, cy0, cx1, cy1); break;
	case VS_SPRITE2: draw_sprite2_scaled_clip(surf, x, y, s->sheet, i + 1, scale, cx0, cy0, cx1, cy1); break;
	case VS_TILE:    draw_tile_scaled_clip(surf, x, y, s->tileBase + (size_t)i * s->tileStride, scale, cx0, cy0, cx1, cy1); break;
	case VS_SPRITE_ARRAY: draw_sprite_obj_scaled_clip(surf, x, y, &s->localArr->sprite[i], scale, cx0, cy0, cx1, cy1); break;
	}
}
static int vsrc_first(const VSource *s)
{
	for (int i = 0; i < s->count; ++i)
		if (vsrc_exists(s, i))
			return i;
	return 0;
}
static int vsrc_last(const VSource *s)
{
	for (int i = s->count - 1; i >= 0; --i)
		if (vsrc_exists(s, i))
			return i;
	return 0;
}
static int vsrc_step(const VSource *s, int cur, int dir)
{
	for (int i = cur + dir; i >= 0 && i < s->count; i += dir)
		if (vsrc_exists(s, i))
			return i;
	return cur;  // already at the first/last existing entry
}

/* Load every non-blank 24x28 tile from shapes<c>.dat (up to 600 entries of
 * [1-byte blank flag][672 pixel bytes]) so all tilesets are browsable; the
 * per-level megaData banks only hold the current map's tiles. Returns the
 * tile count; caller frees *outBuf (NULL when there are none). */
static int load_tileset_file(char c, Uint8 **outBuf)
{
	*outBuf = NULL;

	char name[16];
	snprintf(name, sizeof(name), "shapes%c.dat", c);
	FILE *f = dir_fopen(data_dir(), name, "rb");
	if (f == NULL)
		return 0;

	Uint8 *buf = malloc((size_t)600 * TILE_W * TILE_H);
	int count = 0;
	if (buf != NULL)
	{
		for (int z = 0; z < 600; ++z)
		{
			Uint8 blank;
			if (fread(&blank, 1, 1, f) != 1)
				break;
			if (blank)
				continue;  // blank tile carries no pixel data
			if (fread(buf + (size_t)count * TILE_W * TILE_H, 1, TILE_W * TILE_H, f) != (size_t)(TILE_W * TILE_H))
				break;
			++count;
		}
	}
	fclose(f);

	if (count == 0)
	{
		free(buf);
		return 0;
	}
	*outBuf = buf;
	return count;
}

/* Load compiled sheet newsh<c>.shp (enemy banks, shop, explosions, ...) straight
 * from disk, so the viewer isn't limited to the current level's enemies.
 * Returns false, leaving *out empty, when the file is absent or spriteless. */
static bool try_load_newsh(char c, Sprite2_array *out)
{
	out->data = NULL;
	out->size = 0;

	char fname[16];
	snprintf(fname, sizeof(fname), "newsh%c.shp", c);
	FILE *f = dir_fopen(data_dir(), fname, "rb");
	if (f == NULL)
		return false;

	out->size = ftell_eof(f);
	if (out->size > 2)
		JE_loadCompShapesB(out, f);  // mallocs out->data and reads the file
	fclose(f);

	if (out->data == NULL || sprite2_count(out) <= 0)
	{
		free_sprite2s(out);
		return false;
	}
	return true;
}

/* Sprite browser submenu of the debug menu: magnified checker-backed preview
 * plus a filmstrip of neighbours, over every graphics source, with palette
 * switching so sprites built for a different palette display correctly. */
static void JE_spriteViewer(int off_x, int off_y)
{
	wait_noinput(false, false, true);
	newkey = newmouse = false;  // don't let the click/key that opened us leak in

	/* Build the source list from whatever graphics are currently loaded. */
	VSource src[128];
	int nsrc = 0;
	for (unsigned int t = 0; t < SPRITE_TABLES_MAX; ++t)
		if (sprite_table[t].count > 0)
			nsrc = vsrc_add(src, nsrc, spriteTableNames[t], VS_MAIN, (int)sprite_table[t].count, t, NULL, NULL, 0);

	/* The always-in-memory compiled sheets that live inside tyrian.shp (not
	 * standalone files), so they can't be loaded by name like the newsh*.shp. */
	static const struct { const char *name; const Sprite2_array *a; } sheets[] = {
		{ "Player Shots",   &spriteSheet8 },
		{ "Player Ships",   &spriteSheet9 },
		{ "Power-ups",      &spriteSheet10 },
		{ "Coins / Cubes",  &spriteSheet11 },
		{ "Player Shots 2", &spriteSheet12 },
		{ "T2000 Ships",    &spriteSheetT2000 },
	};
	for (size_t i = 0; i < COUNTOF(sheets); ++i)
	{
		const int c = sprite2_count(sheets[i].a);
		if (c > 0)
			nsrc = vsrc_add(src, nsrc, sheets[i].name, VS_SPRITE2, c, 0, sheets[i].a, NULL, 0);
	}

	/* Load every compiled object sheet (newsh*.shp) from disk, iterating only
	 * the on-disk casing so a case-folding filesystem can't load one twice. */
	static const char newshChars[] = "0123456789abcdefghijklmnopqrstuvwxyz#$%'(@^~";
	Sprite2_array loadedSheets[64];
	char loadedNames[64][16];
	int nLoaded = 0;
	for (const char *pc = newshChars; *pc != '\0' && nLoaded < (int)COUNTOF(loadedSheets)
	                                 && nsrc < (int)COUNTOF(src); ++pc)
	{
		if (try_load_newsh(*pc, &loadedSheets[nLoaded]))
		{
			snprintf(loadedNames[nLoaded], sizeof(loadedNames[0]), "newsh %c", *pc);
			nsrc = vsrc_add(src, nsrc, loadedNames[nLoaded], VS_SPRITE2,
			                sprite2_count(&loadedSheets[nLoaded]), 0, &loadedSheets[nLoaded], NULL, 0);
			++nLoaded;
		}
	}

	/* Stand-alone shape tables outside tyrian.shp: the ending/credits sprites
	 * (estsc.shp) plus the unused-but-present estpa / user ship files. */
	static const struct { const char *name; const char *file; } arrFiles[] = {
		{ "Ending: estsc", "estsc.shp" },
		{ "Ending: estpa", "estpa.shp" },
		{ "User ship 1",   "user1.shp" },
		{ "User ship 2",   "user2.shp" },
	};
	Sprite_array localArrs[COUNTOF(arrFiles)];
	int nArr = 0;
	for (size_t i = 0; i < COUNTOF(arrFiles) && nsrc < (int)COUNTOF(src); ++i)
	{
		if (load_sprite_array_file(arrFiles[i].file, &localArrs[nArr]))
		{
			const int at = nsrc;
			nsrc = vsrc_add(src, nsrc, arrFiles[i].name, VS_SPRITE_ARRAY,
			                (int)localArrs[nArr].count, 0, NULL, NULL, 0);
			src[at].localArr = &localArrs[nArr];
			++nArr;
		}
	}

	/* Every level's tiles come from one of these five shapes<c>.dat files; load
	 * them all so the whole game's tilesets are browsable from anywhere. */
	static const char tileFileChars[] = { ')', 'w', 'x', 'y', 'z' };
	static const char *const tileSetNames[] = {
		"Tiles: Set )", "Tiles: Set W", "Tiles: Set X", "Tiles: Set Y", "Tiles: Set Z"
	};
	Uint8 *tileBufs[COUNTOF(tileFileChars)] = { NULL };
	for (size_t i = 0; i < COUNTOF(tileFileChars); ++i)
	{
		const int c = load_tileset_file(tileFileChars[i], &tileBufs[i]);
		if (c > 0)
			nsrc = vsrc_add(src, nsrc, tileSetNames[i], VS_TILE, c, 0, NULL,
			                tileBufs[i], (size_t)TILE_W * TILE_H);
	}

	if (nsrc == 0)  // nothing loaded at all
	{
		for (size_t i = 0; i < COUNTOF(tileBufs); ++i)
			free(tileBufs[i]);
		for (int i = 0; i < nLoaded; ++i)
			free_sprite2s(&loadedSheets[i]);
		for (int i = 0; i < nArr; ++i)
			free_local_sprite_array(&localArrs[i]);
		return;
	}

	const int px0 = DEBUG_MENU_X + off_x, py0 = DEBUG_MENU_Y + off_y;
	const int px1 = DEBUG_MENU_X + DEBUG_MENU_WIDTH - 1 + off_x, py1 = vga_height - 5 + off_y;
	const int title_h = 15;
	const int mid_x = (px0 + px1) / 2;

	enum {
		C_PANEL_BG = 0xF1, C_TITLE_BG = 0xF3, C_DIVIDER = 0xF6,
		C_EDGE_HI  = 0xFB, C_EDGE_LO  = 0xF4,
		C_CHECK_A  = 0xF2, C_CHECK_B  = 0xF4, C_CELL_SEL = 0xFB
	};

	/* Remember the live palette so we can recolour for previews and restore it
	 * on the way out (palSel == -1 means "use the palette already on screen"). */
	Palette savedPal;
	memcpy(savedPal, colors, sizeof(savedPal));
	int palSel = -1;

	int s = 0;            // current source
	int idx = vsrc_first(&src[s]);

	bool done = false;
	while (!done)
	{
		/* panel + beveled border */
		fill_rectangle_xy(VGAScreen, px0, py0, px1, py1, C_PANEL_BG);
		fill_rectangle_xy(VGAScreen, px0, py0, px1, py0, C_EDGE_HI);
		fill_rectangle_xy(VGAScreen, px0, py0, px0, py1, C_EDGE_HI);
		fill_rectangle_xy(VGAScreen, px0, py1, px1, py1, C_EDGE_LO);
		fill_rectangle_xy(VGAScreen, px1, py0, px1, py1, C_EDGE_LO);

		fill_rectangle_xy(VGAScreen, px0 + 1, py0 + 1, px1 - 1, py0 + title_h - 1, C_TITLE_BG);
		fill_rectangle_xy(VGAScreen, px0 + 1, py0 + title_h, px1 - 1, py0 + title_h, C_DIVIDER);
		draw_font_hv_shadow(VGAScreen, mid_x, py0 + 3, "SPRITE  VIEWER", normal_font, centered, 15, 4, true, 1);

		int sw = 0, sh = 0;
		vsrc_dims(&src[s], idx, &sw, &sh);

		/* info lines */
		const int line_h = 11;
		const int ct = py0 + title_h + 4;
		char buf[48];

		snprintf(buf, sizeof(buf), "Source:  %s", src[s].name);
		draw_font_hv_shadow(VGAScreen, px0 + 10, ct, buf, small_font, left_aligned, 15, 0, true, 1);
		snprintf(buf, sizeof(buf), "%d / %d", s + 1, nsrc);
		draw_font_hv_shadow(VGAScreen, px1 - 10, ct, buf, small_font, right_aligned, 15, 0, true, 1);

		snprintf(buf, sizeof(buf), "Sprite %d / %d    %dx%d", idx, src[s].count - 1, sw, sh);
		draw_font_hv_shadow(VGAScreen, px0 + 10, ct + line_h, buf, small_font, left_aligned, 15, 0, true, 1);
		if (palSel < 0)
			snprintf(buf, sizeof(buf), "Pal: native");
		else
			snprintf(buf, sizeof(buf), "Pal: %d / %d", palSel + 1, palette_count);
		draw_font_hv_shadow(VGAScreen, px1 - 10, ct + line_h, buf, small_font, right_aligned, 15, 0, true, 1);

		/* preview box */
		const int bx0 = px0 + 10, bx1 = px1 - 10;
		const int by0 = ct + 2 * line_h + 6, by1 = py1 - 50;

		/* inset frame */
		fill_rectangle_xy(VGAScreen, bx0, by0, bx1, by0, C_EDGE_LO);
		fill_rectangle_xy(VGAScreen, bx0, by0, bx0, by1, C_EDGE_LO);
		fill_rectangle_xy(VGAScreen, bx0, by1, bx1, by1, C_EDGE_HI);
		fill_rectangle_xy(VGAScreen, bx1, by0, bx1, by1, C_EDGE_HI);

		/* checkerboard so transparent / black pixels are visible */
		for (int yy = by0 + 1; yy < by1; yy += 8)
			for (int xx = bx0 + 1; xx < bx1; xx += 8)
			{
				const int c = (((xx - bx0) / 8 + (yy - by0) / 8) & 1) ? C_CHECK_A : C_CHECK_B;
				fill_rectangle_xy(VGAScreen, xx, yy, MIN(xx + 7, bx1 - 1), MIN(yy + 7, by1 - 1), c);
			}

		/* magnify to fill the box (integer scale, capped) */
		int scale = 1;
		if (sw > 0 && sh > 0)
		{
			const int boxw = bx1 - bx0 - 6, boxh = by1 - by0 - 6;
			while (scale < 8 && sw * (scale + 1) <= boxw && sh * (scale + 1) <= boxh)
				++scale;
			const int dw = sw * scale, dh = sh * scale;
			const int dx = (bx0 + bx1) / 2 - dw / 2, dy = (by0 + by1) / 2 - dh / 2;
			vsrc_draw(VGAScreen, &src[s], idx, dx, dy, scale, bx0 + 1, by0 + 1, bx1 - 1, by1 - 1);
		}

		/* zoom indicator */
		snprintf(buf, sizeof(buf), "x%d", scale);
		draw_font_hv_shadow(VGAScreen, bx1 - 3, by0 + 2, buf, small_font, right_aligned, 15, -2, true, 1);

		/* filmstrip of neighbouring sprites */
		const int strip_y = by1 + 4;
		const int strip_bot = py1 - 24;
		const int cell_h = strip_bot - strip_y;
		const int cells = 7;
		const int cell_w = (bx1 - bx0) / cells;
		for (int k = 0; k < cells; ++k)
		{
			const int si = idx - cells / 2 + k;
			const int cxL = bx0 + k * cell_w;
			const bool cur = (k == cells / 2);

			fill_rectangle_xy(VGAScreen, cxL, strip_y, cxL + cell_w - 2, strip_y + cell_h,
			                  cur ? C_CHECK_A : C_PANEL_BG);
			if (cur)
			{
				fill_rectangle_xy(VGAScreen, cxL, strip_y, cxL + cell_w - 2, strip_y, C_CELL_SEL);
				fill_rectangle_xy(VGAScreen, cxL, strip_y + cell_h, cxL + cell_w - 2, strip_y + cell_h, C_CELL_SEL);
				fill_rectangle_xy(VGAScreen, cxL, strip_y, cxL, strip_y + cell_h, C_CELL_SEL);
				fill_rectangle_xy(VGAScreen, cxL + cell_w - 2, strip_y, cxL + cell_w - 2, strip_y + cell_h, C_CELL_SEL);
			}

			if (vsrc_exists(&src[s], si))
			{
				int tw = 0, th = 0;
				vsrc_dims(&src[s], si, &tw, &th);
				const int tx = cxL + (cell_w - 2) / 2 - tw / 2;
				const int ty = strip_y + cell_h / 2 - th / 2;
				vsrc_draw(VGAScreen, &src[s], si, tx, ty, 1,
				          cxL + 1, strip_y + 1, cxL + cell_w - 3, strip_y + cell_h - 1);
			}
		}

		/* two-line footer, kept inside the panel */
		draw_font_hv(VGAScreen, mid_x, py1 - 20,
		             "L/R sprite    U/D source    [ ] palette",
		             small_font, centered, 15, -3);
		draw_font_hv(VGAScreen, mid_x, py1 - 11,
		             "Home/End   PgUp/PgDn   Esc back",
		             small_font, centered, 15, -3);

		mouseCursor = MOUSE_POINTER_NORMAL;
		JE_mouseStart();
		JE_showVGA();
		JE_mouseReplace();

		push_joysticks_as_keyboard();
		service_SDL_events(true);

		/* wheel walks sprites; left-click = next, right-click = exit */
		if (mouse_scroll != 0)
		{
			idx = vsrc_step(&src[s], idx, mouse_scroll > 0 ? -1 : 1);
			mouse_scroll = 0;
		}
		if (newmouse)
		{
			if (lastmouse_but == SDL_BUTTON_RIGHT)
				done = true;
			else
				idx = vsrc_step(&src[s], idx, 1);
			newmouse = false;
		}

		if (newkey)
		{
			int newPalSel = palSel;
			switch (lastkey_scan)
			{
			case SDL_SCANCODE_LEFT:
				idx = vsrc_step(&src[s], idx, -1);
				break;
			case SDL_SCANCODE_RIGHT:
				idx = vsrc_step(&src[s], idx, 1);
				break;
			case SDL_SCANCODE_PAGEUP:
				for (int n = 0; n < 10; ++n) idx = vsrc_step(&src[s], idx, -1);
				break;
			case SDL_SCANCODE_PAGEDOWN:
				for (int n = 0; n < 10; ++n) idx = vsrc_step(&src[s], idx, 1);
				break;
			case SDL_SCANCODE_HOME:
				idx = vsrc_first(&src[s]);
				break;
			case SDL_SCANCODE_END:
				idx = vsrc_last(&src[s]);
				break;
			case SDL_SCANCODE_UP:
				s = (s == 0) ? nsrc - 1 : s - 1;
				idx = vsrc_first(&src[s]);
				break;
			case SDL_SCANCODE_DOWN:
				s = (s + 1) % nsrc;
				idx = vsrc_first(&src[s]);
				break;
			case SDL_SCANCODE_LEFTBRACKET:
				newPalSel = (palSel <= -1) ? palette_count - 1 : palSel - 1;
				break;
			case SDL_SCANCODE_RIGHTBRACKET:
				newPalSel = (palSel >= palette_count - 1) ? -1 : palSel + 1;
				break;
			case SDL_SCANCODE_ESCAPE:
				done = true;
				break;
			default:
				break;
			}
			if (newPalSel != palSel)
			{
				palSel = newPalSel;
				set_palette(palSel < 0 ? savedPal : palettes[palSel], 0, 255);
			}
			newkey = false;
		}
	}

	/* restore whatever palette was on screen when we opened */
	set_palette(savedPal, 0, 255);

	for (size_t i = 0; i < COUNTOF(tileBufs); ++i)
		free(tileBufs[i]);
	for (int i = 0; i < nLoaded; ++i)
		free_sprite2s(&loadedSheets[i]);
	for (int i = 0; i < nArr; ++i)
		free_local_sprite_array(&localArrs[i]);

	wait_noinput(false, false, true);
}

// The special-weapon id a twiddle (keyboardCombos row) triggers: its entry in
// (100, 100+SPECIAL_NUM], returned as 1..SPECIAL_NUM, or 0 if none is valid.
static int twiddle_special_id(int row)
{
	for (int k = 0; k < 8; ++k)
	{
		const int v = keyboardCombos[row][k];
		if (v > 100 && v <= 100 + SPECIAL_NUM)
			return v - 100;
	}
	return 0;
}

// Is this special safe to equip? The HUD blits special[id].itemgraphic every frame, so an
// out-of-range icon crashes instantly; guard on a real HUD icon, name, and effect type. Unlike
// endless we KEEP Invulnerability -- it doesn't crash, and debug wants it. notes.md §Endless mode.
static bool debug_special_is_safe(int id)
{
	if (id == 0)
		return true;  // None -- clears the equipped special, no icon drawn
	if (id < 1 || id > SPECIAL_NUM)
		return false;

	// Sprite count of spriteSheet10: entry[0] of the Uint16 offset table is the byte offset to
	// sprite 1 -- i.e. the table's own size -- so entry[0] / 2 is the number of sprites.
	unsigned iconMax = 0;
	if (spriteSheet10.data != NULL && spriteSheet10.size >= sizeof(Uint16))
		iconMax = SDL_SwapLE16(((Uint16 *)spriteSheet10.data)[0]) / (unsigned)sizeof(Uint16);

	return special[id].name[0] != '\0'
	    && special[id].stype >= 1 && special[id].stype <= 18
	    && special[id].itemgraphic >= 1 && special[id].itemgraphic <= iconMax;
}

// Debug-only: fault on purpose so the crash logger runs end-to-end (Force Crash row). Pointer must
// be a volatile file-scope global or /O2 folds the null store away and never faults. notes.md §Crash logging.
static int *volatile debug_crash_ptr;  // NULL; never assigned -> the dereference faults
static void debug_force_crash(void)
{
	*debug_crash_ptr = 0xDEAD;
}

void JE_debugMenu(bool center)
{
	SDL_Surface* temp_surface = VGAScreen;
	VGAScreen = VGAScreenSeg;

	// gameplay runs the mouse in relative mode; switch to absolute so the menu
	// can use the pointer, and restore on exit.
	const bool wasRelative = mouseGetRelative();
	mouseSetRelative(false);

	if (shopSpriteSheet.data == NULL)
		JE_loadCompShapes(&shopSpriteSheet, '1');  // mouse pointer sprites

	int off_x = 0, off_y = 0;
	if (center)
	{
		int menu_width = MIN(vga_width, LEGACY_WIDTH);
		off_x = (menu_width - DEBUG_MENU_WIDTH) / 2 - DEBUG_MENU_X;
		off_y = (vga_height - DEBUG_MENU_HEIGHT) / 2 - DEBUG_MENU_Y + 1;
	}

	/* Row identifiers. Keep this enum and menuItems[] below in lockstep order;
	 * everything else refers to rows by name. */
	enum {
		DBG_SHIP, DBG_FRONT_WEAPON, DBG_FRONT_POWER, DBG_REAR_WEAPON, DBG_REAR_POWER,
		DBG_SHIELD, DBG_GENERATOR, DBG_SIDEKICK_L, DBG_SIDEKICK_R, DBG_SPECIAL,
		DBG_TWIDDLE, DBG_AUTOFIRE_TWIDDLE, DBG_TOGGLE_FIRE,
		DBG_AUTOFIRE_SPECIAL, DBG_AUTOFIRE_CHARGE, DBG_INSTANT_CHARGE, DBG_INF_SIDEKICK_AMMO, DBG_INF_GENERATOR,
		DBG_GOD_MODE, DBG_NOCLIP, DBG_EXPERT_MODE, DBG_EXPERT_SETTINGS, DBG_AUTO_DIFFICULTY,
		DBG_DIFFICULTY, DBG_ADD_CASH, DBG_NO_ENEMY_FIRE, DBG_SKIP_LEVEL,
		DBG_PLAY_SOUND, DBG_PLAY_MUSIC, DBG_SPRITE_VIEWER, DBG_HITBOX, DBG_PERF,
		DBG_HANG_TIMEOUT, DBG_FORCE_CRASH,
		DBG_ROW_COUNT
	};

	const char* menuItems[] = {
						[DBG_SHIP]                = "Ship",
						[DBG_FRONT_WEAPON]        = "Front Weapon",
						[DBG_FRONT_POWER]         = "Front Power",
						[DBG_REAR_WEAPON]         = "Rear Weapon",
						[DBG_REAR_POWER]          = "Rear Power",
						[DBG_SHIELD]              = "Shield",
						[DBG_GENERATOR]           = "Generator",
						[DBG_SIDEKICK_L]          = "Sidekick L",
						[DBG_SIDEKICK_R]          = "Sidekick R",
						[DBG_SPECIAL]             = "Special",
						[DBG_TWIDDLE]             = "Twiddle",
						[DBG_AUTOFIRE_TWIDDLE]    = "Autofire Twiddle",
						[DBG_TOGGLE_FIRE]         = "Toggle Fire",
						[DBG_AUTOFIRE_SPECIAL]    = "Autofire Special",
						[DBG_AUTOFIRE_CHARGE]     = "Autofire Charge Sidekicks",
						[DBG_INSTANT_CHARGE]      = "Instant Charge Sidekicks",
						[DBG_INF_SIDEKICK_AMMO]   = "Inf Sidekick Ammo",
						[DBG_INF_GENERATOR]       = "Inf Generator",
						[DBG_GOD_MODE]            = "God Mode",
						[DBG_NOCLIP]              = "Noclip",
						[DBG_EXPERT_MODE]         = "Expert Mode",
						[DBG_EXPERT_SETTINGS]     = "Expert Settings",
						[DBG_AUTO_DIFFICULTY]     = "Auto-Adjust Difficulty",
						[DBG_DIFFICULTY]          = "Difficulty",
						[DBG_ADD_CASH]            = "Add Cash",
						[DBG_NO_ENEMY_FIRE]       = "No Enemy Fire",
						[DBG_SKIP_LEVEL]          = "Skip to Next Level",
						[DBG_PLAY_SOUND]          = "Play Sound",
						[DBG_PLAY_MUSIC]          = "Play Music",
						[DBG_SPRITE_VIEWER]       = "Sprite Viewer",
						[DBG_HITBOX]              = "Hitbox Overlay",
						[DBG_PERF]                = "Perf Overlay",
						[DBG_HANG_TIMEOUT]        = "Hang Watchdog",
						[DBG_FORCE_CRASH]         = "Force Crash (test)",
	};

	const size_t menuCount = DBG_ROW_COUNT;
	size_t selected = 0;

	/* transient debug-action values; persist across menu opens within a session */
	static int dbgSoundId = 1, dbgMusicId = 0, dbgTwiddleId = 0;
	debugTwiddleSpecial = (JE_byte)twiddle_special_id(dbgTwiddleId);  // keep the armed twiddle in sync

	/* Add Cash is an inline numeric field: while the row is selected you type a value (digits
	 * append, Backspace deletes) and Enter sets cash to it. Starts empty each open; the row shows
	 * the live cash when you're not on it. See the value display and input switch below. */
	const int cashMaxDigits = 9;               // 999,999,999 cap: ample, and safely within ulong
	char dbgCashStr[16] = "";
	char dbgHangStr[8] = "";                   // inline typed field for the Hang Watchdog row (seconds)

	wait_noinput(false, false, true);
	newkey = newmouse = false;  // don't let the click/key that opened us leak in

	/* Panel geometry. Spans the DEBUG_MENU_X..+WIDTH area (centred in the
	 * playfield) that the in-game caller saves and restores around this menu. */
	const int px0 = DEBUG_MENU_X + off_x, py0 = DEBUG_MENU_Y + off_y;
	const int px1 = DEBUG_MENU_X + DEBUG_MENU_WIDTH - 1 + off_x, py1 = vga_height - 5 + off_y;
	const int title_h = 15;                 /* height of the title strip      */
	const int items_top = py0 + title_h + 3;
	const int items_bottom = py1 - 9;       /* leave room for the footer hint */
	/* Fixed row density (matching the original ~19-row menu); the list scrolls
	 * when there are more items than fit, so rows never get squashed. */
	const int kVisibleTarget = 19;
	const int visibleRows = ((int)menuCount < kVisibleTarget) ? (int)menuCount : kVisibleTarget;
	const int row_h = (items_bottom - items_top) / visibleRows;
	const int mid_x = (px0 + px1) / 2;
	int scrollTop = 0;  /* index of the first visible row */
	int prev_mx = mouse_x, prev_my = mouse_y;  /* for motion-based hover */

	/* hue 15 is the grey/white ramp (palette indices 240..255), so these are
	 * safe, theme-neutral shades for the panel chrome. */
	enum {
		C_PANEL_BG = 0xF1,  /* body fill: near-black grey                 */
		C_TITLE_BG = 0xF3,  /* title strip, a touch lighter than the body */
		C_DIVIDER  = 0xF6,  /* line under the title                       */
		C_EDGE_HI  = 0xFB,  /* top/left border highlight                  */
		C_EDGE_LO  = 0xF4,  /* bottom/right border shade                  */
		C_SEL_BAR  = 0xF5   /* highlight bar behind the selected row      */
	};

	bool done = false;
	while (!done)
	{
		/* Solid panel, fully repainted every frame: nothing from the game
		 * behind can bleed through and no shading/text can accumulate. */
		fill_rectangle_xy(VGAScreen, px0, py0, px1, py1, C_PANEL_BG);

		/* beveled border */
		fill_rectangle_xy(VGAScreen, px0, py0, px1, py0, C_EDGE_HI); /* top    */
		fill_rectangle_xy(VGAScreen, px0, py0, px0, py1, C_EDGE_HI); /* left   */
		fill_rectangle_xy(VGAScreen, px0, py1, px1, py1, C_EDGE_LO); /* bottom */
		fill_rectangle_xy(VGAScreen, px1, py0, px1, py1, C_EDGE_LO); /* right  */

		/* title strip + heading */
		fill_rectangle_xy(VGAScreen, px0 + 1, py0 + 1, px1 - 1, py0 + title_h - 1, C_TITLE_BG);
		fill_rectangle_xy(VGAScreen, px0 + 1, py0 + title_h, px1 - 1, py0 + title_h, C_DIVIDER);
		draw_font_hv_shadow(VGAScreen, mid_x, py0 + 3, "DEBUG  MENU", normal_font, centered, 15, 4, true, 1);

		/* keep the selection within the scrolled window */
		if ((int)selected < scrollTop)
			scrollTop = (int)selected;
		else if ((int)selected >= scrollTop + visibleRows)
			scrollTop = (int)selected - visibleRows + 1;
		if (scrollTop > (int)menuCount - visibleRows)
			scrollTop = (int)menuCount - visibleRows;
		if (scrollTop < 0)
			scrollTop = 0;

		for (int vis = 0; vis < visibleRows; ++vis)
		{
			size_t i = (size_t)scrollTop + vis;
			int ry = items_top + vis * row_h;
			bool sel = (i == selected);

			char buf[40];
			bool invalid = false;
			switch (i)
			{
			case DBG_SHIP:
				if (player[0].items.ship <= SHIP_NUM)
					snprintf(buf, sizeof(buf), "%s", ships[player[0].items.ship].name);
				else
				{
					snprintf(buf, sizeof(buf), "%d", player[0].items.ship);
					invalid = true;
				}
				break;
			case DBG_FRONT_WEAPON:
				if (player[0].items.weapon[FRONT_WEAPON].id <= PORT_NUM)
					snprintf(buf, sizeof(buf), "%s", weaponPort[player[0].items.weapon[FRONT_WEAPON].id].name);
				else
				{
					snprintf(buf, sizeof(buf), "%d", player[0].items.weapon[FRONT_WEAPON].id);
					invalid = true;
				}
				break;
			case DBG_FRONT_POWER:
				snprintf(buf, sizeof(buf), "%d", player[0].items.weapon[FRONT_WEAPON].power);
				break;
			case DBG_REAR_WEAPON:
				if (player[0].items.weapon[REAR_WEAPON].id <= PORT_NUM)
					snprintf(buf, sizeof(buf), "%s", weaponPort[player[0].items.weapon[REAR_WEAPON].id].name);
				else
				{
					snprintf(buf, sizeof(buf), "%d", player[0].items.weapon[REAR_WEAPON].id);
					invalid = true;
				}
				break;
			case DBG_REAR_POWER:
				snprintf(buf, sizeof(buf), "%d", player[0].items.weapon[REAR_WEAPON].power);
				break;
			case DBG_SHIELD:
				if (player[0].items.shield <= SHIELD_NUM)
					snprintf(buf, sizeof(buf), "%s", shields[player[0].items.shield].name);
				else
				{
					snprintf(buf, sizeof(buf), "%d", player[0].items.shield);
					invalid = true;
				}
				break;
			case DBG_GENERATOR:
				if (player[0].items.generator <= POWER_NUM)
					snprintf(buf, sizeof(buf), "%s", powerSys[player[0].items.generator].name);
				else
				{
					snprintf(buf, sizeof(buf), "%d", player[0].items.generator);
					invalid = true;
				}
				break;
			case DBG_SIDEKICK_L:
				if (player[0].items.sidekick[LEFT_SIDEKICK] <= OPTION_NUM)
					snprintf(buf, sizeof(buf), "%s", options[player[0].items.sidekick[LEFT_SIDEKICK]].name);
				else
				{
					snprintf(buf, sizeof(buf), "%d", player[0].items.sidekick[LEFT_SIDEKICK]);
					invalid = true;
				}
				break;
			case DBG_SIDEKICK_R:
				if (player[0].items.sidekick[RIGHT_SIDEKICK] <= OPTION_NUM)
					snprintf(buf, sizeof(buf), "%s", options[player[0].items.sidekick[RIGHT_SIDEKICK]].name);
				else
				{
					snprintf(buf, sizeof(buf), "%d", player[0].items.sidekick[RIGHT_SIDEKICK]);
					invalid = true;
				}
				break;
			case DBG_SPECIAL:
				if (player[0].items.special <= SPECIAL_NUM)
					snprintf(buf, sizeof(buf), "%s", special[player[0].items.special].name);
				else
				{
					snprintf(buf, sizeof(buf), "%d", player[0].items.special);
					invalid = true;
				}
				break;
			case DBG_TWIDDLE:
			{
				const int sid = twiddle_special_id(dbgTwiddleId);
				if (sid >= 1 && sid <= SPECIAL_NUM)
					snprintf(buf, sizeof(buf), "%s", special[sid].name);
				else
				{
					snprintf(buf, sizeof(buf), "#%d", dbgTwiddleId + 1);
					invalid = true;
				}
				break;
			}
			case DBG_AUTOFIRE_TWIDDLE:
				sprintf(buf, "%s", debugAutofireTwiddle ? "ON" : "OFF");
				break;
			case DBG_TOGGLE_FIRE:
				sprintf(buf, "%s", debugToggleFire ? "ON" : "OFF");
				break;
			case DBG_AUTOFIRE_SPECIAL:
				sprintf(buf, "%s", autoFireSpecial ? "ON" : "OFF");
				break;
			case DBG_AUTOFIRE_CHARGE:
			{
				static const char *const modes[CHARGE_AUTOFIRE_NUM] = { "No", "Yes", "Fully charged only", "Yes (fastest)" };
				snprintf(buf, sizeof(buf), "%s", modes[chargeSidekickAutofire % CHARGE_AUTOFIRE_NUM]);
				break;
			}
			case DBG_INSTANT_CHARGE:
				sprintf(buf, "%s", cheatInstantCharge ? "ON" : "OFF");
				break;
			case DBG_INF_SIDEKICK_AMMO:
				sprintf(buf, "%s", cheatInfiniteSidekickAmmo ? "ON" : "OFF");
				break;
			case DBG_INF_GENERATOR:
				sprintf(buf, "%s", cheatInfiniteGenerator ? "ON" : "OFF");
				break;
			case DBG_GOD_MODE:
				snprintf(buf, sizeof(buf), "%s", debug_god_mode_names[debug_god_mode_get()]);
				break;
			case DBG_NOCLIP:
			{
				static const char *const modes[NOCLIP_NUM] = { "OFF", "ON", "ON (Transparent)" };
				snprintf(buf, sizeof(buf), "%s", modes[noclipMode % NOCLIP_NUM]);
				break;
			}
			case DBG_EXPERT_MODE:
				sprintf(buf, "%s", expertMode ? "ON" : "OFF");
				break;
			case DBG_EXPERT_SETTINGS:
				sprintf(buf, "%s", ">>");  // drill-in to Expert Settings submenu
				break;
			case DBG_AUTO_DIFFICULTY:
				sprintf(buf, "%s", difficultyAdjust ? "ON" : "OFF");
				break;
			case DBG_DIFFICULTY:
				snprintf(buf, sizeof(buf), "%s", difficultyNameB[difficultyLevel]);
				break;
			case DBG_ADD_CASH:  // editable: type a value while selected, Enter sets cash to it
				if (sel)
					snprintf(buf, sizeof(buf), "%s|", dbgCashStr);  // your input + '|' caret (empty => just the caret)
				else
					snprintf(buf, sizeof(buf), "%lu", (unsigned long)player[0].cash);  // live cash when not editing
				break;
			case DBG_NO_ENEMY_FIRE:
				sprintf(buf, "%s", cheatNoEnemyFire ? "ON" : "OFF");
				break;
			case DBG_SKIP_LEVEL:  // action
				sprintf(buf, "%s", "[Enter]");
				break;
			case DBG_PLAY_SOUND:  // Left/Right id, Enter plays
				snprintf(buf, sizeof(buf), "%d", dbgSoundId);
				break;
			case DBG_PLAY_MUSIC:  // Left/Right id, Enter plays
				snprintf(buf, sizeof(buf), "%d", dbgMusicId);
				break;
			case DBG_SPRITE_VIEWER:
				sprintf(buf, "%s", ">>");  // drill-in to Sprite Viewer submenu
				break;
			case DBG_HITBOX:
				sprintf(buf, "%s", debugHitboxOverlay ? "ON" : "OFF");
				break;
			case DBG_PERF:
				sprintf(buf, "%s", debugPerfOverlay ? "ON" : "OFF");
				break;
			case DBG_HANG_TIMEOUT:  // editable: type seconds while selected, Enter applies (clamped)
				if (sel)
					snprintf(buf, sizeof(buf), "%s|", dbgHangStr);  // your input + '|' caret
				else
					snprintf(buf, sizeof(buf), "%ds", crashlog_get_hang_timeout());  // live value
				break;
			case DBG_FORCE_CRASH:  // action: deliberately crash to test the crash logger
				sprintf(buf, "%s", "[Enter]");
				break;
			default:
				buf[0] = '\0';
				break;
			}

			/* trim trailing whitespace */
			for (int j = (int)strlen(buf) - 1; j >= 0 && isspace((unsigned char)buf[j]); --j)
				buf[j] = '\0';

			/* highlight bar behind the active row */
			if (sel)
				fill_rectangle_xy(VGAScreen, px0 + 3, ry - 1, px1 - 3, ry + row_h - 2, C_SEL_BAR);

			/* label (left) */
			draw_font_hv_shadow(VGAScreen, px0 + 12, ry, menuItems[i], small_font, left_aligned,
			                    15, sel ? 5 : -1, true, 1);

			/* value (right); dim plain "OFF", red for invalid entries */
			Sint8 val_bright = sel ? 5 : 0;
			if (!invalid && strcmp(buf, "OFF") == 0)
				val_bright -= 4;
			draw_font_hv_shadow(VGAScreen, px1 - 9, ry, buf, small_font, right_aligned,
			                    invalid ? 4 : 15, val_bright, true, 1);

			/* selection marker */
			if (sel)
				draw_font_hv(VGAScreen, px0 + 5, ry, ">", small_font, left_aligned, 15, 6);
		}

		/* scrollbar track + thumb (only when the list overflows the window) */
		if ((int)menuCount > visibleRows)
		{
			const int track_top = items_top - 1;
			const int track_bot = items_top + visibleRows * row_h - 2;
			const int track_h = track_bot - track_top;
			fill_rectangle_xy(VGAScreen, px1 - 4, track_top, px1 - 3, track_bot, C_EDGE_LO);

			int thumb_h = track_h * visibleRows / (int)menuCount;
			if (thumb_h < 4)
				thumb_h = 4;
			const int denom = (int)menuCount - visibleRows;
			const int thumb_y = track_top + (denom > 0 ? (track_h - thumb_h) * scrollTop / denom : 0);
			fill_rectangle_xy(VGAScreen, px1 - 4, thumb_y, px1 - 3, thumb_y + thumb_h, C_EDGE_HI);
		}

		/* footer hint (the Add Cash row swaps in typing instructions) */
		draw_font_hv(VGAScreen, mid_x, py1 - 8,
		             (selected == DBG_ADD_CASH || selected == DBG_HANG_TIMEOUT)
		                 ? "Type a number   Enter set   Esc close"
		                 : "Left/Right change   Enter use   Esc close",
		             small_font, centered, 15, -3);

		mouseCursor = MOUSE_POINTER_NORMAL;
		JE_mouseStart();
		JE_showVGA();
		JE_mouseReplace();

		push_joysticks_as_keyboard();
		service_SDL_events(true);

		/* wheel scrolls the selection; hover highlights on pointer motion; a click
		 * acts on the row (left = activate/advance, right = reverse, Enter rows drill in) */
		{
			if (mouse_scroll != 0)
			{
				int ns = (int)selected - mouse_scroll;
				if (ns < 0)
					ns = 0;
				if (ns > (int)menuCount - 1)
					ns = (int)menuCount - 1;
				selected = (size_t)ns;
				mouse_scroll = 0;
			}
			if (mouse_x != prev_mx || mouse_y != prev_my)
			{
				const int hov = panel_row_at(mouse_x, mouse_y, px0, px1, items_top,
				                             row_h, visibleRows, scrollTop, (int)menuCount);
				if (hov >= 0)
					selected = hov;
			}
			prev_mx = mouse_x;
			prev_my = mouse_y;
			if (newmouse)
			{
				const int r = panel_row_at(lastmouse_x, lastmouse_y, px0, px1, items_top,
				                           row_h, visibleRows, scrollTop, (int)menuCount);
				if (r >= 0)
				{
					selected = r;
					const bool enterRow = (r == DBG_EXPERT_SETTINGS || r == DBG_ADD_CASH ||
					                       r == DBG_SKIP_LEVEL || r == DBG_PLAY_SOUND ||
					                       r == DBG_PLAY_MUSIC || r == DBG_SPRITE_VIEWER ||
					                       r == DBG_TWIDDLE || r == DBG_FORCE_CRASH ||
					                       r == DBG_HANG_TIMEOUT);
					newkey = true;
					lastkey_scan = (lastmouse_but == SDL_BUTTON_RIGHT) ? SDL_SCANCODE_LEFT
					             : (enterRow ? SDL_SCANCODE_RETURN : SDL_SCANCODE_RIGHT);
				}
				newmouse = false;
			}
		}

		if (newkey)
		{
			switch (lastkey_scan)
			{
			case SDL_SCANCODE_UP:
				selected = (selected == 0) ? menuCount - 1 : selected - 1;
				break;
			case SDL_SCANCODE_DOWN:
				selected = (selected + 1) % menuCount;
				break;
			case SDL_SCANCODE_LEFT:
				switch (selected)
				{
				case DBG_SHIP: if (player[0].items.ship > 0) --player[0].items.ship; break;
				case DBG_FRONT_WEAPON: if (player[0].items.weapon[FRONT_WEAPON].id > 0) --player[0].items.weapon[FRONT_WEAPON].id; break;
				case DBG_FRONT_POWER: if (player[0].items.weapon[FRONT_WEAPON].power > 1) --player[0].items.weapon[FRONT_WEAPON].power; break;
				case DBG_REAR_WEAPON: if (player[0].items.weapon[REAR_WEAPON].id > 0) --player[0].items.weapon[REAR_WEAPON].id; break;
				case DBG_REAR_POWER: if (player[0].items.weapon[REAR_WEAPON].power > 1) --player[0].items.weapon[REAR_WEAPON].power; break;
				case DBG_SHIELD: if (player[0].items.shield > 0) --player[0].items.shield; break;
				case DBG_GENERATOR: if (player[0].items.generator > 0) --player[0].items.generator; break;
				case DBG_SIDEKICK_L: if (player[0].items.sidekick[LEFT_SIDEKICK] > 0) --player[0].items.sidekick[LEFT_SIDEKICK]; break;
				case DBG_SIDEKICK_R: if (player[0].items.sidekick[RIGHT_SIDEKICK] > 0) --player[0].items.sidekick[RIGHT_SIDEKICK]; break;
				case DBG_SPECIAL:  // step to the previous crash-safe special (skip bad-icon slots)
					for (int nid = (int)player[0].items.special - 1; nid >= 0; --nid)
						if (debug_special_is_safe(nid)) { player[0].items.special = (JE_byte)nid; break; }
					break;
				case DBG_TWIDDLE:
					dbgTwiddleId = (dbgTwiddleId + (int)COUNTOF(keyboardCombos) - 1) % (int)COUNTOF(keyboardCombos);
					debugTwiddleSpecial = (JE_byte)twiddle_special_id(dbgTwiddleId);
					break;
				case DBG_AUTOFIRE_TWIDDLE: debugAutofireTwiddle = !debugAutofireTwiddle; break;
				case DBG_TOGGLE_FIRE: debugToggleFire = !debugToggleFire; debugToggleFireActive = false; break;
				case DBG_AUTOFIRE_SPECIAL: autoFireSpecial = !autoFireSpecial; break;
				case DBG_AUTOFIRE_CHARGE: chargeSidekickAutofire = (chargeSidekickAutofire + CHARGE_AUTOFIRE_NUM - 1) % CHARGE_AUTOFIRE_NUM; break;
				case DBG_INSTANT_CHARGE: cheatInstantCharge = !cheatInstantCharge; break;
				case DBG_INF_SIDEKICK_AMMO: cheatInfiniteSidekickAmmo = !cheatInfiniteSidekickAmmo; break;
				case DBG_INF_GENERATOR: cheatInfiniteGenerator = !cheatInfiniteGenerator; break;
				case DBG_GOD_MODE: debug_god_mode_set((debug_god_mode_get() + 3) % 4); break;
				case DBG_NOCLIP: noclipMode = (noclipMode + NOCLIP_NUM - 1) % NOCLIP_NUM; break;
				case DBG_EXPERT_MODE: expertMode = !expertMode; break;
				case DBG_EXPERT_SETTINGS: break;  // opens on Right/Enter
				case DBG_AUTO_DIFFICULTY: difficultyAdjust = !difficultyAdjust; break;
				case DBG_DIFFICULTY: if (difficultyLevel > DIFFICULTY_WIMP) --difficultyLevel; break;
				case DBG_NO_ENEMY_FIRE: cheatNoEnemyFire = !cheatNoEnemyFire; break;
				case DBG_PLAY_SOUND: if (dbgSoundId > 1) --dbgSoundId; break;
				case DBG_PLAY_MUSIC: if (dbgMusicId > 0) --dbgMusicId; break;
				case DBG_SPRITE_VIEWER: break;  // opens on Right/Enter
				case DBG_HITBOX: debugHitboxOverlay = !debugHitboxOverlay; break;
				case DBG_PERF: debugPerfOverlay = !debugPerfOverlay; break;
				default: break;  // Add Cash / Hang Watchdog / Skip Level are Enter-only actions
				}
				break;
			case SDL_SCANCODE_RIGHT:
				switch (selected)
				{
				case DBG_SHIP: ++player[0].items.ship; break;
				case DBG_FRONT_WEAPON: ++player[0].items.weapon[FRONT_WEAPON].id; break;
				case DBG_FRONT_POWER: if (player[0].items.weapon[FRONT_WEAPON].power < 11) ++player[0].items.weapon[FRONT_WEAPON].power; break;
				case DBG_REAR_WEAPON: ++player[0].items.weapon[REAR_WEAPON].id; break;
				case DBG_REAR_POWER: if (player[0].items.weapon[REAR_WEAPON].power < 11) ++player[0].items.weapon[REAR_WEAPON].power; break;
				case DBG_SHIELD: ++player[0].items.shield; break;
				case DBG_GENERATOR: ++player[0].items.generator; break;
				case DBG_SIDEKICK_L: ++player[0].items.sidekick[LEFT_SIDEKICK]; break;
				case DBG_SIDEKICK_R: ++player[0].items.sidekick[RIGHT_SIDEKICK]; break;
				case DBG_SPECIAL:  // step to the next crash-safe special (skip bad-icon slots)
					for (int nid = (int)player[0].items.special + 1; nid <= SPECIAL_NUM; ++nid)
						if (debug_special_is_safe(nid)) { player[0].items.special = (JE_byte)nid; break; }
					break;
				case DBG_TWIDDLE:
					dbgTwiddleId = (dbgTwiddleId + 1) % (int)COUNTOF(keyboardCombos);
					debugTwiddleSpecial = (JE_byte)twiddle_special_id(dbgTwiddleId);
					break;
				case DBG_AUTOFIRE_TWIDDLE: debugAutofireTwiddle = !debugAutofireTwiddle; break;
				case DBG_TOGGLE_FIRE: debugToggleFire = !debugToggleFire; debugToggleFireActive = false; break;
				case DBG_AUTOFIRE_SPECIAL: autoFireSpecial = !autoFireSpecial; break;
				case DBG_AUTOFIRE_CHARGE: chargeSidekickAutofire = (chargeSidekickAutofire + 1) % CHARGE_AUTOFIRE_NUM; break;
				case DBG_INSTANT_CHARGE: cheatInstantCharge = !cheatInstantCharge; break;
				case DBG_INF_SIDEKICK_AMMO: cheatInfiniteSidekickAmmo = !cheatInfiniteSidekickAmmo; break;
				case DBG_INF_GENERATOR: cheatInfiniteGenerator = !cheatInfiniteGenerator; break;
				case DBG_GOD_MODE: debug_god_mode_set((debug_god_mode_get() + 1) % 4); break;
				case DBG_NOCLIP: noclipMode = (noclipMode + 1) % NOCLIP_NUM; break;
				case DBG_EXPERT_MODE: expertMode = !expertMode; break;
				case DBG_EXPERT_SETTINGS: JE_expertSettingsMenu(off_x, off_y); break;
				case DBG_AUTO_DIFFICULTY: difficultyAdjust = !difficultyAdjust; break;
				case DBG_DIFFICULTY: if (difficultyLevel < DIFFICULTY_10) ++difficultyLevel; break;
				case DBG_NO_ENEMY_FIRE: cheatNoEnemyFire = !cheatNoEnemyFire; break;
				case DBG_PLAY_SOUND: if (dbgSoundId < SOUND_COUNT) ++dbgSoundId; break;
				case DBG_PLAY_MUSIC: if (dbgMusicId < MUSIC_NUM - 1) ++dbgMusicId; break;
				case DBG_SPRITE_VIEWER: JE_spriteViewer(off_x, off_y); break;
				case DBG_HITBOX: debugHitboxOverlay = !debugHitboxOverlay; break;
				case DBG_PERF: debugPerfOverlay = !debugPerfOverlay; break;
				default: break;  // Add Cash / Hang Watchdog / Skip Level are Enter-only actions
				}
				break;
			case SDL_SCANCODE_RETURN:
			case SDL_SCANCODE_KP_ENTER:
			case SDL_SCANCODE_SPACE:
				switch (selected)
				{
				case DBG_EXPERT_SETTINGS:  // drill-in
					JE_expertSettingsMenu(off_x, off_y);
					break;
				case DBG_SPRITE_VIEWER:  // drill-in
					JE_spriteViewer(off_x, off_y);
					break;
				case DBG_TWIDDLE:  // request a one-shot fire of the selected twiddle's special
				{
					debugTwiddleSpecial = (JE_byte)twiddle_special_id(dbgTwiddleId);
					if (debugTwiddleSpecial > 0)
					{
						debugTwiddleTrigger = true;
						done = true;  // close the menu so it fires when gameplay resumes
					}
					break;
				}
				case DBG_ADD_CASH:  // apply the typed value (built digit-by-digit, capped, so no overflow)
#if defined(__SWITCH__) || defined(__vita__)
					// No physical keyboard on the consoles: pop the software keyboard to fill the field.
					console_swkbd(dbgCashStr, sizeof(dbgCashStr), cashMaxDigits, dbgCashStr, "Add Cash", true);
#endif
					if (dbgCashStr[0])  // ignore a bare Enter on an empty field (don't zero cash by accident)
					{
						ulong v = 0;
						for (const char *c = dbgCashStr; *c >= '0' && *c <= '9'; ++c)
							v = v * 10u + (ulong)(*c - '0');
						player[0].cash = v;
					}
					break;
				case DBG_HANG_TIMEOUT:  // apply the typed watchdog timeout in seconds (clamps to range)
#if defined(__SWITCH__) || defined(__vita__)
					console_swkbd(dbgHangStr, sizeof(dbgHangStr), sizeof(dbgHangStr) - 1, dbgHangStr, "Hang timeout (seconds)", true);
#endif
					if (dbgHangStr[0])  // ignore a bare Enter on an empty field
					{
						int v = 0;
						for (const char *c = dbgHangStr; *c >= '0' && *c <= '9'; ++c)
							v = v * 10 + (*c - '0');
						crashlog_set_hang_timeout(v);  // clamps into [MIN, MAX]
						dbgHangStr[0] = '\0';  // committed -> clear so the row shows the applied (clamped) value
					}
					break;
				case DBG_SKIP_LEVEL:  // flag it and close so the game processes it
					reallyEndLevel = true;
					done = true;
					break;
				case DBG_FORCE_CRASH:  // deliberately fault to exercise the crash logger
					debug_force_crash();
					break;
				case DBG_PLAY_SOUND:  // Play the selected sound sample
					JE_playSampleNum((JE_byte)dbgSoundId);
					break;
				case DBG_PLAY_MUSIC:  // Play the selected music track
					play_song((unsigned int)dbgMusicId);
					break;
				default:
					break;
				}
				break;
			case SDL_SCANCODE_ESCAPE:
				done = true;
				break;
			default:
			{
				/* Inline typed numeric fields (Add Cash, Hang Watchdog): digits append,
				 * Backspace/Delete removes. Read scancodes directly -- this menu's event
				 * pump can drop SDL_TEXTINPUT. */
				char *editStr = (selected == DBG_ADD_CASH)    ? dbgCashStr
				              : (selected == DBG_HANG_TIMEOUT) ? dbgHangStr
				                                               : NULL;
				const int editMax = (selected == DBG_ADD_CASH) ? cashMaxDigits : 4;
				if (editStr != NULL)
				{
					int digit = -1;
					if (lastkey_scan >= SDL_SCANCODE_1 && lastkey_scan <= SDL_SCANCODE_9)
						digit = lastkey_scan - SDL_SCANCODE_1 + 1;
					else if (lastkey_scan == SDL_SCANCODE_0)
						digit = 0;
					else if (lastkey_scan >= SDL_SCANCODE_KP_1 && lastkey_scan <= SDL_SCANCODE_KP_9)
						digit = lastkey_scan - SDL_SCANCODE_KP_1 + 1;
					else if (lastkey_scan == SDL_SCANCODE_KP_0)
						digit = 0;

					if (digit >= 0)
					{
						size_t l = strlen(editStr);
						if (strcmp(editStr, "0") == 0)  // replace a lone leading zero
							l = 0;
						if ((int)l < editMax)
						{
							editStr[l] = (char)('0' + digit);
							editStr[l + 1] = '\0';
						}
					}
					else if (lastkey_scan == SDL_SCANCODE_BACKSPACE || lastkey_scan == SDL_SCANCODE_DELETE)
					{
						size_t l = strlen(editStr);
						if (l > 0)
							editStr[l - 1] = '\0';
					}
				}
				break;
			}
			}

			newkey = false;
		}
	}

	mouseSetRelative(wasRelative);

	VGAScreen = temp_surface;
}


void JE_inGameHelp(void)
{
	// Presents its own frames inside the gameplay tick's render-list recording
	// window; suspend recording (see JE_doInGameSetup for the rationale).
	const bool rl_was_recording = render_list_recording;
	render_list_recording = false;

	SDL_Surface *temp_surface = VGAScreen;
	VGAScreen = VGAScreenSeg; /* side-effect of game_screen */

	//tempScreenSeg = VGAScreenSeg;

	JE_clearKeyboard();
	JE_wipeKey();

	JE_barShade(VGAScreen, 1, 1, 262, vga_height - 18); /*Main Box*/
	JE_barShade(VGAScreen, 3, 3, 260, vga_height - 20);
	JE_barShade(VGAScreen, 5, 5, 258, vga_height - 22);
	JE_barShade(VGAScreen, 7, 7, 256, vga_height - 24);
	fill_rectangle_xy(VGAScreen, 9, 9, 254, vga_height - 26, 0);

	if (twoPlayerMode)  // Two-Player Help
	{
		helpBoxColor = 3;
		helpBoxBrightness = 3;
		JE_HBox(VGAScreen, 20,  4, 36, 50);

		// weapon help
		blit_sprite(VGAScreenSeg, 2, 21, OPTION_SHAPES, 43);
		helpBoxColor = 5;
		helpBoxBrightness = 3;
		JE_HBox(VGAScreen, 55, 20, 37, 40);

		// sidekick help
		blit_sprite(VGAScreenSeg, 5, 36, OPTION_SHAPES, 41);
		helpBoxColor = 5;
		helpBoxBrightness = 3;
		JE_HBox(VGAScreen, 40, 43, 34, 44);

		// shield/armor help
		blit_sprite(VGAScreenSeg, 2, 79, OPTION_SHAPES, 42);
		helpBoxColor = 5;
		helpBoxBrightness = 3;
		JE_HBox(VGAScreen, 54, 84, 35, 40);

		helpBoxColor = 5;
		helpBoxBrightness = 3;
		JE_HBox(VGAScreen, 5, 126, 38, 55);
		helpBoxColor = 5;
		helpBoxBrightness = 3;
		JE_HBox(VGAScreen, 5, 160, 39, 55);
	}
	else
	{
		// power bar help
		blit_sprite(VGAScreenSeg, 15, 5, OPTION_SHAPES, 40);
		helpBoxColor = 5;
		helpBoxBrightness = 3;
		JE_HBox(VGAScreen, 40, 10, 31, 45);

		// weapon help
		blit_sprite(VGAScreenSeg, 5, 37, OPTION_SHAPES, 39);
		helpBoxColor = 5;
		helpBoxBrightness = 3;
		JE_HBox(VGAScreen, 40, 40, 32, 44);
		helpBoxColor = 5;
		helpBoxBrightness = 3;
		JE_HBox(VGAScreen, 40, 60, 33, 44);

		// sidekick help
		blit_sprite(VGAScreenSeg, 5, 98, OPTION_SHAPES, 41);
		helpBoxColor = 5;
		helpBoxBrightness = 3;
		JE_HBox(VGAScreen, 40, 103, 34, 44);

		// shield/armor help
		blit_sprite(VGAScreenSeg, 2, 138, OPTION_SHAPES, 42);
		helpBoxColor = 5;
		helpBoxBrightness = 3;
		JE_HBox(VGAScreen, 54, 143, 35, 40);
	}

	// "press a key"
	blit_sprite(VGAScreenSeg, 16, vga_height - 11, OPTION_SHAPES, 36);  // in-game text area
	JE_outText(VGAScreenSeg, 120 - JE_textWidth(miscText[5 - 1], TINY_FONT) / 2 + 20, vga_height - 10, miscText[5 - 1], 0, 4);

	do
	{
		service_SDL_events(true);

		JE_mouseStart();
		JE_showVGA();
		JE_mouseReplace();

		// Present at display rate for a smooth cursor; vsync-on paces via showVGA.
		if (!output_vsync)
			limit_render_fps();

		push_joysticks_as_keyboard();
		service_SDL_events(false);

		NETWORK_KEEP_ALIVE();
	} while (!(newkey || newmouse));

	textErase = 1;

	VGAScreen = temp_surface;

	render_list_recording = rl_was_recording;
}

void JE_highScoreCheck(void)
{
	if (shopSpriteSheet.data == NULL)
		JE_loadCompShapes(&shopSpriteSheet, '1');  // need mouse pointer sprite

	Sint32 temp_score;

	for (int temp_p = 0; temp_p < (twoPlayerMode ? 2 : 1); ++temp_p)
	{
		JE_sortHighScores();

		int p = temp_p;
		int table = 10 + (initial_episode_num - 1) * 2;

		if (timedBattleMode)
		{
			// timed battle score is just money
			temp_score = player[0].cash;
			table = timeBattleSelection - 1;
		}
		else if (twoPlayerMode)
		{
			// ask for the highest scorer first
			if (player[0].cash < player[1].cash)
				p = (temp_p == 0) ? 1 : 0;

			temp_score = (p == 0) ? player[0].cash : player[1].cash;
			++table;
		}
		else
		{
			// single player highscore includes cost of upgrades
			temp_score = JE_totalScore(&player[0]);
		}

		int slot;
		for (slot = 0; slot < 3; ++slot)
		{
			if (temp_score > t2kHighScores[table][slot].score)
				break;
		}

		// did you get a high score?
		if (slot < 3)
		{
			// shift down old scores
			for (int i = 2; i > slot; --i)
				memcpy(&t2kHighScores[table][i], &t2kHighScores[table][i - 1], sizeof(T2KHighScoreType));

			wait_noinput(false, true, false);

			JE_clr256(VGAScreen);
			JE_showVGA();
			memcpy(colors, palettes[0], sizeof(colors));

			if (!timedBattleMode) // Doesn't play this music
				play_song(33);

			// Not part of the above condition
			{
				/* Enter Thy name */

				JE_byte flash = 8 * 16 + 10;
				JE_boolean fadein = true;
				JE_boolean quit = false, cancel = false;
				char stemp[30], tempstr[30];
				char buffer[256];

				// Absolute pointer for the on-screen OK/CANCEL buttons here too (same as
				// JE_operation): a Switch touch must click, not steer the ship. Restored
				// after the entry loop below.
				const bool hs_was_relative = mouseGetRelative();
				mouseSetRelative(false);

				strcpy(stemp, "                             ");
				temp = 0;

#if defined(__SWITCH__) || defined(__vita__)
				// No physical keyboard on the consoles: get the name from the software keyboard
				// and fill the field directly (see JE_operation for why not injected as an event).
				{
					char kb[29];
					kb[0] = '\0';
					if (console_swkbd(kb, sizeof(kb), 28, NULL, "Enter your name", false))
					{
						for (const char *c = kb; *c != '\0' && temp < 28; ++c)
						{
							const char u = (unsigned char)*c <= 127U ? (char)toupper((unsigned char)*c) : 0;
							if (u == ' ' || font_ascii[(unsigned char)u] != -1)
								stemp[temp++] = u;
						}
					}
				}
#endif

				// As astoundingly ugly as this makes the shade below look, this is in fact what Tyrian 2000 does.
				if (timedBattleMode)
					JE_loadPic(VGAScreen, 13, false);

				JE_barShade(VGAScreen, 65, 55, 255, 155);

				do
				{
					service_SDL_events(true);

					JE_dString(VGAScreen, JE_fontCenter(miscText[51], FONT_SHAPES), 3, miscText[51], FONT_SHAPES);

					temp3 = twoPlayerMode ? 58 + p : 53;

					JE_dString(VGAScreen, JE_fontCenter(miscText[temp3-1], SMALL_FONT_SHAPES), 30, miscText[temp3-1], SMALL_FONT_SHAPES);

					blit_sprite(VGAScreenSeg, 50, 50, OPTION_SHAPES, 35);  // message box

					if (twoPlayerMode)
					{
						sprintf(buffer, "%s %s", miscText[48 + p], miscText[53]);
						JE_textShade(VGAScreen, 60, 55, buffer, 11, 4, FULL_SHADE);
					}
					else
					{
						JE_textShade(VGAScreen, 60, 55, miscText[53], 11, 4, FULL_SHADE);
					}

					sprintf(buffer, "%s %d", miscText[37], temp_score);
					JE_textShade(VGAScreen, 70, 70, buffer, 11, 4, FULL_SHADE);

					do
					{
						flash = (flash == 8 * 16 + 10) ? 8 * 16 + 2 : 8 * 16 + 10;
						temp3 = (temp3 == 6) ? 2 : 6;

						strncpy(tempstr, stemp, temp);
						tempstr[temp] = '\0';
						JE_outText(VGAScreen, 65, 89, tempstr, 8, 3);
						tempW = 65 + JE_textWidth(tempstr, TINY_FONT);
						JE_barShade(VGAScreen, tempW + 2, 90, tempW + 6, 95);
						fill_rectangle_xy(VGAScreen, tempW + 1, 89, tempW + 5, 94, flash);

						for (int i = 0; i < 14; i++)
						{
							setDelay(1);

							JE_mouseStart();
							JE_showVGA();
							if (fadein)
							{
								fade_palette(colors, 15, 0, 255);
								fadein = false;
							}
							JE_mouseReplace();

							push_joysticks_as_keyboard();
							service_wait_delay();

							if (newkey || newmouse)
								break;
						}

					} while (!newkey && !newmouse && !new_text);

					if (!playing)
						play_song(31);

					if (mouseButton > 0)
					{
						if (mouseX > 56 && mouseX < 142 && mouseY > 123 && mouseY < 149)
						{
							quit = true;
						}
						else if (mouseX > 151 && mouseX < 237 && mouseY > 123 && mouseY < 149)
						{
							quit = true;
							cancel = true;
						}
					}
					else if (new_text)
					{
						for (size_t ti = 0U; last_text[ti] != '\0'; ++ti)
						{
							const char c = (unsigned char)last_text[ti] <= 127U ? toupper(last_text[ti]) : 0;
							if ((c == ' ' || font_ascii[(unsigned char)c] != -1) &&
							    temp < 28)
							{
								stemp[temp] = c;
								temp += 1;
							}
						}
					}
					else if (newkey)
					{
						switch (lastkey_scan)
						{
							case SDL_SCANCODE_BACKSPACE:
							case SDL_SCANCODE_DELETE:
								if (temp)
								{
									temp--;
									stemp[temp] = ' ';
								}
								break;
							case SDL_SCANCODE_ESCAPE:
								quit = true;
								cancel = true;
								break;
							case SDL_SCANCODE_RETURN:
								quit = true;
								break;
							default:
								break;
						}
					}
				} while (!quit);

				mouseSetRelative(hs_was_relative);

				// Timed Battle mode doesn't allow cancelling, so we ignore it
				if (!cancel || timedBattleMode)
				{
					t2kHighScores[table][slot].score = temp_score;
					strcpy(t2kHighScores[table][slot].playerName, stemp);
					t2kHighScores[table][slot].difficulty = difficultyLevel;
				}

				fade_black(15);
				JE_loadPic(VGAScreen, 2, false);

				JE_dString(VGAScreen, JE_fontCenter(miscText[50], FONT_SHAPES), 10, miscText[50], FONT_SHAPES);

				if (timedBattleMode)
					JE_dString(VGAScreen, JE_fontCenter(timed_battle_name[timeBattleSelection], SMALL_FONT_SHAPES), 35, timed_battle_name[timeBattleSelection], SMALL_FONT_SHAPES);
				else
					JE_dString(VGAScreen, JE_fontCenter(episode_name[episodeNum], SMALL_FONT_SHAPES), 35, episode_name[episodeNum], SMALL_FONT_SHAPES);

				for (int i = 0; i < 3; ++i)
				{
					if (i != slot)
					{
						sprintf(buffer, "~#%d:~  %d", i+1, t2kHighScores[table][i].score);
						JE_textShade(VGAScreen,  20, (i * 12) + 65, buffer, 15, 0, FULL_SHADE);
						JE_textShade(VGAScreen, 150, (i * 12) + 65, t2kHighScores[table][i].playerName, 15, 2, FULL_SHADE);
					}
				}

				JE_showVGA();

				fade_palette(colors, 15, 0, 255);

				sprintf(buffer, "~#%d:~  %d", slot+1, t2kHighScores[table][slot].score);

				frameCountMax = 6;
				textGlowFont = TINY_FONT;

				textGlowBrightness = 10;
				JE_outTextGlow(VGAScreenSeg,  20, (slot * 12) + 65, buffer);
				textGlowBrightness = 10;
				JE_outTextGlow(VGAScreenSeg, 150, (slot * 12) + 65, t2kHighScores[table][slot].playerName);
				textGlowBrightness = 10;
				JE_outTextGlow(VGAScreenSeg, JE_fontCenter(miscText[4], TINY_FONT), vga_height, miscText[4]);

				JE_showVGA();

				if (frameCountMax != 0)
					wait_input(true, true, true);

				fade_black(15);
			}

		}
	}
}

// increases game difficulty based on player's total score / total of players' scores
void adjust_difficulty(void)
{
	const float score_multiplier[10] =
	{
		0,     // Wimp  (doesn't exist)
		0.4f,  // Easy
		0.8f,  // Normal
		1.3f,  // Hard
		1.6f,  // Impossible
		2,     // Insanity
		2,     // Suicide
		3,     // Maniacal
		3,     // Zinglon
		3,     // Nortaneous
	};

	assert(initialDifficulty > 0 && initialDifficulty < 10);

	const ulong score = twoPlayerMode ? (player[0].cash + player[1].cash) : JE_totalScore(&player[0]),
	            adjusted_score = roundf(score * score_multiplier[initialDifficulty]);

	uint new_difficulty = 0;

	if (twoPlayerMode)
	{
		if (adjusted_score < 10000)
			new_difficulty = DIFFICULTY_EASY;
		else if (adjusted_score < 20000)
			new_difficulty = DIFFICULTY_NORMAL;
		else if (adjusted_score < 50000)
			new_difficulty = DIFFICULTY_HARD;
		else if (adjusted_score < 80000)
			new_difficulty = DIFFICULTY_IMPOSSIBLE;
		else if (adjusted_score < 125000)
			new_difficulty = DIFFICULTY_INSANITY;
		else if (adjusted_score < 200000)
			new_difficulty = DIFFICULTY_SUICIDE;
		else if (adjusted_score < 400000)
			new_difficulty = DIFFICULTY_MANIACAL;
		else if (adjusted_score < 600000)
			new_difficulty = DIFFICULTY_ZINGLON;
		else
			new_difficulty = DIFFICULTY_NORTANEOUS;
	}
	else
	{
		if (adjusted_score < 40000)
			new_difficulty = DIFFICULTY_EASY;
		else if (adjusted_score < 70000)
			new_difficulty = DIFFICULTY_NORMAL;
		else if (adjusted_score < 150000)
			new_difficulty = DIFFICULTY_HARD;
		else if (adjusted_score < 300000)
			new_difficulty = DIFFICULTY_IMPOSSIBLE;
		else if (adjusted_score < 600000)
			new_difficulty = DIFFICULTY_INSANITY;
		else if (adjusted_score < 1000000)
			new_difficulty = DIFFICULTY_SUICIDE;
		else if (adjusted_score < 2000000)
			new_difficulty = DIFFICULTY_MANIACAL;
		else if (adjusted_score < 3000000)
			new_difficulty = DIFFICULTY_ZINGLON;
		else
			new_difficulty = DIFFICULTY_NORTANEOUS;
	}

	difficultyLevel = MAX((unsigned)difficultyLevel, new_difficulty);
}

bool load_next_demo(void)
{
	if (++demo_num > 5)
		demo_num = 1;

	char demo_filename[9];
	snprintf(demo_filename, sizeof(demo_filename), "demo.%d", demo_num);
	demo_file = dir_fopen_die(data_dir(), demo_filename, "rb"); // TODO: only play demos from existing file (instead of dying)

	difficultyLevel = DIFFICULTY_NORMAL;
	bonusLevelCurrent = false;

	Uint8 temp;
	fread_u8_die(&temp, 1, demo_file);
	JE_initEpisode(temp);

	fread_die(levelName, 1, 10, demo_file);
	levelName[10] = '\0';

	fread_u8_die(&lvlFileNum, 1, demo_file);

	fread_u8_die(&player[0].items.weapon[FRONT_WEAPON].id,  1, demo_file);
	fread_u8_die(&player[0].items.weapon[REAR_WEAPON].id,   1, demo_file);
	fread_u8_die(&player[0].items.super_arcade_mode,        1, demo_file);
	fread_u8_die(&player[0].items.sidekick[LEFT_SIDEKICK],  1, demo_file);
	fread_u8_die(&player[0].items.sidekick[RIGHT_SIDEKICK], 1, demo_file);
	fread_u8_die(&player[0].items.generator,                1, demo_file);

	fread_u8_die(&player[0].items.sidekick_level,           1, demo_file); // could probably ignore
	fread_u8_die(&player[0].items.sidekick_series,          1, demo_file); // could probably ignore

	fread_u8_die(&initial_episode_num,                      1, demo_file); // could probably ignore

	fread_u8_die(&player[0].items.shield,                   1, demo_file);
	fread_u8_die(&player[0].items.special,                  1, demo_file);
	fread_u8_die(&player[0].items.ship,                     1, demo_file);

	for (uint i = 0; i < 2; ++i)
		fread_u8_die(&player[0].items.weapon[i].power,      1, demo_file);

	Uint8 unused[3];
	fread_u8_die(unused, 3, demo_file);

	fread_u8_die(&levelSong, 1, demo_file);

	demo_keys = 0;

	Uint8 temp2[2] = { 0, 0 };
	fread_u8(temp2, 2, demo_file);
	demo_keys_wait = (temp2[0] << 8) | temp2[1];

	printf("loaded demo '%s'\n", demo_filename);

	return true;
}

bool replay_demo_keys(void)
{
	while (demo_keys_wait == 0)
	{
		demo_keys = 0;
		fread_u8(&demo_keys, 1, demo_file);

		Uint8 temp2[2] = { 0, 0 };
		fread_u8(temp2, 2, demo_file);
		demo_keys_wait = (temp2[0] << 8) | temp2[1];

		if (feof(demo_file))
		{
			// no more keys
			return false;
		}
	}

	demo_keys_wait--;

	if (demo_keys & (1 << 0))
		player[0].y -= CURRENT_KEY_SPEED;
	if (demo_keys & (1 << 1))
		player[0].y += CURRENT_KEY_SPEED;

	if (demo_keys & (1 << 2))
		player[0].x -= CURRENT_KEY_SPEED;
	if (demo_keys & (1 << 3))
		player[0].x += CURRENT_KEY_SPEED;

	button[0] = (bool)(demo_keys & (1 << 4));
	button[3] = (bool)(demo_keys & (1 << 5));
	button[1] = (bool)(demo_keys & (1 << 6));
	button[2] = (bool)(demo_keys & (1 << 7));

	return true;
}

/*Street Fighter codes*/
void JE_SFCodes(JE_byte playerNum_, JE_integer PX_, JE_integer PY_, JE_integer mouseX_, JE_integer mouseY_)
{
	JE_byte temp, temp2, temp3, temp4, temp5;

	uint ship = player[playerNum_-1].items.ship;

	/*Get direction*/
	if (playerNum_ == 2 && ship < 15)
	{
		ship = 0;
	}

	if (ship < 15)
	{

		temp2 = (mouseY_ > PY_) +    /*UP*/
		        (mouseY_ < PY_) +    /*DOWN*/
		        (PX_ < mouseX_) +    /*LEFT*/
		        (PX_ > mouseX_);     /*RIGHT*/
		temp = (mouseY_ > PY_) * 1 + /*UP*/
		       (mouseY_ < PY_) * 2 + /*DOWN*/
		       (PX_ < mouseX_) * 3 + /*LEFT*/
		       (PX_ > mouseX_) * 4;  /*RIGHT*/

		if (temp == 0) // no direction being pressed
		{
			if (!button[0]) // if fire button is released
			{
				temp = 9;
				temp2 = 1;
			}
			else
			{
				temp2 = 0;
				temp = 99;
			}
		}

		if (temp2 == 1) // if exactly one direction pressed or fire button is released
		{
			temp += button[0] * 4;

			temp3 = superTyrian ? 21 : 3;
			for (temp2 = 0; temp2 < temp3; temp2++)
			{

				/*Use SuperTyrian ShipCombos or not?*/
				temp5 = superTyrian ? shipCombosB[temp2] : shipCombos[ship][temp2];

				// temp5 == selected combo in ship
				if (temp5 == 0) /* combo doesn't exists */
				{
					// mark twiddles as cancelled/finished
					SFCurrentCode[playerNum_-1][temp2] = 0;
				}
				else
				{
					// get next combo key
					temp4 = keyboardCombos[temp5-1][SFCurrentCode[playerNum_-1][temp2]];

					// correct key
					if (temp4 == temp)
					{
						SFCurrentCode[playerNum_-1][temp2]++;

						temp4 = keyboardCombos[temp5-1][SFCurrentCode[playerNum_-1][temp2]];
						if (temp4 > 100 && temp4 <= 100 + SPECIAL_NUM)
						{
							SFCurrentCode[playerNum_-1][temp2] = 0;
							SFExecuted[playerNum_-1] = temp4 - 100;
						}
					}
					else
					{
						if ((temp != 9) &&
						    (temp4 - 1) % 4 != (temp - 1) % 4 &&
						    (SFCurrentCode[playerNum_-1][temp2] == 0 ||
						     keyboardCombos[temp5-1][SFCurrentCode[playerNum_-1][temp2]-1] != temp))
						{
							SFCurrentCode[playerNum_-1][temp2] = 0;
						}
					}
				}
			}
		}

	}
}

void JE_playCredits(void)
{
	enum { lines_max = 126 };
	enum { line_max_length = 65 };

	char credstr[lines_max][line_max_length + 1];

	int lines = 0;

	JE_byte currentpic = 0, fade = 0;
	JE_shortint fadechg = 1;
	JE_byte currentship = 0;
	JE_integer shipx = 0, shipxwait = 0;
	JE_shortint shipxc = 0, shipxca = 0;

	load_sprites_file(EXTRA_SHAPES, "estsc.shp");

	setDelay2(1000);

	play_song(8);

	// load credits text
	FILE *f = dir_fopen_die(data_dir(), "tyrian.cdt", "rb");
	for (lines = 0; lines < lines_max; ++lines)
	{
		read_encrypted_pascal_string(credstr[lines], sizeof(credstr[lines]), f);
	}
	fclose(f);

	memcpy(colors, palettes[6-1], sizeof(colors));
	JE_clr256(VGAScreen);
	JE_showVGA();
	fade_palette(colors, 2, 0, 255);

	//tempScreenSeg = VGAScreenSeg;

	const int ticks_max = lines * 20 * 3;

	// Smooth-motion pacing for the credits sim: it advances one tick per real
	// tick-period, but we present every display frame (vsync-aligned) in between so
	// the flying ships and portrait fade glide rather than stepping at ~35fps. This
	// state persists across ticks; see the present block at the bottom of the loop.
	const float cred_period = get_delay_period();
	const float cred_counter_to_ms = 1000.0f / (float)SDL_GetPerformanceFrequency();
	Uint64 cred_last = SDL_GetPerformanceCounter();
	float cred_accum = 0.0f;

	for (int ticks = 0; ticks < ticks_max; ++ticks)
	{
		setDelay(1);
		JE_clr256(VGAScreen);

		blit_sprite_hv(VGAScreenSeg, 319 - sprite(EXTRA_SHAPES, currentpic)->width, 100 - (sprite(EXTRA_SHAPES, currentpic)->height / 2), EXTRA_SHAPES, currentpic, 0x0, fade - 15);

		fade += fadechg;
		if (fade == 0 && fadechg == -1)
		{
			fadechg = 1;
			++currentpic;
			if (currentpic >= sprite_table[EXTRA_SHAPES].count)
				currentpic = 0;
		}
		if (fade == 15)
			fadechg = 0;

		if (getDelayTicks2() == 0)
		{
			fadechg = -1;
			setDelay2(900);
		}

		if (ticks % 200 == 0)
		{
			currentship = (mt_rand() % 11) + 1;
			shipxwait = (mt_rand() % 80) + 10;
			if ((mt_rand() % 2) == 1)
			{
				shipx = 1;
				shipxc = 0;
				shipxca = 1;
			}
			else
			{
				shipx = 900;
				shipxc = 0;
				shipxca = -1;
			}
		}

		shipxwait--;
		if (shipxwait == 0)
		{
			if (shipx == 1 || shipx == 900)
				shipxc = 0;
			shipxca = -shipxca;
			shipxwait = (mt_rand() % 40) + 15;
		}
		shipxc += shipxca;
		shipx += shipxc;
		if (shipx < 1)
		{
			shipx = 1;
			shipxwait = 1;
		}
		if (shipx > 900)
		{
			shipx = 900;
			shipxwait = 1;
		}
		int tmp_unknown = shipxc * shipxc;
		if (450 + tmp_unknown < 0 || 450 + tmp_unknown > 900)
		{
			if (shipxca < 0 && shipxc < 0)
				shipxwait = 1;
			if (shipxca > 0 && shipxc > 0)
				shipxwait = 1;
		}

		uint ship_sprite = ships[currentship].shipgraphic;
		if (shipxc < -10)
			ship_sprite -= (shipxc < -20) ? 4 : 2;
		else if (shipxc > 10)
			ship_sprite += (shipxc > 20) ? 4 : 2;

		blit_sprite2x2(VGAScreen, shipx / 40, (vga_height - 16) - (ticks % vga_height), spriteSheet9, ship_sprite);

		const int bottom_line = (ticks / 3) / 20;
		int y = 20 - ((ticks / 3) % 20);

		for (int line = bottom_line - 10; line < bottom_line; ++line)
		{
			if (line >= 0 && line < lines_max)
			{
				if (strcmp(&credstr[line][0], ".") != 0 && strlen(credstr[line]))
				{
					const Uint8 color = credstr[line][0] - 65;
					const char *text = &credstr[line][1];

					const int x = 110 - JE_textWidth(text, SMALL_FONT_SHAPES) / 2;

					JE_outTextAdjust(VGAScreen, x + abs((y / 18) % 4 - 2) - 1, y - 1, text, color, -8, SMALL_FONT_SHAPES, false);
					JE_outTextAdjust(VGAScreen, x,                             y,     text, color, -2, SMALL_FONT_SHAPES, false);
				}
			}

			y += 20;
		}

		fill_rectangle_xy(VGAScreen, 0,  0, 319, 10, 0);
		fill_rectangle_xy(VGAScreen, 0, vga_height - 10, vga_width - 1, vga_height - 1, 0);

		if (currentpic == sprite_table[EXTRA_SHAPES].count - 1)
			JE_outTextAdjust(VGAScreen, 5, vga_height, miscText[54], 2, -2, SMALL_FONT_SHAPES, false);  // levels-in-episode

		if (bottom_line == lines_max - 8)
			fade_song();

		if (ticks == ticks_max - 1)
		{
			--ticks;
			play_song(9);
		}

		NETWORK_KEEP_ALIVE();

		if (smoothMotion)
		{
			// Present at the display refresh for the span of one sim tick. The ship
			// flight and portrait fade glide; the 1px-quantised text scroll still
			// steps (8-bit indexed, no sub-pixel) but at an even, vsync-aligned cadence.
			bool creditsDone = false;
			for (;;)
			{
				JE_showVGA();
				if (!output_vsync)
					limit_render_fps();
				if (JE_anyButton())
				{
					creditsDone = true;
					break;
				}

				const Uint64 now = SDL_GetPerformanceCounter();
				cred_accum += (float)(now - cred_last) * cred_counter_to_ms;
				cred_last = now;
				if (cred_accum >= cred_period)
				{
					cred_accum -= cred_period;
					if (cred_accum > cred_period)
						cred_accum = cred_period;  // never bank more than one tick of backlog
					break;
				}
			}
			if (creditsDone)
				break;
		}
		else
		{
			JE_showVGA();

			wait_delay();

			if (JE_anyButton())
				break;
		}
	}

	fade_black(10);

	free_sprites(EXTRA_SHAPES);
}

void JE_endLevelAni(void)
{
	JE_word x, y;
	JE_byte temp;
	char tempStr[256];

	Sint8 i;

	long endlessInterest = 0, endlessBonus = 0;  // endless: the level-clear payout, shown below

	if (!constantPlay)
	{
		// grant shipedit privileges

		// special
		if (player[0].items.special < 21)
			saveTemp[SAVE_FILES_SIZE + 81 + player[0].items.special] = 1;

		for (uint p = 0; p < COUNTOF(player); ++p)
		{
			// front, rear
			for (uint i = 0; i < COUNTOF(player[p].items.weapon); ++i)
				saveTemp[SAVE_FILES_SIZE + player[p].items.weapon[i].id] = 1;

			// options
			for (uint i = 0; i < COUNTOF(player[p].items.sidekick); ++i)
				saveTemp[SAVE_FILES_SIZE + 51 + player[p].items.sidekick[i]] = 1;
		}
	}

	// Endless mode drives its own ramp through depth- and mutator-scaled enemy stats,
	// keeping the player's chosen base difficulty fixed (endless's own levers key off
	// difficultyLevel too -- see endless.h). The vanilla score-based bump must not fire
	// here, or e.g. Normal would silently climb to Hard/Impossible mid-run.
	if (difficultyAdjust && !endlessMode)
		adjust_difficulty();

	player[0].last_items = player[0].items;
	strcpy(lastLevelName, levelName);

	JE_wipeKey();
	frameCountMax = 4;
	textGlowFont = SMALL_FONT_SHAPES;

	SDL_Color white = { 255, 255, 255 };
	set_colors(white, 254, 254);

	if (!levelTimer || levelTimerCountdown > 0 || !(episodeNum == 4))
		JE_playSampleNum(V_LEVEL_END);
	else
		play_song(21);

	if (bonusLevel)
	{
		JE_outTextGlow(VGAScreenSeg, 20, 20, miscText[17-1]);
	}
	else if (all_players_alive())
	{
		sprintf(tempStr, "%s %s", miscText[27-1], levelName); // "Completed"
		JE_outTextGlow(VGAScreenSeg, 20, 20, tempStr);
	}
	else
	{
		sprintf(tempStr, "%s %s", miscText[62-1], levelName); // "Exiting"
		JE_outTextGlow(VGAScreenSeg, 20, 20, tempStr);
	}

	// Endless banks the level-clear payout now, so the cash total printed just below already
	// includes it (the breakdown is shown further down, where data cubes would normally be).
	if (endlessMode)
		endlessApplyLevelPayout(&endlessInterest, &endlessBonus);

	if (twoPlayerMode)
	{
		for (uint i = 0; i < 2; ++i)
		{
			snprintf(tempStr, sizeof(tempStr), "%s %lu", miscText[40 + i], player[i].cash);
			JE_outTextGlow(VGAScreenSeg, 30, 50 + 20 * i, tempStr);
		}
	}
	else
	{
		sprintf(tempStr, "%s %lu", miscText[28-1], player[0].cash);
		JE_outTextGlow(VGAScreenSeg, 30, 50, tempStr);
	}

	if (timedBattleMode)
	{
		x = (levelTimerCountdown / 10) * 100;
		sprintf(tempStr, "%s %d", miscTextB[6], x);
		JE_outTextGlow(VGAScreenSeg, 40, 75, tempStr);
		player[0].cash += x;
	}

	temp = (totalEnemy == 0) ? 0 : roundf(enemyKilled * 100 / totalEnemy);
	sprintf(tempStr, "%s %d%%", miscText[63-1], temp);
	JE_outTextGlow(VGAScreenSeg, 40, 90, tempStr);

	if (!constantPlay)
		editorLevel += temp / 5;

	if (timedBattleMode)
	{
		for (temp = 1; temp <= *player[0].lives; temp++)
		{
			JE_playSampleNum(S_ITEM);
			x = 20 + 15 * temp;
			y = 115;

			for (i = -15; i <= 10; i++)
			{
				setDelay(frameCountMax);

				blit_sprite_hv(VGAScreenSeg, x, y, OPTION_SHAPES, 46, 0x9, i);

				if (JE_anyButton())
					frameCountMax = 0;

				JE_showVGA();

				wait_delay();
			}
			for (i = 10; i >= 0; i--)
			{
				setDelay(frameCountMax);

				blit_sprite_hv(VGAScreenSeg, x, y, OPTION_SHAPES, 46, 0x9, i);

				if (JE_anyButton())
					frameCountMax = 0;

				JE_showVGA();

				wait_delay();
			}
		}
		x = *player[0].lives * 1000;
		sprintf(tempStr, "%s %d", miscTextB[7], x);
		JE_outTextGlow(VGAScreenSeg, 120, 120, tempStr);
		player[0].cash += x;
	}
	else if (endlessMode)
	{
		// Endless earns cash, not data cubes -- show the clear payout just banked above.
		char payStr[64];
		snprintf(payStr, sizeof(payStr), "Zone Bonus:  +%ld", endlessBonus);
		JE_outTextGlow(VGAScreenSeg, 30, 120, payStr);
		if (endlessInterest > 0)
		{
			snprintf(payStr, sizeof(payStr), "Bank Interest:  +%ld", endlessInterest);
			JE_outTextGlow(VGAScreenSeg, 30, 138, payStr);
		}
	}
	else if (!onePlayerAction && !twoPlayerMode)
	{
		JE_outTextGlow(VGAScreenSeg, 30, 120, miscText[4-1]);   /*Cubes*/

		if (cubeMax > 0)
		{
			if (cubeMax > 4)
				cubeMax = 4;

			if (frameCountMax != 0)
				frameCountMax = 1;

			for (temp = 1; temp <= cubeMax; temp++)
			{
				NETWORK_KEEP_ALIVE();

				JE_playSampleNum(S_ITEM);
				x = 20 + 30 * temp;
				y = 135;
				JE_drawCube(VGAScreenSeg, x, y, 9, 0);
				JE_showVGA();

				for (i = -15; i <= 10; i++)
				{
					setDelay(frameCountMax);

					blit_sprite_hv(VGAScreenSeg, x, y, OPTION_SHAPES, 25, 0x9, i);

					if (JE_anyButton())
						frameCountMax = 0;

					JE_showVGA();

					wait_delay();
				}
				for (i = 10; i >= 0; i--)
				{
					setDelay(frameCountMax);

					blit_sprite_hv(VGAScreenSeg, x, y, OPTION_SHAPES, 25, 0x9, i);

					if (JE_anyButton())
						frameCountMax = 0;

					JE_showVGA();

					wait_delay();
				}
			}
		}
		else
		{
			JE_outTextGlow(VGAScreenSeg, 50, 135, miscText[15-1]);
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
	temp2 = twoPlayerMode ? 150 : 160;
	JE_outTextGlow(VGAScreenSeg, 90, temp2, miscText[5-1]);

	if (!constantPlay)
	{
		do
		{
			setDelay(1);

			NETWORK_KEEP_ALIVE();

			wait_delay();
		} while (!(JE_anyButton() || (frameCountMax == 0 && temp == 1)));
	}

	wait_noinput(false, false, true); // TODO: should up the joystick repeat temporarily instead

	fade_black(15);
	JE_clr256(VGAScreen);
}

void JE_drawCube(SDL_Surface * screen, JE_word x, JE_word y, JE_byte filter, JE_byte brightness)
{
	blit_sprite_dark(screen, x + 4, y + 4, OPTION_SHAPES, 25, false);
	blit_sprite_dark(screen, x + 3, y + 3, OPTION_SHAPES, 25, false);
	blit_sprite_hv(screen, x, y, OPTION_SHAPES, 25, filter, brightness);
}

void JE_handleChat(void)
{
	// STUB(); Annoying piece of crap =P
}

bool str_pop_int(char *str, int *val)
{
	bool success = false;

	char buf[256];
	assert(strlen(str) < sizeof(buf));

	// grab the value from str
	char *end;
	*val = strtol(str, &end, 10);

	if (end != str)
	{
		success = true;

		// shift the rest to the beginning
		strcpy(buf, end);
		strcpy(str, buf);
	}

	return success;
}

void JE_operation(JE_byte slot)
{
	JE_byte flash;
	char stemp[21];
	char tempStr[51];

	// This screen (name entry + on-screen SAVE/CANCEL buttons) needs the absolute pointer.
	// If it was opened during gameplay the mouse is in relative mode; force absolute so a
	// Switch touch is a tap-to-click on the buttons -- in relative mode a touch steers the
	// ship, so the buttons never register and the dialog appears frozen. Restored on exit.
	const bool op_was_relative = mouseGetRelative();
	mouseSetRelative(false);

	if (!performSave)
	{
		if (saveFiles[slot-1].level > 0)
		{
			gameJustLoaded = true;
			JE_loadGame(slot);
			endlessLoadSlot(slot);  // if this slot holds an endless run, re-enter endless mode + restore it
			gameLoaded = true;
		}
	}
	else if (slot % 11 != 0)
	{
		strcpy(stemp, "              ");
		memcpy(stemp, saveFiles[slot-1].name, strlen(saveFiles[slot-1].name));
		temp = strlen(stemp);
		while (stemp[temp-1] == ' ' && --temp);

#if defined(__SWITCH__) || defined(__vita__)
		// No physical keyboard on the consoles: get the name from the software keyboard and
		// fill the field DIRECTLY here (deterministic). Delivering it as an injected SDL
		// TEXTINPUT event raced with service_SDL_events clearing new_text and sometimes lost
		// the whole name. The dialog then shows it and the user taps SAVE to confirm.
		{
			char kb[15];
			int n = (temp < 14) ? temp : 14;
			memcpy(kb, stemp, (size_t)n);
			kb[n] = '\0';   // pre-fill the keyboard with the current (trimmed) name
			if (console_swkbd(kb, sizeof(kb), 14, kb, "Save name", false))
			{
				memset(stemp, ' ', 14);
				stemp[14] = '\0';
				temp = 0;
				for (const char *c = kb; *c != '\0' && temp < 14; ++c)
				{
					const char u = (unsigned char)*c <= 127U ? (char)toupper((unsigned char)*c) : 0;
					if (u == ' ' || font_ascii[(unsigned char)u] != -1)
						stemp[temp++] = u;
				}
			}
		}
#endif

		flash = 8 * 16 + 10;

		wait_noinput(false, true, false);

		JE_barShade(VGAScreen, 65, 55, 255, 155);

		bool quit = false;
		while (!quit)
		{
			service_SDL_events(true);

			blit_sprite(VGAScreen, 50, 50, OPTION_SHAPES, 35);  // message box

			JE_textShade(VGAScreen, 60, 55, miscText[1-1], 11, 4, DARKEN);
			JE_textShade(VGAScreen, 70, 70, levelName, 11, 4, DARKEN);

			do
			{
				flash = (flash == 8 * 16 + 10) ? 8 * 16 + 2 : 8 * 16 + 10;
				temp3 = (temp3 == 6) ? 2 : 6;

				strcpy(tempStr, miscText[2-1]);
				strncat(tempStr, stemp, temp);
				JE_outText(VGAScreen, 65, 89, tempStr, 8, 3);
				tempW = 65 + JE_textWidth(tempStr, TINY_FONT);
				JE_barShade(VGAScreen, tempW + 2, 90, tempW + 6, 95);
				fill_rectangle_xy(VGAScreen, tempW + 1, 89, tempW + 5, 94, flash);

				int text_x = 54 + 45 - (JE_textWidth(miscText[9], FONT_SHAPES) / 2);
				JE_outTextAdjust(VGAScreen, text_x, 128, miscText[9], 15, -5, FONT_SHAPES, true);

				text_x = 149 + 45 - (JE_textWidth(miscText[10], FONT_SHAPES) / 2);
				JE_outTextAdjust(VGAScreen, text_x, 128, miscText[10], 15, -5, FONT_SHAPES, true);

				for (int i = 0; i < 14; i++)
				{
					setDelay(1);

					push_joysticks_as_keyboard();
					service_wait_delay();

					JE_mouseStart();
					JE_showVGA();
					JE_mouseReplace();

					if (newkey || newmouse || new_text)
						break;
				}
			} while (!newkey && !newmouse && !new_text);

			if (mouseButton > 0)
			{
				if (lastmouse_x > 56 && lastmouse_x < 142 && lastmouse_y > 123 && lastmouse_y < 149)
				{
					quit = true;
					if (JE_saveRequest(slot, stemp))
					{
						JE_saveGame(slot, stemp);
						endlessSaveSlot(slot);  // persist/clear the endless run for this slot
					}
				}
				else if (lastmouse_x > 151 && lastmouse_x < 237 && lastmouse_y > 123 && lastmouse_y < 149)
				{
					quit = true;
					JE_playSampleNum(S_SPRING);
				}
			}
			else if (new_text)
			{
				for (size_t ti = 0U; last_text[ti] != '\0'; ++ti)
				{
					const char c = (unsigned char)last_text[ti] <= 127U ? toupper(last_text[ti]) : 0;
					if ((c == ' ' || font_ascii[(unsigned char)c] != -1) &&
					    temp < 14)
					{
						JE_playSampleNum(S_CURSOR);
						stemp[temp] = c;
						temp += 1;
					}
				}
			}
			else if (newkey)
			{
				switch (lastkey_scan)
				{
					case SDL_SCANCODE_BACKSPACE:
					case SDL_SCANCODE_DELETE:
						if (temp)
						{
							temp--;
							stemp[temp] = ' ';
							JE_playSampleNum(S_CLICK);
						}
						break;
					case SDL_SCANCODE_ESCAPE:
						quit = true;
						JE_playSampleNum(S_SPRING);
						break;
					case SDL_SCANCODE_RETURN:
						quit = true;
						if (JE_saveRequest(slot, stemp))
						{
							JE_saveGame(slot, stemp);
							endlessSaveSlot(slot);  // persist/clear the endless run for this slot
						}
						break;
					default:
						break;
				}
			}
		}
	}

	wait_noinput(false, true, false);

	mouseSetRelative(op_was_relative);
}

/* Draw a 1px rectangle outline, clamped to the surface. */
static void debug_box(SDL_Surface *s, int x0, int y0, int x1, int y1, Uint8 col)
{
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > s->w - 1) x1 = s->w - 1;
	if (y1 > s->h - 1) y1 = s->h - 1;
	if (x1 < x0 || y1 < y0)
		return;
	fill_rectangle_xy(s, x0, y0, x1, y0, col);  // top
	fill_rectangle_xy(s, x0, y1, x1, y1, col);  // bottom
	fill_rectangle_xy(s, x0, y0, x0, y1, col);  // left
	fill_rectangle_xy(s, x1, y0, x1, y1, col);  // right
}

/* Debug overlays drawn into game_screen at the end of JE_inGameDisplays, so the
 * render-list residual shows them every presented frame. Positions are sim
 * (collision) coordinates, so the boxes deliberately snap, not interpolate. */
static void JE_drawDebugOverlays(void)
{
	enum { COL_ENEMY = 124, COL_PLAYER = 0xFB, COL_PSHOT = 0xFB, COL_ESHOT = 124 };

	if (debugHitboxOverlay)
	{
		// shootable enemies: hit-area box centred on the enemy sprite (its quadrants
		// are drawn at +/-6 x, +/-7 y of (ex,ey), so the sprite centre is +6/+7)
		for (int i = 0; i < 100; ++i)
		{
			if (enemyAvail[i] != 0)
				continue;
			const bool small = (enemy[i].enemycycle > 0);
			const int cx = enemy[i].ex + enemy[i].mapoffset + 6;
			const int cy = enemy[i].ey + 7;
			const int hw = small ? 13 : 25;
			const int hh = small ? 15 : 29;
			debug_box(VGAScreen, cx - hw, cy - hh, cx + hw, cy + hh, COL_ENEMY);
		}

		// player ship body box, centred on the hull sprite (drawn ~7px right/down
		// of x,y) so it matches the centred collision in JE_playerCollide
		for (uint p = 0; p < (twoPlayerMode ? 2u : 1u); ++p)
		{
			if (!player[p].is_alive)
				continue;
			const int cx = player[p].x + 7, cy = player[p].y + 7;
			debug_box(VGAScreen, cx - 12, cy - 14, cx + 12, cy + 14, COL_PLAYER);
		}

		// player shots, markers centred on the shot sprite (drawn at shotX+1)
		for (int i = 0; i < MAX_PWEAPON; ++i)
		{
			if (shotAvail[i] == 0)
				continue;
			const int sx = playerShotData[i].shotX + 6, sy = playerShotData[i].shotY + 6;
			debug_box(VGAScreen, sx - 1, sy - 1, sx + 1, sy + 1, COL_PSHOT);
		}
		for (int i = 0; i < ENEMY_SHOT_MAX; ++i)
		{
			if (enemyShotAvail[i] != 0)
				continue;
			const int sx = enemyShot[i].sx, sy = enemyShot[i].sy;
			debug_box(VGAScreen, sx - 1, sy - 1, sx + 1, sy + 1, COL_ESHOT);
		}
	}

	if (debugPerfOverlay)
	{
		int activeEnemies = 0, pShots = 0, eShots = 0;
		for (int i = 0; i < 100; ++i)
			if (enemyAvail[i] != 1)
				++activeEnemies;
		for (int i = 0; i < MAX_PWEAPON; ++i)
			if (shotAvail[i] != 0)
				++pShots;
		for (int i = 0; i < ENEMY_SHOT_MAX; ++i)
			if (enemyShotAvail[i] == 0)
				++eShots;

		const int ms = (current_fps > 0) ? (1000 / current_fps) : 0;
		const int px = PLAYFIELD_LEFT + 4;  // just inside the playfield's left edge
		int py = 4;
		char buf[40];

		snprintf(buf, sizeof(buf), "FPS %d (%dms)", current_fps, ms);
		JE_textShade(VGAScreen, px, py, buf, 15, 2, FULL_SHADE); py += 9;
		snprintf(buf, sizeof(buf), "ENEMIES %d", activeEnemies);
		JE_textShade(VGAScreen, px, py, buf, 15, 2, FULL_SHADE); py += 9;
		snprintf(buf, sizeof(buf), "SHOTS P%d E%d", pShots, eShots);
		JE_textShade(VGAScreen, px, py, buf, 15, 2, FULL_SHADE); py += 9;
		snprintf(buf, sizeof(buf), "ALPHA %d%%", (int)(debug_interp_alpha * 100.0f + 0.5f));
		JE_textShade(VGAScreen, px, py, buf, 15, 2, FULL_SHADE);
	}
}

void JE_inGameDisplays(void)
{
	char stemp[21];
	char tempstr[256];

	for (uint i = 0; i < ((twoPlayerMode && !galagaMode) ? 2 : 1); ++i)
	{
		snprintf(tempstr, sizeof(tempstr), "%lu", player[i].cash);

		// x base 27 (nudged 3px left from the original 30); player 1 mirrors at +200.
		if (smoothies[6 - 1])
			JE_textShade(VGAScreen, 27 + 200 * i, vga_height - 25, tempstr, 8, 8, FULL_SHADE);
		else
			JE_textShade(VGAScreen, 27 + 200 * i, vga_height - 25, tempstr, 2, 4, FULL_SHADE);
	}

	// Endless: compact live kill-fire buff readout (Turbodrive / Overdrive), bottom-right of the
	// playfield -- a combo kill counter ("xN"), the buff's fire/damage bonuses, and a draining
	// timer bar. Shifts to clear whichever boss bar is shown: UP for a BOTTOM horizontal bar, LEFT
	// for a RIGHT vertical bar; other layouts never reach the bottom-right corner.
	if (endlessMode && endlessTurbodriveActive())
	{
		const int bank = endlessKillBuffColorBank();
		const int baseRightX = PLAYFIELD_LEFT + PLAYFIELD_WIDTH - 5 + 2;  // +2px right of the FPS counter's edge
		const int rightX = baseRightX - boss_bar_hud_left_shift(baseRightX);
		const int yShift = boss_bar_hud_needs_up_shift() ? 14 : 0;
		const int yBase = 7;  // +7px down from the original placement
		char buf[48];

		snprintf(buf, sizeof(buf), "x%d", endlessKillBuffComboCount());
		JE_textShade(VGAScreen, rightX - JE_textWidth(buf, TINY_FONT), vga_height - 45 + yBase - yShift, buf, bank, 5, FULL_SHADE);

		// Middle line reports the active buff/curse cleanly: a BOON shows only the effects it grants
		//   -- fire boost (Turbodrive/Overdrive) -> "FIRE xN", damage stacks (Overdrive/Overblast) -> "DMG+N%";
		//   an evil curse shows its one-word name -- JAMMED (Backfire) / BURNOUT / MISFIRE.
		if (endlessKillFireIsEvil())
		{
			snprintf(buf, sizeof(buf), "%s", endlessKillFireEvilName());
		}
		else
		{
			const int fireMult = endlessKillBuffFireMultiplier();
			const int dmgPct = endlessKillBuffDamagePercent();
			if (fireMult > 1 && dmgPct > 0)
				snprintf(buf, sizeof(buf), "FIREx%d DMG+%d%%", fireMult, dmgPct);
			else if (fireMult > 1)
				snprintf(buf, sizeof(buf), "FIREx%d", fireMult);
			else
				snprintf(buf, sizeof(buf), "DMG+%d%%", dmgPct);   // Overblast: damage only, no fire boost
		}
		JE_textShade(VGAScreen, rightX - JE_textWidth(buf, TINY_FONT), vga_height - 37 + yBase - yShift, buf, bank, 3, FULL_SHADE);

		const int bw = 60;
		const int mt = endlessKillBuffTicksMax();
		const int fillw = (mt > 0) ? bw * endlessKillBuffTicksLeft() / mt : 0;
		const int barX = rightX - bw;
		const int barY0 = vga_height - 28 + yBase - yShift;
		const int barY1 = vga_height - 26 + yBase - yShift;
		fill_rectangle_xy(VGAScreen, barX, barY0, rightX, barY1, bank * 16 + 2);  // dark track
		// Fill with a very weak vertical gradient within the buff's palette bank: brightest on the
		// top row, one shade darker per row down the bar's 3px height -- a subtle top-to-bottom shade.
		if (fillw > 0)
			for (int y = barY0; y <= barY1; ++y)
			{
				int shade = 11 - (y - barY0);  // +11 top -> +9 bottom (one step per row; the bar is 3px tall)
				fill_rectangle_xy(VGAScreen, barX, y, barX + fillw, y, bank * 16 + shade);
			}
	}

	/*Special Weapon?*/
	if (player[0].items.special > 0)
		blit_sprite2x2(VGAScreen, 25, 1, spriteSheet10, special[player[0].items.special].itemgraphic);

	/*Lives Left*/
	if (onePlayerAction || twoPlayerMode)
	{
		for (int temp = 0; temp < (onePlayerAction ? 1 : 2); temp++)
		{
			const uint extra_lives = *player[temp].lives - 1;

			int y = (temp == 0 && player[0].items.special > 0) ? 35 : 15;
			tempW = (temp == 0) ? 30: 270;

			if (extra_lives >= 5)
			{
				blit_sprite2(VGAScreen, tempW, y, spriteSheet9, 285);
				tempW = (temp == 0) ? 45 : 250;
				sprintf(tempstr, "%d", extra_lives);
				JE_textShade(VGAScreen, tempW, y + 3, tempstr, 15, 1, FULL_SHADE);
			}
			else if (extra_lives >= 1)
			{
				for (uint i = 0; i < extra_lives; ++i)
				{
					blit_sprite2(VGAScreen, tempW, y, spriteSheet9, 285);

					tempW += (temp == 0) ? 12 : -12;
				}
			}

			strcpy(stemp, (temp == 0) ? miscText[49-1] : miscText[50-1]);
			if (isNetworkGame)
			{
				strcpy(stemp, JE_getName(temp+1));
			}

			tempW = (temp == 0) ? 28 : (PLAYFIELD_WIDTH + 22 - JE_textWidth(stemp, TINY_FONT));
			JE_textShade(VGAScreen, tempW, y - 7, stemp, 2, 6, FULL_SHADE);
		}
	}

	/*Super Bombs!!*/
	for (uint i = 0; i < COUNTOF(player); ++i)
	{
		int x = (i == 0) ? 30 : 270;

		for (uint j = player[i].superbombs; j > 0; --j)
		{
			blit_sprite2(VGAScreen, x, 160, spriteSheet9, 304);
			x += (i == 0) ? 12 : -12;
		}
	}

	if (youAreCheating)
	{
		JE_outText(VGAScreen, 90, 170, "Cheaters always prosper.", 3, 4);
	}

	/* Optional FPS counter, bottom-right of the playfield (game_screen space, so
	 * the +24 composite offset keeps it just inside the right edge before the HUD). */
	if (show_fps)
	{
		char fps_str[16];
		snprintf(fps_str, sizeof(fps_str), "%d FPS", current_fps);

		const int fps_right = PLAYFIELD_LEFT + PLAYFIELD_WIDTH - 5;  // ~x318
		const int fps_x = fps_right - JE_textWidth(fps_str, TINY_FONT);
		JE_textShade(VGAScreen, fps_x, 176, fps_str, 15, 2, FULL_SHADE);
	}

	JE_drawDebugOverlays();
}

void JE_mainKeyboardInput(void)
{
	JE_gammaCheck();

	/* { Network Request Commands } */

	if (!isNetworkGame)
	{
		/* { Edited Ships } for Player 1 */
		if (extraAvail && keysactive[SDL_SCANCODE_TAB] && !isNetworkGame && !superTyrian)
		{
			for (x = SDL_SCANCODE_1; x <= SDL_SCANCODE_0; x++)
			{
				if (keysactive[x])
				{
					int z = x - SDL_SCANCODE_1 + 1;
					player[0].items.ship = 90 + z;                     /*Ships*/
					z = (z - 1) * 15;
					player[0].items.weapon[FRONT_WEAPON].id = extraShips[z + 1];
					player[0].items.weapon[REAR_WEAPON].id = extraShips[z + 2];
					player[0].items.special = extraShips[z + 3];
					player[0].items.sidekick[LEFT_SIDEKICK] = extraShips[z + 4];
					player[0].items.sidekick[RIGHT_SIDEKICK] = extraShips[z + 5];
					player[0].items.generator = extraShips[z + 6];
					/*Armor*/
					player[0].items.shield = extraShips[z + 8];
					memset(shotMultiPos, 0, sizeof(shotMultiPos));

					if (player[0].weapon_mode > JE_portConfigs())
						player[0].weapon_mode = 1;

					tempW = player[0].armor;
					JE_getShipInfo();
					if (player[0].armor > tempW && editShip1)
						player[0].armor = tempW;
					else
						editShip1 = true;

					SDL_Surface *temp_surface = VGAScreen;
					VGAScreen = VGAScreenSeg;
					JE_wipeShieldArmorBars();
					JE_drawArmor();
					JE_drawShield();
					VGAScreen = temp_surface;
					JE_drawOptions();

					keysactive[x] = false;
				}
			}
		}

		/* for Player 2 */
		if (extraAvail && keysactive[SDL_SCANCODE_CAPSLOCK] && !isNetworkGame && !superTyrian)
		{
			for (x = SDL_SCANCODE_1; x <= SDL_SCANCODE_0; x++)
			{
				if (keysactive[x])
				{
					int z = x - SDL_SCANCODE_1 + 1;
					player[1].items.ship = 90 + z;
					z = (z - 1) * 15;
					player[1].items.weapon[FRONT_WEAPON].id = extraShips[z + 1];
					player[1].items.weapon[REAR_WEAPON].id = extraShips[z + 2];
					player[1].items.special = extraShips[z + 3];
					player[1].items.sidekick[LEFT_SIDEKICK] = extraShips[z + 4];
					player[1].items.sidekick[RIGHT_SIDEKICK] = extraShips[z + 5];
					player[1].items.generator = extraShips[z + 6];
					/*Armor*/
					player[1].items.shield = extraShips[z + 8];
					memset(shotMultiPos, 0, sizeof(shotMultiPos));

					if (player[1].weapon_mode > JE_portConfigs())
						player[1].weapon_mode = 1;

					tempW = player[1].armor;
					JE_getShipInfo();
					if (player[1].armor > tempW && editShip2)
						player[1].armor = tempW;
					else
						editShip2 = true;

					SDL_Surface *temp_surface = VGAScreen;
					VGAScreen = VGAScreenSeg;
					JE_wipeShieldArmorBars();
					JE_drawArmor();
					JE_drawShield();
					VGAScreen = temp_surface;
					JE_drawOptions();

					keysactive[x] = false;
				}
			}
		}
	}

	/* { In-Game Help } */
	if (keysactive[SDL_SCANCODE_F1])
	{
		if (isNetworkGame)
		{
			helpRequest = true;
		}
		else
		{
			JE_inGameHelp();
			skipStarShowVGA = true;
		}
	}

	/* {!Activate Nort Ship!} */
	if (keysactive[SDL_SCANCODE_F2] && keysactive[SDL_SCANCODE_F4] && keysactive[SDL_SCANCODE_F6] && keysactive[SDL_SCANCODE_F7] &&
	    keysactive[SDL_SCANCODE_F9] && keysactive[SDL_SCANCODE_BACKSLASH] && keysactive[SDL_SCANCODE_SLASH])
	{
		if (isNetworkGame)
		{
			nortShipRequest = true;
		}
		else
		{
			player[0].items.ship = 12;                     // Nort Ship
			player[0].items.special = 13;                  // Astral Zone
			player[0].items.weapon[FRONT_WEAPON].id = 36;  // NortShip Super Pulse
			player[0].items.weapon[REAR_WEAPON].id = 37;   // NortShip Spreader
			shipGr = 1;
		}
	}

	/* {Cheating} */
	if (!isNetworkGame && !twoPlayerMode && !superTyrian && superArcadeMode == SA_NONE)
	{
		if (keysactive[SDL_SCANCODE_F2] && keysactive[SDL_SCANCODE_F3] && keysactive[SDL_SCANCODE_F6])
		{
			youAreCheating = !youAreCheating;
			keysactive[SDL_SCANCODE_F2] = false;
		}

		if (keysactive[SDL_SCANCODE_F2] && keysactive[SDL_SCANCODE_F3] && (keysactive[SDL_SCANCODE_F4] || keysactive[SDL_SCANCODE_F5]))
		{
			for (uint i = 0; i < COUNTOF(player); ++i)
				player[i].armor = 0;

			youAreCheating = !youAreCheating;
			JE_drawTextWindow(miscText[63-1]);
		}

		if (constantPlay && keysactive[SDL_SCANCODE_C])
		{
			youAreCheating = !youAreCheating;
			keysactive[SDL_SCANCODE_C] = false;
		}
	}

	if (superTyrian)
	{
		youAreCheating = false;
	}

	/* {Personal Commands} */

	/* {DEBUG} */
	if (keysactive[SDL_SCANCODE_F10] && keysactive[SDL_SCANCODE_BACKSPACE])
	{
		keysactive[SDL_SCANCODE_F10] = false;
		debug = !debug;

		debugHist = 1;
		debugHistCount = 1;

		/* YKS: clock ticks since midnight replaced by SDL_GetTicks */
		lastDebugTime = SDL_GetTicks();
	}

	/* {CHEAT-SKIP LEVEL} */
	if (keysactive[SDL_SCANCODE_F2] && keysactive[SDL_SCANCODE_F6] && (keysactive[SDL_SCANCODE_F7] || keysactive[SDL_SCANCODE_F8]) && !keysactive[SDL_SCANCODE_F9] &&
	    !superTyrian && superArcadeMode == SA_NONE)
	{
		if (isNetworkGame)
		{
			skipLevelRequest = true;
		}
		else
		{
			levelTimer = true;
			levelTimerCountdown = 0;
			endLevel = true;
			levelEnd = 40;
		}
	}

	/* pause game */
	pause_pressed = pause_pressed || keysactive[SDL_SCANCODE_P];

	/* in-game setup */
	ingamemenu_pressed = ingamemenu_pressed || keysactive[SDL_SCANCODE_ESCAPE];

	if (keysactive[SDL_SCANCODE_BACKSPACE])
	{
		/* toggle screenshot pause */
		if (keysactive[SDL_SCANCODE_NUMLOCKCLEAR])
			superPause = !superPause;

		/* {SMOOTHIES} */
		if (keysactive[SDL_SCANCODE_F12] && keysactive[SDL_SCANCODE_SCROLLLOCK])
		{
			for (temp = SDL_SCANCODE_2; temp <= SDL_SCANCODE_9; temp++)
				if (keysactive[temp])
					smoothies[temp-SDL_SCANCODE_2] = !smoothies[temp-SDL_SCANCODE_2];
			if (keysactive[SDL_SCANCODE_0])
				smoothies[8] = !smoothies[8];
		}
		else

		/* {CYCLE THROUGH FILTER COLORS} */
		if (keysactive[SDL_SCANCODE_MINUS])
		{
			if (levelFilter == -99)
			{
				levelFilter = 0;
			}
			else
			{
				levelFilter++;
				if (levelFilter == 16)
					levelFilter = -99;
			}
		}
		else

		/* {HYPER-SPEED} */
		if (keysactive[SDL_SCANCODE_1])
		{
			fastPlay++;
			if (fastPlay > 2)
				fastPlay = 0;
			keysactive[SDL_SCANCODE_1] = false;
			JE_setNewGameSpeed();
		}

		/* {IN-GAME RANDOM MUSIC SELECTION} */
		if (keysactive[SDL_SCANCODE_SCROLLLOCK])
			play_song(mt_rand() % MUSIC_NUM);
	}
}

void JE_pauseGame(void)
{
	// The pause overlay presents frames while the tick's render-list recording is
	// active; suspend it (see JE_doInGameSetup).
	const bool rl_was_recording = render_list_recording;
	render_list_recording = false;

	mouseSetRelative(false);

	JE_boolean done = false;
	JE_word mouseX, mouseY;

	SDL_Surface *temp_surface = VGAScreen;
	VGAScreen = VGAScreenSeg; /* side-effect of game_screen */

	//tempScreenSeg = VGAScreenSeg; // sega000
	if (!superPause)
	{
		JE_dString(VGAScreenSeg, 120, 90, miscText[22], FONT_SHAPES);

		VGAScreen = VGAScreenSeg;
		JE_showVGA();
	}

	set_volume(tyrMusicVolume / 2, fxVolume);

#ifdef WITH_NETWORK
	if (isNetworkGame)
	{
		network_prepare(PACKET_GAME_PAUSE);
		network_send(4);  // PACKET_GAME_PAUSE

		while (true)
		{
			service_SDL_events(false);

			if (packet_in[0] && SDLNet_Read16(&packet_in[0]->data[0]) == PACKET_GAME_PAUSE)
			{
				network_update();
				break;
			}

			network_update();
			network_check();

			SDL_Delay(16);
		}
	}
#endif

	wait_noinput(false, false, true); // TODO: should up the joystick repeat temporarily instead

	do
	{
		setDelay(2);

		push_joysticks_as_keyboard();
		service_SDL_events(true);

		if ((newkey && lastkey_scan != SDL_SCANCODE_LCTRL && lastkey_scan != SDL_SCANCODE_RCTRL && lastkey_scan != SDL_SCANCODE_LALT && lastkey_scan != SDL_SCANCODE_RALT) ||
			JE_mousePosition(&mouseX, &mouseY) > 0)
		{
#ifdef WITH_NETWORK
			if (isNetworkGame)
			{
				network_prepare(PACKET_WAITING);
				network_send(4);  // PACKET_WAITING
			}
#endif
			done = true;
		}

#ifdef WITH_NETWORK
		if (isNetworkGame)
		{
			network_check();

			if (packet_in[0] && SDLNet_Read16(&packet_in[0]->data[0]) == PACKET_WAITING)
			{
				network_check();

				done = true;
			}
		}
#endif

		wait_delay();
	} while (!done);

#ifdef WITH_NETWORK
	if (isNetworkGame)
	{
		while (!network_is_sync())
		{
			service_SDL_events(false);

			network_check();
			SDL_Delay(16);
		}
	}
#endif

	set_volume(tyrMusicVolume, fxVolume);

	//skipStarShowVGA = true;

	VGAScreen = temp_surface; /* side-effect of game_screen */

	mouseSetRelative(true);

	render_list_recording = rl_was_recording;
}

// Player-ship sprite blits that tint the hull a "running hot" colour while the current
// endless zone is OVERCLOCK; otherwise a plain blit. Keep the render-list ship tag (set by
// the caller) so the tinted hull still interpolates smoothly between ticks.
static void blit_ship2x2(SDL_Surface *surface, int x, int y, Sprite2_array sheet, unsigned int index)
{
	const int tint = endlessShipTintFilter();
	if (tint)
		blit_sprite2x2_filter(surface, x, y, sheet, index, (Uint8)tint);
	else
		blit_sprite2x2(surface, x, y, sheet, index);
}

static void blit_ship2(SDL_Surface *surface, int x, int y, Sprite2_array sheet, unsigned int index)
{
	const int tint = endlessShipTintFilter();
	if (tint)
		blit_sprite2_filter(surface, x, y, sheet, index, (Uint8)tint);
	else
		blit_sprite2(surface, x, y, sheet, index);
}

// Resting/home position for a front-mounted (style 2) option. When BOTH slots hold a
// front option they sit side by side so both are visible and both are seen to launch;
// a lone front option stays centered on the ship as it always has.
// (FRONT_OPTION_SPREAD lives in player.h so the shop preview matches.)
static int front_option_home_x(const Player *this_player, uint i)
{
	const bool both = this_player->sidekick[LEFT_SIDEKICK].style == 2
	               && this_player->sidekick[RIGHT_SIDEKICK].style == 2;
	if (!both)
		return this_player->x;
	return (i == LEFT_SIDEKICK) ? this_player->x - FRONT_OPTION_SPREAD
	                            : this_player->x + FRONT_OPTION_SPREAD;
}

// Front-mounted (launchable) option physics for one sidekick slot. Each slot keeps its
// own attachment state (optionAttachment*[i]), so a LEFT and a RIGHT front option can
// both be launched at once: "Fire Both Sidekicks" sets button[1] and button[2], and each
// pod launches on its own slot's trigger (button[1 + i]). This mirrors the behavior the
// RIGHT slot always had -- the LEFT slot previously just stayed glued to the ship.
static void JE_frontOption(Player *this_player, uint i, int home_x, JE_boolean launch_pressed)
{
	int temp;

	if (!optionAttachmentLinked[i])
	{
		this_player->sidekick[i].y += optionAttachmentMove[i] / 2;
		if (optionAttachmentMove[i] >= -2)
		{
			if (optionAttachmentReturn[i])
				temp = 2;
			else
				temp = 0;

			if (this_player->sidekick[i].y > (this_player->y - 20) + 5)
			{
				temp = 2;
				optionAttachmentMove[i] -= 1 + optionAttachmentReturn[i];
			}
			else if (this_player->sidekick[i].y > (this_player->y - 20) - 0)
			{
				temp = 3;
				if (optionAttachmentMove[i] > 0)
					optionAttachmentMove[i]--;
				else
					optionAttachmentMove[i]++;
			}
			else if (this_player->sidekick[i].y > (this_player->y - 20) - 5)
			{
				temp = 2;
				optionAttachmentMove[i]++;
			}
			else if (optionAttachmentMove[i] < 2 + optionAttachmentReturn[i] * 4)
			{
				optionAttachmentMove[i] += 1 + optionAttachmentReturn[i];
			}

			if (optionAttachmentReturn[i])
				temp = temp * 2;
			if (abs(this_player->sidekick[i].x - home_x) < temp)
				temp = 1;

			if (this_player->sidekick[i].x > home_x)
				this_player->sidekick[i].x -= temp;
			else if (this_player->sidekick[i].x < home_x)
				this_player->sidekick[i].x += temp;

			if (abs(this_player->sidekick[i].y - (this_player->y - 20)) + abs(this_player->sidekick[i].x - home_x) < 8)
			{
				optionAttachmentLinked[i] = true;
				soundQueue[2] = S_CLINK;
			}

			if (launch_pressed)
				optionAttachmentReturn[i] = true;
		}
		else  // sidekick needs to catch up to player
		{
			optionAttachmentMove[i] += 1 + optionAttachmentReturn[i];
			JE_setupExplosion(this_player->sidekick[i].x + 1, this_player->sidekick[i].y + 10, 0, 0, false, false);
		}
	}
	else
	{
		this_player->sidekick[i].x = home_x;
		this_player->sidekick[i].y = this_player->y - 20;
		if (launch_pressed)
		{
			optionAttachmentLinked[i] = false;
			optionAttachmentReturn[i] = false;
			optionAttachmentMove[i] = -20;
			soundQueue[3] = S_WEAPON_26;
		}
	}

	if (this_player->sidekick[i].y < 10)
		this_player->sidekick[i].y = 10;
}

void JE_playerMovement(Player *this_player,
                       JE_byte inputDevice,
                       JE_byte playerNum_,
                       JE_word shipGr_,
                       Sprite2_array *shipGrPtr_,
                       JE_word *mouseX_, JE_word *mouseY_)
{
	JE_integer mouseXC, mouseYC;
	JE_integer accelXC, accelYC;

	if (playerNum_ == 2 || !twoPlayerMode)
	{
		tempW = weaponPort[this_player->items.weapon[REAR_WEAPON].id].opnum;

		if (this_player->weapon_mode > tempW)
			this_player->weapon_mode = 1;
	}

	// Endless per-tick hooks (main player only, once per tick): advance the zone timer +
	// turbodrive decay, apply the GRAVITY pull, and quicken the guns during a TURBODRIVE streak.
	if (endlessMode && this_player == &player[0])
	{
		endlessGameplayTick();
		if (endlessConsumeArmorHudDirty())  // the Overheat DoT just shaved hull -- repaint the event-driven armor bar
		{
			JE_wipeShieldArmorBars();
			VGAScreen = VGAScreenSeg;
			JE_drawShield();
			JE_drawArmor();
			VGAScreen = game_screen;
		}
		if (!vt_ship_owns())  // the VT ship (normal play) applies gravity in vt_ship_step
		{
			// X is nonzero only for an omnidirectional well; both axes are clamped to the playfield
			// at the end of JE_playerMovement, so a sideways/up pull just pins the ship at that edge.
			this_player->x += endlessGravityPullX();
			this_player->y += endlessGravityPullY();
		}
		// Quicken the guns: the Rapid Cyclers perk every tick, plus the kill-fire buff during a
		// TURBODRIVE/Turbodrive streak. Both feed the same shotRepeat-decrement loop.
		{
			// Floor at 0 (not 1): the fire gate below fires when shotRepeat hits 0, so flooring
			// at 1 capped fire at one shot every 2 ticks. Zeroing lets a big buff/perk stack reach
			// the true engine limit of one shot per tick.
			const int dec = endlessPerkFireDecrements()
			              + (endlessTurbodriveActive() ? endlessKillBuffFireDecrements() : 0);
			for (unsigned i = 0; i < COUNTOF(shotRepeat); i++)
				for (int k = 0; k < dec && shotRepeat[i] > 0; k++)
					--shotRepeat[i];
		}

		// Rapid Recharge perk: extra decrements to the special cooldown gate + each sidekick's
		// ammo-refill counter (skips main guns). Sampled once per tick -- the decrement accumulator
		// is stateful and must be read exactly once. notes.md §Economy & perk plumbing.
		{
			const int specDec = endlessPerkSpecialCooldownDecrements();
			for (int k = 0; k < specDec && shotRepeat[SHOT_SPECIAL] > 0; k++)
				--shotRepeat[SHOT_SPECIAL];

			// this_player == &player[0] here (see the guard above), so these are the live sidekicks.
			for (uint i = 0; i < COUNTOF(this_player->sidekick); i++)
			{
				if (this_player->sidekick[i].ammo_max <= 0)
					continue;  // only weapons that actually use recharging ammo
				for (int k = 0; k < specDec && this_player->sidekick[i].ammo_refill_ticks > 0; k++)
					--this_player->sidekick[i].ammo_refill_ticks;
			}
		}
	}

#ifdef WITH_NETWORK
	if (isNetworkGame && thisPlayerNum == playerNum_)
	{
		network_state_prepare();
		memset(&packet_state_out[0]->data[4], 0, 10);
	}
#endif

redo:

	if (isNetworkGame)
	{
		inputDevice = 0;
	}

	mouseXC = 0;
	mouseYC = 0;
	accelXC = 0;
	accelYC = 0;

	// When the variable-timestep ship owns this player, skip the original
	// position/velocity movement here — the render-rate integrator drives it.
	const bool vt = vt_ship_owns() && playerNum_ == 1;

	bool link_gun_analog = false;
	float link_gun_angle = 0;

	/* Draw Player */
	if (!this_player->is_alive)
	{
		if (this_player->exploding_ticks > 0)
		{
			--this_player->exploding_ticks;

			if (levelEndFxWait > 0)
			{
				levelEndFxWait--;
			}
			else
			{
				levelEndFxWait = (mt_rand() % 6) + 3;
				if ((mt_rand() % 3) == 1)
					soundQueue[6] = S_EXPLOSION_9;
				else
					soundQueue[5] = S_EXPLOSION_11;
			}

			int explosion_x = this_player->x + (mt_rand() % 32) - 16;
			int explosion_y = this_player->y + (mt_rand() % 32) - 16;
			JE_setupExplosionLarge(false, 0, explosion_x, explosion_y + 7);
			JE_setupExplosionLarge(false, 0, this_player->x, this_player->y + 7);

			if (levelEnd > 0)
				levelEnd--;
		}
		else
		{
			if (twoPlayerMode || onePlayerAction)  // if arcade mode
			{
				if (*this_player->lives > 1)  // respawn if any extra lives
				{
					--(*this_player->lives);

					reallyEndLevel = false;
					shotMultiPos[playerNum_-1] = 0;
					calc_purple_balls_needed(this_player);
					twoPlayerLinked = false;
					if (galagaMode)
						twoPlayerMode = false;
					this_player->y = SHIP_BOTTOM_MARGIN;
					this_player->invulnerable_ticks = 100;
					this_player->is_alive = true;
					endLevel = false;

					if (galagaMode || episodeNum == 4)
						this_player->armor = this_player->initial_armor;
					else
						this_player->armor = this_player->initial_armor / 2;

					if (galagaMode)
						this_player->shield = 0;
					else
						this_player->shield = this_player->shield_max / 2;

					VGAScreen = VGAScreenSeg; /* side-effect of game_screen */
					JE_drawArmor();
					JE_drawShield();
					VGAScreen = game_screen; /* side-effect of game_screen */
					goto redo;
				}
				else
				{
					if (galagaMode)
						twoPlayerMode = false;
					if (allPlayersGone && isNetworkGame)
						reallyEndLevel = true;
				}

			}
		}
	}
	else if (constantDie)
	{
		// finished exploding?  start dying again
		if (this_player->exploding_ticks == 0)
		{
			this_player->shield = 0;

			if (this_player->armor > 0)
			{
				--this_player->armor;
			}
			else
			{
				this_player->is_alive = false;
				this_player->exploding_ticks = 60;
				levelEnd = 40;
			}

			JE_wipeShieldArmorBars();
			VGAScreen = VGAScreenSeg; /* side-effect of game_screen */
			JE_drawArmor();
			VGAScreen = game_screen; /* side-effect of game_screen */

			// as if instant death weren't enough, player also gets infinite lives in order to enjoy an infinite number of deaths -_-
			if (*player[0].lives < 11)
				++(*player[0].lives);
		}
	}

	if (!this_player->is_alive)
	{
		explosionFollowAmountX = explosionFollowAmountY = 0;
		return;
	}

	if (!endLevel)
	{
		*mouseX_ = this_player->x;
		*mouseY_ = this_player->y;
		// Endless SLUGGISH (classic non-VT path): snapshot the tick-start position so the whole net
		// move can be rescaled at the end. Y is snapshotted separately because the inverted-control
		// flip below rewrites *mouseY_.
		const int sluggishStartX = this_player->x;
		const int sluggishStartY = this_player->y;
		button[1-1] = false;
		button[2-1] = false;
		button[3-1] = false;
		button[4-1] = false;

		/* --- Movement Routine Beginning --- */

		if (!isNetworkGame || playerNum_ == thisPlayerNum)
		{
			if (endLevel)
			{
				this_player->y -= 2;
			}
			else
			{
				if (record_demo || play_demo)
					inputDevice = 1;  // keyboard is required device for demo recording

				// demo playback input
				if (play_demo)
				{
					if (!replay_demo_keys())
					{
						endLevel = true;
						levelEnd = 40;
					}
				}

				/* joystick input */
				if ((inputDevice == 0 || inputDevice >= 3) && joysticks > 0)
				{
					int j = inputDevice  == 0 ? 0 : inputDevice - 3;
					int j_max = inputDevice == 0 ? joysticks : inputDevice - 3 + 1;
					for (; j < j_max; j++)
					{
						poll_joystick(j);

						if (joystick[j].analog)
						{
							mouseXC += joystick_axis_reduce(j, joystick[j].x);
							mouseYC += joystick_axis_reduce(j, joystick[j].y);

							link_gun_analog = joystick_analog_angle(j, &link_gun_angle);
						}
						else if (!vt)
						{
							this_player->x += (joystick[j].direction[3] ? -CURRENT_KEY_SPEED : 0) + (joystick[j].direction[1] ? CURRENT_KEY_SPEED : 0);
							this_player->y += (joystick[j].direction[0] ? -CURRENT_KEY_SPEED : 0) + (joystick[j].direction[2] ? CURRENT_KEY_SPEED : 0);
						}

						button[0] |= joystick[j].action[0];
						button[1] |= joystick[j].action[2];
						button[2] |= joystick[j].action[3];
						button[3] |= joystick[j].action_pressed[1];

						ingamemenu_pressed |= joystick[j].action_pressed[4];
						pause_pressed |= joystick[j].action_pressed[5];
					}

					// vt_ship_step polls the pad at render rate and consumes the change-fire
					// press-edge before this tick reads it, so it latches the edge for us.
					// Fold the latched press in here (no-op when Smooth Motion / VT isn't driving).
					button[3] |= changefire_pressed;
					changefire_pressed = false;
				}

				service_SDL_events(false);

				/* mouse input */
				if ((inputDevice == 0 || inputDevice == 2) && has_mouse)
				{
#if defined(__SWITCH__) || defined(__vita__)
					/* On the consoles mouse_pressed[0] is the touchscreen's auto-fire
					 * (a drag holds it, keyboard.c). Under Toggle Fire a touch must
					 * neither shoot nor flip the toggle, so keep it out of the fire
					 * button entirely; only real buttons reach the latch below. */
					if (!debugToggleFire)
						button[0] |= mouse_pressed[0];
#else
					button[0] |= mouse_pressed[0];
#endif
					button[1] |= mouse_pressed[1];
					button[2] |= mouse_pressed[2];
					button[3] |= mouse_pressed[3];

					if (!vt)
					{
						Sint32 mouseXR;
						Sint32 mouseYR;
						mouseGetRelativePosition(&mouseXR, &mouseYR);
						mouseXC += mouseXR;
						mouseYC += mouseYR;
					}
				}

				/* keyboard input */
				if ((inputDevice == 0 || inputDevice == 1) && !play_demo)
				{
					if (!vt)
					{
						if (keysactive[keySettings[KEY_SETTING_UP]])
							this_player->y -= CURRENT_KEY_SPEED;
						if (keysactive[keySettings[KEY_SETTING_DOWN]])
							this_player->y += CURRENT_KEY_SPEED;

						if (keysactive[keySettings[KEY_SETTING_LEFT]])
							this_player->x -= CURRENT_KEY_SPEED;
						if (keysactive[keySettings[KEY_SETTING_RIGHT]])
							this_player->x += CURRENT_KEY_SPEED;
					}

					button[0] = button[0] || keysactive[keySettings[KEY_SETTING_FIRE]];
					button[3] = button[3] || keysactive[keySettings[KEY_SETTING_CHANGE_FIRE]];
					button[1] = button[1] || keysactive[keySettings[KEY_SETTING_LEFT_SIDEKICK]];
					button[2] = button[2] || keysactive[keySettings[KEY_SETTING_RIGHT_SIDEKICK]];

					if (constantPlay)
					{
						for (unsigned int i = 0; i < 4; i++)
							button[i] = true;

						++this_player->y;
						this_player->x += constantLastX;
					}

					// TODO: check if demo recording still works
					if (record_demo)
					{
						bool new_input = false;

						for (unsigned int i = 0; i < 8; i++)
						{
							bool temp = demo_keys & (1 << i);
							if (temp != keysactive[keySettings[i]])
								new_input = true;
						}

						demo_keys_wait++;

						if (new_input)
						{
							Uint8 temp2[2] = { demo_keys_wait >> 8, demo_keys_wait };
							fwrite_u8(temp2, 2, demo_file);

							demo_keys = 0;
							for (unsigned int i = 0; i < 8; i++)
								demo_keys |= keysactive[keySettings[i]] ? (1 << i) : 0;

							fwrite_u8(&demo_keys, 1, demo_file);

							demo_keys_wait = 0;
						}
					}
				}

				/* Debug Toggle Fire: the fire button is a toggle -- one press starts
				 * auto-firing, the next press stops it. The latch alone drives
				 * button[0], so a held button doesn't fire past its press-edge, and
				 * on the consoles a touch drag (excluded from button[0] above)
				 * neither fires nor flips the toggle. */
				if (debugToggleFire && playerNum_ == 1 && !play_demo && !record_demo)
				{
					static bool toggleFirePrevHeld = false;
					if (button[0] && !toggleFirePrevHeld)
						debugToggleFireActive = !debugToggleFireActive;
					toggleFirePrevHeld = button[0];
					button[0] = debugToggleFireActive;
				}

				if (smoothies[9-1])
				{
					*mouseY_ = this_player->y - (*mouseY_ - this_player->y);
					mouseYC = -mouseYC;
				}

				accelXC += this_player->x - *mouseX_;
				accelYC += this_player->y - *mouseY_;

				if (mouseXC > 30)
					mouseXC = 30;
				else if (mouseXC < -30)
					mouseXC = -30;
				if (mouseYC > 30)
					mouseYC = 30;
				else if (mouseYC < -30)
					mouseYC = -30;

				if (!vt)
				{
					if (mouseXC > 0)
						this_player->x += (mouseXC + 3) / 4;
					else if (mouseXC < 0)
						this_player->x += (mouseXC - 3) / 4;
					if (mouseYC > 0)
						this_player->y += (mouseYC + 3) / 4;
					else if (mouseYC < 0)
						this_player->y += (mouseYC - 3) / 4;
				}

				if (mouseXC > 3)
					accelXC++;
				else if (mouseXC < -2)
					accelXC--;
				if (mouseYC > 2)
					accelYC++;
				else if (mouseYC < -2)
					accelYC--;

				// Endless SLUGGISH (classic path): the VT ship scales its own move (tyrian2.c); mirror
				// it here for the Smooth-Motion-off path. Every source -- keyboard, d-pad, mouse, touch,
				// stick -- has already committed to this_player->x/y above, so rescale this tick's NET
				// displacement with a sub-pixel carry (like endlessGravityPullX/Y) so a fractional scale
				// still averages out. player[0] only; a no-op at scale 1.0, so normal play is untouched.
				if (playerNum_ == 1)
				{
					const float ms = endlessMoveScale();
					if (ms < 1.0f)
					{
						static float carryX = 0.0f, carryY = 0.0f;
						const float wantX = (float)(this_player->x - sluggishStartX) * ms + carryX;
						const float wantY = (float)(this_player->y - sluggishStartY) * ms + carryY;
						const int   dX = (int)(wantX >= 0.0f ? wantX + 0.5f : wantX - 0.5f);
						const int   dY = (int)(wantY >= 0.0f ? wantY + 0.5f : wantY - 0.5f);
						carryX = wantX - (float)dX;
						carryY = wantY - (float)dY;
						this_player->x = sluggishStartX + dX;
						this_player->y = sluggishStartY + dY;
					}
				}

			}   /*endLevel*/

#ifdef WITH_NETWORK
			if (isNetworkGame && playerNum_ == thisPlayerNum)
			{
				Uint16 buttons = 0;
				for (int i = 4 - 1; i >= 0; i--)
				{
					buttons <<= 1;
					buttons |= button[i];
				}

				SDLNet_Write16(this_player->x - *mouseX_, &packet_state_out[0]->data[4]);
				SDLNet_Write16(this_player->y - *mouseY_, &packet_state_out[0]->data[6]);
				SDLNet_Write16(accelXC,                   &packet_state_out[0]->data[8]);
				SDLNet_Write16(accelYC,                   &packet_state_out[0]->data[10]);
				SDLNet_Write16(buttons,                   &packet_state_out[0]->data[12]);

				this_player->x = *mouseX_;
				this_player->y = *mouseY_;

				button[0] = false;
				button[1] = false;
				button[2] = false;
				button[3] = false;

				accelXC = 0;
				accelYC = 0;
			}
#endif
		}  /*isNetworkGame*/

		/* --- Movement Routine Ending --- */

		moveOk = true;

#ifdef WITH_NETWORK
		if (isNetworkGame && !network_state_is_reset())
		{
			if (playerNum_ != thisPlayerNum)
			{
				if (thisPlayerNum == 2)
					difficultyLevel = SDLNet_Read16(&packet_state_in[0]->data[16]);

				Uint16 buttons = SDLNet_Read16(&packet_state_in[0]->data[12]);
				for (int i = 0; i < 4; i++)
				{
					button[i] = buttons & 1;
					buttons >>= 1;
				}

				this_player->x += (Sint16)SDLNet_Read16(&packet_state_in[0]->data[4]);
				this_player->y += (Sint16)SDLNet_Read16(&packet_state_in[0]->data[6]);
				accelXC = (Sint16)SDLNet_Read16(&packet_state_in[0]->data[8]);
				accelYC = (Sint16)SDLNet_Read16(&packet_state_in[0]->data[10]);
			}
			else
			{
				Uint16 buttons = SDLNet_Read16(&packet_state_out[network_delay]->data[12]);
				for (int i = 0; i < 4; i++)
				{
					button[i] = buttons & 1;
					buttons >>= 1;
				}

				this_player->x += (Sint16)SDLNet_Read16(&packet_state_out[network_delay]->data[4]);
				this_player->y += (Sint16)SDLNet_Read16(&packet_state_out[network_delay]->data[6]);
				accelXC = (Sint16)SDLNet_Read16(&packet_state_out[network_delay]->data[8]);
				accelYC = (Sint16)SDLNet_Read16(&packet_state_out[network_delay]->data[10]);
			}
		}
#endif

		/*Street-Fighter codes*/
		if (vt)
		{
			// Movement is skipped above, so *mouseX_/*mouseY_ stay at the ship position:
			// JE_SFCodes sees no direction and twiddles never fire. Rebuild the direction
			// from the raw controls as a 1px-offset target, leaving *mouseX_/*mouseY_
			// (used for aiming below) untouched.
			int dirx = 0, diry = 0;
			if ((inputDevice == 0 || inputDevice == 1) && !play_demo)
			{
				if (keysactive[keySettings[KEY_SETTING_LEFT]])  --dirx;
				if (keysactive[keySettings[KEY_SETTING_RIGHT]]) ++dirx;
				if (keysactive[keySettings[KEY_SETTING_UP]])    --diry;
				if (keysactive[keySettings[KEY_SETTING_DOWN]])  ++diry;
			}
			if ((inputDevice == 0 || inputDevice >= 3) && joysticks > 0)
			{
				int j     = inputDevice == 0 ? 0 : inputDevice - 3;
				int j_max = inputDevice == 0 ? joysticks : inputDevice - 3 + 1;
				for (; j < j_max; j++)
				{
					if (joystick[j].direction[3]) --dirx;  // left
					if (joystick[j].direction[1]) ++dirx;  // right
					if (joystick[j].direction[0]) --diry;  // up
					if (joystick[j].direction[2]) ++diry;  // down
				}
			}
			// Mouse steers the VT ship at render rate, so it isn't in the inputs above.
			// Fold in its accumulated direction raw (the inverted-control flip below must
			// apply to it too); call unconditionally to drain the accumulator.
			{
				int mdirx = 0, mdiry = 0;
				vt_ship_twiddle_dir(playerNum_ - 1, &mdirx, &mdiry);
				dirx += mdirx;
				diry += mdiry;
			}
			if (smoothies[9-1])  // inverted-control levels flip the vertical axis
				diry = -diry;
			if (mouseXC < 0) --dirx; else if (mouseXC > 0) ++dirx;  // analog stick (accumulated above;
			if (mouseYC < 0) --diry; else if (mouseYC > 0) ++diry;  //   mouseYC already flipped if inverted)

			// left => target is to the ship's right (PX_ < mouseX_) etc. — matches the
			// original "ship leads toward the target" sign the detector expects.
			int tx = (int)this_player->x - (dirx > 0 ? 1 : dirx < 0 ? -1 : 0);
			int ty = (int)this_player->y - (diry > 0 ? 1 : diry < 0 ? -1 : 0);
			JE_SFCodes(playerNum_, this_player->x, this_player->y, tx, ty);
		}
		else
			JE_SFCodes(playerNum_, this_player->x, this_player->y, *mouseX_, *mouseY_);

		if (moveOk)
		{
			/* END OF MOVEMENT ROUTINES */

			/*Linking Routines*/

			if (twoPlayerMode && !twoPlayerLinked && this_player->x == *mouseX_ && this_player->y == *mouseY_ &&
			    abs(player[0].x - player[1].x) < 8 && abs(player[0].y - player[1].y) < 8 &&
			    player[0].is_alive && player[1].is_alive && !galagaMode)
			{
				twoPlayerLinked = true;
			}

			if (playerNum_ == 1 && (button[3-1] || button[2-1]) && !galagaMode)
				twoPlayerLinked = false;

			if (twoPlayerMode && twoPlayerLinked && playerNum_ == 2 &&
			    (this_player->x != *mouseX_ || this_player->y != *mouseY_))
			{
				if (button[0])
				{
					if (link_gun_analog)
					{
						linkGunDirec = link_gun_angle;
					}
					else
					{
						JE_real tempR;

						if (abs(this_player->x - *mouseX_) > abs(this_player->y - *mouseY_))
							tempR = (this_player->x - *mouseX_ > 0) ? M_PI_2 : (M_PI + M_PI_2);
						else
							tempR = (this_player->y - *mouseY_ > 0) ? 0 : M_PI;

						if (fabsf(linkGunDirec - tempR) < 0.3f)
							linkGunDirec = tempR;
						else if (linkGunDirec < tempR && linkGunDirec - tempR > -3.24f)
							linkGunDirec += 0.2f;
						else if (linkGunDirec - tempR < M_PI)
							linkGunDirec -= 0.2f;
						else
							linkGunDirec += 0.2f;
					}

					if (linkGunDirec >= (2 * M_PI))
						linkGunDirec -= (2 * M_PI);
					else if (linkGunDirec < 0)
						linkGunDirec += (2 * M_PI);
				}
				else if (!galagaMode)
				{
					twoPlayerLinked = false;
				}
			}
		}
	}

	if (levelEnd > 0 && all_players_dead())
		reallyEndLevel = true;

	/* End Level Fade-Out */
	if (this_player->is_alive && endLevel)
	{
		if (levelEnd == 0)
		{
			reallyEndLevel = true;
		}
		else
		{
			this_player->y -= levelEndWarp;
			if (this_player->y < -vga_height)
				reallyEndLevel = true;

			int trail_spacing = 1;
			int trail_y = this_player->y;
			int num_trails = abs(41 - levelEnd);
			if (num_trails > 20)
				num_trails = 20;

			for (int i = 0; i < num_trails; i++)
			{
				trail_y += trail_spacing;
				trail_spacing++;
			}

			// Tag the whole warp comet with the ship id. During the end-level warp
			// vt_ship_owns() is false, so the render-rate ship-override
			// (update_ship_override) drives ship-range ids by the ship's per-tick
			// velocity. The comet flies up rigidly at that velocity, so the override
			// glides it smoothly between ticks instead of snapping.
			rl_current_id = RL_ID_SHIP_BASE + playerNum_;
			for (int i = 1; i < num_trails; i++)
			{
				trail_y -= trail_spacing;
				trail_spacing--;

				if (trail_y > 0 && trail_y < 170)
				{
					if (shipGr_ == 0)
					{
						blit_sprite2x2(VGAScreen, this_player->x - 17, trail_y - 7, *shipGrPtr_, 13);
						blit_sprite2x2(VGAScreen, this_player->x + 7 , trail_y - 7, *shipGrPtr_, 51);
					}
					else if (shipGr_ == 1)
					{
						blit_sprite2x2(VGAScreen, this_player->x - 17, trail_y - 7, *shipGrPtr_, 220);
						blit_sprite2x2(VGAScreen, this_player->x + 7 , trail_y - 7, *shipGrPtr_, 222);
					}
					else
					{
						blit_sprite2x2(VGAScreen, this_player->x - 5, trail_y - 7, *shipGrPtr_, shipGr_);
					}
				}
			}
			rl_current_id = 0;
		}
	}

	if (play_demo)
	{
		const int playfield_left = PLAYFIELD_LEFT;
		const int insert_coin_x = playfield_left + (PLAYFIELD_WIDTH - JE_textWidth(miscText[7], SMALL_FONT_SHAPES)) / 2;
		const int insert_coin_y = 10;
		JE_dString(VGAScreen, insert_coin_x, insert_coin_y, miscText[7], SMALL_FONT_SHAPES); // insert coin
	}

	if (this_player->is_alive && !endLevel)
	{
		if (!twoPlayerLinked || playerNum_ < 2)
		{
			if (!twoPlayerMode || shipGr2 != 0)  // if not dragonwing
			{
				if (this_player->sidekick[LEFT_SIDEKICK].style == 0)
				{
					this_player->sidekick[LEFT_SIDEKICK].x = *mouseX_ - 14;
					this_player->sidekick[LEFT_SIDEKICK].y = *mouseY_;
				}

				if (this_player->sidekick[RIGHT_SIDEKICK].style == 0)
				{
					this_player->sidekick[RIGHT_SIDEKICK].x = *mouseX_ + 16;
					this_player->sidekick[RIGHT_SIDEKICK].y = *mouseY_;
				}
			}

			if (!vt)
			{
			if (this_player->x_friction_ticks > 0)
			{
				--this_player->x_friction_ticks;
			}
			else
			{
				this_player->x_friction_ticks = 1;

				if (this_player->x_velocity < 0)
					++this_player->x_velocity;
				else if (this_player->x_velocity > 0)
					--this_player->x_velocity;
			}

			if (this_player->y_friction_ticks > 0)
			{
				--this_player->y_friction_ticks;
			}
			else
			{
				this_player->y_friction_ticks = 2;

				if (this_player->y_velocity < 0)
					++this_player->y_velocity;
				else if (this_player->y_velocity > 0)
					--this_player->y_velocity;
			}

			this_player->x_velocity += accelXC;
			this_player->y_velocity += accelYC;

			this_player->x_velocity = MIN(MAX(-4, this_player->x_velocity), 4);
			this_player->y_velocity = MIN(MAX(-4, this_player->y_velocity), 4);

			this_player->x += this_player->x_velocity;
			this_player->y += this_player->y_velocity;
			}

			// if player moved, add new ship x, y history entry
			if (this_player->x - *mouseX_ != 0 || this_player->y - *mouseY_ != 0)
			{
				for (uint i = 1; i < COUNTOF(player->old_x); ++i)
				{
					this_player->old_x[i - 1] = this_player->old_x[i];
					this_player->old_y[i - 1] = this_player->old_y[i];
				}
				this_player->old_x[COUNTOF(player->old_x) - 1] = this_player->x;
				this_player->old_y[COUNTOF(player->old_x) - 1] = this_player->y;
			}
		}
		else  /*twoPlayerLinked*/
		{
			if (shipGr_ == 0)
				this_player->x = player[0].x - 1;
			else
				this_player->x = player[0].x;
			this_player->y = player[0].y + 8;

			this_player->x_velocity = player[0].x_velocity;
			this_player->y_velocity = 4;

			// turret direction marker/shield
			shotMultiPos[SHOT_MISC] = 0;
			b = player_shot_create(0, SHOT_MISC, this_player->x + 1 + roundf(sinf(linkGunDirec + 0.2f) * 26), this_player->y + roundf(cosf(linkGunDirec + 0.2f) * 26), *mouseX_, *mouseY_, 148, playerNum_);
			shotMultiPos[SHOT_MISC] = 0;
			b = player_shot_create(0, SHOT_MISC, this_player->x + 1 + roundf(sinf(linkGunDirec - 0.2f) * 26), this_player->y + roundf(cosf(linkGunDirec - 0.2f) * 26), *mouseX_, *mouseY_, 148, playerNum_);
			shotMultiPos[SHOT_MISC] = 0;
			b = player_shot_create(0, SHOT_MISC, this_player->x + 1 + roundf(sinf(linkGunDirec) * 26), this_player->y + roundf(cosf(linkGunDirec) * 26), *mouseX_, *mouseY_, 147, playerNum_);

			if (shotRepeat[SHOT_REAR] > 0)
			{
				--shotRepeat[SHOT_REAR];
			}
			else if (button[1-1])
			{
				shotMultiPos[SHOT_REAR] = 0;
				b = player_shot_create(0, SHOT_REAR, this_player->x + 1 + roundf(sinf(linkGunDirec) * 20), this_player->y + roundf(cosf(linkGunDirec) * 20), *mouseX_, *mouseY_, linkGunWeapons[this_player->items.weapon[REAR_WEAPON].id-1], playerNum_);
				player_shot_set_direction(b, this_player->items.weapon[REAR_WEAPON].id, linkGunDirec);
			}
		}
	}

	if (!endLevel)
	{
		if (this_player->x > PLAYFIELD_WIDTH - SHIP_RIGHT_MARGIN)
		{
			this_player->x = PLAYFIELD_WIDTH - SHIP_RIGHT_MARGIN;
			constantLastX = -constantLastX;
		}
		if (this_player->x < SHIP_LEFT_MARGIN)
		{
			this_player->x = SHIP_LEFT_MARGIN;
			constantLastX = -constantLastX;
		}

		if (isNetworkGame && playerNum_ == 1)
		{
			// Network mode keeps a 6px tighter bottom bound (room for the other
			// player's ship); keep it relative to SHIP_BOTTOM_MARGIN as that is tuned.
			if (this_player->y > SHIP_BOTTOM_MARGIN - 6)
				this_player->y = SHIP_BOTTOM_MARGIN - 6;
		}
		else
		{
			if (this_player->y > SHIP_BOTTOM_MARGIN)
				this_player->y = SHIP_BOTTOM_MARGIN;
		}

		if (this_player->y < SHIP_TOP_MARGIN)
			this_player->y = SHIP_TOP_MARGIN;

		// Determines the ship banking sprite to display, depending on horizontal velocity and acceleration
		int ship_banking;
		if (vt)
		{
			// Under VT x is constant within a tick and mouse steering never touches
			// x_velocity, so the vanilla formula shows no tilt; use the inter-tick
			// horizontal movement, which captures keyboard, joystick and mouse.
			int dvx, dvy;
			vt_ship_shot_delta(playerNum_ - 1, &dvx, &dvy);
			ship_banking = dvx / 2;
		}
		else
		{
			ship_banking = this_player->x_velocity / 2 + (this_player->x - *mouseX_) / 6;
		}
		ship_banking = MAX(-2, MIN(ship_banking, 2));

		int ship_sprite = ship_banking * 2 + shipGr_;

		explosionFollowAmountX = this_player->x - this_player->last_x_explosion_follow;
		explosionFollowAmountY = this_player->y - this_player->last_y_explosion_follow;

		if (explosionFollowAmountY < 0)
			explosionFollowAmountY = 0;

		this_player->last_x_explosion_follow = this_player->x;
		this_player->last_y_explosion_follow = this_player->y;

		// Tag the ship (shadow + hull) for cross-frame interpolation.
		rl_current_id = RL_ID_SHIP_BASE + playerNum_;

		// Cast-shadow horizontal light offset. The shadow rides the background-2
		// parallax (- mapX2Ofs); this value equals mapX2Ofs at the centre of the ship's
		// x-range, re-centring the shadow under the hull at mid-screen so it swings
		// symmetrically. (The old 30 suited the original 320px parallax curve.)
		const int shadow_light_dx = 18;

		if (shipGr_ == 0)
		{
			if (background2)
			{
				blit_sprite2x2_darken(VGAScreen, this_player->x - 17 - mapX2Ofs + shadow_light_dx, this_player->y - 7 + shadowYDist, *shipGrPtr_, ship_sprite + 13);
				blit_sprite2x2_darken(VGAScreen, this_player->x + 7 - mapX2Ofs + shadow_light_dx, this_player->y - 7 + shadowYDist, *shipGrPtr_, ship_sprite + 51);
				if (superWild)
				{
					blit_sprite2x2_darken(VGAScreen, this_player->x - 16 - mapX2Ofs + shadow_light_dx, this_player->y - 7 + shadowYDist, *shipGrPtr_, ship_sprite + 13);
					blit_sprite2x2_darken(VGAScreen, this_player->x + 6 - mapX2Ofs + shadow_light_dx, this_player->y - 7 + shadowYDist, *shipGrPtr_, ship_sprite + 51);
				}
			}
		}
		else if (shipGr_ == 1)
		{
			if (background2)
			{
				blit_sprite2x2_darken(VGAScreen, this_player->x - 17 - mapX2Ofs + shadow_light_dx, this_player->y - 7 + shadowYDist, *shipGrPtr_, 220);
				blit_sprite2x2_darken(VGAScreen, this_player->x + 7 - mapX2Ofs + shadow_light_dx, this_player->y - 7 + shadowYDist, *shipGrPtr_, 222);
			}
		}
		else
		{
			if (background2)
			{
				blit_sprite2x2_darken(VGAScreen, this_player->x - 5 - mapX2Ofs + shadow_light_dx, this_player->y - 7 + shadowYDist, *shipGrPtr_, ship_sprite);
				if (superWild)
				{
					blit_sprite2x2_darken(VGAScreen, this_player->x - 4 - mapX2Ofs + shadow_light_dx, this_player->y - 7 + shadowYDist, *shipGrPtr_, ship_sprite);
				}
			}
		}

		// Noclip transparent mode draws the hull semi-transparent (same blended blit
		// as the post-hit invulnerability flash), so it reads as a "ghost" ship.
		if (this_player->invulnerable_ticks > 0 || noclipMode == NOCLIP_TRANSPARENT)
		{
			if (this_player->invulnerable_ticks > 0)
				--this_player->invulnerable_ticks;

			if (shipGr_ == 0)
			{
				blit_sprite2x2_blend(VGAScreen, this_player->x - 17, this_player->y - 7, *shipGrPtr_, ship_sprite + 13);
				blit_sprite2x2_blend(VGAScreen, this_player->x + 7 , this_player->y - 7, *shipGrPtr_, ship_sprite + 51);
			}
			else if (shipGr_ == 1)
			{
				blit_sprite2x2_blend(VGAScreen, this_player->x - 17, this_player->y - 7, *shipGrPtr_, 220);
				blit_sprite2x2_blend(VGAScreen, this_player->x + 7 , this_player->y - 7, *shipGrPtr_, 222);
			}
			else
				blit_sprite2x2_blend(VGAScreen, this_player->x - 5, this_player->y - 7, *shipGrPtr_, ship_sprite);
		}
		else
		{
			if (shipGr_ == 0)
			{
				blit_ship2x2(VGAScreen, this_player->x - 17, this_player->y - 7, *shipGrPtr_, ship_sprite + 13);
				blit_ship2x2(VGAScreen, this_player->x + 7, this_player->y - 7, *shipGrPtr_, ship_sprite + 51);
			}
			else if (shipGr_ == 1)
			{
				blit_ship2x2(VGAScreen, this_player->x - 17, this_player->y - 7, *shipGrPtr_, 220);
				blit_ship2x2(VGAScreen, this_player->x + 7, this_player->y - 7, *shipGrPtr_, 222);

				int ship_banking = 0;
				switch (ship_sprite)
				{
				case 5:
					blit_ship2(VGAScreen, this_player->x - 17, this_player->y + 7, *shipGrPtr_, 40);
					tempW = this_player->x - 7;
					ship_banking = -2;
					break;
				case 3:
					blit_ship2(VGAScreen, this_player->x - 17, this_player->y + 7, *shipGrPtr_, 39);
					tempW = this_player->x - 7;
					ship_banking = -1;
					break;
				case 1:
					ship_banking = 0;
					break;
				case -1:
					blit_ship2(VGAScreen, this_player->x + 19, this_player->y + 7, *shipGrPtr_, 58);
					tempW = this_player->x + 9;
					ship_banking = 1;
					break;
				case -3:
					blit_ship2(VGAScreen, this_player->x + 19, this_player->y + 7, *shipGrPtr_, 59);
					tempW = this_player->x + 9;
					ship_banking = 2;
					break;
				}
				if (ship_banking != 0)  // NortSparks
				{
					if (shotRepeat[SHOT_NORTSPARKS] > 0)
					{
						--shotRepeat[SHOT_NORTSPARKS];
					}
					else
					{
						b = player_shot_create(0, SHOT_NORTSPARKS, tempW + (mt_rand() % 8) - 4, this_player->y + (mt_rand() % 8) - 4, *mouseX_, *mouseY_, 671, 1);
						shotRepeat[SHOT_NORTSPARKS] = abs(ship_banking) - 1;
					}
				}
			}
			else
			{
				blit_ship2x2(VGAScreen, this_player->x - 5, this_player->y - 7, *shipGrPtr_, ship_sprite);
			}
		}

		rl_current_id = 0;  // end ship tag

		/*Options Location*/
		if (playerNum_ == 2 && shipGr_ == 0)  // if dragonwing
		{
			if (this_player->sidekick[LEFT_SIDEKICK].style == 0)
			{
				this_player->sidekick[LEFT_SIDEKICK].x = this_player->x - 14 + ship_banking * 2;
				this_player->sidekick[LEFT_SIDEKICK].y = this_player->y;
			}

			if (this_player->sidekick[RIGHT_SIDEKICK].style == 0)
			{
				this_player->sidekick[RIGHT_SIDEKICK].x = this_player->x + 17 + ship_banking * 2;
				this_player->sidekick[RIGHT_SIDEKICK].y = this_player->y;
			}
		}
	}  // !endLevel

	if (moveOk)
	{
		if (this_player->is_alive)
		{
			if (!endLevel)
			{
				if (vt)
				{
					// VT moves the ship between ticks, so (x - last_x_shot_move) reads ~0;
					// use the inter-tick delta so tracking shots (laser, main pulse) follow.
					vt_ship_shot_delta(playerNum_ - 1,
					                   &this_player->delta_x_shot_move,
					                   &this_player->delta_y_shot_move);

					// del 98/99/100 weapons (e.g. Vulcan) give new shots the ship's per-tick
					// movement via (PX - *mouseX_)/(PY - *mouseY_) in player_shot_create.
					// Classically *mouseX_/*mouseY_ hold the start-of-tick position; VT left
					// them at the current one (difference 0). Restore start-of-tick = current
					// - inter-tick delta (the history/accel code above already ran).
					*mouseX_ = this_player->x - this_player->delta_x_shot_move;
					*mouseY_ = this_player->y - this_player->delta_y_shot_move;
				}
				else
				{
					this_player->delta_x_shot_move = this_player->x - this_player->last_x_shot_move;
					this_player->delta_y_shot_move = this_player->y - this_player->last_y_shot_move;
				}

				/* PLAYER SHOT Change */
				if (button[4-1])
				{
					portConfigChange = true;
					if (portConfigDone)
					{
						shotMultiPos[SHOT_REAR] = 0;

						if (superArcadeMode != SA_NONE && superArcadeMode <= SA_LASTSHIP)
						{
							shotMultiPos[SHOT_SPECIAL] = 0;
							shotMultiPos[SHOT_SPECIAL2] = 0;
							if (player[0].items.special == SASpecialWeapon[superArcadeMode-1])
							{
								player[0].items.special = SASpecialWeaponB[superArcadeMode-1];
								this_player->weapon_mode = 2;
							}
							else
							{
								player[0].items.special = SASpecialWeapon[superArcadeMode-1];
								this_player->weapon_mode = 1;
							}
						}
						else if (++this_player->weapon_mode > JE_portConfigs())
							this_player->weapon_mode = 1;

						JE_drawPortConfigButtons();
						portConfigDone = false;
					}
				}

				/* PLAYER SHOT Creation */

				/*SpecialShot*/
				if (!galagaMode)
					JE_doSpecialShot(playerNum_, &this_player->armor, &this_player->shield);

				/*Normal Main Weapons*/
				if (!(twoPlayerLinked && playerNum_ == 2))
				{
					int min, max;

					if (!twoPlayerMode)
						min = 1, max = 2;
					else
						min = max = playerNum_;

					for (temp = min - 1; temp < max; temp++)
					{
						const uint item = this_player->items.weapon[temp].id;

						if (item > 0)
						{
							if (shotRepeat[temp] > 0)
							{
								--shotRepeat[temp];
							}
							else if (button[1-1])
							{
								const uint item_power = galagaMode ? 0 : this_player->items.weapon[temp].power - 1,
								           item_mode = (temp == REAR_WEAPON) ? this_player->weapon_mode - 1 : 0;

								// Zica Laser (port 5) Lv11 tweaks. Length "Long" swaps the vanilla
								// Lv11 shot for two LV10-length side beams; the "Buff" adds the Lv10
								// centre beam. The primary shot pays the port's power cost; the extra
								// beams fire drain-free so the combined weapon still costs one shot.
								const bool zica_l11 = (item == 5 && temp == FRONT_WEAPON && item_power == 10);
								JE_word l11_primary = weaponPort[item].op[item_mode][item_power];
								if (zica_l11 && zicaLaserLength == ZICA_LEN_LONG)
									l11_primary = ZICA_LONG_WEAP_LEFT;

								b = player_shot_create(item, temp, this_player->x, this_player->y, *mouseX_, *mouseY_, l11_primary, playerNum_);

								if (zica_l11 && (zicaLaserLength == ZICA_LEN_LONG || zicaLaserBuff))
								{
									JE_word saved_poweruse = weaponPort[item].poweruse;
									weaponPort[item].poweruse = 0;
									if (zicaLaserLength == ZICA_LEN_LONG)
										player_shot_create(item, temp, this_player->x, this_player->y, *mouseX_, *mouseY_, ZICA_LONG_WEAP_RIGHT, playerNum_);
									if (zicaLaserBuff)
										player_shot_create(item, temp, this_player->x, this_player->y, *mouseX_, *mouseY_, weaponPort[item].op[item_mode][9], playerNum_);
									weaponPort[item].poweruse = saved_poweruse;
								}
							}
						}
					}
				}

				/*Super Charge Weapons*/
				if (playerNum_ == 2)
				{

					if (!twoPlayerLinked)
					{
						rl_current_id = RL_ID_SHIP_BASE + playerNum_;  // charge meter rides with the ship
						blit_sprite2(VGAScreen, this_player->x + (shipGr_ == 0) + 1, this_player->y - 13, spriteSheet10, 77 + chargeLevel + chargeGr * 19);
						rl_current_id = 0;
					}

					if (chargeGrWait > 0)
					{
						chargeGrWait--;
					}
					else
					{
						chargeGr++;
						if (chargeGr == 4)
							chargeGr = 0;
						chargeGrWait = 3;
					}

					if (chargeLevel > 0)
					{
						fill_rectangle_xy(VGAScreenSeg, HUD_X(269), 107 + (chargeLevel - 1) * 3, HUD_X(275), 108 + (chargeLevel - 1) * 3, 193);
					}

					if (chargeWait > 0)
					{
						chargeWait--;
					}
					else
					{
						if (chargeLevel < chargeMax)
							chargeLevel++;

						chargeWait = 28 - this_player->items.weapon[REAR_WEAPON].power * 2;
						if (difficultyLevel > DIFFICULTY_HARD)
							chargeWait -= 5;
					}

					if (chargeLevel > 0)
						fill_rectangle_xy(VGAScreenSeg, HUD_X(269), 107 + (chargeLevel - 1) * 3, HUD_X(275), 108 + (chargeLevel - 1) * 3, 204);

					if (shotRepeat[SHOT_P2_CHARGE] > 0)
					{
						--shotRepeat[SHOT_P2_CHARGE];
					}
					else if (button[1-1] && (!twoPlayerLinked || chargeLevel > 0))
					{
						shotMultiPos[SHOT_P2_CHARGE] = 0;
						b = player_shot_create(16, SHOT_P2_CHARGE, this_player->x, this_player->y, *mouseX_, *mouseY_, chargeGunWeapons[player[1].items.weapon[REAR_WEAPON].id-1] + chargeLevel, playerNum_);

						if (chargeLevel > 0)
							fill_rectangle_xy(VGAScreenSeg, HUD_X(269), 107 + (chargeLevel - 1) * 3, HUD_X(275), 108 + (chargeLevel - 1) * 3, 193);

						chargeLevel = 0;
						chargeWait = 30 - this_player->items.weapon[REAR_WEAPON].power * 2;
					}
				}

				/*SUPER BOMB*/
				temp = playerNum_;
				if (temp == 0)
					temp = 1;  /*Get whether player 1 or 2*/

				if (player[temp-1].superbombs > 0)
				{
					if (shotRepeat[SHOT_P1_SUPERBOMB + temp-1] > 0)
					{
						--shotRepeat[SHOT_P1_SUPERBOMB + temp-1];
					}
					else if ((button[3-1] || button[2-1]) && !(endlessMode && (endlessActiveMods & ENDLESS_MOD_DUD)))
					{  // Dud (gamble curse): the bombs are aboard but jammed -- the fire press does nothing this sector
						--player[temp-1].superbombs;
						shotMultiPos[SHOT_P1_SUPERBOMB + temp-1] = 0;
						b = player_shot_create(16, SHOT_P1_SUPERBOMB + temp-1, this_player->x, this_player->y, *mouseX_, *mouseY_, 535, playerNum_);
					}
				}

				// sidekicks

				if (this_player->sidekick[LEFT_SIDEKICK].style == 4 && this_player->sidekick[RIGHT_SIDEKICK].style == 4)
					optionSatelliteRotate += 0.2f;
				else if (this_player->sidekick[LEFT_SIDEKICK].style == 4 || this_player->sidekick[RIGHT_SIDEKICK].style == 4)
					optionSatelliteRotate += 0.15f;

				switch (this_player->sidekick[LEFT_SIDEKICK].style)
				{
				case 1:  // trailing
				case 3:
					this_player->sidekick[LEFT_SIDEKICK].x = this_player->old_x[COUNTOF(player->old_x) / 2 - 1];
					this_player->sidekick[LEFT_SIDEKICK].y = this_player->old_y[COUNTOF(player->old_x) / 2 - 1];
					break;
				case 2:  // front-mounted (launchable)
					JE_frontOption(this_player, LEFT_SIDEKICK, front_option_home_x(this_player, LEFT_SIDEKICK), button[1 + LEFT_SIDEKICK]);
					break;
				case 4:  // orbiting
					this_player->sidekick[LEFT_SIDEKICK].x = this_player->x + roundf(sinf(optionSatelliteRotate) * 20);
					this_player->sidekick[LEFT_SIDEKICK].y = this_player->y + roundf(cosf(optionSatelliteRotate) * 20);
					break;
				}

				switch (this_player->sidekick[RIGHT_SIDEKICK].style)
				{
				case 4:  // orbiting
					this_player->sidekick[RIGHT_SIDEKICK].x = this_player->x - roundf(sinf(optionSatelliteRotate) * 20);
					this_player->sidekick[RIGHT_SIDEKICK].y = this_player->y - roundf(cosf(optionSatelliteRotate) * 20);
					break;
				case 1:  // trailing
				case 3:
					this_player->sidekick[RIGHT_SIDEKICK].x = this_player->old_x[0];
					this_player->sidekick[RIGHT_SIDEKICK].y = this_player->old_y[0];
					break;
				case 2:  // front-mounted (launchable)
					JE_frontOption(this_player, RIGHT_SIDEKICK, front_option_home_x(this_player, RIGHT_SIDEKICK), button[1 + RIGHT_SIDEKICK]);
					break;
				}

				if (playerNum_ == 2 || !twoPlayerMode)  // if player has sidekicks
				{
					for (uint i = 0; i < COUNTOF(player->items.sidekick); ++i)
					{
						uint shot_i = (i == 0) ? SHOT_LEFT_SIDEKICK : SHOT_RIGHT_SIDEKICK;

						JE_OptionType *this_option = &options[this_player->items.sidekick[i]];

						// fire/refill sidekick
						if (this_option->wport > 0)
						{
							if (shotRepeat[shot_i] > 0)
							{
								--shotRepeat[shot_i];
							}
							else
							{
								const int ammo_max = cheatInfiniteSidekickAmmo ? 0 : this_player->sidekick[i].ammo_max;

								if (!cheatInfiniteSidekickAmmo && ammo_max > 0)  // sidekick has limited ammo
								{
									if (this_player->sidekick[i].ammo_refill_ticks > 0)
									{
										--this_player->sidekick[i].ammo_refill_ticks;
									}
									else  // refill one ammo
									{
										this_player->sidekick[i].ammo_refill_ticks = this_player->sidekick[i].ammo_refill_ticks_max;

										if (this_player->sidekick[i].ammo < ammo_max)
											++this_player->sidekick[i].ammo;

										// draw sidekick refill ammo gauge
										const int y = hud_sidekick_y[twoPlayerMode ? 1 : 0][i] + 13;
										const int hud_x = HUD_X(284);
										draw_segmented_gauge(VGAScreenSeg, hud_x, y, 112, 2, 2, MAX(1, ammo_max / 10), this_player->sidekick[i].ammo);
									}

									if (button[1 + i] && (cheatInfiniteSidekickAmmo || this_player->sidekick[i].ammo > 0))
									{
										b = player_shot_create(this_option->wport, shot_i, this_player->sidekick[i].x, this_player->sidekick[i].y, *mouseX_, *mouseY_, this_option->wpnum + this_player->sidekick[i].charge, playerNum_);

										if (!cheatInfiniteSidekickAmmo)
											--this_player->sidekick[i].ammo;
										if (this_player->sidekick[i].charge > 0)
										{
											shotMultiPos[shot_i] = 0;
											this_player->sidekick[i].charge = 0;
										}
										this_player->sidekick[i].charge_ticks = endlessPerkChargeTicks(20);
										this_player->sidekick[i].animation_enabled = true;

										// draw sidekick discharge ammo gauge
										const int y = hud_sidekick_y[twoPlayerMode ? 1 : 0][i] + 13;
										if (!cheatInfiniteSidekickAmmo)
										{
											const int hud_x = HUD_X(284);
											fill_rectangle_xy(VGAScreenSeg, hud_x, y, hud_x + 28, y + 2, 0);
											draw_segmented_gauge(VGAScreenSeg, hud_x, y, 112, 2, 2, MAX(1, ammo_max / 10), this_player->sidekick[i].ammo);
										}
									}
								}
								else  // has infinite ammo
								{
									/*
									// Tyrian 2000: weapons with charge stages do not auto-fire
									if ((button[0] && !this_option->pwr) || button[1 + i])
									*/

									// Charge sidekicks (pwr > 0) autofire on the held main button per
									// this mode; non-charge sidekicks and the dedicated button always fire.
									const bool charge_autofire =
										chargeSidekickAutofire == CHARGE_AUTOFIRE_ON
										|| chargeSidekickAutofire == CHARGE_AUTOFIRE_FAST
										|| (chargeSidekickAutofire == CHARGE_AUTOFIRE_FULL
										    && this_player->sidekick[i].charge >= this_option->pwr);

									if ((button[0] && (charge_autofire || !this_option->pwr)) || button[1 + i])
									{
										b = player_shot_create(this_option->wport, shot_i, this_player->sidekick[i].x, this_player->sidekick[i].y, *mouseX_, *mouseY_, this_option->wpnum + this_player->sidekick[i].charge, playerNum_);

										if (this_player->sidekick[i].charge > 0)
										{
											shotMultiPos[shot_i] = 0;
											this_player->sidekick[i].charge = 0;
										}
										this_player->sidekick[i].charge_ticks = endlessPerkChargeTicks(20);
										this_player->sidekick[i].animation_enabled = true;

										// "Yes (fastest)": player_shot_create just set shotRepeat from the shot
										// we fired (the top charge stage when Instant Charge holds it maxed,
										// whose shotrepeat is the SLOWEST). Override it with the quickest
										// charge stage's shotrepeat so full-power blasts come out at stage-0
										// speed. notes.md §Weapons.
										if (chargeSidekickAutofire == CHARGE_AUTOFIRE_FAST && this_option->pwr > 0)
										{
											JE_byte fastest = weapons[this_option->wpnum].shotrepeat;
											for (uint s = 1; s <= this_option->pwr; ++s)
												if (weapons[this_option->wpnum + s].shotrepeat < fastest)
													fastest = weapons[this_option->wpnum + s].shotrepeat;
											shotRepeat[shot_i] = fastest;
										}
									}
								}
							}
						}
					}
				}  // end of if player has sidekicks
			}  // !endLevel
		} // this_player->is_alive
	} // moveOK

	// draw sidekicks
	if ((playerNum_ == 2 || !twoPlayerMode) && !endLevel)
	{
		for (uint i = 0; i < COUNTOF(this_player->sidekick); ++i)
		{
			JE_OptionType *this_option = &options[this_player->items.sidekick[i]];

			if (this_option->option > 0)
			{
				if (this_player->sidekick[i].animation_enabled)
				{
					if (++this_player->sidekick[i].animation_frame >= this_option->ani)
					{
						this_player->sidekick[i].animation_frame = 0;
						this_player->sidekick[i].animation_enabled = (this_option->option == 1);
					}
				}

				const int x = this_player->sidekick[i].x,
				          y = this_player->sidekick[i].y;
				const uint sprite = this_option->gr[this_player->sidekick[i].animation_frame] + this_player->sidekick[i].charge;

				rl_current_id = RL_ID_SIDEKICK_BASE + playerNum_ * 2 + (int)i;
				// Style-0 pods sit at a fixed offset from the ship (reset to ship.x/y each
				// tick); attach them to the render-rate ship on both axes or they rubber-band
				// a tick behind it. Other styles move on their own path and interpolate normally.
				rl_shot_attach = (this_player->sidekick[i].style == 0)
				               ? (3 | ((playerNum_ - 1) << 2))
				               : 0;
				if (this_player->sidekick[i].style == 1 || this_player->sidekick[i].style == 2)
					blit_sprite2x2(VGAScreen, x - 6, y, spriteSheet10, sprite);
				else
					blit_sprite2(VGAScreen, x, y, spriteSheet9, sprite);
				rl_current_id = 0;
				rl_shot_attach = 0;
			}

			if (cheatInstantCharge)
			{
				// debug: skip the timed ramp, hold the sidekick at full charge
				this_player->sidekick[i].charge = this_option->pwr;
			}
			else if (--this_player->sidekick[i].charge_ticks == 0)
			{
				if (this_player->sidekick[i].charge < this_option->pwr)
					++this_player->sidekick[i].charge;
				this_player->sidekick[i].charge_ticks = endlessPerkChargeTicks(20);
			}
		}
	}
}

void JE_mainGamePlayerFunctions(void)
{
	/*PLAYER MOVEMENT/MOUSE ROUTINES*/

	if (endLevel && levelEnd > 0)
	{
		levelEnd--;
		levelEndWarp++;
	}

	/*Reset Street-Fighter commands*/
	memset(SFExecuted, 0, sizeof(SFExecuted));

	portConfigChange = false;

	if (twoPlayerMode)
	{
		JE_playerMovement(&player[0],
		                  !galagaMode ? inputDevice[0] : 0, 1, shipGr, shipGrPtr,
		                  &mouseX, &mouseY);
		JE_playerMovement(&player[1],
		                  !galagaMode ? inputDevice[1] : 0, 2, shipGr2, shipGr2ptr,
		                  &mouseXB, &mouseYB);
	}
	else
	{
		JE_playerMovement(&player[0],
		                  0, 1, shipGr, shipGrPtr,
		                  &mouseX, &mouseY);
	}

	/* == Parallax Map Scrolling == */
	JE_word tempX;
	if (twoPlayerMode)
		tempX = (player[0].x + player[1].x) / 2;
	else
		tempX = player[0].x;

	const float left_bound = 40.0f;
	const float right_bound = PLAYFIELD_WIDTH + 64;
	float u = (tempX - left_bound) / (right_bound - left_bound);
	if (u < 0.0f)
		u = 0.0f;
	else if (u > 1.0f)
		u = 1.0f;
	tempW = floorf((1.0f - u) * (24 * 3));
	mapX3Ofs = tempW;
	mapX3Pos = mapX3Ofs % 24;
	mapX3bpPos = 1 - (mapX3Ofs / 24);

	mapX2Ofs   = ((tempW-17) * 2) / 3;
	mapX2Pos   = mapX2Ofs % 24;
	mapX2bpPos = 1 - (mapX2Ofs / 24);

	oldMapXOfs = mapXOfs;
	oldMapXOfs_f = mapXOfs_f;  // both still hold the PREVIOUS tick's value here (updated below)
	mapXOfs    = mapX2Ofs / 2;
	mapXPos    = mapXOfs % 24;
	mapXbpPos  = 1 - (mapXOfs / 24);

	if (background3x1)
	{
		mapX3Ofs = mapXOfs;
		mapX3Pos = mapXPos;
		mapX3bpPos = mapXbpPos - 1;
	}

	// Un-floored mirror of the same offsets. The render list interpolates each background
	// layer's horizontal pan by the FLOAT per-tick delta of these (backgrnd.c), so a slow
	// parallax glides sub-pixel-smooth instead of stepping a whole pixel every few ticks,
	// while staying on the same interpolation timeline as the enemies anchored to it.
	const float w_f = (1.0f - u) * (24.0f * 3.0f);
	mapX3Ofs_f = w_f;
	mapX2Ofs_f = ((w_f - 17.0f) * 2.0f) / 3.0f;
	mapXOfs_f  = mapX2Ofs_f / 2.0f;
	if (background3x1)
		mapX3Ofs_f = mapXOfs_f;
}

const char *JE_getName(JE_byte pnum)
{
	if (pnum == thisPlayerNum && network_player_name[0] != '\0')
		return network_player_name;
	else if (network_opponent_name[0] != '\0')
		return network_opponent_name;

	return miscText[47 + pnum];
}

// Look up a level's display name by section number, for the secret-level pickup
// message. Mirrors JE_loadMap: scan the episode script to section `levelNum`, then
// read the next "]L" declaration, whose name is the 9 chars at offset 13 (as
// JE_loadMap reads it: SDL_strlcpy(levelName, s + 13, 10)). A secret orb often
// targets a *routing* section with no ]L of its own, so the scan crosses section
// boundaries to the next named level. Writes "" if none found; the file-size bound
// keeps a bad number from running the die-on-EOF reader past end of file.
static void JE_getLevelName(int levelNum, char *out, size_t outSize)
{
	if (outSize == 0)
		return;
	out[0] = '\0';

	FILE *f = dir_fopen(data_dir(), episode_file, "rb");
	if (f == NULL)
		return;

	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);

	char s[256];

	// Seek past the first `levelNum` section markers ('*') to the target section.
	int x = 0;
	while (x < levelNum && ftell(f) < fsize)
	{
		read_encrypted_pascal_string(s, sizeof(s), f);
		if (s[0] == '*')
			x++;
	}

	// Find the next "]L" declaration (across sections, to resolve routing sections).
	while (ftell(f) < fsize)
	{
		s[0] = '\0';
		read_encrypted_pascal_string(s, sizeof(s), f);
		if (s[0] == ']' && s[1] == 'L' && strlen(s) >= 13)
		{
			char name[10];
			SDL_strlcpy(name, s + 13, sizeof(name));  // 9-char name field, as the game does
			for (int i = (int)strlen(name) - 1; i >= 0 && name[i] == ' '; --i)
				name[i] = '\0';  // trim the space padding
			SDL_strlcpy(out, name, outSize);
			break;
		}
	}

	fclose(f);
}

void JE_playerCollide(Player *this_player, JE_byte playerNum_)
{
	char tempStr[256];

	for (int z = 0; z < 100; z++)
	{
		if (enemyAvail[z] != 1)
		{
			int enemy_screen_x = enemy[z].ex + enemy[z].mapoffset;

			// Both refs sit ~equally left/up of their sprite centres (ship +7/+7,
			// enemy +6/+7), so the ref-to-ref test is effectively centre-to-centre.
			if (abs(this_player->x - enemy_screen_x) < 12 && abs(this_player->y - enemy[z].ey) < 14)
			{   /*Collide*/
				int evalue = enemy[z].evalue;
				if (evalue > 29999)
				{
					if (evalue == 30000)  // spawn dragonwing in galaga mode, otherwise just a purple ball
					{
						this_player->cash += 100;

						if (!galagaMode)
						{
							handle_got_purple_ball(this_player);
						}
						else
						{
							// spawn the dragonwing?
							if (twoPlayerMode)
								this_player->cash += 2400;
							twoPlayerMode = true;
							twoPlayerLinked = true;
							player[1].items.weapon[REAR_WEAPON].power = 1;
							player[1].armor = 10;
							player[1].is_alive = true;
						}
						enemyAvail[z] = 1;
						soundQueue[7] = S_POWERUP;
					}
					else if (superArcadeMode != SA_NONE && evalue > 30000)
					{
						shotMultiPos[SHOT_FRONT] = 0;
						shotRepeat[SHOT_FRONT] = 10;

						tempW = SAWeapon[superArcadeMode-1][evalue - 30000-1];

						// if picked up already-owned weapon, power weapon up
						if (tempW == player[0].items.weapon[FRONT_WEAPON].id)
						{
							this_player->cash += 1000;
							power_up_weapon(this_player, FRONT_WEAPON);
						}
						// else weapon also gives purple ball
						else
						{
							handle_got_purple_ball(this_player);
						}

						player[0].items.weapon[FRONT_WEAPON].id = tempW;
						this_player->cash += 200;
						soundQueue[7] = S_POWERUP;
						enemyAvail[z] = 1;
					}
					else if (evalue > 32100)
					{
						if (playerNum_ == 1)
						{
							this_player->cash += 250;
							player[0].items.special = evalue - 32100;
							shotMultiPos[SHOT_SPECIAL] = 0;
							shotRepeat[SHOT_SPECIAL] = 10;
							shotMultiPos[SHOT_SPECIAL2] = 0;
							shotRepeat[SHOT_SPECIAL2] = 0;

							if (isNetworkGame)
								snprintf(tempStr, sizeof(tempStr), "%s %s %s", JE_getName(1), miscTextB[4-1], special[evalue - 32100].name);
							else if (twoPlayerMode)
								snprintf(tempStr, sizeof(tempStr), "%s %s", miscText[43-1], special[evalue - 32100].name);
							else
								snprintf(tempStr, sizeof(tempStr), "%s %s", miscText[64-1], special[evalue - 32100].name);
							JE_drawTextWindow(tempStr);
							soundQueue[7] = S_POWERUP;
							enemyAvail[z] = 1;
						}
					}
					else if (evalue > 32000)
					{
						if (playerNum_ == 2)
						{
							enemyAvail[z] = 1;
							if (isNetworkGame)
								snprintf(tempStr, sizeof(tempStr), "%s %s %s", JE_getName(2), miscTextB[4-1], options[evalue - 32000].name);
							else
								snprintf(tempStr, sizeof(tempStr), "%s %s", miscText[44-1], options[evalue - 32000].name);
							JE_drawTextWindow(tempStr);

							// if picked up a different sidekick than player already has, then reset sidekicks to least powerful, else power them up
							if (evalue - 32000u != player[1].items.sidekick_series)
							{
								player[1].items.sidekick_series = evalue - 32000;
								player[1].items.sidekick_level = 101;
							}
							else if (player[1].items.sidekick_level < 103)
							{
								++player[1].items.sidekick_level;
							}

							uint temp = player[1].items.sidekick_level - 100 - 1;
							for (uint i = 0; i < COUNTOF(player[1].items.sidekick); ++i)
								player[1].items.sidekick[i] = optionSelect[player[1].items.sidekick_series][temp][i];

							shotMultiPos[SHOT_LEFT_SIDEKICK] = 0;
							shotMultiPos[SHOT_RIGHT_SIDEKICK] = 0;
							JE_drawOptions();
							soundQueue[7] = S_POWERUP;
						}
						else if (onePlayerAction)
						{
							enemyAvail[z] = 1;
							snprintf(tempStr, sizeof(tempStr), "%s %s", miscText[64-1], options[evalue - 32000].name);
							JE_drawTextWindow(tempStr);

							for (uint i = 0; i < COUNTOF(player[0].items.sidekick); ++i)
								player[0].items.sidekick[i] = evalue - 32000;
							shotMultiPos[SHOT_LEFT_SIDEKICK] = 0;
							shotMultiPos[SHOT_RIGHT_SIDEKICK] = 0;

							JE_drawOptions();
							soundQueue[7] = S_POWERUP;
						}
						if (enemyAvail[z] == 1)
							this_player->cash += 250;
					}
					else if (evalue > 31000)
					{
						this_player->cash += 250;
						if (playerNum_ == 2)
						{
							if (isNetworkGame)
								snprintf(tempStr, sizeof(tempStr), "%s %s %s", JE_getName(2), miscTextB[4-1], weaponPort[evalue - 31000].name);
							else
								snprintf(tempStr, sizeof(tempStr), "%s %s", miscText[44-1], weaponPort[evalue - 31000].name);
							JE_drawTextWindow(tempStr);
							player[1].items.weapon[REAR_WEAPON].id = evalue - 31000;
							shotMultiPos[SHOT_REAR] = 0;
							enemyAvail[z] = 1;
							soundQueue[7] = S_POWERUP;
						}
						else if (onePlayerAction)
						{
							snprintf(tempStr, sizeof(tempStr), "%s %s", miscText[64-1], weaponPort[evalue - 31000].name);
							JE_drawTextWindow(tempStr);
							player[0].items.weapon[REAR_WEAPON].id = evalue - 31000;
							shotMultiPos[SHOT_REAR] = 0;
							enemyAvail[z] = 1;
							soundQueue[7] = S_POWERUP;

							if (player[0].items.weapon[REAR_WEAPON].power == 0)  // does this ever happen?
								player[0].items.weapon[REAR_WEAPON].power = 1;
						}
					}
					else if (evalue > 30000)
					{
						if (playerNum_ == 1 && twoPlayerMode)
						{
							if (isNetworkGame)
								snprintf(tempStr, sizeof(tempStr), "%s %s %s", JE_getName(1), miscTextB[4-1], weaponPort[evalue - 30000].name);
							else
								snprintf(tempStr, sizeof(tempStr), "%s %s", miscText[43-1], weaponPort[evalue - 30000].name);
							JE_drawTextWindow(tempStr);
							player[0].items.weapon[FRONT_WEAPON].id = evalue - 30000;
							shotMultiPos[SHOT_FRONT] = 0;
							enemyAvail[z] = 1;
							soundQueue[7] = S_POWERUP;
						}
						else if (onePlayerAction)
						{
							snprintf(tempStr, sizeof(tempStr), "%s %s", miscText[64-1], weaponPort[evalue - 30000].name);
							JE_drawTextWindow(tempStr);
							player[0].items.weapon[FRONT_WEAPON].id = evalue - 30000;
							shotMultiPos[SHOT_FRONT] = 0;
							enemyAvail[z] = 1;
							soundQueue[7] = S_POWERUP;
						}

						if (enemyAvail[z] == 1)
						{
							player[0].items.special = specialArcadeWeapon[evalue - 30000-1];
							if (player[0].items.special > 0)
							{
								shotMultiPos[SHOT_SPECIAL] = 0;
								shotRepeat[SHOT_SPECIAL] = 0;
								shotMultiPos[SHOT_SPECIAL2] = 0;
								shotRepeat[SHOT_SPECIAL2] = 0;
							}
							this_player->cash += 250;
						}

					}
				}
				else if (evalue > 20000)
				{
					if (twoPlayerLinked)
					{
						// share the armor evenly between linked players
						for (uint i = 0; i < COUNTOF(player); ++i)
						{
							player[i].armor += (evalue - 20000) / COUNTOF(player);
							if (player[i].armor > 28)
								player[i].armor = 28;
						}
					}
					else
					{
						this_player->armor += evalue - 20000;
						// Endless: an armour pickup tops up to the reinforced hull max, not the classic 28.
						const uint armorCap = endlessMode ? this_player->initial_armor : 28;
						if (this_player->armor > armorCap)
							this_player->armor = armorCap;
					}
					enemyAvail[z] = 1;
					VGAScreen = VGAScreenSeg; /* side-effect of game_screen */
					JE_drawArmor();
					VGAScreen = game_screen; /* side-effect of game_screen */
					soundQueue[7] = S_POWERUP;
				}
				else if (evalue > 10000 && enemyAvail[z] == 2)
				{
					if (endlessMode) { enemyAvail[z] = 1; soundQueue[7] = S_POWERUP; endlessGrantSpecial(); }  /* secret orb -> random special in endless (no map warp) */ else if (!bonusLevel)
					{
						play_song(30);  /*Zanac*/
						bonusLevel = true;
						nextLevel = evalue - 10000;
						enemyAvail[z] = 1;

						// Announce the destination secret level (by name) in the bottom
						// HUD message bar, like the "Good luck" text; it auto-erases after
						// a short time (textErase).
						char secretName[16];
						JE_getLevelName(nextLevel, secretName, sizeof(secretName));
						if (secretName[0] != '\0')
							snprintf(tempStr, sizeof(tempStr), "Secret Level: %s", secretName);
						else
							strcpy(tempStr, "Secret Level!");
						JE_drawTextWindow(tempStr);
					}
				}
				else if (enemy[z].scoreitem)
				{
					enemyAvail[z] = 1;
					soundQueue[7] = S_ITEM;
					if (evalue == 1)
					{
						cubeMax++;
						soundQueue[3] = V_DATA_CUBE;
										if (endlessMode)  // datacubes don't accumulate in endless (cube data is inconsistent / crashes)
											{ cubeMax--; soundQueue[3] = S_POWERUP; endlessGrantSpecial(); }  /* datacube -> random special in endless */
					}
					else if (evalue == -1)  // got front weapon powerup
					{
						if (isNetworkGame)
							snprintf(tempStr, sizeof(tempStr), "%s %s %s", JE_getName(1), miscTextB[4-1], miscText[45-1]);
						else if (twoPlayerMode)
							snprintf(tempStr, sizeof(tempStr), "%s %s", miscText[43-1], miscText[45-1]);
						else
							strcpy(tempStr, miscText[45-1]);
						JE_drawTextWindow(tempStr);

						power_up_weapon(&player[0], FRONT_WEAPON);
						soundQueue[7] = S_POWERUP;
					}
					else if (evalue == -2)  // got rear weapon powerup
					{
						if (isNetworkGame)
							snprintf(tempStr, sizeof(tempStr), "%s %s %s", JE_getName(2), miscTextB[4-1], miscText[46-1]);
						else if (twoPlayerMode)
							snprintf(tempStr, sizeof(tempStr), "%s %s", miscText[44-1], miscText[46-1]);
						else
							strcpy(tempStr, miscText[46-1]);
						JE_drawTextWindow(tempStr);

						power_up_weapon(twoPlayerMode ? &player[1] : &player[0], REAR_WEAPON);
						soundQueue[7] = S_POWERUP;
					}
					else if (evalue == -3)
					{
						// picked up orbiting asteroid killer
						shotMultiPos[SHOT_MISC] = 0;
						b = player_shot_create(0, SHOT_MISC, this_player->x, this_player->y, mouseX, mouseY, 104, playerNum_);
						shotAvail[z] = 0;
					}
					else if (evalue == -4)
					{
						if (player[playerNum_-1].superbombs < 10)
							++player[playerNum_-1].superbombs;
					}
					else if (evalue == -5)
					{
						player[0].items.weapon[FRONT_WEAPON].id = 25;  // HOT DOG!
						player[0].items.weapon[REAR_WEAPON].id = 26;
						player[1].items.weapon[REAR_WEAPON].id = 26;

						player[0].last_items = player[0].items;

						for (uint i = 0; i < COUNTOF(player); ++i)
							player[i].weapon_mode = 1;

						memset(shotMultiPos, 0, sizeof(shotMultiPos));
					}
					else if (twoPlayerLinked)
					{
						// players get equal share of pick-up cash when linked
						for (uint i = 0; i < COUNTOF(player); ++i)
							player[i].cash += evalue / COUNTOF(player);
					}
					else
					{
						this_player->cash += evalue;
					}
					JE_setupExplosion(enemy_screen_x, enemy[z].ey, 0, enemyDat[enemy[z].enemytype].explosiontype, true, false);
				}
				else if (this_player->invulnerable_ticks == 0 && enemyAvail[z] == 0 && !noclipMode &&
				         (enemyDat[enemy[z].enemytype].explosiontype & 1) == 0) // explosiontype & 1 == 0: not ground enemy
				{
					int armorleft = enemy[z].armorleft;
					if (armorleft > damageRate)
						armorleft = damageRate;

					int damage_to_enemy = armorleft;

					int playerHit = armorleft;
					if (endlessMode && (endlessActiveMods & ENDLESS_MOD_RAMPAGE))  // Rampage (the brutal Kamikaze): rammers hit ~1.5x harder
					{
						playerHit = armorleft * 3 / 2;
						if (playerHit > 255)
							playerHit = 255;
					}
					JE_playerDamage((JE_byte)playerHit, this_player);

					// player ship gets push-back from collision
					if (enemy[z].armorleft > 0)
					{
						this_player->x_velocity += (enemy[z].exc * enemy[z].armorleft) / 2;
						this_player->y_velocity += (enemy[z].eyc * enemy[z].armorleft) / 2;
					}

					bool has_boss_bar = false;
					for (unsigned int i = 0; i < COUNTOF(boss_bar); i++)
						if (enemy[z].linknum == boss_bar[i].link_num)
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
					int hpMult = endlessMode ? endlessEnemyHpMult(has_boss_bar, bossHpMult, enemy[z].eliteState)
					                         : (has_boss_bar ? bossHpMult : 1);
					if (hpMult > 1)
					{
						enemy[z].damageAccum += damage_to_enemy;
						damage_to_enemy = enemy[z].damageAccum / hpMult;
						enemy[z].damageAccum -= damage_to_enemy * hpMult;
					}

					int armorleft2 = enemy[z].armorleft;
					if (armorleft2 == 255)
						armorleft2 = 30000;

					temp = enemy[z].linknum;
					if (temp == 0)
						temp = 255;

					b = z;

					if (armorleft2 > damage_to_enemy)
					{
						// damage enemy
						if (enemy[z].armorleft != 255)
						{
							if (!enemy[z].healthbar_seen)
							{
								enemy[z].healthbar_seen = true;
								enemy[z].healthbar_max = enemy[z].armorleft;
							}
							enemy[z].armorleft -= damage_to_enemy;
						}
						soundQueue[5] = S_ENEMY_HIT;
					}
					else
					{
						// kill enemy
						for (temp2 = 0; temp2 < 100; temp2++)
						{
							if (enemyAvail[temp2] != 1)
							{
								temp3 = enemy[temp2].linknum;
								if (temp2 == b ||
									(temp != 255 &&
									 (temp == temp3 || temp - 100 == temp3 ||
									  (temp3 > 40 && temp3 / 20 == temp / 20 && temp3 <= temp))))
								{
									int enemy_screen_x = enemy[temp2].ex + enemy[temp2].mapoffset;

									enemy[temp2].linknum = 0;

									enemyAvail[temp2] = 1;

									if (enemyDat[enemy[temp2].enemytype].esize == 1)
									{
										JE_setupExplosionLarge(enemy[temp2].enemyground, enemy[temp2].explonum, enemy_screen_x, enemy[temp2].ey);
										soundQueue[6] = S_EXPLOSION_9;
									}
									else
									{
										JE_setupExplosion(enemy_screen_x, enemy[temp2].ey, 0, 1, false, false);
										soundQueue[5] = S_EXPLOSION_4;
									}
								}
							}
						}
						enemyAvail[z] = 1;
					}
				}
			}

		}
	}
}
