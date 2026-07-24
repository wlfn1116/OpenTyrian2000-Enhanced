#!/usr/bin/env python3
"""Generate src/endless_levelprofile.h from a Tyrian2000Atlas threat export.

Pipeline (see README.md):
  1. Tyrian2000Atlas.exe --exportthreat threat.csv <tyrian-data-dir>
  2. python gen_profile.py            # reads threat.csv here, writes ../../src/endless_levelprofile.h

Every shipped level is run through the atlas's GameSim at every difficulty (0..10); its
LevelThreat.Difficulty01 (~1.0 = an ordinary campaign level) becomes TWO per-difficulty values,
keyed by (episode, lvlFileNum):

  baseDanger  : a small -2..+5 nudge folded into the course's danger GRADE/tier/sort. Coarse on
                purpose -- it rides the letter-grade ladder (F..S+++), which only has ~10 rungs.
  payoutMille : a fine-grained thousandths-of-base cash term folded into the course PAYOUT, so two
                same-grade levels pay different amounts (the grade clumps; the cash should not).
                Decoupled from baseDanger precisely so the payout can vary continuously.
"""
import csv, os, math

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "threat.csv")
OUT = os.path.normpath(os.path.join(HERE, "..", "..", "src", "endless_levelprofile.h"))

ANCHOR = 1.0     # difficulty01 of an "ordinary" campaign level -> 0 on both scales

# GRADE nudge (coarse): rides the danger-score ladder, bands ~9..59, so a few points is plenty.
G_SCALE, G_LO, G_HI = 4.0, -2, 5

# PAYOUT term (fine): thousandths of the base clear reward. An ordinary level adds nothing; the
# easiest pays ~0.4x base less, the hardest ~1.5x base more, in ~continuous steps -> real variety.
P_SCALE, P_LO, P_HI = 1000.0, -400, 1500

def rnd(x): return int(math.floor(x + 0.5))
def clamp(v, lo, hi): return lo if v < lo else hi if v > hi else v
def base_danger(d01):  return clamp(rnd((d01 - ANCHOR) * G_SCALE), G_LO, G_HI)
def payout_mille(d01): return clamp(rnd((d01 - ANCHOR) * P_SCALE), P_LO, P_HI)

rows = list(csv.DictReader(open(CSV, newline='')))
levels = {}   # (ep,file) -> dict
for r in rows:
    ep, f, d = int(r['episode']), int(r['fileNum']), int(r['difficulty'])
    L = levels.setdefault((ep, f), {
        'name': r['name'].strip('"'), 'dur': int(r['duration']),
        'd01': {}, 'ticks': {}, 'ended': {}})
    L['d01'][d] = float(r['difficulty01'])
    L['ticks'][d] = int(r['ticks'])
    L['ended'][d] = r['endedNaturally'] == '1'

def q(xs, p):
    xs = sorted(xs); i = (len(xs) - 1) * p; lo = int(i); hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (i - lo)

# lengthClass thresholds from measured play-length (ticks at Normal) among naturally-ending levels;
# a level that never ends on its own (a boss gate that loops) counts as long.
ended_ticks = [L['ticks'][2] for L in levels.values() if L['ended'].get(2)]
t33, t66 = q(ended_ticks, 1/3), q(ended_ticks, 2/3)
def length_class(L):
    if not L['ended'].get(2): return 2
    t = L['ticks'][2]
    return 0 if t <= t33 else 1 if t <= t66 else 2

def crow(k):
    ep, f = k; L = levels[k]
    bd = ",".join(f"{base_danger(L['d01'][d]):>2d}" for d in range(11))
    pm = ",".join(f"{payout_mille(L['d01'][d]):>5d}" for d in range(11))
    return (f"\t{{ {ep}, {f:>2}, {length_class(L)},\n"
            f"\t  {{ {bd} }},\n"
            f"\t  {{ {pm} }} }},"
            f"  // {L['name'][:10]:<10} d01 N={L['d01'][2]:.2f} I={L['d01'][4]:.2f}")

hdr = '''/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 *
 * Endless mode: GENERATED per-level danger profiles. DO NOT EDIT BY HAND.
 *
 * Produced by tools/endless_levelprofile (Tyrian2000Atlas "--exportthreat" + gen_profile.py):
 * every shipped level run through the atlas's GameSim at every difficulty (0..10), its
 * LevelThreat.Difficulty01 (~1.0 = an ordinary campaign level) mapped to two per-difficulty terms:
 *   baseDanger  = clamp(round((Difficulty01 - %.1f) * %.1f), %d, %d)   -- coarse GRADE/tier/sort nudge
 *   payoutMille = clamp(round((Difficulty01 - %.1f) * %.0f), %d, %d)  -- fine PAYOUT term, thousandths of base
 * Keyed by (episode, lvlFileNum) -- lvlFileNum is endlessCourseFile / forcedLvlFileNum.
 * lengthClass: 0 short, 1 normal, 2 long (measured play length at Normal; boss-loops = long).
 *
 * Requires EndlessLevelProfile (endless_internal.h); include only from endless_mods.c.
 * Regenerate:  Tyrian2000Atlas.exe --exportthreat threat.csv <dataDir> && python gen_profile.py
 */
#pragma once

// { ep, file, lengthClass, baseDanger[difficulty 0..10], payoutMille[difficulty 0..10] }
static const EndlessLevelProfile endlessLevelProfiles[] = {
''' % (ANCHOR, G_SCALE, G_LO, G_HI, ANCHOR, P_SCALE, P_LO, P_HI)

text = hdr + "\n".join(crow(k) for k in sorted(levels)) + "\n};\n"
open(OUT, "w", newline='\n').write(text)
print(f"wrote {OUT}  ({len(levels)} levels)")
print(f"  grade  clamp[{G_LO},{G_HI}] scale {G_SCALE}")
print(f"  payout clamp[{P_LO},{P_HI}] scale {P_SCALE} (thousandths of base)")
# quick spread check at Normal / Impossible
for d in (2, 4, 10):
    pm = sorted(payout_mille(L['d01'][d]) for L in levels.values())
    print(f"  payoutMille @diff{d:>2}: min={pm[0]} p25={q(pm,.25):.0f} med={q(pm,.5):.0f} p75={q(pm,.75):.0f} max={pm[-1]}  distinct={len(set(pm))}")
