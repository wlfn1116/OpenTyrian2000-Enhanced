/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 *
 * Endless mode: depth-scaled enemy difficulty, elites, and the player-side modifiers.
 *
 * One of the endless_*.c files that make up endless mode: endless.h is the public
 * interface, endless_internal.h the state and helpers the group shares.
 */

#include "endless.h"
#include "endless_internal.h"

#include "config.h"        // difficultyLevel, DIFFICULTY_*, player-independent globals
#include "custom_weapon.h" // customWeaponPort / customSidekickSlot (reserved shop slots)
#include "episodes.h"      // item arrays + SHIP_NUM/PORT_NUM/... counts, episodeAvail, JE_initEpisode
#include "joystick.h"      // push_joysticks_as_keyboard
#include "lvlmast.h"       // shapeFile[]
#include "mainint.h"       // JE_getCost
#include "mtrand.h"        // mt_rand
#include "player.h"        // player[]
#include "sprite.h"        // JE_loadCompShapes, enemySpriteSheets, shopSpriteSheet
#include "tyrian2.h"       // itemAvail, itemAvailMax
#include "varz.h"          // eventRec, maxEvent, map* globals

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Combo kill counter: +1 per kill while a kill-fire window is up, reset the instant it lapses.
// Not itself capped (the HUD's plain "xN"); only the derived fire-rate multiplier below is.
int endlessComboKills = 0;
#define ENDLESS_COMBO_KILLS_PER_STEP 25  // every this many combo kills adds +1x to the fire-rate multiplier
// Linear ramp: 2x at combo 0, +1x per step, capped at 8 steps (x10 at a 200 combo).
// Turbodrive, Overdrive and the evil jams share the schedule; only Overblast skips the fire boost.
#define ENDLESS_COMBO_MAX_STEPS       8

// The evil kill-fire mirrors. Fire-jam curses (Backfire, Burnout): while the window is up they add
// cooldown to every shot (slower fire) instead of removing it, ramping with the same combo steps;
// Burnout stacks even more jam. Damage-cut curses (Burnout, Misfire): each kill stacks a shot-damage
// reduction, peaking at the stack cap and floored so you can still fight.
#define ENDLESS_EVIL_JAM_BASE        3   // base extra shotRepeat ticks per shot while a fire-jam curse is up
#define ENDLESS_EVIL_JAM_PER_STEP    3   // + this many more per combo step (up to ENDLESS_COMBO_MAX_STEPS)
#define ENDLESS_EVIL_JAM_STACK_MAX   10  // Burnout: up to this many MORE jam ticks at full stacks
#define ENDLESS_EVIL_DMG_MAX         75  // Burnout / Misfire: up to -this% shot damage at full stacks (mirror of the +150% boon)
#define ENDLESS_EVIL_DMG_FLOOR       25  // ...but never cut the player's shot damage below this %

// --- Depth- and mutator-scaled enemy difficulty --------------------------------
// Endless never changes WHICH enemies appear -- it only scales the stats of what the level
// already spawns, via levers the engine applies at use (notes.md §Difficulty ramp).

// Difficulty factor (percent) tilting the depth ramp: NORMAL = 100 keeps the tuned baseline,
// and the spread is wide so the modes feel distinct (notes.md §Difficulty ramp).
static int endlessDifficultyRampPercent(void)
{
	switch (difficultyLevel)
	{
	case DIFFICULTY_WIMP:       return 50;
	case DIFFICULTY_EASY:       return 75;
	case DIFFICULTY_NORMAL:     return 100;
	case DIFFICULTY_HARD:       return 120;   // 80% elite cap at ~zone 100
	case DIFFICULTY_IMPOSSIBLE: return 134;   // 80% elite cap at ~zone 90
	default:                    return 160;   // Insanity and beyond: 80% elite cap at ~zone 75
	}
}

// Depth driving the enemy-difficulty levers: real run depth x1.25, tilted by base difficulty
// (endlessDifficultyRampPercent). Each lever below has its own slope so the caps mature one
// at a time across the run instead of piling into a wall (notes.md §Endless / Difficulty
// ramp); HUD, score, milestones and the economy still use the real endlessRunDepth.
static int endlessEffectiveDepth(void)
{
	return endlessRunDepth * endlessDifficultyRampPercent() * 5 / 400;
}

// The current zone as the difficulty ramp sees it: the real zone (endlessRunDepth + 1) on NORMAL,
// advanced on harder difficulties and held back on easier ones (same rampPercent as every other
// enemy lever). The player-facing "zone N" thresholds -- the extra-shot tide onset, the contact-
// damage ramp, and the course-danger ramp -- are all expressed against THIS, so harder modes reach
// each one sooner and easier modes later (notes.md §Difficulty ramp).
int endlessDifficultyZone(void)
{
	return 1 + endlessRunDepth * endlessDifficultyRampPercent() / 100;
}

// --- Enemy intensity tuning ------------------------------------------------------
// The four intensity levers all have the same shape: a stock value of 100 (or 1x), a slope per
// EFFECTIVE depth, a set of per-mutator deltas, and a clamp. Every number one of them uses is
// named here, so retuning the ramp means editing this block rather than hunting literals in the
// bodies below. The clamps matter as much as the slopes: each lever tops out at a different
// zone, which is what makes the run's difficulty arrive in waves rather than as one wall
// (notes.md §Difficulty ramp).

// Ordinary-enemy HP, percent of stock.
#define ENDLESS_HP_PER_DEPTH       4    // +% per effective depth
#define ENDLESS_HP_FORTIFIED     120    // FORTIFIED: +% (2.2x HP, clearly felt)
#define ENDLESS_HP_FRAGILE        50    // FRAGILE: -%
#define ENDLESS_HP_MIN            25    // floor: a FRAGILE zone-0 enemy still takes a hit
#define ENDLESS_HP_MAX           600    // reached at effective depth 125

// Boss HP, as a whole multiplier (1 = stock).
#define ENDLESS_BOSS_DEPTH_PER_X   8    // +1x per this many effective depths
#define ENDLESS_BOSS_FORTIFIED     3    // FORTIFIED: +this many x (a 4x boss at depth 0)
#define ENDLESS_BOSS_MARKED        2    // gamble "Marked": the boss you paid to forget comes back bulked up
#define ENDLESS_BOSS_MAX          16    // reached at run depth ~96 on Normal

// Enemy shot cooldown, percent of stock. This one counts DOWN: the deltas are subtracted, so a
// bigger number means faster enemy fire.
#define ENDLESS_FIRE_PER_DEPTH_NUM 3    // -% per effective depth, as NUM/DEN (0.75%)
#define ENDLESS_FIRE_PER_DEPTH_DEN 4
#define ENDLESS_FIRE_FRENZY       50    // FRENZY: -% (about 2x fire on its own)
#define ENDLESS_FIRE_ENRAGE_TICKS 25    // ENRAGE: -1% per this many ticks spent in the zone...
#define ENDLESS_FIRE_ENRAGE_MAX   55    // ...up to -this%
#define ENDLESS_FIRE_OVERCLOCK    30    // OVERCLOCK: -% (everything runs hot)
#define ENDLESS_FIRE_OVERLOAD     55    // OVERLOAD: -% (Overclock cranked way up)
#define ENDLESS_FIRE_MAX_REDUCE   75    // floor of 25% cooldown, i.e. 4x fire rate

// Enemy projectile speed, percent of stock.
#define ENDLESS_SPEED_PER_DEPTH_NUM 5   // +% per effective depth, as NUM/DEN (1.67%)
#define ENDLESS_SPEED_PER_DEPTH_DEN 3
#define ENDLESS_SPEED_SWIFT       70    // SWIFT: +% (1.7x shots)
#define ENDLESS_SPEED_DILATION    45    // DILATION: -% (time dilation, shots crawl)
#define ENDLESS_SPEED_OVERCLOCK   40    // OVERCLOCK: +%
#define ENDLESS_SPEED_OVERLOAD    90    // OVERLOAD: +%
#define ENDLESS_SPEED_MIN         40    // floor: even a dilated shot still travels
#define ENDLESS_SPEED_MAX        240    // 2.4x, reached at run depth ~67 on Normal

// Enemy shot damage, percent of stock. Capped lower than the other levers because the tide
// resumes the climb past the cap (see endlessShotDamagePercent).
#define ENDLESS_DMG_PER_DEPTH_NUM  7    // +% per effective depth, as NUM/DEN (1.75%)
#define ENDLESS_DMG_PER_DEPTH_DEN  4
#define ENDLESS_DMG_DEVASTATING   75    // DEVASTATING: +%
#define ENDLESS_DMG_MAX          220    // intensity cap; the tide climbs on from here

// Ordinary-enemy HP multiplier (100 = normal): +4% per (effective) level; FORTIFIED +120%
// (2.2x HP, clearly felt); FRAGILE -50%.
int endlessArmorPercent(void)
{
	int pct = 100 + endlessEffectiveDepth() * ENDLESS_HP_PER_DEPTH;
	if (endlessActiveMods & ENDLESS_MOD_FORTIFIED)
		pct += ENDLESS_HP_FORTIFIED;
	if (endlessActiveMods & ENDLESS_MOD_FRAGILE)
		pct -= ENDLESS_HP_FRAGILE;
	return endlessClamp(pct, ENDLESS_HP_MIN, ENDLESS_HP_MAX);
}

// Boss HP multiplier (1 = normal): +1x every 8 (effective) levels, reaching the 16x cap at run
// depth ~96 on Normal; FORTIFIED +3x (a 4x boss at depth 0); FRAGILE ~halves it.
int endlessBossHpMult(void)
{
	int mult = 1 + endlessEffectiveDepth() / ENDLESS_BOSS_DEPTH_PER_X;
	if (endlessActiveMods & ENDLESS_MOD_FORTIFIED)
		mult += ENDLESS_BOSS_FORTIFIED;
	if (endlessActiveMods & ENDLESS_MOD_MARKED)
		mult += ENDLESS_BOSS_MARKED;
	if (endlessActiveMods & ENDLESS_MOD_FRAGILE)
		mult = (mult + 1) / 2;   // halve, rounding up: FRAGILE softens a boss, never erases it
	return endlessClamp(mult, 1, ENDLESS_BOSS_MAX);
}

// Enemy shot-cooldown multiplier (100 = normal; LOWER = fires faster): -0.75% per (effective)
// level, bottoming at the 4x-fire floor at run depth ~80 on Normal; FRENZY an extra -50% (~2x
// fire), floored at 25% so deep FRENZY runs reach ~4x.
int endlessFireDelayPercent(void)
{
	int reduce = endlessEffectiveDepth() * ENDLESS_FIRE_PER_DEPTH_NUM / ENDLESS_FIRE_PER_DEPTH_DEN;
	if (endlessActiveMods & ENDLESS_MOD_FRENZY)
		reduce += ENDLESS_FIRE_FRENZY;
	if (endlessActiveMods & ENDLESS_MOD_ENRAGE)  // ramps up the longer you linger in the zone
		reduce += endlessClamp(endlessZoneTicks / ENDLESS_FIRE_ENRAGE_TICKS, 0, ENDLESS_FIRE_ENRAGE_MAX);
	if (endlessActiveMods & ENDLESS_MOD_OVERCLOCK)
		reduce += ENDLESS_FIRE_OVERCLOCK;
	if (endlessActiveMods & ENDLESS_MOD_OVERLOAD)
		reduce += ENDLESS_FIRE_OVERLOAD;
	if (reduce > ENDLESS_FIRE_MAX_REDUCE)
		reduce = ENDLESS_FIRE_MAX_REDUCE;
	return 100 - reduce;
}

// Enemy projectile-speed multiplier (100 = normal): +1.67% per (effective) level, reaching the
// 2.4x cap at run depth ~67 on Normal; SWIFT +70% (1.7x shots).
int endlessShotSpeedPercent(void)
{
	int pct = 100 + endlessEffectiveDepth() * ENDLESS_SPEED_PER_DEPTH_NUM / ENDLESS_SPEED_PER_DEPTH_DEN;
	if (endlessActiveMods & ENDLESS_MOD_SWIFT)
		pct += ENDLESS_SPEED_SWIFT;
	if (endlessActiveMods & ENDLESS_MOD_DILATION)
		pct -= ENDLESS_SPEED_DILATION;
	if (endlessActiveMods & ENDLESS_MOD_OVERCLOCK)
		pct += ENDLESS_SPEED_OVERCLOCK;
	if (endlessActiveMods & ENDLESS_MOD_OVERLOAD)
		pct += ENDLESS_SPEED_OVERLOAD;
	return endlessClamp(pct, ENDLESS_SPEED_MIN, ENDLESS_SPEED_MAX);
}

// The tide resumes the shot-DAMAGE climb past its intensity cap (notes.md §Difficulty ramp).
// Defined here (not in the tide block below) because a macro must precede its user.
#define ENDLESS_TIDE_DMG_STEP  3    // tide levels per +1% enemy shot damage past the 220 intensity cap
#define ENDLESS_TIDE_DMG_CAP   400  // absolute ceiling on the tide-boosted shot-damage percent (sanity backstop)

// Enemy shot-DAMAGE multiplier (100 = normal): +1.75% per (effective) level; DEVASTATING +75%.
// Capped lower than the others, then the tide resumes a SLOW climb (notes.md §Difficulty ramp).
int endlessShotDamagePercent(void)
{
	int pct = 100 + endlessEffectiveDepth() * ENDLESS_DMG_PER_DEPTH_NUM / ENDLESS_DMG_PER_DEPTH_DEN;
	if (endlessActiveMods & ENDLESS_MOD_DEVASTATING)
		pct += ENDLESS_DMG_DEVASTATING;
	if (pct > ENDLESS_DMG_MAX)
		pct = ENDLESS_DMG_MAX;
	// The tide (0 until effective zone 35, then +1 per effective zone, uncapped) adds a gentle
	// +1% per ENDLESS_TIDE_DMG_STEP ON TOP of the intensity cap: ~+30% by zone 100, ~+70% by
	// zone 200 on NORMAL. The high ENDLESS_TIDE_DMG_CAP is only a backstop; the consumer
	// (tyrian2.c) also clamps the final per-shot byte to 255, so a big multiplier can't wrap.
	pct += endlessTideLevel() / ENDLESS_TIDE_DMG_STEP;
	if (pct > ENDLESS_TIDE_DMG_CAP)
		pct = ENDLESS_TIDE_DMG_CAP;
	return pct;
}

// --- Rising tide: quantity scaling past the intensity caps ------------------------
// The intensity levers above saturate by ~effective zone 100-125; the tide adds the one axis
// with NO engine ceiling -- extra shots per volley and a rising elite/champion share -- off
// this single coefficient, staying 0 through the early hump (notes.md §Endless).

#define ENDLESS_TIDE_START      35   // effective zone the tide begins (intensity is ~capped by here)
// Enemy "rising tide" of EXTRA shots per volley (see endlessExtraEnemyShots). NORMAL-difficulty
// baseline: the FIRST extra shot at ENDLESS_TIDE_SHOT_ONSET (zone 25), rising evenly to
// ENDLESS_TIDE_SHOT_ANCHOR_ADD (3) by the anchor zone (100), then +1 shot every ENDLESS_TIDE_SHOT_STEP
// zones with NO hard cap -- so 5 by zone 150, then climbing indefinitely (only the MAX sanity backstop
// and the enemy-shot pool bound it). Harder/easier difficulties travel this same curve sooner/later
// (scaled by the same rampPercent as the other enemy levers).
#define ENDLESS_TIDE_SHOT_ONSET      25   // ZONE (on NORMAL) the FIRST extra shot appears -- start of the early ramp
#define ENDLESS_TIDE_SHOT_ANCHOR     100  // ZONE (on NORMAL) the early ramp reaches ENDLESS_TIDE_SHOT_ANCHOR_ADD
#define ENDLESS_TIDE_SHOT_ANCHOR_ADD 3    // extra shots/volley at the anchor zone (on NORMAL)
#define ENDLESS_TIDE_SHOT_STEP       25   // past the anchor: +1 extra shot every this-many zones (so 5 by zone 150, then more)
#define ENDLESS_TIDE_SHOT_MAX        50   // sanity ceiling on added shots/volley (the enemy-shot pool caps total too)

// The single tide coefficient (the "knob"): 0 through the early game, then +1 per effective zone,
// uncapped. Everything the tide drives is derived from this.
int endlessTideLevel(void)
{
	if (!endlessMode)
		return 0;
	const int t = endlessEffectiveDepth() - ENDLESS_TIDE_START;
	return (t > 0) ? t : 0;
}

// Extra enemy shots per firing volley, difficulty-scaled like every other lever (endlessDifficultyZone).
// Two segments meeting at the anchor (zone 100 on NORMAL): an EARLY ramp -- first extra shot at the
// ONSET zone (25), rising evenly to ANCHOR_ADD (3) by the anchor -- then a steady +1 shot every STEP
// zones with NO hard cap (5 by zone 150 on NORMAL, then climbing). tyrian2.c fans these out around the
// weapon's own shots; the enemy-shot pool (ENEMY_SHOT_MAX) still hard-caps what reaches the screen
// (notes.md §Difficulty ramp).
int endlessExtraEnemyShots(void)
{
	if (!endlessMode)
		return 0;
	const int zone = endlessDifficultyZone();

	int extra;
	if (zone >= ENDLESS_TIDE_SHOT_ANCHOR)
	{
		// Anchor onward: ANCHOR_ADD at the anchor, then +1 every STEP zones -- no hard cap beyond the
		// MAX sanity backstop (3 by zone 100, 5 by zone 150 on NORMAL, then climbing without bound).
		extra = ENDLESS_TIDE_SHOT_ANCHOR_ADD + (zone - ENDLESS_TIDE_SHOT_ANCHOR) / ENDLESS_TIDE_SHOT_STEP;
	}
	else if (zone >= ENDLESS_TIDE_SHOT_ONSET)
	{
		// Early ramp: 1 shot at the onset zone, rising evenly to ANCHOR_ADD by the anchor.
		extra = 1 + (ENDLESS_TIDE_SHOT_ANCHOR_ADD - 1) * (zone - ENDLESS_TIDE_SHOT_ONSET)
		              / (ENDLESS_TIDE_SHOT_ANCHOR - ENDLESS_TIDE_SHOT_ONSET);
	}
	else
	{
		extra = 0;
	}
	if (extra > ENDLESS_TIDE_SHOT_MAX)
		extra = ENDLESS_TIDE_SHOT_MAX;
	return extra;
}

// --- Contact (ramming) damage ramp -------------------------------------------------
// The damage the PLAYER takes from colliding with an enemy climbs deep in a run, so trading hull for
// a ram stops being cheap: no bonus until the START zone (35), then linear to +ANCHOR_PCT (150%) by
// the anchor zone (100), the SAME slope onward, capped at +MAX_PCT (500%). Only the player's RECEIVED
// contact damage scales -- the damage the collision deals to the enemy is left untouched (mainint.c).
// Difficulty-scaled like every other lever (notes.md §Difficulty ramp).
#define ENDLESS_CONTACT_START      35   // zone the contact-damage climb begins (no bonus at/below)
#define ENDLESS_CONTACT_ANCHOR     100  // zone at which the bonus reaches ENDLESS_CONTACT_ANCHOR_PCT
#define ENDLESS_CONTACT_ANCHOR_PCT 150  // +this% player contact damage at the anchor zone
#define ENDLESS_CONTACT_MAX_PCT    500  // ceiling on the added player contact-damage percent

int endlessContactDamagePercent(void)
{
	if (!endlessMode)
		return 100;
	const int zone = endlessDifficultyZone();
	if (zone <= ENDLESS_CONTACT_START)
		return 100;
	int bonus = ENDLESS_CONTACT_ANCHOR_PCT * (zone - ENDLESS_CONTACT_START)
	              / (ENDLESS_CONTACT_ANCHOR - ENDLESS_CONTACT_START);
	if (bonus > ENDLESS_CONTACT_MAX_PCT)
		bonus = ENDLESS_CONTACT_MAX_PCT;
	return 100 + bonus;
}

// --- Elite enemies --------------------------------------------------------------
// A depth-scaled trickle of tougher, tinted, bounty-paying enemies. The roll is cached per
// linkgroup so a multi-tile enemy is one tier as a whole (notes.md §Difficulty ramp).

static signed char endlessEliteLink[256];  // per-linknum tier this level: -1 undecided, else 1/2/3

void endlessResetElites(void)
{
	for (unsigned i = 0; i < COUNTOF(endlessEliteLink); ++i)
		endlessEliteLink[i] = -1;

	// Seed this zone's elite/champion tier stream from the run seed + depth, so the rolls are
	// reproducible for a given seed. Own salt phase: a large offset that can't collide with the
	// outpost (depth*2), level/music (depth*2+1), or light-cone (depth*2+0x40000000) streams.
	endlessEliteRngState = endlessSplitMixSeed((Uint64)endlessRunDepth * 2 + 0x50000000);
}

// The depth-driven SPECIAL-enemy share BEFORE any mutator override: a 2% trickle rising to an 80%
// cap. endlessEliteChancePercent applies the Elite Pack / Apex / Legion overrides on top; the course
// generator also reads this directly, to retire a now-pointless "half enemies elite" once the natural
// share has already passed 50% (see endlessFixRedundantElitePack).
int endlessNaturalEliteChancePercent(void)
{
	int pct = 2 + endlessEffectiveDepth() / 2;
	if (pct > 25)
		// Past the 25% shoulder (effective depth 46), climb the last 55 points to the cap at
		// ~0.54/level, so the share reaches 80% at effective depth 148 (~zone 120 on Normal).
		pct = 25 + (endlessEffectiveDepth() - 46) * 27 / 50;
	if (pct > 80)
		pct = 80;                           // leave a true 100% to the Apex / Legion sectors
	return pct;
}

// Whether the no-elite-tier boons (NOCHAMP / NOELITE) are eligible to be charted yet. They only start
// appearing once the natural special-enemy share climbs PAST 25% -- below that, elites/champions are a
// rare trickle and "no champions / no elites" would be a near-empty boon. The 25% shoulder lands around
// effective depth 47 (~zone 47 on Normal, sooner on harder modes). Gates every generation path that can
// emit either bit; a leaked bit below the threshold is also scrubbed in endlessGenerateCourses.
bool endlessEliteBoonsUnlocked(void)
{
	return endlessNaturalEliteChancePercent() > 25;
}

// Chance (percent) that an eligible enemy becomes SPECIAL: the natural depth share above, except
// Elite Pack forces half and Apex/Legion force all (notes.md §Difficulty ramp). Elite Pack is only
// ever meant to RAISE the share, so the generator stops charting it once the natural share tops 50%
// -- otherwise it would CAP elites BELOW the natural rate (a stealth boon on a danger course).
static int endlessEliteChancePercent(void)
{
	if (endlessActiveMods & ENDLESS_MOD_NOELITE)
		return 0;                               // "no elites or champions" boon: nothing spawns special (wins over Elite Pack / Apex / Legion)
	if (endlessActiveMods & (ENDLESS_MOD_APEX | ENDLESS_MOD_LEGION))
		return 100;
	if (endlessActiveMods & ENDLESS_MOD_ELITEPACK)
		return 50;
	return endlessNaturalEliteChancePercent();
}

// Roll one enemy's tier: 1 normal, 2 elite, 3 champion. Champions are ~half as common as
// elites (1 in 3 of the specials) -- except a LEGION sector makes every special a champion.
static int endlessPickTier(void)
{
	if ((int)(endlessEliteRand() % 100) >= endlessEliteChancePercent())  // seeded elite stream, per (seed, zone)
		return 1;  // normal
	if (endlessActiveMods & ENDLESS_MOD_NOCHAMP)
		return 2;  // "no champions" boon: this special stays an ELITE, never a champion (even under Legion)
	if (endlessActiveMods & ENDLESS_MOD_LEGION)
		return 3;  // every special is a champion
	// Among specials, the champion share climbs with the tide (tougher shooters deeper),
	// from the base ~1/3 toward a majority.
	int champPct = 33 + endlessTideLevel() / 3;
	if (champPct > 70)
		champPct = 70;
	return ((int)(endlessEliteRand() % 100) < champPct) ? 3 : 2;  // seeded elite stream, per (seed, zone)
}

int endlessRollEliteTier(JE_byte linknum)
{
	if (linknum == 0)  // lone single-tile enemy: independent per-enemy roll
		return endlessPickTier();
	// Multi-tile enemy: decide once for the whole linkgroup so every tile is the same tier.
	if (endlessEliteLink[linknum] < 0)
		endlessEliteLink[linknum] = (signed char)endlessPickTier();
	return endlessEliteLink[linknum];
}

// Elite/champion HP multiplier -- a damage divisor applied like the boss one: the enemy
// spends N damage per 1 armor, so it effectively has N times its HP. ~2x, up with depth.
int endlessEliteHpMult(void)
{
	int mult = 2 + endlessEffectiveDepth() / 20;
	if (mult > 5)
		mult = 5;
	return mult;
}

// Combined per-hit HP divisor for an endless enemy. Non-boss elites/champions use the full
// elite multiplier; an elite/champion BOSS (already depth-scaled) gets a gentler x2 bump on
// top, capped so no enemy becomes an unkillable sponge. Ordinary enemies -> 1.
int endlessEnemyHpMult(bool hasBossBar, int bossHpMult, int eliteState)
{
	if (!hasBossBar)
		return (eliteState >= 2) ? endlessEliteHpMult() : 1;
	if (eliteState < 2)
		return bossHpMult;                             // normal boss: unchanged
	int mult = bossHpMult * 2;                         // elite/champion boss: gentle bump
	int cap  = (bossHpMult > ENDLESS_HP_MULT_MAX) ? bossHpMult : ENDLESS_HP_MULT_MAX;
	return (mult > cap) ? cap : mult;                  // capped, but never below the base
}

// Extra cash for destroying an elite / a champion (on top of the normal score value).
// Elite/champion bounties: base scales with depth, doubled by the Bounty Hunter perk, then
// scaled by the Scavenger cash multiplier. (endlessPerkCashPercent is defined above.)
long endlessEliteBounty(void)
{
	long b = 150 + (long)endlessRunDepth * 40;
	if (b > 2500)  // per-elite cap: the tide multiplies elite COUNT, so keep per-kill value bounded
		b = 2500;  // (else deep zones mint enough cash to trivially buy Overdrive -- the tide's own counter)
	if (endlessPerkOwned[PERK_BOUNTY])
		b *= 2;
	return b * endlessPerkCashPercent() / 100;
}
long endlessChampionBounty(void)
{
	long b = 350 + (long)endlessRunDepth * 90;
	if (b > 6000)  // per-champion cap (same reasoning as the elite cap above)
		b = 6000;
	if (endlessPerkOwned[PERK_BOUNTY])
		b *= 2;
	return b * endlessPerkCashPercent() / 100;
}

// Champion aggression, applied per-champion on top of the sector's global scaling: they
// fire noticeably faster and their shots hit harder.
int endlessChampionFireDelayPercent(void)  { return 60; }   // 0.6x cooldown (~1.7x fire rate)
int endlessChampionShotDamagePercent(void) { return 150; }  // +50% shot damage

// Award an elite/champion kill: pay the bounty and post a kill message to the in-game text
// bar. Called from both enemy-death sites (tyrian2.c) for every elite/champion tile -- a
// lone enemy fires once; a multi-tile enemy re-posts the same line (which just redraws).
void endlessAwardEliteKill(int eliteState)
{
	if (!endlessMode || eliteState < 2)
		return;

	const bool champion = (eliteState == 3);
	const long bounty = champion ? endlessChampionBounty() : endlessEliteBounty();
	player[0].cash += bounty;

	// Message reads the same for an elite/champion regular or boss.
	// Label stays left-aligned in the normal message-bar slot; only the cash bonus is right-aligned,
	// its rightmost pixel sitting on x=244 (before the HUD at x=299).
	char label[48], cash[24];
	snprintf(label, sizeof(label), "%s Enemy destroyed!", champion ? "Champion" : "Elite");
	snprintf(cash, sizeof(cash), "+%ld", bounty);
	JE_drawTextWindowSplit(label, cash, 244);
}

// --- Special-weapon pickups -----------------------------------------------------
// Endless has no data-cube archive and no secret-level warps, so the datacubes and secret
// orbs a shipped level drops would otherwise be dead pickups. Instead each grants a random
// SPECIAL weapon (Repulsor, Flare, ...), equipped instantly with a text-bar announcement.

// Number of sprites in the HUD "power-up" sheet (spriteSheet10). A Sprite2_array begins with
// a Uint16 offset table, one entry per sprite; entry[0] is the byte offset to sprite 1's
// pixels -- i.e. the table's own size -- so entry[0] / 2 is the sprite count. Used to reject
// specials whose icon index would blit past the table (see endlessGrantSpecial).
static unsigned endlessHudIconCount(void)
{
	if (spriteSheet10.data == NULL || spriteSheet10.size < sizeof(Uint16))
		return 0;
	return SDL_SwapLE16(((Uint16 *)spriteSheet10.data)[0]) / (unsigned)sizeof(Uint16);
}

// Name of the last special weapon endlessGrantSpecial granted this shop visit, shown in the
// E-Shop "Special Weapon" help line. Reset at shop entry (endlessResetShopPrices).
char endlessLastSpecialName[31] = "";

const char *endlessLastGrantedSpecial(void) { return endlessLastSpecialName; }

void endlessGrantSpecial(void)
{
	if (!endlessMode)
		return;

	// Gather the real, SAFE specials: non-empty name, a dispatcher-handled effect type
	// (stype 1..18), and an in-range itemgraphic -- the HUD redraws the equipped icon every
	// frame, so the bad icon several unfinished specials carry crashes instantly.
	const unsigned iconMax = endlessHudIconCount();

	// Invulnerability (stype 12 in JE_specialComplete -- the invulnerable_ticks effect) is
	// deliberately kept OUT of the endless pool: a Buy Special that could roll full
	// invulnerability would trivialize the run. Excluding by stype covers every
	// invulnerability entry in the data ("Invulnerability" and "Invulnerability [easier]").
	JE_byte pool[SPECIAL_NUM];
	int n = 0;
	for (int id = 1; id <= SPECIAL_NUM; ++id)
		if (special[id].name[0] != '\0' &&
		    special[id].stype >= 1 && special[id].stype <= 18 &&
		    special[id].stype != 12 &&  // never Invulnerability (see note above)
		    special[id].itemgraphic >= 1 && special[id].itemgraphic <= iconMax)
			pool[n++] = (JE_byte)id;
	if (n == 0)
		return;

	// Never hand back the special the player already has equipped -- a pickup/grant should feel like
	// a change, not a dud. Drop the current one from the pool, but only when something else remains
	// (if it's the sole valid special, keep it rather than grant nothing).
	if (n > 1)
	{
		const JE_byte current = player[0].items.special;
		for (int i = 0; i < n; ++i)
			if (pool[i] == current)
			{
				pool[i] = pool[--n];  // swap-remove; order is irrelevant for a uniform pick
				break;
			}
	}

	const JE_byte id = pool[mt_rand() % n];  // pickup/shop grant: gameplay RNG, not the seed
	player[0].items.special = id;
	shotMultiPos[SHOT_SPECIAL]  = 0;
	shotRepeat[SHOT_SPECIAL]    = 0;
	shotMultiPos[SHOT_SPECIAL2] = 0;
	shotRepeat[SHOT_SPECIAL2]   = 0;

	// Copy the granted special's name, trimming the padding whitespace some data names carry
	// (else the E-Shop help reads "Got NAME !" with a gap before the "!").
	const char *s = special[id].name;
	while (*s == ' ' || *s == '\t')
		++s;
	SDL_strlcpy(endlessLastSpecialName, s, sizeof(endlessLastSpecialName));
	for (size_t len = strlen(endlessLastSpecialName);
	     len > 0 && (endlessLastSpecialName[len - 1] == ' ' || endlessLastSpecialName[len - 1] == '\t'); )
		endlessLastSpecialName[--len] = '\0';

	char msg[64];
	snprintf(msg, sizeof(msg), "Special weapon:  %s", endlessLastSpecialName);
	JE_drawTextWindow(msg);
}

// --- Kill-fire buff HUD readout -------------------------------------------------------
// Live combo/timer/fire/damage numbers for JE_inGameDisplays -- the BUFF's own contribution
// only, and no buff NAME (just the numbers) by design.
int endlessKillBuffTicksLeft(void) { return endlessTurbodriveTimer; }
int endlessKillBuffTicksMax(void)  { return endlessBuffWindowTicks(); }

// The combo kill count driving the escalation (see endlessKillBuffFireDecrements) -- shown on
// the HUD as a plain "xN". Universal: climbs for Turbodrive and Overdrive alike (both refresh
// endlessComboKills the same way in endlessCountKill).
int endlessKillBuffComboCount(void)
{
	if (!endlessMode)
		return 0;
	return endlessComboKills;
}

// Themed HUD colour bank (matches the ship tints): red Turbodrive (bank 12 / 0xC0), yellow
// Overdrive (bank 7 / 0x70), blue Overblast (bank 9), bank 4 for an evil curse.
int endlessKillBuffColorBank(void)
{
	if (endlessActiveMods & ENDLESS_MOD_KILLFIRE_EVIL)
		return 4;
	if (endlessActiveMods & ENDLESS_MOD_OVERBLAST)
		return 9;   // blue -- the damage-only buff
	return (endlessActiveMods & ENDLESS_MOD_OVERDRIVE) ? 7 : 12;
}

// The buff's current fire-rate MULTIPLIER (1 = none; 2x..10x from the combo ramp -- same for
// Turbodrive and Overdrive). Derived from the same decrement count the fire block actually applies
// (endlessKillBuffFireDecrements) plus 1 for the weapon's own per-tick decrement, so the HUD
// multiplier can never drift from the real, combo-scaled rate.
int endlessKillBuffFireMultiplier(void)
{
	if (!endlessTurbodriveActive())
		return 1;
	return endlessKillBuffFireDecrements() + 1;
}

int endlessKillBuffDamagePercent(void)
{
	if (!endlessTurbodriveActive() || !(endlessActiveMods & ENDLESS_MOD_DMGUP))
		return 0;  // only the DAMAGE buffs (Overdrive/Overblast) grant a bonus -- Turbodrive is fire-only
	int pct = endlessBuffCharge * 2;  // cash-paid charge adds flat damage on top of the per-kill stacks
	pct += endlessOverdriveStacks * ENDLESS_OVERDRIVE_DMG_MAX / ENDLESS_OVERDRIVE_MAX_STACKS;  // Overdrive OR Overblast: +150% at full stacks (combo 200)
	return pct;
}

// Extra shotRepeat decrements this tick while a kill-fire BOON is up (multiplier = dec+1; the combo
// ramp up top). Returns 0 during an evil curse -- those SLOW fire (see endlessKillFireJamTicks).
int endlessKillBuffFireDecrements(void)
{
	if (!(endlessActiveMods & ENDLESS_MOD_FIREBOOST))
		return 0;  // only Turbodrive/Overdrive quicken fire (not Overblast); the evil mirrors slow it
	int steps = endlessComboKills / ENDLESS_COMBO_KILLS_PER_STEP;
	if (steps > ENDLESS_COMBO_MAX_STEPS)
		steps = ENDLESS_COMBO_MAX_STEPS;
	return 1 + steps;
}

// --- Evil kill-fire curses (Backfire / Burnout / Misfire): the hostile mirrors ------------------
// Is the currently-active kill-fire window an evil curse (slows fire / cuts damage) not a boon?
bool endlessKillFireIsEvil(void)
{
	return endlessMode && endlessTurbodriveActive()
	    && (endlessActiveMods & ENDLESS_MOD_KILLFIRE_EVIL);
}

// Extra shotRepeat cooldown added to every shot while an evil curse is up (0 otherwise), so the guns
// fire SLOWER. Mirrors endlessKillBuffFireDecrements: ramps with the same combo steps, and Evil
// Overdrive piles on more from its per-kill stacks. Applied at shot-reset in shots.c (clamped there).
int endlessKillFireJamTicks(void)
{
	if (!endlessTurbodriveActive() || !(endlessActiveMods & ENDLESS_MOD_FIREJAM))
		return 0;  // only Backfire/Burnout jam fire (not Misfire, which only cuts damage)
	int steps = endlessComboKills / ENDLESS_COMBO_KILLS_PER_STEP;
	if (steps > ENDLESS_COMBO_MAX_STEPS)
		steps = ENDLESS_COMBO_MAX_STEPS;
	int add = ENDLESS_EVIL_JAM_BASE + steps * ENDLESS_EVIL_JAM_PER_STEP;
	if (endlessActiveMods & ENDLESS_MOD_BURNOUT)
		add += endlessOverdriveStacks * ENDLESS_EVIL_JAM_STACK_MAX / ENDLESS_OVERDRIVE_MAX_STACKS;
	return add;
}

// Evil Overdrive: the shot-damage REDUCTION % currently applied (0 otherwise). Peaks at
// ENDLESS_EVIL_DMG_MAX at full stacks; endlessPlayerDamagePercent subtracts it (with a floor).
int endlessKillBuffEvilDamagePenalty(void)
{
	if (!endlessKillFireIsEvil() || !(endlessActiveMods & ENDLESS_MOD_DMGDOWN))
		return 0;  // Burnout OR Misfire cut shot damage
	return endlessOverdriveStacks * ENDLESS_EVIL_DMG_MAX / ENDLESS_OVERDRIVE_MAX_STACKS;
}

// The one-word HUD label naming the active evil curse: JAMMED (Backfire) / BURNOUT / MISFIRE.
// Empty string when no evil kill-fire window is up. (A sector carries at most one evil mod.)
const char *endlessKillFireEvilName(void)
{
	if (!endlessKillFireIsEvil())
		return "";
	if (endlessActiveMods & ENDLESS_MOD_BURNOUT)
		return "BURNOUT";
	if (endlessActiveMods & ENDLESS_MOD_MISFIRE)
		return "MISFIRE";
	return "JAMMED";  // Backfire
}

// GRAVITY: a steady drag growing with zone AND difficulty; the absolute cap stays clear of the
// ship's top speed so full throttle can always climb (notes.md §Difficulty ramp). A plain well
// pulls straight down; an OMNIDIRECTIONAL well (ENDLESS_MOD_GRAVITY_OMNI) pulls along a fixed
// random heading chosen per sector -- same magnitude, so any single axis is still out-climbable.
#define ENDLESS_GRAVITY_BASE     1.6f   // px/tick at zone 0 on NORMAL (difficulty then scales this)
#define ENDLESS_GRAVITY_PER_ZONE 0.04f  // +px/tick per zone cleared, before the difficulty tilt
#define ENDLESS_GRAVITY_MAX      3.6f   // hard cap (72% of VT_VMAX): full throttle always climbs ~1.4 px/tick

// This sector's gravity heading (unit vector). Rolled per sector in endlessRollGravityDir; both ship
// paths read it via the X/Y drift helpers. Defaults to straight down so a well with no roll yet, or a
// plain (non-omni) well, behaves exactly as before.
static float endlessGravityDirX = 0.0f;
static float endlessGravityDirY = 1.0f;

// 16 evenly-spaced unit headings for an omnidirectional well: enough to feel "any direction" while
// avoiding a <math.h> dependency (removed from this file long ago) and staying bit-identical across
// the PC/Switch/Vita builds. A fixed heading per sector keeps it learnable rather than chaotic.
static const float endlessGravityHeadings[16][2] = {
	{  1.000f,  0.000f }, {  0.924f,  0.383f }, {  0.707f,  0.707f }, {  0.383f,  0.924f },
	{  0.000f,  1.000f }, { -0.383f,  0.924f }, { -0.707f,  0.707f }, { -0.924f,  0.383f },
	{ -1.000f,  0.000f }, { -0.924f, -0.383f }, { -0.707f, -0.707f }, { -0.383f, -0.924f },
	{  0.000f, -1.000f }, {  0.383f, -0.924f }, {  0.707f, -0.707f }, {  0.924f, -0.383f },
};

// Pick this sector's gravity heading: a fixed random one for an omni well, else straight down.
// Called once per level from endlessRegenerateLevel (in its own seeded reseed phase).
void endlessRollGravityDir(void)
{
	if (endlessMode && (endlessActiveMods & ENDLESS_MOD_GRAVITY_OMNI))
	{
		const unsigned h = endlessRand() % COUNTOF(endlessGravityHeadings);
		endlessGravityDirX = endlessGravityHeadings[h][0];
		endlessGravityDirY = endlessGravityHeadings[h][1];
	}
	else
	{
		endlessGravityDirX = 0.0f;
		endlessGravityDirY = 1.0f;  // classic Gravity Well: straight down
	}
}

float endlessGravityDrift(void)  // pull magnitude (px/tick), direction-agnostic
{
	// Responds to either bit so a debug-toggled bare OMNI (no GRAVITY) still pulls.
	if (!endlessMode || !(endlessActiveMods & (ENDLESS_MOD_GRAVITY | ENDLESS_MOD_GRAVITY_OMNI)))
		return 0.0f;
	float g = (ENDLESS_GRAVITY_BASE + ENDLESS_GRAVITY_PER_ZONE * (float)endlessRunDepth)
	        * (float)endlessDifficultyRampPercent() / 100.0f;
	if (g > ENDLESS_GRAVITY_MAX)
		g = ENDLESS_GRAVITY_MAX;
	// SLUGGISH slows gravity in lock-step with the ship (endlessMoveScale is 1.0 when not sluggish, so
	// gravity-only sectors are unchanged). A sluggish+gravity sector then stays flyable: full throttle
	// out-climbs the pull by the same ratio it always did, just in the slowed frame -- no strand.
	return g * endlessMoveScale();
}

float endlessGravityDriftX(void) { return endlessGravityDrift() * endlessGravityDirX; }
float endlessGravityDriftY(void) { return endlessGravityDrift() * endlessGravityDirY; }

// Classic (non-VT) path: integer px/tick per axis that tracks the (fractional) drift. Each axis carries
// its own sub-pixel remainder between ticks so the integer nudge averages
// out to exactly the scaled drift, whatever the zone -- not a fixed 1/2 wobble. (int) truncates toward
// zero, so a negative component -- an omni well pulling up/left -- carries correctly too.
int endlessGravityPullX(void)
{
	static float accum = 0.0f;
	accum += endlessGravityDriftX();
	const int step = (int)accum;
	accum -= (float)step;
	return step;
}
int endlessGravityPullY(void)
{
	static float accum = 0.0f;
	accum += endlessGravityDriftY();
	const int step = (int)accum;
	accum -= (float)step;
	return step;
}

// SLUGGISH: the ship crawls. Like GRAVITY, the bite RAMPS with depth and difficulty -- barely slowed
// early, heavy deep in a run -- but floored so you can ALWAYS move. Returns the traverse-speed scale
// (1.0 = normal) applied to the committed per-tick displacement in BOTH ship paths (VT tyrian2.c +
// classic mainint.c), so keyboard, stick, mouse AND touch slow together. endlessGravityDrift multiplies
// gravity by this same factor, so a sluggish+gravity sector stays climbable (the pull slows to match).
#define ENDLESS_SLUGGISH_BASE     0.18f  // fraction slowed at zone 0 on NORMAL (=> 0.82x), before the difficulty tilt
#define ENDLESS_SLUGGISH_PER_ZONE 0.010f // +slowed per zone cleared
#define ENDLESS_SLUGGISH_MAX      0.55f  // hardest slow (=> 0.45x): still clearly movable, never a standstill
float endlessMoveScale(void)
{
	if (!endlessMode || !(endlessActiveMods & ENDLESS_MOD_SLUGGISH))
		return 1.0f;
	float slow = (ENDLESS_SLUGGISH_BASE + ENDLESS_SLUGGISH_PER_ZONE * (float)endlessRunDepth)
	           * (float)endlessDifficultyRampPercent() / 100.0f;
	if (slow > ENDLESS_SLUGGISH_MAX)
		slow = ENDLESS_SLUGGISH_MAX;
	return 1.0f - slow;
}

// SHIELDLESS / DEADGEN: the shield stops recharging. SHIELDLESS just freezes regen (you keep the shield
// you have, then fight on armor once it's spent); DEADGEN is the dead-generator nightmare, which implies
// no regen too. tyrian2.c gates the shield-regen step on this.
bool endlessShieldRegenOff(void)
{
	return endlessMode && (endlessActiveMods & (ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_DEADGEN));
}

// DEADGEN: the generator is dead, so it barely trickles charge -- the main gun (which draws generator
// power per shot, shots.c) sputters and shields never refill, while rear guns / sidekicks / specials
// (power-free) keep you in the fight. Never zero, so every weapon still EVENTUALLY fires -- brutal, not
// unwinnable. Returns the normal rate untouched when the modifier is off (byte-identical normal play).
#define ENDLESS_DEADGEN_POWER_ADD 2u   // generator charge per tick while dead (a normal generator adds ~5-23)
unsigned endlessGeneratorPowerAdd(unsigned normalAdd)
{
	if (endlessMode && (endlessActiveMods & ENDLESS_MOD_DEADGEN))
		return ENDLESS_DEADGEN_POWER_ADD;
	return normalAdd;
}

// Player shot-damage scale (100 = normal): OVERCHARGE is a flat +50%; Overdrive adds +2.5%
// per active kill-stack on top. Applied to the player's shots (tyrian2.c).
int endlessPlayerDamagePercent(void)
{
	if (!endlessMode)
		return 100;
	int pct = (endlessActiveMods & ENDLESS_MOD_OVERCHARGE) ? 150 : 100;
	if ((endlessActiveMods & ENDLESS_MOD_DMGUP) && endlessTurbodriveActive())
		pct += endlessOverdriveStacks * ENDLESS_OVERDRIVE_DMG_MAX / ENDLESS_OVERDRIVE_MAX_STACKS;  // Overdrive OR Overblast: +150% at full stacks (combo 200)
	if (endlessTurbodriveActive() && (endlessActiveMods & ENDLESS_MOD_DMGUP))
		pct += endlessBuffCharge * 2;  // cash-paid charge adds damage only to the DAMAGE buffs (Overdrive/Overblast), not fire-only Turbodrive
	pct += endlessPerkOwned[PERK_DAMAGE] * ENDLESS_PERK_DAMAGE_PCT;  // Heavy Rounds perk (run-persistent)
	if (endlessPerkOwned[PERK_GLASSCANNON])
		pct += ENDLESS_PERK_GLASS_DMG;                              // Glass Cannon relic (paired with -armor)
	// Burnout / Misfire: each kill stacks a shot-damage CUT (mirror of Overdrive/Overblast's bonus),
	// floored so you can still fight. Applied last so it bites into every other bonus.
	if ((endlessActiveMods & ENDLESS_MOD_DMGDOWN) && endlessTurbodriveActive())
	{
		pct -= endlessKillBuffEvilDamagePenalty();
		if (pct < ENDLESS_EVIL_DMG_FLOOR)
			pct = ENDLESS_EVIL_DMG_FLOOR;
	}
	return pct;
}

// Flat reduction applied to each hit the player takes (Bulwark relic), applied in JE_playerDamage.
// Always leaves at least 1 damage so it can't make the player invulnerable.
int endlessPlayerDamageReduce(void)
{
	if (!endlessMode)
		return 0;
	return endlessPerkOwned[PERK_BULWARK] * ENDLESS_PERK_BULWARK;
}

// Single source of truth for the scroll multiplier. Bound fixed-motion scripts use this too,
// while sky/local scripts deliberately do not. notes.md §Endless scroll boost.
int endlessScrollBoostPercent(void)
{
	if (!endlessMode)
		return 0;
	if (endlessActiveMods & (ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_WARP))
		return 220;
	if (endlessActiveMods & (ENDLESS_MOD_OVERCLOCK | ENDLESS_MOD_SLIPSTREAM))
		return 70;
	return 0;
}

// True while a scroll-speed modifier is active -- STABLE across ticks, unlike the fractional
// step count, so the bg bottom-margin gate can't flicker (notes.md §Endless scroll boost).
bool endlessScrollBoostActive(void)
{
	return endlessScrollBoostPercent() != 0;
}

// Smooth vertical scroll for ONE layer: outputs the constant display rate + sub-pixel fraction, and
// (only under a scroll modifier) returns extra px so base + extra tracks a boosted target. Call once
// per channel per tick; channel 0/1/2 = bg layer 1/2/3. notes.md §Slow-scroll smoothing.
int endlessScrollExtraPx(int channel, int fireStep, int delayMax, int baseThisTick,
                         float *rateOut, float *fracOut)
{
	static int carry[3] = { 0, 0, 0 };  // signed pending extra scroll, px*100, per channel
	static int trem[3]  = { 0, 0, 0 };  // remainder of the target's /delayMax division, per channel
	if (rateOut != NULL)
		*rateOut = 0.0f;
	if (fracOut != NULL)
		*fracOut = 0.0f;
	if (channel < 0 || channel > 2)
		return 0;
	const int boost = endlessScrollBoostPercent();
	if (fireStep <= 0)  // the layer isn't scrolling this section
	{
		carry[channel] = 0;
		trem[channel]  = 0;
		return 0;
	}
	if (delayMax < 1)
		delayMax = 1;
	// Target per-tick scroll (px*100): average base rate fireStep/delayMax scaled by the modifier,
	// remainder carried so the long-run average is exact. boost 0 (no modifier) still runs -- the
	// base rate alone is smoothed for the display. notes.md §Slow-scroll smoothing.
	int tnum = fireStep * (100 + boost) + trem[channel];
	int target = tnum / delayMax;
	trem[channel] = tnum - target * delayMax;
	carry[channel] += target - baseThisTick * 100;
	// Only a real modifier drains whole px to ADD scroll (base + extra tracks the target); the base
	// rate alone emits none, so the sim scroll stays byte-identical to the stock game.
	int px = 0;
	if (boost > 0 && carry[channel] >= 100)
	{
		px = carry[channel] / 100;
		carry[channel] -= px * 100;
	}
	if (carry[channel] > 5000 || carry[channel] < -5000)  // guard runaway if a layer is never drawn
		carry[channel] = 0;
	if (rateOut != NULL)
		*rateOut = (float)target / 100.0f;
	if (fracOut != NULL)
		*fracOut = (float)carry[channel] / 100.0f;
	return px;
}

// Player-ship blit filter (0 = none). Only PLAYER-SIDE buffs tint the hull, while the kill-fire
// boost is active: Overdrive burns red, plain Turbodrive electric yellow. (The
// E-Shop "Turbodrive"/"Overdrive" buys and a Turbodrive sector all directly buff the player;
// Overclock/Overload change ENEMY behaviour, so they no longer tint the ship.)
int endlessShipTintFilter(void)
{
	if (!endlessMode)
		return 0;
	if (endlessTurbodriveActive())  // active kill-fire window (boost OR evil curse)
	{
		if (endlessActiveMods & ENDLESS_MOD_KILLFIRE_EVIL)
			return ENDLESS_EVIL_SHIP_FILTER;         // ominous curse tint
		if (endlessActiveMods & ENDLESS_MOD_OVERBLAST)
			return ENDLESS_OVERBLAST_SHIP_FILTER;    // blue -- damage-only buff
		return (endlessActiveMods & ENDLESS_MOD_OVERDRIVE)
		       ? ENDLESS_OVERDRIVE_SHIP_FILTER   // electric yellow
		       : ENDLESS_TURBODRIVE_SHIP_FILTER;   // red
	}
	return 0;
}
