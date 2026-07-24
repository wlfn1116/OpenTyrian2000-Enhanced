/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 *
 * Endless mode: the outpost -- shop stock, prices, the E-Shop buys and the gamble.
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
#include "keyboard.h"      // newkey/lastkey_scan/keysactive, service_SDL_events
#include "lvlmast.h"       // shapeFile[]
#include "mainint.h"       // JE_getCost
#include "musmast.h"       // songBuy, DEFAULT_SONG_BUY (shop / buy-sell music)
#include "palette.h"       // colors, fade_palette, fade_black
#include "player.h"        // player[]
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

// Escalating outpost prices, reset each visit.
long endlessRerollCost = 0;
int  endlessHullCost   = 0;

// E-Shop kill-fire buff: bought once per shop visit, its modifier bits are stashed here and
// OR'd into the next sector's mods at course selection (see endlessSelectCourse), so they
// survive the course choice that would otherwise overwrite endlessActiveMods.
unsigned endlessPurchasedMods = 0;
// Which kill-fire buff was bought this visit (mutually exclusive): 0 none, 1 Turbodrive,
// 2 Overdrive.
int endlessBuffKind = 0;
// Overdrive stacks: +1 per kill while the buff window is up (capped), each worth +5% fire
// and +2.5% damage. Reset to 0 the moment the kill-fire window lapses (see endlessGameplayTick).
int endlessOverdriveStacks = 0;

// Kill-fire buff recharge: the run depth at which the three E-Shop kill-fire buys unlock again
// after a purchase (0 = no lock). Run-persistent and saved (v5), so a mid-cooldown save/resume
// can't wipe it. The lock length scales with depth; see endlessBuffCooldownLength.
int endlessBuffCooldownUntil = 0;

// --- Expanded E-Shop state -----------------------------------------------------------------------
// Buff "charge" scales the kill-fire window/damage with the cash paid (normalized by depth,
// cap 20). Reset each run.
int endlessBuffCharge = 0;
int endlessBuffWindowTicks(void)  // base kill-fire window, extended by charge (up to ~2.5x)
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
bool endlessReviveHeld = false;
int  endlessRevivesUsed = 0;
int  endlessCleanseChargeCount = 0;
long endlessBombCost = 0, endlessExtraPerkCost = 0, endlessCleanseCost = 0;
char endlessGambleMsg[48] = "";
bool endlessGamblePerkWon = false;  // a gamble handed out a free perk pick; the E-Shop dispatch opens MENU_PERKS
int  endlessShopTax = 0;            // Loan Shark: permanent +% on every shop price for the rest of the run
bool endlessGambleRigged = false;   // Rigged: the NEXT gamble secretly rolls twice and keeps the worse result
int  endlessLongCon = 0;            // The Long Con: sectors until a paid-and-forgotten APEX ambush comes due (0 = none)
bool endlessResumeVisit   = false;  // a save was just loaded: the next outpost restores its snapshot instead of rerolling (consumed by endlessBetweenLevels)
bool endlessCreditsShown  = false;  // the zone-100 credits roll has already played this run; rides the save so reloading the zone-101 outpost doesn't replay it
#define ENDLESS_CREDITS_ZONE 100           // zones cleared before the run rolls the credits (once per run, at the outpost that follows)

// Cash the player had on entering the shop this visit. The E-Shop cash-fraction buys (buff /
// Buy Special) price off this snapshot, not live cash, so their cost stays fixed for the whole
// visit (buying one doesn't cheapen the next). Captured in endlessResetShopPrices.
long endlessShopEntryCash = 0;

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
// and to SHOW the payout on the Chart-a-Course monitor (endlessCoursePayout) -- so the two
// can't disagree.
long endlessClearBonusForEx(Uint64 mods, int payoutMille)
{
	const long base = endlessClearBase();
	long tenths = 0;
	for (unsigned i = 0; i < COUNTOF(endlessModTable); ++i)
		if (mods & endlessModTable[i].bit)
			tenths += endlessModTable[i].reward;
	tenths += endlessSynergyBonus(mods);   // combos worse than the sum of their parts pay extra too
	// The sector's MODIFIERS pay in tenths of the base; the shipped LEVEL pays a separate, finer term
	// in thousandths (endless_levelprofile.h payoutMille) so two same-grade levels still differ in cash.
	const long total = base + base * tenths / 10 + base * payoutMille / 1000;
	const long floor = base / 4;
	return (total > floor) ? total : floor;  // always a minimum reward
}

long endlessClearBonusFor(Uint64 mods)
{
	return endlessClearBonusForEx(mods, 0);
}

// Cash paid on CLEARING a level -- the base plus whatever the active sector's mutators add (tenths),
// plus the committed LEVEL's own fine payout term (thousandths). endlessSortiePayoutMille keys off the
// same committed level the course card priced (endlessCoursePayout), so banked == shown.
// Taking the harder route pays off.
static long endlessClearBonus(void)
{
	return endlessClearBonusForEx(endlessActiveMods, endlessSortiePayoutMille());
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

// --- Outpost economy: reroll / hull upgrade -------------------------------------
// These two actions REPLACE the Data Cubes and Ship Specs entries in JE_itemScreen's
// own front menu when endlessMode is on (see game_menu.c), so the economy reuses the
// in-game shop rather than a bespoke screen. Prices escalate per use, reset each visit.

// --- Outpost price tuning ----------------------------------------------------------------
// Every per-visit price is `BASE + zone * PER_ZONE`, so the whole outpost inflates as the run
// deepens; buying the same thing twice in one visit then escalates it again by the REBUY rule
// next to each buy. Retune the outpost here.
#define ENDLESS_PRICE_REROLL_BASE        6000
#define ENDLESS_PRICE_REROLL_PER_ZONE    1000
#define ENDLESS_PRICE_HULL_BASE         15000
#define ENDLESS_PRICE_HULL_PER_ZONE      2000
#define ENDLESS_PRICE_BOMB_BASE          2500
#define ENDLESS_PRICE_BOMB_PER_ZONE       400
#define ENDLESS_PRICE_EXTRAPERK_BASE    70000  // EXTREME: ~100k with 1 perk owned (x1.4 surcharge);
#define ENDLESS_PRICE_EXTRAPERK_PER_ZONE 2500  // extra perks are a luxury on top of the free picks
#define ENDLESS_PRICE_CLEANSE_BASE      25000
#define ENDLESS_PRICE_CLEANSE_PER_ZONE   2500

// Buying the same thing again WITHIN one visit escalates its price: cost = cost * NUM/DEN + ADD.
// Steeper numbers mean "one per visit, really"; gentler ones allow a restock.
#define ENDLESS_REBUY_REROLL_NUM      8      // reroll: x1.6 and a flat bump on top
#define ENDLESS_REBUY_REROLL_DEN      5
#define ENDLESS_REBUY_REROLL_ADD   3000
#define ENDLESS_REBUY_HULL_NUM        3      // hull tier: x1.5 and a flat bump
#define ENDLESS_REBUY_HULL_DEN        2
#define ENDLESS_REBUY_HULL_ADD     5000
#define ENDLESS_REBUY_BOMB_NUM        3      // bombs: x1.5, so a full restock costs more each time
#define ENDLESS_REBUY_BOMB_DEN        2
#define ENDLESS_REBUY_EXTRAPERK_NUM   2      // extra perk: doubles -- perks are strong and bounded
#define ENDLESS_REBUY_EXTRAPERK_DEN   1
#define ENDLESS_REBUY_CLEANSE_NUM     2      // sabotage charge: doubles
#define ENDLESS_REBUY_CLEANSE_DEN     1

// Apply one of the escalations above.
static long endlessRebuy(long cost, long num, long den, long add)
{
	return cost * num / den + add;
}

void endlessResetShopPrices(void)
{
	endlessRerollCost = ENDLESS_PRICE_REROLL_BASE + (long)endlessRunDepth * ENDLESS_PRICE_REROLL_PER_ZONE;
	endlessHullCost   = ENDLESS_PRICE_HULL_BASE + endlessRunDepth * ENDLESS_PRICE_HULL_PER_ZONE;
	endlessBombCost   = ENDLESS_PRICE_BOMB_BASE + (long)endlessRunDepth * ENDLESS_PRICE_BOMB_PER_ZONE;
	endlessExtraPerkCost = ENDLESS_PRICE_EXTRAPERK_BASE + (long)endlessRunDepth * ENDLESS_PRICE_EXTRAPERK_PER_ZONE;
	endlessCleanseCost = ENDLESS_PRICE_CLEANSE_BASE + (long)endlessRunDepth * ENDLESS_PRICE_CLEANSE_PER_ZONE;
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

// E-Shop cash-fraction buys, all priced off the cash the player WALKED IN WITH (frozen at entry,
// so spending inside the shop can't move the price mid-visit) and applied to the NEXT sector.
// Only one of the three kill-fire buffs per visit; Buy Special is a single premium buy.
static long endlessCashFraction(long num, long den)
{
	return endlessShopEntryCash * num / den;
}

long endlessTurbodrivePrice(void) { return endlessCashFraction(2, 3); }    // 66% -- the cheapest kill-fire buy
long endlessOverblastPrice(void)  { return endlessCashFraction(3, 4); }    // 75% -- damage stacks only
long endlessOverdrivePrice(void)  { return endlessCashFraction(19, 20); }  // 95% -- both, and nearly everything you have
long endlessSpecialPrice(void)    { return endlessCashFraction(4, 5); }    // 80% -- a random special weapon
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

// The three kill-fire buys differ only in what they cost, which mod bit they arm and the kind id
// that rides the save. Everything else is shared: the "one buff per visit, not while recharging,
// must be affordable" gate, the charge tier scaled off the cash actually paid, the one-kill-fire-
// effect-at-a-time rule, and the recharge lock that closes all three.
//
// The ENDLESS_BUFF_KIND_* ids live in endless.h -- the shop menu reads them too.
static bool endlessTryBuyKillFire(long cost, unsigned bit, int kind)
{
	if (endlessBuffKind != 0 || endlessBuffOnCooldown() || cost < 1 || player[0].cash < (ulong)cost)
		return false;
	player[0].cash -= cost;
	endlessBuffCharge = endlessBuffChargeFromPaid(cost);  // bigger spend -> longer window + more damage
	// OR'd into the next sector in endlessSelectCourse. Replacing the whole kill-fire field keeps it
	// to one effect at a time, which also clears any gambled curse.
	endlessPurchasedMods = (endlessPurchasedMods & ~ENDLESS_MOD_KILLFIRE_ANY) | bit;
	endlessBuffKind = kind;
	endlessArmBuffCooldown();  // lock all three kill-fire buys for the scaled recharge
	return true;
}

bool endlessTryBuyTurbodrive(void)  // quickened fire for a window after each kill
{
	return endlessTryBuyKillFire(endlessTurbodrivePrice(), ENDLESS_MOD_TURBODRIVE, ENDLESS_BUFF_KIND_TURBODRIVE);
}

bool endlessTryBuyOverdrive(void)   // Turbodrive + per-kill fire/damage stacks (implies the base window)
{
	return endlessTryBuyKillFire(endlessOverdrivePrice(), ENDLESS_MOD_OVERDRIVE, ENDLESS_BUFF_KIND_OVERDRIVE);
}

bool endlessTryBuyOverblast(void)   // Overdrive's per-kill DAMAGE stacks only -- no fire boost
{
	return endlessTryBuyKillFire(endlessOverblastPrice(), ENDLESS_MOD_OVERBLAST, ENDLESS_BUFF_KIND_OVERBLAST);
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
	endlessBombCost = endlessRebuy(endlessBombCost, ENDLESS_REBUY_BOMB_NUM, ENDLESS_REBUY_BOMB_DEN, 0);
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
	endlessExtraPerkCost = endlessRebuy(endlessExtraPerkCost, ENDLESS_REBUY_EXTRAPERK_NUM, ENDLESS_REBUY_EXTRAPERK_DEN, 0);
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
	endlessCleanseCost = endlessRebuy(endlessCleanseCost, ENDLESS_REBUY_CLEANSE_NUM, ENDLESS_REBUY_CLEANSE_DEN, 0);
	return true;
}
// Strip the single most-dangerous hostile bit from a modifier set (one per cleanse charge).
Uint64 endlessStripWorstMod(Uint64 mods)
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
	endlessRerollCost = endlessRebuy(endlessRerollCost, ENDLESS_REBUY_REROLL_NUM, ENDLESS_REBUY_REROLL_DEN, ENDLESS_REBUY_REROLL_ADD);
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
	endlessHullCost = endlessHullCost * ENDLESS_REBUY_HULL_NUM / ENDLESS_REBUY_HULL_DEN + ENDLESS_REBUY_HULL_ADD;  // int-typed: kept in int arithmetic
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
