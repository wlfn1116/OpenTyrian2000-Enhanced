# OpenTyrian2000 Enhanced

OpenTyrian2000 Enhanced is a fork of OpenTyrian2000, which is itself an
open-source port of the DOS game Tyrian. It leaves the original game in place
and adds a widescreen playfield, display-rate motion, a new Endless mode,
weapon-building tools, optional MIDI music, and Nintendo Switch and PlayStation
Vita ports.

Tyrian is a vertical scrolling shooter. Its story mode is set in 20,031, where
you play Trent Hawkins, a fighter pilot hired to fight MicroSol. The game also
has one- and two-player arcade modes and networked multiplayer.

The story mode, arcade modes, and the original levels are unchanged. The
additions below are optional; where noted, they can be turned off.

## What this fork adds

### Widescreen

- The playfield renders at 356x200 (16:9 with square pixels) instead of the
  original 320x200. The HUD keeps its size and stays pinned to the right edge.
- Menus, the shop, and the HUD are still drawn against the original 320-pixel
  layout, centered in the wider image.

### Motion and framerate

- The game simulates at a fixed 35 Hz, as it always has. Drawing is separated
  from the simulation: frames are interpolated between ticks and shown at the
  display's refresh rate.
- The player ship is integrated at the display rate with its own movement
  model, which puts control latency below one simulation tick. This is
  single-player only and is turned off automatically for demo recording, demo
  playback, and network games.
- The playfield can optionally be rendered internally at up to 8x its normal
  resolution, so slow scrolling moves in fractions of a pixel instead of whole
  steps.
- The frame-feedback filter levels (ice, water, lava) and the Destruct minigame
  have their own display-rate smoothing.

### Endless mode

- Plays the game's real, unmodified levels in a random order across episodes,
  and gets harder the deeper you go.
- The added difficulty comes from scaling enemy statistics -- health, fire
  rate, projectile speed, and shot damage -- and from tinted elite and champion
  enemies, not from changing the levels themselves.
- Between levels you dock at an outpost with a shop, and chart a branching
  course whose sectors carry their own modifiers and a danger rank from F to
  S+++.
- You choose a perk after the first cleared zone and every third zone after
  that. Perks last for the whole run.
- Runs are seeded: the same seed and the same choices reproduce the same order
  of levels, courses, shops, and perks.
- A Hardcore option disables saving, so dying or quitting ends the run for good.
  Without it, progress is saved at outpost checkpoints.

### Weapons

- A creator for a fully custom weapon with 11 power levels, usable as a front
  gun, rear gun, or sidekick, with a live firing preview.
- Menus to reproduce the per-episode weapon differences from the original games
  (Episodes 1-3 versus 4 and 5, including the spark-trail variants) instead of
  using a single set, plus submenus for tweaking specific weapons.

### Music

- Optional MIDI playback of the original songs through FluidSynth or the
  operating system's synthesizer. FluidSynth needs a SoundFont (`.sf2`) file to
  produce sound; without one it stays silent. The operating system's
  synthesizer needs no extra file. OPL emulation remains the default, so none of
  this is required.
- Songs loop at their internal loop point rather than restarting the file, and
  the end-of-level jingle no longer repeats.

### Interface

- Reworked boss health bars with configurable style and layout (Setup ->
  Enhancements).
- Optional small health bars on damaged enemies.
- Extra menus for the jukebox, the Destruct minigame, SuperTyrian, and the
  secret Super Arcade ships.
- An optional Debug Mode (Setup -> Enhancements) that adds level select and
  diagnostic entries to the menus.

### Stability

- On Windows, a crash handler writes a stack trace and a dump of the game state
  to `opentyrian_log.log`, with a watchdog that captures the same information if
  the game stops responding.
- A fix for a soft-lock that could happen when a boss was destroyed faster than
  the level script expected.

## Additional necessary files

Like OpenTyrian2000, this requires the Tyrian 2000 data files, which have been
released as freeware:

  https://www.camanis.net/tyrian/tyrian2000.zip

On the PC build, keep the game executable next to the `data` folder.

## Keyboard controls

```
alt-enter      -- toggle full-screen
arrow keys     -- ship movement
space          -- fire weapons
enter          -- toggle rear weapon mode
ctrl/alt       -- fire left/right sidekick
```

Keys are rebindable in the setup menu. Controllers are supported and have their
own configuration menu.

## Network multiplayer

Networked games are started manually from the command line by both players at
the same time:

```
opentyrian2000 --net HOSTNAME --net-player-name NAME --net-player-number NUM
```

HOSTNAME is your opponent's IP address, NUM is either 1 or 2 depending on which
ship you pilot, and NAME is your alias. The game uses UDP port 1333 and UDP hole
punching, so opening ports is usually unnecessary.

Network play is inherited from OpenTyrian2000 and has not been tested. The
display-rate ship control is disabled in network games so both sides stay in
step.

## Consoles

- Nintendo Switch, as a homebrew `.nro` -- see [switch/README.md](switch/README.md).
- PlayStation Vita, as a `.vpk` -- see [vita/README.md](vita/README.md).

Both are unofficial homebrew builds and need a console that can run homebrew.
MIDI music is Windows-64-bit only and is not built for the consoles.

## Building (PC)

The Windows build uses Visual Studio; the project files are in `visualc`.
`rebuild-all.bat` builds the PC, Switch, and Vita targets and collects the
results in `build`. Building the Switch and Vita targets additionally needs
devkitPro and VitaSDK; see the README in each folder.

## More detail

- [RELEASE_NOTES.md](RELEASE_NOTES.md) -- the full list of changes in this release.
- [notes.md](notes.md) -- design notes and known pitfalls for the systems above.

## License

GNU General Public License, version 2 or later, the same as OpenTyrian.

## Links

- OpenTyrian2000 (upstream): https://github.com/KScl/opentyrian2000
- OpenTyrian: https://github.com/opentyrian/opentyrian
