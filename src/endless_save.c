/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 *
 * Endless mode: the endless.sav sidecar and the Quit Level sortie snapshot.
 *
 * One of the endless_*.c files that make up endless mode: endless.h is the public
 * interface, endless_internal.h the state and helpers the group shares.
 */

#include "endless.h"
#include "endless_internal.h"

#include "config.h"        // difficultyLevel, DIFFICULTY_*, player-independent globals
#include "episodes.h"      // item arrays + SHIP_NUM/PORT_NUM/... counts, episodeAvail, JE_initEpisode
#include "file.h"          // dir_fopen / dir_fopen_warn (endless.sav sidecar I/O)
#include "mainint.h"       // JE_getCost
#include "player.h"        // player[]
#include "sprite.h"        // JE_loadCompShapes, enemySpriteSheets, shopSpriteSheet
#include "tyrian2.h"       // itemAvail, itemAvailMax
#include "varz.h"          // eventRec, maxEvent, map* globals

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- "Quit Level" -> outpost retry (see endless.h) -------------------------------------------
bool endlessQuitToOutpost = false;  // ESC-menu Quit (endless): return to the outpost instead of ending the run
bool endlessLockedSortie  = false;  // the reopened outpost is locked to the launch-time loadout/course (hardcore only)
// The launch-time snapshot Quit Level reverts to. The endless run/outpost half lives in an
// EndlessSlotRec (declared near the save code); these primitives hold the loadout + committed level
// and are declared up here (and in endless_internal.h) so endlessResetRun can clear them.
bool     endlessSortieHave  = false; // a launch-time snapshot exists
static Player   endlessSortiePlayer[2];     // player[] loadout at launch (cash / items / superbombs)
static Uint64 endlessSortieModsV = 0;       // endlessActiveMods at launch (the committed level's mutators)
static JE_byte  endlessSortieSec   = 0;     // committed level section
static int      endlessSortieEp    = 0;     // committed episode
static JE_byte  endlessSortieFile  = 0;     // committed lvl file number
// One-shots consumed at the course pick (E-Shop buff / sabotage charges / Long Con), snapshotted
// pre-consumption so a non-hardcore bail can restore them (see endlessRestoreSortie).
unsigned endlessSortiePrePurchased = 0;
int      endlessSortiePreCleanse   = 0;
int      endlessSortiePreLongCon   = 0;

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
