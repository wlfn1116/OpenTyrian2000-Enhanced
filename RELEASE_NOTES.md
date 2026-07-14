# OpenTyrian2000 Enhanced — v1.0

v1.0 is the fork's first release. It builds on OpenTyrian2000 with display-rate rendering, a widescreen playfield, a new Endless mode, weapon-building tools, optional MIDI music, and Nintendo Switch and PlayStation Vita ports.

## Rendering and framerate

The game still simulates at its original 35 Hz, but drawing is no longer tied to it. Frames are interpolated between simulation ticks and presented at the display's refresh rate.

- The player ship runs on a variable timestep with its own physics, which puts control latency below a single simulation tick.
- Background parallax, palette fades, and the power gauge are interpolated at the display rate.
- Optional supersampling renders the playfield at up to 8x internally, so slow scrolling no longer steps between whole pixels.
- The frame-feedback filter levels (ice, water, lava) and the Destruct minigame have their own smoothing paths instead of falling back to 35 Hz.

## Widescreen

The playfield is wider than the original 4:3 view, with the HUD anchored along the right edge. Menus and the HUD are laid out against the wider buffer, so text and gauges stay in place across resolutions.

## Endless mode

A new mode that plays the game's real, unmodified levels in a random, cross-episode order and grows harder as you descend. The difficulty comes from scaling enemy statistics with depth and with the modifiers you take on, not from altering the levels themselves.

- Enemies become tougher through higher health, fire rate, projectile speed, and shot damage, and tinted elite and champion variants — each worth a cash bounty — begin to appear.
- Between levels you dock at an outpost with a shop, banked interest on unspent cash, hull upgrades, stock rerolls, and an extended menu of single-run buffs, gambles, revives, and spare bombs.
- At each stop you chart a branching course; each sector option carries its own risk-and-reward modifiers and a danger rank from F up to S+++.
- You choose a perk after your first cleared zone, then every third zone after that; extra perks can also be bought or won on a lucky gamble. Perks stack and last for the whole run.
- Runs are seeded: the same seed and the same choices reproduce the same sequence of levels, courses, shops, and perk offers.
- A Hardcore option disables saving entirely, so dying or quitting ends the run for good. Otherwise progress is saved and resumed from outpost checkpoints.
- A run ends when you die, and the zone you reach is your score.

## Weapons

- A weapon designer for building a fully custom weapon — eleven power levels, mountable as a front gun, rear gun, or sidekick — with a live preview.
- The per-episode weapon differences from the original games (Episode 1–3 versus 4 and 5, including the spark-trail variants) are reproduced rather than flattened to a single set.
- Extra submenus for tweaking specific weapons.

## Music

- Optional MIDI playback of the original songs through FluidSynth or the operating system's synthesizer. This requires a SoundFont; the OPL emulator remains the default.
- Songs loop at their internal loop point instead of restarting the file, and the end-of-level jingle no longer repeats.

## Interface

- Reworked boss health bars with configurable style and layout.
- Small per-enemy health bars.
- Extra menus for the jukebox, the Destruct minigame, SuperTyrian, and the secret Super Arcade ships.
- An optional Debug Mode, enabled from the Enhancements settings, that adds level-select and diagnostic entries to the menus.

## Ports

- Nintendo Switch, as a homebrew `.nro`.
- PlayStation Vita, as a `.vpk`.

## Stability

- A Windows crash handler that writes a stack trace and a full game-state dump to a log file (`opentyrian_log.log`), along with a watchdog that captures the same information if the game stops responding.
- A fix for a soft-lock that could occur when a boss was destroyed faster than the level script expected.

## Notes

The variable-timestep ship movement is single-player only and is disabled automatically for demo recording, playback, and network games so that those remain deterministic. MIDI support on Windows is limited to 64-bit builds.
