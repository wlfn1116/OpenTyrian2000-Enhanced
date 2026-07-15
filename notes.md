# OpenTyrian2000 Engaged developer notes

Design notes and known pitfalls for the systems this fork adds on top of upstream
OpenTyrian2000. The source keeps short comments at the relevant sites; the full
reasoning lives here. Code comments reference these sections as `notes.md §Name`.

## Build & targets

- `build-all.ps1` is the build entry point. It builds PC (MSVC), Switch
  (devkitA64 `.nro` via `switch/build.sh`) and Vita (VitaSDK `.vpk` via
  `vita/build.ps1`), then collects successful
  deliverables into `build\`. Targets can be selected individually, `-Clean`
  performs a clean rebuild, and one target failing does not stop the others
  unless `-FailFast` is used.
- Collected filenames use the semantic version from `src/opentyrian_version.h`
  and a platform suffix (`Win64`, `Win32`, `Switch`, or `Vita`). The source
  outputs retain the names expected by their toolchains.
- The PC `.exe` must live next to `data\` to run. `build\` is a collection of
  deliverables, not a runtime layout.
- Switch: `DEVKITPRO` must be in msys form (`/opt/devkitpro`); the build entry
  point neutralizes an inherited Windows-style value before invoking devkitPro's
  bash. Batch files need CRLF line endings.
- The Vita build runs from PowerShell (native cmake + ninja) rather than MSYS
  bash. The native tools need native `D:\...` paths (MSYS mangles them into
  `/d/...`), and the shell-hostile data filenames (`shapes).dat`, `newsh%.shp`)
  break per-file argument handling in `vita-pack-vpk` under a POSIX shell.
- `WITH_MIDI` is x64-only on Windows (the vendored midiproc and FluidSynth libs
  are x64), so Win32 builds compile without it.
- The crash logger (`crashlog.c`) is Windows-only; console ports get the stub paths.

## Smooth motion (render list)

The world simulates at the fixed 35Hz tick. Every draw is recorded into a render
list (`render_list.c`) and replayed at the display rate with positions
interpolated between the previous and current tick. Key invariants:

- Anything that moves in a menu or sim needs an `rl_current_id` tag, or it steps
  at the recording tick rate instead of interpolating. This bit the shop's weapon
  preview twice.
- Present timing uses the performance counter, not `SDL_GetTicks`. Ms-quantized
  elapsed time injects several percent of alpha error per frame at high refresh
  rates, which shows up as micro-judder.
- Shots and attachments: shot ids (`rl_id_extrapolates`) extrapolate forward at
  the render rate instead of interpolating a tick behind. They lead by velocity
  plus acceleration (the predicted next displacement), so a decelerating shot
  lands on its next-tick position instead of overshooting and snapping back.
  They are immune to slot recycling because `dx`/`dy` hold the shot's own
  recorded velocity, not a prev/cur diff. Ship-attached shots (laser, main pulse)
  follow the ship via `ship_attach`. An attached shot that also moves on its own
  (the orbiting asteroid-killer, weapon 104) records ship-move + own-move and
  subtracts the ship velocity to recover the interpolatable own-move part.
- The exact/residual replay (`rl_replay`, `use_override` off) must reproduce the
  recorded frame byte-exact; all sub-pixel smoothing is gated on `use_override`.

### Variable-timestep (VT) player ship — `tyrian2.c`

The player ship alone is simulated at the render rate with real dt while the
world stays at 35Hz. The integrator owns the ship: `JE_playerMovement`'s
movement/velocity integration is skipped (guarded by `vt`), player.x/y are
written each frame, and the sprite is driven through the render-list ship
override. The 35Hz sim still reads player.x/y for firing and collisions, so
sprite and hitbox move together and input latency drops below a tick.

- Single-player only. Determinism is broken by design, so VT is force-disabled
  for demo record/playback and network games.
- Control feel is a fresh accel/friction model; the original
  position-as-acceleration model can't be dt-scaled. `VT_DIRECT` exists because a
  pure momentum model feels laggy on a controller (mouse hides it by direct
  positioning).
- The integrator must step on every present-loop iteration, not only rendered
  frames. Skipping the iteration that triggers a sim tick discards that
  iteration's elapsed time, and the ship visibly stutters even at a solid 60fps.
- The VT ship's render-rate `poll_joystick` consumes press-edges, so menu/pause
  handling must get its edge data from `vt_ship_step`'s capture or controller
  menus break (this happened — see git history).

### Smoothie levels (ice/water/lava filters) — `tyrian2.c`, `render_list.c`

The per-pixel filters are frame-feedback effects, so smooth presentation uses two
passes and two buffers:

- `render_gs` — persistent background plasma. Advances one filter step per tick
  (alpha=1, backgrounds at tick positions), evolved per displayed frame at
  interpolated positions. Never holds entities; they would feed back and smear.
- `smoothie_frame` — per-frame display buffer: fresh copy of `render_gs` plus
  interpolated entities plus residual overlays.
- The BG pass applies the filter once, at full strength, with feedback on. Full
  strength matters: a partial blend leaves the background perpetually dim, and an
  accumulating buffer over-advances into runaway feedback. The filters are
  contractive toward the freshly-drawn background, so the plasma stays bounded
  without re-anchoring, and a sub-pixel-shifted background is safe.
- The residual delta diffs game_screen against the pre-overlay snapshot
  (VGAScreen2). Any full-screen grade — a colour flare (`levelFilter != -99`) or
  a brightness-only flash (`levelBrightness != -99`) — must be applied to the
  snapshot too, or it bakes into the residual and freezes the playfield at 35fps
  for its duration. `JE_filterScreenApply` self-skips `-99` components, so steady
  state is a no-op.

### Sub-pixel parallax & scroll tracking — `backgrnd.c`, `render_list.c`, `tyrian2.c`

- Horizontal: each layer's un-floored float offsets are captured at the draw site
  (`bg_layer_dx`/`bg_layer_frac`). Rows are replayed at recorded x plus
  `(frac - dx*inv)`; float endpoints make the pan continuous across tick
  boundaries while still tracking the ticks enemies anchor to.
- Vertical: the same idea via `bg_layer_dy`/`yfrac`. An integer per-tick scroll
  can't express a fractional rate (3.2 → 3,3,3,4 velocity pulse that looks like
  speeding up and slowing down); the float rate makes the displayed velocity
  constant. The published rate lags one tick, matching how `bgScrollDeltaY` is a
  draw-time position diff. Applies to every level — see §Slow-scroll smoothing.
- Scroll-tracked enemies must fold the integer scroll and the sub-pixel fraction
  into one rounded displacement. Rounding them separately loses the fraction at
  scale 1 (it rounds to 0) and the enemy jitters against the smooth background
  whenever supersampling is off.
- Enemy sprite and HP bar use different fracs: the sprite is drawn before the
  `ey` scroll advance (lagged frac), the bar after it (this-tick frac). Feeding
  either the wrong one makes it jump 1px on the ticks the integer scroll steps an
  extra pixel.

### Slow-scroll smoothing — `endless.c`, `tyrian2.c`, `render_list.c`

Levels slow the vertical scroll with the delay gate (`map*YDelayMax`, event 3:
layer 1 = 1px every 3 ticks, layer 2 = 1px every 2 ticks). The average rate is
fractional but the per-tick scroll is integer (0,0,1), so the layer freezes on
the off-ticks and jumps a whole pixel on the fire tick — choppy even with
supersampling on. TYRIAN (ep1) shows it clearly across its layers.

The fix drives the render list's float vertical rate (`bg_layer_dy`/`yfrac`) off
the layer's true average rate on every level, so the display slides at a constant
sub-pixel velocity. `endlessScrollExtraPx` computes it: `target =
fireStep/delayMax` (px*100), `frac = Σ(target − px the base actually moved)`.
Without a scroll modifier it emits no extra scroll px, only the display rate/frac,
so the sim scroll (and demos, collision, events) stays byte-identical to stock.
`bg_smooth_y_active` is on throughout gameplay.

- `fireStep` is the per-fire step, not the live `backMove` (the delay gate has
  already forced that to 0 on off-ticks): `(delayMax>1 && backMove<2) ? 1 :
  backMove`. Feeding the forced value makes `Σ(target − base)` drift.
- Byte-exact no-op on full-speed layers: an integer rate gives `frac == 0`, and
  `rl_iround` is symmetric (`round(−x) == −round(x)`), so the float path equals
  the old integer path exactly — no regression, no new 1x jitter. At 1x a
  fractional layer still steps 1px per N ticks, but now at the correct sub-pixel
  phase and in lockstep with its scroll-tracked enemies (they read the same
  `bg_layer_yfrac`), instead of all freezing then jumping together.
- Only layers 1/2 gate (layer 3 always scrolls `backMove3`/tick); enemies
  scroll-track layers 1/3 only, so they inherit the glue automatically.

### Endless scroll boost — `endless.c` (`endlessScrollExtraPx`)

Speeding the scroll by adding whole `baseMove` lumps pulses the velocity, and on
delay-gated layers (`map*YDelayMax`, backMove forced 0 on off-ticks) the boost
itself froze and lurched. `endlessScrollExtraPx` instead returns per-layer extra
pixels so base + extra tracks a constant target rate (avg base rate ×
(1 + boost/100), minus what the base actually scrolled this tick). A signed
per-channel carry keeps the long-run average exact and never emits negative px,
so lockstep with `curLoc`/events/enemy scroll-tracking is preserved (no drift, no
boss cut-off). Call it once per channel per tick; channels 0/1/2 = background
layers 1/2/3. `rateOut`/`fracOut` expose the smooth float rate and sub-pixel
remainder for the render-list vertical interpolation. With no modifier the same
call runs at boost 0, returning 0 extra px and only the base-rate rate/frac
(§Slow-scroll smoothing).

### Other render-rate presents

- Palette fades (`palette.c smooth_fade_to`): recomputed per presented frame from
  real elapsed time. The classic incremental step is exactly linear, so a
  straight lerp reproduces it with the same duration and end palette.
- Destruct minigame (`destruct.c`): self-contained smooth and supersampled
  present. Static terrain is expanded once per tick into `destruct_bg_hi`; each
  frame copies it and draws interpolated units/shots/crosshairs plus HUD on top.
  Needs both Smooth Motion and Sub-pixel enabled.
- Power/shield HUD gauges smooth at render rate with an AA edge shade.
- Soul of Zinglon's light pillar records its request per tick (`zinglonPillar*`)
  and draws at display rate; interpolated frames shift its centre by the ship
  override so the pillar glides with the ship.
- Picture wipes (U/V/R): the classic path advances the wipe boundary one
  row/column per tick; the smooth path advances it by real elapsed time and
  presents every frame. Same total duration and end image, and a keypress skips
  to the end.

## Supersampling & video — `video.h`, `video_scale.c`

- The interpolated playfield can render into an NxN buffer so motion lands on
  1/N-pixel positions (slow scrolls glide instead of stepping). `0 = Auto`
  matches the video scaler's factor (Native's fractional ratio rounds up so the
  buffer covers every screen pixel); `1 = off`; 2..8 fixed.
- Fit modes when output is larger than the hi buffer: `Sharp` (nearest, classic),
  `Smooth` (sharp-bilinear: integer prescale + linear fit), `None` (point-sample,
  drops the supersampled detail entirely). Downscaling always blends linearly in
  Sharp/Smooth; nearest minification shimmers.
- Enum values persist in the config file: keep `Sharp=0/Smooth=1` and only append
  new modes.

## Widescreen

- The playfield sits at `game_screen[PLAYFIELD_LEFT ..+PLAYFIELD_WIDTH)`;
  `composite_playfield()` crops it to the final buffer's x=0. `PLAYFIELD_LEFT`
  must equal that crop offset, and is deliberately not derived from
  `PLAYFIELD_X_SHIFT` (the background tile phase, an unrelated quantity that only
  happened to be `-PLAYFIELD_LEFT/2`).
- Menus render on a 320px virtual screen centered in the wider buffer. Center
  text on `LEGACY_WIDTH/2`, not `vga_width/2`, or labels drift when blitted.
- HUD overlay draws use composited-buffer coordinates (HUD starts at x=299 at
  320-width equivalents); playfield draws use playfield coordinates. Mixing the
  two caused double-offset bugs (debug panel, xmas snow).
- A surface's pitch is not its logical width: code that steps rows by a width
  constant must use the right one. A "fix" here once squished the ship-specs
  zoom; it was correct as shipped.
- Destruct HUD backdrop: the top 12 rows of pic #11 are two identical box frames
  authored for 320px (x=2 and x=172). Pin each frame flush to its screen edge and
  black out the widened middle; the readout boxes anchor 1px inside the same
  constants, so frame and box stay locked.

## Endless mode — `endless.c`, `endless.h`

### Seeded structure RNG

Endless draws its structure (level order, course mutators, perk offers, shop
stock) from a dedicated SplitMix64 stream re-derived per zone from
`hash(seed, depth)`, separate from the shared gameplay RNG (`mt_rand`) so in-level
combat draws and player-timed gamble/reroll draws can never desync the cross-zone
structure. Elite/champion tier rolls get their own per-zone sub-stream. Same seed
and same choices reproduce the same run against the same build; adding or removing
a structural draw changes what a seed means. Moment-to-moment combat randomness is
deliberately unseeded, so the elite roll sequence is seed-fixed but which enemy
each roll lands on can shift.

Per-level music is deterministic per (seed, depth): the anti-repeat compares
against the previous zone's song recomputed from that zone's own stream, not a
mutable `last`, so a Quit-Level retry replays the same track.

### Difficulty ramp

- Enemy levers are driven by an effective depth: real depth × 1.25, tilted by the
  50..160 difficulty factor (`endlessDifficultyRampPercent`). Real
  `endlessRunDepth` still drives HUD/score/milestones/economy.
- Each lever has its own slope so caps mature one at a time (NORMAL: elite share
  ~37, shot damage ~55, shot speed ~67, fire rate ~80, boss HP ~96, ordinary HP
  ~100).
- Rising tide: intensity levers saturate by effective zone ~100–125 (armor byte
  cap, fire pinned at one tick). The tide adds the one axis with no engine
  ceiling — extra enemy shots per volley and a rising elite/champion share — from
  a single coefficient, starting at zone 35 so it never piles onto the intensity
  ramp. Extra volley shots use `endlessRunDepth` directly, not the shared tide
  level.
- Gravity (Gravity Well course): base plus per-zone ramp, tilted by the same
  difficulty factor, with an absolute cap that stays clear of the ship's top
  speed (`VT_VMAX`) so full throttle can always climb. The VT integrator scales
  it by dt; the classic tick path applies the rounded per-tick amount.

### Course generation & danger labels

- Kill-fire mods: three boons (Turbodrive/Overdrive/Overblast) and three evil
  mirrors (Backfire/Burnout/Misfire) share the combo/stack machinery, so a sector
  carries at most one; sub-masks (`FIREBOOST/FIREJAM/DMGUP/DMGDOWN/STACKED`) say
  which effect each grants. The evil three are also forced gamble outcomes
  (`EGO_CURSE_*`), independent of course injection.
- The generator gauge recolours while a kill-fire boon window is up (main-gun
  fire costs no power then). The value is a palette bank base; the whole 14-shade
  ramp must stay inside one bank (`draw_power_gauge` derives the AA dark end from
  the bank floor).
- `endlessDangerTier` (word) and `endlessDangerRank` (letter F..S+++) band the
  same net danger score; keep their thresholds in lockstep so the pair never
  disagree. Tier thresholds: ≤9 Low, ≤13 Moderate, ≤19 Tough, ≤26 High, ≤33
  Severe, ≤39 Deadly, ≤49 Extreme, ≤59 NIGHTMARE, >59 APOCALYPSE.
- Rare whole-visit flavors (Jackpot ~1/25: all boons; Ambush ~1/20: one forced
  dangerous sector; Gauntlet ~1/7: all hostile). All three dice are rolled up
  front unconditionally so the seed stream stays aligned; precedence Jackpot >
  Ambush > Gauntlet; none fire at depth 0.
- Sector variety is all combinations of the existing `endlessModTable` bits, so
  it needs no new bits/save bump: the danger score, monitor rows, tier/rank and
  payout are all mod-agnostic. Sources, in generation order: distinct named
  hostile themes; a ~50% "widen" that swaps in a random 1-5 bit combo of the
  `combinable[]` hostiles (weights lean toward 2-4 bits); a ~1/3 boon course
  (60% a named boon theme, 40% an emergent 2-3 bit boon combo from
  `endlessMakeBoonCombo`); and MIXED "gambit" sectors — after the boon roll,
  ~35% of each ORDINARY hostile course gains one compatible boon
  (`endlessPickMixBoon`), welding reward onto danger. Compatibility avoids
  same-lever cancels (no frail+fortified, no dilation+swift/overclock) and keeps
  the one-kill-fire rule (only one boon added). Un-named combos read with the
  right tone via three generic-name pools (ominous / fortunate / gambit), chosen
  in `endlessComboName` off `ENDLESS_HOSTILE_MASK` vs `ENDLESS_BOON_MASK`.
- `endlessMixedThemes[]` supplies cosmetic names for the common gambit combos
  (pairs→quads + a few double-boon rares); `FRAGILE|DEVASTATING` is intentionally
  absent (it's the hostile table's "Glass Cannon"). The single-danger guarantee
  (≥4 courses) skips any course carrying a boon bit, so a gambit is never
  flattened into a plain single.
- Safe-special filter: a pickup special needs a non-empty name, a
  dispatcher-handled effect type (stype 1..18), and an in-range `itemgraphic`.
  The HUD redraws the equipped icon every frame, and an out-of-range icon reads
  past the sprite offset table and crashes instantly (several arcade-internal
  specials carry a bad icon).
- The outpost gamble is a neutral-EV, high-variance ladder: a weighted 0..99
  table plus shared ~1/5000 ultra-rare draws (mega jackpot / Kamikaze rush / free
  perk). Every outcome reuses an existing lever and is applied through
  `endlessApplyGambleOutcome`, so the random path and the debug screen's forced
  outcomes can't drift apart.

### Economy & perk plumbing

- Shop prices inflate +19% per cleared depth (cap 100x); the first 5 zones ramp
  at half slope (+9.5%), then the full slope resumes at zone 6, leaving a ~0.4x
  discount across the rest of the run. `endlessRunDepth` is constant within a
  visit, so every `JE_getCost` call scales consistently.
- Rapid Recharge speeds the special-weapon cooldown and sidekick ammo-refill
  counters (not the main guns). Its decrement accumulator is stateful: sample it
  once per tick in the main-player block, then reuse the sampled value.

### Save / resume

- The per-slot `JE_saveGame` (tyrian.sav) is a fixed, checksummed layout with no
  room to extend, so the run lives in a sidecar (`endless.sav`) keyed by the same
  slot: run-persistent state (depth, hull, kills, perks, tokens, gamble debts,
  superbombs) plus the outpost snapshot (this visit's courses, shop stock, prices,
  perk offer, gamble line, pending buys). Restoring the snapshot instead of
  regenerating stops save/reload from rerolling the shop for free and keeps buffs
  already paid for.
- Auto-checkpoint into the "LAST LEVEL" continue slot happens at outpost entry
  (the one coherent resume point). Hardcore suppresses all saving, so dying or
  quitting ends the run with no reload.
- Quit Level in endless reverts the level to its launch state and reopens the
  outpost. Hardcore relocks it (retry same level or quit run; no farm-then-bail);
  non-hardcore unlocks it (re-outfit freely, still no mid-zone farming).
- Reroll runs neither the entry merchant-sort nor the equipped auto-adds, and a
  purchase doesn't sync `last_items`, so shop-stock code must seed from
  `player[0].items`, not stale caches.

## Boss & enemy health bars — `tyrian2.c`, `varz.c`

- Enhanced boss bars: a recessed track plus glossy gradient fill kept inside one
  palette bank (bank 7 = 112..127 normally; elite/champion tints in endless), so
  they read on any level palette. Layout (Top/Bottom/Left/Right; one- or two-bar
  Split/Together/Stacked) comes from the Enhancements menu; geometry must clear
  the corner HUD indicators.
- Bars draw into `game_screen` (playfield space), not composited-buffer space —
  see §Widescreen. `BOSS_BAR_THICK`/`GAP` are shared between the layout code and
  the HUD-shift helper so they can't drift.
- Enhanced boss-bar hit flash fades smoothly at render rate (like the shield/armor
  gauge flash). The bars are captured in the playfield residual, so they'd otherwise
  step at the 35 Hz tick; `draw_boss_bar_present` redraws them every displayed frame
  in `JE_starShowVGA`'s present loop at an interpolated flash (`boss_flash_render`,
  same `color + 1 - alpha` trick), overdrawing the residual bars. `draw_boss_bar_gauge`
  and `draw_boss_bars_enhanced` gained a `(dst, scale)` so the per-frame redraw lands
  on the residual's exact pixels: `bbfill` scales a 1x rect to a `scale`×`scale` block
  identically to the residual re-apply (`x0*scale .. (x1+1)*scale-1`), and the frame
  is now two filled rects instead of an outline+track (byte-identical at scale 1).
  The once-per-tick call passes `decrement = true`; the per-frame call must not, or
  the flash would race down at the display rate. Classic bars keep the per-tick flash.
- Per-enemy bars: one bar per linknum group, spanning the group's on-screen
  bounding box and showing its most-damaged part; only live and damageable slots
  count (armorleft 1..254, `healthbar_seen` latched); boss-linked groups are
  skipped. Bars are recorded (`RC_HP_BAR`) so they interpolate with their enemy.
- Endless reinforced hulls above the 28-unit armour bar draw as stacked rollover
  layers, each 28-unit chunk in its own palette-relative gradient (tuned by eye).
- Shield/armor gauges flash white when depleted by damage (`Gauge Gradients` menu
  toggles `gaugeFlashShield`/`gaugeFlashArmor`, on by default, each independent).
  `JE_playerDamage` starts a per-player countdown (`shieldGaugeFlash`/
  `armorGaugeFlash`) only when the value drops — never on shield regen or armour
  pickups. `JE_updateGaugeFlash` (in `JE_main`) decays it one step per tick.
- The fade is a white pop then a smooth in-family return: the `flash` arg to
  `JE_dBar3` paints a flat white (index 15) at the peak, then for the lower steps
  brightens the bar's own gradient toward its bank top (`flash * 3`, clamped) and
  fades that to normal. This avoids a grey detour and a hue snap at the end — the
  shield bank tops out light-blue and armour cream (not white, unlike the boss
  bar's near-white bank 7), so the peak must reach into white but the tail should
  stay in-family. `bright == 0` keeps the normal draw byte-exact.
- Smoothness: the event-driven bars would otherwise step at the 35 Hz tick. Like
  the power gauge, `gauge_flash_present(alpha)` repaints them every displayed frame
  in `JE_starShowVGA`'s present loop at an interpolated intensity. Interp needs no
  prev array — the counter decrements by exactly 1/tick, so `prev == cur + 1` and
  intensity `= cur + 1 - alpha` (rounds to `cur` at `alpha == 1`, the non-interp /
  per-tick path). `gaugeFlashAlpha` defaults to 1 so all other callers get `cur`.
- Endless stacked armour flashes only the newest/active tier, not the whole column:
  `endlessDrawArmorBar` passes the flash to the last-drawn rollover layer (`total-1`),
  which sits at the bar's bottom (rollovers fill bottom-up) and is the chunk being
  depleted; the fuller tiers below it in the stack keep their normal colour.

## Menus & shop — `game_menu.c`

- Weapon-sim preview presents smoothly by replaying the recorded frame at
  interpolated positions and copying only the preview box (8..143 × 8..182).
  `weaponSimOverlayFn(alpha)` lets the custom-weapon creator draw above the
  finished preview; it receives the interpolation alpha so overlays can glide.
- Menu label data (`menuInt`) is loaded once at startup. Inserting a row (the
  Switch/Vita "Touch" volume row) means shifting labels down once and re-applying
  bumped `menuChoices` counts on every entry (they reset from `menuChoicesDefault`
  at the top of `JE_itemScreen`). Watch for out-of-bounds when a menu's choice
  count grows.
- Buy-menu ship illustration: each weapon id has a mount point in exactly one of
  `front_weapon_*`/`rear_weapon_*` (the other holds -1). The endless shop can put
  a front weapon in the rear slot (or vice versa), so prefer the slot's table but
  fall back to the weapon's real mount table; indexing at [-1] smears the sprite
  across the screen. NortShip specials (ids ~32–40) have no authored position
  (both tables 0) and are pinned to the front mount.
- Chart-a-Course monitor overlay: threats top-left in red, boons bottom-right in
  green — opposite corners so long lines never collide. Palette-18 facts: bank 15
  = dark-red→orange ramp, bank 0 shades 0..7 = green ramp; TINY_FONT bodies at
  shade 7 (edges 3, highlights 10), so brightness offsets slide along the ramp —
  don't go below -2 or the shade-2 edge pixels underflow.
- Shop preview sidekicks mirror gameplay mounts (side/front/orbit; trailing kept
  side-by-side deliberately). Front-mounted (tr==2) pods launch from both slots
  via `button[1+i]`.
- Endless swaps the shop front-menu items 2/3 (Data Cubes → E-Shop, Ship Specs →
  Perks). Stock labels are captured once; the E-Shop labels
  (`menuInt[MENU_ESHOP+1]`) are re-applied after every buy so prices stay live.
  The Perks entry reuses MENU_PERKS as a read-only scrolling list rendered from
  `perkListId[]`.
- Chart-a-Course text plumbing: the danger RANK label centres on the monitor
  window's centre (x=77, under the map), not the asymmetric slot midpoint, so
  every rank width lines up. `endlessModText` draws its own 8-direction outline
  plus `JE_outTextAndDarken` fill; `FULL_SHADE` can't be used because negative
  brightness is `JE_outText`'s shadow sentinel (deep-red tiers would render
  black). Help-line amounts: E-Shop cost = bank 1/brightness 6 (palette-1 cash
  colour); Chart-a-Course runs under palette 18, so its payout = bank
  14/brightness 6, right-aligned to x=305.
- Nav-map planet draws must loop `mapPNum`, not `menuChoices - 1`: in endless
  `menuChoices = courseCount + 2`, which reads past `mapPlanet[5]` and feeds
  garbage to `JE_drawPlanet` (crash).
- Blank shop icons: `JE_drawItem` draws nothing when an item's `itemgraphic` is 0,
  leaving an empty box. `JE_loadItemDat` (episodes.c) fills every icon-less shop
  item — front/rear weapon, generator, sidekick, shield — with placeholder icon
  167 after load, skipping "None" entries (blank on purpose) and empty slots. So an
  unauthored icon now shows 167, not a blank; ships are exempt (they draw
  `shipgraphic`, never `itemgraphic`).

## Weapons — `episodes.c`, `shots.c`, `custom_weapon.c`

- Supersparks: only the ep4/5 item data tags certain projectiles with the ">1000"
  shot-graphic encoding (spark shower from colour bank `sg/1000` behind sprite
  `sg%1000`). The full data diff finds exactly: Mega Pulse (wpns 400–410,
  35↔7035), Beno Wallop Beam (736, 30↔7030 + second bolt 7029↔29), Beno Protron
  -B- (737, 28↔9028), Ice Beam/Blast (621/706, 634↔9634). Retagging is idempotent
  (each bolt is set from its target regardless of current state) because
  `JE_initEpisode` re-applies on the same-episode early-return path.
- Episode differences (`JE_applyEpDiffs`): weapons whose ep1-3 vs ep4/5 data
  differ beyond sparks are rewritten from shipped constants, idempotent for the
  same reason. Only the `[0..max-1]` pattern slots are touched; higher slots carry
  leftover editor garbage the fire cursor never reaches.
- Charge-Laser Cannon: the cut-from-Tyrian-2000 5-stage DOS charge sidekick,
  re-added from the original TYRIAN{1,2,3}.LVL data (wport 4 / wpnum 452 / pwr 5 /
  cost 30000) into scratch weapon slots 900–905; body sprites 87/106/125/144 in
  `spriteSheet9`. Re-offered in its original shops when `chargeLaserCannon` is
  enabled.
- Zica Laser Lv11 tweaks: the port-5 level-11 pattern's native horizontal layout
  is captured at load; the configured base/length options rebuild the side beams
  from full Lv10 beam copies in two scratch slots (the unused 818..1000 weapon-id
  gap). Rebuilt from shipped values, so re-applying is idempotent.
- Fire-cursor wrap must test `>=`, not `==`: the cursor is a persistent per-bay
  global, so swapping weapons mid-cycle can leave it above the new weapon's `max`.
  An exact test then silently kills the gun for hundreds of volleys while the
  cursor crawls through empty slots.
- Charge autofire "Yes (fastest)": `player_shot_create` has just set `shotRepeat`
  from the fired stage; override it with the quickest charge stage's shotrepeat
  (scan all stages) so max-power blasts fire at stage-0 cadence.
- Custom weapon creator: designs compile into `weapons[910+]` and a "Test" port
  60. The sidekick variant fires mode-0 levels; with charge (pwr > 0) the engine
  fires `wpnum + charge`, and consecutive level slots make the per-level curve the
  charge ramp. Sidekick mount style is the engine option `tr` (0 side / 1
  trailing-large / 2 front / 3 trailing / 4 orbit); styles 1–2 draw from 2x2
  `spriteSheet10`, the rest from `spriteSheet9`.

## Audio / MIDI — `loudness.c`, `fluid_music.c`, `win_native_midi.c`

With `WITH_MIDI`, the same `music.mus` songs are converted to Standard MIDI
(vendored midiproc LDS reader) and played through FluidSynth (needs a `.sf2`) or
the OS synth (Native MIDI). OPL emulation stays the default. Both MIDI backends
run their own playback (their own sequencer thread), which allows a mid-song loop
at the "loopStart" marker with channel state carried over the seam, exactly like
the OPL player. SDL Mixer X could only repeat whole files and was removed.
`play_song` is idempotent, which fixes the level-end-jingle repeat.

- Both backends re-derive channel state at the loop seam by replaying every
  pre-loop-point program/CC/pitch/sysex event (notes skipped). The LDS→MIDI
  conversion only emits changes, so without the replay a channel holds its
  end-of-song state on pass 2+ (dropped or mis-voiced instruments).
- Native MIDI opens the stream with `CALLBACK_NULL` and runs its own thread to
  avoid the `CALLBACK_FUNCTION` deadlock SDL Mixer X had (`Mix_HaltMusic` stalling
  on the winmm callback mutex).
- SoundFont autodetect adopts the newest `.sf`/`.sf2`/`.sf3` in `data_dir()` when
  none is configured; `resolve_soundfont` re-anchors a stale configured path there
  (survives moved installs) and clears it if unresolvable so autodetect runs
  again.

## Crash logging — `crashlog.c`, `crashlog_state.c` (Windows only)

- Writes `opentyrian_log.log` with a stack trace and a full game-state dump. The
  debug menu has a Force Crash row that exercises the real path.
- The force-crash pointer must be a `volatile` file-scope global: a provably null
  local store gets folded into `__ud2` or elided by MSVC /O2 and never faults
  (why the old version produced no log).
- Duplicate suppression: the top-level backup filter would re-report the same
  fault the vectored handler already logged, but with a useless 1-frame
  thread-start context, so it skips the recorded (code, addr) pair. This is not a
  latch; only the most recent fault is held.
- Hang watchdog: a background thread watches a heartbeat pumped from
  `service_SDL_events`. If it stalls past the threshold (default 5s), it captures
  the main thread's context under the shortest possible suspension, resumes it
  before the stack walk and symbol load (those take loader/CRT-heap locks, and
  walking while suspended could self-deadlock), then logs. A hung thread makes no
  progress after resume, so the stack stays coherent.
- Item-name lookups in the state dump are guarded (tables empty before
  `JE_loadItemDat`, ids can be garbage) and trimmed into a rotating static buffer
  so one `fprintf` can hold several names without clobbering.

## Console ports — `switch/`, `vita/`, `*_platform.c`

- Both ports share `console_platform.h`; Vita mirrors the Switch port's seams.
- Switch exit crash: libc `exit()` runs the romfs atexit teardown and then
  fcloses all stdio streams; closing a stream whose device is gone (or libnx's
  stdout/stderr) null-derefs in newlib's `_close_r`. Everything that must persist
  has already been flushed by then, so `JE_tyrianHalt` calls `_Exit()` on Switch
  and skips atexit/stdio cleanup entirely.
- Dock/undock (and desktop resolution changes) can leave the frame unpainted
  until input. Fixed by a repaint poll in the event pump (`service_SDL_events`),
  not in the resize handler.
- The console SDL drivers own the window size (window = panel). switch-sdl2 tracks
  dock/undock only while the window stays resizable and never enters SDL
  fullscreen; forcing `FULLSCREEN_DESKTOP` pinned the Switch buffer to 1080p and
  broke handheld layout. The Vita additionally forces supersample=1 (the GPU can't
  sustain the NxN present) and skips the side-gradient cache (its per-frame
  256-colour search dominates fade frames).
- Switch/Vita shoulder buttons cycle the shop preview's rear-gun mode via a
  raw-button edge pattern (console button ids differ from SDL mappings).
- Controller quirks: the right stick is folded into `analog_direction[]` so it
  drives the ship like the left; a button bound to both a confirm and a cancel
  action pushes only cancel (B-is-back); switch-sdl2 reports 8 idle controller
  slots sharing one name, so use only slot 0 (saving the rest clobbers slot 0's
  bindings); the d-pad arrives as buttons (0 hats), so defaults and config loads
  back-fill d-pad button bindings.
- Touch: base multiplier 4.0 cancels `VT_MOUSE_SENS` (0.25) for 1:1 finger→ship
  travel at the sensitivity slider's midpoint. Menus treat a touch as
  tap-to-click at the touched point (absolute mode), gameplay as relative drag.
- Joystick config is flushed to disk on every change (`save_joystick_config_now`):
  the Switch HOME-menu exit path never runs `JE_tyrianHalt`'s config write.
- Vita presents at native size and lets the GPU upscale to 960x544 (a software
  upscale is too slow); the scaler is forced to None and supersample to 1 each
  boot. The Vita IME dialog only draws while the app keeps presenting, so the
  modal keyboard loop (`vita_swkbd`) must present every frame.
- Vita on-screen keyboard: a system common dialog that grabs the control pad until
  `sceImeDialogTerm()` runs; touch (`sceTouchPeek`) is polled separately, so a
  dialog left standing means buttons dead, touch still working (the Switch
  sidesteps all this with the blocking native `swkbdShow`). Two traps drove the
  cancel-freeze: (1) `SDL_IsTextInputActive()` stays true from
  `SDL_StartTextInput()` until `SDL_StopTextInput()`, so gating the modal loop on
  it never lets a cancel end the loop; (2) SDL only terminates the dialog when its
  own event pump happens to observe `FINISHED`. The fix (`vita_swkbd`): drive the
  loop off the native `sceImeDialogGetStatus()` (not `SDL_IsTextInputActive`),
  treat the `SDL_SCANCODE_RETURN` SDL emits only on Enter as the confirm signal
  (so a cancel returns false), and force `sceImeDialogAbort`/`sceImeDialogTerm`
  after `SDL_StopTextInput()` so the pad is always released.
- Console release builds compile with `-DNDEBUG` exactly like desktop Release:
  several asserts are latent tripwires the engine relies on being elided
  (`blit_sprite` bounds, `JE_loadCompShapesB`); live, they abort into a silent
  close.

## Level scripting — `tyrian2.c`

### Map-stop softlock watchdog (`enemyParkedAbove` / `mapStopStallTicks`)

- The event clock is the scroll: `curLoc += backMove`. A scripted map stop
  (events 4/83, usually paired with an event 2 that zeroes `backMove`) freezes the
  script. The only exits are (a) the screen-clear release (`enemyOnScreen == 0`
  restores `backMove` and the clock resumes), (b) a `superEnemy254Jump` fired by a
  linknum-254 enemy death, or (c) `forceEvents` (event 53), which ticks the clock
  even at `backMove == 0`.
- Boss fights lean on the linkgroup death cascade: killing any member of a linknum
  group kills the whole group, the screen clears, release (a) fires, and a later
  timed event (11) ends the level.
- The softlock (reported on ep4 HARVEST): the script stages the fight in the same
  event tick that stops the map. HARVEST t=6331 spawns an invisible ground anchor
  (enemy 66 — no turrets, no launcher, no accel) at y=-108 with the boss linknum
  10, sets the group's armor to 254, and arms the boss bar. Kill all 12 visible
  boss pieces during their descent (t=6280..6331) and the anchor misses the
  cascade: it's inside the y∈[-112,190] keep-alive band, so it counts toward
  `enemyOnScreen`, but shots can't reach it, the stopped scroll can't carry it in,
  and the frozen clock can't move it. The armed bar then shows a full-health
  "boss" that is already dead.
- The watchdog (`enemy_stuck_above_screen` + `count_stuck_above_screen`): each
  tick a dedicated full-pool scan sets `enemyParkedAbove` = live enemies that are
  `ey <= -58` and vertically frozen (`eyc<=0`, `eycc<=0`, `fixedmovey<=0`). -58 =
  ascending shots live to y=-40 and the widest hitbox reaches ~17px below `ey`, so
  ≤ -58 is unhittable; vertical stasis plus a frozen clock means the enemy can be
  neither hit, moved by its own kinematics, nor scripted in. When a non-`forceEvents`
  stop has been held ≥ `MAP_STOP_STALL_LIMIT` (210 ticks ≈ 6 s) with ≥1 such
  enemy, they are culled like anything drifting off the playfield (`enemyAvail=1`,
  no explosion or score); the normal screen-clear release then resumes the level,
  just as the death cascade would have. Culling it also auto-clears the boss bar
  (no live linknum member, so `draw_boss_bar` zeroes `link_num`).
- Horizontal state must be ignored. HARVEST's anchor is spawned with an
  Enemy-Global-Accel event (type 20, `dat=3`) that sets `excc=3`, a sideways sway.
  It never lifts the anchor into reach (the `exrev` wobble keeps `exc` near 0), but
  an earlier predicate that demanded `excc==exc==0` rejected it outright
  (`parkedAbove=0`, watchdog never armed). Only the vertical axis decides "stuck
  above the reach line".
- Detection is a standalone scan, not the draw-loop on-screen census. The
  on-screen census only sees enemies inside the x-window and skips damage-flashing
  ones, so a horizontally-swaying anchor could blink out of the count and keep
  resetting the timer. The dedicated scan sees it every tick.
- Independent of the other on-screen enemies. A real fight leaves small enemies
  (HARVEST types 142/559, `move=(0,0)`) frozen in place beside the anchor; they
  ride the now-stopped scroll, so `enemyOnScreen` stays > 1 (e.g. 4). Requiring
  `parkedAbove == onScreen` (a dead first attempt) never fired. Those small
  enemies are killable and are a normal clear-the-arena step; they must not gate
  recovery. The watchdog keys on `parkedAbove != 0` alone and culls only the
  stuck-above set (they sit at visible `ey > -58`, so the scan never touches them).
- Why it can't false-fire on a live fight: a descending boss (`eyc>0`), a bouncer
  (`eyc` flips), or a homing chaser (drives `eyc>0` toward the player) all fail the
  vertical test and resolve on their own; a boss killed normally cascades the
  anchor away before the timer. In practice it fires only in the orphaned-anchor
  softlock. `forceEvents` levels self-drive the clock and are exempt.
- The crash-log dumps `parkedAbove=` / `stallTicks=` and a per-enemy table
  (`ex,ey exc,eyc excc,eycc link armor type`, with a `<-- stuck-above` marker) so
  a stuck scroll shows exactly which enemies are holding it.

## General pitfalls

- Sprite banks: gameplay sprite draws must use the right one of the four sprite
  banks; a wrong-bank draw renders garbage that looks "almost right".
- `enemycycle` indexes `egr[]` 1-based; an out-of-range or zero index underflows
  `blit_sprite2` into a wild read. `blit_enemy` skips anything that doesn't name a
  real in-sheet sprite.
- Sidekick body blits are 1-based (`blit_sprite2` reads `offsetTable[index-1]`); a
  2x2 mount also reads index+1/+19/+20 and the engine adds the charge stage
  (0..pwr), so clamp base+frames to `1..count-pwr` (minus 20 for 2x2 mounts).
- Menu data model: `menuInt` (labels) / `menuChoices` (counts) / `menuHelp` must
  stay consistent when adding rows to any mode menu.
- Fonts: TINY_FONT glyph shades — body 7, edge 3, sparse highlights 10; the
  minimum safe brightness offset is -2. `'~'` is a brightness-toggle in
  `JE_outText*`/`JE_dString`, never printed, so keep it out of help-line format
  strings.
- The in-game debug menu (opened from the shop front menu, or the Esc-pause "Debug
  Menu" row while Debug Mode is on — there is no key shortcut) is the extension
  point for cheat/diagnostic rows; new rows follow the existing table pattern.
- The Doxygen-style `/** */` documentation in upstream files (font.c,
  config_file.c, …) is upstream convention; leave it be.
