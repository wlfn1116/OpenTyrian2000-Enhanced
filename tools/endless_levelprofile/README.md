# endless_levelprofile generator

Generates [`src/endless_levelprofile.h`](../../src/endless_levelprofile.h) — the per-level
intrinsic-danger table endless mode folds into each course's danger rank, sort order and payout
so the shown danger reflects the shipped **level**, not just its modifiers.

The danger numbers are **derived from the real levels**, not hand-authored: every shipped level is
run through the [Tyrian2000Atlas](https://github.com/wlfn1116/Tyrian2000Atlas) `GameSim` at every
difficulty (0..10) with no player firing back, and its `LevelThreat.Difficulty01` (≈1.0 = an
ordinary campaign level) is mapped to a small `baseDanger` nudge.

## Regenerating

Only needed if the shipped level data changes (it doesn't, for stock Tyrian 2000) or the mapping is
retuned. Requires the Atlas checkout and .NET.

```bash
# 1. Export every level x every difficulty from the game data (writes threat.csv here).
#    Roll-forward is only needed if the net8.0 runtime isn't installed (SDK-only machine).
DOTNET_ROLL_FORWARD=Major dotnet \
  /path/to/Tyrian2000Atlas/bin/Release/net8.0/win-x64/Tyrian2000Atlas.dll \
  --exportthreat threat.csv /path/to/OpenTyrian2000-widescreen/data

# 2. Regenerate src/endless_levelprofile.h from threat.csv.
python gen_profile.py
```

`threat.csv` is committed so step 2 can run without the Atlas. The `--exportthreat` command lives in
the Atlas repo (`Program.cs`).

## Mapping (in `gen_profile.py`)

- `baseDanger = clamp(round((Difficulty01 - 1.0) * 4.0), -2, +5)`, per difficulty. An ordinary level
  reads 0; MINES ≈ −2, TIME WAR ≈ +5. Kept small so the level *nudges* the rank while the sector's
  modifiers still dominate (endless danger scores run ~10–60).
- `lengthClass`: 0 short / 1 normal / 2 long, from measured play length at Normal (boss-loops = long).

## CSV columns

`episode, fileNum, name, bonus, secret, timedBattle, difficulty, difficulty01, ticks,
endedNaturally, shiftsDifficulty, runAtLow, runAtHigh, duration, spawnCount, fireRate, peakFireRate,
trackedShare, bulletDensity, peakBulletDensity, enemyDensity, hulkDensity, saturation, ok`

`difficulty01` is the headline; the rest are the components behind it (kept for transparency / retuning).
