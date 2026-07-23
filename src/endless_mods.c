/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 *
 * Endless mode: the mutator table -- named sector themes, danger tiers and help text.
 *
 * One of the endless_*.c files that make up endless mode: endless.h is the public
 * interface, endless_internal.h the state and helpers the group shares.
 */

#include "endless.h"
#include "endless_internal.h"

#include "config.h"        // difficultyLevel, DIFFICULTY_*, player-independent globals
#include "custom_weapon.h" // customWeaponPort / customSidekickSlot (reserved shop slots)
#include "episodes.h"      // item arrays + SHIP_NUM/PORT_NUM/... counts, episodeAvail, JE_initEpisode
#include "fonthand.h"      // JE_outText
#include "joystick.h"      // push_joysticks_as_keyboard
#include "player.h"        // player[]
#include "sprite.h"        // JE_loadCompShapes, enemySpriteSheets, shopSpriteSheet
#include "tyrian2.h"       // itemAvail, itemAvailMax
#include "varz.h"          // eventRec, maxEvent, map* globals

#include <stdio.h>
#include <string.h>

const EndlessMod endlessModTable[] = {
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

const EndlessTheme endlessHostileThemes[] = {
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
const EndlessTheme endlessKamikazeThemes[] = {
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
const EndlessTheme endlessHomingThemes[] = {
	{ ENDLESS_MOD_HOMING,                              "Stalkers" },
	{ ENDLESS_MOD_HOMING | ENDLESS_MOD_FORTIFIED,      "Bloodhounds" },
	{ ENDLESS_MOD_HOMING | ENDLESS_MOD_FRENZY,         "Harriers" },
	{ ENDLESS_MOD_HOMING | ENDLESS_MOD_SWIFT,          "Pursuers" },
	{ ENDLESS_MOD_HOMING | ENDLESS_MOD_DEVASTATING,    "Predators" },
	{ ENDLESS_MOD_HOMING | ENDLESS_MOD_GRAVITY,        "Undertow" },
	{ ENDLESS_MOD_HOMING | ENDLESS_MOD_ELITEPACK,      "Wolfpack" },
	{ ENDLESS_MOD_HOMING | ENDLESS_MOD_ENRAGE,         "Bloodlust" },
};

const EndlessTheme endlessBoonThemes[] = {
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
const EndlessTheme endlessOverloadThemes[] = {
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
const EndlessTheme endlessRareThemes[] = {
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

Uint64 endlessMakeTheEndMods(void)
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
const EndlessTheme endlessEvilThemes[] = {
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
const EndlessTheme endlessRedlineThemes[] = {
	{ ENDLESS_MOD_OVERHEAT | ENDLESS_MOD_TURBODRIVE,                      "Reactor Redline" },
	{ ENDLESS_MOD_OVERHEAT | ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_FRENZY, "Redline Frenzy" },
};

// SLUGGISH + GRAVITY: the "heavy, inescapable" nightmares -- the ship crawls WHILE dragged down.
// Survivable by design (endlessGravityDrift slows the pull in lock-step with the ship, so full
// throttle still climbs), but brutal -- its own tiny pool injected RARELY (like Kamikaze / Overload)
// rather than shuffled into the rotation. The bare pairing is the headline "Tar Pit".
const EndlessTheme endlessSluggishThemes[] = {
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_GRAVITY,                           "Tar Pit" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_DEVASTATING, "Quicksand" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_FRENZY,      "Quagmire" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_SWIFT,       "Sinkhole" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_FORTIFIED,   "Abyss" },
};

// DEADGEN (dead generator): shields never refill AND the main gun is starved to a sputter -- a super-
// rare, evil sabotage sector with its own pool (injected ~1/55; never in the combinable pool or the
// shuffle). Rear guns / sidekicks / specials still work, so it's brutal, not unwinnable.
const EndlessTheme endlessDeadgenThemes[] = {
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
Uint64 endlessPickThemeMods(const EndlessTheme *tbl, unsigned count, Uint64 must, Uint64 forbid)
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
const char *endlessComboNameSalted(Uint64 mods, unsigned salt)
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
int endlessDangerScore(Uint64 mods)
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
const char *endlessDangerTier(Uint64 mods)
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
int endlessDangerRankLevel(Uint64 mods)
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
const char *endlessDangerRank(Uint64 mods)
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
