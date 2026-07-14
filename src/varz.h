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
#ifndef VARZ_H
#define VARZ_H

#include "episodes.h"
#include "opentyr.h"
#include "player.h"
#include "sprite.h"

#include <stdbool.h>

#define SA 9

enum
{
	SA_NONE = 0,
	SA_NORTSHIPZ = 7,
	SA_LASTSHIP = 9,
	
	// only used for code entry
	SA_DESTRUCT = 10,
	SA_ENGAGE = 11,
	
	// only used in pItems[P_SUPERARCADE]
	SA_SUPERTYRIAN = 254,
	SA_ARCADE = 255
};

// Enemy-shot pool. ENEMY_SHOT_MAX sizes the enemyShot[]/enemyShotAvail[] arrays AND is the pool
// cap used in ENDLESS mode, where the "rising tide" fans out many extra enemy bullets. Normal
// (non-endless) play stays capped at ENEMY_SHOT_NORMAL (the original 60), so those levels fire
// exactly as before -- the enemy-shot creation loop (tyrian2.c) picks the cap by mode. The pool
// must stay below the render-list id headroom: RL_ID_ESHOT_BASE + slot < RL_ID_EXPL_BASE, i.e.
// ENEMY_SHOT_MAX <= 1000 (see render_list.h).
#define ENEMY_SHOT_NORMAL  60   // non-endless cap (= original behaviour)
#define ENEMY_SHOT_MAX     500  // array size + endless cap

#define CURRENT_KEY_SPEED 1  /*Keyboard/Joystick movement rate*/

#define MAX_EXPLOSIONS           200
#define MAX_REPEATING_EXPLOSIONS 20
#define MAX_SUPERPIXELS          50000  // was 101; global spark ring buffer -- bigger = denser/longer explosion showers
#define SUPERPIXELS_CLASSIC      101  // the original DOS spark cap; the "Extra Sparks" toggle (extraSparks, config.c) picks between the two

struct JE_SingleEnemyType
{
	JE_byte     fillbyte;
	JE_integer  ex, ey;     /* POSITION */
	JE_shortint exc, eyc;   /* CURRENT SPEED */
	JE_shortint exca, eyca; /* RANDOM ACCELERATION */
	JE_shortint excc, eycc; /* FIXED ACCELERATION WAITTIME */
	JE_shortint exccw, eyccw;
	JE_byte     armorleft;
	int         damageAccum; /* expert-mode boss HP: accumulates damage, expertBossHpMult pts = 1 armor */
	JE_byte     healthbar_max;  /* armor at the moment of the first hit = denominator for the enemy HP bar */
	JE_boolean  healthbar_seen; /* set once this enemy has taken player damage (gates the enemy HP bar) */
	JE_byte     eshotwait[3], eshotmultipos[3]; /* [1..3] */
	JE_byte     enemycycle;
	JE_byte     ani;
	JE_word     egr[20]; /* [1..20] */
	JE_byte     size;
	JE_byte     linknum;
	JE_byte     aniactive;
	JE_byte     animax;
	JE_byte     aniwhenfire;
	Sprite2_array *sprite2s;
	JE_shortint exrev, eyrev;
	JE_integer  exccadd, eyccadd;
	JE_byte     exccwmax, eyccwmax;
	void       *enemydatofs;
	JE_boolean  edamaged;
	JE_word     enemytype;
	JE_byte     animin;
	JE_word     edgr;
	JE_shortint edlevel;
	JE_shortint edani;
	JE_byte     eliteState; /* endless: 0=undecided, 1=normal, 2=elite, 3=champion (endless.c) */
	JE_byte     filter;
	JE_integer  evalue;
	JE_integer  fixedmovey;
	JE_byte     freq[3]; /* [1..3] */
	JE_byte     launchwait;
	JE_word     launchtype;
	JE_byte     launchfreq;
	JE_byte     xaccel;
	JE_byte     yaccel;
	JE_byte     tur[3]; /* [1..3] */
	JE_word     enemydie; /* Enemy created when this one dies */
	JE_boolean  enemyground;
	JE_byte     explonum;
	JE_integer  mapoffset;
	float       mapoffset_frac;  // sub-pixel part of the parallax anchor, for the smooth-H health bar
	float       scroll_yfrac;    // sub-pixel part of the vertical scroll anchor, for the smooth-V health bar
	JE_boolean  scoreitem;

	JE_boolean  special;
	JE_byte     flagnum;
	JE_boolean  setto;

	JE_byte     iced; /*Duration*/

	JE_byte     launchspecial;

	JE_integer  xminbounce;
	JE_integer  xmaxbounce;
	JE_integer  yminbounce;
	JE_integer  ymaxbounce;
	JE_byte     fill[3]; /* [1..3] */
};

typedef struct JE_SingleEnemyType JE_MultiEnemyType[100]; /* [1..100] */

typedef JE_byte JE_DanCShape[24 * 28]; /* [1..(24*28) div 2] OF WORD */

typedef JE_char JE_CharString[256]; /* [1..256] */

typedef JE_byte JE_Map1Buffer[24 * 28 * 13 * 4]; /* [1..24*28*13*4] */

typedef JE_byte *JE_MapType[300][14]; /* [1..300, 1..14] */
typedef JE_byte *JE_MapType2[600][14]; /* [1..600, 1..14] */
typedef JE_byte *JE_MapType3[600][15]; /* [1..600, 1..15] */

struct JE_EventRecType
{
	JE_word     eventtime;
	JE_byte     eventtype;
	JE_integer  eventdat, eventdat2;
	JE_shortint eventdat3, eventdat5, eventdat6;
	JE_byte     eventdat4;
};

struct JE_MegaDataType1
{
	JE_MapType mainmap;
	struct
	{
		JE_DanCShape sh;
	} shapes[72]; /* [0..71] */
	JE_byte tempdat1;
	/*JE_DanCShape filler;*/
};

struct JE_MegaDataType2
{
	JE_MapType2 mainmap;
	struct
	{
		JE_byte nothing[3]; /* [1..3] */
		JE_byte fill;
		JE_DanCShape sh;
	} shapes[71]; /* [0..70] */
	JE_byte tempdat2;
};

struct JE_MegaDataType3
{
	JE_MapType3 mainmap;
	struct
	{
		JE_byte nothing[3]; /* [1..3] */
		JE_byte fill;
		JE_DanCShape sh;
	} shapes[70]; /* [0..69] */
	JE_byte tempdat3;
};

typedef JE_byte JE_EnemyAvailType[100]; /* [1..100] */

typedef struct {
	JE_integer sx, sy;
	JE_integer sxm, sym;
	JE_shortint sxc, syc;
	JE_byte tx, ty;
	JE_word sgr;
	JE_byte sdmg;
	JE_byte duration;
	JE_word animate;
	JE_word animax;
	JE_byte fill[12];
} EnemyShotType;

typedef struct {
	JE_byte ttl;
	JE_integer x, y;
	JE_word sprite;
	bool followPlayer;
	bool fixedPosition;
	JE_integer deltaY;
	Uint8 id_gen;  // bumped each time this slot is reused, to disambiguate the render-
	               // list interpolation id so a recycled slot isn't mis-paired
} Explosion;

typedef struct {
	unsigned int delay;
	unsigned int ttl;
	unsigned int x, y;
	bool big;
} rep_explosion_type;

typedef struct {
	unsigned int x, y, z;
	signed int delta_x, delta_y;
	Uint8 color;
} superpixel_type;

extern JE_integer tempDat, tempDat2, tempDat3;
extern const JE_byte SANextShip[SA + 2];
extern const JE_word SASpecialWeapon[SA];
extern const JE_word SASpecialWeaponB[SA];
extern const JE_byte SAShip[SA];
extern const JE_word SAWeapon[SA][5];
extern const JE_byte specialArcadeWeapon[PORT_NUM];
extern const JE_byte optionSelect[16][3][2];
extern const JE_word PGR[21];
extern const JE_byte PAni[21];
extern const JE_word linkGunWeapons[38];
extern const JE_word chargeGunWeapons[38];
extern const JE_byte randomEnemyLaunchSounds[3];
extern const JE_byte keyboardCombos[26][8];
extern const JE_byte shipCombosB[21];
extern const JE_byte superTyrianSpecials[4];
extern const JE_byte shipCombos[19][3];
extern JE_byte SFCurrentCode[2][21];
extern JE_byte SFExecuted[2];
extern JE_byte lvlFileNum;
extern JE_byte forcedLvlFileNum;  // one-shot: force the next load's lvlFileNum past JE_loadMap's rescan (0 = section default)
extern JE_word maxEvent, eventLoc;
extern struct JE_EventRecType eventRec[];
extern JE_word levelEnemy[40], levelEnemyMax;
extern JE_word tempBackMove, explodeMove;
extern JE_byte levelEnd;
extern JE_word levelEndFxWait;
extern JE_shortint levelEndWarp;
extern JE_boolean endLevel, reallyEndLevel, waitToEndLevel, playerEndLevel, normalBonusLevelCurrent, bonusLevelCurrent, smallEnemyAdjust, readyToEndLevel, quitRequested;
extern JE_byte newPL[10];
extern JE_word returnLoc;
extern JE_boolean returnActive;
extern JE_word galagaShotFreq;
extern JE_longint galagaLife;
extern JE_boolean debug;
extern Uint32 debugTime, lastDebugTime;
extern JE_longint debugHistCount;
extern JE_real debugHist;
extern JE_word curLoc;
extern JE_boolean firstGameOver, gameLoaded, enemyStillExploding;
extern JE_word totalEnemy;
extern JE_word enemyKilled;
extern struct JE_MegaDataType1 megaData1;
extern struct JE_MegaDataType2 megaData2;
extern struct JE_MegaDataType3 megaData3;
extern JE_byte flash;
extern JE_shortint flashChange;
extern JE_byte displayTime;

extern bool play_demo, record_demo, stopped_demo;
extern Uint8 demo_num;
extern FILE *demo_file;

extern Uint8 demo_keys;
extern Uint16 demo_keys_wait;

extern JE_byte soundQueue[8];
extern JE_boolean enemyContinualDamage;
extern JE_boolean enemiesActive;
extern JE_boolean forceEvents;
extern JE_boolean stopBackgrounds;
extern JE_byte stopBackgroundNum;
extern JE_byte damageRate;
extern JE_boolean background3x1;
extern JE_boolean background3x1b;
extern JE_boolean levelTimer;
extern JE_word levelTimerCountdown;
extern JE_word levelTimerJumpTo;
extern JE_boolean randomExplosions;
extern JE_boolean editShip1, editShip2;
extern JE_boolean globalFlags[10];
extern JE_byte levelSong;
extern JE_boolean loadDestruct;
extern JE_word mapOrigin, mapPNum;
extern JE_byte mapPlanet[5], mapSection[5];
extern JE_boolean moveTyrianLogoUp;
extern JE_boolean skipStarShowVGA;
extern JE_MultiEnemyType enemy;
extern JE_EnemyAvailType enemyAvail;
extern JE_word enemyOffset;
extern JE_word enemyOnScreen;
extern JE_word enemyParkedAbove;
extern JE_word mapStopStallTicks;
extern JE_word superEnemy254Jump;
extern Explosion explosions[MAX_EXPLOSIONS];
extern JE_integer explosionFollowAmountX, explosionFollowAmountY;
extern JE_boolean fireButtonHeld;
extern JE_boolean enemyShotAvail[ENEMY_SHOT_MAX];
extern EnemyShotType enemyShot[ENEMY_SHOT_MAX];
extern JE_byte zinglonDuration;
extern bool zinglonPillarActive;
extern int zinglonPillarCX;
extern int zinglonPillarTemp;
extern JE_byte astralDuration;
extern JE_word flareDuration;
extern JE_boolean flareStart;
extern JE_shortint flareColChg;
extern JE_byte specialWait;
extern JE_byte nextSpecialWait;
extern JE_boolean spraySpecial;
extern JE_byte doIced;
extern JE_boolean infiniteShot;
extern JE_boolean cheatInfiniteSidekickAmmo;
extern JE_boolean cheatInfiniteShields;
extern JE_boolean cheatInfiniteArmor;
extern JE_boolean cheatInfiniteGenerator;  /* debug: weapons don't drain generator power */
extern JE_boolean cheatNoEnemyFire;  /* debug: suppress enemy projectiles */
extern JE_boolean cheatInstantCharge;  /* debug: charge sidekicks reach full charge instantly */

// Noclip debug mode (cycled in the debug menu): pass through enemies without
// collision. The transparent state additionally draws the ship semi-transparent.
enum
{
	NOCLIP_OFF         = 0,  // normal collision
	NOCLIP_ON          = 1,  // pass through enemies, ship drawn normally
	NOCLIP_TRANSPARENT = 2,  // pass through enemies, ship drawn semi-transparent
	NOCLIP_NUM         = 3,
};
extern JE_byte noclipMode;

extern JE_boolean debugHitboxOverlay;  /* debug: draw collision boxes */
extern JE_boolean debugPerfOverlay;    /* debug: draw FPS/timing/counts overlay */
extern JE_boolean autoFireSpecial;
extern JE_byte    debugTwiddleSpecial;      /* debug: selected twiddle's special index (0 = none) */
extern JE_boolean debugAutofireTwiddle;     /* debug: auto-fire the selected twiddle while fire is held */
extern JE_boolean debugTwiddleTrigger;      /* debug: one-shot fire request from the debug menu */
extern JE_boolean debugToggleFire;          /* debug: fire button toggles auto-fire on/off */
extern JE_boolean debugToggleFireActive;    /* debug: Toggle Fire latch -- currently auto-firing */

// "Autofire Charge Sidekicks" mode (cycled in the debug menu). Governs whether a
// charge sidekick (pwr > 0) fires while the main fire button is held.
enum
{
	CHARGE_AUTOFIRE_OFF  = 0,  // only fires on its own dedicated sidekick button
	CHARGE_AUTOFIRE_ON   = 1,  // autofires while the main fire button is held
	CHARGE_AUTOFIRE_FULL = 2,  // autofires on the main button only when fully charged
	CHARGE_AUTOFIRE_FAST = 3,  // like ON, but reloads at the fastest charge stage's shotrepeat;
	                           // paired with Instant Charge this fires full-power shots at top speed
	CHARGE_AUTOFIRE_NUM  = 4,  // (append new modes; the value is saved per slot -- don't renumber)
};
extern JE_byte    chargeSidekickAutofire;
extern JE_boolean difficultyAdjust;
extern JE_boolean expertMode;

/* Expert-mode tunables (adjustable via the Expert Settings debug submenu).
 * Defaults are deliberately mild so expert mode is "harder but fair". */
#define EXPERT_DEF_BOSS_HP      3    /* boss HP multiplier (x)            */
#define EXPERT_DEF_ENEMY_ARMOR  125  /* regular enemy armor scaling (%)   */
#define EXPERT_DEF_ENERGY       150  /* weapon energy use (%)             */
#define EXPERT_DEF_SHOP_COST    2    /* shop purchase cost multiplier (x) */
#define EXPERT_DEF_UPGRADE_COST 2    /* weapon upgrade cost multiplier (x)*/
#define EXPERT_DEF_CASH         150  /* cash earned from enemies (%)      */

extern int expertBossHpMult;     /* divisor: bosses take 1/N damage -> Nx HP */
extern int expertEnemyArmorPct;  /* >=100; scales non-boss enemy armor       */
extern int expertEnergyPct;      /* >=100; scales weapon energy use          */
extern int expertShopCostMult;   /* >=1; scales shop purchase prices         */
extern int expertUpgradeCostMult;/* >=1; scales weapon power upgrade prices  */
extern int expertScorePct;       /* >=100; scales cash earned from enemies   */

/* Shared descriptor for the expert tunables: one source of truth driving the
 * debug submenu, the config load/save, and reset-to-defaults. */
typedef struct
{
	const char* label;   /* submenu label                  */
	const char* cfgKey;  /* opentyrian.cfg option key      */
	int*        value;   /* the live global                */
	int         lo, hi;  /* inclusive adjustable range     */
	int         step;    /* adjust increment               */
	int         def;     /* default value                  */
	char        fmt;     /* 'x' multiplier or '%' percent  */
}
ExpertSetting;

extern ExpertSetting expertSettings[];
extern const int expertSettingsCount;

void clamp_expert_settings(void);  /* clamp each tunable into [lo, hi] */

extern JE_boolean allPlayersGone;
extern const uint shadowYDist;
extern JE_real optionSatelliteRotate;
extern JE_integer optionAttachmentMove[2];                               // per sidekick slot (LEFT/RIGHT)
extern JE_boolean optionAttachmentLinked[2], optionAttachmentReturn[2];  // so both front options can launch
extern JE_byte chargeWait, chargeLevel, chargeMax, chargeGr, chargeGrWait;
extern JE_word neat;
extern rep_explosion_type rep_explosions[MAX_REPEATING_EXPLOSIONS];
extern superpixel_type superpixels[MAX_SUPERPIXELS];
extern unsigned int last_superpixel;

// Optional clip window for JE_drawSP. While active, superpixels are only plotted (and
// recorded for smooth replay) inside [x0,x1) x [y0,y1). The shop weapon preview sets this
// to the preview box so spark trails can't spill out into the item list on the right;
// gameplay leaves it inactive (sparks clip to the full screen).
extern bool superpixelClipActive;
extern int superpixelClipX0, superpixelClipY0, superpixelClipX1, superpixelClipY1;
void JE_setSPClip(int x0, int y0, int x1, int y1);
void JE_clearSPClip(void);
extern JE_byte temp, temp2, temp3;
extern JE_word tempW;
extern JE_boolean doNotSaveBackup;
extern JE_word x, y;
extern JE_integer b;
extern JE_byte **BKwrap1to, **BKwrap2to, **BKwrap3to, **BKwrap1, **BKwrap2, **BKwrap3;
extern JE_shortint specialWeaponFilter, specialWeaponFreq;
extern JE_word specialWeaponWpn;
extern JE_boolean linkToPlayer;
extern JE_word shipGr, shipGr2;
extern Sprite2_array *shipGrPtr, *shipGr2ptr;

static const int hud_sidekick_y[2][2] =
{
	{  64,  82 }, // one player HUD
	{ 108, 126 }, // two player HUD
};

void JE_getShipInfo(void);
JE_word JE_SGr(JE_word ship, Sprite2_array **ptr);

void JE_drawOptions(void);

void JE_tyrianHalt(JE_byte code); /* This ends the game */
void JE_specialComplete(JE_byte playernum, JE_byte specialType);
void JE_doSpecialShot(JE_byte playernum, uint *armor, uint *shield);

void JE_wipeShieldArmorBars(void);
JE_byte JE_playerDamage(JE_byte temp, Player *);

void JE_setupExplosion(JE_integer x, JE_integer y, JE_integer deltaY, JE_integer type, bool fixedPosition, bool followPlayer);
void JE_setupExplosionLarge(JE_boolean enemyground, JE_byte explonum, JE_integer x, JE_integer y);

void JE_drawShield(void);
void JE_drawArmor(void);

JE_word JE_portConfigs(void);

/*SuperPixels*/
// classic_cap forces the classic 101-spark limit for this call even when extraSparks is on
// (used by the superspark weapon trails); pass false to honor the extraSparks setting.
void JE_doSP(JE_word x, JE_word y, JE_word num, JE_byte explowidth, JE_byte color, bool classic_cap);
void JE_drawSP(void);

void JE_drawOptionLevel(void);

#endif /* VARZ_H */
