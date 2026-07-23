/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 *
 * Endless mode, PRIVATE interface.
 *
 * endless.h is what the rest of the game calls. This header is the contract
 * between the endless_*.c files themselves: the run state each of them owns and
 * the helpers they lend one another. Nothing outside endless_*.c includes it.
 *
 * Who owns what:
 *   endless.c         run state, lifecycle, zone milestones, the run-end screen
 *   endless_rng.c     the run seed and the structural RNG
 *   endless_level.c   which shipped level a zone plays, its music, its reroll
 *   endless_combat.c  enemy scaling, elites, specials, player-side modifiers
 *   endless_perks.c   perks: the run-persistent stacking upgrades
 *   endless_shop.c    the outpost: stock, prices, the E-Shop buys, the gamble
 *   endless_mods.c    the mutator table: sector names, danger tiers, help text
 *   endless_course.c  Chart-a-Course: generating and committing the next sector
 *   endless_save.c    the endless.sav sidecar and the Quit Level sortie snapshot
 *
 * Adding state? Define it in the .c that owns it and declare it here only if
 * another one of these files genuinely needs it -- endless_save.c usually does,
 * since it serializes the run.
 */
#ifndef ENDLESS_INTERNAL_H
#define ENDLESS_INTERNAL_H

#include "endless.h"

// Clamp v into [lo, hi]. Every scaled lever in endless mode ends in one of these, so it is worth
// a name: the clamp is the tuning knob that decides WHEN a lever stops growing (opentyr.h's
// MIN/MAX are macros and would evaluate their arguments twice).
static inline int endlessClamp(int v, int lo, int hi)
{
	return (v < lo) ? lo : (v > hi) ? hi : v;
}

// --- endless_rng.c: run seed & structural RNG -----------------------------------
// Structure (level order, mutators, perks, shop stock) draws from a dedicated
// SplitMix64 stream, isolated from the shared gameplay mt_rand.
extern char   endlessRunSeed[ENDLESS_SEED_MAXLEN];  // the run's seed string (also shown in-game)
extern Uint64 endlessEliteRngState;                 // live state of the seeded elite/champion tier stream

Uint32 endlessRand(void);                  // the structural random -- a drop-in for mt_rand() at structural sites
void   endlessReseed(Uint64 salt);         // re-derive the structural stream for a fresh (zone, phase)
Uint64 endlessSplitMixSeed(Uint64 salt);   // derive a stream state for a (zone, phase) without installing it
Uint32 endlessEliteRand(void);             // the elite/champion tier random (its own stream)

// --- endless.c: per-zone timers --------------------------------------------------
#define ENDLESS_TURBODRIVE_TICKS 70   // ~2s of boosted fire per kill (TURBODRIVE)
extern int endlessZoneTicks;          // ticks elapsed this zone (drives ENRAGE)
extern int endlessTurbodriveTimer;    // ticks left in the quickened-fire window after a kill

// --- endless.c: zone milestones --------------------------------------------------
// The outpost BEFORE a milestone swaps its buy/sell music for this track, so the player is warned
// that a set-piece is coming while they're still choosing a course. MIND THE TWO FORMS: songBuy is
// played as-is (play_song(songBuy)), while levelSong is 1-based (play_song(levelSong - 1)), so the
// same track carries two numbers -- keep the pair in step if it's ever retuned.
#define ENDLESS_MILESTONE_SHOP_SONG     26  // "Parlance", songBuy form (0-based)
#define ENDLESS_MILESTONE_SHOP_SONG_LVL 27  // "Parlance", levelSong form (1-based)

int     endlessMilestoneKindOfZone(int zone);  // 0 ordinary, 1 the S+/S++ milestone, 2 the GRAND one
int     endlessMilestoneKind(void);            // ...for the zone about to be charted / played
JE_byte endlessMilestoneSong(int kind);        // the pinned track for a milestone class, 0 otherwise
bool    endlessPerkDueAtDepth(int depth);      // is a forced perk pick due at the outpost for this depth?

// --- endless_level.c: the shipped level behind each zone -------------------------
#define ENDLESS_LEVEL_HISTORY 5    // how many recently-played levels the anti-repeat ring tracks

extern JE_byte endlessLastSong;       // track the last-played zone used; 0 = nothing played yet this run
extern int     endlessLastSongDepth;  // the run depth it was picked for; -1 = none

// Base (shipped) level each zone is built on, and the one before it (crash-log history).
extern char endlessBaseName[11];
extern int  endlessBaseEp;
extern int  endlessBaseLvl;
extern char endlessPrevBaseName[11];
extern int  endlessPrevBaseEp;
extern int  endlessPrevBaseLvl;

extern int     endlessRecentEp[ENDLESS_LEVEL_HISTORY];   // anti-repeat ring, newest first
extern JE_byte endlessRecentSec[ENDLESS_LEVEL_HISTORY];
extern int     endlessRecentCount;                       // valid entries, 0..ENDLESS_LEVEL_HISTORY

// Random endless-safe (ep, section, file) from any installed episode, avoiding the recently-played
// levels; the file distinguishes the two Ep1-section-3 TYRIAN cuts. fileOut may be NULL.
bool endlessRandomSafeLevel(int *epOut, JE_byte *secOut, JE_byte *fileOut);

// --- endless_combat.c: kill-fire combo, scaling, gravity -------------------------
extern int  endlessComboKills;          // +1 per kill while a kill-fire window is up, reset when it lapses
extern char endlessLastSpecialName[31]; // name of the last special weapon endlessGrantSpecial handed out

int  endlessDifficultyZone(void);              // the current zone as the difficulty ramp sees it
int  endlessNaturalEliteChancePercent(void);   // the depth-driven SPECIAL-enemy share, before mutators
bool endlessEliteBoonsUnlocked(void);          // are NOCHAMP / NOELITE eligible to be charted yet?
void endlessRollGravityDir(void);              // pick this sector's gravity heading (called at zone start)

// --- endless_perks.c: run-persistent, stacking upgrades --------------------------
// Free pick-1-of-3 after each cleared zone; each effect folds into an existing player-side
// lever so there's no new subsystem. Reset each run. Tunables below are all by-eye.
#define ENDLESS_PERK_DAMAGE_PCT    12  // +% shot damage per Heavy Rounds stack
#define ENDLESS_PERK_FIRE_PCT      20  // fire-decrement accumulator % per Rapid Cyclers stack
#define ENDLESS_PERK_ARMOR_STEP     8  // +max armor per Ablative Plating stack
#define ENDLESS_PERK_CASH_PCT      15  // +% cash (clears + bounties) per Scavenger stack
#define ENDLESS_PERK_REGEN_TICKS  140  // ticks per +1 armor at 1 Nanorepair stack (faster with more)
#define ENDLESS_PERK_SIPHON_PCT    12  // heal-on-kill chance % per Siphon stack
#define ENDLESS_PERK_BULWARK        1  // incoming damage reduced by this per Bulwark stack (min 1 dmg kept)
#define ENDLESS_PERK_ADRENALINE_PCT 45 // extra fire-accumulator % per Adrenaline stack while badly hurt
#define ENDLESS_PERK_ADRENALINE_HP  3  // Adrenaline triggers when armor < 1/this of max
#define ENDLESS_PERK_GLASS_DMG     40  // Glass Cannon: +% shot damage
#define ENDLESS_PERK_GLASS_ARMOR    8  // Glass Cannon: -max armor (the drawback)
#define ENDLESS_PERK_SPECIALCD_PCT 25  // extra special-cooldown-decrement accumulator % per stack
#define ENDLESS_PERK_POWERUSE_PCT  15  // -% generator power drawn per main-weapon shot, per Efficient Coils stack
#define ENDLESS_PERK_POWERUSE_MIN  20  // ...but firing never costs less than this % of stock power
#define ENDLESS_PERK_SHIELDRGN_STEP 3  // shield-regen interval cut by this many ticks per Shield Matrix stack (base 15)
#define ENDLESS_PERK_SHIELDRGN_MIN  3  // ...but never quicker than +1 shield per this many ticks (floor)
#define ENDLESS_PERK_CHARGE_STEP    4  // ticks cut from the charge-sidekick charge interval per Rapid Charger stack (base 20)
#define ENDLESS_PERK_CHARGE_MIN     4  // ...but a charge level never builds quicker than this many ticks (floor)
#define ENDLESS_PERK_SHOTSPEED_PCT 25  // +% shot travel speed per High-Velocity Rounds stack

// "Buy Extra Perk" (E-Shop) surcharge: every perk stack the player already holds adds this % to the
// extra-perk price, capped, on top of the base depth price + per-visit doubling. So the deeper the
// perk collection, the pricier it gets to grow it further (perks are strong and bounded).
#define ENDLESS_EXTRA_PERK_OWNED_PCT  40   // +% per owned perk stack
#define ENDLESS_EXTRA_PERK_OWNED_CAP 1000  // ...but the owned-count surcharge tops out here (+1000% = x11)

enum {
	PERK_DAMAGE, PERK_FIRERATE, PERK_ARMOR, PERK_CASH,
	PERK_REGEN, PERK_SIPHON, PERK_BOUNTY,
	PERK_BULWARK, PERK_ADRENALINE, PERK_GLASSCANNON,  // relic-like
	PERK_SPECIALCD,
	PERK_AUTOSPECIAL,
	PERK_POWERUSE,
	PERK_SHIELDREGEN,
	PERK_CHARGERATE,
	PERK_SHOTSPEED,    // append new perks here; the index is the on-disk save slot, so don't renumber
	PERK_COUNT
};

typedef struct {
	const char *name;      // menu label (<= 23 chars, menuInt width)
	const char *desc;      // help-line description
	JE_byte     maxStack;  // how many times it can be taken
} EndlessPerk;

extern const EndlessPerk endlessPerkTable[PERK_COUNT];
extern JE_byte endlessPerkOwned[PERK_COUNT];  // stack counts, reset each run
extern int endlessPerkChoice[3];              // this visit's offered perk ids
extern int endlessPerkChoiceN;                // how many are offered (0..3)
extern int endlessRegenTick;                  // Nanorepair countdown (reset each run)
extern int endlessPerkDepthDone;              // run depth whose perk pick is already resolved; -1 = none

int endlessPerkCashPercent(void);             // Scavenger cash multiplier (100 = unchanged)

// --- endless_shop.c: the outpost -------------------------------------------------
extern long endlessRerollCost;        // escalating outpost prices, reset each visit
extern int  endlessHullCost;
extern long endlessShopEntryCash;     // cash on entering the shop -- the E-Shop cash-fraction buys price off this

// E-Shop kill-fire buff: bought once per shop visit, its modifier bits stashed here and OR'd into
// the next sector's mods at course selection, so they survive the choice that overwrites the mods.
extern unsigned endlessPurchasedMods;
extern int endlessBuffKind;           // which buff was bought: 0 none, 1 Turbodrive, 2 Overdrive
extern int endlessOverdriveStacks;    // +1 per kill while the window is up (capped), reset when it lapses
extern int endlessBuffCooldownUntil;  // run depth at which the kill-fire buys unlock again (0 = no lock)
extern int endlessBuffCharge;         // cash-paid tier that scales the window/damage (0..20)

int endlessBuffWindowTicks(void);     // base kill-fire window, extended by charge (up to ~2.5x)

extern bool endlessReviveHeld;          // a held revive token survives one lethal hit
extern int  endlessRevivesUsed;         // revives spent this run (the price doubles per use)
extern int  endlessCleanseChargeCount;  // pre-bought strips of the worst mutator off the next course
extern long endlessBombCost, endlessExtraPerkCost, endlessCleanseCost;
extern char endlessGambleMsg[48];       // last gamble outcome, for the E-Shop help line
extern bool endlessGamblePerkWon;       // a gamble handed out a free perk pick; the dispatch opens MENU_PERKS
extern int  endlessShopTax;             // Loan Shark: permanent +% on every shop price for the rest of the run
extern bool endlessGambleRigged;        // Rigged: the NEXT gamble secretly rolls twice and keeps the worse result
extern int  endlessLongCon;             // The Long Con: sectors until a paid-and-forgotten APEX ambush comes due
extern bool endlessResumeVisit;         // a save was just loaded: the next outpost restores its snapshot
extern bool endlessCreditsShown;        // the zone-100 credits roll already played this run (rides the save)

long   endlessClearBonusFor(Uint64 mods);   // clear payout for an ARBITRARY modifier set at the current depth
Uint64 endlessStripWorstMod(Uint64 mods);   // strip the single most-dangerous hostile bit (one per cleanse charge)

// --- endless_mods.c: the mutator registry ----------------------------------------
// Payout/help registry: per-bit clear-cash `reward` (tenths of the base) + monitor `word`.
// Generation lives in endlessGenerateCourses; every bit a course can carry is listed here.
typedef struct {
	Uint64      bit;
	short       reward;    // clear-cash reward in TENTHS of the base (10 = 1.0x base; may be < 0)
	const char *word;      // short phrase for the generated help line
} EndlessMod;

// Named course themes: a NAME dictionary of evocative labels for combos worth naming
// (endlessComboNameSalted looks up an exact bit-set here; anything unlisted gets a generated name).
// Generation and payout are driven by endlessModTable, not these rows -- so this is purely
// cosmetic naming, free to extend or trim without touching behaviour.
typedef struct { Uint64 mods; const char *name; } EndlessTheme;

// The row counts below are part of the contract: COUNTOF() at the call sites in other endless_*.c
// files reads them from here. Grow a table without bumping its number and endless_mods.c fails to
// compile ("too many initializers"), so the pair cannot silently drift.
extern const EndlessMod   endlessModTable[36];
extern const EndlessTheme endlessHostileThemes[147];
extern const EndlessTheme endlessKamikazeThemes[12];
extern const EndlessTheme endlessHomingThemes[8];
extern const EndlessTheme endlessBoonThemes[43];
extern const EndlessTheme endlessOverloadThemes[20];
extern const EndlessTheme endlessRareThemes[44];
extern const EndlessTheme endlessEvilThemes[30];
extern const EndlessTheme endlessRedlineThemes[2];
extern const EndlessTheme endlessSluggishThemes[5];
extern const EndlessTheme endlessDeadgenThemes[5];

// Bits that make a sector DANGEROUS. The danger score sums only these, so a pure-boon course -- e.g.
// Bounty, which pays big but adds no danger -- never reads as a high tier. Cursed is handled apart
// (it's a trap, not a danger). The type-aware naming classifies a combo as hostile / boon / mixed
// off these masks.
#define ENDLESS_HOSTILE_MASK ( \
	ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING | \
	ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_APEX | ENDLESS_MOD_LEGION | ENDLESS_MOD_ENRAGE | \
	ENDLESS_MOD_KAMIKAZE | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_OVERCLOCK | ENDLESS_MOD_OVERLOAD | \
	ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_BURNOUT | ENDLESS_MOD_MISFIRE | ENDLESS_MOD_OVERHEAT | \
	ENDLESS_MOD_HOMING | ENDLESS_MOD_RAMPAGE | ENDLESS_MOD_TOPSY | ENDLESS_MOD_SLUGGISH | \
	ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_DEADGEN | ENDLESS_MOD_SLIPSTREAM | ENDLESS_MOD_WARP | \
	ENDLESS_MOD_THEEND )

// Bits that HELP you -- the boon side. A course carrying any of these plus a hostile bit is a "mixed"
// gambit (risk + reward). Faster scrolling (SLIPSTREAM/WARP) counts HOSTILE -- the level rushing at
// you cuts reaction time; the evil kill-fire mirrors are hostile (above), and CURSED is a trap
// counted on the hostile side, so none of those belong here.
#define ENDLESS_BOON_MASK ( \
	ENDLESS_MOD_FRAGILE | ENDLESS_MOD_BOUNTY | ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_OVERCHARGE | \
	ENDLESS_MOD_DILATION | ENDLESS_MOD_FAVOR | ENDLESS_MOD_OVERDRIVE | \
	ENDLESS_MOD_OVERBLAST | ENDLESS_MOD_NOCHAMP | ENDLESS_MOD_NOELITE )

Uint64      endlessMakeTheEndMods(void);   // "The End" -- the sector every GRAND milestone deals
Uint64      endlessPickThemeMods(const EndlessTheme *tbl, unsigned count, Uint64 must, Uint64 forbid);
const char *endlessComboNameSalted(Uint64 mods, unsigned salt);  // salt steps a GENERATED pick to the next word
int         endlessDangerScore(Uint64 mods);      // net danger: summed hostile reward, minus boon credits
const char *endlessDangerTier(Uint64 mods);       // tier word shown before a course's description
int         endlessDangerRankLevel(Uint64 mods);  // 0 (F) .. 10 (END)
const char *endlessDangerRank(Uint64 mods);       // the letter grade for that level

// --- endless_course.c: Chart-a-Course --------------------------------------------
#define ENDLESS_MAX_COURSES 5

extern int      endlessCourseCnt;
extern int      endlessCourseEp[ENDLESS_MAX_COURSES];
extern JE_byte  endlessCourseSec[ENDLESS_MAX_COURSES];
extern JE_byte  endlessCourseFile[ENDLESS_MAX_COURSES];  // each course's specific lvlFileNum (see forcedLvlFileNum)
extern Uint64   endlessCourseMod[ENDLESS_MAX_COURSES];
extern int      endlessLastEp;
extern JE_byte  endlessLastSec;
extern bool     endlessForced;   // this visit is a forced "Ambush" (single dangerous sector)

// Resolve a saved/chosen (episode, section) back to a real endless-safe level file.
bool endlessResolveCourseFile(int ep, JE_byte sec, JE_byte requestedFile, JE_byte *resolvedFile);

// --- endless_save.c: the Quit Level sortie snapshot ------------------------------
extern bool     endlessSortieHave;         // a launch-time snapshot exists
extern unsigned endlessSortiePrePurchased; // one-shots snapshotted pre-consumption at the course pick,
extern int      endlessSortiePreCleanse;   // so a non-hardcore bail can restore them
extern int      endlessSortiePreLongCon;

#endif // ENDLESS_INTERNAL_H
