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
 *
 * This file holds the run state, the run lifecycle (reset / kill counting / the
 * per-tick modifiers) and the zone milestones. The rest of endless mode lives in
 * the sibling endless_*.c files -- endless_internal.h maps out which owns what.
 */

#include "endless.h"
#include "endless_internal.h"

#include "config.h"        // difficultyLevel, DIFFICULTY_*, player-independent globals
#include "custom_weapon.h" // customWeaponPort / customSidekickSlot (reserved shop slots)
#include "episodes.h"      // item arrays + SHIP_NUM/PORT_NUM/... counts, episodeAvail, JE_initEpisode
#include "fonthand.h"      // JE_outText
#include "keyboard.h"      // newkey/lastkey_scan/keysactive, service_SDL_events
#include "loudness.h"      // fade_song
#include "mainint.h"       // JE_getCost
#include "mtrand.h"        // mt_rand
#include "nortsong.h"      // JE_playSampleNum, setDelay, wait_delayorinput, limit_render_fps
#include "nortvars.h"      // JE_anyButton
#include "palette.h"       // colors, fade_palette, fade_black
#include "player.h"        // player[]
#include "sprite.h"        // JE_loadCompShapes, enemySpriteSheets, shopSpriteSheet
#include "tyrian2.h"       // itemAvail, itemAvailMax
#include "varz.h"          // eventRec, maxEvent, map* globals
#include "video.h"         // VGAScreen/VGAScreen2, JE_showVGA, output_vsync

#include <stdio.h>
#include <string.h>

// --- Run state ------------------------------------------------------------------

int      endlessRunDepth  = 0;   // levels cleared this run (0 on the first level)
Uint64   endlessActiveMods = 0;  // ENDLESS_MOD_* bits for the current level (64-bit: TOPSY/SLUGGISH use bits 32-33)
int      endlessArmorBonus = 0;  // run-persistent +max armor bought at the outpost
int      endlessRunKills = 0;    // total enemies destroyed this run (shown on the end screen)
int      endlessRunBossKills = 0;// boss-tier enemies destroyed this run

// Per-zone timers (reset each zone): elapsed ticks drive ENRAGE; the turbodrive timer counts
// down the quickened-fire window after each kill. Advanced by endlessGameplayTick.
int endlessZoneTicks      = 0;
int endlessTurbodriveTimer = 0;
static bool endlessArmorHudDirty = false;  // set when the Overheat DoT shaves hull; the game loop repaints the (event-driven) armor bar

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
int endlessMilestoneKindOfZone(int zone)
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
int endlessMilestoneKind(void)
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

// The pinned track for a milestone class, or 0 for an ordinary zone.
JE_byte endlessMilestoneSong(int kind)
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
bool endlessPerkDueAtDepth(int depth)
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
	// "Bosses slain" stat counts only real bar-spawning bosses, not the high-armor regulars an
	// armor-threshold test here used to wrongly sweep in.
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
