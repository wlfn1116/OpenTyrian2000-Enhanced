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

	// -- doubles --
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY,       "Onslaught" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT,        "Juggernaut" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING,  "Siege" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,           "Barrage" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_DEVASTATING,     "Fusillade" },
	{ ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,      "Piercing Storm" },
	{ ENDLESS_MOD_ENRAGE | ENDLESS_MOD_FORTIFIED,       "War of Attrition" },
	{ ENDLESS_MOD_ENRAGE | ENDLESS_MOD_SWIFT,           "Escalation" },
	{ ENDLESS_MOD_ENRAGE | ENDLESS_MOD_DEVASTATING,     "Wrath" },
	{ ENDLESS_MOD_GRAVITY | ENDLESS_MOD_FORTIFIED,      "Dense Matter" },
	{ ENDLESS_MOD_GRAVITY | ENDLESS_MOD_SWIFT,          "Event Horizon" },
	{ ENDLESS_MOD_GRAVITY | ENDLESS_MOD_DEVASTATING,    "Crushing Weight" },
	{ ENDLESS_MOD_GRAVITY | ENDLESS_MOD_FRENZY,         "Whirlpool" },
	{ ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_FORTIFIED,    "Praetorians" },
	{ ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_SWIFT,        "Vanguard" },
	{ ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_DEVASTATING,  "Elite Guard" },
	{ ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_FRENZY,       "Warband" },
	{ ENDLESS_MOD_OVERCLOCK | ENDLESS_MOD_FORTIFIED,    "Meltdown" },
	{ ENDLESS_MOD_OVERCLOCK | ENDLESS_MOD_DEVASTATING,  "Reactor Breach" },
	{ ENDLESS_MOD_OVERCLOCK | ENDLESS_MOD_GRAVITY,      "Riptide" },  // not the Overdrive buff -- renamed to avoid the clash with the OVERDRIVE boon below
	{ ENDLESS_MOD_OVERCLOCK | ENDLESS_MOD_ELITEPACK,    "Prototype Swarm" },
	{ ENDLESS_MOD_SLIPSTREAM | ENDLESS_MOD_FRENZY,      "Fast Lane" },
	{ ENDLESS_MOD_SLIPSTREAM | ENDLESS_MOD_DEVASTATING, "Runaway" },
	{ ENDLESS_MOD_SLIPSTREAM | ENDLESS_MOD_FORTIFIED,   "Bypass" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_ENRAGE,          "Bloodrage" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_OVERCLOCK,       "Redline" },
	{ ENDLESS_MOD_SWIFT | ENDLESS_MOD_OVERCLOCK,        "Hypervelocity" },
	{ ENDLESS_MOD_ENRAGE | ENDLESS_MOD_GRAVITY,         "Accretion" },
	{ ENDLESS_MOD_ENRAGE | ENDLESS_MOD_ELITEPACK,       "Elite Fury" },
	{ ENDLESS_MOD_ENRAGE | ENDLESS_MOD_OVERCLOCK,       "Overburn" },
	{ ENDLESS_MOD_GRAVITY | ENDLESS_MOD_ELITEPACK,      "Neutron Star" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_DEVASTATING,    "Glass Cannon" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_FORTIFIED,		"Capsize" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_FRENZY,			"Vertigo" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_SWIFT,			"Corkscrew" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_DEVASTATING,		"Upheaval" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_ENRAGE,			"Whirligig" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_ELITEPACK,		"Off Kilter" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_OVERCLOCK,		"Barrel Roll" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_GRAVITY,			"Somersault" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_SLUGGISH,			"Head Rush" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_FORTIFIED,		"Anchored" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_FRENZY,		"Slog" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_SWIFT,			"Bogged Down" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_DEVASTATING,	"Lead Boots" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_ENRAGE,		"War of Attrition II" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_ELITEPACK,		"Ballast" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_OVERCLOCK,		"Millstone" },
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

	// -- triples --
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
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,          "Tumbler" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING, "Keelhaul" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_DEVASTATING,   "Free Fall" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_FRENZY,       "Disorient" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,       "Bullet Wade" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING, "Deadlock" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_SWIFT | ENDLESS_MOD_ELITEPACK,    "Molasses Run" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,        "Kill Box" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING, "Last Ditch" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_SWIFT,     "No Quarter" },

	// -- quads --
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
	//    several levers keeping the stronger NOELITE the rarer sight. Never pair NOCHAMP with NOELITE
	//    (endlessEnforceEliteRules strips the redundant NOCHAMP if they ever meet). --
	{ ENDLESS_MOD_NOCHAMP | ENDLESS_MOD_OVERCHARGE, "Purge" },
	{ ENDLESS_MOD_NOCHAMP | ENDLESS_MOD_BOUNTY,     "Trophy Room" },
	{ ENDLESS_MOD_NOCHAMP | ENDLESS_MOD_FRAGILE,    "Mop Up" },
	{ ENDLESS_MOD_NOELITE | ENDLESS_MOD_OVERCHARGE, "Marksman" },
	{ ENDLESS_MOD_NOELITE | ENDLESS_MOD_FAVOR,      "Clean Slate" },
	// -- boon triples --
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_OVERCHARGE, "Ascension" },
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
	// -- the bare elite tiers --
	{ ENDLESS_MOD_APEX,   "Apex Swarm" },
	{ ENDLESS_MOD_LEGION, "Legion" },
	// -- pairs: an elite tier plus one danger --
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_FORTIFIED,                          "Apex Titans" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_SWIFT,                              "Apex Hunters" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_DEVASTATING,                        "Annihilation" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_OVERLOAD,                           "Extinction" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_FRENZY, "Alpha Strike" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_ENRAGE, "Omega" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_GRAVITY, "Final Hour" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_ELITEPACK, "Last Stand" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_OVERCLOCK, "Endgame" },
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_FORTIFIED,                        "Iron Legion" },
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_SWIFT,                            "Blitz Legion" },
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_DEVASTATING,                      "Ragnarok" },
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_OVERLOAD,                         "Judgment Day" },
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_FRENZY, "Mass Extinction" },
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_ENRAGE, "The Reaping" },
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_GRAVITY, "Harbinger" },
	// -- triples: an elite tier plus two dangers --
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING, "Apex Siege" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,   "Apex Predator" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY,    "Apex Onslaught" },
	{ ENDLESS_MOD_APEX | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_DEVASTATING, "Event Apex" },
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,     "Final Legion" },
	{ ENDLESS_MOD_LEGION | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING, "Legion Siege" },
	// -- FIVE dangers at once, no elite tier: the Cataclysm pool (endlessRareInjections draws these
	//    with APEX and LEGION forbidden, so they stay the "just everything at once" sectors) --
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_ENRAGE,  "Hell Unleashed" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_GRAVITY, "Void Storm" },
	{ ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING, "Total War" },
	{ ENDLESS_MOD_OVERCLOCK | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING, "Doomsday" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_GRAVITY, "Nemesis" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_ELITEPACK, "Leviathan" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_OVERCLOCK, "Behemoth" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_ELITEPACK, "Titanfall" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_OVERCLOCK, "Wrath of God" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_OVERCLOCK, "Purgatory" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_GRAVITY, "Tartarus" },
	{ ENDLESS_MOD_TOPSY | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,   "Kaleidoscope" },
	{ ENDLESS_MOD_SLUGGISH | ENDLESS_MOD_GRAVITY | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,  "Oubliette" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_ENRAGE, "Naked Siege" },
	{ ENDLESS_MOD_SHIELDLESS | ENDLESS_MOD_TOPSY | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,  "Sensory Overload" },
	{ ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_GRAVITY,   "Doomtide" },
	{ ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING | ENDLESS_MOD_ENRAGE | ENDLESS_MOD_ELITEPACK,    "Deathstorm" },
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
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_DEVASTATING, "Sitting Duck" },
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_FORTIFIED,   "Uphill Battle" },
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_SWIFT,       "Overwhelmed" },
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_GRAVITY,     "Dead Weight" },
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_FRENZY,      "Friendly Fire" },
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_ENRAGE,      "Slow Burn" },
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_ELITEPACK,   "Cornered" },
	{ ENDLESS_MOD_BACKFIRE | ENDLESS_MOD_OVERCLOCK,   "Vapor Lock" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_DEVASTATING,  "Death Rattle" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_FORTIFIED,    "Losing Battle" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_ENRAGE,       "Downward Spiral" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_ELITEPACK,    "Outclassed" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_FRENZY,       "Flatline" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_SWIFT,        "Cinders" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_GRAVITY,      "Tailspin" },
	{ ENDLESS_MOD_BURNOUT | ENDLESS_MOD_OVERCLOCK,    "System Failure" },
	{ ENDLESS_MOD_MISFIRE | ENDLESS_MOD_DEVASTATING,  "Peashooter" },
	{ ENDLESS_MOD_MISFIRE | ENDLESS_MOD_FORTIFIED,    "Stonewall" },
	{ ENDLESS_MOD_MISFIRE | ENDLESS_MOD_SWIFT,        "Outgunned" },
	{ ENDLESS_MOD_MISFIRE | ENDLESS_MOD_FRENZY,       "Popgun" },
	{ ENDLESS_MOD_MISFIRE | ENDLESS_MOD_ENRAGE,       "Fizzle" },
	{ ENDLESS_MOD_MISFIRE | ENDLESS_MOD_GRAVITY,      "Downhill" },
	{ ENDLESS_MOD_MISFIRE | ENDLESS_MOD_ELITEPACK,    "No Contest" },
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
	// -- doubles --
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_FORTIFIED,   "Can Opener" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_FRENZY,      "Return Fire" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_SWIFT,       "Quickdraw" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_DEVASTATING, "Standoff" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_ELITEPACK,   "Giant Slayer" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_GRAVITY,     "Heavy Hitter" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_ENRAGE,      "Beat the Clock" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_SHIELDLESS,  "All In" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_TOPSY,       "Topsy Duel" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_FRENZY,         "Paper Storm" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_SWIFT,          "Glass Darts" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_ELITEPACK,      "Brittle Elites" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_ENRAGE,         "Short Fuse" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_GRAVITY,        "Feather Fall" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_OVERCLOCK,      "Blur" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_SHIELDLESS,     "Trade Blows" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_FORTIFIED,     "War of Patience" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_FRENZY,        "Slow Motion" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_DEVASTATING,   "Read the Room" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_ELITEPACK,     "Matrix" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_GRAVITY,       "Deep Focus" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_ENRAGE,        "Steady Hand" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_SHIELDLESS,    "Cold Read" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_TOPSY,         "Slow Spin" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_FORTIFIED,       "Big Game" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_FRENZY,          "Hot Zone" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_SWIFT,           "High Stakes" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_DEVASTATING,     "Danger Pay" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_ELITEPACK,       "Trophy Hunt" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_GRAVITY,         "Deep Dive" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_ENRAGE,          "Overtime" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_OVERCLOCK,       "Rush Job" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_SHIELDLESS,      "Hazard Bonus" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_FORTIFIED,   "Grind" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_FRENZY,      "Trigger Happy" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_SWIFT,       "Fast Hands" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_ELITEPACK,   "Cull the Herd" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_ENRAGE,      "Second Wind" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_GRAVITY,     "Dig In" },
	{ ENDLESS_MOD_FAVOR | ENDLESS_MOD_FORTIFIED,        "Toll Road" },
	{ ENDLESS_MOD_FAVOR | ENDLESS_MOD_SWIFT,            "Hazard Discount" },
	{ ENDLESS_MOD_FAVOR | ENDLESS_MOD_DEVASTATING,      "Combat Pay" },
	{ ENDLESS_MOD_FAVOR | ENDLESS_MOD_FRENZY,           "Loss Leader" },
	{ ENDLESS_MOD_OVERBLAST | ENDLESS_MOD_FORTIFIED,    "Sledge" },
	{ ENDLESS_MOD_OVERBLAST | ENDLESS_MOD_SWIFT,        "Piercer" },
	{ ENDLESS_MOD_OVERBLAST | ENDLESS_MOD_ELITEPACK,    "Headhunter" },
	{ ENDLESS_MOD_OVERDRIVE | ENDLESS_MOD_FORTIFIED,    "Snowball" },
	{ ENDLESS_MOD_OVERDRIVE | ENDLESS_MOD_ELITEPACK,    "Killstreaker" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_SLIPSTREAM,     "Blitz" },
	{ ENDLESS_MOD_SLIPSTREAM | ENDLESS_MOD_BOUNTY,      "Smash and Grab" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_SLIPSTREAM,    "Time Warp" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_SLIPSTREAM,  "Power Play" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_SLIPSTREAM,  "Payday" },
	{ ENDLESS_MOD_NOCHAMP | ENDLESS_MOD_FORTIFIED,   "Tough Crowd" },
	{ ENDLESS_MOD_NOCHAMP | ENDLESS_MOD_FRENZY,      "Crowd Control" },
	{ ENDLESS_MOD_NOCHAMP | ENDLESS_MOD_SWIFT,       "Skeleton Crew" },
	{ ENDLESS_MOD_NOCHAMP | ENDLESS_MOD_ELITEPACK,   "Demotion" },
	{ ENDLESS_MOD_NOELITE | ENDLESS_MOD_FORTIFIED,   "Grunt Work" },
	{ ENDLESS_MOD_NOELITE | ENDLESS_MOD_DEVASTATING, "Green Troops" },

	// -- triples --
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

	// -- quads --
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,      "Last Word" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING, "Overmatch" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_DEVASTATING,  "Eye of the Storm" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,       "Shattering" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_ENRAGE,            "Powder Keg" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT | ENDLESS_MOD_DEVASTATING,        "Death and Taxes" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,          "Payday Run" },
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_SWIFT,   "Elite Overmatch" },
	{ ENDLESS_MOD_TURBODRIVE | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,      "Feeding Frenzy" },

	// -- rare gambits: TWO boons welded to real danger (bigger upside, bigger risk) --
	{ ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_DILATION | ENDLESS_MOD_FORTIFIED | ENDLESS_MOD_DEVASTATING, "Perfect Storm" },
	{ ENDLESS_MOD_FRAGILE | ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_FRENZY | ENDLESS_MOD_SWIFT,           "Blood Bargain" },
	{ ENDLESS_MOD_BOUNTY | ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_FORTIFIED,     "High Roller" },
	{ ENDLESS_MOD_DILATION | ENDLESS_MOD_OVERCHARGE | ENDLESS_MOD_ELITEPACK | ENDLESS_MOD_DEVASTATING, "Bullet Ballet" },
};

// Every named-theme pool, in LOOKUP ORDER: the first pool holding an exact bitset names it, so
// the more specific pools come first. Adding a pool is one row here -- nothing else to update.
#define THEME_POOL(t) { (t), COUNTOF(t) }
static const struct { const EndlessTheme *tbl; unsigned n; } endlessThemePools[] = {
	THEME_POOL(endlessGravityOmniThemes),
	THEME_POOL(endlessHostileThemes),
	THEME_POOL(endlessMixedThemes),
	THEME_POOL(endlessKamikazeThemes),
	THEME_POOL(endlessHomingThemes),
	THEME_POOL(endlessOverloadThemes),
	THEME_POOL(endlessBoonThemes),
	THEME_POOL(endlessRareThemes),
	THEME_POOL(endlessEvilThemes),
	THEME_POOL(endlessRedlineThemes),
	THEME_POOL(endlessSluggishThemes),
	THEME_POOL(endlessDeadgenThemes),
	THEME_POOL(endlessWarpThemes),
};
#undef THEME_POOL

// Find the theme for an exact effect-bit set (NULL = Calm / no modifiers).
static const EndlessTheme *endlessFindTheme(Uint64 mods)
{
	for (unsigned p = 0; p < COUNTOF(endlessThemePools); ++p)
		for (unsigned i = 0; i < endlessThemePools[p].n; ++i)
			if (endlessThemePools[p].tbl[i].mods == mods)
				return &endlessThemePools[p].tbl[i];
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

// --- Course danger tier ---------------------------------------------------------------------
// A sector's net danger, and the two ways it is shown: the tier WORD on the Chart-a-Course help
// line ("Danger: Severe") and the letter GRADE on the planet monitor. Both come off one score, so
// they can never disagree, and the score shares its reward table with the payout -- so a course
// that reads more dangerous always pays more.
//
// ENDLESS_HOSTILE_MASK / ENDLESS_BOON_MASK are defined earlier (with the generic name pools, which
// classify combos by tone from them); the danger score / tier / rank below reuse the same masks.

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

// THE DANGER LADDER. One table drives both the tier WORD and the letter GRADE, so the pair can
// never disagree -- they used to be two hand-maintained if-chains that had to be kept in step.
// `maxScore` is the inclusive top of each band; the last row catches everything above it (its
// score is never read). These thresholds are the tuning knobs for how a net danger score reads
// on the Chart-a-Course monitor: they are spread so single-danger sectors fan out into distinct
// rungs rather than all landing on "Low".
static const struct { int maxScore; const char *tier; } endlessDangerBands[] = {
	{  9, "Low"        },  // grade E -- every hostile course floors at score 1, so nothing hostile reads F
	{ 13, "Moderate"   },  // grade D
	{ 19, "Tough"      },  // grade C
	{ 26, "High"       },  // grade B
	{ 33, "Severe"     },  // grade A
	{ 39, "Deadly"     },  // grade S
	{ 49, "Extreme"    },  // grade S+
	{ 59, "NIGHTMARE"  },  // grade S++
	{  0, "APOCALYPSE" },  // grade S+++ -- the catch-all; maxScore unused
};

// Which rung of the ladder a hostile score lands on: 0 (mildest) .. COUNTOF-1 (the catch-all).
static unsigned endlessDangerBand(int score)
{
	unsigned b = 0;
	while (b + 1 < COUNTOF(endlessDangerBands) && score > endlessDangerBands[b].maxScore)
		++b;
	return b;
}

// Tier word shown before a course's description: a one-glance risk read off the net danger score.
// Cursed sectors are Traps; no hostile bits is a Boon (Calm with no mods at all).
const char *endlessDangerTier(Uint64 mods)
{
	if (mods & ENDLESS_MOD_THEEND)          return "FINALITY";  // the 100th-zone finale, a rung above APOCALYPSE
	if (mods & ENDLESS_MOD_CURSED)          return "Trap";
	if ((mods & ENDLESS_HOSTILE_MASK) == 0) return (mods == 0) ? "Calm" : "Boon";
	return endlessDangerBands[endlessDangerBand(endlessDangerScore(mods))].tier;
}

// Letter-grade twin of endlessDangerTier, off the same ladder: level 0 (F, no hostile bits at
// all) up to 9 (S+++). The numeric level is the single source of truth for both the letter string
// and the green-to-red tint the monitor draws it in (game_menu.c endlessRankHue[]), so those two
// can never drift either.
int endlessDangerRankLevel(Uint64 mods)
{
	if (mods & ENDLESS_MOD_THEEND) return 10;         // END -- the 100th-zone finale's own grade
	if ((mods & ENDLESS_HOSTILE_MASK) == 0) return 0; // F
	return 1 + (int)endlessDangerBand(endlessDangerScore(mods));  // E .. S+++
}
// Grade 10 ("END") is off the letter scale on purpose -- it belongs to the finale alone. game_menu.c's
// endlessRankHue[] is indexed by the same level, so the two arrays must stay the same length.
static const char *const endlessRankName[11] = { "F", "E", "D", "C", "B", "A", "S", "S+", "S++", "S+++", "END" };
const char *endlessDangerRank(Uint64 mods)
{
	return endlessRankName[endlessDangerRankLevel(mods)];
}
