/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 *
 * Endless mode — see endless.h.
 *
 * The run plays real, UNMODIFIED shipped levels in a random cross-episode order.
 * Between levels the player visits an OUTPOST (bank interest, reroll the shop, buy
 * hull upgrades, then the standard item shop) and CHARTS A COURSE: a branching
 * choice of the next sector, each option carrying its own risk/reward MUTATORS.
 * Difficulty ramps through depth-scaled enemy stats (HP / boss HP / fire rate /
 * projectile speed) plus whatever mutators the chosen sector adds. The run ends on
 * death; only the depth reached (a high score) persists.
 */

#include "endless.h"

#include "config.h"        // difficultyLevel, DIFFICULTY_*, player-independent globals
#include "custom_weapon.h" // customWeaponPort / customSidekickSlot (reserved shop slots)
#include "episodes.h"      // item arrays + SHIP_NUM/PORT_NUM/... counts, episodeAvail, JE_initEpisode
#include "file.h"          // dir_fopen / dir_fopen_warn (endless.sav sidecar I/O)
#include "font.h"          // draw_font_hv_shadow, JE_textWidth, fonts, alignment
#include "fonthand.h"      // JE_outText
#include "game_menu.h"     // JE_itemScreen, JE_getLevelSections
#include "joystick.h"      // push_joysticks_as_keyboard
#include "keyboard.h"      // newkey/lastkey_scan/keysactive, service_SDL_events
#include "loudness.h"      // fade_song
#include "lvlmast.h"       // shapeFile[]
#include "mainint.h"       // JE_getCost
#include "mouse.h"         // mouse_x/y, JE_mouseStart/Replace, mouseCursor
#include "mtrand.h"        // mt_rand
#include "musmast.h"       // songBuy, DEFAULT_SONG_BUY (shop / buy-sell music)
#include "nortsong.h"      // JE_playSampleNum, setDelay, wait_delayorinput, limit_render_fps
#include "nortvars.h"      // JE_anyButton
#include "palette.h"       // colors, fade_palette, fade_black
#include "picload.h"       // JE_loadPic
#include "player.h"        // player[]
#include "sndmast.h"       // S_SELECT, S_CURSOR, S_SPRING
#include "sprite.h"        // JE_loadCompShapes, enemySpriteSheets, shopSpriteSheet
#include "tyrian2.h"       // itemAvail, itemAvailMax
#include "varz.h"          // eventRec, maxEvent, map* globals
#include "video.h"         // VGAScreen/VGAScreen2, JE_showVGA, output_vsync

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The stock Tyrian 2000 weapon table fills ports 1-47 with real weapons; ports
// 48-60 are dummy "Test" placeholders (see custom_weapon.c), so the shop only
// ever offers front/rear weapons from 1-47.
#define ENDLESS_REAL_WEAPON_PORTS 47

// --- Run state ------------------------------------------------------------------

int      endlessRunDepth  = 0;   // levels cleared this run (0 on the first level)
Uint64   endlessActiveMods = 0;  // ENDLESS_MOD_* bits for the current level (64-bit: TOPSY/SLUGGISH use bits 32-33)
int      endlessArmorBonus = 0;  // run-persistent +max armor bought at the outpost
int      endlessRunKills = 0;    // total enemies destroyed this run (shown on the end screen)
int      endlessRunBossKills = 0;// boss-tier enemies destroyed this run

// --- Run seed & structural RNG --------------------------------------------------
// Structure (level order, mutators, perks, shop stock) draws from a dedicated SplitMix64
// stream, isolated from the shared gameplay mt_rand (notes.md §Seeded structure RNG).
static char   endlessRunSeed[ENDLESS_SEED_MAXLEN] = "";  // the run's seed string (also shown in-game)
static Uint64 endlessSeedHash = 0;   // FNV-1a hash of endlessRunSeed; the base for every per-zone reseed
static Uint64 endlessRngState = 0;   // live SplitMix64 state (structural stream)
static Uint64 endlessEliteRngState = 0;  // live SplitMix64 state for the seeded elite/champion tier rolls

// FNV-1a over the string's bytes: maps any typed text to a 64-bit seed, so every seed is valid.
static Uint64 endlessHashString(const char *s)
{
	Uint64 h = 14695981039346656037ULL;  // FNV offset basis
	for (; *s != '\0'; ++s)
		h = (h ^ (Uint8)*s) * 1099511628211ULL;  // FNV prime
	return h;
}

// One SplitMix64 step on `state`: the endless RNG core, shared by the structural stream and the
// separate elite-tier stream below. Every use is `...Rand() % n`.
static Uint32 endlessSplitMixNext(Uint64 *state)
{
	Uint64 z = (*state += 0x9E3779B97F4A7C15ULL);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	z ^= z >> 31;
	return (Uint32)(z >> 32);
}

// Derive a fresh stream state for a (zone, phase): mix the run's seed hash with a salt so each
// (seed, salt) is an independent, reproducible sequence.
static Uint64 endlessSplitMixSeed(Uint64 salt)
{
	Uint64 z = endlessSeedHash + (salt + 1) * 0x9E3779B97F4A7C15ULL;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	z ^= z >> 31;
	return z;
}

// The structural random: a drop-in for mt_rand() at the structural call sites (level order,
// course mutators, perk offers, shop stock). Re-derived per (zone, phase) by endlessReseed.
static Uint32 endlessRand(void)
{
	return endlessSplitMixNext(&endlessRngState);
}

// Re-derive the structural stream for a fresh (zone, phase): each zone's generation is independent
// of what the player did in earlier zones. Called at each outpost and each level start (see
// endlessBetweenLevels / endlessRegenerateLevel).
static void endlessReseed(Uint64 salt)
{
	endlessRngState = endlessSplitMixSeed(salt);
}

// The elite/champion tier random: its own seeded stream, per (seed, zone) in endlessResetElites;
// only the roll sequence is seed-fixed (notes.md §Seeded structure RNG).
static Uint32 endlessEliteRand(void)
{
	return endlessSplitMixNext(&endlessEliteRngState);
}

void endlessSetSeed(const char *s)
{
	SDL_strlcpy(endlessRunSeed, (s != NULL) ? s : "", sizeof(endlessRunSeed));
	endlessSeedHash = endlessHashString(endlessRunSeed);
	endlessReseed(0);
}

const char *endlessSeedString(void)
{
	return endlessRunSeed;
}

// Per-zone timers (reset each zone): elapsed ticks drive ENRAGE; the turbodrive timer counts
// down the quickened-fire window after each kill. Advanced by endlessGameplayTick.
static int endlessZoneTicks      = 0;
#define ENDLESS_TURBODRIVE_TICKS 70   // ~2s of boosted fire per kill (TURBODRIVE)
static int endlessTurbodriveTimer = 0;

// Escalating outpost prices, reset each visit.
static long endlessRerollCost = 0;
static int  endlessHullCost   = 0;

// E-Shop kill-fire buff: bought once per shop visit, its modifier bits are stashed here and
// OR'd into the next sector's mods at course selection (see endlessSelectCourse), so they
// survive the course choice that would otherwise overwrite endlessActiveMods.
static unsigned endlessPurchasedMods = 0;
// Which kill-fire buff was bought this visit (mutually exclusive): 0 none, 1 Turbodrive,
// 2 Overdrive.
static int endlessBuffKind = 0;
// Overdrive stacks: +1 per kill while the buff window is up (capped), each worth +5% fire
// and +2.5% damage. Reset to 0 the moment the kill-fire window lapses (see endlessGameplayTick).
static int endlessOverdriveStacks = 0;

// Kill-fire buff recharge: the run depth at which the three E-Shop kill-fire buys unlock again
// after a purchase (0 = no lock). Run-persistent and saved (v5), so a mid-cooldown save/resume
// can't wipe it. The lock length scales with depth; see endlessBuffCooldownLength.
static int endlessBuffCooldownUntil = 0;

// --- Expanded E-Shop state -----------------------------------------------------------------------
// Buff "charge" scales the kill-fire window/damage with the cash paid (normalized by depth,
// cap 20). Reset each run.
static int endlessBuffCharge = 0;
static int endlessBuffWindowTicks(void)  // base kill-fire window, extended by charge (up to ~2.5x)
{
	// charge 0 -> 1.0x (~2s), charge 10 -> 1.75x (~3.5s), charge 20 -> 2.5x (~5s)
	return ENDLESS_TURBODRIVE_TICKS * (40 + 3 * endlessBuffCharge) / 40;
}
static int endlessBuffChargeFromPaid(long paid)  // cash paid -> charge tier (normalized by depth), cap 20
{
	const long unit = 2000 + (long)endlessRunDepth * 150;
	const int c = (int)(paid / unit);
	return (c > 20) ? 20 : (c < 0 ? 0 : c);
}

// Revive token: a held one-shot that survives a lethal hit (price doubles per revive spent this run).
// Cleanse charges: pre-bought strips of the worst mutator off the next chosen course. Both are
// run-persistent (reset each run). Per-visit escalating costs for the new buys are (re)set in
// endlessResetShopPrices; endlessGambleMsg holds the last gamble outcome for the E-Shop help line.
static bool endlessReviveHeld = false;
static int  endlessRevivesUsed = 0;
static int  endlessCleanseChargeCount = 0;
static long endlessBombCost = 0, endlessExtraPerkCost = 0, endlessCleanseCost = 0;
static char endlessGambleMsg[48] = "";
static bool endlessGamblePerkWon = false;  // a gamble handed out a free perk pick; the E-Shop dispatch opens MENU_PERKS
static int  endlessShopTax = 0;            // Loan Shark: permanent +% on every shop price for the rest of the run
static bool endlessGambleRigged = false;   // Rigged: the NEXT gamble secretly rolls twice and keeps the worse result
static int  endlessLongCon = 0;            // The Long Con: sectors until a paid-and-forgotten APEX ambush comes due (0 = none)
static bool endlessArmorHudDirty = false;  // set when the Overheat DoT shaves hull; the game loop repaints the (event-driven) armor bar
static bool endlessResumeVisit   = false;  // a save was just loaded: the next outpost restores its snapshot instead of rerolling (consumed by endlessBetweenLevels)
static bool endlessCreditsShown  = false;  // the zone-100 credits roll has already played this run; rides the save so reloading the zone-101 outpost doesn't replay it
#define ENDLESS_CREDITS_ZONE 100           // zones cleared before the run rolls the credits (once per run, at the outpost that follows)

// The track the last-played zone actually used, and the run depth it was picked for. Remembering
// the REAL song (rather than re-deriving an approximation of it from the previous zone's stream)
// is what makes the never-two-in-a-row guarantee exact; both ride the save so a resumed run still
// knows what it just heard. See endlessPickLevelMusic.
static JE_byte endlessLastSong      = 0;   // 0 = nothing played yet this run
static int     endlessLastSongDepth = -1;  // -1 = none

// --- Milestone zones -------------------------------------------------------------------------
// Every 50th zone is a set-piece: Chart-a-Course offers a full slate of five S-tier sectors and
// nothing else (endlessGenerateCourses), and clearing one is worth a guaranteed perk pick
// (endlessBetweenLevels). The plain milestone (50, 150, 250, ...) charts S+/S++; the GRAND
// milestone -- every 100th zone -- charts S++/S+++. Keyed off the REAL zone the player sees, not
// the difficulty-scaled one, so the numbers match the HUD.
#define ENDLESS_MILESTONE_EVERY 50
#define ENDLESS_MILESTONE_GRAND 100

// Milestone class of an arbitrary ZONE number: 0 = ordinary, 1 = the S+/S++ milestone, 2 = the
// S++/S+++ one. Taking the zone as a parameter (rather than reading the run depth) is what lets the
// music picker look at a zone's NEIGHBOURS without any RNG.
static int endlessMilestoneKindOfZone(int zone)
{
	if (zone <= 0)
		return 0;
	if (zone % ENDLESS_MILESTONE_GRAND == 0)
		return 2;
	if (zone % ENDLESS_MILESTONE_EVERY == 0)
		return 1;
	return 0;
}

// The zone about to be charted / played (the run depth counts zones CLEARED).
static int endlessMilestoneKind(void)
{
	return endlessMilestoneKindOfZone(endlessRunDepth + 1);
}

// Was run depth `depth` a milestone zone? (A run depth IS the zone just cleared, so this is the
// test for "the outpost I'm standing in follows a milestone".)
static bool endlessMilestoneClearedAt(int depth)
{
	return endlessMilestoneKindOfZone(depth) != 0;
}

// Forced perk picks come on a fixed cadence: after the first cleared zone, then every 3rd zone
// (depths 1, 4, 7, ...). Perks are strong, so they stay sparing.
#define ENDLESS_PERK_EVERY 3

// Each milestone class flies to its OWN pinned track, so the two set-pieces stay distinct. Both are
// 1-based into musicTitle[] (musmast.c), matching levelSong -- the level start plays levelSong - 1.
#define ENDLESS_MILESTONE_SONG_GRAND 35  // "One Mustn't Fall" -- every 100th zone
#define ENDLESS_MILESTONE_SONG_PLAIN 37  // "A Field for Mag"  -- the other 50th zones (50, 150, 250, ...)

// The outpost BEFORE a milestone swaps its buy/sell music for this track, so the player is warned
// that a set-piece is coming while they're still choosing a course. MIND THE TWO FORMS: songBuy is
// played as-is (play_song(songBuy)), while levelSong is 1-based (play_song(levelSong - 1)), so the
// same track carries two numbers -- keep the pair in step if it's ever retuned.
#define ENDLESS_MILESTONE_SHOP_SONG     26  // "Parlance", songBuy form (0-based)
#define ENDLESS_MILESTONE_SHOP_SONG_LVL 27  // "Parlance", levelSong form (1-based)

// The pinned track for a milestone class, or 0 for an ordinary zone.
static JE_byte endlessMilestoneSong(int kind)
{
	return (kind == 2) ? ENDLESS_MILESTONE_SONG_GRAND
	     : (kind == 1) ? ENDLESS_MILESTONE_SONG_PLAIN
	     : 0;
}

// Is a forced perk pick due at the outpost for run depth `depth` (the zone just cleared)? Three
// reasons: the cadence above; a cleared MILESTONE zone (50, 100, 150, ...); or the zone right
// after a depth where those two COLLIDED. A collision (depth 100, 250, 400, ... -- every third
// milestone) would otherwise hand out one perk where the player earned two, so the second is
// deferred by a zone instead of being swallowed; the cadence itself is unaffected and carries on
// from its own schedule. Derived purely from the depth, so it needs no persisted state and comes
// out the same across a save/reload or a mid-zone bail.
static bool endlessPerkDueAtDepth(int depth)
{
	if (depth <= 0)
		return false;
	if (depth % ENDLESS_PERK_EVERY == 1 || endlessMilestoneClearedAt(depth))
		return true;
	const int prev = depth - 1;  // deferred half of a collision on the previous zone
	return endlessMilestoneClearedAt(prev) && prev % ENDLESS_PERK_EVERY == 1;
}

// Hardcore mode for the current run (see endless.h): no saving at all + a locked outpost on a
// mid-zone bail. Chosen on the seed screen, applied in newEndlessGame, cleared by endlessResetRun.
bool endlessHardcore = false;

// --- "Quit Level" -> outpost retry (see endless.h) -------------------------------------------
bool endlessQuitToOutpost = false;  // ESC-menu Quit (endless): return to the outpost instead of ending the run
bool endlessLockedSortie  = false;  // the reopened outpost is locked to the launch-time loadout/course (hardcore only)
// The launch-time snapshot Quit Level reverts to. The endless run/outpost half lives in an
// EndlessSlotRec (declared near the save code); these primitives hold the loadout + committed level
// and are declared up here so endlessResetRun (above the rec typedef) can clear them.
static bool     endlessSortieHave  = false; // a launch-time snapshot exists
static Player   endlessSortiePlayer[2];     // player[] loadout at launch (cash / items / superbombs)
static Uint64 endlessSortieModsV = 0;       // endlessActiveMods at launch (the committed level's mutators)
static JE_byte  endlessSortieSec   = 0;     // committed level section
static int      endlessSortieEp    = 0;     // committed episode
static JE_byte  endlessSortieFile  = 0;     // committed lvl file number
// One-shots consumed at the course pick (E-Shop buff / sabotage charges / Long Con), snapshotted
// pre-consumption so a non-hardcore bail can restore them (see endlessRestoreSortie).
static unsigned endlessSortiePrePurchased = 0;
static int      endlessSortiePreCleanse   = 0;
static int      endlessSortiePreLongCon   = 0;

// Base (shipped) level each zone is built on, captured in endlessRegenerateLevel just before it
// renames levelName to "ZONE n" (levelName still holds the level's authored name there). When a
// new zone starts, the current base rolls down into "previous". Read only by the crash logger
// (endless base-level accessors below); both are reset per run in endlessResetRun.
static char endlessBaseName[11]     = "";
static int  endlessBaseEp           = 0;
static int  endlessBaseLvl          = 0;
static char endlessPrevBaseName[11] = "";
static int  endlessPrevBaseEp       = 0;
static int  endlessPrevBaseLvl      = 0;

// Recently-played base levels (anti-repeat): a small ring keyed by (episode, section), [0] = the
// zone just played. The next zone's course/level picker avoids everything in this window, so the
// same base level can't recur twice in a row and won't return until at least ENDLESS_LEVEL_HISTORY
// other zones have been played (the window shrinks gracefully when too few safe levels exist to
// fill it). Recorded in endlessRegenerateLevel (the one choke point every zone load passes through),
// reset in endlessResetRun, persisted with the run (save v6), and surfaced in the crash log.
#define ENDLESS_LEVEL_HISTORY 5
static int     endlessRecentEp[ENDLESS_LEVEL_HISTORY];
static JE_byte endlessRecentSec[ENDLESS_LEVEL_HISTORY];
static int     endlessRecentCount;  // valid entries, 0..ENDLESS_LEVEL_HISTORY

// Push (ep, sec) as the newest recently-played level. A repeat of the current newest (e.g. a locked-
// sortie relaunch reloading the same committed level) is ignored so it can't crowd the window.
static void endlessRecordRecentLevel(int ep, int sec)
{
	if (endlessRecentCount > 0 && endlessRecentEp[0] == ep && endlessRecentSec[0] == (JE_byte)sec)
		return;
	for (int i = ENDLESS_LEVEL_HISTORY - 1; i > 0; --i)
	{
		endlessRecentEp[i]  = endlessRecentEp[i - 1];
		endlessRecentSec[i] = endlessRecentSec[i - 1];
	}
	endlessRecentEp[0]  = ep;
	endlessRecentSec[0] = (JE_byte)sec;
	if (endlessRecentCount < ENDLESS_LEVEL_HISTORY)
		++endlessRecentCount;
}

// Is (ep, sec) among the newest `window` recently-played levels? window is clamped to what's tracked.
static bool endlessLevelInRecent(int ep, JE_byte sec, int window)
{
	if (window > endlessRecentCount)
		window = endlessRecentCount;
	for (int i = 0; i < window; ++i)
		if (endlessRecentEp[i] == ep && endlessRecentSec[i] == sec)
			return true;
	return false;
}

// Combo kill counter: +1 per kill while a kill-fire window is up, reset the instant it lapses.
// Not itself capped (the HUD's plain "xN"); only the derived fire-rate multiplier below is.
static int endlessComboKills = 0;
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

// --- Perks: run-persistent, stacking upgrades -----------------------------------------
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

static const EndlessPerk endlessPerkTable[PERK_COUNT] = {
	{ "Heavy Rounds",     "Your shots deal more damage.",         5 },
	{ "Rapid Cyclers",    "Your guns fire noticeably faster.",    4 },
	{ "Ablative Plating", "Raises your maximum armor.",           6 },
	{ "Scavenger",        "More cash from clears and bounties.",  4 },
	{ "Nanorepair",       "Slowly regenerate armor in flight.",   3 },
	{ "Siphon",           "Chance to restore armor on a kill.",   3 },
	{ "Bounty Hunter",    "Elite and champion bounties doubled.", 1 },
	{ "Bulwark",          "Take less damage from every hit.",     5 },
	{ "Adrenaline",       "Fire much faster when badly hurt.",    3 },
	{ "Glass Cannon",     "Big damage, but a weaker hull.",       1 },
	{ "Rapid Recharge",   "Specials and ammo recharge faster.",   4 },
	{ "Autofire Special", "Auto-fires your special as you shoot.", 1 },
	{ "Efficient Coils",  "Your weapons draw less generator power.", 5 },
	{ "Shield Matrix",    "Your shield recharges faster.",        4 },
	{ "Rapid Charger",    "Charge sidekicks power up faster.",    4 },
	{ "High-Velocity Shots", "Your shots travel faster.",        3 },
};

bool endlessPerkPending = false;             // a perk pick is queued for the next shop
static JE_byte endlessPerkOwned[PERK_COUNT]; // stack counts, reset each run
static int endlessPerkChoice[3];             // this visit's offered perk ids
static int endlessPerkChoiceN = 0;           // how many are offered (0..3)
static int endlessRegenTick = 0;             // Nanorepair countdown (reset each run)
static int endlessPerkCashPercent(void);     // Scavenger cash multiplier (defined below)
// The run depth whose post-zone perk pick has already been resolved (taken or declined); -1 =
// none yet. endlessBetweenLevels offers the forced pick only when this lags the current depth,
// so re-entering the same outpost (e.g. after a save/reload) can't hand out a second perk.
static int endlessPerkDepthDone = -1;

// Cash the player had on entering the shop this visit. The E-Shop cash-fraction buys (buff /
// Buy Special) price off this snapshot, not live cash, so their cost stays fixed for the whole
// visit (buying one doesn't cheapen the next). Captured in endlessResetShopPrices.
static long endlessShopEntryCash = 0;

#define ENDLESS_HULL_STEP 6      // +armor per hull upgrade
#define ENDLESS_HULL_BASE 60     // starting cap on the run-persistent armor bonus (zone 0)
#define ENDLESS_HULL_MAX  150    // absolute ceiling (ship base + this + perks stays < 255, byte-safe)

// The hull-reinforcement cap rises with depth, so the outpost always has another tier to sell
// on a deep run. Every 6 zones unlocks one more +6 step, up to ENDLESS_HULL_MAX.
static int endlessHullMax(void)
{
	int m = ENDLESS_HULL_BASE + (endlessRunDepth / 6) * ENDLESS_HULL_STEP;
	return (m > ENDLESS_HULL_MAX) ? ENDLESS_HULL_MAX : m;
}

// The pre-difficulty "choose your seed" screen (see endless.h), shown for a new Endless run.
// Styled after difficultySelect (the adjacent screen): pic-2 background, centered rows.
bool endlessSeedSelect(char *outSeed, size_t outN, bool *outHardcore)
{
	if (shopSpriteSheet.data == NULL)
		JE_loadCompShapes(&shopSpriteSheet, '1');  // mouse-pointer sprites (as difficultySelect does)

	char seed[ENDLESS_SEED_MAXLEN] = "";  // the seed being typed ("" => a random seed is rolled on Start)
	size_t len = 0;
	bool hardcore = false;  // the Hardcore toggle (default off); written to *outHardcore on Start

	enum { ROW_SEED, ROW_RANDOM, ROW_HARDCORE, ROW_START, ROW_COUNT };
	int selected = ROW_SEED;

	const int xCenter = 320 / 2;  // fixed 320 center (see difficultySelect / gameplaySelect rationale)
	const int yRows   = 82;
	const int dyRows  = 20;
	const int hRow    = 15;

	// Background + static titles: drawn once into VGAScreen2, copied to VGAScreen each frame.
	JE_loadPic(VGAScreen2, 2, false);
	draw_font_hv_shadow(VGAScreen2, xCenter, 20, "ENDLESS", large_font, centered, 15, -3, false, 2);
	draw_font_hv_shadow(VGAScreen2, xCenter, 54, "Type a seed for a repeatable run,",  small_font, centered, 15, 2, false, 1);
	draw_font_hv_shadow(VGAScreen2, xCenter, 64, "or leave it blank for a random one.", small_font, centered, 15, 2, false, 1);

	wait_noinput(true, true, true);

	bool first = true, done = false, commit = false;
	int prev_mx = mouse_x, prev_my = mouse_y;
	newkey = newmouse = new_text = false;  // input flags are consumed + cleared at the END of each pass
	while (!done)
	{
		memcpy(VGAScreen->pixels, VGAScreen2->pixels, (size_t)VGAScreen->pitch * VGAScreen->h);

		char seedRow[48];
		if (len > 0)
			snprintf(seedRow, sizeof(seedRow), "Seed: %s_", seed);
		else
			SDL_strlcpy(seedRow, "Seed: (random)", sizeof(seedRow));
		const char *label[ROW_COUNT] = { seedRow, "Randomize", hardcore ? "Hardcore: On" : "Hardcore: Off", "Start" };

		int rowW[ROW_COUNT];
		for (int i = 0; i < ROW_COUNT; ++i)
		{
			rowW[i] = JE_textWidth(label[i], normal_font);
			draw_font_hv_shadow(VGAScreen, xCenter, yRows + dyRows * i, label[i],
			                    normal_font, centered, 15, -4 + (i == selected ? 2 : 0), false, 2);
		}

		// A line under the rows spells out what the current Hardcore choice means, then the controls.
		draw_font_hv_shadow(VGAScreen, xCenter, yRows + dyRows * ROW_COUNT + 4,
		                    hardcore ? "Hardcore: no saving, and no second chances."
		                             : "Standard: save anytime; bail a level to re-outfit.",
		                    small_font, centered, 15, 2, false, 1);
		draw_font_hv_shadow(VGAScreen, xCenter, yRows + dyRows * ROW_COUNT + 18,
		                    "Up/Down Move    Enter Select    Esc Back", small_font, centered, 15, 4, false, 1);

		if (first)
		{
			fade_palette(colors, 10, 0, 255);
			first = false;
		}

		// Present at the render rate, smooth like every other menu: pump SDL events every frame and
		// end the pass the moment input arrives. JE_mouseStart calls service_SDL_events, so input
		// accumulates onto the edge flags (cleared only at the end of the pass, never mid-pass), so
		// no keystroke is dropped. setDelay bounds an idle pass; input ends it early.
		mouseCursor = MOUSE_POINTER_NORMAL;
		push_joysticks_as_keyboard();
		setDelay(1);
		for (;;)
		{
			JE_mouseStart();   // service_SDL_events(false): pump + accumulate, then prep the cursor
			JE_showVGA();
			JE_mouseReplace();
			if (newkey || newmouse || new_text || getDelayTicks() == 0)
				break;
			if (!output_vsync)
				limit_render_fps();
		}

		// Row hit-testing (centered rows): which row, if any, is the cursor over. Only a mouse
		// MOVE re-selects (so arrow-key navigation isn't yanked back to a resting cursor); a click
		// still acts on whatever row it lands on.
		int hover = -1;
		for (int i = 0; i < ROW_COUNT; ++i)
		{
			const int x0 = xCenter - rowW[i] / 2, x1 = xCenter + rowW[i] / 2;
			const int y0 = yRows + dyRows * i,    y1 = y0 + hRow;
			if (mouse_x >= x0 && mouse_x < x1 && mouse_y >= y0 && mouse_y < y1)
				hover = i;
		}
		const bool mouseMoved = (mouse_x != prev_mx || mouse_y != prev_my);
		prev_mx = mouse_x;
		prev_my = mouse_y;
		if (mouseMoved && hover >= 0 && hover != selected)
		{
			selected = hover;
			JE_playSampleNum(S_CURSOR);
		}

		bool activate = false;
		if (newmouse)
		{
			if (lastmouse_but == SDL_BUTTON_RIGHT)
			{
				JE_playSampleNum(S_SPRING);
				done = true;  // cancel
			}
			else if (hover >= 0)
			{
				selected = hover;
				activate = true;
			}
		}
		else if (newkey)
		{
			switch (lastkey_scan)
			{
			case SDL_SCANCODE_UP:
				selected = (selected == 0) ? ROW_COUNT - 1 : selected - 1;
				JE_playSampleNum(S_CURSOR);
				break;
			case SDL_SCANCODE_DOWN:
				selected = (selected + 1) % ROW_COUNT;
				JE_playSampleNum(S_CURSOR);
				break;
			case SDL_SCANCODE_BACKSPACE:
				if (len > 0)
					seed[--len] = '\0';
				selected = ROW_SEED;
				break;
			case SDL_SCANCODE_LEFT:
			case SDL_SCANCODE_RIGHT:
				// A natural left/right on the on/off Hardcore row flips it.
				if (selected == ROW_HARDCORE)
				{
					hardcore = !hardcore;
					JE_playSampleNum(S_CLICK);
				}
				break;
			case SDL_SCANCODE_RETURN:
			case SDL_SCANCODE_KP_ENTER:
				activate = true;
				break;
			case SDL_SCANCODE_ESCAPE:
				JE_playSampleNum(S_SPRING);
				done = true;  // cancel
				break;
			default:
				break;
			}
		}

		// Typed characters always edit the seed field -- any printable ASCII is a valid seed byte.
		if (new_text)
		{
			for (size_t ti = 0; last_text[ti] != '\0'; ++ti)
			{
				const unsigned char c = (unsigned char)last_text[ti];
				if (c >= 32 && c < 127 && len < sizeof(seed) - 1)
					seed[len++] = (char)c;
			}
			seed[len] = '\0';
			selected = ROW_SEED;
		}

		if (activate && !done)
		{
			if (selected == ROW_RANDOM)
			{
				snprintf(seed, sizeof(seed), "%lu", (unsigned long)(1u + mt_rand() % 999999999u));  // a fresh random seed to preview / share
				len = strlen(seed);
				JE_playSampleNum(S_SELECT);
			}
			else if (selected == ROW_HARDCORE)
			{
				hardcore = !hardcore;  // Enter / click on the toggle flips it, doesn't start the run
				JE_playSampleNum(S_CLICK);
			}
			else  // ROW_SEED or ROW_START: begin the run
			{
				if (len == 0)  // blank field => roll a random seed so a run always has one
					snprintf(seed, sizeof(seed), "%lu", (unsigned long)(1u + mt_rand() % 999999999u));
				SDL_strlcpy(outSeed, seed, outN);
				if (outHardcore)
					*outHardcore = hardcore;
				JE_playSampleNum(S_SELECT);
				commit = true;
				done = true;
			}
		}

		newkey = newmouse = new_text = false;  // consume this pass's input so nothing repeats next pass
	}

	fade_black(commit ? 10 : 15);  // fade out like difficultySelect, so the next screen fades in cleanly
	return commit;
}

void endlessResetRun(void)
{
	endlessRunDepth   = 0;
	endlessActiveMods = 0;
	endlessArmorBonus = 0;
	endlessRunKills   = 0;
	endlessRunBossKills = 0;
	endlessPurchasedMods = 0;
	endlessBuffKind = 0;
	endlessBuffCooldownUntil = 0;  // kill-fire recharge: fresh run, no lock
	endlessOverdriveStacks = 0;
	endlessComboKills = 0;
	endlessPerkPending = false;
	endlessPerkChoiceN = 0;
	endlessPerkDepthDone = -1;
	endlessResumeVisit = false;
	endlessCreditsShown = false;   // fresh run: the zone-100 credits are still ahead (a resume reloads this in endlessApplyCurrent)
	endlessLastSong = 0;           // no zone has played yet, so nothing for the music anti-repeat to avoid
	endlessLastSongDepth = -1;
	endlessRegenTick = 0;
	endlessBuffCharge = 0;
	endlessReviveHeld = false;
	endlessRevivesUsed = 0;
	endlessCleanseChargeCount = 0;
	endlessGamblePerkWon = false;
	endlessShopTax = 0;         // gamble state: clear any lingering debt tax / rigged flag / long con
	endlessGambleRigged = false;
	endlessLongCon = 0;
	endlessLockedSortie = false;   // no locked "gave up" outpost on a fresh run
	endlessQuitToOutpost = false;
	endlessSortieHave = false;     // no launch-time snapshot yet
	endlessSortiePrePurchased = 0; // no pre-pick one-shots stashed yet
	endlessSortiePreCleanse = 0;
	endlessSortiePreLongCon = 0;
	// Default to non-hardcore. newEndlessGame overrides this from the seed screen right after the
	// reset; a save/resume (endlessApplyCurrent runs this reset) correctly leaves it cleared, since
	// hardcore runs never save and so are never resumed.
	endlessHardcore = false;
	endlessBaseName[0] = endlessPrevBaseName[0] = '\0';  // crash-log base-level history: fresh run
	endlessBaseEp = endlessBaseLvl = endlessPrevBaseEp = endlessPrevBaseLvl = 0;
	endlessRecentCount = 0;  // anti-repeat ring: fresh run (a resume reloads it in endlessApplyCurrent)
	player[0].superbombs = 0;  // fresh run: no bombs (they persist across levels within a run)
	memset(endlessPerkOwned, 0, sizeof(endlessPerkOwned));
	endlessSetSeed("");  // safe default; newEndlessGame / a resume load sets the real run seed next
}

void endlessCountKill(int linknum)
{
	if (!endlessMode)
		return;

	// A multi-part enemy is several enemy[] slots sharing one nonzero linknum, all removed
	// consecutively in a single kill loop, so count the whole enemy once, not once per tile
	// (else the run tally and the Overdrive stack balloon). Lone enemies are linknum 0 and always
	// counted. Two live enemies never share a linknum, so deduping consecutive same-linknum calls
	// is safe.
	static int lastCountedLink = 0;
	if (linknum != 0 && linknum == lastCountedLink)
		return;
	lastCountedLink = linknum;

	++endlessRunKills;
	// Boss kills are tallied in draw_boss_bar (when a boss's health bar empties), so the
	// "Bosses slain" stat counts only real bar-spawning bosses, not the high-armor regulars the
	// old armor >= ENDLESS_BOSS_ARMOR test here wrongly swept in.
	if (endlessActiveMods & ENDLESS_MOD_KILLFIRE_ANY)
	{
		endlessTurbodriveTimer = endlessBuffWindowTicks();  // refresh the window (boost OR evil jam; charge lengthens it)
		++endlessComboKills;                              // combo kill -> compounds the fire boost / evil jam
	}
	if ((endlessActiveMods & ENDLESS_MOD_STACKED) && endlessOverdriveStacks < ENDLESS_OVERDRIVE_MAX_STACKS)
		++endlessOverdriveStacks;                        // per-kill damage stack (Overdrive/Overblast) or damage-cut stack (Burnout/Misfire)

	// Siphon perk: a per-kill chance to restore 1 armor (up to the ship's max).
	if (endlessPerkOwned[PERK_SIPHON] > 0
	    && (int)(mt_rand() % 100) < endlessPerkOwned[PERK_SIPHON] * ENDLESS_PERK_SIPHON_PCT  // per-kill: gameplay RNG, not the seed
	    && player[0].armor < player[0].initial_armor)
		++player[0].armor;
}

void endlessPreloadBanks(void)
{
	// Apply the level's first enemy-sprite-bank load (event type 5) immediately, so the starting
	// banks are resident before anything spawns. The engine zeroes the bank slots at level start
	// and schedules this load on the event timeline; applying it up front is a harmless safety net
	// (it loads exactly the level's own first banks) so the earliest enemies render. Later event-5s
	// still fire as normal.
	for (int i = 0; i < maxEvent; ++i)
	{
		if (eventRec[i].eventtype != 5)
			continue;

		const int banks[4] = {
			eventRec[i].eventdat,  eventRec[i].eventdat2,
			eventRec[i].eventdat3, eventRec[i].eventdat4,
		};
		for (int s = 0; s < 4; ++s)
		{
			const int b = banks[s];
			if (b > 0 && b <= 36 && enemySpriteSheetIds[s] != (Uint8)b)
			{
				JE_loadCompShapes(&enemySpriteSheets[s], shapeFile[b - 1]);
				enemySpriteSheetIds[s] = (Uint8)b;
			}
		}
		break;  // only the initial bank set
	}
}

// Random endless-safe (ep, section, file) from any installed episode, avoiding the recently-played
// levels; the file distinguishes the two Ep1-section-3 TYRIAN cuts. fileOut may be NULL.
static bool endlessRandomSafeLevel(int *epOut, JE_byte *secOut, JE_byte *fileOut)
{
	// Build the full cross-episode pool once (each JE_getLevelSections parses a levels%d.dat off
	// disk), then sample it in memory -- so the recent-play rejection below can retry for free
	// instead of re-reading files. Sampling is uniform per LEVEL (not per episode as the old two-
	// step pick was), which spreads variety more evenly across the differently-sized episodes.
	struct { int ep; JE_byte sec, file; } pool[EPISODE_MAX * 64];
	int npool = 0;
	for (int e = 1; e <= EPISODE_MAX && npool < (int)COUNTOF(pool); ++e)
	{
		if (!episodeAvail[e - 1])
			continue;
		JE_byte secs[64], files[64];
		const uint n = JE_getLevelSections(e, secs, files, COUNTOF(secs));
		for (uint i = 0; i < n && npool < (int)COUNTOF(pool); ++i)
		{
			pool[npool].ep   = e;
			pool[npool].sec  = secs[i];
			pool[npool].file = files[i];
			++npool;
		}
	}
	if (npool == 0)
		return false;

	// Prefer a level outside the whole recent-play window; if too few levels exist to honour it,
	// relax the window one step at a time (window 0 excludes nothing, so this always terminates
	// with a pick -- a repeat a few zones early beats failing to choose a level at all).
	for (int window = endlessRecentCount; window >= 0; --window)
	{
		for (int attempt = 0; attempt < npool * 4 + 8; ++attempt)
		{
			const int k = (int)(endlessRand() % (uint)npool);
			if (endlessLevelInRecent(pool[k].ep, pool[k].sec, window))
				continue;
			*epOut  = pool[k].ep;
			*secOut = pool[k].sec;
			if (fileOut != NULL)
				*fileOut = pool[k].file;
			return true;
		}
	}
	return false;
}

JE_byte endlessPickNextLevel(void)
{
	// Fallback single-level picker (used if the branching choice can't build candidates).
	// Picks a random endless-safe level from any installed episode, switching episode data
	// if needed (the weapon arsenal is shared, so the loadout is unaffected).
	int ep;
	JE_byte sec, file;
	if (!endlessRandomSafeLevel(&ep, &sec, &file))
	{
		forcedLvlFileNum = 0;
		return FIRST_LEVEL;
	}

	if (ep != episodeNum)
		JE_initEpisode(ep);
	forcedLvlFileNum = file;  // load this ']L''s file, not just the section's first (see JE_loadMap)
	return sec;
}

// --- Per-level random music ------------------------------------------------------
// Endless plays a random track each level (1-based song numbers into musicTitle[]), avoiding
// an immediate repeat between zones. The non-level jingles and a few misfits are left out:
static const JE_byte endlessLevelSongs[] = {  // omits shop #3, level-end #10, game-over #11, high-score #34, MusicMan #19, ZANAC3 #31, BEER #41
	1, 2, 4, 5, 6, 7, 8, 9, 12, 13, 14, 15, 16, 17, 18, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 32, 33, 35, 36, 37, 38, 39, 40,
};

static void endlessPickLevelMusic(void)
{
	const int zone = endlessRunDepth + 1;

	// Re-entering the SAME zone -- a Quit Level retry, a locked relaunch, or a reload that drops
	// back into it -- keeps the track it already had, even if the player picked a different course.
	// The music belongs to the zone, not to the level, and a retry must never reshuffle it.
	if (endlessLastSong != 0 && endlessLastSongDepth == endlessRunDepth)
	{
		levelSong = endlessLastSong;
		return;
	}

	// Otherwise: deterministic per (seed, zone). Start from the previous zone's stream, taking its
	// FIRST draw -- only an APPROXIMATION of what that zone actually played (it may have re-rolled),
	// but it's the fallback for the cases where the exact value below isn't available: a pre-v10
	// save, or a debug zone jump that skipped the intervening zones.
	JE_byte prev = 0;
	if (endlessRunDepth > 0)
	{
		endlessReseed((Uint64)(endlessRunDepth - 1) * 2 + 1);
		prev = endlessLevelSongs[endlessRand() % COUNTOF(endlessLevelSongs)];
		endlessReseed((Uint64)endlessRunDepth * 2 + 1);  // re-prime this zone's stream
	}

	// A MILESTONE zone is pinned to its class's theme. The rolls below still run, so the seeded
	// stream stays aligned with an ordinary zone.
	const JE_byte pinned = endlessMilestoneSong(endlessMilestoneKindOfZone(zone));

	// No track may play two zones running. Three sources for "what must this zone avoid", layered
	// least- to most-authoritative: the approximation above; a milestone predecessor's pinned theme
	// (RNG-free, so exact); and finally the song the previous zone REALLY played, remembered from
	// when it started (exact whenever the run walked into this zone normally). The successor's
	// pinned theme is likewise exact, so a milestone is never announced by its own track either.
	const JE_byte prevPinned = endlessMilestoneSong(endlessMilestoneKindOfZone(zone - 1));
	if (prevPinned != 0)
		prev = prevPinned;
	if (endlessLastSong != 0 && endlessLastSongDepth == endlessRunDepth - 1)
		prev = endlessLastSong;
	const JE_byte nextPinned = endlessMilestoneSong(endlessMilestoneKindOfZone(zone + 1));
	// When the NEXT zone is a milestone, the outpost between here and there plays the warning track
	// (endlessBetweenLevels). Keep this zone off it too, or the level would run straight into its own
	// music in the shop and the warning would read as "nothing changed".
	const JE_byte nextShop = (nextPinned != 0) ? (JE_byte)ENDLESS_MILESTONE_SHOP_SONG_LVL : 0;

	JE_byte s = endlessLevelSongs[endlessRand() % COUNTOF(endlessLevelSongs)];
	for (int guard = 0; guard < 6 && (s == prev || s == nextPinned || s == nextShop); ++guard)
		s = endlessLevelSongs[endlessRand() % COUNTOF(endlessLevelSongs)];

	levelSong = (pinned != 0) ? pinned : s;  // the level-start play_song(levelSong - 1) uses this

	// Remember it for the next zone's anti-repeat (and for a retry of this one).
	endlessLastSong      = levelSong;
	endlessLastSongDepth = endlessRunDepth;
}

// True while the zone being played is a milestone (either class). The level script's own music
// events are ignored there (tyrian2.c events 34/35), so nothing can unseat the pinned theme.
bool endlessMilestoneZone(void)
{
	return endlessMode && endlessMilestoneKind() != 0;
}

// Whether this zone shows the "light cone" spotlight -- rolled in endlessRegenerateLevel.
static bool endlessLightCone = false;

bool endlessLightConeActive(void) { return endlessLightCone; }

// Base (shipped) level accessors for the crash logger. "Base" = the current ZONE's underlying
// authored level; "Prev" = the one the previous zone was built on ("" name until the 2nd zone).
const char *endlessBaseLevelName(void)     { return endlessBaseName; }
int         endlessBaseLevelEpisode(void)  { return endlessBaseEp; }
int         endlessBaseLevelSection(void)  { return endlessBaseLvl; }
const char *endlessPrevLevelName(void)     { return endlessPrevBaseName; }
int         endlessPrevLevelEpisode(void)  { return endlessPrevBaseEp; }
int         endlessPrevLevelSection(void)  { return endlessPrevBaseLvl; }

// Anti-repeat recent-level ring, newest first (i = 0). Bounds-checked so the crash logger can walk
// it without trusting the count. Out-of-range indices read as 0.
int endlessRecentLevelCount(void)          { return endlessRecentCount; }
int endlessRecentLevelEpisode(int i)       { return (i >= 0 && i < endlessRecentCount) ? endlessRecentEp[i]  : 0; }
int endlessRecentLevelSection(int i)       { return (i >= 0 && i < endlessRecentCount) ? endlessRecentSec[i] : 0; }

static void endlessRollGravityDir(void);  // defined with the gravity code below; rolls this sector's pull heading

void endlessRegenerateLevel(void)
{
	// Endless plays the shipped level exactly as authored -- its enemies, their
	// placement, spawn timing, and terrain are all left untouched. This hook only fixes
	// up the bits of per-level state that a random level jump would otherwise leave in a
	// state that crashes (or misbehaves in) a plain single-player run.

	// Remember the shipped level this zone is built on before we overwrite levelName below --
	// JE_loadMap (which ran just before this) left the level's authored name in levelName. Roll
	// the current base down to "previous" first. Surfaced only in the crash log.
	memcpy(endlessPrevBaseName, endlessBaseName, sizeof(endlessPrevBaseName));
	endlessPrevBaseEp  = endlessBaseEp;
	endlessPrevBaseLvl = endlessBaseLvl;
	SDL_strlcpy(endlessBaseName, levelName, sizeof(endlessBaseName));
	endlessBaseEp  = episodeNum;
	endlessBaseLvl = mainLevel;

	// Log this zone's base level into the anti-repeat ring so the next course/level pick avoids it
	// (and the few before it). This is the single point every zone load passes through, so it also
	// covers the first zone, the debug jump, and the locked-sortie relaunch (a same-level reload is
	// ignored by endlessRecordRecentLevel).
	endlessRecordRecentLevel(episodeNum, mainLevel);

	// Show the run zone on the HUD instead of the underlying level's name. And endless
	// has no datacubes -- a randomly-picked level's cube data can crash on collect.
	snprintf(levelName, sizeof(levelName), "ZONE %d", endlessRunDepth + 1);
	cubeMax = 0;
	lastCubeMax = 0;

	// Pin the planet-map hub to one safe planet (see endlessBetweenLevels for why).
	mapOrigin = 1;
	mapPNum = 1;
	mapPlanet[0] = 1;
	mapSection[0] = mainLevel;

	// A randomly-jumped level may carry special-mode flags (Galaga 2-player, bonus,
	// extra-game) whose machinery doesn't fit a 1-player endless run and can crash.
	galagaMode = false;
	extraGame = false;
	bonusLevelCurrent = false;
	normalBonusLevelCurrent = false;

	// Fresh elite decisions each level (linknums are reused per level).
	endlessResetElites();

	// A resumed outpost snapshot is consumed by the shop; once a level actually starts, make
	// sure no stale resume flag can leak into a LATER outpost (e.g. an in-shop load).
	endlessResumeVisit = false;

	// Reset the per-zone timers (ENRAGE ramp, TURBODRIVE/Overdrive window).
	endlessZoneTicks = 0;
	endlessTurbodriveTimer = 0;
	endlessOverdriveStacks = 0;
	endlessComboKills = 0;

	// Re-derive the seeded stream for this zone's level-start draws (music), keyed by depth so it
	// stays fixed for a seed regardless of what the player did at the outpost (gamble/reroll/buys).
	endlessReseed((Uint64)endlessRunDepth * 2 + 1);

	// Random level-appropriate music each level (overrides the shipped level's own track).
	endlessPickLevelMusic();

	// Roll this zone's light cone (spotlight): a seeded 1-in-10 chance in its own reseed phase,
	// fixed per (seed, depth) without shifting the existing music/course/shop draws.
	endlessReseed((Uint64)endlessRunDepth * 2 + 0x40000000);
	endlessLightCone = (endlessRand() % 10 == 0);

	// Roll this sector's gravity-well heading: an omnidirectional well picks a fixed random heading
	// for the whole sector, a plain well points straight down. Own reseed phase (keyed by depth) so it
	// stays fixed per (seed, zone) without shifting the music / course / light-cone draws above.
	endlessReseed((Uint64)endlessRunDepth * 2 + 0x50000000);
	endlessRollGravityDir();
}

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
static int endlessDifficultyZone(void)
{
	return 1 + endlessRunDepth * endlessDifficultyRampPercent() / 100;
}

// Ordinary-enemy HP multiplier (100 = normal): +4% per (effective) level; FORTIFIED +120%
// (2.2x HP, clearly felt); FRAGILE -50%.
int endlessArmorPercent(void)
{
	int pct = 100 + endlessEffectiveDepth() * 4;
	if (endlessActiveMods & ENDLESS_MOD_FORTIFIED)
		pct += 120;
	if (endlessActiveMods & ENDLESS_MOD_FRAGILE)
		pct -= 50;
	if (pct < 25)
		pct = 25;
	if (pct > 600)
		pct = 600;
	return pct;
}

// Boss HP multiplier (1 = normal): +1x every 8 (effective) levels, reaching the 16x cap at run
// depth ~96 on Normal; FORTIFIED +3x (a 4x boss at depth 0); FRAGILE ~halves it.
int endlessBossHpMult(void)
{
	int mult = 1 + endlessEffectiveDepth() / 8;
	if (endlessActiveMods & ENDLESS_MOD_FORTIFIED)
		mult += 3;
	if (endlessActiveMods & ENDLESS_MOD_MARKED)  // gamble "Marked": the boss you paid to forget comes back bulked up
		mult += 2;
	if (endlessActiveMods & ENDLESS_MOD_FRAGILE)
		mult = (mult + 1) / 2;
	if (mult > 16)
		mult = 16;
	if (mult < 1)
		mult = 1;
	return mult;
}

// Enemy shot-cooldown multiplier (100 = normal; LOWER = fires faster): -0.75% per (effective)
// level, bottoming at the 4x-fire floor at run depth ~80 on Normal; FRENZY an extra -50% (~2x
// fire), floored at 25% so deep FRENZY runs reach ~4x.
int endlessFireDelayPercent(void)
{
	int reduce = endlessEffectiveDepth() * 3 / 4;
	if (endlessActiveMods & ENDLESS_MOD_FRENZY)
		reduce += 50;
	if (endlessActiveMods & ENDLESS_MOD_ENRAGE)  // ramps up the longer you linger in the zone
	{
		int e = endlessZoneTicks / 25;
		reduce += (e > 55) ? 55 : e;
	}
	if (endlessActiveMods & ENDLESS_MOD_OVERCLOCK)  // everything runs hot
		reduce += 30;
	if (endlessActiveMods & ENDLESS_MOD_OVERLOAD)   // Overclock cranked way up
		reduce += 55;
	if (reduce > 75)
		reduce = 75;
	return 100 - reduce;
}

// Enemy projectile-speed multiplier (100 = normal): +1.67% per (effective) level, reaching the
// 2.4x cap at run depth ~67 on Normal; SWIFT +70% (1.7x shots).
int endlessShotSpeedPercent(void)
{
	int pct = 100 + endlessEffectiveDepth() * 5 / 3;
	if (endlessActiveMods & ENDLESS_MOD_SWIFT)
		pct += 70;
	if (endlessActiveMods & ENDLESS_MOD_DILATION)  // time dilation: shots crawl
		pct -= 45;
	if (endlessActiveMods & ENDLESS_MOD_OVERCLOCK)  // everything runs hot
		pct += 40;
	if (endlessActiveMods & ENDLESS_MOD_OVERLOAD)   // Overclock cranked way up
		pct += 90;
	if (pct > 240)
		pct = 240;
	if (pct < 40)
		pct = 40;
	return pct;
}

// The tide resumes the shot-DAMAGE climb past its intensity cap (notes.md §Difficulty ramp).
// Defined here (not in the tide block below) because a macro must precede its user.
#define ENDLESS_TIDE_DMG_STEP  3    // tide levels per +1% enemy shot damage past the 220 intensity cap
#define ENDLESS_TIDE_DMG_CAP   400  // absolute ceiling on the tide-boosted shot-damage percent (sanity backstop)

// Enemy shot-DAMAGE multiplier (100 = normal): +1.75% per (effective) level; DEVASTATING +75%.
// Capped lower than the others, then the tide resumes a SLOW climb (notes.md §Difficulty ramp).
int endlessShotDamagePercent(void)
{
	int pct = 100 + endlessEffectiveDepth() * 7 / 4;
	if (endlessActiveMods & ENDLESS_MOD_DEVASTATING)
		pct += 75;
	if (pct > 220)
		pct = 220;
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
static int endlessNaturalEliteChancePercent(void)
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
static bool endlessEliteBoonsUnlocked(void)
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
static char endlessLastSpecialName[31] = "";

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

// --- Time-based & player-side modifiers -----------------------------------------
// endlessZoneTicks / endlessTurbodriveTimer live up top; advanced by endlessGameplayTick.

void endlessGameplayTick(void)
{
	if (!endlessMode)
		return;
	++endlessZoneTicks;

	// Overheat (gamble deal): the reactor runs hot -- shed 1 hull every ~80 ticks (a hair faster than
	// a single Nanorepair stack can mend). Floored at 1 so the DoT itself never lands the killing blow
	// (that keeps it clear of the death/revive path in varz.c); it just steadily bleeds you toward
	// one-hit-from-death while your guns run wild. Cleared next sector when the OVERHEAT bit lapses.
	if ((endlessActiveMods & ENDLESS_MOD_OVERHEAT) && player[0].armor > 1 && (endlessZoneTicks % 80) == 0)
	{
		--player[0].armor;
		endlessArmorHudDirty = true;  // JE_drawArmor is event-driven -- ask the game loop to repaint the bar (mainint.c)
	}

	if (endlessTurbodriveTimer > 0)
	{
		--endlessTurbodriveTimer;
		if (endlessTurbodriveTimer == 0)
		{
			endlessOverdriveStacks = 0;  // the kill-fire window lapsed -> lose the Overdrive stacks
			endlessComboKills = 0;        // and the combo resets -- back to the base 2x on the next kill
		}
	}

	// Nanorepair perk: regenerate 1 armor every so often (interval shortens with more stacks).
	if (endlessPerkOwned[PERK_REGEN] > 0)
	{
		if (++endlessRegenTick >= ENDLESS_PERK_REGEN_TICKS / endlessPerkOwned[PERK_REGEN])
		{
			endlessRegenTick = 0;
			if (player[0].armor < player[0].initial_armor)
				++player[0].armor;
		}
	}
}

// True (once) if the Overheat DoT just shaved a point of hull this tick; the game loop uses it to
// repaint the armor bar, which is otherwise only redrawn on a damage/special/setup event.
bool endlessConsumeArmorHudDirty(void)
{
	const bool dirty = endlessArmorHudDirty;
	endlessArmorHudDirty = false;
	return dirty;
}

bool endlessTurbodriveActive(void)
{
	return endlessMode && endlessTurbodriveTimer > 0;
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
static void endlessRollGravityDir(void)
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

// Cash multiplier (100 = unchanged) from the Scavenger perk, applied to the clear bonus and
// elite/champion bounties.
static int endlessPerkCashPercent(void)
{
	return 100 + endlessPerkOwned[PERK_CASH] * ENDLESS_PERK_CASH_PCT;
}

// +max armor from the Ablative Plating perk; added to the ship's armor each level start (varz.c),
// alongside the outpost hull upgrade (endlessArmorBonus).
int endlessPerkArmorBonus(void)
{
	int bonus = endlessPerkOwned[PERK_ARMOR] * ENDLESS_PERK_ARMOR_STEP;
	if (endlessPerkOwned[PERK_GLASSCANNON])
		bonus -= ENDLESS_PERK_GLASS_ARMOR;  // Glass Cannon relic drawback (varz.c clamps armor >= 1)
	return bonus;
}

// Rapid Cyclers perk: extra shotRepeat decrements this tick, as a smooth fractional rate via an
// accumulator (like the scroll-step boost). Applied every tick from the player fire block.
int endlessPerkFireDecrements(void)
{
	static int accum = 0;
	if (!endlessMode)
	{
		accum = 0;
		return 0;
	}
	int rate = endlessPerkOwned[PERK_FIRERATE] * ENDLESS_PERK_FIRE_PCT;
	// Adrenaline relic: a big extra boost while armor is below 1/N of the ship's max.
	if (endlessPerkOwned[PERK_ADRENALINE] > 0 && player[0].initial_armor > 0
	    && player[0].armor * ENDLESS_PERK_ADRENALINE_HP < player[0].initial_armor)
		rate += endlessPerkOwned[PERK_ADRENALINE] * ENDLESS_PERK_ADRENALINE_PCT;
	if (rate == 0)
	{
		accum = 0;
		return 0;
	}
	accum += rate;
	int steps = accum / 100;
	accum -= steps * 100;
	return steps;
}

// Rapid Recharge perk: extra cooldown decrements/tick (fractional accumulator). The caller
// applies them to the special-fire gate AND the sidekick ammo refill -- not the main guns.
int endlessPerkSpecialCooldownDecrements(void)
{
	static int accum = 0;
	if (!endlessMode || endlessPerkOwned[PERK_SPECIALCD] == 0)
	{
		accum = 0;
		return 0;
	}
	accum += endlessPerkOwned[PERK_SPECIALCD] * ENDLESS_PERK_SPECIALCD_PCT;
	int steps = accum / 100;
	accum -= steps * 100;
	return steps;
}

// Autofire Special perk: while owned, the equipped special weapon fires on its own as long as the
// main fire button is held -- the run-persistent equivalent of the debug "Autofire Special" toggle.
// Read in varz.c's special-fire path, OR'd with the debug autoFireSpecial global.
bool endlessPerkAutoFireSpecial(void)
{
	return endlessMode && endlessPerkOwned[PERK_AUTOSPECIAL] > 0;
}

// Efficient Coils perk: power-use scale per main-weapon shot (100 = normal, lower = cheaper);
// applied in shots.c player_shot_create. Floored so firing is never entirely free.
int endlessPerkPowerUsePercent(void)
{
	if (!endlessMode)
		return 100;
	int pct = 100 - endlessPerkOwned[PERK_POWERUSE] * ENDLESS_PERK_POWERUSE_PCT;
	return pct < 20 ? 20 : pct;
}

// Shield Matrix perk: shortens the shield-regen interval from `base` (tyrian2.c), floored; a
// no-op outside endless / with no stacks. A quicker shield still drains the generator quicker.
int endlessPerkShieldWait(int base)
{
	if (!endlessMode || endlessPerkOwned[PERK_SHIELDREGEN] == 0)
		return base;
	int wait = base - endlessPerkOwned[PERK_SHIELDREGEN] * ENDLESS_PERK_SHIELDRGN_STEP;
	return wait < ENDLESS_PERK_SHIELDRGN_MIN ? ENDLESS_PERK_SHIELDRGN_MIN : wait;
}

// Rapid Charger perk: shortens the charge-sidekick charge interval from `base` (mainint.c),
// floored; a no-op outside endless / with no stacks.
int endlessPerkChargeTicks(int base)
{
	if (!endlessMode || endlessPerkOwned[PERK_CHARGERATE] == 0)
		return base;
	int t = base - endlessPerkOwned[PERK_CHARGERATE] * ENDLESS_PERK_CHARGE_STEP;
	return t < ENDLESS_PERK_CHARGE_MIN ? ENDLESS_PERK_CHARGE_MIN : t;
}

// High-Velocity Rounds perk: shot travel-speed scale (100 = normal), applied in shots.c
// player_shot_create to the genuine shot velocities. A no-op outside endless / with no stacks.
int endlessPerkShotSpeedPercent(void)
{
	if (!endlessMode)
		return 100;
	return 100 + endlessPerkOwned[PERK_SHOTSPEED] * ENDLESS_PERK_SHOTSPEED_PCT;
}

// Roll this shop visit's perk offers: up to 3 distinct perks that aren't already maxed out.
// Called once per post-zone shop (endlessBetweenLevels), before the perk menu is shown.
void endlessGeneratePerkChoices(void)
{
	int pool[PERK_COUNT], n = 0;
	for (int i = 0; i < PERK_COUNT; ++i)
		if (endlessPerkOwned[i] < endlessPerkTable[i].maxStack)
			pool[n++] = i;

	// Partial Fisher-Yates: shuffle the first min(3, n) slots and take them.
	endlessPerkChoiceN = n < 3 ? n : 3;
	for (int i = 0; i < endlessPerkChoiceN; ++i)
	{
		int j = i + (int)(endlessRand() % (unsigned)(n - i));
		int t = pool[i]; pool[i] = pool[j]; pool[j] = t;
		endlessPerkChoice[i] = pool[i];
	}
}

int endlessPerkChoiceCount(void)
{
	return endlessPerkChoiceN;
}

const char *endlessPerkChoiceName(int i)
{
	if (i < 0 || i >= endlessPerkChoiceN)
		return "";
	return endlessPerkTable[endlessPerkChoice[i]].name;
}

// Help-line text for an offered perk: its description plus how many the player already owns.
const char *endlessPerkChoiceDesc(int i)
{
	static char buf[80];
	if (i < 0 || i >= endlessPerkChoiceN)
		return "";
	const int id = endlessPerkChoice[i];
	snprintf(buf, sizeof(buf), "%s  (Owned: %d)", endlessPerkTable[id].desc, endlessPerkOwned[id]);
	return buf;
}

// Acquire offered perk i. The forced post-zone pick is FREE (perks come sparingly -- see the cadence
// gate in endlessBetweenLevels); the paid path is the E-Shop "Buy Extra Perk", which charges up front
// in endlessTryBuyExtraPerk before opening this menu.
void endlessTakePerk(int i)
{
	if (i < 0 || i >= endlessPerkChoiceN)
		return;
	const int id = endlessPerkChoice[i];
	if (endlessPerkOwned[id] < endlessPerkTable[id].maxStack)
		++endlessPerkOwned[id];
	endlessPerkDepthDone = endlessRunDepth;  // this zone's perk is resolved (survives a save/reload)
}

// Cash paid for declining the perk ("take the cash"), scaling with depth so it stays tempting.
long endlessPerkDeclineBonus(void)
{
	return 1000 + (long)endlessRunDepth * 200;
}

void endlessDeclinePerk(void)
{
	player[0].cash += endlessPerkDeclineBonus();
	endlessPerkDepthDone = endlessRunDepth;  // this zone's perk is resolved (survives a save/reload)
}

// Perk registry accessors, used by the endless debug screen to list / toggle / stack perks.
int         endlessPerkCount(void)          { return PERK_COUNT; }
const char *endlessPerkName(int id)         { return (id >= 0 && id < PERK_COUNT) ? endlessPerkTable[id].name : ""; }
const char *endlessPerkDesc(int id)         { return (id >= 0 && id < PERK_COUNT) ? endlessPerkTable[id].desc : ""; }
int         endlessPerkMaxStack(int id)     { return (id >= 0 && id < PERK_COUNT) ? endlessPerkTable[id].maxStack : 0; }
int         endlessPerkGetOwned(int id)     { return (id >= 0 && id < PERK_COUNT) ? endlessPerkOwned[id] : 0; }

void endlessPerkSetOwned(int id, int n)
{
	if (id < 0 || id >= PERK_COUNT)
		return;
	if (n < 0)
		n = 0;
	if (n > endlessPerkTable[id].maxStack)
		n = endlessPerkTable[id].maxStack;
	endlessPerkOwned[id] = (JE_byte)n;
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

// Starting cash for a fresh run, by chosen difficulty (easier = more to spend).
long endlessStartingCash(void)
{
	switch (difficultyLevel)
	{
	case DIFFICULTY_WIMP:       return 20000;
	case DIFFICULTY_EASY:       return 15000;
	case DIFFICULTY_NORMAL:     return 11000;
	case DIFFICULTY_HARD:       return  8000;
	case DIFFICULTY_IMPOSSIBLE: return  6000;
	default:                    return  4000;  // Insanity and beyond
	}
}

// --- Mutators -------------------------------------------------------------------
// Payout/help registry: per-bit clear-cash `reward` (tenths of the base) + monitor `word`.
// Generation lives in endlessGenerateCourses; every bit a course can carry is listed here.
typedef struct {
	Uint64      bit;
	short       reward;    // clear-cash reward in TENTHS of the base (10 = 1.0x base; may be < 0)
	const char *word;      // short phrase for the generated help line
} EndlessMod;

static const EndlessMod endlessModTable[] = {
	// -- hostile --  (`word` is the Chart-a-Course monitor label: a plain WHAT-IT-DOES phrase,
	//                 not flavor -- "more enemy HP", not "tough hulls". Keep each under ~105px
	//                 in TINY_FONT (roughly 20 chars) so it fits the monitor's 113px columns;
	//                 game_menu.c anchors threats top-left and boons bottom-right)
	{ ENDLESS_MOD_FORTIFIED,     10, "more enemy HP" },
	{ ENDLESS_MOD_FRENZY,        10, "faster enemy fire" },
	{ ENDLESS_MOD_SWIFT,          8, "faster enemy shots" },
	{ ENDLESS_MOD_DEVASTATING,   10, "harder enemy hits" },
	{ ENDLESS_MOD_ENRAGE,        10, "enemy fire rate climbs" },
	{ ENDLESS_MOD_GRAVITY,        8, "downward pull" },
	{ ENDLESS_MOD_ELITEPACK,     20, "half enemies elite" },
	{ ENDLESS_MOD_OVERCLOCK,     10, "faster enemy attacks" },  // fire rate + shot speed; ALSO speeds the scroll -- the monitor adds a separate scroll row (endlessCourseModRows) so "+ fire" ambiguity never returns
	{ ENDLESS_MOD_SLIPSTREAM,     6, "faster scrolling" },      // the level rushes at you -- less reaction time
	{ ENDLESS_MOD_KAMIKAZE,      12, "enemies home in" },   // moderate homing, NO ram -- the mid sector tier (what Homing used to be)
	{ ENDLESS_MOD_HOMING,         6, "light homing" },      // the gentlest homing tier -- enemies barely lean toward you
	{ ENDLESS_MOD_RAMPAGE,       50, "enemies ram you" },   // gamble-only brutal Kamikaze: strong homing + extra ram damage (top-tier danger weight)
	{ ENDLESS_MOD_OVERLOAD,      15, "extreme enemy attacks" },
	{ ENDLESS_MOD_APEX,          40, "all enemies elite" },
	{ ENDLESS_MOD_LEGION,        50, "all champion enemies" },
	{ ENDLESS_MOD_WARP,          20, "much faster scrolling" },  // Slipstream cranked way up (rare injected)
	{ ENDLESS_MOD_BACKFIRE, 12, "kills jam your guns" },
	{ ENDLESS_MOD_BURNOUT,   18, "kills weaken guns" },
	{ ENDLESS_MOD_MISFIRE,   14, "kills cut your damage" },
	{ ENDLESS_MOD_OVERHEAT,  14, "hull burns over time" },  // the reactor cooks you (gamble deal + rare Redline sector)
	{ ENDLESS_MOD_TOPSY,     10, "upside-down view" },      // the playfield flips; controls invert with it (boss-style) -- a pure disorientation tax
	{ ENDLESS_MOD_SLUGGISH,  15, "your ship slowed" },      // ship + mouse/touch crawl -- half the reach to dodge with
	{ ENDLESS_MOD_SHIELDLESS, 12, "no shield regen" },      // shields never recharge -- once spent, you fly on armor
	{ ENDLESS_MOD_DEADGEN,   30, "generator dead" },        // no shield regen AND the main gun is starved of power (super-rare)
	// The 100th-zone finale marker. A NULL word means "no monitor row and no help-line phrase": it is
	// a label, not a mechanic, so the threat list stays purely the sector's real dangers. The reward
	// IS the finale bounty (roughly 15x the base clear on its own), and since the danger score sums
	// the same table it also guarantees the sector outranks everything else on the slate.
	{ ENDLESS_MOD_THEEND,   150, NULL },
	// -- boons: they HELP you, so little/no cash (a couple pay big instead) --
	{ ENDLESS_MOD_FRAGILE,       -5, "less enemy HP" },
	{ ENDLESS_MOD_TURBODRIVE,      0, "kills quicken guns" },
	{ ENDLESS_MOD_OVERCHARGE,     0, "more weapon damage" },
	{ ENDLESS_MOD_DILATION,       0, "slower enemy shots" },
	{ ENDLESS_MOD_FAVOR,          0, "cheaper next shop" },
	{ ENDLESS_MOD_OVERDRIVE,   0, "kills stack firepower" },
	{ ENDLESS_MOD_OVERBLAST,   0, "kills stack damage" },
	{ ENDLESS_MOD_BOUNTY,        30, "big cash payout" },
	{ ENDLESS_MOD_CURSED,        40, "cash now, empty shop" },
	{ ENDLESS_MOD_NOCHAMP,        0, "no champion enemies" },     // no clear-cash: the boon already costs you the elite/champion bounties
	{ ENDLESS_MOD_NOELITE,        0, "no elites or champions" },  // (same -- these thin the very enemies that pay the fat bounties)
};

// The base level-clear reward, before per-modifier bonuses. It scales up with the run so the
// payout stays a meaningful supplement against the depth-inflated shop prices (weapons run
// ~5k-30k, upgrades far more); every modifier reward is a multiple of this. Capped so a very
// deep run can't mint an absurd single payout.
static long endlessClearBase(void)
{
	long base = 900 + (long)endlessRunDepth * 220;
	return (base > 60000) ? 60000 : base;
}

// Clear payout for an ARBITRARY modifier set at the current depth: the base plus the summed
// per-modifier reward (each in tenths of the base). Used both to pay out (endlessClearBonus)
// and to SHOW the payout on a course's help line (endlessComboHelp) -- so the two can't disagree.
static long endlessClearBonusFor(Uint64 mods)
{
	const long base = endlessClearBase();
	long tenths = 0;
	for (unsigned i = 0; i < COUNTOF(endlessModTable); ++i)
		if (mods & endlessModTable[i].bit)
			tenths += endlessModTable[i].reward;
	const long total = base + base * tenths / 10;
	const long floor = base / 4;
	return (total > floor) ? total : floor;  // always a minimum reward
}

// Cash paid on CLEARING a level -- the base plus whatever the active sector's mutators add.
// Taking the harder route pays off.
static long endlessClearBonus(void)
{
	return endlessClearBonusFor(endlessActiveMods);
}

// --- Shop stock -----------------------------------------------------------------

// Whether an item id is a real, buyable item in its category — filters out the
// blank/placeholder slots and the reserved custom-weapon ("Test") ports.
static bool endlessItemBuyable(JE_byte costType, int id)
{
	const char *name = NULL;
	switch (costType)
	{
	case 2: name = ships[id].name;    break;  // ship
	case 3:                                    // front weapon
	case 4:                                    // rear weapon
		if (id == customWeaponPort)
			return false;
		name = weaponPort[id].name;
		if (SDL_strncasecmp(name, "Test", 4) == 0)
			return false;
		break;
	case 5: name = shields[id].name;  break;  // shield
	case 6: name = powerSys[id].name; break;  // generator
	case 7:                                    // left sidekick
	case 8:                                    // right sidekick
		if (id == customSidekickSlot)
			return false;
		name = options[id].name;
		if (strncmp(name, "None", 4) == 0)
			return false;
		break;
	}
	// Data section-divider placeholders (e.g. "Miscellaneous Option Weapons") aren't real
	// buyable items -- keep them out of the shop.
	if (name != NULL && SDL_strncasecmp(name, "Miscellaneous", 13) == 0)
		return false;
	return name != NULL && name[0] != '\0';
}

// True if id is in the exclude list -- used to keep a weapon that already appears in one
// menu (e.g. front weapons) from being repeated in the paired menu (rear weapons).
static bool endlessIdExcluded(const JE_byte *exclude, int excludeCount, int id)
{
	for (int k = 0; k < excludeCount; ++k)
		if (exclude[k] == id)
			return true;
	return false;
}

// Fill one shop category (an itemAvail[] row) with a random, shuffled selection of ids -- any
// buyable item can appear at any depth (no price or rarity preference). Any id in exclude[]
// (excludeCount entries) is held back so paired menus don't show the same item twice; pass
// NULL/0 for categories with no pairing.
static void endlessFillCategory(int availIdx, JE_byte costType, int idMax, bool allowNone, int curItem, const JE_byte *exclude, int excludeCount)
{
	const int want = 5;  // items shown per category: None + 4 where None is allowed, else 5
	int n = 0;

	if (allowNone)
		itemAvail[availIdx][n++] = 0;

	// Always include the currently-equipped item so JE_itemScreen doesn't append it as an
	// extra (6th) row -- it auto-adds the equipped item whenever it isn't already offered.
	if (curItem > 0)
	{
		bool present = false;
		for (int k = 0; k < n; ++k)
			if (itemAvail[availIdx][k] == curItem)
				present = true;
		if (!present)
			itemAvail[availIdx][n++] = curItem;
	}

	// Build the pool of every buyable, non-excluded id not already placed (the equipped item,
	// and for rear weapons everything the front row already offered, are held out here). idMax
	// never exceeds PORT_NUM across the call sites, so PORT_NUM+1 slots always suffice.
	JE_byte pool[PORT_NUM + 1];
	int poolN = 0;
	for (int id = 1; id <= idMax && poolN < (int)COUNTOF(pool); ++id)
	{
		if (!endlessItemBuyable(costType, id))
			continue;
		if (endlessIdExcluded(exclude, excludeCount, id))
			continue;
		bool dup = false;
		for (int k = 0; k < n; ++k)
			if (itemAvail[availIdx][k] == id)
				dup = true;
		if (!dup)
			pool[poolN++] = (JE_byte)id;
	}

	// Shuffle the pool (Fisher-Yates on the structural stream) and take the first `want` -- a
	// genuinely random selection every visit, no price or rarity preference.
	for (int i = poolN - 1; i > 0; --i)
	{
		const int j = (int)(endlessRand() % (Uint32)(i + 1));
		const JE_byte t = pool[i]; pool[i] = pool[j]; pool[j] = t;
	}
	for (int i = 0; i < poolN && n < want; ++i)
		itemAvail[availIdx][n++] = pool[i];

	itemAvailMax[availIdx] = n;
}

// Randomize the whole shop inventory (uniformly random per visit). Called on entering the
// outpost and on each reroll.
static void endlessFillShop(void)
{
	memset(itemAvail, 0, sizeof(itemAvail));
	memset(itemAvailMax, 0, sizeof(itemAvailMax));

	// Cursed Bounty: the outpost is barren -- every category stays empty this visit.
	if (endlessActiveMods & ENDLESS_MOD_CURSED)
		return;

	// Seed each category with the player's LIVE equipped item (player[0].items), never the stale
	// last_items, so a reroll keeps whatever is on the ship (notes.md §Save / resume).
	const PlayerItems *it = &player[0].items;

	// Front and rear share one id pool: fill front first (holding back the equipped rear), then
	// rear excluding the front row, so no weapon id lands in both menus at once.
	const JE_byte rearEquip = it->weapon[REAR_WEAPON].id;

	// itemAvail rows per category (see itemAvailMap in game_menu.c): 0 ships, 1 front,
	// 2 rear, 3 generator, 5 left sidekick, 6 right sidekick, 8 shield.
	endlessFillCategory(0, 2, SHIP_NUM,   false, it->ship,                        NULL, 0);  // ships
	endlessFillCategory(1, 3, ENDLESS_REAL_WEAPON_PORTS, false, it->weapon[FRONT_WEAPON].id, &rearEquip, rearEquip > 0 ? 1 : 0);  // front weapons (skip equipped rear)
	endlessFillCategory(2, 4, ENDLESS_REAL_WEAPON_PORTS, true,  it->weapon[REAR_WEAPON].id,  itemAvail[1], itemAvailMax[1]);  // rear (+None), no dupes vs front
	endlessFillCategory(3, 6, POWER_NUM,  false, it->generator,                   NULL, 0);  // generators
	endlessFillCategory(5, 7, OPTION_NUM, true,  it->sidekick[LEFT_SIDEKICK],     NULL, 0);  // left sidekick (+None)
	endlessFillCategory(6, 8, OPTION_NUM, true,  it->sidekick[RIGHT_SIDEKICK],    NULL, 0);  // right sidekick (+None)
	endlessFillCategory(8, 5, SHIELD_NUM, false, it->shield,                      NULL, 0);  // shields
}

// --- Zone milestones ------------------------------------------------------------

// One flavour line per 5-zone band (index = zone / 5), shown on the run-end summary screen.
// The tone escalates from ominous to apocalyptic as the zone climbs. Clamps to the last line
// past the end of the list.
static const char *endlessMilestoneLine(int d)
{
	static const char *const lines[] = {
		"The gate seals shut behind you.",   //   0-4
		"The zones grow aware of you.",       //   5
		"The enemy has marked your name.",    //  10
		"A dark tide rises to meet you.",     //  15
		"No signal reaches home from here.",  //  20
		"The swarm hungers, and multiplies.", //  25
		"Hostile stars wheel overhead.",      //  30
		"Beyond every chart ever drawn.",     //  35
		"The enemy tide has no ebb.",         //  40
		"Every zone bleeds a little more.",   //  45
		"Few living souls have come this far.", //  50
		"The stars themselves grow cold.",    //  55
		"A dreadful hush between the guns.",  //  60
		"The void hums with ancient malice.", //  65
		"The abyss has turned to stare back.", //  70
		"Your name is forgotten out here.",   //  75
		"Only the guns remember you now.",    //  80
		"The dark has grown teeth.",          //  85
		"Past where reason dares to follow.", //  90
		"The hull screams, and still holds.", //  95
		"Legends come this far to die.",      // 100
		"The enemy pours out of nowhere.",    // 105
		"Reality frays along the seams.",     // 110
		"The stars burn wrong out here.",     // 115
		"No light was meant to reach here.",  // 120
		"The swarm goes on without end.",     // 125
		"You are their ghost story now.",     // 130
		"The abyss forgets its own floor.",   // 135
		"Time itself loses the thread.",      // 140
		"The last known star winks out.",     // 145
		"Beyond the beyond, and climbing.",   // 150
		"These zones should not exist.",      // 155
		"The void has learned your name.",    // 160
		"Still it grows. Still you press on.", // 165
		"No rescue was ever coming.",         // 170
		"The end is a place. You near it.",   // 175
		"Further now than death itself.",     // 180
		"The dark is all that remains.",      // 185
		"You were never meant to reach here.", // 190
		"And still the zones unfold.",        // 195
		"Two hundred zones of ruin behind.",  // 200
		"The nightmare has no far edge.",     // 205
		"No sane soul flies these zones.",    // 210
		"The guns glow white with wrath.",    // 215
		"The void itself recoils from you.",  // 220
		"You are the terror they flee.",      // 225
		"Beyond every legend ever told.",     // 230
		"Even the dark runs out of dark.",    // 235
		"The final zones of creation.",       // 240
		"One breath from the end of all.",    // 245
		"The last dark. Press on.",		      // 250+ (final change)
	};
	int i = d / 5;
	if (i < 0)
		i = 0;
	if (i >= (int)COUNTOF(lines))
		i = (int)COUNTOF(lines) - 1;
	return lines[i];
}

// --- Outpost economy: reroll / hull upgrade -------------------------------------
// These two actions REPLACE the Data Cubes and Ship Specs entries in JE_itemScreen's
// own front menu when endlessMode is on (see game_menu.c), so the economy reuses the
// in-game shop rather than a bespoke screen. Prices escalate per use, reset each visit.

void endlessResetShopPrices(void)
{
	endlessRerollCost = 6000 + (long)endlessRunDepth * 1000;
	endlessHullCost   = 15000 + endlessRunDepth * 2000;
	endlessBombCost   = 2500 + (long)endlessRunDepth * 400;
	endlessExtraPerkCost = 70000 + (long)endlessRunDepth * 2500;  // EXTREME: ~100k with 1 perk owned (x1.4 surcharge); extra perks are a rare luxury on top of the free post-zone picks
	endlessCleanseCost = 25000 + (long)endlessRunDepth * 2500;
	endlessCleanseChargeCount = 0;  // fresh visit: no pending sabotage strips carried in
	endlessGambleMsg[0] = '\0';
	endlessPurchasedMods = 0;   // fresh visit: no pending buff
	endlessBuffKind = 0;        // a kill-fire buff (Turbodrive or Overdrive) can be bought again
	endlessLastSpecialName[0] = '\0';             // no special bought yet this visit
	endlessShopEntryCash = (long)player[0].cash;  // freeze the cash-fraction buy prices for this visit
}

long endlessRerollPrice(void) { return endlessRerollCost; }
int  endlessHullPrice(void)   { return endlessHullCost; }
bool endlessHullMaxed(void)   { return endlessArmorBonus >= endlessHullMax(); }

// E-Shop cash-fraction buys, all priced off the shop-entry cash and applied to the NEXT sector.
// Only one of the three kill-fire buffs per visit; Buy Special is a single premium buy.
long endlessTurbodrivePrice(void)       { return endlessShopEntryCash * 2 / 3; }    // 66%
long endlessOverblastPrice(void) { return endlessShopEntryCash * 3 / 4; }   // 75% (Overblast)
long endlessOverdrivePrice(void) { return endlessShopEntryCash * 19 / 20; } // 95% (Overdrive)
long endlessSpecialPrice(void)    { return endlessShopEntryCash * 4 / 5; }    // 80%
bool endlessBuffBought(void)      { return endlessBuffKind != 0; }
int  endlessBuffKindBought(void)  { return endlessBuffKind; }

// Kill-fire buff recharge. After any Turbodrive/Overblast/Overdrive buy, all three lock until the
// run reaches endlessBuffCooldownUntil, so a cash-rich late run can't buy one every sector. The
// lock length (in sectors) scales with depth: base 2, +1 for every full 20 zones past depth 50
// (2 early, 3 by zone 70, 4 by zone 90, ...). Stored at purchase time as an absolute unlock depth,
// so it can't drift as the run deepens.
#define ENDLESS_BUFF_COOLDOWN_BASE        2
#define ENDLESS_BUFF_COOLDOWN_RAMP_START 50
#define ENDLESS_BUFF_COOLDOWN_RAMP_STEP  20
static int endlessBuffCooldownLength(void)
{
	int n = ENDLESS_BUFF_COOLDOWN_BASE;
	if (endlessRunDepth > ENDLESS_BUFF_COOLDOWN_RAMP_START)
		n += (endlessRunDepth - ENDLESS_BUFF_COOLDOWN_RAMP_START) / ENDLESS_BUFF_COOLDOWN_RAMP_STEP;
	return n;
}
static void endlessArmBuffCooldown(void) { endlessBuffCooldownUntil = endlessRunDepth + endlessBuffCooldownLength(); }

bool endlessBuffOnCooldown(void)   { return endlessRunDepth < endlessBuffCooldownUntil; }
int  endlessBuffCooldownLeft(void) { int d = endlessBuffCooldownUntil - endlessRunDepth; return (d > 0) ? d : 0; }

bool endlessTryBuyTurbodrive(void)  // Turbodrive
{
	long cost = endlessTurbodrivePrice();
	if (endlessBuffKind != 0 || endlessBuffOnCooldown() || cost < 1 || player[0].cash < (ulong)cost)
		return false;
	player[0].cash -= cost;
	endlessBuffCharge = endlessBuffChargeFromPaid(cost);  // bigger spend -> longer window + more damage
	endlessPurchasedMods = (endlessPurchasedMods & ~ENDLESS_MOD_KILLFIRE_ANY) | ENDLESS_MOD_TURBODRIVE;  // OR'd into the next sector in endlessSelectCourse; one kill-fire effect at a time (clears any gambled curse)
	endlessBuffKind = 1;
	endlessArmBuffCooldown();  // lock all three kill-fire buys for the scaled recharge
	return true;
}

bool endlessTryBuyOverdrive(void)  // Overdrive: Turbodrive + Overblast fire-rate/damage stacks
{
	long cost = endlessOverdrivePrice();
	if (endlessBuffKind != 0 || endlessBuffOnCooldown() || cost < 1 || player[0].cash < (ulong)cost)
		return false;
	player[0].cash -= cost;
	endlessBuffCharge = endlessBuffChargeFromPaid(cost);  // bigger spend -> longer window + more damage
	endlessPurchasedMods = (endlessPurchasedMods & ~ENDLESS_MOD_KILLFIRE_ANY) | ENDLESS_MOD_OVERDRIVE;  // implies the base kill-fire window (see endlessCountKill); one kill-fire effect at a time (clears any gambled curse)
	endlessBuffKind = 2;
	endlessArmBuffCooldown();  // lock all three kill-fire buys for the scaled recharge
	return true;
}

bool endlessTryBuyOverblast(void)  // Overblast: Overdrive's per-kill DAMAGE stacks only -- no fire boost
{
	long cost = endlessOverblastPrice();
	if (endlessBuffKind != 0 || endlessBuffOnCooldown() || cost < 1 || player[0].cash < (ulong)cost)
		return false;
	player[0].cash -= cost;
	endlessBuffCharge = endlessBuffChargeFromPaid(cost);  // bigger spend -> longer window + more damage
	endlessPurchasedMods = (endlessPurchasedMods & ~ENDLESS_MOD_KILLFIRE_ANY) | ENDLESS_MOD_OVERBLAST;  // one kill-fire effect at a time (clears any gambled curse)
	endlessBuffKind = 3;
	endlessArmBuffCooldown();  // lock all three kill-fire buys for the scaled recharge
	return true;
}

// The kill-fire buff bits bought this shop visit but not yet applied (endlessSelectCourse ORs
// them into the next sector). Exposed so the debug level-select can fold them in too.
unsigned endlessPendingMods(void) { return endlessPurchasedMods; }

bool endlessTryBuySpecial(void)
{
	long cost = endlessSpecialPrice();
	if (cost < 1 || player[0].cash < (ulong)cost)
		return false;
	player[0].cash -= cost;
	endlessGrantSpecial();  // equips a random valid special (+ HUD message in-level)
	return true;
}

// --- Buy Bomb (F2): +1 to the superbomb stockpile (the AST. CITY collectable bomb). -------------
long endlessBombPrice(void) { return endlessBombCost; }
bool endlessBombFull(void)  { return player[0].superbombs >= 10; }
bool endlessTryBuyBomb(void)
{
	if (player[0].superbombs >= 10 || player[0].cash < (ulong)endlessBombCost)
		return false;
	player[0].cash -= endlessBombCost;
	++player[0].superbombs;
	endlessBombCost = endlessBombCost * 3 / 2;  // escalate within the visit so a full restock costs more
	return true;
}

// --- Revive token (F3): survive one lethal hit, full-restore (die = run over otherwise). In a
// permadeath run this cheats death outright, so it's priced STUPIDLY high -- a rare, run-defining
// splurge -- and still doubles per revive already spent this run so it never becomes immortality. ---
long endlessRevivePrice(void)
{
	const int steps = endlessRevivesUsed > 5 ? 5 : endlessRevivesUsed;
	return (150000 + (long)endlessRunDepth * 10000) * (1 << steps);
}
bool endlessReviveArmed(void) { return endlessReviveHeld; }
bool endlessTryBuyRevive(void)
{
	const long cost = endlessRevivePrice();
	if (endlessReviveHeld || player[0].cash < (ulong)cost)
		return false;
	player[0].cash -= cost;
	endlessReviveHeld = true;
	return true;
}
// Consume a held revive at the moment of death: true = the player survives (caller clears the screen).
bool endlessConsumeRevive(void)
{
	if (!endlessMode || !endlessReviveHeld)
		return false;
	endlessReviveHeld = false;
	++endlessRevivesUsed;
	player[0].armor = player[0].initial_armor;  // full hull restore
	return true;
}

// --- Extra Perk (F4): pay for a bonus perk pick; the E-Shop dispatch opens the perk menu after. -
// Total perk stacks the player currently holds (summed across every perk); drives the owned-count
// surcharge below so a bigger collection costs more to grow.
static int endlessPerkTotalOwned(void)
{
	int total = 0;
	for (int i = 0; i < PERK_COUNT; ++i)
		total += endlessPerkOwned[i];
	return total;
}
long endlessExtraPerkPrice(void)
{
	// Base = depth price, doubled per buy this visit (endlessExtraPerkCost). Surcharge = a capped
	// per-owned-perk % on top, recomputed live so it climbs as you actually accumulate perks.
	int surcharge = endlessPerkTotalOwned() * ENDLESS_EXTRA_PERK_OWNED_PCT;
	if (surcharge > ENDLESS_EXTRA_PERK_OWNED_CAP)
		surcharge = ENDLESS_EXTRA_PERK_OWNED_CAP;
	return endlessExtraPerkCost * (100 + surcharge) / 100;
}
bool endlessTryBuyExtraPerk(void)
{
	const long price = endlessExtraPerkPrice();  // single source of truth: the same value shown in the E-Shop help line
	if (player[0].cash < (ulong)price)
		return false;
	player[0].cash -= price;
	endlessExtraPerkCost = endlessExtraPerkCost * 2;  // escalate the base steeply (perks are strong and bounded)
	endlessGeneratePerkChoices();                     // dispatch opens MENU_PERKS to pick one
	return true;
}

// --- Sabotage Sector (F5): buy "cleanse charges" that strip the worst mutator off the next chosen
// course (applied in endlessSelectCourse). Handy against a forced Ambush you can't route around. --
long endlessCleansePrice(void)   { return endlessCleanseCost; }
int  endlessCleanseCharges(void) { return endlessCleanseChargeCount; }
bool endlessTryBuyCleanse(void)
{
	if (endlessCleanseChargeCount >= 3 || player[0].cash < (ulong)endlessCleanseCost)
		return false;
	player[0].cash -= endlessCleanseCost;
	++endlessCleanseChargeCount;
	endlessCleanseCost = endlessCleanseCost * 2;
	return true;
}
// Strip the single most-dangerous hostile bit from a modifier set (one per cleanse charge).
static Uint64 endlessStripWorstMod(Uint64 mods)
{
	static const Uint64 order[] = {  // nastiest first
		ENDLESS_MOD_LEGION, ENDLESS_MOD_APEX, ENDLESS_MOD_DEADGEN, ENDLESS_MOD_RAMPAGE, ENDLESS_MOD_OVERLOAD,
		ENDLESS_MOD_WARP,  // extreme scroll -- right below Overload on the danger ladder
		ENDLESS_MOD_ELITEPACK, ENDLESS_MOD_DEVASTATING, ENDLESS_MOD_SHIELDLESS, ENDLESS_MOD_FORTIFIED, ENDLESS_MOD_FRENZY,
		ENDLESS_MOD_SLUGGISH, ENDLESS_MOD_SWIFT, ENDLESS_MOD_OVERCLOCK, ENDLESS_MOD_ENRAGE, ENDLESS_MOD_SLIPSTREAM,
		ENDLESS_MOD_GRAVITY | ENDLESS_MOD_GRAVITY_OMNI, ENDLESS_MOD_TOPSY,  // gravity + its omni flag strip together, so a sabotaged well is fully cleared (not left as an orphaned omni pull)
		ENDLESS_MOD_KAMIKAZE, ENDLESS_MOD_HOMING,  // the two mild homing tiers -- stripped last
	};
	for (unsigned i = 0; i < COUNTOF(order); ++i)
		if (mods & order[i])
			return mods & ~order[i];
	return mods;
}

// --- Gamble (F6): a flat depth-scaled fee for a random outcome. Wins span 0..51, pushes 52..61,
// and a long tail 62..99. Roughly neutral EV at high variance, but the tail is harsh: it can strip
// gun power, erase a perk, steal the special, halve cash, or mark the player for a deferred ambush.
// The wins stay worthwhile (a revive, a free perk, a hull tier, +gun power, a fat next-clear).
// Every branch reuses an existing lever: player items/perks, endlessArmorBonus, endlessReviveHeld,
// the shop-tax/rigged/long-con state above, and next-sector mod bits.
long endlessGamblePrice(void) { return 25000 + (long)endlessRunDepth * 2000; }  // steep: the gamble is a slot machine (some outcomes scale off this cost), priced so it can't be spam-pulled to fish for jackpots/free perks
const char *endlessGambleResult(void) { return endlessGambleMsg; }
bool endlessGambleWonPerk(void) { return endlessGamblePerkWon; }
// Clear the gamble-won-perk flag once its perk menu has been opened. Without this the flag stayed set
// after the pick, so every LATER successful E-Shop buy (Turbodrive/Overblast/Overdrive/...) saw it
// still true and wrongly re-opened the perk menu -- showing the stale, previously-offered choices.
void endlessClearGamblePerk(void) { endlessGamblePerkWon = false; }
int  endlessShopTaxPercent(void) { return endlessShopTax; }
// Every DISTINCT gamble outcome, as a stable ID. endlessTryGamble maps a random roll to one of these
// (preserving the weighted ladder + sub-rolls); the debug "Gamble Outcomes" page fires them straight
// by ID via endlessForceGambleOutcome. Keep endlessGambleOutcomeNames[] below in the same order.
enum {
	EGO_JACKPOT, EGO_WIN, EGO_REVIVE, EGO_PERK, EGO_HULL, EGO_OVERCLOCK, EGO_SPECIAL,
	EGO_ARSENAL, EGO_SECONDWIND, EGO_BLOODMONEY, EGO_OVERBLAST, EGO_OVERCHARGE, EGO_FAVOR, EGO_GOLDEN,
	EGO_DOUBLENOTHING, EGO_REFUND, EGO_NOTHING,
	EGO_LOANSHARK, EGO_NITRO, EGO_OVERHEAT, EGO_GLASSCANNON,
	EGO_MELTDOWN, EGO_STICKY, EGO_RUSTBUCKET, EGO_AMNESIA, EGO_DUD,
	EGO_SWINDLED, EGO_CURSE_JAM, EGO_CURSE_FAIL, EGO_CURSE_MISFIRE, EGO_CURSE_FRENZY,
	EGO_MARKED, EGO_LONGCON,
	EGO_ROBBED, EGO_DISARMED, EGO_PSYCH, EGO_RIGGED, EGO_CLEANED,
	EGO_RAMPAGE,  // ultra-rare (~1/5000): the original brutal Kamikaze -- rammers on the next sector
	EGO_MEGAJACKPOT,  // ultra-rare (~1/5000): the dream pull -- a flat, pile-independent +$1,000,000
	EGO_COUNT
};
static const char *const endlessGambleOutcomeNames[EGO_COUNT] = {
	"Jackpot (+5x)", "Win (+2x)", "Revive token", "Free perk pick", "Hull tier +", "Overclock gun +1", "Special weapon",
	"Arsenal (max bombs)", "Second wind (heal)", "Blood money", "Overblast next", "Overcharged next", "Merchant Favor", "Golden Touch",
	"Double or Nothing", "Refund fee", "Nothing (house wins)",
	"Loan Shark (+tax)", "Nitro deal", "Overheat deal", "Glass Cannon",
	"Meltdown (gun -1)", "Sticky Fingers", "Rustbucket", "Amnesia (-perk)", "Dud Arsenal",
	"Swindled (-20%)", "Curse: gun jam", "Curse: gun fail", "Curse: misfire", "Curse: frenzy",
	"Marked (boss+)", "The Long Con",
	"Robbed (-bomb)", "Disarmed (-revive)", "Jackpot Psych", "Rigged next pull", "Cleaned out (-50%)",
	"Kamikaze Rush", "Mega Jackpot (+$1M)",
};

// Apply a single outcome's EFFECT (no fee, no roll). Shared by the random gamble and the debug page,
// so the two can never drift. `cost` scales the cash payouts (the debug page passes the live price).
static void endlessApplyGambleOutcome(int id, long cost)
{
	switch (id)
	{
	case EGO_JACKPOT: { const long win = cost * 5; player[0].cash += win; snprintf(endlessGambleMsg, sizeof endlessGambleMsg, "JACKPOT!  +$%ld", win); break; }
	case EGO_MEGAJACKPOT: { const long win = 1000000; player[0].cash += win; snprintf(endlessGambleMsg, sizeof endlessGambleMsg, "MEGA JACKPOT!  +$%ld", win); break; }
	case EGO_WIN:     { const long win = cost * 2; player[0].cash += win; snprintf(endlessGambleMsg, sizeof endlessGambleMsg, "Win!  +$%ld", win); break; }
	case EGO_REVIVE:
		if (!endlessReviveHeld)
		{ endlessReviveHeld = true; SDL_strlcpy(endlessGambleMsg, "Won a REVIVE token!", sizeof endlessGambleMsg); }
		else
		{ const long win = cost * 4; player[0].cash += win; snprintf(endlessGambleMsg, sizeof endlessGambleMsg, "Revive held --  +$%ld", win); }
		break;
	case EGO_PERK:
		endlessGeneratePerkChoices();  // the E-Shop dispatch opens MENU_PERKS when endlessGambleWonPerk() is set
		if (endlessPerkChoiceCount() > 0)
		{ endlessGamblePerkWon = true; SDL_strlcpy(endlessGambleMsg, "Won a free perk pick!", sizeof endlessGambleMsg); }
		else
		{ const long win = cost * 3; player[0].cash += win; snprintf(endlessGambleMsg, sizeof endlessGambleMsg, "Perks maxed --  +$%ld", win); }
		break;
	case EGO_HULL:
		if (endlessArmorBonus < endlessHullMax())
		{ endlessArmorBonus += ENDLESS_HULL_STEP; snprintf(endlessGambleMsg, sizeof endlessGambleMsg, "Hull tier!  +%d armor", ENDLESS_HULL_STEP); }
		else
		{ const long win = cost * 3; player[0].cash += win; snprintf(endlessGambleMsg, sizeof endlessGambleMsg, "Hull maxed --  +$%ld", win); }
		break;
	case EGO_OVERCLOCK:  // +1 permanent front-gun power (the bait that makes Meltdown sting)
		if ((int)player[0].items.weapon[FRONT_WEAPON].power < 11)
		{ ++player[0].items.weapon[FRONT_WEAPON].power; SDL_strlcpy(endlessGambleMsg, "Overclocked!  +1 gun power", sizeof endlessGambleMsg); }
		else
		{ const long win = cost * 3; player[0].cash += win; snprintf(endlessGambleMsg, sizeof endlessGambleMsg, "Guns maxed --  +$%ld", win); }
		break;
	case EGO_SPECIAL: endlessGrantSpecial(); snprintf(endlessGambleMsg, sizeof endlessGambleMsg, "Won a special weapon! (%s)", endlessLastSpecialName); break;
	case EGO_ARSENAL:
	{
		int got = 0;
		while (player[0].superbombs < 10) { ++player[0].superbombs; ++got; }
		if (got > 0)
			snprintf(endlessGambleMsg, sizeof endlessGambleMsg, "Arsenal!  +%d bombs", got);
		else
		{ const long win = cost * 2; player[0].cash += win; snprintf(endlessGambleMsg, sizeof endlessGambleMsg, "Bombs full --  +$%ld", win); }
		break;
	}
	case EGO_SECONDWIND:
		player[0].armor = player[0].initial_armor;  // full hull restore (bonuses stack on top, as with Revive)
		SDL_strlcpy(endlessGambleMsg, "Second wind! Hull restored.", sizeof endlessGambleMsg);
		break;
	case EGO_BLOODMONEY:  // floor of a normal Win, plus a fat bonus the more wrecked your hull is
	{
		const int maxA = player[0].initial_armor > 0 ? player[0].initial_armor : 1;
		const int miss = maxA - (int)player[0].armor;
		const long win = cost * 2 + (long)cost * 3 * (miss > 0 ? miss : 0) / maxA;
		player[0].cash += win;
		snprintf(endlessGambleMsg, sizeof endlessGambleMsg, "Blood money!  +$%ld", win);
		break;
	}
	case EGO_OVERBLAST:
		endlessPurchasedMods = (endlessPurchasedMods & ~ENDLESS_MOD_KILLFIRE_ANY) | ENDLESS_MOD_OVERBLAST;
		SDL_strlcpy(endlessGambleMsg, "Overblast next sector!", sizeof endlessGambleMsg);
		break;
	case EGO_OVERCHARGE:
		endlessPurchasedMods |= ENDLESS_MOD_OVERCHARGE;
		SDL_strlcpy(endlessGambleMsg, "Overcharged next sector!", sizeof endlessGambleMsg);
		break;
	case EGO_FAVOR:
		endlessPurchasedMods |= ENDLESS_MOD_FAVOR;
		SDL_strlcpy(endlessGambleMsg, "Favor: cheap shop next!", sizeof endlessGambleMsg);
		break;
	case EGO_GOLDEN:
		endlessPurchasedMods |= ENDLESS_MOD_BOUNTY;
		SDL_strlcpy(endlessGambleMsg, "Golden touch: big clear next!", sizeof endlessGambleMsg);
		break;
	case EGO_DOUBLENOTHING:  // a straight coin-flip on your entire cash pile
		if (endlessRand() % 2)
		{
			if (player[0].cash > 1000000000UL)
				player[0].cash = 2000000000UL;   // clamp so the doubling can't wrap the counter
			else
				player[0].cash *= 2;
			SDL_strlcpy(endlessGambleMsg, "DOUBLED! The pile is yours.", sizeof endlessGambleMsg);
		}
		else
		{ player[0].cash = 0; SDL_strlcpy(endlessGambleMsg, "NOTHING. Wiped clean.", sizeof endlessGambleMsg); }
		break;
	case EGO_REFUND: player[0].cash += cost; SDL_strlcpy(endlessGambleMsg, "Machine jammed -- fee back.", sizeof endlessGambleMsg); break;
	case EGO_NOTHING: SDL_strlcpy(endlessGambleMsg, "Nothing. The house wins.", sizeof endlessGambleMsg); break;
	case EGO_LOANSHARK:  // a fortune now, a permanent tax on every price for the rest of the run
	{
		const long win = cost * 3;  // scales off the live fee (like the other wins) so it stays a real lump sum -- borrowing against your future
		player[0].cash += win;
		endlessShopTax += 25;  // compounds if you take the deal twice -- debt you never climb out of
		snprintf(endlessGambleMsg, sizeof endlessGambleMsg, "Loan shark: +$%ld, +25%% tax", win);
		break;
	}
	case EGO_NITRO:  // +damage next sector, but any hit is fatal (see varz.c damage path)
		endlessPurchasedMods |= (ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_NITRO);
		SDL_strlcpy(endlessGambleMsg, "Nitro: +power, one hit kills!", sizeof endlessGambleMsg);
		break;
	case EGO_OVERHEAT:  // kills quicken your guns, but the hull cooks (see endlessGameplayTick DoT)
		endlessPurchasedMods = (endlessPurchasedMods & ~ENDLESS_MOD_KILLFIRE_ANY) | ENDLESS_MOD_TURBODRIVE;
		endlessPurchasedMods |= ENDLESS_MOD_OVERHEAT;
		SDL_strlcpy(endlessGambleMsg, "Overheat: fast guns, hull cooks!", sizeof endlessGambleMsg);
		break;
	case EGO_GLASSCANNON:  // +2 permanent gun power, but shave permanent max hull
	{
		const int pw = (int)player[0].items.weapon[FRONT_WEAPON].power;
		player[0].items.weapon[FRONT_WEAPON].power = (pw + 2 > 11) ? 11 : (pw + 2);
		const int floorBonus = 8 - (int)player[0].initial_armor;  // keep effective max hull >= ~8
		endlessArmorBonus -= 2 * ENDLESS_HULL_STEP;
		if (endlessArmorBonus < floorBonus)
			endlessArmorBonus = floorBonus;
		if ((int)player[0].armor + endlessArmorBonus < 1)  // never leave the hull sitting at 0 in the shop
			player[0].armor = (JE_byte)(1 - endlessArmorBonus);
		SDL_strlcpy(endlessGambleMsg, "Glass cannon: +2 power, -hull!", sizeof endlessGambleMsg);
		break;
	}
	case EGO_MELTDOWN:  // -1 permanent gun power
		if ((int)player[0].items.weapon[FRONT_WEAPON].power > 1)
		{ --player[0].items.weapon[FRONT_WEAPON].power; SDL_strlcpy(endlessGambleMsg, "Meltdown!  -1 gun power.", sizeof endlessGambleMsg); }
		else
			SDL_strlcpy(endlessGambleMsg, "Guns already stripped bare.", sizeof endlessGambleMsg);
		break;
	case EGO_STICKY:  // steal the equipped special
		if (player[0].items.special > 0)
		{
			player[0].items.special = 0;
			shotMultiPos[SHOT_SPECIAL]  = 0; shotRepeat[SHOT_SPECIAL]  = 0;
			shotMultiPos[SHOT_SPECIAL2] = 0; shotRepeat[SHOT_SPECIAL2] = 0;
			SDL_strlcpy(endlessGambleMsg, "Sticky fingers: special gone!", sizeof endlessGambleMsg);
		}
		else
		{ const long loss = (long)(player[0].cash / 10); player[0].cash -= loss; snprintf(endlessGambleMsg, sizeof endlessGambleMsg, "Frisked!  -$%ld", loss); }
		break;
	case EGO_RUSTBUCKET:  // knock a shield or generator down a tier
		if (player[0].items.shield > 1 && (endlessRand() % 2))
		{ --player[0].items.shield; SDL_strlcpy(endlessGambleMsg, "Rustbucket: shield sags!", sizeof endlessGambleMsg); }
		else if (player[0].items.generator > 1)
		{ --player[0].items.generator; SDL_strlcpy(endlessGambleMsg, "Rustbucket: reactor sags!", sizeof endlessGambleMsg); }
		else if (player[0].items.shield > 1)
		{ --player[0].items.shield; SDL_strlcpy(endlessGambleMsg, "Rustbucket: shield sags!", sizeof endlessGambleMsg); }
		else
		{ const long loss = (long)(player[0].cash / 10); player[0].cash -= loss; snprintf(endlessGambleMsg, sizeof endlessGambleMsg, "Gear too cheap -- $%ld gone", loss); }
		break;
	case EGO_AMNESIA:  // erase a random owned perk
	{
		int owned[PERK_COUNT], n = 0;
		for (int i = 0; i < PERK_COUNT; ++i)
			if (endlessPerkOwned[i] > 0)
				owned[n++] = i;
		if (n > 0)
		{ --endlessPerkOwned[owned[endlessRand() % (unsigned)n]]; SDL_strlcpy(endlessGambleMsg, "Amnesia: a perk erased!", sizeof endlessGambleMsg); }
		else
		{ const long loss = (long)(player[0].cash / 10); player[0].cash -= loss; snprintf(endlessGambleMsg, sizeof endlessGambleMsg, "Mind blank -- robbed  -$%ld", loss); }
		break;
	}
	case EGO_DUD:  // fill your bombs to the brim... then jam them for the next sector
		while (player[0].superbombs < 10)
			++player[0].superbombs;
		endlessPurchasedMods |= ENDLESS_MOD_DUD;
		SDL_strlcpy(endlessGambleMsg, "Loaded up... duds next sector!", sizeof endlessGambleMsg);
		break;
	case EGO_SWINDLED: { const long loss = (long)(player[0].cash / 5); player[0].cash -= loss; snprintf(endlessGambleMsg, sizeof endlessGambleMsg, "Swindled!  -$%ld", loss); break; }
	case EGO_CURSE_JAM:
		endlessPurchasedMods = (endlessPurchasedMods & ~ENDLESS_MOD_KILLFIRE_ANY) | ENDLESS_MOD_BACKFIRE;
		SDL_strlcpy(endlessGambleMsg, "Cursed: guns jam next!", sizeof endlessGambleMsg);
		break;
	case EGO_CURSE_FAIL:
		endlessPurchasedMods = (endlessPurchasedMods & ~ENDLESS_MOD_KILLFIRE_ANY) | ENDLESS_MOD_BURNOUT;
		SDL_strlcpy(endlessGambleMsg, "Cursed: guns fail next!", sizeof endlessGambleMsg);
		break;
	case EGO_CURSE_MISFIRE:
		endlessPurchasedMods = (endlessPurchasedMods & ~ENDLESS_MOD_KILLFIRE_ANY) | ENDLESS_MOD_MISFIRE;
		SDL_strlcpy(endlessGambleMsg, "Cursed: guns misfire next!", sizeof endlessGambleMsg);
		break;
	case EGO_CURSE_FRENZY:
		endlessPurchasedMods |= ENDLESS_MOD_FRENZY;
		SDL_strlcpy(endlessGambleMsg, "Cursed: fast fire next!", sizeof endlessGambleMsg);
		break;
	case EGO_MARKED:
		endlessPurchasedMods |= ENDLESS_MOD_MARKED;
		SDL_strlcpy(endlessGambleMsg, "Marked: the next boss bulks up!", sizeof endlessGambleMsg);
		break;
	case EGO_LONGCON:
		endlessLongCon = 3;  // an APEX ambush comes due three sectors from now (endlessSelectCourse)
		SDL_strlcpy(endlessGambleMsg, "The long con... something coming.", sizeof endlessGambleMsg);
		break;
	case EGO_ROBBED:
		if (player[0].superbombs > 0)
			--player[0].superbombs;
		SDL_strlcpy(endlessGambleMsg, "Robbed: lost a bomb.", sizeof endlessGambleMsg);
		break;
	case EGO_DISARMED:
		if (endlessReviveHeld)
		{ endlessReviveHeld = false; SDL_strlcpy(endlessGambleMsg, "Disarmed! Revive gone.", sizeof endlessGambleMsg); }
		else
		{ const long loss = (long)(player[0].cash / 5); player[0].cash -= loss; snprintf(endlessGambleMsg, sizeof endlessGambleMsg, "Pickpocketed!  -$%ld", loss); }
		break;
	case EGO_PSYCH:  // the cruel fake-out: flashes a jackpot, then snatches a little extra
	{
		const long loss = (player[0].cash < (ulong)cost) ? (long)player[0].cash : cost;
		player[0].cash -= (ulong)loss;
		snprintf(endlessGambleMsg, sizeof endlessGambleMsg, "'JACKPOT!' ...psych. -$%ld", loss);
		break;
	}
	case EGO_RIGGED:
		endlessGambleRigged = true;  // your NEXT pull rolls twice and keeps the worse
		SDL_strlcpy(endlessGambleMsg, "The house is watching you...", sizeof endlessGambleMsg);
		break;
	case EGO_RAMPAGE:  // the original brutal Kamikaze as a jackpot-of-doom: rammers next sector
		endlessPurchasedMods |= ENDLESS_MOD_RAMPAGE;
		SDL_strlcpy(endlessGambleMsg, "KAMIKAZE RUSH! Rammers next!", sizeof endlessGambleMsg);
		break;
	default:  // EGO_CLEANED: half your cash -- the brutal tail that mirrors the jackpot
	{
		const long loss = (long)(player[0].cash / 2);
		player[0].cash -= loss;
		snprintf(endlessGambleMsg, sizeof endlessGambleMsg, "Cleaned out!  -$%ld", loss);
		break;
	}
	}
}

// Map a 0..99 roll to an outcome ID, reproducing the weighted ladder and its sub-rolls exactly
// (each sub-bucket consumes one endlessRand to pick within it, as the inline version did).
static int endlessRollToOutcome(int roll)
{
	if (roll < 5)  return EGO_JACKPOT;
	if (roll < 12) { switch (endlessRand() % 3) { case 0: return EGO_REVIVE; case 1: return EGO_HULL; default: return EGO_OVERCLOCK; } }  // EGO_PERK pulled out -> now a 1/2500 ultra-rare draw (endlessTryGamble)
	if (roll < 24) return EGO_WIN;
	if (roll < 31) return EGO_SPECIAL;
	if (roll < 37) { switch (endlessRand() % 3) { case 0: return EGO_ARSENAL; case 1: return EGO_SECONDWIND; default: return EGO_BLOODMONEY; } }
	if (roll < 43) return (endlessRand() % 2) ? EGO_OVERBLAST : EGO_OVERCHARGE;
	if (roll < 48) return EGO_FAVOR;
	if (roll < 52) return EGO_GOLDEN;
	if (roll < 56) return EGO_DOUBLENOTHING;
	if (roll < 59) return EGO_REFUND;
	if (roll < 62) return EGO_NOTHING;
	if (roll < 66) return EGO_LOANSHARK;
	if (roll < 71) { switch (endlessRand() % 3) { case 0: return EGO_NITRO; case 1: return EGO_OVERHEAT; default: return EGO_GLASSCANNON; } }
	if (roll < 78) { switch (endlessRand() % 5) { case 0: return EGO_MELTDOWN; case 1: return EGO_STICKY; case 2: return EGO_RUSTBUCKET; case 3: return EGO_AMNESIA; default: return EGO_DUD; } }
	if (roll < 84) return EGO_SWINDLED;
	if (roll < 90) { switch (endlessRand() % 5) { case 0: return EGO_CURSE_JAM; case 1: return EGO_CURSE_FAIL; case 2: return EGO_CURSE_MISFIRE; default: return EGO_CURSE_FRENZY; } }
	if (roll < 94) return (endlessRand() % 2) ? EGO_MARKED : EGO_LONGCON;
	if (roll < 97) { switch (endlessRand() % 4) { case 0: return EGO_ROBBED; case 1: return EGO_DISARMED; case 2: return EGO_PSYCH; default: return EGO_RIGGED; } }
	return EGO_CLEANED;
}

bool endlessTryGamble(void)
{
	endlessGamblePerkWon = false;  // cleared each pull; set only by the free-perk outcome

	const long cost = endlessGamblePrice();
	if (player[0].cash < (ulong)cost)
		return false;
	player[0].cash -= cost;

	// A few ultra-rare outcomes are rolled apart from the 0..99 ladder below, since their odds don't fit
	// a percentile bucket. One shared 1-in-5000 draw keeps each exact: value 0 = the dream MEGA JACKPOT
	// (+$1M), value 1 = the "jackpot of doom" Kamikaze (rammers next sector), values 2-3 = a free perk
	// pick (2/5000 = 1/2500). (The debug "Gamble Outcomes" page can also fire any of these directly.)
	const Uint32 ultraRare = endlessRand() % 5000;
	if (ultraRare == 0)
	{
		endlessApplyGambleOutcome(EGO_MEGAJACKPOT, cost);
		return true;
	}
	if (ultraRare == 1)
	{
		endlessApplyGambleOutcome(EGO_RAMPAGE, cost);
		return true;
	}
	if (ultraRare == 2 || ultraRare == 3)  // 2/5000 = 1/2500: the free perk pick
	{
		endlessApplyGambleOutcome(EGO_PERK, cost);
		return true;
	}

	int roll = (int)(endlessRand() % 100);
	if (endlessGambleRigged)  // "Rigged": the house quietly rolls a second time and keeps the WORSE (higher) one
	{
		const int second = (int)(endlessRand() % 100);
		if (second > roll)
			roll = second;
		endlessGambleRigged = false;
	}

	endlessApplyGambleOutcome(endlessRollToOutcome(roll), cost);
	return true;
}

// --- Debug hooks: enumerate and fire gamble outcomes by ID (the E-Shop debug "Gamble Outcomes" page).
int endlessGambleOutcomeCount(void) { return EGO_COUNT; }
const char *endlessGambleOutcomeName(int id) { return (id >= 0 && id < EGO_COUNT) ? endlessGambleOutcomeNames[id] : ""; }
void endlessForceGambleOutcome(int id)
{
	if (!endlessMode || id < 0 || id >= EGO_COUNT)
		return;
	endlessGamblePerkWon = false;
	endlessApplyGambleOutcome(id, endlessGamblePrice());  // no fee charged: this is a test trigger
	if (endlessGamblePerkWon)      // the free-perk outcome fired (choices are already rolled): queue a real
		endlessPerkPending = true; // pick -- the debug screen opens it on close, else it rides the next shop gate
	endlessGamblePerkWon = false;  // the debug screen has no E-Shop dispatch to consume the inline-perk flag
}

// Reroll the shop stock (rarity-by-depth) for the current price. Returns true if bought.
bool endlessTryReroll(void)
{
	if (player[0].cash < (ulong)endlessRerollCost)
		return false;
	player[0].cash -= endlessRerollCost;
	endlessRerollCost = endlessRerollCost * 8 / 5 + 3000;
	endlessFillShop();
	return true;
}

// Buy a run-persistent +armor hull upgrade for the current price. Returns true if bought.
bool endlessTryReinforce(void)
{
	if (endlessArmorBonus >= endlessHullMax() || player[0].cash < (ulong)endlessHullCost)
		return false;
	player[0].cash -= endlessHullCost;
	endlessArmorBonus += ENDLESS_HULL_STEP;
	endlessHullCost = endlessHullCost * 3 / 2 + 5000;
	return true;
}

// Apply the level-clear payout -- bank interest on unspent cash plus the depth/mutator-scaled
// clear bonus -- and report the two amounts so the level-end screen can show them. Skipped
// (both zero) before the first level is cleared. JE_endLevelAni calls this right before it
// prints the running cash total, so the reward is already banked when the shop opens.
void endlessApplyLevelPayout(long *interestOut, long *bonusOut)
{
	long interest = 0, bonus = 0;
	if (endlessMode && endlessRunDepth > 0)
	{
		interest = (long)(player[0].cash / 10);   // 10% bank interest on unspent cash
		// The interest cap RISES with depth, so banking toward a big buy (a deep hull tier, or a
		// saved-up Overdrive) is a real strategy on a long run -- cash becomes a reserve you manage,
		// not just per-zone Overdrive throughput. The depth-scaled ceiling still stops it snowballing.
		long icap = 3000 + (long)endlessRunDepth * 80;
		if (interest > icap)
			interest = icap;
		bonus = endlessClearBonus() * endlessPerkCashPercent() / 100;  // Scavenger perk scales the clear bonus
		player[0].cash += interest;
		player[0].cash += bonus;
	}
	if (interestOut)
		*interestOut = interest;
	if (bonusOut)
		*bonusOut = bonus;
}

void endlessBetweenLevels(void)
{
	// Pin the planet-map hub to one safe planet before the shop opens. Endless jumps to random
	// levels, so a level's ]G planet data can leave mapPNum/mapPlanet out of range, and
	// JE_itemScreen's planet monitor would then read mapPlanet[]/mapSection[] (size 5) out of
	// bounds and crash. (endlessRegenerateLevel re-pins these per level too, but the very first
	// shop runs before any level has loaded.)
	mapOrigin = 1;
	mapPNum = 1;
	mapPlanet[0] = 1;
	mapSection[0] = 1;

	// Endless has no data cubes. Clear any left over from a prior campaign game so they don't
	// linger as icons in the buy/sell menu; the first endless shop opens before any level (and
	// thus before endlessRegenerateLevel, which also zeroes these) has loaded.
	cubeMax = 0;
	lastCubeMax = 0;

	mouseSetRelative(false);  // menus use absolute mouse; start_level_first re-enables relative for gameplay

	// Clearing ZONE 100 rolls the credits once, then the run carries straight on into zone 101. The
	// flag is set BEFORE the roll and the outpost auto-saves right below it, so watching them through
	// or skipping them both leave a zone-101 save that won't play them again on reload. (`>=` rather
	// than `==` so a debug zone jump over the mark still gets its one showing.)
	if (endlessRunDepth >= ENDLESS_CREDITS_ZONE && !endlessCreditsShown)
	{
		endlessCreditsShown = true;
		VGAScreen = VGAScreenSeg;  // the level loop may have left it on VGAScreen2 (as JE_itemScreen does)
		fade_black(10);            // the level-complete screen is still up; JE_playCredits fades in from black
		JE_playCredits();
	}

	// Label anything saved from this outpost as the zone the player resumes into, and make sure
	// the slot reads as occupied. (The first outpost runs before any level has set these.)
	snprintf(levelName, sizeof(levelName), "ZONE %d", endlessRunDepth + 1);
	strcpy(lastLevelName, levelName);
	if (saveLevel < 1)
		saveLevel = FIRST_LEVEL;

	// The level-clear payout (bank interest + clear bonus) is applied earlier, on the
	// level-end screen (endlessApplyLevelPayout, called from JE_endLevelAni), so the shop
	// opens with the reward already banked.

	// Generate the next-level courses (offered in the shop's Start Level submenu), reset the
	// reroll/hull prices, stock the shop, then open it. The player picks a course in Start Level,
	// which sets mainLevel + the mutators and launches (see game_menu.c).
	// On a normal visit, roll everything fresh. On a resumed visit (a save was just loaded) or a
	// locked "gave up the level" reopen, the courses, prices, shop stock, perk offer and pending
	// buys were all restored from the snapshot; regenerating any of them would be a free reroll
	// (and, when locked, would break the lock), so skip the whole step.
	if (endlessResumeVisit || endlessLockedSortie)
	{
		endlessResumeVisit = false;
	}
	else
	{
		// Re-derive the seeded stream for this outpost's generation (courses, shop stock, perk
		// offers), keyed by depth. Player-timed draws later this visit (gamble, reroll) advance the
		// stream too, but the next zone reseeds from its own depth, so they can't shift the run's
		// structure: a given seed always yields the same courses/shop/perks at a given depth.
		endlessReseed((Uint64)endlessRunDepth * 2);

		endlessGenerateCourses();
		endlessResetShopPrices();
		endlessFillShop();

		// Queue the forced perk pick that opens the shop ahead of its normal front menu (see
		// JE_itemScreen), then roll this visit's offers. endlessPerkDueAtDepth decides: the every-3rd-
		// zone cadence, a cleared milestone zone, or the deferred half of a collision between the two.
		// Skip it if this depth's perk was already resolved, so re-opening the same outpost after a
		// save/reload doesn't hand out a second perk (endlessPerkDepthDone is part of the save).
		if (endlessPerkDueAtDepth(endlessRunDepth) && endlessPerkDepthDone != endlessRunDepth)
		{
			endlessGeneratePerkChoices();
			endlessPerkPending = true;
		}
	}

	// Auto-checkpoint into the bottom "LAST LEVEL" continue slot -- outpost entry is the one
	// coherent resume point (courses/shop/perks set up; lastLevelName is "ZONE N" already).
	// HARDCORE allows no saving of any kind, so it is suppressed (notes.md §Endless save).
	if (!endlessHardcore)
	{
		const JE_byte autoSlot = twoPlayerMode ? 22 : 11;
		JE_saveGame(autoSlot, "LAST LEVEL    ");
		endlessSaveSlot(autoSlot);  // side-effect-free run capture into the sidecar (endlessMode is true here)
	}

	// Pin the shop's theme. A random level's ']i' command can leave songBuy on that level's own
	// track, and JE_itemScreen plays songBuy on entry, so it's set here every visit -- which also
	// makes it survive a save/load for free, since it's re-derived from the (saved) run depth rather
	// than stored. The outpost that charts a MILESTONE zone swaps in the warning track instead of
	// the usual buy/sell theme, so the player hears that something is coming while they still have
	// the course list in front of them.
	songBuy = endlessMilestoneKind() ? ENDLESS_MILESTONE_SHOP_SONG : DEFAULT_SONG_BUY;
	JE_itemScreen();
}

// --- Next-level courses ---------------------------------------------------------
// The next-level choice IS the shop's "Start Level" submenu: game_menu.c drives its planet
// monitor from these courses. Each is a shipped level plus its own risk/reward mutators.
// Generated once per shop visit; picking one applies its mutators and launches.

#define ENDLESS_MAX_COURSES 5

static int      endlessCourseCnt = 0;
static int      endlessCourseEp[ENDLESS_MAX_COURSES];
static JE_byte  endlessCourseSec[ENDLESS_MAX_COURSES];
static JE_byte  endlessCourseFile[ENDLESS_MAX_COURSES];  // each course's specific lvlFileNum (see forcedLvlFileNum)
static Uint64 endlessCourseMod[ENDLESS_MAX_COURSES];
static JE_byte  endlessCourseNameSalt[ENDLESS_MAX_COURSES];  // per-visit nudge so no two offered names read the same
static int      endlessLastEp = 0;
static JE_byte  endlessLastSec = 0;
static bool     endlessForced = false;  // this visit is a forced "Ambush" (single dangerous sector)

// Named course themes: a NAME dictionary of evocative labels for combos worth naming
// (endlessComboName looks up an exact bit-set here; anything unlisted gets a generated name).
// Generation and payout are driven by endlessModTable, not these rows -- so this is purely
// cosmetic naming, free to extend or trim without touching behaviour.
typedef struct { Uint64 mods; const char *name; } EndlessTheme;

static const EndlessTheme endlessHostileThemes[] = {
	// -- single dangers --
	{ ENDLESS_MOD_FORTIFIED,   "Fortified" },
	{ ENDLESS_MOD_FRENZY,      "Frenzy" },
	{ ENDLESS_MOD_SWIFT,       "Swift Death" },
	{ ENDLESS_MOD_DEVASTATING, "Devastating" },
	{ ENDLESS_MOD_ENRAGE,      "Enrage" },
	{ ENDLESS_MOD_GRAVITY,     "Gravity Well" },
	{ ENDLESS_MOD_ELITEPACK,   "Elite Pack" },
	{ ENDLESS_MOD_OVERCLOCK,   "Overclock" },
	{ ENDLESS_MOD_SLIPSTREAM,  "Slipstream" },    // faster scroll: the level rushes at you (a threat, not the old boon)
	{ ENDLESS_MOD_TOPSY,       "Topsy Turvy" },  // fork: upside-down screen (boss-style -- controls invert with the view)
	{ ENDLESS_MOD_SLUGGISH,    "Molasses" },      // fork: slowed ship (depth-scaled; combos below -- its gravity pairing is a rare injection)

	// -- pairs: tough + shots --
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY,       "Onslaught" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT,        "Juggernaut" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING,  "Siege" },
	// -- pairs: bullet dangers --
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,           "Barrage" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_DEVASTATING,     "Fusillade" },
	{ ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,      "Piercing Storm" },
	// -- pairs: enrage (the fight drags on, fury climbs) --
	{ ENDLESS_MOD_ENRAGE | ENDLESS_MOD_FORTIFIED,       "War of Attrition" },
	{ ENDLESS_MOD_ENRAGE | ENDLESS_MOD_SWIFT,           "Escalation" },
	{ ENDLESS_MOD_ENRAGE | ENDLESS_MOD_DEVASTATING,     "Wrath" },
	// -- pairs: gravity (dodge while dragged) --
	{ ENDLESS_MOD_GRAVITY | ENDLESS_MOD_FORTIFIED,      "Dense Matter" },
	{ ENDLESS_MOD_GRAVITY | ENDLESS_MOD_SWIFT,          "Event Horizon" },
	{ ENDLESS_MOD_GRAVITY | ENDLESS_MOD_DEVASTATING,    "Crushing Weight" },
	{ ENDLESS_MOD_GRAVITY | ENDLESS_MOD_FRENZY,         "Whirlpool" },
	// -- pairs: elite pack --
	{ ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_FORTIFIED,    "Praetorians" },
	{ ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_SWIFT,        "Vanguard" },
	{ ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_DEVASTATING,  "Elite Guard" },
	{ ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_FRENZY,       "Warband" },
	// -- pairs: overclock (it already speeds fire+shots+scroll, so pair with new axes only) --
	{ ENDLESS_MOD_OVERCLOCK | ENDLESS_MOD_FORTIFIED,    "Meltdown" },
	{ ENDLESS_MOD_OVERCLOCK | ENDLESS_MOD_DEVASTATING,  "Reactor Breach" },
	{ ENDLESS_MOD_OVERCLOCK | ENDLESS_MOD_GRAVITY,      "Riptide" },  // not the Overdrive buff -- renamed to avoid the clash with the OVERDRIVE boon below
	{ ENDLESS_MOD_OVERCLOCK | ENDLESS_MOD_ELITEPACK,    "Prototype Swarm" },
	// -- pairs: slipstream (the level rushes past while ...; no Overclock pairing -- Overclock
	//    already carries the same +70% scroll, so welding them would be a redundant bit) --
	{ ENDLESS_MOD_SLIPSTREAM | ENDLESS_MOD_FRENZY,      "Fast Lane" },
	{ ENDLESS_MOD_SLIPSTREAM | ENDLESS_MOD_DEVASTATING, "Runaway" },
	{ ENDLESS_MOD_SLIPSTREAM | ENDLESS_MOD_FORTIFIED,   "Bypass" },
	// -- more pairs: fury / speed / mass --
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_ENRAGE,          "Bloodrage" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_OVERCLOCK,       "Redline" },
	{ ENDLESS_MOD_SWIFT | ENDLESS_MOD_OVERCLOCK,        "Hypervelocity" },
	{ ENDLESS_MOD_ENRAGE | ENDLESS_MOD_GRAVITY,         "Accretion" },
	{ ENDLESS_MOD_ENRAGE | ENDLESS_MOD_ELITEPACK,       "Elite Fury" },
	{ ENDLESS_MOD_ENRAGE | ENDLESS_MOD_OVERCLOCK,       "Overburn" },
	{ ENDLESS_MOD_GRAVITY | ENDLESS_MOD_ELITEPACK,      "Neutron Star" },

	// -- triples (hard) --
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,       "Nightmare" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,     "Maelstrom" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,  "Fortress Guns" },
	{ ENDLESS_MOD_ENRAGE | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,     "Deadly Escalation" },
	{ ENDLESS_MOD_GRAVITY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,    "Singularity" },
	{ ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING, "Elite Siege" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_DEVASTATING,    "Blitzkrieg" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_DEVASTATING,   "Colossus" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_DEVASTATING,    "Siege Engine" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_GRAVITY,        "Heavy Siege" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_ENRAGE,         "Bloodwall" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_DEVASTATING,      "Vortex" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_DEVASTATING,       "Firestorm" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_GRAVITY,            "Cyclone" },
	{ ENDLESS_MOD_SWIFT | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_GRAVITY,            "Death Spiral" },
	{ ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,     "Death Squad" },
	{ ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_FRENZY | ENDLESS_MOD_DEVASTATING,    "War Machine" },
	{ ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_DEVASTATING,   "Doomguard" },
	{ ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_DEVASTATING,    "Elite Wrath" },
	{ ENDLESS_MOD_OVERCLOCK | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,     "Overkill" },
	{ ENDLESS_MOD_OVERCLOCK | ENDLESS_MOD_FRENZY | ENDLESS_MOD_DEVASTATING,    "Full Auto" },
	{ ENDLESS_MOD_OVERCLOCK | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING, "Iron Storm" },

	// -- quads (brutal) --
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING, "Cataclysm" },
	{ ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,   "Elite Nightmare" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_GRAVITY,     "Abaddon" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_ENRAGE, "Armageddon" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_ENRAGE,    "Apocalypse" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_ENRAGE,      "Berserker Rush" },
	{ ENDLESS_MOD_GRAVITY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_ENRAGE,   "Event Collapse" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_ENRAGE, "Black Star" },
	{ ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING, "Praetorian Guard" },
	{ ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,  "Elite Storm" },
	{ ENDLESS_MOD_OVERCLOCK | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,  "Bullet Hell" },
	{ ENDLESS_MOD_OVERCLOCK | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING, "Overlord" },

	// -- more triples --
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_ELITEPACK, "Bloodtide" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_OVERCLOCK, "Warpath" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT | ENDLESS_MOD_ENRAGE, "Ironclad" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT | ENDLESS_MOD_GRAVITY, "Rampart" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT | ENDLESS_MOD_ELITEPACK, "Hailstorm" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT | ENDLESS_MOD_OVERCLOCK, "Thunderclap" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_GRAVITY, "Pandemonium" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_ELITEPACK, "Requiem" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_OVERCLOCK, "Damnation" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_ELITEPACK, "Perdition" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_OVERCLOCK, "Warhead" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_OVERCLOCK, "Sledgehammer" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_ENRAGE, "Warmonger" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_ELITEPACK, "Devastator" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_OVERCLOCK, "Ravager" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_GRAVITY, "Desolation" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_ELITEPACK, "Bombardment" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_OVERCLOCK, "Storm Surge" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_ELITEPACK, "Iron Rain" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_OVERCLOCK, "Steel Storm" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_OVERCLOCK, "Death March" },
	{ ENDLESS_MOD_SWIFT | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_ELITEPACK, "Hellfire" },
	{ ENDLESS_MOD_SWIFT | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_OVERCLOCK, "Brimstone" },
	{ ENDLESS_MOD_SWIFT | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_ELITEPACK, "Wildfire" },
	{ ENDLESS_MOD_SWIFT | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_OVERCLOCK, "Firewall" },
	{ ENDLESS_MOD_SWIFT | ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_OVERCLOCK, "Overrun" },
	{ ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_GRAVITY, "Stampede" },
	{ ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_OVERCLOCK, "Warzone" },
	{ ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_OVERCLOCK, "Killzone" },
	{ ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_OVERCLOCK, "Crossfire" },
	{ ENDLESS_MOD_ENRAGE | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_ELITEPACK, "Massacre" },
	{ ENDLESS_MOD_ENRAGE | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_OVERCLOCK, "Butchery" },
	{ ENDLESS_MOD_ENRAGE | ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_OVERCLOCK, "Bloodbath" },
	{ ENDLESS_MOD_GRAVITY | ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_OVERCLOCK, "Slaughter" },

	// -- mixed (a boon bit riding a hostile course) --
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_DEVASTATING,    "Glass Cannon" },

	// -- fork mods: TOPSY (flipped view) pairs. Safe with everything (purely visual), so it mixes
	//    freely -- these are the NAMED pairings; TOPSY is also in the combinable pool for emergent combos.
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_FORTIFIED,      "Capsize" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_FRENZY,         "Vertigo" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_SWIFT,          "Corkscrew" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_DEVASTATING,    "Upheaval" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_ENRAGE,         "Whirligig" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_ELITEPACK,      "Off Kilter" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_OVERCLOCK,      "Barrel Roll" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_GRAVITY,        "Somersault" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_SLUGGISH,       "Head Rush" },
	// -- fork mods: SLUGGISH (slowed ship) pairs. GRAVITY is deliberately absent here -- that pairing
	//    is the rare Tar Pit injection (endlessSluggishThemes) -- but every other pairing is fair game.
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_FORTIFIED,   "Anchored" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_FRENZY,      "Slog" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_SWIFT,       "Bogged Down" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_DEVASTATING, "Lead Boots" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_ENRAGE,      "War of Attrition II" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_ELITEPACK,   "Ballast" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_OVERCLOCK,   "Millstone" },
	// -- fork mods: a few triples (no SLUGGISH+GRAVITY here) --
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,          "Tumbler" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING, "Keelhaul" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_DEVASTATING,   "Free Fall" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_FRENZY,       "Disorient" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,       "Bullet Wade" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING, "Deadlock" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_SWIFT | ENDLESS_MOD_ELITEPACK,    "Molasses Run" },

	// -- SHIELDLESS (shields never recharge): a defense debuff, safe to combine and in the pool too --
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_FORTIFIED,   "Attrition" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_FRENZY,      "Exposed" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_SWIFT,       "Pincushion" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_DEVASTATING, "Glass Hull" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_ENRAGE,      "Overexposed" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_ELITEPACK,   "Outmatched" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_OVERCLOCK,   "No Cover" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_GRAVITY,     "Naked Descent" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_TOPSY,       "Spin Out" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_SLUGGISH,    "Sitting Target" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,        "Kill Box" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING, "Last Ditch" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_SWIFT,     "No Quarter" },
};

// KAMIKAZE sectors (homing rammers) are much harder to fly, so they get their own pool and are
// injected RARELY (see endlessGenerateCourses) instead of shuffled in with the normal hostiles.
static const EndlessTheme endlessKamikazeThemes[] = {
	{ ENDLESS_MOD_KAMIKAZE,                              "Kamikaze" },
	{ ENDLESS_MOD_KAMIKAZE | ENDLESS_MOD_FORTIFIED,      "Battering Ram" },
	{ ENDLESS_MOD_KAMIKAZE | ENDLESS_MOD_FRENZY,         "Zealots" },
	{ ENDLESS_MOD_KAMIKAZE | ENDLESS_MOD_SWIFT,          "Dive Bombers" },
	{ ENDLESS_MOD_KAMIKAZE | ENDLESS_MOD_DEVASTATING,    "Suicide Run" },
	{ ENDLESS_MOD_KAMIKAZE | ENDLESS_MOD_GRAVITY,        "Black Hole" },
	{ ENDLESS_MOD_KAMIKAZE | ENDLESS_MOD_ELITEPACK,      "Berserkers" },
	{ ENDLESS_MOD_KAMIKAZE | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING, "Kamikaze Storm" },
	{ ENDLESS_MOD_KAMIKAZE | ENDLESS_MOD_ENRAGE,        "Fanatics" },
	{ ENDLESS_MOD_KAMIKAZE | ENDLESS_MOD_OVERCLOCK,     "Rocket Swarm" },
	{ ENDLESS_MOD_KAMIKAZE | ENDLESS_MOD_FRENZY | ENDLESS_MOD_DEVASTATING,    "Death Charge" },
	{ ENDLESS_MOD_KAMIKAZE | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING, "Siege Ram" },
};

// HOMING sectors are the MILD cousin of Kamikaze: enemies gently drift toward you (no ram bonus),
// just enough to be a nuisance. Their own pool, injected at a moderate rate (see endlessGenerateCourses)
// so you meet them regularly -- while the real Kamikaze pool above stays super rare.
static const EndlessTheme endlessHomingThemes[] = {
	{ ENDLESS_MOD_HOMING,                              "Stalkers" },
	{ ENDLESS_MOD_HOMING | ENDLESS_MOD_FORTIFIED,      "Bloodhounds" },
	{ ENDLESS_MOD_HOMING | ENDLESS_MOD_FRENZY,         "Harriers" },
	{ ENDLESS_MOD_HOMING | ENDLESS_MOD_SWIFT,          "Pursuers" },
	{ ENDLESS_MOD_HOMING | ENDLESS_MOD_DEVASTATING,    "Predators" },
	{ ENDLESS_MOD_HOMING | ENDLESS_MOD_GRAVITY,        "Undertow" },
	{ ENDLESS_MOD_HOMING | ENDLESS_MOD_ELITEPACK,      "Wolfpack" },
	{ ENDLESS_MOD_HOMING | ENDLESS_MOD_ENRAGE,         "Bloodlust" },
};

static const EndlessTheme endlessBoonThemes[] = {
	{ ENDLESS_MOD_FRAGILE,    "Fragile Foe" },
	{ ENDLESS_MOD_BOUNTY,     "Bounty Run" },
	{ ENDLESS_MOD_TURBODRIVE,  "Turbodrive" },   // the shop's Turbodrive buy sets this same bit (endlessTryBuyTurbodrive)
	{ ENDLESS_MOD_OVERDRIVE, "Overdrive" },   // the shop's Overdrive buy sets this same bit (endlessTryBuyOverdrive)
	{ ENDLESS_MOD_OVERBLAST, "Overblast" },   // the shop's Overblast buy sets this same bit (endlessTryBuyOverblast)
	{ ENDLESS_MOD_OVERCHARGE, "Overcharged" },
	{ ENDLESS_MOD_DILATION,   "Time Dilation" },
	{ ENDLESS_MOD_FAVOR,      "Merchant's Favor" },
	{ ENDLESS_MOD_CURSED,     "Cursed Bounty" },
	{ ENDLESS_MOD_NOCHAMP,    "Leaderless" },     // no champions -- the elite pack loses its purple overlords
	{ ENDLESS_MOD_NOELITE,    "Rank and File" },  // no elites or champions -- only ordinary troops (the stronger, rarer boon)
	// -- boon pairs (stack your buffs) --
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_OVERCHARGE, "Ascendant" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_OVERCHARGE,  "Bullet Time" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_TURBODRIVE,   "In the Zone" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_BOUNTY,       "Easy Money" },
	{ ENDLESS_MOD_FAVOR | ENDLESS_MOD_BOUNTY,         "Windfall" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_TURBODRIVE,    "Blood Frenzy" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_OVERCHARGE,   "Executioner" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_FAVOR,        "Clearance" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_BOUNTY,     "Killing Spree" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_BOUNTY,    "Mercenary" },
	{ ENDLESS_MOD_OVERDRIVE | ENDLESS_MOD_OVERCHARGE, "Power Surge" },
	{ ENDLESS_MOD_OVERBLAST | ENDLESS_MOD_OVERCHARGE, "Deadeye" },
	{ ENDLESS_MOD_OVERBLAST | ENDLESS_MOD_DILATION,   "Sharpshooter" },
	{ ENDLESS_MOD_OVERBLAST | ENDLESS_MOD_BOUNTY,     "Bounty Hunter" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_OVERCHARGE, "Ascension" },
	// -- more boon combos --
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_DILATION, "Killstreak" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_OVERDRIVE, "Bloodrush" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_CURSED, "Adrenaline" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_FAVOR, "Momentum" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_OVERDRIVE, "Berserk" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_CURSED, "Fortune" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_FAVOR, "Jackpot" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_CURSED, "Gold Rush" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_FAVOR, "Bonanza" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_BOUNTY, "Lucky Break" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_OVERDRIVE, "Fire Sale" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_CURSED, "Discount" },
	// -- no-elite-tier boon pairs. NOCHAMP gets the wider set (three), NOELITE fewer (two), one of the
	//    several levers keeping the stronger NOELITE the rarer sight. Never pair NOCHAMP with NOELITE. --
	{ ENDLESS_MOD_NOCHAMP | ENDLESS_MOD_OVERCHARGE, "Purge" },
	{ ENDLESS_MOD_NOCHAMP | ENDLESS_MOD_BOUNTY,     "Trophy Room" },
	{ ENDLESS_MOD_NOCHAMP | ENDLESS_MOD_FRAGILE,    "Mop Up" },
	{ ENDLESS_MOD_NOELITE | ENDLESS_MOD_OVERCHARGE, "Marksman" },
	{ ENDLESS_MOD_NOELITE | ENDLESS_MOD_FAVOR,      "Clean Slate" },
};

// WARP (Slipstream cranked way up -- the level hurtles past) is a rare scroll THREAT with its own
// injection in endlessGenerateCourses; naming-only here, like the omni-gravity table, so it never
// enters the hostile shuffle pool.
static const EndlessTheme endlessWarpThemes[] = {
	{ ENDLESS_MOD_WARP,       "Warp Speed" },
};

// OVERLOAD (Overclock cranked way up) is a rare, brutal hostile with its own pool, injected
// rarely (see endlessGenerateCourses) rather than shuffled into the normal rotation.
static const EndlessTheme endlessOverloadThemes[] = {
	{ ENDLESS_MOD_OVERLOAD,                           "Overload" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_FORTIFIED,   "Core Breach" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_DEVASTATING, "Chain Reaction" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_ELITEPACK,   "Singularity Core" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_FRENZY,      "Detonation" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_SWIFT,       "Overvelocity" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_GRAVITY,     "Implosion" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_ENRAGE,      "Overheat" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING, "Fusion Core" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,     "Total Meltdown" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY, "Core Meltdown" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT, "Fission" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_ENRAGE, "Critical Mass" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_GRAVITY, "Chain Blast" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_ELITEPACK, "Cascade" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_OVERCLOCK, "Feedback Loop" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT, "Power Spike" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_FRENZY | ENDLESS_MOD_DEVASTATING, "Blackout" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_FRENZY | ENDLESS_MOD_ENRAGE, "Flashpoint" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_FRENZY | ENDLESS_MOD_GRAVITY, "Ground Zero" },
};

// Super-rare, super-hard sectors, injected explicitly (not part of the shuffle pool) so they
// stay rare -- the Apex/Legion elite tiers with an extra danger, plus pure 5-danger nightmares.
static const EndlessTheme endlessRareThemes[] = {
	{ ENDLESS_MOD_APEX,   "Apex Swarm" },
	{ ENDLESS_MOD_LEGION, "Legion" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_FORTIFIED,                          "Apex Titans" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_SWIFT,                              "Apex Hunters" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_DEVASTATING,                        "Annihilation" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_OVERLOAD,                           "Extinction" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING, "Apex Siege" },
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_FORTIFIED,                        "Iron Legion" },
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_SWIFT,                            "Blitz Legion" },
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_DEVASTATING,                      "Ragnarok" },
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_OVERLOAD,                         "Judgment Day" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_ENRAGE,  "Hell Unleashed" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_GRAVITY, "Void Storm" },
	{ ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING, "Total War" },
	{ ENDLESS_MOD_OVERCLOCK | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING, "Doomsday" },
	// -- more Apex-tier (elite) rares --
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_FRENZY, "Alpha Strike" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_ENRAGE, "Omega" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_GRAVITY, "Final Hour" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_ELITEPACK, "Last Stand" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_OVERCLOCK, "Endgame" },
	// -- more Legion-tier (champion) rares --
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_FRENZY, "Mass Extinction" },
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_ENRAGE, "The Reaping" },
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_GRAVITY, "Harbinger" },
	// -- more pure multi-danger nightmares (Cataclysm pool) --
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_GRAVITY, "Nemesis" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_ELITEPACK, "Leviathan" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_OVERCLOCK, "Behemoth" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_ELITEPACK, "Titanfall" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_OVERCLOCK, "Wrath of God" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_OVERCLOCK, "Purgatory" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_GRAVITY, "Tartarus" },
	// -- fork-mod nightmares (Cataclysm pool): the flipped view / crawling ship welded onto a full danger stack --
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,   "Kaleidoscope" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,  "Oubliette" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_ENRAGE, "Naked Siege" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_TOPSY | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,  "Sensory Overload" },
	// -- more Apex-tier (elite) rares --
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,   "Apex Predator" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY,    "Apex Onslaught" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_DEVASTATING, "Event Apex" },
	// -- more Legion-tier (champion) rares --
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,     "Final Legion" },
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING, "Legion Siege" },
	// -- more pure multi-danger nightmares (Cataclysm pool: no Apex/Legion bit) --
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_GRAVITY,   "Doomtide" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_ELITEPACK,    "Deathstorm" },
	// -- the top of the Cataclysm pool: SIX and SEVEN simultaneous dangers, and the only S+++ sectors
	//    an ordinary zone can be dealt without an Apex/Legion tier. Ruination is the full enemy-stat
	//    wall plus a well (56, S++ -- it inherits the combination the old fixed "The End" used, which
	//    now lives on the milestone marker instead); the other two push past 60 into S+++ by adding a
	//    system loss or the disorienting pair. Rare by construction: only the ~1/45 Cataclysm
	//    injection deals from this sub-pool (see endlessGenerateCourses).
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_GRAVITY, "Ruination" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_SHIELDLESS, "Death Knell" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_TOPSY | ENDLESS_MOD_SLUGGISH, "Black Sun" },
};

// "The End" -- the sector every GRAND (100th-zone) milestone deals. It is NOT a fixed bitset: only
// its CORE is constant, and the rest is re-rolled per milestone off the seeded stream, so a run's
// zone-100 finisher differs from its zone-200 one and from every other run's. Naming, the END rank,
// the FINALITY danger word and the bounty all hang off the ENDLESS_MOD_THEEND marker rather than on
// matching an exact combination, which is what lets the dangers vary freely.
//
// The CORE is the enemy at its worst -- every enemy-stat lever at once -- and nothing else. Kept out
// on purpose: the homing tiers, which turn a gun fight into a chase, and the two handicaps that
// simply take a system away from you (Shieldless, Deadgen). What varies is the special-enemy tier,
// the scroll pace, and a coin each for the well, the flipped view and the slowed ship -- 80
// combinations, each still recognisably The End.
#define ENDLESS_THEEND_CORE (ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT \
                             | ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_ENRAGE)

static Uint64 endlessMakeTheEndMods(void)
{
	Uint64 m = ENDLESS_MOD_THEEND | ENDLESS_THEEND_CORE;

	// One special-enemy tier, always: every enemy an elite, or -- less often -- every one a champion.
	// Elite Pack is deliberately not offered: deep runs retire it as redundant (see
	// endlessFixRedundantElitePack), which would rewrite the finale's bitset out from under it.
	m |= (endlessRand() % 3 == 0) ? ENDLESS_MOD_LEGION : ENDLESS_MOD_APEX;

	// One scroll pace, sometimes none -- the level can hold still or come at you at any speed.
	static const Uint64 scroll[] = {
		0, ENDLESS_MOD_SLIPSTREAM, ENDLESS_MOD_OVERCLOCK, ENDLESS_MOD_OVERLOAD, ENDLESS_MOD_WARP,
	};
	m |= scroll[endlessRand() % COUNTOF(scroll)];

	// A coin each for the hazards that act on the ship rather than the enemy. Gravity and Sluggish can
	// both land -- that is the Tar Pit pairing, brutal but always flyable: endlessGravityDrift scales
	// the pull down in lock-step with the ship, so full throttle still climbs.
	if (endlessRand() % 2)
		m |= ENDLESS_MOD_GRAVITY;
	if (endlessRand() % 2)
		m |= ENDLESS_MOD_TOPSY;
	if (endlessRand() % 2)
		m |= ENDLESS_MOD_SLUGGISH;

	return m;
}

// EVIL Turbodrive / Overdrive: hostile mirrors of the two boons -- they SLOW your fire (Evil
// Overdrive also cuts damage) as your kill combo climbs. Injected as rare pickable course
// sectors; the three bare bits are ALSO forced gamble outcomes (EGO_CURSE_*), independent of
// courses. Adding a row here auto-wires its name/monitor/payout (see endlessFindTheme).
static const EndlessTheme endlessEvilThemes[] = {
	{ ENDLESS_MOD_BACKFIRE,                           "Backfire" },
	{ ENDLESS_MOD_BURNOUT,                            "Burnout" },
	{ ENDLESS_MOD_MISFIRE,                            "Misfire" },
	// -- Backfire (Evil Turbodrive, fire jam) + one danger --
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_DEVASTATING, "Sitting Duck" },
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_FORTIFIED,   "Uphill Battle" },
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_SWIFT,       "Overwhelmed" },
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_GRAVITY,     "Dead Weight" },
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_FRENZY,      "Friendly Fire" },
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_ENRAGE,      "Slow Burn" },
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_ELITEPACK,   "Cornered" },
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_OVERCLOCK,   "Vapor Lock" },
	// -- Burnout (Evil Overdrive, jam + damage cut) + one danger (nastier) --
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_DEVASTATING,  "Death Rattle" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_FORTIFIED,    "Losing Battle" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_ENRAGE,       "Downward Spiral" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_ELITEPACK,    "Outclassed" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_FRENZY,       "Flatline" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_SWIFT,        "Cinders" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_GRAVITY,      "Tailspin" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_OVERCLOCK,    "System Failure" },
	// -- Misfire (Evil Overblast, damage cut only) + one danger --
	{ ENDLESS_MOD_MISFIRE | ENDLESS_MOD_DEVASTATING,  "Peashooter" },
	{ ENDLESS_MOD_MISFIRE | ENDLESS_MOD_FORTIFIED,    "Stonewall" },
	{ ENDLESS_MOD_MISFIRE | ENDLESS_MOD_SWIFT,        "Outgunned" },
	{ ENDLESS_MOD_MISFIRE | ENDLESS_MOD_FRENZY,       "Popgun" },
	{ ENDLESS_MOD_MISFIRE | ENDLESS_MOD_ENRAGE,       "Fizzle" },
	{ ENDLESS_MOD_MISFIRE | ENDLESS_MOD_GRAVITY,      "Downhill" },
	{ ENDLESS_MOD_MISFIRE | ENDLESS_MOD_ELITEPACK,    "No Contest" },
	// -- two dangers welded to the curse: the memorable evil nightmares --
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING, "Forced March" },
	{ ENDLESS_MOD_BURNOUT  | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,     "Last Legs" },
	{ ENDLESS_MOD_MISFIRE  | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT,       "Stalemate" },
	{ ENDLESS_MOD_BURNOUT  | ENDLESS_MOD_FRENZY | ENDLESS_MOD_DEVASTATING,    "No Way Out" },
};

// Reactor Redline: the gamble "Overheat" deal loose in the wild -- your kills scream the guns faster
// (Turbodrive) while the redlined core steadily cooks the hull (the OVERHEAT chip DoT). Its own tiny
// pool so endlessFindTheme can name it and endlessGenerateCourses can inject it super-rarely (like
// Kamikaze / Overload) -- fast fire welded to a self-inflicted burn.
static const EndlessTheme endlessRedlineThemes[] = {
	{ ENDLESS_MOD_OVERHEAT | ENDLESS_MOD_TURBODRIVE,                      "Reactor Redline" },
	{ ENDLESS_MOD_OVERHEAT | ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_FRENZY, "Redline Frenzy" },
};

// SLUGGISH + GRAVITY: the "heavy, inescapable" nightmares -- the ship crawls WHILE dragged down.
// Survivable by design (endlessGravityDrift slows the pull in lock-step with the ship, so full
// throttle still climbs), but brutal -- its own tiny pool injected RARELY (like Kamikaze / Overload)
// rather than shuffled into the rotation. The bare pairing is the headline "Tar Pit".
static const EndlessTheme endlessSluggishThemes[] = {
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_GRAVITY,                           "Tar Pit" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_DEVASTATING, "Quicksand" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_FRENZY,      "Quagmire" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_SWIFT,       "Sinkhole" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_FORTIFIED,   "Abyss" },
};

// DEADGEN (dead generator): shields never refill AND the main gun is starved to a sputter -- a super-
// rare, evil sabotage sector with its own pool (injected ~1/55; never in the combinable pool or the
// shuffle). Rear guns / sidekicks / specials still work, so it's brutal, not unwinnable.
static const EndlessTheme endlessDeadgenThemes[] = {
	{ ENDLESS_MOD_DEADGEN,                            "Dead Reactor" },
	{ ENDLESS_MOD_DEADGEN | ENDLESS_MOD_DEVASTATING,  "Defenseless" },
	{ ENDLESS_MOD_DEADGEN | ENDLESS_MOD_FRENZY,       "Powerless" },
	{ ENDLESS_MOD_DEADGEN | ENDLESS_MOD_SWIFT,        "Brownout" },
	{ ENDLESS_MOD_DEADGEN | ENDLESS_MOD_ELITEPACK,    "Cold Start" },
};

// NAMING ONLY: the bare omnidirectional gravity well gets its own headline so Chart-a-Course reads it
// as its own thing. Generation never draws from this table (the OMNI bit is added by the 50% roll on
// any gravity course in endlessGenerateCourses); it only supplies the name. Every OMNI *combo* has no
// entry here and falls through to its plain-gravity twin's name via the mask in endlessFindTheme.
static const EndlessTheme endlessGravityOmniThemes[] = {
	{ ENDLESS_MOD_GRAVITY | ENDLESS_MOD_GRAVITY_OMNI, "Rogue Well" },
};

// MIXED "gambit" sectors: a real BOON welded to real DANGER, so the sector reads as risk AND reward
// (threats on the monitor's red column, boons on the green). Every entry pairs a boon with hostiles
// on DIFFERENT levers, so nothing cancels: FRAGILE never rides FORTIFIED (opposite HP), DILATION never
// rides SWIFT/OVERCLOCK (opposite shot speed), and at most one kill-fire boon appears (they share one
// stack). Naming/monitor/payout are all driven by endlessModTable, so these rows are purely cosmetic
// labels -- generation grafts boons onto hostile courses (endlessPickMixBoon) and the matches land here;
// anything unlisted falls through to the "gambit" generic names. FRAGILE|DEVASTATING is intentionally
// absent -- it's the hostile table's "Glass Cannon".
static const EndlessTheme endlessMixedThemes[] = {
	// -- pairs: your shots hit harder (Overcharge) vs one threat --
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_FORTIFIED,   "Can Opener" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_FRENZY,      "Return Fire" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_SWIFT,       "Quickdraw" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_DEVASTATING, "Standoff" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_ELITEPACK,   "Giant Slayer" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_GRAVITY,     "Heavy Hitter" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_ENRAGE,      "Beat the Clock" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_SHIELDLESS,  "All In" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_TOPSY,       "Topsy Duel" },
	// -- pairs: frail foes (Fragile) vs one threat --
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_FRENZY,         "Paper Storm" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_SWIFT,          "Glass Darts" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_ELITEPACK,      "Brittle Elites" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_ENRAGE,         "Short Fuse" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_GRAVITY,        "Feather Fall" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_OVERCLOCK,      "Blur" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_SHIELDLESS,     "Trade Blows" },
	// -- pairs: enemy shots crawl (Dilation) vs one threat --
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_FORTIFIED,     "War of Patience" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_FRENZY,        "Slow Motion" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_DEVASTATING,   "Read the Room" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_ELITEPACK,     "Matrix" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_GRAVITY,       "Deep Focus" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_ENRAGE,        "Steady Hand" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_SHIELDLESS,    "Cold Read" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_TOPSY,         "Slow Spin" },
	// -- pairs: fat cash (Bounty) vs one threat (pure risk-for-money, no safety) --
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_FORTIFIED,       "Big Game" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_FRENZY,          "Hot Zone" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_SWIFT,           "High Stakes" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_DEVASTATING,     "Danger Pay" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_ELITEPACK,       "Trophy Hunt" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_GRAVITY,         "Deep Dive" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_ENRAGE,          "Overtime" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_OVERCLOCK,       "Rush Job" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_SHIELDLESS,      "Hazard Bonus" },
	// -- pairs: kills feed your guns (Turbodrive) vs one threat --
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_FORTIFIED,   "Grind" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_FRENZY,      "Trigger Happy" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_SWIFT,       "Fast Hands" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_ELITEPACK,   "Cull the Herd" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_ENRAGE,      "Second Wind" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_GRAVITY,     "Dig In" },
	// -- pairs: cheaper shop next (Favor) vs one threat --
	{ ENDLESS_MOD_FAVOR | ENDLESS_MOD_FORTIFIED,        "Toll Road" },
	{ ENDLESS_MOD_FAVOR | ENDLESS_MOD_SWIFT,            "Hazard Discount" },
	{ ENDLESS_MOD_FAVOR | ENDLESS_MOD_DEVASTATING,      "Combat Pay" },
	{ ENDLESS_MOD_FAVOR | ENDLESS_MOD_FRENZY,           "Loss Leader" },
	// -- pairs: kills stack your damage (Overblast) / firepower (Overdrive) --
	{ ENDLESS_MOD_OVERBLAST | ENDLESS_MOD_FORTIFIED,    "Sledge" },
	{ ENDLESS_MOD_OVERBLAST | ENDLESS_MOD_SWIFT,        "Piercer" },
	{ ENDLESS_MOD_OVERBLAST | ENDLESS_MOD_ELITEPACK,    "Headhunter" },
	{ ENDLESS_MOD_OVERDRIVE | ENDLESS_MOD_FORTIFIED,    "Snowball" },
	{ ENDLESS_MOD_OVERDRIVE | ENDLESS_MOD_ELITEPACK,    "Killstreaker" },
	// -- pairs: a boon riding the faster-scroll threat (Slipstream was a boon once; these combos
	//    now read as gambits -- reachable when a boon is grafted onto a Slipstream course) --
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_SLIPSTREAM,     "Blitz" },
	{ ENDLESS_MOD_SLIPSTREAM | ENDLESS_MOD_BOUNTY,      "Smash and Grab" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_SLIPSTREAM,    "Time Warp" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_SLIPSTREAM,  "Power Play" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_SLIPSTREAM,  "Payday" },

	// -- triples: boon + two threats --
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT,       "Armor Piercer" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,          "Counterstrike" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING, "Slugfest" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_SWIFT,       "Elite Duel" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_FRENZY | ENDLESS_MOD_DEVASTATING,    "Trading Blows" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_DEVASTATING,   "Sinker" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY,        "Bullet Dance" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_FRENZY | ENDLESS_MOD_DEVASTATING,      "Zen Garden" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING,   "Stonewall Zen" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_FRENZY,        "Slow Dance" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,             "Confetti" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,        "Glass Storm" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_FRENZY | ENDLESS_MOD_ENRAGE,            "Tinderbox" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_SWIFT,          "Spun Glass" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT,           "Bounty Board" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_FRENZY | ENDLESS_MOD_DEVASTATING,        "Blood Money" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,         "Dead or Alive" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_FORTIFIED,       "Kingpin" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,          "Frenzy Feed" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_ELITEPACK,   "Grinder" },
	{ ENDLESS_MOD_FAVOR | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT,            "Risk Premium" },
	{ ENDLESS_MOD_OVERBLAST | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT,        "Overpenetrate" },
	{ ENDLESS_MOD_OVERDRIVE | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,           "Chain Lightning" },

	// -- quads: boon + three threats --
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,      "Last Word" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING, "Overmatch" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_DEVASTATING,  "Eye of the Storm" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,       "Shattering" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_ENRAGE,            "Powder Keg" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,        "Death and Taxes" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,          "Payday Run" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT,   "Elite Overmatch" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,      "Feeding Frenzy" },

	// -- pairs: no champions (NOCHAMP) vs one threat. Rides an Elite Pack fine -- the elites remain, only
	//    the champion spikes are gone -- but NEVER Legion (all-champion; NOCHAMP would erase the whole threat) --
	{ ENDLESS_MOD_NOCHAMP | ENDLESS_MOD_FORTIFIED,   "Tough Crowd" },
	{ ENDLESS_MOD_NOCHAMP | ENDLESS_MOD_FRENZY,      "Crowd Control" },
	{ ENDLESS_MOD_NOCHAMP | ENDLESS_MOD_SWIFT,       "Skeleton Crew" },
	{ ENDLESS_MOD_NOCHAMP | ENDLESS_MOD_ELITEPACK,   "Demotion" },   // an elite pack with its champions busted down to elites
	// -- pairs: no elite tier at all (NOELITE) vs one threat. Only levers OTHER than the elite share, so
	//    nothing cancels (never rides Elite Pack / Apex / Legion -- those would be fully negated) --
	{ ENDLESS_MOD_NOELITE | ENDLESS_MOD_FORTIFIED,   "Grunt Work" },
	{ ENDLESS_MOD_NOELITE | ENDLESS_MOD_DEVASTATING, "Green Troops" },

	// -- rare gambits: TWO boons welded to real danger (bigger upside, bigger risk) --
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_DILATION | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING, "Perfect Storm" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,           "Blood Bargain" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_FORTIFIED,     "High Roller" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_DEVASTATING, "Bullet Ballet" },
};

// Find the theme for an exact effect-bit set (NULL = Calm / no modifiers).
static const EndlessTheme *endlessFindTheme(Uint64 mods)
{
	for (unsigned i = 0; i < COUNTOF(endlessGravityOmniThemes); ++i)
		if (endlessGravityOmniThemes[i].mods == mods)
			return &endlessGravityOmniThemes[i];
	for (unsigned i = 0; i < COUNTOF(endlessHostileThemes); ++i)
		if (endlessHostileThemes[i].mods == mods)
			return &endlessHostileThemes[i];
	for (unsigned i = 0; i < COUNTOF(endlessMixedThemes); ++i)
		if (endlessMixedThemes[i].mods == mods)
			return &endlessMixedThemes[i];
	for (unsigned i = 0; i < COUNTOF(endlessKamikazeThemes); ++i)
		if (endlessKamikazeThemes[i].mods == mods)
			return &endlessKamikazeThemes[i];
	for (unsigned i = 0; i < COUNTOF(endlessHomingThemes); ++i)
		if (endlessHomingThemes[i].mods == mods)
			return &endlessHomingThemes[i];
	for (unsigned i = 0; i < COUNTOF(endlessOverloadThemes); ++i)
		if (endlessOverloadThemes[i].mods == mods)
			return &endlessOverloadThemes[i];
	for (unsigned i = 0; i < COUNTOF(endlessBoonThemes); ++i)
		if (endlessBoonThemes[i].mods == mods)
			return &endlessBoonThemes[i];
	for (unsigned i = 0; i < COUNTOF(endlessRareThemes); ++i)
		if (endlessRareThemes[i].mods == mods)
			return &endlessRareThemes[i];
	for (unsigned i = 0; i < COUNTOF(endlessEvilThemes); ++i)
		if (endlessEvilThemes[i].mods == mods)
			return &endlessEvilThemes[i];
	for (unsigned i = 0; i < COUNTOF(endlessRedlineThemes); ++i)
		if (endlessRedlineThemes[i].mods == mods)
			return &endlessRedlineThemes[i];
	for (unsigned i = 0; i < COUNTOF(endlessSluggishThemes); ++i)
		if (endlessSluggishThemes[i].mods == mods)
			return &endlessSluggishThemes[i];
	for (unsigned i = 0; i < COUNTOF(endlessDeadgenThemes); ++i)
		if (endlessDeadgenThemes[i].mods == mods)
			return &endlessDeadgenThemes[i];
	for (unsigned i = 0; i < COUNTOF(endlessWarpThemes); ++i)
		if (endlessWarpThemes[i].mods == mods)
			return &endlessWarpThemes[i];
	// OMNI fallthrough: an omnidirectional gravity combo with no exact name reads as its plain-gravity
	// twin (Dense Matter, Event Horizon, ...). Strip the cosmetic OMNI bit and retry once -- the retry
	// has no OMNI bit, so it can't recurse further.
	if (mods & ENDLESS_MOD_GRAVITY_OMNI)
		return endlessFindTheme(mods & ~ENDLESS_MOD_GRAVITY_OMNI);
	return NULL;
}

// Pick a random theme's mods from a table, restricted to entries that include ALL `must` bits
// and NONE of the `forbid` bits. This lets the injections draw straight from the name tables
// (endlessBoonThemes / endlessRareThemes) -- so adding a row there makes that sector appear,
// with no separate injection pool to keep in sync. Returns `must` if nothing matches.
static Uint64 endlessPickThemeMods(const EndlessTheme *tbl, unsigned count, Uint64 must, Uint64 forbid)
{
	unsigned n = 0;
	for (unsigned i = 0; i < count; ++i)
		if ((tbl[i].mods & must) == must && (tbl[i].mods & forbid) == 0)
			++n;
	if (n == 0)
		return must;
	unsigned pick = endlessRand() % n;
	for (unsigned i = 0; i < count; ++i)
		if ((tbl[i].mods & must) == must && (tbl[i].mods & forbid) == 0)
			if (pick-- == 0)
				return tbl[i].mods;
	return must;  // unreachable
}

// Bits that make a sector DANGEROUS. The danger score sums only these, so a pure-boon course -- e.g.
// Bounty, which pays big but adds no danger -- never reads as a high tier. Cursed is handled apart
// (it's a trap, not a danger). Defined here (ahead of endlessComboName) because the type-aware naming
// below classifies a combo as hostile / boon / mixed off these masks.
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

// Evocative names for un-curated (randomly generated) combos, picked deterministically per bitset so a
// given combo always reads the same. Three flavors so an un-named combo still reads the RIGHT tone: an
// ominous word for pure danger, a fortunate word for a pure boon combo, a "gambit" word for a mixed one.
// Big pools keep same-chart hash collisions rare (the unique-name pass in endlessGenerateCourses catches
// the rest). Every word must be unique across ALL name tables (curated included -- see the dup scan in
// notes.md) and stick to font_ascii-displayable characters: letters, space, apostrophe, hyphen.
static const char *const endlessGenericNames[] = {
	"Havoc", "Chaos", "Carnage", "Ruin", "Fury", "Terror", "Doom", "Peril",
	"Menace", "Scourge", "Bedlam", "Mayhem", "Torment", "Dread", "Malice",
	"Ordeal", "Gauntlet", "Crucible", "Inferno", "Tempest", "Reckoning",
	"Oblivion", "Rampage", "Turmoil", "Onset", "Affliction",
	"Anguish", "Vengeance", "Spite", "Blight", "Bane", "Calamity",
	"Catastrophe", "Shockwave", "Aftershock", "Fallout", "Deluge", "Torrent",
	"Broadside", "Dragnet", "Crosshairs", "Deathtrap", "Minefield", "Gallows",
	"Ill Omen", "Tribulation", "Strife", "Discord", "Incursion", "Squall",
	"Whirlwind", "Ashfall", "Backdraft", "Misfortune", "Jeopardy", "Hazard",
	"Distress", "Duress", "Snare", "Pitfall", "Vendetta", "Hostile Ground",
	"Scorched Sky", "Grim Tide", "Nightfall", "Darkfall", "Downpour",
	"Hornet Nest", "Viper Pit", "Killing Floor", "Furnace", "Cauldron",
	"Ravage", "Rupture", "Sundering", "Retribution", "Reprisal",
	"Dire Straits", "Storm Front", "Headwinds", "Foul Weather", "Wasteland",
	"Badlands", "Rough Waters", "Breaking Point", "Boiling Point",
	"Fever Pitch", "Graveyard", "Onrush",
};
static const char *const endlessBoonGenericNames[] = {
	"Godsend", "Reprieve", "Tailwind", "Grace", "Easy Street", "Good Omen",
	"Lucky Star", "Silver Lining", "Uplift", "Bright Side", "Fair Winds",
	"Respite", "Blessing", "Sanctuary", "Boon", "Windswept",
	"Serendipity", "Providence", "Good Fortune", "Charmed", "Lucky Streak",
	"Golden Hour", "Halcyon", "Oasis", "Haven", "Safe Harbor", "Safe Passage",
	"Smooth Sailing", "Clear Skies", "Blue Skies", "Daybreak", "First Light",
	"New Dawn", "Morning Star", "North Star", "Guiding Light", "Beacon",
	"Lodestar", "Godspeed", "Cakewalk", "Breather", "Mercy", "Benediction",
	"Deliverance", "Prosperity", "Abundance", "Milk Run", "Sunday Drive",
	"Scenic Route", "Fair Weather", "Green Light", "All Clear", "Home Free",
	"Open Road", "Free Ride", "Sweet Spot", "Kind Stars", "Full Sails",
	"Warm Welcome", "Gentle Current", "Good Graces", "Guardian Angel",
	"Fresh Start", "Head Start", "Helping Hand", "Stroke of Luck",
	"Free Pass", "Sunrise",
};
static const char *const endlessMixedGenericNames[] = {
	"Gambit", "Trade-off", "Bargain", "Double Edge", "Wager", "Faustian",
	"Devil's Deal", "Two-Edged", "Give and Take", "Long Shot", "Roll the Dice",
	"Loaded Dice", "Bittersweet", "Mixed Bag", "Toss-Up", "Wild Card", "Crossroads",
	"Coin Flip", "Heads or Tails", "Double Down", "Ante Up", "Calculated Risk",
	"Risky Business", "Gray Area", "Fine Print", "Hidden Cost", "Price to Pay",
	"Steep Price", "Fair Trade", "Horse Trade", "Quid Pro Quo", "Tit for Tat",
	"Tug of War", "Balancing Act", "Tightrope", "Razor's Edge", "Knife's Edge",
	"Double Bind", "Dilemma", "Conundrum", "Paradox", "Pandora's Box",
	"Poison Apple", "Forbidden Fruit", "Siren Song", "Fool's Gold",
	"Fool's Errand", "Devil's Due", "Snake Eyes", "Hedged Bet", "Side Bet",
	"Long Odds", "Even Odds", "Leap of Faith", "Blind Bargain", "Gilded Cage",
	"Mixed Blessing", "Rose and Thorn", "Gift Horse", "Trojan Horse",
	"Silver Hook", "Honey Trap", "Hard Bargain", "Push Your Luck",
	"Buyer Beware", "Sucker Bet", "Raised Stakes",
};

// `salt` steps a GENERATED pick to the next word in its list -- 0 everywhere except the per-visit
// unique-name pass in endlessGenerateCourses (two distinct bitsets can hash to the same word, and
// one chart must never offer two sectors reading the same). Curated names ignore it: the theme
// tables hold no duplicate names, so distinct combos can't clash through them.
static const char *endlessComboNameSalted(Uint64 mods, unsigned salt)
{
	if (mods & ENDLESS_MOD_THEEND)
		return "The End";                  // the finale is named by its marker, whatever dangers it rolled
	const EndlessTheme *t = endlessFindTheme(mods);
	if (t)
		return t->name;                    // curated combos keep their cool names
	if (mods == 0)
		return "Calm Sector";
	// Mask the cosmetic OMNI bit so an un-named omni gravity combo shares its plain-gravity twin's
	// generated name (the direction is a runtime surprise, not a different sector on the chart).
	Uint64 key = mods & ~ENDLESS_MOD_GRAVITY_OMNI;
	Uint64 h = (key ^ (key >> 4) ^ (key >> 9)) + salt;  // mix so nearby bitsets differ
	// Classify off the danger/boon masks so the generated name matches the sector's tone. Cursed counts
	// hostile-side (it's a trap), matching how the monitor lists it.
	const bool hasHostile = (key & (ENDLESS_HOSTILE_MASK | ENDLESS_MOD_CURSED)) != 0;
	const bool hasBoon    = (key & ENDLESS_BOON_MASK) != 0;
	if (hasBoon && hasHostile)
		return endlessMixedGenericNames[h % COUNTOF(endlessMixedGenericNames)];
	if (hasBoon)
		return endlessBoonGenericNames[h % COUNTOF(endlessBoonGenericNames)];
	return endlessGenericNames[h % COUNTOF(endlessGenericNames)];
}

const char *endlessComboName(Uint64 mods)
{
	return endlessComboNameSalted(mods, 0);
}

// --- Course danger tier + description ------------------------------------------------------------
// The Chart-a-Course help line reads "<Tier>: <what's there>". The tier turns the screen into a
// quick risk read at a glance; the payout (drawn highlighted right after this text by game_menu.c)
// is the matching reward -- and since both derive from the same reward table, they never disagree.

// ENDLESS_HOSTILE_MASK / ENDLESS_BOON_MASK are defined earlier (just above endlessComboName), which
// classifies combos by tone from them; the danger score / tier / rank below reuse the same masks.

// Max pixel width of the description on the Chart-a-Course bar (drawn at x=10 in the 320-wide legacy
// layout, with the highlighted payout right-aligned to x=305 after it). A curated line wider than this
// drops back to the generated worst-first phrase, which packs in only as many threats as fit. Kept a
// hair under the payout's left edge so even a max-length line plus a fat payout stays on the bar.
#define ENDLESS_HELP_DESC_MAX 250

// A survival boon riding a hostile sector makes it play less deadly than its raw threat list:
// frail or crawling-shot foes, harder-hitting or kill-fed guns, or a blitz-past pass all buy time.
// endlessDangerScore credits these against the hostile total so the tier reads net danger. Credits
// are in the same reward-tenths as endlessModTable. Pure-cash boons (Bounty, Favor, Cursed) buy no
// safety, so they grant no credit and don't appear here.
static const struct { Uint64 bit; int credit; } endlessBoonMitigation[] = {
	{ ENDLESS_MOD_DILATION,    8 },  // enemy shots crawl: the biggest dodge cushion
	{ ENDLESS_MOD_FRAGILE,     8 },  // frail foes die fast: fewer guns left firing
	{ ENDLESS_MOD_NOELITE,     8 },  // no elite/champion tier at all: the tanky, hard-hitting shooters simply never appear
	{ ENDLESS_MOD_NOCHAMP,     5 },  // no champions: drops the nastiest tier (1.7x fire, +50% shot dmg, fat HP)
	{ ENDLESS_MOD_OVERCHARGE,  5 },  // shots hit harder: quicker kills
	{ ENDLESS_MOD_OVERDRIVE,   5 },  // each kill stacks fire and damage
	{ ENDLESS_MOD_OVERBLAST,   4 },  // each kill stacks damage
	{ ENDLESS_MOD_TURBODRIVE, 3 },  // each kill quickens the guns
};

// The sector's net danger: its hostile modifiers' summed reward (endlessModTable, in tenths of the
// base) minus any survival-boon credit above. A course with no hostile bits scores 0, so the tier
// words it as Boon/Calm and never needs a number. A hostile course floors at 1, so even a heavily
// mitigated danger still reads at least "Low" rather than collapsing to a boon.
static int endlessDangerScore(Uint64 mods)
{
	const Uint64 h = mods & ENDLESS_HOSTILE_MASK;
	if (h == 0)
		return 0;
	int t = 0;
	for (unsigned i = 0; i < COUNTOF(endlessModTable); ++i)
		if (h & endlessModTable[i].bit)
			t += endlessModTable[i].reward;
	for (unsigned i = 0; i < COUNTOF(endlessBoonMitigation); ++i)
		if (mods & endlessBoonMitigation[i].bit)
			t -= endlessBoonMitigation[i].credit;
	return (t < 1) ? 1 : t;
}

// Tier word shown before a course's description: a one-glance risk read off the net danger score.
// Cursed sectors are Traps; no hostile bits is a Boon (Calm with no mods at all). The nine hostile
// bands below spread the whole score range so single-danger sectors fan out into distinct rungs;
// the thresholds are the tuning knobs, and endlessDangerRank below must stay in lockstep with them
// so the word and the letter grade never disagree.
static const char *endlessDangerTier(Uint64 mods)
{
	if (mods & ENDLESS_MOD_THEEND)          return "FINALITY";  // the 100th-zone finale, a rung above APOCALYPSE
	if (mods & ENDLESS_MOD_CURSED)          return "Trap";
	if ((mods & ENDLESS_HOSTILE_MASK) == 0) return (mods == 0) ? "Calm" : "Boon";
	const int s = endlessDangerScore(mods);
	if (s <=  9) return "Low";
	if (s <= 13) return "Moderate";
	if (s <= 19) return "Tough";
	if (s <= 26) return "High";
	if (s <= 33) return "Severe";
	if (s <= 39) return "Deadly";
	if (s <= 49) return "Extreme";
	if (s <= 59) return "NIGHTMARE";
	return "APOCALYPSE";
}

// Letter-grade twin of endlessDangerTier: the same bands off the same score, level 0 (F, no hostile
// bits at all) up to 9 (S+++); keep the thresholds in lockstep so the pair never disagree. Every
// hostile course floors at score 1, so even the mildest danger reads E, not F. The numeric level is
// the single source of truth for both the letter string and the green-to-red tint the monitor draws
// it in (game_menu.c endlessRankHue[]), so those two can never drift.
static int endlessDangerRankLevel(Uint64 mods)
{
	if (mods & ENDLESS_MOD_THEEND) return 10;         // END -- the 100th-zone finale's own grade
	if ((mods & ENDLESS_HOSTILE_MASK) == 0) return 0; // F
	const int s = endlessDangerScore(mods);
	if (s <=  9) return 1; // E
	if (s <= 13) return 2; // D
	if (s <= 19) return 3; // C
	if (s <= 26) return 4; // B
	if (s <= 33) return 5; // A
	if (s <= 39) return 6; // S
	if (s <= 49) return 7; // S+
	if (s <= 59) return 8; // S++
	return 9;              // S+++
}
// Grade 10 ("END") is off the letter scale on purpose -- it belongs to the finale alone. game_menu.c's
// endlessRankHue[] is indexed by the same level, so the two arrays must stay the same length.
static const char *const endlessRankName[11] = { "F", "E", "D", "C", "B", "A", "S", "S+", "S++", "S+++", "END" };
static const char *endlessDangerRank(Uint64 mods)
{
	return endlessRankName[endlessDangerRankLevel(mods)];
}

// Curated one-line descriptions for memorable combos. These are mechanic-first summaries: each line
// tells the player what changes or why the combination matters, rather than repeating the sector's
// atmospheric name. Keyed by EXACT modifier bitset (same convention as endlessFindTheme); anything
// not here falls back to the generated mechanical body. Kept short on purpose -- endlessComboHelp
// width-checks the finished line and reverts to the auto phrase if a curated one won't fit.
static const struct { Uint64 mods; const char *desc; } endlessCuratedDesc[] = {
	// -- single dangers --
	{ ENDLESS_MOD_FORTIFIED,   "enemies have more HP" },
	{ ENDLESS_MOD_FRENZY,      "enemy fire rate increased" },
	{ ENDLESS_MOD_SWIFT,       "enemy shots move faster" },
	{ ENDLESS_MOD_DEVASTATING, "enemy hits deal more damage" },
	{ ENDLESS_MOD_ENRAGE,      "enemy fire rate keeps rising" },
	{ ENDLESS_MOD_GRAVITY,     "constant downward pull" },
	{ ENDLESS_MOD_ELITEPACK,   "half of enemies are elite" },
	{ ENDLESS_MOD_OVERCLOCK,   "enemy fire/shots + scroll faster" },
	{ ENDLESS_MOD_SLIPSTREAM,  "level scrolls 70% faster" },
	{ ENDLESS_MOD_KAMIKAZE,    "enemies home in moderately" },
	{ ENDLESS_MOD_HOMING,      "enemies home in slightly" },
	{ ENDLESS_MOD_OVERLOAD,    "enemy fire/shots + scroll extreme" },
	{ ENDLESS_MOD_WARP,        "level scrolls 3.2x faster" },
	{ ENDLESS_MOD_RAMPAGE,     "strong homing + ram damage" },
	{ ENDLESS_MOD_OVERHEAT,    "hull slowly loses armor" },
	{ ENDLESS_MOD_TOPSY,       "view and controls flip" },
	{ ENDLESS_MOD_SLUGGISH,    "all ship movement is slowed" },
	{ ENDLESS_MOD_SHIELDLESS,  "shields cannot recharge" },
	{ ENDLESS_MOD_DEADGEN,     "no shield regen; gun starved" },
	{ ENDLESS_MOD_GRAVITY | ENDLESS_MOD_GRAVITY_OMNI, "gravity pulls any direction" },
	// -- signature elite tiers --
	{ ENDLESS_MOD_APEX,        "every foe is elite" },
	{ ENDLESS_MOD_LEGION,      "every foe a champion" },
	// -- common hostile pairs --
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT,       "more enemy HP; faster shots" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY,      "more enemy HP; faster fire" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING, "more enemy HP; harder hits" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,          "faster enemy fire and shots" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_DEVASTATING,    "faster fire; harder hits" },
	{ ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,     "faster shots; harder hits" },
	{ ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_FORTIFIED,   "half elite; more enemy HP" },
	{ ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_SWIFT,       "half elite; faster shots" },
	{ ENDLESS_MOD_GRAVITY | ENDLESS_MOD_SWIFT,         "downward pull; faster shots" },
	{ ENDLESS_MOD_ENRAGE | ENDLESS_MOD_FORTIFIED,      "more HP; fire rate rises" },
	{ ENDLESS_MOD_ENRAGE | ENDLESS_MOD_SWIFT,          "fast shots; fire rate rises" },
	{ ENDLESS_MOD_ENRAGE | ENDLESS_MOD_DEVASTATING,    "hard hits; fire rate rises" },
	{ ENDLESS_MOD_GRAVITY | ENDLESS_MOD_FORTIFIED,     "downward pull; more enemy HP" },
	{ ENDLESS_MOD_GRAVITY | ENDLESS_MOD_DEVASTATING,   "downward pull; harder hits" },
	{ ENDLESS_MOD_GRAVITY | ENDLESS_MOD_FRENZY,        "downward pull; faster fire" },
	{ ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_DEVASTATING, "half elite; harder hits" },
	{ ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_FRENZY,      "half elite; faster fire" },
	{ ENDLESS_MOD_OVERCLOCK | ENDLESS_MOD_FORTIFIED,   "foe fire/shot/scroll+; more HP" },
	{ ENDLESS_MOD_OVERCLOCK | ENDLESS_MOD_DEVASTATING, "foe fire/shot/scroll+; hard hits" },
	{ ENDLESS_MOD_OVERCLOCK | ENDLESS_MOD_GRAVITY,     "foe fire/shot/scroll+; pull down" },
	{ ENDLESS_MOD_OVERCLOCK | ENDLESS_MOD_ELITEPACK,   "foe fire/shot/scroll+; 1/2 elite" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_ENRAGE,         "fast fire that keeps rising" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_OVERCLOCK,      "foe fire++; shots/scroll faster" },
	{ ENDLESS_MOD_SWIFT | ENDLESS_MOD_OVERCLOCK,       "foe shots++; fire/scroll faster" },
	{ ENDLESS_MOD_ENRAGE | ENDLESS_MOD_GRAVITY,        "pull down; fire rate rises" },
	{ ENDLESS_MOD_ENRAGE | ENDLESS_MOD_ELITEPACK,      "half elite; fire rate rises" },
	{ ENDLESS_MOD_ENRAGE | ENDLESS_MOD_OVERCLOCK,      "foe fire rises; shot/scroll faster" },
	{ ENDLESS_MOD_GRAVITY | ENDLESS_MOD_ELITEPACK,     "downward pull; half elite" },
	// -- slipstream (faster scroll) pairs --
	{ ENDLESS_MOD_SLIPSTREAM | ENDLESS_MOD_FRENZY,      "scroll +70%; faster fire" },
	{ ENDLESS_MOD_SLIPSTREAM | ENDLESS_MOD_DEVASTATING, "scroll +70%; harder hits" },
	{ ENDLESS_MOD_SLIPSTREAM | ENDLESS_MOD_FORTIFIED,   "scroll +70%; more enemy HP" },
	// -- disorientation, drag and stripped defenses --
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_FORTIFIED,       "flipped view; more enemy HP" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_FRENZY,          "flipped view; faster fire" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_SWIFT,           "flipped view; faster shots" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_DEVASTATING,     "flipped view; harder hits" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_ENRAGE,          "flipped view; fire rate rises" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_ELITEPACK,       "flipped view; half elite" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_OVERCLOCK,       "view flip; foe fire/shot/scroll+" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_GRAVITY,         "flipped view + downward pull" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_SLUGGISH,        "flipped view; ship slowed" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_FORTIFIED,    "slower ship; more enemy HP" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_FRENZY,       "slower ship; faster fire" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_SWIFT,        "slower ship; faster shots" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_DEVASTATING,  "slower ship; harder hits" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_ENRAGE,       "slower ship; fire rate rises" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_ELITEPACK,    "slower ship; half elite" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_OVERCLOCK,    "ship slow; foe fire/shot/scroll+" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_FORTIFIED,  "no shield regen; more foe HP" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_FRENZY,     "no shield regen; faster fire" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_SWIFT,      "no shield regen; fast shots" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_DEVASTATING,"no shield regen; harder hits" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_ENRAGE,     "no regen; fire rate rises" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_ELITEPACK,  "no shield regen; half elite" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_OVERCLOCK,  "no regen; foe fire/shot/scroll+" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_GRAVITY,    "no regen; downward pull" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_TOPSY,      "no regen; view flips" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_SLUGGISH,   "no regen; ship slowed" },
	// -- marquee triples / quads --
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,       "more HP; faster fire and shots" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,     "fast fire/shots; harder hits" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,  "more HP; fast, harder shots" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING, "more HP; fast shots hit hard" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_ENRAGE,    "fast, hard shots; fire rises" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_ENRAGE, "HP+; shots fast/hard; fire+" },
	{ ENDLESS_MOD_OVERCLOCK | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING, "foe fire/shot++; scroll+; hit+" },
	{ ENDLESS_MOD_ENRAGE | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,     "fast, hard shots; fire rises" },
	{ ENDLESS_MOD_GRAVITY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,    "downward pull; fast hard shots" },
	{ ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING, "half elite; more HP; hard hits" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_DEVASTATING, "more HP; fast fire; hard hits" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_DEVASTATING,"pull down; more HP; hard hits" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_DEVASTATING,   "pull; fast fire; hard hits" },
	{ ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,  "half elite; fast hard shots" },
	{ ENDLESS_MOD_OVERCLOCK | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,  "foe shots++; fire/scroll+; hit+" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,           "view flips; fire/shots faster" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING,  "view flips; more HP; hard hits" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_DEVASTATING,    "view flips; pull; hard hits" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,        "ship slowed; fast fire/shots" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING, "slow ship; more HP; hard hits" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,      "no regen; fast fire and shots" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING, "no regen; more HP; hard hits" },
	// -- pursuit sectors --
	{ ENDLESS_MOD_HOMING | ENDLESS_MOD_FORTIFIED,       "light homing; more enemy HP" },
	{ ENDLESS_MOD_HOMING | ENDLESS_MOD_FRENZY,          "light homing; faster fire" },
	{ ENDLESS_MOD_HOMING | ENDLESS_MOD_SWIFT,           "light homing; faster shots" },
	{ ENDLESS_MOD_HOMING | ENDLESS_MOD_DEVASTATING,     "light homing; harder hits" },
	{ ENDLESS_MOD_HOMING | ENDLESS_MOD_GRAVITY,         "light homing; downward pull" },
	{ ENDLESS_MOD_HOMING | ENDLESS_MOD_ELITEPACK,       "light homing; half elite" },
	{ ENDLESS_MOD_HOMING | ENDLESS_MOD_ENRAGE,          "light homing; fire rate rises" },
	{ ENDLESS_MOD_KAMIKAZE | ENDLESS_MOD_FORTIFIED,     "moderate homing; more foe HP" },
	{ ENDLESS_MOD_KAMIKAZE | ENDLESS_MOD_FRENZY,        "moderate homing; faster fire" },
	{ ENDLESS_MOD_KAMIKAZE | ENDLESS_MOD_SWIFT,         "moderate homing; fast shots" },
	{ ENDLESS_MOD_KAMIKAZE | ENDLESS_MOD_DEVASTATING,   "moderate homing; harder hits" },
	{ ENDLESS_MOD_KAMIKAZE | ENDLESS_MOD_GRAVITY,       "moderate homing; pull down" },
	{ ENDLESS_MOD_KAMIKAZE | ENDLESS_MOD_ELITEPACK,     "moderate homing; half elite" },
	{ ENDLESS_MOD_KAMIKAZE | ENDLESS_MOD_ENRAGE,        "homing; fire rate keeps rising" },
	{ ENDLESS_MOD_KAMIKAZE | ENDLESS_MOD_OVERCLOCK,     "homing; foe fire/shot/scroll+" },
	// -- overload, apex and legion --
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_FORTIFIED,     "foe fire/shot/scroll++; more HP" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_DEVASTATING,   "foe fire/shot/scroll++; hard hits" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_ELITEPACK,     "foe fire/shot/scroll++; 1/2 elite" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_FRENZY,        "foe fire++; shots/scroll extreme" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_SWIFT,         "foe shots++; fire/scroll extreme" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_GRAVITY,       "foe fire/shot/scroll++; pull down" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_ENRAGE,        "foe fire rises; shots/scroll++" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING, "foe fire/shot/scroll++; HP+; hit+" },
	{ ENDLESS_MOD_OVERLOAD | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,     "foe shots+++; fire/scroll++; hit+" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_FORTIFIED,         "all elite; more enemy HP" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_SWIFT,             "all elite; faster shots" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_DEVASTATING,       "all elite; harder hits" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_OVERLOAD,          "all elite; foe fire/shot/scroll++" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_FRENZY,            "all elite; faster fire" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_ENRAGE,            "all elite; fire rate rises" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_GRAVITY,           "all elite; downward pull" },
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_FORTIFIED,       "all champions; more enemy HP" },
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_SWIFT,           "all champions; faster shots" },
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_DEVASTATING,     "all champions; harder hits" },
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_OVERLOAD,        "all champs; foe fire/shot/scroll++" },
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_FRENZY,          "all champions; faster fire" },
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_ENRAGE,          "champions; fire rate rises" },
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_GRAVITY,         "all champions; downward pull" },
	// -- boons --
	{ ENDLESS_MOD_BOUNTY,      "larger clear-cash payout" },
	{ ENDLESS_MOD_FRAGILE,     "enemies have less HP" },
	{ ENDLESS_MOD_TURBODRIVE, "kills quicken your guns" },
	{ ENDLESS_MOD_OVERCHARGE,  "your weapon damage +50%" },
	{ ENDLESS_MOD_DILATION,    "enemy shots move 45% slower" },
	{ ENDLESS_MOD_FAVOR,       "next shop is cheaper" },
	{ ENDLESS_MOD_OVERDRIVE,   "kills stack fire and damage" },
	{ ENDLESS_MOD_OVERBLAST,   "kills stack weapon damage" },
	{ ENDLESS_MOD_CURSED,      "more cash; next shop empty" },
	{ ENDLESS_MOD_NOCHAMP,     "no champion enemies spawn" },
	{ ENDLESS_MOD_NOELITE,     "no elite or champion enemies" },
	// -- stacked boons --
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_OVERCHARGE, "+50% damage; kill-fire boost" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_OVERCHARGE,   "+50% damage; enemy shots slow" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_TURBODRIVE,   "slow shots; kills boost fire" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_BOUNTY,        "less enemy HP; larger payout" },
	{ ENDLESS_MOD_FAVOR | ENDLESS_MOD_BOUNTY,          "larger payout; cheaper shop" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_TURBODRIVE,    "less foe HP; kills boost fire" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_OVERCHARGE,    "less foe HP; your damage +50%" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_FAVOR,         "less foe HP; cheaper next shop" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_BOUNTY,     "fire boost; larger payout" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_BOUNTY,     "+50% damage; larger payout" },
	{ ENDLESS_MOD_OVERDRIVE | ENDLESS_MOD_OVERCHARGE,  "+50% base; kills stack fire/dmg" },
	{ ENDLESS_MOD_OVERBLAST | ENDLESS_MOD_OVERCHARGE,  "+50% base; kills stack damage" },
	{ ENDLESS_MOD_OVERBLAST | ENDLESS_MOD_DILATION,    "slow foe shots; damage stacks" },
	{ ENDLESS_MOD_OVERBLAST | ENDLESS_MOD_BOUNTY,      "damage stacks; larger payout" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_OVERCHARGE, "+damage; slow shots; kill-fire" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_DILATION,      "less foe HP; foe shots -45%" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_OVERDRIVE,     "less HP; kills stack fire/dmg" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_FAVOR,      "kills boost fire; cheaper shop" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_OVERDRIVE,  "kills boost fire and damage" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_FAVOR,      "+50% damage; cheaper next shop" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_FAVOR,        "foe shots -45%; cheaper shop" },
	// -- gambits: a boon riding the faster-scroll threat --
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_SLIPSTREAM,    "less foe HP; scroll +70%" },
	{ ENDLESS_MOD_SLIPSTREAM | ENDLESS_MOD_BOUNTY,     "scroll +70%; larger payout" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_SLIPSTREAM,   "foe shots -45%; scroll +70%" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_SLIPSTREAM, "kills boost fire; scroll +70%" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_SLIPSTREAM, "+50% damage; scroll +70%" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_BOUNTY,       "foe shots -45%; larger payout" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_OVERDRIVE,    "slow shots; kills stack fire/dmg" },
	// -- evil self-curses --
	{ ENDLESS_MOD_BACKFIRE,    "kills briefly slow your fire" },
	{ ENDLESS_MOD_BURNOUT,     "kill-stacks cut fire + damage" },
	{ ENDLESS_MOD_MISFIRE,     "kill-stacks cut your damage" },
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_DEVASTATING, "kills jam guns; harder foe hits" },
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_FORTIFIED,   "kills jam guns; more enemy HP" },
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_SWIFT,       "kills jam guns; faster shots" },
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_GRAVITY,     "kills jam guns; downward pull" },
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_FRENZY,      "gun jams; foe fire faster" },
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_ENRAGE,      "your fire jams; foe fire rises" },
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_ELITEPACK,   "kills jam guns; half foes elite" },
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_OVERCLOCK,   "gun jams; foe fire/shot/scroll+" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_DEVASTATING,  "kills cut fire/dmg; harder hits" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_FORTIFIED,    "kills cut fire/dmg; more foe HP" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_ENRAGE,       "guns weaken; foe fire rises" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_ELITEPACK,    "kills cut fire/dmg; half elite" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_FRENZY,       "kills cut fire; foe fire faster" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_SWIFT,        "kills cut fire/dmg; fast shots" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_GRAVITY,      "kills cut fire/dmg; pull down" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_OVERCLOCK,    "guns weaken; foe fire/shot/scroll+" },
	{ ENDLESS_MOD_MISFIRE | ENDLESS_MOD_DEVASTATING,  "kills cut dmg; foes hit harder" },
	{ ENDLESS_MOD_MISFIRE | ENDLESS_MOD_FORTIFIED,    "kills cut damage; more enemy HP" },
	{ ENDLESS_MOD_MISFIRE | ENDLESS_MOD_SWIFT,        "kills cut dmg; foe shots faster" },
	{ ENDLESS_MOD_MISFIRE | ENDLESS_MOD_FRENZY,       "kills cut dmg; foe fire faster" },
	{ ENDLESS_MOD_MISFIRE | ENDLESS_MOD_ENRAGE,       "kills cut damage; foe fire rises" },
	{ ENDLESS_MOD_MISFIRE | ENDLESS_MOD_GRAVITY,      "kills cut damage; downward pull" },
	{ ENDLESS_MOD_MISFIRE | ENDLESS_MOD_ELITEPACK,    "kills cut dmg; half foes elite" },
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING, "gun jams; more HP; harder hits" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,      "fire/dmg fall; fast hard shots" },
	{ ENDLESS_MOD_MISFIRE | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT,        "damage falls; HP+; fast shots" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_FRENZY | ENDLESS_MOD_DEVASTATING,     "fire/dmg fall; foe fire+; hit+" },
	// -- rare self-sabotage nightmares --
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_GRAVITY, "ship slowed; downward pull" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_DEVASTATING, "ship slow; pull; harder hits" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_FRENZY, "ship slow; pull; faster fire" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_SWIFT, "ship slow; pull; faster shots" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_FORTIFIED, "ship slow; pull; more foe HP" },
	{ ENDLESS_MOD_DEADGEN | ENDLESS_MOD_DEVASTATING, "no regen/weak gun; hard hits" },
	{ ENDLESS_MOD_DEADGEN | ENDLESS_MOD_FRENZY,      "no regen/weak gun; fast fire" },
	{ ENDLESS_MOD_DEADGEN | ENDLESS_MOD_SWIFT,       "no regen/weak gun; fast shots" },
	{ ENDLESS_MOD_DEADGEN | ENDLESS_MOD_ELITEPACK,   "no regen/weak gun; 1/2 elite" },
};

static const char *endlessCuratedDescFor(Uint64 mods)
{
	for (unsigned i = 0; i < COUNTOF(endlessCuratedDesc); ++i)
		if (endlessCuratedDesc[i].mods == mods)
			return endlessCuratedDesc[i].desc;
	// Same OMNI fallthrough endlessFindTheme does: an omnidirectional well is the same SECTOR as its
	// plain-gravity twin, so it keeps the twin's curated line rather than dropping to the generated
	// threat list. Strip the cosmetic bit and retry once (the retry can't recurse -- no OMNI left).
	if (mods & ENDLESS_MOD_GRAVITY_OMNI)
		return endlessCuratedDescFor(mods & ~ENDLESS_MOD_GRAVITY_OMNI);
	return NULL;
}

// Render the first `shown` of `total` worst-first threats as a natural phrase: a comma list with the
// final pair joined by "and" ("A", "A and B", "A, B and C"), or a truncated worst-first run tagged
// "and more" when milder threats are left off ("A and more", "A, B and more"). Shared by the fit loop
// below so the width it measures is exactly the width it renders.
static void endlessJoinThreats(const int *idx, int shown, int total, char *out, size_t sz)
{
	char acc[128] = "";
	for (int i = 0; i < shown; ++i)
	{
		const char *w = endlessModTable[idx[i]].word;
		char next[128];
		if (i == 0)
			snprintf(next, sizeof next, "%s", w);
		else if (i == shown - 1 && shown == total)   // final word of a COMPLETE list -> "... and w"
			snprintf(next, sizeof next, "%s and %s", acc, w);
		else
			snprintf(next, sizeof next, "%s, %s", acc, w);
		SDL_strlcpy(acc, next, sizeof acc);
	}
	if (shown < total)
		snprintf(out, sz, "%s and more", acc);
	else
		snprintf(out, sz, "%s", acc);
}

// Auto-generate a terse body for an un-curated combo: the active threats worst-first (highest reward),
// packing in as many as FIT the given pixel budget and tagging "and more" when the rest are dropped --
// so a busy sector always leads with its deadliest dangers and a cut line only ever loses the mildest.
// Pure-boon sectors list their top benefits the same way (a boon's low reward just sorts it later).
static void endlessAutoBody(Uint64 mods, char *out, size_t sz, unsigned font, int maxW)
{
	int idx[COUNTOF(endlessModTable)], n = 0;
	for (unsigned i = 0; i < COUNTOF(endlessModTable); ++i)
		if ((mods & endlessModTable[i].bit) && endlessModTable[i].word != NULL)
			idx[n++] = (int)i;  // a NULL word is a label-only bit (the finale marker), not a threat to list

	if (n == 0) { snprintf(out, sz, "no threats"); return; }

	// Order the active mods by reward, worst first (insertion sort; n is tiny).
	for (int a = 1; a < n; ++a)
	{
		const int key = idx[a];
		int b = a - 1;
		while (b >= 0 && endlessModTable[idx[b]].reward < endlessModTable[key].reward)
		{
			idx[b + 1] = idx[b];
			--b;
		}
		idx[b + 1] = key;
	}

	// Prefer the complete list; if it overruns the budget, fall back to the widest leading (worst-first)
	// run that fits -- always showing at least the single deadliest threat. The truncated forms grow
	// monotonically, so a linear scan from 2 up finds the cutoff; the full list is tried first because
	// its "and w" tail can be shorter than the truncated "and more" it would replace.
	int shown = n;
	char trial[160];
	endlessJoinThreats(idx, n, n, trial, sizeof trial);
	if (n > 1 && JE_textWidth(trial, font) > maxW)
	{
		shown = 1;
		for (int k = 2; k < n; ++k)
		{
			endlessJoinThreats(idx, k, n, trial, sizeof trial);
			if (JE_textWidth(trial, font) <= maxW)
				shown = k;
			else
				break;
		}
	}
	endlessJoinThreats(idx, shown, n, out, sz);
}

// Hard-truncate a string until it fits maxW pixels -- a last-resort safety (curated and auto bodies
// are already short), so nothing can ever spill past the help bar.
static void endlessClampWidth(char *s, unsigned font, int maxW)
{
	for (int len = (int)strlen(s); len > 0 && JE_textWidth(s, font) > maxW; )
		s[--len] = '\0';
}

const char *endlessComboHelp(Uint64 mods)
{
	static char buf[192];

	if (mods == 0)  // Calm: "Calm" already IS the description, so no separate threat phrase
	{
		snprintf(buf, sizeof buf, "Calm: clear skies ahead");
		return buf;
	}

	const char *tier = endlessDangerTier(mods);

	// The "<Tier>: " prefix eats into the bar, so the generated body only gets what's left of the budget.
	char prefix[32];
	snprintf(prefix, sizeof prefix, "%s: ", tier);
	const int bodyMaxW = ENDLESS_HELP_DESC_MAX - JE_textWidth(prefix, TINY_FONT);

	// Body: an exact-match curated one-liner, else a fixed short line for any Cursed trap, else the
	// generated phrase. Width-check the curated line against the bar; if it would overrun, fall back
	// to the worst-first auto body (which fits itself to bodyMaxW) and hard-clamp as a final safety
	// net -- so nothing can push the payout off the bar.
	const char *cur = endlessCuratedDescFor(mods);
	if (cur == NULL && (mods & ENDLESS_MOD_CURSED))
		cur = "rich now, barren shop next";
	if (cur != NULL)
	{
		snprintf(buf, sizeof buf, "%s%s", prefix, cur);
		if (JE_textWidth(buf, TINY_FONT) <= ENDLESS_HELP_DESC_MAX)
			return buf;
	}

	char body[128];
	endlessAutoBody(mods, body, sizeof body, TINY_FONT, bodyMaxW);
	snprintf(buf, sizeof buf, "%s%s", prefix, body);
	endlessClampWidth(buf, TINY_FONT, ENDLESS_HELP_DESC_MAX);
	return buf;
}

// The TRUTHFUL clear payout for course i at the current depth -- derived from the same table
// (endlessClearBonusFor) that actually pays it out, so the shown and banked amounts can't
// disagree. Shown highlighted on the course's help line (game_menu.c).
long endlessCoursePayout(int i)
{
	if (i < 0 || i >= endlessCourseCnt)
		return 0;
	return endlessClearBonusFor(endlessCourseMod[i]);
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

// Build a random pure-hostile combo whose letter grade is exactly `rank` (7 = S+, 8 = S++, 9 = S+++),
// distinct from the `usedN` bitsets already dealt this visit. The bands mirror endlessDangerRankLevel
// -- keep the two in lockstep -- and the built combo is checked against it before being handed back,
// so a retuned band can never silently mislabel a milestone. S+++ is open-ended, but the build stops
// well short of piling on every bit in the pool: brutal, still flyable.
static Uint64 endlessMakeRankCombo(int rank, const Uint64 *used, int usedN)
{
	const int lo = (rank <= 7) ? 40 : (rank == 8) ? 50 : 60;
	const int hi = (rank <= 7) ? 49 : (rank == 8) ? 59 : 95;

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

void endlessGenerateCourses(void)
{
	endlessCourseCnt = 0;

	const int milestone = endlessMilestoneKind();  // 0 ordinary zone, 1 S+/S++ slate, 2 S++/S+++ slate

	// This visit offers a random 2..5 course choices (fewer only if there aren't enough
	// distinct safe levels to fill them); a milestone always asks for the full slate.
	int wantCourses = 2 + (int)(endlessRand() % (ENDLESS_MAX_COURSES - 1));  // 2..5
	if (milestone)
		wantCourses = ENDLESS_MAX_COURSES;

	// Gather up to wantCourses candidate levels. endlessRandomSafeLevel already keeps each pick out
	// of the recent-play window (so no course repeats the just-played zone or the few before it);
	// here we only dedup WITHIN this visit, by (episode, section) -- a visit never offers two courses
	// that share a section, so the two TYRIAN cuts can't both appear at once (they'd read as the same
	// course), but either cut can be the section-3 course on any given visit.
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

	// Course 0 is always clean (no modifiers, neither good nor bad).
	endlessCourseMod[0] = 0;

	// Courses 1+ get distinct hostile themes shuffled from the pool (kamikaze is its own pool),
	// so a visit never offers the same danger twice.
	int idx[COUNTOF(endlessHostileThemes)];
	for (unsigned i = 0; i < COUNTOF(endlessHostileThemes); ++i)
		idx[i] = (int)i;
	for (int i = (int)COUNTOF(endlessHostileThemes) - 1; i > 0; --i)
	{
		const int j = endlessRand() % (i + 1);
		const int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
	}
	for (int c = 1; c < endlessCourseCnt; ++c)
		endlessCourseMod[c] = endlessHostileThemes[idx[c - 1]].mods;

	// WIDEN VARIETY: about half the hostile courses instead get a RANDOM combination of the
	// common hostile bits (any un-named combo still gets a generated name/help). Weighted
	// toward 2-4 bits; kamikaze/overload/warp/sluggish stay out (special or rare-injected -- SLUGGISH's
	// only combinable pairing, gravity, is a rare injection, so it can't emerge randomly here).
	static const Uint64 combinable[] = {
		ENDLESS_MOD_FORTIFIED, ENDLESS_MOD_FRENZY, ENDLESS_MOD_SWIFT, ENDLESS_MOD_DEVASTATING,
		ENDLESS_MOD_ENRAGE, ENDLESS_MOD_GRAVITY, ENDLESS_MOD_ELITEPACK, ENDLESS_MOD_OVERCLOCK,
		ENDLESS_MOD_TOPSY,  // the flipped-view mod mixes freely with everything (purely visual, no softlock)
		ENDLESS_MOD_SHIELDLESS,  // a pure defense debuff -- safe to stack onto any combo (DEADGEN stays out: super-rare, injected only)
		// SLIPSTREAM stays out: Overclock (in the pool) already carries the same +70% scroll, so a
		// random pairing would be a redundant bit. Slipstream sectors come from the named-theme shuffle.
	};
	const int dangerRamp = endlessDangerRamp();  // 0 (z40) -> 100 (z100) -> 350 (z250 cap) -- deep-run danger tilt
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
		if (want > (int)COUNTOF(combinable))
			want = (int)COUNTOF(combinable);
		int ord[COUNTOF(combinable)];
		for (unsigned k = 0; k < COUNTOF(combinable); ++k)
			ord[k] = (int)k;
		for (int k = (int)COUNTOF(combinable) - 1; k > 0; --k)
		{
			const int j = endlessRand() % (k + 1);
			const int tmp = ord[k]; ord[k] = ord[j]; ord[j] = tmp;
		}
		Uint64 combo = 0;
		for (int k = 0; k < want; ++k)
			combo |= combinable[ord[k]];
		endlessCourseMod[c] = combo;
	}

	// A boon course is uncommon (~1 in 3 visits replaces a hostile one): most draw a named boon theme
	// (single or curated combo), but ~40% instead roll a fresh emergent boon pair/triple, so
	// pure-good sectors vary beyond the named set too. The danger ramp thins boon courses deep (~1/3
	// early down to ~1/6 at the cap), but they never vanish.
	if (endlessCourseCnt > 1 && (endlessRand() % (3 + dangerRamp * 3 / 100)) == 0)
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

	// MIXED "gambit" sectors: graft a compatible boon onto some ORDINARY hostile courses, welding a real
	// reward onto real danger. This is the main source of good-mixed-with-bad variety -- roughly a third of
	// the plain hostile courses become a risk/reward gambit each visit. Only "ordinary" courses (hostile
	// bits drawn from the combinable pool) are eligible, so rare signatures (Apex / Overload / Kamikaze /
	// Tar Pit / dead generator / evil-mirror / ...) and boon/clean routes keep their identity. The boon is
	// chosen to never fight the course's threats (endlessPickMixBoon), and only one is added, so the
	// one-kill-fire rule holds. Named pairs/triples/quads land their endlessMixedThemes label; the rest read
	// as a generated "gambit". Runs after the boon roll (won't override a boon course) and before the danger
	// sort, so the mitigation credit lands the gambit at the right rung.
	{
		Uint64 mixCommon = 0;
		for (unsigned k = 0; k < COUNTOF(combinable); ++k)
			mixCommon |= combinable[k];
		// Slipstream isn't in the combinable pool (redundant beside Overclock's scroll), but its named
		// hostile sectors are ordinary enough to gamble on -- keep them boon-graft eligible so the
		// Blitz / Time Warp / Power Play / Payday / Smash and Grab gambits stay reachable.
		mixCommon |= ENDLESS_MOD_SLIPSTREAM;
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

	// Every rare/super-rare danger injection below routes its "1 in N" divisor through
	// endlessDangerRareDiv, so from zone 40 they all grow steadily more frequent (about 2x by zone
	// 100). The base rarities (the comments' "~1 in N") are the zone-<=40 values.
	//
	// Homing sectors are the GENTLEST homing tier (enemies barely lean toward you, no ram) -- rare,
	// ~1 in 25 visits one hostile course becomes a random theme from the homing pool.
	if (endlessCourseCnt > 1 && (endlessRand() % endlessDangerRareDiv(25)) == 0)
	{
		const int slot = 1 + endlessRand() % (endlessCourseCnt - 1);
		endlessCourseMod[slot] = endlessHomingThemes[endlessRand() % COUNTOF(endlessHomingThemes)].mods;
	}

	// Kamikaze sectors are now the MODERATE homing tier (strength 3, no ram -- the brutal rammer moved
	// to the RAMPAGE gamble). Still rare -- ~1 in 50 visits one hostile course becomes a kamikaze theme.
	// Rolled after homing so that, on a clash on one slot, the harder kamikaze wins it.
	if (endlessCourseCnt > 1 && (endlessRand() % endlessDangerRareDiv(50)) == 0)
	{
		const int slot = 1 + endlessRand() % (endlessCourseCnt - 1);
		endlessCourseMod[slot] = endlessKamikazeThemes[endlessRand() % COUNTOF(endlessKamikazeThemes)].mods;
	}

	// Warp Speed (much faster scroll -- a rare scroll THREAT: the level hurtles at you) -- ~1 in 12 visits.
	if (endlessCourseCnt > 1 && (endlessRand() % endlessDangerRareDiv(12)) == 0)
	{
		const int slot = 1 + endlessRand() % (endlessCourseCnt - 1);
		endlessCourseMod[slot] = ENDLESS_MOD_WARP;
	}

	// Overload (Overclock cranked way up, a rare brutal sector) -- ~1 in 14 visits.
	if (endlessCourseCnt > 1 && (endlessRand() % endlessDangerRareDiv(14)) == 0)
	{
		const int slot = 1 + endlessRand() % (endlessCourseCnt - 1);
		endlessCourseMod[slot] = endlessOverloadThemes[endlessRand() % COUNTOF(endlessOverloadThemes)].mods;
	}

	// Evil Turbodrive / Overdrive (a curse that turns your own kill streak against you: jammed
	// guns, and for Evil Overdrive weaker shots too) -- rare, ~1 in 16 visits one hostile course
	// becomes an evil-mirror sector drawn from its own pool.
	if (endlessCourseCnt > 1 && (endlessRand() % endlessDangerRareDiv(16)) == 0)
	{
		const int slot = 1 + endlessRand() % (endlessCourseCnt - 1);
		endlessCourseMod[slot] = endlessEvilThemes[endlessRand() % COUNTOF(endlessEvilThemes)].mods;
	}

	// Reactor Redline (the gamble "Overheat" as a wild sector: kills quicken your guns, but the
	// redlined core cooks your hull) -- super rare, ~1 in 60 visits one hostile course goes redline.
	if (endlessCourseCnt > 1 && (endlessRand() % endlessDangerRareDiv(60)) == 0)
	{
		const int slot = 1 + endlessRand() % (endlessCourseCnt - 1);
		endlessCourseMod[slot] = endlessRedlineThemes[endlessRand() % COUNTOF(endlessRedlineThemes)].mods;
	}

	// Tar Pit (SLUGGISH + GRAVITY: the ship crawls WHILE dragged down) -- rare, ~1 in 30 visits one
	// hostile course becomes a heavy-and-slow nightmare from its own pool. Brutal, but always flyable
	// (endlessGravityDrift slows the pull with the ship). SLUGGISH stays out of the combinable pool, so
	// this injection is the ONLY place the sluggish+gravity pairing appears.
	if (endlessCourseCnt > 1 && (endlessRand() % endlessDangerRareDiv(30)) == 0)
	{
		const int slot = 1 + endlessRand() % (endlessCourseCnt - 1);
		endlessCourseMod[slot] = endlessSluggishThemes[endlessRand() % COUNTOF(endlessSluggishThemes)].mods;
	}

	// Apex Swarm (every enemy elite) is a rare, nasty sector -- ~1 in 40 visits it takes over one
	// hostile course, drawn from the Apex-tier rare themes (bare Apex, or Apex + an extra danger).
	// Rolled late so it can override a boon slot.
	if (endlessCourseCnt > 1 && (endlessRand() % endlessDangerRareDiv(40)) == 0)
	{
		const int slot = 1 + endlessRand() % (endlessCourseCnt - 1);
		endlessCourseMod[slot] = endlessPickThemeMods(endlessRareThemes, COUNTOF(endlessRareThemes), ENDLESS_MOD_APEX, ENDLESS_MOD_LEGION);
	}

	// Legion (every enemy a CHAMPION) is rarer still -- among the deadliest sectors; drawn from
	// the Legion-tier rare themes.
	if (endlessCourseCnt > 1 && (endlessRand() % endlessDangerRareDiv(70)) == 0)
	{
		const int slot = 1 + endlessRand() % (endlessCourseCnt - 1);
		endlessCourseMod[slot] = endlessPickThemeMods(endlessRareThemes, COUNTOF(endlessRareThemes), ENDLESS_MOD_LEGION, 0);
	}

	// Cataclysm: an extreme multi-danger nightmare (no elite tier -- just everything at once), a
	// rare pure-hostile apex -- ~1 in 45 visits. Drawn from the rare themes carrying neither the
	// Apex nor Legion bit (the 5+-danger pure combos).
	if (endlessCourseCnt > 1 && (endlessRand() % endlessDangerRareDiv(45)) == 0)
	{
		const int slot = 1 + endlessRand() % (endlessCourseCnt - 1);
		endlessCourseMod[slot] = endlessPickThemeMods(endlessRareThemes, COUNTOF(endlessRareThemes), 0, ENDLESS_MOD_APEX | ENDLESS_MOD_LEGION);
	}

	// Dead Generator (DEADGEN: no shield regen AND a power-starved main gun) -- SUPER rare, ~1 in 55
	// visits one hostile course becomes a sabotage sector from its own pool. The nastiest handicap in
	// the game, so it's the rarest; rear guns / sidekicks / specials carry the fight. Rolled last of the
	// danger injections so it claims the slot when it fires.
	if (endlessCourseCnt > 1 && (endlessRand() % endlessDangerRareDiv(55)) == 0)
	{
		const int slot = 1 + endlessRand() % (endlessCourseCnt - 1);
		endlessCourseMod[slot] = endlessDeadgenThemes[endlessRand() % COUNTOF(endlessDeadgenThemes)].mods;
	}

	// Guarantee the offered courses are all distinct. The shuffle assigns distinct base themes, but
	// the random-combo widening (and, rarely, an injection) can independently land two slots on the
	// same modifier set, e.g. two "Fusillade"s (FRENZY|DEVASTATING). Re-roll any duplicate to an
	// as-yet-unused hostile theme, so Chart-a-Course is always a real choice of different sectors.
	// (Boon/signature slots never collide; their bits can't be produced by the widen.)
	for (int c = 1; c < endlessCourseCnt; ++c)
	{
		bool duplicate = false;
		for (int k = 0; k < c && !duplicate; ++k)
			if (endlessCourseMod[k] == endlessCourseMod[c])
				duplicate = true;
		if (!duplicate)
			continue;

		for (unsigned t = 0; t < COUNTOF(endlessHostileThemes); ++t)
		{
			const Uint64 m = endlessHostileThemes[idx[t]].mods;  // idx[] = the shuffled theme order
			bool used = false;
			for (int k = 0; k < endlessCourseCnt && !used; ++k)
				if (k != c && endlessCourseMod[k] == m)
					used = true;
			if (!used)
			{
				endlessCourseMod[c] = m;
				break;
			}
		}
	}

	// --- Rare whole-visit flavors: Jackpot / Gauntlet / Ambush (mutually exclusive) ----------
	// Jackpot (~1/25) all boons; Ambush (~1/20) one forced dangerous sector; Gauntlet (~1/7)
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
	const bool jackpotRoll  = ((endlessRand() % (25 + dangerRamp * 25 / 100)) == 0);  // 1/25 -> 1/50 (mid) -> ~1/112 (cap)
	const bool gauntletRoll = ((int)(endlessRand() % 100) < gauntletPct);
	const bool ambushRoll   = ((int)(endlessRand() % 100) < ambushPct);
	// A milestone zone deals its own fixed slate below, so none of the three may fire there (an
	// Ambush would collapse the visit to one course). The dice are still rolled above, so the seed
	// stream stays aligned whether or not this zone is a milestone.
	const bool doJackpot  = jackpotRoll && (endlessRunDepth > 0) && !milestone;
	const bool doAmbush   = !doJackpot && ambushRoll && (endlessRunDepth > 0) && !milestone;
	const bool doGauntlet = !doJackpot && !doAmbush && gauntletRoll && (endlessRunDepth > 0) && !milestone;

	if (doJackpot)
	{
		// Every course a pure boon. Deal DISTINCT boon themes (shuffle the table, take one per
		// course), skipping the Cursed entries -- those read as Traps, not clean boons -- so the
		// jackpot is all upside. Below the 25%-share unlock, skip the no-elite-tier boons too (they'd
		// be near-empty this shallow), so the jackpot deals only themes that actually help here.
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
	else if (doGauntlet)
	{
		// No Calm route and no boon: turn every non-hostile course (the clean course 0 and any boon
		// slot) into a fresh, distinct hostile theme. Courses already carrying a hostile bit
		// (including a rare injected Apex / Kamikaze / Overload / etc.) keep their theme, so the
		// gauntlet still fans out into varied dangers. Consumes no RNG (a deterministic pick from the
		// already-shuffled idx[] order), so the stream stays aligned.
		for (int c = 0; c < endlessCourseCnt; ++c)
		{
			if (endlessCourseMod[c] & ENDLESS_HOSTILE_MASK)
				continue;  // already a danger -- leave it be
			for (unsigned t = 0; t < COUNTOF(endlessHostileThemes); ++t)
			{
				const Uint64 m = endlessHostileThemes[idx[t]].mods;
				bool used = false;
				for (int k = 0; k < endlessCourseCnt && !used; ++k)
					if (k != c && endlessCourseMod[k] == m)
						used = true;
				if (!used)
				{
					endlessCourseMod[c] = m;
					break;
				}
			}
		}
	}

	endlessForced = doAmbush;
	if (endlessForced)
	{
		endlessCourseCnt = 1;  // collapse to a single sector (keeps course 0's level)
		static const unsigned combos[] = {
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
		endlessCourseMod[0] = combos[endlessRand() % COUNTOF(combos)];
	}

	// DIVERSE CHOICE: with a full slate (4-5 courses), make sure at least one hostile course is a SINGLE
	// negative modifier -- a clean, legible "light" option beside the busy combos, so Chart-a-Course reads
	// as a real spread of risk rather than a wall of multi-danger sectors. If none ended up a lone hostile
	// bit, thin the mildest ORDINARY course (built only from the common combinable bits) down to just its
	// least-nasty bit -- never touching a rare signature (Tar Pit / Overload / Apex / dead generator / ...)
	// or a boon / clean route. Runs before the OMNI roll + sort, so the reduced course sorts to the easy end.
	if (endlessCourseCnt >= 4)
	{
		bool haveSingle = false;
		for (int c = 1; c < endlessCourseCnt && !haveSingle; ++c)
		{
			const Uint64 h = endlessCourseMod[c] & ENDLESS_HOSTILE_MASK;
			if (h != 0 && (h & (h - 1)) == 0)   // exactly one hostile bit
				haveSingle = true;
		}
		if (!haveSingle)
		{
			Uint64 commonMask = 0;
			for (unsigned k = 0; k < COUNTOF(combinable); ++k)
				commonMask |= combinable[k];
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
			if (best >= 0)
			{
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
		}
	}

	// MILESTONE SLATE: on every 50th zone the whole chart is replaced by five S-tier sectors. A plain
	// milestone deals S+/S++ split 2-and-3, which rung gets the pair decided by the seed; a GRAND
	// (100th) one has a FIXED shape -- one END course, two S+++ and two S++. The LEVELS gathered above
	// are kept; only the mutator sets are re-dealt, so the slate is still five different sectors. Runs
	// after every ordinary generation step (nothing above can survive into it) and before the OMNI
	// roll / sort / naming, which finish it off like any other chart.
	if (milestone)
	{
		const int lowRank = (milestone == 2) ? 8 : 7;  // S++ / S+  (see endlessDangerRankLevel)
		int lowN = 2 + (int)(endlessRand() % 2);       // plain: 2 or 3 of the lower rung, the rest one higher
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
	for (int c = 0; c < endlessCourseCnt; ++c)
		if ((endlessCourseMod[c] & ENDLESS_MOD_GRAVITY) && (endlessRand() % 2))
			endlessCourseMod[c] |= ENDLESS_MOD_GRAVITY_OMNI;

	// Below the 25%-share unlock the no-elite-tier boons must not appear at all: scrub both bits from every
	// course as the final guarantee, in case any generation path above leaked one this shallow. The pick
	// sites already avoid emitting them here, so this normally does nothing.
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
	// a danger course. Swap any such redundant ELITEPACK for a comparable hostile now, after every
	// mod-adding step (themes, widen, gambits, injections, gauntlet, ambush) and before the sort, so
	// the danger ordering, tier word, name and reward all reflect the real sector. Course 0 is normally
	// clean, but an Ambush collapses its combo onto slot 0, so scan from 0. (See endlessFixRedundantElitePack.)
	for (int c = 0; c < endlessCourseCnt; ++c)
		endlessFixRedundantElitePack(c);

	// Present the courses from lowest danger to highest, so Chart-a-Course always reads as a left-
	// to-right safety ramp (the clean/boon route first, the deadliest sector last). Sort the three
	// parallel course arrays together by hostile danger score, the same metric that drives the
	// Low/Moderate/.../NIGHTMARE tier word. A stable insertion sort over this tiny list keeps equal-
	// danger courses in their generated order and consumes no RNG (it runs after every draw), so the
	// seed's structure is unchanged. Ambush already collapsed to one course above, so it's a no-op there.
	for (int i = 1; i < endlessCourseCnt; ++i)
	{
		const int      ep   = endlessCourseEp[i];
		const JE_byte  sec  = endlessCourseSec[i];
		const JE_byte  file = endlessCourseFile[i];
		const Uint64   mod  = endlessCourseMod[i];
		const int      key  = endlessDangerScore(mod);
		int j = i - 1;
		while (j >= 0 && endlessDangerScore(endlessCourseMod[j]) > key)
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

	// UNIQUE NAMES: the offered modifier sets are distinct by now, but two different un-curated
	// bitsets can still HASH to the same generated word (two "Toss-Up"s on one chart). Bump the
	// later course's name-salt until every offered label is unique. RNG-free and deterministic,
	// so a reloaded outpost re-derives the same names; only generated names ever need a salt
	// (curated names ignore it and never clash -- the theme tables hold no duplicates).
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

// Resolve a saved/chosen (episode, section) back to a real endless-safe level file. Prefer the
// exact persisted file when present; v7 and older saves only have the section, so use its first
// safe match. Returning false means the script entry has no corresponding binary level data.
static bool endlessResolveCourseFile(int ep, JE_byte sec, JE_byte requestedFile, JE_byte *resolvedFile)
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
	static char buf[64];
	if (i < 0 || i >= endlessCourseCnt)
		return "";
	if (endlessForced && i == 0)
	{
		snprintf(buf, sizeof buf, "Ambush! %s - no way around it",
		         endlessDangerTier(endlessCourseMod[0]));
		return buf;
	}
	const Uint64 mods = endlessCourseMod[i];
	// The letter rank (F easiest .. S+++ hardest) is drawn separately, on the planet monitor's
	// RANK field (endlessCourseRank + game_menu.c's overlay), so the help line is just the tier.
	if (mods == 0)
		snprintf(buf, sizeof buf, "Calm: clear skies ahead");
	else if (mods & ENDLESS_MOD_CURSED)
		snprintf(buf, sizeof buf, "Trap: rich now, barren shop next");
	else if ((mods & ENDLESS_HOSTILE_MASK) == 0)
		snprintf(buf, sizeof buf, "Boon: no danger here");
	else
		snprintf(buf, sizeof buf, "Danger: %s", endlessDangerTier(mods));
	return buf;
}

// The highlighted course's letter danger grade (F easiest .. S+++ hardest) for the monitor's
// RANK field -- moved off the help line so it reads ON the planet monitor. Delegates to the
// same endlessDangerRank/score/thresholds as the "Danger:" word, so the two never disagree.
const char *endlessCourseRank(int i)
{
	if (i < 0 || i >= endlessCourseCnt)
		return "";
	return endlessDangerRank(endlessCourseMod[i]);
}

// Numeric danger level 0 (F) .. 9 (S+++) for course i, or -1 if out of range. The monitor uses
// it to pick the letter's green->red tint; it maps the same letter endlessCourseRank returns.
int endlessCourseRankLevel(int i)
{
	if (i < 0 || i >= endlessCourseCnt)
		return -1;
	return endlessDangerRankLevel(endlessCourseMod[i]);
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

// Draw a glowing line horizontally centred on the widescreen surface (vga_width), so the
// run-end screen respects the widescreen edit instead of hugging the left.
static void endlessGlowCentered(int y, unsigned int font, const char *s)
{
	textGlowFont = font;
	JE_outTextGlow(VGAScreen, (vga_width - JE_textWidth(s, font)) / 2, y, s);
}

void endlessOnRunEnd(void)
{
	// Run-over summary, styled like the level-end tally: glowing stat lines (the same
	// JE_outTextGlow effect JE_endLevelAni uses), centred on the widescreen. The caller has
	// already faded to black, so we clear to black, fade the (untouched) game palette back
	// in, glow the lines in, wait for a key, then fade out. Returns to the title screen.
	VGAScreen = VGAScreenSeg;
	JE_clr256(VGAScreen);
	JE_showVGA();
	fade_palette(colors, 15, 0, 255);

	JE_wipeKey();
	frameCountMax = 4;
	SDL_Color white = { 255, 255, 255 };
	set_colors(white, 254, 254);

	char buf[128];
	endlessGlowCentered(22, FONT_SHAPES, "RUN OVER");

	int y = 54;

	snprintf(buf, sizeof(buf), "You fell in Zone %d", endlessRunDepth + 1);
	endlessGlowCentered(y, SMALL_FONT_SHAPES, buf);  y += 18;

	snprintf(buf, sizeof(buf), "Zones cleared:   %d", endlessRunDepth);
	endlessGlowCentered(y, SMALL_FONT_SHAPES, buf);  y += 18;

	snprintf(buf, sizeof(buf), "Enemies destroyed:   %d", endlessRunKills);
	endlessGlowCentered(y, SMALL_FONT_SHAPES, buf);  y += 18;

	snprintf(buf, sizeof(buf), "Bosses slain:   %d", endlessRunBossKills);
	endlessGlowCentered(y, SMALL_FONT_SHAPES, buf);  y += 18;

	snprintf(buf, sizeof(buf), "Cash amassed:   $%lu", (unsigned long)player[0].cash);
	endlessGlowCentered(y, SMALL_FONT_SHAPES, buf);  y += 18;

	if (endlessArmorBonus > 0)
	{
		snprintf(buf, sizeof(buf), "Hull reinforced:   +%d", endlessArmorBonus);
		endlessGlowCentered(y, SMALL_FONT_SHAPES, buf);  y += 18;
	}

	endlessGlowCentered(y + 10, SMALL_FONT_SHAPES, endlessMilestoneLine(endlessRunDepth + 1));

	// Require inputs released first (the player may have died mid-fire), then wait for a
	// fresh key/button so the summary can't flash past.
	wait_noinput(true, true, true);
	do
	{
		setDelay(1);
		wait_delay();
	} while (!JE_anyButton());

	wait_noinput(false, false, true);
	fade_black(15);
	JE_clr256(VGAScreen);
}

void endlessEndRunToTitle(void)
{
	// Voluntarily quitting an in-progress run back to the title. In HARDCORE this is as final as
	// dying -- there's no save to resume -- so it gets the same Run Over summary the death path shows
	// (the shop has already faded to black on Quit, so endlessOnRunEnd fades its summary in cleanly).
	// In non-hardcore a quit stays silent: the run may have a save to come back to, so it isn't over.
	if (endlessHardcore)
	{
		fade_song();       // silence the shop track so the summary plays clean, like the death path
		endlessOnRunEnd();
	}
	endlessMode = false;
}

// --- Save / resume (endless.sav sidecar) ---------------------------------------------------
// tyrian.sav can't be extended (fixed checksummed layout), so the run lives in an endless.sav sidecar keyed
// by the same slot; restoring the snapshot rather than regenerating stops reload rerolling the shop. notes.md §Save / resume.

#define ENDLESS_SAVE_FILE    "endless.sav"
#define ENDLESS_SAVE_VERSION 10     // v1 run-state only; v2 outpost snapshot; v3 seed; v4 locked sortie; v5 buff recharge; v6 recent-level ring; v7 64-bit mods; v8 exact course files; v9 credits-shown flag; v10 last zone's song
#define ENDLESS_SAVE_PERKS   16     // fixed perk-array width on disk; now == PERK_COUNT (headroom used up).
                                    // Adding a 17th perk means bumping this (and the save version) or it won't persist.

typedef struct {
	bool used;

	// --- run-persistent ---
	Sint32 runDepth, armorBonus, runKills, runBossKills;
	Sint32 buffCharge, revivesUsed, shopTax, longCon, perkDepthDone, superbombs;
	Uint8  reviveHeld, gambleRigged;
	Uint8  perkOwned[ENDLESS_SAVE_PERKS];

	// --- outpost snapshot: prices + pending buys ---
	Sint32 rerollCost, hullCost, bombCost, extraPerkCost, cleanseCost, shopEntryCash;
	Uint32 purchasedMods;
	Sint32 buffKind, cleanseCharges;
	Uint8  gamblePerkWon, perkPending;
	char   gambleMsg[48];
	char   lastSpecialName[31];

	// --- outpost snapshot: this visit's perk offer ---
	Sint32 perkChoiceN;
	Sint32 perkChoice[3];

	// --- outpost snapshot: this visit's courses ---
	Sint32 courseCnt;
	Sint32 courseEp[ENDLESS_MAX_COURSES];
	Uint8  courseSec[ENDLESS_MAX_COURSES];
	Uint8  courseFile[ENDLESS_MAX_COURSES];  // v8: exact binary level file for each saved course
	Uint64 courseMod[ENDLESS_MAX_COURSES];   // v7: was Uint32 (read narrow from v3-v6 files)
	Sint32 lastEp;
	Uint8  lastSec, forced;

	// --- outpost snapshot: this visit's shop stock ---
	Uint8  itemAvail[9][10];
	Uint8  itemAvailMax[9];

	// --- run seed (v3) ---
	char   seed[ENDLESS_SEED_MAXLEN];

	// --- locked sortie (v4): a "gave up the level" outpost, locked to the launch-time choices ---
	Uint8  lockedSortie;  // 1 = this save reopens the locked retry outpost (else a normal outpost)
	Uint64 sortieMods;    // endlessActiveMods of the committed level (v7: was Uint32)
	Uint8  sortieSec;     // committed level section
	Sint32 sortieEp;      // committed episode
	Uint8  sortieFile;    // committed lvl file number

	// --- kill-fire buff recharge (v5) ---
	Sint32 buffCooldownUntil;  // run depth at which the E-Shop kill-fire buys unlock again (0 = no lock)

	// --- anti-repeat recent-level ring (v6): the last few played (ep, sec), [0] = newest ---
	Uint8  recentCount;
	Sint32 recentEp[ENDLESS_LEVEL_HISTORY];
	Uint8  recentSec[ENDLESS_LEVEL_HISTORY];

	// --- zone-100 credits (v9) ---
	Uint8  creditsShown;  // 1 = this run has already rolled the credits, so resuming won't replay them

	// --- per-zone music continuity (v10) ---
	Uint8  lastSong;       // the track the last-played zone really used (0 = none yet)
	Sint32 lastSongDepth;  // that zone's run depth (only meaningful when lastSong != 0)
} EndlessSlotRec;

// One record per save slot, mirrored to endless.sav. Read-modify-write on each save keeps the
// other slots' records intact.
static EndlessSlotRec endlessSlotCache[SAVE_FILES_NUM];

// Restore a chart from disk while migrating v7-and-older records that did not persist courseFile.
// Invalid legacy entries (notably Episode 1 section 44 / nonexistent file 20) are dropped and the
// remaining parallel arrays are compacted. If nothing usable remains, regenerate deterministically
// for this depth so the outpost always has a launchable course.
static void endlessRestoreSavedCourses(const EndlessSlotRec *r)
{
	int savedCount = r->courseCnt;
	if (savedCount < 0) savedCount = 0;
	if (savedCount > ENDLESS_MAX_COURSES) savedCount = ENDLESS_MAX_COURSES;

	memset(endlessCourseEp, 0, sizeof(endlessCourseEp));
	memset(endlessCourseSec, 0, sizeof(endlessCourseSec));
	memset(endlessCourseFile, 0, sizeof(endlessCourseFile));
	memset(endlessCourseMod, 0, sizeof(endlessCourseMod));

	int restoredCount = 0;
	bool dropped = false;
	for (int i = 0; i < savedCount; ++i)
	{
		JE_byte file;
		if (!endlessResolveCourseFile(r->courseEp[i], r->courseSec[i], r->courseFile[i], &file))
		{
			fprintf(stderr, "warning: dropping invalid saved course episode %d section %u\n",
			        r->courseEp[i], (unsigned int)r->courseSec[i]);
			dropped = true;
			continue;
		}
		endlessCourseEp[restoredCount] = r->courseEp[i];
		endlessCourseSec[restoredCount] = r->courseSec[i];
		endlessCourseFile[restoredCount] = file;
		endlessCourseMod[restoredCount] = r->courseMod[i];
		++restoredCount;
	}
	endlessCourseCnt = restoredCount;

	// A forced visit represents one specific unavoidable course; if that entry was invalid, rebuild
	// the whole visit rather than silently turning a different saved option into an Ambush.
	if (endlessCourseCnt == 0 || (endlessForced && dropped))
	{
		endlessReseed((Uint64)endlessRunDepth * 2);
		endlessGenerateCourses();
	}
}

// Little-endian field I/O over a FILE*. The write side is fire-and-forget; the read side never
// dies -- any short/failed read just aborts the load, so a missing or corrupt sidecar simply
// means "no endless save".
static void endlessPutU8(FILE *f, unsigned v)                 { Uint8 b = (Uint8)v; fwrite(&b, 1, 1, f); }
static void endlessPutU32(FILE *f, Uint32 v)                  { v = SDL_SwapLE32(v); fwrite(&v, 4, 1, f); }
static void endlessPutU64(FILE *f, Uint64 v)                  { v = SDL_SwapLE64(v); fwrite(&v, 8, 1, f); }
static void endlessPutBytes(FILE *f, const void *p, size_t n) { fwrite(p, 1, n, f); }
static bool endlessGetU8(FILE *f, Uint8 *v)                   { return fread(v, 1, 1, f) == 1; }
static bool endlessGetU32(FILE *f, Uint32 *v)                 { Uint32 b; if (fread(&b, 4, 1, f) != 1) return false; *v = SDL_SwapLE32(b); return true; }
static bool endlessGetU64(FILE *f, Uint64 *v)                 { Uint64 b; if (fread(&b, 8, 1, f) != 1) return false; *v = SDL_SwapLE64(b); return true; }
static bool endlessGetBytes(FILE *f, void *p, size_t n)       { return fread(p, 1, n, f) == n; }

static void endlessWriteRec(FILE *f, const EndlessSlotRec *r)
{
	endlessPutU8(f, r->used ? 1 : 0);

	const Sint32 s32[] = {
		r->runDepth, r->armorBonus, r->runKills, r->runBossKills, r->buffCharge, r->revivesUsed,
		r->shopTax, r->longCon, r->perkDepthDone, r->superbombs,
		r->rerollCost, r->hullCost, r->bombCost, r->extraPerkCost, r->cleanseCost, r->shopEntryCash,
		r->buffKind, r->cleanseCharges, r->perkChoiceN, r->courseCnt, r->lastEp,
	};
	for (unsigned i = 0; i < COUNTOF(s32); ++i)
		endlessPutU32(f, (Uint32)s32[i]);

	endlessPutU32(f, r->purchasedMods);
	endlessPutU8(f, r->reviveHeld);
	endlessPutU8(f, r->gambleRigged);
	endlessPutU8(f, r->gamblePerkWon);
	endlessPutU8(f, r->perkPending);
	endlessPutU8(f, r->lastSec);
	endlessPutU8(f, r->forced);

	endlessPutBytes(f, r->perkOwned, ENDLESS_SAVE_PERKS);
	endlessPutBytes(f, r->gambleMsg, sizeof(r->gambleMsg));
	endlessPutBytes(f, r->lastSpecialName, sizeof(r->lastSpecialName));

	for (unsigned i = 0; i < COUNTOF(r->perkChoice); ++i)
		endlessPutU32(f, (Uint32)r->perkChoice[i]);
	for (unsigned i = 0; i < ENDLESS_MAX_COURSES; ++i)
		endlessPutU32(f, (Uint32)r->courseEp[i]);
	for (unsigned i = 0; i < ENDLESS_MAX_COURSES; ++i)
		endlessPutU64(f, r->courseMod[i]);
	endlessPutBytes(f, r->courseSec, ENDLESS_MAX_COURSES);
	endlessPutBytes(f, r->courseFile, ENDLESS_MAX_COURSES);

	endlessPutBytes(f, r->itemAvail, sizeof(r->itemAvail));
	endlessPutBytes(f, r->itemAvailMax, sizeof(r->itemAvailMax));
	endlessPutBytes(f, r->seed, sizeof(r->seed));

	endlessPutU8(f, r->lockedSortie);        // v4 locked-sortie block
	endlessPutU64(f, r->sortieMods);         // v7: 64-bit (was U32 in v4-v6)
	endlessPutU8(f, r->sortieSec);
	endlessPutU32(f, (Uint32)r->sortieEp);
	endlessPutU8(f, r->sortieFile);

	endlessPutU32(f, (Uint32)r->buffCooldownUntil);  // v5 kill-fire recharge

	endlessPutU8(f, r->recentCount);                 // v6 anti-repeat recent-level ring
	for (unsigned i = 0; i < ENDLESS_LEVEL_HISTORY; ++i)
		endlessPutU32(f, (Uint32)r->recentEp[i]);
	endlessPutBytes(f, r->recentSec, ENDLESS_LEVEL_HISTORY);

	endlessPutU8(f, r->creditsShown);                // v9 zone-100 credits

	endlessPutU8(f, r->lastSong);                    // v10 per-zone music continuity
	endlessPutU32(f, (Uint32)r->lastSongDepth);
}

static bool endlessReadRec(FILE *f, EndlessSlotRec *r, int version)
{
	memset(r, 0, sizeof(*r));

	Uint8 used;
	if (!endlessGetU8(f, &used))
		return false;
	r->used = used != 0;

	Sint32 *const s32[] = {
		&r->runDepth, &r->armorBonus, &r->runKills, &r->runBossKills, &r->buffCharge, &r->revivesUsed,
		&r->shopTax, &r->longCon, &r->perkDepthDone, &r->superbombs,
		&r->rerollCost, &r->hullCost, &r->bombCost, &r->extraPerkCost, &r->cleanseCost, &r->shopEntryCash,
		&r->buffKind, &r->cleanseCharges, &r->perkChoiceN, &r->courseCnt, &r->lastEp,
	};
	for (unsigned i = 0; i < COUNTOF(s32); ++i)
	{
		Uint32 t;
		if (!endlessGetU32(f, &t))
			return false;
		*s32[i] = (Sint32)t;
	}

	if (!endlessGetU32(f, &r->purchasedMods)
	    || !endlessGetU8(f, &r->reviveHeld) || !endlessGetU8(f, &r->gambleRigged)
	    || !endlessGetU8(f, &r->gamblePerkWon) || !endlessGetU8(f, &r->perkPending)
	    || !endlessGetU8(f, &r->lastSec) || !endlessGetU8(f, &r->forced))
		return false;

	if (!endlessGetBytes(f, r->perkOwned, ENDLESS_SAVE_PERKS)
	    || !endlessGetBytes(f, r->gambleMsg, sizeof(r->gambleMsg))
	    || !endlessGetBytes(f, r->lastSpecialName, sizeof(r->lastSpecialName)))
		return false;

	for (unsigned i = 0; i < COUNTOF(r->perkChoice); ++i)
	{
		Uint32 t;
		if (!endlessGetU32(f, &t))
			return false;
		r->perkChoice[i] = (Sint32)t;
	}
	for (unsigned i = 0; i < ENDLESS_MAX_COURSES; ++i)
	{
		Uint32 t;
		if (!endlessGetU32(f, &t))
			return false;
		r->courseEp[i] = (Sint32)t;
	}
	for (unsigned i = 0; i < ENDLESS_MAX_COURSES; ++i)
	{
		if (version >= 7)
		{
			if (!endlessGetU64(f, &r->courseMod[i]))
				return false;
		}
		else
		{
			Uint32 t;   // v3-v6 stored the course mods 32-bit (high bits were unused back then)
			if (!endlessGetU32(f, &t))
				return false;
			r->courseMod[i] = t;
		}
	}
	if (!endlessGetBytes(f, r->courseSec, ENDLESS_MAX_COURSES))
		return false;
	if (version >= 8 && !endlessGetBytes(f, r->courseFile, ENDLESS_MAX_COURSES))
		return false;

	if (!endlessGetBytes(f, r->itemAvail, sizeof(r->itemAvail))
	    || !endlessGetBytes(f, r->itemAvailMax, sizeof(r->itemAvailMax))
	    || !endlessGetBytes(f, r->seed, sizeof(r->seed)))
		return false;

	// Never trust a terminator off disk.
	r->gambleMsg[sizeof(r->gambleMsg) - 1] = '\0';
	r->lastSpecialName[sizeof(r->lastSpecialName) - 1] = '\0';
	r->seed[sizeof(r->seed) - 1] = '\0';

	// v4 locked-sortie block. Older (v3) records don't carry it -- the memset above already left
	// lockedSortie = 0, so they simply read as "not a locked outpost".
	if (version >= 4)
	{
		Uint8  u8;
		Uint32 u32;
		if (!endlessGetU8(f, &u8))
			return false;
		r->lockedSortie = u8;
		if (version >= 7)   // v7 widened sortieMods to 64-bit; v4-v6 stored it 32-bit
		{
			Uint64 u64;
			if (!endlessGetU64(f, &u64))
				return false;
			r->sortieMods = u64;
		}
		else
		{
			if (!endlessGetU32(f, &u32))
				return false;
			r->sortieMods = u32;
		}
		if (!endlessGetU8(f, &u8))
			return false;
		r->sortieSec = u8;
		if (!endlessGetU32(f, &u32))
			return false;
		r->sortieEp = (Sint32)u32;
		if (!endlessGetU8(f, &u8))
			return false;
		r->sortieFile = u8;
	}

	// v5 kill-fire buff recharge. Older (v3/v4) records lack it -- the memset above left
	// buffCooldownUntil = 0 ("no lock"), so a resumed pre-v5 run can buy immediately.
	if (version >= 5)
	{
		Uint32 u32;
		if (!endlessGetU32(f, &u32))
			return false;
		r->buffCooldownUntil = (Sint32)u32;
	}

	// v6 anti-repeat recent-level ring. Older records lack it -- the memset above left recentCount = 0,
	// so a resumed pre-v6 run just starts with an empty window (it refills as zones are played).
	if (version >= 6)
	{
		if (!endlessGetU8(f, &r->recentCount))
			return false;
		for (unsigned i = 0; i < ENDLESS_LEVEL_HISTORY; ++i)
		{
			Uint32 u32;
			if (!endlessGetU32(f, &u32))
				return false;
			r->recentEp[i] = (Sint32)u32;
		}
		if (!endlessGetBytes(f, r->recentSec, ENDLESS_LEVEL_HISTORY))
			return false;
		if (r->recentCount > ENDLESS_LEVEL_HISTORY)
			r->recentCount = ENDLESS_LEVEL_HISTORY;
	}

	// v9 zone-100 credits. Older records lack it -- the memset above left creditsShown = 0, so a
	// pre-v9 run already past zone 100 gets one (harmless) showing at its next outpost.
	if (version >= 9 && !endlessGetU8(f, &r->creditsShown))
		return false;

	// v10 per-zone music continuity. Older records lack it -- lastSong stays 0 ("nothing remembered"),
	// and the picker falls back to deriving the previous zone's song approximately.
	if (version >= 10)
	{
		Uint32 u32;
		if (!endlessGetU8(f, &r->lastSong) || !endlessGetU32(f, &u32))
			return false;
		r->lastSongDepth = (Sint32)u32;
	}
	return true;
}

// Load every slot's record into the cache (all-unused on a missing / short / corrupt / wrong-
// version file -- this is optional data, so any problem just means "no endless save").
static void endlessReadAllSlots(void)
{
	memset(endlessSlotCache, 0, sizeof(endlessSlotCache));

	FILE *f = dir_fopen(get_user_directory(), ENDLESS_SAVE_FILE, "rb");
	if (f == NULL)
		return;

	Uint8 hdr[6];
	if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)
	    || memcmp(hdr, "OTES", 4) != 0 || hdr[4] < 3 || hdr[4] > ENDLESS_SAVE_VERSION)
	{
		fclose(f);  // accept v3 (pre-locked-sortie), v4 and v5; anything else is "no endless save"
		return;
	}

	const int count = hdr[5];
	for (int s = 0; s < count; ++s)
	{
		EndlessSlotRec rec;
		if (!endlessReadRec(f, &rec, hdr[4]))
			break;  // truncated: keep the full records already read
		if (s < SAVE_FILES_NUM)
			endlessSlotCache[s] = rec;
	}

	fclose(f);
}

// Write the whole cache back to disk (fixed record layout, so a slot is simply overwritten).
static void endlessWriteAllSlots(void)
{
	FILE *f = dir_fopen_warn(get_user_directory(), ENDLESS_SAVE_FILE, "wb");
	if (f == NULL)
		return;

	const Uint8 hdr[6] = { 'O', 'T', 'E', 'S', ENDLESS_SAVE_VERSION, (Uint8)SAVE_FILES_NUM };
	fwrite(hdr, 1, sizeof(hdr), f);
	for (int s = 0; s < SAVE_FILES_NUM; ++s)
		endlessWriteRec(f, &endlessSlotCache[s]);

	fclose(f);
}

// Snapshot the live run AND the current outpost into a record.
static void endlessCaptureCurrent(EndlessSlotRec *r)
{
	memset(r, 0, sizeof(*r));
	r->used = true;

	r->runDepth      = endlessRunDepth;
	r->armorBonus    = endlessArmorBonus;
	r->runKills      = endlessRunKills;
	r->runBossKills  = endlessRunBossKills;
	r->buffCharge    = endlessBuffCharge;
	r->buffCooldownUntil = endlessBuffCooldownUntil;
	r->revivesUsed   = endlessRevivesUsed;
	r->shopTax       = endlessShopTax;
	r->longCon       = endlessLongCon;
	r->perkDepthDone = endlessPerkDepthDone;
	r->superbombs    = player[0].superbombs;
	r->reviveHeld    = endlessReviveHeld ? 1 : 0;
	r->gambleRigged  = endlessGambleRigged ? 1 : 0;
	for (int i = 0; i < ENDLESS_SAVE_PERKS; ++i)
		r->perkOwned[i] = (i < PERK_COUNT) ? endlessPerkOwned[i] : 0;

	r->rerollCost    = (Sint32)endlessRerollCost;
	r->hullCost      = endlessHullCost;
	r->bombCost      = (Sint32)endlessBombCost;
	r->extraPerkCost = (Sint32)endlessExtraPerkCost;
	r->cleanseCost   = (Sint32)endlessCleanseCost;
	r->shopEntryCash = (Sint32)endlessShopEntryCash;
	r->purchasedMods = endlessPurchasedMods;
	r->buffKind      = endlessBuffKind;
	r->cleanseCharges= endlessCleanseChargeCount;
	r->gamblePerkWon = endlessGamblePerkWon ? 1 : 0;
	r->perkPending   = endlessPerkPending ? 1 : 0;
	SDL_strlcpy(r->gambleMsg, endlessGambleMsg, sizeof(r->gambleMsg));
	SDL_strlcpy(r->lastSpecialName, endlessLastSpecialName, sizeof(r->lastSpecialName));

	r->perkChoiceN = endlessPerkChoiceN;
	for (int i = 0; i < 3; ++i)
		r->perkChoice[i] = endlessPerkChoice[i];

	r->courseCnt = endlessCourseCnt;
	for (int i = 0; i < ENDLESS_MAX_COURSES; ++i)
	{
		r->courseEp[i]  = endlessCourseEp[i];
		r->courseSec[i] = endlessCourseSec[i];
		r->courseFile[i] = endlessCourseFile[i];
		r->courseMod[i] = endlessCourseMod[i];
	}
	r->lastEp  = endlessLastEp;
	r->lastSec = endlessLastSec;
	r->forced  = endlessForced ? 1 : 0;

	memcpy(r->itemAvail, itemAvail, sizeof(r->itemAvail));
	memcpy(r->itemAvailMax, itemAvailMax, sizeof(r->itemAvailMax));

	SDL_strlcpy(r->seed, endlessRunSeed, sizeof(r->seed));

	// Locked "gave up the level" outpost (v4): only meaningful when saving FROM the locked shop
	// (endlessLockedSortie). memset(r,0) at the top leaves these cleared for a normal save.
	r->lockedSortie = endlessLockedSortie ? 1 : 0;
	r->sortieMods   = endlessSortieModsV;
	r->sortieSec    = endlessSortieSec;
	r->sortieEp     = endlessSortieEp;
	r->sortieFile   = endlessSortieFile;

	// Anti-repeat recent-level ring (v6).
	r->recentCount = (Uint8)endlessRecentCount;
	for (int i = 0; i < ENDLESS_LEVEL_HISTORY; ++i)
	{
		r->recentEp[i]  = endlessRecentEp[i];
		r->recentSec[i] = endlessRecentSec[i];
	}

	r->creditsShown = endlessCreditsShown ? 1 : 0;  // v9 zone-100 credits

	r->lastSong      = endlessLastSong;             // v10 per-zone music continuity
	r->lastSongDepth = endlessLastSongDepth;
}

// Lay a saved record back over the live state. endlessResetRun first, so per-zone/per-visit
// transients we DON'T persist (combat timers, elite rolls, ...) start clean; then restore both
// the run and the outpost snapshot, and arm endlessResumeVisit so the next outpost is the
// SAVED one rather than a fresh (free) reroll.
static void endlessApplyCurrent(const EndlessSlotRec *r)
{
	endlessResetRun();

	endlessRunDepth      = r->runDepth;
	endlessArmorBonus    = r->armorBonus;
	endlessRunKills      = r->runKills;
	endlessRunBossKills  = r->runBossKills;
	endlessBuffCharge    = r->buffCharge;
	endlessBuffCooldownUntil = r->buffCooldownUntil;
	endlessRevivesUsed   = r->revivesUsed;
	endlessShopTax       = r->shopTax;
	endlessLongCon       = r->longCon;
	endlessPerkDepthDone = r->perkDepthDone;
	player[0].superbombs = (r->superbombs < 0) ? 0 : (r->superbombs > 10 ? 10 : r->superbombs);
	endlessReviveHeld    = r->reviveHeld != 0;
	endlessGambleRigged  = r->gambleRigged != 0;
	for (int i = 0; i < PERK_COUNT && i < ENDLESS_SAVE_PERKS; ++i)
	{
		int v = r->perkOwned[i];
		const int maxs = endlessPerkTable[i].maxStack;
		endlessPerkOwned[i] = (JE_byte)(v < 0 ? 0 : (v > maxs ? maxs : v));
	}

	endlessRerollCost         = r->rerollCost;
	endlessHullCost           = r->hullCost;
	endlessBombCost           = r->bombCost;
	endlessExtraPerkCost      = r->extraPerkCost;
	endlessCleanseCost        = r->cleanseCost;
	endlessShopEntryCash      = r->shopEntryCash;
	endlessPurchasedMods      = r->purchasedMods;
	endlessBuffKind           = r->buffKind;
	endlessCleanseChargeCount = r->cleanseCharges;
	endlessGamblePerkWon      = r->gamblePerkWon != 0;
	endlessPerkPending        = r->perkPending != 0;
	SDL_strlcpy(endlessGambleMsg, r->gambleMsg, sizeof(endlessGambleMsg));
	SDL_strlcpy(endlessLastSpecialName, r->lastSpecialName, sizeof(endlessLastSpecialName));
	endlessSetSeed(r->seed);  // restore the run seed (endlessResetRun blanked it); rehashes + primes the stream

	endlessPerkChoiceN = r->perkChoiceN;
	if (endlessPerkChoiceN < 0) endlessPerkChoiceN = 0;
	if (endlessPerkChoiceN > 3) endlessPerkChoiceN = 3;
	for (int i = 0; i < 3; ++i)
		endlessPerkChoice[i] = r->perkChoice[i];

	endlessLastEp  = r->lastEp;
	endlessLastSec = r->lastSec;
	endlessForced  = r->forced != 0;

	// Anti-repeat recent-level ring (v6). endlessResetRun (above) already cleared it, so a pre-v6
	// record (recentCount 0) simply resumes with an empty window.
	endlessRecentCount = (r->recentCount > ENDLESS_LEVEL_HISTORY) ? ENDLESS_LEVEL_HISTORY : r->recentCount;
	for (int i = 0; i < ENDLESS_LEVEL_HISTORY; ++i)
	{
		endlessRecentEp[i]  = r->recentEp[i];
		endlessRecentSec[i] = r->recentSec[i];
	}

	// Zone-100 credits (v9): endlessResetRun above cleared the flag, so a pre-v9 record simply reads
	// as "not shown yet".
	endlessCreditsShown = r->creditsShown != 0;

	// Per-zone music continuity (v10). A pre-v10 record has lastSong 0; force the depth back to "none"
	// with it, since a zeroed record would otherwise read as a real entry for depth 0.
	endlessLastSong      = r->lastSong;
	endlessLastSongDepth = (r->lastSong != 0) ? r->lastSongDepth : -1;

	endlessRestoreSavedCourses(r);

	memcpy(itemAvail, r->itemAvail, sizeof(itemAvail));
	memcpy(itemAvailMax, r->itemAvailMax, sizeof(itemAvailMax));

	// Locked-sortie retry (v4): a save made from the "gave up the level" outpost reopens locked and
	// relaunches the same committed level. endlessResetRun (above) already cleared these to unlocked.
	endlessLockedSortie = r->lockedSortie != 0;
	if (endlessLockedSortie)
	{
		endlessSortieModsV = r->sortieMods;
		endlessSortieSec   = (JE_byte)r->sortieSec;
		endlessSortieEp    = r->sortieEp;
		endlessSortieFile  = (JE_byte)r->sortieFile;
		endlessSortieHave  = true;
	}

	endlessResumeVisit = true;  // next outpost: restore this snapshot, do not reroll
}

void endlessSaveSlot(JE_byte slot)
{
	if (slot < 1 || slot > SAVE_FILES_NUM)
		return;

	endlessReadAllSlots();
	if (endlessMode)
		endlessCaptureCurrent(&endlessSlotCache[slot - 1]);
	else if (endlessSlotCache[slot - 1].used)
		endlessSlotCache[slot - 1].used = false;  // a normal save over an endless slot drops its stale record
	else
		return;  // campaign save over a non-endless slot: nothing to store or clear
	endlessWriteAllSlots();
}

bool endlessLoadSlot(JE_byte slot)
{
	if (slot < 1 || slot > SAVE_FILES_NUM)
		return false;

	endlessReadAllSlots();
	if (!endlessSlotCache[slot - 1].used)
		return false;

	endlessApplyCurrent(&endlessSlotCache[slot - 1]);
	endlessMode = true;  // JE_loadGame cleared it for a normal load; this slot is an endless run
	return true;
}

// True from the moment a run is restored until its outpost reopens and consumes the snapshot.
// The title-load path runs the outpost at JE_main's entry (flag already cleared by the time a
// level starts); an in-shop load can't, so JE_main checks this after JE_loadMap and detours to
// the outpost when it's still set (see tyrian2.c). Mirrors the endlessBetweenLevels gate.
bool endlessResumePending(void) { return endlessResumeVisit; }

// --- "Quit Level" -> locked-outpost retry ----------------------------------------------------
// The endless run/outpost half of the launch-time snapshot (the loadout + committed-level half are
// the endlessSortie* primitives up top). Reusing the save record means depth, perks, prices,
// purchased mods, courses, shop stock and seed are all reverted by the tested capture/apply code.
static EndlessSlotRec endlessSortieRec;

void endlessCaptureSortie(void)
{
	if (!endlessMode)
		return;

	// We're about to run a level, so by definition we're no longer sitting in a locked "gave up"
	// outpost. Clear it before the capture so the snapshot itself reads as unlocked.
	endlessLockedSortie = false;

	endlessCaptureCurrent(&endlessSortieRec);                          // endless run + outpost state
	memcpy(endlessSortiePlayer, player, sizeof(endlessSortiePlayer));  // full loadout (cash / items / superbombs)
	endlessSortieModsV = endlessActiveMods;   // the committed level's mutators...
	endlessSortieSec   = mainLevel;           // ...its section (== the level being loaded)...
	endlessSortieEp    = episodeNum;          // ...its episode...
	endlessSortieFile  = lvlFileNum;          // ...and its level file
	endlessSortieHave  = true;
}

void endlessRestoreSortie(void)
{
	if (!endlessSortieHave)
		return;

	// Grab run-scoped state before endlessApplyCurrent -> endlessResetRun clobbers it. A bail is a
	// retry of the same run, so its hardcore mode must survive the reset (the reset is meant for the
	// save/resume path, which starts a fresh non-hardcore run) -- otherwise the very first bail would
	// silently un-lock the outpost AND re-enable saving for the rest of the run.
	const bool     wasHardcore = endlessHardcore;
	const unsigned preBuff     = endlessSortiePrePurchased;
	const int      preCleanse  = endlessSortiePreCleanse;
	const int      preLongCon  = endlessSortiePreLongCon;

	endlessApplyCurrent(&endlessSortieRec);                           // revert endless state; arms endlessResumeVisit (also cleared endlessSortieHave via endlessResetRun)
	memcpy(player, endlessSortiePlayer, sizeof(endlessSortiePlayer)); // revert loadout (wins over the superbombs field applyCurrent touched)
	endlessActiveMods   = endlessSortieModsV;                         // the committed level's mutators (for the relaunch)
	endlessSortieHave   = true;                                       // the committed-level statics are still valid -- keep the invariant
	endlessHardcore     = wasHardcore;                                // keep the run's mode across the reset

	if (endlessHardcore)
	{
		// Hardcore: the reopened outpost is LOCKED to the launch-time choices. The relaunch re-arms
		// the committed level directly (endlessArmLockedRelaunch), so the post-pick snapshot's zeroed
		// one-shots are correct -- leave them as endlessApplyCurrent restored them (no double-spend).
		endlessLockedSortie = true;
	}
	else
	{
		// Non-hardcore: the outpost reopens UNLOCKED and the player re-picks a course through
		// endlessSelectCourse, which re-consumes these one-shots. Restore their PRE-pick values so a
		// bought buff / queued sabotage / Long Con carry to the next course instead of being lost.
		endlessLockedSortie       = false;
		endlessPurchasedMods      = preBuff;
		endlessCleanseChargeCount = preCleanse;
		endlessLongCon            = preLongCon;
	}
}

bool endlessSortieValid(void) { return endlessSortieHave; }

void endlessArmLockedRelaunch(void)
{
	// Re-arm the same level directly -- not via endlessSelectCourse, whose one-shot consumption
	// (Long Con decrement, Sabotage/cleanse charges, purchased-mod fold-in) must not fire twice.
	if (endlessSortieEp != episodeNum)
		JE_initEpisode((JE_byte)endlessSortieEp);  // may reset mainLevel/lvlFileNum -- so set them after
	endlessActiveMods = endlessSortieModsV;
	mainLevel = endlessSortieSec;
	if (endlessSortieFile != 0)
		lvlFileNum = endlessSortieFile;
	forcedLvlFileNum = endlessSortieFile;  // keep JE_loadMap's rescan from reverting to the section's first ']L'
	nextLevel = mainLevel;
	jumpSection = true;  // exits the shop loop; JE_loadMap then loads the committed level
}
