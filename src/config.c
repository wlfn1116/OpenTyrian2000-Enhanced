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
#include "config.h"

#include "crashlog.h"
#include "custom_weapon.h"
#include "episodes.h"
#include "file.h"
#include "joystick.h"
#include "keyboard.h"
#include "loudness.h"
#include "mtrand.h"
#include "nortsong.h"
#include "opentyr.h"
#include "player.h"
#include "varz.h"
#include "vga256d.h"
#include "video.h"
#include "video_scale.h"

#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>

#ifdef _MSC_VER
#include <direct.h>
#define mkdir _mkdir
#else
#include <unistd.h>
#endif

/* Configuration Load/Save handler */

const JE_byte cryptKey[10] = /* [1..10] */
{
	15, 50, 89, 240, 147, 34, 86, 9, 32, 208
};

const DosKeySettings defaultDosKeySettings =
{
	72, 80, 75, 77, 57, 28, 29, 56
};

const MouseSettings defaultMouseSettings =
{
	1, 4, 5
};

const KeySettings defaultKeySettings =
{
	SDL_SCANCODE_UP,
	SDL_SCANCODE_DOWN,
	SDL_SCANCODE_LEFT,
	SDL_SCANCODE_RIGHT,
	SDL_SCANCODE_SPACE,
	SDL_SCANCODE_RETURN,
	SDL_SCANCODE_LCTRL,
	SDL_SCANCODE_LALT,
};

static const char *const keySettingNames[] =
{
	"up",
	"down",
	"left",
	"right",
	"fire",
	"change fire",
	"left sidekick",
	"right sidekick",
};

static const char *const mouseSettingNames[] =
{
	"left mouse",
	"right mouse",
	"middle mouse",
};

static const char *const mouseSettingValues[] =
{
	"fire main weapon",
	"fire left sidekick",
	"fire right sidekick",
	"fire both sidekicks",
	"change rear mode",
};

char defaultHighScoreNames[39][23]; /* [1..39] of string [22] */
char defaultTeamNames[10][25]; /* [1..22] of string [24] */

const JE_EditorItemAvailType initialItemAvail =
{
	1,1,1,0,0,1,1,0,1,1,1,1,1,0,1,0,1,1,1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0, /* Front/Rear Weapons 1-38  */
	0,0,0,0,0,0,0,0,0,0,1,                                                           /* Fill                     */
	1,0,0,0,0,1,0,0,0,1,1,0,1,0,0,0,0,0,                                             /* Sidekicks          51-68 */
	0,0,0,0,0,0,0,0,0,0,0,                                                           /* Fill                     */
	1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,                                                   /* Special Weapons    81-93 */
	0,0,0,0,0                                                                        /* Fill                     */
};

/* Last 2 bytes = Word
 *
 * Max Value = 1680
 * X div  60 = Armor  (1-28)
 * X div 168 = Shield (1-12)
 * X div 280 = Engine (1-06)
 */

JE_boolean smoothies[9] = /* [1..9] */
{ 0, 0, 0, 0, 0, 0, 0, 0, 0 };

JE_byte starShowVGASpecialCode;

/* CubeData */
JE_word lastCubeMax, cubeMax;
JE_word cubeList[4]; /* [1..4] */

/* High-Score Stuff */
JE_boolean gameHasRepeated;  // can only get highscore on first play-through

/* Difficulty */
JE_shortint difficultyLevel, oldDifficultyLevel,
            initialDifficulty;  // can only get highscore on initial episode

/* Timed Battle */
JE_byte timeBattleSelection;

/* Player Stuff */
uint    power, lastPower, powerAdd;
JE_byte shieldWait, shieldT;

JE_byte          shotRepeat[11], shotMultiPos[11];
JE_boolean       portConfigChange, portConfigDone;

/* Level Data */
char    lastLevelName[11], levelName[11]; /* string [10] */
JE_byte mainLevel, nextLevel, saveLevel;   /*Current Level #*/

/* Keyboard Junk */
DosKeySettings dosKeySettings;
KeySettings keySettings;

/* Mouse settings */
MouseSettings mouseSettings;

/* Configuration */
JE_shortint levelFilter, levelFilterNew, levelBrightness, levelBrightnessChg;
JE_boolean  filtrationAvail, filterActive, filterFade, filterFadeStart;

JE_boolean gameJustLoaded;

JE_boolean galagaMode;

JE_boolean extraGame;

JE_boolean engageMode;

JE_boolean twoPlayerMode, twoPlayerLinked, onePlayerAction, timedBattleMode, superTyrian;
JE_boolean endlessMode;  // Endless roguelite mode (see endless.c)
JE_boolean trentWin = false;
JE_byte    superArcadeMode;

JE_byte    superArcadePowerUp;

JE_real linkGunDirec;
JE_byte inputDevice[2] = { 1, 2 }; // 0:any  1:keyboard  2:mouse  3+:joystick

JE_byte secretHint;
JE_byte background3over;
JE_byte background2over;
JE_byte gammaCorrection;
JE_boolean superPause = false;
JE_boolean explosionTransparent,
           youAreCheating,
           displayScore,
           background2, smoothScroll, wild, superWild, starActive,
           topEnemyOver,
           skyEnemyOverAll,
           background2notTransparent;

JE_byte soundEffects; // dummy value for config
JE_byte versionNum;   /* SW 1.0 and SW/Reg 1.1 = 0 or 1
                       * EA 1.2 = 2        T2K = 3*/

JE_byte    fastPlay;
JE_boolean pentiumMode;

/* Savegame files */
JE_byte    gameSpeed;
JE_byte    processorType;  /* detail level: 1=Low 2=Normal 3=High 4=Pentium 5=Laptop VGA 6=Wild */

JE_SaveFilesType saveFiles; /*array[1..saveLevelnum] of savefiletype;*/
JE_SaveGameTemp saveTemp;

T2KHighScoreType t2kHighScores[20][3];

JE_word editorLevel;   /*Initial value 800*/

/* Enhancement settings (persisted in the [enhancements] config section). */
int bossBarStyle   = BOSS_BAR_ENHANCED;
int bossBarLayout  = BOSS_BAR_RIGHT;
int bossBarTwoMode = BOSS_BAR_TWO_STACKED;
/* When off: debug menu and debug level select hidden; buy/sell and pause menus
   keep their stock layout. */
bool debugMode     = true;
/* Wider horizontal background parallax: a strafe sweeps all three layers across their full map
   width (revealing the ~1 tile normally hidden off the left) instead of the narrow stock sway.
   Where the mid/deep layers over-pan past their map's side edge, the edge continues as a
   horizontally-flipped mirror of itself (backgrnd.c bg_mirror_setup/bg_mirror_tile) instead of
   ending in a content seam. Off restores the exact original amplitude and draw. Read as the
   parallax_span selector in mainint.c and the mirror gate in backgrnd.c. Off by default. */
bool extraParallax = false;
/* Where a background layer's read window slides past its map row's side edge, continue it as
   a horizontally-flipped mirror image (backgrnd.c bg_mirror_setup/bg_mirror_tile). Works in
   both parallax modes: Extra Parallax over-pans the mid/deep layers far past their edges, and
   even the stock span uncovers ~12px of layer 3's left edge at far-left. Off = the original
   draw: the uncovered span shows adjacent-row tiles (the visible content seam), with the
   pointer clamped in bounds only under Extra Parallax (stock keeps its harmless edge read). */
bool mirroredLayers = true;
/* Thin health bar near an enemy once damaged (draw_enemy_health_bars in tyrian2.c). */
bool enemyBars       = true;
int enemyBarLayout   = ENEMY_BAR_HORIZONTAL;
int enemyBarPosition = ENEMY_BAR_POS_BOTTOM;
int enemyBarOpacity  = 75;
/* Interpolate motion between the fixed ~35Hz logic ticks for high-refresh play;
   off = classic one present per tick. Gates vt_ship_owns() and the JE_starShowVGA
   interpolation loop (tyrian2.c). */
bool smoothMotion  = true;

void set_smooth_motion(bool enabled)
{
	// Only on a real off->on transition (not the config-load self-call), so a
	// deliberately saved Sub-pixel choice survives a restart.
	if (enabled && !smoothMotion)
		render_supersample = 0;  // re-arm Auto supersampling
	smoothMotion = enabled;
	if (!smoothMotion)
		render_supersample = 1;
}

/* Bigger explosion "superspark" ring buffer (MAX_SUPERPIXELS) vs the classic 101-spark cap;
   off = the original sparser DOS spark showers. Read by JE_doSP (varz.c). */
bool extraSparks = true;
/* Where each superspark weapon leaves its ep4/5 projectile trail (JE_applySuperSparks,
   episodes.c): SUPER_SPARKS_AUTO (vanilla per-episode), _ON (every episode), _OFF (no episode).
   Default On so the trails show in ep1-3 as well (matches the original Mega Pulse request). */
int superSparkMode[SSW_COUNT] = { SUPER_SPARKS_ON, SUPER_SPARKS_ON, SUPER_SPARKS_ON, SUPER_SPARKS_ON };
/* Cap a weapon's projectile trail at the classic 101-spark limit even when extraSparks is
   on, so the trail keeps its classic density (JE_doSP calls in shots.c). On by default. */
bool superSparkClassicCap[SSW_COUNT] = { true, true, true, true };
/* The ep4/5 Wallop Beam's second bolt (JE_applySuperSparks, episodes.c). Unlike the spark
   trails this changes firepower (two bolts per volley), so it defaults to Auto = each
   episode's shipped pattern rather than On. */
int wallopSecondBolt = SUPER_SPARKS_AUTO;

/* Config keys for the per-weapon trail settings; indexed by SuperSparkWeapon. */
static const char *const superSparkKeys[SSW_COUNT]    = { "superspark_mega_pulse", "superspark_wallop_beam", "superspark_protron_b", "superspark_ice" };
static const char *const superSparkCapKeys[SSW_COUNT] = { "superspark_mega_pulse_cap", "superspark_wallop_beam_cap", "superspark_protron_b_cap", "superspark_ice_cap" };

/* Which episode's data each non-spark difference weapon uses (JE_applyEpDiffs, episodes.c):
   EPDIFF_AUTO (per-episode, vanilla), _EP13, or _EP45. Default Auto so vanilla is unchanged. */
int epDiffMode[EDW_COUNT] = { EPDIFF_AUTO, EPDIFF_AUTO, EPDIFF_AUTO, EPDIFF_AUTO, EPDIFF_AUTO, EPDIFF_AUTO, EPDIFF_AUTO, EPDIFF_AUTO };
/* Config keys for the per-weapon episode-difference settings; indexed by EpDiffWeapon. */
static const char *const epDiffKeys[EDW_COUNT] = {
	"epdiff_xega_ball", "epdiff_microsol_opt5", "epdiff_flare", "epdiff_needle_laser",
	"epdiff_bubble_gum", "epdiff_flying_punch", "epdiff_pretzel_missile", "epdiff_dragon_frost"
};

/* Map a trail-tagged shot's sprite back to its weapon's cap setting, for the JE_doSP calls
   in shots.c (a flying shot only knows its graphic). Unknown sprites (e.g. sparky custom
   weapon bullets) honor extraSparks uncapped. */
bool superSparkCapForSprite(JE_word sprite)
{
	switch (sprite)
	{
	case 35:           return superSparkClassicCap[SSW_MEGA_PULSE];
	case 30: case 29:  return superSparkClassicCap[SSW_WALLOP_BEAM];
	case 28:           return superSparkClassicCap[SSW_PROTRON_B];
	case 634:          return superSparkClassicCap[SSW_ICE];
	default:           return false;
	}
}
/* HUD gauge gradient direction (Enhancements -> Gauge Gradients). GAUGE_GRAD_UP
   reproduces the classic vertical gauges; other values run the gradient down the column or
   across its width. Read by draw_power_gauge (tyrian2.c) and JE_dBar3 (nortvars.c). */
int gaugeGradGenerator = GAUGE_GRAD_UP;
int gaugeGradShield    = GAUGE_GRAD_RIGHT;
int gaugeGradArmor     = GAUGE_GRAD_LEFT;
bool gaugeFlashShield  = true;
bool gaugeFlashArmor   = true;
/* Zica Laser Lv11 tweaks (JE_applyZicaLaserConfig in episodes.c; front-weapon fire
   loop in mainint.c). */
int zicaLaserBase = ZICA_BASE_EP4;      /* ZICA_BASE_*: Lv11 shot pattern */
int zicaLaserLength = ZICA_LEN_SHORT;   /* ZICA_LEN_* : Lv11 shot length */
bool zicaLaserLock = false;             /* Length=Long: ship-lock the side beams (default = free) */
bool zicaLaserBuff = true;              /* also fire the Lv10 beam alongside the Lv11 shots */
/* Re-add the cut DOS "Charge-Laser Cannon" sidekick to its original shops + the debug
   menu (JE_addChargeLaserCannon in episodes.c). */
bool chargeLaserCannon = true;
/* Christmas mode override: -1 = auto-detect by date (original), 0 = force off, 1 = force
   on. Set to 0/1 by the Enhancements toggle so the choice persists. */
int xmasMode = 0;

Config opentyrian_config;  // implicitly initialized

// The custom weapon is persisted as a raw JE_WeaponType per power level (a compact
// comma-separated integer blob built by customWeaponSerializeLevel), plus the shared
// weapon-wide identity keys. See custom_weapon.c for the blob layout.

bool load_opentyrian_config(void)
{
	// defaults
	fullscreen_display = -1;
#ifdef __vita__
	// Vita's SGX GPU + A9 CPU can't afford a software-upscaled present every frame; present at
	// native size and let the GPU scale to the 960x544 panel. notes.md §Console ports.
	set_scaler_by_name("None");
#else
	set_scaler_by_name("4x");  // first-boot default: plain nearest-neighbour 4x (crisp pixels)
#endif
	memcpy(keySettings, defaultKeySettings, sizeof(keySettings));
	memcpy(mouseSettings, defaultMouseSettings, sizeof(mouseSettings));
	
	Config *config = &opentyrian_config;
	
	FILE *file = dir_fopen_warn(get_user_directory(), "opentyrian.cfg", "r");
	if (file == NULL)
		return false;

	if (!config_parse(config, file))
	{
		fclose(file);
		
		return false;
	}
	
	ConfigSection *section;
	
	section = config_find_section(config, "video", NULL);
	if (section != NULL)
	{
		config_get_int_option(section, "fullscreen", &fullscreen_display);

		const char* scaler_name;
		if (config_get_string_option(section, "scaler", &scaler_name))
			set_scaler_by_name(scaler_name);
#ifdef __vita__
		// Ignore any saved scaler on Vita: software upscaling every frame is too slow for the
		// hardware regardless of what a prior session wrote. Native + GPU upscale only. The
		// in-session Graphics menu can still change it to experiment, but each boot resets here.
		set_scaler_by_name("None");
#endif

		const char* scaling_mode;
		if (config_get_string_option(section, "scaling_mode", &scaling_mode))
			set_scaling_mode_by_name(scaling_mode);

		config_get_int_option(section, "fps", &fps_cap);
		set_fps(fps_cap);

		int vsync_enabled = output_vsync ? 1 : 0;
		config_get_int_option(section, "vsync", &vsync_enabled);
		set_vsync(vsync_enabled != 0);

		int show_fps_enabled = show_fps ? 1 : 0;
		config_get_int_option(section, "show_fps", &show_fps_enabled);
		show_fps = (show_fps_enabled != 0);

#if defined(__SWITCH__) || defined(__vita__)
		// Touchscreen ship-control sensitivity slider (Setup > Sound and the pause menu).
		config_get_int_option(section, "touch_sensitivity", &touch_sensitivity);
		if (touch_sensitivity < 0 || touch_sensitivity > TOUCH_SENS_MAX)
			touch_sensitivity = TOUCH_SENS_DEFAULT;
#endif

		// Sub-pixel supersampling: 0 = Auto (follow the scaler), 1 = off, 2..8 fixed.
		config_get_int_option(section, "render_supersample", &render_supersample);
		if (render_supersample < 0 || render_supersample > RENDER_SUPERSAMPLE_MAX)
			render_supersample = 0;

		// Sub-pixel filter: 0 = Sharp (crisp pixels), 1 = Smooth (antialiased edges),
		// 2 = None (raw, unfiltered nearest at every ratio).
		config_get_int_option(section, "render_supersample_filter", &render_supersample_filter);
		if (render_supersample_filter < SS_FILTER_SHARP || render_supersample_filter > SS_FILTER_NONE)
			render_supersample_filter = SS_FILTER_NONE;

	}

	section = config_find_section(config, "keyboard", NULL);
	if (section != NULL)
	{
		for (size_t i = 0; i < COUNTOF(keySettings); ++i)
		{
			const char *keyName;
			if (config_get_string_option(section, keySettingNames[i], &keyName))
			{
				SDL_Scancode scancode = SDL_GetScancodeFromName(keyName);
				if (scancode != SDL_SCANCODE_UNKNOWN)
					keySettings[i] = scancode;
			}
		}
	}

	section = config_find_section(config, "mouse", NULL);
	if (section != NULL)
	{
		for (size_t i = 0; i < COUNTOF(mouseSettings); ++i)
		{
			const char *mouseValue;
			if (config_get_string_option(section, mouseSettingNames[i], &mouseValue))
			{
				for (size_t val = 1; val <= COUNTOF(mouseSettingValues); ++val)
				{
					if (strcmp(mouseValue, mouseSettingValues[val - 1]))
						continue;

					mouseSettings[i] = val;
					break;
				}
			}
		}
	}

	section = config_find_section(config, "enhancements", NULL);
	if (section != NULL)
	{
		config_get_int_option(section, "boss_bar_style", &bossBarStyle);
		config_get_int_option(section, "boss_bar_layout", &bossBarLayout);
		config_get_int_option(section, "boss_bar_two_mode", &bossBarTwoMode);

		int debug_mode_enabled = debugMode ? 1 : 0;
		config_get_int_option(section, "debug_mode", &debug_mode_enabled);
		debugMode = (debug_mode_enabled != 0);

		int hang_timeout = crashlog_get_hang_timeout();
		config_get_int_option(section, "hang_timeout", &hang_timeout);
		crashlog_set_hang_timeout(hang_timeout);  // clamps into range

		int enemy_bars_enabled = enemyBars ? 1 : 0;
		config_get_int_option(section, "enemy_bars", &enemy_bars_enabled);
		enemyBars = (enemy_bars_enabled != 0);

		config_get_int_option(section, "enemy_bar_layout", &enemyBarLayout);
		config_get_int_option(section, "enemy_bar_position", &enemyBarPosition);
		config_get_int_option(section, "enemy_bar_opacity", &enemyBarOpacity);

		int smooth_motion_enabled = smoothMotion ? 1 : 0;
		config_get_int_option(section, "smooth_motion", &smooth_motion_enabled);
		smoothMotion = (smooth_motion_enabled != 0);

		int extra_sparks_enabled = extraSparks ? 1 : 0;
		config_get_int_option(section, "extra_sparks", &extra_sparks_enabled);
		extraSparks = (extra_sparks_enabled != 0);

		int extra_parallax_enabled = extraParallax ? 1 : 0;
		config_get_int_option(section, "extra_parallax", &extra_parallax_enabled);
		extraParallax = (extra_parallax_enabled != 0);

		int mirrored_layers_enabled = mirroredLayers ? 1 : 0;
		config_get_int_option(section, "mirrored_layers", &mirrored_layers_enabled);
		mirroredLayers = (mirrored_layers_enabled != 0);

		// Music device (OPL3 / FluidSynth / Native MIDI) + SoundFont path. The
		// MIDI devices only take effect in a WITH_MIDI build; otherwise init_audio()
		// falls back to OPL (see loudness.c).
		const char *music_device_name;
		if (config_get_string_option(section, "music_device", &music_device_name))
		{
			for (int i = 0; i < MUSIC_DEVICE_MAX; ++i)
			{
				if (strcmp(music_device_name, music_device_names[i]) == 0)
				{
					music_device = (MusicDevice)i;
					break;
				}
			}
		}

		const char *soundfont_name;
		if (config_get_string_option(section, "soundfont", &soundfont_name))
			SDL_strlcpy(soundfont, soundfont_name, sizeof(soundfont));

		// Legacy keys from when only the Mega Pulse had these settings; the new per-weapon
		// keys below override them when present.
		config_get_int_option(section, "mega_pulse_sparks", &superSparkMode[SSW_MEGA_PULSE]);
		int legacy_cap = superSparkClassicCap[SSW_MEGA_PULSE] ? 1 : 0;
		config_get_int_option(section, "mega_pulse_classic_cap", &legacy_cap);
		superSparkClassicCap[SSW_MEGA_PULSE] = (legacy_cap != 0);

		for (int i = 0; i < SSW_COUNT; ++i)
		{
			config_get_int_option(section, superSparkKeys[i], &superSparkMode[i]);
			if (superSparkMode[i] < 0 || superSparkMode[i] >= SUPER_SPARKS_COUNT)
				superSparkMode[i] = SUPER_SPARKS_ON;

			int spark_cap = superSparkClassicCap[i] ? 1 : 0;
			config_get_int_option(section, superSparkCapKeys[i], &spark_cap);
			superSparkClassicCap[i] = (spark_cap != 0);
		}

		config_get_int_option(section, "superspark_wallop_second_bolt", &wallopSecondBolt);
		if (wallopSecondBolt < 0 || wallopSecondBolt >= SUPER_SPARKS_COUNT)
			wallopSecondBolt = SUPER_SPARKS_AUTO;

		for (int i = 0; i < EDW_COUNT; ++i)
		{
			config_get_int_option(section, epDiffKeys[i], &epDiffMode[i]);
			if (epDiffMode[i] < 0 || epDiffMode[i] >= EPDIFF_MODE_COUNT)
				epDiffMode[i] = EPDIFF_AUTO;
		}

		config_get_int_option(section, "gauge_grad_generator", &gaugeGradGenerator);
		config_get_int_option(section, "gauge_grad_shield", &gaugeGradShield);
		config_get_int_option(section, "gauge_grad_armor", &gaugeGradArmor);

		int gauge_flash_shield_enabled = gaugeFlashShield ? 1 : 0;
		config_get_int_option(section, "gauge_flash_shield", &gauge_flash_shield_enabled);
		gaugeFlashShield = (gauge_flash_shield_enabled != 0);

		int gauge_flash_armor_enabled = gaugeFlashArmor ? 1 : 0;
		config_get_int_option(section, "gauge_flash_armor", &gauge_flash_armor_enabled);
		gaugeFlashArmor = (gauge_flash_armor_enabled != 0);

		config_get_int_option(section, "zica_l11_base", &zicaLaserBase);
		if (zicaLaserBase < 0 || zicaLaserBase >= ZICA_BASE_COUNT)
			zicaLaserBase = ZICA_BASE_EP4;

		config_get_int_option(section, "zica_l11_length", &zicaLaserLength);
		if (zicaLaserLength < 0 || zicaLaserLength >= ZICA_LEN_COUNT)
			zicaLaserLength = ZICA_LEN_SHORT;

		int zica_l11_lock = zicaLaserLock ? 1 : 0;
		config_get_int_option(section, "zica_l11_lock", &zica_l11_lock);
		zicaLaserLock = (zica_l11_lock != 0);

		// Back-compat: earlier builds saved this as 0/1 (bool) or 0-4 (mode); any non-zero = on.
		int zica_laser_buff = zicaLaserBuff ? 1 : 0;
		config_get_int_option(section, "zica_laser_buff", &zica_laser_buff);
		zicaLaserBuff = (zica_laser_buff != 0);

		int charge_laser_cannon = chargeLaserCannon ? 1 : 0;
		config_get_int_option(section, "charge_laser_cannon", &charge_laser_cannon);
		chargeLaserCannon = (charge_laser_cannon != 0);

		config_get_int_option(section, "xmas", &xmasMode);
		if (xmasMode < -1 || xmasMode > 1)
			xmasMode = 0;

		// Clamp to valid ranges in case of a hand-edited or stale config.
		if (bossBarStyle < BOSS_BAR_CLASSIC || bossBarStyle > BOSS_BAR_ENHANCED)
			bossBarStyle = BOSS_BAR_ENHANCED;
		if (bossBarLayout < BOSS_BAR_TOP || bossBarLayout > BOSS_BAR_RIGHT)
			bossBarLayout = BOSS_BAR_RIGHT;
		if (bossBarTwoMode < BOSS_BAR_TWO_TOGETHER || bossBarTwoMode > BOSS_BAR_TWO_STACKED)
			bossBarTwoMode = BOSS_BAR_TWO_STACKED;
		if (enemyBarLayout < ENEMY_BAR_HORIZONTAL || enemyBarLayout > ENEMY_BAR_VERTICAL)
			enemyBarLayout = ENEMY_BAR_HORIZONTAL;
		if (enemyBarPosition < ENEMY_BAR_POS_BOTTOM || enemyBarPosition > ENEMY_BAR_POS_CENTER)
			enemyBarPosition = ENEMY_BAR_POS_BOTTOM;
		if (enemyBarOpacity < 0)
			enemyBarOpacity = 0;
		else if (enemyBarOpacity > 100)
			enemyBarOpacity = 100;
		if (gaugeGradGenerator < 0 || gaugeGradGenerator >= GAUGE_GRAD_COUNT)
			gaugeGradGenerator = GAUGE_GRAD_UP;
		if (gaugeGradShield < 0 || gaugeGradShield >= GAUGE_GRAD_COUNT)
			gaugeGradShield = GAUGE_GRAD_RIGHT;
		if (gaugeGradArmor < 0 || gaugeGradArmor >= GAUGE_GRAD_COUNT)
			gaugeGradArmor = GAUGE_GRAD_LEFT;

		for (int i = 0; i < expertSettingsCount; ++i)
			config_get_int_option(section, expertSettings[i].cfgKey, expertSettings[i].value);
		clamp_expert_settings();  // guard against a hand-edited or stale config

		// Custom Weapon Creator: master toggle + the saved per-power-level raw designs.
		// Each level is a compact comma-separated integer blob (see custom_weapon.c);
		// customWeaponInit() fills in a default design when none is present, and clamps.
		int custom_weapon_enabled = customWeaponEnabled ? 1 : 0;
		config_get_int_option(section, "custom_weapon_enabled", &custom_weapon_enabled);
		customWeaponEnabled = (custom_weapon_enabled != 0);

		// Weapon-wide identity (maps to the single port; shared across all levels).
		const char *custom_weapon_name;
		if (config_get_string_option(section, "custom_weapon_name", &custom_weapon_name))
		{
			strncpy(customWeaponName, custom_weapon_name, sizeof(customWeaponName) - 1);
			customWeaponName[sizeof(customWeaponName) - 1] = '\0';
		}
		config_get_int_option(section, "custom_weapon_cost",          &customWeaponCost);
		config_get_int_option(section, "custom_weapon_power_use",     &customWeaponPowerUse);
		config_get_int_option(section, "custom_weapon_equip_slot",    &customWeaponEquipSlot);
		config_get_int_option(section, "custom_weapon_item_graphic",  &customWeaponItemGraphic);
		config_get_int_option(section, "custom_weapon_charge_stages", &customWeaponChargeStages);
		config_get_int_option(section, "custom_weapon_modes",         &customWeaponModes);
		config_get_int_option(section, "custom_weapon_sk_mount",      &customSidekickMount);
		config_get_int_option(section, "custom_weapon_sk_sprite",     &customSidekickSprite);
		config_get_int_option(section, "custom_weapon_sk_frames",     &customSidekickFrames);
		config_get_int_option(section, "custom_weapon_sk_frame_step", &customSidekickFrameStep);
		config_get_int_option(section, "custom_weapon_sk_animate",    &customSidekickAnimate);

		// Each (mode, power level)'s raw weapon: custom_weapon_m<M>_l<N>_raw. Mode 0 also
		// accepts the pre-modes key custom_weapon_l<N>_raw so an older config migrates.
		for (int m = 0; m < CUSTOM_WEAPON_MODES; ++m)
			for (int p = 0; p < CUSTOM_POWER_LEVELS; ++p)
			{
				char key[64];
				const char *blob;
				if (m == 0)
				{
					snprintf(key, sizeof(key), "custom_weapon_l%d_raw", p + 1);
					if (config_get_string_option(section, key, &blob))
						customWeaponDeserializeLevel(0, p, blob);
				}
				snprintf(key, sizeof(key), "custom_weapon_m%d_l%d_raw", m + 1, p + 1);
				if (config_get_string_option(section, key, &blob))
					customWeaponDeserializeLevel(m, p, blob);
			}

		customWeaponEditLevel = 0;
		customWeaponEditMode = 0;
	}

	// Smooth Motion owns the sub-pixel render path. Keep it disabled when motion
	// interpolation is off, then apply the scaler constraint to the final state.
	set_smooth_motion(smoothMotion);
	if (render_supersample != 1)
		scaler = scaler_plain_equivalent(scaler);

	fclose(file);

	return true;
}

bool save_opentyrian_config(void)
{
	Config *config = &opentyrian_config;
	
	ConfigSection *section;
	
	section = config_find_or_add_section(config, "video", NULL);
	if (section == NULL)
		exit(EXIT_FAILURE);  // out of memory
	
	config_set_int_option(section, "fullscreen", fullscreen_display);
	
	config_set_string_option(section, "scaler", scalers[scaler].name);
	
	config_set_string_option(section, "scaling_mode", scaling_mode_names[scaling_mode]);

	config_set_int_option(section, "fps", fps_cap);

	config_set_int_option(section, "vsync", output_vsync ? 1 : 0);

	config_set_int_option(section, "show_fps", show_fps ? 1 : 0);

#if defined(__SWITCH__) || defined(__vita__)
	config_set_int_option(section, "touch_sensitivity", touch_sensitivity);
#endif

	config_set_int_option(section, "render_supersample", render_supersample);

	config_set_int_option(section, "render_supersample_filter", render_supersample_filter);

	section = config_find_or_add_section(config, "keyboard", NULL);
	if (section == NULL)
		exit(EXIT_FAILURE);  // out of memory

	for (size_t i = 0; i < COUNTOF(keySettings); ++i)
	{
		const char *keyName = SDL_GetScancodeName(keySettings[i]);
		if (keyName[0] == '\0')
			keyName = NULL;
		config_set_string_option(section, keySettingNames[i], keyName);
	}

#ifndef TARGET_WIN32
	mkdir(get_user_directory(), 0700);
#else
	mkdir(get_user_directory());
#endif

	// Tyrian 2000 doesn't save mouse settings, so we do it ourselves
	section = config_find_or_add_section(config, "mouse", NULL);
	if (section == NULL)
		exit(EXIT_FAILURE);  // out of memory
	
	for (size_t i = 0; i < COUNTOF(mouseSettings); ++i)
		config_set_string_option(section, mouseSettingNames[i], mouseSettingValues[mouseSettings[i] - 1]);

	section = config_find_or_add_section(config, "enhancements", NULL);
	if (section == NULL)
		exit(EXIT_FAILURE);  // out of memory

	config_set_int_option(section, "boss_bar_style", bossBarStyle);
	config_set_int_option(section, "boss_bar_layout", bossBarLayout);
	config_set_int_option(section, "boss_bar_two_mode", bossBarTwoMode);
	config_set_int_option(section, "debug_mode", debugMode ? 1 : 0);
	config_set_int_option(section, "hang_timeout", crashlog_get_hang_timeout());
	config_set_int_option(section, "enemy_bars", enemyBars ? 1 : 0);
	config_set_int_option(section, "enemy_bar_layout", enemyBarLayout);
	config_set_int_option(section, "enemy_bar_position", enemyBarPosition);
	config_set_int_option(section, "enemy_bar_opacity", enemyBarOpacity);
	config_set_int_option(section, "smooth_motion", smoothMotion ? 1 : 0);
	config_set_int_option(section, "extra_sparks", extraSparks ? 1 : 0);
	config_set_int_option(section, "extra_parallax", extraParallax ? 1 : 0);
	config_set_int_option(section, "mirrored_layers", mirroredLayers ? 1 : 0);
	config_set_string_option(section, "music_device", music_device_names[music_device]);
	config_set_string_option(section, "soundfont", soundfont);
	for (int i = 0; i < SSW_COUNT; ++i)
	{
		config_set_int_option(section, superSparkKeys[i], superSparkMode[i]);
		config_set_int_option(section, superSparkCapKeys[i], superSparkClassicCap[i] ? 1 : 0);
	}
	config_set_int_option(section, "superspark_wallop_second_bolt", wallopSecondBolt);
	for (int i = 0; i < EDW_COUNT; ++i)
		config_set_int_option(section, epDiffKeys[i], epDiffMode[i]);
	config_set_int_option(section, "gauge_grad_generator", gaugeGradGenerator);
	config_set_int_option(section, "gauge_grad_shield", gaugeGradShield);
	config_set_int_option(section, "gauge_grad_armor", gaugeGradArmor);
	config_set_int_option(section, "gauge_flash_shield", gaugeFlashShield ? 1 : 0);
	config_set_int_option(section, "gauge_flash_armor", gaugeFlashArmor ? 1 : 0);
	config_set_int_option(section, "zica_l11_base", zicaLaserBase);
	config_set_int_option(section, "zica_l11_length", zicaLaserLength);
	config_set_int_option(section, "zica_l11_lock", zicaLaserLock ? 1 : 0);
	config_set_int_option(section, "zica_laser_buff", zicaLaserBuff ? 1 : 0);
	config_set_int_option(section, "charge_laser_cannon", chargeLaserCannon ? 1 : 0);
	config_set_int_option(section, "xmas", xmasMode);

	config_set_int_option(section, "custom_weapon_enabled", customWeaponEnabled ? 1 : 0);
	// Weapon-wide identity (maps to the single port; shared across all levels).
	config_set_string_option(section, "custom_weapon_name",    customWeaponName);
	config_set_int_option(section, "custom_weapon_cost",          customWeaponCost);
	config_set_int_option(section, "custom_weapon_power_use",     customWeaponPowerUse);
	config_set_int_option(section, "custom_weapon_equip_slot",    customWeaponEquipSlot);
	config_set_int_option(section, "custom_weapon_item_graphic",  customWeaponItemGraphic);
	config_set_int_option(section, "custom_weapon_charge_stages", customWeaponChargeStages);
	config_set_int_option(section, "custom_weapon_modes",         customWeaponModes);
	config_set_int_option(section, "custom_weapon_sk_mount",      customSidekickMount);
	config_set_int_option(section, "custom_weapon_sk_sprite",     customSidekickSprite);
	config_set_int_option(section, "custom_weapon_sk_frames",     customSidekickFrames);
	config_set_int_option(section, "custom_weapon_sk_frame_step", customSidekickFrameStep);
	config_set_int_option(section, "custom_weapon_sk_animate",    customSidekickAnimate);
	// Each (mode, power level)'s raw weapon, as a compact comma-separated integer blob.
	for (int m = 0; m < CUSTOM_WEAPON_MODES; ++m)
		for (int p = 0; p < CUSTOM_POWER_LEVELS; ++p)
		{
			char key[64], blob[16384];   // one raw-weapon blob; sized for the widest (255-bullet) design
			snprintf(key, sizeof(key), "custom_weapon_m%d_l%d_raw", m + 1, p + 1);
			customWeaponSerializeLevel(m, p, blob, sizeof(blob));
			config_set_string_option(section, key, blob);
		}

	for (int i = 0; i < expertSettingsCount; ++i)
		config_set_int_option(section, expertSettings[i].cfgKey, *expertSettings[i].value);

	FILE *file = dir_fopen(get_user_directory(), "opentyrian.cfg", "w");
	if (file == NULL)
		return false;

	config_write(config, file);
	
#if _POSIX_C_SOURCE >= 1 || _XOPEN_SOURCE || _POSIX_SOURCE
	fsync(fileno(file));
#endif
	fclose(file);
	
	return true;
}

static void playeritems_to_pitems(JE_PItemsType pItems, PlayerItems *items, JE_byte initial_episode_num)
{
	pItems[0]  = items->weapon[FRONT_WEAPON].id;
	pItems[1]  = items->weapon[REAR_WEAPON].id;
	pItems[2]  = items->super_arcade_mode;
	pItems[3]  = items->sidekick[LEFT_SIDEKICK];
	pItems[4]  = items->sidekick[RIGHT_SIDEKICK];
	pItems[5]  = items->generator;
	pItems[6]  = items->sidekick_level;
	pItems[7]  = items->sidekick_series;
	pItems[8]  = initial_episode_num;
	pItems[9]  = items->shield;
	pItems[10] = items->special;
	pItems[11] = items->ship;
}

static void pitems_to_playeritems(PlayerItems *items, JE_PItemsType pItems, JE_byte *initial_episode_num)
{
	items->weapon[FRONT_WEAPON].id  = pItems[0];
	items->weapon[REAR_WEAPON].id   = pItems[1];
	items->super_arcade_mode        = pItems[2];
	items->sidekick[LEFT_SIDEKICK]  = pItems[3];
	items->sidekick[RIGHT_SIDEKICK] = pItems[4];
	items->generator                = pItems[5];
	items->sidekick_level           = pItems[6];
	items->sidekick_series          = pItems[7];
	if (initial_episode_num != NULL)
		*initial_episode_num        = pItems[8];
	items->shield                   = pItems[9];
	items->special                  = pItems[10];
	items->ship                     = pItems[11];
}

void JE_saveGame(JE_byte slot, const char *name)
{
	saveFiles[slot-1].initialDifficulty = initialDifficulty;
	saveFiles[slot-1].gameHasRepeated = gameHasRepeated;
	saveFiles[slot-1].level = saveLevel;
	
	if (superTyrian)
		player[0].items.super_arcade_mode = SA_SUPERTYRIAN;
	else if (superArcadeMode == SA_NONE && onePlayerAction)
		player[0].items.super_arcade_mode = SA_ARCADE;
	else
		player[0].items.super_arcade_mode = superArcadeMode;
	
	playeritems_to_pitems(saveFiles[slot-1].items, &player[0].items, initial_episode_num);
	
	if (twoPlayerMode)
		playeritems_to_pitems(saveFiles[slot-1].lastItems, &player[1].items, 0);
	else
		playeritems_to_pitems(saveFiles[slot-1].lastItems, &player[0].last_items, 0);
	
	saveFiles[slot-1].score  = player[0].cash;
	saveFiles[slot-1].score2 = player[1].cash;
	
	memcpy(&saveFiles[slot-1].levelName, &lastLevelName, sizeof(lastLevelName));
	saveFiles[slot-1].cubes  = lastCubeMax;

	if (strcmp(lastLevelName, "Completed") == 0)
	{
		temp = episodeNum - 1;
		if (temp < 1)
		{
			temp = EPISODE_AVAILABLE; /* JE: {Episodemax is 4 for completion purposes} */
		}
		saveFiles[slot-1].episode = temp;
	}
	else
	{
		saveFiles[slot-1].episode = episodeNum;
	}

	saveFiles[slot-1].difficulty = difficultyLevel;
	saveFiles[slot-1].secretHint = secretHint;
	saveFiles[slot - 1].input1 = inputDevice[0];
	saveFiles[slot - 1].input2 = inputDevice[1];

	saveFiles[slot - 1].autoFireSpecial = autoFireSpecial;
	saveFiles[slot - 1].chargeSidekickAutofire = chargeSidekickAutofire;
	saveFiles[slot - 1].difficultyAdjust = difficultyAdjust;
	saveFiles[slot - 1].cheatInfiniteSidekickAmmo = cheatInfiniteSidekickAmmo;
	saveFiles[slot - 1].cheatInfiniteShields = cheatInfiniteShields;
	saveFiles[slot - 1].cheatInfiniteArmor = cheatInfiniteArmor;
	saveFiles[slot - 1].expertMode = expertMode;

	strcpy(saveFiles[slot-1].name, name);
	
	for (uint port = 0; port < 2; ++port)
	{
		// if two-player, use first player's front and second player's rear weapon
		saveFiles[slot-1].power[port] = player[twoPlayerMode ? port : 0].items.weapon[port].power;
	}
	
	JE_saveConfiguration();
}

void JE_loadGame(JE_byte slot)
{
	superTyrian = false;
	onePlayerAction = false;
	twoPlayerMode = false;
	extraGame = false;
	galagaMode = false;
	timedBattleMode = false;
	endlessMode = false;  // saves are never endless (high-scores-only mode); always load as a normal game

	initialDifficulty = saveFiles[slot-1].initialDifficulty;
	gameHasRepeated   = saveFiles[slot-1].gameHasRepeated;
	twoPlayerMode     = (slot-1) > 10;
	difficultyLevel   = saveFiles[slot-1].difficulty;
	
	pitems_to_playeritems(&player[0].items, saveFiles[slot-1].items, &initial_episode_num);
	
	superArcadeMode = player[0].items.super_arcade_mode;
	
	if (superArcadeMode == SA_SUPERTYRIAN)
		superTyrian = true;
	if (superArcadeMode != SA_NONE)
		onePlayerAction = true;
	if (superArcadeMode > SA_LASTSHIP)
		superArcadeMode = SA_NONE;
	
	if (twoPlayerMode)
	{
		onePlayerAction = false;
		
		pitems_to_playeritems(&player[1].items, saveFiles[slot-1].lastItems, NULL);
	}
	else
	{
		pitems_to_playeritems(&player[0].last_items, saveFiles[slot-1].lastItems, NULL);
	}

	/* Compatibility with old version */
	if (player[1].items.sidekick_level < 101)
	{
		player[1].items.sidekick_level = 101;
		player[1].items.sidekick_series = player[1].items.sidekick[LEFT_SIDEKICK];
	}
	
	player[0].cash = saveFiles[slot-1].score;
	player[1].cash = saveFiles[slot-1].score2;
	
	mainLevel   = saveFiles[slot-1].level;
	cubeMax     = saveFiles[slot-1].cubes;
	lastCubeMax = cubeMax;

	secretHint = saveFiles[slot - 1].secretHint;
	inputDevice[0] = saveFiles[slot - 1].input1;
	inputDevice[1] = saveFiles[slot - 1].input2;

	autoFireSpecial = saveFiles[slot - 1].autoFireSpecial;
	chargeSidekickAutofire = saveFiles[slot - 1].chargeSidekickAutofire;
	difficultyAdjust = saveFiles[slot - 1].difficultyAdjust;
	cheatInfiniteSidekickAmmo = saveFiles[slot - 1].cheatInfiniteSidekickAmmo;
	cheatInfiniteShields = saveFiles[slot - 1].cheatInfiniteShields;
	cheatInfiniteArmor = saveFiles[slot - 1].cheatInfiniteArmor;
	expertMode = saveFiles[slot - 1].expertMode;

	for (uint port = 0; port < 2; ++port)
	{
		// if two-player, use first player's front and second player's rear weapon
		player[twoPlayerMode ? port : 0].items.weapon[port].power = saveFiles[slot-1].power[port];
	}
	
	int episode = saveFiles[slot-1].episode;

	memcpy(&levelName, &saveFiles[slot-1].levelName, sizeof(levelName));

	if (strcmp(levelName, "Completed") == 0)
	{
		if (episode == EPISODE_AVAILABLE)
			episode = 1;
		else if (episode < EPISODE_AVAILABLE)
			episode++;
		/* Increment episode.  Episode EPISODE_AVAILABLE goes to 1. */
	}

	JE_initEpisode(episode);
	saveLevel = mainLevel;
	memcpy(&lastLevelName, &levelName, sizeof(levelName));
}

void JE_initProcessorType(void)
{
	/* SYN: Originally this proc looked at your hardware specs and chose appropriate options. We don't care, so I'll just set
	   decent defaults here. */

	wild = false;
	superWild = false;
	smoothScroll = true;
	explosionTransparent = true;
	filtrationAvail = false;
	background2 = true;
	displayScore = true;

	switch (processorType)
	{
		case 1: /* 386 */
			background2 = false;
			displayScore = false;
			explosionTransparent = false;
			break;
		case 2: /* 486 - Default */
			break;
		case 3: /* High Detail */
			smoothScroll = false;
			break;
		case 4: /* Pentium */
			wild = true;
			filtrationAvail = true;
			break;
		case 5: /* Nonstandard VGA */
			smoothScroll = false;
			break;
		case 6: /* SuperWild */
			wild = true;
			superWild = true;
			filtrationAvail = true;
			break;
	}

	switch (gameSpeed)
	{
		case 1:  /* Slug Mode */
			fastPlay = 3;
			break;
		case 2:  /* Slower */
			fastPlay = 4;
			break;
		case 3: /* Slow */
			fastPlay = 5;
			break;
		case 4: /* Normal */
			fastPlay = 0;
			break;
		case 5: /* Pentium Hyper */
			fastPlay = 1;
			break;
	}

}

void JE_setNewGameSpeed(void)
{
	pentiumMode = false;

	Uint16 speed;
	switch (fastPlay)
	{
	default:
		assert(false);
		// fall through
	case 0:  // Normal
		speed = 0x4300;
		smoothScroll = true;
		frameCountMax = 2;
		break;
	case 1:  // Pentium Hyper
		speed = 0x3000;
		smoothScroll = true;
		frameCountMax = 2;
		break;
	case 2:
		speed = 0x2000;
		smoothScroll = false;
		frameCountMax = 2;
		break;
	case 3:  // Slug mode
		speed = 0x5300;
		smoothScroll = true;
		frameCountMax = 4;
		break;
	case 4:  // Slower
		speed = 0x4300;
		smoothScroll = true;
		frameCountMax = 3;
		break;
	case 5:  // Slow
		speed = 0x4300;
		smoothScroll = true;
		frameCountMax = 2;
		pentiumMode = true;
		break;
	}

	setDelaySpeed(speed);
	setDelay(frameCountMax);
}

void JE_encryptSaveTemp(void)
{
	JE_SaveGameTemp s3;
	JE_word x;
	JE_byte y;

	memcpy(&s3, &saveTemp, sizeof(s3));

	y = 0;
	for (x = 0; x < SAVE_FILE_SIZE; x++)
	{
		y += s3[x];
	}
	saveTemp[SAVE_FILE_SIZE] = y;

	y = 0;
	for (x = 0; x < SAVE_FILE_SIZE; x++)
	{
		y -= s3[x];
	}
	saveTemp[SAVE_FILE_SIZE+1] = y;

	y = 1;
	for (x = 0; x < SAVE_FILE_SIZE; x++)
	{
		y = (y * s3[x]) + 1;
	}
	saveTemp[SAVE_FILE_SIZE+2] = y;

	y = 0;
	for (x = 0; x < SAVE_FILE_SIZE; x++)
	{
		y = y ^ s3[x];
	}
	saveTemp[SAVE_FILE_SIZE+3] = y;

	for (x = 0; x < SAVE_FILE_SIZE; x++)
	{
		saveTemp[x] = saveTemp[x] ^ cryptKey[(x+1) % 10];
		if (x > 0)
		{
			saveTemp[x] = saveTemp[x] ^ saveTemp[x - 1];
		}
	}
}

void JE_decryptSaveTemp(void)
{
	JE_boolean correct = true;
	JE_SaveGameTemp s2;
	int x;
	JE_byte y;

	/* Decrypt save game file */
	for (x = (SAVE_FILE_SIZE - 1); x >= 0; x--)
	{
		s2[x] = (JE_byte)saveTemp[x] ^ (JE_byte)(cryptKey[(x+1) % 10]);
		if (x > 0)
		{
			s2[x] ^= (JE_byte)saveTemp[x - 1];
		}

	}

	/* for (x = 0; x < SAVE_FILE_SIZE; x++) printf("%c", s2[x]); */

	/* Check save file for correctitude */
	y = 0;
	for (x = 0; x < SAVE_FILE_SIZE; x++)
	{
		y += s2[x];
	}
	if (saveTemp[SAVE_FILE_SIZE] != y)
	{
		correct = false;
		printf("Failed additive checksum: %d vs %d\n", saveTemp[SAVE_FILE_SIZE], y);
	}

	y = 0;
	for (x = 0; x < SAVE_FILE_SIZE; x++)
	{
		y -= s2[x];
	}
	if (saveTemp[SAVE_FILE_SIZE+1] != y)
	{
		correct = false;
		printf("Failed subtractive checksum: %d vs %d\n", saveTemp[SAVE_FILE_SIZE+1], y);
	}

	y = 1;
	for (x = 0; x < SAVE_FILE_SIZE; x++)
	{
		y = (y * s2[x]) + 1;
	}
	if (saveTemp[SAVE_FILE_SIZE+2] != y)
	{
		correct = false;
		printf("Failed multiplicative checksum: %d vs %d\n", saveTemp[SAVE_FILE_SIZE+2], y);
	}

	y = 0;
	for (x = 0; x < SAVE_FILE_SIZE; x++)
	{
		y = y ^ s2[x];
	}
	if (saveTemp[SAVE_FILE_SIZE+3] != y)
	{
		correct = false;
		printf("Failed XOR'd checksum: %d vs %d\n", saveTemp[SAVE_FILE_SIZE+3], y);
	}

	/* Barf and die if save file doesn't validate */
	if (!correct)
	{
		fprintf(stderr, "Error reading save file!\n");
		crashlog_report_fatal("FATAL (save file checksum mismatch)",
		                      "tyrian.sav failed its checksum validation (corrupt or truncated save)");
		exit(255);
	}

	/* Keep decrypted version plz */
	memcpy(&saveTemp, &s2, sizeof(s2));
}

const char *get_user_directory(void)
{
	static char user_dir[500] = "";
	
	if (strlen(user_dir) == 0)
	{
#if defined(__SWITCH__)
		// Fixed writable location on the SD card; switch_platform_init() creates it.
		strcpy(user_dir, "sdmc:/switch/opentyrian2000");
#elif defined(__vita__)
		// Fixed writable location on the memory card; vita_platform_init() creates it.
		strcpy(user_dir, "ux0:data/opentyrian2000");
#elif !defined(TARGET_WIN32)
		char *xdg_config_home = getenv("XDG_CONFIG_HOME");
		if (xdg_config_home != NULL)
		{
			snprintf(user_dir, sizeof(user_dir), "%s/opentyrian2000", xdg_config_home);
		}
		else
		{
			char *home = getenv("HOME");
			if (home != NULL)
			{
				snprintf(user_dir, sizeof(user_dir), "%s/.config/opentyrian2000", home);
			}
			else
			{
				strcpy(user_dir, ".");
			}
		}
#else
		strcpy(user_dir, ".");
#endif
	}
	
	return user_dir;
}

// for compatibility
Uint8 joyButtonAssign[4] = {1, 4, 5, 5};
Uint8 inputDevice_ = 0, jConfigure = 0, midiPort = 1;
bool configuration_loaded = false;

void JE_loadConfiguration(void)
{
	FILE *fi;
	int z;
	JE_byte *p;
	int y;
	
	fi = dir_fopen_warn(get_user_directory(), "tyrian.cfg", "rb");
	if (fi && ftell_eof(fi) == 28)
	{
		background2 = 0;
		fread_bool_die(&background2, fi);
		fread_u8_die(&gameSpeed, 1, fi);
		
		fread_u8_die(&inputDevice_, 1, fi);
		fread_u8_die(&jConfigure, 1, fi);
		
		fread_u8_die(&versionNum, 1, fi);
		
		fread_u8_die(&processorType, 1, fi);
		fread_u8_die(&midiPort, 1, fi);
		fread_u8_die(&soundEffects, 1, fi);
		fread_u8_die(&gammaCorrection, 1, fi);
		fread_s8_die(&difficultyLevel, 1, fi);
		
		fread_u8_die(joyButtonAssign, 4, fi);
		
		fread_u16_die(&tyrMusicVolume, 1, fi);
		fread_u16_die(&fxVolume, 1, fi);
		
		fread_u8_die(inputDevice, 2, fi);

		fread_u8_die(dosKeySettings, 8, fi);
		
		fclose(fi);
	}
	else
	{
		printf("\nInvalid or missing TYRIAN.CFG! Continuing using defaults.\n\n");
		
		soundEffects = 1;
		memcpy(&dosKeySettings, &defaultDosKeySettings, sizeof(dosKeySettings));
		background2 = true;
		tyrMusicVolume = 191;
		fxVolume = 191;
		gammaCorrection = 0;
		processorType = 4;  // detail level "Pentium"
		gameSpeed = 4;
		versionNum = 3;
	}
	
	load_opentyrian_config();
	
	if (tyrMusicVolume > 255)
		tyrMusicVolume = 255;
	if (fxVolume > 255)
		fxVolume = 255;
	
	set_volume(tyrMusicVolume, fxVolume);
	
	fi = dir_fopen_warn(get_user_directory(), "tyrian.sav", "rb");
	if (fi)
	{

		fseek(fi, 0, SEEK_SET);
		fread_die(saveTemp, 1, sizeof(saveTemp), fi);
		JE_decryptSaveTemp();

		/* SYN: The original mostly blasted the save file into raw memory. However, our lives are not so
		   easy, because the C struct is necessarily a different size. So instead we have to loop
		   through each record and load fields manually. *emo tear* :'( */

		p = saveTemp;
		for (z = 0; z < SAVE_FILES_NUM; z++)
		{
			memcpy(&saveFiles[z].encode, p, sizeof(JE_word)); p += 2;
			saveFiles[z].encode = SDL_SwapLE16(saveFiles[z].encode);
			
			memcpy(&saveFiles[z].level, p, sizeof(JE_word)); p += 2;
			saveFiles[z].level = SDL_SwapLE16(saveFiles[z].level);
			
			memcpy(&saveFiles[z].items, p, sizeof(JE_PItemsType)); p += sizeof(JE_PItemsType);
			
			memcpy(&saveFiles[z].score, p, sizeof(JE_longint)); p += 4;
			saveFiles[z].score = SDL_SwapLE32(saveFiles[z].score);
			
			memcpy(&saveFiles[z].score2, p, sizeof(JE_longint)); p += 4;
			saveFiles[z].score2 = SDL_SwapLE32(saveFiles[z].score2);
			
			/* SYN: Pascal strings are prefixed by a byte holding the length! */
			memset(&saveFiles[z].levelName, 0, sizeof(saveFiles[z].levelName));
			memcpy(&saveFiles[z].levelName, &p[1], *p);
			p += 10;
			
			/* This was a BYTE array, not a STRING, in the original. Go fig. */
			memcpy(&saveFiles[z].name, p, 14);
			p += 14;
			
			memcpy(&saveFiles[z].cubes, p, sizeof(JE_byte)); p++;
			memcpy(&saveFiles[z].power, p, sizeof(JE_byte) * 2); p += 2;
			memcpy(&saveFiles[z].episode, p, sizeof(JE_byte)); p++;
			memcpy(&saveFiles[z].lastItems, p, sizeof(JE_PItemsType)); p += sizeof(JE_PItemsType);
			memcpy(&saveFiles[z].difficulty, p, sizeof(JE_byte)); p++;
			memcpy(&saveFiles[z].secretHint, p, sizeof(JE_byte)); p++;
			memcpy(&saveFiles[z].input1, p, sizeof(JE_byte)); p++;
			memcpy(&saveFiles[z].input2, p, sizeof(JE_byte)); p++;
			
			/* booleans were 1 byte in pascal -- working around it */
			Uint8 temp;
			memcpy(&temp, p, 1); p++;
			saveFiles[z].gameHasRepeated = temp != 0;
			
			memcpy(&saveFiles[z].initialDifficulty, p, sizeof(JE_byte)); p++;
			
			memcpy(&saveFiles[z].highScore1, p, sizeof(JE_longint)); p += 4;
			saveFiles[z].highScore1 = SDL_SwapLE32(saveFiles[z].highScore1);
			
			memcpy(&saveFiles[z].highScore2, p, sizeof(JE_longint)); p += 4;
			saveFiles[z].highScore2 = SDL_SwapLE32(saveFiles[z].highScore2);
			
			memset(&saveFiles[z].highScoreName, 0, sizeof(saveFiles[z].highScoreName));
			memcpy(&saveFiles[z].highScoreName, &p[1], *p);
			p += 30;
			
			memcpy(&saveFiles[z].highScoreDiff, p, sizeof(JE_byte)); p++;

			memcpy(&temp, p, 1); p++;  // autoFireSpecial
			saveFiles[z].autoFireSpecial = temp != 0;

			memcpy(&temp, p, 1); p++;  // chargeSidekickAutofire
			saveFiles[z].chargeSidekickAutofire = temp;  // 0=OFF, 1=ON, 2=fully-charged-only, 3=ON (fastest)

			memcpy(&temp, p, 1); p++;  // difficultyAdjust
			saveFiles[z].difficultyAdjust = temp != 0;

			memcpy(&temp, p, 1); p++;  // cheatInfiniteSidekickAmmo
			saveFiles[z].cheatInfiniteSidekickAmmo = temp != 0;

			memcpy(&temp, p, 1); p++;  // cheatInfiniteShields
			saveFiles[z].cheatInfiniteShields = temp != 0;

			memcpy(&temp, p, 1); p++;  // cheatInfiniteArmor
			saveFiles[z].cheatInfiniteArmor = temp != 0;

			memcpy(&temp, p, 1); p++;  // expertMode
			saveFiles[z].expertMode = temp != 0;
		}

		/* SYN: This is truncating to bytes. I have no idea what this is doing or why. */
		/* TODO: Figure out what this is about and make sure it isn't broken. */
		editorLevel = (saveTemp[SIZEOF_SAVEGAMETEMP - 5] << 8) | saveTemp[SIZEOF_SAVEGAMETEMP - 6];

		// T2K High Scores are unencrypted after saveTemp
		for (z = 0; z < 10; ++z)
		{
			JE_byte len;

			for (y = 0; y < 3; ++y)
			{
				fread_s32_die(&t2kHighScores[z][y].score, 1, fi);
				t2kHighScores[z][y].score = SDL_SwapLE32(t2kHighScores[z][y].score);

				fread_u8_die(&len, 1, fi);
				fread_die(t2kHighScores[z][y].playerName, 1, 29, fi);

				t2kHighScores[z][y].playerName[len] = '\0';
				fread_u8_die(&t2kHighScores[z][y].difficulty, 1, fi);
			}
		}
		for (z = 10; z < 20; ++z)
		{
			JE_byte len;

			for (y = 0; y < 3; ++y)
			{
				fread_s32_die(&t2kHighScores[z][y].score, 1, fi);
				t2kHighScores[z][y].score = SDL_SwapLE32(t2kHighScores[z][y].score);

				fseek(fi, 4, SEEK_CUR); // Unknown long int that seems to have no effect
				fread_u8_die(&len, 1, fi);

				fread_die(t2kHighScores[z][y].playerName, 1, 29, fi);
				t2kHighScores[z][y].playerName[len] = '\0';
				fread_u8_die(&t2kHighScores[z][y].difficulty, 1, fi);
			}
		}

		fclose(fi);
	}
	else
	{
		/* We didn't have a save file! Let's make up random stuff! */
		editorLevel = 800;

		for (z = 0; z < 100; z++)
		{
			saveTemp[SAVE_FILES_SIZE + z] = initialItemAvail[z];
		}

		for (z = 0; z < SAVE_FILES_NUM; z++)
		{
			saveFiles[z].level = 0;

			for (y = 0; y < 14; y++)
			{
				saveFiles[z].name[y] = ' ';
			}
			saveFiles[z].name[14] = 0;

			saveFiles[z].highScore1 = ((mt_rand() % 20) + 1) * 1000;

			if (z % 6 > 2)
			{
				saveFiles[z].highScore2 = ((mt_rand() % 20) + 1) * 1000;
				strcpy(saveFiles[z].highScoreName, defaultTeamNames[mt_rand() % COUNTOF(defaultTeamNames)]);
			}
			else
			{
				strcpy(saveFiles[z].highScoreName, defaultHighScoreNames[mt_rand() % COUNTOF(defaultHighScoreNames)]);
			}

			saveFiles[z].autoFireSpecial = false;

			saveFiles[z].chargeSidekickAutofire = CHARGE_AUTOFIRE_OFF;
			saveFiles[z].difficultyAdjust = true;
			saveFiles[z].cheatInfiniteSidekickAmmo = false;
			saveFiles[z].cheatInfiniteShields = false;
			saveFiles[z].cheatInfiniteArmor = false;
			saveFiles[z].expertMode = false;
		}

		for (z = 0; z < 10; ++z)
		{
			for (y = 0; y < 3; ++y)
			{
				// Timed Battle scores
				t2kHighScores[z][y].score = ((mt_rand() % 50) + 1) * 100;
				strcpy(t2kHighScores[z][y].playerName, defaultHighScoreNames[mt_rand() % COUNTOF(defaultHighScoreNames)]);
			}
		}
		for (z = 10; z < 20; ++z)
		{
			for (y = 0; y < 3; ++y)
			{
				// Main Game scores
				t2kHighScores[z][y].score = ((mt_rand() % 20) + 1) * 1000;
				if (z & 1)
					strcpy(t2kHighScores[z][y].playerName, defaultTeamNames[mt_rand() % COUNTOF(defaultTeamNames)]);
				else
					strcpy(t2kHighScores[z][y].playerName, defaultHighScoreNames[mt_rand() % COUNTOF(defaultHighScoreNames)]);
			}
		}
	}
	
	JE_initProcessorType();
	configuration_loaded = true;
}

void JE_saveConfiguration(void)
{
	FILE *f;
	JE_byte *p;
	int z;

	// Don't save nothing
	if (!configuration_loaded)
		return;

	p = saveTemp;
	for (z = 0; z < SAVE_FILES_NUM; z++)
	{
		JE_SaveFileType tempSaveFile;
		memcpy(&tempSaveFile, &saveFiles[z], sizeof(tempSaveFile));
		
		tempSaveFile.encode = SDL_SwapLE16(tempSaveFile.encode);
		memcpy(p, &tempSaveFile.encode, sizeof(JE_word)); p += 2;
		
		tempSaveFile.level = SDL_SwapLE16(tempSaveFile.level);
		memcpy(p, &tempSaveFile.level, sizeof(JE_word)); p += 2;
		
		memcpy(p, &tempSaveFile.items, sizeof(JE_PItemsType)); p += sizeof(JE_PItemsType);
		
		tempSaveFile.score = SDL_SwapLE32(tempSaveFile.score);
		memcpy(p, &tempSaveFile.score, sizeof(JE_longint)); p += 4;
		
		tempSaveFile.score2 = SDL_SwapLE32(tempSaveFile.score2);
		memcpy(p, &tempSaveFile.score2, sizeof(JE_longint)); p += 4;
		
		/* SYN: Pascal strings are prefixed by a byte holding the length! */
		memset(p, 0, sizeof(tempSaveFile.levelName));
		*p = strlen(tempSaveFile.levelName);
		memcpy(&p[1], &tempSaveFile.levelName, *p);
		p += 10;
		
		/* This was a BYTE array, not a STRING, in the original. Go fig. */
		memcpy(p, &tempSaveFile.name, 14);
		p += 14;
		
		memcpy(p, &tempSaveFile.cubes, sizeof(JE_byte)); p++;
		memcpy(p, &tempSaveFile.power, sizeof(JE_byte) * 2); p += 2;
		memcpy(p, &tempSaveFile.episode, sizeof(JE_byte)); p++;
		memcpy(p, &tempSaveFile.lastItems, sizeof(JE_PItemsType)); p += sizeof(JE_PItemsType);
		memcpy(p, &tempSaveFile.difficulty, sizeof(JE_byte)); p++;
		memcpy(p, &tempSaveFile.secretHint, sizeof(JE_byte)); p++;
		memcpy(p, &tempSaveFile.input1, sizeof(JE_byte)); p++;
		memcpy(p, &tempSaveFile.input2, sizeof(JE_byte)); p++;
		
		/* booleans were 1 byte in pascal -- working around it */
		Uint8 temp = tempSaveFile.gameHasRepeated != false;
		memcpy(p, &temp, 1); p++;
		
		memcpy(p, &tempSaveFile.initialDifficulty, sizeof(JE_byte)); p++;
		
		tempSaveFile.highScore1 = SDL_SwapLE32(tempSaveFile.highScore1);
		memcpy(p, &tempSaveFile.highScore1, sizeof(JE_longint)); p += 4;
		
		tempSaveFile.highScore2 = SDL_SwapLE32(tempSaveFile.highScore2);
		memcpy(p, &tempSaveFile.highScore2, sizeof(JE_longint)); p += 4;
		
		memset(p, 0, sizeof(tempSaveFile.highScoreName));
		*p = strlen(tempSaveFile.highScoreName);
		memcpy(&p[1], &tempSaveFile.highScoreName, *p);
		p += 30;
		
		memcpy(p, &tempSaveFile.highScoreDiff, sizeof(JE_byte)); p++;

		temp = tempSaveFile.autoFireSpecial != false;
		memcpy(p, &temp, 1); p++;

		temp = tempSaveFile.chargeSidekickAutofire;  // 0=OFF, 1=ON, 2=fully-charged-only, 3=ON (fastest)
		memcpy(p, &temp, 1); p++;

		temp = tempSaveFile.difficultyAdjust != false;
		memcpy(p, &temp, 1); p++;

		temp = tempSaveFile.cheatInfiniteSidekickAmmo != false;
		memcpy(p, &temp, 1); p++;

		temp = tempSaveFile.cheatInfiniteShields != false;
		memcpy(p, &temp, 1); p++;

		temp = tempSaveFile.cheatInfiniteArmor != false;
		memcpy(p, &temp, 1); p++;

		temp = tempSaveFile.expertMode != false;
		memcpy(p, &temp, 1); p++;
	}
	
	saveTemp[SIZEOF_SAVEGAMETEMP - 6] = editorLevel >> 8;
	saveTemp[SIZEOF_SAVEGAMETEMP - 5] = editorLevel;
	
	JE_encryptSaveTemp();
	
#ifndef TARGET_WIN32
	mkdir(get_user_directory(), 0700);
#else
	mkdir(get_user_directory());
#endif
	
	f = dir_fopen_warn(get_user_directory(), "tyrian.sav", "wb");
	if (f != NULL)
	{
		fwrite_die(saveTemp, 1, sizeof(saveTemp), f);

		// T2K High Scores are unencrypted after saveTemp
		for (z = 0; z < 10; ++z)
		{
			JE_longint templi;
			JE_byte len;

			for (y = 0; y < 3; ++y)
			{
				templi = SDL_SwapLE32(t2kHighScores[z][y].score);
				len = strlen(t2kHighScores[z][y].playerName);
				fwrite_s32_die(&templi, f);

				fwrite_u8_die(&len, 1, f);
				fwrite_die(t2kHighScores[z][y].playerName, 1, 29, f);
				fwrite_u8_die(&t2kHighScores[z][y].difficulty, 1, f);
			}
		}
		for (z = 10; z < 20; ++z)
		{
			JE_longint templi;
			JE_byte len;

			for (y = 0; y < 3; ++y)
			{
				templi = SDL_SwapLE32(t2kHighScores[z][y].score);
				len = strlen(t2kHighScores[z][y].playerName);
				fwrite_s32_die(&templi, f);

				templi = 0x12345678;
				fwrite_s32_die(&templi, f); // Unknown long int that seems to have no effect

				fwrite_u8_die(&len, 1, f);
				fwrite_die(t2kHighScores[z][y].playerName, 1, 29, f);
				fwrite_u8_die(&t2kHighScores[z][y].difficulty, 1, f);
			}
		}

#if _POSIX_C_SOURCE >= 1 || _XOPEN_SOURCE || _POSIX_SOURCE
		fsync(fileno(f));
#endif
		fclose(f);
	}
	
	JE_decryptSaveTemp();
	
	f = dir_fopen_warn(get_user_directory(), "tyrian.cfg", "wb");
	if (f != NULL)
	{
		fwrite_bool_die(&background2, f);
		fwrite_u8_die(&gameSpeed, 1, f);
		
		fwrite_u8_die(&inputDevice_, 1, f);
		fwrite_u8_die(&jConfigure, 1, f);
		
		fwrite_u8_die(&versionNum, 1, f);
		fwrite_u8_die(&processorType, 1, f);
		fwrite_u8_die(&midiPort, 1, f);
		fwrite_u8_die(&soundEffects, 1, f);
		fwrite_u8_die(&gammaCorrection, 1, f);
		fwrite_s8_die(&difficultyLevel, 1, f);
		fwrite_u8_die(joyButtonAssign, 4, f);
		
		fwrite_u16_die(&tyrMusicVolume, f);
		fwrite_u16_die(&fxVolume, f);
		
		fwrite_u8_die(inputDevice, 2, f);
		
		fwrite_u8_die(dosKeySettings, 8, f);
		
#if _POSIX_C_SOURCE >= 1 || _XOPEN_SOURCE || _POSIX_SOURCE
		fsync(fileno(f));
#endif
		fclose(f);
	}
	
	save_opentyrian_config();
}
