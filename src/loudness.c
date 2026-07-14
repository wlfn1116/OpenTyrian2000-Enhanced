/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 * Copyright (C) 2007-2009  The OpenTyrian Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include "loudness.h"

#include "file.h"
#include "lds_play.h"
#include "nortsong.h"
#include "opentyr.h"
#include "params.h"

// Optional MIDI music backends -- FluidSynth or the OS synth (see loudness.h and
// notes.md §Audio / MIDI). OPL stays the default.
#ifdef WITH_MIDI
#include <midiproc.h>
#include "win_native_midi.h"  // NATIVE_MIDI plays through our own deadlock-free Win32 player
#include "fluid_music.h"       // FLUIDSYNTH plays through our own libfluidsynth player
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>  // FindFirstFileA, for soundfont autodetect
#endif
#endif

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define OUTPUT_QUALITY 4  // 44.1 kHz

int audioSampleRate = 0;

bool music_stopped = true;
unsigned int song_playing = 0;

bool audio_disabled = false, music_disabled = false, samples_disabled = false;

// Selected music synthesizer. Always defined (config/menu reference it); only the
// MIDI devices require a WITH_MIDI build, and init_audio() forces OPL otherwise.
MusicDevice music_device = OPL;
char soundfont[4096] = { 0 };

// True once FluidSynth is the active MIDI backend AND a readable SoundFont is
// configured -- i.e. the user's .sf2 will actually be heard. Surfaced in the
// Sound menu and crash log so the choice is confirmable (see init_audio()).
bool midi_soundfont_loaded = false;

const char *const music_device_names[MUSIC_DEVICE_MAX] = {
	"OPL3",
	"FluidSynth",
	"Native MIDI",
};

// Basename of the active SoundFont path, for status/UI display; "" when unset.
const char *soundfont_basename(void)
{
	const char *base = soundfont;
	for (const char *p = soundfont; *p != '\0'; ++p)
		if (*p == '/' || *p == '\\')
			base = p + 1;
	return base;
}

#ifdef WITH_MIDI
// music_device values that route through a MIDI backend rather than the OPL path.
// (FLUIDSYNTH==1, NATIVE_MIDI==2 are distinct bits, so this masks correctly.)
static const uint8_t IS_MIDI_DEVICE = FLUIDSYNTH | NATIVE_MIDI;

// Per-song converted SMF plus loop/duration metadata; the active MIDI backend
// (fluid_music or win_native_midi) owns playback (notes.md §Audio / MIDI).
typedef struct MidiData {
	Uint8 *data;
	Uint32 size;
	Uint32 duration;
	Uint32 loop_start;
	Uint32 loop_end;
} MidiData;
static MidiData *midi_data = NULL;
#endif

static SDL_AudioDeviceID audioDevice = 0;

static Uint8 musicVolume = 255;
static Uint8 sampleVolume = 255;

static const float volumeRange = 30.0f;  // dB

// Fixed point Q20.12; needs to be able to store (10 * INT16_MIN/MAX)
static Sint32 volumeFactorTable[256];
#define TO_FIXED(x) ((Sint32)((x) * (1 << 12)))
#define FIXED_TO_INT(x) ((Sint32)((x) >> 12))

// Twice the Loudness update rate (in updates/second).  In Tyrian, Loudness
// updates were performed at the same rate as the game timer, which varied
// depending on the game speed (~69.57 Hz at most game speeds).  We don't have
// the same limitations, so we'll keep the update rate constant, but we do want
// to stick to integer math, so we'll update at 69.5 Hz.
static const int ldsUpdate2Rate = 139;  // 69.5 * 2

static int samplesPerLdsUpdate;
static int samplesPerLdsUpdateFrac;

static int samplesUntilLdsUpdate = 0;
static int samplesUntilLdsUpdateFrac = 0;

static FILE *music_file = NULL;
static Uint32 *song_offset;
static Uint16 song_count = 0;

#define NO_SONG_PLAYING 0xFFFFFFFF

#define CHANNEL_COUNT 8
static const Sint16 *channelSamples[CHANNEL_COUNT];
static size_t channelSampleCount[CHANNEL_COUNT] = { 0 };
static Uint8 channelVolume[CHANNEL_COUNT];
#define CHANNEL_VOLUME_LEVELS 8

static void audioCallback(void *userdata, Uint8 *stream, int size);

static void load_song(unsigned int song_num);

#ifdef WITH_MIDI

// Called from the MIDI player's sequencer thread when a one-shot song ends (looping
// songs loop internally). The game polls `playing` to know when a jingle has ended.
static void midi_finished(void)
{
	playing = false;
	songlooped = false;
}

static bool file_readable(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return false;
	fclose(f);
	return true;
}

// Tear down whichever MIDI backend is active. Both are safe no-ops if their backend
// was never started, so this can run unconditionally on any audio teardown.
static void deinit_midi(void)
{
	midi_soundfont_loaded = false;
	fm_quit();   // stop + tear down the FluidSynth player
	wnm_quit();  // stop + close the native player
}

// Convert each LDS song in music.mus to an in-memory SMF (midiproc), recording
// loop/duration metadata (loop_end == 0xFFFFFFFF marks a one-shot song).
static void convert_midi_data(void)
{
	midi_data = malloc(song_count * sizeof(*midi_data));

	for (unsigned int i = 0; i < song_count; ++i)
	{
		memset(&midi_data[i], 0, sizeof(MidiData));
		midi_data[i].loop_end = 0xFFFFFFFF;

		Uint32 start = song_offset[i];
		Uint32 end = song_offset[i + 1];
		Uint32 size = end - start;
		Uint8 *buf = malloc(size);

		fseek(music_file, start, SEEK_SET);
		if (fread(buf, 1, size, music_file) != size)
		{
			fprintf(stderr, "warning: failed to read song %d\n", i + 1);
			free(buf);
			continue;
		}

		HMIDIContainer midi_container = MIDPROC_Container_Create();
		if (!MIDPROC_Process(buf, size, "lds", midi_container))
		{
			fprintf(stderr, "warning: failed to process song %d\n", i + 1);
			MIDPROC_Container_Delete(midi_container);
			free(buf);
			continue;
		}

		size_t midi_data_size = 0;
		MIDPROC_Container_SerializeAsSMF(midi_container, &(midi_data[i].data), &midi_data_size);
		midi_data[i].size = (Uint32)midi_data_size;
		if (midi_data[i].size == 0)
		{
			fprintf(stderr, "warning: failed to serialize song %d\n", i + 1);
			MIDPROC_Container_Delete(midi_container);
			free(buf);
			continue;
		}

		midi_data[i].duration = MIDPROC_Container_GetDuration(midi_container, 0, false);
		MIDPROC_Container_DetectLoops(midi_container, false, true, false, false);
		midi_data[i].loop_start = MIDPROC_Container_GetLoopBeginTimestamp(midi_container, 0, false);
		midi_data[i].loop_end = MIDPROC_Container_GetLoopEndTimestamp(midi_container, 0, false);

		MIDPROC_Container_Delete(midi_container);
		free(buf);
	}
}

// Start a song on the active MIDI backend. A looping song (loop_end <= duration)
// repeats forever from its loopStart marker; a one-shot plays once and trips midi_finished.
static void play_midi_song(unsigned int song_num)
{
	if (midi_data[song_num].data == NULL || midi_data[song_num].size == 0)
		return;

	bool loops = (midi_data[song_num].loop_end <= midi_data[song_num].duration);

	if (music_device == NATIVE_MIDI)
	{
		wnm_set_volume(musicVolume);
		if (!wnm_play(midi_data[song_num].data, midi_data[song_num].size, loops, midi_finished))
			return;
	}
	else  // FLUIDSYNTH
	{
		fm_set_volume(musicVolume);
		if (!fm_play(midi_data[song_num].data, midi_data[song_num].size, loops, midi_finished))
			return;
	}

	song_playing = song_num;
	playing = true;
	songlooped = false;
}

// True for the SoundFont file extensions FluidSynth can actually load. Excludes
// e.g. .sfz (a different format) and .sfArk (compressed), which it can't.
static bool is_soundfont_ext(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (dot == NULL)
		return false;
	return SDL_strcasecmp(dot, ".sf2") == 0
	    || SDL_strcasecmp(dot, ".sf3") == 0
	    || SDL_strcasecmp(dot, ".sf") == 0;
}

// If no SoundFont is configured, adopt the newest .sf/.sf2/.sf3 in the data dir
// so a dropped-in bank just works (notes.md §Audio / MIDI).
static void autodetect_soundfont(void)
{
	if (soundfont[0] != '\0')
		return;  // an explicit config/CLI soundfont already chosen -- respect it
#ifdef _WIN32
	char pattern[4096];
	snprintf(pattern, sizeof(pattern), "%s/*.sf*", data_dir());

	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;

	char best[256] = { 0 };
	FILETIME bestTime = { 0, 0 };
	do
	{
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		if (!is_soundfont_ext(fd.cFileName))
			continue;  // *.sf* also matches .sfz/.sfark etc.
		if (best[0] == '\0' || CompareFileTime(&fd.ftLastWriteTime, &bestTime) > 0)
		{
			SDL_strlcpy(best, fd.cFileName, sizeof(best));
			bestTime = fd.ftLastWriteTime;
		}
	} while (FindNextFileA(h, &fd));
	FindClose(h);

	if (best[0] != '\0')
		snprintf(soundfont, sizeof(soundfont), "%s/%s", data_dir(), best);
#endif
}

// Re-anchor a stale configured SoundFont path to data_dir() (survives moved installs
// and CWD changes); clear it if unresolvable so autodetect runs (notes.md §Audio / MIDI).
static void resolve_soundfont(void)
{
	if (soundfont[0] == '\0')
		return;
	if (file_readable(soundfont))
		return;  // resolves already (absolute path, or relative with a matching CWD)

	char candidate[4096];
	snprintf(candidate, sizeof(candidate), "%s/%s", data_dir(), soundfont_basename());
	if (file_readable(candidate))
	{
		fprintf(stderr, "midi: soundfont '%s' re-anchored to '%s'\n", soundfont, candidate);
		SDL_strlcpy(soundfont, candidate, sizeof(soundfont));
	}
	else
	{
		fprintf(stderr, "midi: configured soundfont '%s' not found; will autodetect\n", soundfont);
		soundfont[0] = '\0';
	}
}

#endif /* WITH_MIDI */

bool init_audio(void)
{
#ifndef WITH_MIDI
	music_device = OPL;  // no MIDI support compiled in
#else
	#ifdef NO_NATIVE_MIDI
	if (music_device == NATIVE_MIDI)
		music_device = FLUIDSYNTH;
	#endif
#endif

	if (audio_disabled)
		return false;

	SDL_AudioSpec ask, got;

	ask.freq = 11025 * OUTPUT_QUALITY;
	ask.format = AUDIO_S16SYS;
	ask.channels = 1;
	ask.samples = 256 * OUTPUT_QUALITY; // ~23 ms
	ask.callback = audioCallback;

	if (SDL_InitSubSystem(SDL_INIT_AUDIO))
	{
		fprintf(stderr, "error: failed to initialize SDL audio: %s\n", SDL_GetError());
		audio_disabled = true;
		return false;
	}

	int allowedChanges = SDL_AUDIO_ALLOW_FREQUENCY_CHANGE;
#if SDL_VERSION_ATLEAST(2, 0, 9)
	allowedChanges |= SDL_AUDIO_ALLOW_SAMPLES_CHANGE;
#endif
	audioDevice = SDL_OpenAudioDevice(/*device*/ NULL, /*iscapture*/ 0, &ask, &got, allowedChanges);

	if (audioDevice == 0)
	{
		fprintf(stderr, "error: SDL failed to open audio device: %s\n", SDL_GetError());
		audio_disabled = true;
		return false;
	}

	audioSampleRate = got.freq;

	samplesPerLdsUpdate = 2 * (audioSampleRate / ldsUpdate2Rate);
	samplesPerLdsUpdateFrac = 2 * (audioSampleRate % ldsUpdate2Rate);

	volumeFactorTable[0] = 0;
	for (size_t i = 1; i < 256; ++i)
		volumeFactorTable[i] = TO_FIXED(powf(10, (255 - i) * (-volumeRange / (20.0f * 255))));

	opl_init();

#ifdef WITH_MIDI
	if (music_device == FLUIDSYNTH)
	{
		resolve_soundfont();     // re-anchor a configured path to data_dir() (survives moves/CWD)
		autodetect_soundfont();  // if none is configured, adopt one from data_dir()
		if (!fm_init(soundfont, audioSampleRate))
		{
			fprintf(stderr, "error: failed to initialize FluidSynth, falling back to OPL...\n");
			music_device = OPL;
		}
		else
		{
			midi_soundfont_loaded = fm_soundfont_loaded();  // confirm the .sf2 was heard
		}
	}
	else if (music_device == NATIVE_MIDI)
	{
		// Our own Win32 player; ignores SoundFonts (uses the OS synth).
		if (!wnm_init())
		{
			fprintf(stderr, "error: failed to open native MIDI, falling back to OPL...\n");
			music_device = OPL;
		}
	}
#endif

	SDL_PauseAudioDevice(audioDevice, 0); // unpause

	return true;
}

// Tear down and re-open the audio device(s), resuming whatever song was playing.
// Used when the music device or soundfont changes at runtime (see the menu).
bool restart_audio(void)
{
	if (audio_disabled)
		return false;

	unsigned int prev_song = song_playing;

	deinit_audio();
	if (!init_audio())
		return false;

	if (prev_song != NO_SONG_PLAYING)
		play_song(prev_song);

	return true;
}

// Renders OPL music (when OPL is selected) plus the sample channels. MIDI backends
// play on their own thread, so music stays silent here and samples still mix.
static void audioCallback(void *userdata, Uint8 *stream, int size)
{
	(void)userdata;

	Sint16 *const samples = (Sint16 *)stream;
	const int samplesCount = size / sizeof (Sint16);

	if ((music_device == OPL) && !music_disabled && !music_stopped)
	{
		Sint16 *remaining = samples;
		int remainingCount = samplesCount;
		while (remainingCount > 0)
		{
			if (samplesUntilLdsUpdate == 0)
			{
				lds_update();

				// The number of samples that should be produced per Loudness
				// update is not an integer, but we can only produce an integer
				// number of samples, so we accumulate the fractional samples
				// until it amounts to a whole sample.
				samplesUntilLdsUpdate += samplesPerLdsUpdate;
				samplesUntilLdsUpdateFrac += samplesPerLdsUpdateFrac;
				if (samplesUntilLdsUpdateFrac >= ldsUpdate2Rate)
				{
					samplesUntilLdsUpdate += 1;
					samplesUntilLdsUpdateFrac -= ldsUpdate2Rate;
				}
			}

			int count = MIN(samplesUntilLdsUpdate, remainingCount);

			opl_update(remaining, count);

			remaining += count;
			remainingCount -= count;

			samplesUntilLdsUpdate -= count;
		}
	}
	else
	{
		for (int i = 0; i < samplesCount; ++i)
			samples[i] = 0;
	}

	Sint32 musicVolumeFactor = volumeFactorTable[musicVolume];
	musicVolumeFactor *= 2;  // OPL emulator is too quiet

	if (samples_disabled && !music_disabled)
	{
		// Mix music
		Sint16 *remaining = samples;
		int remainingCount = samplesCount;
		while (remainingCount > 0)
		{
			Sint32 sample = *remaining * musicVolumeFactor;

			sample = FIXED_TO_INT(sample);
			*remaining = MIN(MAX(INT16_MIN, sample), INT16_MAX);

			remaining += 1;
			remainingCount -= 1;
		}
	}
	else if (!samples_disabled)
	{
		Sint32 sampleVolumeFactor = volumeFactorTable[sampleVolume];
		Sint32 sampleVolumeFactors[CHANNEL_VOLUME_LEVELS];
		for (int i = 0; i < CHANNEL_VOLUME_LEVELS; ++i)
			sampleVolumeFactors[i] = sampleVolumeFactor * (i + 1) / CHANNEL_VOLUME_LEVELS;

		// Mix music and channels
		Sint16 *remaining = samples;
		int remainingCount = samplesCount;
		while (remainingCount > 0)
		{
			Sint32 sample = *remaining * musicVolumeFactor;

			for (size_t i = 0; i < CHANNEL_COUNT; ++i)
			{
				if (channelSampleCount[i] > 0)
				{
					sample += *channelSamples[i] * sampleVolumeFactors[channelVolume[i]];

					channelSamples[i] += 1;
					channelSampleCount[i] -= 1;
				}
			}

			sample = FIXED_TO_INT(sample);
			*remaining = MIN(MAX(INT16_MIN, sample), INT16_MAX);

			remaining += 1;
			remainingCount -= 1;
		}
	}
}

void deinit_audio(void)
{
	if (audio_disabled)
		return;

#ifdef WITH_MIDI
	deinit_midi();  // safe no-op unless a MIDI device is open
#endif

	if (audioDevice != 0)
	{
		SDL_PauseAudioDevice(audioDevice, 1); // pause
		SDL_CloseAudioDevice(audioDevice);
		audioDevice = 0;
	}

	SDL_QuitSubSystem(SDL_INIT_AUDIO);

	song_playing = NO_SONG_PLAYING;
	music_stopped = true;

	memset(channelSampleCount, 0, sizeof channelSampleCount);

	lds_free();
}

void load_music(void)  // FKA NortSong.loadSong
{
	if (music_file == NULL)
	{
		music_file = dir_fopen_die(data_dir(), "music.mus", "rb");

		fread_u16_die(&song_count, 1, music_file);

		song_offset = malloc((song_count + 1) * sizeof(*song_offset));

		fread_u32_die(song_offset, song_count, music_file);

		song_offset[song_count] = ftell_eof(music_file);

#ifdef WITH_MIDI
		convert_midi_data();
#endif
	}
}

static void load_song(unsigned int song_num)  // FKA NortSong.loadSong
{
	if (song_num < song_count)
	{
		unsigned int song_size = song_offset[song_num + 1] - song_offset[song_num];
		lds_load(music_file, song_offset[song_num], song_size);
	}
	else
	{
		fprintf(stderr, "warning: failed to load song %d\n", song_num + 1);
	}
}

void play_song(unsigned int song_num)  // FKA NortSong.playSong
{
	if (song_num >= song_count)
	{
		fprintf(stderr, "warning: song %d does not exist\n", song_num + 1);
		return;
	}
	if (audio_disabled)
		return;

#ifdef WITH_MIDI
	if (music_device & IS_MIDI_DEVICE)
	{
		if (music_disabled)
			return;
		if (song_num != song_playing)
		{
			play_midi_song(song_num);  // start the requested song
		}
		else
		{
			// Same song already selected: just make sure it isn't paused; do not
			// restart it. The game re-issues play_song(jingle) every tick during the
			// level-end sequence, so restarting whenever the backend reports "not
			// playing" replayed the finished jingle over and over (the "plays more
			// than once" bug). OPL is idempotent here for exactly this reason.
			if (music_device == NATIVE_MIDI) wnm_resume();  // was paused by stop_song
			else                             fm_resume();
		}
		return;
	}
#endif

	if (song_num != song_playing)
	{
		SDL_LockAudioDevice(audioDevice);

		music_stopped = true;

		SDL_UnlockAudioDevice(audioDevice);

		load_song(song_num);

		song_playing = song_num;
	}

	SDL_LockAudioDevice(audioDevice);

	music_stopped = false;

	SDL_UnlockAudioDevice(audioDevice);
}

void restart_song(void)  // FKA Player.selectSong(1)
{
	if (audio_disabled)
		return;

#ifdef WITH_MIDI
	if (music_device & IS_MIDI_DEVICE)
	{
		if (!music_disabled && song_playing < song_count)
			play_midi_song(song_playing);  // replay from the start
		return;
	}
#endif

	SDL_LockAudioDevice(audioDevice);

	lds_rewind();

	music_stopped = false;

	SDL_UnlockAudioDevice(audioDevice);
}

void stop_song(void)  // FKA Player.selectSong(0)
{
	if (audio_disabled)
		return;

#ifdef WITH_MIDI
	if (music_device & IS_MIDI_DEVICE)
	{
		if (music_device == NATIVE_MIDI) wnm_pause();
		else                             fm_pause();
		return;
	}
#endif

	SDL_LockAudioDevice(audioDevice);

	music_stopped = true;

	SDL_UnlockAudioDevice(audioDevice);
}

void fade_song(void)  // FKA Player.selectSong($C001)
{
	if (audio_disabled)
		return;

#ifdef WITH_MIDI
	if (music_device & IS_MIDI_DEVICE)
	{
		// Ramps the music down over 6 s, then trips the finish hook (playing=false),
		// exactly like the OPL lds_fade path the callers expect.
		if (music_device == NATIVE_MIDI) wnm_fade_out(6000);
		else                             fm_fade_out(6000);
		return;
	}
#endif

	SDL_LockAudioDevice(audioDevice);

	lds_fade(1);

	SDL_UnlockAudioDevice(audioDevice);
}

void set_volume(Uint8 musicVolume_, Uint8 sampleVolume_)  // FKA NortSong.setVol and Player.setVol
{
	if (audio_disabled)
		return;

	SDL_LockAudioDevice(audioDevice);

	musicVolume = musicVolume_;
	sampleVolume = sampleVolume_;

	SDL_UnlockAudioDevice(audioDevice);

#ifdef WITH_MIDI
	if (music_device == NATIVE_MIDI)       wnm_set_volume(musicVolume_);
	else if (music_device & IS_MIDI_DEVICE) fm_set_volume(musicVolume_);
#endif
}

// Toggle music on/off. For OPL the audio callback simply stops rendering while
// music_disabled is set; the MIDI backends play on their own device/thread, so
// pause/resume them explicitly here.
void set_music_disabled(bool disabled)
{
	music_disabled = disabled;

#ifdef WITH_MIDI
	if (!audio_disabled && (music_device & IS_MIDI_DEVICE))
	{
		if (music_device == NATIVE_MIDI)
		{
			if (disabled) wnm_pause(); else wnm_resume();
		}
		else
		{
			if (disabled) fm_pause(); else fm_resume();
		}
	}
#endif
}

void multiSamplePlay(const Sint16 *samples, size_t sampleCount, Uint8 chan, Uint8 vol)  // FKA Player.multiSamplePlay
{
	assert(chan < CHANNEL_COUNT);
	assert(vol < CHANNEL_VOLUME_LEVELS);

	if (audio_disabled || samples_disabled)
		return;

	SDL_LockAudioDevice(audioDevice);

	channelSamples[chan] = samples;
	channelSampleCount[chan] = sampleCount;
	channelVolume[chan] = vol;

	SDL_UnlockAudioDevice(audioDevice);
}

// Silence every sample channel. Channels hold pointers into the sound-sample buffers,
// so call this (on the main thread) before those buffers are freed or reallocated at
// runtime (e.g. reloading Christmas voices), else the audio callback reads freed memory.
void stop_sample_channels(void)
{
	if (audio_disabled)
		return;

	SDL_LockAudioDevice(audioDevice);

	for (size_t i = 0; i < CHANNEL_COUNT; ++i)
		channelSampleCount[i] = 0;

	SDL_UnlockAudioDevice(audioDevice);
}
