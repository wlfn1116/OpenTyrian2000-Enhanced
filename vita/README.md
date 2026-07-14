# OpenTyrian 2000 — PS Vita port

A homebrew PlayStation Vita build of the OpenTyrian 2000 widescreen fork, producing a
`.vpk` you install with VitaShell / a homebrew-enabled (HENkaku/Enso) Vita. It mirrors the
Nintendo Switch port feature-for-feature: touch controls, on-screen keyboard for name entry,
dual-analog ship control, the touch-sensitivity slider, and the same always-fullscreen
display handling — just mapped to Vita hardware.

This is **not** a signed retail title; it needs homebrew (HENkaku/Enso) to run.

---

## What you get

`vita/build/OpenTyrian2000.vpk` (~6 MB) — a self-contained package: the game plus the
freeware Tyrian 2000 data baked into `app0:data`. Writable config and saves go to
`ux0:data/opentyrian2000` (created on first run). Title ID `OTYR20000`.

## Requirements

- **VitaSDK** with the `sdl2` package:
  ```sh
  # VitaSDK installed at D:\vitasdk (set VITASDK otherwise)
  vdpm sdl2
  ```
- **A native Windows CMake (>= 3.16) and Ninja.**
  - The build uses a *native* Windows cmake and `ninja.exe`. The MSYS/devkitPro `cmake` on
    `PATH` does **not** work: it hands the native Ninja POSIX `/d/...` paths it can't resolve.
  - `ninja.exe` is expected at `%VITASDK%\bin\ninja.exe` (drop the single binary there), or set
    `$env:NINJA_EXE`. A native cmake is auto-located (Program Files, or a pip `cmake` install);
    override with `$env:CMAKE_EXE`.

## Build

From PowerShell (recommended on Windows):

```powershell
powershell -ExecutionPolicy Bypass -File vita\build.ps1
# clean:
powershell -ExecutionPolicy Bypass -File vita\build.ps1 -Clean
```

Or from Git Bash (forwards to the PowerShell script):

```sh
bash vita/build.sh          # build
bash vita/build.sh clean    # clean
```

Output: `vita/build/OpenTyrian2000.vpk`.

### How the VPK is assembled (and why not `vita_create_vpk`)

CMake builds only `eboot.bin` (the SELF). The VPK is assembled by the build script:
`vita-mksfoex` writes `param.sfo`, the tree is staged (`eboot.bin`, `sce_sys/`, `data/`) and
zipped with `cmake -E tar --format=zip`. The reason for not using cmake's `vita_create_vpk`
FILE list: the freeware `data/` folder has shell-hostile filenames (`shapes).dat`,
`newsh%.shp`, `newsh^.shp`, …) that get mangled when `vita-pack-vpk`'s per-file `-a src=dst`
args pass through cmake's custom commands on `cmd.exe`. Staging with `robocopy`/`cp` and
zipping the whole tree copies those names verbatim.

### Data bundling

`build.ps1` copies `data/` into the VPK's `app0:data`, excluding the Windows/DOS distro cruft
(`*.exe *.dll *.pif *.ico *.box *.ovl *.cfg *.sav *.txt` — same list as the Switch romfs) plus
`WeedsGM3.sf2` (~55 MB): that soundfont only feeds the optional FluidSynth MIDI path, which is
off here exactly as on Switch (`WITH_MIDI` is Windows-x64-only). To ship a user-updatable data
copy instead, drop the files in `ux0:data/opentyrian2000` — `file.c` prefers it over `app0:`.

### LiveArea art

`make_livearea.ps1` regenerates the LiveArea/VPK images (`icon0.png`, `pic0.png`, `bg0.png`,
`startup.png`) from the shared box art (`switch/icon.jpg`). They must be **8-bit indexed**
PNGs — the VPK promoter rejects 24-bit truecolor LiveArea images (install fails at ~98% with
`0x8010113D`). The script produces indexed PNGs via a GIF round-trip through GDI+.

## Install

Copy `OpenTyrian2000.vpk` to the Vita (FTP / USB / SD2Vita) and install it with **VitaShell**
(press `X` on the file → install). It appears on the LiveArea as *OpenTyrian 2000*.

## Controls (defaults — rebindable in-game)

| Input | Action |
|-------|--------|
| Left / Right analog stick, D-pad | Move ship / menu navigation (both sticks steer) |
| ✕ | Fire / confirm |
| ○ | Change rear weapon mode / cancel |
| L / R | Left / right sidekick |
| START | In-game menu |
| SELECT | Pause |
| Front touch | Menus: tap to click. Gameplay: drag to steer (trackpad-style), auto-fires |

Text entry (high-score and save names, debug numeric fields) uses the Vita's system IME.

The **Touch** sensitivity slider (Setup menu, the in-game pause menu, and the shop options)
scales the touch-drag ship control; its middle notch is the classic 1:1 feel.

## Known limitations / next steps (awaiting on-hardware testing)

- **Button order is a best guess.** Vita SDL button indices (`0 △, 1 ○, 2 ✕, 3 □, 4 L, 5 R,
  6 down, 7 left, 8 up, 9 right, 10 select, 11 start`) come from the SDL Vita source; on first
  run the game writes `ux0:data/opentyrian2000/joystick_info.txt` with the detected caps —
  check it and re-bind in the Controls menu if anything is off.
- **IME text entry** is wired through SDL's screen-keyboard support; the exact confirm/cancel
  behaviour may need tuning on real hardware (as the Switch software keyboard did).
- **Performance.** The smooth-60fps interpolation + sub-pixel supersampling is GPU-heavy for
  the Vita's SGX; if it doesn't hold 60 fps, cap the supersample (Setup → Graphics → Sub-pixel)
  or we can pin `effective_supersample()` to 1× on `__vita__` as the Switch once did.
- **Networking** is compiled out (`WITH_NETWORK` off), same as the Switch build.
