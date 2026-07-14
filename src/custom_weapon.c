/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 *
 * Custom Weapon Creator — see custom_weapon.h.
 *
 * The player edits the engine's raw weapon struct (JE_WeaponType) directly, one
 * independent design per (fire mode, power level). materialize() copies each into
 * a reserved scratch weapon slot and wires up a reserved weaponPort, so the weapon
 * fires (and previews) through the exact same code path as every stock weapon. It
 * can also be equipped as a sidekick (a synthesized options[] sidekick that fires the
 * mode-0 level-1 compiled weapon).
 *
 * Because the design IS a JE_WeaponType, importing a stock weapon is a plain
 * struct copy — byte-for-byte identical firing behaviour, then fully editable.
 * A rear gun with two fire modes (opnum 2) imports and edits both banks.
 */

#include "custom_weapon.h"

#include "config.h"     // get_user_directory
#include "episodes.h"   // weapons[], weaponPort, options[], OPTION_NUM, chargeLaserSlot, WEAP_NUM, PORT_NUM
#include "file.h"       // dir_fopen, dir_fopen_warn
#include "player.h"     // player[], FRONT_WEAPON, REAR_WEAPON, LEFT_SIDEKICK, RIGHT_SIDEKICK
#include "sprite.h"     // spriteSheet9 / spriteSheet10 (sidekick body sheets)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

JE_WeaponType customWeaponRaw[CUSTOM_WEAPON_MODES][CUSTOM_POWER_LEVELS];

int  customWeaponModes = 1;

char customWeaponName[31]    = "Custom Weapon";
int  customWeaponCost        = 5000;
int  customWeaponPowerUse    = 3;
int  customWeaponEquipSlot   = CUSTOM_EQUIP_FRONT;
int  customWeaponItemGraphic = 0;    // 0 = borrow a default at init; else the shop icon
int  customWeaponChargeStages = 1;   // shot count (1 = no charging); option pwr = this - 1

int  customWeaponEditLevel = 0;
int  customWeaponEditMode  = 0;
bool customWeaponEnabled   = true;
int  customWeaponPort      = 0;
int  customSidekickSlot    = 0;

// Sidekick body appearance (see custom_weapon.h). Defaults reproduce the previous hardcoded
// side pod (the cut Charge-Laser's known-good frame in spriteSheet9).
int  customSidekickMount     = 0;   // side pod
int  customSidekickSprite    = 87;  // Charge-Laser side-pod body frame
int  customSidekickFrames    = 1;   // static (no flip-book) by default
int  customSidekickFrameStep = 1;   // consecutive sprites when animated
int  customSidekickAnimate   = 1;   // animate while firing

CustomBulletPreset customBulletPreset[CUSTOM_BULLET_PRESET_MAX];
int                customBulletPresetCount = 0;

CustomWeaponSlot customWeaponLib[CUSTOM_WEAPON_LIB_MAX];
int              customWeaponLibCount    = 0;
int              customWeaponCurrentSlot = 0;

static int clampi(int v, int lo, int hi)
{
	return (v < lo) ? lo : (v > hi) ? hi : v;
}

// The scratch weapon slot backing (mode, level).
static int customScratchSlot(int mode, int level)
{
	return CUSTOM_WEAP_BASE + mode * CUSTOM_POWER_LEVELS + level;
}

int customBulletMaxPower(int presetIdx)
{
	if (presetIdx < 0 || presetIdx >= customBulletPresetCount)
		return 1;
	return clampi(customBulletPreset[presetIdx].maxPower, 1, CUSTOM_POWER_LEVELS);
}

// Keep a raw design within the engine's hard limits (bullet count, sound index).
// Deliberately light: an imported stock weapon is already valid; this mainly
// guards hand-edited, randomized, or config-loaded designs.
static void sanitizeRawWeapon(JE_WeaponType *w)
{
	w->multi = (JE_byte)clampi(w->multi, 1, CUSTOM_BULLETS_MAX);
	w->max   = (JE_byte)clampi(w->max,   1, CUSTOM_BULLETS_MAX);
	// NOTE: shotrepeat 0 is valid and important — it fires a shot every tick, which is
	// what makes a laser render as one solid beam (104 stock weapons rely on it, incl.
	// every "Laser" power level). Flooring it to 1 halves the fire rate and opens gaps
	// between beam segments, so do not clamp it here.
	if (w->sound > CUSTOM_SOUND_MAX)
		w->sound = CUSTOM_SOUND_MAX;
}

// A blank one-bullet design (fires nothing until edited) — what a fresh install
// starts with, matching a blank creator canvas.
static void makeBlankWeapon(JE_WeaponType *w)
{
	memset(w, 0, sizeof(*w));
	sanitizeRawWeapon(w);   // floors multi/max to 1
}

// A plain two-bullet upward blaster — what the editor's RESET actions restore.
static void makeDefaultWeapon(JE_WeaponType *w)
{
	memset(w, 0, sizeof(*w));
	w->shotrepeat = 8;
	w->multi      = 2;
	w->max        = 2;
	for (int i = 0; i < 2; ++i)
	{
		w->attack[i] = 12;
		w->del[i]    = 255;              // long life (avoid the 98..121 sentinels)
		w->sx[i]     = 0;
		w->sy[i]     = 11;              // positive = travels up
		w->bx[i]     = (i == 0) ? -4 : 4;
		w->by[i]     = 0;
		w->sg[i]     = 261;             // a plain bolt sprite
	}
	w->trail = 0xFF;   // none
	w->sound = 1;
}

// Small LCG so Randomize gives variety without depending on the game RNG state.
static int customRand(int range)
{
	static unsigned int seed = 2463534242u;
	seed = seed * 1103515245u + 12345u;
	return (range > 0) ? (int)((seed >> 16) % (unsigned)range) : 0;
}

// The number of fire modes a source port defines (1 or 2), clamped to what we support.
static int sourcePortModes(int port)
{
	int m = weaponPort[port].opnum;
	if (m < 1) m = 1;
	if (m > CUSTOM_WEAPON_MODES) m = CUSTOM_WEAPON_MODES;
	return m;
}

// Resolve an import source + fire mode + base power level to a concrete weapon number.
static int resolveSourceWeapon(int presetIdx, int mode, int basePower)
{
	if (presetIdx < 0 || presetIdx >= customBulletPresetCount)
		return 0;

	const CustomBulletPreset *bp = &customBulletPreset[presetIdx];
	if (bp->sourcePort > 0 && bp->sourcePort <= PORT_NUM)
	{
		const int m    = clampi(mode, 0, sourcePortModes(bp->sourcePort) - 1);
		const int maxp = clampi(bp->maxPower, 1, CUSTOM_POWER_LEVELS);
		const int lvl  = clampi(basePower, 1, maxp) - 1;   // 0-based op[] index
		int wn = weaponPort[bp->sourcePort].op[m][lvl];
		for (int p = lvl; wn <= 0 && p >= 0; --p)          // fall back to a lower defined level
			wn = weaponPort[bp->sourcePort].op[m][p];
		for (int p = lvl + 1; wn <= 0 && p < 11; ++p)      // ... or a higher one
			wn = weaponPort[bp->sourcePort].op[m][p];
		return wn;
	}

	// Sidekick source: a charge sidekick's escalating shots sit at wpnum + 0..pwr, and
	// maxPower carries the shot count (pwr + 1). basePower selects the stage (1 =
	// uncharged). A non-charge sidekick has maxPower 1, so this is just wpnum.
	const int stages = clampi(bp->maxPower, 1, CUSTOM_POWER_LEVELS);
	return bp->sourceWeapon + clampi(basePower, 1, stages) - 1;
}

void customWeaponImportLevel(int presetIdx, int basePower)
{
	const int m  = clampi(customWeaponEditMode,  0, CUSTOM_WEAPON_MODES - 1);
	const int p  = clampi(customWeaponEditLevel, 0, CUSTOM_POWER_LEVELS - 1);
	const int wn = resolveSourceWeapon(presetIdx, m, basePower);
	if (wn <= 0 || wn > WEAP_NUM || weapons[wn].sg[0] == 0)
		return;

	customWeaponRaw[m][p] = weapons[wn];   // byte-for-byte copy
	sanitizeRawWeapon(&customWeaponRaw[m][p]);
}

void customWeaponImportAllLevels(int presetIdx)
{
	if (presetIdx < 0 || presetIdx >= customBulletPresetCount)
		return;

	const CustomBulletPreset *bp = &customBulletPreset[presetIdx];
	if (bp->sourcePort > 0 && bp->sourcePort <= PORT_NUM)
	{
		const int modes = sourcePortModes(bp->sourcePort);
		const int maxp  = clampi(bp->maxPower, 1, CUSTOM_POWER_LEVELS);
		customWeaponModes = modes;
		for (int m = 0; m < modes; ++m)
			for (int p = 0; p < CUSTOM_POWER_LEVELS; ++p)
			{
				const int lvl = (p < maxp) ? p : maxp - 1;   // clamp beyond the source's top level
				int wn = weaponPort[bp->sourcePort].op[m][lvl];
				for (int q = lvl; wn <= 0 && q >= 0; --q)     // fall back to a lower defined level
					wn = weaponPort[bp->sourcePort].op[m][q];
				if (wn > 0 && wn <= WEAP_NUM && weapons[wn].sg[0] != 0)
				{
					customWeaponRaw[m][p] = weapons[wn];
					sanitizeRawWeapon(&customWeaponRaw[m][p]);
				}
			}
		customWeaponPowerUse = clampi(weaponPort[bp->sourcePort].poweruse, 0, 255);
		if (customWeaponEditMode >= customWeaponModes)
			customWeaponEditMode = 0;
	}
	else
	{
		// Sidekick source (one mode). A charge sidekick lays its escalating shots out at
		// consecutive weapon numbers wpnum + 0..pwr; maxPower carries that count (pwr + 1).
		// Copy each stage into the matching power level so the whole charge ramp comes
		// across (e.g. the Charge-Laser's 6 shots -> power levels 1..6), then adopt the
		// real shot count. Levels past the last valid stage repeat the top stage.
		const int wn     = bp->sourceWeapon;                              // stage-0 weapon
		const int stages = clampi(bp->maxPower, 1, CUSTOM_POWER_LEVELS);  // charge-shot count
		customWeaponModes    = 1;
		customWeaponEditMode = 0;

		int lastValid = -1;
		for (int p = 0; p < CUSTOM_POWER_LEVELS; ++p)
		{
			const int stageWn = (p < stages) ? (wn + p) : -1;
			if (stageWn > 0 && stageWn <= WEAP_NUM && weapons[stageWn].sg[0] != 0)
			{
				customWeaponRaw[0][p] = weapons[stageWn];
				sanitizeRawWeapon(&customWeaponRaw[0][p]);
				lastValid = p;
			}
			else if (lastValid >= 0)
			{
				customWeaponRaw[0][p] = customWeaponRaw[0][lastValid];  // repeat the top stage
			}
		}

		// Adopt the real charge-shot count (1 = a non-charge sidekick). If it actually
		// charges, equip it as a sidekick since that's the only slot charging works in.
		customWeaponChargeStages = clampi(lastValid + 1, 1, CUSTOM_POWER_LEVELS);
		if (customWeaponChargeStages >= 2 &&
		    customWeaponEquipSlot != CUSTOM_EQUIP_LEFT &&
		    customWeaponEquipSlot != CUSTOM_EQUIP_RIGHT &&
		    customWeaponEquipSlot != CUSTOM_EQUIP_BOTH)
		{
			customWeaponEquipSlot = CUSTOM_EQUIP_LEFT;
		}

		// Clone the source sidekick's BODY (mount / sprite / animation) too, so importing a
		// stock sidekick reproduces how it looks and where it sits — e.g. the Micro Sol
		// FrontBlaster comes across front-mounted with its own pod sprite, not just its shot.
		if (bp->sourceOption > 0 && bp->sourceOption <= OPTION_NUM)
		{
			const JE_OptionType *so = &options[bp->sourceOption];
			customSidekickMount     = clampi(so->tr, 0, CUSTOM_SIDEKICK_MOUNTS - 1);
			customSidekickSprite    = clampi(so->gr[0], 1, 65535);   // body sprite is 1-based
			customSidekickFrames    = clampi(so->ani, 1, 20);
			customSidekickFrameStep = (so->ani > 1) ? clampi((int)so->gr[1] - (int)so->gr[0], 0, 40) : 1;
			customSidekickAnimate   = clampi(so->option, 1, 2);
		}
	}

	// Adopt the source's name — this is a full editable clone.
	strncpy(customWeaponName, bp->name, sizeof(customWeaponName) - 1);
	customWeaponName[sizeof(customWeaponName) - 1] = '\0';
}

// Append src's bullet segments onto dst (keeping dst's whole-volley fields), up to CUSTOM_BULLETS_MAX
// -- the core of "combining" two weapons into one design. dst keeps its fire rate, homing, spiral,
// sound, trail, etc.; only the per-bullet shape/motion arrays grow. Bullets past the cap are dropped.
static void combineWeaponInto(JE_WeaponType *dst, const JE_WeaponType *src)
{
	int n = clampi(dst->multi, 1, CUSTOM_BULLETS_MAX);
	const int add = clampi(src->multi, 1, CUSTOM_BULLETS_MAX);
	for (int i = 0; i < add && n < CUSTOM_BULLETS_MAX; ++i, ++n)
	{
		dst->sg[n]     = src->sg[i];
		dst->attack[n] = src->attack[i];
		dst->del[n]    = src->del[i];
		dst->sx[n]     = src->sx[i];
		dst->sy[n]     = src->sy[i];
		dst->bx[n]     = src->bx[i];
		dst->by[n]     = src->by[i];
	}
	dst->multi = (JE_byte)n;
	if (dst->max < dst->multi)   // fire all the combined bullets together each shot
		dst->max = dst->multi;
}

void customWeaponAddLevel(int presetIdx, int basePower)
{
	const int m  = clampi(customWeaponEditMode,  0, CUSTOM_WEAPON_MODES - 1);
	const int p  = clampi(customWeaponEditLevel, 0, CUSTOM_POWER_LEVELS - 1);
	const int wn = resolveSourceWeapon(presetIdx, m, basePower);
	if (wn <= 0 || wn > WEAP_NUM || weapons[wn].sg[0] == 0)
		return;

	combineWeaponInto(&customWeaponRaw[m][p], &weapons[wn]);
	sanitizeRawWeapon(&customWeaponRaw[m][p]);
}

void customWeaponAddAllLevels(int presetIdx)
{
	if (presetIdx < 0 || presetIdx >= customBulletPresetCount)
		return;

	const CustomBulletPreset *bp = &customBulletPreset[presetIdx];
	const int modes = clampi(customWeaponModes, 1, CUSTOM_WEAPON_MODES);

	if (bp->sourcePort > 0 && bp->sourcePort <= PORT_NUM)
	{
		// Add the source port's whole power curve: each of its power levels is combined onto the
		// matching custom level (levels past the source's top repeat its top level).
		const int smodes = sourcePortModes(bp->sourcePort);
		const int maxp   = clampi(bp->maxPower, 1, CUSTOM_POWER_LEVELS);
		for (int m = 0; m < modes; ++m)
		{
			const int sm = clampi(m, 0, smodes - 1);   // reuse the source's last mode if it has fewer
			for (int p = 0; p < CUSTOM_POWER_LEVELS; ++p)
			{
				const int lvl = (p < maxp) ? p : maxp - 1;   // clamp beyond the source's top level
				int wn = weaponPort[bp->sourcePort].op[sm][lvl];
				for (int q = lvl; wn <= 0 && q >= 0; --q)     // fall back to a lower defined level
					wn = weaponPort[bp->sourcePort].op[sm][q];
				if (wn > 0 && wn <= WEAP_NUM && weapons[wn].sg[0] != 0)
				{
					combineWeaponInto(&customWeaponRaw[m][p], &weapons[wn]);
					sanitizeRawWeapon(&customWeaponRaw[m][p]);
				}
			}
		}
	}
	else
	{
		// Sidekick source (mode 0 only): combine each escalating charge shot onto the matching
		// power level, repeating the top stage past the source's range so every custom level gets
		// the look (a non-charge sidekick has one shot, which is added to all 11 levels).
		const int wn     = bp->sourceWeapon;
		const int stages = clampi(bp->maxPower, 1, CUSTOM_POWER_LEVELS);
		int lastValidWn = 0;
		for (int p = 0; p < CUSTOM_POWER_LEVELS; ++p)
		{
			int stageWn = (p < stages) ? (wn + p) : 0;
			if (stageWn > 0 && stageWn <= WEAP_NUM && weapons[stageWn].sg[0] != 0)
				lastValidWn = stageWn;
			else
				stageWn = lastValidWn;   // repeat the top valid stage
			if (stageWn > 0 && stageWn <= WEAP_NUM && weapons[stageWn].sg[0] != 0)
			{
				combineWeaponInto(&customWeaponRaw[0][p], &weapons[stageWn]);
				sanitizeRawWeapon(&customWeaponRaw[0][p]);
			}
		}
	}
}

void customWeaponReset(void)
{
	const int m = clampi(customWeaponEditMode,  0, CUSTOM_WEAPON_MODES - 1);
	const int p = clampi(customWeaponEditLevel, 0, CUSTOM_POWER_LEVELS - 1);
	makeDefaultWeapon(&customWeaponRaw[m][p]);
}

void customWeaponResetAllLevels(void)
{
	for (int m = 0; m < CUSTOM_WEAPON_MODES; ++m)
		for (int p = 0; p < CUSTOM_POWER_LEVELS; ++p)
			makeDefaultWeapon(&customWeaponRaw[m][p]);
	strcpy(customWeaponName, "Custom Weapon");
	customWeaponCost         = 5000;
	customWeaponPowerUse     = 3;
	customWeaponEquipSlot    = CUSTOM_EQUIP_FRONT;
	customWeaponItemGraphic  = clampi(weaponPort[1].itemgraphic, 1, 237);
	customWeaponChargeStages = 1;
	customWeaponModes        = 1;
	customWeaponEditMode     = 0;
	customSidekickMount      = 0;
	customSidekickSprite     = 87;
	customSidekickFrames     = 1;
	customSidekickFrameStep  = 1;
	customSidekickAnimate    = 1;
}

void customWeaponCopyToAllLevels(void)
{
	// Copy the level currently being edited into every level of the current mode.
	const int m   = clampi(customWeaponEditMode,  0, CUSTOM_WEAPON_MODES - 1);
	const int src = clampi(customWeaponEditLevel, 0, CUSTOM_POWER_LEVELS - 1);
	for (int p = 0; p < CUSTOM_POWER_LEVELS; ++p)
		if (p != src)
			customWeaponRaw[m][p] = customWeaponRaw[m][src];
}

void customWeaponAutoScaleLevels(void)
{
	const int m = clampi(customWeaponEditMode,  0, CUSTOM_WEAPON_MODES - 1);
	const int a = clampi(customWeaponEditLevel, 0, CUSTOM_POWER_LEVELS - 1);
	const JE_WeaponType anchor = customWeaponRaw[m][a];   // reference design (by value)
	const int baseMulti = clampi(anchor.multi, 1, CUSTOM_BULLETS_MAX);

	for (int p = 0; p < CUSTOM_POWER_LEVELS; ++p)
	{
		if (p == a)
			continue;   // never touch the level the player actually tuned

		// Geometric power ramp about the anchor: exactly 1.0 at the anchor, weaker below and
		// stronger above (~15% per level). Done as a small loop so we don't pull in <math.h>.
		double mult = 1.0;
		const int steps = p - a;
		for (int s = 0; s < (steps >= 0 ? steps : -steps); ++s)
			mult = (steps >= 0) ? mult * 1.15 : mult / 1.15;

		JE_WeaponType w = anchor;   // start from the anchor, then scale the power axes

		// Damage per bullet — only real damage (1..98) scales; Ice (99), chain (101..249)
		// and piercing (>=250) are semantic codes, left exactly as designed.
		for (int i = 0; i < baseMulti; ++i)
		{
			const int at = anchor.attack[i];
			if (at >= 1 && at <= 98)
				w.attack[i] = (JE_byte)clampi((int)(at * mult + 0.5), 1, 98);
		}

		// Fire rate — more power fires faster, so the shot period (shotrepeat + 1) scales by
		// 1/mult. Keeps 0 ("every tick", a solid laser) reachable at the strong end.
		{
			const int period = (int)((anchor.shotrepeat + 1) / mult + 0.5);
			w.shotrepeat = (JE_byte)clampi(period - 1, 0, 255);
		}

		// Bullet count — more bullets at higher power. Shrink by keeping the centre-most
		// segments; grow by fanning out extra copies of the anchor's segments.
		const int target = clampi((int)(baseMulti * mult + 0.5), 1, CUSTOM_BULLETS_MAX);
		if (target < baseMulti)
		{
			const int start = (baseMulti - target) / 2;   // centred slice of the original fan
			for (int i = 0; start > 0 && i < target; ++i)
			{
				const int s = start + i;
				w.sg[i] = w.sg[s];  w.attack[i] = w.attack[s];  w.del[i] = w.del[s];
				w.sx[i] = w.sx[s];  w.sy[i]     = w.sy[s];
				w.bx[i] = w.bx[s];  w.by[i]     = w.by[s];
			}
		}
		else if (target > baseMulti)
		{
			for (int i = baseMulti; i < target; ++i)
			{
				const int s    = i % baseMulti;                     // reuse an original segment
				const int rank = (i - baseMulti) / baseMulti + 1;   // 1,1,..,2,2,..
				const int sign = ((i - baseMulti) & 1) ? 1 : -1;    // alternate sides
				w.sg[i] = anchor.sg[s];  w.attack[i] = w.attack[s];  w.del[i] = anchor.del[s];
				w.sx[i] = anchor.sx[s];  w.sy[i]     = anchor.sy[s];
				w.bx[i] = (JE_shortint)clampi(anchor.bx[s] + sign * rank * 8, -128, 127);
				w.by[i] = anchor.by[s];
			}
		}
		w.multi = (JE_byte)target;
		if (w.max < w.multi)
			w.max = w.multi;

		customWeaponRaw[m][p] = w;
	}
}

// The design (raw weapon) the editor is currently pointed at.
static JE_WeaponType *customEditingRaw(void)
{
	const int m = clampi(customWeaponEditMode,  0, CUSTOM_WEAPON_MODES - 1);
	const int p = clampi(customWeaponEditLevel, 0, CUSTOM_POWER_LEVELS - 1);
	return &customWeaponRaw[m][p];
}

int customWeaponAddBullet(int afterIndex)
{
	JE_WeaponType *w = customEditingRaw();
	const int n = clampi(w->multi, 1, CUSTOM_BULLETS_MAX);
	if (n >= CUSTOM_BULLETS_MAX)
		return -1;   // already at CUSTOM_BULLETS_MAX

	const int from = clampi(afterIndex, 0, n - 1);
	const int at   = from + 1;   // insert the copy directly after its source

	// Open a gap at `at` by shifting the higher segments up one slot.
	for (int i = n; i > at; --i)
	{
		w->sg[i] = w->sg[i - 1];  w->attack[i] = w->attack[i - 1];  w->del[i] = w->del[i - 1];
		w->sx[i] = w->sx[i - 1];  w->sy[i]     = w->sy[i - 1];
		w->bx[i] = w->bx[i - 1];  w->by[i]     = w->by[i - 1];
	}

	// The new segment duplicates its source, nudged sideways so the two don't overlap.
	w->sg[at] = w->sg[from];  w->attack[at] = w->attack[from];  w->del[at] = w->del[from];
	w->sx[at] = w->sx[from];  w->sy[at]     = w->sy[from];
	w->bx[at] = (JE_shortint)clampi(w->bx[from] + 8, -128, 127);
	w->by[at] = w->by[from];

	w->multi = (JE_byte)(n + 1);
	if (w->max < w->multi)   // keep the pattern cycle at least as long as the bullet count
		w->max = w->multi;
	return at;
}

int customWeaponRemoveBullet(int index)
{
	JE_WeaponType *w = customEditingRaw();
	const int n = clampi(w->multi, 1, CUSTOM_BULLETS_MAX);
	if (n <= 1)
		return -1;   // a design must keep at least one bullet

	const int at = clampi(index, 0, n - 1);

	// Close the gap by shifting the higher segments down one slot.
	for (int i = at; i < n - 1; ++i)
	{
		w->sg[i] = w->sg[i + 1];  w->attack[i] = w->attack[i + 1];  w->del[i] = w->del[i + 1];
		w->sx[i] = w->sx[i + 1];  w->sy[i]     = w->sy[i + 1];
		w->bx[i] = w->bx[i + 1];  w->by[i]     = w->by[i + 1];
	}

	// Clear the now-unused top slot so a later Add starts from a clean segment.
	const int last = n - 1;
	w->sg[last] = 0;  w->attack[last] = 0;  w->del[last] = 0;
	w->sx[last] = 0;  w->sy[last] = 0;  w->bx[last] = 0;  w->by[last] = 0;

	w->multi = (JE_byte)(n - 1);
	// Leave `max` alone (matching the Bullets row): a design that deliberately keeps
	// max > multi for a variation pattern survives a segment removal.
	return (at >= n - 1) ? at - 1 : at;   // select the segment now occupying this slot
}

int customWeaponAddChargeState(void)
{
	if (customWeaponChargeStages >= CUSTOM_POWER_LEVELS)
		return -1;   // every power level is already a charge state

	++customWeaponChargeStages;
	// Jump to the new top stage's design so its shot can be tuned right away. Its starting
	// content is whatever that power level already holds (a copy of the previous top when
	// the weapon was imported, else the default) — non-destructive, edit it from here.
	customWeaponEditLevel = customWeaponChargeStages - 1;
	return customWeaponEditLevel;
}

int customWeaponRemoveChargeState(void)
{
	if (customWeaponChargeStages <= 1)
		return -1;   // one shot left = no charging; can't remove further

	--customWeaponChargeStages;
	if (customWeaponEditLevel >= customWeaponChargeStages)
		customWeaponEditLevel = customWeaponChargeStages - 1;   // keep the edit level in range
	return customWeaponEditLevel;
}

void customWeaponRandomize(void)
{
	// Prefix + noun, "Neon Blaster" style. Big pools = thousands of combinations; every
	// word is short enough that any pair fits the 30-char name buffer.
	static const char *const parts1[] = {
		"Neon", "Hyper", "Chaos", "Star", "Vortex", "Plasma", "Ion", "Rift",
		"Quantum", "Nova", "Solar", "Cosmic", "Astral", "Void", "Nebula", "Photon",
		"Gamma", "Omega", "Delta", "Sigma", "Turbo", "Mega", "Ultra", "Cyber",
		"Atomic", "Nuclear", "Fusion", "Pulse", "Volt", "Blaze", "Frost", "Thunder",
		"Shadow", "Crimson", "Azure", "Ember", "Static", "Havoc", "Doom", "Fury",
		"Lunar", "Stellar", "Prism", "Zenith", "Apex", "Titan", "Warp", "Flux",
		"Spectral", "Radiant", "Searing", "Venom", "Dread", "Savage", "Wraith", "Onyx",
		"Cobalt", "Scarlet", "Viper", "Phantom", "Inferno", "Tempest", "Cyclone", "Meteor",
		"Pulsar", "Quasar", "Prime", "Hex", "Neutron", "Tesla", "Sonic", "Blitz",
		"Rogue", "Feral", "Molten", "Frenzy", "Rune", "Aether", "Chrome", "Storm",
	};
	static const char *const parts2[] = {
		"Blaster", "Cannon", "Spray", "Lance", "Storm", "Fang", "Burst", "Ray",
		"Beam", "Blast", "Bomb", "Rifle", "Repeater", "Driver", "Launcher", "Reaper",
		"Striker", "Slayer", "Breaker", "Shredder", "Piercer", "Wave", "Barrage", "Volley",
		"Salvo", "Hammer", "Spike", "Needle", "Dagger", "Blade", "Edge", "Claw",
		"Talon", "Sting", "Jet", "Stream", "Torrent", "Flare", "Coil", "Whip",
		"Scythe", "Saber", "Disruptor", "Annihilator", "Obliterator", "Devastator", "Vaporizer", "Nullifier",
		"Destroyer", "Enforcer", "Punisher", "Executioner", "Gun", "Turret", "Emitter", "Projector",
		"Igniter", "Repulsor", "Maw", "Render", "Bringer", "Cutter", "Screamer", "Howitzer",
	};

	const int m = clampi(customWeaponEditMode,  0, CUSTOM_WEAPON_MODES - 1);
	const int p = clampi(customWeaponEditLevel, 0, CUSTOM_POWER_LEVELS - 1);

	// Start from a random real weapon look (always valid), then perturb a little.
	if (customBulletPresetCount > 0)
	{
		const int idx = customRand(customBulletPresetCount);
		customWeaponImportLevel(idx, 1 + customRand(customBulletMaxPower(idx)));
	}
	else
	{
		makeDefaultWeapon(&customWeaponRaw[m][p]);
	}

	JE_WeaponType *w = &customWeaponRaw[m][p];
	w->shotrepeat = (JE_byte)clampi((int)w->shotrepeat + customRand(7) - 3, 1, 30);
	w->sound      = (JE_byte)(1 + customRand(CUSTOM_SOUND_MAX));

	snprintf(customWeaponName, sizeof(customWeaponName), "%s %s",
	         parts1[customRand(COUNTOF(parts1))], parts2[customRand(COUNTOF(parts2))]);
}

// The sprite sheet a mount style draws its body from: front (2) and trailing-large (1)
// use the 2x2 spriteSheet10, every other style the single-tile spriteSheet9 (mirrors the
// sidekick draw in mainint.c).
static Sprite2_array *sidekickSheet(int mount)
{
	return (mount == 1 || mount == 2) ? &spriteSheet10 : &spriteSheet9;
}

int customSidekickSpriteCount(int mount)
{
	const Sprite2_array *s = sidekickSheet(mount);
	if (s->data == NULL || s->size < 2)
		return 0;   // sheet not loaded yet
	return SDL_SwapLE16(((const Uint16 *)s->data)[0]) / 2;   // offset table's first entry = #sprites * 2
}

// Synthesize the custom sidekick (an options[] entry firing the compiled custom weapon);
// rebuilt whenever the weapon changes. It fires the mode-0 designs: with charge (pwr > 0)
// the engine fires wpnum + charge, and the consecutive level slots make the per-level curve
// the charge ramp. Body appearance comes from the customSidekick* globals.
static void customSidekickMaterialize(void)
{
	if (customSidekickSlot <= 0 || customSidekickSlot > OPTION_NUM)
		return;

	// A charge sidekick fires wpnum + charge (charge in 0..pwr), so it needs a valid
	// weapon at each of those slots. customWeaponChargeStages is the shot COUNT (1..11),
	// so pwr = count - 1, and the top stage stays within our compiled mode-0 levels
	// (CUSTOM_WEAP_BASE .. CUSTOM_WEAP_BASE + CUSTOM_POWER_LEVELS-1).
	const int pwr = clampi(customWeaponChargeStages - 1, 0, CUSTOM_POWER_LEVELS - 1);

	const int mount   = clampi(customSidekickMount,     0, CUSTOM_SIDEKICK_MOUNTS - 1);
	const int frames  = clampi(customSidekickFrames,    1, 20);
	const int step    = clampi(customSidekickFrameStep, 0, 40);
	const int animate = clampi(customSidekickAnimate,   1, 2);

	// Clamp every body sprite the engine can read into the sheet's valid range. The blit is
	// 1-BASED (blit_sprite2 reads offsetTable[index-1], so index 0 underflows and crashes); a
	// 2x2 mount also reads index+1/+19/+20; and the engine adds `charge` (0..pwr) at draw time.
	// So a body index must stay in 1 .. count - pwr - (20 for a 2x2 mount). Clamp the base and
	// every animation frame to that window (the blit is not otherwise bounds-checked).
	const int count = customSidekickSpriteCount(mount);
	const int extra = (mount == 1 || mount == 2) ? 20 : 0;          // blit_sprite2x2 index+1/+19/+20
	const int hiIdx = (count > 0 && count - pwr - extra >= 1) ? count - pwr - extra : 1;
	const int base  = clampi(customSidekickSprite, 1, hiIdx);

	JE_OptionType *o = &options[customSidekickSlot];
	memset(o, 0, sizeof(*o));
	strncpy(o->name, customWeaponName, 30);
	o->name[30]    = '\0';
	o->pwr         = (JE_byte)pwr;                   // 0 = instant fire; N = N more charge shots
	o->itemgraphic = 193;                            // valid shop icon
	o->cost        = (JE_word)clampi(customWeaponCost, 0, 64000);
	o->tr          = (JE_byte)mount;                 // mount style (also selects the body sprite sheet)
	o->option      = (JE_byte)animate;               // 1 = animate while firing, 2 = always
	o->opspd       = 3;
	o->ani         = (JE_byte)frames;
	for (int f = 0; f < 20; ++f)                     // simple flip-book: base, base+step, base+2*step, ...
		o->gr[f] = (JE_word)clampi((f < frames) ? base + f * step : base, 1, hiIdx);
	o->wport       = (JE_byte)customWeaponPort;      // fires through the custom port ...
	o->wpnum       = CUSTOM_WEAP_BASE;               // ... mode-0 level 1 (+ charge steps up levels)
	o->ammo        = 0;                              // infinite
	o->stop        = true;
	o->icongr      = 6;
}

void customWeaponMaterialize(void)
{
	if (customWeaponPort <= 0 || customWeaponPort > PORT_NUM)
		return;

	customWeaponModes = clampi(customWeaponModes, 1, CUSTOM_WEAPON_MODES);

	// Compile every (mode, level): the design already IS a weapon struct, so this is
	// a plain copy into its own scratch slot (kept separate from the source weapons).
	for (int m = 0; m < CUSTOM_WEAPON_MODES; ++m)
		for (int p = 0; p < CUSTOM_POWER_LEVELS; ++p)
		{
			sanitizeRawWeapon(&customWeaponRaw[m][p]);
			weapons[customScratchSlot(m, p)] = customWeaponRaw[m][p];
		}

	// The reserved port: fire mode M, power level P (1..11) fires the matching slot.
	// A single-mode weapon points op[1] at the mode-0 slots so a rear-gun toggle is a
	// no-op instead of firing an undesigned bank.
	strncpy(weaponPort[customWeaponPort].name, customWeaponName, 30);
	weaponPort[customWeaponPort].name[30] = '\0';
	weaponPort[customWeaponPort].opnum = (JE_byte)customWeaponModes;
	for (int p = 0; p < 11; ++p)
	{
		weaponPort[customWeaponPort].op[0][p] = customScratchSlot(0, p);
		weaponPort[customWeaponPort].op[1][p] = customScratchSlot(customWeaponModes >= 2 ? 1 : 0, p);
	}
	weaponPort[customWeaponPort].cost        = (JE_word)clampi(customWeaponCost, 0, 64000);
	weaponPort[customWeaponPort].itemgraphic = (JE_word)clampi(customWeaponItemGraphic, 1, 237);  // shop/HUD icon
	weaponPort[customWeaponPort].poweruse    = (JE_word)clampi(customWeaponPowerUse, 0, 255);

	customSidekickMaterialize();  // keep the sidekick in sync with the weapon

	// The compiled weapon just changed under the live fire state. Restart every bay's fire cursor
	// so none is left pointing past the new design's pattern length (`max`) — those slots hold
	// sg 0, which player_shot_create discards (an invisible gun) — and drop any cooldown inherited
	// from the previous design so the new one fires on the next tick. Materialize only runs from
	// menus / item-data load, so this matches the reset the game does on every in-game weapon switch.
	memset(shotMultiPos, 0, sizeof(shotMultiPos));
	memset(shotRepeat, 1, sizeof(shotRepeat));
}

void customWeaponSelectLevel(int level)
{
	customWeaponEditLevel = clampi(level, 0, CUSTOM_POWER_LEVELS - 1);
}

void customWeaponSelectMode(int mode)
{
	customWeaponEditMode = clampi(mode, 0, CUSTOM_WEAPON_MODES - 1);
}

bool customWeaponEquip(void)
{
	if (!customWeaponEnabled || customWeaponPort <= 0 || customWeaponPort > PORT_NUM)
		return false;

	customWeaponMaterialize();

	switch (customWeaponEquipSlot)
	{
	case CUSTOM_EQUIP_REAR:
		player[0].items.weapon[REAR_WEAPON].id = (Uint8)customWeaponPort;
		player[0].items.weapon[REAR_WEAPON].power = 1;
		break;
	case CUSTOM_EQUIP_LEFT:
		if (customSidekickSlot <= 0)
			return false;
		player[0].items.sidekick[LEFT_SIDEKICK] = (Uint8)customSidekickSlot;
		break;
	case CUSTOM_EQUIP_RIGHT:
		if (customSidekickSlot <= 0)
			return false;
		player[0].items.sidekick[RIGHT_SIDEKICK] = (Uint8)customSidekickSlot;
		break;
	case CUSTOM_EQUIP_BOTH:
		if (customSidekickSlot <= 0)
			return false;
		player[0].items.sidekick[LEFT_SIDEKICK]  = (Uint8)customSidekickSlot;
		player[0].items.sidekick[RIGHT_SIDEKICK] = (Uint8)customSidekickSlot;
		break;
	default:  // CUSTOM_EQUIP_FRONT
		player[0].items.weapon[FRONT_WEAPON].id = (Uint8)customWeaponPort;
		player[0].items.weapon[FRONT_WEAPON].power = 1;
		break;
	}
	return true;
}

// ---- import-source list -----------------------------------------------------

// Copy a weapon/sidekick name for the picker, stripping the data's cosmetic
// shop formatting (leading/trailing padding and the " Ammo <count>" suffix that
// some sidekicks carry, e.g. "Phoenix Device Ammo 8" -> "Phoenix Device").
static void copyBulletName(char *dst, size_t dstsize, const char *src)
{
	if (dstsize == 0)
		return;

	while (*src == ' ' || *src == '\t')  // skip leading padding
		++src;
	size_t len = strlen(src);

	for (size_t i = 0; i + 5 <= len; ++i)  // cut a trailing standalone " Ammo[ <count>]" token
	{
		if (src[i] == ' ' && memcmp(&src[i + 1], "Ammo", 4) == 0 &&
		    (src[i + 5] == ' ' || src[i + 5] == '\0'))
		{
			len = i;
			break;
		}
	}

	while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t'))  // trim trailing
		--len;
	if (len > dstsize - 1)
		len = dstsize - 1;
	memcpy(dst, src, len);
	dst[len] = '\0';
}

static bool bulletNameSeen(const char *name)
{
	for (int j = 0; j < customBulletPresetCount; ++j)
		if (SDL_strcasecmp(customBulletPreset[j].name, name) == 0)
			return true;
	return false;
}

// Add one front/rear weapon port as an import source (remembers the port so the
// look can be sampled at any of its power levels / modes). Deduped by name.
static void addPortPreset(int port)
{
	if (customBulletPresetCount >= CUSTOM_BULLET_PRESET_MAX)
		return;

	const char *pn = weaponPort[port].name;
	if (pn[0] == '\0' || SDL_strcasecmp(pn, "None") == 0 || SDL_strcasecmp(pn, "Test") == 0)
		return;

	int maxLvl = 0;  // highest defined power level (1-based); skip a port with no weapons
	for (int p = 10; p >= 0; --p)
		if (weaponPort[port].op[0][p] > 0) { maxLvl = p + 1; break; }
	if (maxLvl == 0)
		return;

	char nm[31];
	copyBulletName(nm, sizeof(nm), pn);
	if (bulletNameSeen(nm))
		return;

	CustomBulletPreset *bp = &customBulletPreset[customBulletPresetCount++];
	strcpy(bp->name, nm);
	bp->sourcePort   = (JE_word)port;
	bp->sourceWeapon = 0;
	bp->sourceOption = 0;
	bp->maxPower     = (JE_byte)maxLvl;
}

// Add one sidekick option as an import source. A non-charge sidekick is a single fixed
// weapon; a charge sidekick (option pwr > 0) has pwr+1 escalating shots at wpnum + 0..pwr,
// which maxPower records so its whole charge ramp can be imported.
static void addOptionPreset(int opt)
{
	if (customBulletPresetCount >= CUSTOM_BULLET_PRESET_MAX)
		return;

	const char *on = options[opt].name;
	if (on[0] == '\0' || SDL_strcasecmp(on, "None") == 0)
		return;

	const int wn = options[opt].wpnum;
	if (wn <= 0 || wn > WEAP_NUM || weapons[wn].sg[0] == 0)
		return;

	char nm[31];
	copyBulletName(nm, sizeof(nm), on);
	if (bulletNameSeen(nm))
		return;

	CustomBulletPreset *bp = &customBulletPreset[customBulletPresetCount++];
	strcpy(bp->name, nm);
	bp->sourcePort   = 0;
	bp->sourceWeapon = (JE_word)wn;
	bp->sourceOption = (JE_word)opt;   // remember the option so Import can clone its body
	bp->maxPower     = (JE_byte)clampi(options[opt].pwr + 1, 1, CUSTOM_POWER_LEVELS);  // charge-shot count
}

static void buildBulletPresets(void)
{
	customBulletPresetCount = 0;

	for (int i = 1; i <= PORT_NUM; ++i)
		addPortPreset(i);

	for (int i = 1; i <= OPTION_NUM; ++i)
		addOptionPreset(i);
}

// ---- persistence ------------------------------------------------------------

// How many bullet slots actually carry data, so the serializer only writes those. The per-bullet
// arrays are now WEAPON_MULTI_MAX (255) wide, but a typical weapon uses only a handful of bullets;
// writing the full width would bloat every saved design to ~12 KB. At least the fired range
// (multi/max), extended upward to include any higher slot that still holds data (so a design is
// never silently truncated). deserializeRaw recovers this width from the blob's integer count.
static int usedBulletCount(const JE_WeaponType *w)
{
	int n = (w->multi > w->max) ? w->multi : w->max;
	if (n < 1) n = 1;
	if (n > CUSTOM_BULLETS_MAX) n = CUSTOM_BULLETS_MAX;
	for (int i = CUSTOM_BULLETS_MAX - 1; i >= n; --i)
		if (w->sg[i] || w->attack[i] || w->del[i] || w->sx[i] || w->sy[i] || w->bx[i] || w->by[i])
		{
			n = i + 1;
			break;
		}
	return n;
}

// Serialize one raw weapon to a compact, whitespace-free comma list of ints (parser-safe).
static void serializeRaw(const JE_WeaponType *w, char *buf, size_t bufSize)
{
	int o = 0;
	#define PUT(v) do { \
		if (o != 0 && o < (int)bufSize - 1) buf[o++] = ','; \
		int _r = snprintf(buf + o, (o < (int)bufSize) ? (size_t)(bufSize - o) : 0, "%d", (int)(v)); \
		if (_r > 0) o += _r; \
	} while (0)

	const int nb = usedBulletCount(w);   // only the populated slots (arrays are WEAPON_MULTI_MAX wide)

	PUT(w->drain); PUT(w->shotrepeat); PUT(w->multi); PUT(w->weapani);
	PUT(w->max); PUT(w->tx); PUT(w->ty); PUT(w->aim);
	for (int i = 0; i < nb; ++i) PUT(w->attack[i]);
	for (int i = 0; i < nb; ++i) PUT(w->del[i]);
	for (int i = 0; i < nb; ++i) PUT(w->sx[i]);
	for (int i = 0; i < nb; ++i) PUT(w->sy[i]);
	for (int i = 0; i < nb; ++i) PUT(w->bx[i]);
	for (int i = 0; i < nb; ++i) PUT(w->by[i]);
	for (int i = 0; i < nb; ++i) PUT(w->sg[i]);
	PUT(w->acceleration); PUT(w->accelerationx); PUT(w->circlesize);
	PUT(w->sound); PUT(w->trail); PUT(w->shipblastfilter);

	#undef PUT
}

// Parse a raw weapon back from serializeRaw()'s comma list, then clamp it to valid limits.
static void deserializeRaw(JE_WeaponType *w, const char *str)
{
	memset(w, 0, sizeof(*w));

	// Room for the widest blob: 8 leading scalars + 7 arrays x CUSTOM_BULLETS_MAX + 6 trailing scalars.
	long vals[14 + 7 * CUSTOM_BULLETS_MAX];
	int cnt = 0;
	while (*str != '\0' && cnt < (int)COUNTOF(vals))
	{
		char *end;
		long v = strtol(str, &end, 10);
		if (end == str)
			break;
		vals[cnt++] = v;
		str = end;
		while (*str == ',' || *str == ' ')
			++str;
	}

	// Detect the per-bullet array width this blob was written with. The layout is 8 leading scalars +
	// 7 arrays of width W + 6 trailing scalars = 14 + 7*W integers, so W = (cnt - 14) / 7. Configs
	// written before the bullet cap was raised used W = 8; reading exactly W per array keeps the
	// trailing scalars aligned, so an old design still loads correctly (its unused slots stay zero).
	int width = (cnt >= 14) ? (cnt - 14) / 7 : CUSTOM_BULLETS_MAX;
	if (width < 1) width = 1;
	if (width > CUSTOM_BULLETS_MAX) width = CUSTOM_BULLETS_MAX;

	int i = 0;
	#define GET() ((i < cnt) ? vals[i++] : 0)

	w->drain = (JE_word)GET(); w->shotrepeat = (JE_byte)GET(); w->multi = (JE_byte)GET(); w->weapani = (JE_word)GET();
	w->max = (JE_byte)GET(); w->tx = (JE_byte)GET(); w->ty = (JE_byte)GET(); w->aim = (JE_byte)GET();
	for (int k = 0; k < width; ++k) w->attack[k] = (JE_byte)GET();
	for (int k = 0; k < width; ++k) w->del[k] = (JE_byte)GET();
	for (int k = 0; k < width; ++k) w->sx[k] = (JE_shortint)GET();
	for (int k = 0; k < width; ++k) w->sy[k] = (JE_shortint)GET();
	for (int k = 0; k < width; ++k) w->bx[k] = (JE_shortint)GET();
	for (int k = 0; k < width; ++k) w->by[k] = (JE_shortint)GET();
	for (int k = 0; k < width; ++k) w->sg[k] = (JE_word)GET();
	w->acceleration = (JE_shortint)GET(); w->accelerationx = (JE_shortint)GET(); w->circlesize = (JE_byte)GET();
	w->sound = (JE_byte)GET(); w->trail = (JE_byte)GET(); w->shipblastfilter = (JE_byte)GET();

	#undef GET

	sanitizeRawWeapon(w);
}

void customWeaponSerializeLevel(int mode, int level, char *buf, size_t bufSize)
{
	mode  = clampi(mode,  0, CUSTOM_WEAPON_MODES - 1);
	level = clampi(level, 0, CUSTOM_POWER_LEVELS - 1);
	serializeRaw(&customWeaponRaw[mode][level], buf, bufSize);
}

void customWeaponDeserializeLevel(int mode, int level, const char *str)
{
	mode  = clampi(mode,  0, CUSTOM_WEAPON_MODES - 1);
	level = clampi(level, 0, CUSTOM_POWER_LEVELS - 1);
	deserializeRaw(&customWeaponRaw[mode][level], str);
}

// ---- weapon library ---------------------------------------------------------

// Copy the editable globals into / out of a library slot.
static void storeToSlot(int i)
{
	if (i < 0 || i >= CUSTOM_WEAPON_LIB_MAX)
		return;
	CustomWeaponSlot *s = &customWeaponLib[i];
	strncpy(s->name, customWeaponName, sizeof(s->name) - 1);
	s->name[sizeof(s->name) - 1] = '\0';
	s->cost         = customWeaponCost;
	s->powerUse     = customWeaponPowerUse;
	s->equipSlot    = customWeaponEquipSlot;
	s->itemGraphic  = customWeaponItemGraphic;
	s->chargeStages = customWeaponChargeStages;
	s->modes        = customWeaponModes;
	s->sidekickMount     = customSidekickMount;
	s->sidekickSprite    = customSidekickSprite;
	s->sidekickFrames    = customSidekickFrames;
	s->sidekickFrameStep = customSidekickFrameStep;
	s->sidekickAnimate   = customSidekickAnimate;
	memcpy(s->raw, customWeaponRaw, sizeof(customWeaponRaw));
}

static void loadFromSlot(int i)
{
	if (i < 0 || i >= CUSTOM_WEAPON_LIB_MAX)
		return;
	const CustomWeaponSlot *s = &customWeaponLib[i];
	strncpy(customWeaponName, s->name, sizeof(customWeaponName) - 1);
	customWeaponName[sizeof(customWeaponName) - 1] = '\0';
	customWeaponCost         = s->cost;
	customWeaponPowerUse     = s->powerUse;
	customWeaponEquipSlot    = s->equipSlot;
	customWeaponItemGraphic  = s->itemGraphic;
	customWeaponChargeStages = s->chargeStages;
	customWeaponModes        = clampi(s->modes, 1, CUSTOM_WEAPON_MODES);
	customSidekickMount      = s->sidekickMount;
	customSidekickSprite     = s->sidekickSprite;
	customSidekickFrames     = s->sidekickFrames;
	customSidekickFrameStep  = s->sidekickFrameStep;
	customSidekickAnimate    = s->sidekickAnimate;
	memcpy(customWeaponRaw, s->raw, sizeof(customWeaponRaw));
	customWeaponEditLevel = 0;   // open the switched-to weapon at level 1 / mode 1
	customWeaponEditMode  = 0;
}

// Fill a slot with the built-in default weapon (used for New and for unwritten slots).
static void slotSetDefault(CustomWeaponSlot *s)
{
	memset(s, 0, sizeof(*s));
	strcpy(s->name, "Custom Weapon");
	s->cost         = 5000;
	s->powerUse     = 3;
	s->equipSlot    = CUSTOM_EQUIP_FRONT;
	s->itemGraphic  = clampi(weaponPort[1].itemgraphic, 1, 237);
	s->chargeStages = 1;
	s->modes        = 1;
	s->sidekickMount     = 0;    // side pod
	s->sidekickSprite    = 87;   // Charge-Laser side-pod body frame
	s->sidekickFrames    = 1;
	s->sidekickFrameStep = 1;
	s->sidekickAnimate   = 1;
	for (int m = 0; m < CUSTOM_WEAPON_MODES; ++m)
		for (int p = 0; p < CUSTOM_POWER_LEVELS; ++p)
			makeDefaultWeapon(&s->raw[m][p]);
}

void customWeaponSelectSlot(int slot)
{
	if (customWeaponLibCount <= 0)
		return;
	slot = clampi(slot, 0, customWeaponLibCount - 1);
	if (slot == customWeaponCurrentSlot)
		return;
	storeToSlot(customWeaponCurrentSlot);   // keep the working copy
	customWeaponCurrentSlot = slot;
	loadFromSlot(slot);
	customWeaponMaterialize();
}

int customWeaponLibraryNew(void)
{
	if (customWeaponLibCount >= CUSTOM_WEAPON_LIB_MAX)
		return -1;
	storeToSlot(customWeaponCurrentSlot);
	const int ni = customWeaponLibCount++;
	slotSetDefault(&customWeaponLib[ni]);
	snprintf(customWeaponLib[ni].name, sizeof(customWeaponLib[ni].name), "Weapon %d", ni + 1);
	customWeaponCurrentSlot = ni;
	loadFromSlot(ni);
	customWeaponMaterialize();
	return ni;
}

int customWeaponLibraryDuplicate(void)
{
	if (customWeaponLibCount >= CUSTOM_WEAPON_LIB_MAX)
		return -1;
	storeToSlot(customWeaponCurrentSlot);
	const int ni = customWeaponLibCount++;
	customWeaponLib[ni] = customWeaponLib[customWeaponCurrentSlot];   // copy the whole design
	// Mark the copy in its name if there's room.
	char *nm = customWeaponLib[ni].name;
	if (strlen(nm) <= sizeof(customWeaponLib[ni].name) - 6)
		strcat(nm, " Copy");
	customWeaponCurrentSlot = ni;
	loadFromSlot(ni);
	customWeaponMaterialize();
	return ni;
}

int customWeaponLibraryDelete(void)
{
	if (customWeaponLibCount <= 1)
		return -1;   // always keep at least one weapon
	for (int i = customWeaponCurrentSlot; i < customWeaponLibCount - 1; ++i)
		customWeaponLib[i] = customWeaponLib[i + 1];
	--customWeaponLibCount;
	if (customWeaponCurrentSlot >= customWeaponLibCount)
		customWeaponCurrentSlot = customWeaponLibCount - 1;
	loadFromSlot(customWeaponCurrentSlot);
	customWeaponMaterialize();
	return customWeaponCurrentSlot;
}

void customWeaponLibrarySave(void)
{
	if (customWeaponLibCount < 1)           // never write an empty library (would lose the weapon)
	{
		customWeaponLibCount    = 1;
		customWeaponCurrentSlot = 0;
	}
	customWeaponCurrentSlot = clampi(customWeaponCurrentSlot, 0, customWeaponLibCount - 1);
	storeToSlot(customWeaponCurrentSlot);   // capture the working copy first

	FILE *f = dir_fopen(get_user_directory(), "custom_weapons.cfg", "w");
	if (f == NULL)
		return;

	fprintf(f, "custom_weapons 1\n");
	fprintf(f, "count %d\n", customWeaponLibCount);
	fprintf(f, "current %d\n", customWeaponCurrentSlot);

	char blob[16384];   // one raw-weapon blob; sized for the widest (255-bullet) design
	for (int i = 0; i < customWeaponLibCount; ++i)
	{
		const CustomWeaponSlot *s = &customWeaponLib[i];
		fprintf(f, "weapon %d\n", i);
		fprintf(f, "name %s\n", s->name);
		fprintf(f, "props %d %d %d %d %d %d\n",
		        s->cost, s->powerUse, s->equipSlot, s->itemGraphic, s->chargeStages, s->modes);
		fprintf(f, "sidekick %d %d %d %d %d\n",
		        s->sidekickMount, s->sidekickSprite, s->sidekickFrames, s->sidekickFrameStep, s->sidekickAnimate);
		for (int m = 0; m < CUSTOM_WEAPON_MODES; ++m)
			for (int p = 0; p < CUSTOM_POWER_LEVELS; ++p)
			{
				serializeRaw(&s->raw[m][p], blob, sizeof(blob));
				fprintf(f, "raw %d %d %s\n", m, p, blob);
			}
	}

	fclose(f);
}

void customWeaponLibraryLoad(void)
{
	FILE *f = dir_fopen_warn(get_user_directory(), "custom_weapons.cfg", "r");
	if (f == NULL)
	{
		// No library file yet: seed a single slot from the current working copy. This also
		// migrates an old single custom weapon (loaded from opentyrian.cfg) into the library.
		customWeaponLibCount    = 1;
		customWeaponCurrentSlot = 0;
		storeToSlot(0);
		return;
	}

	for (int i = 0; i < CUSTOM_WEAPON_LIB_MAX; ++i)   // valid defaults for any unwritten slot
		slotSetDefault(&customWeaponLib[i]);

	int count = 1, current = 0, cur = -1;
	char line[16384];   // must hold a whole "raw m p <blob>" line for the widest (255-bullet) design
	while (fgets(line, sizeof(line), f) != NULL)
	{
		if (strncmp(line, "count ", 6) == 0)
			count = clampi(atoi(line + 6), 1, CUSTOM_WEAPON_LIB_MAX);
		else if (strncmp(line, "current ", 8) == 0)
			current = atoi(line + 8);
		else if (strncmp(line, "weapon ", 7) == 0)
			cur = clampi(atoi(line + 7), 0, CUSTOM_WEAPON_LIB_MAX - 1);
		else if (cur >= 0 && strncmp(line, "name ", 5) == 0)
		{
			char *nm = line + 5;
			size_t len = strlen(nm);
			while (len > 0 && (nm[len - 1] == '\n' || nm[len - 1] == '\r'))
				nm[--len] = '\0';   // strip the newline
			strncpy(customWeaponLib[cur].name, nm, sizeof(customWeaponLib[cur].name) - 1);
			customWeaponLib[cur].name[sizeof(customWeaponLib[cur].name) - 1] = '\0';
		}
		else if (cur >= 0 && strncmp(line, "props ", 6) == 0)
		{
			CustomWeaponSlot *s = &customWeaponLib[cur];
			sscanf(line + 6, "%d %d %d %d %d %d",
			       &s->cost, &s->powerUse, &s->equipSlot, &s->itemGraphic, &s->chargeStages, &s->modes);
		}
		else if (cur >= 0 && strncmp(line, "sidekick ", 9) == 0)
		{
			CustomWeaponSlot *s = &customWeaponLib[cur];  // absent in old files -> keeps slotSetDefault values
			sscanf(line + 9, "%d %d %d %d %d",
			       &s->sidekickMount, &s->sidekickSprite, &s->sidekickFrames, &s->sidekickFrameStep, &s->sidekickAnimate);
		}
		else if (cur >= 0 && strncmp(line, "raw ", 4) == 0)
		{
			int m = 0, p = 0, consumed = 0;
			if (sscanf(line + 4, "%d %d %n", &m, &p, &consumed) >= 2 &&
			    m >= 0 && m < CUSTOM_WEAPON_MODES && p >= 0 && p < CUSTOM_POWER_LEVELS)
				deserializeRaw(&customWeaponLib[cur].raw[m][p], line + 4 + consumed);
		}
	}
	fclose(f);

	customWeaponLibCount    = count;
	customWeaponCurrentSlot = clampi(current, 0, count - 1);
	loadFromSlot(customWeaponCurrentSlot);   // working copy <- the saved current weapon
}

void customWeaponInit(void)
{
	buildBulletPresets();

	customWeaponEditLevel = clampi(customWeaponEditLevel, 0, CUSTOM_POWER_LEVELS - 1);
	customWeaponModes     = clampi(customWeaponModes,     1, CUSTOM_WEAPON_MODES);
	customWeaponEditMode  = clampi(customWeaponEditMode,  0, CUSTOM_WEAPON_MODES - 1);
	customWeaponChargeStages = clampi(customWeaponChargeStages, 1, CUSTOM_POWER_LEVELS);
	if (customWeaponEquipSlot < 0 || customWeaponEquipSlot >= CUSTOM_EQUIP_COUNT)
		customWeaponEquipSlot = CUSTOM_EQUIP_FRONT;

	// Sidekick body appearance (the sprite is additionally clamped to its sheet in materialize).
	customSidekickMount     = clampi(customSidekickMount,     0, CUSTOM_SIDEKICK_MOUNTS - 1);
	customSidekickFrames    = clampi(customSidekickFrames,    1, 20);
	customSidekickFrameStep = clampi(customSidekickFrameStep, 0, 40);
	customSidekickAnimate   = clampi(customSidekickAnimate,   1, 2);
	if (customSidekickSprite < 1)
		customSidekickSprite = 1;   // body sprite index is 1-based (blit reads offsetTable[index-1])

	// Default the shop icon to a real weapon's icon the first time (borrow port 1's).
	if (customWeaponItemGraphic < 1)
		customWeaponItemGraphic = clampi(weaponPort[1].itemgraphic, 1, 237);

	// If nothing has been loaded (fresh run, or a config with no custom weapon),
	// fill in a valid blank design (the editor's RESET restores the demo blaster
	// instead). A loaded design has multi >= 1 on at least one slot, so this
	// never clobbers it.
	bool anyDefined = false;
	for (int m = 0; m < CUSTOM_WEAPON_MODES; ++m)
		for (int p = 0; p < CUSTOM_POWER_LEVELS; ++p)
			if (customWeaponRaw[m][p].multi != 0)
				anyDefined = true;
	if (!anyDefined)
		for (int m = 0; m < CUSTOM_WEAPON_MODES; ++m)
			for (int p = 0; p < CUSTOM_POWER_LEVELS; ++p)
				makeBlankWeapon(&customWeaponRaw[m][p]);

	// Load the saved weapon library once. The working copy set up above seeds/overrides
	// the "current" slot; later re-inits (episode reloads) keep the in-memory library and
	// just re-materialize below.
	static bool libraryLoaded = false;
	if (!libraryLoaded)
	{
		libraryLoaded = true;
		customWeaponLibraryLoad();
	}

	// Claim a spare port slot so we never clobber a real weapon. The Tyrian 2000
	// data fills every port name, but ports 48-60 are dummy "Test" placeholders
	// (verified against data/tyrian.hdt); scan from the top for an empty or "Test" slot.
	customWeaponPort = 0;
	for (int i = PORT_NUM; i >= 1; --i)
	{
		if (weaponPort[i].name[0] == '\0' || SDL_strcasecmp(weaponPort[i].name, "Test") == 0)
		{
			customWeaponPort = i;
			break;
		}
	}
	if (customWeaponPort == 0)
		customWeaponPort = PORT_NUM;  // fallback: last slot

	// Claim a spare option (sidekick) slot for the sidekick: scan from the top for a
	// "None" placeholder, skipping the one the Charge-Laser may have taken. 0 = none
	// free (sidekick equipping is then unavailable, handled gracefully at equip time).
	customSidekickSlot = 0;
	for (int i = OPTION_NUM; i >= 1; --i)
	{
		if (i == chargeLaserSlot)
			continue;
		if (strncmp(options[i].name, "None", 4) == 0)
		{
			customSidekickSlot = i;
			break;
		}
	}

	customWeaponMaterialize();  // compile all modes/levels + build the sidekick option
}
