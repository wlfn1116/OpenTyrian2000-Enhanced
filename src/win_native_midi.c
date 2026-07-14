/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 *
 * win_native_midi -- see win_native_midi.h. A deadlock-free replacement for SDL
 * Mixer X's Win32 native-MIDI backend: parses a Standard MIDI File into a single
 * time-ordered event list and plays it from our own thread with midiOutShortMsg,
 * using CALLBACK_NULL (no winmm callback thread, so a stop never waits on one).
 */
#include "win_native_midi.h"

#ifdef _WIN32

#include "SDL.h"

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "winmm.lib")

// One playable MIDI message at an absolute time. Meta events are consumed during
// parsing (tempo feeds the time map, the rest are dropped); only channel-voice
// messages and SysEx reach the synth.
typedef struct
{
	Uint64 time_us;   // absolute time from song start, microseconds
	Uint8  status;    // 0x80..0xEF channel message, or 0xF0 == SysEx (see sx_*)
	Uint8  d1, d2;    // data bytes (channel messages)
	Uint32 sx_off;    // SysEx: offset into g_sysex
	Uint32 sx_len;    // SysEx: byte count (0 for channel messages)
} WnmEvent;

static HMIDIOUT    g_hmo = NULL;
static SDL_Thread *g_thread = NULL;

static WnmEvent   *g_events = NULL;      // owned by the player thread while running
static size_t      g_event_count = 0;
static Uint8      *g_sysex = NULL;       // concatenated SysEx payloads
static bool        g_loop = false;
static void      (*g_on_finish)(void) = NULL;

// Loop point from the SMF's "loopStart" marker (meta 0x06), as written by the
// LDS->MIDI conversion for songs that jump back mid-song instead of restarting.
static bool        g_loop_marker = false;
static Uint64      g_loop_us = 0;        // loop target, microseconds from song start
static size_t      g_loop_idx = 0;       // first event at/after the loop target

static SDL_atomic_t g_stop, g_paused, g_active, g_vol_dirty, g_master_vol, g_fade_ms;

static Uint8  g_chan_vol[16];   // last song-set channel volume (CC7), pre-scaling
static double g_fade_scale = 1.0;  // thread-only: 1.0 normally, ramps to 0 on fade
static bool   g_period_set = false;

// --- MIDI output helpers (all called only from the player thread, or from the
//     main thread while the player thread is stopped/joined) ----------------

static void send_short(Uint8 status, Uint8 d1, Uint8 d2)
{
	if (g_hmo != NULL)
		midiOutShortMsg(g_hmo, (DWORD)status | ((DWORD)d1 << 8) | ((DWORD)d2 << 16));
}

// Apply master volume (and any active fade) to a raw 0..127 channel volume.
static Uint8 scaled_vol(int raw)
{
	int m = SDL_AtomicGet(&g_master_vol);
	double v = (double)raw * ((double)m / 255.0) * g_fade_scale;
	int iv = (int)(v + 0.5);
	if (iv < 0)   iv = 0;
	if (iv > 127) iv = 127;
	return (Uint8)iv;
}

// (Re)send CC7 (channel volume) to every channel so the master volume/fade takes
// effect even on channels the song never touched.
static void send_channel_volumes(void)
{
	for (int ch = 0; ch < 16; ++ch)
		send_short((Uint8)(0xB0 | ch), 7, scaled_vol(g_chan_vol[ch]));
}

static void all_notes_off(void)
{
	for (int ch = 0; ch < 16; ++ch)
	{
		send_short((Uint8)(0xB0 | ch), 120, 0);  // All Sound Off
		send_short((Uint8)(0xB0 | ch), 123, 0);  // All Notes Off
	}
}

// Put every channel into a clean, known state (used at song start and on loop).
static void reset_channels(void)
{
	for (int ch = 0; ch < 16; ++ch)
	{
		send_short((Uint8)(0xB0 | ch), 121, 0);  // Reset All Controllers
		send_short((Uint8)(0xB0 | ch), 123, 0);  // All Notes Off
		g_chan_vol[ch] = 100;                     // General MIDI default
	}
	send_channel_volumes();
}

static void send_event(const WnmEvent *e)
{
	if (e->status == 0xF0)  // SysEx
	{
		if (g_hmo == NULL || e->sx_len == 0)
			return;
		MIDIHDR hdr;
		memset(&hdr, 0, sizeof(hdr));
		hdr.lpData = (LPSTR)(g_sysex + e->sx_off);
		hdr.dwBufferLength = hdr.dwBytesRecorded = e->sx_len;
		if (midiOutPrepareHeader(g_hmo, &hdr, sizeof(hdr)) == MMSYSERR_NOERROR)
		{
			midiOutLongMsg(g_hmo, &hdr, sizeof(hdr));
			// CALLBACK_NULL: the driver sets MHDR_DONE when finished with the buffer.
			for (int i = 0; i < 200 && !(hdr.dwFlags & MHDR_DONE); ++i)
				SDL_Delay(1);
			midiOutUnprepareHeader(g_hmo, &hdr, sizeof(hdr));
		}
		return;
	}

	// Channel volume (CC7) is scaled by the master volume; remember the raw value
	// so a later master-volume change can be re-applied on top of it.
	if ((e->status & 0xF0) == 0xB0 && e->d1 == 7)
	{
		g_chan_vol[e->status & 0x0F] = e->d2;
		send_short(e->status, 7, scaled_vol(e->d2));
		return;
	}

	send_short(e->status, e->d1, e->d2);
}

// Restart at the loop target with the exact channel state the song had there. Reset every channel,
// then replay each state event before the target (in order, notes skipped) so every loop begins
// identically, like OPL. notes.md §Audio / MIDI.
static void restore_loop_state(void)
{
	all_notes_off();
	reset_channels();
	for (int ch = 0; ch < 16; ++ch)
		send_short((Uint8)(0xC0 | ch), 0, 0);  // program -> default (reset_channels doesn't reset it)

	size_t target = g_loop_marker ? g_loop_idx : 0;
	for (size_t i = 0; i < target; ++i)
	{
		Uint8 hi = g_events[i].status & 0xF0;
		if (hi == 0xB0 || hi == 0xC0 || hi == 0xE0 || g_events[i].status == 0xF0)
			send_event(&g_events[i]);  // control change / program / pitch bend / sysex
	}
}

// --- Player thread ---------------------------------------------------------

static int SDLCALL wnm_thread(void *userdata)
{
	(void)userdata;

	SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);  // keep event timing tight under game load

	g_fade_scale = 1.0;
	reset_channels();

	const Uint64 freq = SDL_GetPerformanceFrequency();
	Uint64 last = SDL_GetPerformanceCounter();
	Uint64 pos_us = 0;      // current playback position
	size_t ei = 0;          // next event to send
	bool silenced = false;  // are we currently holding notes off (paused)?

	bool   fading = false;
	Uint64 fade_accum_us = 0, fade_total_us = 0, fade_last_apply_us = 0;

	for (;;)
	{
		if (SDL_AtomicGet(&g_stop))
			break;

		const Uint64 now = SDL_GetPerformanceCounter();
		const Uint64 dt_us = (now - last) * 1000000ULL / freq;
		last = now;

		if (SDL_AtomicGet(&g_paused))
		{
			if (!silenced) { all_notes_off(); silenced = true; }
			SDL_Delay(5);
			continue;
		}
		silenced = false;

		if (SDL_AtomicGet(&g_vol_dirty))
		{
			send_channel_volumes();
			SDL_AtomicSet(&g_vol_dirty, 0);
		}

		// A fade request ramps g_fade_scale 1 -> 0 over the requested time, then
		// ends the song exactly like a finished one-shot (so callers waiting on
		// wnm_playing()/the finish hook proceed the same way they do for FluidSynth).
		const int req_fade = SDL_AtomicGet(&g_fade_ms);
		if (req_fade > 0)
		{
			fading = true;
			fade_total_us = (Uint64)req_fade * 1000ULL;
			fade_accum_us = 0;
			fade_last_apply_us = 0;
			SDL_AtomicSet(&g_fade_ms, 0);
		}
		if (fading)
		{
			fade_accum_us += dt_us;
			if (fade_accum_us >= fade_total_us)
			{
				all_notes_off();
				SDL_AtomicSet(&g_active, 0);
				if (g_on_finish) g_on_finish();
				break;
			}
			g_fade_scale = 1.0 - (double)fade_accum_us / (double)fade_total_us;
			if (fade_accum_us - fade_last_apply_us >= 25000)  // re-apply ~40x/sec
			{
				send_channel_volumes();
				fade_last_apply_us = fade_accum_us;
			}
		}

		pos_us += dt_us;

		while (ei < g_event_count && g_events[ei].time_us <= pos_us)
			send_event(&g_events[ei++]);

		if (ei >= g_event_count)  // reached the end of the song
		{
			if (g_loop && !fading)
			{
				restore_loop_state();  // reset + replay pre-target state (see above)
				ei = g_loop_marker ? g_loop_idx : 0;
				pos_us = g_loop_marker ? g_loop_us : 0;
				continue;
			}
			all_notes_off();
			SDL_AtomicSet(&g_active, 0);
			if (g_on_finish) g_on_finish();
			break;
		}

		SDL_Delay(1);
	}

	all_notes_off();
	return 0;
}

// --- Standard MIDI File parsing -------------------------------------------

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
	bool ok = true;

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

		WnmEvent *o = &g_events[g_event_count++];
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

	(void)ok;
	return true;
}

// --- Public API ------------------------------------------------------------

bool wnm_init(void)
{
	if (g_hmo != NULL)
		return true;
	static bool vol_defaulted = false;
	if (!vol_defaulted) { SDL_AtomicSet(&g_master_vol, 255); vol_defaulted = true; }  // don't clobber a set volume on re-open
	MMRESULT r = midiOutOpen(&g_hmo, MIDI_MAPPER, 0, 0, CALLBACK_NULL);
	if (r != MMSYSERR_NOERROR)
	{
		g_hmo = NULL;
		fprintf(stderr, "wnm: midiOutOpen failed (%u)\n", (unsigned)r);
		return false;
	}
	if (!g_period_set) { timeBeginPeriod(1); g_period_set = true; }  // 1 ms scheduler
	fprintf(stderr, "wnm: native MIDI player ready (built-in, MIDI mapper)\n");
	return true;
}

void wnm_stop(void)
{
	if (g_thread != NULL)
	{
		SDL_AtomicSet(&g_stop, 1);
		SDL_WaitThread(g_thread, NULL);  // returns promptly -- the thread polls g_stop
		g_thread = NULL;
	}
	SDL_AtomicSet(&g_active, 0);
	if (g_hmo != NULL)
		all_notes_off();
	free_song();
}

bool wnm_play(const uint8_t *smf, size_t size, bool loop, void (*on_finish)(void))
{
	wnm_stop();  // stop + join any current song and free it

	if (g_hmo == NULL && !wnm_init())
		return false;

	if (!parse_smf(smf, size))
	{
		fprintf(stderr, "wnm: could not parse SMF (%zu bytes)\n", size);
		return false;
	}

	g_loop = loop;
	g_on_finish = on_finish;
	g_fade_scale = 1.0;
	SDL_AtomicSet(&g_stop, 0);
	SDL_AtomicSet(&g_paused, 0);
	SDL_AtomicSet(&g_vol_dirty, 0);
	SDL_AtomicSet(&g_fade_ms, 0);
	SDL_AtomicSet(&g_active, 1);

	g_thread = SDL_CreateThread(wnm_thread, "wnm_midi", NULL);
	if (g_thread == NULL)
	{
		SDL_AtomicSet(&g_active, 0);
		free_song();
		fprintf(stderr, "wnm: failed to start player thread: %s\n", SDL_GetError());
		return false;
	}
	return true;
}

void wnm_quit(void)
{
	wnm_stop();
	if (g_hmo != NULL)
	{
		midiOutReset(g_hmo);
		midiOutClose(g_hmo);
		g_hmo = NULL;
	}
	if (g_period_set) { timeEndPeriod(1); g_period_set = false; }
}

void wnm_pause(void)  { SDL_AtomicSet(&g_paused, 1); }
void wnm_resume(void) { SDL_AtomicSet(&g_paused, 0); }
bool wnm_playing(void){ return SDL_AtomicGet(&g_active) != 0; }

void wnm_fade_out(uint32_t ms)
{
	if (ms == 0) { wnm_stop(); return; }
	if (wnm_playing())
		SDL_AtomicSet(&g_fade_ms, (int)ms);
}

void wnm_set_volume(uint8_t vol255)
{
	SDL_AtomicSet(&g_master_vol, vol255);
	SDL_AtomicSet(&g_vol_dirty, 1);
}

#else  /* !_WIN32 -- no native MIDI; loudness.c only calls these on Windows */

bool wnm_init(void) { return false; }
void wnm_quit(void) {}
bool wnm_play(const uint8_t *smf, size_t size, bool loop, void (*on_finish)(void))
{
	(void)smf; (void)size; (void)loop; (void)on_finish; return false;
}
void wnm_stop(void) {}
void wnm_pause(void) {}
void wnm_resume(void) {}
void wnm_fade_out(uint32_t ms) { (void)ms; }
bool wnm_playing(void) { return false; }
void wnm_set_volume(uint8_t vol255) { (void)vol255; }

#endif /* _WIN32 */
