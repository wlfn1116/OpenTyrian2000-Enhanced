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
- Collected filenames use a platform suffix (`Win64`, `Win32`, `Switch`, or
  `Vita`) without embedding the version. The source outputs retain the names
  expected by their toolchains.
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
- Parallax amplitude (all layers) — gated by the **Extra Parallax** toggle
  (Enhancements menu, `extraParallax` in config, below Debug Mode). OFF computes the
  stock `w_f = (1-u)*72` over the `[40,363]` normalization and makes `bg_clamp_map` a
  no-op, so the parallax + draw path matches stock — with ONE deliberate exception:
  when the ship is pinned fully left (`u<=0`), the bg2 overlay (EP1 TYRIAN clouds etc.)
  has its sub-pixel scroll fraction snapped to 0 (`mapX2Ofs_f = (float)mapX2Ofs`).
  Layer 2 is one strip at a single X offset, so it can only translate uniformly — a 1px
  shift trades the left gap for a right one. The real artifact is that at the far-left
  `mapX2Ofs` is 36 (int) / 36.667 (float), and the smoothed replay rounds that 0.667px
  fraction UP to a whole pixel → clouds drawn 1px right. Snapping the fraction makes the
  smoothed render land crisply on the integer pixel (1px left of the rounded-up spot),
  no fractional spill on either edge. Only the render-list interpolation path (Smooth
  Motion on) is affected; integer `mapX2Ofs` + glued layer-2 enemies untouched.
  `bg2CrispLeft` flag in the parallax block.
- Layer-2 right-edge coverage guard (both modes). The 14-tile (336px) bg2 strip is
  1px too narrow to reach the playfield's right edge (col `PLAYFIELD_RIGHT` = 322)
  once the parallax pushes `mapX2Ofs` to its far-right value `-2`: the strip draws at
  screen `x = mapX2Pos + PLAYFIELD_X_SHIFT = -14` and ends at col 321 → 1px cloud gap
  on the right. The near layer only reaches `-1` (`x=-13`, just covers 322), so clamp
  `mapX2Ofs` (and `mapX2Ofs_f`) to a floor of `-1` so the strip always reaches the
  right edge. ~1px pan freeze over the last few px of ship travel; both int and float
  clamped so the smoothed replay agrees. Applied after `mapXOfs` is derived (near layer
  keeps its own `-1` floor independently). The near map is 14
  tiles (336px) but the window is only 299px and the compositor crops the first 24px,
  so ~1 tile stayed permanently off the left. `mainint.c`'s parallax block keeps the
  original coupled 4:2:1 ratio (`mapX3Ofs = tempW`, `mapX2Ofs = (tempW-17)*2/3`,
  `mapXOfs = mapX2Ofs/2`) driven by a shared float `w_f`; ON reshapes `w_f` so a strafe
  sweeps *every* layer across its full width. Bound ground enemies ride
  `mapXOfs`/`mapX2Ofs`/`oldMapX3Ofs` via `tempMapXOfs` and stay glued (they slide much
  further now — intentional).
- ON pans the NEAR layer FLUSH at both extremes (0 px of map off either edge at the
  walls). `mapXOfs` sweeps `36` (far-left: plane-px 0 at the window's left edge =
  `PLAYFIELD_LEFT - PLAYFIELD_X_SHIFT`) down by the slack (`336 - 299 = 37`) to `-1`
  (far-right: the map's last px at the window's right edge). Crucially it's normalized
  over the ship's ACTUAL travel `[SHIP_LEFT_MARGIN, PLAYFIELD_WIDTH-SHIP_RIGHT_MARGIN]`
  = `[29,303]`, not the stock `[40,363]` u (which only reaches ~0.81 at the right wall
  → `mapXOfs` stuck at ~2, i.e. the ~4px of map left off the right edge). `w_f` is
  back-derived as `3*near + 17` so the mid/deep layers keep the coupled ratio and still
  over-pan (`mapX3Ofs → 125`, `mapX2Ofs → 72` at far-left). At `mapXOfs == 36`,
  `mapXPos == 12`, so the near layer's lone wrong-row boundary tile spans screen
  `[0,24)`, entirely inside the crop margin — no visible seam.
- Over-pan past the map's side edge is handled by the edge mirror (next section),
  which also subsumed the old `bg_clamp_map` out-of-bounds guard.
- The legacy z-order can put a layer and its bound enemies on opposite sides of
  `JE_mainGamePlayerFunctions`, which also recalculates horizontal parallax. The
  entity may therefore carry the previous anchor while its layer carries the new
  one (or vice versa), becoming 1px misaligned at integer crossings. Finalization
  normalizes tagged enemy sprites and HP bars to the absolute anchor their layer
  actually recorded; simulation and exact/residual replay retain their original
  integer coordinates.
- `background3x1` is not a parallax ratio — it welds layer 3 to layer 1
  (`mainint.c` sets `mapX3Ofs = mapXOfs`), making it a co-scrolling foreground
  overlay for pieces that belong to layer 1's plane but must draw over the ship.
  The two therefore have to pan as one, but layer 1 always records before the
  mid-tick parallax update while `background3over == 1` records layer 3 after it,
  so the *shared* anchor was sampled a tick apart. The stock whole-pixel blit
  truncated that sub-pixel difference away; the interpolated pan resolves it, and
  a foreground welded to the terrain shows it as a horizontal seam whenever the
  player strafes (EP4 SURFACE overhangs). `draw_background_3` now pans from the
  anchor layer 1 recorded (`bg_layer_xofs[1]`) whenever `background3x1` is set,
  keeping the integer at `mapX3Ofs` since that is what its rows blit at. Bound
  Top Enemy sprites need no extra handling — the normalizer above already pulls
  them onto their layer's recorded anchor. No-op unless `background3x1` is set,
  and a no-op even then when layer 3 draws before the update (`background3over`
  0/2 — BRAINIAC, DREAD-NOT), where both anchors are already the same value.
- Vertical: the same idea via `bg_layer_dy`/`yfrac`. An integer per-tick scroll
  can't express a fractional rate (3.2 → 3,3,3,4 velocity pulse that looks like
  speeding up and slowing down); the float rate makes the displayed velocity
  constant. The published rate lags one tick, matching how `bgScrollDeltaY` is a
  draw-time position diff. Applies to every level — see §Slow-scroll smoothing.
- Keep a vertically bound command's whole-pixel draw-phase correction separate
  from the layer's fractional phase. Replay adds the whole part exactly and rounds
  only the shared fractional offset with `floor(x + .5)`. Rounding the combined
  absolute coordinate half-away-from-zero is not translation-invariant: a
  negative background row and positive enemy choose opposite pixels at exactly
  `.5`, producing the scale-1-only 1px mismatch under fractional fast scroll.
- Every enemy bank is authored and recorded at a common pre-advance phase; HP bars
  are recorded after each enemy advance and receive the inverse whole-pixel correction.
  Layer 3 terrain is the sole draw-order exception: it is recorded post-advance, so
  replay preserves its authored base step, subtracts only the modifier-added pixels,
  and applies the bound entities' lagged fractional phase. This keeps the shipped
  placement while preventing the offset from growing with scroll speed (BRAINIAC).
- Finalization canonicalizes the bound part of an enemy's vertical displacement
  to the exact float layer rate. Replay applies that layer transform independently
  on every bound command, including new, clipped-count-changed, and snapped commands;
  only enemy-local motion comes from cross-tick matching. The large-jump guard tests
  only local motion rather than rejecting a legitimate high-speed layer delta.
- The layer offset and the entity-local offset are rounded as ONE value
  (`rl_layer_y_offset` takes `par_yown100`). Rounding them separately puts each on
  its own staircase; whenever the entity moves against the scroll (own opposing the
  layer rate, e.g. own100 = -200 for a 1px/tick upward swimmer on a 1px/tick layer)
  the thresholds interleave and the sum steps down-up-down inside a single tick — a
  1px sawtooth visible at scale 1 and diluted to 1/N px at supersample N (EP5
  CORAL's launched fish around the locked boss). A resting entity (own 0) still
  gets the background rows' expression bit-for-bit, so terrain gluing is unchanged,
  and a scroll-cancelling boss (own == -rate) reduces to the constant `frac` — held
  perfectly still at every alpha even under a fractional endless boost rate.
- The smooth-scroll accumulator is quantized in hundredths. Replay reconstructs those
  integer hundredths and evaluates the canonical layer offset in double precision;
  subtracting a large rate directly in `float` can turn an exact `-N.5` tick endpoint
  into `-N.500000004` and round it one pixel backward.

### Extra Parallax edge mirror — `backgrnd.c`, `render_list.c`

- The problem: with Extra Parallax on, the mid/deep layers over-pan far enough at
  far-left that their map's left edge slides *into* the window (deep layer's plane-px 0
  reaches screen x `mapX3Ofs − 36` = 89 at full pan, i.e. 65 px inside the 24-px window
  edge; mid layer 36 − 72 → 12 px inside… up to ~2–3 tile columns). The maps are stored
  row-major, so the columns read past the row start came from the *previous row's right
  side* — a hard content seam where the layer visibly "ended".
- The fix: columns outside `[0, row_width)` re-read the same row's columns in reflected
  order and render horizontally FLIPPED (`bg_mirror_tile`): tile column `c < 0` →
  column `−1−c` flipped, `c ≥ w` → `2w−1−c` flipped. Per-tile reflection + pixel flip
  compose to the exact plane-pixel mirror `p → −1−p` about the edge, so the layer
  continues as a seamless reflection of itself (mirrored-repeat, like GL's
  `MIRRORED_REPEAT`). Same-row sourcing means vertical scroll stays coherent for free.
- Gated by the **Mirrored Layers** toggle (`mirroredLayers`, default on, config key
  `mirrored_layers`), an Enhancements row directly under Extra Parallax. Independent of
  `extraParallax`: the stock span also uncovers ~12px of layer 3's left edge at
  far-left (plane-px 0 sits at screen `mapX3Ofs − 36` = 36 vs window-left 24), so the
  mirror covers that sliver too. Off = the original draw: adjacent-row wrap seam, with
  the pointer clamp applied only under Extra Parallax (`bg_mirror_setup` reduces to the
  old `bg_clamp_map` expression, `mirror_w` 0); stock+off is byte-for-byte original.
  Adding the 11th row auto-compresses the Enhancements pitch 15→13 px (the `y<=172`
  fit rule).
- Ship cast-shadow re-centre: the shadow x rides `- mapX2Ofs + shadow_light_dx`
  (`mainint.c` player draw), where the constant is mapX2Ofs at mid-travel so the swing
  is symmetric. Extra Parallax widens the sweep (72..−1 vs stock 36..−1), moving the
  mid-travel value from 18 to 34 (u=0.5 → w_f=69.5 → (69−17)*2/3), so
  `shadow_light_dx = extraParallax ? 34 : 18`.
- Plumbing: the row blitters (`blit_background_row`/`_blend`/`_scaled`) take
  `mirror_w` (map row width in tiles: 14, layer 3 = 15; 0 = off → stock reads,
  byte-for-byte) and `col0` (map-column index of `map[0]`). `rl_rec_bg_row` stores both
  in the `RenderCmd` (`bg_mirror_w`/`bg_col0`) so every render-rate replay — 1x and
  supersampled — mirrors identically. `col0` comes from the layer's `bpPos`:
  `mapXbpPos − 1` for the 14-wide layers (the `mapYPos` pointers carry a −1 bias),
  `mapX3bpPos` for the 15-wide layer 3; the smoothie x-sync and `background3x1` welds
  inherit the right value because they redirect `bpPos` itself.
- `bg_mirror_setup` replaced `bg_clamp_map`: reflected columns never dereference before
  `mainmap[0][0]`, so the old top-of-scroll clamp (which snapped the whole call's X
  phase to column 0) is only kept as a fallback for the level-end state that re-points
  `mapYPos` at row 0 without the −1 bias. Normal top-of-scroll far-left now keeps its
  column phase and mirrors instead of popping.
- The near layer passes mirror params too but never triggers them *in-window* (it pans
  flush by design); its lone boundary tile at `[0,24)` sits in the crop margin.
- **Right-edge margin strip** (`bg_edge_px`, Mirrored Layers only). A row is
  `BG_TILE_COUNT*24` = 336 px — exactly the near map — so at the far-right pan extreme
  (`mapXOfs` −1 → row x = −13) it ends *flush* with `PLAYFIELD_RIGHT` (322) and nothing
  covers the columns past it. That is fine for what's displayed, but the lava and water
  smoothie filters **sample to the right** of the pixel they write (`src_pixel + waver`,
  `waver ∈ [−1,7]` lava / `[−1,3]` water), so along the screen's right edge they read
  the black fill instead of terrain. `waver` is a triangle wave over the linear pixel
  index (period ≈ 23 scanlines), which stamps the miss out as the sawtooth **"black
  triangles"** reported on EP1 ASSASSIN / EP4 LAVA RUN. Both filters also read their own
  row above/below at `+waver`, so black *beyond* the direct read range still bleeds
  leftward across frames (steady-state deficit ≈ ½ per waver-step). And the smooth-motion
  replay independently shifts a row up to a tick's pan (≤12 px) further left, uncovering
  columns that are actually displayed.
  Fix: append up to one more tile column to the right, clipped to `surface->w`, so a row
  starting at `x ≤ 20` now runs all the way to buffer column 355 — no black fill to the
  right of the terrain at any pan position, nothing for the filters to pick up. This is
  the first live use of the `c ≥ w` branch; the appended column is a flipped copy of the
  row's last column, sits entirely at x ≥ 323 (past the crop, invisible) at tick
  positions, and only comes into view when a replay shift pulls it in — which is the
  point. Clipped rather than whole-tile so the row can never run past `surface->w` and
  wrap onto the next scanline; `blit_background_row_scaled` derives the same 1x px count
  (`/ scale`) so hi-res replays cover the same columns. Zero when `mirror_w == 0`, so
  Mirrored Layers off is unchanged (out-of-row columns there would wrap into the next
  map row).
- Enemies bound to the layers are sprites, not tiles: they slide over the mirrored
  region unreflected (same as before; intentional).

### Slow-scroll smoothing — `endless_combat.c`, `tyrian2.c`, `render_list.c`

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
- Byte-exact no-op on full-speed layers: an integer rate gives `frac == 0`, so
  the float path equals the old integer path exactly — no regression, no new 1x jitter. At 1x a
  fractional layer still steps 1px per N ticks, but now at the correct sub-pixel
  phase and in lockstep with its scroll-tracked enemies (they read the same
  `bg_layer_yfrac`), instead of all freezing then jumping together.
- Only layers 1/2 gate (layer 3 always scrolls `backMove3`/tick); enemies
  scroll-track layers 1/3 only, so they inherit the glue automatically.

### Endless scroll boost — `endless_combat.c` (`endlessScrollExtraPx`)

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

`fixedmovey` has mixed level-script semantics. Sky formations use it as local
motion and leave it unscaled; layer-bound enemies use it to modify (often exactly
cancel) their normal layer advance. An opposing `fixedmovey`/`eyc` pair first
cancels at stock speed (BRAINIAC uses -1/+1); only the remaining fixed component
is scroll-relative. That residual is scaled directly from the batch's actual
`tempBackMove + tempScrollExtraPx`, with a signed per-enemy carry for non-integral
ratios. A residual `fixedmovey == -baseStep` object thus cancels every boosted
pixel exactly, while an already-cancelled pair cannot acquire speed-dependent
drift. Delay-gated sections use the same modifier percentage with a carry because
stock `fixedmovey` runs even on gate-off ticks.
The removed `endlessExtraScrollSteps` path used an independent pulse phase, so
its average was right but individual ticks increasingly separated objects from
their layer at higher modifiers.

The sky bank (slots 0..24, `JE_drawEnemy(25)`) has no `tempBackMove` channel:
anything attached to background layer 2 authors the ride into the enemy's own
motion, in either of two styles. GYGES's plain glass structures spawn with
`eyc == backMove2` (event dat3); its first chain structure instead rides on
`fixedmovey == backMove2` (event dat6) with the `eycc` oscillator swinging
`eyc` symmetrically between `±eyrev` (average 0) for the sway. The boost
therefore outran both until the per-enemy attachment test in `JE_drawEnemy` —
ride = `fixedmovey` + (`eyc` unless `eycc` oscillates it) `== backMove2 > 0`,
no `yaccel` homing (it mutates `eyc` after the test and marks a free flyer) —
began adding `endlessScrollExtraPx2` to their scroll advance. The same test binds their
sprites and health bars to layer 2's canonical presentation transform
(`par_ylayer 2`, lagged fractional clock, bar pulled back by the post-advance
scroll part), so under a fractional boost rate they render pixel-locked to the
glass rather than pulsing against its smooth float rate. Free flyers, and the
whole bank at stock speed (`extraPx2 == 0`), keep byte-identical simulation.

Event-time spawn phase needs the same treatment. Level events are keyed to
layer 1's integer `curLoc`; a boosted tick can cross several event coordinates,
so the next tick may first process a spawn with `curLoc > eventtime`. Without a
catch-up, only the records on skipped coordinates start one or more pixels behind
their terrain (AST CITY exposes this densely). `tyrian2.c` retains the exact
layer-1 interval and layer-3 delta crossed by the preceding tick, then advances a
new bound enemy through the missed fraction of its layer plus its own `eyc` and
scaled `fixedmovey`. This also preserves fixed-motion cancellation in BRAINIAC.
Free-flying sky slots are excluded, as they are not vertically layer-bound. A
spawn that passes the sky attachment test rides layer 2 — a DIFFERENT layer
than the event clock, which changes the math. The glass and the event clock
quantize their boosted fractional rates through independent carries, so the
integer identity "glass == ratio × curLoc" that stock keeps exact wanders ±1px
with the relative carry phase. Pieces of one structure spawn on different ticks,
inherit different phases, and keep them for life — a permanent 1px seam inside
the structure (GYGES's chain machine showed it on Slipstream). Per-tick pulse
proration cannot fix this: even a late-0 spawn can land on a shifted-phase tick.
Each sky spawn is instead anchored to the ideal line — `ratio100 × late` at the
stock layer ratio (the boost cancels out of it) plus the current cross-layer
carry phase (`eventScrollSkyRatio100`/`eventScrollSkyPhase100`, captured in
hundredths from the smoothing carries at the end of each tick's scroll block).
Local motion beyond the ride is prorated like the other banks, and sky
`fixedmovey` itself never scales (local-motion semantics). Event jumps and
`forceEvents` timeline-only increments invalidate the catch-up.

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

## Endless mode — `endless*.c`, `endless.h`, `endless_internal.h`

Endless mode is split across nine files. `endless.h` is the public interface the
rest of the game calls; `endless_internal.h` is the private contract between the
endless files (shared run state, shared helpers) and maps out which file owns what:

| file | owns |
| --- | --- |
| `endless.c` | run state, lifecycle, zone milestones, the run-end screen |
| `endless_rng.c` | the run seed and the structural RNG, the seed-select screen |
| `endless_level.c` | which shipped level a zone plays, its music, its reroll |
| `endless_combat.c` | enemy scaling, elites, specials, player-side modifiers |
| `endless_perks.c` | perks |
| `endless_shop.c` | the outpost: stock, prices, the E-Shop buys, the gamble |
| `endless_mods.c` | the mutator table: sector names, danger tiers, help text |
| `endless_course.c` | Chart-a-Course: generating and committing the next sector |
| `endless_save.c` | the `endless.sav` sidecar and the Quit Level sortie snapshot |

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
  ramp (drives the elite share + shot-damage climb).
- Extra volley shots (`endlessExtraEnemyShots`) run off the difficulty-scaled zone
  (`endlessDifficultyZone` = real zone on NORMAL), not the tide coefficient. Two
  segments meeting at the anchor (zone 100): an early ramp that adds the FIRST
  extra shot at zone 25 and rises evenly to `ANCHOR_ADD` (3) by zone 100, then a
  steady +1 shot every `STEP` (25) zones with NO hard cap — so 5 by zone 150 and
  climbing indefinitely (only the `MAX` 50 sanity backstop and the `ENEMY_SHOT_MAX`
  pool bound it). NORMAL: 1@z25, 3@z100, 5@z150, 7@z200, 9@z250… Harder/easier
  difficulties travel the same curve sooner/later.
- Contact-damage ramp (`endlessContactDamagePercent`): the collision/ram damage
  the PLAYER receives scales up with depth — no bonus until zone 35, linear to
  +150% by zone 100, same slope onward, capped +500% (~zone 252). Applied in the
  `mainint.c` ram-collision path to `playerHit` ONLY; `damage_to_enemy` keeps the
  unscaled value, so enemies aren't ground down any faster. Composes after the
  RAMPAGE ×1.5, then the final byte clamps to 255. Difficulty-scaled off the same
  `endlessDifficultyZone`. Base ram damage is small (`damageRate`, usually 2/tick),
  so the effective per-tick hit steps 2→3→4→5 as the multiplier crosses 150/200/
  250%.
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
- Faster scroll always reads hostile (2026-07). Mechanics unchanged: Overclock
  (+70%) / Overload (+220%) still speed fire + shots + scroll together, and
  Slipstream / Warp are the scroll-only versions at the same strengths — but
  Slipstream/Warp moved to `ENDLESS_HOSTILE_MASK` (red rows, weights 6/20, paid
  like threats, cleansable), so no faster-scroll mod ever shows as a green boon.
  The PRESENTATION is decoupled: Overclock/Overload's monitor word is now
  "faster/extreme enemy attacks" and `endlessCourseModRows` appends a separate
  display-only red "(much) faster scrolling" row (suppressed if a real scroll
  bit is present), replacing the ambiguous "faster scroll + fire" phrasing;
  curated help lines attribute fire to the foe ("foe fire/shot/scroll+").
  Slipstream's old boon combos (Blitz / Smash and Grab / Time Warp / Power
  Play / Payday) moved to `endlessMixedThemes` (reachable via boon grafts —
  `mixCommon` explicitly includes SLIPSTREAM), its hostile pairs (Fast Lane /
  Runaway / Bypass) to the hostile table, and "Warp Speed" lives in a
  naming-only `endlessWarpThemes` kept out of the shuffle pool. Slipstream
  stays OUT of the combinable widen pool (Overclock already carries the same
  +70% scroll — pairing them would be a redundant bit). Same bits, no save bump.
- Rare whole-visit flavors (Jackpot ~1/25: all boons; Ambush ~1/20: one forced
  dangerous sector; Gauntlet ~1/7: all hostile). All three dice are rolled up
  front unconditionally so the seed stream stays aligned; precedence Jackpot >
  Ambush > Gauntlet; none fire at depth 0. All three are suppressed on a
  milestone zone (below) — the dice still roll, only the effect is gated.
- MILESTONE ZONES (`endlessMilestoneKind`, keyed off the REAL zone
  `endlessRunDepth + 1`, not the difficulty-scaled one): every 50th zone charts a
  full FIVE-course slate of nothing but S-tier sectors. Zones 50/150/250/… run
  S+/S++, split 2-of-one and 3-of-the-other with the seed deciding which rung gets
  the pair. Every 100th zone (100/200/300/…) has a FIXED shape instead: **1 END +
  2 S+++ + 2 S++** — the END course is "The End" (below), and the four generated
  slots split evenly. The 2-or-3 roll still runs on a grand milestone and is then
  overridden, so the seed stream stays aligned with a plain one.
  `endlessMakeRankCombo(rank)` builds each sector: shuffle
  `endlessMilestonePool[]`, greedily take bits that don't overshoot the rank's
  score band, stop the moment the score is inside it, then VERIFY with
  `endlessDangerRankLevel` before handing it back (so a retuned band can't
  silently mislabel a slate) and reshuffle if it missed. Bit weights are read off
  `endlessModTable` via `endlessModReward`, never duplicated. `group` in the pool
  marks mutually redundant bits — at most one scroll mod, one homing tier, one
  elite tier, one shield handicap. Out of the pool on purpose: ELITEPACK (the
  deep-run redundancy swap would move the score), and the super-rare signatures
  (Deadgen / evil kill-fire / Redline) — a milestone is a wall of ordinary
  dangers, not a scheduled visit from the rarest sector in the game. Bands are
  S+ 40-49, S++ 50-59, S+++ 60-95 (the rank itself is open-ended above 60; the
  build stops at 95 so a slate stays flyable). The override runs after every
  ordinary generation step and before the OMNI roll / danger sort / unique-name
  pass, so a milestone chart is finished off like any other; it keeps the levels
  gathered up top and re-deals only the mutator sets. Reward follows danger
  automatically (same `endlessModTable` the payout reads), so these pay big.
  The milestone constants and `endlessMilestoneKind` (the zone about to be
  charted) / `endlessMilestoneClearedAt` (a depth that WAS one) live in
  `endless.c` with the other run-progress state, since the outpost needs them long
  before the course generator does.
- A GRAND milestone always deals **"The End"**, and it is NOT a fixed bitset — only
  its CORE is constant, the rest is re-rolled per milestone off the seeded stream,
  so a run's zone-100 finisher differs from its zone-200 one and from every other
  run's (80 variants, all reachable). `ENDLESS_THEEND_CORE` is the enemy at its
  worst and nothing else: Fortified, Frenzy, Swift, Devastating, Enrage. What
  varies (`endlessMakeTheEndMods`): the special-enemy tier (always one — Apex, or
  Legion at 1-in-3), the scroll pace (none / Slipstream / Overclock / Overload /
  Warp), and a coin apiece for Gravity, Topsy and Sluggish. Gravity+Sluggish can
  both land — that's the Tar Pit pairing, brutal but always flyable, since
  `endlessGravityDrift` scales the pull down in lock-step with the ship.
  Deliberately excluded: the homing tiers, which turn a gun fight into a chase, and
  the two handicaps that simply take a system away (Shieldless, Deadgen). Elite
  Pack is out too — deep runs retire it as redundant
  (`endlessFixRedundantElitePack`), which would rewrite the finale's bitset from
  under it.
- Everything that makes The End *the* End hangs off one marker bit,
  `ENDLESS_MOD_THEEND` (bit 39), not off matching an exact combination — which is
  precisely what lets the dangers vary. The marker carries no mechanic; no gameplay
  lever reads it. It supplies: the name ("The End", special-cased at the top of
  `endlessComboNameSalted`), the **END** rank (`endlessDangerRankLevel` returns 10,
  off the letter scale — `endlessRankName[]` and game_menu.c's `endlessRankHue[]`
  are both indexed by that level and MUST stay the same length), the **FINALITY**
  danger word (a rung above APOCALYPSE in `endlessDangerTier`), and the bounty: its
  `endlessModTable` reward is 150, so the finale pays ~25-31x base (≈570-710k at
  zone 100). Because the danger score sums that same table, the marker also puts
  the sector at 238-301 — far above the 95 ceiling `endlessMakeRankCombo` tops
  S+++ courses out at — so it is always strictly the worst course on the slate and
  the sort always puts it last. It is its OWN rank, not one of the two generated
  rungs, which is why the grand slate reads 1 END + 2 S+++ + 2 S++. Pinned into
  slot 0 so every later draw sees it in `used`.
- Moving The End onto the marker left the Cataclysm sub-pool (the `endlessRareThemes`
  rows carrying neither Apex nor Legion, dealt by the ~1/45 injection) without that
  bitset, so three rows were added at its top: **Ruination** (56, S++ — the same
  six-danger enemy wall plus a well that the old fixed The End used, so the
  combination stays in the game), **Death Knell** (68) and **Black Sun** (73).
  Those last two are S+++, and the only S+++ sectors an ORDINARY zone can be dealt
  without an Apex/Legion tier — a rare shot at a genuine nightmare outside the
  milestones. The sub-pool now spans 46-73 across 20 rows.
- The marker's `word` in `endlessModTable` is **NULL**, meaning "label, not
  mechanic": `endlessCourseModRows` and `endlessAutoBody` both skip NULL-word bits,
  so the monitor's threat column and the help line list only the sector's real
  dangers — 6-10 rows (11 when Overload/Overclock adds its display-only scroll
  row), against a 16-row monitor. That is the point of the design:
  the finale is read as a long wall of threats plus an off-scale rank, not as a
  curated one-liner. (Any future label-only bit gets this behaviour for free.)
- Each milestone CLASS is PINNED to its own track, so the two set-pieces stay
  distinct: `ENDLESS_MILESTONE_SONG_GRAND` 35 = "One Mustn't Fall" on every 100th
  zone, `ENDLESS_MILESTONE_SONG_PLAIN` 37 = "A Field for Mag" on the other 50th
  zones. Both are 1-based into `musicTitle[]`, matching `levelSong`, which the
  level start plays as `levelSong - 1`. `endlessPickLevelMusic` still runs its
  normal rolls first and overrides the result, so the seeded song stream stays
  aligned with an ordinary zone.
- The OUTPOST that charts a milestone announces it: `endlessBetweenLevels` sets
  `songBuy` to `ENDLESS_MILESTONE_SHOP_SONG` ("Parlance") instead of
  `DEFAULT_SONG_BUY`, so the warning plays while the player still has the course
  list in front of them. It needs no save field — `songBuy` is re-derived from the
  (saved) run depth at every outpost entry, and every endless route into the shop
  goes through `endlessBetweenLevels`, including the in-shop load that JE_main
  detours via `endlessResumePending`. MIND THE INDEXING: `songBuy` is played as-is
  (`play_song(songBuy)`, and the `]i` script command stores `temp - 1` into it),
  while `levelSong` is 1-based — so the same track has two constants,
  `ENDLESS_MILESTONE_SHOP_SONG` 26 and `..._LVL` 27. Keep them in step. The
  levelSong form exists because the zone BEFORE a milestone must also avoid that
  track (see below), or its level music would run straight into the shop playing
  the same thing and the warning would read as "nothing changed". The ordinary
  buy/sell theme never needed this: song 3 isn't in `endlessLevelSongs` at all.
- NO track ever plays twice running anywhere in the shop → level → shop chain,
  exactly — not statistically. A zone rejects the song its predecessor really
  played, the one its successor is PINNED to, and the warning track its following
  outpost will play if that outpost charts a milestone (so zones 49 and 51 both
  dodge "A Field for Mag", 99 and 101 both dodge "One Mustn't Fall", and 49/99 also
  dodge "Parlance"). Rejection is a bounded 6-retry loop, so a pathological seed
  can't spin. Two mechanisms make the neighbour tests exact:
  - The SUCCESSOR test is RNG-free: `endlessMilestoneKindOfZone(zone)` takes the
    zone as a parameter, so a neighbouring milestone's pinned song is simply
    looked up.
  - The PREDECESSOR test remembers rather than re-derives:
    `endlessLastSong` / `endlessLastSongDepth` record what the last-played zone
    actually used, and ride the save (v10). The old approach replayed the previous
    zone's stream and took its FIRST draw, which is only an approximation — that
    zone may itself have re-rolled — and left a real ~1-in-1150 chance of an
    audible back-to-back repeat between two ordinary zones. Deriving it exactly
    instead is impossible without recursing back through every prior zone, hence
    the stored value. The first-draw derivation survives as the fallback for when
    there's nothing remembered: a pre-v10 save, or a debug zone jump that skipped
    the intervening zones.
  `endlessLastSongDepth` also pins a RETRY: re-entering the same zone (Quit Level,
  a locked relaunch, a reload) returns the stored song immediately, so the track
  can't reshuffle — even if the player re-charts to a different course, since the
  music belongs to the zone, not the level. Verified against the real SplitMix RNG
  over 20k seeds × 210 zones (4.18M transitions): 0 wrong milestone tracks, 0 wrong
  shop tracks, 0 adjacent repeats of any kind (level→level, level→shop, shop→level),
  0 retries or reloads that changed a track.
- The pin also has to survive the level's own script: `endlessMilestoneZone()`
  (the one public predicate, in `endless.h`) makes tyrian2.c ignore event 35 (play
  new song) AND event 34 (start music fade) on a milestone — suppressing only 35
  would strand the pinned track at the fade floor for the rest of the zone, since
  34/35 are normally used as a pair. Event 35 still restores the volume. Songs 35
  and 37 remain in the ordinary random pool (`endlessLevelSongs`), so they can
  also come up on a normal zone (just never adjacent to the milestone that owns
  them); pulling one from that array would make it milestone-exclusive, at the
  cost of reshuffling every seed's song order.
- Forced perk picks are decided by ONE predicate, `endlessPerkDueAtDepth(depth)`
  (depth = the zone just cleared), for three reasons: the every-3rd-zone cadence
  (`ENDLESS_PERK_EVERY`, depths 1, 4, 7, …); a cleared MILESTONE zone, the payoff
  for surviving the S-tier slate; or the zone right after a depth where those two
  COLLIDED. A collision — depth 100, 250, 400, … i.e. every third milestone, the
  ones where `depth % 50 == 0 && depth % 3 == 1` — would otherwise hand out one
  perk where the player earned two, so the second is DEFERRED by a zone instead
  of being swallowed (…97, 100, **101**, 103, 106…); the cadence is unaffected and
  carries on from its own schedule. Derived purely from the depth, deliberately:
  no "owed perk" flag to persist, so it needs no save field and comes out the same
  across a save/reload or a mid-zone bail. `endlessPerkDepthDone` still caps it at
  one pick per depth, so re-entering the same outpost can't farm a second.
- Zone-100 credits: clearing zone 100 rolls `JE_playCredits` once, at the outpost
  that follows (top of `endlessBetweenLevels`, before the course roll and the
  auto-save), then the run carries on into zone 101. `endlessCreditsShown` gates
  it and rides the save (v9), so reloading the zone-101 outpost — or bailing out
  of zone 101 with Quit Level — never replays them. The test is `>=` so a debug
  zone jump over the mark still gets its one showing.
- The fork's credit — three consecutive rows, "OpenTyrian 2000" / "Engaged" /
  "wlfn" — is SPLICED into the roll in code, leaving `tyrian.cdt` (and its
  Switch/Vita romfs copies) untouched at exactly 126 records. `JE_playCredits`
  reads the file, then finds the insert point by SCANNING — last non-blank line
  (the "The   End" card, row 119), up to the top of the blank run above it (row
  111), then +4 — rather than hardcoding a row, and memmoves the tail down.
  Result: 4 spacers, the card, 4 spacers, "The End", which is the same gap the
  roll puts between credited people. Everything below keeps its position relative
  to the END of the roll, so the `lines_max - 8` song fade and the final held
  frame (which parks "The End" at y=81) land exactly where they always did.
- Deep-run danger escalation (`endlessDangerRamp`, a SCALE — not a percent). It's a
  TWO-STAGE ramp off `endlessDifficultyZone`: a gentle first stage 0→100 across
  zones 40→100 (`MID_SCALE`, byte-for-byte the old single-stage ramp — zone≤100
  unchanged), then a STEEPER second stage 100→500 across zones 100→250
  (`FULL_SCALE`), capped. So it tilts the Chart-a-Course rolls ~2× harder by zone
  100 and ~6× by zone 250, then holds. Levers (baseline → zone 100 → zone 250 cap):
  widen share 50→75→85% (capped, so a few legible curated themes survive); bits avg
  ~2.8→~4.05→5; boon-course roll 1/3→1/6→1/18; gambit boon-graft 35→15→5% (floored);
  every rare/super-rare injection routes its "1 in N" through `endlessDangerRareDiv`
  (~N/2 at mid, ~N/6 at cap — e.g. Apex 1/40→1/20→1/6, Deadgen 1/55→1/27→1/9);
  Jackpot 1/25→1/50→~1/150. The two danger-only whole-visit flavors use a percentage
  form HARD-CAPPED below certainty (`ENDLESS_DANGER_GAUNTLET_CAP_PCT` 45,
  `..._AMBUSH_CAP_PCT` 15), and they SATURATE at those caps by ~zone 160-190 — so the
  jump from ~5× to ~6× lands entirely on the uncapped levers (rarer boons/jackpots,
  more frequent rare injections), NOT on the "no safe route" odds. Gauntlet 14→25→45%,
  Ambush 5→9→15%; a calm route still survives ~46% of visits even at the deepest cap —
  danger is never a sure thing. None of these change the endlessRand DRAW COUNT (only
  thresholds/moduli), so the seed stream stays aligned; course 0 stays clean unless
  Gauntlet/Ambush fires. Past zone 250 the course tilt is frozen at ~6×; the always-on
  enemy levers (stats, extra shots, contact damage) keep climbing.
- Sector variety is all combinations of the existing `endlessModTable` bits, so
  it needs no new bits/save bump: the danger score, monitor rows, tier/rank and
  payout are all mod-agnostic. Sources, in generation order: distinct named
  hostile themes; a ~50% "widen" that swaps in a random 1-5 bit combo of the
  `combinable[]` hostiles (weights lean toward 2-4 bits); a ~1/3 boon course
  (60% a named boon theme, 40% an emergent 2-3 bit boon combo from
  `endlessMakeBoonCombo`). Pure-boon and Jackpot generation swap the
  Turbodrive/Overblast rarity slots, making Overblast the common roll while the
  theme table remains the canonical name dictionary. MIXED "gambit" sectors —
  after the boon roll,
  ~35% of each ORDINARY hostile course gains one compatible boon
  (`endlessPickMixBoon`), welding reward onto danger. Kill-fire boons get a
  separate 4% roll within those gambits instead of ordinary candidate weight,
  keeping Turbodrive/Overblast rare in hostile+boon combinations.
  Compatibility avoids same-lever cancels (no frail+fortified, no
  dilation+swift/overclock) and keeps the one-kill-fire rule (only one boon
  added). Un-named combos read with the right tone via three generic-name pools
  (ominous / fortunate / gambit), chosen in `endlessComboName` off
  `ENDLESS_HOSTILE_MASK` vs `ENDLESS_BOON_MASK`.
- Names are unique per chart: distinct bitsets can hash to the same generic
  word, so a final RNG-free pass in `endlessGenerateCourses` (after the danger
  sort — it must consume no `endlessRand`, keeping the seed stream aligned)
  bumps `endlessCourseNameSalt[]` until every offered label differs; the salt
  steps a generated name to the next word in its pool and is a no-op on curated
  names. Curated names must therefore be unique across ALL theme tables — the
  evil twins of "Crossfire"/"Death March" were renamed "Friendly
  Fire"/"Forced March" for this — and must not reuse a generic-pool word.
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
- Sidecar version history: v3 seed, v4 locked sortie, v5 buff recharge, v6
  recent-level ring, v7 64-bit mods, v8 exact course files, v9 zone-100
  credits-shown flag, v10 last zone's song + its depth. Each new field is appended
  and read behind a `version >= N` guard, so older sidecars still load (a missing
  field reads as the memset-zero default — note v10's apply step has to map a
  zeroed `lastSong` back to depth −1, or a pre-v10 record would read as a real
  entry for depth 0).
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
- E-Shop rows are tinted by category so related buys share a hue
  (`endless_eshop_row_bank` in `JE_drawMenuChoices`, keyed by row x ==
  `curSel[MENU_ESHOP]`): buffs Turbodrive/Overblast/Overdrive = green (bank 12),
  Reinforce/Extra-Perk = cyan (8), Special Weapon = red (4), Bomb/Revive = purple
  (5), Gamble = fiery red→yellow (7), Reroll/Sabotage/Done = default gold (15).
  Only `curMenu == MENU_ESHOP` is recoloured; the perk list stays gold. Measured
  palette-1 (shop `colors`) bank hues, TINY_FONT body ≈ shade 9: 0 grey, 1 tan/gold
  (cash), 2 olive-green, 3 steel-blue, 4 red/salmon, 5 muted-purple, 6 blue-grey,
  7 red→orange→yellow (vivid, also HP/boss-bar bank), 8 light-cyan, 9 pure-blue
  (dark low shades — avoid on the dark list bg), 10/11/14 tan/brown, 12 vivid
  green, 13 muted-teal, 15 red→gold (the default menu-text ramp).
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
- Charge-sidekick autofire (`chargeSidekickAutofire`, per-slot, default On) has two
  edit points on one byte: the debug menu's "Autofire Charge Sidekicks" row cycles
  all four modes (Off / On / Charged / Fast=fastest), while Setup → Enhancements →
  Weapon Tweaks → "Sidekick Autofire" cycles only the three player-visible ones and
  skips `CHARGE_AUTOFIRE_FAST`. Fast still *renders* as "Fast" in the Weapon Tweaks
  row if the debug menu set it, but the visible cycle can never land back on it.
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
- Why the anchor exists at all: the `stopBackgroundNum == 1` release census only
  covers the two batches drawn with `tempBackMove = backMove` — `JE_drawEnemy(50)`
  (slots 25–49) and `JE_drawEnemy(100)` (slots 75–99). HARVEST's 12 boss pieces are
  event-7 "Top Enemy" spawns, i.e. slots 50–74, drawn by `JE_drawEnemy(75)` on
  layer 3 — invisible to that census. The event-10 anchor lands in slots 75–99, so
  it alone holds the stop, and the cascade that kills the pieces kills it too.
- The watchdog (`enemy_stuck_orphaned` = `enemy_stuck_above_screen` +
  `enemy_link_group_reachable`, counted by `count_stuck_above_screen`): each
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
- **The geometry test alone false-fires on every live HARVEST fight.** The anchor
  is spawned at `ey=-108` and frozen (event 19 `dat2=0`, event 20 `dat2=0`) whether
  or not the boss is alive, so a normal fight trips the same predicate and the
  watchdog culled the anchor ~6 s in. That released the stop mid-fight: the clock
  resumed, `t=6332` restored `backMove3=2`, the still-alive boss scrolled off the
  bottom, and the level ran on to its `t=7100` end event — the boss fight simply
  evaporated. Hence the second gate, `enemy_link_group_reachable`: killing ANY
  linkgroup member cascades the whole group, so a single live partner that is *not*
  itself stuck-above means the fight is winnable and nothing is orphaned. The 12
  boss pieces rest at `ey ≈ +4`/`+32` (they spawn at -56/-28, ride `backMove3=2`
  for 30 ticks, then event 19 sets `eyc=-2` and exactly cancels the ride), so
  during a real fight the anchor is always rescued. `linknum == 0` is "unlinked" —
  those never cascade, so each is its own group and is never rescued.
- Why it can't false-fire otherwise: a descending boss (`eyc>0`), a bouncer
  (`eyc` flips), or a homing chaser (drives `eyc>0` toward the player) all fail the
  vertical test and resolve on their own; a boss killed normally cascades the
  anchor away before the timer. In practice it fires only in the orphaned-anchor
  softlock. `forceEvents` levels self-drive the clock and are exempt.
- The crash-log dumps `parkedAbove=` / `stallTicks=` and a per-enemy table
  (`ex,ey exc,eyc excc,eycc link armor type`), marking each stuck-above enemy
  `(orphaned)` — what the watchdog culls — or `(group reachable)` — a live fight's
  parked anchor, left alone — so a stuck scroll shows exactly what is holding it.

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
