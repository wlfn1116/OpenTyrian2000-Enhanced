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
#ifndef CONFIG_H
#define CONFIG_H

#include "opentyr.h"
#include "config_file.h"

#include "SDL.h"

#include <stdio.h>

#define SAVE_FILES_NUM (11 * 2)

/* These are necessary because the size of the structure has changed from the original, but we
   need to know the original sizes in order to find things in TYRIAN.SAV */
#define SAVE_FILES_SIZE 2552
#define SIZEOF_SAVEGAMETEMP SAVE_FILES_SIZE + 4 + 100
#define SAVE_FILE_SIZE (SIZEOF_SAVEGAMETEMP - 4)

/*#define SAVE_FILES_SIZE (2502 - 4)
#define SAVE_FILE_SIZE (SAVE_FILES_SIZE)*/

enum
{
	DIFFICULTY_WIMP = 0,
	DIFFICULTY_EASY,
	DIFFICULTY_NORMAL,
	DIFFICULTY_HARD,
	DIFFICULTY_IMPOSSIBLE,
	DIFFICULTY_INSANITY,
	DIFFICULTY_SUICIDE,
	DIFFICULTY_MANIACAL,
	DIFFICULTY_ZINGLON,  // aka Lord of the Game
	DIFFICULTY_NORTANEOUS,
	DIFFICULTY_10,
};

// NOTE: Do not reorder.  This ordering corresponds to the keyboard
//       configuration menu and to the bits stored in demo files.
enum
{
	KEY_SETTING_UP,
	KEY_SETTING_DOWN,
	KEY_SETTING_LEFT,
	KEY_SETTING_RIGHT,
	KEY_SETTING_FIRE,
	KEY_SETTING_CHANGE_FIRE,
	KEY_SETTING_LEFT_SIDEKICK,
	KEY_SETTING_RIGHT_SIDEKICK,
};

typedef JE_byte DosKeySettings[8];  // fka KeySettingType

typedef SDL_Scancode KeySettings[8];

typedef JE_byte MouseSettings[3];

typedef JE_byte JE_PItemsType[12]; /* [1..12] */

typedef JE_byte JE_EditorItemAvailType[100]; /* [1..100] */

typedef struct
{
	JE_word       encode;
	JE_word       level;
	JE_PItemsType items;
	JE_longint    score;
	JE_longint    score2;
	char          levelName[11]; /* string [9]; */ /* SYN: Added one more byte to match lastLevelName below */
	JE_char       name[15]; /* [1..14] */ /* SYN: Added extra byte for null */
	JE_byte       cubes;
	JE_byte       power[2]; /* [1..2] */
	JE_byte       episode;
	JE_PItemsType lastItems;
	JE_byte       difficulty;
	JE_byte       secretHint;
	JE_byte       input1;
	JE_byte       input2;
	JE_boolean    gameHasRepeated; /*See if you went from one episode to another*/
	JE_byte       initialDifficulty;

	/* High Scores - Each episode has both sets of 1&2 player selections - with 3 in each */
	JE_longint    highScore1;
	JE_longint    highScore2;  // unused
	char          highScoreName[30]; /* string [29] */
	JE_byte       highScoreDiff;
	JE_boolean    autoFireSpecial;
	JE_byte       chargeSidekickAutofire;
	JE_boolean    difficultyAdjust;
	JE_boolean    cheatInfiniteSidekickAmmo;
	JE_boolean    cheatInfiniteShields;
	JE_boolean    cheatInfiniteArmor;
	JE_boolean    expertMode;
} JE_SaveFileType;

typedef JE_SaveFileType JE_SaveFilesType[SAVE_FILES_NUM]; /* [1..savefilesnum] */
typedef JE_byte JE_SaveGameTemp[SAVE_FILES_SIZE + 4 + 100]; /* [1..sizeof(savefilestype) + 4 + 100] */

typedef struct
{
	// Tyrian 2000 uses a different high scores struct and appends it to TYRIAN.SAV
	JE_longint    score;
	char          playerName[30];
	JE_byte       difficulty;
} T2KHighScoreType;

// First 10 are timed battle, next 10 are episodes
extern T2KHighScoreType t2kHighScores[20][3];

extern const JE_byte cryptKey[10];
extern const DosKeySettings defaultDosKeySettings;  // fka defaultKeySettings
extern const KeySettings defaultKeySettings;
extern const MouseSettings defaultMouseSettings;
extern char defaultHighScoreNames[39][23];
extern char defaultTeamNames[10][25];
extern const JE_EditorItemAvailType initialItemAvail;
extern JE_boolean smoothies[9];
extern JE_byte starShowVGASpecialCode;
extern JE_word lastCubeMax, cubeMax;
extern JE_word cubeList[4];
extern JE_boolean gameHasRepeated;
extern JE_shortint difficultyLevel, oldDifficultyLevel, initialDifficulty;
extern JE_byte timeBattleSelection;
extern uint power, lastPower, powerAdd;
extern JE_byte shieldWait, shieldT;

enum
{
	SHOT_FRONT,
	SHOT_REAR,
	SHOT_LEFT_SIDEKICK,
	SHOT_RIGHT_SIDEKICK,
	SHOT_MISC,
	SHOT_P2_CHARGE,
	SHOT_P1_SUPERBOMB,
	SHOT_P2_SUPERBOMB,
	SHOT_SPECIAL,
	SHOT_NORTSPARKS,
	SHOT_SPECIAL2
};

extern JE_byte shotRepeat[11], shotMultiPos[11];
extern JE_boolean portConfigChange, portConfigDone;
extern char lastLevelName[11], levelName[11];
extern JE_byte mainLevel, nextLevel, saveLevel;
extern DosKeySettings dosKeySettings;  // fka keySettings
extern KeySettings keySettings;
extern MouseSettings mouseSettings;
extern JE_shortint levelFilter, levelFilterNew, levelBrightness, levelBrightnessChg;
extern JE_boolean filtrationAvail, filterActive, filterFade, filterFadeStart;
extern JE_boolean gameJustLoaded;
extern JE_boolean galagaMode;
extern JE_boolean extraGame;
extern JE_boolean twoPlayerMode, twoPlayerLinked, onePlayerAction, timedBattleMode, superTyrian, trentWin;
extern JE_boolean endlessMode;  // Endless roguelite mode (see endless.c)
extern JE_byte superArcadeMode;
extern JE_byte superArcadePowerUp;
extern JE_real linkGunDirec;
extern JE_byte inputDevice[2];
extern JE_byte secretHint;
extern JE_byte background3over;
extern JE_byte background2over;
extern JE_byte gammaCorrection;
extern JE_boolean superPause, explosionTransparent, youAreCheating, displayScore, background2, smoothScroll, wild, superWild, starActive, topEnemyOver, skyEnemyOverAll, background2notTransparent;
extern JE_byte versionNum;
extern JE_byte fastPlay;
extern JE_boolean pentiumMode;
extern JE_byte gameSpeed;
extern JE_byte processorType;
extern JE_SaveFilesType saveFiles;
extern JE_SaveGameTemp saveTemp;
extern JE_word editorLevel;
extern int fps_cap;

/* ---- Enhancements (Setup -> Enhancements...) ---- */

typedef enum
{
	BOSS_BAR_CLASSIC  = 0,  // original small double-sided bar
	BOSS_BAR_ENHANCED = 1,  // redesigned framed gauge
} BossBarStyle;

typedef enum
{
	BOSS_BAR_TOP    = 0,  // horizontal, along the top
	BOSS_BAR_BOTTOM = 1,  // horizontal, along the bottom
	BOSS_BAR_LEFT   = 2,  // vertical, left edge
	BOSS_BAR_RIGHT  = 3,  // vertical, right edge
} BossBarLayout;

typedef enum
{
	BOSS_BAR_TWO_TOGETHER = 0,  // horizontal: stacked;       vertical: side by side on the chosen side
	BOSS_BAR_TWO_SPLIT    = 1,  // horizontal: halves side by side;  vertical: one left + one right
	BOSS_BAR_TWO_STACKED  = 2,  // horizontal: stacked;       vertical: stacked (one above the other) on the chosen side
} BossBarTwoMode;

typedef enum
{
	ENEMY_BAR_HORIZONTAL = 0,  // bar runs horizontally (fills left->right)
	ENEMY_BAR_VERTICAL   = 1,  // bar runs vertically   (fills bottom->up)
} EnemyBarLayout;

typedef enum
{
	ENEMY_BAR_POS_BOTTOM = 0,  // below the enemy (the original placement)
	ENEMY_BAR_POS_TOP    = 1,  // above the enemy
	ENEMY_BAR_POS_LEFT   = 2,  // left of the enemy
	ENEMY_BAR_POS_RIGHT  = 3,  // right of the enemy
	ENEMY_BAR_POS_CENTER = 4,  // over the enemy's centre
} EnemyBarPosition;

extern int bossBarStyle;    // BossBarStyle
extern int bossBarLayout;   // BossBarLayout
extern int bossBarTwoMode;  // BossBarTwoMode
extern bool debugMode;      // gates the debug menu and debug level select
extern bool enemyBars;      // show a small health bar on damaged enemies
extern int enemyBarLayout;    // EnemyBarLayout
extern int enemyBarPosition;  // EnemyBarPosition
extern int enemyBarOpacity;   // 0..100 (percent; 0 hides the bars)
extern bool smoothMotion;   // interpolate motion between logic ticks for smooth high-refresh play
void set_smooth_motion(bool enabled);  // disabling smooth motion also disables supersampling
extern bool extraSparks;    // raise the explosion superspark limit far above the classic 101 cap
// Superspark projectile trails (menu: Enhancements -> Weapon Tweaks -> Superspark Weapons). Only
// ep4/5 item data tags these projectiles; retagged per-episode by JE_applySuperSparks. notes.md §Weapons.
enum
{
	SUPER_SPARKS_AUTO = 0,  // no trail in ep1-3, trail in ep4/5 (vanilla per-episode)
	SUPER_SPARKS_ON,        // spark trail in every episode (ep4/5 behavior everywhere)
	SUPER_SPARKS_OFF,       // no spark trail in any episode (ep1-3 behavior everywhere)
	SUPER_SPARKS_COUNT
};
// The affected weapons, each with its own trail mode and classic-limit cap. Ice Beam and
// Ice Blast share one entry: both fire the same spark-tagged sprite (634), so their trails
// are indistinguishable in flight.
typedef enum
{
	SSW_MEGA_PULSE = 0,  // Mega Pulse front gun (port 19, wpns 400-410; sprite 35, spark bank 7)
	SSW_WALLOP_BEAM,     // Beno Wallop Beam sidekick (wpn 736; sprites 30/29, bank 7)
	SSW_PROTRON_B,       // Beno Protron System -B- sidekick (wpn 737; sprite 28, bank 9)
	SSW_ICE,             // Ice Beam + Ice Blast specials (wpns 621/706; sprite 634, bank 9)
	SSW_COUNT
} SuperSparkWeapon;
extern int  superSparkMode[SSW_COUNT];       // SUPER_SPARKS_* : where each weapon leaves its trail
extern bool superSparkClassicCap[SSW_COUNT]; // cap that trail at the classic limit even when extraSparks is on
bool superSparkCapForSprite(JE_word sprite); // cap setting for a trail-tagged shot sprite (JE_doSP calls in shots.c)
// The ep4/5 Beno Wallop Beam also fires a second bolt each volley (multi/max grow to 2)
// that the ep1-3 record lacks entirely; this forces that double-bolt pattern in/out of
// every episode with the same SUPER_SPARKS_* Auto/On/Off semantics (Auto = as shipped).
extern int wallopSecondBolt;

// Weapons whose ep1-3 (tyrian.hdt) and ep4/5 (tyrian4/5.lvl) item data differ beyond the superspark
// trail above (full diff of the two data sets): gameplay reworks, a blast sprite, retuned sounds.
// epDiffMode[] forces one episode's data; JE_applyEpDiffs rewrites from shipped constants. notes.md §Weapons.
enum
{
	EPDIFF_AUTO = 0,     // per-episode default: ep1-3 data in ep1-3, ep4/5 data in ep4/5
	EPDIFF_EP13,         // force the ep1-3 behavior in every episode
	EPDIFF_EP45,         // force the ep4/5 behavior in every episode
	EPDIFF_MODE_COUNT
};
typedef enum
{
	EDW_XEGA_BALL = 0,   // Xega Ball special (wpn 720): ep1-3 two weak balls vs ep4/5 one strong ball
	EDW_MICROSOL_OPT5,   // MicroSol Option 5 (wpn 23): ep1-3 8-way fan vs ep4/5 twin shot
	EDW_FLARE,           // Flare / Super Bomb (wpn 622): blast sprite 20 (ep1-3) vs 21 (ep4/5)
	EDW_NEEDLE_LASER,    // Needle Laser (wpn 781): firing sound 31 vs 13
	EDW_BUBBLE_GUM,      // Bubble Gum-Gun (wpn 792): firing sound 30 vs 13
	EDW_FLYING_PUNCH,    // Flying Punch (wpn 794): firing sound 31 vs 30
	EDW_PRETZEL_MISSILE, // Pretzel Missile (wpn 795): firing sound 31 vs 30
	EDW_DRAGON_FROST,    // Dragon Frost (wpn 806): firing sound 31 vs 30
	EDW_COUNT
} EpDiffWeapon;
extern int epDiffMode[EDW_COUNT];  // EPDIFF_* : which episode's data each weapon uses

// Per-gauge gradient direction for the three vertical HUD gauges (menu: Enhancements ->
// Gauge Gradients). Each gauge can run its shade gradient up the column (Up = classic),
// down it, or across the 9-pixel width (Left/Right). Default Up = the vanilla look.
typedef enum
{
	GAUGE_GRAD_UP = 0,   // vertical, brightest at the top    (classic)
	GAUGE_GRAD_DOWN,     // vertical, brightest at the bottom
	GAUGE_GRAD_LEFT,     // horizontal, brightest at the left column
	GAUGE_GRAD_RIGHT,    // horizontal, brightest at the right column
	GAUGE_GRAD_COUNT
} GaugeGradientDir;

extern int gaugeGradGenerator;  // GaugeGradientDir for the generator power gauge
extern int gaugeGradShield;     // GaugeGradientDir for the shield gauge
extern int gaugeGradArmor;      // GaugeGradientDir for the armor gauge

extern bool gaugeFlashShield;
extern bool gaugeFlashArmor;

// Zica Laser Lv11 tweaks (menu: Enhancements -> Weapon Tweaks). Three independent axes:
//   Base   : the Lv11 horizontal shot pattern, forced in every episode.
//   Length : Lv11 shot length -- Short (vanilla) or Long (as long as the Lv10 beam).
//   Buff   : also fire the Lv10 ship-locked beam alongside the Lv11 shots.
// The defaults (Auto / Short / off) reproduce vanilla Tyrian exactly.
enum
{
	ZICA_BASE_AUTO = 0,  // per-episode default: ep1-3 columns, ep4+ spread (vanilla)
	ZICA_BASE_EP13,      // force the ep1-3 two-column pattern in every episode
	ZICA_BASE_EP4,       // force the ep4 centred-spread pattern in every episode
	ZICA_BASE_COUNT
};
enum
{
	ZICA_LEN_SHORT = 0,  // vanilla fast bolts
	ZICA_LEN_LONG,       // beams as long as the Lv10 shot
	ZICA_LEN_COUNT
};

extern int  zicaLaserBase;      // ZICA_BASE_* : Lv11 horizontal shot pattern
extern int  zicaLaserLength;    // ZICA_LEN_*  : Lv11 shot length
extern bool zicaLaserLock;      // Length=Long: lock the side beams to the ship (like the Lv10 beam)
extern bool zicaLaserBuff;      // also fire the Lv10 beam alongside the Lv11 shots
extern bool chargeLaserCannon;  // re-add the cut DOS "Charge-Laser Cannon" sidekick to shops
extern int  xmasMode;           // -1 = auto (by date), 0 = force off, 1 = force on

extern Config opentyrian_config;

void JE_initProcessorType(void);
void JE_setNewGameSpeed(void);
const char *get_user_directory(void);
void JE_loadConfiguration(void);
void JE_saveConfiguration(void);
bool save_opentyrian_config(void);  // write opentyrian.cfg now (settings + custom weapon)

void JE_saveGame(JE_byte slot, const char *name);
void JE_loadGame(JE_byte slot);

void JE_encryptSaveTemp(void);
void JE_decryptSaveTemp(void);

#endif /* CONFIG_H */
