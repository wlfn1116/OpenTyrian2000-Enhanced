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
#include "game_menu.h"

#include "backgrnd.h"
#include "config.h"
#include "crashlog.h"
#include "custom_weapon.h"
#include "endless.h"
#include "episodes.h"
#include "file.h"
#include "font.h"
#include "fonthand.h"
#include "joystick.h"
#include "keyboard.h"
#include "loudness.h"
#include "lvllib.h"
#include "mainint.h"
#include "mouse.h"
#include "musmast.h"
#include "network.h"
#include "nortsong.h"
#include "nortvars.h"
#include "params.h"
#include "pcxmast.h"
#include "picload.h"
#include "player.h"
#include "render_list.h"
#include "shots.h"
#include "sprite.h"
#include "tyrian2.h"
#include "varz.h"
#include "vga256d.h"
#include "video.h"

#include <assert.h>
#include <limits.h>
#include <math.h>

enum
{
	MENU_FULL_GAME       =  0,
	MENU_UPGRADES        =  1,
	MENU_OPTIONS         =  2,
	MENU_PLAY_NEXT_LEVEL =  3,
	MENU_UPGRADE_SUB     =  4,
	MENU_KEYBOARD_CONFIG =  5,
	MENU_LOAD_SAVE       =  6,
	MENU_DATA_CUBES      =  7,
	MENU_DATA_CUBE_SUB   =  8,
	MENU_2_PLAYER_ARCADE =  9,
	MENU_1_PLAYER_ARCADE = 10,  // Also networked games.
	MENU_LIMITED_OPTIONS = 11,  // Hides save/load menus.
	MENU_JOYSTICK_CONFIG = 12,
	MENU_SUPER_TYRIAN = 13,
	MENU_MOUSE_CONFIG = 14,  // T2000
	MENU_DEBUG_PLAY_LEVEL = 15,
	MENU_ESHOP = 16,  // endless "E-Shop": reroll / reinforce / buy buff / buy special
	MENU_PERKS = 17,  // endless perk pick: forced 1-of-3 (+ decline) gate before the buy/sell front menu
};

// Horizontal centre of the monitor window's readout slot (the panel under the map, y173). Both the
// shop cash total and the endless course RANK are centred on this x (value 77). The slot is
// asymmetric -- the corner bulb eats its right end -- so this is the window centre, not the slot
// midpoint. See JE_drawEndlessCourseMods (ENDLESS_RANK_CX) and notes.md §Menus & shop.
#define MENU_MONITOR_CENTER_X 77

/*** Structs ***/
struct cube_struct
{
	char title[81];
	char header[13];
	int face_sprite;
	char text[90][36];
	int last_line;
};

/*** Globals ***/
static int joystick_config = 0; // which joystick is being configured in menu

static JE_word yLoc;
static JE_shortint yChg;
static int newPal, curPal, oldPal;
static JE_boolean quikSave;
static JE_byte oldMenu;
static JE_boolean backFromHelp;
static JE_integer lastDirection;
static JE_boolean firstMenu9, paletteChanged;
static JE_MenuChoiceType menuChoices;
static JE_integer col, colC;
static JE_byte lastCurSel;
static JE_integer curMenu;
static JE_byte curSel[MENU_MAX]; /* [1..maxmenu] */
static JE_byte curItemType, curItem, cursor;
static JE_boolean leftPower, rightPower, rightPowerAfford;
static JE_byte currentCube;

// Endless: MENU_PERKS doubles as the forced perk PICK and (with this flag set) a read-only perk
// LIST; the flag keeps the two uses' dispatch / Esc / help / draw behaviour apart.
static bool endlessPerkListMode = false;
static int  perkListId[32];  // perk-list display row (curSel-2) -> perk id; -1 = "(none yet)" sentinel. Sized well above PERK_COUNT.

// Vertical scroll for the endless read-only Perks list (MENU_PERKS in list mode) when the run's
// collection outgrows one page; mirrors the buy/sell sub-list scroll (upgradeSubScrollTop) below.
static int perkListScrollTop = 1;   // 1-based first visible row (tempW); the window follows the selection
static int perkListPrevSel = 0;     // curSel last frame; the view re-follows only when it changes
#define PERK_LIST_VIS 14            // rows visible per page before scrolling (14 = the most that fit above the cash bar)

static JE_boolean keyboardUsed;
static JE_byte weaponSimTime;

// Vertical scroll for the buy/sell sub-item list (MENU_UPGRADE_SUB) when it overflows:
// the 1-based first visible row; the window follows the selection.
static int upgradeSubScrollTop = 1;
static int upgradeSubPrevSel = 0;    // curSel last frame; the view follows only when it changes

// Flush the current joystick's bindings to opentyrian.cfg right away: the Switch HOME-menu
// exit never runs JE_tyrianHalt's config write (notes.md §Console ports).
static void save_joystick_config_now(void)
{
	if (joystick_config >= 0 && joystick_config < joysticks)
		save_joystick_assignments(&opentyrian_config, joystick_config);
	save_opentyrian_config();
}

// Visible rows in the buy/sell sub-list. The shop frame fits 6 rows (row N at
// y = 40 + (N-1)*26). Overflow shows 5 so the trailing "None" and "DONE" rows fall
// below the fold together, rather than "None" dangling alone.
static int upgradeSubVisibleRows(void)
{
	const int total_rows = menuChoices[MENU_UPGRADE_SUB] - 1;  // items (incl None) + DONE
	return total_rows <= 6 ? 6 : 5;
}

static JE_byte planetAni, planetAniWait;
static JE_byte currentDotNum, currentDotWait;
static JE_real navX, navY, newNavX, newNavY;
static JE_integer tempNavX, tempNavY;
static JE_byte planetDots[5]; /* [1..5] */
static JE_integer planetDotX[5][10], planetDotY[5][10]; /* [1..5, 1..10] */

// Where the planet monitor was last shown (nav pan start) for display-rate
// interpolation of the lock-on scroll. See JE_navScreenSmoothPresent.
static JE_real nav_smooth_prev_x, nav_smooth_prev_y;
static void JE_drawNavMonitor(void);
static void JE_navScreenAdvance(void);
static void JE_navDrawFrame(JE_real dispNavX, JE_real dispNavY);
static void JE_navScreenSmoothPresent(void);
static PlayerItems old_items[2];  // TODO: should not be global if possible

static struct cube_struct cube[4];

/* Debug level menu data */
#define DEBUG_LEVEL_MAX 200
static JE_word debugMapSection[DEBUG_LEVEL_MAX];
static JE_byte debugLvlFileNum[DEBUG_LEVEL_MAX];
static char debugLevelName[DEBUG_LEVEL_MAX][18];
static uint debugLevelCount;
static bool debugPlayMenu;

#define DEBUG_MENU_MAX (50 + 2)
static char debugMenuInt[DEBUG_MENU_MAX][18];

static const JE_MenuChoiceType menuChoicesDefault = { 9, 9, 9, 0, 0, 11, (SAVE_FILES_NUM / 2) + 2, 0, 0, 6, 4, 6, 7, 5, 6, 0, 7, 5 };  // [16]=E-Shop: 5 buys + Done; [17]=Perks: 3 + decline (set at runtime)
static const JE_byte menuEsc[MENU_MAX] = { 0, 1, 1, 1, 2, 3, 3, 1, 8, 0, 0, 11, 3, 0, 2, 1, 1, 1 };  // [16]=E-Shop, [17]=Perks -> back to buy/sell (MENU_FULL_GAME); Perks Esc is special-cased to "take the cash"
static const JE_byte itemAvailMap[7] = { 1, 2, 3, 9, 4, 6, 7 };
static const JE_word planetX[21] = { 200, 150, 240, 300, 270, 280, 320, 260, 220, 150, 160, 210, 80, 240, 220, 180, 310, 330, 150, 240, 200 };
static const JE_word planetY[21] = {  40,  90,  90,  80, 170,  30,  50, 130, 120, 150, 220, 200, 80,  50, 160,  10,  55,  55,  90,  90,  40 };
static const uint cube_line_chars = sizeof(*cube->text) - 1;
static const uint cube_line_width = 150;

/*** Functions ***/
static Uint8 *playeritem_map(PlayerItems *items, uint i)
{
	Uint8 *const map[] =
	{
		&items->ship,
		&items->weapon[FRONT_WEAPON].id,
		&items->weapon[REAR_WEAPON].id,
		&items->shield,
		&items->generator,
		&items->sidekick[LEFT_SIDEKICK],
		&items->sidekick[RIGHT_SIDEKICK],
	};
	assert(i < COUNTOF(map));
	return map[i];
}

static void ensure_equipped_items_visible(void)
{
	/* Add currently equipped items to the shop inventory if they are not
	 * already present. This mirrors the initial setup performed when the
	 * buy/sell screen is entered and is required when equipment is changed
	 * via the debug menu. */
	for (int i = 0; i < 7; i++)
	{
		int item = *playeritem_map(&player[0].items, i);

		int slot = 0;
		for (; slot < itemAvailMax[itemAvailMap[i] - 1]; ++slot)
		{
			if (itemAvail[itemAvailMap[i] - 1][slot] == item)
				break;
		}

		if (slot == itemAvailMax[itemAvailMap[i] - 1])
		{
			itemAvail[itemAvailMap[i] - 1][slot] = item;
			itemAvailMax[itemAvailMap[i] - 1]++;
		}
	}
}

/* Parse the ]L / * entries of an episode's level file into the debug-level arrays,
 * using JE_loadMap's offsets. Opens non-fatally: an absent episode yields an empty list. */
static void load_debug_levels(int episode)
{
	debugLevelCount = 0;
	const unsigned int levelFileCount = JE_levelFileCount(episode);
	if (levelFileCount == 0)
		return;

	char fname[16];
	snprintf(fname, sizeof(fname), "levels%d.dat", episode);
	FILE* f = dir_fopen_warn(data_dir(), fname, "rb");
	if (f == NULL)
		return;

	JE_word section = 0;
	long end = ftell_eof(f);
	char s[256];
	while (ftell(f) < end && debugLevelCount < COUNTOF(debugMapSection))
	{
		read_encrypted_pascal_string(s, sizeof(s), f);

		if (s[0] == '*')
		{
			section++;
		}
		if (s[0] == ']' && s[1] == 'L')
		{
			const int fileNum = atoi(s + 25);
			if (fileNum < 1 || (unsigned int)fileNum > levelFileCount)
			{
				fprintf(stderr, "warning: episode %d section %u references missing level file %d\n",
				        episode, (unsigned int)section, fileNum);
				continue;
			}

			debugMapSection[debugLevelCount] = section;

			char name_buf[10];
			SDL_strlcpy(name_buf, s + 13, sizeof(name_buf));
			size_t len = strlen(name_buf);
			while (len > 0 && name_buf[len - 1] == ' ')
				name_buf[--len] = '\0';
			SDL_strlcpy(debugLevelName[debugLevelCount], name_buf,
				sizeof(debugLevelName[0]));

			debugLvlFileNum[debugLevelCount] = (JE_byte)fileNum;
			debugLevelCount++;
		}
	}

	fclose(f);
}

uint JE_getLevelSections(int episode, JE_byte *out, JE_byte *fileOut, uint maxOut)
{
	const unsigned int levelFileCount = JE_levelFileCount(episode);
	if (levelFileCount == 0)
		return 0;

	char fname[16];
	snprintf(fname, sizeof(fname), "levels%d.dat", episode);
	FILE *f = dir_fopen_warn(data_dir(), fname, "rb");
	if (f == NULL)
		return 0;

	JE_word section = 0;
	bool sectionUnsafe = false;  // a mode-switch command seen since the last '*'
	long end = ftell_eof(f);
	char s[256];
	uint n = 0;

	while (ftell(f) < end && n < maxOut)
	{
		read_encrypted_pascal_string(s, sizeof(s), f);

		if (s[0] == '*')
		{
			section++;
			sectionUnsafe = false;
		}
		else if (s[0] == ']')
		{
			// Section commands that switch the game into a special mode (ENGAGE,
			// Galaga 2-player, 2-player jump). A level carrying one of these isn't a
			// plain single-player level, so keep it out of the endless pool.
			if (s[1] == 'e' || s[1] == 'g' || s[1] == '2')
				sectionUnsafe = true;
			else if (s[1] == 'L')
			{
				const int fileNum = atoi(s + 25);
				if (fileNum < 1 || (unsigned int)fileNum > levelFileCount)
				{
					fprintf(stderr, "warning: episode %d section %u references missing level file %d\n",
					        episode, (unsigned int)section, fileNum);
					continue;
				}

				// Strip the level name (padded with spaces) for a name blocklist too.
				char name[10];
				SDL_strlcpy(name, s + 13, sizeof(name));
				size_t len = strlen(name);
				while (len > 0 && name[len - 1] == ' ')
					name[--len] = '\0';

				const bool special = sectionUnsafe
					|| SDL_strcasecmp(name, "ALE") == 0
					|| SDL_strcasecmp(name, "TIME WAR") == 0
					|| SDL_strcasecmp(name, "SQUADRON") == 0
					// Episode 5 levels 5-7 are an unfinished copy of a SAVARA level with no
					// proper ending, so they can never be cleared -- keep them out too.
					|| (episode == 5 && section >= 5 && section <= 7);

				if (!special)
				{
					// Each ']L' is a distinct pool entry (section, lvlFileNum) -- a section can
					// carry two level files; see JE_getLevelSections's doc in game_menu.h.
					if (fileOut != NULL)
						fileOut[n] = (JE_byte)fileNum;
					out[n++] = (JE_byte)section;
				}
			}
		}
	}

	fclose(f);
	return n;
}

// Look up the authored name of the level at (episode, section, fileNum) -- the same 9-char name
// (offset 13, space-padded) JE_getLevelSections reads for its blocklist -- without loading the
// level. fileNum 0 matches the section's first ']L'; a non-zero fileNum picks a specific cut
// (Episode 1 section 3's two TYRIAN files). `out` is set to "" if no matching entry is found.
// Used by the endless Radar perk to name each charted course's base level. (Distinct from
// mainint.c's static JE_getLevelName, which resolves routing sections in the current episode.)
void JE_getLevelSectionName(int episode, JE_byte section, JE_byte fileNum, char *out, size_t outSize)
{
	if (out == NULL || outSize == 0)
		return;
	out[0] = '\0';

	const unsigned int levelFileCount = JE_levelFileCount(episode);
	if (levelFileCount == 0)
		return;

	char fname[16];
	snprintf(fname, sizeof(fname), "levels%d.dat", episode);
	FILE *f = dir_fopen_warn(data_dir(), fname, "rb");
	if (f == NULL)
		return;

	JE_word sec = 0;
	long end = ftell_eof(f);
	char s[256];

	while (ftell(f) < end)
	{
		read_encrypted_pascal_string(s, sizeof(s), f);

		if (s[0] == '*')
		{
			sec++;
		}
		else if (s[0] == ']' && s[1] == 'L')
		{
			const int fn = atoi(s + 25);
			if (fn < 1 || (unsigned int)fn > levelFileCount)
				continue;
			if (sec != section || (fileNum != 0 && fn != (int)fileNum))
				continue;

			// The name is a fixed 9-char field (offset 13), space-padded, with the file number
			// following at offset 25 -- so copy exactly 9 chars first, never the whole tail.
			char name[10];
			SDL_strlcpy(name, s + 13, sizeof(name));
			size_t len = strlen(name);
			while (len > 0 && name[len - 1] == ' ')
				name[--len] = '\0';
			SDL_strlcpy(out, name, outSize);
			break;
		}
	}

	fclose(f);
}

JE_longint JE_cashLeft(void)
{
	JE_longint tempL = player[0].cash;

	// Only the seven real item rows (curSel 2..8) map to a player item and have a price.
	// Action rows like "Custom" and "Done" don't — mapping them would index playeritem_map
	// (a 7-entry table) out of bounds, which crashes when the Custom row is highlighted.
	if (curSel[MENU_UPGRADES] < 2 || curSel[MENU_UPGRADES] > 8)
		return tempL;

	JE_word itemNum = *playeritem_map(&player[0].items, curSel[MENU_UPGRADES] - 2);

	tempL -= JE_getCost(curSel[MENU_UPGRADES], itemNum);

	tempW = 0;

	switch (curSel[MENU_UPGRADES])
	{
	case 3:
	case 4:
	{
		long base_cost = weaponPort[itemNum].cost;
		if (expertMode)
		{
			// matches the power-upgrade scaling in JE_getCost
			if (base_cost > LONG_MAX / expertUpgradeCostMult)
				base_cost = LONG_MAX;
			else
				base_cost = base_cost * expertUpgradeCostMult;
		}
		for (uint i = 1; i < player[0].items.weapon[curSel[MENU_UPGRADES] - 3].power; ++i)
		{
			long step_cost = weapon_upgrade_cost(base_cost, i);
			tempL -= step_cost;
		}
		break;
	}
	}

	return tempL;
}

// ---- Generator power gauge (upgrade screen) ---------------------------------
// Raw pixels (not a blit), updated once per logic tick. Each tick the clean interface
// background under the bar is snapshotted, so JE_weaponSimSmoothPresent can redraw
// it at an interpolated level every presented frame.
enum { PB_X0 = 141, PB_X1 = 149, PB_Y0 = 56, PB_Y1 = 146 };  // gauge slot (inclusive)

static Uint8 menu_power_bg[PB_Y1 - PB_Y0 + 1][PB_X1 - PB_X0 + 1];
static bool menu_power_bg_valid = false;
static int menu_power_prev = 500, menu_power_cur = 500;  // gauge value (0..900) at prev/cur tick

static void menu_power_capture_bg(SDL_Surface *s)
{
	const Uint8 *base = (const Uint8 *)s->pixels + PB_Y0 * s->pitch + PB_X0;
	for (int y = 0; y <= PB_Y1 - PB_Y0; y++)
		memcpy(menu_power_bg[y], base + y * s->pitch, PB_X1 - PB_X0 + 1);
	menu_power_bg_valid = true;
}

// Draw the generator gauge at the given level (0..900), restoring the captured
// background first so redrawing at a lower level leaves no stub.
static void draw_menu_power_bar(SDL_Surface *s, int power_value)
{
	if (power_value > 900)
		power_value = 900;
	else if (power_value < 0)
		power_value = 0;

	if (menu_power_bg_valid)  // erase the previous bar back to the clean interface
	{
		Uint8 *base = (Uint8 *)s->pixels + PB_Y0 * s->pitch + PB_X0;
		for (int y = 0; y <= PB_Y1 - PB_Y0; y++)
			memcpy(base + y * s->pitch, menu_power_bg[y], PB_X1 - PB_X0 + 1);
	}

	// Left/Right generator gradient: mirror the in-game horizontal ramp (shades 115..123).
	// Only the shading follows the setting; the preview keeps its own gauge position and size.
	if (gaugeGradGenerator == GAUGE_GRAD_LEFT || gaugeGradGenerator == GAUGE_GRAD_RIGHT)
	{
		// Squished 2px: at full power the bar top sits at PB_Y0+2 (2px lower than the vertical
		// modes), compressing the whole ramp into a 2px-shorter span; bottom stays at PB_Y1.
		const int barTop = PB_Y1 - power_value * (PB_Y1 - PB_Y0 - 2) / 900;
		for (int j = 0; j <= PB_X1 - PB_X0; j++)
		{
			const int off = (gaugeGradGenerator == GAUGE_GRAD_RIGHT) ? j : (PB_X1 - PB_X0 - j);
			fill_rectangle_xy(s, PB_X0 + j, barTop, PB_X0 + j, PB_Y1, (Uint8)(113 + 2 + off));
		}
		return;
	}

	int temp, temp2, temp3;

	for (temp = 147 - power_value / 10; temp <= 146; temp++)
	{
		temp2 = 113 + (146 - temp) / 9 + 2;
		temp3 = (temp + 1) % 6;
		if (temp3 == 1)
			temp2 += 3;
		else if (temp3 != 0)
			temp2 += 2;

		JE_pix(s, 141, temp, temp2 - 3);
		JE_pix(s, 142, temp, temp2 - 3);
		JE_pix(s, 143, temp, temp2 - 2);
		JE_pix(s, 144, temp, temp2 - 1);
		fill_rectangle_xy(s, 145, temp, 149, temp, temp2);
	}

	// Bright leading edge one row above the bar top.
	temp = 147 - power_value / 10;
	temp2 = 113 + (146 - temp) / 9 + 4;

	JE_pix(s, 141, temp - 1, temp2 - 1);
	JE_pix(s, 142, temp - 1, temp2 - 1);
	JE_pix(s, 143, temp - 1, temp2 - 1);
	JE_pix(s, 144, temp - 1, temp2 - 1);
	fill_rectangle_xy(s, 145, temp - 1, 149, temp - 1, temp2);
}

// Smooth weapon-sim preview: replay the recorded frame at interpolated positions, copying only
// the preview box. weaponSimOverlayFn (NULL except in the custom weapon creator) draws over the
// finished box each present, receiving the interpolation alpha so overlays glide. notes.md §Menus & shop.
static void (*weaponSimOverlayFn)(float alpha) = NULL;

// The non-blit overlay (power gauge, segment bars, cost/cash text) comes from the
// captured residual, since the render list records only blits.
// Caller must already have done, with VGAScreen == VGAScreenSeg:
//   rl_begin_record(); <draw frame>; rl_end_record(); rl_finalize();
//   rl_capture_residual(VGAScreenSeg, game_screen);
static void JE_weaponSimSmoothPresent(void)
{
	enum { RX0 = 8, RY0 = 8, RX1 = 143, RY1 = 182 };  // preview region (inclusive)

	// Smooth Motion off: present the already-drawn authoritative frame once and let
	// menuWaitWithSmoothCursor() wait out the tick — classic, non-interpolated.
	if (!smoothMotion)
	{
		if (weaponSimOverlayFn) weaponSimOverlayFn(1.0f);
		JE_mouseStart();
		JE_showVGA();
		JE_mouseReplace();
		return;
	}

	const float period = 3.0f * get_delay_period();  // setDelay(3) duration (ms)

	// Present interpolated frames for the whole tick, paced by the menu's own setDelay()
	// timer; alpha sweeps 0..1 and the wait_delay() after us becomes a no-op.
	for (;;)
	{
		const Uint32 remaining = getDelayTicks();  // ms until the setDelay(3) target
		if (remaining == 0 || period <= 0.0f)
			break;

		float alpha = 1.0f - (float)remaining / period;
		if (alpha < 0.0f)
			alpha = 0.0f;
		else if (alpha > 1.0f)
			alpha = 1.0f;

		// Reconstruct the recorded frame at interpolated positions into game_screen,
		// then copy just the preview region over the live menu surface.
		rl_replay_interp(game_screen, alpha, false, 1);  // menu preview stays 1x

		const Uint8 *src = (const Uint8 *)game_screen->pixels + RY0 * game_screen->pitch + RX0;
		Uint8 *dst = (Uint8 *)VGAScreenSeg->pixels + RY0 * VGAScreenSeg->pitch + RX0;
		for (int row = RY0; row <= RY1; ++row)
		{
			memcpy(dst, src, RX1 - RX0 + 1);
			src += game_screen->pitch;
			dst += VGAScreenSeg->pitch;
		}

		// The gauge sits partly outside the copied region and is a non-blit overlay,
		// so redraw it here at the interpolated level.
		if (menu_power_bg_valid)
		{
			float level = (float)menu_power_prev + ((float)menu_power_cur - (float)menu_power_prev) * alpha;
			draw_menu_power_bar(VGAScreenSeg, (int)(level + 0.5f));
		}

		if (weaponSimOverlayFn) weaponSimOverlayFn(alpha);
		JE_mouseStart();
		JE_showVGA();
		JE_mouseReplace();

		if (!output_vsync)
			limit_render_fps();
		service_SDL_events(false);
	}
}

// Wait out the rest of a menu tick while re-presenting at the display rate, so the
// cursor glides instead of stepping per tick. (A no-op after the weapon sim and nav
// screen, whose present loops already consumed the tick.)
static void menuWaitWithSmoothCursor(void)
{
	for (;;)
	{
		if (getDelayTicks() == 0)
			break;
		JE_mouseStart();   // services SDL events + draws the cursor at its live pos
		JE_showVGA();
		JE_mouseReplace(); // restore the pixels under the cursor for the next pass
		if (!output_vsync)
			limit_render_fps();
	}
}

/* Configure the buy/sell (MENU_FULL_GAME) menu for Debug Mode:
 *   on:  insert "Debug Menu" and "Debug Level" before "Quit Game" (item 7 -> 9)
 *   off: the original 7-item layout
 * The stock quit label is captured once to restore it exactly. */
static void configure_buysell_debug_menu(void)
{
	static char quitLabel[sizeof(menuInt[1][6])];
	static bool quitLabelSaved = false;

	if (!quitLabelSaved)
	{
		// menuInt[1][6] holds the stock "Quit Game" entry as loaded from the
		// game's menu text (item 7 in the unmodified buy/sell menu).
		SDL_strlcpy(quitLabel, menuInt[1][6], sizeof(quitLabel));
		quitLabelSaved = true;
	}

	if (debugMode)
	{
		menuChoices[MENU_FULL_GAME] = 9;
		SDL_strlcpy(menuInt[1][6], "Debug Menu", sizeof(menuInt[1][6]));
		SDL_strlcpy(menuInt[1][7], "Debug Level", sizeof(menuInt[1][7]));
		SDL_strlcpy(menuInt[1][8], quitLabel, sizeof(menuInt[1][8]));
	}
	else
	{
		menuChoices[MENU_FULL_GAME] = 7;
		SDL_strlcpy(menuInt[1][6], quitLabel, sizeof(menuInt[1][6]));
	}
}

/* Endless swaps the shop's front-menu items 2/3 (Data Cubes -> E-Shop, Ship Specs -> Perks) and
 * captures the stock labels so a campaign shop restores them. The E-Shop labels carry live prices,
 * so this is re-called after each buy. notes.md §Menus & shop. */
static void configure_endless_shop_menu(void)
{
	static char stockCubes[sizeof(menuInt[1][1])];
	static char stockSpecs[sizeof(menuInt[1][2])];
	static bool stockSaved = false;

	if (!stockSaved)
	{
		SDL_strlcpy(stockCubes, menuInt[1][1], sizeof(stockCubes));
		SDL_strlcpy(stockSpecs, menuInt[1][2], sizeof(stockSpecs));
		stockSaved = true;
	}

	if (endlessMode)
	{
		// Front buy/sell menu: item 2 opens the E-Shop; item 3 opens the read-only Perks list
		// (endless has no use for Ship Specs -- your run-persistent perks matter more).
		SDL_strlcpy(menuInt[1][1], "E-Shop", sizeof(menuInt[1][1]));
		SDL_strlcpy(menuInt[1][2], "Perks", sizeof(menuInt[1][2]));

		// E-Shop submenu (menuInt row MENU_ESHOP+1): title, 11 buys, Done. Names only -- the
		// exact cost of each buy is shown in the help line at the bottom (see the help block).
		char (*e)[24] = menuInt[MENU_ESHOP + 1];
		SDL_strlcpy(e[0], "Endless Shop", sizeof(e[0]));
		// Grouped by category so same-colour rows sit together (see endless_eshop_row_bank):
		// neutral shop actions (Reroll, Sabotage), upgrades (Reinforce, Extra Perk), the weapon,
		// the three kill-fire buffs, the consumables (Revive, Bomb), then Gamble last before Done.
		SDL_strlcpy(e[1], "Buy Shop Reroll", sizeof(e[1]));
		SDL_strlcpy(e[2], "Buy Sector Sabotage", sizeof(e[2]));
		SDL_strlcpy(e[3], endlessHullMaxed() ? "Hull Maxed" : "Buy Reinforce", sizeof(e[3]));
		SDL_strlcpy(e[4], "Buy Extra Perk", sizeof(e[4]));
		SDL_strlcpy(e[5], "Buy Special Weapon", sizeof(e[5]));
		SDL_strlcpy(e[6], endlessBuffKindBought() == ENDLESS_BUFF_KIND_TURBODRIVE ? "Turbodrive ON" : "Buy Turbodrive", sizeof(e[6]));
		SDL_strlcpy(e[7], endlessBuffKindBought() == ENDLESS_BUFF_KIND_OVERBLAST ? "Overblast ON" : "Buy Overblast", sizeof(e[7]));
		SDL_strlcpy(e[8], endlessBuffKindBought() == ENDLESS_BUFF_KIND_OVERDRIVE ? "Overdrive ON" : "Buy Overdrive", sizeof(e[8]));
		SDL_strlcpy(e[9], endlessReviveArmed() ? "Revive Ready" : "Buy Revive", sizeof(e[9]));
		SDL_strlcpy(e[10], endlessBombFull() ? "Bombs Full" : "Buy Bomb", sizeof(e[10]));
		SDL_strlcpy(e[11], "Buy Gamble", sizeof(e[11]));
		SDL_strlcpy(e[12], "Done", sizeof(e[12]));
		menuChoices[MENU_ESHOP] = 13;
	}
	else
	{
		SDL_strlcpy(menuInt[1][1], stockCubes, sizeof(menuInt[1][1]));
		SDL_strlcpy(menuInt[1][2], stockSpecs, sizeof(menuInt[1][2]));
	}
}

/* Populate the endless perk-pick menu (MENU_PERKS) from the offers rolled this visit
 * (endlessGeneratePerkChoices). Rows: title, up to 3 perk names, then "Take the Cash".
 * The exact perk effect + owned count shows in the help line; the decline cash is there too. */
static void configure_endless_perk_menu(void)
{
	char (*p)[24] = menuInt[MENU_PERKS + 1];
	const int n = endlessPerkChoiceCount();

	SDL_strlcpy(p[0], "Choose a Perk", sizeof(p[0]));
	for (int i = 0; i < n; ++i)
		SDL_strlcpy(p[i + 1], endlessPerkChoiceName(i), sizeof(p[i + 1]));
	SDL_strlcpy(p[n + 1], "Take the Cash", sizeof(p[n + 1]));

	menuChoices[MENU_PERKS] = n + 2;  // rows 2..(n+1) = perks; row (n+2) = decline
}

/* Populate MENU_PERKS as a read-only list of the perks owned this run (the "Perks" front-menu entry
 * that replaces Ship Specs in endless). Rows render from perkListId[], not menuInt, so the list can
 * hold every owned perk and scroll. notes.md §Menus & shop. */
static void configure_endless_perk_list_menu(void)
{
	SDL_strlcpy(menuInt[MENU_PERKS + 1][0], "Perks", sizeof(menuInt[MENU_PERKS + 1][0]));  // title, drawn by JE_drawMenuHeader

	const int total = endlessPerkCount();
	const int cap   = (int)(sizeof(perkListId) / sizeof(perkListId[0]));
	int n = 0;
	for (int id = 0; id < total && n < cap; ++id)
		if (endlessPerkGetOwned(id) > 0)
			perkListId[n++] = id;

	if (n == 0)              // nothing earned yet: a single "(none yet)" info row
		perkListId[n++] = -1;

	menuChoices[MENU_PERKS] = n + 2;  // rows 2..(n+1) = perk/info lines; row (n+2) = Done

	perkListScrollTop = 1;  // open at the top of the list
	perkListPrevSel   = 0;  // force the window to re-follow the selection on the first draw
}

/* Render the endless read-only Perks list, scrolling when it outgrows a page: a window of
 * PERK_LIST_VIS rows follows curSel across the whole list. Rows come from perkListId[], not menuInt.
 * Called from the menu draw dispatch in place of JE_drawMenuChoices. */
static void draw_endless_perk_list(void)
{
	const int total_rows = menuChoices[MENU_PERKS] - 1;   // perk/info rows + Done; tempW = 1..total_rows
	const int sel_row    = curSel[MENU_PERKS] - 1;        // tempW of the selected row
	const int visible    = PERK_LIST_VIS;

	int max_top = total_rows - visible + 1;
	if (max_top < 1)
		max_top = 1;
	if (perkListScrollTop > max_top)
		perkListScrollTop = max_top;
	if (perkListScrollTop < 1)
		perkListScrollTop = 1;
	// Follow the selection only when it just moved (keyboard / mouse), keeping the selected row visible.
	if (curSel[MENU_PERKS] != perkListPrevSel)
	{
		if (sel_row < perkListScrollTop)
			perkListScrollTop = sel_row;
		else if (sel_row > perkListScrollTop + visible - 1)
			perkListScrollTop = sel_row - visible + 1;
		perkListPrevSel = curSel[MENU_PERKS];
	}

	for (int tempW = 1; tempW <= total_rows; ++tempW)
	{
		if (tempW < perkListScrollTop || tempW >= perkListScrollTop + visible)
			continue;  // scrolled out of view
		const int tempY = 38 + (tempW - perkListScrollTop) * 10;

		const bool doneRow = (tempW == total_rows);
		const int  id      = doneRow ? -1 : perkListId[tempW - 1];  // -1 also flags the "(none yet)" info row

		char rowStr[32];
		if (doneRow)
			SDL_strlcpy(rowStr, "Done", sizeof(rowStr));
		else
			SDL_strlcpy(rowStr, id >= 0 ? endlessPerkName(id) : "(none yet)", sizeof(rowStr));

		const bool selected = (tempW == sel_row);

		char line[34];
		if (selected)  // leading '~' toggles the highlight, as in JE_drawMenuChoices
		{
			line[0] = '~';
			SDL_strlcpy(line + 1, rowStr, sizeof(line) - 1);
		}
		else
			SDL_strlcpy(line, rowStr, sizeof(line));

		JE_outTextAndDarken(VGAScreen, 166, tempY, line, 15, 2, TINY_FONT);

		// Owned/max stack count (e.g. "2/5"), right-aligned just left of the scroll-bar column.
		if (id >= 0)
		{
			char cnt[16];
			snprintf(cnt, sizeof(cnt), "%d/%d", endlessPerkGetOwned(id), endlessPerkMaxStack(id));

			char cline[18];
			if (selected)  // match the row's highlight so the count brightens with its name
			{
				cline[0] = '~';
				SDL_strlcpy(cline + 1, cnt, sizeof(cline) - 1);
			}
			else
				SDL_strlcpy(cline, cnt, sizeof(cline));

			const int count_right = 302;  // a few px left of the scroll-bar track (x=306)
			JE_outTextAndDarken(VGAScreen, count_right - JE_textWidth(cnt, TINY_FONT), tempY, cline, 15, 2, TINY_FONT);
		}
	}

	// Scroll bar (track + thumb), shown only when the list overflows one page.
	if (total_rows > visible)
	{
		const int track_x  = 306;
		const int track_y0 = 38;
		const int track_y1 = 38 + visible * 10 - 4;
		const int track_h  = track_y1 - track_y0;
		int thumb_h = track_h * visible / total_rows;
		if (thumb_h < 6)
			thumb_h = 6;
		const int thumb_y = track_y0 + (track_h - thumb_h) * (perkListScrollTop - 1) / (max_top - 1);
		fill_rectangle_xy(VGAScreen, track_x, track_y0, track_x + 2, track_y1, 241);
		fill_rectangle_xy(VGAScreen, track_x, thumb_y, track_x + 2, thumb_y + thumb_h, 252);
	}
}

/* Insert "Debug Menu" / "Debug Level" before an arcade-style mode menu's Quit item
 * when Debug Mode is on (mirrors configure_buysell_debug_menu). Stock Quit label
 * captured once per menu.  menuIntIdx: the menuInt[] row (= curMenu + 1).
 * defaultChoices: normal item count, Quit being its last item. */
static void configure_mode_debug_menu(int menu, int menuIntIdx, int defaultChoices,
                                      char *savedQuit, bool *quitSaved)
{
	const size_t entrySize = sizeof(menuInt[0][0]);

	if (!*quitSaved)
	{
		SDL_strlcpy(savedQuit, menuInt[menuIntIdx][defaultChoices - 1], entrySize);
		*quitSaved = true;
	}

	if (debugMode)
	{
		menuChoices[menu] = defaultChoices + 2;
		SDL_strlcpy(menuInt[menuIntIdx][defaultChoices - 1], "Debug Menu",  entrySize);
		SDL_strlcpy(menuInt[menuIntIdx][defaultChoices],     "Debug Level", entrySize);
		SDL_strlcpy(menuInt[menuIntIdx][defaultChoices + 1], savedQuit,     entrySize);
	}
	else
	{
		menuChoices[menu] = defaultChoices;
		SDL_strlcpy(menuInt[menuIntIdx][defaultChoices - 1], savedQuit, entrySize);
	}
}

/* Set up the Debug Mode entries for the arcade / Super Tyrian mode menus (these use
 * their menuChoicesDefault item counts; the buy/sell menu is special-cased). */
static void configure_arcade_debug_menus(void)
{
	static char quit1P[sizeof(menuInt[0][0])], quit2P[sizeof(menuInt[0][0])], quitST[sizeof(menuInt[0][0])];
	static bool saved1P = false, saved2P = false, savedST = false;

	configure_mode_debug_menu(MENU_1_PLAYER_ARCADE, MENU_1_PLAYER_ARCADE + 1, 4, quit1P, &saved1P);
	configure_mode_debug_menu(MENU_2_PLAYER_ARCADE, MENU_2_PLAYER_ARCADE + 1, 6, quit2P, &saved2P);
	configure_mode_debug_menu(MENU_SUPER_TYRIAN,    MENU_SUPER_TYRIAN + 1,    5, quitST, &savedST);
}

/* Add / remove the "Custom" row in the Upgrade Ship menu (item 9, just below Right
 * Sidekick) to match the customWeaponEnabled toggle. The stock "Done" label lives at
 * menuInt[2][8]; when Custom is present it moves down to menuInt[2][9] (item 10). */
static void configure_custom_weapon_menu(void)
{
	const size_t entrySize = sizeof(menuInt[0][0]);
	static char savedDone[sizeof(menuInt[0][0])];
	static bool savedDoneValid = false;

	if (!savedDoneValid)
	{
		SDL_strlcpy(savedDone, menuInt[2][8], entrySize);  // stock "Done" (item 9)
		savedDoneValid = true;
	}

	if (customWeaponEnabled)
	{
		menuChoices[MENU_UPGRADES] = 10;
		SDL_strlcpy(menuInt[2][8], "Custom", entrySize);   // item 9
		SDL_strlcpy(menuInt[2][9], savedDone, entrySize);  // item 10 = Done
	}
	else
	{
		menuChoices[MENU_UPGRADES] = 9;
		SDL_strlcpy(menuInt[2][8], savedDone, entrySize);  // restore Done at item 9
	}
}

#if defined(__SWITCH__) || defined(__vita__)
/* Switch/Vita: insert a "Touch" volume row into the shop Options / Limited Options submenus
 * after Sound Volume (item 6 in both). Labels come from data (menuInt), so shift them down
 * once (captured on first call) and re-apply the bumped menuChoices on every entry (they
 * reset from menuChoicesDefault at the top of JE_itemScreen). Value bar, input handling and
 * help text key off item 6 elsewhere (all #if defined(__SWITCH__) || defined(__vita__)). */
static void configure_options_touch_menu(void)
{
	const size_t entrySize = sizeof(menuInt[0][0]);
	static bool shifted = false;

	if (!shifted)
	{
		// MENU_OPTIONS (menuInt[3]): items 6..9 (labels [5]..[8]) move down to 7..10 ([6]..[9]).
		SDL_strlcpy(menuInt[3][9], menuInt[3][8], entrySize);  // Exit
		SDL_strlcpy(menuInt[3][8], menuInt[3][7], entrySize);  // Mouse Setup
		SDL_strlcpy(menuInt[3][7], menuInt[3][6], entrySize);  // Keyboard Setup
		SDL_strlcpy(menuInt[3][6], menuInt[3][5], entrySize);  // Joystick Setup
		SDL_strlcpy(menuInt[3][5], "Touch", entrySize);  // new item 6

		// MENU_LIMITED_OPTIONS (menuInt[12]): item 6 (Exit, label [5]) moves down to item 7 ([6]).
		SDL_strlcpy(menuInt[12][6], menuInt[12][5], entrySize);  // Exit
		SDL_strlcpy(menuInt[12][5], "Touch", entrySize);   // new item 6

		shifted = true;
	}

	menuChoices[MENU_OPTIONS] = menuChoicesDefault[MENU_OPTIONS] + 1;                  // 9 -> 10
	menuChoices[MENU_LIMITED_OPTIONS] = menuChoicesDefault[MENU_LIMITED_OPTIONS] + 1;  // 6 -> 7
}
#endif

/* Crash-log breadcrumb: map the live shop submenu (curMenu) to a readable name so a crash inside
 * the buy/sell screens records exactly which one was open (upgrade ship, a config screen, the
 * endless E-Shop, ...) instead of a single generic "shop". Called once per menu (re)entry from the
 * main loop, after MENU_FULL_GAME has been resolved to its mode-specific variant. Positional order
 * matches the MENU_* enum (0..MENU_MAX-1); every entry is a string literal, as crashlog_set_phase
 * requires (the pointer is read later from the fault handler). */
static void set_shop_phase(void)
{
	static const char *const names[MENU_MAX] = {
		"shop: buy/sell",             //  0  MENU_FULL_GAME
		"shop: upgrade ship",         //  1  MENU_UPGRADES
		"shop: options",              //  2  MENU_OPTIONS
		"shop: play next level",      //  3  MENU_PLAY_NEXT_LEVEL
		"shop: buy item",             //  4  MENU_UPGRADE_SUB
		"shop: keyboard config",      //  5  MENU_KEYBOARD_CONFIG
		"shop: load/save game",       //  6  MENU_LOAD_SAVE
		"shop: data cubes",           //  7  MENU_DATA_CUBES
		"shop: reading data cube",    //  8  MENU_DATA_CUBE_SUB
		"shop: 2-player menu",        //  9  MENU_2_PLAYER_ARCADE
		"shop: arcade/network menu",  // 10  MENU_1_PLAYER_ARCADE
		"shop: options",              // 11  MENU_LIMITED_OPTIONS
		"shop: joystick config",      // 12  MENU_JOYSTICK_CONFIG
		"shop: SuperTyrian menu",     // 13  MENU_SUPER_TYRIAN
		"shop: mouse config",         // 14  MENU_MOUSE_CONFIG
		"shop: debug play level",     // 15  MENU_DEBUG_PLAY_LEVEL
		"shop: endless E-Shop",       // 16  MENU_ESHOP
		"shop: endless perks",        // 17  MENU_PERKS
	};
	if ((unsigned)curMenu < MENU_MAX && names[curMenu] != NULL)
		crashlog_set_phase(names[curMenu]);
}

// Sort every merchant category (itemAvail rows) ascending by item id, with the "None"
// entry (id 0) always sunk to the BOTTOM of its list. Run once on shop entry and again
// after an endless reroll regenerates the stock -- endlessFillCategory seeds None at the
// TOP of a rerolled category, so without this re-sort a reroll would show None first.
static void sort_shop_inventory(void)
{
	for (int x = 0; x < 9; x++)
	{
		if (itemAvailMax[x] <= 1)
			continue;
		for (int a = 0; a < itemAvailMax[x] - 1; a++)
		{
			for (int b = a; b < itemAvailMax[x]; b++)
			{
				if (itemAvail[x][a] == 0 || (itemAvail[x][a] > itemAvail[x][b] && itemAvail[x][b] != 0))
				{
					JE_byte t = itemAvail[x][a];
					itemAvail[x][a] = itemAvail[x][b];
					itemAvail[x][b] = t;
				}
			}
		}
	}
}

void JE_itemScreen(void)
{
	bool quit = false;

	crashlog_set_phase("shop / buy-sell menu");

	/* Center the buy/sell screen independently from the gameplay HUD */
	set_menu_centered(true);

	if (shopSpriteSheet.data == NULL)
		JE_loadCompShapes(&shopSpriteSheet, '1');

	load_cubes();

	VGAScreen = VGAScreenSeg;

	memcpy(menuChoices, menuChoicesDefault, sizeof(menuChoices));
	configure_buysell_debug_menu();
	configure_arcade_debug_menus();
	configure_custom_weapon_menu();
#if defined(__SWITCH__) || defined(__vita__)
	configure_options_touch_menu();
#endif
	configure_endless_shop_menu();

	play_song(songBuy);

	JE_loadPic(VGAScreen, 1, false);

	curPal = 1;
	newPal = 0;

	JE_showVGA();

	set_palette(colors, 0, 255);

	col = 1;
	gameLoaded = false;
	curItemType = 1;
	cursor = 1;
	curItem = 0;

	for (unsigned int i = 0; i < COUNTOF(curSel); ++i)
		curSel[i] = 2;

	curMenu = MENU_FULL_GAME;

	// Endless: after a cleared zone, open on the forced perk pick before the normal front menu.
	// Choosing a perk or the decline sends us to MENU_FULL_GAME and clears the pending flag, and
	// nothing routes back here this visit -- so the perk screen is strictly one-shot.
	if (endlessMode && endlessPerkPending)
	{
		endlessPerkListMode = false;  // this is the forced PICK, not the read-only list
		configure_endless_perk_menu();
		curSel[MENU_PERKS] = 2;
		curMenu = MENU_PERKS;
	}

	int temp_weapon_power[7]; // assumes there'll never be more than 6 weapons to choose from, 7th is "Done"

	/* JE: (* Check for where Pitems and Select match up - if no match then add to the itemavail list *) */
	for (int i = 0; i < 7; i++)
	{
		int item = *playeritem_map(&player[0].last_items, i);

		int slot = 0;

		for (; slot < itemAvailMax[itemAvailMap[i]-1]; ++slot)
		{
			if (itemAvail[itemAvailMap[i]-1][slot] == item)
				break;
		}

		if (slot == itemAvailMax[itemAvailMap[i]-1])
		{
			itemAvail[itemAvailMap[i]-1][slot] = item;
			itemAvailMax[itemAvailMap[i]-1]++;
		}
	}

	memcpy(VGAScreen2->pixels, VGAScreen->pixels, VGAScreen2->pitch * VGAScreen2->h);

	keyboardUsed = false;
	firstMenu9 = false;
	backFromHelp = false;

	/* JE: Sort items in merchant inventory (None sinks to the bottom of each list) */
	sort_shop_inventory();

	do
	{
		/* Re-apply centering after returning from submenus */
		set_menu_centered(true);
		quit = false;

		JE_getShipInfo();

		if (curMenu == MENU_FULL_GAME)
		{
			if (twoPlayerMode)
				curMenu = MENU_2_PLAYER_ARCADE;

			if (isNetworkGame || onePlayerAction)
				curMenu = MENU_1_PLAYER_ARCADE;

			if (superTyrian)
				curMenu = MENU_SUPER_TYRIAN;
		}

		set_shop_phase();  // crash-log breadcrumb: record which shop submenu is now open

		paletteChanged = false;

		leftPower = false;
		rightPower = false;

		/* SYN: note reindexing... "firstMenu9" refers to Menu 8 here :( */
		if (curMenu != MENU_DATA_CUBE_SUB || firstMenu9)
		{
			memcpy(VGAScreen->pixels, VGAScreen2->pixels, VGAScreen->pitch * VGAScreen->h);
		}

		if (curMenu == MENU_UPGRADES &&
		    (curSel[curMenu] == 3 || curSel[curMenu] == 4))
		{
			// reset temp_weapon_power[] every time we select upgrading front or back
			const uint item       = player[0].items.weapon[curSel[MENU_UPGRADES] - 3].id,
			           item_power = player[0].items.weapon[curSel[MENU_UPGRADES] - 3].power,
			           i = curSel[MENU_UPGRADES] - 2;  // 1 or 2 (front or rear)

			// set power level of owned weapon
			for (int slot = 0; slot < itemAvailMax[itemAvailMap[i]-1]; ++slot)
			{
				if (itemAvail[itemAvailMap[i]-1][slot] == item)
					temp_weapon_power[slot] = item_power;
				else
					temp_weapon_power[slot] = 1;
			}

			// set power level for "Done"
			temp_weapon_power[itemAvailMax[itemAvailMap[i]-1]] = item_power;
		}

		if (curMenu == MENU_PLAY_NEXT_LEVEL)
		{
			planetAni = 0;
			keyboardUsed = false;
			currentDotNum = 0;
			currentDotWait = 8;
			planetAniWait = 3;
			JE_updateNavScreen();
		}

		/* Draw menu title for everything but upgrade ship submenus */
		if (curMenu != MENU_UPGRADE_SUB)
		{
			JE_drawMenuHeader();
		}

		/* Draw menu choices for simple menus */
		if (curMenu == MENU_PERKS && endlessPerkListMode)
		{
			// Endless read-only Perks list: its own scrolling renderer (the list can exceed the
			// menuInt row count, so it can't go through the menuInt-backed JE_drawMenuChoices).
			draw_endless_perk_list();
		}
		else if ((curMenu >= MENU_FULL_GAME && curMenu <= MENU_PLAY_NEXT_LEVEL) ||
		    (curMenu >= MENU_2_PLAYER_ARCADE && curMenu <= MENU_LIMITED_OPTIONS) ||
		    curMenu >= MENU_SUPER_TYRIAN)
		{
			JE_drawMenuChoices();
		}

		if (curMenu == MENU_MOUSE_CONFIG)
		{
			for (int i = 0; i < 3; ++i)
			{
				int tempY = 51 + i * 24;
				JE_textShade(VGAScreen, 215, tempY, joyButtonNames[mouseSettings[i] - 1], 15, 2, PART_SHADE);
			}
		}

		/* Data cube icons */
		if (curMenu == MENU_FULL_GAME)
		{
			for (int i = 1; i <= cubeMax; i++)
			{
				blit_sprite_dark(VGAScreen, 190 + i * 18 + 2, 37 + 1, OPTION_SHAPES, 34, false);
				blit_sprite(VGAScreen, 190 + i * 18, 37, OPTION_SHAPES, 34);  // data cube
			}
		}

		/* load/save menu */
		if (curMenu == MENU_LOAD_SAVE)
		{
			int min, max;

			if (twoPlayerMode)
			{
				min = 13;
				max = 24;
			}
			else
			{
				min = 2;
				max = 13;
			}

			for (int x = min; x <= max; x++)
			{
				/* Highlight if current selection */
				temp2 = (x - min + 2 == curSel[curMenu]) ? 15 : 28;

				/* Write save game slot */
				if (x == max)
					strcpy(tempStr, miscText[6-1]);
				else if (saveFiles[x-2].level == 0)
					strcpy(tempStr, miscText[3-1]);
				else
					strcpy(tempStr, saveFiles[x-2].name);

				int tempY = 38 + (x - min)*11;

				JE_textShade(VGAScreen, 163, tempY, tempStr, temp2 / 16, temp2 % 16 - 8, DARKEN);

				if (x < max) /* x == max isn't a save slot */
				{
					/* Highlight if current selection */
					temp2 = (x - min + 2 == curSel[curMenu]) ? 252 : 250;

					if (saveFiles[x-2].level == 0)
					{
						strcpy(tempStr, "-----"); /* Empty save slot */
					}
					else
					{
						char buf[20];

						strcpy(tempStr, saveFiles[x-2].levelName);

						snprintf(buf, sizeof buf, "%s%d", miscTextB[1-1], saveFiles[x-2].episode);
						JE_textShade(VGAScreen, 297, tempY, buf, temp2 / 16, temp2 % 16 - 8, DARKEN);
					}

					JE_textShade(VGAScreen, 245, tempY, tempStr, temp2 / 16, temp2 % 16 - 8, DARKEN);
				}

				JE_drawMenuHeader();
			}
		}

		if (curMenu == MENU_KEYBOARD_CONFIG)
		{
			for (int x = 2; x <= 11; x++)
			{
				if (x == curSel[curMenu])
				{
					temp2 = 15;
				}
				else
				{
					temp2 = 28;
				}

				JE_textShade(VGAScreen, 166, 38 + (x - 2)*12, menuInt[curMenu + 1][x-1], temp2 / 16, temp2 % 16 - 8, DARKEN);

				if (x < 10) /* 10 = reset to defaults, 11 = done */
				{
					temp2 = (x == curSel[curMenu]) ? 252 : 250;
					JE_textShade(VGAScreen, 236, 38 + (x - 2)*12, SDL_GetScancodeName(keySettings[x-2]), temp2 / 16, temp2 % 16 - 8, DARKEN);
				}
			}

			menuChoices[MENU_KEYBOARD_CONFIG] = 11;
		}

		if (curMenu == MENU_JOYSTICK_CONFIG)
		{
			const char *const menu_item[] =
			{
				"JOYSTICK",
				"ANALOG AXES",
				" SENSITIVITY",
				" THRESHOLD",
				menuInt[6][1],
				menuInt[6][4],
				menuInt[6][2],
				menuInt[6][3],
				menuInt[6][5],
				menuInt[6][6],
				menuInt[6][7],
				menuInt[6][8],
				"MENU",
				"PAUSE",
				menuInt[6][9],
				menuInt[6][10]
			};

			for (uint i = 0; i < COUNTOF(menu_item); i++)
			{
				int temp = (i == curSel[curMenu] - 2u) ? 15 : 28;

				JE_textShade(VGAScreen, 166, 38 + i * 8, menu_item[i], temp / 16, temp % 16 - 8, DARKEN);

				temp = (i == curSel[curMenu] - 2u) ? 252 : 250;

				char value[30] = "";
				if (joysticks == 0 && i < 14) // no joysticks, everything disabled
				{
					sprintf(value, "-");
				}
				else if (i == 0) // joystick number
				{
					sprintf(value, "%d", joystick_config + 1);
				}
				else if (i == 1) // joystick is analog
				{
					sprintf(value, "%s", joystick[joystick_config].analog ? "TRUE" : "FALSE");
				}
				else if (i < 4)  // joystick analog settings
				{
					if (!joystick[joystick_config].analog)
						temp -= 3;
					sprintf(value, "%d", i == 2 ? joystick[joystick_config].sensitivity : joystick[joystick_config].threshold);
				}
				else if (i < 14) // assignments
				{
					joystick_assignments_to_string(value, sizeof(value), joystick[joystick_config].assignment[i - 4]);
				}

				JE_textShade(VGAScreen, 236, 38 + i * 8, value, temp / 16, temp % 16 - 8, DARKEN);
			}

			menuChoices[curMenu] = COUNTOF(menu_item) + 1;
		}

		if (curMenu == MENU_UPGRADE_SUB)
		{
			/* Move cursor until we hit either "Done" or a weapon the player can afford */
			while (curSel[MENU_UPGRADE_SUB] < menuChoices[MENU_UPGRADE_SUB] &&
				JE_getCost(curSel[MENU_UPGRADES], itemAvail[itemAvailMap[curSel[MENU_UPGRADES] - 2] - 1][curSel[MENU_UPGRADE_SUB] - 2]) > (unsigned long)player[0].cash)
			{
				curSel[MENU_UPGRADE_SUB] += lastDirection;
				if (curSel[MENU_UPGRADE_SUB] < 2)
					curSel[MENU_UPGRADE_SUB] = menuChoices[MENU_UPGRADE_SUB];
				else if (curSel[MENU_UPGRADE_SUB] > menuChoices[MENU_UPGRADE_SUB])
					curSel[MENU_UPGRADE_SUB] = 2;
			}

			if (curSel[MENU_UPGRADE_SUB] == menuChoices[MENU_UPGRADE_SUB])
			{
				/* If cursor on "Done", use previous weapon */
				*playeritem_map(&player[0].items, curSel[MENU_UPGRADES] - 2) = *playeritem_map(&old_items[0], curSel[MENU_UPGRADES] - 2);
			}
			else
			{
				/* Otherwise display the selected weapon */
				*playeritem_map(&player[0].items, curSel[MENU_UPGRADES] - 2) = itemAvail[itemAvailMap[curSel[MENU_UPGRADES]-2]-1][curSel[MENU_UPGRADE_SUB]-2];
			}

			/* Get power level info for front and rear weapons */
			if ((curSel[MENU_UPGRADES] == 3 || curSel[MENU_UPGRADES] == 4) && curSel[MENU_UPGRADE_SUB] < menuChoices[MENU_UPGRADE_SUB])
			{
				if (curSel[MENU_UPGRADES] == 4 && itemAvail[itemAvailMap[curSel[MENU_UPGRADES]-2]-1][curSel[MENU_UPGRADE_SUB]-2] == 0)
				{
					// "None" on rear weapon menu cannot be upgraded
					// ("None" on front weapon menu can -- this is accurate to the original game)
					leftPower = false;
					rightPower = false;
				}
				else
				{
					const uint port = curSel[MENU_UPGRADES] - 3,  // 0 or 1 (front or back)
					           item_level = player[0].items.weapon[port].power;

					// calculate upgradeCost
					JE_getCost(curSel[MENU_UPGRADES], itemAvail[itemAvailMap[curSel[MENU_UPGRADES]-2]-1][curSel[MENU_UPGRADE_SUB]-2]);

					leftPower  = item_level > 1;  // can downgrade
					rightPower = item_level < 11; // can upgrade

					if (rightPower)
					{
						JE_longint cash_left = JE_cashLeft();
						rightPowerAfford = cash_left >= 0 && (ulong)cash_left >= upgradeCost; // can afford upgrade
					}
				}
			}
			else
			{
				// Nothing else can be upgraded or downgraded
				leftPower = false;
				rightPower = false;
			}

			/* submenu title  e.g., "Left Sidekick" */
			JE_dString(VGAScreen, 74 + JE_fontCenter(menuInt[2][curSel[MENU_UPGRADES]-1], FONT_SHAPES), 10, menuInt[2][curSel[MENU_UPGRADES]-1], FONT_SHAPES);

			/* Vertical scroll when the list overflows: the window shows visible_rows
			   rows from upgradeSubScrollTop and keeps the selected row visible. */
			const int sub_total_rows = menuChoices[curMenu] - 1;   // rows are tempW = 1..sub_total_rows
			const int sub_sel_row    = curSel[curMenu] - 1;        // tempW of the selected row
			const int visible_rows   = upgradeSubVisibleRows();
			int sub_max_top = sub_total_rows - visible_rows + 1;
			if (sub_max_top < 1)
				sub_max_top = 1;
			if (upgradeSubScrollTop > sub_max_top)
				upgradeSubScrollTop = sub_max_top;
			if (upgradeSubScrollTop < 1)
				upgradeSubScrollTop = 1;
			// Follow the selection only when it just moved (keyboard / mouse wheel /
			// click / the affordability skip above), so the selected row stays visible.
			if (curSel[curMenu] != upgradeSubPrevSel)
			{
				if (sub_sel_row < upgradeSubScrollTop)
					upgradeSubScrollTop = sub_sel_row;
				else if (sub_sel_row > upgradeSubScrollTop + visible_rows - 1)
					upgradeSubScrollTop = sub_sel_row - visible_rows + 1;
				upgradeSubPrevSel = curSel[curMenu];
			}

			/* Iterate through all submenu options */
			for (tempW = 1; tempW < menuChoices[curMenu]; tempW++)
			{
				if (tempW < upgradeSubScrollTop || tempW >= upgradeSubScrollTop + visible_rows)
					continue;  // scrolled out of view
				int tempY = 40 + (tempW - upgradeSubScrollTop) * 26; /* Calculate y position */
				unsigned long temp_cost;

				/* Is this a item or None/DONE? */
				if (tempW < menuChoices[MENU_UPGRADE_SUB] - 1)
				{
					/* Get base cost for choice */
					temp_cost = JE_getCost(curSel[MENU_UPGRADES], itemAvail[itemAvailMap[curSel[MENU_UPGRADES]-2]-1][tempW-1]);
				}
				else
				{
					/* "None" is free :) */
					temp_cost = 0;
				}

				int afford_shade = (temp_cost > (unsigned long)player[0].cash) ? 4 : 0;  // can player afford current weapon at all

				temp = itemAvail[itemAvailMap[curSel[MENU_UPGRADES]-2]-1][tempW-1]; /* Item ID */
				switch (curSel[MENU_UPGRADES]-1)
				{
					case 1: /* ship */
						if (temp > 90)
							snprintf(tempStr, sizeof(tempStr), "Custom Ship %d", temp - 90);
						else
							strcpy(tempStr, ships[temp].name);
						break;
					case 2: /* front and rear weapon */
					case 3:
						strcpy(tempStr, weaponPort[temp].name);
						break;
					case 4: /* shields */
						strcpy(tempStr, shields[temp].name);
						break;
					case 5: /* generator */
						strcpy(tempStr, powerSys[temp].name);
						break;
					case 6: /* sidekicks */
					case 7:
						strcpy(tempStr, options[temp].name);
						break;
				}
				if (tempW == curSel[curMenu]-1)
					temp2 = 15;
				else
					temp2 = 28;

				JE_getShipInfo();

				/* item-owned marker (pulled left of the scroll bar when it is shown) */
				if (temp == *playeritem_map(&old_items[0], curSel[MENU_UPGRADES] - 2) && temp != 0 && tempW != menuChoices[curMenu]-1)
				{
					const bool sub_has_scrollbar = sub_total_rows > visible_rows;
					const int marker_bar_right = sub_has_scrollbar ? 288 : 300;
					const int marker_x         = sub_has_scrollbar ? 286 : 298;
					fill_rectangle_xy(VGAScreen, 160, tempY+7, marker_bar_right, tempY+11, 227);
					blit_sprite2(VGAScreen, marker_x, tempY+2, shopSpriteSheet, 247);
				}

				/* Draw DONE */
				if (tempW == menuChoices[curMenu]-1)
				{
					strcpy(tempStr, miscText[13]);
				}
				JE_textShade(VGAScreen, 185, tempY, tempStr, temp2 / 16, temp2 % 16 - 8 - afford_shade, DARKEN);

				/* Draw icon if not DONE. NOTE: None is a normal item with a blank icon. */
				if (tempW < menuChoices[curMenu]-1)
				{
					JE_drawItem(curSel[MENU_UPGRADES]-1, temp, 160, tempY-4);
				}

				/* Make selected text brighter */
				temp2 = (tempW == curSel[curMenu]-1) ? 15 : 28;

				/* Draw Cost: if it's not the DONE option */
				if (tempW != menuChoices[curMenu]-1)
				{
					char buf[20];

					snprintf(buf, sizeof buf, "Cost: %lu", temp_cost);
					JE_textShade(VGAScreen, 187, tempY+10, buf, temp2 / 16, temp2 % 16 - 8 - afford_shade, DARKEN);
				}
			}

			/* Scroll bar (track + thumb) shown only when the list overflows. */
			if (sub_total_rows > visible_rows)
			{
				const int track_x  = 306;
				const int track_y0 = 38;
				const int track_y1 = 38 + visible_rows * 26 - 6;
				const int track_h  = track_y1 - track_y0;
				int thumb_h = track_h * visible_rows / sub_total_rows;
				if (thumb_h < 6)
					thumb_h = 6;
				const int thumb_y = track_y0 + (track_h - thumb_h) * (upgradeSubScrollTop - 1) / (sub_max_top - 1);
				fill_rectangle_xy(VGAScreen, track_x, track_y0, track_x + 2, track_y1, 241);
				fill_rectangle_xy(VGAScreen, track_x, thumb_y, track_x + 2, thumb_y + thumb_h, 252);
			}
		}

		/* Draw current money and shield/armor bars, when appropriate */
		if (((curMenu <= MENU_OPTIONS ||
		      curMenu == MENU_KEYBOARD_CONFIG ||
		      curMenu == MENU_LOAD_SAVE ||
		      curMenu >= MENU_1_PLAYER_ARCADE) &&
		     !twoPlayerMode) ||
		    (curMenu == MENU_UPGRADE_SUB &&
		     (curSel[MENU_UPGRADES] >= 1 && curSel[MENU_UPGRADES] <= 6)))
		{
			if (curMenu != MENU_UPGRADE_SUB)
			{
				char buf[20];

				snprintf(buf, sizeof buf, "%lu", player[0].cash);
				// Centre the cash total in the monitor slot, matching the endless course RANK readout
				// (same slot, same row) instead of growing rightward from a fixed left edge.
				// y172: DARKEN draws the body at y+1, so this lands on row 173 -- level with the RANK.
				JE_textShade(VGAScreen, MENU_MONITOR_CENTER_X - JE_textWidth(buf, TINY_FONT) / 2, 172, buf, 1, 6, DARKEN);
			}
			if (endlessMode)
			{
				// Reinforced hull exceeds the classic 28-armour bar; draw it as colour-coded
				// rollover rows (same 28-per-layer rollover as the in-game HUD armour bar) so it
				// can't march off the panel into the shield gauge. Layer colours tunable.
				static const int shopArmorLayerCol[] = { 14, 30, 46, 62, 78, 94, 110, 126 };
				int a = player[0].armor;
				for (int L = 0; a > 0 && L < (int)COUNTOF(shopArmorLayerCol); ++L)
				{
					JE_barDrawShadow(VGAScreen, 42, 152, 3, shopArmorLayerCol[L], (a > 28) ? 28 : a, 2, 13);
					a -= 28;
				}
			}
			else
				JE_barDrawShadow(VGAScreen, 42, 152, 3, 14, player[0].armor, 2, 13);
			// Shield strength (item mpwr) rescaled so the strongest shield (HXS Class C, mpwr 14)
			// fills exactly 10 bars and every weaker shield proportionally fewer -- matching the
			// in-game shield gauge, whose full height scales the same way (shield_max = mpwr*2).
			// Rounded to the nearest bar; None (mpwr 0) draws nothing; clamped so it can't overrun.
			{
				int shieldBars = (shields[player[0].items.shield].mpwr * 10 + 7) / 14;
				if (shieldBars > 10)
					shieldBars = 10;
				JE_barDrawShadow(VGAScreen, 104, 152, 1, 14, shieldBars, 2, 13);
			}
		}

		/* Draw crap on the left side of the screen, i.e. two player scores, ship graphic, etc. */
		if ((curMenu >= MENU_FULL_GAME && curMenu <= MENU_OPTIONS) ||
			curMenu == MENU_KEYBOARD_CONFIG ||
			curMenu == MENU_LOAD_SAVE ||
			(curMenu >= MENU_2_PLAYER_ARCADE) ||
			(curMenu == MENU_UPGRADE_SUB &&
				(curSel[MENU_UPGRADES] == 2 || curSel[MENU_UPGRADES] == 5)))
		{
			if (twoPlayerMode)
			{
				char buf[50];

				for (uint i = 0; i < 2; ++i)
				{
					snprintf(buf, sizeof(buf), "%s %lu", miscText[40 + i], player[i].cash);
					JE_textShade(VGAScreen, 25, 50 + 10 * i, buf, 15, 0, FULL_SHADE);
				}
			}
			else if (superArcadeMode != SA_NONE || superTyrian)
			{
				helpBoxColor = 15;
				helpBoxBrightness = 4;
				if (!superTyrian)
					JE_helpBox(VGAScreen, 35, 25, superShips[superArcadeMode], 18);
				else
					JE_helpBox(VGAScreen, 35, 25, superShips[SA+3], 18);
				helpBoxBrightness = 1;

				JE_textShade(VGAScreen, 25, 50, superShips[SA+1], 15, 0, FULL_SHADE);
				JE_helpBox(VGAScreen,   25, 60, weaponPort[player[0].items.weapon[FRONT_WEAPON].id].name, 22);
				JE_textShade(VGAScreen, 25, 120, superShips[SA+2], 15, 0, FULL_SHADE);
				JE_helpBox(VGAScreen,   25, 130, special[player[0].items.special].name, 22);
			}
			else
			{
				draw_ship_illustration();
			}
		}

		/* Changing the volume? */
		if (curMenu == MENU_OPTIONS ||
		    curMenu == MENU_LIMITED_OPTIONS)
		{
			JE_barDrawShadow(VGAScreen, 225, 70, 1, music_disabled ? 12 : 16, tyrMusicVolume / 12, 3, 13);
			JE_barDrawShadow(VGAScreen, 225, 86, 1, samples_disabled ? 12 : 16, fxVolume / 12, 3, 13);
#if defined(__SWITCH__) || defined(__vita__)
			// Touch Sensitivity (item 6, y=102): same bar style as the two volume rows above. The
			// marker slot goes bright once the fill reaches it -- compare drawn bar counts (amt vs
			// mark), not the raw value, so it flips exactly on the middle bar.
			{
				const int amt = touch_sensitivity / 12;
				const int mark = TOUCH_SENS_DEFAULT / 12;
				JE_barDrawShadow(VGAScreen, 225, 102, 1, 16, amt, 3, 13);
				JE_barDrawMark(VGAScreen, 225, 102,
				               amt >= mark ? TOUCH_SENS_MARK_COL : TOUCH_SENS_MARK_COL_DIM, mark, 3, 13);
			}
#endif
		}

		/* "firstmenu9" refers to menu 8 because of reindexing */
		if (curMenu == MENU_DATA_CUBES ||
		    (curMenu == MENU_DATA_CUBE_SUB && (firstMenu9 || backFromHelp)))
		{
			firstMenu9 = false;
			menuChoices[MENU_DATA_CUBES] = cubeMax + 2;
			fill_rectangle_xy(VGAScreen, 1, 1, 145, 170, 0);

			blit_sprite(VGAScreenSeg, 1, 1, OPTION_SHAPES, 20); /* Portrait area background */

			if (curMenu == MENU_DATA_CUBES)
			{
				if (cubeMax == 0)
				{
					JE_helpBox(VGAScreen, 166, 80, miscText[16 - 1], 30);
					tempW = 160;
					temp2 = 252;
				}
				else
				{
					for (int x = 1; x <= cubeMax; x++)
					{
						JE_drawCube(VGAScreenSeg, 166, 38 + (x - 1) * 28, 13, 0);
						if (x + 1 == curSel[curMenu])
						{
							temp2 = 252;
						}
						else
						{
							temp2 = 250;
						}

						helpBoxColor = temp2 / 16;
						helpBoxBrightness = (temp2 % 16) - 8;
						helpBoxShadeType = DARKEN;
						JE_helpBox(VGAScreen, 192, 44 + (x - 1) * 28, cube[x - 1].title, 24);
					}
					int x = cubeMax + 1;
					if (x + 1 == curSel[curMenu])
					{
						temp2 = 252;
					}
					else
					{
						temp2 = 250;
					}
					tempW = 44 + (x - 1) * 28;
				}

				JE_textShade(VGAScreen, 172, tempW, miscText[6 - 1], temp2 / 16, (temp2 % 16) - 8, DARKEN);
			}

			if (curSel[MENU_DATA_CUBES] < menuChoices[MENU_DATA_CUBES])
			{
				const int face_sprite = cube[curSel[MENU_DATA_CUBES] - 2].face_sprite;

				if (face_sprite != -1)
				{
					const int face_x = 77 - (sprite(FACE_SHAPES, face_sprite)->width / 2),
					          face_y = 92 - (sprite(FACE_SHAPES, face_sprite)->height / 2);

					blit_sprite(VGAScreenSeg, face_x, face_y, FACE_SHAPES, face_sprite);  // datacube face

					// modify palette for face
					paletteChanged = true;
					temp2 = (face_sprite < 12) ? facepal[face_sprite] : 0;
					newPal = 0;

					for (temp = 1; temp <= 255 - (3 * 16); temp++)
						colors[temp] = palettes[temp2][temp];
				}
			}
		}

		/* 2 player input devices */
		if (curMenu == MENU_2_PLAYER_ARCADE)
		{
			for (uint i = 0; i < COUNTOF(inputDevice); i++)
			{
				if (inputDevice[i] > 2 + joysticks)
					inputDevice[i] = inputDevice[i == 0 ? 1 : 0] == 1 ? 2 : 1;

				char temp[64];
				if (joysticks > 1 && inputDevice[i] > 2)
					sprintf(temp, "%s %d", inputDevices[2], inputDevice[i] - 2);
				else
					sprintf(temp, "%s", inputDevices[inputDevice[i] - 1]);
				JE_dString(VGAScreen, 186, 38 + 2 * (i + 1) * 16, temp, SMALL_FONT_SHAPES);
			}
		}

		/* JE: { - Step VI - Help text for current cursor location } */

		flash = false;

		/* JE: {Reset player weapons} */
		memset(shotMultiPos, 0, sizeof(shotMultiPos));

		JE_drawScore();

		JE_drawMainMenuHelpText();

		if (newPal > 0) /* can't reindex this :( */
		{
			curPal = newPal;
			memcpy(colors, palettes[newPal - 1], sizeof(colors));
			set_palette(palettes[newPal - 1], 0, 255);
			newPal = 0;
		}

		/* datacube title under face */
		if ((curMenu == MENU_DATA_CUBES || curMenu == MENU_DATA_CUBE_SUB) &&
			curSel[MENU_DATA_CUBES] < menuChoices[MENU_DATA_CUBES])
		{
			JE_textShade(VGAScreen, 75 - JE_textWidth(cube[curSel[MENU_DATA_CUBES] - 2].header, TINY_FONT) / 2, 173, cube[curSel[MENU_DATA_CUBES] - 2].header, 14, 3, DARKEN);
		}

		/* SYN: Everything above was just drawing the screen. In the rest of it, we process
		   any user input (and do a few other things) */

		/* SYN: Let's start by getting fresh events from SDL */
		service_SDL_events(true);

		if (constantPlay)
		{
			mainLevel = mapSection[mapPNum-1];
			jumpSection = true;
		}
		else
		{
			do
			{
			/* Inner loop -- this handles animations on menus that need them and handles
			   some keyboard events. Events it can't handle end the loop and fall through
			   to the main keyboard handler below.

			   Also, I think all timing is handled in here. Somehow. */

				NETWORK_KEEP_ALIVE();

				mouseCursor = MOUSE_POINTER_NORMAL;

				col += colC;
				if (col < -2 || col > 6)
				{
					colC = (-1 * colC);
				}

				// data cube reading
				if (curMenu == MENU_DATA_CUBE_SUB)
				{
					if (mouseX > 164 && mouseX < 299 && mouseY > 47 && mouseY < 153)
					{
						if (mouseY > 100)
							mouseCursor = MOUSE_POINTER_DOWN;
						else
							mouseCursor = MOUSE_POINTER_UP;
					}

					fill_rectangle_xy(VGAScreen, 160, 49, 310, 158, 228);
					if (yLoc + yChg < 0)
					{
						yChg = 0;
						yLoc = 0;
					}

					yLoc += yChg;
					temp = yLoc / 12;
					temp2 = yLoc % 12;
					tempW = 38 + 12 - temp2;
					temp3 = cube[curSel[MENU_DATA_CUBES] - 2].last_line;

					for (int x = temp + 1; x <= temp + 10; x++)
					{
						if (x <= temp3)
						{
							JE_outTextAndDarken(VGAScreen, 161, tempW, cube[curSel[MENU_DATA_CUBES] - 2].text[x-1], 14, 3, TINY_FONT);
							tempW += 12;
						}
					}

					fill_rectangle_xy(VGAScreen, 160, 39, 310, 48, 228);
					fill_rectangle_xy(VGAScreen, 160, 157, 310, 166, 228);

					int percent_read = (cube[currentCube].last_line <= 9)
					                   ? 100
					                   : (yLoc * 100) / ((cube[currentCube].last_line - 9) * 12);

					char buf[55];
					snprintf(buf, sizeof(buf), "%s %d%%", miscText[11], percent_read);
					JE_outTextAndDarken(VGAScreen, 176, 160, buf, 14, 1, TINY_FONT);

					JE_dString(VGAScreen, 260, 160, miscText[12], SMALL_FONT_SHAPES);

					if (temp2 == 0)
						yChg = 0;

					JE_mouseStart();

					JE_showVGA();

					if (backFromHelp)
					{
						fade_palette(colors, 10, 0, 255);
						backFromHelp = false;
					}
					JE_mouseReplace();

					setDelay(1);
				}
				else
				{
					/* current menu is not 8 (read data cube) */

					if (curMenu == MENU_PLAY_NEXT_LEVEL)
					{
						// Remember the current pan, advance one tick, draw the authoritative
						// frame; JE_navScreenSmoothPresent interpolates between the two.
						nav_smooth_prev_x = navX;
						nav_smooth_prev_y = navY;
						JE_navScreenAdvance();
						JE_navDrawFrame(navX, navY);
					}

					if (curMenu == MENU_DATA_CUBES &&
					    curSel[MENU_DATA_CUBES] < menuChoices[MENU_DATA_CUBES])
					{
						/* Draw flashy cube */
						blit_sprite_hv_blend(VGAScreenSeg, 166, 38 + (curSel[MENU_DATA_CUBES] - 2) * 28, OPTION_SHAPES, 25, 13, col);
					}

					/* IF (curmenu = 5) AND (cursel [2] IN [3, 4, 6, 7, 8]) */
					if (curMenu == MENU_UPGRADE_SUB &&
					    (curSel[MENU_UPGRADES] == 3 ||
					     curSel[MENU_UPGRADES] == 4 ||
					     (curSel[MENU_UPGRADES] >= 6 &&
					      curSel[MENU_UPGRADES] <= 8)))
					{
						setDelay(3);

						// Record the weapon-sim frame for interpolated presentation
						// (preview region only). VGAScreen == VGAScreenSeg here.
						rl_begin_record();
						JE_weaponSimUpdate();
						JE_drawScore();
						rl_end_record();
						rl_finalize();
						// Capture the non-blit overlay (gauge, segment bars, cost/cash text)
						// as residual so the region copy doesn't wipe it. game_screen is safe
						// scratch; VGAScreen2 (menu backdrop snapshot) must stay intact.
						rl_capture_residual(VGAScreenSeg, game_screen);

						service_SDL_events(false);

						if (newPal > 0)
						{
							curPal = newPal;
							set_palette(palettes[newPal - 1], 0, 255);
							newPal = 0;
						}

						if (paletteChanged)
						{
							set_palette(colors, 0, 255);
							paletteChanged = false;
						}

						if (backFromHelp)
						{
							fade_palette(colors, 10, 0, 255);
							backFromHelp = false;
						}

						JE_weaponSimSmoothPresent();  // cursor + showVGA happen inside

					}
					else if (curMenu == MENU_PLAY_NEXT_LEVEL)
					{
						// Planet nav screen: JE_navScreenSmoothPresent shows the pan
						// interpolated and consumes the whole tick (the wait below is a no-op).
						setDelay(2);

						if (newPal > 0)
						{
							curPal = newPal;
							set_palette(palettes[newPal - 1], 0, 255);
							newPal = 0;
						}

						if (paletteChanged)
						{
							set_palette(colors, 0, 255);
							paletteChanged = false;
						}

						JE_navScreenSmoothPresent();  // cursor + showVGA happen inside

						if (backFromHelp)
						{
							fade_palette(colors, 10, 0, 255);
							backFromHelp = false;
						}
					}
					else  /* current menu is anything but weapon sim or datacube */
					{
						setDelay(2);

						JE_drawScore();

						if (newPal > 0)
						{
							curPal = newPal;
							set_palette(palettes[newPal - 1], 0, 255);
							newPal = 0;
						}

						JE_mouseStart();

						if (paletteChanged)
						{
							set_palette(colors, 0, 255);
							paletteChanged = false;
						}

						JE_showVGA(); /* SYN: This is the where the screen updates for most menus */

						JE_mouseReplace();

						if (backFromHelp)
						{
							fade_palette(colors, 10, 0, 255);
							backFromHelp = false;
						}

					}
				}

				menuWaitWithSmoothCursor();  // was wait_delay(); keeps the cursor smooth

				push_joysticks_as_keyboard();
				service_SDL_events(false);
				mouseButton = JE_mousePosition(&mouseX, &mouseY);
				inputDetected = newkey || mouseButton > 0;

#if defined(__SWITCH__) || defined(__vita__)
				// The shoulder buttons cycle the rear gun's fire mode in the weapon
				// preview, mirroring the [/] key (SDL_SCANCODE_SLASH below): L = previous
				// mode, R = next. Raw button reads with local edge state, because the
				// menus only receive confirm/cancel/directions from a controller
				// (push_joysticks_as_keyboard) and the shoulders aren't bound to any of
				// those. poll_joysticks (inside push_joysticks_as_keyboard, just above)
				// already ran SDL_JoystickUpdate this tick.
				{
#if defined(__SWITCH__)
					static const int shoulder_btn[2] = { 6, 7 };  // switch-sdl2: 6 = L, 7 = R
#else
					static const int shoulder_btn[2] = { 4, 5 };  // Vita: 4 = L, 5 = R
#endif
					static bool shoulder_was[2];
					for (int s = 0; s < 2; ++s)
					{
						const bool down = joysticks > 0 && joystick[0].handle != NULL &&
						                  SDL_JoystickGetButton(joystick[0].handle, shoulder_btn[s]) != 0;

						if (down && !shoulder_was[s] &&
						    curMenu == MENU_UPGRADE_SUB && curSel[MENU_UPGRADES] == 4)
						{
							const uint opnum = weaponPort[player[0].items.weapon[REAR_WEAPON].id].opnum;
							if (s == 0)
								player[0].weapon_mode = (player[0].weapon_mode > 1) ? player[0].weapon_mode - 1 : opnum;
							else if (++player[0].weapon_mode > opnum)
								player[0].weapon_mode = 1;
						}
						shoulder_was[s] = down;
					}
				}
#endif

				/* Mouse wheel moves the buy/sell sub-list selection like the up/down
				   arrows; inputDetected makes the outer loop redraw at the new selection. */
				if (curMenu == MENU_UPGRADE_SUB && mouse_scroll != 0)
				{
					const int dir = (mouse_scroll > 0) ? -1 : 1;  // wheel up = towards the top
					int steps = (mouse_scroll > 0) ? mouse_scroll : -mouse_scroll;
					while (steps-- > 0)
					{
						lastDirection = dir;

						curSel[curMenu] += dir;
						if (curSel[curMenu] < 2)
							curSel[curMenu] = menuChoices[curMenu];
						else if (curSel[curMenu] > menuChoices[curMenu])
							curSel[curMenu] = 2;

						// keep the front/rear weapon power preview in sync, as the arrows do
						if (curSel[MENU_UPGRADES] == 3 || curSel[MENU_UPGRADES] == 4)
						{
							player[0].items.weapon[curSel[MENU_UPGRADES]-3].power = temp_weapon_power[curSel[MENU_UPGRADE_SUB]-2];
							if (curSel[MENU_UPGRADES] == 4)
								player[0].weapon_mode = 1;
						}
					}
					JE_playSampleNum(S_CURSOR);
					mouse_scroll = 0;
					inputDetected = true;  // redraw at the new selection
				}

				// Endless hardcore forbids all saving/loading, so the Alt+S / Alt+L quick keys are
				// disabled too (not just the menu rows).
				if (curMenu != MENU_LOAD_SAVE && !(endlessMode && endlessHardcore))
				{
					if (keysactive[SDL_SCANCODE_S] && (keysactive[SDL_SCANCODE_LALT] || keysactive[SDL_SCANCODE_RALT]))
					{
						if (curMenu == MENU_DATA_CUBE_SUB ||
						    curMenu == MENU_DATA_CUBES)
						{
							curMenu = MENU_FULL_GAME;
						}
						quikSave = true;
						oldMenu = curMenu;
						curMenu = MENU_LOAD_SAVE;
						performSave = true;
						newPal = 1;
						oldPal = curPal;
					}
					if (keysactive[SDL_SCANCODE_L] && (keysactive[SDL_SCANCODE_LALT] || keysactive[SDL_SCANCODE_RALT]))
					{
						if (curMenu == MENU_DATA_CUBE_SUB ||
						    curMenu == MENU_DATA_CUBES)
						{
							curMenu = MENU_FULL_GAME;
						}
						quikSave = true;
						oldMenu = curMenu;
						curMenu = MENU_LOAD_SAVE;
						performSave = false;
						newPal = 1;
						oldPal = curPal;
					}
				}

				if (curMenu == MENU_DATA_CUBE_SUB)
				{
					if (mouseButton > 0 && mouseCursor != MOUSE_POINTER_NORMAL)
					{
						inputDetected = false;
						if (mouseCursor == MOUSE_POINTER_UP)
							yChg = -1;
						else
							yChg = 1;
					}

					if (keysactive[SDL_SCANCODE_PAGEUP])
					{
						yChg = -2;
						inputDetected = false;
					}
					if (keysactive[SDL_SCANCODE_PAGEDOWN])
					{
						yChg = 2;
						inputDetected = false;
					}

					bool joystick_up = false, joystick_down = false;
					for (int j = 0; j < joysticks; j++)
					{
						joystick_up |= joystick[j].direction[0];
						joystick_down |= joystick[j].direction[2];
					}

					if (keysactive[SDL_SCANCODE_UP] || joystick_up)
					{
						yChg = -1;
						inputDetected = false;
					}

					if (keysactive[SDL_SCANCODE_DOWN] || joystick_down)
					{
						yChg = 1;
						inputDetected = false;
					}

					if (yChg < 0 && yLoc == 0)
					{
						yChg = 0;
					}
					if (yChg  > 0 && (yLoc / 12) > cube[currentCube].last_line - 10)
					{
						yChg = 0;
					}
				}

			} while (!inputDetected);
		}

		keyboardUsed = false;

		/* The rest of this just grabs input events, handles them, then proceeds on. */

		if (mouseButton > 0)
		{
			lastDirection = 1;

			mouseButton = JE_mousePosition(&mouseX, &mouseY);

			if (curMenu == MENU_DATA_CUBES && cubeMax == 0)
			{
				curMenu = MENU_FULL_GAME;
				JE_playSampleNum(S_SPRING);
				newPal = 1;
				JE_wipeKey();
			}

			if (curMenu == MENU_DATA_CUBE_SUB)
			{
				if ((mouseX > 258) && (mouseX < 290) && (mouseY > 159) && (mouseY < 171))
				{
					curMenu = MENU_DATA_CUBES;
					JE_playSampleNum(S_SPRING);
				}
			}

			if (curMenu == MENU_OPTIONS ||
			    curMenu == MENU_LIMITED_OPTIONS)
			{
				if ((mouseX >= (225 - 4)) && (mouseY >= 70) && (mouseY <= 82))
				{
					if (music_disabled)
					{
						music_disabled = false;
						restart_song();
					}

					curSel[MENU_OPTIONS] = 4;

					tyrMusicVolume = (mouseX - (225 - 4)) / 4 * 12;
					if (tyrMusicVolume > 255)
						tyrMusicVolume = 255;
				}

				if ((mouseX >= (225 - 4)) && (mouseY >= 86) && (mouseY <= 98))
				{
					samples_disabled = false;

					curSel[MENU_OPTIONS] = 5;

					fxVolume = (mouseX - (225 - 4)) / 4 * 12;
					if (fxVolume > 255)
						fxVolume = 255;
				}

#if defined(__SWITCH__) || defined(__vita__)
				// Touch Sensitivity bar (item 6, y=102): drag to set, same feel as the volume bars.
				if ((mouseX >= (225 - 4)) && (mouseY >= 102) && (mouseY <= 114))
				{
					curSel[curMenu] = 6;

					touch_sensitivity = (mouseX - (225 - 4)) / 4 * 12;
					if (touch_sensitivity > TOUCH_SENS_MAX)
						touch_sensitivity = TOUCH_SENS_MAX;
				}
#endif

				set_volume(tyrMusicVolume, fxVolume);

				JE_playSampleNum(S_CURSOR);
			}

			if (mouseY > 20 && curMenu != MENU_DATA_CUBE_SUB)
			{
				int selection = menuChoices[curMenu] + 1; /* invalid by default */

				if (curMenu == MENU_DEBUG_PLAY_LEVEL)
				{
					if (mouseX > 165 && mouseX < 325 && mouseY >= 38)
					{
						int col = (mouseX - 165) / 80;
						int row = (mouseY - 38) / 8;
						selection = row * 2 + col + 2;
					}
				}
				else if (mouseX > 170 && mouseX < 308)
				{
					const JE_byte mouseSelectionY[MENU_MAX] = { 16, 16, 16, 16, 26, 12, 11, 28, 0, 16, 16, 16, 8, 16, 24, 16, 16, 16 };  // [16]=E-Shop, [17]=Perks pick: 16px rows like the buy/sell menus

					// The read-only perk LIST draws at tight 10px rows in a small font (JE_drawMenuChoices);
					// the forced perk PICK keeps the standard 16px pitch.
					const int rowPitch = (curMenu == MENU_ESHOP || (curMenu == MENU_PERKS && endlessPerkListMode)) ? 10 : mouseSelectionY[curMenu];
					selection = (mouseY - 38) / rowPitch + 2;

					/* The upgrade sub-list scrolls, so map the click through the
					   current scroll window; a click below the last visible row misses. */
					if (curMenu == MENU_UPGRADE_SUB)
					{
						const int vis = (mouseY - 38) / 26;
						selection = (vis >= 0 && vis < upgradeSubVisibleRows())
						            ? upgradeSubScrollTop + vis + 1
						            : menuChoices[curMenu] + 1;  // outside the list
					}

					/* The read-only Perks list scrolls too; map the click through its window
					   (10px rows), a click below the last visible row misses. */
					if (curMenu == MENU_PERKS && endlessPerkListMode)
					{
						const int vis = (mouseY - 38) / 10;
						const int tempW = perkListScrollTop + vis;  // 1-based row; curSel = tempW + 1
						selection = (vis >= 0 && vis < PERK_LIST_VIS && tempW >= 1 && tempW <= menuChoices[curMenu] - 1)
						            ? tempW + 1
						            : menuChoices[curMenu] + 1;  // outside the visible list
					}

					if (curMenu == MENU_2_PLAYER_ARCADE)
					{
						if (selection > 5)
							selection--;
						if (selection > 3)
							selection--;
					}

					if (curMenu == MENU_FULL_GAME)
					{
						if (debugMode)
						{
							// items 7-9 (debug block + Quit) draw 16px lower after the
							// "Start Level" gap; shift the click target to match
							if (selection >= 8)
								selection--;
						}
						else
						{
							// "Quit Game" (item 7) is drawn 16px lower with a blank
							// gap above it; clamp clicks at or below it to Quit
							if (selection > 7)
								selection = 7;
						}
					}

					/* is play next level screen? */
					if (curMenu == MENU_PLAY_NEXT_LEVEL)
					{
						if (selection == menuChoices[curMenu] + 1)
							selection = menuChoices[curMenu];
					}

					// Endless perk PICK / E-Shop: "Take the Cash" / "Done" is drawn a row lower (blank-line
					// gap); clamp clicks at or below the gap onto it. The read-only Perks LIST is excluded --
					// it maps clicks through its own scroll window above, where a miss must stay a miss.
					if (((curMenu == MENU_PERKS && !endlessPerkListMode) || curMenu == MENU_ESHOP) && selection > menuChoices[curMenu])
						selection = menuChoices[curMenu];
				}

				if (selection <= menuChoices[curMenu])
				{
					if (curMenu == MENU_UPGRADE_SUB &&
						selection == menuChoices[MENU_UPGRADE_SUB])
					{
						player[0].cash = JE_cashLeft();
						curMenu = MENU_UPGRADES;
						JE_playSampleNum(S_ITEM);
					}
					else
					{
						JE_playSampleNum(S_CLICK);
						if (curSel[curMenu] == selection)
						{
							JE_menuFunction(curSel[curMenu]);
						}
						else
						{
							if (curMenu == MENU_UPGRADE_SUB &&
								JE_getCost(curSel[MENU_UPGRADES], itemAvail[itemAvailMap[curSel[MENU_UPGRADES] - 2] - 1][selection - 2]) > (unsigned long)player[0].cash)
							{
								JE_playSampleNum(S_CLINK);
							}
							else
							{
								if (curSel[MENU_UPGRADES] == 4)
									player[0].weapon_mode = 1;

								curSel[curMenu] = selection;
							}

							/* in front or rear weapon upgrade screen? */
							if (curMenu == MENU_UPGRADE_SUB &&
								(curSel[MENU_UPGRADES] == 3 || curSel[MENU_UPGRADES] == 4))
							{
								player[0].items.weapon[curSel[MENU_UPGRADES] - 3].power = temp_weapon_power[curSel[MENU_UPGRADE_SUB] - 2];
							}
						}
					}
				}

				wait_noinput(false, true, false);
			}

			if (curMenu == MENU_UPGRADE_SUB &&
			    (curSel[MENU_UPGRADES] == 3 || curSel[MENU_UPGRADES] == 4))
			{
				if ((mouseX >= 23) && (mouseX <= 36) && (mouseY >= 149) && (mouseY <= 168))
				{
					JE_playSampleNum(S_CURSOR);
					switch (curSel[MENU_UPGRADES])
					{
					case 3:
					case 4:
						if (leftPower)
							player[0].items.weapon[curSel[MENU_UPGRADES]-3].power = --temp_weapon_power[curSel[MENU_UPGRADE_SUB]-2];
						else
							JE_playSampleNum(S_CLINK);

						break;
					}
					wait_noinput(false, true, false);
				}

				if ((mouseX >= 119) && (mouseX <= 131) && (mouseY >= 149) && (mouseY <= 168))
				{
					JE_playSampleNum(S_CURSOR);
					switch (curSel[MENU_UPGRADES])
					{
					case 3:
					case 4:
						if (rightPower && rightPowerAfford)
							player[0].items.weapon[curSel[MENU_UPGRADES]-3].power = ++temp_weapon_power[curSel[MENU_UPGRADE_SUB]-2];
						else
							JE_playSampleNum(S_CLINK);

						break;
					}
					wait_noinput(false, true, false);
				}
			}
		}
		else if (newkey)
		{
			switch (lastkey_scan)
			{
			case SDL_SCANCODE_SLASH:
				// if in rear weapon upgrade screen
				if (curMenu == MENU_UPGRADE_SUB && curSel[MENU_UPGRADES] == 4)
				{
					// cycle weapon modes
					if (++player[0].weapon_mode > weaponPort[player[0].items.weapon[REAR_WEAPON].id].opnum)
						player[0].weapon_mode = 1;
				}
				break;

			case SDL_SCANCODE_SPACE:
			case SDL_SCANCODE_RETURN:
				keyboardUsed = true;

				// if front or rear weapon, update "Done" power level
				if (curMenu == MENU_UPGRADE_SUB && (curSel[MENU_UPGRADES] == 3 || curSel[MENU_UPGRADES] == 4))
					temp_weapon_power[itemAvailMax[itemAvailMap[curSel[MENU_UPGRADES]-2]-1]] = player[0].items.weapon[curSel[MENU_UPGRADES]-3].power;

				JE_menuFunction(curSel[curMenu]);
				break;

			case SDL_SCANCODE_ESCAPE:
				keyboardUsed = true;

				// Endless perk pick is a forced choice: Esc doesn't back out, it takes the cash.
				// The read-only perk LIST (endlessPerkListMode) isn't forced -- Esc just backs out
				// to the buy/sell menu via the normal menuEsc mapping below.
				if (curMenu == MENU_PERKS)
				{
					if (endlessPerkListMode)
						endlessPerkListMode = false;   // fall through to the normal Esc handling
					else
					{
						JE_menuFunction(menuChoices[MENU_PERKS]);
						break;
					}
				}

				JE_playSampleNum(S_SPRING);
				if (curMenu == MENU_LOAD_SAVE && quikSave)
				{
					curMenu = oldMenu;
					newPal = oldPal;
				}
				else if (menuEsc[curMenu] == 0)
				{
					if (JE_quitRequest())
					{
						gameLoaded = true;
						mainLevel = 0;
					}
				}
				else
				{
					if (curMenu == MENU_UPGRADE_SUB)  // leaving upgrade menu without buying
					{
						player[0].items = old_items[0];
						curSel[MENU_UPGRADE_SUB] = lastCurSel;
						player[0].cash = JE_cashLeft();
					}

					if (curMenu != MENU_DATA_CUBE_SUB)
						newPal = 1;

					curMenu = menuEsc[curMenu] - 1;
				}
				break;

			case SDL_SCANCODE_F1:
				if (!isNetworkGame)
				{
					fade_black(10);
					JE_helpSystem(2);

					play_song(songBuy);

					JE_loadPic(VGAScreen, 1, false);
					newPal = 1;

					switch (curMenu)
					{
					case 3:
						newPal = 18;
						break;
					case 7:
					case 8:
						break;
					}

					memcpy(VGAScreen2->pixels, VGAScreen->pixels, VGAScreen2->pitch * VGAScreen2->h);

					curPal = newPal;
					memcpy(colors, palettes[newPal-1], sizeof(colors));
					JE_showVGA();
					newPal = 0;
					backFromHelp = true;
				}
				break;

			case SDL_SCANCODE_UP:
				keyboardUsed = true;
				lastDirection = -1;

				if (curMenu != MENU_DATA_CUBE_SUB)
					JE_playSampleNum(S_CURSOR);

				curSel[curMenu]--;
				if (curSel[curMenu] < 2)
					curSel[curMenu] = menuChoices[curMenu];

				// if in front or rear weapon upgrade screen
				if (curMenu == MENU_UPGRADE_SUB &&
				    (curSel[MENU_UPGRADES] == 3 || curSel[MENU_UPGRADES] == 4))
				{
					player[0].items.weapon[curSel[MENU_UPGRADES]-3].power = temp_weapon_power[curSel[MENU_UPGRADE_SUB]-2];
					if (curSel[MENU_UPGRADES] == 4)
						player[0].weapon_mode = 1;
				}

				// if joystick config, skip disabled items when digital
				if (curMenu == MENU_JOYSTICK_CONFIG &&
				    joysticks > 0 &&
				    !joystick[joystick_config].analog &&
				    curSel[curMenu] == 5)
				{
					curSel[curMenu] = 3;
				}

				break;

			case SDL_SCANCODE_DOWN:
				keyboardUsed = true;
				lastDirection = 1;

				if (curMenu != MENU_DATA_CUBE_SUB)
					JE_playSampleNum(S_CURSOR);

				curSel[curMenu]++;
				if (curSel[curMenu] > menuChoices[curMenu])
					curSel[curMenu] = 2;

				// if in front or rear weapon upgrade screen
				if (curMenu == MENU_UPGRADE_SUB &&
				    (curSel[MENU_UPGRADES] == 3 || curSel[MENU_UPGRADES] == 4))
				{
					player[0].items.weapon[curSel[MENU_UPGRADES]-3].power = temp_weapon_power[curSel[MENU_UPGRADE_SUB]-2];
					if (curSel[MENU_UPGRADES] == 4)
						player[0].weapon_mode = 1;
				}

				// if in joystick config, skip disabled items when digital
				if (curMenu == MENU_JOYSTICK_CONFIG &&
				    joysticks > 0 &&
				    !joystick[joystick_config].analog &&
				    curSel[curMenu] == 4)
				{
					curSel[curMenu] = 6;
				}

				break;

			case SDL_SCANCODE_HOME:
				if (curMenu == MENU_DATA_CUBE_SUB)
					yLoc = 0;
				break;

			case SDL_SCANCODE_END:
				if (curMenu == MENU_DATA_CUBE_SUB)
					yLoc = (cube[currentCube].last_line - 9) * 12;
				break;

			case SDL_SCANCODE_LEFT:
				if (curMenu == MENU_JOYSTICK_CONFIG)
				{
					if (joysticks > 0)
					{
						switch (curSel[curMenu])
						{
							case 2:
								if (joystick_config == 0)
									joystick_config = joysticks;
								joystick_config--;
								break;
							case 3:
								joystick[joystick_config].analog = !joystick[joystick_config].analog;
								break;
							case 4:
								if (joystick[joystick_config].sensitivity == 0)
									joystick[joystick_config].sensitivity = 10;
								else
									joystick[joystick_config].sensitivity--;
								break;
							case 5:
								if (joystick[joystick_config].threshold == 0)
									joystick[joystick_config].threshold = 10;
								else
									joystick[joystick_config].threshold--;
								break;
							default:
								break;
						}
					}
				}

				if (curMenu == MENU_2_PLAYER_ARCADE)
				{
					switch (curSel[curMenu])
					{
					case 3:
					case 4:
						JE_playSampleNum(S_CURSOR);

						int temp = curSel[curMenu] - 3;
						do
						{
							if (joysticks == 0)
								inputDevice[temp == 0 ? 1 : 0] = inputDevice[temp]; // swap controllers
							if (inputDevice[temp] <= 1)
								inputDevice[temp] = 2 + joysticks;
							else
								inputDevice[temp]--;
						} while (inputDevice[temp] == inputDevice[temp == 0 ? 1 : 0]);
						break;
					}
				}

				if (curMenu == MENU_OPTIONS ||
				    curMenu == MENU_UPGRADE_SUB ||
				    curMenu == MENU_LIMITED_OPTIONS)
				{
					JE_playSampleNum(S_CURSOR);
				}

				switch (curMenu)
				{
				case 2:
				case 11:
					switch (curSel[curMenu])
					{
					case 4:
						JE_changeVolume(&tyrMusicVolume, -12, &fxVolume, 0);
						if (music_disabled)
						{
							music_disabled = false;
							restart_song();
						}
						break;
					case 5:
						JE_changeVolume(&tyrMusicVolume, 0, &fxVolume, -12);
						samples_disabled = false;
						break;
#if defined(__SWITCH__) || defined(__vita__)
					case 6:
						touch_sensitivity -= 12;
						if (touch_sensitivity < 0)
							touch_sensitivity = 0;
						break;
#endif
					}
					break;
				case 4:
					switch (curSel[MENU_UPGRADES])
					{
					case 3:
					case 4:
						if (leftPower)
							player[0].items.weapon[curSel[MENU_UPGRADES]-3].power = --temp_weapon_power[curSel[MENU_UPGRADE_SUB]-2];
						else
							JE_playSampleNum(S_CLINK);

						break;
					}
					break;
				}
				break;

			case SDL_SCANCODE_RIGHT:
				if (curMenu == MENU_JOYSTICK_CONFIG)
				{
					if (joysticks > 0)
					{
						switch (curSel[curMenu])
						{
							case 2:
								joystick_config++;
								joystick_config %= joysticks;
								break;
							case 3:
								joystick[joystick_config].analog = !joystick[joystick_config].analog;
								break;
							case 4:
								joystick[joystick_config].sensitivity++;
								joystick[joystick_config].sensitivity %= 11;
								break;
							case 5:
								joystick[joystick_config].threshold++;
								joystick[joystick_config].threshold %= 11;
								break;
							default:
								break;
						}
					}
				}

				if (curMenu == MENU_2_PLAYER_ARCADE)
				{
					switch (curSel[curMenu])
					{
					case 3:
					case 4:
						JE_playSampleNum(S_CURSOR);

						int temp = curSel[curMenu] - 3;
						do
						{
							if (joysticks == 0)
								inputDevice[temp == 0 ? 1 : 0] = inputDevice[temp]; // swap controllers
							if (inputDevice[temp] >= 2 + joysticks)
								inputDevice[temp] = 1;
							else
								inputDevice[temp]++;
						} while (inputDevice[temp] == inputDevice[temp == 0 ? 1 : 0]);
						break;
					}
				}

				if (curMenu == MENU_OPTIONS ||
				    curMenu == MENU_UPGRADE_SUB ||
				    curMenu == MENU_LIMITED_OPTIONS)
				{
					JE_playSampleNum(S_CURSOR);
				}

				switch (curMenu)
				{
				case 2:
				case 11:
					switch (curSel[curMenu])
					{
					case 4:
						JE_changeVolume(&tyrMusicVolume, 12, &fxVolume, 0);
						if (music_disabled)
						{
							music_disabled = false;
							restart_song();
						}
						break;
					case 5:
						JE_changeVolume(&tyrMusicVolume, 0, &fxVolume, 12);
						samples_disabled = false;
						break;
#if defined(__SWITCH__) || defined(__vita__)
					case 6:
						touch_sensitivity += 12;
						if (touch_sensitivity > TOUCH_SENS_MAX)
							touch_sensitivity = TOUCH_SENS_MAX;
						break;
#endif
					}
					break;
				case 4:
					switch (curSel[MENU_UPGRADES])
					{
					case 3:
					case 4:
						if (rightPower && rightPowerAfford)
							player[0].items.weapon[curSel[MENU_UPGRADES]-3].power = ++temp_weapon_power[curSel[MENU_UPGRADE_SUB]-2];
						else
							JE_playSampleNum(S_CLINK);

						break;
					}
					break;
				}
				break;

			default:
				break;
			}
		}

	} while (!(quit || gameLoaded || jumpSection));

#ifdef WITH_NETWORK
	if (!quit && isNetworkGame)
	{
		JE_barShade(VGAScreen, 3, 3, 316, 196);
		JE_barShade(VGAScreen, 1, 1, 318, 198);
		JE_dString(VGAScreen, 10, 160, "Waiting for other player.", SMALL_FONT_SHAPES);

		network_prepare(PACKET_WAITING);
		network_send(4);  // PACKET_WAITING

		while (true)
		{
			service_SDL_events(false);
			JE_showVGA();

			if (packet_in[0] && SDLNet_Read16(&packet_in[0]->data[0]) == PACKET_WAITING)
			{
				network_update();
				break;
			}

			network_update();
			network_check();

			SDL_Delay(16);
		}

		network_state_reset();
	}

	if (isNetworkGame)
	{
		while (!network_is_sync())
		{
			service_SDL_events(false);
			JE_showVGA();

			network_check();
			SDL_Delay(16);
		}
	}
#endif

	if (gameLoaded)
		fade_black(10);

	/* Keep the buy/sell screen centered until after the fade when jumping to
	   the next level.  Otherwise, reset the offset now. */
	if (!jumpSection)
		set_menu_centered(false);
}

void draw_ship_illustration(void)
{
	// full of evil hardcoding

	// ship
	{
		assert(player[0].items.ship > 0);

		const int sprite_id = (player[0].items.ship < COUNTOF(ships))  // shipedit ships get a default
		                      ? ships[player[0].items.ship].bigshipgraphic - 1
		                      : 31;

		const int ship_x[] = { 31, 0, 0, 0, 35, 31, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 36, 30 },
		          ship_y[] = { 36, 0, 0, 0, 33, 35, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 33, 30 };

		const int x = ship_x[sprite_id - 27],
		          y = ship_y[sprite_id - 27];

		// Gencore II's illustration (sprite 45, a 95px-wide uncompressed block) fills its
		// "background" with palette 162 -- the same colour (#241408) as the shop wall (226) -- so
		// it blends in everywhere except its square top-right corner, which overhangs the panel's
		// lighter bevel in 3 spots (window ~141-142,30-31 in 16:9) where those dark pixels show.
		// The raw sprite can't flag them transparent, so save the bevel and paint it back.
		Uint8 * const seg = (Uint8 *)VGAScreenSeg->pixels;
		const int segpitch = VGAScreenSeg->pitch;
		const bool patch_corner = (sprite_id == 45);
		Uint8 under[3] = { 0 };
		if (patch_corner)
		{
			under[0] = seg[(y    ) * segpitch + (x + 93)];
			under[1] = seg[(y    ) * segpitch + (x + 94)];
			under[2] = seg[(y + 1) * segpitch + (x + 94)];
		}

		blit_sprite(VGAScreenSeg, x, y, OPTION_SHAPES, sprite_id);

		if (patch_corner)
		{
			seg[(y    ) * segpitch + (x + 93)] = under[0];
			seg[(y    ) * segpitch + (x + 94)] = under[1];
			seg[(y + 1) * segpitch + (x + 94)] = under[2];
		}
	}

	// generator
	{
		assert(player[0].items.generator > 0 && player[0].items.generator < 7);

		const int sprite_id = (player[0].items.generator == 1)  // generator 1 and generator 2 have the same sprite
		                      ? player[0].items.generator + 15
		                      : player[0].items.generator + 14;

		const int generator_x[5] = { 62, 64, 67, 66, 63 },
		          generator_y[5] = { 84, 85, 86, 84, 97 };
		const int x = generator_x[sprite_id - 16],
		          y = generator_y[sprite_id - 16];

		blit_sprite(VGAScreenSeg, x, y, WEAPON_SHAPES, sprite_id);
	}

	const int weapon_sprites[60] =
	{
		-1,  0,  1,  2,  3,  4,  5,  6,  7,  8,
		 9, 10, 11, 21,  5, 13, -1, 14, 15,  0,
		14,  9,  8,  2, 15,  0, 13,  0,  8,  8,
		11,  1,  0,  0,  0,  0,  0,  0,  0,  0,
		 0,  2,  1,  0,  0,  1,  1,  1
	};

	// Each weapon id has a mount point in exactly one of these tables (the other holds -1); prefer
	// the slot's table but fall back to the weapon's real mount table so an endless cross-slot
	// weapon never indexes at [-1]. NortShip specials pin to the front. notes.md §Menus & shop.
	const int front_weapon_xy_list[60] =
	{
		 -1,  4,  9,  3,  8,  2,  5, 10,  1, -1,
		 -1, -1, -1,  7,  8, -1, -1,  0, -1,  4,
		  0, -1, -1,  3, -1,  4, -1,  4, -1, -1,
		 -1,  9,  0,  0,  0,  0,  0,  0,  0,  0,
		  0,  3,  9,  4,  4,  9,  9,  9
	};
	const int front_weapon_x[12] = { 59, 66, 66, 54, 61, 51, 58, 51, 61, 52, 53, 58 };
	const int front_weapon_y[12] = { 38, 53, 41, 36, 48, 35, 41, 35, 53, 41, 39, 31 };

	const int rear_weapon_xy_list[60] =
	{
		-1, -1, -1, -1, -1, -1, -1, -1, -1,  0,
		 1,  2,  3, -1,  4,  5, -1, -1,  6, -1,
		-1,  1,  0, -1,  6, -1,  5, -1,  0,  0,
		 3,  0,  0,  0,  0,  0,  0,  0,  0,  0,
		 0, -1, -1, -1, -1, -1, -1, -1
	};
	const int rear_weapon_x[7] = { 41, 27,  49,  43, 51, 39, 41 };
	const int rear_weapon_y[7] = { 92, 92, 113, 102, 97, 96, 76 };

	for (int slot = FRONT_WEAPON; slot <= REAR_WEAPON; ++slot)
	{
		const int id = player[0].items.weapon[slot].id;
		if (id <= 0 || id >= 60 || weapon_sprites[id] < 0)
			continue;

		const int fi = front_weapon_xy_list[id];  // front-mount index, or -1
		const int ri = rear_weapon_xy_list[id];   // rear-mount index, or -1

		// Unauthored special weapon (fill default in both tables): pin to front.
		const bool front_only = (fi == 0 && ri == 0);

		int use_fi = -1, use_ri = -1;
		if (front_only)
			use_fi = fi;
		else if (slot == FRONT_WEAPON)  // prefer front, fall back to rear
		{
			if (fi >= 0) use_fi = fi;
			else         use_ri = ri;
		}
		else  // REAR_WEAPON: prefer rear, fall back to front
		{
			if (ri >= 0) use_ri = ri;
			else         use_fi = fi;
		}

		int x, y;
		if (use_fi >= 0)
		{
			x = front_weapon_x[use_fi];
			y = front_weapon_y[use_fi];
		}
		else if (use_ri >= 0)
		{
			x = rear_weapon_x[use_ri];
			y = rear_weapon_y[use_ri];
		}
		else
		{
			continue;  // no illustration mount point for this weapon
		}

		blit_sprite(VGAScreenSeg, x, y, WEAPON_SHAPES, weapon_sprites[id]);
	}

	// sidekicks
	JE_drawItem(6, player[0].items.sidekick[LEFT_SIDEKICK], 3, 84);
	JE_drawItem(7, player[0].items.sidekick[RIGHT_SIDEKICK], 129, 84);

	// shield
	blit_sprite_hv(VGAScreenSeg, 28, 23, OPTION_SHAPES, 26, 15, shields[player[0].items.shield].mpwr - 10);
}

void load_cubes(void)
{
	for (int cube_slot = 0; cube_slot < cubeMax; ++cube_slot)
	{
		memset(cube[cube_slot].text, 0, sizeof(cube->text));

		load_cube(cube_slot, cubeList[cube_slot]);
	}
}

bool load_cube(int cube_slot, int cube_index)
{
	FILE *f = dir_fopen_die(data_dir(), cube_file, "rb");

	char buf[256];

	// seek to the cube
	while (cube_index > 0)
	{
		read_encrypted_pascal_string(buf, sizeof(buf), f);
		if (buf[0] == '*')
			--cube_index;
	}

	str_pop_int(&buf[4], &cube[cube_slot].face_sprite);
	--cube[cube_slot].face_sprite;

	read_encrypted_pascal_string(cube[cube_slot].title, sizeof(cube[cube_slot].title), f);
	read_encrypted_pascal_string(cube[cube_slot].header, sizeof(cube[cube_slot].header), f);

	uint line = 0, line_chars = 0, line_width = 0;

	// for each line of decrypted text, split the line into words
	// and add them individually to the lines of wrapped text
	for (; ; )
	{
		read_encrypted_pascal_string(buf, sizeof(buf), f);

		// end of data
		if (buf[0] == '*')
			break;

		// new paragraph
		if (strlen(buf) == 0)
		{
			if (line_chars == 0)
				line += 4;  // subsequent new paragaphs indicate 4-line break
			else
				++line;
			line_chars = 0;
			line_width = 0;

			continue;
		}

		uint word_start = 0;
		for (uint i = 0; ; ++i)
		{
			bool end_of_line = (buf[i] == '\0'),
			     end_of_word = end_of_line || (buf[i] == ' ');

			if (end_of_word)
			{
				buf[i] = '\0';

				char *word = &buf[word_start];
				word_start = i + 1;

				uint word_chars = strlen(word),
				     word_width = JE_textWidth(word, TINY_FONT);

				// word won't fit; no can do
				if (word_chars > cube_line_chars || word_width > cube_line_width)
					break;

				bool prepend_space = true;

				line_chars += word_chars + (prepend_space ? 1 : 0);
				line_width += word_width + (prepend_space ? 6 : 0);

				// word won't fit on current line; use next
				if (line_chars > cube_line_chars || line_width > cube_line_width)
				{
					++line;
					line_chars = word_chars;
					line_width = word_width;

					prepend_space = false;
				}

				// append word
				if (line < COUNTOF(cube->text))
				{
					if (prepend_space)
						strcat(cube[cube_slot].text[line], " ");
					strcat(cube[cube_slot].text[line], word);

					// track last line with text
					cube[cube_slot].last_line = line + 1;
				}
			}

			if (end_of_line)
				break;
		}
	}

	fclose(f);

	return true;
}

void JE_drawItem(JE_byte itemType, JE_word itemNum, JE_word x, JE_word y)
{
	JE_word tempW = 0;

	if (itemNum > 0)
	{
		switch (itemType)
		{
			case 2:
			case 3:
				tempW = weaponPort[itemNum].itemgraphic;
				break;
			case 5:
				tempW = powerSys[itemNum].itemgraphic;
				break;
			case 6:
			case 7:
				tempW = options[itemNum].itemgraphic;
				break;
			case 4:
				tempW = shields[itemNum].itemgraphic;
				break;
		}

		if (itemType == 1)
		{
			if (itemNum > 90)
			{
				shipGrPtr = &spriteSheet9;
				shipGr = JE_SGr(itemNum - 90, &shipGrPtr);
				blit_sprite2x2(VGAScreen, x, y, *shipGrPtr, shipGr);
			}
			else if (ships[itemNum].shipgraphic == 1)
			{
				// The Nort Ship's shipgraphic (1) is a sentinel, not a real sprite index -- the
				// gameplay draw (JE_playerMovement) special-cases it into a two-piece hull. Blitting
				// sprite 1 here (as for a normal ship) shows garbage, so draw the same two halves,
				// centred on x to match a normal ship's footprint (and gameplay's x-17/x+7 spacing).
				blit_sprite2x2(VGAScreen, x - 12, y, spriteSheet9, 220);
				blit_sprite2x2(VGAScreen, x + 12, y, spriteSheet9, 222);
			}
			else if (ships[itemNum].shipgraphic > 500)
			{
				blit_sprite2x2(VGAScreen, x, y, spriteSheetT2000, ships[itemNum].shipgraphic - 500);
			}
			else
			{
				blit_sprite2x2(VGAScreen, x, y, spriteSheet9, ships[itemNum].shipgraphic);
			}
		}
		else if (tempW > 0)
		{
			blit_sprite2x2(VGAScreen, x, y, shopSpriteSheet, tempW);
		}
	}
}

void JE_drawMenuHeader(void)
{
	switch (curMenu)
	{
		case MENU_DATA_CUBE_SUB:
			strcpy(tempStr, cube[curSel[MENU_DATA_CUBES]-2].header);
			break;
		case MENU_DATA_CUBES:
			strcpy(tempStr, menuInt[1][1]);
			break;
		case MENU_LOAD_SAVE:
			strcpy(tempStr, menuInt[3][performSave + 1]);
			break;
		default:
			if (curMenu == MENU_DEBUG_PLAY_LEVEL)
				strcpy(tempStr, debugMenuInt[0]);
			else
				strcpy(tempStr, menuInt[curMenu + 1][0]);
			break;
	}
	JE_dString(VGAScreen, 74 + JE_fontCenter(tempStr, FONT_SHAPES), 10, tempStr, FONT_SHAPES);
}

// Endless E-Shop: tint each buy row by WHAT IT IS, so related purchases share a colour and the
// player can read the menu at a glance. Banks are palette-1 (the shop palette; see notes.md
// "Menus & shop"): 12=green, 8=cyan, 4=red, 5=purple, 7=fiery red->yellow, 15=default gold. Keyed
// by menu row x (== curSel[MENU_ESHOP]); the row order is fixed in configure_endless_shop_menu().
static unsigned int endless_eshop_row_bank(JE_byte x)
{
	switch (x)
	{
	case 7: case 8: case 9:  return 12;  // Turbodrive / Overblast / Overdrive -- kill-fire BUFFS (green)
	case 4: case 5:          return  8;  // Reinforce / Extra Perk             -- permanent UPGRADES (cyan)
	case 6:                  return  4;  // Special Weapon                     -- OFFENSE (red)
	case 10: case 11:        return  5;  // Revive / Bomb                      -- held CONSUMABLES (purple)
	case 12:                 return  7;  // Gamble                             -- RISK (fiery red/yellow)
	default:                 return 15;  // Reroll / Sabotage / Done           -- neutral shop actions (gold)
	}
}

void JE_drawMenuChoices(void)
{
	JE_byte x;
	char *str;
	// Endless read-only perk list: the normal buy/sell menu, just drawn in a small font at tight rows.
	const bool perkList = (curMenu == MENU_PERKS && endlessPerkListMode);
	// The endless E-Shop holds many items, so it reuses the same compact small-font / tight-row list.
	const bool tightFont = perkList || (curMenu == MENU_ESHOP);

	for (x = 2; x <= menuChoices[curMenu]; x++)
	{
		int line_height = (curMenu == MENU_DEBUG_PLAY_LEVEL) ? 8 : tightFont ? 10 : 16;
		int tempY;
		if (curMenu == MENU_DEBUG_PLAY_LEVEL)
		{
			int index = x - 2;
			int row = index / 2;
			tempY = 38 + (row + 1) * line_height;
		}
		else
		{
			tempY = 38 + (x - 1) * line_height;
		}
		/* Extra spacing after "Start Level": the original's blank gap above "Quit
		 * Game"; with Debug Mode on it also offsets the debug block (items 7-9). */
		if (curMenu == MENU_FULL_GAME && x >= 7)
		{
			tempY += 16;
		}

		// Endless perk menu / E-Shop: a blank line separating the buys from the final Done row.
		if ((curMenu == MENU_PERKS || curMenu == MENU_ESHOP) && x == menuChoices[curMenu])
		{
			tempY += tightFont ? 10 : 16;
		}

		if (curMenu == MENU_2_PLAYER_ARCADE)
		{
			if (x > 3)
			{
				tempY += 16;
			}
			if (x > 4)
			{
				tempY += 16;
			}
		}

		if (!(curMenu == MENU_PLAY_NEXT_LEVEL &&
			x == menuChoices[curMenu]))
		{
			tempY -= line_height;
		}

		if (curMenu == MENU_MOUSE_CONFIG)
		{
			tempY += (x-2) * 8;
		}

		const char* entry = (curMenu == MENU_DEBUG_PLAY_LEVEL)
			? debugMenuInt[x - 1]
			: menuInt[curMenu + 1][x - 1];

		str = malloc(strlen(entry) + 2);
		if (curSel[curMenu] == x)
		{
			str[0] = '~';
			strcpy(str + 1, entry);
		}
		else
		{
			strcpy(str, entry);
		}

		unsigned int font = (curMenu == MENU_DEBUG_PLAY_LEVEL)
			? TINY_FONT : SMALL_FONT_SHAPES;
		int text_x = 166;
		if (curMenu == MENU_DEBUG_PLAY_LEVEL)
		{
			int index = x - 2;
			int col = index % 2;
			text_x = 165 + col * 80;
			JE_outTextAndDarken(VGAScreen, text_x, tempY, str, 15, 2, TINY_FONT);
		}
		else if (tightFont)
		{
			// Same buy/sell menu, small font: the shape font has no smaller size, so draw the rows
			// in TINY_FONT (perk list + endless E-Shop). The leading '~' still toggles the highlight.
			// E-Shop rows are tinted by category (endless_eshop_row_bank); the perk list stays gold.
			unsigned int bank = (curMenu == MENU_ESHOP) ? endless_eshop_row_bank(x) : 15;
			JE_outTextAndDarken(VGAScreen, text_x, tempY, str, bank, 2, TINY_FONT);
		}
		else
		{
			JE_dString(VGAScreen, text_x, tempY, str, font);
		}
		free(str);

		// Endless "gave up the level" outpost: grey out the rows locked to the launch-time choices
		// (E-Shop = item 2, Upgrade Ship = item 4) so the disabled items read as disabled. Dims only
		// the glyph pixels (no box over the background); item 6 (Start Level) relaunches, stays bright.
		if (endlessMode && endlessLockedSortie && curMenu == MENU_FULL_GAME && (x == 2 || x == 4))
			JE_dStringDarken(VGAScreen, text_x, tempY, entry, font);

		// Endless hardcore: NO saving of any kind -- grey out Load Game (item 2) and Save Game
		// (item 3) in the shop options submenu so the disabled rows read as disabled. (Items 2/3
		// sit above the Switch-only Touch row, so this holds on Switch too.)
		if (endlessMode && endlessHardcore && curMenu == MENU_OPTIONS && (x == 2 || x == 3))
			JE_dStringDarken(VGAScreen, text_x, tempY, entry, font);
	}
}

// Chart-a-Course monitor overlay: the course's modifiers on the planet monitor -- threats from the
// top-left in red, boons from the bottom-right in green, opposite corners so long lines never
// collide. Palette/shade facts in notes.md §Menus & shop.
#define ENDLESS_MODS_LEFT_X    22
#define ENDLESS_MODS_RIGHT_X  132
#define ENDLESS_MODS_TOP_Y     20
#define ENDLESS_MODS_BOTTOM_Y 159
#define ENDLESS_MODS_ROW_H      9

// Danger-grade slot under the map. The slot is asymmetric (the corner bulb eats its right end), so
// centre on the monitor window's centre (MENU_MONITOR_CENTER_X), not the slot midpoint, and every
// rank lines up -- the same slot/centre the shop cash total uses. notes.md §Menus & shop.
#define ENDLESS_RANK_CX        MENU_MONITOR_CENTER_X
#define ENDLESS_RANK_Y        173   // endlessModText draws the body AT this row (no +1 like DARKEN)

// Rank-letter tint, 0 (F) .. 10 (END): a green-to-red ramp. The easy grades are GREEN from bank 0 --
// the nav palette's clean green ramp (shades 3-7), the same green the boon mod rows use -- and C..END
// ride bank 15, the pale-yellow -> orange -> deep-red fire ramp. {bank, brightness}; brightness kept
// in [-2,+5] so the glyph's shades never leave the bank. (bank 8 was used for the greens before, but
// its shades 10-12 render brown/gray in this palette, not green.) Indexed by endlessCourseRankLevel,
// so this MUST stay as long as endless_mods.c's endlessRankName[]. notes.md §Menus & shop.
static const struct { unsigned int bank; int bright; } endlessRankHue[11] = {
	{  0,  0 },  // F     green        (bank 0 shade 7)
	{  0, -1 },  // E     green
	{  0, -2 },  // D     deep green
	{ 15,  5 },  // C     pale yellow
	{ 15,  4 },  // B     amber
	{ 15,  3 },  // A     orange
	{ 15,  2 },  // S     orange
	{ 15,  1 },  // S+    orange-red
	{ 15,  0 },  // S++   red-orange
	{ 15, -1 },  // S+++  red
	{ 15, -2 },  // END   darkest red -- off the letter scale, the 100th-zone finale alone
};

// Danger weight -> bank-15 brightness. Bands mirror the endlessModTable weights: nuisances
// (homing/swift/gravity, <=9), standard dangers (10..13), heavies (elite packs, overloads,
// the harsh self-curses, 14..24), and the apex tier (Apex/Legion/Rampage/a cursed fortune).
static int endlessThreatShade(int weight)
{
	if (weight >= 25) return -2;
	if (weight >= 14) return -1;
	if (weight >= 10) return 0;
	return 1;
}

// One overlay row: a 1px black 8-direction outline, then the tinted fill via JE_outTextAndDarken.
// FULL_SHADE can't be used -- its negative brightness is JE_outText's shadow sentinel, so deep-red
// tiers would render black. notes.md §Menus & shop.
static void endlessModText(int x, int y, const char *s, unsigned int bank, int brightness)
{
	for (int dy = -1; dy <= 1; ++dy)
		for (int dx = -1; dx <= 1; ++dx)
			if (dx != 0 || dy != 0)
				JE_outText(VGAScreen, x + dx, y + dy, s, 0, -1);
	JE_outTextAndDarken(VGAScreen, x, y, s, bank, brightness, TINY_FONT);
}

static void JE_drawEndlessCourseMods(void)
{
	if (!endlessMode || curMenu != MENU_PLAY_NEXT_LEVEL)
		return;
	const int course = curSel[MENU_PLAY_NEXT_LEVEL] - 2;
	if (course < 0 || course >= endlessCourseCount())
		return;

	// The course's danger grade, centred in the slot under the map: "RANK " in the help-line beige,
	// then the letter tinted by danger (green F -> red S+++, endlessRankHue). Both go through
	// endlessModText so they share ONE uniform black outline -- that keeps the red end from blending
	// into the red slot, and the shadow stays a single colour across the whole line. Shown for every
	// valid course, including Calm ones that have no mod rows below.
	const char *rank  = endlessCourseRank(course);
	const int   level = endlessCourseRankLevel(course);
	char full[16];
	snprintf(full, sizeof(full), "RANK %s", rank);
	const int rx = ENDLESS_RANK_CX - JE_textWidth(full, TINY_FONT) / 2;
	endlessModText(rx, ENDLESS_RANK_Y, "RANK ", 14, 1);
	if (level >= 0)
		endlessModText(rx + JE_textWidth("RANK ", TINY_FONT), ENDLESS_RANK_Y,
		               rank, endlessRankHue[level].bank, endlessRankHue[level].bright);

	EndlessCourseModRow rows[16];
	const int n = endlessCourseModRows(course, rows, (int)COUNTOF(rows));

	int boons = 0;
	for (int i = 0; i < n; ++i)
		if (!rows[i].hostile)
			++boons;

	int threat_y = ENDLESS_MODS_TOP_Y;
	int boon_y   = ENDLESS_MODS_BOTTOM_Y - (boons - 1) * ENDLESS_MODS_ROW_H;
	for (int i = 0; i < n; ++i)
	{
		if (rows[i].hostile)
		{
			if (threat_y > ENDLESS_MODS_BOTTOM_Y)
				break;  // no real course has this many rows; never spill off the monitor
			endlessModText(ENDLESS_MODS_LEFT_X, threat_y, rows[i].word,
			               15, endlessThreatShade(rows[i].weight));
			threat_y += ENDLESS_MODS_ROW_H;
		}
		else
		{
			endlessModText(ENDLESS_MODS_RIGHT_X - JE_textWidth(rows[i].word, TINY_FONT),
			               boon_y, rows[i].word, 0, 0);
			boon_y += ENDLESS_MODS_ROW_H;
		}
	}
}

// Draw the whole planet monitor at the current tempNavX/tempNavY. Pure draw, no
// animation state change, so it can be re-issued at an interpolated pan.
static void JE_drawNavMonitor(void)
{
	JE_byte x;

	fill_rectangle_xy(VGAScreen, 19, 16, 135, 169, 2);
	JE_drawNavLines(true);
	JE_drawNavLines(false);
	JE_drawDots();

	for (x = 0; x < 11; x++)
		JE_drawPlanet(x);

	// Loop mapPNum (entries actually written), not menuChoices-1: in endless menuChoices is
	// (course count + 2), so menuChoices-1 reads past mapPlanet[] and feeds JE_drawPlanet a garbage
	// planet number (crash). notes.md §Menus & shop.
	for (x = 0; x < mapPNum; x++)
	{
		if (mapPlanet[x] > 11)
			JE_drawPlanet(mapPlanet[x] - 1);
	}

	if (mapOrigin > 11)
		JE_drawPlanet(mapOrigin - 1);

	blit_sprite(VGAScreenSeg, 0, 0, OPTION_SHAPES, 28);  // navigation screen interface

	JE_drawEndlessCourseMods();  // endless: the highlighted course's threats/boons, on the monitor

	fill_rectangle_xy(VGAScreen, 314, 0, 319, 199, 230);
}

// Advance the lock-on pan (navX/navY easing toward the selected planet), the
// planet spin animation, and the travel-dot reveal by one menu logic tick.
static void JE_navScreenAdvance(void)
{
	if (curSel[MENU_PLAY_NEXT_LEVEL] < menuChoices[MENU_PLAY_NEXT_LEVEL])
	{
		const unsigned int origin_x_offset = sprite(PLANET_SHAPES, PGR[mapOrigin-1]-1)->width / 2,
		                   origin_y_offset = sprite(PLANET_SHAPES, PGR[mapOrigin-1]-1)->height / 2,
		                   dest_x_offset = sprite(PLANET_SHAPES, PGR[mapPlanet[curSel[MENU_PLAY_NEXT_LEVEL]-2] - 1]-1)->width / 2,
		                   dest_y_offset = sprite(PLANET_SHAPES, PGR[mapPlanet[curSel[MENU_PLAY_NEXT_LEVEL]-2] - 1]-1)->height / 2;

		newNavX = (planetX[mapOrigin-1] - origin_x_offset
		          + planetX[mapPlanet[curSel[MENU_PLAY_NEXT_LEVEL]-2] - 1] - dest_x_offset) / 2.0f;
		newNavY = (planetY[mapOrigin-1] - origin_y_offset
		          + planetY[mapPlanet[curSel[MENU_PLAY_NEXT_LEVEL]-2] - 1] - dest_y_offset) / 2.0f;
	}

	navX = navX + (newNavX - navX) / 2.0f;
	navY = navY + (newNavY - navY) / 2.0f;

	if (fabsf(newNavX - navX) < 1)
		navX = newNavX;
	if (fabsf(newNavY - navY) < 1)
		navY = newNavY;

	if (planetAniWait > 0)
	{
		planetAniWait--;
	}
	else
	{
		planetAni++;
		if (planetAni > 14)
			planetAni = 0;
		planetAniWait = 3;
	}

	if (currentDotWait > 0)
	{
		currentDotWait--;
	}
	else
	{
		if (currentDotNum < planetDots[curSel[MENU_PLAY_NEXT_LEVEL]-2])
			currentDotNum++;
		currentDotWait = 5;
	}
}

void JE_updateNavScreen(void)
{
	tempNavX = roundf(navX);
	tempNavY = roundf(navY);
	JE_drawNavMonitor();
	JE_navScreenAdvance();
}

// Draw the complete play-next-level frame (monitor at dispNavX/dispNavY plus menu
// chrome), mirroring the menu loop's per-tick draw so it can be re-issued per frame.
static void JE_navDrawFrame(JE_real dispNavX, JE_real dispNavY)
{
	tempNavX = roundf(dispNavX);
	tempNavY = roundf(dispNavY);
	JE_drawNavMonitor();
	JE_drawMainMenuHelpText();
	JE_drawMenuHeader();
	JE_drawMenuChoices();
	if (extraGame)
		JE_dString(VGAScreen, 170, 140, miscText[68 - 1], FONT_SHAPES);
	JE_drawScore();
}

// Present the planet monitor smoothly: redraw the whole frame every displayed frame
// at a pan interpolated from nav_smooth_prev_* to the current navX/navY. The caller
// has already advanced the pan this tick and issued setDelay(2).
static void JE_navScreenSmoothPresent(void)
{
	// Smooth Motion off: present the authoritative frame (already drawn at the
	// current position) once and let menuWaitWithSmoothCursor wait out the tick.
	if (!smoothMotion)
	{
		JE_mouseStart();
		JE_showVGA();
		JE_mouseReplace();
		return;
	}

	const float period = 2.0f * get_delay_period();  // setDelay(2) duration (ms)

	for (;;)
	{
		const Uint32 remaining = getDelayTicks();  // ms until the setDelay(2) target
		if (remaining == 0 || period <= 0.0f)
			break;

		float alpha = 1.0f - (float)remaining / period;
		if (alpha < 0.0f)
			alpha = 0.0f;
		else if (alpha > 1.0f)
			alpha = 1.0f;

		// Restore the clean VGAScreen2 backdrop before every redraw: the help text's
		// drop-shadow (blit_sprite_dark, halves brightness) is not idempotent, so
		// redrawing over the previous frame would progressively blacken it.
		memcpy(VGAScreen->pixels, VGAScreen2->pixels, (size_t)VGAScreen->pitch * VGAScreen->h);

		JE_navDrawFrame(nav_smooth_prev_x + (navX - nav_smooth_prev_x) * alpha,
		                nav_smooth_prev_y + (navY - nav_smooth_prev_y) * alpha);

		JE_mouseStart();
		JE_showVGA();
		JE_mouseReplace();

		if (!output_vsync)
			limit_render_fps();
		service_SDL_events(false);
	}
}

void JE_drawLines(SDL_Surface *surface, JE_boolean dark)
{
	JE_byte x, y;
	JE_integer tempX, tempY;
	JE_integer tempX2, tempY2;
	JE_word tempW, tempW2;

	tempX2 = -10;
	tempY2 = 0;

	tempW = 0;
	for (x = 0; x < 20; x++)
	{
		tempW += 15;
		tempX = tempW - tempX2;

		if (tempX > 18 && tempX < 135)
		{
			if (dark)
				JE_rectangle(surface, tempX + 1, 0, tempX + 1, 199, 32+3);
			else
				JE_rectangle(surface, tempX, 0, tempX, 199, 32+5);
		}
	}

	tempW = 0;
	for (y = 0; y < 20; y++)
	{
		tempW += 15;
		tempY = tempW - tempY2;

		if (tempY > 15 && tempY < 169)
		{
			if (dark)
				JE_rectangle(surface, 0, tempY + 1, 319, tempY + 1, 32+3);
			else
				JE_rectangle(surface, 0, tempY, 319, tempY, 32+5);

			tempW2 = 0;

			for (x = 0; x < 20; x++)
			{
				tempW2 += 15;
				tempX = tempW2 - tempX2;
				if (tempX > 18 && tempX < 135)
				{
					JE_pix3(surface, tempX, tempY, 32+6);
				}
			}
		}
	}
}

/* SYN: This was originally PROC drawlines... yes, there were two different procs called
   drawlines in different scopes in the same file. Dammit, Jason, why do you do this to me? */

void JE_drawNavLines(JE_boolean dark)
{
	JE_byte x, y;
	JE_integer tempX, tempY;
	JE_integer tempX2, tempY2;
	JE_word tempW, tempW2;

	tempX2 = tempNavX >> 1;
	tempY2 = tempNavY >> 1;

	tempW = 0;
	for (x = 1; x <= 20; x++)
	{
		tempW += 15;
		tempX = tempW - tempX2;

		if (tempX > 18 && tempX < 135)
		{
			if (dark)
				JE_rectangle(VGAScreen, tempX + 1, 16, tempX + 1, 169, 1);
			else
				JE_rectangle(VGAScreen, tempX, 16, tempX, 169, 5);
		}
	}

	tempW = 0;
	for (y = 1; y <= 20; y++)
	{
		tempW += 15;
		tempY = tempW - tempY2;

		if (tempY > 15 && tempY < 169)
		{
			if (dark)
				JE_rectangle(VGAScreen, 19, tempY + 1, 135, tempY + 1, 1);
			else
				JE_rectangle(VGAScreen, 8, tempY, 160, tempY, 5);

			tempW2 = 0;

			for (x = 0; x < 20; x++)
			{
				tempW2 += 15;
				tempX = tempW2 - tempX2;
				if (tempX > 18 && tempX < 135)
					JE_pix3(VGAScreen, tempX, tempY, 7);
			}
		}
	}
}

void JE_drawDots(void)
{
	JE_byte x, y;
	JE_integer tempX, tempY;

	for (x = 0; x < mapPNum; x++)
	{
		for (y = 0; y < planetDots[x]; y++)
		{
			tempX = planetDotX[x][y] - tempNavX + 66 - 2;
			tempY = planetDotY[x][y] - tempNavY + 85 - 2;
			if (tempX > 0 && tempX < 140 && tempY > 0 && tempY < 168)
				blit_sprite(VGAScreenSeg, tempX, tempY, OPTION_SHAPES, (x == curSel[MENU_PLAY_NEXT_LEVEL]-2 && y < currentDotNum) ? 30 : 29);  // navigation dots
		}
	}
}

void JE_drawPlanet(JE_byte planetNum)
{
	// PGR[], planetX[], planetY[] are all size 21. Guard against an out-of-range planet number
	// (e.g. from stale/out-of-bounds mapPlanet[] data) so an OOB PGR read can't yield a bad
	// sprite index and fault -- the whole crash class flagged in endlessBetweenLevels.
	if (planetNum >= 21)
		return;

	JE_integer tempZ = PGR[planetNum]-1,
	           tempX = planetX[planetNum] + 66 - tempNavX - sprite(PLANET_SHAPES, tempZ)->width / 2,
	           tempY = planetY[planetNum] + 85 - tempNavY - sprite(PLANET_SHAPES, tempZ)->height / 2;

	if (tempX > -7 && tempX + sprite(PLANET_SHAPES, tempZ)->width < 170 && tempY > 0 && tempY < 160)
	{
		if (PAni[planetNum])
			tempZ += planetAni;

		blit_sprite_dark(VGAScreenSeg, tempX + 3, tempY + 3, PLANET_SHAPES, tempZ, false);
		blit_sprite(VGAScreenSeg, tempX, tempY, PLANET_SHAPES, tempZ);  // planets
	}
}

void JE_scaleBitmap(SDL_Surface *dst_bitmap, const SDL_Surface *src_bitmap,  int x1, int y1, int x2, int y2)
{
	/* This function scales one screen and writes the result to another.
	 *  The only code that calls it is the code run when you select 'ship
	 * specs' from the main menu.
	 *
	 * Originally this used fixed point math.  I haven't seen that in ages :).
	 * But we're well past the point of needing that.*/

	assert(src_bitmap != NULL && dst_bitmap != NULL);
	assert(x1 >= 0 && y1 >= 0 && x2 < dst_bitmap->w && y2 < dst_bitmap->h);

	int w = x2 - x1 + 1,
	    h = y2 - y1 + 1;
	float base_skip_w = src_bitmap->pitch / (float)w,
	      base_skip_h = src_bitmap->h / (float)h;
	float cumulative_skip_w, cumulative_skip_h;

	//Okay, it's time to loop through and add bits of A to a rectangle in B
	Uint8 *dst = dst_bitmap->pixels;  /* 8-bit specific */
	const Uint8 *src, *src_w;  /* 8-bit specific */

	dst += y1 * dst_bitmap->pitch + x1;
	cumulative_skip_h = 0;

	for (int i = 0; i < h; i++)
	{
		//this sets src to the beginning of our desired line
		src = src_w = (Uint8 *)(src_bitmap->pixels) + (src_bitmap->w * ((unsigned int)cumulative_skip_h));
		cumulative_skip_h += base_skip_h;
		cumulative_skip_w = 0;

		for (int j = 0; j < w; j++)
		{
			//copy and move pointers
			*dst = *src;
			dst++;

			cumulative_skip_w += base_skip_w;
			src = src_w + ((unsigned int)cumulative_skip_w); //value is floored
		}

		dst += dst_bitmap->pitch - w;
	}
}

void JE_initWeaponView(void)
{
	fill_rectangle_xy(VGAScreen, 8, 8, 144, 177, 0);

	player[0].sidekick[LEFT_SIDEKICK].x = 72 - 15;
	player[0].sidekick[LEFT_SIDEKICK].y = 120;
	player[0].sidekick[RIGHT_SIDEKICK].x = 72 + 15;
	player[0].sidekick[RIGHT_SIDEKICK].y = 120;

	player[0].x = 72;
	player[0].y = 110;
	player[0].delta_x_shot_move = 0;
	player[0].delta_y_shot_move = 0;
	player[0].last_x_explosion_follow = 72;
	player[0].last_y_explosion_follow = 110;
	power = 500;
	lastPower = 500;
	menu_power_prev = menu_power_cur = 500;  // reset gauge interpolation state
	menu_power_bg_valid = false;             // background re-captured on first draw

	memset(shotAvail, 0, sizeof(shotAvail));

	memset(shotRepeat, 1, sizeof(shotRepeat));
	memset(shotMultiPos, 0, sizeof(shotMultiPos));

	initialize_starfield();
}

void JE_computeDots(void)
{
	JE_integer tempX, tempY;
	JE_longint distX, distY;
	JE_byte x, y;

	for (x = 0; x < mapPNum; x++)
	{
		distX = (int)(planetX[mapPlanet[x]-1]) - (int)(planetX[mapOrigin-1]);
		distY = (int)(planetY[mapPlanet[x]-1]) - (int)(planetY[mapOrigin-1]);
		tempX = abs(distX) + abs(distY);

		if (tempX != 0)
			planetDots[x] = roundf(sqrtf(sqrtf((distX * distX) + (distY * distY)))) - 1;
		else
			planetDots[x] = 0;

		if (planetDots[x] > 10)
			planetDots[x] = 10;

		for (y = 0; y < planetDots[x]; y++)
		{
			tempX = JE_partWay(planetX[mapOrigin-1], planetX[mapPlanet[x]-1], planetDots[x], y);
			tempY = JE_partWay(planetY[mapOrigin-1], planetY[mapPlanet[x]-1], planetDots[x], y);
			/* ??? Why does it use temp? =P */
			planetDotX[x][y] = tempX;
			planetDotY[x][y] = tempY;
		}
	}
}

JE_integer JE_partWay(JE_integer start, JE_integer finish, JE_byte dots, JE_byte dist)
{
	return (finish - start) / (dots + 2) * (dist + 1) + start;
}

void JE_doShipSpecs(void)
{
	/* This function is called whenever you select 'ship specs' in the
	 * game menu.  It draws the nice green tech screen and scales it onto
	 * the main window.  To do this we need two temp buffers, so we're going
	 * to use VGAScreen and game_screen for the purpose (making things more
	 * complex than they would be if we just malloc'd, but faster)
	 *
	 * Originally the whole system was pretty oddly designed.  So I changed it.
	 * Currently drawFunkyScreen creates the image, scaleInPicture draws it,
	 * and doFunkyScreen ties everything together.  Before it was more like
	 * an oddly designed, unreusable, global sharing hierarchy. */

	//create the image we want
	wait_noinput(true, true, true);
	JE_drawShipSpecs(game_screen, VGAScreen2);

	//reset VGAScreen2, which we clobbered
	JE_loadPic(VGAScreen2, 1, false);

	//draw it
	JE_playSampleNum(S_SPRING);
	JE_scaleInPicture(VGAScreen, game_screen);
	wait_input(true, true, true);
}

// E-Shop cost highlight: the game's cash colour (bank 1) reads cleanly under the shop's palette 1.
// notes.md §Menus & shop.
#define ENDLESS_COST_HL_BANK   1
#define ENDLESS_COST_HL_BRIGHT 6

// Chart-a-Course runs under palette 18, where bank 1 clashes; bank 14 (the course text's own
// colour) reads correctly there. notes.md §Menus & shop.
#define ENDLESS_COURSE_HL_BANK   14
#define ENDLESS_COURSE_HL_BRIGHT 6

// Right edge both the Chart-a-Course payout and the E-Shop cost right-align to, flush against the
// help bar's right end (description left, price right). notes.md §Menus & shop.
#define ENDLESS_COURSE_PAYOUT_RIGHT 305

void JE_drawMainMenuHelpText(void)
{
	char tempStr[67];
	char costStr[24] = "";  // endless: an amount drawn highlighted after tempStr (E-Shop cost / perk decline)
	JE_byte temp;

	temp = curSel[curMenu] - 2;

	if (curMenu == MENU_DEBUG_PLAY_LEVEL)
	{
		if (curSel[curMenu] == menuChoices[MENU_DEBUG_PLAY_LEVEL])
		{
			memcpy(tempStr, mainMenuHelp[12 - 1], sizeof(tempStr));
		}
		else
		{
			snprintf(tempStr, sizeof(tempStr), "Debug: Play the level %s.",
				debugMenuInt[curSel[curMenu] - 1]);
		}
	}
	else if (curMenu == MENU_JOYSTICK_CONFIG) // joystick settings menu help
	{
		const int help[16] = { 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 24, 11 };
		memcpy(tempStr, mainMenuHelp[help[curSel[curMenu] - 2]], sizeof(tempStr));
	}
	else if (curMenu < MENU_PLAY_NEXT_LEVEL || curMenu >= MENU_2_PLAYER_ARCADE)
	{
		if (debugMode && curMenu == MENU_FULL_GAME && curSel[curMenu] == 7)
		{
			snprintf(tempStr, sizeof(tempStr), "Debug: equip your ship.");
		}
		else if (debugMode && curMenu == MENU_FULL_GAME && curSel[curMenu] == 8)
		{
			snprintf(tempStr, sizeof(tempStr), "Debug: select a level.");
		}
		else if (debugMode && curMenu == MENU_FULL_GAME && curSel[curMenu] == 9)
		{
			memcpy(tempStr, mainMenuHelp[5 - 1], sizeof(tempStr));
		}
		else if (debugMode &&
		         (curMenu == MENU_1_PLAYER_ARCADE || curMenu == MENU_2_PLAYER_ARCADE || curMenu == MENU_SUPER_TYRIAN) &&
		         curSel[curMenu] >= menuChoices[curMenu] - 2)
		{
			// The inserted Debug entries have no menuHelp[] slot (0 would index
			// mainMenuHelp[-1]); supply their text, and read Quit's from its original slot.
			if (curSel[curMenu] == menuChoices[curMenu] - 2)
				snprintf(tempStr, sizeof(tempStr), "Debug: equip your ship.");
			else if (curSel[curMenu] == menuChoices[curMenu] - 1)
				snprintf(tempStr, sizeof(tempStr), "Debug: select a level.");
			else
				memcpy(tempStr, mainMenuHelp[menuHelp[curMenu][menuChoices[curMenu] - 4] - 1], sizeof(tempStr));
		}
		else if (endlessMode && endlessLockedSortie && curMenu == MENU_FULL_GAME && curSel[curMenu] == 6)
		{
			snprintf(tempStr, sizeof(tempStr), "Retry the level you gave up.");
		}
		else if (endlessMode && endlessLockedSortie && curMenu == MENU_FULL_GAME &&
		         (curSel[curMenu] == 2 || curSel[curMenu] == 4))
		{
			// Locked "gave up the level" outpost: the E-Shop and loadout are frozen.
			snprintf(tempStr, sizeof(tempStr), "Locked - you gave up the level.");
		}
		else if (endlessMode && curMenu == MENU_FULL_GAME && curSel[curMenu] == 2)
		{
			snprintf(tempStr, sizeof(tempStr), "Open the E-Shop.");
		}
		else if (endlessMode && curMenu == MENU_FULL_GAME && curSel[curMenu] == 3)
		{
			snprintf(tempStr, sizeof(tempStr), "Review the perks you've acquired.");
		}
		else if (curMenu == MENU_ESHOP)
		{
			// The E-Shop has no menuHelp[] row, so supply each entry's help directly. The
			// description goes in tempStr; the price goes in costStr, drawn HIGHLIGHTED after it.
			switch (curSel[MENU_ESHOP])
			{
			case 2:
				SDL_strlcpy(tempStr, "Reroll the shop stock.", sizeof(tempStr));
				snprintf(costStr, sizeof(costStr), "$%ld", endlessRerollPrice());
				break;
			case 4:
				if (endlessHullMaxed())
					SDL_strlcpy(tempStr, "Hull upgrades maxed out.", sizeof(tempStr));
				else
				{
					SDL_strlcpy(tempStr, "Permanently boost max armor.", sizeof(tempStr));
					snprintf(costStr, sizeof(costStr), "$%d", endlessHullPrice());
				}
				break;
			case 7:
				if (endlessBuffKindBought() == ENDLESS_BUFF_KIND_TURBODRIVE)
					SDL_strlcpy(tempStr, "Turbodrive active for next sector.", sizeof(tempStr));
				else if (endlessBuffKindBought() != ENDLESS_BUFF_KIND_NONE)
					SDL_strlcpy(tempStr, "A kill-fire buff is already bought.", sizeof(tempStr));
				else if (endlessBuffOnCooldown())
					snprintf(tempStr, sizeof(tempStr), "Drives recharge in %d sector(s).", endlessBuffCooldownLeft());
				else
				{
					SDL_strlcpy(tempStr, "Each kill quickens your guns.", sizeof(tempStr));
					snprintf(costStr, sizeof(costStr), "$%ld", endlessTurbodrivePrice());
				}
				break;
			case 8:
				if (endlessBuffKindBought() == ENDLESS_BUFF_KIND_OVERBLAST)
					SDL_strlcpy(tempStr, "Overblast active for next sector.", sizeof(tempStr));
				else if (endlessBuffKindBought() != ENDLESS_BUFF_KIND_NONE)
					SDL_strlcpy(tempStr, "A kill-fire buff is already bought.", sizeof(tempStr));
				else if (endlessBuffOnCooldown())
					snprintf(tempStr, sizeof(tempStr), "Drives recharge in %d sector(s).", endlessBuffCooldownLeft());
				else
				{
					SDL_strlcpy(tempStr, "Each kill stacks your shot damage.", sizeof(tempStr));
					snprintf(costStr, sizeof(costStr), "$%ld", endlessOverblastPrice());
				}
				break;
			case 9:
				if (endlessBuffKindBought() == ENDLESS_BUFF_KIND_OVERDRIVE)
					SDL_strlcpy(tempStr, "Overdrive active for next sector.", sizeof(tempStr));
				else if (endlessBuffKindBought() != ENDLESS_BUFF_KIND_NONE)
					SDL_strlcpy(tempStr, "A kill-fire buff is already bought.", sizeof(tempStr));
				else if (endlessBuffOnCooldown())
					snprintf(tempStr, sizeof(tempStr), "Drives recharge in %d sector(s).", endlessBuffCooldownLeft());
				else
				{
					SDL_strlcpy(tempStr, "Turbodrive + Overblast together.", sizeof(tempStr));
					snprintf(costStr, sizeof(costStr), "$%ld", endlessOverdrivePrice());
				}
				break;
			case 6:
				if (endlessLastGrantedSpecial()[0] != '\0')
					snprintf(tempStr, sizeof(tempStr), "Got %s!  Buy another:", endlessLastGrantedSpecial());
				else
					SDL_strlcpy(tempStr, "Buy a random special weapon.", sizeof(tempStr));
				snprintf(costStr, sizeof(costStr), "$%ld", endlessSpecialPrice());
				break;
			case 11:  // Buy Bomb
				if (endlessBombFull())
					SDL_strlcpy(tempStr, "Bomb stockpile is full (10).", sizeof(tempStr));
				else
				{
					SDL_strlcpy(tempStr, "Stock an extra bomb.", sizeof(tempStr));
					snprintf(costStr, sizeof(costStr), "$%ld", endlessBombPrice());
				}
				break;
			case 10:  // Revive Token
				if (endlessReviveArmed())
					SDL_strlcpy(tempStr, "A revive is armed - survive one death.", sizeof(tempStr));
				else
				{
					SDL_strlcpy(tempStr, "Survive one lethal hit.", sizeof(tempStr));
					snprintf(costStr, sizeof(costStr), "$%ld", endlessRevivePrice());
				}
				break;
			case 5:  // Extra Perk
				SDL_strlcpy(tempStr, "Pick a bonus perk now.", sizeof(tempStr));
				snprintf(costStr, sizeof(costStr), "$%ld", endlessExtraPerkPrice());
				break;
			case 3:  // Sabotage Sector
				if (endlessCleanseCharges() > 0)
					snprintf(tempStr, sizeof(tempStr), "%d strip(s) queued.  Buy more:", endlessCleanseCharges());
				else
					SDL_strlcpy(tempStr, "Strip the next sector's worst danger.", sizeof(tempStr));
				snprintf(costStr, sizeof(costStr), "$%ld", endlessCleansePrice());
				break;
			case 12:  // Gamble
				if (endlessGambleResult()[0] != '\0')
					SDL_strlcpy(tempStr, endlessGambleResult(), sizeof(tempStr));
				else
					SDL_strlcpy(tempStr, "Roll the dice: fortune or ruin.", sizeof(tempStr));
				snprintf(costStr, sizeof(costStr), "$%ld", endlessGamblePrice());
				break;
			default:
				SDL_strlcpy(tempStr, "Return to the buy/sell menu.", sizeof(tempStr));
				break;
			}
		}
		else if (curMenu == MENU_PERKS)
		{
			if (endlessPerkListMode)
			{
				// Read-only perk list: "Done" returns; each perk row shows its stack count + effect.
				if (curSel[MENU_PERKS] == menuChoices[MENU_PERKS])
					SDL_strlcpy(tempStr, "Return to the buy/sell menu.", sizeof(tempStr));
				else
				{
					const int id = perkListId[curSel[MENU_PERKS] - 2];
					if (id >= 0)
						snprintf(tempStr, sizeof(tempStr), "Owned %d.  %s", endlessPerkGetOwned(id), endlessPerkDesc(id));
					else
						SDL_strlcpy(tempStr, "You haven't earned any perks yet.", sizeof(tempStr));
				}
			}
			// Perk pick: show the hovered perk's effect (+ owned count), or the decline's cash.
			else if (curSel[MENU_PERKS] == menuChoices[MENU_PERKS])
			{
				SDL_strlcpy(tempStr, "Take no perk this zone for a cash bonus.", sizeof(tempStr));
				snprintf(costStr, sizeof(costStr), "+$%ld", endlessPerkDeclineBonus());
			}
			else
				SDL_strlcpy(tempStr, endlessPerkChoiceDesc(curSel[MENU_PERKS] - 2), sizeof(tempStr));
		}
		else if (customWeaponEnabled && curMenu == MENU_UPGRADES && curSel[curMenu] >= 9)
		{
			// Custom weapons inserts a "Custom" row at item 9 and pushes Done to item 10.
			// menuHelp[MENU_UPGRADES] has no entries for these shifted rows (temp 7 would
			// read Done's slot, temp 8 would index mainMenuHelp[-1]), so supply them here.
			if (curSel[curMenu] == 9)
				SDL_strlcpy(tempStr, "Create and equip a custom weapon.", sizeof(tempStr));
			else
				memcpy(tempStr, mainMenuHelp[12 - 1], sizeof(tempStr));  // Done
		}
#if defined(__SWITCH__) || defined(__vita__)
		else if ((curMenu == MENU_OPTIONS || curMenu == MENU_LIMITED_OPTIONS) && curSel[curMenu] >= 6)
		{
			// Switch inserts a Touch Sensitivity row at item 6; items below it shift down one, so
			// read each shifted item's ORIGINAL help slot (temp-1) and supply touch's own text.
			if (curSel[curMenu] == 6)
				SDL_strlcpy(tempStr, "Touchscreen ship control sensitivity.", sizeof(tempStr));
			else
				memcpy(tempStr, mainMenuHelp[(menuHelp[curMenu][temp - 1]) - 1], sizeof(tempStr));
		}
#endif
		else
		{
			memcpy(tempStr, mainMenuHelp[(menuHelp[curMenu][temp]) - 1], sizeof(tempStr));
		}
	}
	else if (curMenu == MENU_KEYBOARD_CONFIG &&
	         curSel[MENU_KEYBOARD_CONFIG] == 10)
	{
		memcpy(tempStr, mainMenuHelp[25-1], sizeof(tempStr));
	}
	else if (leftPower || rightPower)
	{
		memcpy(tempStr, mainMenuHelp[24-1], sizeof(tempStr));
	}
	else if (endlessMode && curMenu == MENU_PLAY_NEXT_LEVEL && temp < endlessCourseCount())
	{
		SDL_strlcpy(tempStr, endlessCourseHelp(temp), sizeof(tempStr));
		// '~' is a brightness toggle in this font renderer, never drawn (see fonthand.c), so keep
		// it out of format strings -- an earlier "~$%ld" here shifted the palette bank and corrupted
		// the digits rather than printing a tilde. notes.md §General pitfalls.
		snprintf(costStr, sizeof(costStr), "$%ld", endlessCoursePayout(temp));
	}
	else if (temp == menuChoices[curMenu] - 2 ||
	         (curMenu == MENU_DATA_CUBES && cubeMax == 0))
	{
		memcpy(tempStr, mainMenuHelp[12-1], sizeof(tempStr));
	}
	else
	{
		memcpy(tempStr, mainMenuHelp[17 + curMenu - 3], sizeof(tempStr));
	}
	
	JE_textShade(VGAScreen, 10, 187, tempStr, 14, 1, DARKEN);
	// Endless: draw the price/payout in a highlight colour so it stands out. The Chart a Course
	// screen runs under a different palette (see the defines above), so it gets its own palette-safe
	// pair instead of the E-Shop's -- and the payout is RIGHT-ALIGNED to the bar there (reward flush
	// right, opposite the left-aligned tier/description). Other cost lines sit just after their text.
	if (costStr[0] != '\0')
	{
		const int hlBank   = (curMenu == MENU_PLAY_NEXT_LEVEL) ? ENDLESS_COURSE_HL_BANK   : ENDLESS_COST_HL_BANK;
		const int hlBright = (curMenu == MENU_PLAY_NEXT_LEVEL) ? ENDLESS_COURSE_HL_BRIGHT : ENDLESS_COST_HL_BRIGHT;
		const int afterText = 10 + JE_textWidth(tempStr, TINY_FONT) + 5;
		int cost_x = afterText;
		if (curMenu == MENU_PLAY_NEXT_LEVEL || curMenu == MENU_ESHOP)
		{
			cost_x = ENDLESS_COURSE_PAYOUT_RIGHT - JE_textWidth(costStr, TINY_FONT);
			if (cost_x < afterText)  // description unusually long -- keep a gap, never overlap
				cost_x = afterText;
		}
		JE_textShade(VGAScreen, cost_x, 187, costStr, hlBank, hlBright, DARKEN);
	}

	// Endless: show the run's seed on the E-Shop help line, right-aligned opposite "Open the
	// E-Shop." so it's always visible from the outpost. Bank/brightness tuned by eye to read as
	// secondary chrome (not a price).
	if (endlessMode && curMenu == MENU_FULL_GAME && curSel[curMenu] == 2)
	{
		char seedStr[16 + ENDLESS_SEED_MAXLEN];
		snprintf(seedStr, sizeof(seedStr), "Seed: %s", endlessSeedString());
		const int afterText = 10 + JE_textWidth(tempStr, TINY_FONT) + 8;
		int seed_x = ENDLESS_COURSE_PAYOUT_RIGHT - JE_textWidth(seedStr, TINY_FONT);
		if (seed_x < afterText)  // never overlap the help text on the left
			seed_x = afterText;
		JE_textShade(VGAScreen, seed_x, 187, seedStr, 14, 3, DARKEN);
	}
}

JE_boolean JE_saveRequest(JE_byte slot, const char *savename)
{
	bool save_selected = true, done = false;

	JE_clearKeyboard();
	JE_wipeKey();
	wait_noinput(true, true, true);

	JE_barShade(VGAScreen, 65, 55, 255, 155);

	while (!done)
	{
		Uint8 col = 8;
		int colC = 1;

		do
		{
			service_SDL_events(true);
			setDelay(4);

			blit_sprite(VGAScreen, 50, 50, OPTION_SHAPES, 35);  // message box
			JE_textShade(VGAScreen, 70, 66, miscText[68], 0, 5, FULL_SHADE); // Are you sure you want to save?
			JE_textShade(VGAScreen, 74, 90, miscText[1], 12, 1, FULL_SHADE); // Save name:
			JE_textShade(VGAScreen, 135, 90, savename, 12, 1, FULL_SHADE);
			JE_textShade(VGAScreen, 74, 100, miscText[69], 12, 1, FULL_SHADE); // Original save:
			JE_textShade(VGAScreen, 140, 100, saveFiles[slot-1].name, 12, 1, FULL_SHADE);

			col += colC;
			if (col > 8 || col < 2)
				colC = -colC;

			int temp_x, temp_c;

			temp_x = 54 + 45 - (JE_textWidth(miscText[9], FONT_SHAPES) / 2);
			temp_c = save_selected ? col - 12 : -5;

			JE_outTextAdjust(VGAScreen, temp_x, 128, miscText[9], 15, temp_c, FONT_SHAPES, true);

			temp_x = 149 + 45 - (JE_textWidth(miscText[10], FONT_SHAPES) / 2);
			temp_c = !save_selected ? col - 12 : -5;

			JE_outTextAdjust(VGAScreen, temp_x, 128, miscText[10], 15, temp_c, FONT_SHAPES, true);

			if (has_mouse)
			{
				JE_mouseStart();
				JE_showVGA();
				JE_mouseReplace();
			}
			else
			{
				JE_showVGA();
			}

			// not wait_delay(): keep the cursor at display rate for the rest of the tick
			menuWaitWithSmoothCursor();

			push_joysticks_as_keyboard();
			service_SDL_events(false);

		} while (!newkey && !mousedown);

		if (mousedown)
		{
			if (lastmouse_y > 123 && lastmouse_y < 149)
			{
				if (lastmouse_x > 56 && lastmouse_x < 142)
				{
					save_selected = true;
					done = true;
				}
				else if (lastmouse_x > 151 && lastmouse_x < 237)
				{
					save_selected = false;
					done = true;
				}
			}
			mousedown = false;
		}
		else if (newkey)
		{
			switch (lastkey_scan)
			{
				case SDL_SCANCODE_LEFT:
				case SDL_SCANCODE_RIGHT:
				case SDL_SCANCODE_TAB:
					save_selected = !save_selected;
					JE_playSampleNum(S_CURSOR);
					break;
				case SDL_SCANCODE_RETURN:
				case SDL_SCANCODE_SPACE:
					done = true;
					break;
				case SDL_SCANCODE_ESCAPE:
					save_selected = false;
					done = true;
					break;
				default:
					break;
			}
		}
	}

	JE_playSampleNum(save_selected ? S_SELECT : S_CLICK);
	return save_selected;
}

JE_boolean JE_quitRequest(void)
{
	bool quit_selected = true, done = false;

	JE_clearKeyboard();
	JE_wipeKey();
	wait_noinput(true, true, true);

	JE_barShade(VGAScreen, 65, 55, 255, 155);

	while (!done)
	{
		Uint8 col = 8;
		int colC = 1;

		do
		{
			service_SDL_events(true);
			setDelay(4);

			blit_sprite(VGAScreen, 50, 50, OPTION_SHAPES, 35);  // message box
			JE_textShade(VGAScreen, 70, 60, miscText[28], 0, 5, FULL_SHADE);
			JE_helpBox(VGAScreen, 70, 90, miscText[30], 30);

			col += colC;
			if (col > 8 || col < 2)
				colC = -colC;

			int temp_x, temp_c;

			temp_x = 54 + 45 - (JE_textWidth(miscText[9], FONT_SHAPES) / 2);
			temp_c = quit_selected ? col - 12 : -5;

			JE_outTextAdjust(VGAScreen, temp_x, 128, miscText[9], 15, temp_c, FONT_SHAPES, true);

			temp_x = 149 + 45 - (JE_textWidth(miscText[10], FONT_SHAPES) / 2);
			temp_c = !quit_selected ? col - 12 : -5;

			JE_outTextAdjust(VGAScreen, temp_x, 128, miscText[10], 15, temp_c, FONT_SHAPES, true);

			if (has_mouse)
			{
				JE_mouseStart();
				JE_showVGA();
				JE_mouseReplace();
			}
			else
			{
				JE_showVGA();
			}

			// not wait_delay(): keep the cursor at display rate for the rest of the tick
			menuWaitWithSmoothCursor();

			push_joysticks_as_keyboard();
			service_SDL_events(false);

		} while (!newkey && !mousedown);

		if (mousedown)
		{
			if (lastmouse_y > 123 && lastmouse_y < 149)
			{
				if (lastmouse_x > 56 && lastmouse_x < 142)
				{
					quit_selected = true;
					done = true;
				}
				else if (lastmouse_x > 151 && lastmouse_x < 237)
				{
					quit_selected = false;
					done = true;
				}
			}
			mousedown = false;
		}
		else if (newkey)
		{
			switch (lastkey_scan)
			{
				case SDL_SCANCODE_LEFT:
				case SDL_SCANCODE_RIGHT:
				case SDL_SCANCODE_TAB:
					quit_selected = !quit_selected;
					JE_playSampleNum(S_CURSOR);
					break;
				case SDL_SCANCODE_RETURN:
				case SDL_SCANCODE_SPACE:
					done = true;
					break;
				case SDL_SCANCODE_ESCAPE:
					quit_selected = false;
					done = true;
					break;
				default:
					break;
			}
		}
	}

	JE_playSampleNum(quit_selected ? S_SPRING : S_CLICK);

#ifdef WITH_NETWORK
	if (isNetworkGame && quit_selected)
	{
		network_prepare(PACKET_QUIT);
		network_send(4);  // PACKET QUIT

		network_tyrian_halt(0, true);
	}
#endif

	return quit_selected;
}

void JE_genItemMenu(JE_byte itemNum)
{
	menuChoices[MENU_UPGRADE_SUB] = itemAvailMax[itemAvailMap[itemNum - 2] - 1] + 2;

	temp3 = 2;
	temp2 = *playeritem_map(&player[0].items, itemNum - 2);

	strcpy(menuInt[5][0], menuInt[2][itemNum - 1]);

	for (tempW = 0; tempW < itemAvailMax[itemAvailMap[itemNum - 2] - 1]; tempW++)
	{
		temp = itemAvail[itemAvailMap[itemNum - 2] - 1][tempW];
		switch (itemNum)
		{
		case 2:
			strcpy(tempStr, ships[temp].name);
			break;
		case 3:
		case 4:
			strcpy(tempStr, weaponPort[temp].name);
			break;
		case 5:
			strcpy(tempStr, shields[temp].name);
			break;
		case 6:
			strcpy(tempStr, powerSys[temp].name);
			break;
		case 7:
		case 8:
			strcpy(tempStr, options[temp].name);
			break;
		}
		if (temp == temp2)
		{
			temp3 = tempW + 2;
		}
		strcpy(menuInt[5][tempW], tempStr);
	}

	strcpy(menuInt[5][tempW], miscText[13]);

	curSel[MENU_UPGRADE_SUB] = temp3;
}

void JE_scaleInPicture(SDL_Surface *dst, const SDL_Surface *src)
{
	// JE_scaleBitmap samples at pitch / w; pitch is always vga_width (the real surface
	// width). The loop must reach vga_width so the final frame samples 1:1; capping at
	// LEGACY_WIDTH would compress the full-width row and squish the picture.
	for (int i = 2; i <= vga_width / 2; i += 2)
	{
		if (JE_anyButton())
			break;

		JE_scaleBitmap(dst, src, vga_width / 2 - i, 0, vga_width / 2 + i - 1,
			100 + roundf(i * 200.0f / vga_width) - 1);
		JE_showVGA();

		SDL_Delay(1);
	}
}

void JE_drawScore(void)
{
	char cl[24];
	if (curMenu == MENU_UPGRADE_SUB)
	{
		sprintf(cl, "%d", JE_cashLeft());
		// Centre on the monitor slot at the same row the main shop draws the cash total (see the
		// MENU_MONITOR_CENTER_X / y172 draw in JE_menuFunction), so the readout stays put instead
		// of jumping to a fixed left edge when the weapon-sim submenu takes over the display.
		JE_textShade(VGAScreen, MENU_MONITOR_CENTER_X - JE_textWidth(cl, TINY_FONT) / 2, 172, cl, 1, 6, DARKEN);
	}
}

/*
 * Helper used by debug and normal level selection. Mirrors the
 * behaviour of the "Next Level" menu and ensures that all variables
 * required for a level jump are updated consistently.
 */
static void select_level(JE_word section, JE_byte file_num)
{
	mainLevel = (JE_byte)section;
	if (file_num != 0)
	{
		lvlFileNum = file_num;
		// Make the exact file survive JE_loadMap's section rescan (which would reset lvlFileNum to
		// the section's first ']L'), so a debug/level pick of a section's non-first cut actually loads.
		forcedLvlFileNum = file_num;
	}
	nextLevel = mainLevel;
	jumpSection = true;
}

/* ---- Debug level browser: return-to-outpost snapshot -----------------------------------
 * The three ENGAGE mini-games (** ALE **, TIME WAR, SQUADRON) are dead ends in the campaign
 * script: their ']L' next-level pointer is the episode's END GAME section, and their failure
 * path reloads the "LAST LEVEL" backup save. Both are right when you fly there off the nav
 * map at the end of an episode -- but a level picked out of the debug browser has neither a
 * finished episode nor a matching backup save behind it, so either way the player is dumped
 * at level 1 of the next episode. Snapshot where the jump came from (and the loadout ']e'
 * is about to overwrite with the ENGAGE Stalker) so JE_main can hand the outpost back. */
static struct
{
	bool armed;                        // a browser pick is in flight -- good for one level only
	JE_byte episode, section;          // where the jump was made from
	JE_byte lvlFile;
	Player players[COUNTOF(player)];   // full loadout: ship, weapons, cash, armor
	JE_boolean superTyrian, onePlayerAction, twoPlayerMode;
}
debugJump;

/* Arm the snapshot; call right before the browser's select_level(), and before any
 * JE_initEpisode() it does, so the recorded episode is the one being left. */
static void select_debug_level_capture(void)
{
	debugJump.episode = (JE_byte)episodeNum;
	debugJump.section = mainLevel;
	debugJump.lvlFile = lvlFileNum;
	memcpy(debugJump.players, player, sizeof(debugJump.players));
	debugJump.superTyrian = superTyrian;
	debugJump.onePlayerAction = onePlayerAction;
	debugJump.twoPlayerMode = twoPlayerMode;
	debugJump.armed = true;
}

bool debugLevelJumpTake(void)
{
	const bool armed = debugJump.armed;
	debugJump.armed = false;  // always disarm: one level, so it can never apply to the next one
	return armed;
}

void debugLevelJumpReturn(void)
{
	if (debugJump.episode != episodeNum)
		JE_initEpisode(debugJump.episode);  // reloads level + item data; set the level fields after

	memcpy(player, debugJump.players, sizeof(debugJump.players));
	superTyrian     = debugJump.superTyrian;      // ']e' forced these on for the mini-game
	onePlayerAction = debugJump.onePlayerAction;
	twoPlayerMode   = debugJump.twoPlayerMode;

	mainLevel = debugJump.section;
	saveLevel = mainLevel;
	nextLevel = mainLevel;
	lvlFileNum = debugJump.lvlFile;
	forcedLvlFileNum = 0;  // the browser's one-shot file override died with the mini-game
}

/* Map a screen point to the absolute row index of the scrolled list, or -1. */
static int level_row_at(int mx, int my, int px0, int px1, int items_top,
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

/* A typed digit (0-9) from a scancode, or -1. Both the number row and the keypad. */
static int scancode_digit(int sc)
{
	if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)     return sc - SDL_SCANCODE_1 + 1;
	if (sc == SDL_SCANCODE_0)                             return 0;
	if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return sc - SDL_SCANCODE_KP_1 + 1;
	if (sc == SDL_SCANCODE_KP_0)                          return 0;
	return -1;
}

/* Endless-only: the SECTOR modifiers (dangers / boons that define a zone) the debug screen can
 * toggle onto the jumped-to zone. The four PERSONAL kill-fire buffs (Turbodrive / Overdrive and
 * their evil mirrors) live in endlessDebugBuffMods below, on the "Perks & Buffs" page. */
static const struct { Uint64 bit; const char *name; } endlessDebugSectorMods[] = {
	{ ENDLESS_MOD_FORTIFIED,   "Fortified"      },
	{ ENDLESS_MOD_FRENZY,      "Frenzy"         },
	{ ENDLESS_MOD_SWIFT,       "Swift"          },
	{ ENDLESS_MOD_DEVASTATING, "Devastating"    },
	{ ENDLESS_MOD_FRAGILE,     "Fragile"        },
	{ ENDLESS_MOD_BOUNTY,      "Bounty"         },
	{ ENDLESS_MOD_ELITEPACK,   "Elite Pack"     },
	{ ENDLESS_MOD_APEX,        "Apex Swarm"     },
	{ ENDLESS_MOD_LEGION,      "Legion"         },
	{ ENDLESS_MOD_ENRAGE,      "Enrage"         },
	{ ENDLESS_MOD_KAMIKAZE,    "Kamikaze (mid)" },
	{ ENDLESS_MOD_HOMING,      "Homing (light)" },
	{ ENDLESS_MOD_GRAVITY,     "Gravity Well"   },
	{ ENDLESS_MOD_GRAVITY_OMNI,"Gravity (omni)" },  // omni pulls along a random heading; toggle alone or with Gravity Well
	{ ENDLESS_MOD_OVERCHARGE,  "Overcharged"    },
	{ ENDLESS_MOD_DILATION,    "Time Dilation"  },
	{ ENDLESS_MOD_FAVOR,       "Merchant Favor" },
	{ ENDLESS_MOD_CURSED,      "Cursed Bounty"  },
	{ ENDLESS_MOD_NOCHAMP,     "No Champions"   },  // boon: champions demoted to elites
	{ ENDLESS_MOD_NOELITE,     "No Elites"      },  // boon: no elite/champion tier at all (supersedes No Champions)
	{ ENDLESS_MOD_OVERCLOCK,   "Overclock"      },
	{ ENDLESS_MOD_SLIPSTREAM,  "Slipstream"     },
	{ ENDLESS_MOD_OVERLOAD,    "Overload"       },
	{ ENDLESS_MOD_WARP,        "Warp Speed"     },
	{ ENDLESS_MOD_TOPSY,       "Topsy-Turvy"    },  // fork: upside-down screen (boss-style controls)
	{ ENDLESS_MOD_SLUGGISH,    "Sluggish Ship"  },  // fork: slowed movement (kbd/mouse/touch)
	{ ENDLESS_MOD_SHIELDLESS,  "No Shield Regen" }, // fork: shields never recharge
	{ ENDLESS_MOD_DEADGEN,     "Dead Generator" },  // fork: no shields + starved main gun (super-rare)
	// Gamble-only next-sector effects, also toggleable here for zone-jump testing. NITRO/OVERHEAT
	// normally ride with OVERCHARGE / TURBODRIVE (toggle those too for the full "deal"); here each
	// is isolable. All four are read straight from endlessActiveMods in-level, so the jump applies them.
	{ ENDLESS_MOD_MARKED,      "Marked (boss+)" },
	{ ENDLESS_MOD_NITRO,       "Nitro (1-hit)"  },
	{ ENDLESS_MOD_OVERHEAT,    "Overheat DoT"   },
	{ ENDLESS_MOD_DUD,         "Dud Bombs"      },
	{ ENDLESS_MOD_RAMPAGE,     "Rampage (ram!)" },  // the original brutal Kamikaze; also the ~1/5000 gamble outcome
};

/* The four PERSONAL kill-fire mods -- the two boons and their two evil mirrors -- grouped on the
 * debug "Perks & Buffs" page (they buff/debuff YOU, not the sector). */
static const struct { unsigned bit; const char *name; } endlessDebugBuffMods[] = {
	{ ENDLESS_MOD_TURBODRIVE,  "Turbodrive"    },
	{ ENDLESS_MOD_OVERBLAST,    "Overblast"      },
	{ ENDLESS_MOD_OVERDRIVE,    "Overdrive"      },
	{ ENDLESS_MOD_BACKFIRE,     "Backfire"       },
	{ ENDLESS_MOD_BURNOUT,      "Burnout"        },
	{ ENDLESS_MOD_MISFIRE,      "Misfire"        },
};

/* All levels across every installed episode, for the endless base-level selector. */
#define ENDLESS_BASE_MAX 256
static int     endlessBaseEp[ENDLESS_BASE_MAX];
static JE_word endlessBaseSec[ENDLESS_BASE_MAX];
static JE_byte endlessBaseFile[ENDLESS_BASE_MAX];
static char    endlessBaseName[ENDLESS_BASE_MAX][18];
static int     endlessBaseCount;

// Gather every episode's ]L levels into one list (mirrors load_debug_levels, but across all
// episodes and remembering each level's episode so the jump can switch to it).
static void endlessLoadAllLevels(void)
{
	endlessBaseCount = 0;
	for (int ep = 1; ep <= EPISODE_MAX && endlessBaseCount < ENDLESS_BASE_MAX; ep++)
	{
		if (!episodeAvail[ep - 1])
			continue;
		const unsigned int levelFileCount = JE_levelFileCount(ep);
		if (levelFileCount == 0)
			continue;

		char fname[16];
		snprintf(fname, sizeof(fname), "levels%d.dat", ep);
		FILE *f = dir_fopen_warn(data_dir(), fname, "rb");
		if (f == NULL)
			continue;

		JE_word section = 0;
		long end = ftell_eof(f);
		char s[256];
		while (ftell(f) < end && endlessBaseCount < ENDLESS_BASE_MAX)
		{
			read_encrypted_pascal_string(s, sizeof(s), f);
			if (s[0] == '*')
				section++;
			if (s[0] == ']' && s[1] == 'L')
			{
				const int fileNum = atoi(s + 25);
				if (fileNum < 1 || (unsigned int)fileNum > levelFileCount)
					continue;

				endlessBaseEp[endlessBaseCount] = ep;
				endlessBaseSec[endlessBaseCount] = section;

				char name_buf[10];
				SDL_strlcpy(name_buf, s + 13, sizeof(name_buf));
				size_t len = strlen(name_buf);
				while (len > 0 && name_buf[len - 1] == ' ')
					name_buf[--len] = '\0';
				SDL_strlcpy(endlessBaseName[endlessBaseCount], name_buf, sizeof(endlessBaseName[0]));

				endlessBaseFile[endlessBaseCount] = (JE_byte)fileNum;
				endlessBaseCount++;
			}
		}
		fclose(f);
	}
}

/* The dedicated endless debug jump screen: a scrollable form of rows -- Zone (type a number),
 * Base Level (Random or any level, scroll with L/R), each modifier (toggle), then Start. Enter
 * launches; returns true if launched (select_level armed the jump) or false if cancelled. */
static bool endlessDebugScreen(void)
{
	const JE_byte startEp = episodeNum;
	endlessLoadAllLevels();

	SDL_Surface *temp_surface = VGAScreen;
	VGAScreen = VGAScreenSeg;

	const int pw = 248;
	const int px0 = (LEGACY_WIDTH - pw) / 2;
	const int px1 = px0 + pw - 1;
	const int py0 = 12;
	const int py1 = vga_height - 12;
	const int title_h = 15;
	const int items_top = py0 + title_h + 3;
	const int items_bottom = py1 - 10;
	const int row_h = 10;
	const int pageRows = (items_bottom - items_top) / row_h;
	const int mid_x = (px0 + px1) / 2;

	enum {
		C_PANEL_BG = 0xF1, C_TITLE_BG = 0xF3, C_DIVIDER = 0xF6,
		C_EDGE_HI  = 0xFB, C_EDGE_LO  = 0xF4, C_SEL_BAR = 0xF5
	};

	const int NSEC   = (int)COUNTOF(endlessDebugSectorMods);
	const int NBUF   = (int)COUNTOF(endlessDebugBuffMods);
	const int NPERKS = endlessPerkCount();
	const int NGAM   = endlessGambleOutcomeCount();

	// Three pages, built each frame from row "kinds" so paging needs no fixed ROW_* offsets:
	//   page 0 "Sectors":         Zone, Base, the sector danger/boon mods, Start.
	//   page 1 "Perks & Buffs":   the four personal kill-fire buffs, the perks, Start.
	//   page 2 "Gamble Outcomes": every gamble outcome (Space/L-R/click fires it), Start.
	enum { RK_ZONE, RK_BASE, RK_SECMOD, RK_BUFMOD, RK_PERK, RK_GAMBLE, RK_LAUNCH };
	char dbgGambleMsg[48];  // last gamble outcome fired on this screen (shown in the footer on page 2)
	dbgGambleMsg[0] = '\0';

	int      dbgZone = endlessRunDepth + 1;
	bool     dbgZoneTyped = false;
	int      dbgBase = -1;             // -1 = Random, else index into endlessBase*
	Uint64   dbgMods = endlessActiveMods | endlessPendingMods();  // sector + personal-buff bits (pre-toggled)
	int      dbgPerks[32];             // owned stacks per perk, pre-loaded from the run (toggle/stack here)
	for (int i = 0; i < NPERKS && i < 32; ++i)
		dbgPerks[i] = endlessPerkGetOwned(i);

	int  page = 0;
	int  selected = 0, scrollTop = 0;
	int  prev_mx = mouse_x, prev_my = mouse_y;
	bool chosen = false, done = false;

	wait_noinput(false, false, true);
	newkey = newmouse = false;

	while (!done)
	{
		// Build the current page's row list (kinds -> concrete rows), so paging needs no fixed offsets.
		struct { int kind, idx; } rows[8 + 64];
		int rowCount = 0;
		if (page == 0)
		{
			rows[rowCount].kind = RK_ZONE; rows[rowCount].idx = 0; ++rowCount;
			rows[rowCount].kind = RK_BASE; rows[rowCount].idx = 0; ++rowCount;
			for (int i = 0; i < NSEC; ++i) { rows[rowCount].kind = RK_SECMOD; rows[rowCount].idx = i; ++rowCount; }
		}
		else if (page == 1)
		{
			for (int i = 0; i < NBUF; ++i)   { rows[rowCount].kind = RK_BUFMOD; rows[rowCount].idx = i; ++rowCount; }
			for (int i = 0; i < NPERKS; ++i) { rows[rowCount].kind = RK_PERK;   rows[rowCount].idx = i; ++rowCount; }
		}
		else
		{
			for (int i = 0; i < NGAM; ++i)   { rows[rowCount].kind = RK_GAMBLE; rows[rowCount].idx = i; ++rowCount; }
		}
		rows[rowCount].kind = RK_LAUNCH; rows[rowCount].idx = 0; ++rowCount;

		if (selected > rowCount - 1)
			selected = rowCount - 1;
		if (selected < 0)
			selected = 0;

		const int visibleRows = (pageRows < rowCount) ? pageRows : rowCount;

		if (selected < scrollTop)
			scrollTop = selected;
		else if (selected >= scrollTop + visibleRows)
			scrollTop = selected - visibleRows + 1;
		if (scrollTop > rowCount - visibleRows)
			scrollTop = rowCount - visibleRows;
		if (scrollTop < 0)
			scrollTop = 0;

		fill_rectangle_xy(VGAScreen, px0, py0, px1, py1, C_PANEL_BG);
		fill_rectangle_xy(VGAScreen, px0, py0, px1, py0, C_EDGE_HI);
		fill_rectangle_xy(VGAScreen, px0, py0, px0, py1, C_EDGE_HI);
		fill_rectangle_xy(VGAScreen, px0, py1, px1, py1, C_EDGE_LO);
		fill_rectangle_xy(VGAScreen, px1, py0, px1, py1, C_EDGE_LO);
		fill_rectangle_xy(VGAScreen, px0 + 1, py0 + 1, px1 - 1, py0 + title_h - 1, C_TITLE_BG);
		fill_rectangle_xy(VGAScreen, px0 + 1, py0 + title_h, px1 - 1, py0 + title_h, C_DIVIDER);
		draw_font_hv_shadow(VGAScreen, mid_x, py0 + 3, page == 0 ? "ENDLESS  -  ZONE JUMP" : page == 1 ? "ENDLESS  -  PERKS AND BUFFS" : "ENDLESS  -  GAMBLE OUTCOMES", normal_font, centered, 15, 4, true, 1);

		for (int vis = 0; vis < visibleRows; ++vis)
		{
			const int i = scrollTop + vis;
			const int ry = items_top + vis * row_h;
			const bool sel = (i == selected);

			if (sel)
				fill_rectangle_xy(VGAScreen, px0 + 3, ry - 1, px1 - 3, ry + row_h - 2, C_SEL_BAR);

			char buf[48];
			switch (rows[i].kind)
			{
			case RK_ZONE:
				snprintf(buf, sizeof(buf), "Zone:   %d", dbgZone);
				draw_font_hv_shadow(VGAScreen, px0 + 8, ry, buf, small_font, left_aligned, 15, sel ? 5 : -1, true, 1);
				break;
			case RK_BASE:
				if (dbgBase < 0)
					SDL_strlcpy(buf, "Base:   Random", sizeof(buf));
				else
					snprintf(buf, sizeof(buf), "Base:   %s  (Ep %d)", endlessBaseName[dbgBase], endlessBaseEp[dbgBase]);
				draw_font_hv_shadow(VGAScreen, px0 + 8, ry, buf, small_font, left_aligned, 15, sel ? 5 : -1, true, 1);
				break;
			case RK_LAUNCH:
				draw_font_hv_shadow(VGAScreen, px0 + 8, ry, "> Start Zone", small_font, left_aligned, 15, sel ? 6 : 2, true, 1);
				break;
			case RK_PERK:
			{
				const int p = rows[i].idx;
				const int n = dbgPerks[p], mx = endlessPerkMaxStack(p);
				if (mx > 1)
					snprintf(buf, sizeof(buf), "%s %s  %d/%d", n > 0 ? "ON" : "- ", endlessPerkName(p), n, mx);
				else
					snprintf(buf, sizeof(buf), "%s %s", n > 0 ? "ON" : "- ", endlessPerkName(p));
				draw_font_hv_shadow(VGAScreen, px0 + 14, ry, buf, small_font, left_aligned, 15, n > 0 ? 6 : (sel ? 5 : -3), true, 1);
				break;
			}
			case RK_BUFMOD:
			{
				const int m = rows[i].idx;
				const bool on = (dbgMods & endlessDebugBuffMods[m].bit) != 0;
				snprintf(buf, sizeof(buf), "%s %s", on ? "ON" : "- ", endlessDebugBuffMods[m].name);
				draw_font_hv_shadow(VGAScreen, px0 + 14, ry, buf, small_font, left_aligned, 15, on ? 6 : (sel ? 5 : -3), true, 1);
				break;
			}
			case RK_GAMBLE:
				snprintf(buf, sizeof(buf), "%s", endlessGambleOutcomeName(rows[i].idx));
				draw_font_hv_shadow(VGAScreen, px0 + 14, ry, buf, small_font, left_aligned, 15, sel ? 6 : -3, true, 1);
				break;
			default:  // RK_SECMOD
			{
				const int m = rows[i].idx;
				const bool on = (dbgMods & endlessDebugSectorMods[m].bit) != 0;
				snprintf(buf, sizeof(buf), "%s %s", on ? "ON" : "- ", endlessDebugSectorMods[m].name);
				draw_font_hv_shadow(VGAScreen, px0 + 14, ry, buf, small_font, left_aligned, 15, on ? 6 : (sel ? 5 : -3), true, 1);
				break;
			}
			}
		}

		if (rowCount > visibleRows)
		{
			const int track_top = items_top - 1;
			const int track_bot = items_top + visibleRows * row_h - 2;
			const int track_h = track_bot - track_top;
			fill_rectangle_xy(VGAScreen, px1 - 4, track_top, px1 - 3, track_bot, C_EDGE_LO);
			int thumb_h = track_h * visibleRows / rowCount;
			if (thumb_h < 4)
				thumb_h = 4;
			const int denom = rowCount - visibleRows;
			const int thumb_y = track_top + (denom > 0 ? (track_h - thumb_h) * scrollTop / denom : 0);
			fill_rectangle_xy(VGAScreen, px1 - 4, thumb_y, px1 - 3, thumb_y + thumb_h, C_EDGE_HI);
		}

		if (page == 2 && dbgGambleMsg[0] != '\0')  // show the last fired outcome's result on the Gamble page
			draw_font_hv(VGAScreen, mid_x, py1 - 9, dbgGambleMsg, small_font, centered, 15, 6);
		else
			draw_font_hv(VGAScreen, mid_x, py1 - 9, page == 2 ? "Space/Enter Fire   Tab Page   Esc Back" : "Tab Page   L/R Change   Enter Start   Esc Back", small_font, centered, 15, -3);

		mouseCursor = MOUSE_POINTER_NORMAL;
		JE_mouseStart();
		JE_showVGA();
		JE_mouseReplace();
		push_joysticks_as_keyboard();
		service_SDL_events(true);

#if defined(__SWITCH__) || defined(__vita__)
		// Y (Switch) / Square (Vita) pages this screen just like Tab. Menus only deliver
		// confirm/cancel/directions from a controller (push_joysticks_as_keyboard), so this
		// face button isn't bound to any action -- read it raw with local edge state and
		// synthesize a Tab. Both ports report it as button 3 (Switch A/B/X/Y = 0/1/2/3;
		// Vita triangle/circle/cross/square = 0/1/2/3). poll_joysticks (above) already ran
		// SDL_JoystickUpdate this tick. Guard on !newkey so a real key still wins.
		{
			static bool page_btn_was;
			const bool down = joysticks > 0 && joystick[0].handle != NULL &&
			                  SDL_JoystickGetButton(joystick[0].handle, 3) != 0;
			if (down && !page_btn_was && !newkey)
			{
				newkey = true;
				lastkey_scan = SDL_SCANCODE_TAB;
			}
			page_btn_was = down;
		}
#endif

		/* wheel moves the selection; hover highlights; left-click acts on the row (toggle /
		 * cycle / +zone / start); right-click cancels. */
		if (mouse_scroll != 0)
		{
			selected -= mouse_scroll;
			if (selected < 0)
				selected = 0;
			if (selected > rowCount - 1)
				selected = rowCount - 1;
			mouse_scroll = 0;
		}
		if (mouse_x != prev_mx || mouse_y != prev_my)
		{
			const int hov = level_row_at(mouse_x, mouse_y, px0, px1, items_top, row_h, visibleRows, scrollTop, rowCount);
			if (hov >= 0)
				selected = hov;
		}
		prev_mx = mouse_x;
		prev_my = mouse_y;
		if (newmouse)
		{
			if (lastmouse_but == SDL_BUTTON_RIGHT)
			{
				newkey = true;
				lastkey_scan = SDL_SCANCODE_ESCAPE;
			}
			else
			{
				const int r = level_row_at(lastmouse_x, lastmouse_y, px0, px1, items_top, row_h, visibleRows, scrollTop, rowCount);
				if (r >= 0)
				{
					selected = r;
					newkey = true;   // launch the Start row, otherwise "adjust" the row (=RIGHT)
					lastkey_scan = (rows[r].kind == RK_LAUNCH) ? SDL_SCANCODE_RETURN : SDL_SCANCODE_RIGHT;
				}
			}
			newmouse = false;
		}

		if (newkey)
		{
			const int selKind = rows[selected].kind;
			const int digit = scancode_digit(lastkey_scan);
			if (digit >= 0 && selKind == RK_ZONE)   // digits type a zone number (only on the Zone row)
			{
				if (!dbgZoneTyped)
				{
					dbgZone = digit;
					dbgZoneTyped = true;
				}
				else if (dbgZone < 1000)
					dbgZone = dbgZone * 10 + digit;
			}
			else switch (lastkey_scan)
			{
			case SDL_SCANCODE_TAB:
				page = (page + 1) % 3;   // cycle Sectors -> Perks & Buffs -> Gamble Outcomes
				selected = 0;
				scrollTop = 0;
				break;
			case SDL_SCANCODE_UP:
				selected = (selected == 0) ? rowCount - 1 : selected - 1;
				break;
			case SDL_SCANCODE_DOWN:
				selected = (selected + 1) % rowCount;
				break;
			case SDL_SCANCODE_BACKSPACE:
				if (selKind == RK_ZONE)
				{
					dbgZone /= 10;
					dbgZoneTyped = true;
				}
				break;
			case SDL_SCANCODE_LEFT:
			case SDL_SCANCODE_RIGHT:
			{
				const int dir = (lastkey_scan == SDL_SCANCODE_RIGHT) ? 1 : -1;
				if (selKind == RK_ZONE)
				{
					dbgZone += dir;
					if (dbgZone < 1)
						dbgZone = 1;
					if (dbgZone > 9999)
						dbgZone = 9999;
					dbgZoneTyped = true;
				}
				else if (selKind == RK_BASE)
				{
					dbgBase += dir;                       // -1 (Random) .. count-1, wrapping
					if (dbgBase < -1)
						dbgBase = endlessBaseCount - 1;
					else if (dbgBase > endlessBaseCount - 1)
						dbgBase = -1;
				}
				else if (selKind == RK_SECMOD)
					dbgMods ^= endlessDebugSectorMods[rows[selected].idx].bit;
				else if (selKind == RK_BUFMOD)
					dbgMods ^= endlessDebugBuffMods[rows[selected].idx].bit;
				else if (selKind == RK_PERK)
				{
					const int p = rows[selected].idx, mx = endlessPerkMaxStack(p);
					dbgPerks[p] += dir;                          // L/R adjust the stack, wrapping at the ends
					if (dbgPerks[p] < 0)
						dbgPerks[p] = mx;
					else if (dbgPerks[p] > mx)
						dbgPerks[p] = 0;
				}
				else if (selKind == RK_GAMBLE)                   // L/R (or click) fires the selected outcome
				{
					endlessForceGambleOutcome(rows[selected].idx);
					SDL_strlcpy(dbgGambleMsg, endlessGambleResult(), sizeof dbgGambleMsg);
				}
				break;
			}
			case SDL_SCANCODE_SPACE:
				if (selKind == RK_SECMOD)
				{
					dbgMods ^= endlessDebugSectorMods[rows[selected].idx].bit;
					break;
				}
				if (selKind == RK_BUFMOD)
				{
					dbgMods ^= endlessDebugBuffMods[rows[selected].idx].bit;
					break;
				}
				if (selKind == RK_PERK)
				{
					const int p = rows[selected].idx;   // Space bumps the stack up, wrapping to 0 past max
					if (++dbgPerks[p] > endlessPerkMaxStack(p))
						dbgPerks[p] = 0;
					break;
				}
				if (selKind == RK_GAMBLE)               // Space fires the selected gamble outcome
				{
					endlessForceGambleOutcome(rows[selected].idx);
					SDL_strlcpy(dbgGambleMsg, endlessGambleResult(), sizeof dbgGambleMsg);
					break;
				}
				if (selKind != RK_LAUNCH)
					break;                       // Space on Zone / Base does nothing
				/* fall through: Space on the Start row launches */
			case SDL_SCANCODE_RETURN:
				if (selKind == RK_GAMBLE)  // Enter on a gamble row fires it; only the Start row launches
				{
					endlessForceGambleOutcome(rows[selected].idx);
					SDL_strlcpy(dbgGambleMsg, endlessGambleResult(), sizeof dbgGambleMsg);
					break;
				}
				endlessRunDepth = (dbgZone > 0) ? dbgZone - 1 : 0;  // jump to the typed zone
				endlessActiveMods = dbgMods | endlessPendingMods();  // apply the chosen combo + personal buffs + any fired next-sector gamble mods
				for (int p = 0; p < NPERKS; ++p)                     // apply the perk stacks
					endlessPerkSetOwned(p, dbgPerks[p]);
				if (dbgBase < 0)
				{
					const JE_byte sec = endlessPickNextLevel();  // random endless-safe level (switches episode)
					if (episodeNum != startEp)
						initial_episode_num = episodeNum;
					select_level(sec, 0);
				}
				else
				{
					if (endlessBaseEp[dbgBase] != episodeNum)
						JE_initEpisode((JE_byte)endlessBaseEp[dbgBase]);
					if (episodeNum != startEp)
						initial_episode_num = episodeNum;
					select_level(endlessBaseSec[dbgBase], endlessBaseFile[dbgBase]);  // sets forcedLvlFileNum
				}
				chosen = true;
				done = true;
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

	VGAScreen = temp_surface;

	// A debug "Free perk pick" outcome was fired but we're not launching a level -> open the perk
	// pick now, on return to the menu (the shop's front-gate perk gate already passed this visit).
	// If a level WAS launched, the queued endlessPerkPending rides the normal next-shop gate instead.
	if (!chosen && endlessMode && endlessPerkPending)
	{
		endlessPerkListMode = false;
		configure_endless_perk_menu();
		curSel[MENU_PERKS] = 2;
		curMenu = MENU_PERKS;
	}
	return chosen;
}

/* Self-contained scrollable debug level picker (campaign/arcade). Returns true if a level was
 * chosen (select_level() has already armed the jump) or false if the user cancelled. Endless
 * mode has its own dedicated form -- see endlessDebugScreen(). */
bool JE_debugLevelSelect(void)
{
	if (endlessMode)
		return endlessDebugScreen();  // endless gets its own Zone / Base Level / Modifiers form

	const JE_byte startEp = episodeNum;
	int dispEp = episodeNum;            // episode currently being browsed
	load_debug_levels(dispEp);          // start on the player's current episode

	SDL_Surface *temp_surface = VGAScreen;
	VGAScreen = VGAScreenSeg;

	const int pw = 248;
	// Centered within the legacy 320px content area (set_menu_centered(true) is
	// active on this screen); using vga_width would double-offset it.
	const int px0 = (LEGACY_WIDTH - pw) / 2;
	const int px1 = px0 + pw - 1;
	const int py0 = 12;
	const int py1 = vga_height - 12;
	const int title_h = 15;
	const int items_top = py0 + title_h + 3;
	const int items_bottom = py1 - 10;
	const int row_h = 10;
	const int pageRows = (items_bottom - items_top) / row_h;
	const int mid_x = (px0 + px1) / 2;

	enum {
		C_PANEL_BG = 0xF1, C_TITLE_BG = 0xF3, C_DIVIDER = 0xF6,
		C_EDGE_HI  = 0xFB, C_EDGE_LO  = 0xF4, C_SEL_BAR = 0xF5
	};

	int selected = 0, scrollTop = 0;
	int prev_mx = mouse_x, prev_my = mouse_y;  // for motion-based hover
	bool chosen = false, done = false;

	wait_noinput(false, false, true);
	newkey = newmouse = false;  // don't let the click/key that opened us leak in

	while (!done)
	{
		const int count = (int)debugLevelCount;
		const int visibleRows = (pageRows < count) ? pageRows : count;  // 0 when empty

		if (selected > count - 1)
			selected = count - 1;
		if (selected < 0)
			selected = 0;

		fill_rectangle_xy(VGAScreen, px0, py0, px1, py1, C_PANEL_BG);
		fill_rectangle_xy(VGAScreen, px0, py0, px1, py0, C_EDGE_HI);
		fill_rectangle_xy(VGAScreen, px0, py0, px0, py1, C_EDGE_HI);
		fill_rectangle_xy(VGAScreen, px0, py1, px1, py1, C_EDGE_LO);
		fill_rectangle_xy(VGAScreen, px1, py0, px1, py1, C_EDGE_LO);
		fill_rectangle_xy(VGAScreen, px0 + 1, py0 + 1, px1 - 1, py0 + title_h - 1, C_TITLE_BG);
		fill_rectangle_xy(VGAScreen, px0 + 1, py0 + title_h, px1 - 1, py0 + title_h, C_DIVIDER);

		char title[40];
		snprintf(title, sizeof(title), "DEBUG LEVELS  < EP %d >", dispEp);
		draw_font_hv_shadow(VGAScreen, mid_x, py0 + 3, title, normal_font, centered, 15, 4, true, 1);

		if (count == 0)
		{
			draw_font_hv(VGAScreen, mid_x, items_top + 8, "(no levels)", small_font, centered, 15, -2);
		}
		else
		{
			/* keep the selection within the scrolled window */
			if (selected < scrollTop)
				scrollTop = selected;
			else if (selected >= scrollTop + visibleRows)
				scrollTop = selected - visibleRows + 1;
			if (scrollTop > count - visibleRows)
				scrollTop = count - visibleRows;
			if (scrollTop < 0)
				scrollTop = 0;

			for (int vis = 0; vis < visibleRows; ++vis)
			{
				const int i = scrollTop + vis;
				const int ry = items_top + vis * row_h;
				const bool sel = (i == selected);

				if (sel)
					fill_rectangle_xy(VGAScreen, px0 + 3, ry - 1, px1 - 3, ry + row_h - 2, C_SEL_BAR);

				char num[8], info[16];
				snprintf(num, sizeof(num), "%d", i + 1);
				snprintf(info, sizeof(info), "L%d", debugMapSection[i]);

				// Number right-aligned to a fixed edge, name at a fixed x; a single
				// left-aligned string would shift the name with the digit count.
				const int numRight = px0 + 24;
				const int nameLeft = px0 + 30;
				draw_font_hv_shadow(VGAScreen, numRight, ry, num, small_font, right_aligned, 15, sel ? 5 : -1, true, 1);
				draw_font_hv_shadow(VGAScreen, nameLeft, ry, debugLevelName[i], small_font, left_aligned, 15, sel ? 5 : -1, true, 1);
				draw_font_hv_shadow(VGAScreen, px1 - 11, ry, info, small_font, right_aligned, 15, sel ? 5 : 0, true, 1);

				if (sel)
					draw_font_hv(VGAScreen, px0 + 4, ry, ">", small_font, left_aligned, 15, 6);
			}

			/* scrollbar (only when the list overflows the window) */
			if (count > visibleRows)
			{
				const int track_top = items_top - 1;
				const int track_bot = items_top + visibleRows * row_h - 2;
				const int track_h = track_bot - track_top;
				fill_rectangle_xy(VGAScreen, px1 - 4, track_top, px1 - 3, track_bot, C_EDGE_LO);

				int thumb_h = track_h * visibleRows / count;
				if (thumb_h < 4)
					thumb_h = 4;
				const int denom = count - visibleRows;
				const int thumb_y = track_top + (denom > 0 ? (track_h - thumb_h) * scrollTop / denom : 0);
				fill_rectangle_xy(VGAScreen, px1 - 4, thumb_y, px1 - 3, thumb_y + thumb_h, C_EDGE_HI);
			}
		}

		draw_font_hv(VGAScreen, mid_x, py1 - 9, "L/R Episode   Enter Play   Esc Back",
		             small_font, centered, 15, -3);

		mouseCursor = MOUSE_POINTER_NORMAL;
		JE_mouseStart();
		JE_showVGA();
		JE_mouseReplace();

		push_joysticks_as_keyboard();
		service_SDL_events(true);

		/* wheel scrolls the selection; hover highlights on pointer motion; click
		 * selects (title-strip halves change episode, right-click cancels) */
		{
			if (mouse_scroll != 0)
			{
				selected -= mouse_scroll;
				if (selected < 0)
					selected = 0;
				if (count > 0 && selected > count - 1)
					selected = count - 1;
				mouse_scroll = 0;
			}
			if (count > 0 && (mouse_x != prev_mx || mouse_y != prev_my))
			{
				const int hov = level_row_at(mouse_x, mouse_y, px0, px1, items_top, row_h, visibleRows, scrollTop, count);
				if (hov >= 0)
					selected = hov;
			}
			prev_mx = mouse_x;
			prev_my = mouse_y;
			if (newmouse)
			{
				if (lastmouse_but == SDL_BUTTON_RIGHT)
				{
					newkey = true;
					lastkey_scan = SDL_SCANCODE_ESCAPE;
				}
				else if (lastmouse_y >= py0 && lastmouse_y <= py0 + title_h &&
				         lastmouse_x >= px0 && lastmouse_x <= px1)
				{
					newkey = true;
					lastkey_scan = (lastmouse_x < mid_x) ? SDL_SCANCODE_LEFT : SDL_SCANCODE_RIGHT;
				}
				else
				{
					const int r = level_row_at(lastmouse_x, lastmouse_y, px0, px1, items_top, row_h, visibleRows, scrollTop, count);
					if (r >= 0)
					{
						selected = r;
						newkey = true;
						lastkey_scan = SDL_SCANCODE_RETURN;
					}
				}
				newmouse = false;
			}
		}

		if (newkey)
		{
			switch (lastkey_scan)
			{
			case SDL_SCANCODE_LEFT:
			case SDL_SCANCODE_RIGHT:
			{
				// step to the next available episode in that direction (wrapping)
				const int dir = (lastkey_scan == SDL_SCANCODE_RIGHT) ? 1 : -1;
				int e = dispEp;
				for (int n = 0; n < EPISODE_MAX; ++n)
				{
					e += dir;
					if (e > EPISODE_MAX)
						e = 1;
					else if (e < 1)
						e = EPISODE_MAX;
					if (episodeAvail[e - 1])
						break;
				}
				if (e != dispEp)
				{
					dispEp = e;
					load_debug_levels(dispEp);
					selected = 0;
					scrollTop = 0;
				}
				break;
			}
			case SDL_SCANCODE_UP:
				if (count > 0)
					selected = (selected == 0) ? count - 1 : selected - 1;
				break;
			case SDL_SCANCODE_DOWN:
				if (count > 0)
					selected = (selected + 1) % count;
				break;
			case SDL_SCANCODE_PAGEUP:
				selected = (selected - visibleRows < 0) ? 0 : selected - visibleRows;
				break;
			case SDL_SCANCODE_PAGEDOWN:
				if (count > 0)
					selected = (selected + visibleRows > count - 1) ? count - 1 : selected + visibleRows;
				break;
			case SDL_SCANCODE_HOME:
				selected = 0;
				break;
			case SDL_SCANCODE_END:
				if (count > 0)
					selected = count - 1;
				break;
			case SDL_SCANCODE_RETURN:
			case SDL_SCANCODE_SPACE:
				if (count > 0)
				{
					// switch the game to the browsed episode if needed (reloads
					// level + item data), then arm the jump to the chosen level.
					// Snapshot first, while episodeNum is still where we came from.
					select_debug_level_capture();
					JE_initEpisode((JE_byte)dispEp);  // no-op if already current
					if (episodeNum != startEp)
						initial_episode_num = episodeNum;
					select_level(debugMapSection[selected], debugLvlFileNum[selected]);
					chosen = true;
					done = true;
				}
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

	VGAScreen = temp_surface;
	return chosen;
}

/* ---- Custom Weapon Creator ------------------------------------------------
 * A self-contained editor screen: the left panel is a live firing preview
 * (the same ship-in-a-box the buy/sell screen uses, driven straight through
 * player_shot_create + simulate_player_shots), the right panel is a scrollable
 * list of the custom weapon's editable fields plus actions. Every edit calls
 * customWeaponMaterialize(), so the preview reflects changes instantly. */

enum  // rows of the editor list (weapon-wide, per-bullet, import, then actions)
{
	// "Library": which saved weapon is loaded, plus New / Duplicate / Delete
	CWROW_LIB_SELECT = 0, // which saved weapon (library slot) is loaded
	CWROW_LIB_NEW,        // inline action: add a fresh weapon
	CWROW_LIB_DUPLICATE,  // inline action: copy the current weapon
	CWROW_LIB_DELETE,     // inline action: remove the current weapon
	// Import actions sit directly above the import picker they act on
	CWROW_IMPORT_THIS, // inline action: import the source into this level
	CWROW_IMPORT_ALL,  // inline action: clone the whole source weapon
	CWROW_ADD_THIS,    // inline action: combine the source onto this level
	CWROW_ADD_ALL,     // inline action: combine the source onto every level
	CWROW_IMPORT_SRC,
	CWROW_IMPORT_PWR,
	// "Setup": weapon-wide identity + shop presentation
	CWROW_NAME,
	CWROW_EQUIP_SLOT,
	CWROW_COST,
	CWROW_ICON,        // weaponPort itemgraphic (shop/HUD icon)
	CWROW_POWER_USE,
	// "Levels": which power-level / fire-mode variant is being edited, the charge stages
	// (the power levels ARE a sidekick's charge stages), and the two actions that fill the
	// whole level curve from the level in front of you.
	CWROW_POWER_LEVEL,
	CWROW_TWO_MODES,   // opnum 1 or 2 (does a rear-gun toggle give a 2nd design?)
	CWROW_FIRE_MODE,   // which fire mode is being edited
	CWROW_CHARGE,      // sidekick charge stages (option pwr; the power levels ARE the stages)
	CWROW_ADD_CHARGE,    // inline action: add a charge state (sidekick only)
	CWROW_REMOVE_CHARGE, // inline action: remove a charge state
	CWROW_AUTOSCALE,   // inline action: build all levels from this one, scaled up + down
	CWROW_COPY_ALL,    // inline action: copy this level to every level (flatten)
	// "Firing": this mode + power level's whole-volley raw fields
	CWROW_FIRE_RATE,   // shotrepeat
	CWROW_BULLETS,     // multi
	CWROW_PATTERN,     // max
	CWROW_ANIM,        // weapani
	CWROW_HOMING,      // aim
	CWROW_ACCEL_Y,     // acceleration
	CWROW_ACCEL_X,     // accelerationx
	CWROW_SPIRAL,      // circlesize
	CWROW_SOUND,       // sound
	CWROW_TRAIL,       // trail
	CWROW_BLAST,       // shipblastfilter
	// The selected bullet's per-bullet fields, with the add/remove actions right above
	CWROW_ADD_SEGMENT,     // inline action: duplicate the selected bullet
	CWROW_REMOVE_SEGMENT,  // inline action: delete the selected bullet
	CWROW_BULLET_SEL,  // which bullet the rows below edit
	CWROW_B_SPRITE,    // sg
	CWROW_B_DAMAGE,    // attack (1-98 dmg, 99 Ice)
	CWROW_B_CHAIN,     // attack 101-249: explode into weapon N on enemy hit
	CWROW_B_LIFE,      // del
	CWROW_B_VELX,      // sx
	CWROW_B_VELY,      // sy
	CWROW_B_OFFX,      // bx
	CWROW_B_OFFY,      // by
	// "Sidekick": the sidekick body's mount + sprite + animation (used when equipped as a sidekick)
	CWROW_SK_MOUNT,    // option tr: Side / Trail Big / Front / Trail / Orbit
	CWROW_SK_SPRITE,   // base body sprite (gr[0]); sheet depends on the mount
	CWROW_SK_FRAMES,   // animation frame count (ani)
	CWROW_SK_STEP,     // sprite step between animation frames
	CWROW_SK_ANIMATE,  // animate while firing / always
	// "Preview": practice targets / stats / ship position (editor-only test harness, shown last)
	CWROW_SHOW_TARGETS,// preview toggle: practice dummies so on-hit behaviour is visible
	CWROW_SHOW_STATS,  // preview toggle: live DPS / damage / fire-rate readout
	CWROW_TARGET_COUNT,// how many practice dummies
	CWROW_TARGET_SPREAD,// horizontal spacing between dummies
	CWROW_TARGET_HEIGHT,// vertical position of the dummy row
	CWROW_TARGET_X,    // horizontal position of the dummy row
	CWROW_TARGET_ARMOR,// dummy toughness (how fast the armour bar drains)
	CWROW_SHIP_X,      // preview ship X = where shots are fired from
	CWROW_SHIP_Y,      // preview ship Y
	CWROW_FIELD_COUNT
};

// Action ids (shared by the inline action rows above and the bottom action list).
enum { CWACT_ADD_BULLET, CWACT_REMOVE_BULLET, CWACT_ADD_CHARGE, CWACT_REMOVE_CHARGE, CWACT_IMPORT_LEVEL, CWACT_IMPORT_ALL, CWACT_COPY_ALL, CWACT_RANDOMIZE, CWACT_RESET, CWACT_RESET_ALL, CWACT_EQUIP, CWACT_DONE, CWACT_LIB_NEW, CWACT_LIB_DUPLICATE, CWACT_LIB_DELETE, CWACT_AUTOSCALE, CWACT_SAVE, CWACT_ADD_LEVEL, CWACT_ADD_ALL_LEVELS };
static const char *const cwActLabel[] = { "ADD SEGMENT", "REMOVE SEGMENT", "ADD CHARGE STATE", "REMOVE CHARGE STATE", "IMPORT THIS LEVEL", "IMPORT ALL LEVELS", "COPY TO ALL", "RANDOMIZE", "RESET", "RESET ALL", "EQUIP", "DONE", "NEW WEAPON", "DUPLICATE", "DELETE WEAPON", "AUTO-SCALE LEVELS", "SAVE", "ADD THIS LEVEL", "ADD ALL LEVELS" };

// Map an inline action row to its action id; returns -1 for a normal (adjustable) field row.
static int cwInlineActionId(int row)
{
	switch (row)
	{
	case CWROW_ADD_SEGMENT:    return CWACT_ADD_BULLET;
	case CWROW_REMOVE_SEGMENT: return CWACT_REMOVE_BULLET;
	case CWROW_ADD_CHARGE:     return CWACT_ADD_CHARGE;
	case CWROW_REMOVE_CHARGE:  return CWACT_REMOVE_CHARGE;
	case CWROW_AUTOSCALE:      return CWACT_AUTOSCALE;
	case CWROW_COPY_ALL:       return CWACT_COPY_ALL;
	case CWROW_IMPORT_THIS:    return CWACT_IMPORT_LEVEL;
	case CWROW_IMPORT_ALL:     return CWACT_IMPORT_ALL;
	case CWROW_ADD_THIS:       return CWACT_ADD_LEVEL;
	case CWROW_ADD_ALL:        return CWACT_ADD_ALL_LEVELS;
	case CWROW_LIB_NEW:        return CWACT_LIB_NEW;
	case CWROW_LIB_DUPLICATE:  return CWACT_LIB_DUPLICATE;
	case CWROW_LIB_DELETE:     return CWACT_LIB_DELETE;
	default:                   return -1;
	}
}

// Editor rows are partitioned into categories. The top-of-panel selector filters the
// visible rows to one category (or "All"), so you aren't scrolling one long list.
// Order follows the natural build workflow: pick a saved weapon, seed it from a stock
// weapon (Import), give it an identity (Setup), shape its power curve (Levels), tune the
// volley (Firing) and each bullet (Bullet), then test it (Preview).
enum
{
	CWCAT_ALL = 0,   // every row (the classic flat list)
	CWCAT_LIBRARY,   // pick / add / delete saved weapons
	CWCAT_IMPORT,    // seed the design from a stock weapon
	CWCAT_SETUP,     // weapon-wide identity: name / equip / cost / icon / power
	CWCAT_LEVELS,    // power-level / fire-mode variant, charge stages, fill-all actions
	CWCAT_FIRING,    // this design's whole-volley fields: rate / bullets / homing / ...
	CWCAT_BULLET,    // per-bullet segment editing
	CWCAT_SIDEKICK,  // sidekick body: mount / sprite / animation (when equipped as a sidekick)
	CWCAT_PREVIEW,   // preview / test options: target dummies, stats, ship position
	CWCAT_COUNT
};
static const char *const cwCatName[CWCAT_COUNT] = { "All", "Library", "Import", "Setup", "Levels", "Firing", "Bullet", "Sidekick", "Preview" };

// Which category a field row belongs to (also drives its colour band).
static int cwRowCategory(int row)
{
	switch (row)
	{
	case CWROW_LIB_SELECT: case CWROW_LIB_NEW: case CWROW_LIB_DUPLICATE: case CWROW_LIB_DELETE:
		return CWCAT_LIBRARY;
	case CWROW_POWER_LEVEL: case CWROW_TWO_MODES: case CWROW_FIRE_MODE:
	case CWROW_CHARGE: case CWROW_ADD_CHARGE: case CWROW_REMOVE_CHARGE:
	case CWROW_AUTOSCALE: case CWROW_COPY_ALL:
		return CWCAT_LEVELS;
	case CWROW_SHOW_TARGETS: case CWROW_SHOW_STATS:
	case CWROW_TARGET_COUNT: case CWROW_TARGET_SPREAD: case CWROW_TARGET_HEIGHT:
	case CWROW_TARGET_X: case CWROW_TARGET_ARMOR: case CWROW_SHIP_X: case CWROW_SHIP_Y:
		return CWCAT_PREVIEW;
	case CWROW_NAME: case CWROW_EQUIP_SLOT: case CWROW_COST: case CWROW_ICON:
	case CWROW_POWER_USE:
		return CWCAT_SETUP;
	case CWROW_SK_MOUNT: case CWROW_SK_SPRITE: case CWROW_SK_FRAMES:
	case CWROW_SK_STEP: case CWROW_SK_ANIMATE:
		return CWCAT_SIDEKICK;
	case CWROW_FIRE_RATE: case CWROW_BULLETS: case CWROW_PATTERN: case CWROW_ANIM:
	case CWROW_HOMING: case CWROW_ACCEL_Y: case CWROW_ACCEL_X: case CWROW_SPIRAL:
	case CWROW_SOUND: case CWROW_TRAIL: case CWROW_BLAST:
		return CWCAT_FIRING;
	case CWROW_ADD_SEGMENT: case CWROW_REMOVE_SEGMENT: case CWROW_BULLET_SEL:
	case CWROW_B_SPRITE: case CWROW_B_DAMAGE: case CWROW_B_CHAIN: case CWROW_B_LIFE:
	case CWROW_B_VELX: case CWROW_B_VELY: case CWROW_B_OFFX: case CWROW_B_OFFY:
		return CWCAT_BULLET;
	case CWROW_IMPORT_THIS: case CWROW_IMPORT_ALL: case CWROW_ADD_THIS: case CWROW_ADD_ALL:
	case CWROW_IMPORT_SRC: case CWROW_IMPORT_PWR:
		return CWCAT_IMPORT;
	default:
		return CWCAT_SETUP;
	}
}

// Dark, muted colour bands from the shop backdrop palette (palette 0) so the bright row
// text stays legible over them; one per category, plus grey for the pinned action buttons.
enum
{
	CW_BG_LIBRARY  = 0x82,  // steel  - Library (pick / add / delete weapons)
	CW_BG_LEVELS   = 0x91,  // navy   - Levels (power level / modes / charge / fill-all)
	CW_BG_PREVIEW  = 0x63,  // slate  - Preview (targets / stats / ship)
	CW_BG_SETUP    = 0x53,  // purple - Setup (name / equip / cost / icon / power)
	CW_BG_SIDEKICK = 0x72,  // olive  - Sidekick (mount / sprite / animation) - tunable
	CW_BG_FIRING   = 0x24,  // green  - Firing (rate / bullets / homing / ...)
	CW_BG_BULLET   = 0xD2,  // teal   - Bullet (per-segment editing)
	CW_BG_IMPORT   = 0x13,  // amber  - Import (clone a stock weapon)
	CW_BG_ACTION   = 0x03,  // grey   - action buttons (randomize/reset/equip/done)
};

// The colour band for a category.
static Uint8 cwCatColor(int cat)
{
	switch (cat)
	{
	case CWCAT_LIBRARY:  return CW_BG_LIBRARY;
	case CWCAT_LEVELS:   return CW_BG_LEVELS;
	case CWCAT_PREVIEW:  return CW_BG_PREVIEW;
	case CWCAT_SETUP:    return CW_BG_SETUP;
	case CWCAT_SIDEKICK: return CW_BG_SIDEKICK;
	case CWCAT_FIRING:   return CW_BG_FIRING;
	case CWCAT_BULLET:   return CW_BG_BULLET;
	case CWCAT_IMPORT:   return CW_BG_IMPORT;
	default:             return CW_BG_ACTION;
	}
}

// The colour band for a field row, derived from its category.
static Uint8 cwGroupColor(int row) { return cwCatColor(cwRowCategory(row)); }

// Some rows only make sense for a particular Equip slot: the fire-mode toggle exists only
// on a rear gun, and charging is a sidekick-only mechanic (a front/rear custom gun can't
// charge). Those rows are drawn greyed and left inert until the weapon is equipped where
// they apply, so the editor only offers controls that actually do something.
static bool cwRowActive(int row)
{
	const int slot = customWeaponEquipSlot;
	const bool isRear  = (slot == CUSTOM_EQUIP_REAR);
	const bool isSidekick = (slot == CUSTOM_EQUIP_LEFT || slot == CUSTOM_EQUIP_RIGHT || slot == CUSTOM_EQUIP_BOTH);
	switch (row)
	{
	case CWROW_TWO_MODES: case CWROW_FIRE_MODE:
		return isRear;    // a fire-mode toggle only happens on a rear gun
	case CWROW_CHARGE: case CWROW_ADD_CHARGE: case CWROW_REMOVE_CHARGE:
		return isSidekick;   // charging only works for a sidekick
	case CWROW_SK_MOUNT: case CWROW_SK_SPRITE: case CWROW_SK_FRAMES:
	case CWROW_SK_STEP: case CWROW_SK_ANIMATE:
		return isSidekick;   // the sidekick body only matters when equipped as a sidekick
	default:
		return true;
	}
}

// Why a greyed row is inactive (shown on the hint line so it isn't a mystery).
static const char *cwRowInactiveReason(int row)
{
	switch (row)
	{
	case CWROW_TWO_MODES: case CWROW_FIRE_MODE:
		return "Only used on a Rear Gun - change Equip To";
	case CWROW_CHARGE: case CWROW_ADD_CHARGE: case CWROW_REMOVE_CHARGE:
	case CWROW_SK_MOUNT: case CWROW_SK_SPRITE: case CWROW_SK_FRAMES:
	case CWROW_SK_STEP: case CWROW_SK_ANIMATE:
		return "Only used on a Sidekick - change Equip To";
	default:
		return "Not used with the current Equip To";
	}
}

static const char *const cwSlotNames[] = { "Front Gun", "Rear Gun", "Left Sidekick", "Right Sidekick", "Both Sidekicks" };

// Sidekick mount styles, indexed by the engine's option "tr" (0..4). "Trail Big" (tr 1) and
// "Front" (tr 2) draw a 2x2 body from spriteSheet10; the rest a single tile from spriteSheet9.
static const char *const cwMountNames[] = { "Side Pod", "Trail (Big)", "Front", "Trailing", "Orbit" };

// Editor-only selection state (not part of the saved design).
static int cwImportSrc = 0;   // index into customBulletPreset[] to import from
static int cwImportPwr = 1;   // source power level for "Import This Level"
static int cwBulletSel = 0;   // which bullet the per-bullet rows edit

// Combat-preview settings (editor-only, not part of the saved design; kept across opens).
static bool cwShowTargets  = true;   // practice target dummies in the box
static bool cwShowStats    = true;   // live DPS / damage / fire-rate readout
static int  cwTargetCount  = 3;      // number of dummies (0..CW_DUMMY_MAX)
static int  cwTargetSpread = 32;     // horizontal spacing between dummies
static int  cwTargetHeight = 48;     // vertical position of the dummy row
static int  cwTargetX      = 76;     // horizontal centre of the dummy row (box centre ~76)
static int  cwTargetArmor  = 120;    // dummy toughness
static int  cwShipX = 70;            // preview ship position = shot origin (drives player[0].x each tick)
static int  cwShipY = 110;
static int  cwCategory = CWCAT_ALL;  // which row category the top selector is showing

// Direct numeric entry: on a numeric value row, typing digits builds an exact value
// (so a shop price of 900, or sprite 214, doesn't need scrolling in coarse steps).
// cwNumRow is the row currently being typed into (-1 = not typing); cwNumText holds
// the digits entered so far. Committed on Enter / arrows / leaving the row.
static int  cwNumRow = -1;
static char cwNumText[12];

// Frames remaining to show the "saved" confirmation on the hint line (0 = not showing).
static int  cwSavedFlash = 0;

static int cwClamp(int v, int lo, int hi)
{
	return (v < lo) ? lo : (v > hi) ? hi : v;
}

// Current mode + level's raw weapon (edits happen in place).
static JE_WeaponType *cwCur(void)
{
	const int m = cwClamp(customWeaponEditMode,  0, CUSTOM_WEAPON_MODES - 1);
	const int p = cwClamp(customWeaponEditLevel, 0, CUSTOM_POWER_LEVELS - 1);
	return &customWeaponRaw[m][p];
}

// The selected bullet index, clamped to the weapon's current bullet count.
static int cwBulletIndex(void)
{
	int mx = cwCur()->multi;
	if (mx < 1) mx = 1;
	if (mx > CUSTOM_BULLETS_MAX) mx = CUSTOM_BULLETS_MAX;
	return cwClamp(cwBulletSel, 0, mx - 1);
}

// ---- combat-preview target dummies (data + layout) -----------------------------
// The dummies give the shots something to hit so on-hit behaviour (Damage, Ice,
// Explode-To chaining, Homing) is visible. Their count/spacing/height/armour are set
// from the editor rows via cwLayoutDummies(); the collision + draw code lives further
// down (near JE_customWeaponCreator).
typedef struct { int cx, cy, armor, hit, ice, dead; } CwDummy;
enum { CW_DUMMY_MAX = 6, CW_DUMMY_R = 8 };
static CwDummy cwDummies[CW_DUMMY_MAX];
static int cwDummyCount = 0;

// Lay the active dummies out as a centred horizontal row from the current settings,
// resetting each to full armour. Called at creator entry and whenever a setting changes.
static void cwLayoutDummies(void)
{
	cwDummyCount = cwClamp(cwTargetCount, 0, CW_DUMMY_MAX);
	for (int i = 0; i < cwDummyCount; ++i)
	{
		const int off = 2 * i - (cwDummyCount - 1);   // symmetric fan: - .. 0 .. +
		cwDummies[i].cx    = cwClamp(cwTargetX + off * cwTargetSpread / 2, 8 + CW_DUMMY_R, 143 - CW_DUMMY_R);
		cwDummies[i].cy    = cwClamp(cwTargetHeight, 8 + CW_DUMMY_R, 168);
		cwDummies[i].armor = cwTargetArmor;
		cwDummies[i].hit   = cwDummies[i].ice = cwDummies[i].dead = 0;
	}
}

// --- readable decoders for the sentinel-encoded raw fields -------------------
static void cwFmtDamage(int a, char *b, size_t n)
{
	// 101-249 (explode-into-weapon) is shown by the separate -Explode To row, not here.
	if (a <= 0)       snprintf(b, n, "None");
	else if (a == 99) snprintf(b, n, "Ice");
	else              snprintf(b, n, "%d", a);
}
static void cwFmtLife(int d, char *b, size_t n)
{
	if (d == 98)                 snprintf(b, n, "Aim X");
	else if (d == 99)            snprintf(b, n, "Aim X+Y");
	else if (d == 100)           snprintf(b, n, "Aim Y");
	else if (d == 121)           snprintf(b, n, "No Trail");
	else if (d > 100 && d < 120) snprintf(b, n, "Anim %dfr", d - 100 + 1);
	else if (d >= 255)           snprintf(b, n, "Life Max");
	else                         snprintf(b, n, "Life %d", d);
}
static void cwFmtVelY(int v, char *b, size_t n)
{
	if (v == 98)      snprintf(b, n, "Hover");
	else if (v > 100) snprintf(b, n, "Locked %d", v);
	else if (v > 0)   snprintf(b, n, "Up %d", v);
	else if (v < 0)   snprintf(b, n, "Down %d", -v);
	else              snprintf(b, n, "Still");
}
static void cwFmtVelX(int v, char *b, size_t n)
{
	if (v > 100)    snprintf(b, n, "Locked %d", v);
	else if (v > 0) snprintf(b, n, "Right %d", v);
	else if (v < 0) snprintf(b, n, "Left %d", -v);
	else            snprintf(b, n, "0");
}
static void cwFmtTrail(int t, char *b, size_t n)
{
	if (t == 255 || t == 0) snprintf(b, n, "None");
	else if (t == 98)       snprintf(b, n, "Smoke");
	else if (t == 198)      snprintf(b, n, "Partial");
	else                    snprintf(b, n, "%d", t);
}

static const char *cwRowLabel(int row)
{
	switch (row)
	{
	case CWROW_LIB_SELECT:  return "Weapon";
	case CWROW_POWER_LEVEL: return "Power Level";
	case CWROW_TWO_MODES:   return "Two Modes";
	case CWROW_FIRE_MODE:   return "Fire Mode";
	case CWROW_SHOW_TARGETS:return "Preview Targets";
	case CWROW_SHOW_STATS:  return "Stats Readout";
	case CWROW_TARGET_COUNT: return "Target Count";
	case CWROW_TARGET_SPREAD:return "Target Spread";
	case CWROW_TARGET_HEIGHT:return "Target Height";
	case CWROW_TARGET_X:     return "Target X";
	case CWROW_TARGET_ARMOR: return "Target Armor";
	case CWROW_SHIP_X:      return "Ship X";
	case CWROW_SHIP_Y:      return "Ship Y";
	case CWROW_NAME:        return "Name";
	case CWROW_EQUIP_SLOT:  return "Equip To";
	case CWROW_COST:        return "Cost";
	case CWROW_ICON:        return "Shop Icon";
	case CWROW_POWER_USE:   return "Power Use";
	case CWROW_SK_MOUNT:    return "Mount";
	case CWROW_SK_SPRITE:   return "Body Sprite";
	case CWROW_SK_FRAMES:   return "Body Frames";
	case CWROW_SK_STEP:     return "Frame Step";
	case CWROW_SK_ANIMATE:  return "Animate";
	case CWROW_CHARGE:      return "Charge Stages";
	case CWROW_IMPORT_SRC:  return "Import From";
	case CWROW_IMPORT_PWR:  return "Import Level";
	case CWROW_FIRE_RATE:   return "Fire Rate";
	case CWROW_BULLETS:     return "Bullets";
	case CWROW_PATTERN:     return "Pattern";
	case CWROW_ANIM:        return "Anim Frames";
	case CWROW_HOMING:      return "Homing";
	case CWROW_ACCEL_Y:     return "Accel Up";
	case CWROW_ACCEL_X:     return "Accel Side";
	case CWROW_SPIRAL:      return "Spiral";
	case CWROW_SOUND:       return "Sound";
	case CWROW_TRAIL:       return "Trail";
	case CWROW_BLAST:       return "Blast FX";
	case CWROW_BULLET_SEL:  return "Bullet Segment";
	case CWROW_B_SPRITE:    return "-Sprite";
	case CWROW_B_DAMAGE:    return "-Damage";
	case CWROW_B_CHAIN:     return "-Explode To";
	case CWROW_B_LIFE:      return "-Life";
	case CWROW_B_VELX:      return "-Speed X";
	case CWROW_B_VELY:      return "-Speed Y";
	case CWROW_B_OFFX:      return "-Offset X";
	case CWROW_B_OFFY:      return "-Offset Y";
	}
	return "";
}

// One-line description of the selected field, shown on the hint line.
static const char *cwRowHelp(int row)
{
	switch (row)
	{
	case CWROW_LIB_SELECT:  return "Pick a saved weapon (Left/Right)";
	case CWROW_LIB_NEW:       return "Enter: add a new blank weapon to the library";
	case CWROW_LIB_DUPLICATE: return "Enter: copy the current weapon to a new slot";
	case CWROW_LIB_DELETE:    return "Enter: delete the current weapon";
	case CWROW_POWER_LEVEL: return "Level being edited (= charge stage when charging)";
	case CWROW_TWO_MODES:   return "2nd design for the rear-gun fire toggle";
	case CWROW_FIRE_MODE:   return "Which fire mode you're editing";
	case CWROW_AUTOSCALE:   return "Enter: build all levels from this one (scale up + down)";
	case CWROW_COPY_ALL:    return "Enter: copy this level to every level (all identical)";
	case CWROW_SHOW_TARGETS:return "Practice targets: see hits, freeze, chains, homing";
	case CWROW_SHOW_STATS:  return "Live DPS / damage / fire-rate readout in the box";
	case CWROW_TARGET_COUNT: return "How many practice targets (0 = none)";
	case CWROW_TARGET_SPREAD:return "Horizontal spacing between targets";
	case CWROW_TARGET_HEIGHT:return "How high up the targets sit";
	case CWROW_TARGET_X:     return "Slide the whole target row left-right";
	case CWROW_TARGET_ARMOR: return "Target toughness (higher = armour drains slower)";
	case CWROW_SHIP_X:      return "Move the ship / shot origin left-right";
	case CWROW_SHIP_Y:      return "Move the ship / shot origin up-down";
	case CWROW_NAME:        return "Type to rename";
	case CWROW_EQUIP_SLOT:  return "Where it mounts (gun or sidekick)";
	case CWROW_COST:        return "Shop price";
	case CWROW_POWER_USE:   return "Generator drain per shot";
	case CWROW_ICON:        return "Shop/HUD icon (preview shown bottom-left)";
	case CWROW_SK_MOUNT:    return "Where the sidekick sits (Front = ahead of the ship)";
	case CWROW_SK_SPRITE:   return "Sidekick body sprite; charge stages step up from here";
	case CWROW_SK_FRAMES:   return "Body animation frames (1 = static)";
	case CWROW_SK_STEP:     return "Sprite gap between body animation frames";
	case CWROW_SK_ANIMATE:  return "When the body animates (on fire / always)";
	case CWROW_CHARGE:      return "Sidekick charge shots (1 = off); edit each via Power Level";
	case CWROW_ADD_CHARGE:    return "Enter: add a charge shot, then tune its sprite below";
	case CWROW_REMOVE_CHARGE: return "Enter: remove the top charge shot";
	case CWROW_IMPORT_SRC:  return "Stock weapon to copy from";
	case CWROW_IMPORT_PWR:  return "Source level for Import This Level";
	case CWROW_FIRE_RATE:   return "Ticks between shots (0 = every tick; needed for solid lasers)";
	case CWROW_BULLETS:     return "Bullets per shot (max 8)";
	case CWROW_PATTERN:     return "Cycle len; above Bullets = variation";
	case CWROW_ANIM:        return "Sprite frames cycled (1 = static, no animation)";
	case CWROW_HOMING:      return "Guides shots at enemies when above 5";
	case CWROW_ACCEL_Y:     return "Vertical acceleration";
	case CWROW_ACCEL_X:     return "Sideways acceleration";
	case CWROW_SPIRAL:      return "Corkscrew radius (0 = straight)";
	case CWROW_SOUND:       return "Fire sound effect";
	case CWROW_TRAIL:       return "Exhaust trail effect";
	case CWROW_BLAST:       return "Impact palette filter";
	case CWROW_ADD_SEGMENT:    return "Enter: duplicate the selected bullet";
	case CWROW_REMOVE_SEGMENT: return "Enter: delete the selected bullet";
	case CWROW_IMPORT_THIS:    return "Enter: copy the source into this level";
	case CWROW_IMPORT_ALL:     return "Enter: clone the whole source weapon";
	case CWROW_ADD_THIS:       return "Enter: add the source's shots onto this level (combine)";
	case CWROW_ADD_ALL:        return "Enter: add the source's shots onto every level (combine)";
	case CWROW_BULLET_SEL:  return "Which bullet the rows below edit";
	case CWROW_B_SPRITE:    return "Bullet sprite number";
	case CWROW_B_DAMAGE:    return "Damage per hit (99 = Ice); chaining is set below";
	case CWROW_B_CHAIN:     return "On enemy hit, burst into this weapon # (Off = deal Damage)";
	case CWROW_B_LIFE:      return "Life ticks, or Aim / No-Trail / Anim";
	case CWROW_B_VELX:      return "+ right, - left; above 100 locked";
	case CWROW_B_VELY:      return "+ up, - down; 98 hover, 100+ locked";
	case CWROW_B_OFFX:      return "Spawn X offset from ship";
	case CWROW_B_OFFY:      return "Spawn Y offset from ship";
	}
	return "";
}

static void cwRowValue(int row, char *buf, size_t n)
{
	JE_WeaponType *w = cwCur();
	const int b = cwBulletIndex();
	switch (row)
	{
	case CWROW_LIB_SELECT:
		snprintf(buf, n, "%d/%d %s", customWeaponCurrentSlot + 1, customWeaponLibCount, customWeaponName);
		break;
	case CWROW_POWER_LEVEL:
		// With charging on, levels 1..N ARE the charge stages, so this row doubles as the
		// charge-stage selector: show "Stage S/N" (stage 1 = uncharged, stage N = full).
		if (customWeaponChargeStages > 1 && customWeaponEditLevel < customWeaponChargeStages)
			snprintf(buf, n, "Stage %d / %d", customWeaponEditLevel + 1, customWeaponChargeStages);
		else
			snprintf(buf, n, "%d / %d", customWeaponEditLevel + 1, CUSTOM_POWER_LEVELS);
		break;
	case CWROW_TWO_MODES:   snprintf(buf, n, "%s", customWeaponModes >= 2 ? "Yes" : "No"); break;
	case CWROW_FIRE_MODE:
		if (customWeaponModes >= 2) snprintf(buf, n, "%d / %d", customWeaponEditMode + 1, customWeaponModes);
		else                        snprintf(buf, n, "n/a");
		break;
	case CWROW_SHOW_TARGETS: snprintf(buf, n, "%s", cwShowTargets ? "On" : "Off"); break;
	case CWROW_SHOW_STATS:   snprintf(buf, n, "%s", cwShowStats ? "On" : "Off"); break;
	case CWROW_TARGET_COUNT: snprintf(buf, n, "%d", cwTargetCount); break;
	case CWROW_TARGET_SPREAD:snprintf(buf, n, "%d", cwTargetSpread); break;
	case CWROW_TARGET_HEIGHT:snprintf(buf, n, "%d", cwTargetHeight); break;
	case CWROW_TARGET_X:     snprintf(buf, n, "%d", cwTargetX); break;
	case CWROW_TARGET_ARMOR: snprintf(buf, n, "%d", cwTargetArmor); break;
	case CWROW_SHIP_X:       snprintf(buf, n, "%d", cwShipX); break;
	case CWROW_SHIP_Y:       snprintf(buf, n, "%d", cwShipY); break;
	case CWROW_NAME:        snprintf(buf, n, "%s", customWeaponName); break;
	case CWROW_EQUIP_SLOT:  snprintf(buf, n, "%s", cwSlotNames[cwClamp(customWeaponEquipSlot, 0, CUSTOM_EQUIP_COUNT - 1)]); break;
	case CWROW_COST:        snprintf(buf, n, "%d", customWeaponCost); break;
	case CWROW_ICON:        snprintf(buf, n, "%d", customWeaponItemGraphic); break;
	case CWROW_POWER_USE:   snprintf(buf, n, "%d", customWeaponPowerUse); break;
	case CWROW_SK_MOUNT:    snprintf(buf, n, "%s", cwMountNames[cwClamp(customSidekickMount, 0, CUSTOM_SIDEKICK_MOUNTS - 1)]); break;
	case CWROW_SK_SPRITE:   snprintf(buf, n, "%d", customSidekickSprite); break;
	case CWROW_SK_FRAMES:   snprintf(buf, n, "%d", customSidekickFrames); break;
	case CWROW_SK_STEP:     snprintf(buf, n, "%d", customSidekickFrameStep); break;
	case CWROW_SK_ANIMATE:  snprintf(buf, n, "%s", customSidekickAnimate >= 2 ? "Always" : "On Fire"); break;
	case CWROW_CHARGE:      if (customWeaponChargeStages > 1) snprintf(buf, n, "%d stages (Lv 1-%d)", customWeaponChargeStages, customWeaponChargeStages); else snprintf(buf, n, "Off"); break;
	case CWROW_IMPORT_SRC:  snprintf(buf, n, "%s", customBulletPresetCount > 0 ? customBulletPreset[cwClamp(cwImportSrc, 0, customBulletPresetCount - 1)].name : "-"); break;
	case CWROW_IMPORT_PWR:
	{
		const int mp = customBulletMaxPower(cwImportSrc);
		if (mp > 1) snprintf(buf, n, "%d / %d", cwClamp(cwImportPwr, 1, mp), mp);
		else        snprintf(buf, n, "n/a");
		break;
	}
	case CWROW_FIRE_RATE:   if (w->shotrepeat == 0) snprintf(buf, n, "Every tick"); else snprintf(buf, n, "%d", w->shotrepeat); break;
	case CWROW_BULLETS:     snprintf(buf, n, "%d", w->multi); break;
	case CWROW_PATTERN:     snprintf(buf, n, "%d", w->max); break;
	case CWROW_ANIM:        snprintf(buf, n, "%d", w->weapani + 1); break;  // engine cycles weapani+1 frames
	case CWROW_HOMING:      if (w->aim > 5) snprintf(buf, n, "On (%d)", w->aim); else snprintf(buf, n, "Off"); break;
	case CWROW_ACCEL_Y:     snprintf(buf, n, "%+d", w->acceleration); break;
	case CWROW_ACCEL_X:     snprintf(buf, n, "%+d", w->accelerationx); break;
	case CWROW_SPIRAL:      if (w->circlesize) snprintf(buf, n, "%d", w->circlesize); else snprintf(buf, n, "Off"); break;
	case CWROW_SOUND:       if (w->sound) snprintf(buf, n, "%d", w->sound); else snprintf(buf, n, "None"); break;
	case CWROW_TRAIL:       cwFmtTrail(w->trail, buf, n); break;
	case CWROW_BLAST:       snprintf(buf, n, "%d", w->shipblastfilter); break;
	case CWROW_BULLET_SEL:  snprintf(buf, n, "%d / %d", b + 1, (w->multi < 1) ? 1 : w->multi); break;
	case CWROW_B_SPRITE:    snprintf(buf, n, "%d", w->sg[b]); break;
	case CWROW_B_DAMAGE:    if (w->attack[b] >= 101 && w->attack[b] <= 249) snprintf(buf, n, "Explodes"); else cwFmtDamage(w->attack[b], buf, n); break;
	case CWROW_B_CHAIN:     if (w->attack[b] >= 101 && w->attack[b] <= 249) snprintf(buf, n, "Weapon %d", w->attack[b] - 100); else snprintf(buf, n, "Off"); break;
	case CWROW_B_LIFE:      cwFmtLife(w->del[b], buf, n); break;
	case CWROW_B_VELX:      cwFmtVelX(w->sx[b], buf, n); break;
	case CWROW_B_VELY:      cwFmtVelY(w->sy[b], buf, n); break;
	case CWROW_B_OFFX:      snprintf(buf, n, "%+d", w->bx[b]); break;
	case CWROW_B_OFFY:      snprintf(buf, n, "%+d", w->by[b]); break;
	default:                buf[0] = '\0'; break;
	}
}

// Adjust a field by dir (-1/+1). Editor-only selection rows return early; every
// design field clamps to its valid range and re-materializes.
static void cwRowAdjust(int row, int dir)
{
	JE_WeaponType *w = cwCur();
	const int b = cwBulletIndex();
	switch (row)
	{
	case CWROW_LIB_SELECT:
		if (customWeaponLibCount > 0)
			customWeaponSelectSlot((customWeaponCurrentSlot + customWeaponLibCount + dir) % customWeaponLibCount);
		return;  // switches the working weapon (materializes internally)
	case CWROW_POWER_LEVEL:
		customWeaponSelectLevel(cwClamp(customWeaponEditLevel + dir, 0, CUSTOM_POWER_LEVELS - 1));
		return;  // just changes which (already-compiled) level the preview shows
	case CWROW_FIRE_MODE:
		if (customWeaponModes >= 2)
			customWeaponSelectMode(cwClamp(customWeaponEditMode + dir, 0, customWeaponModes - 1));
		return;  // view-only: both modes are already compiled
	case CWROW_SHOW_TARGETS:
		cwShowTargets = !cwShowTargets;  // binary preview switch (Left/Right/Enter all flip)
		return;  // editor-only preview state
	case CWROW_SHOW_STATS:
		cwShowStats = !cwShowStats;
		return;  // editor-only preview state
	case CWROW_TARGET_COUNT:
		cwTargetCount = cwClamp(cwTargetCount + dir, 0, CW_DUMMY_MAX);
		cwLayoutDummies();
		return;
	case CWROW_TARGET_SPREAD:
		cwTargetSpread = cwClamp(cwTargetSpread + dir * 4, 6, 60);
		cwLayoutDummies();
		return;
	case CWROW_TARGET_HEIGHT:
		cwTargetHeight = cwClamp(cwTargetHeight + dir * 4, 16, 160);
		cwLayoutDummies();
		return;
	case CWROW_TARGET_X:
		cwTargetX = cwClamp(cwTargetX + dir * 4, 20, 132);
		cwLayoutDummies();
		return;
	case CWROW_TARGET_ARMOR:
		cwTargetArmor = cwClamp(cwTargetArmor + dir * 20, 20, 600);
		cwLayoutDummies();
		return;
	case CWROW_SHIP_X:  // ranges keep the 24x28 ship sprite fully inside the 8..143 x 8..182 box
		cwShipX = cwClamp(cwShipX + dir * 2, 14, 124);
		return;
	case CWROW_SHIP_Y:
		cwShipY = cwClamp(cwShipY + dir * 2, 16, 160);
		return;
	case CWROW_IMPORT_SRC:
		if (customBulletPresetCount > 0)
		{
			cwImportSrc = (cwImportSrc + customBulletPresetCount + dir) % customBulletPresetCount;
			cwImportPwr = cwClamp(cwImportPwr, 1, customBulletMaxPower(cwImportSrc));
		}
		return;  // editor-only state
	case CWROW_IMPORT_PWR:
		cwImportPwr = cwClamp(cwImportPwr + dir, 1, customBulletMaxPower(cwImportSrc));
		return;  // editor-only state
	case CWROW_BULLET_SEL:
		cwBulletSel = cwClamp(cwBulletIndex() + dir, 0, (w->multi < 1 ? 1 : w->multi) - 1);
		return;  // editor-only selection

	case CWROW_TWO_MODES:   customWeaponModes = cwClamp(customWeaponModes + (dir >= 0 ? 1 : -1), 1, CUSTOM_WEAPON_MODES);
	                        if (customWeaponModes < 2) customWeaponEditMode = 0; break;
	case CWROW_EQUIP_SLOT:  customWeaponEquipSlot = (customWeaponEquipSlot + CUSTOM_EQUIP_COUNT + dir) % CUSTOM_EQUIP_COUNT; break;
	case CWROW_COST:        customWeaponCost      = cwClamp(customWeaponCost + dir * 500, 0, 64000); break;
	case CWROW_ICON:        customWeaponItemGraphic = cwClamp(customWeaponItemGraphic + dir, 1, 237); break;
	case CWROW_POWER_USE:   customWeaponPowerUse  = cwClamp(customWeaponPowerUse + dir, 0, 255); break;
	case CWROW_CHARGE:      customWeaponChargeStages = cwClamp(customWeaponChargeStages + dir, 1, CUSTOM_POWER_LEVELS); break;
	case CWROW_SK_MOUNT:
		customSidekickMount = (customSidekickMount + CUSTOM_SIDEKICK_MOUNTS + dir) % CUSTOM_SIDEKICK_MOUNTS;
		{  // the mount picks the body sheet, so re-clamp the sprite to the new sheet's 1-based range
			const int cnt = customSidekickSpriteCount(customSidekickMount);
			if (cnt > 0) customSidekickSprite = cwClamp(customSidekickSprite, 1, cnt);
		}
		break;
	case CWROW_SK_SPRITE:
		{  // body sprite is 1-based (blit reads offsetTable[index-1]); 0 would crash
			const int cnt = customSidekickSpriteCount(customSidekickMount);
			customSidekickSprite = cwClamp(customSidekickSprite + dir, 1, (cnt > 0) ? cnt : 65535);
		}
		break;
	case CWROW_SK_FRAMES:   customSidekickFrames    = cwClamp(customSidekickFrames + dir, 1, 20); break;
	case CWROW_SK_STEP:     customSidekickFrameStep = cwClamp(customSidekickFrameStep + dir, 0, 40); break;
	case CWROW_SK_ANIMATE:  customSidekickAnimate   = cwClamp(customSidekickAnimate + (dir >= 0 ? 1 : -1), 1, 2); break;

	// Ranges span each field's full valid range so any imported value stays reachable.
	case CWROW_FIRE_RATE:   w->shotrepeat = (JE_byte)cwClamp(w->shotrepeat + dir, 0, 255); break;
	case CWROW_BULLETS:     w->multi = (JE_byte)cwClamp(w->multi + dir, 1, CUSTOM_BULLETS_MAX);
	                        if (w->max < w->multi) w->max = w->multi; break;
	case CWROW_PATTERN:     w->max = (JE_byte)cwClamp(w->max + dir, 1, CUSTOM_BULLETS_MAX); break;
	case CWROW_ANIM:        w->weapani = (JE_word)cwClamp((int)w->weapani + dir, 0, 255); break;
	case CWROW_HOMING:      w->aim = (JE_byte)cwClamp(w->aim + dir, 0, 255); break;
	case CWROW_ACCEL_Y:     w->acceleration  = (JE_shortint)cwClamp(w->acceleration + dir, -128, 127); break;
	case CWROW_ACCEL_X:     w->accelerationx = (JE_shortint)cwClamp(w->accelerationx + dir, -128, 127); break;
	case CWROW_SPIRAL:      w->circlesize = (JE_byte)cwClamp(w->circlesize + dir, 0, 255); break;
	case CWROW_SOUND:       w->sound = (JE_byte)cwClamp(w->sound + dir, 0, CUSTOM_SOUND_MAX); break;
	case CWROW_TRAIL:       w->trail = (JE_byte)cwClamp(w->trail + dir, 0, 255); break;
	case CWROW_BLAST:       w->shipblastfilter = (JE_byte)cwClamp(w->shipblastfilter + dir, 0, 255); break;

	case CWROW_B_SPRITE:    w->sg[b] = (JE_word)cwClamp((int)w->sg[b] + dir, 0, 65535); break;
	case CWROW_B_DAMAGE:  // 0=None, 1-98 damage, 99 Ice; leaves the explode range to -Explode To
	{
		const int dmg = (w->attack[b] <= 99) ? w->attack[b] : 10;  // editing damage cancels an explosion
		w->attack[b] = (JE_byte)cwClamp(dmg + dir, 0, 99);
		break;
	}
	case CWROW_B_CHAIN:   // 0=Off (normal damage), else explode into weapon N (attack = 100+N)
	{
		const int tgt = (w->attack[b] >= 101 && w->attack[b] <= 249) ? w->attack[b] - 100 : 0;
		const int nt  = cwClamp(tgt + dir, 0, 149);
		w->attack[b] = (JE_byte)(nt == 0 ? 10 : 100 + nt);  // Off restores a modest damage
		break;
	}
	case CWROW_B_LIFE:      w->del[b] = (JE_byte)cwClamp(w->del[b] + dir, 0, 255); break;
	case CWROW_B_VELX:      w->sx[b] = (JE_shortint)cwClamp(w->sx[b] + dir, -128, 127); break;
	case CWROW_B_VELY:      w->sy[b] = (JE_shortint)cwClamp(w->sy[b] + dir, -128, 127); break;
	case CWROW_B_OFFX:      w->bx[b] = (JE_shortint)cwClamp(w->bx[b] + dir, -128, 127); break;
	case CWROW_B_OFFY:      w->by[b] = (JE_shortint)cwClamp(w->by[b] + dir, -128, 127); break;
	default: return;  // CWROW_NAME is edited by typing
	}
	customWeaponMaterialize();
}

// ---- direct numeric entry ------------------------------------------------------
// Many value rows have wide ranges or coarse Left/Right steps (cost jumps by 500, a
// bullet sprite runs 0..65535), so the editor also lets you type an exact number for
// them. cwNumericRange says which rows accept typing and their valid range; cwSetNumeric
// applies a typed value; cwCommitNumeric finalizes whatever is being typed.

// Fill lo/hi with a row's valid range and return true if it accepts a typed number.
// A negative lo marks a signed field (a leading '-' may be typed).
static bool cwNumericRange(int row, int *lo, int *hi)
{
	switch (row)
	{
	case CWROW_COST:          *lo = 0;    *hi = 64000;            return true;
	case CWROW_ICON:          *lo = 1;    *hi = 237;              return true;
	case CWROW_POWER_USE:     *lo = 0;    *hi = 255;              return true;
	case CWROW_SK_SPRITE:     { const int cnt = customSidekickSpriteCount(customSidekickMount);
	                            *lo = 1; *hi = (cnt > 0) ? cnt : 65535; return true; }  // 1-based body sprite
	case CWROW_SK_FRAMES:     *lo = 1;    *hi = 20;               return true;
	case CWROW_SK_STEP:       *lo = 0;    *hi = 40;               return true;
	case CWROW_FIRE_RATE:     *lo = 0;    *hi = 255;              return true;
	case CWROW_HOMING:        *lo = 0;    *hi = 255;              return true;
	case CWROW_ACCEL_Y:       *lo = -128; *hi = 127;              return true;
	case CWROW_ACCEL_X:       *lo = -128; *hi = 127;              return true;
	case CWROW_SPIRAL:        *lo = 0;    *hi = 255;              return true;
	case CWROW_SOUND:         *lo = 0;    *hi = CUSTOM_SOUND_MAX; return true;
	case CWROW_TRAIL:         *lo = 0;    *hi = 255;              return true;
	case CWROW_BLAST:         *lo = 0;    *hi = 255;              return true;
	case CWROW_B_SPRITE:      *lo = 0;    *hi = 65535;            return true;
	case CWROW_B_DAMAGE:      *lo = 0;    *hi = 99;               return true;
	case CWROW_B_LIFE:        *lo = 0;    *hi = 255;              return true;
	case CWROW_B_VELX:        *lo = -128; *hi = 127;              return true;
	case CWROW_B_VELY:        *lo = -128; *hi = 127;              return true;
	case CWROW_B_OFFX:        *lo = -128; *hi = 127;              return true;
	case CWROW_B_OFFY:        *lo = -128; *hi = 127;              return true;
	// editor-only preview settings (coarse steps, so typing helps here too)
	case CWROW_TARGET_COUNT:  *lo = 0;    *hi = CW_DUMMY_MAX;     return true;
	case CWROW_TARGET_SPREAD: *lo = 6;    *hi = 60;               return true;
	case CWROW_TARGET_HEIGHT: *lo = 16;   *hi = 160;              return true;
	case CWROW_TARGET_X:      *lo = 20;   *hi = 132;              return true;
	case CWROW_TARGET_ARMOR:  *lo = 20;   *hi = 600;              return true;
	case CWROW_SHIP_X:        *lo = 14;   *hi = 124;              return true;
	case CWROW_SHIP_Y:        *lo = 16;   *hi = 160;              return true;
	default:                                                      return false;
	}
}

// Apply a typed value to a row (clamped to its range), re-materializing when it changed
// the actual design. Rows cwNumericRange rejects are ignored.
static void cwSetNumeric(int row, int v)
{
	int lo, hi;
	if (!cwNumericRange(row, &lo, &hi))
		return;
	v = cwClamp(v, lo, hi);

	JE_WeaponType *w = cwCur();
	const int b = cwBulletIndex();
	switch (row)
	{
	case CWROW_COST:          customWeaponCost        = v; break;
	case CWROW_ICON:          customWeaponItemGraphic = v; break;
	case CWROW_POWER_USE:     customWeaponPowerUse    = v; break;
	case CWROW_SK_SPRITE:     customSidekickSprite    = v; break;
	case CWROW_SK_FRAMES:     customSidekickFrames    = v; break;
	case CWROW_SK_STEP:       customSidekickFrameStep = v; break;
	case CWROW_FIRE_RATE:     w->shotrepeat      = (JE_byte)v;     break;
	case CWROW_HOMING:        w->aim             = (JE_byte)v;     break;
	case CWROW_ACCEL_Y:       w->acceleration    = (JE_shortint)v; break;
	case CWROW_ACCEL_X:       w->accelerationx   = (JE_shortint)v; break;
	case CWROW_SPIRAL:        w->circlesize      = (JE_byte)v;     break;
	case CWROW_SOUND:         w->sound           = (JE_byte)v;     break;
	case CWROW_TRAIL:         w->trail           = (JE_byte)v;     break;
	case CWROW_BLAST:         w->shipblastfilter = (JE_byte)v;     break;
	case CWROW_B_SPRITE:      w->sg[b]     = (JE_word)v;     break;
	case CWROW_B_DAMAGE:      w->attack[b] = (JE_byte)v;     break;  // 0 none / 1-98 dmg / 99 Ice
	case CWROW_B_LIFE:        w->del[b]    = (JE_byte)v;     break;
	case CWROW_B_VELX:        w->sx[b]     = (JE_shortint)v; break;
	case CWROW_B_VELY:        w->sy[b]     = (JE_shortint)v; break;
	case CWROW_B_OFFX:        w->bx[b]     = (JE_shortint)v; break;
	case CWROW_B_OFFY:        w->by[b]     = (JE_shortint)v; break;
	case CWROW_TARGET_COUNT:  cwTargetCount  = v; cwLayoutDummies(); return;
	case CWROW_TARGET_SPREAD: cwTargetSpread = v; cwLayoutDummies(); return;
	case CWROW_TARGET_HEIGHT: cwTargetHeight = v; cwLayoutDummies(); return;
	case CWROW_TARGET_X:      cwTargetX      = v; cwLayoutDummies(); return;
	case CWROW_TARGET_ARMOR:  cwTargetArmor  = v; cwLayoutDummies(); return;
	case CWROW_SHIP_X:        cwShipX = v; return;
	case CWROW_SHIP_Y:        cwShipY = v; return;
	default: return;
	}
	customWeaponMaterialize();
}

// Finalize any in-progress typed value to its field and leave typing mode. An empty
// buffer (or a lone '-') is discarded, leaving the field unchanged.
static void cwCommitNumeric(void)
{
	if (cwNumRow < 0)
		return;
	if (cwNumText[0] != '\0' && !(cwNumText[0] == '-' && cwNumText[1] == '\0'))
		cwSetNumeric(cwNumRow, SDL_atoi(cwNumText));
	cwNumRow = -1;
	cwNumText[0] = '\0';
}

// Run one editor action, shared by the inline action rows and the bottom action list.
// done/equipped are set by EQUIP and DONE (harmless for the other actions).
static void cwPerformAction(int act, bool *done, bool *equipped)
{
	switch (act)
	{
	case CWACT_ADD_BULLET:
	{
		const int ni = customWeaponAddBullet(cwBulletIndex());
		if (ni >= 0) { cwBulletSel = ni; customWeaponMaterialize(); JE_playSampleNum(S_SELECT); }
		else         JE_playSampleNum(S_SPRING);  // already at 8 bullets
		break;
	}
	case CWACT_REMOVE_BULLET:
	{
		const int ni = customWeaponRemoveBullet(cwBulletIndex());
		if (ni >= 0) { cwBulletSel = ni; customWeaponMaterialize(); JE_playSampleNum(S_SELECT); }
		else         JE_playSampleNum(S_SPRING);  // can't remove the last segment
		break;
	}
	case CWACT_ADD_CHARGE:
		if (customWeaponAddChargeState() >= 0) { customWeaponMaterialize(); JE_playSampleNum(S_SELECT); }
		else                                   JE_playSampleNum(S_SPRING);  // already 11 states
		break;
	case CWACT_REMOVE_CHARGE:
		if (customWeaponRemoveChargeState() >= 0) { customWeaponMaterialize(); JE_playSampleNum(S_SELECT); }
		else                                      JE_playSampleNum(S_SPRING);  // only 1 shot left
		break;
	case CWACT_IMPORT_LEVEL: customWeaponImportLevel(cwImportSrc, cwImportPwr); customWeaponMaterialize(); JE_playSampleNum(S_SELECT); break;
	case CWACT_IMPORT_ALL:   customWeaponImportAllLevels(cwImportSrc);          customWeaponMaterialize(); JE_playSampleNum(S_SELECT); break;
	case CWACT_ADD_LEVEL:      customWeaponAddLevel(cwImportSrc, cwImportPwr);  customWeaponMaterialize(); JE_playSampleNum(S_SELECT); break;
	case CWACT_ADD_ALL_LEVELS: customWeaponAddAllLevels(cwImportSrc);          customWeaponMaterialize(); JE_playSampleNum(S_SELECT); break;
	case CWACT_COPY_ALL:     customWeaponCopyToAllLevels();                     customWeaponMaterialize(); JE_playSampleNum(S_SELECT); break;
	case CWACT_AUTOSCALE:    customWeaponAutoScaleLevels();                     customWeaponMaterialize(); JE_playSampleNum(S_SELECT); break;
	case CWACT_RANDOMIZE:    customWeaponRandomize();                           customWeaponMaterialize(); JE_playSampleNum(S_SELECT); break;
	case CWACT_RESET:        customWeaponReset();                               customWeaponMaterialize(); JE_playSampleNum(S_SELECT); break;
	case CWACT_RESET_ALL:    customWeaponResetAllLevels();                      customWeaponMaterialize(); JE_playSampleNum(S_SELECT); break;
	case CWACT_EQUIP:
		if (customWeaponEquip()) { *equipped = true; JE_playSampleNum(S_SELECT); *done = true; }
		else                     JE_playSampleNum(S_SPRING);  // e.g. no free sidekick slot
		break;
	case CWACT_SAVE:
		customWeaponLibrarySave();   // flush the weapon library to disk now, without leaving
		save_opentyrian_config();    // ... and the active design in the main config
		JE_playSampleNum(S_SELECT);
		cwSavedFlash = 40;           // show "Weapon saved!" on the hint line briefly
		break;
	case CWACT_DONE:         *done = true; break;
	case CWACT_LIB_NEW:
		if (customWeaponLibraryNew() >= 0) JE_playSampleNum(S_SELECT);
		else                               JE_playSampleNum(S_SPRING);  // library full
		break;
	case CWACT_LIB_DUPLICATE:
		if (customWeaponLibraryDuplicate() >= 0) JE_playSampleNum(S_SELECT);
		else                                     JE_playSampleNum(S_SPRING);  // library full
		break;
	case CWACT_LIB_DELETE:
		if (customWeaponLibraryDelete() >= 0) JE_playSampleNum(S_SELECT);
		else                                  JE_playSampleNum(S_SPRING);  // can't delete the last one
		break;
	}
}

// Restore the backdrop snapshot (VGAScreen2) into VGAScreen over an inclusive rect.
// Used to clip the (full-screen) preview starfield and any shots that fly out of the
// box back to the box, so nothing smears onto the surrounding UI.
static void cwRestoreRect(int x0, int y0, int x1, int y1)
{
	if (x1 < x0 || y1 < y0)
		return;
	for (int y = y0; y <= y1; ++y)
	{
		const Uint8 *src = (const Uint8 *)VGAScreen2->pixels + y * VGAScreen2->pitch + x0;
		Uint8       *dst = (Uint8 *)VGAScreen->pixels + y * VGAScreen->pitch + x0;
		memcpy(dst, src, (size_t)(x1 - x0 + 1));
	}
}

// ---- sprite preview + segment hitbox overlay ------------------------------------
// The number of sprites in a compiled Sprite2 sheet: the offset table (one JE_word per
// sprite) precedes the sprite data, so the first offset is the table's byte length.
static int cwSprite2Count(Sprite2_array s)
{
	if (s.data == NULL || s.size < 2)
		return 0;
	return SDL_SwapLE16(((const Uint16 *)s.data)[0]) / 2;
}

// Resolve a raw sg to a drawable Sprite2 sheet + index, mirroring the shot draw
// (shots.c:99-113): sg wraps mod 1000, >500 -> spriteSheet12[sg-500] else spriteSheet8[sg].
// Returns false for a blended (sg>=60000) or empty/out-of-range sprite.
static bool cwResolveShotSprite(JE_word sg, Sprite2_array *sheet, int *index)
{
	JE_word af = sg;
	if (af >= 60000)
		return false;                       // blended OPTION_SHAPES sprite; caller handles
	if (af > 1000)
		af %= 1000;
	Sprite2_array s = (af > 500) ? spriteSheet12 : spriteSheet8;
	int idx        = (af > 500) ? (af - 500) : af;
	if (idx < 1 || idx > cwSprite2Count(s))
		return false;                       // 0 = no sprite, or beyond the sheet
	*sheet = s;
	*index = idx;
	return true;
}

// Decode a Sprite2 sprite's opaque bounding box within its 12px-wide cell (rows are 12px;
// each control byte = low-nibble transparent skip + high-nibble opaque run, 0x0f ends).
static bool cwSpriteExtent(JE_word sg, int *ox, int *oy, int *ow, int *oh)
{
	Sprite2_array s;
	int index;
	if (!cwResolveShotSprite(sg, &s, &index))
		return false;

	const Uint8 *data = (const Uint8 *)s.data + SDL_SwapLE16(((const Uint16 *)s.data)[index - 1]);
	int x = 0, row = 0, minx = 12, maxx = -1, miny = 9999, maxy = -1;
	for (; *data != 0x0f; ++data)
	{
		x += *data & 0x0f;                  // transparent skip within the row
		int count = (*data & 0xf0) >> 4;    // opaque run length
		if (count == 0)                     // move to the next row
		{
			++row;
			x = 0;
			continue;
		}
		if (x < minx) minx = x;
		data += count;                      // skip the run's pixel bytes
		x    += count;
		if (x - 1 > maxx) maxx = x - 1;
		if (row < miny) miny = row;
		if (row > maxy) maxy = row;
	}
	if (maxx < minx || maxy < miny)
		return false;
	*ox = minx; *oy = miny; *ow = maxx - minx + 1; *oh = maxy - miny + 1;
	return true;
}

// Draw the segment's shot sprite at (x, y) — its cell top-left — exactly as the shot
// renderer would (so the preview matches what actually fires).
static void cwDrawSegmentSprite(int x, int y, JE_word sg)
{
	Sprite2_array sheet;
	int index;
	if (cwResolveShotSprite(sg, &sheet, &index))
		blit_sprite2(VGAScreen, x, y, sheet, index);
	else if (sg >= 60000)
		blit_sprite_blend(VGAScreen, x, y, OPTION_SHAPES, sg - 60001);
}

// The editor row selected while presenting; drives the preview overlay (-1 = none).
static int cwOvRow = -1;
// The live player shot the segment corners are following (-1 = none). Locked once per
// tick in cwUpdateTrackedShot() so the brackets stay on one shot as it flies up.
static int cwTrackedShot = -1;
static int cwTrackedSeg  = -1;  // which bullet segment that shot belongs to (so switching
                                // segments re-locks even when segments share a sprite)

// Pick/keep the shot the corners track. Keep following the current one only while it is
// alive, still the edited segment's sprite, AND still the segment we locked onto — otherwise
// re-acquire, preferring a live shot near the selected segment's spawn column (so segments
// that share a sprite resolve to the right one), freshest (nearest-ship) as the tie-break.
// Call once per tick, after the shots have moved.
static void cwUpdateTrackedShot(int row)
{
	if (row < 0 || cwGroupColor(row) != CW_BG_BULLET)
	{
		cwTrackedShot = -1;
		cwTrackedSeg  = -1;
		return;
	}
	const int b = cwBulletIndex();
	const JE_word sg = cwCur()->sg[b];
	if (cwTrackedShot >= 0 && cwTrackedShot < MAX_PWEAPON && cwTrackedSeg == b
	    && shotAvail[cwTrackedShot] != 0 && playerShotData[cwTrackedShot].shotGr == sg)
		return;  // still valid for this segment — keep following it

	const int spawnX = player[0].x + cwCur()->bx[b] + 1;  // where this segment's shots appear
	int best = -1, bestScore = INT_MAX;
	for (int i = 0; i < MAX_PWEAPON; ++i)
		if (shotAvail[i] != 0 && playerShotData[i].shotGr == sg)
		{
			const int dx = playerShotData[i].shotX - spawnX;
			// nearest spawn column dominates; fresher (larger shotY) breaks ties
			const int score = (dx < 0 ? -dx : dx) * 4 - playerShotData[i].shotY;
			if (score < bestScore) { bestScore = score; best = i; }
		}
	cwTrackedShot = best;
	cwTrackedSeg  = (best >= 0) ? b : -1;
}

// Draw only the four 2px corner brackets of a box (no full outline), clipped to the box.
static void cwDrawSegmentCorners(int x0, int y0, int x1, int y1)
{
	enum { BX0 = 8, BY0 = 8, BX1 = 143, BY1 = 182 };
	if (x0 < BX0) x0 = BX0;
	if (y0 < BY0) y0 = BY0;
	if (x1 > BX1) x1 = BX1;
	if (y1 > BY1) y1 = BY1;
	if (x1 - x0 < 2 || y1 - y0 < 2)
		return;
	const Uint8 c = 0x9C;  // bright blue (palette 0 bank 9)
	fill_rectangle_xy(VGAScreen, x0, y0, x0 + 1, y0, c); fill_rectangle_xy(VGAScreen, x0, y0, x0, y0 + 1, c);
	fill_rectangle_xy(VGAScreen, x1 - 1, y0, x1, y0, c); fill_rectangle_xy(VGAScreen, x1, y0, x1, y0 + 1, c);
	fill_rectangle_xy(VGAScreen, x0, y1 - 1, x0, y1, c); fill_rectangle_xy(VGAScreen, x0, y1, x0 + 1, y1, c);
	fill_rectangle_xy(VGAScreen, x1, y1 - 1, x1, y1, c); fill_rectangle_xy(VGAScreen, x1 - 1, y1, x1, y1, c);
}

// Drawn on top of the finished preview box (via weaponSimOverlayFn). Bullet-segment
// category: 2px corner brackets tracking one live shot of the edited segment, interpolated
// with alpha so they glide with it. Sprite or Shop Icon row: an isolated sprite/icon swatch
// in the bottom-left corner so scrubbing either shows what you are picking.
static void cwDrawPreviewOverlay(float alpha)
{
	const int row = cwOvRow;
	if (row < 0)
		return;
	const bool segCat  = (cwGroupColor(row) == CW_BG_BULLET);
	const bool iconRow = (row == CWROW_ICON);
	if (!segCat && !iconRow)
		return;

	JE_WeaponType *w = cwCur();
	const int b = cwBulletIndex();

	if (segCat && cwTrackedShot >= 0 && cwTrackedShot < MAX_PWEAPON && shotAvail[cwTrackedShot] != 0)
	{
		const PlayerShotDataType *s = &playerShotData[cwTrackedShot];
		const int dvx = (s->shotXM > 100 || s->shotXM < -100) ? 0 : s->shotXM;  // per-tick delta
		const int dvy = (s->shotYM > 100 || s->shotYM < -100) ? 0 : s->shotYM;  // (skip locked sentinels)
		// Player shots EXTRAPOLATE forward at the render rate (render_list.c:799/814:
		// cur + dx*alpha), so the brackets must too, or they trail the bullet by a tick.
		const int px = s->shotX + 1 + (int)(dvx * alpha);  // shot is drawn at shotX+1 (shots.c:111)
		const int py = s->shotY     + (int)(dvy * alpha);
		int ox, oy, ow, oh;
		if (cwSpriteExtent(s->shotGr, &ox, &oy, &ow, &oh))
			cwDrawSegmentCorners(px + ox - 1, py + oy - 1, px + ox + ow, py + oy + oh);
	}

	if (row == CWROW_B_SPRITE || iconRow)
	{
		enum { PV = 32 };
		const int px0 = 10, py0 = 182 - 2 - PV;
		fill_rectangle_xy(VGAScreen, px0, py0, px0 + PV, py0 + PV, 0);   // dark backing
		JE_rectangle(VGAScreen, px0, py0, px0 + PV, py0 + PV, 0xFB);     // frame
		const int cx = px0 + PV / 2, cy = py0 + PV / 2;
		if (iconRow)
		{
			blit_sprite2x2(VGAScreen, cx - 12, cy - 14, shopSpriteSheet, cwClamp(customWeaponItemGraphic, 1, 237));
		}
		else
		{
			int sx, sy, sw, sh;
			if (cwSpriteExtent(w->sg[b], &sx, &sy, &sw, &sh))
				cwDrawSegmentSprite(cx - (sx + sw / 2), cy - (sy + sh / 2), w->sg[b]);
			else
				cwDrawSegmentSprite(cx - 6, cy - 6, w->sg[b]);
		}
	}
}

// ---- combat preview: collision + effects ---------------------------------------
// The preview box has no enemies, so a weapon's on-hit behaviour (Damage, Ice,
// Explode-To chaining, Homing) is normally invisible — every weapon just fires straight
// bullets. A lightweight collision pass against the dummies (laid out above) mirrors the
// game (tyrian2.c ~2263): throw real impact explosions, cascade the chained weapon, freeze
// on Ice, and drain a regenerating armour bar so damage magnitude reads at a glance. Homing
// is nudged here too, since the shop shot simulator (simulate_player_shots) ignores aim.

// Position the dummies from the current settings and clear any leftover explosions so the
// preview starts clean. Called when the creator opens.
static void cwResetDummies(void)
{
	cwLayoutDummies();
	for (int j = 0; j < MAX_EXPLOSIONS; ++j)
		explosions[j].ttl = 0;
}

// Curve the edited weapon's live shots toward the nearest dummy (X only, so the bend is
// clearly visible) when Homing is on. Mirrors the game's per-tick +/-1 nudge (shots.c:279);
// the shop simulator doesn't home, so we do it here. All preview shots are the custom weapon.
static void cwHomeShots(void)
{
	if (cwCur()->aim <= 5 || cwDummyCount == 0)
		return;
	for (int z = 0; z < MAX_PWEAPON - 1; ++z)
	{
		if (shotAvail[z] == 0)
			continue;
		PlayerShotDataType *s = &playerShotData[z];
		if (s->shotXM > 100 || s->shotXM < -100)
			continue;  // ship-locked sentinel — leave it alone
		const int scx = s->shotX + 6, scy = s->shotY + 6;
		int bi = -1, bd = INT_MAX;
		for (int d = 0; d < cwDummyCount; ++d)
		{
			const int dx = cwDummies[d].cx - scx, dy = cwDummies[d].cy - scy;
			const int dist = dx * dx + dy * dy;
			if (dist < bd) { bd = dist; bi = d; }
		}
		if (bi < 0)
			continue;
		if (scx < cwDummies[bi].cx && s->shotXM < 20)       ++s->shotXM;
		else if (scx > cwDummies[bi].cx && s->shotXM > -20) --s->shotXM;
	}
}

// Collide live shots against the dummies (called after they move). Mirrors the game's
// on-hit handling: an Explode-To carrier spawns the target weapon at the impact and is
// consumed; Ice (99) freezes; other damage drains armour; every hit throws a real impact
// explosion. Non-piercing shots are consumed on contact, exactly like in-game.
static void cwCollideShots(void)
{
	if (cwDummyCount == 0)
		return;
	int chainSpawns = 0;  // cap per-tick cascade spawns so a self-chaining weapon stays tame
	for (int z = 0; z < MAX_PWEAPON - 1; ++z)
	{
		if (shotAvail[z] == 0)
			continue;
		PlayerShotDataType *s = &playerShotData[z];
		const int scx = s->shotX + 6, scy = s->shotY + 6;
		for (int d = 0; d < cwDummyCount; ++d)
		{
			if (abs(scx - cwDummies[d].cx) >= CW_DUMMY_R + 4 ||
			    abs(scy - cwDummies[d].cy) >= CW_DUMMY_R + 4)
				continue;

			if (s->chainReaction > 0 && s->chainReaction <= 149)
			{
				if (chainSpawns < 16)  // spawn the chained weapon at the impact point
				{
					shotMultiPos[SHOT_MISC] = 0;
					player_shot_create(0, SHOT_MISC, s->shotX, s->shotY, mouseX, mouseY,
					                   s->chainReaction, 1);
					++chainSpawns;
				}
				JE_setupExplosion(s->shotX, s->shotY, 0, 0, false, false);
				shotAvail[z] = 0;   // the carrier is consumed
			}
			else
			{
				if (s->shotDmg == 99)   // Ice — freezes instead of damaging (game sets dmg 0)
				{
					cwDummies[d].ice = 18;
				}
				else
				{
					int dmg = s->shotDmg;
					if (dmg >= 250) dmg -= 250;   // piercing carries damage + 250
					cwDummies[d].armor -= dmg;
					cwDummies[d].hit = 3;
					if (cwDummies[d].armor <= 0)
					{
						cwDummies[d].armor = cwTargetArmor;  // endless test dummy: refill
						cwDummies[d].dead  = 8;
					}
				}
				JE_setupExplosion(s->shotX, s->shotY, 0, 0, false, false);
				if (s->shotDmg < 250)   // non-piercing shots stop at the target
					shotAvail[z] = 0;
			}
			break;  // one dummy per shot per tick
		}
	}
}

// Advance and draw the impact explosions — an editor-local pump over the shared
// explosions[] array (no background scroll here, so none of the game's explodeMove drift).
// Kept inside the record so they interpolate smoothly and clip to the box like the shots.
static void cwPumpExplosions(void)
{
	for (int j = 0; j < MAX_EXPLOSIONS; ++j)
	{
		if (explosions[j].ttl == 0)
			continue;
		if (!explosions[j].fixedPosition)
			explosions[j].sprite++;
		explosions[j].y += explosions[j].deltaY;
		rl_current_id = RL_ID_EXPL_BASE + j * 4 + (explosions[j].id_gen & 3);
		blit_sprite2(VGAScreen, explosions[j].x, explosions[j].y, explosionSpriteSheet, explosions[j].sprite + 1);
		rl_current_id = 0;
		explosions[j].ttl--;
	}
}

// Draw the dummies (reticle body + crosshair + frame + a regenerating armour bar). The
// flash timers tint them on hit / freeze / break; they tick down and armour regenerates
// here (once per tick). Drawn before the shots so bullets pass in front.
static void cwDrawDummies(void)
{
	for (int d = 0; d < cwDummyCount; ++d)
	{
		CwDummy *t = &cwDummies[d];
		const int x0 = t->cx - CW_DUMMY_R, y0 = t->cy - CW_DUMMY_R;
		const int x1 = t->cx + CW_DUMMY_R, y1 = t->cy + CW_DUMMY_R;

		Uint8 body = 0x06, ring = 0xFB;              // steel body, gold frame
		if (t->ice  > 0) { body = 0x9A; ring = 0x9C; }   // frozen: blue
		if (t->hit  > 0)   body = 0x0D;                  // hit flash: bright
		if (t->dead > 0) { body = 0x0F; ring = 0xF5; }   // broken: white flash, red frame

		fill_rectangle_xy(VGAScreen, x0, y0, x1, y1, body);
		fill_rectangle_xy(VGAScreen, x0 + 2, t->cy, x1 - 2, t->cy, 0);  // crosshair
		fill_rectangle_xy(VGAScreen, t->cx, y0 + 2, t->cx, y1 - 2, 0);
		JE_rectangle(VGAScreen, x0, y0, x1, y1, ring);

		// Armour bar just below, width proportional to armour left (green high, red low).
		const int bw   = 2 * CW_DUMMY_R;
		const int fill = (cwTargetArmor > 0) ? t->armor * bw / cwTargetArmor : 0;
		fill_rectangle_xy(VGAScreen, x0, y1 + 3, x1, y1 + 4, 0x02);
		if (fill > 0)
			fill_rectangle_xy(VGAScreen, x0, y1 + 3, x0 + fill, y1 + 4,
			                  (t->armor * 2 >= cwTargetArmor) ? 0x2B : 0xF5);

		if (t->armor < cwTargetArmor) ++t->armor;  // passive regen
		if (t->hit  > 0) --t->hit;
		if (t->ice  > 0) --t->ice;
		if (t->dead > 0) --t->dead;
	}
}

// Live stats readout (top-left of the box) for the level/mode currently previewing:
// approximate DPS, per-shot damage x bullet count, and fire rate. The weapon fires every
// shotrepeat+1 ticks at the SIM_FPS=35 sim rate (nortsong.c). Ice does no HP damage; a
// chaining bullet's real damage is the weapon it spawns, so it's flagged, not counted.
static void cwDrawStats(void)
{
	const JE_WeaponType *w = cwCur();
	int multi = w->multi;
	if (multi < 1) multi = 1;
	if (multi > CUSTOM_BULLETS_MAX) multi = CUSTOM_BULLETS_MAX;

	int dmg = 0;
	bool ice = false, chain = false;
	for (int i = 0; i < multi; ++i)
	{
		const int a = w->attack[i];
		if (a == 99)                   ice = true;              // freeze, no HP damage
		else if (a >= 101 && a <= 249) chain = true;           // explodes into another weapon
		else if (a >= 250)             dmg += a - 250;          // piercing carries damage + 250
		else                           dmg += a;
	}
	const int period = w->shotrepeat + 1;     // ticks between volleys
	const int dps    = dmg * 35 / period;     // SIM_FPS = 35 ticks/sec
	const int rate10 = 35 * 10 / period;      // volleys/sec x10 (one decimal)

	char l1[24], l2[24], l3[24];
	snprintf(l1, sizeof(l1), "DPS ~%d%s", dps, chain ? " +ch" : (ice && dmg == 0) ? " ice" : "");
	snprintf(l2, sizeof(l2), "%d dmg x%d", dmg, multi);
	snprintf(l3, sizeof(l3), "%d.%d/sec", rate10 / 10, rate10 % 10);

	enum { SX = 11, SY = 10 };
	fill_rectangle_xy(VGAScreen, SX - 2, SY - 1, SX + 92, SY + 24, 0);   // dark backing for legibility
	JE_rectangle(VGAScreen, SX - 2, SY - 1, SX + 92, SY + 24, 0xF4);     // subtle frame
	draw_font_hv_shadow(VGAScreen, SX, SY + 1,  l1, small_font, left_aligned, 15, 4, false, 1);
	draw_font_hv_shadow(VGAScreen, SX, SY + 9,  l2, small_font, left_aligned, 15, 1, false, 1);
	draw_font_hv_shadow(VGAScreen, SX, SY + 17, l3, small_font, left_aligned, 15, 1, false, 1);
}

// Draw the equipped sidekick's body sprite in the preview box, roughly where it would sit
// relative to the ship, so you can SEE the mount + sprite you're editing. Only shown when
// the weapon is actually equipped as a sidekick. The materialized option holds a base sprite
// already clamped in-range, so the (unclipped) blit is safe.
static void cwDrawSidekickPreview(void)
{
	const bool isSidekick = (customWeaponEquipSlot == CUSTOM_EQUIP_LEFT ||
	                         customWeaponEquipSlot == CUSTOM_EQUIP_RIGHT ||
	                         customWeaponEquipSlot == CUSTOM_EQUIP_BOTH);
	if (!isSidekick || customSidekickSlot <= 0 || customSidekickSlot > OPTION_NUM)
		return;

	const JE_OptionType *o = &options[customSidekickSlot];
	const int tr = o->tr;
	int x, y;
	switch (tr)
	{
	case 2:  x = cwShipX - 6;  y = cwShipY - 22; break;   // front (ahead of the ship)
	case 1:  x = cwShipX - 6;  y = cwShipY + 16; break;   // trailing (large body)
	case 3:  x = cwShipX - 4;  y = cwShipY + 16; break;   // trailing (single tile)
	case 4:  x = cwShipX + 16; y = cwShipY;      break;   // orbit (show to one side)
	default: x = cwShipX - 15; y = cwShipY;      break;   // side pod
	}
	// keep the whole body inside the box (8..143 x 8..182); the sidekick blit isn't clipped
	x = (x < 10) ? 10 : (x > 118) ? 118 : x;
	y = (y < 12) ? 12 : (y > 152) ? 152 : y;

	if (tr == 1 || tr == 2)
		blit_sprite2x2(VGAScreen, x, y, spriteSheet10, o->gr[0]);
	else
		blit_sprite2(VGAScreen, x, y, spriteSheet9, o->gr[0]);
}

bool JE_customWeaponCreator(bool canEquip)
{
	// If opened before any item data has loaded (e.g. from the title-screen Setup menu),
	// load it now — otherwise ships[]/weapons[]/weaponPort[] are zeroed and the preview
	// draws from garbage sprite indices (the intermittent title-screen crash).
	// JE_loadItemDat() also runs customWeaponInit().
	if (weaponPort[1].name[0] == '\0')
		JE_loadItemDat();
	else if (customWeaponPort <= 0)
		customWeaponInit();

	// The impact explosions need the explosion sprite sheet, which the game only loads
	// lazily at level start — it can be NULL here (creator opened from Setup, or the
	// between-levels shop). Load it on demand, exactly like the level loader does, or the
	// first hit would blit from a NULL sheet and crash.
	if (explosionSpriteSheet.data == NULL)
		JE_loadCompShapes(&explosionSpriteSheet, '6');

	customWeaponSelectLevel(0);   // always open editing power level 1 ...
	customWeaponSelectMode(0);    // ... fire mode 1
	customWeaponMaterialize();    // make sure all compiled slots reflect the saved design

	// Reset editor-only selection state each time the screen opens.
	cwImportSrc = cwClamp(cwImportSrc, 0, (customBulletPresetCount > 0) ? customBulletPresetCount - 1 : 0);
	cwImportPwr = customBulletMaxPower(cwImportSrc);
	cwBulletSel = 0;

	// Action buttons (context-dependent: no "Equip" without an active ship). They stay pinned
	// at the bottom of the panel, always visible whatever category is showing. The segment,
	// import, and fill-all-levels (auto-scale / copy) actions live inline next to what they act on.
	// Laid out two per line (each half clickable), in this order — so the pairs come out
	// Randomize|Save, Reset|Reset All, Equip|Done (or Done alone when there's no Equip).
	int actIds[8], actCount = 0;
	actIds[actCount++] = CWACT_RANDOMIZE;
	actIds[actCount++] = CWACT_SAVE;
	actIds[actCount++] = CWACT_RESET;
	actIds[actCount++] = CWACT_RESET_ALL;
	if (canEquip)
		actIds[actCount++] = CWACT_EQUIP;
	actIds[actCount++] = CWACT_DONE;
	const int actRowCount = (actCount + 1) / 2;   // two actions per line

	const bool prevCentered = (video_get_menu_x_offset() != 0);
	set_menu_centered(true);

	Palette savedPalette;
	memcpy(savedPalette, colors, sizeof(Palette));  // restored on exit

	SDL_Surface *temp_surface = VGAScreen;
	VGAScreen = VGAScreenSeg;

	// Backdrop: the shop's own picture + palette, so the screen looks native and never
	// shows black margins. Snapshot it to VGAScreen2 so cwRestoreRect() can wipe the
	// area around the preview box each tick.
	JE_loadPic(VGAScreen, 1, true);
	memcpy(VGAScreen2->pixels, VGAScreen->pixels, VGAScreen2->pitch * VGAScreen2->h);

	// The preview box is fixed at 8,8..143,182 so JE_weaponSimSmoothPresent() — the
	// buy/sell screen's own interpolated presenter — can be reused verbatim, giving
	// the same smooth 60fps shots as the shop.
	enum { BOX_X0 = 8, BOX_Y0 = 8, BOX_X1 = 143, BOX_Y1 = 182 };
	// panY0/panY1 sit one pixel above the interior so the panel's border lines up
	// exactly with the preview box's frame (drawn at BOX_Y0-1 .. BOX_Y1+1 = 7..183).
	const int panX0 = 150, panX1 = 313, panY0 = 7, panY1 = 183;
	const int itemsTop = panY0 + 13;
	const int row_h = 8;
	const int catY = itemsTop;                              // category selector (pinned, top)
	const int fieldsTop = catY + row_h + 3;                 // scrolling field rows start here
	const int actionsTop = panY1 - 2 - actRowCount * row_h; // action buttons (pinned, bottom; 2 per line)
	const int panMidX = (panX0 + panX1) / 2;                // split point for paired half-width buttons
	const int fieldVis = (actionsTop - 2 - fieldsTop) / row_h;  // field rows visible at once
	const int labelX = panX0 + 5, valueX = panX1 - 5;

	enum { C_PANEL = 0xF1, C_DIV = 0xF6, C_HI = 0xFB, C_LO = 0xF4, C_SEL = 0xF5 };

	const Uint8 previewShip = (player[0].items.ship != 0) ? player[0].items.ship : 1;

	JE_initWeaponView();  // reset shot arrays, place the ship at 72,110 (inside the box)
	cwResetDummies();     // place the practice targets + clear any leftover explosions

	int selected = 0, fieldScrollTop = 0;
	int prev_mx = mouse_x, prev_my = mouse_y;
	bool done = false, equipped = false;

	wait_noinput(false, false, true);
	newkey = newmouse = new_text = false;

	cwOvRow = -1;
	cwTrackedShot = -1;
	cwTrackedSeg = -1;
	cwNumRow = -1;         // not typing a number yet
	cwNumText[0] = '\0';
	cwSavedFlash = 0;
	weaponSimOverlayFn = cwDrawPreviewOverlay;  // enable the sprite/hitbox overlay while open

	while (!done)
	{
		setDelay(3);

		if (cwSavedFlash > 0)
			--cwSavedFlash;

		// Build the visible field list for the current category (All = every field row).
		int fieldRows[CWROW_FIELD_COUNT], fieldCount = 0;
		for (int r = 0; r < CWROW_FIELD_COUNT; ++r)
			if (cwCategory == CWCAT_ALL || cwRowCategory(r) == cwCategory)
				fieldRows[fieldCount++] = r;

		// Nav list: [0] = category selector, [1..fieldCount] = fields, then the action buttons.
		const int navCount = 1 + fieldCount + actCount;
		selected = cwClamp(selected, 0, navCount - 1);
		const int selField = (selected >= 1 && selected <= fieldCount) ? fieldRows[selected - 1] : -1;

		if (cwNumRow >= 0 && cwNumRow != selField)  // moved off the field being typed: apply it
			cwCommitNumeric();

		if (selField >= 0)  // keep the selected field inside the scrolling window
		{
			const int fi = selected - 1;
			if (fi < fieldScrollTop)                  fieldScrollTop = fi;
			else if (fi >= fieldScrollTop + fieldVis) fieldScrollTop = fi - fieldVis + 1;
		}
		fieldScrollTop = cwClamp(fieldScrollTop, 0, (fieldCount > fieldVis) ? fieldCount - fieldVis : 0);

		// --- record the box: dark bg, starfield, the custom weapon firing, the ship ---
		power = lastPower = 900;  // never let the preview starve for power
		rl_begin_record();
		fill_rectangle_xy(VGAScreen, BOX_X0, BOX_Y0, BOX_X1, BOX_Y1, 0);
		update_and_draw_starfield(VGAScreen, 1);
		if (cwShowTargets)
			cwDrawDummies();  // practice targets behind the shots (also regen + tick flashes)
		player[0].x = cwShipX;   // user-positioned ship = shot origin
		player[0].y = cwShipY;
		mouseX = player[0].x;
		mouseY = player[0].y;
		if (shotRepeat[0] > 0)
			--shotRepeat[0];
		else  // preview the fire mode + power level currently being edited
			player_shot_create(customWeaponPort, 0, player[0].x, player[0].y, mouseX, mouseY,
			                   CUSTOM_WEAP_BASE + customWeaponEditMode * CUSTOM_POWER_LEVELS + customWeaponEditLevel, 1);
		if (cwShowTargets)
			cwHomeShots();    // curve the shots toward a target when Homing is on
		simulate_player_shots();
		JE_drawSP();          // draw the shot superspark trail (sparky custom bullets) inside
		                      // the box; cwRestoreRect below clips any that flew past the edges
		if (cwShowTargets)
			cwCollideShots(); // impacts, chain cascade, freeze, armour depletion
		JE_drawItem(1, previewShip, player[0].x - 5, player[0].y - 7);
			cwDrawSidekickPreview();  // show the sidekick body sprite when equipped as one
		if (cwShowTargets)
			cwPumpExplosions(); // draw the impact explosions on top
		if (cwShowStats)
			cwDrawStats();      // live DPS / damage / fire-rate HUD (independent of targets)
		rl_end_record();

		// --- clip the box: restore the backdrop everywhere outside it, wiping the
		//     full-screen starfield and any shots that flew past the edges ---
		cwRestoreRect(0, 0, LEGACY_WIDTH - 1, BOX_Y0 - 1);                   // above
		cwRestoreRect(0, BOX_Y1 + 1, LEGACY_WIDTH - 1, vga_height - 1);      // below
		cwRestoreRect(0, BOX_Y0, BOX_X0 - 1, BOX_Y1);                        // left
		cwRestoreRect(BOX_X1 + 1, BOX_Y0, LEGACY_WIDTH - 1, BOX_Y1);         // right
		JE_rectangle(VGAScreen, BOX_X0 - 1, BOX_Y0 - 1, BOX_X1 + 1, BOX_Y1 + 1, C_HI);  // box frame

		// --- editor panel (over the backdrop, right of the box) ---
		fill_rectangle_xy(VGAScreen, panX0, panY0, panX1, panY1, C_PANEL);
		JE_rectangle(VGAScreen, panX0, panY0, panX1, panY1, C_HI);
		draw_font_hv_shadow(VGAScreen, (panX0 + panX1) / 2, panY0 + 2, "CUSTOM WEAPON CREATOR",
		                    small_font, centered, 15, 3, false, 1);
		fill_rectangle_xy(VGAScreen, panX0 + 2, panY0 + 11, panX1 - 2, panY0 + 11, C_DIV);

		// Category selector (pinned at top): Left/Right cycles which group of rows shows.
		{
			const bool sel = (selected == 0);
			fill_rectangle_xy(VGAScreen, panX0 + 2, catY - 1, panX1 - 2, catY + row_h - 2, sel ? C_SEL : C_PANEL);
			char cat[32];
			snprintf(cat, sizeof(cat), "< %s >", cwCatName[cwClamp(cwCategory, 0, CWCAT_COUNT - 1)]);
			draw_font_hv_shadow(VGAScreen, labelX, catY, "View", small_font, left_aligned, 15, sel ? 5 : 3, false, 1);
			draw_font_hv_shadow(VGAScreen, valueX, catY, cat, small_font, right_aligned, 15, sel ? 6 : 4, false, 1);
		}
		fill_rectangle_xy(VGAScreen, panX0 + 2, catY + row_h - 1, panX1 - 2, catY + row_h - 1, C_DIV);

		// In a focused category, tint the whole field area with its band colour so it runs
		// all the way down to the pinned action buttons, not just behind the few rows.
		if (cwCategory != CWCAT_ALL)
			fill_rectangle_xy(VGAScreen, panX0 + 2, fieldsTop - 1, panX1 - 2, actionsTop - 3, cwCatColor(cwCategory));

		// Scrolling field rows for the selected category.
		for (int vis = 0; vis < fieldVis; ++vis)
		{
			const int fi = fieldScrollTop + vis;
			if (fi >= fieldCount)
				break;
			const int row = fieldRows[fi];
			const int ry = fieldsTop + vis * row_h;
			const bool sel = (selected == fi + 1);

			// Each row wears its category's tint; the selected row overrides it.
			fill_rectangle_xy(VGAScreen, panX0 + 2, ry - 1, panX1 - 2, ry + row_h - 2,
			                  sel ? C_SEL : cwGroupColor(row));
			// Text is a LIGHT shade of the row's own category hue (bank), so it always reads
			// lighter than its dark band; the selected row uses the gold UI bank.
			const Uint8 txtHue = sel ? 15 : (Uint8)(cwGroupColor(row) >> 4);
			// A row that doesn't apply to the current Equip slot is drawn dim (unless it's the
			// one under the cursor, which stays bright so its value + hint are still readable).
			const bool dim = !sel && !cwRowActive(row);

			if (cwInlineActionId(row) >= 0)  // an inline action row (drawn as a centered button)
			{
				draw_font_hv_shadow(VGAScreen, (panX0 + panX1) / 2, ry, cwActLabel[cwInlineActionId(row)],
				                    small_font, centered, txtHue, dim ? 2 : 4, false, 1);
			}
			else
			{
				char val[40];
				const bool typing = (cwNumRow == row);
				if (typing)
					snprintf(val, sizeof(val), "%s_", cwNumText);  // show the digits typed so far + caret
				else
					cwRowValue(row, val, sizeof(val));
				const bool editing = typing || (row == CWROW_NAME && sel);
				// Long values (bullet/custom names) may need the whole row; only draw the label
				// when it won't collide with the right-aligned value.
				if (labelX + JE_textWidth(cwRowLabel(row), small_font) + 6 <= valueX - JE_textWidth(val, small_font))
					draw_font_hv_shadow(VGAScreen, labelX, ry, cwRowLabel(row), small_font, left_aligned, txtHue, dim ? 2 : 3, false, 1);
				draw_font_hv_shadow(VGAScreen, valueX, ry, val, small_font, right_aligned, txtHue, editing ? 6 : (dim ? 2 : 5), false, 1);
			}
		}

		if (fieldCount > fieldVis)  // field scrollbar (over the scrolling region only)
		{
			const int trackTop = fieldsTop - 1, trackBot = fieldsTop + fieldVis * row_h - 2;
			const int trackH = trackBot - trackTop;
			fill_rectangle_xy(VGAScreen, panX1 - 3, trackTop, panX1 - 2, trackBot, C_LO);
			int thumbH = trackH * fieldVis / fieldCount;
			if (thumbH < 4) thumbH = 4;
			const int denom = fieldCount - fieldVis;
			const int thumbY = trackTop + (denom > 0 ? (trackH - thumbH) * fieldScrollTop / denom : 0);
			fill_rectangle_xy(VGAScreen, panX1 - 3, thumbY, panX1 - 2, thumbY + thumbH, C_HI);
		}

		// Action buttons (pinned at the bottom, always visible whatever the category), two per
		// line: action a sits at row a/2, left half (col 0) or right half (col 1). An action
		// left without a partner (odd count) spans the full width.
		fill_rectangle_xy(VGAScreen, panX0 + 2, actionsTop - 2, panX1 - 2, actionsTop - 2, C_DIV);
		for (int a = 0; a < actCount; ++a)
		{
			const int col = a % 2;
			const bool alone = (col == 0 && a + 1 >= actCount);   // even action with no right partner
			const int bx0 = alone ? panX0 + 2 : (col == 0 ? panX0 + 2 : panMidX + 1);
			const int bx1 = alone ? panX1 - 2 : (col == 0 ? panMidX - 1 : panX1 - 2);
			const int ry = actionsTop + (a / 2) * row_h;
			const bool sel = (selected == fieldCount + 1 + a);
			fill_rectangle_xy(VGAScreen, bx0, ry - 1, bx1, ry + row_h - 2, sel ? C_SEL : CW_BG_ACTION);
			draw_font_hv_shadow(VGAScreen, (bx0 + bx1) / 2, ry, cwActLabel[actIds[a]],
			                    small_font, centered, sel ? 15 : (Uint8)(CW_BG_ACTION >> 4), 4, false, 1);
		}

		{
			// Selected-item help on the bottom line.
			static char hintBuf[96];
			const char *hint;
			int lo, hi;
			if (cwSavedFlash > 0)            hint = "Weapon saved!";
			else if (cwNumRow >= 0)          hint = "Type a value   -   Enter: set   -   Esc: cancel";
			else if (selected == 0)          hint = "Left/Right: pick category   -   Esc: back";
			else if (selField == CWROW_NAME) hint = "Type to rename   -   Esc: back";
			else if (selField >= 0 && !cwRowActive(selField)) hint = cwRowInactiveReason(selField);
			else if (selField >= 0 && cwNumericRange(selField, &lo, &hi))
			{
				// Numeric field: hint that you can type an exact value, if the line still fits.
				snprintf(hintBuf, sizeof(hintBuf), "%s   -   or type a number", cwRowHelp(selField));
				hint = (JE_textWidth(hintBuf, small_font) <= LEGACY_WIDTH - 16) ? hintBuf : cwRowHelp(selField);
			}
			else if (selField >= 0)          hint = cwRowHelp(selField);
			else                             hint = "Enter: use   -   Esc: back";
			draw_font_hv_shadow(VGAScreen, LEGACY_WIDTH / 2, vga_height - 12, hint, small_font, centered, 15, 2, false, 1);
		}

		rl_finalize();
		rl_capture_residual(VGAScreenSeg, game_screen);

		push_joysticks_as_keyboard();
		service_SDL_events(false);

		mouseCursor = MOUSE_POINTER_NORMAL;
		cwOvRow = selField;           // overlay keys off the selected field row (-1 = none)
		cwUpdateTrackedShot(selField);  // lock the hitbox onto one live shot (after they moved)
		JE_weaponSimSmoothPresent();  // smooth box interpolation + cursor + present (Smooth Motion on)
		menuWaitWithSmoothCursor();   // waits out the tick if Smooth Motion is off

		// --- input gathered over the tick: wheel scrolls, motion hovers, click acts ---
		if (mouse_scroll != 0)
		{
			selected = cwClamp(selected - mouse_scroll, 0, navCount - 1);
			mouse_scroll = 0;
		}

		// Map the mouse to a nav item across the three panel zones (category / fields / actions).
		int hoverNav = -1;
		if (mouse_x >= panX0 && mouse_x <= panX1)
		{
			if (mouse_y >= catY && mouse_y < catY + row_h)
				hoverNav = 0;
			else if (mouse_y >= fieldsTop && mouse_y < fieldsTop + fieldVis * row_h)
			{
				const int fi = fieldScrollTop + (mouse_y - fieldsTop) / row_h;
				if (fi < fieldCount) hoverNav = 1 + fi;
			}
			else if (mouse_y >= actionsTop && mouse_y < actionsTop + actRowCount * row_h)
			{
				// two actions per line: pick the row, then the left/right half by mouse x
				int a = ((mouse_y - actionsTop) / row_h) * 2;
				if (a + 1 < actCount && mouse_x >= panMidX)   // row has a right half and we're on it
					a += 1;
				if (a < actCount)
					hoverNav = fieldCount + 1 + a;
			}
		}
		if (hoverNav >= 0 && (mouse_x != prev_mx || mouse_y != prev_my))
			selected = hoverNav;
		prev_mx = mouse_x;
		prev_my = mouse_y;

		if (newmouse)
		{
			if (cwNumRow >= 0)  // a click anywhere finalizes an in-progress typed value first
				cwCommitNumeric();
			if (lastmouse_but == SDL_BUTTON_RIGHT)
			{
				done = true;
			}
			else if (hoverNav >= 0)
			{
				selected = hoverNav;
				if (hoverNav == 0)  // category selector: cycle forward
				{
					cwCategory = (cwCategory + 1) % CWCAT_COUNT;
					fieldScrollTop = 0;
					JE_playSampleNum(S_CURSOR);
				}
				else if (hoverNav <= fieldCount)  // a field row: click adjusts / triggers inline action
				{
					const int f = fieldRows[hoverNav - 1];
					if (!cwRowActive(f))
						JE_playSampleNum(S_SPRING);  // doesn't apply to the current Equip slot
					else if (cwInlineActionId(f) >= 0)
						cwPerformAction(cwInlineActionId(f), &done, &equipped);
					else
						{ cwRowAdjust(f, +1); JE_playSampleNum(S_CURSOR); }
				}
				else  // an action button
				{
					cwPerformAction(actIds[hoverNav - fieldCount - 1], &done, &equipped);
				}
			}
			newmouse = false;
		}

		if (selField == CWROW_NAME && new_text)  // inline name editing
		{
			size_t len = strlen(customWeaponName);
			for (size_t ti = 0; last_text[ti] != '\0'; ++ti)
			{
				const unsigned char c = (unsigned char)last_text[ti];
				if (c >= 32 && c < 127 && len < sizeof(customWeaponName) - 1)
					customWeaponName[len++] = (char)c;
			}
			customWeaponName[len] = '\0';
			customWeaponMaterialize();
		}
		else if (new_text && selField >= 0 && cwRowActive(selField))  // direct numeric entry into a value field
		{
			int lo, hi;
			if (cwNumericRange(selField, &lo, &hi))
			{
				for (size_t ti = 0; last_text[ti] != '\0'; ++ti)
				{
					const char c = last_text[ti];
					const bool digit = (c >= '0' && c <= '9');
					const bool minus = (c == '-' && lo < 0);
					if (!digit && !minus)
						continue;
					if (cwNumRow != selField) { cwNumRow = selField; cwNumText[0] = '\0'; }  // begin typing
					size_t len = strlen(cwNumText);
					if (minus)
					{
						if (len == 0) { cwNumText[0] = '-'; cwNumText[1] = '\0'; }  // sign only as the first char
					}
					else if (len < sizeof(cwNumText) - 1)
					{
						cwNumText[len] = c;
						cwNumText[len + 1] = '\0';
					}
				}
			}
		}
		new_text = false;

		if (newkey)
		{
			switch (lastkey_scan)
			{
			case SDL_SCANCODE_UP:    if (cwNumRow >= 0) cwCommitNumeric(); selected = (selected == 0) ? navCount - 1 : selected - 1; break;
			case SDL_SCANCODE_DOWN:  if (cwNumRow >= 0) cwCommitNumeric(); selected = (selected + 1) % navCount; break;
			case SDL_SCANCODE_LEFT:
				if (cwNumRow >= 0)             { cwCommitNumeric(); JE_playSampleNum(S_SELECT); break; }  // arrows finish typing
				if (selected == 0)             { cwCategory = (cwCategory + CWCAT_COUNT - 1) % CWCAT_COUNT; fieldScrollTop = 0; JE_playSampleNum(S_CURSOR); }
				else if (selField < 0)         break;
				else if (!cwRowActive(selField)) JE_playSampleNum(S_SPRING);  // greyed: doesn't apply here
				else                           { cwRowAdjust(selField, -1); JE_playSampleNum(S_CURSOR); }
				break;
			case SDL_SCANCODE_RIGHT:
				if (cwNumRow >= 0)             { cwCommitNumeric(); JE_playSampleNum(S_SELECT); break; }  // arrows finish typing
				if (selected == 0)             { cwCategory = (cwCategory + 1) % CWCAT_COUNT; fieldScrollTop = 0; JE_playSampleNum(S_CURSOR); }
				else if (selField < 0)         break;
				else if (!cwRowActive(selField)) JE_playSampleNum(S_SPRING);  // greyed: doesn't apply here
				else                           { cwRowAdjust(selField, +1); JE_playSampleNum(S_CURSOR); }
				break;
			case SDL_SCANCODE_BACKSPACE:
				if (cwNumRow >= 0)  // editing a number: erase the last typed digit
				{
					size_t len = strlen(cwNumText);
					if (len > 0)
						cwNumText[len - 1] = '\0';
					if (cwNumText[0] == '\0')
						cwNumRow = -1;  // buffer empty: leave typing mode, value unchanged
				}
				else if (selField == CWROW_NAME)
				{
					size_t len = strlen(customWeaponName);
					if (len > 0)
						customWeaponName[len - 1] = '\0';
					customWeaponMaterialize();
				}
				break;
			case SDL_SCANCODE_RETURN:
			case SDL_SCANCODE_SPACE:
				if (cwNumRow >= 0)  // finish direct numeric entry
					{ cwCommitNumeric(); JE_playSampleNum(S_SELECT); }
				else if (selected == 0)  // category selector: Enter also cycles forward
					{ cwCategory = (cwCategory + 1) % CWCAT_COUNT; fieldScrollTop = 0; JE_playSampleNum(S_CURSOR); }
				else if (selField < 0)  // an action button
					cwPerformAction(actIds[selected - fieldCount - 1], &done, &equipped);
				else if (!cwRowActive(selField))
					JE_playSampleNum(S_SPRING);  // greyed: doesn't apply to the current Equip slot
				else if (cwInlineActionId(selField) >= 0)
					cwPerformAction(cwInlineActionId(selField), &done, &equipped);
				else if (selField == CWROW_LIB_SELECT || selField == CWROW_POWER_LEVEL ||
				         selField == CWROW_TWO_MODES ||
				         selField == CWROW_FIRE_MODE || selField == CWROW_SHOW_TARGETS ||
				         selField == CWROW_SHOW_STATS || selField == CWROW_EQUIP_SLOT ||
				         selField == CWROW_IMPORT_SRC || selField == CWROW_IMPORT_PWR ||
				         selField == CWROW_BULLET_SEL ||
				         selField == CWROW_SK_MOUNT || selField == CWROW_SK_ANIMATE)
				{
					cwRowAdjust(selField, +1);  // Enter cycles the enum-style / selector fields
					JE_playSampleNum(S_CURSOR);
				}
				break;
			case SDL_SCANCODE_ESCAPE:
				if (cwNumRow >= 0)  // cancel an in-progress typed value, stay in the editor
					{ cwNumRow = -1; cwNumText[0] = '\0'; JE_playSampleNum(S_CURSOR); }
				else
					done = true;
				break;
			default: break;
			}
			newkey = false;
		}
	}

	weaponSimOverlayFn = NULL;  // stop overlaying (JE_weaponSimSmoothPresent is shared with the shop)
	for (int j = 0; j < MAX_EXPLOSIONS; ++j)  // don't leak editor impacts back to the shop/game
		explosions[j].ttl = 0;
	wait_noinput(false, false, true);

	VGAScreen = temp_surface;
	set_menu_centered(prevCentered);
	memcpy(colors, savedPalette, sizeof(Palette));  // restore the caller's palette
	set_palette(colors, 0, 255);

	customWeaponLibrarySave();  // persist the whole weapon library to its own file
	save_opentyrian_config();  // persist the active design to disk immediately, not just at game exit
	return equipped;
}

void JE_menuFunction(JE_byte select)
{
	JE_byte x;
	JE_word curSelect;

	col = 0;
	colC = -1;
	JE_playSampleNum(S_CLICK);

	curSelect = curSel[curMenu];

	switch (curMenu)
	{
	case MENU_FULL_GAME:
		switch (select)
		{
		case 2: //cubes -> endless: open the E-Shop submenu
			if (endlessMode)
			{
				if (endlessLockedSortie)  // locked "gave up the level" outpost: the E-Shop is off-limits
					break;
				curMenu = MENU_ESHOP;
				curSel[MENU_ESHOP] = 2;
				configure_endless_shop_menu();  // refresh the E-Shop prices on entry
			}
			else
			{
				curMenu = MENU_DATA_CUBES;
				curSel[MENU_DATA_CUBES] = 2;
			}
			break;
		case 3: // endless: read-only Perks list; campaign: stock Ship Specs
			if (endlessMode)
			{
				endlessPerkListMode = true;
				configure_endless_perk_list_menu();
				curSel[MENU_PERKS] = 2;
				curMenu = MENU_PERKS;
			}
			else
				JE_doShipSpecs();
			break;
		case 4://upgradeship
			if (endlessMode && endlessLockedSortie)  // locked: the loadout is frozen to the launch-time choices
				break;
			curMenu = MENU_UPGRADES;
			break;
		case 5: //options
			curMenu = MENU_OPTIONS;
			break;
		case 6: //nextlevel
			if (endlessMode && endlessLockedSortie)
			{
				// Locked "gave up the level" outpost: relaunch the same committed level directly (no
				// course choice, no re-run of endlessSelectCourse). Arms jumpSection -> the shop loop
				// exits and JE_loadMap loads the level.
				endlessArmLockedRelaunch();
				break;
			}
			curMenu = MENU_PLAY_NEXT_LEVEL;
			newPal = 18;
			if (endlessMode)
			{
				// Endless: the "next level" list IS the course choice -- each entry a shipped
				// level plus its mutators, pre-generated for this shop visit. Map each to a
				// distinct planet for the monitor; picking one applies its mutators + launches.
				const int n = endlessCourseCount();
				mapPNum = (JE_byte)n;
				for (x = 0; x < n; x++)
				{
					mapPlanet[x] = endlessCoursePlanet(x);
					mapSection[x] = endlessCourseSection(x);
				}
				JE_computeDots();
				navX = planetX[mapOrigin - 1];
				navY = planetY[mapOrigin - 1];
				newNavX = navX;
				newNavY = navY;
				menuChoices[MENU_PLAY_NEXT_LEVEL] = n + 2;
				curSel[MENU_PLAY_NEXT_LEVEL] = 2;
				strcpy(menuInt[4][0], "Chart a Course");
				for (x = 0; x < n; x++)
					SDL_strlcpy(menuInt[4][x + 1], endlessCourseName(x), sizeof(menuInt[4][x + 1]));
				strcpy(menuInt[4][x + 1], miscText[5]);
			}
			else
			{
				JE_computeDots();
				navX = planetX[mapOrigin - 1];
				navY = planetY[mapOrigin - 1];
				newNavX = navX;
				newNavY = navY;
				menuChoices[MENU_PLAY_NEXT_LEVEL] = mapPNum + 2;
				curSel[MENU_PLAY_NEXT_LEVEL] = 2;
				strcpy(menuInt[4][0], "Start Level");
				for (x = 0; x < mapPNum; x++)
				{
					temp = mapPlanet[x];
					strcpy(menuInt[4][x + 1], pName[temp - 1]);
				}
				strcpy(menuInt[4][x + 1], miscText[5]);
			}
			break;
		case 7:
			if (debugMode)
			{
				// Debug Menu (equipment): only present when Debug Mode is on.
				JE_debugMenu(true);
				ensure_equipped_items_visible();
				old_items[0] = player[0].items;
				player[0].last_items = player[0].items;
			}
			else
			{
				// With Debug Mode off, item 7 is the stock "Quit Game".
				if (JE_quitRequest())
				{
					gameLoaded = true;
					mainLevel = 0;
				}
			}
			break;
		case 8: // debug play level (only reachable when Debug Mode is on)
			// Scrollable picker; on selection it arms the jump (jumpSection),
			// so the buy/sell loop exits and the chosen level loads.
			JE_debugLevelSelect();
			break;
		case 9: //quit (only reachable when Debug Mode is on)
			if (JE_quitRequest())
			{
				gameLoaded = true;
				mainLevel = 0;
			}
			break;
		}
		break;

	case MENU_PERKS:  // endless perk pick (forced 1-of-3 + decline); one-shot gate before the shop
		if (endlessPerkListMode)  // read-only perk list reached from the buy/sell menu -- any pick just returns
		{
			endlessPerkListMode = false;
			curMenu = MENU_FULL_GAME;
			break;
		}
		if (select == menuChoices[MENU_PERKS])  // "Take the Cash" (decline)
			endlessDeclinePerk();
		else
			endlessTakePerk(select - 2);        // offers are rows 2.., choice index = select - 2
		endlessPerkPending = false;             // consumed -- can't return this visit
		JE_playSampleNum(S_SELECT);
		curMenu = MENU_FULL_GAME;
		break;

	case MENU_ESHOP:  // endless E-Shop: reroll / reinforce / kill-fire buff / random special
		if (select == menuChoices[MENU_ESHOP]) //done
		{
			curMenu = MENU_FULL_GAME;
		}
		else if (select == 5)  // Extra Perk: charge, then open the perk PICK menu to choose one
		{
			if (endlessTryBuyExtraPerk())
			{
				JE_playSampleNum(S_SELECT);
				endlessPerkListMode = false;   // a real forced PICK, not the read-only list
				configure_endless_perk_menu();
				curSel[MENU_PERKS] = 2;
				curMenu = MENU_PERKS;
			}
			else
				JE_playSampleNum(S_SPRING);
			configure_endless_shop_menu();
		}
		else
		{
			bool bought = false;
			switch (select)  // Extra Perk (5) handled above; order matches configure_endless_shop_menu
			{
			case 2: bought = endlessTryReroll();          break;  // reroll the shop stock
			case 3: bought = endlessTryBuyCleanse();      break;  // sabotage: strip the next sector's worst mod
			case 4: bought = endlessTryReinforce();       break;  // +max armor (run-persistent)
			case 6: bought = endlessTryBuySpecial();      break;  // random special weapon
			case 7: bought = endlessTryBuyTurbodrive();  break;  // Turbodrive (kill-fire boost)
			case 8: bought = endlessTryBuyOverblast();    break;  // Overblast (damage-only stacks)
			case 9: bought = endlessTryBuyOverdrive();   break;  // Overdrive (+ escalating fire+damage stacks)
			case 10: bought = endlessTryBuyRevive();      break;  // one-shot revive token
			case 11: bought = endlessTryBuyBomb();         break;  // +1 superbomb (cap 10)
			case 12: bought = endlessTryGamble();         break;  // random good/bad outcome (pinned last)
			}
			JE_playSampleNum(bought ? S_SELECT : S_SPRING);
			if (bought && select == 2)  // a reroll regenerated the stock: re-sort so None sinks to the bottom
				sort_shop_inventory();
			configure_endless_shop_menu();  // refresh prices / bought-state labels
			// A free perk pick can only come from the Gamble (select 12) -- no other buy grants one.
			// Gate the perk-menu open on select==12 so a stale/leaked "won a perk" flag can never make
			// an ordinary buy (Bomb/Special/Turbo/...) spuriously pop the perk pick. Then clear the flag
			// UNCONDITIONALLY below: it's a one-dispatch signal that must never survive to a later buy.
			if (bought && select == 12 && endlessGambleWonPerk())  // gamble handed us a free perk: open the pick menu now
			{
				endlessPerkListMode = false;   // a real forced PICK, not the read-only list
				configure_endless_perk_menu();
				curSel[MENU_PERKS] = 2;
				curMenu = MENU_PERKS;
			}
			endlessClearGamblePerk();  // one-shot: never let "won a perk" persist past the buy that set it
		}
		break;

	case MENU_UPGRADES:
		if (select == menuChoices[MENU_UPGRADES]) //done (item 9, or 10 when Custom is present)
		{
			curMenu = MENU_FULL_GAME;
		}
		else if (customWeaponEnabled && select == 9) // Custom: open the creator, equip from there
		{
			if (JE_customWeaponCreator(true))
			{
				// Equipped: reflect the new loadout so the shop keeps it and the custom
				// weapon shows in its Front/Rear weapon list.
				ensure_equipped_items_visible();
				old_items[0] = player[0].items;
				player[0].last_items = player[0].items;
			}
		}
		else // selected item to upgrade
		{
			old_items[0] = player[0].items;

			weaponSimTime = 0;
			lastDirection = 1;
			JE_genItemMenu(select);
			JE_initWeaponView();
			upgradeSubScrollTop = 1;  // start the (possibly scrolling) sub-list at the top
			upgradeSubPrevSel = 0;    // force the view to snap to the selection on entry
			curMenu = MENU_UPGRADE_SUB;
			lastCurSel = curSel[MENU_UPGRADE_SUB];
			player[0].cash = player[0].cash * 2 - JE_cashLeft();
		}
		break;

	case MENU_OPTIONS:
		switch (select)
		{
		case 2:  // Load Game
		case 3:  // Save Game
			// Endless hardcore forbids ALL saving/loading (these rows are greyed out in
			// JE_drawMenuChoices); a deny beep confirms the press did nothing.
			if (endlessMode && endlessHardcore)
			{
				JE_playSampleNum(S_SPRING);
				break;
			}
			curMenu = MENU_LOAD_SAVE;
			performSave = (select == 3);  // item 2 = Load, item 3 = Save
			quikSave = false;
			break;
#if defined(__SWITCH__) || defined(__vita__)
		// Item 6 is Touch Sensitivity (a bar, adjusted via left/right); the config rows below
		// it and Exit each shift down by one.
		case 7:
			curMenu = MENU_JOYSTICK_CONFIG;
			break;
		case 8:
			curMenu = MENU_KEYBOARD_CONFIG;
			break;
		case 9:
			curMenu = MENU_MOUSE_CONFIG;
			break;
		case 10:
			curMenu = MENU_FULL_GAME;
			break;
#else
		case 6:
			curMenu = MENU_JOYSTICK_CONFIG;
			break;
		case 7:
			curMenu = MENU_KEYBOARD_CONFIG;
			break;
		case 8:
			curMenu = MENU_MOUSE_CONFIG;
			break;
		case 9:
			curMenu = MENU_FULL_GAME;
			break;
#endif
		}
		break;

	case MENU_PLAY_NEXT_LEVEL:
		if (select == menuChoices[MENU_PLAY_NEXT_LEVEL]) //exit
		{
			curMenu = MENU_FULL_GAME;
			newPal = 1;
			debugPlayMenu = false;
		}
		else if (endlessMode)
		{
			// Endless: the chosen entry is a course -- apply its mutators + episode, then launch.
			select_level(endlessSelectCourse(curSelect - 2), 0);
		}
		else
		{
			if (debugPlayMenu)
			{
				select_debug_level_capture();
				select_level(debugMapSection[curSelect - 2],
					debugLvlFileNum[curSelect - 2]);
				debugPlayMenu = false;
			}
			else
			{
				select_level(mapSection[curSelect - 2], 0);
			}
		}
		break;

	case MENU_DEBUG_PLAY_LEVEL:
		if (select == menuChoices[MENU_DEBUG_PLAY_LEVEL])
		{
			curMenu = MENU_FULL_GAME;
			newPal = 1;
			debugPlayMenu = false;
		}
		// Guard the index: the 2-column grid can put the cursor on an empty cell past the
		// last level, and select_level() with a garbage section/file crashes. Out of range
		// -> ignore (safe no-op) rather than load junk.
		else if (curSelect >= 2 && (uint)(curSelect - 2) < debugLevelCount)
		{
			select_debug_level_capture();
			select_level(debugMapSection[curSelect - 2],
				debugLvlFileNum[curSelect - 2]);
			debugPlayMenu = false;
		}
		break;

	case MENU_UPGRADE_SUB:
		if (curSel[MENU_UPGRADE_SUB] < menuChoices[MENU_UPGRADE_SUB])
		{
			// select done
			curSel[MENU_UPGRADE_SUB] = menuChoices[MENU_UPGRADE_SUB];
		}
		else // if done is selected
		{
			JE_playSampleNum(S_ITEM);

			player[0].cash = JE_cashLeft();
			curMenu = MENU_UPGRADES;
		}
		break;

	case MENU_KEYBOARD_CONFIG:
		if (curSelect == 10) /* reset to defaults */
		{
			memcpy(keySettings, defaultKeySettings, sizeof(keySettings));
		}
		else if (curSelect == 11) /* done */
		{
			curMenu = isNetworkGame
				? MENU_LIMITED_OPTIONS
				: MENU_OPTIONS;
		}
		else /* change key */
		{
			temp2 = 254;
			int tempY = 38 + (curSelect - 2) * 12;
			JE_textShade(VGAScreen, 236, tempY, SDL_GetScancodeName(keySettings[curSelect-2]), (temp2 / 16), (temp2 % 16) - 8, DARKEN);
			JE_showVGA();

			wait_noinput(true, true, true);

			col = 248;
			colC = 1;

			do
			{
				setDelay(1);

				col += colC;
				if (col < 243 || col > 248)
				{
					colC *= -1;
				}
				JE_rectangle(VGAScreen, 230, tempY - 2, 300, tempY + 7, col);

				poll_joysticks();
				service_SDL_events(true);

				JE_showVGA();

				wait_delay();
			} while (!newkey && !mousedown && !joydown);
			
			if (newkey)
			{
				// already used? then swap
				for (uint i = 0; i < COUNTOF(keySettings); ++i)
				{
					if (keySettings[i] == lastkey_scan)
					{
						keySettings[i] = keySettings[curSelect-2];
						break;
					}
				}
				
				if (lastkey_scan != SDL_SCANCODE_ESCAPE && // reserved for menu
				    lastkey_scan != SDL_SCANCODE_F11 &&    // reserved for gamma
				    lastkey_scan != SDL_SCANCODE_P)        // reserved for pause
				{
					JE_playSampleNum(S_CLICK);
					keySettings[curSelect-2] = lastkey_scan;
					++curSelect;
				}
				
				JE_wipeKey();
			}
		}
		break;

	case MENU_LOAD_SAVE:
		if (curSelect == 13)
		{
			if (quikSave)
			{
				curMenu = oldMenu;
				newPal = oldPal;
			}
			else
			{
				curMenu = MENU_OPTIONS;
			}
		}
		else
		{
			if (twoPlayerMode)
				temp = 11;
			else
				temp = 0;
			JE_operation(curSelect - 1 + temp);
			if (quikSave)
			{
				curMenu = oldMenu;
				newPal = oldPal;
			}
		}
		break;

	case MENU_DATA_CUBES:
		if (curSelect == menuChoices[curMenu])
		{
			curMenu = MENU_FULL_GAME;
			newPal = 1;
		}
		else
		{
			if (cubeMax > 0)
			{
				firstMenu9 = true;
				curMenu = MENU_DATA_CUBE_SUB;
				yLoc = 0;
				yChg = 0;
				currentCube = curSel[MENU_DATA_CUBES] - 2;
			}
			else
			{
				curMenu = MENU_FULL_GAME;
				newPal = 1;
			}
		}
		break;

	case MENU_DATA_CUBE_SUB:
		curMenu = MENU_DATA_CUBES;
		break;

	case MENU_2_PLAYER_ARCADE:
		switch (curSel[curMenu])
		{
		case 2:
			mainLevel = mapSection[mapPNum-1];
			jumpSection = true;
			break;
		case 3:
		case 4:
			JE_playSampleNum(S_CURSOR);

			int temp = curSel[curMenu] - 3;
			do
			{
				if (joysticks == 0)
					inputDevice[temp == 0 ? 1 : 0] = inputDevice[temp]; // swap controllers
				if (inputDevice[temp] >= 2 + joysticks)
					inputDevice[temp] = 1;
				else
					inputDevice[temp]++;
			} while (inputDevice[temp] == inputDevice[temp == 0 ? 1 : 0]);
			break;
		case 5:
			curMenu = MENU_OPTIONS;
			break;
		case 6:
			if (debugMode)
			{
				// Debug Menu (equipment); only present when Debug Mode is on.
				JE_debugMenu(true);
				ensure_equipped_items_visible();
				old_items[0] = player[0].items;
				player[0].last_items = player[0].items;
			}
			else if (JE_quitRequest())
			{
				// With Debug Mode off, item 6 is the stock "Quit Game".
				gameLoaded = true;
				mainLevel = 0;
			}
			break;
		case 7: // debug play level (only reachable when Debug Mode is on)
			JE_debugLevelSelect();
			break;
		case 8: // quit (only reachable when Debug Mode is on)
			if (JE_quitRequest())
			{
				gameLoaded = true;
				mainLevel = 0;
			}
			break;
		}
		break;

	case MENU_1_PLAYER_ARCADE:
		switch (curSel[curMenu])
		{
		case 2:
			mainLevel = mapSection[mapPNum-1];
			jumpSection = true;
			break;
		case 3:
			curMenu = isNetworkGame
				? MENU_LIMITED_OPTIONS
				: MENU_OPTIONS;
			break;
		case 4:
			if (debugMode)
			{
				// Debug Menu (equipment); only present when Debug Mode is on.
				JE_debugMenu(true);
				ensure_equipped_items_visible();
				old_items[0] = player[0].items;
				player[0].last_items = player[0].items;
			}
			else if (JE_quitRequest())
			{
				// With Debug Mode off, item 4 is the stock "Quit Game".
				gameLoaded = true;
				mainLevel = 0;
			}
			break;
		case 5: // debug play level (only reachable when Debug Mode is on)
			JE_debugLevelSelect();
			break;
		case 6: // quit (only reachable when Debug Mode is on)
			if (JE_quitRequest())
			{
				gameLoaded = true;
				mainLevel = 0;
			}
			break;
		}
		break;

	case MENU_LIMITED_OPTIONS:
		switch (select)
		{
		case 2:
			curMenu = MENU_JOYSTICK_CONFIG;
			break;
		case 3:
			curMenu = MENU_KEYBOARD_CONFIG;
			break;
#if defined(__SWITCH__) || defined(__vita__)
		// Item 6 is Touch Sensitivity (a bar); Exit shifts down from item 6 to item 7.
		case 7:
			curMenu = MENU_1_PLAYER_ARCADE;
			break;
#else
		case 6:
			curMenu = MENU_1_PLAYER_ARCADE;
			break;
#endif
		}
		break;

	case MENU_JOYSTICK_CONFIG:
		if (joysticks == 0 && select != 17)
			break;

		switch (select)
		{
		case 2:
			joystick_config++;
			joystick_config %= joysticks;
			break;
		case 3:
			joystick[joystick_config].analog = !joystick[joystick_config].analog;
			break;
		case 4:
			if (joystick[joystick_config].analog)
			{
				joystick[joystick_config].sensitivity++;
				joystick[joystick_config].sensitivity %= 11;
			}
			break;
		case 5:
			if (joystick[joystick_config].analog)
			{
				joystick[joystick_config].threshold++;
				joystick[joystick_config].threshold %= 11;
			}
			break;
		case 16:
			reset_joystick_assignments(joystick_config);
			save_joystick_config_now();
			break;
		case 17:
			save_joystick_config_now();  // also persists analog/sensitivity/threshold on the way out
			curMenu = isNetworkGame
				? MENU_LIMITED_OPTIONS
				: MENU_OPTIONS;
			break;
		default:
			if (joysticks == 0)
				break;

			// int temp = 254;
			// JE_textShade(VGAScreen, 236, 38 + i * 8, value, temp / 16, temp % 16 - 8, DARKEN);

			JE_rectangle(VGAScreen, 235, 21 + select * 8, 310, 30 + select * 8, 248);

			Joystick_assignment temp;
			if (detect_joystick_assignment(joystick_config, &temp))
			{
				// if the detected assignment was already set, unset it
				for (uint i = 0; i < COUNTOF(*joystick->assignment); i++)
				{
					if (joystick_assignment_cmp(&temp, &joystick[joystick_config].assignment[select - 6][i]))
					{
						joystick[joystick_config].assignment[select - 6][i].type = NONE;
						goto joystick_assign_done;
					}
				}

				// if there is an empty assignment, set it
				for (uint i = 0; i < COUNTOF(*joystick->assignment); i++)
				{
					if (joystick[joystick_config].assignment[select - 6][i].type == NONE)
					{
						joystick[joystick_config].assignment[select - 6][i] = temp;
						goto joystick_assign_done;
					}
				}

				// if no assignments are empty, shift them all forward and set the last one
				for (uint i = 0; i < COUNTOF(*joystick->assignment); i++)
				{
					if (i == COUNTOF(*joystick->assignment) - 1)
						joystick[joystick_config].assignment[select - 6][i] = temp;
					else
						joystick[joystick_config].assignment[select - 6][i] = joystick[joystick_config].assignment[select - 6][i + 1];
				}

joystick_assign_done:
				curSelect++;

				poll_joysticks();
				save_joystick_config_now();  // persist the new binding to disk immediately
			}
		}
		break;

	case MENU_SUPER_TYRIAN:
		switch (curSel[curMenu])
		{
		case 2:
			mainLevel = mapSection[mapPNum-1];
			jumpSection = true;
			break;
		case 3:
			JE_doShipSpecs();
			break;
		case 4:
			curMenu = MENU_OPTIONS;
			break;
		case 5:
			if (debugMode)
			{
				// Debug Menu (equipment); only present when Debug Mode is on.
				JE_debugMenu(true);
				ensure_equipped_items_visible();
				old_items[0] = player[0].items;
				player[0].last_items = player[0].items;
			}
			else if (JE_quitRequest())
			{
				// With Debug Mode off, item 5 is the stock "Quit Game".
				if (isNetworkGame)
				{
					JE_tyrianHalt(0);
				}
				gameLoaded = true;
				mainLevel = 0;
			}
			break;
		case 6: // debug play level (only reachable when Debug Mode is on)
			JE_debugLevelSelect();
			break;
		case 7: // quit (only reachable when Debug Mode is on)
			if (JE_quitRequest())
			{
				if (isNetworkGame)
				{
					JE_tyrianHalt(0);
				}
				gameLoaded = true;
				mainLevel = 0;
			}
			break;
		}
		break;

	case MENU_MOUSE_CONFIG:
		switch(curSel[curMenu])
		{
			case 2:
			case 3:
			case 4:
				temp = curSel[curMenu] - 2;
				if (++mouseSettings[temp] > 5)
					mouseSettings[temp] = 1;
				break;
			case 5:
				memcpy(mouseSettings, defaultMouseSettings, sizeof(mouseSettings));
				break;
			case 6:
				curMenu = MENU_OPTIONS;
				break;
		}
	}

	old_items[0] = player[0].items;
}

void JE_drawShipSpecs(SDL_Surface * screen, SDL_Surface * temp_screen)
{
	/* In this function we create our ship description image.
	 *
	 * We use a temp screen for convenience.  Bad design maybe (Jason!),
	 * but it'll be okay (and the alternative is malloc/a large stack) */

	int temp_x = 0, temp_y = 0, temp_index;
	Uint8 *src, *dst;

	//first, draw the text and other assorted flavoring.
	JE_clr256(screen);
	JE_drawLines(screen, true);
	JE_drawLines(screen, false);
	JE_rectangle(screen, 0, 0, 319, 199, 37);
	JE_rectangle(screen, 1, 1, 318, 198, 35);

	verticalHeight = 9;
	JE_outText(screen, 10, 2, ships[player[0].items.ship].name, 12, 3);
	JE_helpBox(screen, 100, 20, shipInfo[player[0].items.ship-1][0], 40);
	JE_helpBox(screen, 100, 100, shipInfo[player[0].items.ship-1][1], 40);
	verticalHeight = 7;

	JE_outText(screen, JE_fontCenter(miscText[4], TINY_FONT), 190, miscText[4], 12, 2);

	//now draw the green ship over that.
	//This hardcoded stuff is for positioning our little ship graphic
	if (player[0].items.ship > 90)
	{
		temp_index = 32;
	}
	else if (player[0].items.ship > 0)
	{
		temp_index = ships[player[0].items.ship].bigshipgraphic;
	}
	else
	{
		temp_index = ships[old_items[0].ship].bigshipgraphic;
	}

	switch (temp_index)
	{
		case 32:
			temp_x = 35;
			temp_y = 33;
			break;
		case 28:
			temp_x = 31;
			temp_y = 36;
			break;
		case 33:
			temp_x = 31;
			temp_y = 35;
			break;
		case 45:
			temp_x = 36;
			temp_y = 33;
			break;
		case 46:
			temp_x = 30;
			temp_y = 30;
			break;
		default:
			assert(0);
	}
	temp_x -= 30;

	//draw the ship into our temp buffer.
	JE_clr256(temp_screen);
	blit_sprite(temp_screen, temp_x, temp_y, OPTION_SHAPES, temp_index - 1);  // ship illustration

	/* But wait!  Our ship is fully colored, not green!
	 * With a little work we could get the sprite dimensions and greenify
	 * the area it resides in.  For now, let's just greenify the (almost
	 * entirely) black screen.

	 * We can't work in place.  In fact we'll need to overlay the result
	 * To avoid our temp screen dependence this has been rewritten to
	 * only write one line at a time.*/
	dst = screen->pixels;
	src = temp_screen->pixels;
	for (int y = 0; y < screen->h; y++)
	{
		for (int x = 0; x < screen->pitch; x++)
		{
			int avg = 0;
			if (y > 0)
				avg += *(src - screen->pitch) & 0x0f;
			if (y < screen->h - 1)
				avg += *(src + screen->pitch) & 0x0f;
			if (x > 0)
				avg += *(src - 1) & 0x0f;
			if (x < screen->pitch - 1)
				avg += *(src + 1) & 0x0f;
			avg /= 4;

			if ((*src & 0x0f) > avg)
				*dst = (*src & 0x0f) | 0xc0;
			//else
			//	*dst = 0;

			src++;
			dst++;
		}
	}
}

// Draw the equipped sidekick bodies in the weapon-sim preview, on top of the ship, mirroring
// the in-game draw (JE_playerMovement): a front/large body (tr 1/2) is a 2x2 from spriteSheet10,
// the rest a single tile from spriteSheet9. Positions are set in JE_weaponViewFrame. Without this
// the shop showed sidekick shots coming from nothing -- the pods themselves were never drawn.
static void JE_drawSimSidekicks(void)
{
	for (uint i = 0; i < 2; ++i)
	{
		const JE_OptionType *o = &options[player[0].items.sidekick[i]];
		if (o->option == 0)
			continue;  // slot empty ("None")

		// The equipped option can change as you browse the shop without the frame counter being
		// reset, so keep it in range before indexing gr[], then advance it like gameplay.
		if (player[0].sidekick[i].animation_frame >= o->ani)
			player[0].sidekick[i].animation_frame = 0;
		if (player[0].sidekick[i].animation_enabled)
		{
			if (++player[0].sidekick[i].animation_frame >= o->ani)
			{
				player[0].sidekick[i].animation_frame = 0;
				player[0].sidekick[i].animation_enabled = (o->option == 1);
			}
		}

		const int x = player[0].sidekick[i].x, y = player[0].sidekick[i].y;
		const uint sprite = o->gr[player[0].sidekick[i].animation_frame];

		// Tag the body like gameplay so JE_weaponSimSmoothPresent's rl_replay_interp can
		// match it across ticks — otherwise a moving pod (orbiting satellite) steps at the
		// ~23Hz menu tick instead of gliding.
		rl_current_id = RL_ID_SIDEKICK_BASE + 2 + (int)i;
		if (o->tr == 1 || o->tr == 2)
			blit_sprite2x2(VGAScreen, x - 6, y, spriteSheet10, sprite);
		else
			blit_sprite2(VGAScreen, x, y, spriteSheet9, sprite);
		rl_current_id = 0;
	}
}

void JE_weaponSimUpdate(void)
{
	char buf[32];

	JE_weaponViewFrame();

	++weaponSimTime;
	weaponSimTime %= 150;

	if (leftPower || rightPower)
	{
		if (!leftPower)
			blit_sprite(VGAScreenSeg, 24, 149, OPTION_SHAPES, 13);  // downgrade disabled
		if (!rightPower || !rightPowerAfford)
			blit_sprite(VGAScreenSeg, 119, 149, OPTION_SHAPES, 14);  // upgrade disabled

		temp = player[0].items.weapon[curSel[MENU_UPGRADES]-3].power;

		if ((curMenu == MENU_UPGRADE_SUB) && (curSel[MENU_UPGRADES] == 4)
			&& weaponPort[player[0].items.weapon[REAR_WEAPON].id].opnum == 2
			&& (weaponSimTime >= 75))
		{
			// [/] Rear Weapon Mode
#if defined(__SWITCH__) || defined(__vita__)
			// No [/] key on the consoles; the shoulder buttons cycle the mode
			// (see the L/R handler in JE_itemScreen). x nudged left so the
			// longer caption stays inside the preview border (ends at x=143).
			JE_outText(VGAScreen, 22, 137, "[L/R] Rear Weapon Mode", 1, 4);
#else
			JE_outText(VGAScreen, 28, 137, miscText[70], 1, 4);
#endif
		}
		else
		{
			if (leftPower)
			{
				sprintf(buf, "%lu", downgradeCost);
				JE_outText(VGAScreen, 26, 137, buf, 1, 4);
			}
			if (rightPower)
			{
				sprintf(buf, "%lu", upgradeCost);
				JE_outText(VGAScreen, 104, 137, buf, (rightPowerAfford) ? 1 : 7, 4);
			}

			sprintf(buf, "%s %d", miscTextB[5], temp);
			JE_outText(VGAScreen, 58, 137, buf, 15, 4);
		}

		for (int x = 1; x <= temp; x++)
		{
			fill_rectangle_xy(VGAScreen, 39 + x * 6, 151, 39 + x * 6 + 4, 151, 251);
			JE_pix(VGAScreen, 39 + x * 6, 151, 252);
			fill_rectangle_xy(VGAScreen, 39 + x * 6, 152, 39 + x * 6 + 4, 164, 250);
			fill_rectangle_xy(VGAScreen, 39 + x * 6, 165, 39 + x * 6 + 4, 165, 249);
		}
	}
	else
		blit_sprite(VGAScreenSeg, 20, 146, OPTION_SHAPES, 17);  // hide power level interface

	JE_drawItem(1, player[0].items.ship, player[0].x - 5, player[0].y - 7);

	JE_drawSimSidekicks();  // pods on top of the ship, matching gameplay layering
}

void JE_weaponViewFrame(void)
{
	fill_rectangle_xy(VGAScreen, 8, 8, 143, 182, 0);

	/* JE: (* Port Configuration Display *)
	(*    drawportconfigbuttons;*/

	update_and_draw_starfield(VGAScreen, 1);

	mouseX = player[0].x;
	mouseY = player[0].y;

	// Endless perks quicken the guns in the preview exactly as in play: apply the fire-rate perks'
	// extra shotRepeat decrements once per tick, so the preview's fire cadence (and the generator
	// drain on the power gauge) reflects the perks you own. Sampled once per tick.
	if (endlessMode)
	{
		const int dec = endlessPerkFireDecrements();
		for (unsigned i = 0; i < COUNTOF(shotRepeat); i++)
			for (int k = 0; k < dec && shotRepeat[i] > 0; k++)
				--shotRepeat[i];
	}

	// create shots in weapon simulator
	for (uint i = 0; i < 2; ++i)
	{
		if (shotRepeat[i] > 0)
		{
			--shotRepeat[i];
		}
		else
		{
			const uint item       = player[0].items.weapon[i].id,
			           item_power = player[0].items.weapon[i].power - 1,
			           item_mode = (i == REAR_WEAPON) ? player[0].weapon_mode - 1 : 0;

			// Zica Laser Lv11 tweaks: mirror the in-game fire (JE_mainGamePlayerFunctions)
			// so the preview matches. Long swaps in the two LV10-length side beams; Buff
			// adds the Lv10 centre beam. Extra beams are drain-free.
			const bool zica_l11 = (item == 5 && item_power == 10);
			JE_word l11_primary = weaponPort[item].op[item_mode][item_power];
			if (zica_l11 && zicaLaserLength == ZICA_LEN_LONG)
				l11_primary = ZICA_LONG_WEAP_LEFT;

			b = player_shot_create(item, i, player[0].x, player[0].y, mouseX, mouseY, l11_primary, 1);

			if (zica_l11 && (zicaLaserLength == ZICA_LEN_LONG || zicaLaserBuff))
			{
				JE_word saved_poweruse = weaponPort[item].poweruse;
				weaponPort[item].poweruse = 0;
				if (zicaLaserLength == ZICA_LEN_LONG)
					player_shot_create(item, i, player[0].x, player[0].y, mouseX, mouseY, ZICA_LONG_WEAP_RIGHT, 1);
				if (zicaLaserBuff)
					player_shot_create(item, i, player[0].x, player[0].y, mouseX, mouseY, weaponPort[item].op[item_mode][9], 1);
				weaponPort[item].poweruse = saved_poweruse;
			}
		}
	}

	// Position + fire both sidekicks, mirroring the gameplay mounts so the preview is faithful:
	// side pods (tr 0), front pods (tr 2), and orbiting satellites (tr 4) use their gameplay
	// offsets; trailing companions (tr 1/3) keep a side-by-side layout. notes.md §Menus & shop.
	const bool bothFront = options[player[0].items.sidekick[LEFT_SIDEKICK]].tr == 2
	                    && options[player[0].items.sidekick[RIGHT_SIDEKICK]].tr == 2;

	// advance the shared satellite angle exactly like gameplay
	if (options[player[0].items.sidekick[LEFT_SIDEKICK]].tr == 4
	    && options[player[0].items.sidekick[RIGHT_SIDEKICK]].tr == 4)
		optionSatelliteRotate += 0.2f;
	else if (options[player[0].items.sidekick[LEFT_SIDEKICK]].tr == 4
	         || options[player[0].items.sidekick[RIGHT_SIDEKICK]].tr == 4)
		optionSatelliteRotate += 0.15f;

	for (uint i = 0; i < 2; ++i)
	{
		const uint item = player[0].items.sidekick[i];
		const JE_OptionType *o = &options[item];
		const uint shot_i = (i == LEFT_SIDEKICK) ? SHOT_LEFT_SIDEKICK : SHOT_RIGHT_SIDEKICK;

		switch (o->tr)
		{
		case 0:  // fixed side pod (e.g. Zica Supercharger)
			player[0].sidekick[i].x = player[0].x + ((i == LEFT_SIDEKICK) ? -14 : 16);
			player[0].sidekick[i].y = player[0].y;
			break;
		case 2:  // front-mounted
			player[0].sidekick[i].x = !bothFront ? player[0].x
			                        : (i == LEFT_SIDEKICK ? player[0].x - FRONT_OPTION_SPREAD
			                                              : player[0].x + FRONT_OPTION_SPREAD);
			player[0].sidekick[i].y = MAX(10, player[0].y - 20);
			break;
		case 4:  // orbiting satellite (e.g. Satellite Marlo) -- the two slots orbit opposite ends
		{
			const int dx = roundf(sinf(optionSatelliteRotate) * 20),
			          dy = roundf(cosf(optionSatelliteRotate) * 20);
			player[0].sidekick[i].x = player[0].x + ((i == LEFT_SIDEKICK) ? dx : -dx);
			player[0].sidekick[i].y = player[0].y + ((i == LEFT_SIDEKICK) ? dy : -dy);
			break;
		}
		default:  // trailing companions (tr 1/3) -- deliberately side by side, not gameplay-faithful
			player[0].sidekick[i].x = (i == LEFT_SIDEKICK) ? 72 - 15 : 72 + 15;
			player[0].sidekick[i].y = 120;
			break;
		}

		if (o->wport > 0)
		{
			if (shotRepeat[shot_i] > 0)
			{
				--shotRepeat[shot_i];
			}
			else
			{
				b = player_shot_create(o->wport, shot_i, player[0].sidekick[i].x, player[0].sidekick[i].y, mouseX, mouseY, o->wpnum, 1);
				player[0].sidekick[i].animation_enabled = true;  // animate the body while it fires
			}
		}
	}

	simulate_player_shots();

	// Clip spark trails to the preview box (matches the fill_rectangle above). Sparks drift
	// outward for ~15 frames, so near the box's right edge a superspark trail (Mega Pulse etc.)
	// would otherwise spill past the frame into the item list on the right of the shop.
	JE_setSPClip(8, 8, 143, 182);
	JE_drawSP();  // draw the shot superspark trails inside the box before the interface sprite
	              // frames it; without this the sparks generate but never show
	JE_clearSPClip();

	blit_sprite(VGAScreenSeg, 0, 0, OPTION_SHAPES, 12); // upgrade interface

	// weapon mode indicator
	if (player[0].weapon_mode == 1)
	{
		blit_sprite(VGAScreenSeg, 3, 56, OPTION_SHAPES, 18);  // lit
		blit_sprite(VGAScreenSeg, 3, 64, OPTION_SHAPES, 19);  // unlit
	}
	else // == 2
	{
		blit_sprite(VGAScreenSeg, 3, 56, OPTION_SHAPES, 19);  // unlit
		blit_sprite(VGAScreenSeg, 3, 64, OPTION_SHAPES, 18);  // lit
	}

	/*========================Power Bar=========================*/

	power += powerAdd;
	if (power > 900)
		power = 900;

	// Snapshot the clean interface under the gauge (interface sprite just blitted, slot
	// still untouched) so JE_weaponSimSmoothPresent can redraw the bar at levels
	// interpolated between the previous and current tick.
	menu_power_capture_bg(VGAScreen);
	menu_power_prev = menu_power_cur;
	menu_power_cur = power;

	draw_menu_power_bar(VGAScreen, power);

	lastPower = 147 - (power / 10);

	//JE_waitFrameCount();  TODO: didn't do anything?
}
