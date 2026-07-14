/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 *
 * Endless mode — see endless.c.
 *
 * A run of real, UNMODIFIED shipped levels: outpost + Chart-a-Course between zones,
 * difficulty via depth- and mutator-scaled enemy STATS (notes.md §Endless mode).
 */
#ifndef ENDLESS_H
#define ENDLESS_H

#include "opentyr.h"

#include <stdbool.h>

// Master mode flag `endlessMode` lives in config.c/.h with the other game-mode
// flags (twoPlayerMode, superTyrian, ...) so save/load and the rest of the engine
// can reference it uniformly.

// Sector mutator EFFECT bits, folded into the difficulty levers, special-enemy spawns and
// the clear-cash reward. Curated combos are the named themes in endless.c.
enum {
	ENDLESS_MOD_FORTIFIED   = 1u << 0,  // +enemy & boss HP (hostile)
	ENDLESS_MOD_FRENZY      = 1u << 1,  // enemies fire much faster (hostile)
	ENDLESS_MOD_SWIFT       = 1u << 2,  // much faster enemy projectiles (hostile)
	ENDLESS_MOD_FRAGILE     = 1u << 3,  // enemies have much less HP (boon)
	ENDLESS_MOD_BOUNTY      = 1u << 4,  // big clear payout, no added danger (boon)
	ENDLESS_MOD_DEVASTATING = 1u << 5,  // enemy shots deal much more damage (hostile)
	ENDLESS_MOD_ELITEPACK   = 1u << 6,  // half of all enemies are elite/champion (hostile)
	ENDLESS_MOD_APEX        = 1u << 7,  // every enemy is elite/champion (hostile, super rare)
	ENDLESS_MOD_LEGION      = 1u << 8,  // every enemy is a CHAMPION (hostile, ultra rare)
	ENDLESS_MOD_ENRAGE      = 1u << 9,  // enemies fire faster the longer the zone runs (hostile)
	ENDLESS_MOD_KAMIKAZE    = 1u << 10, // enemies home in on you at a moderate pace, NO ram bonus (hostile; the mid homing tier -- the brutal rammer is RAMPAGE below)
	ENDLESS_MOD_GRAVITY     = 1u << 11, // a constant pull drags your ship down (hostile)
	ENDLESS_MOD_TURBODRIVE   = 1u << 12, // each kill briefly quickens your guns (boon)
	ENDLESS_MOD_OVERCHARGE  = 1u << 13, // your weapons hit much harder (boon)
	ENDLESS_MOD_DILATION    = 1u << 14, // enemy shots move much slower (boon)
	ENDLESS_MOD_FAVOR       = 1u << 15, // the next outpost slashes its prices (boon)
	ENDLESS_MOD_CURSED      = 1u << 16, // a fortune in cash, but the next shop is barren (mixed)
	ENDLESS_MOD_OVERCLOCK   = 1u << 17, // faster enemy fire, projectiles AND scroll pace (hostile)
	ENDLESS_MOD_SLIPSTREAM  = 1u << 18, // just the scroll pace is sped up (the easy Overclock)
	ENDLESS_MOD_OVERLOAD    = 1u << 19, // Overclock cranked WAY up -- brutal fire/shots/scroll (rare)
	ENDLESS_MOD_WARP        = 1u << 20, // Slipstream cranked WAY up -- the level BLURS past (rare boon)
	ENDLESS_MOD_OVERDRIVE = 1u << 21,// E-Shop "Overdrive" buy: Turbodrive + each kill stacks fire/damage
	ENDLESS_MOD_BACKFIRE  = 1u << 22, // Backfire (Evil Turbodrive): each kill briefly JAMS your guns (slower fire)
	ENDLESS_MOD_BURNOUT   = 1u << 23, // Burnout (Evil Overdrive): the jam PLUS each kill stacks a fire+damage penalty
	ENDLESS_MOD_OVERBLAST = 1u << 24, // E-Shop "Overblast" buy: Overdrive's DAMAGE half only -- each kill stacks shot damage, no fire boost (boon)
	ENDLESS_MOD_MISFIRE   = 1u << 25, // Misfire (Evil Overblast): each kill stacks a shot-damage CUT, no jam (hostile)
	// --- gamble-only next-sector effects (never rolled as a course theme; set via the E-Shop gamble) ---
	ENDLESS_MOD_MARKED    = 1u << 26, // Marked: the next sector's boss is beefed up (+2 boss HP mult)
	ENDLESS_MOD_NITRO     = 1u << 27, // Nitro: your shots hit harder (paired OVERCHARGE), but ANY hit is fatal
	ENDLESS_MOD_OVERHEAT  = 1u << 28, // Overheat: kills quicken your guns (paired TURBODRIVE), but the hull cooks (chip DoT)
	ENDLESS_MOD_DUD       = 1u << 29, // Dud: your superbombs refuse to fire this sector
	// --- hostile sector theme: the gentlest homing tier ---
	ENDLESS_MOD_HOMING    = 1u << 30, // enemies barely lean toward you -- the EASIEST homing tier, no ram (hostile)
};

// Bit 31 as a #define, not an enum constant: (1u << 31) overflows `int`, which a C enum constant
// must fit in. RAMPAGE: aggressive homing rammers with extra ram damage (gamble-only, ~1/5000).
#define ENDLESS_MOD_RAMPAGE (1u << 31)

// The six kill-fire mods -- boons Turbodrive/Overdrive/Overblast, evil mirrors Backfire/Burnout/
// Misfire -- a sector carries at most one (notes.md §Course generation & danger labels).
#define ENDLESS_MOD_FIREBOOST      (ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_OVERDRIVE)
#define ENDLESS_MOD_FIREJAM        (ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_BURNOUT)
#define ENDLESS_MOD_DMGUP          (ENDLESS_MOD_OVERDRIVE | ENDLESS_MOD_OVERBLAST)
#define ENDLESS_MOD_DMGDOWN        (ENDLESS_MOD_BURNOUT | ENDLESS_MOD_MISFIRE)
#define ENDLESS_MOD_STACKED        (ENDLESS_MOD_DMGUP | ENDLESS_MOD_DMGDOWN)
#define ENDLESS_MOD_KILLFIRE_GOOD  (ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_OVERDRIVE | ENDLESS_MOD_OVERBLAST)
#define ENDLESS_MOD_KILLFIRE_EVIL  (ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_BURNOUT | ENDLESS_MOD_MISFIRE)
#define ENDLESS_MOD_KILLFIRE_ANY   (ENDLESS_MOD_KILLFIRE_GOOD | ENDLESS_MOD_KILLFIRE_EVIL)

// An enemy type at/above this base armor counts as a "boss" -- excluded from elite rolls
// (bosses get their own HP scaling) and tallied separately on the run-end screen.
#define ENDLESS_BOSS_ARMOR 200

// Palette bank (high nibble) the elite/champion tints remap enemy sprites into via the blit
// filter (see blit_sprite2_filter). The exact hue depends on the level's palette; tuned by
// eye. The tiny enemy HP bars reuse these banks so a bar matches its enemy's tint.
#define ENDLESS_ELITE_FILTER    0xD0
#define ENDLESS_CHAMPION_FILTER 0x50  // bank 5 = purple (an "epic" aura; 0xB0 read as brown)

// Hull tints flash only for PLAYER-side kill-fire buffs (Overclock/Overload change enemy
// behaviour, so they do not tint the ship). Palette-relative; tuned by eye.
#define ENDLESS_TURBODRIVE_SHIP_FILTER 0xC0  // bank 12 = red (Turbodrive)
#define ENDLESS_OVERDRIVE_SHIP_FILTER 0x70 // bank 7 = electric yellow -- the escalating Overdrive buff (tunable)
#define ENDLESS_OVERBLAST_SHIP_FILTER 0x90 // bank 9 = blue -- the damage-only Overblast buff (tunable)
#define ENDLESS_EVIL_SHIP_FILTER      0x40 // bank 4 -- the Backfire/Burnout/Misfire curse tint (tuned by eye)

// Power-gauge recolour while a kill-fire BOON window is up (fire costs no power then). A bank
// BASE: the 14-shade ramp must stay in one bank (notes.md §Course generation & danger labels).
#define ENDLESS_FREE_POWER_GAUGE_BASE 1 // bank 0 (gray)

// Per-kill DAMAGE stack cap: the bonus scales so ENDLESS_OVERDRIVE_DMG_MAX lands exactly at the
// cap, matching the fire ramp's combo-200 peak. Burnout/Misfire's damage CUT mirrors it.
#define ENDLESS_OVERDRIVE_MAX_STACKS 200
#define ENDLESS_OVERDRIVE_DMG_MAX    150  // Overdrive/Overblast: +this% shot damage at the stack cap (i.e. at ~200 combo)

// Ceiling on any endless enemy's effective HP multiplier, so even an elite/champion boss
// deep in a run can't become an unkillable sponge.
#define ENDLESS_HP_MULT_MAX 24

// Base (shipped) level the current / previous ZONE is built on, for the crash logger. The name
// is the level's authored name (endless renames the live levelName to "ZONE n"); "" until set.
const char *endlessBaseLevelName(void);
int         endlessBaseLevelEpisode(void);
int         endlessBaseLevelSection(void);
const char *endlessPrevLevelName(void);
int         endlessPrevLevelEpisode(void);
int         endlessPrevLevelSection(void);

// Anti-repeat recent-level ring (newest first, i = 0): the base levels the next zone's picker keeps
// away from, so no level recurs twice in a row or within a few zones. For the crash logger; indices
// out of range read as 0.
int endlessRecentLevelCount(void);
int endlessRecentLevelEpisode(int i);
int endlessRecentLevelSection(int i);

// How many levels have been cleared this run (0 on a fresh run). Drives the difficulty
// ramp and is the depth/score of the run.
extern int endlessRunDepth;

// Run kill tallies, shown on the run-end screen. Both reset each run. endlessRunBossKills is
// bumped in draw_boss_bar (tyrian2.c) the moment a boss health bar empties, so it counts only
// enemies that actually spawned a boss bar -- not high-armor mini-bosses or tough regulars.
extern int endlessRunKills;
extern int endlessRunBossKills;

// Count a destroyed enemy toward the total kill tally (no-op outside endless mode). Pass the
// killed enemy's linknum. A multi-part enemy (shared nonzero linknum) is counted once, not per
// tile, so the Overdrive stack and kill tally stay per-enemy.
void endlessCountKill(int linknum);

// Mutator bits (ENDLESS_MOD_*) active on the current level; set by endlessChooseNextLevel.
extern unsigned endlessActiveMods;

// Run-persistent bonus to max armor, bought at the outpost ("Reinforce Hull"). Added to
// the ship's armor each level start (varz.c ship-info). Reset each run.
extern int endlessArmorBonus;

// Reset all per-run state at the start of a new Endless run.
void endlessResetRun(void);

// --- Run seed ------------------------------------------------------------------------
// A run's STRUCTURE (level order, mutators, perks, shop stock) is reproducible from a seed;
// combat randomness stays unseeded by design (notes.md §Seeded structure RNG).
#define ENDLESS_SEED_MAXLEN 24  // seed string buffer (incl. NUL); also the on-disk save width

// HARDCORE mode: a per-run toggle chosen on the seed screen -- NO saving at all, and a mid-zone
// bail re-locks the outpost to the launch-time choices (notes.md §Save / resume).
extern bool endlessHardcore;

// The pre-difficulty "choose your seed" screen: type any text for a seeded run, or take a random
// one. Also toggles HARDCORE for the run. Writes the chosen seed (always non-empty) into outSeed and
// the hardcore choice into *outHardcore (may be NULL); returns false if cancelled (Esc).
bool endlessSeedSelect(char *outSeed, size_t outN, bool *outHardcore);

// Establish the run's seed (copy the string + hash it + prime the stream). Call once when a run
// begins, right after endlessResetRun. Any string is valid; "" hashes to a fixed default.
void endlessSetSeed(const char *s);

// The current run's seed string, for the in-game display (shown on the E-Shop help line).
const char *endlessSeedString(void);

// --- Save / resume ------------------------------------------------------------------
// Run-persistent endless state rides in a sidecar (endless.sav) keyed by the same save slot,
// because the tyrian.sav layout is fixed and checksummed. Loading a slot with an endless
// record re-enters endless mode at that zone's outpost. notes.md §Endless save / resume.
void endlessSaveSlot(JE_byte slot);  // persist the current run to `slot` (or clear that slot's endless record when not in endless mode); call right after JE_saveGame
bool endlessLoadSlot(JE_byte slot);  // if `slot` holds an endless run, enter endless mode + restore it (true); else leave the normal load untouched (false); call right after JE_loadGame
bool endlessResumePending(void);     // true after an endless save was loaded but before its outpost has reopened; lets the main loop route an in-shop load back to the outpost instead of straight into the level

// --- "Quit Level" -> outpost retry ----------------------------------------------------------
// In endless the ESC menu's Quit Level reverts the level to its launch state and reopens the
// outpost: HARDCORE relocked (retry the same level or quit the run -- no farm-then-bail),
// non-hardcore unlocked (re-outfit freely; launch state still reverted). Session state only.
extern bool endlessQuitToOutpost;  // set by the ESC-menu Quit in endless; consumed by the game loop
extern bool endlessLockedSortie;   // the reopened outpost is locked to the launch-time choices (hardcore only)

void endlessCaptureSortie(void);   // snapshot the launch-time state; called at every endless level start
void endlessRestoreSortie(void);   // revert to that snapshot; in hardcore arm the locked outpost (sets endlessLockedSortie)
bool endlessSortieValid(void);     // a sortie snapshot is available to restore / relaunch
void endlessArmLockedRelaunch(void); // re-arm the committed level (episode + mods + level) WITHOUT re-running course selection

// Fallback single-level picker: a random endless-safe level from any installed episode
// (switching episode data as needed). endlessChooseNextLevel is the normal path.
JE_byte endlessPickNextLevel(void);

// Next-level "courses": the choice presented in the shop's Start Level submenu (see
// game_menu.c, which drives its planet monitor from these). Each course is a shipped level
// plus its own risk/reward mutators; course 0 is always clean (no modifiers) and a
// pure-upside BOUNTY course is rare. Generated once per shop visit by endlessBetweenLevels.
void        endlessGenerateCourses(void);  // build this visit's courses
int         endlessCourseCount(void);      // how many courses (up to 3)
const char *endlessCourseName(int i);      // short label for the menu list
const char *endlessCourseHelp(int i);      // one-line risk summary (tier) for the help line
const char *endlessCourseRank(int i);      // letter danger grade (F..S+++) for the monitor RANK field
int         endlessCourseRankLevel(int i); // numeric danger level 0(F)..9(S+++), or -1; drives the tint
JE_byte     endlessCoursePlanet(int i);    // star-map planet index for the monitor (cosmetic)
JE_byte     endlessCourseSection(int i);   // the course's level section (for mapSection[])
JE_byte     endlessSelectCourse(int i);    // apply course i (mutators + episode); returns its section
long        endlessCoursePayout(int i);    // truthful clear payout for course i (shown highlighted in help)

// One row of the Chart-a-Course monitor overlay: an active modifier's short label, its danger
// weight (the same reward-tenths endlessDangerScore sums, driving how dark its red tint draws),
// and which side of the monitor it lists on (hostile/trap = left in red, boon = right in green).
typedef struct {
	const char *word;
	int         weight;
	bool        hostile;
} EndlessCourseModRow;

// Fill rows[] with course i's individual modifiers, worst-first (highest weight leading), up to
// max entries. Returns the count. game_menu.c draws these ON the planet monitor itself.
int endlessCourseModRows(int i, EndlessCourseModRow *rows, int max);

// The between-level step: bank interest + clear reward on cash, then open the standard
// item shop (JE_itemScreen). Also pins the planet-map hub so the shop's planet monitor
// can't read out of bounds. Reroll/hull upgrades live inside the shop's own front menu.
void endlessBetweenLevels(void);

// Outpost economy actions, wired into JE_itemScreen's front menu in endless mode (the
// Reroll and Reinforce entries replace Data Cubes and Ship Specs; see game_menu.c).
// Prices escalate per use and reset each shop visit.
void endlessResetShopPrices(void);
long endlessRerollPrice(void);    // current reroll cost (for the menu label)
int  endlessHullPrice(void);      // current hull-upgrade cost (for the menu label)
bool endlessHullMaxed(void);      // true once the run's armor bonus is capped
bool endlessTryReroll(void);      // buy a shop reroll; false if unaffordable
bool endlessTryReinforce(void);   // buy a +armor hull upgrade; false if unaffordable/maxed

// E-Shop cash-fraction buys (the "E-Shop" submenu; see game_menu.c). Turbodrive (kill-fire
// boost) costs 66% of cash; Overblast (Overdrive's DAMAGE half only, no fire boost) is 75%;
// Overdrive is Turbodrive PLUS the escalating per-kill fire/damage stack for 95%. Only ONE of
// the three kill-fire buffs is buyable per visit. Buy Special grants a random special for 80% --
// pricey enough to be effectively one premium buy per visit. All price off the shop-entry cash;
// all apply to the NEXT sector.
long endlessTurbodrivePrice(void);         // Turbodrive cost (66% of entry cash), for the label
long endlessOverblastPrice(void);    // Overblast cost (75% of entry cash), for the label
long endlessOverdrivePrice(void);   // Overdrive cost (95% of entry cash), for the label
long endlessSpecialPrice(void);      // Buy-Special cost (80% of entry cash), for the label
bool endlessBuffBought(void);        // true once ANY kill-fire buff is bought this visit
int  endlessBuffKindBought(void);    // which buff this visit: 0 none, 1 Turbodrive, 2 Overdrive, 3 Overblast
bool endlessBuffOnCooldown(void);    // a kill-fire buy is on recharge (a prior buy locked all three); no buy this visit
int  endlessBuffCooldownLeft(void);  // sectors until the kill-fire buys unlock again (0 = ready now)
bool endlessTryBuyTurbodrive(void);        // buy Turbodrive; false if unaffordable / a buff already owned / on recharge
bool endlessTryBuyOverblast(void);   // buy Overblast; false if unaffordable / a buff already owned / on recharge
bool endlessTryBuyOverdrive(void);// buy Overdrive; false if unaffordable / a buff already owned / on recharge
bool endlessTryBuySpecial(void);     // buy a random special weapon; false if unaffordable

// Expanded E-Shop buys. Bomb: +1 superbomb (cap 10). Revive: a held one-shot that survives a lethal
// hit (endlessConsumeRevive, called from the death path). Extra Perk: opens a bonus perk pick.
// Sabotage: cleanse charges that strip the worst mutator off the NEXT chosen course. Gamble: a
// random good/bad outcome (result text via endlessGambleResult, shown on the E-Shop help line).
long endlessBombPrice(void);
bool endlessBombFull(void);
bool endlessTryBuyBomb(void);
long endlessRevivePrice(void);
bool endlessReviveArmed(void);       // a revive token is currently held
bool endlessTryBuyRevive(void);
bool endlessConsumeRevive(void);     // spend a held revive on death; true = survived (caller clears screen)
long endlessExtraPerkPrice(void);
bool endlessTryBuyExtraPerk(void);   // charges + rolls the offers; the dispatch then opens MENU_PERKS
long endlessCleansePrice(void);
int  endlessCleanseCharges(void);    // sabotage strips queued for the next course select
bool endlessTryBuyCleanse(void);
long endlessGamblePrice(void);
bool endlessTryGamble(void);
const char *endlessGambleResult(void);  // last gamble outcome text (for the E-Shop help line)
bool endlessGambleWonPerk(void);        // last gamble handed out a free perk pick (dispatch opens MENU_PERKS)
void endlessClearGamblePerk(void);      // consume that flag once the perk menu has opened, so later E-Shop buys don't re-open it
int  endlessShopTaxPercent(void);       // Loan Shark: permanent +% added to every shop price this run (0 = none)
int  endlessGambleOutcomeCount(void);   // number of distinct gamble outcomes (for the debug "Gamble Outcomes" page)
const char *endlessGambleOutcomeName(int id);  // display name of gamble outcome `id`
void endlessForceGambleOutcome(int id); // debug: fire outcome `id`'s effect directly (no fee), for testing
const char *endlessLastGrantedSpecial(void);  // name of the last special granted this shop visit ("" if none)
unsigned endlessPendingMods(void);   // kill-fire buff bits bought this visit, not yet applied (for the debug jump)

// Called once per endless level, right after JE_loadMap loads the shipped level, to fix
// up the per-level state a random level jump would otherwise leave in a crashing state
// (level name -> "ZONE n", datacube count, planet-map hub, special-mode flags). The
// level's actual content -- enemies, placement, terrain -- is left exactly as authored.
void endlessRegenerateLevel(void);

// The "light cone" (spotlight) effect is decoupled from a level's own script in endless mode:
// a level that ships with the spotlight has it stripped, and instead each zone gets an
// independent, seeded 1-in-10 chance of it (decided in endlessRegenerateLevel). True when the
// current zone rolled the cone; read by the starShowVGASpecialCode override in tyrian2.c.
bool endlessLightConeActive(void);

// Load the level's initial enemy sprite banks up front (right after the engine resets
// the bank table at level start), so ambient/early enemies aren't invisible when they
// spawn before the level's scheduled bank-load event fires.
void endlessPreloadBanks(void);

// Starting cash for a fresh run, by chosen difficulty (easier = more).
long endlessStartingCash(void);

// Called when a run ends (the player died): shows the glowing Run Over summary, then the caller
// returns to the title screen.
void endlessOnRunEnd(void);

// Called when the player VOLUNTARILY quits an in-progress run to the title (from any outpost's Quit
// Game). Clears endlessMode; in hardcore it first shows the Run Over summary (quitting is final --
// no save to resume), matching the death path. The caller then returns to the title.
void endlessEndRunToTitle(void);

// Apply the level-clear payout (bank interest on unspent cash + the depth/mutator-scaled
// clear bonus) to the player's cash, reporting the two amounts so the level-end screen can
// show them. Both zero before the first level is cleared. Applied by JE_endLevelAni.
void endlessApplyLevelPayout(long *interestOut, long *bonusOut);

// Grant a random special weapon (Repulsor, Flare, ...) and announce it in the in-game text
// bar. In endless the datacube and secret-orb pickups a level drops call this instead of
// their normal (dead-in-endless) behaviour.
void endlessGrantSpecial(void);

// Depth- and mutator-scaled enemy difficulty levers, applied by the engine on top of
// the base difficulty. HP is split like the expert-mode toggle: ordinary enemies scale
// by an armor percent, bosses (at the 254 armor cap) scale by a damage divisor instead.
int endlessArmorPercent(void);      // ordinary-enemy HP scale (100 = unchanged); 254 cap applies
int endlessBossHpMult(void);        // boss HP divisor (1 = unchanged); N = N times boss HP
int endlessFireDelayPercent(void);  // enemy shot-cooldown scale (100 = unchanged; lower = fires faster)
int endlessShotSpeedPercent(void);  // enemy projectile-speed scale (100 = unchanged; higher = faster)
int endlessShotDamagePercent(void); // enemy shot-damage scale (100 = unchanged; higher = hits harder)

// Rising-tide "quantity" scaling: past where the intensity levers above cap out (~zone 100), the
// tide adds the one axis with no engine ceiling -- more enemy shots per volley and a rising share
// of elite/champion shooters. Both read the single tide coefficient, which is 0 through the early
// game and then climbs without bound. Tune the onset/slope via ENDLESS_TIDE_* in endless.c.
int endlessTideLevel(void);        // the single tide knob (0 early, then +1 per effective zone)
int endlessExtraEnemyShots(void);  // extra enemy shots to add to each firing volley at this tide

// Player-side + time-based modifier hooks (see endless.c).
int  endlessPlayerDamagePercent(void);  // OVERCHARGE / Overdrive stacks + Heavy Rounds perk: your shot-damage scale (100 = normal)
void endlessGameplayTick(void);         // once per game tick (main player): zone timer + turbodrive/Overdrive decay
bool endlessConsumeArmorHudDirty(void); // true once after the Overheat DoT shaves hull -> the game loop repaints the armor bar
bool endlessTurbodriveActive(void);      // TURBODRIVE kill-streak fire boost currently active?

// Live kill-fire buff readout for the in-game HUD (Turbodrive / Overdrive). All report the
// BUFF's own contribution while its window is active; see JE_inGameDisplays. No name string is
// exposed by design -- the HUD shows only numbers (combo count, fire/damage %, timer).
int endlessKillBuffTicksLeft(void);    // window ticks remaining (drains ~2s after the last kill)
int endlessKillBuffTicksMax(void);     // full window length, for the timer bar proportion
int endlessKillBuffComboCount(void);   // combo kill count driving the escalation, shown as "xN"
int endlessKillBuffColorBank(void);    // themed palette bank (red Overdrive / yellow Turbodrive)
int endlessKillBuffFireMultiplier(void);// fire-rate multiplier the buff is granting (1 = none; 2x..7x, ~9x w/ Overdrive)
int endlessKillBuffDamagePercent(void); // shot-damage bonus % the buff is granting (0 during Turbodrive)
int  endlessKillBuffFireDecrements(void); // extra shotRepeat decrements this tick (Turbodrive base + Overdrive stacks)
int  endlessPerkSpecialCooldownDecrements(void); // Rapid Recharge perk: extra cooldown decrements/tick, applied by the caller to the special-weapon gate AND sidekick ammo refill
int   endlessGravityPull(void);         // GRAVITY: per-tick downward nudge (classic non-VT ship path)
float endlessGravityDrift(void);        // GRAVITY: downward drag in px per 35Hz tick (VT ship path)
int  endlessExtraScrollSteps(void);     // OVERCLOCK/SLIPSTREAM/OVERLOAD/WARP: extra scroll steps this tick
bool endlessScrollBoostActive(void);    // true while any scroll-speed modifier is active (stable across the tick, unlike the fractional step count)
int  endlessScrollExtraPx(int channel, int fireStep, int delayMax, int baseThisTick, float *rateOut, float *fracOut); // SMOOTH boost: extra scroll px this tick for layer `channel` (0/1/2); call once/channel/tick (notes.md §Endless scroll boost)
int  endlessShipTintFilter(void);       // player-ship blit filter: electric yellow while the TURBODRIVE buff is active (0 = none)

// Evil kill-fire curses (Evil Turbodrive / Evil Overdrive): the hostile mirrors of the boons.
// They reuse the same combo/stack machinery but slow your fire (and, for Evil Overdrive, cut your
// damage) instead of boosting it. See endless.c and the fire hooks in mainint.c / shots.c.
bool endlessKillFireIsEvil(void);            // is the active kill-fire window an evil curse (not a boon)?
int  endlessKillFireJamTicks(void);          // extra shotRepeat cooldown per shot while an evil curse is up (0 otherwise)
int  endlessKillBuffEvilDamagePenalty(void); // Evil Overdrive: shot-damage REDUCTION % currently applied (0 otherwise), for the HUD
const char *endlessKillFireEvilName(void);   // one-word HUD label for the active curse: JAMMED (Backfire) / BURNOUT / MISFIRE ("" if none)

// Display name / help for an arbitrary modifier bitset: a curated theme name if one exists,
// otherwise a generated one -- so the course generator can offer any combination.
const char *endlessComboName(unsigned mods);
const char *endlessComboHelp(unsigned mods);

// Special enemies come in two tiers: ELITE (tougher, tinted, pays a bounty) and CHAMPION
// (an elite that also fires faster and hits harder, a distinct tint, ~half as common). A
// depth-scaled trickle spawns them; the Elite Pack / Apex mutators force many/all. Toughness
// rides the boss-style damage accumulator; the roll is cached per linkgroup so a multi-tile
// enemy is one tier as a whole (see the JE_drawEnemy hook + endless.c).
void endlessResetElites(void);               // clear per-level decisions (each level start)
int  endlessRollEliteTier(JE_byte linknum);  // spawn tier: 1 normal, 2 elite, 3 champion (per linkgroup)
int  endlessEliteHpMult(void);               // elite & champion HP multiplier (boss-style divisor)
int  endlessEnemyHpMult(bool hasBossBar, int bossHpMult, int eliteState);  // combined per-hit HP divisor
long endlessEliteBounty(void);               // extra cash for destroying an elite
long endlessChampionBounty(void);            // extra cash for destroying a champion (more)
int  endlessChampionFireDelayPercent(void);  // champion extra fire-cooldown scale (lower = faster)
int  endlessChampionShotDamagePercent(void); // champion extra shot-damage scale (higher = harder)

// Award an elite/champion kill: pay its bounty into the player's cash and post a message to
// the in-game text bar ("Elite/Champion Enemy destroyed!  +cash"). No-op if eliteState < 2.
void endlessAwardEliteKill(int eliteState);

// --- Perks: run-persistent, stacking upgrades chosen after each cleared zone -----------
// After finishing a zone the shop opens on a forced perk pick (before the normal buy/sell
// front menu): three random perks plus a "take the cash" decline. Once you leave it you can't
// return this visit. Perks stack and last the whole run; their effects fold into the same
// player-side levers the sector mods / E-Shop buffs use.
extern bool endlessPerkPending;      // a perk pick is queued for the next shop's front gate

void        endlessGeneratePerkChoices(void);  // roll this visit's offers (call before the shop)
int         endlessPerkChoiceCount(void);      // how many perks are offered (usually 3)
const char *endlessPerkChoiceName(int i);      // menu label for offered perk i
const char *endlessPerkChoiceDesc(int i);      // help-line description (+ owned count) for offer i
void        endlessTakePerk(int i);            // acquire offered perk i (increments its stack); the post-zone pick is free
long        endlessPerkDeclineBonus(void);     // cash paid for taking no perk (scales with depth)
void        endlessDeclinePerk(void);          // take the cash instead of a perk

int endlessPerkArmorBonus(void);     // +max armor from Ablative Plating (added at ship-info, varz.c); may be negative (Glass Cannon)
int endlessPerkFireDecrements(void); // extra shotRepeat decrements/tick from Rapid Cyclers (+ Adrenaline when hurt)
int endlessPlayerDamageReduce(void); // flat reduction on each hit taken (Bulwark relic); applied in JE_playerDamage
bool endlessPerkAutoFireSpecial(void); // Autofire Special perk: auto-fire the equipped special while fire is held (varz.c)
int endlessPerkPowerUsePercent(void);  // Efficient Coils perk: generator power-use scale per main-weapon shot (100 = normal, lower = cheaper); applied in shots.c
int endlessPerkShieldWait(int base);   // Shield Matrix perk: shield-regen interval (ticks between +1 shield) reduced from `base`, floored; applied at the shield-regen reset in tyrian2.c
int endlessPerkChargeTicks(int base);  // Rapid Charger perk: charge-base sidekick charge interval (ticks per +1 charge level) reduced from `base`, floored; applied at the sidekick charge loop in mainint.c

// Perk registry accessors (for the endless debug screen: list / toggle / stack perks).
int         endlessPerkCount(void);          // number of perks (PERK_COUNT)
const char *endlessPerkName(int id);         // perk display name
const char *endlessPerkDesc(int id);         // perk one-line effect description (for the perk-list help)
int         endlessPerkMaxStack(int id);     // max stacks this perk allows
int         endlessPerkGetOwned(int id);     // current owned stacks
void        endlessPerkSetOwned(int id, int n); // set owned stacks (clamped 0..max)

#endif // ENDLESS_H
