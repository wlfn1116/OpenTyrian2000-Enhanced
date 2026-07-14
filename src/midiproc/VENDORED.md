# Vendored: midiproc

C++ MIDI-processing library used to convert Tyrian's LDS (LOUDNESS/AdLib) songs
into Standard MIDI Files at load time (see `MIDIProcessorLDS.cpp`). OpenTyrian
calls the C API in `src/midiproc.h` (`MIDPROC_*`).

- Upstream: https://github.com/andyvand/midiproc
- Commit:   67213aafc05657a778000d85f1cbaac03eba5649
- License:  see `LICENSE`

## Build integration

Compiled directly into the executable (no separate DLL). Only used when the
project is built with `WITH_MIDI` (see `visualc/opentyrian.vcxproj`). Add
`src/*.cpp` here to the build as C++; `compat/os_compat.c` is a no-op on Windows
(`#if !defined(_WIN32)`) and is not compiled.

## Local modifications

- `src/midiproc.h`: added a `MIDIPROC_STATIC` guard so `EXPORT` expands to
  nothing when the library is compiled into the executable (avoids
  dllexport/dllimport on the `MIDPROC_*` symbols).
- `src/midiproc.h` + `src/midiproc.cpp`: added
  `MIDPROC_Container_SerializeAsSMFLoop()`, which serializes only the looped
  part of a song (loop point to end, with the pre-loop synth state replayed at
  timestamp 0 and an all-notes-off at the seam). Endlessly repeating that whole
  file equals looping the original at its loop point — used by `loudness.c` to
  make FluidSynth honor LDS loop points (our SDL Mixer X 2.6.0 FluidSynth
  backend can only restart a MIDI file from the beginning).
- `src/MIDIContainer.h`: added the trivial `GetTimeDivision()` accessor needed
  by the above.

The MSVC project compiles these files (see the dedicated ItemGroup in the
`.vcxproj`) with:

- `MIDIPROC_STATIC` — neutralizes `EXPORT` (compiled into the exe, not a DLL).
- `NOMINMAX` — `compat/os_compat.h` includes `<windows.h>`, whose `min`/`max`
  macros would otherwise clash with the C++ `std::min`/`std::max` used here.
  Note: do NOT define `WIN32_LEAN_AND_MEAN` — the XMI/RCP processors need
  `FOURCC`/`mmioFOURCC` from `<mmsystem.h>`, which lean mode excludes.
- `NDEBUG` + `/U_DEBUG` — the project links the release CRT (`/MD`) even in Debug
  (`UseDebugLibraries=false`); keeping `_DEBUG` out of these C++ TUs avoids the
  STL's debug iterators pulling the debug-only `_CrtDbgReport`.

These files are excluded from Win32 / non-`WITH_MIDI` configurations.
