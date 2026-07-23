/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 *
 * Endless mode: picking the shipped level behind each zone, its music and its reroll.
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
#include "loudness.h"      // fade_song
#include "lvlmast.h"       // shapeFile[]
#include "player.h"        // player[]
#include "sprite.h"        // JE_loadCompShapes, enemySpriteSheets, shopSpriteSheet
#include "tyrian2.h"       // itemAvail, itemAvailMax
#include "varz.h"          // eventRec, maxEvent, map* globals

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The track the last-played zone actually used, and the run depth it was picked for. Remembering
// the REAL song (rather than re-deriving an approximation of it from the previous zone's stream)
// is what makes the never-two-in-a-row guarantee exact; both ride the save so a resumed run still
// knows what it just heard. See endlessPickLevelMusic.
JE_byte endlessLastSong      = 0;   // 0 = nothing played yet this run
int     endlessLastSongDepth = -1;  // -1 = none

// Base (shipped) level each zone is built on, captured in endlessRegenerateLevel just before it
// renames levelName to "ZONE n" (levelName still holds the level's authored name there). When a
// new zone starts, the current base rolls down into "previous". Read only by the crash logger
// (endless base-level accessors below); both are reset per run in endlessResetRun.
char endlessBaseName[11]     = "";
int  endlessBaseEp           = 0;
int  endlessBaseLvl          = 0;
char endlessPrevBaseName[11] = "";
int  endlessPrevBaseEp       = 0;
int  endlessPrevBaseLvl      = 0;

// Recently-played base levels (anti-repeat): a small ring keyed by (episode, section), [0] = the
// zone just played. The next zone's course/level picker avoids everything in this window, so the
// same base level can't recur twice in a row and won't return until at least ENDLESS_LEVEL_HISTORY
// other zones have been played (the window shrinks gracefully when too few safe levels exist to
// fill it). Recorded in endlessRegenerateLevel (the one choke point every zone load passes through),
// reset in endlessResetRun, persisted with the run (save v6), and surfaced in the crash log.
int     endlessRecentEp[ENDLESS_LEVEL_HISTORY];
JE_byte endlessRecentSec[ENDLESS_LEVEL_HISTORY];
int     endlessRecentCount;  // valid entries, 0..ENDLESS_LEVEL_HISTORY

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
bool endlessRandomSafeLevel(int *epOut, JE_byte *secOut, JE_byte *fileOut)
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
