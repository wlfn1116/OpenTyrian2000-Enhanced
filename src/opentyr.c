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
#include "opentyr.h"

#include "config.h"
#include "crashlog.h"
#include "custom_weapon.h"
#include "destruct.h"
#include "editship.h"
#include "episodes.h"
#include "file.h"
#include "font.h"
#include "fonthand.h"
#include "game_menu.h"
#include "helptext.h"
#include "joystick.h"
#include "jukebox.h"
#include "keyboard.h"
#include "loudness.h"
#include "mainint.h"
#include "mouse.h"
#include "mtrand.h"
#include "network.h"
#include "nortsong.h"
#include "nortvars.h"
#include "opentyrian_version.h"
#include "palette.h"
#include "params.h"
#include "picload.h"
#include "sprite.h"
#include "console_platform.h"
#include "tyrian2.h"
#include "varz.h"
#include "vga256d.h"
#include "video.h"
#include "video_scale.h"
#include "xmas.h"

#include "SDL.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

const char *opentyrian_str = "OpenTyrian2000";
const char *opentyrian_version = OPENTYRIAN_VERSION;

#if !defined(__SWITCH__) && !defined(__vita__)
// The consoles (Switch / Vita) have a single, always-fullscreen display managed by the
// video driver, so the Window/Display picker is meaningless there and is omitted from
// the Graphics menu below.
static size_t getDisplayPickerItemsCount(void)
{
	return 1 + (size_t)SDL_GetNumVideoDisplays();
}

static const char *getDisplayPickerItem(size_t i, char *buffer, size_t bufferSize)
{
	if (i == 0)
		return "Window";

	snprintf(buffer, bufferSize, "Display %d", (int)i);
	return buffer;
}
#endif

static size_t getScalerPickerItemsCount(void)
{
	return (size_t)scalers_count;
}

static const char *getScalerPickerItem(size_t i, char *buffer, size_t bufferSize)
{
	(void)buffer, (void)bufferSize;

	return scalers[i].name;
}

static size_t getScalingModePickerItemsCount(void)
{
	return (size_t)ScalingMode_MAX;
}

static const char* getScalingModePickerItem(size_t i, char* buffer, size_t bufferSize)
{
	(void)buffer, (void)bufferSize;

	return scaling_mode_names[i];
}

static const int fps_options[] = { 0, 35, 60, 120 };

static size_t getFPSPickerItemsCount(void)
{
	return COUNTOF(fps_options);
}

static const char* getFPSPickerItem(size_t i, char* buffer, size_t bufferSize)
{
	if (fps_options[i] == 0)
		return "Uncapped";

	snprintf(buffer, bufferSize, "%d", fps_options[i]);
	return buffer;
}

/* ---- Graphics: sub-pixel supersampling picker ---- */

// Index maps directly onto render_supersample: 0 = Auto (follow the scaler),
// 1 = Off, 2..8 = fixed NxN. Keep the last entry == RENDER_SUPERSAMPLE_MAX.
static const char *const supersampleNames[] = { "Auto", "Off", "2x", "3x", "4x", "5x", "6x", "7x", "8x" };

static size_t getSupersamplePickerItemsCount(void) { return COUNTOF(supersampleNames); }
static const char* getSupersamplePickerItem(size_t i, char* buffer, size_t bufferSize)
{
	(void)buffer; (void)bufferSize;
	return supersampleNames[i];
}

// Index maps onto render_supersample_filter (SS_FILTER_SHARP / SS_FILTER_SMOOTH /
// SS_FILTER_NONE). Keep this order in sync with the enum — persisted as the index.
static const char *const ssFilterNames[] = { "Sharp", "Smooth", "None" };

static size_t getSSFilterPickerItemsCount(void) { return COUNTOF(ssFilterNames); }
static const char* getSSFilterPickerItem(size_t i, char* buffer, size_t bufferSize)
{
	(void)buffer; (void)bufferSize;
	return ssFilterNames[i];
}

/* ---- Enhancements: boss health bar pickers ---- */

static const char *const bossBarStyleNames[]  = { "Classic", "Enhanced" };
static const char *const bossBarLayoutNames[] = { "Top", "Bottom", "Left", "Right" };
static const char *const bossBarTwoNames[]    = { "Together", "Split", "Stacked" };

static size_t getBossBarStyleItemsCount(void) { return COUNTOF(bossBarStyleNames); }
static const char* getBossBarStyleItem(size_t i, char* buffer, size_t bufferSize)
{
	(void)buffer; (void)bufferSize;
	return bossBarStyleNames[i];
}

static size_t getBossBarLayoutItemsCount(void) { return COUNTOF(bossBarLayoutNames); }
static const char* getBossBarLayoutItem(size_t i, char* buffer, size_t bufferSize)
{
	(void)buffer; (void)bufferSize;
	return bossBarLayoutNames[i];
}

static size_t getBossBarTwoItemsCount(void) { return COUNTOF(bossBarTwoNames); }
static const char* getBossBarTwoItem(size_t i, char* buffer, size_t bufferSize)
{
	(void)buffer; (void)bufferSize;
	return bossBarTwoNames[i];
}

/* ---- Enhancements: enemy health bar pickers ---- */

static const char *const enemyBarLayoutNames[]   = { "Horizontal", "Vertical" };
static const char *const enemyBarPositionNames[] = { "Bottom", "Top", "Left", "Right", "Center" };

static size_t getEnemyBarLayoutItemsCount(void) { return COUNTOF(enemyBarLayoutNames); }
static const char* getEnemyBarLayoutItem(size_t i, char* buffer, size_t bufferSize)
{
	(void)buffer; (void)bufferSize;
	return enemyBarLayoutNames[i];
}

static size_t getEnemyBarPositionItemsCount(void) { return COUNTOF(enemyBarPositionNames); }
static const char* getEnemyBarPositionItem(size_t i, char* buffer, size_t bufferSize)
{
	(void)buffer; (void)bufferSize;
	return enemyBarPositionNames[i];
}

/* ---- Enhancements: HUD gauge gradient pickers ---- */

// Indexed by GaugeGradientDir (config.h). "Up" is the classic vertical gauge look; Left/Right
// run the gradient across the bar's 9-pixel width instead. The label names the bar's bright end.
static const char *const gaugeGradNames[] = { "Up", "Down", "Left", "Right" };

static size_t getGaugeGradItemsCount(void) { return COUNTOF(gaugeGradNames); }
static const char* getGaugeGradItem(size_t i, char* buffer, size_t bufferSize)
{
	(void)buffer; (void)bufferSize;
	return gaugeGradNames[i];
}

/* ---- Sound: music synthesizer picker ---- */

// music_device_names[] / MUSIC_DEVICE_MAX come from loudness.h. The MIDI devices
// (FluidSynth / Native MIDI) only produce sound in a WITH_MIDI build; otherwise
// init_audio() forces the choice back to OPL.
static size_t getMusicDeviceItemsCount(void)
{
#ifdef WITH_MIDI
	return MUSIC_DEVICE_MAX;
#else
	return 1;  // only OPL3 works without a MIDI build (e.g. the Switch port)
#endif
}
static const char* getMusicDeviceItem(size_t i, char* buffer, size_t bufferSize)
{
	(void)buffer; (void)bufferSize;
	return music_device_names[i];
}

/* ---- Weapon Tweaks: Zica Laser pickers ---- */

// Indexed by the ZICA_BASE_* / ZICA_LEN_* enums (config.h).
static const char *const zicaBaseNames[]   = { "Auto", "Ep 1-3", "Ep 4+" };
static const char *const zicaLengthNames[] = { "Short", "Long" };

static size_t getZicaBaseItemsCount(void) { return COUNTOF(zicaBaseNames); }
static const char* getZicaBaseItem(size_t i, char* buffer, size_t bufferSize)
{
	(void)buffer; (void)bufferSize;
	return zicaBaseNames[i];
}

static size_t getZicaLengthItemsCount(void) { return COUNTOF(zicaLengthNames); }
static const char* getZicaLengthItem(size_t i, char* buffer, size_t bufferSize)
{
	(void)buffer; (void)bufferSize;
	return zicaLengthNames[i];
}

// Indexed by the SUPER_SPARKS_* enum (config.h).
static const char *const sparkModeNames[] = { "Auto", "On", "Off" };
static size_t getSparkModeItemsCount(void) { return COUNTOF(sparkModeNames); }
static const char* getSparkModeItem(size_t i, char* buffer, size_t bufferSize)
{
	(void)buffer; (void)bufferSize;
	return sparkModeNames[i];
}

// Which superspark weapon the shared Sparks/Spark Cap rows edit; set when one of the
// per-weapon submenus under Superspark Weapons is entered (the submenus share their
// item ids, so this picks the superSparkMode/superSparkClassicCap slot they act on).
static int currentSparkWeapon = SSW_MEGA_PULSE;

// Likewise for the Episode Differences submenus: which EpDiffWeapon the shared "Version:"
// row edits (all eight per-weapon submenus share MENU_ITEM_EPDIFF_MODE / epDiffMode slot).
static int currentDiffWeapon = EDW_XEGA_BALL;

// Toggle Christmas mode from the Extra menu. Christmas swaps the shape table
// (tyrianc.shp) and voice samples (voicesc.snd) for festive versions, so the toggle
// reloads them (same swap xmas_prompt does at startup). Persisted via xmasMode across
// restarts. Returns false if enabling was refused for missing Christmas data files.
static bool toggle_xmas_mode(void)
{
	const bool want = !xmas;

	if (want && (!dir_file_exists(data_dir(), "tyrianc.shp") || !dir_file_exists(data_dir(), "voicesc.snd")))
		return false;  // can't enable Christmas without its data files

	xmas = want;
	override_xmas = true;      // honour this explicit choice over date auto-detection
	xmasMode = xmas ? 1 : 0;   // persist the choice

	free_main_shape_tables();
	JE_loadMainShapeTables(xmas ? "tyrianc.shp" : "tyrian.shp");

	if (!audio_disabled)
	{
		// loadSndFile frees/reallocs the sample buffers the audio callback reads via
		// the channel pointers, so silence the channels first (see stop_sample_channels).
		stop_sample_channels();
		loadSndFile(xmas);
	}

	return true;
}

// While supersampling is enabled, algorithm scalers (Scale2x/hqNx) are bypassed by
// the in-game hi path, which would make gameplay and pause/menus look different —
// so switch to the same-size plain scaler. Called after the Sub-pixel setting changes;
// the scaler picker enforces the same rule (algorithm entries grayed and unselectable).
static void enforcePlainScalerForSupersample(void)
{
	if (render_supersample != 1 && !scaler_is_plain(scaler))
	{
		const uint plain = scaler_plain_equivalent(scaler);
		if (!init_scaler(plain))
			init_scaler(scaler);  // shouldn't happen; keep a working scaler
	}
}

typedef enum
{
	MENU_ITEM_NONE = 0,
	MENU_ITEM_DONE,
	MENU_ITEM_GRAPHICS,
	MENU_ITEM_SOUND,
	MENU_ITEM_ENHANCEMENTS,
	MENU_ITEM_JUKEBOX,
	MENU_ITEM_DESTRUCT,
	MENU_ITEM_DISPLAY,
	MENU_ITEM_SCALER,
	MENU_ITEM_SCALING_MODE,
	MENU_ITEM_SUPERSAMPLE,
	MENU_ITEM_SS_FILTER,
	MENU_ITEM_FPS,
	MENU_ITEM_VSYNC,
	MENU_ITEM_SHOW_FPS,
	MENU_ITEM_MUSIC_VOLUME,
	MENU_ITEM_SOUND_VOLUME,
	MENU_ITEM_MUSIC_DEVICE,         // music synthesizer: OPL3 / FluidSynth / Native MIDI
	MENU_ITEM_TOUCH_SENS,           // Switch only: touchscreen ship sensitivity slider
	MENU_ITEM_BOSS_BARS,
	MENU_ITEM_BOSS_BAR_STYLE,
	MENU_ITEM_BOSS_BAR_LAYOUT,
	MENU_ITEM_BOSS_BAR_TWO,
	MENU_ITEM_ENEMY_BARS_MENU,
	MENU_ITEM_ENEMY_BARS,
	MENU_ITEM_ENEMY_BAR_LAYOUT,
	MENU_ITEM_ENEMY_BAR_POS,
	MENU_ITEM_ENEMY_BAR_OPACITY,
	MENU_ITEM_GAUGE_GRADS_MENU,
	MENU_ITEM_GAUGE_GRAD_GEN,
	MENU_ITEM_GAUGE_GRAD_SHIELD,
	MENU_ITEM_GAUGE_GRAD_ARMOR,
	MENU_ITEM_DEBUG_MODE,
	MENU_ITEM_SMOOTH_MOTION,
	MENU_ITEM_EXTRA_SPARKS,
	MENU_ITEM_XMAS,
	MENU_ITEM_WEAPON_TWEAKS,
	MENU_ITEM_ZICA_LASER,
	MENU_ITEM_ZICA_BASE,
	MENU_ITEM_ZICA_LENGTH,
	MENU_ITEM_ZICA_LOCK,
	MENU_ITEM_ZICA_BUFF,
	MENU_ITEM_SUPERSPARKS,          // opens the Superspark Weapons submenu
	MENU_ITEM_SPARKS_MEGA_PULSE,    // per-weapon submenu entries...
	MENU_ITEM_SPARKS_WALLOP,
	MENU_ITEM_SPARKS_PROTRON_B,
	MENU_ITEM_SPARKS_ICE,
	MENU_ITEM_SPARKS_MODE,          // shared rows inside those submenus (see currentSparkWeapon)
	MENU_ITEM_SPARKS_CAP,
	MENU_ITEM_WALLOP_BOLT,          // Wallop Beam only: the ep4/5 second bolt per volley
	MENU_ITEM_EPDIFFS,              // opens the Episode Differences submenu
	MENU_ITEM_EPDIFF_XEGA,          // per-weapon submenu entries (mirror EpDiffWeapon order)...
	MENU_ITEM_EPDIFF_MICROSOL,
	MENU_ITEM_EPDIFF_FLARE,
	MENU_ITEM_EPDIFF_NEEDLE,
	MENU_ITEM_EPDIFF_BUBBLE,
	MENU_ITEM_EPDIFF_PUNCH,
	MENU_ITEM_EPDIFF_PRETZEL,
	MENU_ITEM_EPDIFF_DRAGON,
	MENU_ITEM_EPDIFF_MODE,          // shared "Version:" row inside those submenus (see currentDiffWeapon)
	MENU_ITEM_CHARGE_LASER,
	MENU_ITEM_CUSTOM_WEAPONS,
	MENU_ITEM_CUSTOM_CREATOR,
	MENU_ITEM_SUPERTYRIAN,
	MENU_ITEM_ARCADE_MENU,
	MENU_ITEM_CMDLINE_MENU,
	MENU_ITEM_RICH_MODE,
	MENU_ITEM_CONSTANT_PLAY,
	MENU_ITEM_CONSTANT_DIE,
	MENU_ITEM_ARCADE_SHIP_BASE,  // keep LAST: ids BASE+0..BASE+8 are the 9 arcade ships
} MenuItemId;

/* Adjust a setup-menu item's value in response to left/right input (dir is -1
 * or +1). Items without a cyclable value are ignored. */
static void adjustMenuItemValue(MenuItemId id, int dir)
{
	switch (id)
	{
	case MENU_ITEM_MUSIC_VOLUME:
		JE_playSampleNum(S_CURSOR);
		JE_changeVolume(&tyrMusicVolume, dir * 8, &fxVolume, 0);
		break;
	case MENU_ITEM_SOUND_VOLUME:
		JE_changeVolume(&tyrMusicVolume, 0, &fxVolume, dir * 8);
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_TOUCH_SENS:
		touch_sensitivity = MIN(MAX(0, touch_sensitivity + dir * 8), TOUCH_SENS_MAX);
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_FPS:
		if (dir > 0)
			fps_cap += 5;
		else
			fps_cap = fps_cap > 5 ? fps_cap - 5 : 0;
		set_fps(fps_cap);
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_SUPERSAMPLE:
		render_supersample = (render_supersample + (int)COUNTOF(supersampleNames) + dir) % (int)COUNTOF(supersampleNames);
		enforcePlainScalerForSupersample();
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_SS_FILTER:
		render_supersample_filter = (render_supersample_filter + (int)COUNTOF(ssFilterNames) + dir) % (int)COUNTOF(ssFilterNames);
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_VSYNC:
		set_vsync(!output_vsync);
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_SHOW_FPS:
		show_fps = !show_fps;
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_MUSIC_DEVICE:
	{
		int n = (int)getMusicDeviceItemsCount();
		music_device = (MusicDevice)((music_device + n + dir) % n);
		restart_audio();  // apply live (tears down + re-inits the synth; see loudness.c)
		JE_playSampleNum(S_CURSOR);
		break;
	}
	case MENU_ITEM_BOSS_BAR_STYLE:
		bossBarStyle = (bossBarStyle + (int)COUNTOF(bossBarStyleNames) + dir) % (int)COUNTOF(bossBarStyleNames);
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_BOSS_BAR_LAYOUT:
		bossBarLayout = (bossBarLayout + (int)COUNTOF(bossBarLayoutNames) + dir) % (int)COUNTOF(bossBarLayoutNames);
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_BOSS_BAR_TWO:
		bossBarTwoMode = (bossBarTwoMode + (int)COUNTOF(bossBarTwoNames) + dir) % (int)COUNTOF(bossBarTwoNames);
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_ENEMY_BARS:
		enemyBars = !enemyBars;
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_ENEMY_BAR_LAYOUT:
		enemyBarLayout = (enemyBarLayout + (int)COUNTOF(enemyBarLayoutNames) + dir) % (int)COUNTOF(enemyBarLayoutNames);
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_ENEMY_BAR_POS:
		enemyBarPosition = (enemyBarPosition + (int)COUNTOF(enemyBarPositionNames) + dir) % (int)COUNTOF(enemyBarPositionNames);
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_ENEMY_BAR_OPACITY:
		enemyBarOpacity = dir > 0 ? MIN(100, enemyBarOpacity + 5) : MAX(0, enemyBarOpacity - 5);
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_GAUGE_GRAD_GEN:
		gaugeGradGenerator = (gaugeGradGenerator + (int)COUNTOF(gaugeGradNames) + dir) % (int)COUNTOF(gaugeGradNames);
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_GAUGE_GRAD_SHIELD:
		gaugeGradShield = (gaugeGradShield + (int)COUNTOF(gaugeGradNames) + dir) % (int)COUNTOF(gaugeGradNames);
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_GAUGE_GRAD_ARMOR:
		gaugeGradArmor = (gaugeGradArmor + (int)COUNTOF(gaugeGradNames) + dir) % (int)COUNTOF(gaugeGradNames);
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_DEBUG_MODE:
		debugMode = !debugMode;
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_SMOOTH_MOTION:
		smoothMotion = !smoothMotion;
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_EXTRA_SPARKS:
		extraSparks = !extraSparks;
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_XMAS:
		JE_playSampleNum(toggle_xmas_mode() ? S_CURSOR : S_SPRING);
		break;
	case MENU_ITEM_ZICA_BASE:
		zicaLaserBase = (zicaLaserBase + (int)COUNTOF(zicaBaseNames) + dir) % (int)COUNTOF(zicaBaseNames);
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_ZICA_LENGTH:
		zicaLaserLength = (zicaLaserLength + (int)COUNTOF(zicaLengthNames) + dir) % (int)COUNTOF(zicaLengthNames);
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_ZICA_LOCK:
		zicaLaserLock = !zicaLaserLock;
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_ZICA_BUFF:
		zicaLaserBuff = !zicaLaserBuff;
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_SPARKS_MODE:
		superSparkMode[currentSparkWeapon] = (superSparkMode[currentSparkWeapon] + (int)COUNTOF(sparkModeNames) + dir) % (int)COUNTOF(sparkModeNames);
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_SPARKS_CAP:
		superSparkClassicCap[currentSparkWeapon] = !superSparkClassicCap[currentSparkWeapon];
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_WALLOP_BOLT:
		wallopSecondBolt = (wallopSecondBolt + (int)COUNTOF(sparkModeNames) + dir) % (int)COUNTOF(sparkModeNames);
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_EPDIFF_MODE:
		epDiffMode[currentDiffWeapon] = (epDiffMode[currentDiffWeapon] + (int)COUNTOF(zicaBaseNames) + dir) % (int)COUNTOF(zicaBaseNames);
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_CHARGE_LASER:
		chargeLaserCannon = !chargeLaserCannon;
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_CUSTOM_WEAPONS:
		customWeaponEnabled = !customWeaponEnabled;
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_RICH_MODE:
		richMode = !richMode;
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_CONSTANT_PLAY:
		constantPlay = !constantPlay;
		JE_playSampleNum(S_CURSOR);
		break;
	case MENU_ITEM_CONSTANT_DIE:
		constantDie = !constantDie;
		JE_playSampleNum(S_CURSOR);
		break;
	default:
		break;
	}
}

typedef enum
{
	MENU_NONE = 0,
	MENU_SETUP,
	MENU_GRAPHICS,
	MENU_SOUND,
	MENU_ENHANCEMENTS,
	MENU_BOSS_BARS,
	MENU_ENEMY_BARS,
	MENU_GAUGE_GRADIENTS,
	MENU_WEAPON_TWEAKS,
	MENU_ZICA_LASER,
	MENU_SUPERSPARKS,
	MENU_SPARKS_MEGA_PULSE,
	MENU_SPARKS_WALLOP,
	MENU_SPARKS_PROTRON_B,
	MENU_SPARKS_ICE,
	MENU_EPDIFFS,
	MENU_EPDIFF_XEGA,
	MENU_EPDIFF_MICROSOL,
	MENU_EPDIFF_FLARE,
	MENU_EPDIFF_NEEDLE,
	MENU_EPDIFF_BUBBLE,
	MENU_EPDIFF_PUNCH,
	MENU_EPDIFF_PRETZEL,
	MENU_EPDIFF_DRAGON,
	MENU_EXTRA,
	MENU_ARCADE,
	MENU_CMDLINE,
} MenuId;

// Runs the shared options-menu framework starting at the given root menu.
// Returns true if a full game was launched (SuperTyrian / Super Arcade), in
// which case the caller (title screen) should start the game.
static bool runOptionsMenu(MenuId startMenu);

void setupMenu(void)
{
	runOptionsMenu(MENU_SETUP);
}

bool extraMenu(void)
{
	return runOptionsMenu(MENU_EXTRA);
}

static bool runOptionsMenu(MenuId startMenu)
{

	typedef struct
	{
		MenuItemId id;
		const char *name;
		const char *description;
		size_t (*getPickerItemsCount)(void);
		const char *(*getPickerItem)(size_t i, char *buffer, size_t bufferSize);
	} MenuItem;

	typedef struct
	{
		const char *header;
		const MenuItem items[12];
	} Menu;

	static const Menu menus[] = {
		[MENU_SETUP] = {
			.header = "Setup",
			.items = {
				{ MENU_ITEM_GRAPHICS, "Graphics...", "Change the graphics settings." },
				{ MENU_ITEM_SOUND, "Sound...", "Change the sound settings." },
#if defined(__SWITCH__) || defined(__vita__)
				{ MENU_ITEM_TOUCH_SENS, "Touch", "Touchscreen ship control sensitivity." },
#endif
				{ MENU_ITEM_ENHANCEMENTS, "Enhancements...", "Change the gameplay enhancement settings." },
				{ MENU_ITEM_DONE, "Done", "Return to the main menu." },
				{ -1 }
			},
		},
		[MENU_GRAPHICS] = {
			.header = "Graphics",
			.items = {
#if !defined(__SWITCH__) && !defined(__vita__)
				{ MENU_ITEM_DISPLAY, "Display:", "Change the display mode.", getDisplayPickerItemsCount, getDisplayPickerItem },
#endif
				{ MENU_ITEM_SCALER, "Scaler:", "Change the pixel art scaling algorithm.", getScalerPickerItemsCount, getScalerPickerItem },
								{ MENU_ITEM_SCALING_MODE, "Scaling Mode:", "Change the scaling mode.", getScalingModePickerItemsCount, getScalingModePickerItem },
								{ MENU_ITEM_SUPERSAMPLE, "Sub-pixel:", "Supersample in-game motion; Auto matches the scaler.", getSupersamplePickerItemsCount, getSupersamplePickerItem },
								{ MENU_ITEM_SS_FILTER, "Filter:", "Sub-pixel filter: Sharp, Smooth, or None (raw).", getSSFilterPickerItemsCount, getSSFilterPickerItem },
								{ MENU_ITEM_VSYNC, "VSync:", "Sync presentation to your monitor's refresh rate." },
								{ MENU_ITEM_FPS, "FPS Cap:", "Cap presented frames when VSync is off (0 = uncapped).", getFPSPickerItemsCount, getFPSPickerItem },
								{ MENU_ITEM_SHOW_FPS, "Show FPS:", "Show a frame-rate counter while playing." },
								{ MENU_ITEM_DONE, "Done", "Return to the previous menu." },
								{ -1 }
						},
				},
		[MENU_SOUND] = {
			.header = "Sound",
			.items = {
				{ MENU_ITEM_MUSIC_VOLUME, "Music Volume", "Change volume with the left/right arrow keys." },
				{ MENU_ITEM_SOUND_VOLUME, "Sound Volume", "Change volume with the left/right arrow keys." },
				{ MENU_ITEM_MUSIC_DEVICE, "Music Synth:", "Synthesizer for music (FluidSynth needs a .sf2).", getMusicDeviceItemsCount, getMusicDeviceItem },
				{ MENU_ITEM_DONE, "Done", "Return to the previous menu." },
				{ -1 }
			},
		},
		[MENU_ENHANCEMENTS] = {
			.header = "Enhancements",
			.items = {
				{ MENU_ITEM_SMOOTH_MOTION, "Smooth Motion:", "Interpolate motion for smooth high-refresh play." },
				{ MENU_ITEM_EXTRA_SPARKS, "Extra Sparks:", "Denser, longer-lasting explosion spark showers." },
				{ MENU_ITEM_DEBUG_MODE, "Debug Mode:", "Enable the debug menu and debug level select." },
				{ MENU_ITEM_ENEMY_BARS_MENU, "Enemy Bars...", "Health bars on enemies you've damaged." },
				{ MENU_ITEM_BOSS_BARS, "Boss Health Bars...", "Style and layout of the boss health bars." },
				{ MENU_ITEM_GAUGE_GRADS_MENU, "Gauge Gradients...", "Gradient direction of each HUD gauge." },
				{ MENU_ITEM_WEAPON_TWEAKS, "Weapon Tweaks...", "Zica Laser buff and the Charge-Laser Cannon." },
				{ MENU_ITEM_CUSTOM_WEAPONS, "Custom Weapons:", "Enable the custom weapon and its buy/sell slot." },
				{ MENU_ITEM_CUSTOM_CREATOR, "Custom Weapon Creator...", "Design your own weapon with a live preview." },
				{ MENU_ITEM_DONE, "Done", "Return to the previous menu." },
				{ -1 }
			},
		},
		[MENU_WEAPON_TWEAKS] = {
			.header = "Weapon Tweaks",
			.items = {
				{ MENU_ITEM_SUPERSPARKS, "Superspark Weapons...", "Weapons whose spark trails differ per episode." },
				{ MENU_ITEM_EPDIFFS, "Episode Differences...", "Other weapons that differ between Ep 1-3 and Ep 4-5." },
				{ MENU_ITEM_CHARGE_LASER, "Charge-Laser:", "Re-add the cut DOS charge sidekick to its shops." },
				{ MENU_ITEM_DONE, "Done", "Return to the previous menu." },
				{ -1 }
			},
		},
		[MENU_ZICA_LASER] = {
			.header = "Zica Laser",
			.items = {
				{ MENU_ITEM_ZICA_BASE, "Zica L11 Base:", "Lv11 shot pattern: Ep1-3 columns or Ep4 spread.", getZicaBaseItemsCount, getZicaBaseItem },
				{ MENU_ITEM_ZICA_LENGTH, "Zica L11 Length:", "Lv11 length; Long is as long as the L10 shot.", getZicaLengthItemsCount, getZicaLengthItem },
				{ MENU_ITEM_ZICA_LOCK, "Zica L11 Lock:", "Long beams follow the ship (Length=Long only)." },
				{ MENU_ITEM_ZICA_BUFF, "Zica L11 Buff:", "Also fire the L10 beam alongside the L11 shots." },
				{ MENU_ITEM_DONE, "Done", "Return to the previous menu." },
				{ -1 }
			},
		},
		[MENU_SUPERSPARKS] = {
			// Only the Tyrian 2000 (ep4/5) item data gives these weapons a superspark
			// projectile trail; each submenu forces that trail on/off per weapon.
			.header = "Superspark Weapons",
			.items = {
				{ MENU_ITEM_SPARKS_MEGA_PULSE, "Mega Pulse...", "Ep1-3 Mega Pulse spark trail and its cap." },
				{ MENU_ITEM_SPARKS_WALLOP, "Beno Wallop Beam...", "Ep1-3 Wallop Beam sidekick spark trail and cap." },
				{ MENU_ITEM_SPARKS_PROTRON_B, "Beno Protron -B-...", "Ep1-3 Protron -B- sidekick spark trail and cap." },
				{ MENU_ITEM_SPARKS_ICE, "Ice Beam / Blast...", "Ep1-3 Ice Beam/Blast special spark trail and cap." },
				{ MENU_ITEM_DONE, "Done", "Return to the previous menu." },
				{ -1 }
			},
		},
// The per-weapon submenus share their rows: which weapon they edit comes from
// currentSparkWeapon, set when the submenu is entered.
#define SPARK_MENU_ITEMS \
				{ MENU_ITEM_SPARKS_MODE, "Sparks:", "Trail: Auto (per-episode), On (all), or Off (none).", getSparkModeItemsCount, getSparkModeItem }, \
				{ MENU_ITEM_SPARKS_CAP, "Spark Cap:", "Force the classic spark limit despite Extra Sparks." }, \
				{ MENU_ITEM_DONE, "Done", "Return to the previous menu." }, \
				{ -1 }
		[MENU_SPARKS_MEGA_PULSE] = { .header = "Mega Pulse",       .items = { SPARK_MENU_ITEMS } },
		[MENU_SPARKS_WALLOP]     = {
			// Same shared rows plus the Wallop-only second-bolt toggle (the ep4/5 data
			// fires two bolts per volley; the ep1-3 data one).
			.header = "Beno Wallop Beam",
			.items = {
				{ MENU_ITEM_SPARKS_MODE, "Sparks:", "Trail: Auto (per-episode), On (all), or Off (none).", getSparkModeItemsCount, getSparkModeItem },
				{ MENU_ITEM_SPARKS_CAP, "Spark Cap:", "Force the classic spark limit despite Extra Sparks." },
				{ MENU_ITEM_WALLOP_BOLT, "2nd Bolt:", "Ep4/5 second bolt per volley: Auto, On, or Off.", getSparkModeItemsCount, getSparkModeItem },
				{ MENU_ITEM_DONE, "Done", "Return to the previous menu." },
				{ -1 }
			},
		},
		[MENU_SPARKS_PROTRON_B]  = { .header = "Beno Protron -B-", .items = { SPARK_MENU_ITEMS } },
		[MENU_SPARKS_ICE]        = { .header = "Ice Beam / Blast", .items = { SPARK_MENU_ITEMS } },
#undef SPARK_MENU_ITEMS
		[MENU_EPDIFFS] = {
			// Weapons whose ep1-3 vs ep4/5 item data differ beyond the spark trail (the Zica
			// Laser is one such weapon, so it lives here too). Each opens a per-weapon submenu.
			.header = "Episode Differences",
			.items = {
				{ MENU_ITEM_ZICA_LASER, "Zica Laser...", "Zica Laser Lv11 pattern, length, lock, and buff." },
				{ MENU_ITEM_EPDIFF_XEGA, "Xega Ball...", "Ep1-3 two weak balls vs Ep4-5 one strong ball." },
				{ MENU_ITEM_EPDIFF_MICROSOL, "MicroSol Option 5...", "Ep1-3 8-way fan vs Ep4-5 twin shot (MicroSol ship)." },
				{ MENU_ITEM_EPDIFF_FLARE, "Flare / Super Bomb...", "Which episode's blast sprite the Flare uses." },
				{ MENU_ITEM_EPDIFF_NEEDLE, "Needle Laser...", "Which episode's firing sound it uses." },
				{ MENU_ITEM_EPDIFF_BUBBLE, "Bubble Gum-Gun...", "Which episode's firing sound it uses." },
				{ MENU_ITEM_EPDIFF_PUNCH, "Flying Punch...", "Which episode's firing sound it uses." },
				{ MENU_ITEM_EPDIFF_PRETZEL, "Pretzel Missile...", "Which episode's firing sound it uses." },
				{ MENU_ITEM_EPDIFF_DRAGON, "Dragon Frost...", "Which episode's firing sound it uses." },
				{ MENU_ITEM_DONE, "Done", "Return to the previous menu." },
				{ -1 }
			},
		},
// The eight per-weapon submenus share their one row: which weapon "Version:" edits comes
// from currentDiffWeapon, set when the submenu is entered (ids/menu-ids mirror EpDiffWeapon).
#define EPDIFF_MENU_ITEMS \
				{ MENU_ITEM_EPDIFF_MODE, "Version:", "Auto (per-episode), or force the Ep 1-3 / Ep 4-5 data.", getZicaBaseItemsCount, getZicaBaseItem }, \
				{ MENU_ITEM_DONE, "Done", "Return to the previous menu." }, \
				{ -1 }
		[MENU_EPDIFF_XEGA]     = { .header = "Xega Ball",         .items = { EPDIFF_MENU_ITEMS } },
		[MENU_EPDIFF_MICROSOL] = { .header = "MicroSol Option 5", .items = { EPDIFF_MENU_ITEMS } },
		[MENU_EPDIFF_FLARE]    = { .header = "Flare / Super Bomb", .items = { EPDIFF_MENU_ITEMS } },
		[MENU_EPDIFF_NEEDLE]   = { .header = "Needle Laser",      .items = { EPDIFF_MENU_ITEMS } },
		[MENU_EPDIFF_BUBBLE]   = { .header = "Bubble Gum-Gun",    .items = { EPDIFF_MENU_ITEMS } },
		[MENU_EPDIFF_PUNCH]    = { .header = "Flying Punch",      .items = { EPDIFF_MENU_ITEMS } },
		[MENU_EPDIFF_PRETZEL]  = { .header = "Pretzel Missile",   .items = { EPDIFF_MENU_ITEMS } },
		[MENU_EPDIFF_DRAGON]   = { .header = "Dragon Frost",      .items = { EPDIFF_MENU_ITEMS } },
#undef EPDIFF_MENU_ITEMS
		[MENU_BOSS_BARS] = {
			.header = "Boss Health Bars",
			.items = {
				{ MENU_ITEM_BOSS_BAR_STYLE, "Boss Bars:", "Classic or redesigned boss health bars.", getBossBarStyleItemsCount, getBossBarStyleItem },
				{ MENU_ITEM_BOSS_BAR_LAYOUT, "Bar Layout:", "Top/Bottom (horizontal) or Left/Right (vertical).", getBossBarLayoutItemsCount, getBossBarLayoutItem },
				{ MENU_ITEM_BOSS_BAR_TWO, "Two Bars:", "How two boss bars are arranged.", getBossBarTwoItemsCount, getBossBarTwoItem },
				{ MENU_ITEM_DONE, "Done", "Return to the previous menu." },
				{ -1 }
			},
		},
		[MENU_ENEMY_BARS] = {
			.header = "Enemy Bars",
			.items = {
				{ MENU_ITEM_ENEMY_BARS, "Enemy Bars:", "Show a small health bar on enemies you've damaged." },
				{ MENU_ITEM_ENEMY_BAR_LAYOUT, "Layout:", "Horizontal or vertical bar.", getEnemyBarLayoutItemsCount, getEnemyBarLayoutItem },
				{ MENU_ITEM_ENEMY_BAR_POS, "Position:", "Where the bar sits relative to the enemy.", getEnemyBarPositionItemsCount, getEnemyBarPositionItem },
				{ MENU_ITEM_ENEMY_BAR_OPACITY, "Opacity:", "Bar transparency. Left/right to adjust." },
				{ MENU_ITEM_DONE, "Done", "Return to the previous menu." },
				{ -1 }
			},
		},
		[MENU_GAUGE_GRADIENTS] = {
			.header = "Gauge Gradients",
			.items = {
				{ MENU_ITEM_GAUGE_GRAD_GEN, "Generator:", "Generator gauge gradient direction (bright end).", getGaugeGradItemsCount, getGaugeGradItem },
				{ MENU_ITEM_GAUGE_GRAD_SHIELD, "Shield:", "Shield gauge gradient direction (bright end).", getGaugeGradItemsCount, getGaugeGradItem },
				{ MENU_ITEM_GAUGE_GRAD_ARMOR, "Armor:", "Armor gauge gradient direction (bright end).", getGaugeGradItemsCount, getGaugeGradItem },
				{ MENU_ITEM_DONE, "Done", "Return to the previous menu." },
				{ -1 }
			},
		},
		[MENU_EXTRA] = {
			.header = "Extra",
			.items = {
				{ MENU_ITEM_JUKEBOX, "Jukebox", "Listen to the music of Tyrian." },
				{ MENU_ITEM_DESTRUCT, "Destruct", "Play the secret Destruct mini-game." },
				{ MENU_ITEM_SUPERTYRIAN, "SuperTyrian", "Play the tougher SuperTyrian mode." },
			{ MENU_ITEM_ARCADE_MENU, "Super Arcade...", "Play as one of the secret Super Arcade ships." },
				{ MENU_ITEM_CMDLINE_MENU, "Command Line...", "Toggle the command-line cheat options." },
				{ MENU_ITEM_XMAS, "Christmas Mode:", "Festive graphics and voices." },
				{ MENU_ITEM_DONE, "Done", "Return to the main menu." },
				{ -1 }
			},
		},
		[MENU_ARCADE] = {
			.header = "Super Arcade",
			.items = {
				// name = the title-screen code (specialName[i]); the ship name
				// (superShips[i+1]) is the description shown at the bottom of the screen.
				{ MENU_ITEM_ARCADE_SHIP_BASE + 0, specialName[0], superShips[1] },
				{ MENU_ITEM_ARCADE_SHIP_BASE + 1, specialName[1], superShips[2] },
				{ MENU_ITEM_ARCADE_SHIP_BASE + 2, specialName[2], superShips[3] },
				{ MENU_ITEM_ARCADE_SHIP_BASE + 3, specialName[3], superShips[4] },
				{ MENU_ITEM_ARCADE_SHIP_BASE + 4, specialName[4], superShips[5] },
				{ MENU_ITEM_ARCADE_SHIP_BASE + 5, specialName[5], superShips[6] },
				{ MENU_ITEM_ARCADE_SHIP_BASE + 6, specialName[6], superShips[7] },
				{ MENU_ITEM_ARCADE_SHIP_BASE + 7, specialName[7], superShips[8] },
				{ MENU_ITEM_ARCADE_SHIP_BASE + 8, specialName[8], superShips[9] },
				{ MENU_ITEM_DONE, "Done", "Return to the previous menu." },
				{ -1 }
			},
		},
		[MENU_CMDLINE] = {
			.header = "Command Line",
			.items = {
				{ MENU_ITEM_RICH_MODE, "Rich Mode:", "Start a new game with maximum money (LOOT)." },
				{ MENU_ITEM_CONSTANT_PLAY, "Constant Play:", "Testing mode; the C key toggles invincibility (CONSTANT)." },
				{ MENU_ITEM_CONSTANT_DIE, "Constant Die:", "Testing mode: ship constantly self-destructs (DEATH)." },
				{ MENU_ITEM_DONE, "Done", "Return to the previous menu." },
				{ -1 }
			},
		},
	};

	char buffer[100];

	if (shopSpriteSheet.data == NULL)
		JE_loadCompShapes(&shopSpriteSheet, '1');  // need mouse pointer sprites

	bool restart = true;

	MenuId menuParents[COUNTOF(menus)] = { MENU_NONE };
	size_t selectedMenuItemIndexes[COUNTOF(menus)] = { 0 };
	MenuId currentMenu = startMenu;
	MenuItemId currentPicker = MENU_ITEM_NONE;
	size_t pickerSelectedIndex = 0;

	/* See comment in JE_helpSystem regarding the virtual screen width. */
	const int xCenter = 320 / 2;
	const int yMenuHeader = 4;
	const int wMenuItemName = 135;
	const int wMenuItemValue = 95;
	const int wMenuItem = wMenuItemName + wMenuItemValue;
	const int xMenuItem = xCenter - wMenuItem / 2;
	const int xMenuItemName = xMenuItem;
	const int xMenuItemValue = xMenuItemName + wMenuItemName;
	const int yMenuItems = 37;
	int dyMenuItems = 21;  // row pitch; compressed when a menu has many rows (see below)
	const int hMenuItem = 13;

	for (; ; )
	{
		if (restart)
		{
			JE_loadPic(VGAScreen2, 2, false);
			fill_rectangle_wh(VGAScreen2, 0, 192, vga_width, 8, 0);
		}

		// Restore background.
		memcpy(VGAScreen->pixels, VGAScreen2->pixels, (size_t)VGAScreen->pitch * VGAScreen->h);

		const Menu *menu = &menus[currentMenu];

		// Draw header.
		draw_font_hv_shadow(VGAScreen, xCenter, yMenuHeader, menu->header, large_font, centered, 15, -3, false, 2);

		int yPicker = 0;
		const int dyPickerItem = 15;
		const int dyPickerItemPadding = 2;
		const int hPickerItem = dyPickerItem - dyPickerItemPadding;

		size_t *const selectedMenuItemIndex = &selectedMenuItemIndexes[currentMenu];
		const MenuItem *menuItems = menu->items;

		// Count the rows up front and tighten the pitch when the classic 21px
		// spacing would run off the bottom (the Graphics menu outgrew it): fit the
		// last row's baseline within y<=172 so its 13px height clears the bottom
		// text strip at y=192.
		size_t menuItemsCount = 0;
		while (menuItems[menuItemsCount].id != (MenuItemId)-1)
			++menuItemsCount;
		dyMenuItems = 21;
		if (menuItemsCount > 1 && yMenuItems + dyMenuItems * (int)(menuItemsCount - 1) > 172)
			dyMenuItems = (172 - yMenuItems) / (int)(menuItemsCount - 1);

		// Draw menu items.

		for (size_t i = 0; i < menuItemsCount; ++i)
		{
			const MenuItem *const menuItem = &menuItems[i];

			const int y = yMenuItems + dyMenuItems * i;

			const bool selected = i == *selectedMenuItemIndex;
			const bool disabled = currentPicker != MENU_ITEM_NONE && !selected;

			if (selected)
				yPicker = y;

			const char *const name = menuItem->name;

			draw_font_hv_shadow(VGAScreen, xMenuItemName, y, name, normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);

			switch (menuItem->id)
			{
			case MENU_ITEM_DISPLAY:;
				const char *value = "Window";
				if (fullscreen_display >= 0)
				{
					snprintf(buffer, sizeof(buffer), "Display %d", fullscreen_display + 1);
					value = buffer;
				}

				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, value, normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_SCALER:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, scalers[scaler].name, normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_SCALING_MODE:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, scaling_mode_names[scaling_mode], normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_SUPERSAMPLE:
				if (render_supersample == 0)
					snprintf(buffer, sizeof(buffer), "Auto (%dx)", effective_supersample());
				else
					snprintf(buffer, sizeof(buffer), "%s", supersampleNames[render_supersample]);
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, buffer, normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_SS_FILTER:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, ssFilterNames[render_supersample_filter], normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_FPS:
				if (fps_cap == 0)
					snprintf(buffer, sizeof(buffer), "Uncapped");
				else
					snprintf(buffer, sizeof(buffer), "%d", fps_cap);
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, buffer, normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_VSYNC:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, output_vsync ? "On" : "Off", normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_SHOW_FPS:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, show_fps ? "On" : "Off", normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_MUSIC_VOLUME:
				JE_barDrawShadow(VGAScreen, xMenuItemValue, y, 1, music_disabled ? 170 : 174, (tyrMusicVolume + 4) / 8, 2, 10);
				JE_rectangle(VGAScreen, xMenuItemValue - 2, y - 2, xMenuItemValue + 96, y + 11, 242);
				break;

			case MENU_ITEM_SOUND_VOLUME:
				JE_barDrawShadow(VGAScreen, xMenuItemValue, y, 1, samples_disabled ? 170 : 174, (fxVolume + 4) / 8, 2, 10);
				JE_rectangle(VGAScreen, xMenuItemValue - 2, y - 2, xMenuItemValue + 96, y + 11, 242);
				break;

			case MENU_ITEM_TOUCH_SENS:
			{
				// Same bar as the volume sliders; middle == the classic touch feel. The marker slot
				// goes bright once the fill actually reaches it -- compare the drawn bar counts
				// (amt vs mark), not the raw value, so it flips exactly on the middle bar.
				const int amt = (touch_sensitivity + 4) / 8;
				const int mark = (TOUCH_SENS_DEFAULT + 4) / 8;
				JE_barDrawShadow(VGAScreen, xMenuItemValue, y, 1, 174, amt, 2, 10);
				JE_barDrawMark(VGAScreen, xMenuItemValue, y,
				               amt >= mark ? TOUCH_SENS_MARK_COL : TOUCH_SENS_MARK_COL_DIM, mark, 2, 10);
				JE_rectangle(VGAScreen, xMenuItemValue - 2, y - 2, xMenuItemValue + 96, y + 11, 242);
				break;
			}

			case MENU_ITEM_MUSIC_DEVICE:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, music_device_names[music_device], normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_BOSS_BAR_STYLE:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, bossBarStyleNames[bossBarStyle], normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_BOSS_BAR_LAYOUT:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, bossBarLayoutNames[bossBarLayout], normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_BOSS_BAR_TWO:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, bossBarTwoNames[bossBarTwoMode], normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_ENEMY_BARS:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, enemyBars ? "On" : "Off", normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_ENEMY_BAR_LAYOUT:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, enemyBarLayoutNames[enemyBarLayout], normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_ENEMY_BAR_POS:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, enemyBarPositionNames[enemyBarPosition], normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_ENEMY_BAR_OPACITY:
				snprintf(buffer, sizeof(buffer), "%d%%", enemyBarOpacity);
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, buffer, normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_GAUGE_GRAD_GEN:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, gaugeGradNames[gaugeGradGenerator], normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_GAUGE_GRAD_SHIELD:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, gaugeGradNames[gaugeGradShield], normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_GAUGE_GRAD_ARMOR:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, gaugeGradNames[gaugeGradArmor], normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_DEBUG_MODE:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, debugMode ? "On" : "Off", normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_SMOOTH_MOTION:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, smoothMotion ? "On" : "Off", normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_EXTRA_SPARKS:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, extraSparks ? "On" : "Off", normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_XMAS:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, xmas ? "On" : "Off", normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_ZICA_BASE:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, zicaBaseNames[zicaLaserBase], normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_ZICA_LENGTH:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, zicaLengthNames[zicaLaserLength], normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_ZICA_LOCK:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, zicaLaserLock ? "On" : "Off", normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_ZICA_BUFF:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, zicaLaserBuff ? "On" : "Off", normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_SPARKS_MODE:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, sparkModeNames[superSparkMode[currentSparkWeapon]], normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_SPARKS_CAP:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, superSparkClassicCap[currentSparkWeapon] ? "On" : "Off", normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_WALLOP_BOLT:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, sparkModeNames[wallopSecondBolt], normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_EPDIFF_MODE:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, zicaBaseNames[epDiffMode[currentDiffWeapon]], normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_CHARGE_LASER:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, chargeLaserCannon ? "On" : "Off", normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_CUSTOM_WEAPONS:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, customWeaponEnabled ? "On" : "Off", normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_RICH_MODE:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, richMode ? "On" : "Off", normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_CONSTANT_PLAY:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, constantPlay ? "On" : "Off", normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_CONSTANT_DIE:
				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, constantDie ? "On" : "Off", normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			default:
				// Super Arcade ship rows use the code as the name and have no value.
				break;
			}
		}

		// Draw status text. The Music Synth row shows live SoundFont status so the
		// user can confirm FluidSynth actually picked up their .sf2 (see loudness.c).
		const char *statusText = menuItems[*selectedMenuItemIndex].description;
		char musicSynthStatus[128];
		if (menuItems[*selectedMenuItemIndex].id == MENU_ITEM_MUSIC_DEVICE)
		{
			if (music_device == FLUIDSYNTH)
			{
				if (midi_soundfont_loaded)
					// %.34s caps a long filename so the one-line status stays on-screen (~54 chars).
					snprintf(musicSynthStatus, sizeof(musicSynthStatus), "SoundFont loaded: %.34s", soundfont_basename());
				else
					snprintf(musicSynthStatus, sizeof(musicSynthStatus), "No SoundFont found -- put a .sf2 in the data folder.");
				statusText = musicSynthStatus;
			}
			else if (music_device == NATIVE_MIDI)
			{
				statusText = "Native OS synth -- ignores custom SoundFonts.";
			}
		}
		JE_textShade(VGAScreen, xMenuItemName, 190, statusText, 15, 4, PART_SHADE);

		// Draw picker box and items.

		if (currentPicker != MENU_ITEM_NONE)
		{
			const MenuItem *selectedMenuItem = &menuItems[*selectedMenuItemIndex];
			const size_t pickerItemsCount = selectedMenuItem->getPickerItemsCount();

			const int hPicker = dyPickerItem * pickerItemsCount - dyPickerItemPadding;
			yPicker = MIN(yPicker, 200 - 10 - (hPicker + 5 + 2));

			JE_rectangle(VGAScreen, xMenuItemValue - 5, yPicker- 3, xMenuItemValue + wMenuItemValue + 5 - 1, yPicker + hPicker + 3 - 1, 248);
			JE_rectangle(VGAScreen, xMenuItemValue - 4, yPicker- 4, xMenuItemValue + wMenuItemValue + 4 - 1, yPicker + hPicker + 4 - 1, 250);
			JE_rectangle(VGAScreen, xMenuItemValue - 3, yPicker- 5, xMenuItemValue + wMenuItemValue + 3 - 1, yPicker + hPicker + 5 - 1, 248);
			fill_rectangle_wh(VGAScreen, xMenuItemValue - 2, yPicker - 2, wMenuItemValue + 2 + 2, hPicker + 2 + 2, 224);

			for (size_t i = 0; i < pickerItemsCount; ++i)
			{
				const int y = yPicker + dyPickerItem * (int)i;

				const bool selected = i == pickerSelectedIndex;

				// Algorithm scalers are unavailable while Sub-pixel is on (the hi
				// path bypasses them in-game); gray them out.
				const bool grayed = currentPicker == MENU_ITEM_SCALER
				                 && render_supersample != 1 && !scaler_is_plain((uint)i);

				const char *value = selectedMenuItem->getPickerItem(i, buffer, sizeof buffer);

				draw_font_hv_shadow(VGAScreen, xMenuItemValue, y, value, normal_font, left_aligned, 15, -3 + (selected ? 2 : 0) + (grayed ? -4 : 0), false, 2);
			}
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

		bool mouseMoved = false;
		int oldFullscreenDisplay = fullscreen_display;
		do
		{
			SDL_Delay(1);  // fine poll so the cursor redraws at display rate on motion

			Uint16 oldMouseX = mouse_x;
			Uint16 oldMouseY = mouse_y;

			push_joysticks_as_keyboard();
			service_SDL_events(false);

			mouseMoved = mouse_x != oldMouseX || mouse_y != oldMouseY;
		} while (!(newkey || newmouse || mouseMoved || fullscreen_display != oldFullscreenDisplay));

		if (currentPicker == MENU_ITEM_NONE)
		{
			// Handle menu item interaction.

			bool action = false;

			if (mouseMoved || newmouse)
			{
				// Find menu item name or value that was hovered or clicked.
				if (mouse_x >= xMenuItem && mouse_x < xMenuItem + wMenuItem)
				{
					for (size_t i = 0; i < menuItemsCount; ++i)
					{
						const int yMenuItem = yMenuItems + dyMenuItems * i;
						if (mouse_y >= yMenuItem && mouse_y < yMenuItem + hMenuItem)
						{
							if (*selectedMenuItemIndex != i)
							{
								JE_playSampleNum(S_CURSOR);

								*selectedMenuItemIndex = i;
							}

							if (newmouse && lastmouse_but == SDL_BUTTON_LEFT &&
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
									switch (menuItems[*selectedMenuItemIndex].id)
									{
									case MENU_ITEM_DISPLAY:
									case MENU_ITEM_SCALER:
									case MENU_ITEM_SCALING_MODE:
									case MENU_ITEM_SUPERSAMPLE:
									case MENU_ITEM_SS_FILTER:
									case MENU_ITEM_FPS:
									case MENU_ITEM_VSYNC:
									case MENU_ITEM_SHOW_FPS:
									case MENU_ITEM_MUSIC_DEVICE:
									case MENU_ITEM_BOSS_BAR_STYLE:
									case MENU_ITEM_BOSS_BAR_LAYOUT:
									case MENU_ITEM_BOSS_BAR_TWO:
									case MENU_ITEM_ENEMY_BARS:
									case MENU_ITEM_ENEMY_BAR_LAYOUT:
									case MENU_ITEM_ENEMY_BAR_POS:
									case MENU_ITEM_GAUGE_GRAD_GEN:
									case MENU_ITEM_GAUGE_GRAD_SHIELD:
									case MENU_ITEM_GAUGE_GRAD_ARMOR:
									case MENU_ITEM_DEBUG_MODE:
									case MENU_ITEM_SMOOTH_MOTION:
									case MENU_ITEM_EXTRA_SPARKS:
									case MENU_ITEM_XMAS:
									case MENU_ITEM_SPARKS_MODE:
									case MENU_ITEM_SPARKS_CAP:
									case MENU_ITEM_WALLOP_BOLT:
									case MENU_ITEM_EPDIFF_MODE:
									case MENU_ITEM_RICH_MODE:
									case MENU_ITEM_CONSTANT_PLAY:
									case MENU_ITEM_CONSTANT_DIE:
									{
										action = true;
										break;
									}
									case MENU_ITEM_ENEMY_BAR_OPACITY:
									{
										JE_playSampleNum(S_CURSOR);

										int value = (lastmouse_x - xMenuItemValue) * 100 / (wMenuItemValue - 1);
										enemyBarOpacity = MIN(MAX(0, value), 100);
										break;
									}
									case MENU_ITEM_MUSIC_VOLUME:
									{
										JE_playSampleNum(S_CURSOR);

										int value = (lastmouse_x - xMenuItemValue) * 255 / (wMenuItemValue - 1);
										tyrMusicVolume = MIN(MAX(0, value), 255);

										set_volume(tyrMusicVolume, fxVolume);
										break;
									}
									case MENU_ITEM_SOUND_VOLUME:
									{
										int value = (lastmouse_x - xMenuItemValue) * 255 / (wMenuItemValue - 1);
										fxVolume = MIN(MAX(0, value), 255);

										set_volume(tyrMusicVolume, fxVolume);

										JE_playSampleNum(S_CURSOR);
										break;
									}
									case MENU_ITEM_TOUCH_SENS:
									{
										int value = (lastmouse_x - xMenuItemValue) * TOUCH_SENS_MAX / (wMenuItemValue - 1);
										touch_sensitivity = MIN(MAX(0, value), TOUCH_SENS_MAX);

										JE_playSampleNum(S_CURSOR);
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

					currentMenu = menuParents[currentMenu];
				}
			}
			else if (newkey)
			{
				switch (lastkey_scan)
				{
				case SDL_SCANCODE_UP:
				{
					JE_playSampleNum(S_CURSOR);

					*selectedMenuItemIndex = *selectedMenuItemIndex == 0
						? menuItemsCount - 1
						: *selectedMenuItemIndex - 1;
					break;
				}
				case SDL_SCANCODE_DOWN:
				{
					JE_playSampleNum(S_CURSOR);

					*selectedMenuItemIndex = *selectedMenuItemIndex == menuItemsCount - 1
						? 0
						: *selectedMenuItemIndex + 1;
					break;
				}
				case SDL_SCANCODE_LEFT:
				{
					adjustMenuItemValue(menuItems[*selectedMenuItemIndex].id, -1);
					break;
				}
				case SDL_SCANCODE_RIGHT:
				{
					adjustMenuItemValue(menuItems[*selectedMenuItemIndex].id, +1);
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

					currentMenu = menuParents[currentMenu];
					break;
				}
				default:
					break;
				}
			}

			if (action)
			{
				const MenuItemId selectedMenuItemId = menuItems[*selectedMenuItemIndex].id;

				switch (selectedMenuItemId)
				{
				case MENU_ITEM_DONE:
				{
					JE_playSampleNum(S_SELECT);

					currentMenu = menuParents[currentMenu];
					break;
				}
				case MENU_ITEM_GRAPHICS:
				{
					JE_playSampleNum(S_SELECT);

					menuParents[MENU_GRAPHICS] = currentMenu;
					currentMenu = MENU_GRAPHICS;
					selectedMenuItemIndexes[currentMenu] = 0;
					break;
				}
				case MENU_ITEM_SOUND:
				{
					JE_playSampleNum(S_SELECT);

					menuParents[MENU_SOUND] = currentMenu;
					currentMenu = MENU_SOUND;
					selectedMenuItemIndexes[currentMenu] = 0;
					break;
				}
				case MENU_ITEM_ENHANCEMENTS:
				{
					JE_playSampleNum(S_SELECT);

					menuParents[MENU_ENHANCEMENTS] = currentMenu;
					currentMenu = MENU_ENHANCEMENTS;
					selectedMenuItemIndexes[currentMenu] = 0;
					break;
				}
				case MENU_ITEM_BOSS_BARS:
				{
					JE_playSampleNum(S_SELECT);

					menuParents[MENU_BOSS_BARS] = currentMenu;
					currentMenu = MENU_BOSS_BARS;
					selectedMenuItemIndexes[currentMenu] = 0;
					break;
				}
				case MENU_ITEM_ENEMY_BARS_MENU:
				{
					JE_playSampleNum(S_SELECT);

					menuParents[MENU_ENEMY_BARS] = currentMenu;
					currentMenu = MENU_ENEMY_BARS;
					selectedMenuItemIndexes[currentMenu] = 0;
					break;
				}
				case MENU_ITEM_GAUGE_GRADS_MENU:
				{
					JE_playSampleNum(S_SELECT);

					menuParents[MENU_GAUGE_GRADIENTS] = currentMenu;
					currentMenu = MENU_GAUGE_GRADIENTS;
					selectedMenuItemIndexes[currentMenu] = 0;
					break;
				}
				case MENU_ITEM_WEAPON_TWEAKS:
				{
					JE_playSampleNum(S_SELECT);

					menuParents[MENU_WEAPON_TWEAKS] = currentMenu;
					currentMenu = MENU_WEAPON_TWEAKS;
					selectedMenuItemIndexes[currentMenu] = 0;
					break;
				}
				case MENU_ITEM_ZICA_LASER:
				{
					JE_playSampleNum(S_SELECT);

					menuParents[MENU_ZICA_LASER] = currentMenu;
					currentMenu = MENU_ZICA_LASER;
					selectedMenuItemIndexes[currentMenu] = 0;
					break;
				}
				case MENU_ITEM_SUPERSPARKS:
				{
					JE_playSampleNum(S_SELECT);

					menuParents[MENU_SUPERSPARKS] = currentMenu;
					currentMenu = MENU_SUPERSPARKS;
					selectedMenuItemIndexes[currentMenu] = 0;
					break;
				}
				case MENU_ITEM_SPARKS_MEGA_PULSE:
				case MENU_ITEM_SPARKS_WALLOP:
				case MENU_ITEM_SPARKS_PROTRON_B:
				case MENU_ITEM_SPARKS_ICE:
				{
					JE_playSampleNum(S_SELECT);

					// The submenus' item ids are shared; remember which weapon they edit.
					// Both id and menu-id runs mirror the SuperSparkWeapon enum order.
					currentSparkWeapon = SSW_MEGA_PULSE + (selectedMenuItemId - MENU_ITEM_SPARKS_MEGA_PULSE);
					const MenuId sparkMenu = MENU_SPARKS_MEGA_PULSE + (selectedMenuItemId - MENU_ITEM_SPARKS_MEGA_PULSE);
					menuParents[sparkMenu] = currentMenu;
					currentMenu = sparkMenu;
					selectedMenuItemIndexes[currentMenu] = 0;
					break;
				}
				case MENU_ITEM_EPDIFFS:
				{
					JE_playSampleNum(S_SELECT);

					menuParents[MENU_EPDIFFS] = currentMenu;
					currentMenu = MENU_EPDIFFS;
					selectedMenuItemIndexes[currentMenu] = 0;
					break;
				}
				case MENU_ITEM_EPDIFF_XEGA:
				case MENU_ITEM_EPDIFF_MICROSOL:
				case MENU_ITEM_EPDIFF_FLARE:
				case MENU_ITEM_EPDIFF_NEEDLE:
				case MENU_ITEM_EPDIFF_BUBBLE:
				case MENU_ITEM_EPDIFF_PUNCH:
				case MENU_ITEM_EPDIFF_PRETZEL:
				case MENU_ITEM_EPDIFF_DRAGON:
				{
					JE_playSampleNum(S_SELECT);

					// The submenus' one row is shared; remember which weapon it edits.
					// Both id and menu-id runs mirror the EpDiffWeapon enum order.
					currentDiffWeapon = EDW_XEGA_BALL + (selectedMenuItemId - MENU_ITEM_EPDIFF_XEGA);
					const MenuId diffMenu = MENU_EPDIFF_XEGA + (selectedMenuItemId - MENU_ITEM_EPDIFF_XEGA);
					menuParents[diffMenu] = currentMenu;
					currentMenu = diffMenu;
					selectedMenuItemIndexes[currentMenu] = 0;
					break;
				}
				case MENU_ITEM_ARCADE_MENU:
				{
					JE_playSampleNum(S_SELECT);

					menuParents[MENU_ARCADE] = currentMenu;
					currentMenu = MENU_ARCADE;
					selectedMenuItemIndexes[currentMenu] = 0;
					break;
				}
				case MENU_ITEM_CMDLINE_MENU:
				{
					JE_playSampleNum(S_SELECT);

					menuParents[MENU_CMDLINE] = currentMenu;
					currentMenu = MENU_CMDLINE;
					selectedMenuItemIndexes[currentMenu] = 0;
					break;
				}
				case MENU_ITEM_JUKEBOX:
				{
					JE_playSampleNum(S_SELECT);

					fade_black(10);
					set_menu_centered(false);

					jukebox();

					set_menu_centered(true);
					restart = true;
					break;
				}
				case MENU_ITEM_DESTRUCT:
				{
					JE_playSampleNum(S_SELECT);
					fade_black(10);

					set_menu_centered(false);
					JE_destructGame();
					set_menu_centered(true);

					restart = true;
					break;
				}
				case MENU_ITEM_SUPERTYRIAN:
				{
					JE_playSampleNum(S_SELECT);
					fade_black(10);

					if (newSuperTyrianGame())
						return true;  // game launched; the title screen starts it

					set_menu_centered(true);
					restart = true;
					break;
				}
				case MENU_ITEM_DISPLAY:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = (size_t)(fullscreen_display + 1);
					break;
				}
				case MENU_ITEM_SCALER:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = scaler;
					break;
				}
				case MENU_ITEM_SCALING_MODE:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = scaling_mode;
					break;
				}
				case MENU_ITEM_SUPERSAMPLE:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = (size_t)render_supersample;
					break;
				}
				case MENU_ITEM_SS_FILTER:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = (size_t)render_supersample_filter;
					break;
				}
				case MENU_ITEM_MUSIC_DEVICE:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = music_device;
					break;
				}
				case MENU_ITEM_BOSS_BAR_STYLE:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = bossBarStyle;
					break;
				}
				case MENU_ITEM_BOSS_BAR_LAYOUT:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = bossBarLayout;
					break;
				}
				case MENU_ITEM_BOSS_BAR_TWO:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = bossBarTwoMode;
					break;
				}
				case MENU_ITEM_ENEMY_BAR_LAYOUT:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = enemyBarLayout;
					break;
				}
				case MENU_ITEM_ENEMY_BAR_POS:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = enemyBarPosition;
					break;
				}
				case MENU_ITEM_GAUGE_GRAD_GEN:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = gaugeGradGenerator;
					break;
				}
				case MENU_ITEM_GAUGE_GRAD_SHIELD:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = gaugeGradShield;
					break;
				}
				case MENU_ITEM_GAUGE_GRAD_ARMOR:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = gaugeGradArmor;
					break;
				}
				case MENU_ITEM_MUSIC_VOLUME:
				{
					JE_playSampleNum(S_CLICK);

					set_music_disabled(!music_disabled);
					if (!music_disabled)
						restart_song();
					break;
				}
				case MENU_ITEM_SOUND_VOLUME:
				{
					samples_disabled = !samples_disabled;

					JE_playSampleNum(S_CLICK);
					break;
				}
				case MENU_ITEM_VSYNC:
				{
					set_vsync(!output_vsync);
					JE_playSampleNum(S_CLICK);
					break;
				}
				case MENU_ITEM_SHOW_FPS:
				{
					show_fps = !show_fps;
					JE_playSampleNum(S_CLICK);
					break;
				}
				case MENU_ITEM_ENEMY_BARS:
				{
					enemyBars = !enemyBars;
					JE_playSampleNum(S_CLICK);
					break;
				}
				case MENU_ITEM_DEBUG_MODE:
				{
					debugMode = !debugMode;
					JE_playSampleNum(S_CLICK);
					break;
				}
				case MENU_ITEM_SMOOTH_MOTION:
				{
					smoothMotion = !smoothMotion;
					JE_playSampleNum(S_CLICK);
					break;
				}
				case MENU_ITEM_EXTRA_SPARKS:
				{
					extraSparks = !extraSparks;
					JE_playSampleNum(S_CLICK);
					break;
				}
				case MENU_ITEM_XMAS:
				{
					JE_playSampleNum(toggle_xmas_mode() ? S_CLICK : S_SPRING);
					break;
				}
				case MENU_ITEM_ZICA_BASE:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = zicaLaserBase;
					break;
				}
				case MENU_ITEM_ZICA_LENGTH:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = zicaLaserLength;
					break;
				}
				case MENU_ITEM_ZICA_LOCK:
				{
					zicaLaserLock = !zicaLaserLock;
					JE_playSampleNum(S_CLICK);
					break;
				}
				case MENU_ITEM_ZICA_BUFF:
				{
					zicaLaserBuff = !zicaLaserBuff;
					JE_playSampleNum(S_CLICK);
					break;
				}
				case MENU_ITEM_SPARKS_MODE:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = superSparkMode[currentSparkWeapon];
					break;
				}
				case MENU_ITEM_SPARKS_CAP:
				{
					superSparkClassicCap[currentSparkWeapon] = !superSparkClassicCap[currentSparkWeapon];
					JE_playSampleNum(S_CLICK);
					break;
				}
				case MENU_ITEM_WALLOP_BOLT:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = wallopSecondBolt;
					break;
				}
				case MENU_ITEM_EPDIFF_MODE:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = epDiffMode[currentDiffWeapon];
					break;
				}
				case MENU_ITEM_CHARGE_LASER:
				{
					chargeLaserCannon = !chargeLaserCannon;
					JE_playSampleNum(S_CLICK);
					break;
				}
				case MENU_ITEM_CUSTOM_WEAPONS:
				{
					customWeaponEnabled = !customWeaponEnabled;
					JE_playSampleNum(S_CLICK);
					break;
				}
				case MENU_ITEM_CUSTOM_CREATOR:
				{
					JE_playSampleNum(S_SELECT);
					JE_customWeaponCreator(false);  // Setup context: design only (no active ship to equip)
					restart = true;
					break;
				}
				case MENU_ITEM_RICH_MODE:
				{
					richMode = !richMode;
					JE_playSampleNum(S_CLICK);
					break;
				}
				case MENU_ITEM_CONSTANT_PLAY:
				{
					constantPlay = !constantPlay;
					JE_playSampleNum(S_CLICK);
					break;
				}
				case MENU_ITEM_CONSTANT_DIE:
				{
					constantDie = !constantDie;
					JE_playSampleNum(S_CLICK);
					break;
				}
				default:
					// Super Arcade ship rows: launch that ship's game.
					if (selectedMenuItemId >= MENU_ITEM_ARCADE_SHIP_BASE)
					{
						JE_playSampleNum(S_SELECT);
						fade_black(10);

						if (newSuperArcadeGame(selectedMenuItemId - MENU_ITEM_ARCADE_SHIP_BASE))
							return true;  // game launched; the title screen starts it

						set_menu_centered(true);
						restart = true;
					}
					break;
				}
			}

			if (currentMenu == MENU_NONE)
			{
				// Persist every setting changed in this menu now (Show FPS, vsync, volumes,
				// supersample, ...). On Switch the app is normally closed via the HOME menu,
				// which never runs the clean-exit save, so otherwise the changes never stick.
				save_opentyrian_config();

				fade_black(10);

				return false;
			}
		}
		else
		{
			const MenuItem *selectedMenuItem = &menuItems[*selectedMenuItemIndex];

			// Handle picker interaction.

			bool action = false;

			if (mouseMoved || newmouse)
			{
				const size_t pickerItemsCount = selectedMenuItem->getPickerItemsCount();

				// Find picker item that was hovered clicked.
				if (mouse_x >= xMenuItemValue && mouse_x < xMenuItemValue + wMenuItemValue)
				{
					for (size_t i = 0; i < pickerItemsCount; ++i)
					{
						const int yPickerItem = yPicker + dyPickerItem * i;

						if (mouse_y >= yPickerItem && mouse_y < yPickerItem + hPickerItem)
						{
							if (pickerSelectedIndex != i)
							{
								JE_playSampleNum(S_CURSOR);

								pickerSelectedIndex = i;
							}

							// Act on picker item.
							if (newmouse && lastmouse_but == SDL_BUTTON_LEFT &&
							    lastmouse_x >= xMenuItemValue && lastmouse_y < xMenuItemValue + wMenuItemName &&
							    lastmouse_y >= yPickerItem && lastmouse_y < yPickerItem + hPickerItem)
							{
								action = true;
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

					currentPicker = MENU_ITEM_NONE;
				}
			}
			else if (newkey)
			{
				switch (lastkey_scan)
				{
				case SDL_SCANCODE_UP:
				{
					JE_playSampleNum(S_CURSOR);

					const size_t pickerItemsCount = selectedMenuItem->getPickerItemsCount();

					pickerSelectedIndex = pickerSelectedIndex == 0
						? pickerItemsCount - 1
						: pickerSelectedIndex - 1;
					break;
				}
				case SDL_SCANCODE_DOWN:
				{
					JE_playSampleNum(S_CURSOR);

					const size_t pickerItemsCount = selectedMenuItem->getPickerItemsCount();

					pickerSelectedIndex = pickerSelectedIndex == pickerItemsCount - 1
						? 0
						: pickerSelectedIndex + 1;
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

					currentPicker = MENU_ITEM_NONE;
					break;
				}
				default:
					break;
				}
			}

			if (action)
			{
				JE_playSampleNum(S_CLICK);

				switch (selectedMenuItem->id)
				{
				case MENU_ITEM_DISPLAY:
				{
					if ((int)pickerSelectedIndex - 1 != fullscreen_display)
						reinit_fullscreen((int)pickerSelectedIndex - 1);
					break;
				}
				case MENU_ITEM_SCALER:
				{
					// Algorithm scalers are grayed out while Sub-pixel is on; refuse.
					if (render_supersample != 1 && !scaler_is_plain(pickerSelectedIndex))
					{
						JE_playSampleNum(S_SPRING);
						break;
					}
					if (pickerSelectedIndex != scaler)
					{
						const int oldScaler = scaler;
						if (!init_scaler(pickerSelectedIndex) &&  // try new scaler
							!init_scaler(oldScaler))              // revert on fail
						{
							exit(EXIT_FAILURE);
						}
					}
					break;
				}
				case MENU_ITEM_SCALING_MODE:
				{
					scaling_mode = pickerSelectedIndex;
					break;
				}
				case MENU_ITEM_SUPERSAMPLE:
				{
					render_supersample = (int)pickerSelectedIndex;
					enforcePlainScalerForSupersample();
					break;
				}
				case MENU_ITEM_SS_FILTER:
				{
					render_supersample_filter = (int)pickerSelectedIndex;
					break;
				}
				case MENU_ITEM_FPS:
				{
					fps_cap = fps_options[pickerSelectedIndex];
					set_fps(fps_cap);
					break;
				}
				case MENU_ITEM_MUSIC_DEVICE:
				{
					music_device = (MusicDevice)pickerSelectedIndex;
					restart_audio();
					break;
				}
				case MENU_ITEM_BOSS_BAR_STYLE:
				{
					bossBarStyle = (int)pickerSelectedIndex;
					break;
				}
				case MENU_ITEM_BOSS_BAR_LAYOUT:
				{
					bossBarLayout = (int)pickerSelectedIndex;
					break;
				}
				case MENU_ITEM_BOSS_BAR_TWO:
				{
					bossBarTwoMode = (int)pickerSelectedIndex;
					break;
				}
				case MENU_ITEM_ENEMY_BAR_LAYOUT:
				{
					enemyBarLayout = (int)pickerSelectedIndex;
					break;
				}
				case MENU_ITEM_ENEMY_BAR_POS:
				{
					enemyBarPosition = (int)pickerSelectedIndex;
					break;
				}
				case MENU_ITEM_GAUGE_GRAD_GEN:
				{
					gaugeGradGenerator = (int)pickerSelectedIndex;
					break;
				}
				case MENU_ITEM_GAUGE_GRAD_SHIELD:
				{
					gaugeGradShield = (int)pickerSelectedIndex;
					break;
				}
				case MENU_ITEM_GAUGE_GRAD_ARMOR:
				{
					gaugeGradArmor = (int)pickerSelectedIndex;
					break;
				}
				case MENU_ITEM_ZICA_BASE:
				{
					zicaLaserBase = (int)pickerSelectedIndex;
					break;
				}
				case MENU_ITEM_ZICA_LENGTH:
				{
					zicaLaserLength = (int)pickerSelectedIndex;
					break;
				}
				case MENU_ITEM_SPARKS_MODE:
				{
					superSparkMode[currentSparkWeapon] = (int)pickerSelectedIndex;
					break;
				}
				case MENU_ITEM_WALLOP_BOLT:
				{
					wallopSecondBolt = (int)pickerSelectedIndex;
					break;
				}
				case MENU_ITEM_EPDIFF_MODE:
				{
					epDiffMode[currentDiffWeapon] = (int)pickerSelectedIndex;
					break;
				}
				default:
					break;
				}

				currentPicker = MENU_ITEM_NONE;
			}
		}
	}
}

int main(int argc, char *argv[])
{
#if defined(__SWITCH__) || defined(__vita__)
	console_platform_init();   // console early init (mount data + ensure the writable user dir exists), before any file access
#endif

	install_crash_handler();  // write opentyrian_log.log next to the exe on any unhandled crash / CRT fatal
	watchdog_init();          // ...and on a hard main-thread hang (infinite loop), which throws no exception

	mt_srand(time(NULL));

	printf("\nWelcome to... >> %s %s <<\n\n", opentyrian_str, opentyrian_version);

	printf("Copyright (C) 2022 The OpenTyrian Development Team\n");
	printf("Copyright (C) 2022 Kaito Sinclaire\n\n");

	printf("This program comes with ABSOLUTELY NO WARRANTY.\n");
	printf("This is free software, and you are welcome to redistribute it\n");
	printf("under certain conditions.  See the file COPYING for details.\n\n");

	if (SDL_Init(0))
	{
		printf("Failed to initialize SDL: %s\n", SDL_GetError());
		return -1;
	}

	// Note for this reorganization:
	// Tyrian 2000 requires help text to be loaded before the configuration,
	// because the default high score names are stored in help text

	JE_paramCheck(argc, argv);

	if (!override_xmas) // arg handler may override
		xmas = xmas_time();

	JE_loadHelpText();

	/* The debug entries in the buy/sell menu are inserted at runtime by
	 * JE_itemScreen, only when Debug Mode is enabled; off keeps the stock layout. */

	/*debuginfo("Help text complete");*/

	JE_loadConfiguration();

	// A saved Christmas choice (Extra menu, xmasMode 0/1) overrides the date
	// auto-detection above and suppresses the "Activate Christmas?" prompt. Command line
	// still wins: skipped if an arg already forced a choice (override_xmas set).
	if (!override_xmas && xmasMode >= 0)
	{
		xmas = (xmasMode != 0);
		override_xmas = true;
	}

	JE_scanForEpisodes();

	init_video();
	init_keyboard();
	init_joysticks();
	printf("assuming mouse detected\n"); // SDL can't tell us if there isn't one

	if (xmas && (!dir_file_exists(data_dir(), "tyrianc.shp") || !dir_file_exists(data_dir(), "voicesc.snd")))
	{
		xmas = false;

		fprintf(stderr, "warning: Christmas is missing.\n");
	}

	JE_loadPals();
	JE_loadMainShapeTables(xmas ? "tyrianc.shp" : "tyrian.shp");

	if (xmas && !override_xmas)
	{
		// xmas_prompt() draws across the full vga_width buffer (like jukebox()
		// and JE_destructGame()), so it needs the menu-centering pillarbox off.
		set_menu_centered(false);
		bool xmas_accepted = xmas_prompt();
		set_menu_centered(true);

		if (!xmas_accepted)
		{
			xmas = false;

			free_main_shape_tables();
			JE_loadMainShapeTables("tyrian.shp");
		}
	}

	/* Default Options */
	youAreCheating = false;
	smoothScroll = true;
	loadDestruct = false;

	if (!audio_disabled)
	{
		printf("initializing SDL audio...\n");

		init_audio();

		load_music();

		loadSndFile(xmas);
	}
	else
	{
		printf("audio disabled\n");
	}

	if (record_demo)
		printf("demo recording enabled (input limited to keyboard)\n");

	JE_loadExtraShapes();  /*Editship*/

	if (isNetworkGame)
	{
#ifdef WITH_NETWORK
		if (network_init())
		{
			network_tyrian_halt(3, false);
		}
#else
		fprintf(stderr, "OpenTyrian was compiled without networking support.");
		JE_tyrianHalt(5);
#endif
	}

#ifdef NDEBUG
	if (!isNetworkGame)
		intro_logos();
#endif

	for (; ; )
	{
		crashlog_set_phase("title / main menu");

		JE_initPlayerData();
		JE_sortHighScores();

		play_demo = false;
		stopped_demo = false;

		gameLoaded = false;
		jumpSection = false;

#ifdef WITH_NETWORK
		if (isNetworkGame)
		{
			networkStartScreen();
		}
		else
#endif
		{
			if (!titleScreen())
			{
				// Player quit from title screen.
				break;
			}
		}

		if (loadDestruct)
		{
			JE_destructGame();

			loadDestruct = false;
		}
		else
		{
			set_menu_centered(false);
			JE_main();
			set_menu_centered(true);

			if (trentWin)
			{
				// Player beat SuperTyrian.
				break;
			}
		}
	}

	JE_tyrianHalt(0);

	return 0;
}
