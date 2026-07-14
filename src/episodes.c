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
#include <string.h>

#include "episodes.h"

#include "config.h"
#include "custom_weapon.h"
#include "file.h"
#include "lvllib.h"
#include "lvlmast.h"
#include "opentyr.h"

/* MAIN Weapons Data */
JE_WeaponPortType weaponPort;
JE_WeaponType     weapons[WEAP_NUM + 1]; /* [0..weapnum] */

/* Items */
JE_PowerType   powerSys;
JE_ShipType    ships;
JE_OptionType  options[OPTION_NUM + 1]; /* [0..optionnum] */
JE_ShieldType  shields;
JE_SpecialType special;

/* Enemy data */
JE_EnemyDatType enemyDat;

/* EPISODE variables */
JE_byte    initial_episode_num, episodeNum = 0;
JE_boolean episodeAvail[EPISODE_MAX]; /* [1..episodemax] */
char       episode_file[13], cube_file[13];

JE_longint episode1DataLoc;

/* Tells the game whether the level currently loaded is a bonus level. */
JE_boolean bonusLevel;

/* Tells if the game jumped back to Episode 1 */
JE_boolean jumpBackToEpisode1;

// Re-adds the cut-from-Tyrian-2000 "Charge-Laser Cannon", a 5-stage DOS charge sidekick
// (sprites survive in spriteSheet9); values below are verbatim from the DOS LVLs. notes.md §Weapons.
#define CHARGELASER_WEAP_BASE 900  // 6 scratch weapon slots in the unused WEAP_END1(818)..WEAP_START2(1000) gap

// Option slot claimed for the current episode (differs per episode), or 0 if none
// was free; the shop loader (tyrian2.c) reads this.
JE_byte chargeLaserSlot = 0;

static void JE_addChargeLaserCannon(void)
{
	// Claim the first free ("None") sidekick slot; free slots differ per episode, so
	// this never clobbers a real weapon. Names are space-padded to 30 chars, hence the
	// prefix match.
	int slot = 0;
	for (int i = 1; i <= OPTION_NUM; ++i)
	{
		if (strncmp(options[i].name, "None", 4) == 0)
		{
			slot = i;
			break;
		}
	}
	chargeLaserSlot = slot;  // 0 if no free slot; shops check for > 0
	if (slot == 0)
		return;  // no spare slot in this episode's item data

	// The six charge stages (fired as wpnum + charge, 0..5), verbatim from DOS weapons
	// 452..457 (byte-identical across TYRIAN{1,2,3}.LVL). Each is one straight-up bolt
	// differing only in fire rate, damage, and sprite; rate SLOWS and damage RISES with
	// charge. All other fields are zero, hence the memset.
	static const struct { JE_byte shotrepeat, attack; JE_word sg; } stage[6] =
	{
		{  4,  2, 260 },  // charge 0: fast, weak
		{  6,  3, 261 },  // charge 1
		{  8,  5, 262 },  // charge 2
		{ 10, 10, 263 },  // charge 3
		{ 12, 20, 264 },  // charge 4
		{ 14, 40, 265 },  // charge 5: slow, powerful
	};
	for (int k = 0; k < 6; ++k)
	{
		JE_WeaponType *w = &weapons[CHARGELASER_WEAP_BASE + k];
		memset(w, 0, sizeof(*w));
		w->shotrepeat      = stage[k].shotrepeat;
		w->multi           = 1;
		w->max             = 1;
		w->attack[0]       = stage[k].attack;
		w->del[0]          = 255;
		w->sy[0]           = 11;             // bolt travels straight up at speed 11
		w->sg[0]           = stage[k].sg;    // Charge-Laser bolt sprite (player-shot / spriteSheet8 260..265)
		w->sound           = 6;              // original's fire sound
		w->trail           = 255;
		w->shipblastfilter = 208;
	}

	// Its in-game sidekick frames (87/106/125/144, each x3), still in spriteSheet9.
	static const JE_word grFrames[20] =
		{ 87,87,87, 106,106,106, 125,125,125, 144,144,144 };

	JE_OptionType *o = &options[slot];
	memset(o, 0, sizeof(*o));
	strcpy(o->name, "Charge-Laser Cannon");
	o->pwr         = 5;      // five charge stages -- its defining trait
	o->itemgraphic = 193;    // shop icon (matches the original record)
	o->cost        = 30000;
	o->tr          = 0;      // side-mounted (style 0 -> drawn from spriteSheet9)
	o->option      = 1;      // always animating
	o->opspd       = 3;
	o->ani         = 12;     // 4 frames x 3, as in the original
	memcpy(o->gr, grFrames, sizeof(o->gr));
	o->wport       = 4;      // power-drain port (as original; also the Zica Flamethrower's)
	o->wpnum       = CHARGELASER_WEAP_BASE;
	o->ammo        = 0;      // infinite -- a charge weapon, not an ammo weapon
	o->stop        = true;
	o->icongr      = 6;
}

// The Zica Laser (port 5) Lv11 native horizontal layout, captured before we reshape it so
// ZICA_BASE_AUTO can restore the episode's vanilla pattern. notes.md §Weapons.
static JE_shortint zicaNativeSx[8], zicaNativeBx[8];
static bool zicaNativeCaptured = false;

// Apply the configured Zica Lv11 tweaks (config.h: zicaLaserBase / zicaLaserLength):
//  1. shape the base Lv11 weapon (wpn 209) to the chosen horizontal pattern (SHORT shots);
//  2. build the two LV10-length side beams (scratch wpns) the LONG length fires instead.
// The Lv10 centre beam ("Buff") and the short-vs-long choice are handled at fire time
// (mainint.c / game_menu.c); this only prepares the weapon data. Safe to call repeatedly.
static void JE_applyZicaLaserConfig(void)
{
	const int wn11 = weaponPort[5].op[0][10];  // Zica Laser (port 5), Lv11 weapon (209)
	const int wn10 = weaponPort[5].op[0][9];   // Lv10 weapon (208) -- the long-beam template
	if (wn11 <= 0 || wn11 > WEAP_NUM || wn10 <= 0 || wn10 > WEAP_NUM)
		return;

	// Effective horizontal pattern: spread (ep4) vs two straight columns (ep1-3).
	bool spread;
	if (zicaLaserBase == ZICA_BASE_EP13)
		spread = false;
	else if (zicaLaserBase == ZICA_BASE_EP4)
		spread = true;
	else  // ZICA_BASE_AUTO: whatever this episode's data shipped with
		spread = zicaNativeCaptured ? (zicaNativeSx[0] != 0) : true;

	static const JE_shortint sx_spread[8]   = { -1, -1, -1, -1,  1,  1,  1,  1 };
	static const JE_shortint bx_spread[8]   = {  0,  0,  0,  0,  0,  0,  0,  0 };
	static const JE_shortint sx_straight[8] = {  0,  0,  0,  0,  0,  0,  0,  0 };
	static const JE_shortint bx_straight[8] = { -8, -8, -8, -8,  8,  8,  8,  8 };

	// (1) Shape the base Lv11 (short) weapon. Auto restores the captured native layout
	// (byte-identical to vanilla); forced modes overwrite sx/bx (sy/by/sg keep the vanilla
	// short bolts).
	if (zicaLaserBase == ZICA_BASE_AUTO && zicaNativeCaptured)
	{
		memcpy(weapons[wn11].sx, zicaNativeSx, sizeof(weapons[wn11].sx));
		memcpy(weapons[wn11].bx, zicaNativeBx, sizeof(weapons[wn11].bx));
	}
	else if (zicaLaserBase != ZICA_BASE_AUTO)
	{
		memcpy(weapons[wn11].sx, spread ? sx_spread : sx_straight, sizeof(weapons[wn11].sx));
		memcpy(weapons[wn11].bx, spread ? bx_spread : bx_straight, sizeof(weapons[wn11].bx));
	}

	// (2) Build the two LONG side beams: full copies of the Lv10 beam (8 solid segments,
	// so they look and reach like the real Lv10 shot) placed left/right. The Lv10 template
	// is ship-locked (sx=120); zicaLaserLock chooses how the side beams move:
	//   Lock on  -> stay ship-locked (glued to the ship). A locked shot can't drift,
	//               so both patterns become two locked columns.
	//   Lock off -> free-flying (default): columns go straight up (sx=0), spread drifts (sx=+-1).
	memcpy(&weapons[ZICA_LONG_WEAP_LEFT],  &weapons[wn10], sizeof(JE_WeaponType));
	memcpy(&weapons[ZICA_LONG_WEAP_RIGHT], &weapons[wn10], sizeof(JE_WeaponType));
	for (int i = 0; i < 8; ++i)
	{
		JE_shortint lsx, rsx, lbx, rbx;
		if (zicaLaserLock)
		{
			lsx = rsx = 120;         // ship-locked (sentinel), glued to the ship
			lbx = -8; rbx = 8;       // 8px left/right so both columns are visible
		}
		else if (spread)
		{
			lsx = -1; rsx = 1;       // drift out from the centre (ep4)
			lbx = 0;  rbx = 0;
		}
		else
		{
			lsx = rsx = 0;           // straight up (ep1-3)
			lbx = -8; rbx = 8;
		}
		weapons[ZICA_LONG_WEAP_LEFT].sx[i]  = lsx;
		weapons[ZICA_LONG_WEAP_RIGHT].sx[i] = rsx;
		weapons[ZICA_LONG_WEAP_LEFT].bx[i]  = lbx;
		weapons[ZICA_LONG_WEAP_RIGHT].bx[i] = rbx;
	}
}

// Superspark trails: retags the ep4/5 ">1000" spark-shower shot graphics per superSparkMode
// (Auto = per-episode shipped, On/Off = force). Idempotent. notes.md §Weapons.

// Retag one weapon's fire pattern between plain[i] and tagged[i] per the SUPER_SPARKS_* mode.
static void JE_retagWeaponSparks(int wn, int mode, const JE_word *plain, const JE_word *tagged, int nsprites)
{
	if (wn <= 0 || wn > WEAP_NUM)
		return;
	// Only the first weapons[wn].max shot-graphic slots form the repeating fire pattern
	// (shotMultiPos cycles 1..max in shots.c); retag each real bolt (either state),
	// leaving the unused padding slots (never a listed sprite) alone.
	for (int j = 0; j < weapons[wn].max && j < 8; ++j)
	{
		for (int k = 0; k < nsprites; ++k)
		{
			if (weapons[wn].sg[j] == plain[k] || weapons[wn].sg[j] == tagged[k])
			{
				bool tag;
				switch (mode)
				{
				case SUPER_SPARKS_ON:  tag = true;  break;
				case SUPER_SPARKS_OFF: tag = false; break;
				default:               tag = (episodeNum > 3); break;  // Auto = the shipped value
				}
				weapons[wn].sg[j] = tag ? tagged[k] : plain[k];
				break;
			}
		}
	}
}

static void JE_applySuperSparks(void)
{
	// Mega Pulse: resolve the power-level weapons through the port table.
	static const JE_word pulsePlain[]  = { 35 },  pulseTagged[]  = { 7035 };
	for (int lvl = 0; lvl < 11; ++lvl)
		JE_retagWeaponSparks(weaponPort[19].op[0][lvl], superSparkMode[SSW_MEGA_PULSE],
		                     pulsePlain, pulseTagged, COUNTOF(pulsePlain));

	// Beno Wallop Beam sidekick (option wpnum 736). The ep4/5 record also fires a second
	// bolt each volley (multi/max 2; sprite 29 launched 2px ahead of the sprite-30 bolt,
	// same damage/speed) that the ep1-3 record lacks. wallopSecondBolt forces that pattern
	// in/out of every episode; slot 1 is rebuilt from the shipped ep4/5 values (identical
	// in tyrian4.lvl and tyrian5.lvl -- ep1-3 only carries garbage padding there), so this
	// stays idempotent. Runs before the spark retag below so the rebuilt bolt's trail also
	// follows the Sparks mode.
	{
		JE_WeaponType *w = &weapons[736];
		bool second;
		switch (wallopSecondBolt)
		{
		case SUPER_SPARKS_ON:  second = true;  break;
		case SUPER_SPARKS_OFF: second = false; break;
		default:               second = (episodeNum > 3); break;  // Auto = the shipped pattern
		}
		if (second)
		{
			w->multi = 2;
			w->max   = 2;
			w->attack[1] = 10;
			w->del[1]    = 255;
			w->sx[1] = 0;  w->sy[1] = 10;
			w->bx[1] = 0;  w->by[1] = -2;
			w->sg[1] = 7029;  // spark-tagged like the shipped data; the retag applies the mode
		}
		else
		{
			w->multi = 1;  // slot 1 becomes unused padding again
			w->max   = 1;
		}
	}
	static const JE_word wallopPlain[] = { 30, 29 }, wallopTagged[] = { 7030, 7029 };
	JE_retagWeaponSparks(736, superSparkMode[SSW_WALLOP_BEAM], wallopPlain, wallopTagged, COUNTOF(wallopPlain));

	// Beno Protron System -B- sidekick (option wpnum 737).
	static const JE_word protronPlain[] = { 28 }, protronTagged[] = { 9028 };
	JE_retagWeaponSparks(737, superSparkMode[SSW_PROTRON_B], protronPlain, protronTagged, COUNTOF(protronPlain));

	// Ice Beam (special wpn 621) and Ice Blast (special wpn 706) share one setting: both
	// fire the same spark-tagged sprite.
	static const JE_word icePlain[] = { 634 }, iceTagged[] = { 9634 };
	JE_retagWeaponSparks(621, superSparkMode[SSW_ICE], icePlain, iceTagged, COUNTOF(icePlain));
	JE_retagWeaponSparks(706, superSparkMode[SSW_ICE], icePlain, iceTagged, COUNTOF(icePlain));
}

// Weapons whose ep1-3 vs ep4/5 item data differ beyond the superspark trail. epDiffMode[]
// forces either data set (Auto = keep the running episode's); every field is rewritten from
// the shipped constants, so it is idempotent exactly like JE_applySuperSparks. Only the
// meaningful [0..max-1] pattern slots are touched (higher slots carry editor garbage).
static void JE_applyEpDiffs(void)
{
	for (int w = 0; w < EDW_COUNT; ++w)
	{
		bool ep45;
		switch (epDiffMode[w])
		{
		case EPDIFF_EP13: ep45 = false;             break;
		case EPDIFF_EP45: ep45 = true;              break;
		default:          ep45 = (episodeNum > 3);  break;  // Auto = the shipped era
		}

		switch (w)
		{
		case EDW_XEGA_BALL:
		{
			// ep1-3: two balls, a six-step spreading pattern, 4 damage each.
			// ep4/5: a single ball, one step, 8 damage.
			JE_WeaponType *x = &weapons[720];
			if (ep45)
			{
				x->multi = 1;  x->max = 1;
				x->attack[0] = 8;  x->del[0] = 255;
				x->sx[0] = 0;  x->sy[0] = 10;  x->bx[0] = -20;  x->by[0] = -15;
				x->sg[0] = 60022;
			}
			else
			{
				static const JE_shortint sx[6] = { 0, 0, -8, 8, -10, 10 };
				static const JE_shortint sy[6] = { 10, 10, 8, 8, 0, 0 };
				static const JE_shortint bx[6] = { -20, -20, -30, -10, -40, 0 };
				static const JE_shortint by[6] = { -15, -15, -10, -10, -10, -10 };
				x->multi = 2;  x->max = 6;
				for (int i = 0; i < 6; ++i)
				{
					x->attack[i] = 4;  x->del[i] = 255;
					x->sx[i] = sx[i];  x->sy[i] = sy[i];  x->bx[i] = bx[i];  x->by[i] = by[i];
					x->sg[i] = 60022;
				}
			}
			break;
		}
		case EDW_MICROSOL_OPT5:
		{
			// The MicroSol bonus ship's 5th built-in weapon (the only one of its six that
			// differs). ep1-3: a costly (drain 160) eight-way fan, 3 damage each.
			// ep4/5: a cheap (drain 40) twin shot, 1 damage each.
			JE_WeaponType *m = &weapons[23];
			if (ep45)
			{
				m->drain = 40;  m->multi = 2;  m->max = 2;  m->acceleration = 0;
				m->attack[0] = 1;   m->attack[1] = 1;
				m->del[0] = 255;    m->del[1] = 255;
				m->sx[0] = -14;     m->sx[1] = -14;
				m->sy[0] = 0;       m->sy[1] = 0;
				m->bx[0] = -8;      m->bx[1] = 8;
				m->by[0] = 0;       m->by[1] = 0;
				m->sg[0] = 99;      m->sg[1] = 99;
			}
			else
			{
				static const JE_shortint sx[8] = { 1, -1, 2, -2, 3, -3, 4, -4 };
				static const JE_shortint sy[8] = { 3, 3, 2, 2, 1, 1, 0, 0 };
				m->drain = 160;  m->multi = 8;  m->max = 8;  m->acceleration = 1;
				for (int i = 0; i < 8; ++i)
				{
					m->attack[i] = 3;  m->del[i] = 255;
					m->sx[i] = sx[i];  m->sy[i] = sy[i];  m->bx[i] = 0;  m->by[i] = 0;
					m->sg[i] = 73;
				}
			}
			break;
		}
		case EDW_FLARE:
			// Flare / Super Bomb (wpn 622): the first four blast frames are sprite 20 (ep1-3)
			// or 21 (ep4/5); the rest of the pattern is identical.
			for (int i = 0; i < 4; ++i)
				weapons[622].sg[i] = ep45 ? 21 : 20;
			break;
		case EDW_NEEDLE_LASER:  weapons[781].sound = ep45 ? 13 : 31;  break;
		case EDW_BUBBLE_GUM:    weapons[792].sound = ep45 ? 13 : 30;  break;
		case EDW_FLYING_PUNCH:  weapons[794].sound = ep45 ? 30 : 31;  break;
		case EDW_PRETZEL_MISSILE: weapons[795].sound = ep45 ? 30 : 31;  break;
		case EDW_DRAGON_FROST:  weapons[806].sound = ep45 ? 30 : 31;  break;
		}
	}
}

// The classic ammo sidekicks advertise their magazine size right in the shop name --
// "MegaMissile    Ammo 5", "Atom Bombs     Ammo 20", ... -- with every "Ammo N" aligned
// at column 15 so they line up down the item list. The two Tyrian 2000 additions, the
// Bubble Gum-Gun (ammo 80) and the Flying Punch (ammo 20), carry a real magazine
// (option.ammo > 0, so the in-game HUD gauge already works) but shipped without that
// label, leaving them the only ammo sidekicks whose count never appeared in the shop.
// Re-add the label in the original style for any ammo option that lacks it.
static void JE_labelAmmoSidekicks(void)
{
	for (int i = 1; i <= OPTION_NUM; ++i)
	{
		if (options[i].ammo == 0                        // charge/infinite sidekick: no magazine
		    || strncmp(options[i].name, "None", 4) == 0 // empty slot
		    || strstr(options[i].name, "Ammo") != NULL) // already labelled (the classic weapons)
			continue;

		// Names are space-padded to 30 chars in the data; trim to find where the real name ends.
		size_t len = strlen(options[i].name);
		while (len > 0 && options[i].name[len - 1] == ' ')
			--len;

		char label[16];
		int label_len = snprintf(label, sizeof(label), "Ammo %d", options[i].ammo);

		// Align to column 15 like the originals (or one space past the name if it already
		// reaches that far), clamped so the label never overruns the 30-char name field.
		size_t col = (len < 15) ? 15 : len + 1;
		if (col + (size_t)label_len > 30)
			col = (30 >= (size_t)label_len) ? 30 - (size_t)label_len : 0;

		for (size_t k = len; k < col; ++k)
			options[i].name[k] = ' ';
		memcpy(options[i].name + col, label, label_len);
		options[i].name[col + label_len] = '\0';
	}
}

void JE_loadItemDat(void)
{
	FILE *f = NULL;
	
	if (episodeNum <= 3)
	{
		f = dir_fopen_die(data_dir(), "tyrian.hdt", "rb");
		fread_s32_die(&episode1DataLoc, 1, f);
		fseek(f, episode1DataLoc, SEEK_SET);
	}
	else
	{
		// episode 4 stores item data in the level file
		f = dir_fopen_die(data_dir(), levelFile, "rb");
		fseek(f, lvlPos[lvlNum-1], SEEK_SET);
	}

	JE_word itemNum[7]; /* [1..7] */
	fread_u16_die(itemNum, 7, f);

	const int weapons_bounds[2][2] = {{0, WEAP_END1}, {WEAP_START2, WEAP_NUM}};
	for (int bank = 0; bank < 2; ++bank)
	{
		for (int i = weapons_bounds[bank][0]; i < weapons_bounds[bank][1] + 1; ++i)
		{
			fread_u16_die(&weapons[i].drain,           1, f);
			fread_u8_die( &weapons[i].shotrepeat,      1, f);
			fread_u8_die( &weapons[i].multi,           1, f);
			fread_u16_die(&weapons[i].weapani,         1, f);
			fread_u8_die( &weapons[i].max,             1, f);
			fread_u8_die( &weapons[i].tx,              1, f);
			fread_u8_die( &weapons[i].ty,              1, f);
			fread_u8_die( &weapons[i].aim,             1, f);
			fread_u8_die(  weapons[i].attack,          8, f);
			fread_u8_die(  weapons[i].del,             8, f);
			fread_s8_die(  weapons[i].sx,              8, f);
			fread_s8_die(  weapons[i].sy,              8, f);
			fread_s8_die(  weapons[i].bx,              8, f);
			fread_s8_die(  weapons[i].by,              8, f);
			fread_u16_die( weapons[i].sg,              8, f);
			fread_s8_die( &weapons[i].acceleration,    1, f);
			fread_s8_die( &weapons[i].accelerationx,   1, f);
			fread_u8_die( &weapons[i].circlesize,      1, f);
			fread_u8_die( &weapons[i].sound,           1, f);
			fread_u8_die( &weapons[i].trail,           1, f);
			fread_u8_die( &weapons[i].shipblastfilter, 1, f);
		}
	}
	
	for (int i = 0; i < PORT_NUM + 1; ++i)
	{
		Uint8 nameLen;
		fread_u8_die( &nameLen,                   1, f);
		fread_die(    &weaponPort[i].name,    1, 30, f);
		weaponPort[i].name[MIN(nameLen, 30)] = '\0';
		fread_u8_die( &weaponPort[i].opnum,       1, f);
		fread_u16_die( weaponPort[i].op[0],      11, f);
		fread_u16_die( weaponPort[i].op[1],      11, f);
		fread_u16_die(&weaponPort[i].cost,        1, f);
		fread_u16_die(&weaponPort[i].itemgraphic, 1, f);
		fread_u16_die(&weaponPort[i].poweruse,    1, f);
	}

	for (int i = 0; i < SPECIAL_NUM + 1; ++i)
	{
		Uint8 nameLen;
		fread_u8_die( &nameLen,                1, f);
		fread_die(    &special[i].name,    1, 30, f);
		special[i].name[MIN(nameLen, 30)] = '\0';
		fread_u16_die(&special[i].itemgraphic, 1, f);
		fread_u8_die( &special[i].pwr,         1, f);
		fread_u8_die( &special[i].stype,       1, f);
		fread_u16_die(&special[i].wpn,         1, f);
	}

	for (int i = 0; i < POWER_NUM + 1; ++i)
	{
		Uint8 nameLen;
		fread_u8_die( &nameLen,                 1, f);
		fread_die(    &powerSys[i].name,    1, 30, f);
		powerSys[i].name[MIN(nameLen, 30)] = '\0';
		fread_u16_die(&powerSys[i].itemgraphic, 1, f);
		fread_u8_die( &powerSys[i].power,       1, f);
		fread_s8_die( &powerSys[i].speed,       1, f);
		fread_u16_die(&powerSys[i].cost,        1, f);
	}

	for (int i = 0; i < SHIP_NUM + 1; ++i)
	{
		Uint8 nameLen;
		fread_u8_die( &nameLen,                 1, f);
		fread_die(    &ships[i].name,       1, 30, f);
		ships[i].name[MIN(nameLen, 30)] = '\0';
		fread_u16_die(&ships[i].shipgraphic,    1, f);
		fread_u16_die(&ships[i].itemgraphic,    1, f);
		fread_u8_die( &ships[i].ani,            1, f);
		fread_s8_die( &ships[i].spd,            1, f);
		fread_u8_die( &ships[i].dmg,            1, f);
		fread_u16_die(&ships[i].cost,           1, f);
		fread_u8_die( &ships[i].bigshipgraphic, 1, f);
	}

	for (int i = 0; i < OPTION_NUM + 1; ++i)
	{
		Uint8 nameLen;
		fread_u8_die(  &nameLen,                1, f);
		fread_die(     &options[i].name,    1, 30, f);
		options[i].name[MIN(nameLen, 30)] = '\0';
		fread_u8_die(  &options[i].pwr,         1, f);
		fread_u16_die( &options[i].itemgraphic, 1, f);
		fread_u16_die( &options[i].cost,        1, f);
		fread_u8_die(  &options[i].tr,          1, f);
		fread_u8_die(  &options[i].option,      1, f);
		fread_s8_die(  &options[i].opspd,       1, f);
		fread_u8_die(  &options[i].ani,         1, f);
		fread_u16_die(  options[i].gr,         20, f);
		fread_u8_die(  &options[i].wport,       1, f);
		fread_u16_die( &options[i].wpnum,       1, f);
		fread_u8_die(  &options[i].ammo,        1, f);
		fread_bool_die(&options[i].stop,           f);
		fread_u8_die(  &options[i].icongr,      1, f);
	}

	for (int i = 0; i < SHIELD_NUM + 1; ++i)
	{
		Uint8 nameLen;
		fread_u8_die( &nameLen,                1, f);
		fread_die(    &shields[i].name,    1, 30, f);
		shields[i].name[MIN(nameLen, 30)] = '\0';
		fread_u8_die( &shields[i].tpwr,        1, f);
		fread_u8_die( &shields[i].mpwr,        1, f);
		fread_u16_die(&shields[i].itemgraphic, 1, f);
		fread_u16_die(&shields[i].cost,        1, f);
	}

	const int enemies_bounds[2][2] = {{0, ENEMY_END1}, {ENEMY_START2, ENEMY_NUM}};
	for (int bank = 0; bank < 2; ++bank)
	{
		for (int i = enemies_bounds[bank][0]; i < enemies_bounds[bank][1] + 1; ++i)
		{
			fread_u8_die( &enemyDat[i].ani,           1, f);
			fread_u8_die(  enemyDat[i].tur,           3, f);
			fread_u8_die(  enemyDat[i].freq,          3, f);
			fread_s8_die( &enemyDat[i].xmove,         1, f);
			fread_s8_die( &enemyDat[i].ymove,         1, f);
			fread_s8_die( &enemyDat[i].xaccel,        1, f);
			fread_s8_die( &enemyDat[i].yaccel,        1, f);
			fread_s8_die( &enemyDat[i].xcaccel,       1, f);
			fread_s8_die( &enemyDat[i].ycaccel,       1, f);
			fread_s16_die(&enemyDat[i].startx,        1, f);
			fread_s16_die(&enemyDat[i].starty,        1, f);
			fread_s8_die( &enemyDat[i].startxc,       1, f);
			fread_s8_die( &enemyDat[i].startyc,       1, f);
			fread_u8_die( &enemyDat[i].armor,         1, f);
			fread_u8_die( &enemyDat[i].esize,         1, f);
			fread_u16_die( enemyDat[i].egraphic,     20, f);
			fread_u8_die( &enemyDat[i].explosiontype, 1, f);
			fread_u8_die( &enemyDat[i].animate,       1, f);
			fread_u8_die( &enemyDat[i].shapebank,     1, f);
			fread_s8_die( &enemyDat[i].xrev,          1, f);
			fread_s8_die( &enemyDat[i].yrev,          1, f);
			fread_u16_die(&enemyDat[i].dgr,           1, f);
			fread_s8_die( &enemyDat[i].dlevel,        1, f);
			fread_s8_die( &enemyDat[i].dani,          1, f);
			fread_u8_die( &enemyDat[i].elaunchfreq,   1, f);
			fread_u16_die(&enemyDat[i].elaunchtype,   1, f);
			fread_s16_die(&enemyDat[i].value,         1, f);
			fread_u16_die(&enemyDat[i].eenemydie,     1, f);
		}
	}

	fclose(f);

	if (chargeLaserCannon)
		JE_addChargeLaserCannon();  // re-add the cut DOS Charge-Laser Cannon (shops + debug menu)
	else
		chargeLaserSlot = 0;        // disabled: shops/debug keep the stock item layout

	// Capture the Zica Lv11 native pattern now, while wpn 209 still holds freshly-loaded
	// data, so ZICA_BASE_AUTO can restore it later (JE_applyZicaLaserConfig may reshape it).
	{
		const int wn11 = weaponPort[5].op[0][10];
		if (wn11 > 0 && wn11 <= WEAP_NUM)
		{
			memcpy(zicaNativeSx, weapons[wn11].sx, sizeof(zicaNativeSx));
			memcpy(zicaNativeBx, weapons[wn11].bx, sizeof(zicaNativeBx));
			zicaNativeCaptured = true;
		}
	}

	JE_applyZicaLaserConfig();      // shape the configured Lv11 pattern + build the long beams

	JE_applySuperSparks();          // restore the ep4/5 superspark trails on the ep1-3 weapons

	JE_applyEpDiffs();              // force the configured ep1-3/ep4-5 data on the other diff weapons

	JE_labelAmmoSidekicks();        // show the magazine size in the shop name (Flying Punch, Bubble Gum-Gun)

	// Wobbley's first animation frame ships as stray sprite 166 (a neighbouring small-pod
	// graphic) while the rest of its loop is the 246/265/284/303 wobble; snap frame 0 to the
	// base rest frame so it no longer flashes the wrong pod once per cycle.
	for (int i = 0; i <= OPTION_NUM; ++i)
		if (strncmp(options[i].name, "Wobbley", 7) == 0 && options[i].gr[0] == 166)
			options[i].gr[0] = options[i].gr[1];

	// Give every icon-less shop item a placeholder icon (167) so it no longer renders a
	// blank box; skip the "None" entries (their blank icon is intentional) and empty slots.
	for (int i = 1; i <= PORT_NUM; ++i)
		if (weaponPort[i].itemgraphic == 0 && weaponPort[i].name[0] && strncmp(weaponPort[i].name, "None", 4) != 0)
			weaponPort[i].itemgraphic = 167;
	for (int i = 1; i <= POWER_NUM; ++i)
		if (powerSys[i].itemgraphic == 0 && powerSys[i].name[0] && strncmp(powerSys[i].name, "None", 4) != 0)
			powerSys[i].itemgraphic = 167;
	for (int i = 1; i <= OPTION_NUM; ++i)
		if (options[i].itemgraphic == 0 && options[i].name[0] && strncmp(options[i].name, "None", 4) != 0)
			options[i].itemgraphic = 167;
	for (int i = 1; i <= SHIELD_NUM; ++i)
		if (shields[i].itemgraphic == 0 && shields[i].name[0] && strncmp(shields[i].name, "None", 4) != 0)
			shields[i].itemgraphic = 167;

	customWeaponInit();             // claim a free port + compile the user's custom weapon
}

void JE_initEpisode(JE_byte newEpisode)
{
	if (newEpisode == episodeNum)
	{
		// Same episode: item data isn't reloaded, but the Zica Lv11 config may have changed
		// in the menu since -- reapply so it takes effect next game (restores/reshapes wpn 209
		// from the captured native, so it is idempotent).
		JE_applyZicaLaserConfig();
		JE_applySuperSparks();      // likewise reapply the superspark trail modes (self-correcting)
		JE_applyEpDiffs();          // and the other per-weapon ep1-3/ep4-5 choices
		return;
	}

	episodeNum = newEpisode;
	
	snprintf(levelFile,    sizeof(levelFile),    "tyrian%hhu.lvl",  episodeNum);
	snprintf(cube_file,    sizeof(cube_file),    "cubetxt%hhu.dat", episodeNum);
	snprintf(episode_file, sizeof(episode_file), "levels%hhu.dat",  episodeNum);
	
	JE_analyzeLevel();
	JE_loadItemDat();
}

void JE_scanForEpisodes(void)
{
	for (int i = 0; i < EPISODE_MAX; ++i)
	{
		char ep_file[20];
		snprintf(ep_file, sizeof(ep_file), "tyrian%d.lvl", i + 1);
		episodeAvail[i] = dir_file_exists(data_dir(), ep_file);
	}
}

unsigned int JE_findNextEpisode(void)
{
	unsigned int newEpisode = episodeNum;
	
	jumpBackToEpisode1 = false;
	
	while (true)
	{
		newEpisode++;
		
		if (newEpisode > EPISODE_MAX)
		{
			newEpisode = 1;
			jumpBackToEpisode1 = true;
			gameHasRepeated = true;
		}
		
		if (episodeAvail[newEpisode-1] || newEpisode == episodeNum)
		{
			break;
		}
	}
	
	return newEpisode;
}
