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
#include "menus.h"

#include "config.h"
#include "episodes.h"
#include "font.h"
#include "fonthand.h"
#include "joystick.h"
#include "keyboard.h"
#include "mouse.h"
#include "network.h"
#include "nortsong.h"
#include "opentyr.h"
#include "palette.h"
#include "picload.h"
#include "sprite.h"
#include "vga256d.h"
#include "video.h"

char episode_name[6][31];
char difficulty_name[7][21];
char gameplay_name[GAMEPLAY_NAME_COUNT][26];
char timed_battle_name[4][23];

// Wait for menu input (key, mouse button, or motion); returns true if the mouse moved.
// Polls at 1 ms so the caller's redraw loop — and thus the cursor — can run at the
// display refresh rate.
static bool menuWaitForInput(void)
{
	const Uint16 startMouseX = mouse_x;
	const Uint16 startMouseY = mouse_y;

	for (;;)
	{
		push_joysticks_as_keyboard();
		service_SDL_events(false);

		NETWORK_KEEP_ALIVE();

		const bool mouseMoved = mouse_x != startMouseX || mouse_y != startMouseY;
		if (newkey || newmouse || mouseMoved)
			return mouseMoved;

		SDL_Delay(1);  // brief idle poll; a still cursor doesn't need redrawing
	}
}

bool gameplaySelect(void)
{
	enum MenuItemIndex
	{
		MENU_ITEM_1_PLAYER_FULL_GAME = 0,
		MENU_ITEM_ENDLESS,             // inserted just below the full game
		MENU_ITEM_1_PLAYER_ARCADE,
		MENU_ITEM_1_PLAYER_TIMED_BATTLE,
		MENU_ITEM_2_PLAYER_ARCADE,
		MENU_ITEM_NETWORK,
		MENU_ITEM_COUNT,
	};

	if (shopSpriteSheet.data == NULL)
		JE_loadCompShapes(&shopSpriteSheet, '1');  // need mouse pointer sprites

	bool restart = true;

	const size_t menuItemsCount = MENU_ITEM_COUNT;
	size_t selectedIndex = MENU_ITEM_1_PLAYER_FULL_GAME;

	/* Menus render on a 320px virtual screen centered in the wider VGA buffer; center on
	 * the virtual width, not vga_width, or text drifts when the menu is blitted over. */
	const int xCenter = 320 / 2;
	const int yMenuHeader = 20;
	const int yMenuItems = 54;
	const int dyMenuItems = 24;
	const int hMenuItem = 13;
	int wMenuItem[MENU_ITEM_COUNT] = { 0 };

	for (; ; )
	{
		if (restart)
		{
			JE_loadPic(VGAScreen2, 2, false);

			// Draw header.
			draw_font_hv_shadow(VGAScreen2, xCenter, yMenuHeader, gameplay_name[0], large_font, centered, 15, -3, false, 2);
		}

		// Restore background and header.
		memcpy(VGAScreen->pixels, VGAScreen2->pixels, (size_t)VGAScreen->pitch * VGAScreen->h);

		// Draw menu items.
		for (size_t i = 0; i < menuItemsCount; ++i)
		{
			// "Endless" is inserted after the full-game entry; every other label still comes
			// from gameplay_name[] (full game = [1], the rest map straight across after the
			// insert: Arcade = [2], Timed = [3], 2P = [4], Network = [5]).
			const char *const text = (i == MENU_ITEM_ENDLESS)             ? "1 Player Endless"
			                       : (i == MENU_ITEM_1_PLAYER_FULL_GAME)  ? gameplay_name[1]
			                       :                                        gameplay_name[i];

			wMenuItem[i] = JE_textWidth(text, normal_font);
			const int x = xCenter - wMenuItem[i] / 2;
			const int y = yMenuItems + dyMenuItems * i;

			const bool selected = i == selectedIndex;
			const bool disabled = i == MENU_ITEM_NETWORK;

			draw_font_hv_shadow(VGAScreen, x, y, text, normal_font, left_aligned, 15, -4 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
		}

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
		if (!output_vsync)
			limit_render_fps();  // pace the cursor redraw to the render-fps cap

		const bool mouseMoved = menuWaitForInput();

		// Handle interaction.

		bool action = false;
		bool cancel = false;

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

				cancel = true;
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

				cancel = true;
				break;
			}
			default:
				break;
			}
		}

		if (action)
		{
			switch (selectedIndex)
			{
			case MENU_ITEM_1_PLAYER_FULL_GAME:
			case MENU_ITEM_1_PLAYER_ARCADE:
			case MENU_ITEM_1_PLAYER_TIMED_BATTLE:
			case MENU_ITEM_2_PLAYER_ARCADE:
			{
				JE_playSampleNum(S_SELECT);

				fade_black(10);

				onePlayerAction = selectedIndex == MENU_ITEM_1_PLAYER_ARCADE;
				timedBattleMode = selectedIndex == MENU_ITEM_1_PLAYER_TIMED_BATTLE;
				twoPlayerMode = selectedIndex == MENU_ITEM_2_PLAYER_ARCADE;
				endlessMode = false;
				return true;
			}
			case MENU_ITEM_ENDLESS:
			{
				JE_playSampleNum(S_SELECT);

				fade_black(10);

				onePlayerAction = false;
				timedBattleMode = false;
				twoPlayerMode = false;
				endlessMode = true;  // newGame() runs the endless setup (difficulty + first shop)
				return true;
			}
			case MENU_ITEM_NETWORK:
			{
				JE_playSampleNum(S_SPRING);
				break;
			}
			default:
				break;
			}
		}

		if (cancel)
		{
			fade_black(15);

			return false;
		}
	}
}

bool episodeSelect(void)
{
	if (shopSpriteSheet.data == NULL)
		JE_loadCompShapes(&shopSpriteSheet, '1');  // need mouse pointer sprites

	bool restart = true;

	const size_t menuItemsCount = EPISODE_AVAILABLE;
	size_t selectedIndex = 0;

	const int xCenter = 320 / 2;
	const int yMenuHeader = 20;
	const int yMenuItems = 50;
	const int dyMenuItems = 30;
	const int hMenuItem = 13;
	int wMenuItem[EPISODE_AVAILABLE] = { 0 };

	for (; ; )
	{
		if (restart)
		{
			JE_loadPic(VGAScreen2, 2, false);

			// Draw header.
			draw_font_hv_shadow(VGAScreen2, xCenter, yMenuHeader, episode_name[0], large_font, centered, 15, -3, false, 2);
		}

		// Restore background and header.
		memcpy(VGAScreen->pixels, VGAScreen2->pixels, (size_t)VGAScreen->pitch * VGAScreen->h);

		// Draw menu items.
		for (size_t i = 0; i < menuItemsCount; ++i)
		{
			const char* const text = episode_name[i + 1];

			wMenuItem[i] = JE_textWidth(text, normal_font);
			const int x = xCenter - wMenuItem[i] / 2;
			const int y = yMenuItems + dyMenuItems * i;

			const bool selected = i == selectedIndex;
			const bool disabled = !episodeAvail[i];

			draw_font_hv_shadow(VGAScreen, x, y, text, normal_font, left_aligned, 15, -4 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
		}

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
		if (!output_vsync)
			limit_render_fps();  // pace the cursor redraw to the render-fps cap

		const bool mouseMoved = menuWaitForInput();

		// Handle interaction.

		bool action = false;
		bool cancel = false;

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

				cancel = true;
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

				cancel = true;
				break;
			}
			default:
				break;
			}
		}

		if (action)
		{
			if (episodeAvail[selectedIndex])
			{
				JE_playSampleNum(S_SELECT);

				fade_black(10);

				JE_initEpisode(selectedIndex + 1);
				initial_episode_num = episodeNum;
				return true;
			}
			else
			{
				JE_playSampleNum(S_SPRING);
			}
		}

		if (cancel)
		{
			fade_black(15);

			return false;
		}
	}
}

bool difficultySelect(void)
{
	if (shopSpriteSheet.data == NULL)
		JE_loadCompShapes(&shopSpriteSheet, '1');  // need mouse pointer sprites

	bool restart = true;

	const size_t menuItemsCount = COUNTOF(difficulty_name) - 1;
	// All difficulties are shown up front; the classic hidden-difficulty unlock codes
	// (Shift+G for Impossible, Shift+] for Suicide, and typing L-O-R-D for Lord of Game) are
	// no longer required.
	size_t menuItemsVisibleCount = menuItemsCount;
	size_t selectedIndex = 1;

	/* See gameplaySelect() for rationale behind using a fixed 320px center. */
	const int xCenter = 320 / 2;
	const int yMenuHeader = 20;
	const int yMenuItems = 54;
	const int dyMenuItems = 24;
	const int hMenuItem = 13;
	int wMenuItem[COUNTOF(difficulty_name) - 1] = { 0 };

	for (; ; )
	{
		if (restart)
		{
			JE_loadPic(VGAScreen2, 2, false);

			// Draw header.
			draw_font_hv_shadow(VGAScreen2, xCenter, yMenuHeader, difficulty_name[0], large_font, centered, 15, -3, false, 2);
		}

		// Restore background and header.
		memcpy(VGAScreen->pixels, VGAScreen2->pixels, (size_t)VGAScreen->pitch * VGAScreen->h);

		// Draw menu items.
		for (size_t i = 0; i < menuItemsVisibleCount; ++i)
		{
			const char *const text = difficulty_name[i + 1];

			wMenuItem[i] = JE_textWidth(text, normal_font);
			const int x = xCenter - wMenuItem[i] / 2;
			const int y = yMenuItems + dyMenuItems * i;

			const bool selected = i == selectedIndex;

			draw_font_hv_shadow(VGAScreen, x, y, text, normal_font, left_aligned, 15, -4 + (selected ? 2 : 0), false, 2);
		}

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
		if (!output_vsync)
			limit_render_fps();  // pace the cursor redraw to the render-fps cap

		const bool mouseMoved = menuWaitForInput();

		// Handle interaction.

		bool action = false;
		bool cancel = false;

		if (mouseMoved || newmouse)
		{
			// Find menu item that was hovered or clicked.
			for (size_t i = 0; i < menuItemsVisibleCount; ++i)
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

				cancel = true;
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
					? menuItemsVisibleCount - 1
					: selectedIndex - 1;
				break;
			}
			case SDL_SCANCODE_DOWN:
			{
				JE_playSampleNum(S_CURSOR);

				selectedIndex = selectedIndex == menuItemsVisibleCount - 1
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

				cancel = true;
				break;
			}
			default:
				break;
			}
		}

		if (action)
		{
			JE_playSampleNum(S_SELECT);

			switch (selectedIndex)
			{
			case 0:
				difficultyLevel = DIFFICULTY_EASY;
				break;
			case 1:
				difficultyLevel = DIFFICULTY_NORMAL;
				break;
			case 2:
				difficultyLevel = DIFFICULTY_HARD;
				break;
			case 3:
				difficultyLevel = DIFFICULTY_IMPOSSIBLE;
				break;
			case 4:
				difficultyLevel = DIFFICULTY_SUICIDE;
				break;
			case 5:
				difficultyLevel = DIFFICULTY_LORD_OF_GAME;
				break;
			}

			fade_black(10);

			return true;
		}

		if (cancel)
		{
			fade_black(15);

			return false;
		}
	}
}

bool timedBattleSelect(void)
{
	if (shopSpriteSheet.data == NULL)
		JE_loadCompShapes(&shopSpriteSheet, '1');  // need mouse pointer sprites

	bool restart = true;

	const size_t menuItemsCount = COUNTOF(timed_battle_name) - 1;
	size_t selectedIndex = 0;

	/* See gameplaySelect() for rationale behind using a fixed 320px center. */
	const int xCenter = 320 / 2;
	const int yMenuHeader = 20;
	const int yMenuItems = 54;
	const int dyMenuItems = 24;
	const int hMenuItem = 13;
	int wMenuItem[COUNTOF(timed_battle_name) - 1] = { 0 };

	for (; ; )
	{
		if (restart)
		{
			JE_loadPic(VGAScreen2, 2, false);

			// Draw header.
			draw_font_hv_shadow(VGAScreen2, xCenter, yMenuHeader, timed_battle_name[0], large_font, centered, 15, -3, false, 2);
		}

		// Restore background and header.
		memcpy(VGAScreen->pixels, VGAScreen2->pixels, (size_t)VGAScreen->pitch * VGAScreen->h);

		// Draw menu items.
		for (size_t i = 0; i < menuItemsCount; ++i)
		{
			const char *const text = timed_battle_name[i + 1];

			wMenuItem[i] = JE_textWidth(text, normal_font);
			const int x = xCenter - wMenuItem[i] / 2;
			const int y = yMenuItems + dyMenuItems * i;

			const bool selected = i == selectedIndex;

			draw_font_hv_shadow(VGAScreen, x, y, text, normal_font, left_aligned, 15, -4 + (selected ? 2 : 0), false, 2);
		}

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
		if (!output_vsync)
			limit_render_fps();  // pace the cursor redraw to the render-fps cap

		const bool mouseMoved = menuWaitForInput();

		// Handle interaction.

		bool action = false;
		bool cancel = false;

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

				cancel = true;
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

				cancel = true;
				break;
			}
			default:
				break;
			}
		}

		if (action)
		{
			JE_playSampleNum(S_SELECT);

			fade_black(10);

			// I don't know how T2K determines episode from this,
			// I'm assuming it's hardcoded
			JE_initEpisode(selectedIndex == 0 ? 1 : 5);
			timeBattleSelection = selectedIndex + 1;
			return true;
		}

		if (cancel)
		{
			fade_black(15);

			return false;
		}
	}
}
