/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 *
 * Endless mode: Chart-a-Course -- generating and committing the next sector.
 *
 * One of the endless_*.c files that make up endless mode: endless.h is the public
 * interface, endless_internal.h the state and helpers the group shares.
 */

#include "endless.h"
#include "endless_internal.h"

#include "config.h"        // difficultyLevel, DIFFICULTY_*, player-independent globals
#include "custom_weapon.h" // customWeaponPort / customSidekickSlot (reserved shop slots)
#include "episodes.h"      // item arrays + SHIP_NUM/PORT_NUM/... counts, episodeAvail, JE_initEpisode
#include "game_menu.h"     // JE_itemScreen, JE_getLevelSections
#include "joystick.h"      // push_joysticks_as_keyboard
#include "player.h"        // player[]
#include "sprite.h"        // JE_loadCompShapes, enemySpriteSheets, shopSpriteSheet
#include "tyrian2.h"       // itemAvail, itemAvailMax
#include "varz.h"          // eventRec, maxEvent, map* globals

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Next-level courses ---------------------------------------------------------
// The next-level choice IS the shop's "Start Level" submenu: game_menu.c drives its planet
// monitor from these courses. Each is a shipped level plus its own risk/reward mutators.
// Generated once per shop visit; picking one applies its mutators and launches.

int      endlessCourseCnt = 0;
int      endlessCourseEp[ENDLESS_MAX_COURSES];
JE_byte  endlessCourseSec[ENDLESS_MAX_COURSES];
JE_byte  endlessCourseFile[ENDLESS_MAX_COURSES];  // each course's specific lvlFileNum (see forcedLvlFileNum)
Uint64 endlessCourseMod[ENDLESS_MAX_COURSES];
static JE_byte  endlessCourseNameSalt[ENDLESS_MAX_COURSES];  // per-visit nudge so no two offered names read the same
static char     endlessCourseBaseName[ENDLESS_MAX_COURSES][10];  // Radar perk: authored level name behind each course (9 chars + NUL)
int      endlessLastEp = 0;
JE_byte  endlessLastSec = 0;
bool     endlessForced = false;  // this visit is a forced "Ambush" (single dangerous sector)

// The intrinsic-danger nudge for course i's shipped level at the current run difficulty
// (endless_levelprofile.h): folded into the course's shown danger tier/grade, its sort position and
// its payout so all three reflect the LEVEL, not just its modifiers. 0 for an out-of-range course.
static int endlessCourseBaseDanger(int i)
{
	if (i < 0 || i >= endlessCourseCnt)
		return 0;
	return endlessLevelBaseDanger(endlessCourseEp[i], endlessCourseFile[i], difficultyLevel);
}

// The TRUTHFUL clear payout for course i at the current depth -- the modifier table plus the LEVEL's
// fine payoutMille term (thousandths of base), exactly as endlessClearBonus banks it on clear (via
// endlessSortiePayoutMille, keyed off the same committed level), so shown == banked. This is what
// varies the cash between same-grade courses. Shown highlighted on the help line (game_menu.c).
long endlessCoursePayout(int i)
{
	if (i < 0 || i >= endlessCourseCnt)
		return 0;
	return endlessClearBonusForEx(endlessCourseMod[i],
	                              endlessLevelPayoutMille(endlessCourseEp[i], endlessCourseFile[i], difficultyLevel));
}

// Pick a random BOON bit safe to weld onto a hostile course that already carries `hostiles`, turning it
// into a MIXED "gambit" sector (real reward on real danger). Boons that would fight an existing threat on
// the SAME lever are held back -- frail foes vs +HP (Fortified), crawling shots vs faster shots
// (Swift/Overclock) -- so the sector's red/green monitor rows never contradict. The overpowered kill-fire
// boons get a small roll of their own instead of two full shares in the ordinary candidate pool; this keeps
// them uncommon in gambits while preserving the named pure-boon courses. Only one boon is added, so the
// one-kill-fire-mod rule holds. The first three ordinary boons are always eligible, so this never returns 0.
static Uint64 endlessPickMixBoon(Uint64 hostiles)
{
	// Split evenly between the two kill-fire boons allowed on mixed courses. Overdrive remains a
	// pure-boon/shop effect: a hostile course should not casually roll the strongest version.
	if (endlessRand() % 100 < 4)
		return (endlessRand() % 2) ? ENDLESS_MOD_TURBODRIVE : ENDLESS_MOD_OVERBLAST;

	// The no-elite-tier boons get a small roll of their own too, so a gambit that thins the specials stays
	// uncommon. NOELITE wipes the whole elite/champion tier, so it's held back when the course's danger IS
	// that tier (it would cancel, not gamble); NOCHAMP only clips the champion spikes, so it may ride an
	// Elite Pack (a real trade) but not an all-champion Legion. Weighted ~2:1 toward the milder NOCHAMP, so
	// NOELITE gambits are the rarer sight; if an on-lever threat blocks the pick, fall through to the pool.
	// Gated on the 25%-share unlock like every other no-elite-tier path (roll first so the stream is stable).
	if ((endlessRand() % 100 < 6) && endlessEliteBoonsUnlocked())
	{
		if ((endlessRand() % 3) == 0)  // ~1/3 of the roll aims for the stronger, rarer NOELITE
		{
			if (!(hostiles & (ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_APEX | ENDLESS_MOD_LEGION)))
				return ENDLESS_MOD_NOELITE;
		}
		else if (!(hostiles & ENDLESS_MOD_LEGION))
			return ENDLESS_MOD_NOCHAMP;
	}

	Uint64 cand[5];
	int n = 0;
	cand[n++] = ENDLESS_MOD_OVERCHARGE;   // more player damage -- always safe
	cand[n++] = ENDLESS_MOD_BOUNTY;       // pure cash, no safety -- always safe
	cand[n++] = ENDLESS_MOD_FAVOR;        // cheaper next shop -- always safe
	if (!(hostiles & ENDLESS_MOD_FORTIFIED))                        // frail vs +HP would cancel
		cand[n++] = ENDLESS_MOD_FRAGILE;
	if (!(hostiles & (ENDLESS_MOD_SWIFT | ENDLESS_MOD_OVERCLOCK)))  // slow shots vs fast shots would cancel
		cand[n++] = ENDLESS_MOD_DILATION;
	return cand[endlessRand() % n];
}

// The boon table doubles as the canonical name dictionary, so keep its semantic bitsets intact.
// At generation time, swap the Turbodrive and Overblast rarity slots instead: Overblast inherits
// Turbodrive's many common theme slots, while Turbodrive inherits Overblast's few rare ones. This is
// an involution, so distinct Jackpot themes remain distinct after the mapping. Mixed gambits already
// split the two evenly; Reactor Redline deliberately stays Turbodrive because it promises fast guns.
static Uint64 endlessSwapTurbodriveOverblast(Uint64 mods)
{
	const bool hadTurbodrive = (mods & ENDLESS_MOD_TURBODRIVE) != 0;
	const bool hadOverblast  = (mods & ENDLESS_MOD_OVERBLAST) != 0;
	mods &= ~(ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_OVERBLAST);
	if (hadTurbodrive)
		mods |= ENDLESS_MOD_OVERBLAST;
	if (hadOverblast)
		mods |= ENDLESS_MOD_TURBODRIVE;
	return mods;
}

// Build a random emergent PURE-BOON combo (2, sometimes 3 bits) for a boon course -- more variety than
// the named boon themes alone. The pool holds only ONE kill-fire boon (Overblast), so two can never
// stack, and every pair is on an independent lever, so nothing cancels. Only NOCHAMP is in the pool of
// the two no-elite-tier boons: they SHARE the elite lever (NOELITE supersedes NOCHAMP), so pooling both
// could roll a self-cancelling pair -- keeping NOELITE out of the emergent pool sidesteps that and leaves
// NOELITE to the named themes alone, one lever keeping the stronger boon the rarer one.
static Uint64 endlessMakeBoonCombo(void)
{
	Uint64 pool[7];
	int poolN = 0;
	pool[poolN++] = ENDLESS_MOD_FRAGILE;
	pool[poolN++] = ENDLESS_MOD_BOUNTY;
	pool[poolN++] = ENDLESS_MOD_OVERCHARGE;
	pool[poolN++] = ENDLESS_MOD_DILATION;
	pool[poolN++] = ENDLESS_MOD_FAVOR;
	pool[poolN++] = ENDLESS_MOD_OVERBLAST;
	if (endlessEliteBoonsUnlocked())        // NOCHAMP only once elites are a real presence (>25% share)
		pool[poolN++] = ENDLESS_MOD_NOCHAMP;
	int ord[COUNTOF(pool)];
	for (int k = 0; k < poolN; ++k)
		ord[k] = k;
	for (int k = poolN - 1; k > 0; --k)
	{
		const int j = endlessRand() % (k + 1);
		const int t = ord[k]; ord[k] = ord[j]; ord[j] = t;
	}
	const int want = 2 + (endlessRand() % 100 < 40);   // 2, sometimes 3
	Uint64 combo = 0;
	for (int k = 0; k < want && k < poolN; ++k)
		combo |= pool[ord[k]];
	return combo;
}

// --- Deep-run course-danger escalation ---------------------------------------------
// From ENDLESS_DANGER_RAMP_START (zone 40) the Chart-a-Course rolls tilt steadily against the player:
// busier multi-danger combos, higher danger ratings, more frequent rare/super-rare dangerous sectors,
// and more danger-only (Gauntlet/Ambush) visits with fewer calm/boon/jackpot ones. The tilt keeps
// climbing the whole way from zone 40 to ENDLESS_DANGER_RAMP_FULL (zone 250) and CAPS there: about
// TWICE as dangerous by the mid-point (zone 100), about SIX times by the cap. It's a TWO-STAGE ramp --
// a gentle first stage to the mid-point, then a steeper second stage to the cap -- so the deep end can
// reach ~6x by zone 250 while the zone<=100 range stays byte-for-byte the approved ~2x tuning. Even at
// the cap the player never loses the last shot at a calm sector / boon / jackpot: course 0 stays clean
// unless Gauntlet/Ambush fires, and BOTH of those are hard-capped WELL below certainty (the CAP_PCT
// knobs -> a calm route survives ~46% of visits even at the deepest cap), while the boon/jackpot rolls
// only thin, never vanish. So most of the deep-end escalation past the mid-point lands on the UNCAPPED
// levers -- rarer boons/jackpots and more frequent rare/super-rare injections -- not on making danger a
// sure thing. Difficulty-scaled like every other lever, so harder modes ramp sooner (notes.md §Course
// generation & danger labels).
#define ENDLESS_DANGER_RAMP_START 40   // zone the tilt begins (no escalation at/below)
#define ENDLESS_DANGER_RAMP_MID   100  // zone the tilt reaches its "~2x" tuning (scale == ENDLESS_DANGER_RAMP_MID_SCALE)
#define ENDLESS_DANGER_RAMP_FULL  250  // zone the tilt caps at its "~6x" tuning (scale == ENDLESS_DANGER_RAMP_FULL_SCALE)
#define ENDLESS_DANGER_RAMP_MID_SCALE  100  // scale at the mid-point -- the approved ~2x point; keep at 100 so zone<=MID is unchanged
#define ENDLESS_DANGER_RAMP_FULL_SCALE 500  // scale at the cap -- the deep-end "~6x" (only the uncapped levers read this far)
#define ENDLESS_DANGER_GAUNTLET_CAP_PCT 45  // ceiling on the all-hostile Gauntlet chance -- keeps a calm route always possible
#define ENDLESS_DANGER_AMBUSH_CAP_PCT   15  // ceiling on the one-forced-danger Ambush chance

// The tilt SCALE (NOT a percentage): 0 at START, MID_SCALE (100) at MID, then a steeper stage up to
// FULL_SCALE at FULL, capped. zone<=MID reproduces the earlier single-stage ramp exactly (MID_SCALE==100
// over START..MID); only the second stage is new.
static int endlessDangerRamp(void)
{
	if (!endlessMode)
		return 0;
	const int zone = endlessDifficultyZone();
	if (zone <= ENDLESS_DANGER_RAMP_START)
		return 0;
	if (zone <= ENDLESS_DANGER_RAMP_MID)  // first stage: 0 -> MID_SCALE across START..MID (unchanged)
		return ENDLESS_DANGER_RAMP_MID_SCALE * (zone - ENDLESS_DANGER_RAMP_START)
		         / (ENDLESS_DANGER_RAMP_MID - ENDLESS_DANGER_RAMP_START);
	// Second stage: MID_SCALE -> FULL_SCALE across MID..FULL, then held at the cap.
	const int s = ENDLESS_DANGER_RAMP_MID_SCALE
	                + (ENDLESS_DANGER_RAMP_FULL_SCALE - ENDLESS_DANGER_RAMP_MID_SCALE) * (zone - ENDLESS_DANGER_RAMP_MID)
	                    / (ENDLESS_DANGER_RAMP_FULL - ENDLESS_DANGER_RAMP_MID);
	return (s > ENDLESS_DANGER_RAMP_FULL_SCALE) ? ENDLESS_DANGER_RAMP_FULL_SCALE : s;
}

// Shrink a "1 in N" rare-danger divisor with the ramp (toward N/2 at the mid-point, ~N/6 at the cap),
// so rare / super-rare dangerous sectors show up ~2x as often by zone 100 and ~6x by zone 250.
// Floored at 1 (never a divide-by-zero).
static int endlessDangerRareDiv(int base)
{
	const int d = base * 100 / (100 + endlessDangerRamp());
	return (d < 1) ? 1 : d;
}

// --- Rare sector injections ---------------------------------------------------------------
// After the ordinary hostile/boon/gambit courses are dealt, each row below gets ONE "1 in N"
// roll; if it fires, it takes over a random non-clean course slot. THE TUNING KNOBS ARE HERE:
// `oneInN` is the base rarity at zone <= ENDLESS_DANGER_RAMP_START, and every one of them is
// routed through endlessDangerRareDiv, so from zone 40 they all grow steadily more frequent
// (~2x by zone 100, ~6x by the cap).
//
// ORDER MATTERS. The rolls happen top to bottom and each overwrites the slot it lands on, so a
// row placed later wins a contested slot -- kamikaze after homing (the harder tier wins the
// clash), the elite-tier sectors late enough to override a boon slot, and dead generator last
// of all because it is the nastiest handicap in the game.
//
// A row either draws from a named-theme `pool` (optionally narrowed to entries carrying all of
// `must` and none of `forbid`) or, with no pool, deals the fixed `mods` bitset.
typedef struct {
	int                 oneInN;   // base rarity: 1 in this many visits, before the danger ramp
	const EndlessTheme *pool;     // pool to draw the sector from; NULL = deal `mods` instead
	unsigned            poolN;
	Uint64              must;     // pool entries must carry all of these bits...
	Uint64              forbid;   // ...and none of these
	Uint64              mods;     // the fixed bitset, when there is no pool
} EndlessRareInjection;

#define RARE_FROM(n, tbl)               { (n), (tbl), COUNTOF(tbl), 0, 0, 0 }
#define RARE_PICK(n, tbl, must, forbid) { (n), (tbl), COUNTOF(tbl), (must), (forbid), 0 }
#define RARE_FIXED(n, bits)             { (n), NULL, 0, 0, 0, (bits) }
static const EndlessRareInjection endlessRareInjections[] = {
	// Homing: the GENTLEST homing tier -- enemies barely lean toward you, no ram.
	RARE_FROM(25, endlessHomingThemes),
	// Kamikaze: the MODERATE homing tier (strength 3, no ram -- the brutal rammer moved to the
	// RAMPAGE gamble). After homing, so the harder tier wins a clash.
	RARE_FROM(50, endlessKamikazeThemes),
	// Warp Speed: a scroll THREAT -- the level hurtles at you.
	RARE_FIXED(12, ENDLESS_MOD_WARP),
	// Overload: Overclock cranked way up.
	RARE_FROM(14, endlessOverloadThemes),
	// Evil Turbodrive / Overdrive: your own kill streak turned against you -- jammed guns, and
	// for Evil Overdrive weaker shots too.
	RARE_FROM(16, endlessEvilThemes),
	// Reactor Redline: the gamble "Overheat" loose in the wild -- kills quicken your guns, but
	// the redlined core cooks your hull.
	RARE_FROM(60, endlessRedlineThemes),
	// Tar Pit (SLUGGISH + GRAVITY): the ship crawls WHILE dragged down. Brutal but always flyable
	// (endlessGravityDrift slows the pull with the ship). SLUGGISH stays out of the combinable
	// pool, so this is the ONLY place the sluggish+gravity pairing appears.
	RARE_FROM(30, endlessSluggishThemes),
	// Apex Swarm (every enemy elite), from the Apex-tier rare themes: bare Apex, or Apex plus one
	// extra danger. Late enough to override a boon slot.
	RARE_PICK(40, endlessRareThemes, ENDLESS_MOD_APEX, ENDLESS_MOD_LEGION),
	// Legion (every enemy a CHAMPION) -- rarer still, among the deadliest sectors.
	RARE_PICK(70, endlessRareThemes, ENDLESS_MOD_LEGION, 0),
	// Cataclysm: an extreme multi-danger nightmare with no elite tier -- just everything at once.
	// The rare themes carrying neither Apex nor Legion (the 5+-danger pure combos).
	RARE_PICK(45, endlessRareThemes, 0, ENDLESS_MOD_APEX | ENDLESS_MOD_LEGION),
	// Dead Generator (DEADGEN): no shield regen AND a power-starved main gun. The nastiest
	// handicap in the game, so it is the rarest and rolled LAST -- it claims the slot when it
	// fires. Rear guns / sidekicks / specials carry the fight.
	RARE_FROM(55, endlessDeadgenThemes),
};
#undef RARE_FROM
#undef RARE_PICK
#undef RARE_FIXED

// Retire a now-pointless "half enemies elite" (ELITEPACK) on course c. ELITEPACK pins the special-
// enemy share to 50%, which is a difficulty BUMP only while the natural depth share sits below it;
// once the natural share tops 50% (deep runs -- see endlessNaturalEliteChancePercent), pinning it to
// 50% would CAP elites below the natural rate, turning a "hostile" course into a stealth boon. Past
// that crossover, swap ELITEPACK for a comparable hostile bit so the sector stays a real threat; the
// name, danger tier and clear reward all re-derive from the new bitset. APEX/LEGION force a 100%
// share (always a genuine increase over the 80% natural cap), so those are left untouched.
//
// Deterministic (no RNG) and driven by the same endlessRunDepth the launched level will see, so the
// Chart-a-Course preview and the played sector always agree, and a reloaded outpost re-derives the
// same swap. The replacement avoids any bitset another offered course already uses, so the visit
// keeps distinct sectors.
static void endlessFixRedundantElitePack(int c)
{
	const Uint64 mods = endlessCourseMod[c];
	if (!(mods & ENDLESS_MOD_ELITEPACK))
		return;
	if (mods & (ENDLESS_MOD_APEX | ENDLESS_MOD_LEGION))
		return;                               // 100% share -- a real increase, keep it
	if (endlessNaturalEliteChancePercent() <= 50)
		return;                               // still raises the share -- ELITEPACK is genuine here

	const Uint64 base = mods & ~(Uint64)ENDLESS_MOD_ELITEPACK;
	// Ordered by rough danger so the stand-in is a fair replacement; the first bit not already on the
	// course (and, where possible, not colliding with another offered course) wins.
	static const Uint64 subs[] = {
		ENDLESS_MOD_DEVASTATING, ENDLESS_MOD_FORTIFIED, ENDLESS_MOD_ENRAGE,
		ENDLESS_MOD_FRENZY, ENDLESS_MOD_SWIFT, ENDLESS_MOD_GRAVITY, ENDLESS_MOD_OVERCLOCK,
	};
	Uint64 fallback = base;                   // if every candidate collides, at least drop ELITEPACK
	bool haveFallback = false;
	for (unsigned i = 0; i < COUNTOF(subs); ++i)
	{
		if (base & subs[i])
			continue;                         // already present -- adding it would be a no-op
		const Uint64 cand = base | subs[i];
		if (!haveFallback) { fallback = cand; haveFallback = true; }
		bool clash = false;
		for (int k = 0; k < endlessCourseCnt && !clash; ++k)
			if (k != c && endlessCourseMod[k] == cand)
				clash = true;
		if (!clash)
		{
			endlessCourseMod[c] = cand;
			return;
		}
	}
	endlessCourseMod[c] = fallback;           // every stand-in collided/was present -- best effort
}

// --- Milestone slates (see endlessMilestoneKind, up top) -------------------------------------
// ENDLESS_THEME_THE_END -- the pinned 100th-zone sector -- is defined with its naming table, next to
// endlessRareThemes, so the macro and the row that names it sit together and can't drift.
//
// Hostile bits a milestone slate is built from. `group` marks mutually redundant bits -- at most one
// per nonzero group lands on a course: one scroll modifier (Overclock already carries Slipstream's
// scroll), one homing tier, one elite tier, one shield handicap. The super-rare signatures (dead
// generator, the evil kill-fire mirrors, reactor redline) stay out: a milestone should be a wall of
// ordinary dangers, not a scheduled visit from the rarest sector in the game.
static const struct { Uint64 bit; int group; } endlessMilestonePool[] = {
	{ ENDLESS_MOD_FORTIFIED,   0 },
	{ ENDLESS_MOD_FRENZY,      0 },
	{ ENDLESS_MOD_SWIFT,       0 },
	{ ENDLESS_MOD_DEVASTATING, 0 },
	{ ENDLESS_MOD_ENRAGE,      0 },
	{ ENDLESS_MOD_GRAVITY,     0 },
	{ ENDLESS_MOD_TOPSY,       0 },
	{ ENDLESS_MOD_SLUGGISH,    0 },
	{ ENDLESS_MOD_SLIPSTREAM,  1 },  // scroll pace
	{ ENDLESS_MOD_OVERCLOCK,   1 },
	{ ENDLESS_MOD_OVERLOAD,    1 },
	{ ENDLESS_MOD_WARP,        1 },
	{ ENDLESS_MOD_HOMING,      2 },  // homing tier
	{ ENDLESS_MOD_KAMIKAZE,    2 },
	{ ENDLESS_MOD_APEX,        3 },  // elite tier
	{ ENDLESS_MOD_LEGION,      3 },
	{ ENDLESS_MOD_SHIELDLESS,  4 },  // shield handicap
};

// The danger weight one modifier bit contributes, read straight off endlessModTable so a milestone's
// target rank can never drift from the table the rank bands are computed from.
static int endlessModReward(Uint64 bit)
{
	for (unsigned i = 0; i < COUNTOF(endlessModTable); ++i)
		if (endlessModTable[i].bit == bit)
			return endlessModTable[i].reward;
	return 0;
}

// Build a random pure-hostile combo whose letter grade is exactly `rank` (6 = S, 7 = S+, 8 = S++,
// 9 = S+++), distinct from the `usedN` bitsets already dealt this visit. The bands mirror
// endlessDangerRankLevel -- keep the two in lockstep -- and the built combo is checked against it
// (synergy included) before being handed back, so a retuned band/synergy can never silently mislabel
// a milestone. S+++ is open-ended, but the build stops well short of piling on every bit in the pool:
// brutal, still flyable.
static Uint64 endlessMakeRankCombo(int rank, const Uint64 *used, int usedN)
{
	const int lo = (rank <= 6) ? 34 : (rank == 7) ? 40 : (rank == 8) ? 50 : 60;
	const int hi = (rank <= 6) ? 39 : (rank == 7) ? 49 : (rank == 8) ? 59 : 95;

	Uint64 best = 0;  // first in-band combo seen, used if every attempt collides with an offered one
	for (int attempt = 0; attempt < 60; ++attempt)
	{
		int ord[COUNTOF(endlessMilestonePool)];
		for (unsigned k = 0; k < COUNTOF(endlessMilestonePool); ++k)
			ord[k] = (int)k;
		for (int k = (int)COUNTOF(endlessMilestonePool) - 1; k > 0; --k)
		{
			const int j = endlessRand() % (k + 1);
			const int t = ord[k]; ord[k] = ord[j]; ord[j] = t;
		}

		// Randomised greedy: walk the shuffled pool taking every bit that doesn't overshoot the band,
		// and stop the moment the running score is inside it.
		Uint64 combo = 0;
		int score = 0;
		unsigned groups = 0;
		for (unsigned k = 0; k < COUNTOF(endlessMilestonePool) && score < lo; ++k)
		{
			const int g = endlessMilestonePool[ord[k]].group;
			if (g != 0 && (groups & (1u << g)))
				continue;
			const int w = endlessModReward(endlessMilestonePool[ord[k]].bit);
			if (score + w > hi)
				continue;
			combo |= endlessMilestonePool[ord[k]].bit;
			score += w;
			if (g != 0)
				groups |= 1u << g;
		}
		if (endlessDangerRankLevel(combo) != rank)
			continue;  // this shuffle couldn't reach the band -- reshuffle

		if (best == 0)
			best = combo;
		bool clash = false;
		for (int k = 0; k < usedN && !clash; ++k)
			if (used[k] == combo)
				clash = true;
		if (!clash)
			return combo;
	}
	return best;
}

// --- Chart-a-Course generation ------------------------------------------------------------
// endlessGenerateCourses at the bottom of this block runs the phases below IN ORDER. Each phase
// reads and rewrites the shared endlessCourse* arrays, so ORDER IS BEHAVIOUR -- and because every
// draw comes off the seeded structural stream, moving a phase (or adding one that draws) changes
// what every existing seed generates. Add new phases at the end unless you mean to reshuffle.

// The "ordinary" hostile bits: the ones the widen/gambit/diverse-choice phases are allowed to
// combine freely. Everything else is either a rare signature sector (injected) or a boon.
// Kamikaze / overload / warp / sluggish stay out -- they are special or rare-injected (SLUGGISH's
// only combinable pairing, gravity, is a rare injection, so it can't emerge randomly here).
static const Uint64 endlessCombinableMods[] = {
	ENDLESS_MOD_FORTIFIED, ENDLESS_MOD_FRENZY, ENDLESS_MOD_SWIFT, ENDLESS_MOD_DEVASTATING,
	ENDLESS_MOD_ENRAGE, ENDLESS_MOD_GRAVITY, ENDLESS_MOD_ELITEPACK, ENDLESS_MOD_OVERCLOCK,
	ENDLESS_MOD_TOPSY,  // the flipped-view mod mixes freely with everything (purely visual, no softlock)
	ENDLESS_MOD_SHIELDLESS,  // a pure defense debuff -- safe to stack onto any combo (DEADGEN stays out: super-rare, injected only)
	// SLIPSTREAM stays out: Overclock (in the pool) already carries the same +70% scroll, so a
	// random pairing would be a redundant bit. Slipstream sectors come from the named-theme shuffle.
};

// Every combinable bit OR'd together -- "is this course built only from ordinary dangers?".
static Uint64 endlessCombinableMask(void)
{
	Uint64 mask = 0;
	for (unsigned k = 0; k < COUNTOF(endlessCombinableMods); ++k)
		mask |= endlessCombinableMods[k];
	return mask;
}

// Gather up to `wantCourses` candidate levels. endlessRandomSafeLevel already keeps each pick out
// of the recent-play window (so no course repeats the just-played zone or the few before it);
// here we only dedup WITHIN this visit, by (episode, section) -- a visit never offers two courses
// that share a section, so the two TYRIAN cuts can't both appear at once (they'd read as the same
// course), but either cut can be the section-3 course on any given visit.
static void endlessGatherCourseLevels(int wantCourses)
{
	for (int guard = 0; guard < 40 && endlessCourseCnt < wantCourses; ++guard)
	{
		int ep;
		JE_byte sec, file;
		if (!endlessRandomSafeLevel(&ep, &sec, &file))
			break;

		bool dup = false;
		for (int k = 0; k < endlessCourseCnt && !dup; ++k)
			if (endlessCourseEp[k] == ep && endlessCourseSec[k] == sec)
				dup = true;
		if (dup)
			continue;

		endlessCourseEp[endlessCourseCnt] = ep;
		endlessCourseSec[endlessCourseCnt] = sec;
		endlessCourseFile[endlessCourseCnt] = file;
		endlessCourseMod[endlessCourseCnt] = 0;
		++endlessCourseCnt;
	}
	if (endlessCourseCnt == 0)  // fallback: guarantee at least one course
	{
		int ep = episodeNum;
		JE_byte sec = FIRST_LEVEL, file = 0;
		endlessRandomSafeLevel(&ep, &sec, &file);
		endlessCourseEp[0] = ep;
		endlessCourseSec[0] = sec;
		endlessCourseFile[0] = file;
		endlessCourseMod[0] = 0;
		endlessCourseCnt = 1;
	}
}

// Shuffle the hostile-theme table into `idx`. The order is reused by three later phases (the
// initial deal, the duplicate re-roll and the gauntlet), which is why it is drawn once here.
static void endlessShuffleThemeOrder(int *idx)
{
	for (unsigned i = 0; i < COUNTOF(endlessHostileThemes); ++i)
		idx[i] = (int)i;
	for (int i = (int)COUNTOF(endlessHostileThemes) - 1; i > 0; --i)
	{
		const int j = endlessRand() % (i + 1);
		const int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
	}
}

// The first unused hostile theme in the shuffled order, or 0 if every one is already on the chart.
// Consumes no RNG, so callers stay aligned with the seed stream.
static Uint64 endlessUnusedHostileTheme(const int *idx, int forCourse)
{
	for (unsigned t = 0; t < COUNTOF(endlessHostileThemes); ++t)
	{
		const Uint64 m = endlessHostileThemes[idx[t]].mods;
		bool used = false;
		for (int k = 0; k < endlessCourseCnt && !used; ++k)
			if (k != forCourse && endlessCourseMod[k] == m)
				used = true;
		if (!used)
			return m;
	}
	return 0;
}

// Course 0 is always clean; courses 1+ get distinct hostile themes off the shuffled pool, so a
// visit never offers the same danger twice.
static void endlessDealHostileThemes(const int *idx)
{
	endlessCourseMod[0] = 0;
	for (int c = 1; c < endlessCourseCnt; ++c)
		endlessCourseMod[c] = endlessHostileThemes[idx[c - 1]].mods;
}

// WIDEN VARIETY: about half the hostile courses instead get a RANDOM combination of the ordinary
// hostile bits (any un-named combo still gets a generated name/help), weighted toward 2-4 bits.
static void endlessWidenHostileCombos(int dangerRamp)
{
	for (int c = 1; c < endlessCourseCnt; ++c)
	{
		// Deep runs push more courses off the (often single-danger) curated themes onto busy random
		// multi-danger combos: the ~50% base share climbs to ~75% by the mid-point, capped at 85% so a
		// few legible curated themes always survive (the diverse-choice guarantee leans on them).
		int comboShare = 50 + dangerRamp * 25 / 100;
		if (comboShare > 85)
			comboShare = 85;
		if (endlessRand() % 100 >= comboShare)
			continue;
		// Bit-count weights leaning heavier on triples/quads than before, so busy multi-danger sectors
		// show up as often as clean singles (avg ~2.8 bits, up from ~2.3); the danger ramp lifts every
		// threshold, so deep sectors pile on more simultaneous dangers (avg ~4 bits at the cap).
		int want = 1 + (endlessRand() % 100 < 80 + dangerRamp * 15 / 100)
		             + (endlessRand() % 100 < 55 + dangerRamp * 35 / 100)
		             + (endlessRand() % 100 < 30 + dangerRamp * 40 / 100)
		             + (endlessRand() % 100 < 12 + dangerRamp * 38 / 100);
		if (want > (int)COUNTOF(endlessCombinableMods))
			want = (int)COUNTOF(endlessCombinableMods);
		int ord[COUNTOF(endlessCombinableMods)];
		for (unsigned k = 0; k < COUNTOF(endlessCombinableMods); ++k)
			ord[k] = (int)k;
		for (int k = (int)COUNTOF(endlessCombinableMods) - 1; k > 0; --k)
		{
			const int j = endlessRand() % (k + 1);
			const int tmp = ord[k]; ord[k] = ord[j]; ord[j] = tmp;
		}
		Uint64 combo = 0;
		for (int k = 0; k < want; ++k)
			combo |= endlessCombinableMods[ord[k]];
		endlessCourseMod[c] = combo;
	}
}

// A boon course is uncommon (~1 in 3 visits replaces a hostile one): most draw a named boon theme
// (single or curated combo), but ~40% instead roll a fresh emergent boon pair/triple, so
// pure-good sectors vary beyond the named set too. The danger ramp thins boon courses deep (~1/3
// early, ~1/5 by the mid, gently past that), but they never vanish. The deep-thinning coefficient is
// kept a touch gentle (a small nudge, since the extra milestone zones force danger more often now).
static void endlessDealBoonCourse(int dangerRamp)
{
	if (endlessCourseCnt > 1 && (endlessRand() % (3 + dangerRamp * 2 / 100)) == 0)
	{
		const int slot = 1 + endlessRand() % (endlessCourseCnt - 1);
		if (endlessRand() % 100 < 40)
			endlessCourseMod[slot] = endlessMakeBoonCombo();
		else
			// Forbid the no-elite-tier boons until the 25%-share unlock, so a shallow boon course draws a
			// different theme instead of a near-empty "no elites" one.
			endlessCourseMod[slot] = endlessSwapTurbodriveOverblast(
				endlessPickThemeMods(endlessBoonThemes, COUNTOF(endlessBoonThemes), 0,
				                     endlessEliteBoonsUnlocked() ? 0 : (ENDLESS_MOD_NOCHAMP | ENDLESS_MOD_NOELITE)));
	}
}

// MIXED "gambit" sectors: graft a compatible boon onto some ORDINARY hostile courses, welding a real
// reward onto real danger. This is the main source of good-mixed-with-bad variety -- roughly a third of
// the plain hostile courses become a risk/reward gambit each visit. Only "ordinary" courses (hostile
// bits drawn from the combinable pool) are eligible, so rare signatures (Apex / Overload / Kamikaze /
// Tar Pit / dead generator / evil-mirror / ...) and boon/clean routes keep their identity. The boon is
// chosen to never fight the course's threats (endlessPickMixBoon), and only one is added, so the
// one-kill-fire rule holds. Named pairs/triples/quads land their endlessMixedThemes label; the rest read
// as a generated "gambit". Runs after the boon roll (won't override a boon course) and before the danger
// sort, so the mitigation credit lands the gambit at the right rung.
static void endlessGraftGambits(int dangerRamp)
{
	// Slipstream isn't in the combinable pool (redundant beside Overclock's scroll), but its named
	// hostile sectors are ordinary enough to gamble on -- keep them boon-graft eligible so the
	// Blitz / Time Warp / Power Play / Payday / Smash and Grab gambits stay reachable.
	const Uint64 mixCommon = endlessCombinableMask() | ENDLESS_MOD_SLIPSTREAM;
	for (int c = 1; c < endlessCourseCnt; ++c)
	{
		const Uint64 h = endlessCourseMod[c] & ENDLESS_HOSTILE_MASK;
		if (h == 0 || (h & ~mixCommon) != 0)       // only ordinary hostile courses
			continue;
		if (endlessCourseMod[c] & ENDLESS_BOON_MASK) // already a gambit / carries a boon
			continue;
		int gambitPct = 35 - dangerRamp * 20 / 100;  // ~35% gain a boon early -> ~15% by the mid -> 5% floor deep
		if (gambitPct < 5)
			gambitPct = 5;
		if (endlessRand() % 100 >= gambitPct)  // fewer mitigations grafted onto hostiles as the run deepens
			continue;
		endlessCourseMod[c] |= endlessPickMixBoon(h);
	}
}

// Rare / super-rare danger injections, rolled IN TABLE ORDER (see endlessRareInjections): each
// gets one "1 in N" roll, and the one that fires last wins a contested slot.
static void endlessInjectRareSectors(void)
{
	for (unsigned k = 0; k < COUNTOF(endlessRareInjections); ++k)
	{
		const EndlessRareInjection *inj = &endlessRareInjections[k];
		if (endlessCourseCnt > 1 && (endlessRand() % endlessDangerRareDiv(inj->oneInN)) == 0)
		{
			const int slot = 1 + endlessRand() % (endlessCourseCnt - 1);
			endlessCourseMod[slot] = (inj->pool != NULL)
				? endlessPickThemeMods(inj->pool, inj->poolN, inj->must, inj->forbid)
				: inj->mods;
		}
	}
}

// Guarantee the offered courses are all distinct. The shuffle assigns distinct base themes, but
// the random-combo widening (and, rarely, an injection) can independently land two slots on the
// same modifier set, e.g. two "Fusillade"s (FRENZY|DEVASTATING). Re-roll any duplicate to an
// as-yet-unused hostile theme, so Chart-a-Course is always a real choice of different sectors.
// (Boon/signature slots never collide; their bits can't be produced by the widen.)
static void endlessDedupeCourseMods(const int *idx)
{
	for (int c = 1; c < endlessCourseCnt; ++c)
	{
		bool duplicate = false;
		for (int k = 0; k < c && !duplicate; ++k)
			if (endlessCourseMod[k] == endlessCourseMod[c])
				duplicate = true;
		if (!duplicate)
			continue;

		const Uint64 m = endlessUnusedHostileTheme(idx, c);
		if (m != 0)
			endlessCourseMod[c] = m;
	}
}

// JACKPOT: every course a pure boon. Deal DISTINCT boon themes (shuffle the table, take one per
// course), skipping the Cursed entries -- those read as Traps, not clean boons -- so the jackpot
// is all upside. Below the 25%-share unlock, skip the no-elite-tier boons too (they'd be
// near-empty this shallow), so the jackpot deals only themes that actually help here.
static void endlessDealJackpot(void)
{
	const Uint64 jackpotSkip = ENDLESS_MOD_CURSED
		| (endlessEliteBoonsUnlocked() ? 0 : (ENDLESS_MOD_NOCHAMP | ENDLESS_MOD_NOELITE));
	int bidx[COUNTOF(endlessBoonThemes)];
	int bn = 0;
	for (unsigned i = 0; i < COUNTOF(endlessBoonThemes); ++i)
		if ((endlessBoonThemes[i].mods & jackpotSkip) == 0)
			bidx[bn++] = (int)i;
	for (int i = bn - 1; i > 0; --i)
	{
		const int j = endlessRand() % (i + 1);
		const int t = bidx[i]; bidx[i] = bidx[j]; bidx[j] = t;
	}
	for (int c = 0; c < endlessCourseCnt && c < bn; ++c)
		endlessCourseMod[c] = endlessSwapTurbodriveOverblast(endlessBoonThemes[bidx[c]].mods);
}

// GAUNTLET: no Calm route and no boon -- turn every non-hostile course (the clean course 0 and any
// boon slot) into a fresh, distinct hostile theme. Courses already carrying a hostile bit
// (including a rare injected Apex / Kamikaze / Overload / etc.) keep their theme, so the gauntlet
// still fans out into varied dangers. Consumes no RNG, so the stream stays aligned.
static void endlessDealGauntlet(const int *idx)
{
	for (int c = 0; c < endlessCourseCnt; ++c)
	{
		if (endlessCourseMod[c] & ENDLESS_HOSTILE_MASK)
			continue;  // already a danger -- leave it be
		const Uint64 m = endlessUnusedHostileTheme(idx, c);
		if (m != 0)
			endlessCourseMod[c] = m;
	}
}

// AMBUSH: collapse the visit to one forced dangerous sector (keeping course 0's level). Kept fair
// -- homing rather than full kamikaze -- because the player gets no choice about flying it.
static void endlessDealAmbush(void)
{
	static const unsigned ambushCombos[] = {
		ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY,
		ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,
		ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT,
		ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,
		ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,
		ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,
		ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,
		ENDLESS_MOD_HOMING | ENDLESS_MOD_DEVASTATING,   // homing, not full kamikaze: an ambush is unavoidable, so keep it fair
		ENDLESS_MOD_HOMING | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,
		ENDLESS_MOD_GRAVITY | ENDLESS_MOD_SWIFT,
		ENDLESS_MOD_ENRAGE | ENDLESS_MOD_DEVASTATING,
		ENDLESS_MOD_OVERCLOCK | ENDLESS_MOD_DEVASTATING,
		ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING,
		ENDLESS_MOD_HOMING | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_DEVASTATING,
	};
	endlessCourseCnt = 1;  // collapse to a single sector (keeps course 0's level)
	endlessCourseMod[0] = ambushCombos[endlessRand() % COUNTOF(ambushCombos)];
}

// DIVERSE CHOICE: with a full slate (4-5 courses), make sure at least one hostile course is a SINGLE
// negative modifier -- a clean, legible "light" option beside the busy combos, so Chart-a-Course reads
// as a real spread of risk rather than a wall of multi-danger sectors. If none ended up a lone hostile
// bit, thin the mildest ORDINARY course (built only from the common combinable bits) down to just its
// least-nasty bit -- never touching a rare signature (Tar Pit / Overload / Apex / dead generator / ...)
// or a boon / clean route. Runs before the OMNI roll + sort, so the reduced course sorts to the easy end.
static void endlessEnsureLegibleChoice(void)
{
	if (endlessCourseCnt < 4)
		return;

	for (int c = 1; c < endlessCourseCnt; ++c)
	{
		const Uint64 h = endlessCourseMod[c] & ENDLESS_HOSTILE_MASK;
		if (h != 0 && (h & (h - 1)) == 0)   // exactly one hostile bit -- nothing to do
			return;
	}

	const Uint64 commonMask = endlessCombinableMask();
	int best = -1, bestBits = 99;
	for (int c = 1; c < endlessCourseCnt; ++c)
	{
		const Uint64 h = endlessCourseMod[c] & ENDLESS_HOSTILE_MASK;
		if (h == 0 || (h & ~commonMask) != 0)   // skip boon/clean routes and rare signature sectors
			continue;
		if (endlessCourseMod[c] & ENDLESS_BOON_MASK)  // don't flatten a mixed gambit into a plain single
			continue;
		int bits = 0;
		for (Uint64 x = h; x != 0; x &= x - 1)
			++bits;
		if (bits >= 2 && bits < bestBits) { bestBits = bits; best = c; }
	}
	if (best < 0)
		return;

	// Keep only the least-dangerous hostile bit (lowest reward) so it reads as the easy route.
	const Uint64 h = endlessCourseMod[best] & ENDLESS_HOSTILE_MASK;
	Uint64 keep = 0;
	int keepReward = 0x7fffffff;
	for (unsigned t = 0; t < COUNTOF(endlessModTable); ++t)
		if ((h & endlessModTable[t].bit) && endlessModTable[t].reward < keepReward)
		{
			keepReward = endlessModTable[t].reward;
			keep = endlessModTable[t].bit;
		}
	if (keep != 0)
		endlessCourseMod[best] = keep;
}

// MILESTONE SLATE: on a milestone zone the whole chart is replaced by five high-tier sectors. The
// minor (kind 3) milestone deals S/S+ and the plain (kind 1) one S+/S++, each split 2-and-3 with the
// seed deciding which rung gets the pair; a GRAND (kind 2) one has a FIXED shape -- one END course,
// two S+++ and two S++. The LEVELS gathered above are kept; only the mutator sets are re-dealt, so the
// slate is still five different sectors. Runs after every ordinary generation step (nothing above can
// survive into it) and before the OMNI roll / sort / naming, which finish it off like any other chart.
static void endlessDealMilestoneSlate(int milestone)
{
	const int lowRank = (milestone == 2) ? 8 : (milestone == 3) ? 6 : 7;  // S++ / S / S+  (see endlessDangerRankLevel)
	int lowN = 2 + (int)(endlessRand() % 2);       // minor/plain: 2 or 3 of the lower rung, the rest one higher
	if (milestone == 2)
		lowN = 2;                                  // grand: exactly 2 S++ (the roll above is still consumed,
		                                           // so the seed stream stays aligned with a plain milestone)
	if (lowN > endlessCourseCnt - 1)
		lowN = endlessCourseCnt - 1;               // short slate (too few distinct levels): keep both rungs present
	if (lowN < 1)
		lowN = 1;

	// A GRAND milestone always deals "The End" -- a run far enough to roll the credits ought to be
	// able to chart something by that name -- with its dangers rolled fresh for this milestone. It
	// is its OWN rank (END), not one of the two generated rungs, so the four remaining slots split
	// 2 S++ / 2 S+++ evenly. Its marker carries a 150 reward, so its danger score clears the 95
	// ceiling the builder tops S+++ courses out at by a mile: it is always the worst course offered
	// and the sort puts it last. Pinned into slot 0 so every later draw sees it in `used`; the slot
	// index itself is invisible, since that same sort re-orders the whole slate afterwards.
	int first = 0;
	if (milestone == 2)
	{
		endlessCourseMod[0] = endlessMakeTheEndMods();
		first = 1;
	}
	for (int c = first, lowLeft = lowN; c < endlessCourseCnt; ++c)
	{
		const int rank = (lowLeft > 0) ? lowRank : lowRank + 1;
		if (lowLeft > 0)
			--lowLeft;
		endlessCourseMod[c] = endlessMakeRankCombo(rank, endlessCourseMod, c);
	}
}

// GRAVITY WELL variant: whenever a course carries a gravity well (from any source above -- a named
// theme, the random-combo widen, a rare injection, or the ambush), flip a coin to make it
// OMNIDIRECTIONAL -- the pull runs along a fixed random heading for that sector (rolled per-sector
// in endlessRegenerateLevel) instead of straight down. Decided ONCE here, as the course is charted,
// so it's fixed for the seed and rides the save. Runs after every gravity-adding step, before the
// sort (OMNI is masked out of the danger score, so it doesn't disturb the ordering). Curated
// sectors -- "The End" included -- keep their names through it: endlessFindTheme strips the
// cosmetic bit and retries when no exact row matches.
static void endlessRollGravityVariants(void)
{
	for (int c = 0; c < endlessCourseCnt; ++c)
		if ((endlessCourseMod[c] & ENDLESS_MOD_GRAVITY) && (endlessRand() % 2))
			endlessCourseMod[c] |= ENDLESS_MOD_GRAVITY_OMNI;
}

// The elite-tier rules, applied to the finished chart. RNG-free, so where they sit among the
// phases doesn't disturb the seed stream -- only that they run after every mod-adding step and
// before the sort, so the danger ordering, tier word, name and reward all reflect the real sector.
static void endlessEnforceEliteRules(void)
{
	// Below the 25%-share unlock the no-elite-tier boons must not appear at all: scrub both bits from
	// every course as the final guarantee, in case any generation path above leaked one this shallow.
	// The pick sites already avoid emitting them here, so this normally does nothing.
	if (!endlessEliteBoonsUnlocked())
		for (int c = 0; c < endlessCourseCnt; ++c)
			endlessCourseMod[c] &= ~(Uint64)(ENDLESS_MOD_NOCHAMP | ENDLESS_MOD_NOELITE);

	// NOELITE (no elites or champions) supersedes NOCHAMP (no champions) -- the two must never ride one
	// sector, so strip the redundant NOCHAMP wherever both landed. The generators above never pair them on
	// purpose, but this makes the "can't have both" guarantee hold no matter how the bits were assembled.
	for (int c = 0; c < endlessCourseCnt; ++c)
		if ((endlessCourseMod[c] & ENDLESS_MOD_NOELITE) && (endlessCourseMod[c] & ENDLESS_MOD_NOCHAMP))
			endlessCourseMod[c] &= ~(Uint64)ENDLESS_MOD_NOCHAMP;

	// Deep runs: the natural elite share can climb past the 50% that "half enemies elite" (ELITEPACK)
	// pins it to, at which point ELITEPACK would CAP elites below the natural rate -- a stealth boon on
	// a danger course. Course 0 is normally clean, but an Ambush collapses its combo onto slot 0, so
	// scan from 0. (See endlessFixRedundantElitePack.)
	for (int c = 0; c < endlessCourseCnt; ++c)
		endlessFixRedundantElitePack(c);
}

// Present the courses from lowest danger to highest, so Chart-a-Course always reads as a left-
// to-right safety ramp (the clean/boon route first, the deadliest sector last). Sort the four
// parallel course arrays together by hostile danger score, the same metric that drives the
// Low/Moderate/.../NIGHTMARE tier word. A stable insertion sort over this tiny list keeps equal-
// danger courses in their generated order and consumes no RNG (it runs after every draw), so the
// seed's structure is unchanged. Ambush already collapsed to one course above, so it's a no-op there.
// Course i's danger score WITH its level's intrinsic baseDanger folded in -- the metric the chart
// sorts on (and the same one endlessCourseRank/Tier show), so a course backed by a harder level
// sorts rightward even when its modifier set is milder.
static int endlessCourseDangerScore(int i)
{
	return endlessDangerScoreEx(endlessCourseMod[i], endlessCourseBaseDanger(i));
}

static void endlessSortCoursesByDanger(void)
{
	for (int i = 1; i < endlessCourseCnt; ++i)
	{
		const int      ep   = endlessCourseEp[i];
		const JE_byte  sec  = endlessCourseSec[i];
		const JE_byte  file = endlessCourseFile[i];
		const Uint64   mod  = endlessCourseMod[i];
		// The insertion element is out of the arrays during the shift below, so score it from the
		// captured (mod, ep, file) rather than endlessCourseDangerScore(i).
		const int      key  = endlessDangerScoreEx(mod, endlessLevelBaseDanger(ep, file, difficultyLevel));
		int j = i - 1;
		while (j >= 0 && endlessCourseDangerScore(j) > key)
		{
			endlessCourseEp[j + 1]   = endlessCourseEp[j];
			endlessCourseSec[j + 1]  = endlessCourseSec[j];
			endlessCourseFile[j + 1] = endlessCourseFile[j];
			endlessCourseMod[j + 1]  = endlessCourseMod[j];
			--j;
		}
		endlessCourseEp[j + 1]   = ep;
		endlessCourseSec[j + 1]  = sec;
		endlessCourseFile[j + 1] = file;
		endlessCourseMod[j + 1]  = mod;
	}
}

// UNIQUE NAMES: the offered modifier sets are distinct by now, but two different un-curated
// bitsets can still HASH to the same generated word (two "Toss-Up"s on one chart). Bump the
// later course's name-salt until every offered label is unique. RNG-free and deterministic,
// so a reloaded outpost re-derives the same names; only generated names ever need a salt
// (curated names ignore it and never clash -- the theme tables hold no duplicates).
static void endlessMakeCourseNamesUnique(void)
{
	for (int c = 0; c < endlessCourseCnt; ++c)
		endlessCourseNameSalt[c] = 0;
	for (int c = 1; c < endlessCourseCnt; ++c)
	{
		for (int guard = 0; guard < 64; ++guard)
		{
			const char *name = endlessComboNameSalted(endlessCourseMod[c], endlessCourseNameSalt[c]);
			bool clash = false;
			for (int k = 0; k < c && !clash; ++k)
				if (strcmp(name, endlessComboNameSalted(endlessCourseMod[k], endlessCourseNameSalt[k])) == 0)
					clash = true;
			if (!clash)
				break;
			++endlessCourseNameSalt[c];
		}
	}
}

// Cache the authored base-level name behind each finalized course, so the Radar perk's per-frame
// help line (endlessCourseHelp) reads a string instead of re-parsing levels*.dat every frame. Runs
// after the danger sort (so index i matches the displayed order) and after any course restore, off
// the final (episode, section, file) each course actually launches. Empty for unused slots.
void endlessNameCourseBaseLevels(void)
{
	for (int i = 0; i < endlessCourseCnt && i < ENDLESS_MAX_COURSES; ++i)
		JE_getLevelSectionName(endlessCourseEp[i], endlessCourseSec[i], endlessCourseFile[i],
		                       endlessCourseBaseName[i], sizeof endlessCourseBaseName[i]);
	for (int i = endlessCourseCnt; i < ENDLESS_MAX_COURSES; ++i)
		endlessCourseBaseName[i][0] = '\0';
}

void endlessGenerateCourses(void)
{
	endlessCourseCnt = 0;

	const int milestone = endlessMilestoneKind();  // 0 ordinary zone, 1 S+/S++ slate, 2 S++/S+++ slate

	// This visit offers a random 2..5 course choices (fewer only if there aren't enough
	// distinct safe levels to fill them); a milestone always asks for the full slate.
	int wantCourses = 2 + (int)(endlessRand() % (ENDLESS_MAX_COURSES - 1));  // 2..5
	if (milestone)
		wantCourses = ENDLESS_MAX_COURSES;
	endlessGatherCourseLevels(wantCourses);

	int idx[COUNTOF(endlessHostileThemes)];
	endlessShuffleThemeOrder(idx);

	const int dangerRamp = endlessDangerRamp();  // 0 (z40) -> 100 (z100) -> 350 (z250 cap) -- deep-run danger tilt

	endlessDealHostileThemes(idx);
	endlessWidenHostileCombos(dangerRamp);
	endlessDealBoonCourse(dangerRamp);
	endlessGraftGambits(dangerRamp);
	endlessInjectRareSectors();
	endlessDedupeCourseMods(idx);

	// --- Rare whole-visit flavors: Jackpot / Gauntlet / Ambush (mutually exclusive) ----------
	// Jackpot (~1/22) all boons; Ambush (~1/20) one forced dangerous sector; Gauntlet (~1/7)
	// all hostile. All three dice roll up front UNCONDITIONALLY so the seed stream stays
	// aligned; precedence Jackpot > Ambush > Gauntlet; none fire at depth 0. The danger ramp tilts
	// the odds deep: the danger-only Gauntlet/Ambush grow more common while the all-boon Jackpot thins.
	// Gauntlet/Ambush use a percentage form HARD-CAPPED below certainty (the CAP_PCT knobs), so even at
	// the ramp cap a large share of visits still offer a calm route -- danger is never a sure thing.
	int gauntletPct = 14 + dangerRamp * 11 / 100;  // ~1/7 early -> 25% by the mid -> capped
	if (gauntletPct > ENDLESS_DANGER_GAUNTLET_CAP_PCT)
		gauntletPct = ENDLESS_DANGER_GAUNTLET_CAP_PCT;
	int ambushPct = 5 + dangerRamp * 4 / 100;      // ~1/20 early -> ~9% by the mid -> capped
	if (ambushPct > ENDLESS_DANGER_AMBUSH_CAP_PCT)
		ambushPct = ENDLESS_DANGER_AMBUSH_CAP_PCT;
	const bool jackpotRoll  = ((endlessRand() % (22 + dangerRamp * 22 / 100)) == 0);  // ~1/22 -> 1/44 (mid) -> ~1/99 (cap) -- small bump to offset the extra milestone danger zones
	const bool gauntletRoll = ((int)(endlessRand() % 100) < gauntletPct);
	const bool ambushRoll   = ((int)(endlessRand() % 100) < ambushPct);
	// A milestone zone deals its own fixed slate below, so none of the three may fire there (an
	// Ambush would collapse the visit to one course). The dice are still rolled above, so the seed
	// stream stays aligned whether or not this zone is a milestone.
	const bool doJackpot  = jackpotRoll && (endlessRunDepth > 0) && !milestone;
	const bool doAmbush   = !doJackpot && ambushRoll && (endlessRunDepth > 0) && !milestone;
	const bool doGauntlet = !doJackpot && !doAmbush && gauntletRoll && (endlessRunDepth > 0) && !milestone;

	if (doJackpot)
		endlessDealJackpot();
	else if (doGauntlet)
		endlessDealGauntlet(idx);

	endlessForced = doAmbush;
	if (endlessForced)
		endlessDealAmbush();

	endlessEnsureLegibleChoice();
	if (milestone)
		endlessDealMilestoneSlate(milestone);

	endlessRollGravityVariants();
	endlessEnforceEliteRules();
	endlessSortCoursesByDanger();
	endlessMakeCourseNamesUnique();
	endlessNameCourseBaseLevels();  // cache each course's base-level name for the Radar perk (after the sort)
}

// Resolve a saved/chosen (episode, section) back to a real endless-safe level file. Prefer the
// exact persisted file when present; v7 and older saves only have the section, so use its first
// safe match. Returning false means the script entry has no corresponding binary level data.
bool endlessResolveCourseFile(int ep, JE_byte sec, JE_byte requestedFile, JE_byte *resolvedFile)
{
	JE_byte secs[64], files[64];
	const uint n = JE_getLevelSections(ep, secs, files, COUNTOF(secs));
	JE_byte firstMatch = 0;
	for (uint i = 0; i < n; ++i)
	{
		if (secs[i] != sec)
			continue;
		if (firstMatch == 0)
			firstMatch = files[i];
		if (requestedFile != 0 && files[i] == requestedFile)
		{
			*resolvedFile = files[i];
			return true;
		}
	}
	if (firstMatch == 0)
		return false;
	*resolvedFile = firstMatch;
	return true;
}

int endlessCourseCount(void)
{
	return endlessCourseCnt;
}

const char *endlessCourseName(int i)
{
	if (i < 0 || i >= endlessCourseCnt)
		return "";
	if (endlessForced && i == 0)
		return "Ambush!";
	return endlessComboNameSalted(endlessCourseMod[i], endlessCourseNameSalt[i]);
}

// The help line is a short RISK SUMMARY only -- the itemized threats/boons are drawn on the
// planet monitor itself (endlessCourseModRows + game_menu.c's overlay), so the old generated
// "A, B and more" listing would just duplicate them.
const char *endlessCourseHelp(int i)
{
	static char buf[80];
	if (i < 0 || i >= endlessCourseCnt)
		return "";
	if (endlessForced && i == 0)
	{
		snprintf(buf, sizeof buf, "Ambush! %s - no way around it",
		         endlessDangerTierEx(endlessCourseMod[0], endlessCourseBaseDanger(0)));
	}
	else
	{
		const Uint64 mods = endlessCourseMod[i];
		const int    bd   = endlessCourseBaseDanger(i);   // the shipped level's own danger (hostile courses only)
		// The letter rank (F easiest .. S+++ hardest) is drawn separately, on the planet monitor's
		// RANK field (endlessCourseRank + game_menu.c's overlay), so the help line is just the tier.
		// A clean/boon course always reads Calm/Boon (its score ignores baseDanger, so the safe route
		// is never demoted); the level's danger surfaces in its PAYOUT instead. bd only bites on the
		// hostile branch below, where it lifts the tier to match the shipped stage.
		if (mods & ENDLESS_MOD_CURSED)
			snprintf(buf, sizeof buf, "Trap: rich now, barren shop next");
		else if (endlessDangerScoreEx(mods, bd) == 0)
			snprintf(buf, sizeof buf, "%s", (mods == 0) ? "Calm: clear skies ahead" : "Boon: no danger here");
		else
			snprintf(buf, sizeof buf, "Danger: %s", endlessDangerTierEx(mods, bd));
	}
	// Radar perk: reveal the shipped level behind this sector, right after the tier read.
	if (endlessPerkRadarActive() && endlessCourseBaseName[i][0] != '\0')
	{
		const size_t len = strlen(buf);
		snprintf(buf + len, sizeof buf - len, " (%s)", endlessCourseBaseName[i]);
	}
	return buf;
}

// The highlighted course's letter danger grade (F easiest .. S+++ hardest) for the monitor's
// RANK field -- moved off the help line so it reads ON the planet monitor. Delegates to the
// same endlessDangerRank/score/thresholds as the "Danger:" word, so the two never disagree.
const char *endlessCourseRank(int i)
{
	if (i < 0 || i >= endlessCourseCnt)
		return "";
	return endlessDangerRankEx(endlessCourseMod[i], endlessCourseBaseDanger(i));
}

// Numeric danger level 0 (F) .. 9 (S+++) for course i, or -1 if out of range. The monitor uses
// it to pick the letter's green->red tint; it maps the same letter endlessCourseRank returns.
int endlessCourseRankLevel(int i)
{
	if (i < 0 || i >= endlessCourseCnt)
		return -1;
	return endlessDangerRankLevelEx(endlessCourseMod[i], endlessCourseBaseDanger(i));
}

// The highlighted course's individual modifiers for the monitor overlay, worst-first. The
// weight is the modifier's endlessModTable reward (the same tenths endlessDangerScore sums),
// so the overlay's darkest-red tints land on exactly the bits that drive the tier word.
// Cursed lists on the hostile side -- it's a trap, and deep red is the right warning.
int endlessCourseModRows(int i, EndlessCourseModRow *rows, int max)
{
	if (i < 0 || i >= endlessCourseCnt)
		return 0;
	const Uint64 mods = endlessCourseMod[i];
	int n = 0;
	for (unsigned t = 0; t < COUNTOF(endlessModTable) && n < max; ++t)
	{
		if ((mods & endlessModTable[t].bit) == 0)
			continue;
		if (endlessModTable[t].word == NULL)
			continue;  // label-only bit (the finale marker): it pays and ranks, but lists no threat
		rows[n].word    = endlessModTable[t].word;
		rows[n].weight  = endlessModTable[t].reward;
		rows[n].hostile = (endlessModTable[t].bit & (ENDLESS_HOSTILE_MASK | ENDLESS_MOD_CURSED)) != 0;
		// An omnidirectional well doesn't pull DOWN, so relabel the gravity row to say so (OMNI has no
		// row of its own -- it rides the gravity bit and only changes the pull's direction).
		if (endlessModTable[t].bit == ENDLESS_MOD_GRAVITY && (mods & ENDLESS_MOD_GRAVITY_OMNI))
			rows[n].word = "pull any direction";
		++n;
	}
	// Overclock/Overload ALSO speed the scroll, but their table row only claims the enemy attacks --
	// list the scroll effect as its OWN red row (same words as Slipstream/Warp), so the two effects
	// never blur back into one ambiguous "scroll + fire" phrase. Display-only: the course's danger
	// score/payout still come from the single bit. Skipped when a real scroll bit already supplies
	// the row (no such generated combo exists; belt-and-braces for hand-built sets).
	if (n < max && (mods & ENDLESS_MOD_OVERLOAD) && !(mods & ENDLESS_MOD_WARP))
	{
		rows[n].word    = "much faster scrolling";
		rows[n].weight  = 14;  // sorts right under Overload's 15; same deep-red band as Warp's 20
		rows[n].hostile = true;
		++n;
	}
	else if (n < max && (mods & ENDLESS_MOD_OVERCLOCK) && !(mods & (ENDLESS_MOD_SLIPSTREAM | ENDLESS_MOD_WARP)))
	{
		rows[n].word    = "faster scrolling";
		rows[n].weight  = 9;  // sorts right under Overclock's 10; same pale-red band as Slipstream's 6
		rows[n].hostile = true;
		++n;
	}
	for (int a = 1; a < n; ++a)  // insertion sort, worst first; n is tiny
	{
		const EndlessCourseModRow key = rows[a];
		int b = a - 1;
		while (b >= 0 && rows[b].weight < key.weight)
		{
			rows[b + 1] = rows[b];
			--b;
		}
		rows[b + 1] = key;
	}
	return n;
}

JE_byte endlessCoursePlanet(int i)
{
	// Distinct valid star-map planets (1..21) so the monitor shows a different world per
	// course. Purely cosmetic.
	static const JE_byte planets[ENDLESS_MAX_COURSES] = { 4, 9, 13, 17, 21 };
	return planets[(i < 0 || i >= ENDLESS_MAX_COURSES) ? 0 : i];
}

JE_byte endlessCourseSection(int i)
{
	if (i < 0 || i >= endlessCourseCnt)
		i = 0;
	return endlessCourseSec[i];
}

JE_byte endlessSelectCourse(int i)
{
	if (i < 0 || i >= endlessCourseCnt)
		i = 0;

	// Revalidate at the launch boundary too. This is the last line of defense for legacy saves or
	// externally edited/corrupt sidecars: substitute a safe level instead of handing JE_loadMap an
	// invalid section and terminating the game.
	JE_byte resolvedFile;
	if (endlessResolveCourseFile(endlessCourseEp[i], endlessCourseSec[i], endlessCourseFile[i], &resolvedFile))
	{
		endlessCourseFile[i] = resolvedFile;
	}
	else
	{
		int fallbackEp;
		JE_byte fallbackSec, fallbackFile;
		if (!endlessRandomSafeLevel(&fallbackEp, &fallbackSec, &fallbackFile))
		{
			fprintf(stderr, "error: no valid Endless level is available\n");
			forcedLvlFileNum = 0;
			return FIRST_LEVEL;
		}
		fprintf(stderr, "warning: replacing invalid saved course episode %d section %u\n",
		        endlessCourseEp[i], (unsigned int)endlessCourseSec[i]);
		endlessCourseEp[i] = fallbackEp;
		endlessCourseSec[i] = fallbackSec;
		endlessCourseFile[i] = fallbackFile;
	}

	// Stash the one-shots this pick is about to consume, so a non-hardcore mid-zone bail (which
	// reopens the outpost unlocked and re-picks a course through here) can restore them first;
	// otherwise a bought E-Shop buff would be silently forfeit. Hardcore relaunches the committed
	// level directly, never re-entering this, so its stash is simply never read (see
	// endlessRestoreSortie).
	endlessSortiePrePurchased = endlessPurchasedMods;
	endlessSortiePreCleanse   = endlessCleanseChargeCount;
	endlessSortiePreLongCon   = endlessLongCon;

	if (endlessCourseEp[i] != episodeNum)
		JE_initEpisode(endlessCourseEp[i]);  // load that episode's data (arsenal is shared)
	forcedLvlFileNum = endlessCourseFile[i];  // load this course's exact level file (see JE_loadMap)
	endlessActiveMods = endlessCourseMod[i] | endlessPurchasedMods;  // fold in E-Shop buffs
	if ((endlessActiveMods & ENDLESS_MOD_NOELITE) && (endlessActiveMods & ENDLESS_MOD_NOCHAMP))
		endlessActiveMods &= ~(Uint64)ENDLESS_MOD_NOCHAMP;          // NOELITE supersedes NOCHAMP -- never both at once
	endlessPurchasedMods = 0;                                        // consumed by this sector
	for (int c = 0; c < endlessCleanseChargeCount; ++c)  // Sabotage: strip the worst hostile bit per charge
		endlessActiveMods = endlessStripWorstMod(endlessActiveMods);
	endlessCleanseChargeCount = 0;
	// The Long Con: an APEX ambush the player gambled for and forgot, added after the cleanse pass so
	// no queued sabotage charge can strip it -- you paid to not see this coming, and you don't.
	if (endlessLongCon > 0 && --endlessLongCon == 0)
		endlessActiveMods |= ENDLESS_MOD_APEX;
	endlessLastEp = endlessCourseEp[i];
	endlessLastSec = endlessCourseSec[i];
	return endlessCourseSec[i];
}
