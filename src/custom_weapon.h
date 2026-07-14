/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 *
 * Custom Weapon Creator: the player edits a RAW engine weapon (JE_WeaponType)
 * directly — one independent design per power level — which is copied into a
 * reserved weapons[]/weaponPort[] slot and fired through the normal engine path,
 * so it behaves and previews exactly like a stock weapon.
 *
 * Every stock weapon can be imported byte-for-byte and then edited, so the
 * built-in arsenal serves as a starting point for the player's own designs.
 */
#ifndef CUSTOM_WEAPON_H
#define CUSTOM_WEAPON_H

#include "opentyr.h"
#include "episodes.h"   // JE_WeaponType, weaponPort, PORT_NUM, ...

// The custom weapon carries an independent raw design for each of the 11 power
// levels, so buying weapon-power upgrades in the shop steps through them exactly
// like a stock weapon.
#define CUSTOM_POWER_LEVELS 11

// A weapon port can have up to two fire modes (op[0] / op[1], selected by opnum):
// front guns only use mode 0, but a REAR gun toggles between them in-game. The
// custom weapon carries an independent design per (mode, power level).
#define CUSTOM_WEAPON_MODES 2

// The most simultaneous bullets a weapon can describe = the width of the engine's per-bullet
// arrays (WEAPON_MULTI_MAX, from episodes.h). Originally 8; raised there so custom weapons can
// exceed the old cap. The editor exposes exactly this many segments.
#define CUSTOM_BULLETS_MAX WEAPON_MULTI_MAX

// Scratch weapon slots for the compiled custom designs: one per (mode, level),
// CUSTOM_WEAP_BASE + mode*CUSTOM_POWER_LEVELS + (level-1). Lives in the unused
// WEAP_END1(818)..WEAP_START2(1000) gap, past the Charge-Laser (900-905) and Zica
// side-beams (906-907); 2 modes x 11 levels = 910..931 fits.
#define CUSTOM_WEAP_BASE 910

// Highest sound sample the engine can play (see sndmast.h SFX_COUNT).
#define CUSTOM_SOUND_MAX 31

enum  // where the custom weapon equips
{
	CUSTOM_EQUIP_FRONT = 0,   // front weapon bay
	CUSTOM_EQUIP_REAR,        // rear weapon bay
	CUSTOM_EQUIP_LEFT,        // left sidekick
	CUSTOM_EQUIP_RIGHT,       // right sidekick
	CUSTOM_EQUIP_BOTH,        // both sidekicks
	CUSTOM_EQUIP_COUNT
};

// The raw editable design of each (mode, power level): the engine's own weapon
// struct. The editor reads/writes customWeaponRaw[editMode][editLevel] in place.
extern JE_WeaponType customWeaponRaw[CUSTOM_WEAPON_MODES][CUSTOM_POWER_LEVELS];

// How many fire modes the weapon has (1 or 2 = the port's opnum). A second mode
// is only reachable when the weapon is equipped as a rear gun (mode toggle).
extern int customWeaponModes;

// Weapon-wide identity. These map to the single reserved port (not to any one
// level or mode), so they are shared across all designs.
extern char customWeaponName[31];
extern int  customWeaponCost;       // shop price, 0 .. 64000
extern int  customWeaponPowerUse;   // generator drain (port poweruse)
extern int  customWeaponEquipSlot;  // CUSTOM_EQUIP_*
extern int  customWeaponItemGraphic; // shop/HUD icon (weaponPort itemgraphic), 1 .. 237

// Number of charge shots when equipped as a sidekick (maps to option pwr + 1):
// 1 = no charging (fires instantly like a normal sidekick); N = the sidekick steps through N
// escalating shots as it holds to charge, then releases. Charge shot S (1..N) fires the
// mode-0 power level S design (customWeaponRaw[0][S-1]) — the per-level designs ARE the
// charge stages, each independently editable. Only the sidekick equip slots use it.
// (Engine: charge counts 0..pwr, firing wpnum + charge; e.g. the 6-shot Charge-Laser
// is pwr 5. So the stage count the player edits is pwr + 1.)
extern int  customWeaponChargeStages;

// Which power level / fire mode the editor is currently editing.
extern int customWeaponEditLevel;   // 0 .. CUSTOM_POWER_LEVELS-1
extern int customWeaponEditMode;    // 0 .. CUSTOM_WEAPON_MODES-1

// Master feature toggle (shows the "Custom" buy/sell row; gates equipping).
extern bool customWeaponEnabled;

// Port index claimed for the custom weapon (resolved at init). 0 = none free.
extern int customWeaponPort;

// Option (sidekick) slot claimed for the custom weapon's sidekick (0 = none free).
extern int customSidekickSlot;

// Sidekick body appearance (used when equipped as a sidekick). Mount style is the engine's
// option "tr": it selects both the position (0 side pod, 1 trailing large, 2 front-mounted,
// 3 trailing, 4 orbiting) AND the sprite sheet (styles 1-2 use the 2x2 spriteSheet10, the
// rest spriteSheet9 -- see mainint.c). gr[f] = Sprite + f*FrameStep; the engine draws frame
// + charge, so a charge sidekick steps through consecutive body sprites.
#define CUSTOM_SIDEKICK_MOUNTS 5   // tr values 0..4 are all valid mount styles
extern int customSidekickMount;     // option tr (0..4)
extern int customSidekickSprite;    // base body sprite (gr[0]); sheet depends on the mount
extern int customSidekickFrames;    // animation frame count (ani; 1 = static)
extern int customSidekickFrameStep; // sprite step between animation frames (0/1 typical)
extern int customSidekickAnimate;   // option: 1 = animate while firing, 2 = always animate

// Largest body sprite a mount's sheet can address (so the editor + materialize can clamp
// the sprite index — the sidekick blit is not bounds-checked). 0 if the sheet isn't loaded.
int customSidekickSpriteCount(int mount);

// Import sources: one named entry per real weapon port (sampleable across its
// power levels) and per sidekick option. Used by the editor's Import actions to
// seed a level (or the whole weapon) from a stock weapon.
typedef struct
{
	char    name[31];      // source weapon/sidekick name shown in the picker
	JE_word sourcePort;    // weaponPort index (1..PORT_NUM) when it has power levels; 0 = sidekick-sourced
	JE_word sourceWeapon;  // weapon number to use when sourcePort == 0 (a sidekick's single shot)
	JE_word sourceOption;  // options[] index when sidekick-sourced (0 otherwise); lets Import clone the body
	JE_byte maxPower;      // sampleable stages: a port's highest power level, or a charge sidekick's shot count (pwr+1); 1 if it has neither
} CustomBulletPreset;

// Headroom for every unique source across all PORT_NUM (60) ports + OPTION_NUM (37)
// sidekicks; dedup keeps the real count well below this.
#define CUSTOM_BULLET_PRESET_MAX 112
extern CustomBulletPreset customBulletPreset[CUSTOM_BULLET_PRESET_MAX];
extern int                customBulletPresetCount;

// Number of base power levels the given import source can be sampled at (1..11);
// 1 means the look is fixed (a sidekick shot, or a weapon with a single level).
int customBulletMaxPower(int presetIdx);

// One-time setup: gather the import-source list, claim a free port + sidekick slot,
// fill in a default design if none is loaded, and materialize. Call once after
// JE_loadItemDat() (also safe to call again — it never clobbers a loaded design).
void customWeaponInit(void);

// Copy every power level's raw design into weapons[CUSTOM_WEAP_BASE + level] and
// wire up weaponPort[customWeaponPort] + the sidekick. Safe to call after
// every edit.
void customWeaponMaterialize(void);

// Equip the (freshly materialized) custom weapon into the player's front/rear bay
// or sidekick slot per customWeaponEquipSlot. Returns true on success.
bool customWeaponEquip(void);

// Switch which power level / fire mode the editor is editing (edits happen in
// place, so these are just clamped assignments — no save/load dance).
void customWeaponSelectLevel(int level);
void customWeaponSelectMode(int mode);

// Import a stock weapon. Level: copy the source's chosen base power level into the
// level currently being edited. All: copy the source's whole power curve into all
// 11 levels and adopt its name / power drain (a full editable clone).
void customWeaponImportLevel(int presetIdx, int basePower);
void customWeaponImportAllLevels(int presetIdx);

// ADD (combine) a stock weapon on top of the current design instead of replacing it: the source's
// bullets are appended to what is already there, up to CUSTOM_BULLETS_MAX, so two or more weapons
// can be layered into one. Level adds the source's chosen base power level onto the level being
// edited; All adds each of the source's levels onto the matching custom level across all 11. Only
// bullet segments are added -- the design keeps its own name, cost and whole-volley fields.
void customWeaponAddLevel(int presetIdx, int basePower);
void customWeaponAddAllLevels(int presetIdx);

// Restore the built-in default design to the current level / roll a random (but
// always valid) design into the current level.
void customWeaponReset(void);
void customWeaponRandomize(void);

// Add / remove a bullet segment (one of the design's simultaneous bullets) in the
// design currently being edited. Add duplicates the segment at afterIndex, inserts
// the copy just after it (nudged sideways so it is visibly distinct), and returns the
// new segment's index — or -1 if the design is already at CUSTOM_BULLETS_MAX. Remove
// deletes the segment at index, shifting the rest down, and returns the index that
// should be selected next — or -1 if only one segment remains (a design must keep at
// least one bullet).
int customWeaponAddBullet(int afterIndex);
int customWeaponRemoveBullet(int index);

// Add / remove a sidekick charge state (bump customWeaponChargeStages, the shot count).
// Add jumps the edit level to the new top stage so its shot can be tuned immediately, and
// returns that level index — or -1 if already at CUSTOM_POWER_LEVELS states. Remove drops
// the top state (clamping the edit level) and returns the new top level index — or -1 if
// only one state remains (1 = no charging). Each charge state is the power level design at
// its index, so its sprite/damage/etc. are edited via the normal per-level fields.
int customWeaponAddChargeState(void);
int customWeaponRemoveChargeState(void);

// Copy the level currently being edited into all 11 levels (flatten), or restore
// the built-in default — including the weapon-wide identity — to all 11 levels.
void customWeaponCopyToAllLevels(void);
void customWeaponResetAllLevels(void);

// Auto-scale: treat the level currently being edited as the "anchor" and generate a power curve
// for the other ten levels of the current fire mode -- scaling damage, fire rate and bullet count
// down for levels below the anchor and up for levels above it. The anchor level is left exactly as
// designed, as is every field that defines the weapon's look and feel (sprite, motion, sound, ...).
void customWeaponAutoScaleLevels(void);

// Persistence helpers (used by config.c). One (mode, level) design serializes to a
// compact, whitespace-free comma-separated list of integers (safe for the parser).
void customWeaponSerializeLevel(int mode, int level, char *buf, size_t bufSize);
void customWeaponDeserializeLevel(int mode, int level, const char *str);

// ---- weapon library (save many designs) -------------------------------------
// The editable globals above are the *working copy* of one library slot; the library
// keeps a collection of complete weapons, persisted to its own file (custom_weapons.cfg
// in the user directory). Only one weapon is equipped/materialized at a time (the engine
// reserves a single custom port + sidekick slot).
#define CUSTOM_WEAPON_LIB_MAX 32

typedef struct
{
	char name[31];
	int  cost, powerUse, equipSlot, itemGraphic, chargeStages, modes;
	int  sidekickMount, sidekickSprite, sidekickFrames, sidekickFrameStep, sidekickAnimate;
	JE_WeaponType raw[CUSTOM_WEAPON_MODES][CUSTOM_POWER_LEVELS];
} CustomWeaponSlot;

extern CustomWeaponSlot customWeaponLib[CUSTOM_WEAPON_LIB_MAX];
extern int customWeaponLibCount;    // number of saved weapons (always >= 1)
extern int customWeaponCurrentSlot; // which slot the working copy came from

// Switch which library weapon is being edited: stashes the working copy back to its slot,
// then loads the chosen one into the globals and materializes it.
void customWeaponSelectSlot(int slot);

// Add a fresh default weapon (New) / a copy of the current one (Duplicate) as a new slot and
// switch to it; returns the new slot, or -1 if the library is full. Delete drops the current
// slot (at least one is always kept) and returns the slot now selected, or -1 if only one left.
int  customWeaponLibraryNew(void);
int  customWeaponLibraryDuplicate(void);
int  customWeaponLibraryDelete(void);

// Load / save the whole library file. Load is called once from customWeaponInit (seeds a
// single slot from the working copy when no file exists yet, migrating an old single weapon).
void customWeaponLibraryLoad(void);
void customWeaponLibrarySave(void);

#endif // CUSTOM_WEAPON_H
