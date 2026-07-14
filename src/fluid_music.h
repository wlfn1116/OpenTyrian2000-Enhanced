/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 *
 * fluid_music -- a small, self-contained SoundFont MIDI player built directly on
 * libfluidsynth. It owns a fluid_synth plus fluid's own audio driver, and runs a
 * sequencer thread that parses an in-memory Standard MIDI File and plays it,
 * looping mid-song at the "loopStart" marker (meta 0x06) exactly like
 * win_native_midi does for the OS synth.
 *
 * Owning playback (rather than SDL Mixer X's whole-file repeat) is what lets a looping
 * song jump straight back to its loop point with channel state intact. notes.md §Audio / MIDI.
 *
 * Input is the same in-memory SMF loudness.c already produces from music.mus via
 * midiproc. The real implementation is built only WITH_MIDI; other builds (and any
 * without FluidSynth) get no-op stubs so the file always compiles.
 */
#ifndef FLUID_MUSIC_H
#define FLUID_MUSIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Create the synth + audio driver and load `soundfont` (may be "" -> silent synth).
// `sample_rate` <= 0 defaults to 44100. Returns false if the synth or audio driver
// can't be created (caller then falls back to OPL).
bool fm_init(const char *soundfont, int sample_rate);
void fm_quit(void);   // stop playback and tear down synth + driver

// Start playing an in-memory SMF. `loop` repeats it -- at the loopStart marker if the
// song has one, else from the top -- until stopped; `on_finish` (may be NULL) is
// called from the sequencer thread when a non-looping song ends. Replaces any song
// already playing. Returns false if the SMF can't be played.
bool fm_play(const uint8_t *smf, size_t size, bool loop, void (*on_finish)(void));

void fm_stop(void);            // stop and silence
void fm_pause(void);           // silence and freeze the playback clock
void fm_resume(void);          // continue from where pause froze it
void fm_fade_out(uint32_t ms); // ramp music down over ms, then finish like a one-shot
bool fm_playing(void);         // true while a song is actively playing
void fm_set_volume(uint8_t vol255);  // master music volume, 0..255

bool fm_soundfont_loaded(void);  // true if the SoundFont actually loaded into the synth

#endif /* FLUID_MUSIC_H */
