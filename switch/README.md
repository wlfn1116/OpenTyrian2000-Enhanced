# OpenTyrian 2000 (widescreen fork) — Nintendo Switch homebrew

This builds `opentyrian2000.nro`, a Switch homebrew app that runs from the
**Homebrew Launcher** on a CFW/Atmosphère console, or in a Switch emulator.

> This is an unofficial homebrew build, **not** an eShop release. It requires a
> console running custom firmware (which voids warranty and violates Nintendo's
> ToS — do this at your own risk) or an emulator.

---

## 1. Install the toolchain (one time)

You need **devkitPro** with the Switch toolchain and the SDL2 portlib.

1. Install devkitPro following the official guide:
   <https://devkitpro.org/wiki/Getting_Started>
   - On Windows, run the **devkitProUpdater** graphical installer (needs admin).
   - On Linux/macOS, install `dkp-pacman` per the guide.

2. Install the Switch packages:
   ```sh
   dkp-pacman -S switch-dev switch-sdl2
   ```
   (`switch-dev` pulls in devkitA64 + libnx + tools; `switch-sdl2` pulls in SDL2
   and its dependency chain and the `pkg-config` wrapper this Makefile uses.)

3. Make sure `DEVKITPRO` is set in your environment (the installer normally does
   this — typically `DEVKITPRO=/opt/devkitpro`, or `C:/devkitpro` on Windows).
   On Windows, build from the **“MSYS2 for devkitPro”** shell so the env vars and
   Unix tools are present.

## 2. Add the game data

The engine needs the Tyrian 2000 data files. Either:

- **Bundle them (self-contained .nro):** copy the data files into
  [`switch/romfs/`](romfs/) before building — see the note in that folder. They
  get packed into the `.nro` and mounted at `romfs:/`.
- **…or ship them on the SD card:** copy them to `sdmc:/switch/opentyrian2000/`.
  The game checks that path first, then the bundled `romfs:/`.

## 3. Build

From this `switch/` directory, in the devkitPro shell:

```sh
make          # produces opentyrian2000.nro
make clean    # remove build artifacts
```

Or use the helper script (sets DEVKITPRO/PATH itself, logs to `build.log`) — this is
what works when driving the build from outside the devkitPro shell, e.g. Windows
PowerShell:

```powershell
& "D:\devkitPro\msys2\usr\bin\bash.exe" /d/Projects/OpenTyrian2000-widescreen/switch/build.sh
```

> Do **not** run the build through a devkitPro MSYS2 *login* shell (`bash -lc ...`) — a
> profile script there silently truncates multi-command runs. Invoke `build.sh` directly.

## 4. Install & run

Copy `opentyrian2000.nro` to your SD card, conventionally:

```
sdmc:/switch/opentyrian2000/opentyrian2000.nro
```

Then launch it from the **Homebrew Launcher** (hold R while opening a game, or use
your CFW's album entry). Config and save files are written to
`sdmc:/switch/opentyrian2000/` (created automatically on first run).

To run in an emulator, load `opentyrian2000.nro` directly.

---

## Controls

Joy-Con / Pro Controller are exposed through SDL's joystick API. The game has an
in-game controller/joystick configuration menu — use it to bind buttons to taste.
The face buttons and D-pad work out of the box for menus and flying.

## Known limitations (v1)

- **Text entry** (e.g. high-score names): the Switch has no physical keyboard.
  Default names are used; on-screen software-keyboard integration is a planned
  follow-up (SDL routes `SDL_TEXTINPUT` through the system keyboard when the game
  calls `SDL_StartTextInput`, which not every entry screen does yet).
- **Networking / netplay** is compiled out (`WITH_NETWORK` off). It can be enabled
  later by installing `switch-sdl2_net`, defining `WITH_NETWORK`, and linking it.
- **Resolution**: renders through the existing widescreen scaler; handheld is
  1280×720 and docked is 1920×1080. Aspect handling may need tuning on hardware.

## Troubleshooting

- **`pkg-config` wrapper not found**: this Makefile calls
  `$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-pkg-config`. If your devkitPro
  names it differently, either fix the `PKGCONF` line in the `Makefile`, or
  replace the `CFLAGS`/`LIBS` `pkg-config` calls with the literal flags:
  ```makefile
  CFLAGS += ... -I$(PORTLIBS)/include/SDL2 -D_GNU_SOURCE=1 -D_REENTRANT
  LIBS   := -lSDL2 -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lnx -lm
  ```
- **“Please set DEVKITPRO …”**: your shell doesn't have the devkitPro environment;
  use the devkitPro MSYS2 shell (Windows) or `source /etc/profile.d/devkit-env.sh`.
- **Game exits immediately / can't find data**: the data files aren't in
  `switch/romfs/` (rebuild) or `sdmc:/switch/opentyrian2000/`. Filenames are
  case-sensitive on romfs — keep them lowercase as the engine expects.
