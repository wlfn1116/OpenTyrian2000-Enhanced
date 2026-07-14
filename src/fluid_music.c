/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 *
 * fluid_music -- see fluid_music.h. A self-contained SoundFont MIDI player built on
 * libfluidsynth: it owns a fluid_synth plus fluid's own audio driver, parses a Standard
 * MIDI File into a time-ordered event list, and plays it from its own sequencer thread.
 * A looping song jumps back to its "loopStart" marker with channel state carried over the
 * seam, exactly like win_native_midi does for the OS synth.
 */
#include "fluid_music.h"

#ifdef WITH_MIDI

#include "SDL.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 5287)  // enum-type mismatch inside FluidSynth's own synth.h
#endif
#include <fluidsynth.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Master volume ceiling: 0..255 music volume maps to fluid_synth gain 0..FM_MAX_GAIN.
// Kept below 1.0 for headroom so busy SoundFonts don't clip. (Tunable.)
#define FM_MAX_GAIN 0.6f

// One playable MIDI message at an absolute time (mirrors win_native_midi's model).
// Meta events are consumed during parsing (tempo feeds the time map, loopStart marks
// the loop point, the rest are dropped); only channel-voice messages and SysEx play.
typedef struct
{
	Uint64 time_us;   // absolute time from song start, microseconds
	Uint8  status;    // 0x80..0xEF channel message, or 0xF0 == SysEx
	Uint8  d1, d2;    // data bytes (channel messages)
	Uint32 sx_off;    // SysEx: offset into g_sysex
	Uint32 sx_len;    // SysEx: byte count (0 for channel messages)
} FmEvent;

static fluid_settings_t     *g_settings = NULL;
static fluid_synth_t        *g_synth = NULL;
static fluid_audio_driver_t *g_driver = NULL;
static SDL_Thread           *g_thread = NULL;
static bool                  g_sf_loaded = false;

static FmEvent  *g_events = NULL;        // owned by the player thread while running
static size_t    g_event_count = 0;
static Uint8    *g_sysex = NULL;         // concatenated SysEx payloads
static bool      g_loop = false;
static void    (*g_on_finish)(void) = NULL;

// Loop point from the SMF's "loopStart" marker (meta 0x06), as written by the
// LDS->MIDI conversion for songs that jump back mid-song instead of restarting.
static bool      g_loop_marker = false;
static Uint64    g_loop_us = 0;          // loop target, microseconds from song start
static size_t    g_loop_idx = 0;         // first event at/after the loop target

static SDL_atomic_t g_stop, g_paused, g_active, g_fade_ms, g_master_vol;
static double g_fade_scale = 1.0;        // thread-only: 1.0 normally, ramps to 0 on fade
static float  g_last_gain = -1.0f;       // last gain pushed to the synth (avoid redundant calls)

// From lds_play.c: the jukebox fades a song once it loops. The old SDL Mixer X path
// set this from its finished hook; we set it when the sequencer wraps.
extern bool songlooped;

// --- FluidSynth output helpers (called from the sequencer thread, or from the main
//     thread while the sequencer thread is stopped/joined) ---------------------

// Push the current master volume (scaled by any active fade) to the synth's gain.
static void fm_apply_gain(void)
{
	if (g_synth == NULL)
		return;
	int m = SDL_AtomicGet(&g_master_vol);
	float gain = FM_MAX_GAIN * ((float)m / 255.0f) * (float)g_fade_scale;
	if (gain < 0.0f)
		gain = 0.0f;
	if (gain != g_last_gain)
	{
		fluid_synth_set_gain(g_synth, gain);
		g_last_gain = gain;
	}
}

// Release every channel's notes (used at pause, stop, and the loop seam). Sounding
// notes get their note-off; instruments/controllers are left intact.
static void fm_all_notes_off(void)
{
	if (g_synth == NULL)
		return;
	for (int ch = 0; ch < 16; ++ch)
		fluid_synth_all_notes_off(g_synth, ch);
}

static void fm_send_event(const FmEvent *e)
{
	if (e->status == 0xF0)  // SysEx
	{
		if (e->sx_len == 0)
			return;
		const Uint8 *d = g_sysex + e->sx_off;
		int len = (int)e->sx_len;
		// fluid_synth_sysex wants the payload without the leading 0xF0 / trailing 0xF7.
		if (len > 0 && d[0] == 0xF0) { ++d; --len; }
		if (len > 0 && d[len - 1] == 0xF7) { --len; }
		if (len > 0)
			fluid_synth_sysex(g_synth, (const char *)d, len, NULL, NULL, NULL, 0);
		return;
	}

	int ch = e->status & 0x0F;
	switch (e->status & 0xF0)
	{
	case 0x80: fluid_synth_noteoff(g_synth, ch, e->d1); break;
	case 0x90:
		if (e->d2 == 0) fluid_synth_noteoff(g_synth, ch, e->d1);  // note-on vel 0 == note-off
		else            fluid_synth_noteon(g_synth, ch, e->d1, e->d2);
		break;
	case 0xA0: fluid_synth_key_pressure(g_synth, ch, e->d1, e->d2); break;
	case 0xB0: fluid_synth_cc(g_synth, ch, e->d1, e->d2); break;      // CC0/CC32 bank handled internally
	case 0xC0: fluid_synth_program_change(g_synth, ch, e->d1); break;
	case 0xD0: fluid_synth_channel_pressure(g_synth, ch, e->d1); break;
	case 0xE0: fluid_synth_pitch_bend(g_synth, ch, e->d1 | (e->d2 << 7)); break;  // 14-bit, center 8192
	default: break;
	}
}

// Restart at the loop target (the loopStart marker, or the top if none): reset to a clean synth
// and re-establish the exact channel state the song had there. Replaying every state event before
// the target (in order, notes skipped) makes each loop begin identically, like OPL's register
// state. notes.md §Audio / MIDI.
static void fm_restore_loop_state(void)
{
	fm_all_notes_off();
	fluid_synth_system_reset(g_synth);  // programs->0, controllers/pitch->default

	size_t target = g_loop_marker ? g_loop_idx : 0;
	for (size_t i = 0; i < target; ++i)
	{
		Uint8 hi = g_events[i].status & 0xF0;
		if (hi == 0xB0 || hi == 0xC0 || hi == 0xE0 || g_events[i].status == 0xF0)
			fm_send_event(&g_events[i]);  // control change / program / pitch bend / sysex
	}
}

// --- Sequencer thread ------------------------------------------------------

static int SDLCALL fm_thread(void *userdata)
{
	(void)userdata;

	SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);  // keep event timing tight under game load

	g_fade_scale = 1.0;
	g_last_gain = -1.0f;
	fluid_synth_system_reset(g_synth);  // clean baseline (the synth is reused across songs)
	fm_apply_gain();

	const Uint64 freq = SDL_GetPerformanceFrequency();
	Uint64 last = SDL_GetPerformanceCounter();
	Uint64 pos_us = 0;      // current playback position
	size_t ei = 0;          // next event to send
	bool silenced = false;  // are we currently holding notes off (paused)?

	bool   fading = false;
	Uint64 fade_accum_us = 0, fade_total_us = 0;

	for (;;)
	{
		if (SDL_AtomicGet(&g_stop))
			break;

		const Uint64 now = SDL_GetPerformanceCounter();
		const Uint64 dt_us = (now - last) * 1000000ULL / freq;
		last = now;

		if (SDL_AtomicGet(&g_paused))
		{
			if (!silenced) { fm_all_notes_off(); silenced = true; }
			SDL_Delay(5);
			continue;
		}
		silenced = false;

		// A fade request ramps g_fade_scale 1 -> 0 over the requested time, then ends
		// the song exactly like a finished one-shot (callers waiting on fm_playing() /
		// the finish hook proceed the same way).
		const int req_fade = SDL_AtomicGet(&g_fade_ms);
		if (req_fade > 0)
		{
			fading = true;
			fade_total_us = (Uint64)req_fade * 1000ULL;
			fade_accum_us = 0;
			SDL_AtomicSet(&g_fade_ms, 0);
		}
		if (fading)
		{
			fade_accum_us += dt_us;
			if (fade_accum_us >= fade_total_us)
			{
				g_fade_scale = 0.0;
				fm_apply_gain();
				fm_all_notes_off();
				SDL_AtomicSet(&g_active, 0);
				if (g_on_finish) g_on_finish();
				break;
			}
			g_fade_scale = 1.0 - (double)fade_accum_us / (double)fade_total_us;
		}

		fm_apply_gain();  // pick up volume changes (and the fade ramp)

		pos_us += dt_us;

		while (ei < g_event_count && g_events[ei].time_us <= pos_us)
			fm_send_event(&g_events[ei++]);

		if (ei >= g_event_count)  // reached the end of the song
		{
			if (g_loop && !fading)
			{
				fm_restore_loop_state();  // reset + replay pre-target state (see above)
				ei = g_loop_marker ? g_loop_idx : 0;
				pos_us = g_loop_marker ? g_loop_us : 0;
				songlooped = true;  // as with OPL: the song has reached its loop
				continue;
			}
			fm_all_notes_off();
			SDL_AtomicSet(&g_active, 0);
			if (g_on_finish) g_on_finish();
			break;
		}

		SDL_Delay(1);
	}

	fm_all_notes_off();
	return 0;
}

// --- Standard MIDI File parsing (mirrors win_native_midi.c's parser) --------

// Growable scratch event list (absolute ticks) before the tempo map is applied.
typedef struct
{
	Uint32 tick;
	Uint32 seq;    // preserves order among same-tick events (stable merge)
	Uint8  type;   // 0 channel, 1 sysex, 2 tempo, 3 loopStart marker
	Uint8  status, d1, d2;
	Uint32 tempo;  // type 2
	Uint32 sx_off, sx_len;  // type 1
} TmpEv;

static Uint32 rd_u32(const Uint8 *p) { return ((Uint32)p[0] << 24) | ((Uint32)p[1] << 16) | ((Uint32)p[2] << 8) | p[3]; }
static Uint16 rd_u16(const Uint8 *p) { return (Uint16)(((Uint16)p[0] << 8) | p[1]); }

static Uint32 read_vlq(const Uint8 **pp, const Uint8 *end)
{
	Uint32 v = 0;
	while (*pp < end)
	{
		Uint8 b = *(*pp)++;
		v = (v << 7) | (b & 0x7F);
		if (!(b & 0x80))
			break;
	}
	return v;
}

static int cmp_tmp(const void *a, const void *b)
{
	const TmpEv *x = (const TmpEv *)a, *y = (const TmpEv *)b;
	if (x->tick != y->tick) return (x->tick < y->tick) ? -1 : 1;
	if (x->seq  != y->seq)  return (x->seq  < y->seq)  ? -1 : 1;
	return 0;
}

static void free_song(void)
{
	free(g_events); g_events = NULL; g_event_count = 0;
	free(g_sysex);  g_sysex = NULL;
	g_loop_marker = false;
	g_loop_us = 0;
	g_loop_idx = 0;
}

// Parse `data` into g_events/g_sysex. Returns false (and leaves nothing allocated)
// if the file isn't a usable SMF. Handles format 0/1 (tracks are merged by time).
static bool parse_smf(const Uint8 *data, size_t size)
{
	free_song();

	if (size < 14 || memcmp(data, "MThd", 4) != 0)
		return false;
	Uint32 hlen = rd_u32(data + 4);
	if (hlen < 6)
		return false;
	Uint16 ntrks = rd_u16(data + 10);
	Uint16 division = rd_u16(data + 12);

	const Uint8 *p = data + 8 + hlen;
	const Uint8 *end = data + size;

	TmpEv *tmp = NULL; size_t ntmp = 0, captmp = 0;
	Uint8 *sx = NULL;  size_t nsx = 0, capsx = 0;
	Uint32 seq = 0;

	for (int t = 0; t < ntrks && p + 8 <= end; ++t)
	{
		if (memcmp(p, "MTrk", 4) != 0)
			break;
		Uint32 tlen = rd_u32(p + 4);
		const Uint8 *tp = p + 8;
		const Uint8 *tend = tp + tlen;
		if (tend > end || tend < tp)  // clamp a bogus/overflowing length
			tend = end;
		p = tend;

		Uint32 tick = 0;
		Uint8 running = 0;
		while (tp < tend)
		{
			tick += read_vlq(&tp, tend);
			if (tp >= tend)
				break;

			Uint8 status = *tp;
			if (status & 0x80) { tp++; }
			else               { status = running; }  // running status
			if (status < 0x80)
				break;  // malformed

			if (status == 0xFF)  // meta event
			{
				if (tp >= tend) break;
				Uint8 mtype = *tp++;
				Uint32 mlen = read_vlq(&tp, tend);
				if (mlen > (Uint32)(tend - tp)) mlen = (Uint32)(tend - tp);
				if (mtype == 0x51 && mlen >= 3)
				{
					TmpEv e; memset(&e, 0, sizeof(e));
					e.tick = tick; e.seq = seq++; e.type = 2;
					e.tempo = ((Uint32)tp[0] << 16) | ((Uint32)tp[1] << 8) | tp[2];
					if (ntmp == captmp) { captmp = captmp ? captmp * 2 : 256; tmp = realloc(tmp, captmp * sizeof(*tmp)); }
					tmp[ntmp++] = e;
				}
				else if (mtype == 0x06 && mlen == 9 && memcmp(tp, "loopStart", 9) == 0)
				{
					// Marker written by the LDS->MIDI conversion at the song's
					// jump-back position; the player loops here, not to time 0.
					TmpEv e; memset(&e, 0, sizeof(e));
					e.tick = tick; e.seq = seq++; e.type = 3;
					if (ntmp == captmp) { captmp = captmp ? captmp * 2 : 256; tmp = realloc(tmp, captmp * sizeof(*tmp)); }
					tmp[ntmp++] = e;
				}
				tp += mlen;
				running = 0;  // meta cancels running status
				continue;
			}

			if (status == 0xF0 || status == 0xF7)  // SysEx
			{
				Uint32 slen = read_vlq(&tp, tend);
				if (slen > (Uint32)(tend - tp)) slen = (Uint32)(tend - tp);
				Uint32 off = (Uint32)nsx;
				Uint32 need = slen + (status == 0xF0 ? 1u : 0u);
				if (nsx + need > capsx) { while (nsx + need > capsx) capsx = capsx ? capsx * 2 : 256; sx = realloc(sx, capsx); }
				if (status == 0xF0) sx[nsx++] = 0xF0;
				memcpy(sx + nsx, tp, slen); nsx += slen;
				TmpEv e; memset(&e, 0, sizeof(e));
				e.tick = tick; e.seq = seq++; e.type = 1;
				e.sx_off = off; e.sx_len = (Uint32)nsx - off;
				if (ntmp == captmp) { captmp = captmp ? captmp * 2 : 256; tmp = realloc(tmp, captmp * sizeof(*tmp)); }
				tmp[ntmp++] = e;
				tp += slen;
				running = 0;
				continue;
			}

			// channel-voice message
			Uint8 hi = status & 0xF0;
			int nbytes = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
			Uint8 d1 = 0, d2 = 0;
			if (nbytes >= 1) { if (tp >= tend) break; d1 = *tp++; }
			if (nbytes >= 2) { if (tp >= tend) break; d2 = *tp++; }
			running = status;

			TmpEv e; memset(&e, 0, sizeof(e));
			e.tick = tick; e.seq = seq++; e.type = 0;
			e.status = status; e.d1 = d1; e.d2 = d2;
			if (ntmp == captmp) { captmp = captmp ? captmp * 2 : 256; tmp = realloc(tmp, captmp * sizeof(*tmp)); }
			tmp[ntmp++] = e;
		}
	}

	if (ntmp == 0) { free(tmp); free(sx); return false; }

	qsort(tmp, ntmp, sizeof(*tmp), cmp_tmp);

	g_events = malloc(ntmp * sizeof(*g_events));
	if (g_events == NULL) { free(tmp); free(sx); return false; }
	g_event_count = 0;
	g_sysex = sx;  // ownership transferred (sx_off values index into it)

	// Apply the tempo map to turn ticks into absolute microseconds.
	const bool smpte = (division & 0x8000) != 0;
	double us_per_tick = 0.0;   // SMPTE: fixed
	Uint32 ppqn = 0;
	if (smpte)
	{
		int fps = 256 - (division >> 8);      // -(int8) high byte: 24/25/29/30
		int tpf = division & 0xFF;
		if (fps <= 0) fps = 24;
		if (tpf <= 0) tpf = 1;
		us_per_tick = 1000000.0 / ((double)fps * (double)tpf);
	}
	else
	{
		ppqn = division ? division : 480;
	}

	Uint64 cur_us = 0;
	Uint32 last_tick = 0;
	Uint32 tempo = 500000;  // 120 BPM until told otherwise
	for (size_t i = 0; i < ntmp; ++i)
	{
		Uint32 dtick = tmp[i].tick - last_tick;
		last_tick = tmp[i].tick;
		if (smpte) cur_us += (Uint64)((double)dtick * us_per_tick);
		else       cur_us += (Uint64)dtick * tempo / ppqn;

		if (tmp[i].type == 2) { if (tmp[i].tempo) tempo = tmp[i].tempo; continue; }
		if (tmp[i].type == 3) { if (!g_loop_marker) { g_loop_marker = true; g_loop_us = cur_us; } continue; }

		FmEvent *o = &g_events[g_event_count++];
		o->time_us = cur_us;
		if (tmp[i].type == 1)
		{
			o->status = 0xF0; o->d1 = o->d2 = 0;
			o->sx_off = tmp[i].sx_off; o->sx_len = tmp[i].sx_len;
		}
		else
		{
			o->status = tmp[i].status; o->d1 = tmp[i].d1; o->d2 = tmp[i].d2;
			o->sx_off = 0; o->sx_len = 0;
		}
	}

	free(tmp);
	if (g_event_count == 0) { free_song(); return false; }

	// Resolve the loop marker to the first event at/after it (events sharing the
	// marker's timestamp are included regardless of their order in the file).
	if (g_loop_marker)
	{
		g_loop_idx = g_event_count;
		for (size_t i = 0; i < g_event_count; ++i)
		{
			if (g_events[i].time_us >= g_loop_us)
			{
				g_loop_idx = i;
				break;
			}
		}
		if (g_loop_idx >= g_event_count)  // marker past the last event: fall back to a full restart
			g_loop_marker = false;
	}

	return true;
}

// --- Public API ------------------------------------------------------------

bool fm_init(const char *soundfont, int sample_rate)
{
	if (g_synth != NULL)
		return true;  // already initialized

	static bool vol_defaulted = false;
	if (!vol_defaulted) { SDL_AtomicSet(&g_master_vol, 255); vol_defaulted = true; }  // don't clobber a set volume on re-open

	g_sf_loaded = false;
	if (sample_rate <= 0)
		sample_rate = 44100;

	g_settings = new_fluid_settings();
	if (g_settings == NULL)
	{
		fprintf(stderr, "fm: new_fluid_settings failed\n");
		return false;
	}

	fluid_settings_setnum(g_settings, "synth.sample-rate", (double)sample_rate);
	fluid_settings_setint(g_settings, "synth.threadsafe-api", 1);  // send events from our thread safely

	g_synth = new_fluid_synth(g_settings);
	if (g_synth == NULL)
	{
		fprintf(stderr, "fm: new_fluid_synth failed\n");
		delete_fluid_settings(g_settings); g_settings = NULL;
		return false;
	}

	if (soundfont != NULL && soundfont[0] != '\0')
	{
		if (fluid_synth_sfload(g_synth, soundfont, 1) != FLUID_FAILED)
		{
			g_sf_loaded = true;
			fluid_synth_program_reset(g_synth);  // channels pick up the new bank's presets
		}
		else
		{
			fprintf(stderr, "fm: could not load soundfont '%s'\n", soundfont);
		}
	}

	g_last_gain = -1.0f;
	fm_apply_gain();  // set initial gain from the master volume

	// FluidSynth spawns its own audio-render thread pulling from the synth. Try the
	// platform default first, then explicit Windows backends the DLL ships.
	g_driver = new_fluid_audio_driver(g_settings, g_synth);
	if (g_driver == NULL)
	{
		static const char *const drivers[] = { "wasapi", "dsound", "waveout" };
		for (size_t i = 0; i < SDL_arraysize(drivers) && g_driver == NULL; ++i)
		{
			fluid_settings_setstr(g_settings, "audio.driver", drivers[i]);
			g_driver = new_fluid_audio_driver(g_settings, g_synth);
		}
	}
	if (g_driver == NULL)
	{
		fprintf(stderr, "fm: could not open a FluidSynth audio driver\n");
		delete_fluid_synth(g_synth); g_synth = NULL;
		delete_fluid_settings(g_settings); g_settings = NULL;
		return false;
	}

	fprintf(stderr, "fm: FluidSynth ready (sr=%d, soundfont=%s%s)\n",
	        sample_rate,
	        (soundfont != NULL && soundfont[0] != '\0') ? soundfont : "(none)",
	        g_sf_loaded ? "" : "  [NOT LOADED]");
	return true;
}

void fm_stop(void)
{
	if (g_thread != NULL)
	{
		SDL_AtomicSet(&g_stop, 1);
		SDL_WaitThread(g_thread, NULL);  // returns promptly -- the thread polls g_stop
		g_thread = NULL;
	}
	SDL_AtomicSet(&g_active, 0);
	if (g_synth != NULL)
		fm_all_notes_off();
	free_song();
}

bool fm_play(const uint8_t *smf, size_t size, bool loop, void (*on_finish)(void))
{
	fm_stop();  // stop + join any current song and free it

	if (g_synth == NULL)
		return false;  // not initialized

	if (!parse_smf(smf, size))
	{
		fprintf(stderr, "fm: could not parse SMF (%zu bytes)\n", size);
		return false;
	}

	g_loop = loop;
	g_on_finish = on_finish;
	g_fade_scale = 1.0;
	g_last_gain = -1.0f;  // force a gain re-apply on the new thread
	SDL_AtomicSet(&g_stop, 0);
	SDL_AtomicSet(&g_paused, 0);
	SDL_AtomicSet(&g_fade_ms, 0);
	SDL_AtomicSet(&g_active, 1);

	g_thread = SDL_CreateThread(fm_thread, "fm_seq", NULL);
	if (g_thread == NULL)
	{
		SDL_AtomicSet(&g_active, 0);
		free_song();
		fprintf(stderr, "fm: failed to start sequencer thread: %s\n", SDL_GetError());
		return false;
	}
	return true;
}

void fm_quit(void)
{
	fm_stop();
	if (g_driver != NULL) { delete_fluid_audio_driver(g_driver); g_driver = NULL; }
	if (g_synth != NULL) { delete_fluid_synth(g_synth); g_synth = NULL; }
	if (g_settings != NULL) { delete_fluid_settings(g_settings); g_settings = NULL; }
	g_sf_loaded = false;
	g_last_gain = -1.0f;
}

void fm_pause(void)  { SDL_AtomicSet(&g_paused, 1); }
void fm_resume(void) { SDL_AtomicSet(&g_paused, 0); }
bool fm_playing(void){ return SDL_AtomicGet(&g_active) != 0; }

void fm_fade_out(uint32_t ms)
{
	if (ms == 0) { fm_stop(); return; }
	if (fm_playing())
		SDL_AtomicSet(&g_fade_ms, (int)ms);
}

void fm_set_volume(uint8_t vol255)
{
	SDL_AtomicSet(&g_master_vol, vol255);  // the sequencer thread applies it as gain
}

bool fm_soundfont_loaded(void) { return g_sf_loaded; }

#else  /* !WITH_MIDI -- no FluidSynth; loudness.c only calls these WITH_MIDI */

bool fm_init(const char *soundfont, int sample_rate) { (void)soundfont; (void)sample_rate; return false; }
void fm_quit(void) {}
bool fm_play(const uint8_t *smf, size_t size, bool loop, void (*on_finish)(void))
{
	(void)smf; (void)size; (void)loop; (void)on_finish; return false;
}
void fm_stop(void) {}
void fm_pause(void) {}
void fm_resume(void) {}
void fm_fade_out(uint32_t ms) { (void)ms; }
bool fm_playing(void) { return false; }
void fm_set_volume(uint8_t vol255) { (void)vol255; }
bool fm_soundfont_loaded(void) { return false; }

#endif /* WITH_MIDI */
