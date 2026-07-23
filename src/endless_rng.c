/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 *
 * Endless mode: the run seed, the structural RNG, and the seed-select screen.
 *
 * One of the endless_*.c files that make up endless mode: endless.h is the public
 * interface, endless_internal.h the state and helpers the group shares.
 */

#include "endless.h"
#include "endless_internal.h"

#include "config.h"        // difficultyLevel, DIFFICULTY_*, player-independent globals
#include "font.h"          // draw_font_hv_shadow, JE_textWidth, fonts, alignment
#include "fonthand.h"      // JE_outText
#include "joystick.h"      // push_joysticks_as_keyboard
#include "keyboard.h"      // newkey/lastkey_scan/keysactive, service_SDL_events
#include "mouse.h"         // mouse_x/y, JE_mouseStart/Replace, mouseCursor
#include "mtrand.h"        // mt_rand
#include "nortsong.h"      // JE_playSampleNum, setDelay, wait_delayorinput, limit_render_fps
#include "palette.h"       // colors, fade_palette, fade_black
#include "picload.h"       // JE_loadPic
#include "player.h"        // player[]
#include "sndmast.h"       // S_SELECT, S_CURSOR, S_SPRING
#include "sprite.h"        // JE_loadCompShapes, enemySpriteSheets, shopSpriteSheet
#include "varz.h"          // eventRec, maxEvent, map* globals
#include "video.h"         // VGAScreen/VGAScreen2, JE_showVGA, output_vsync

#include <stdio.h>
#include <string.h>

// --- Run seed & structural RNG --------------------------------------------------
// Structure (level order, mutators, perks, shop stock) draws from a dedicated SplitMix64
// stream, isolated from the shared gameplay mt_rand (notes.md §Seeded structure RNG).
char   endlessRunSeed[ENDLESS_SEED_MAXLEN] = "";  // the run's seed string (also shown in-game)
static Uint64 endlessSeedHash = 0;   // FNV-1a hash of endlessRunSeed; the base for every per-zone reseed
static Uint64 endlessRngState = 0;   // live SplitMix64 state (structural stream)
Uint64 endlessEliteRngState = 0;  // live SplitMix64 state for the seeded elite/champion tier rolls

// FNV-1a over the string's bytes: maps any typed text to a 64-bit seed, so every seed is valid.
static Uint64 endlessHashString(const char *s)
{
	Uint64 h = 14695981039346656037ULL;  // FNV offset basis
	for (; *s != '\0'; ++s)
		h = (h ^ (Uint8)*s) * 1099511628211ULL;  // FNV prime
	return h;
}

// One SplitMix64 step on `state`: the endless RNG core, shared by the structural stream and the
// separate elite-tier stream below. Every use is `...Rand() % n`.
static Uint32 endlessSplitMixNext(Uint64 *state)
{
	Uint64 z = (*state += 0x9E3779B97F4A7C15ULL);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	z ^= z >> 31;
	return (Uint32)(z >> 32);
}

// Derive a fresh stream state for a (zone, phase): mix the run's seed hash with a salt so each
// (seed, salt) is an independent, reproducible sequence.
Uint64 endlessSplitMixSeed(Uint64 salt)
{
	Uint64 z = endlessSeedHash + (salt + 1) * 0x9E3779B97F4A7C15ULL;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	z ^= z >> 31;
	return z;
}

// The structural random: a drop-in for mt_rand() at the structural call sites (level order,
// course mutators, perk offers, shop stock). Re-derived per (zone, phase) by endlessReseed.
Uint32 endlessRand(void)
{
	return endlessSplitMixNext(&endlessRngState);
}

// Re-derive the structural stream for a fresh (zone, phase): each zone's generation is independent
// of what the player did in earlier zones. Called at each outpost and each level start (see
// endlessBetweenLevels / endlessRegenerateLevel).
void endlessReseed(Uint64 salt)
{
	endlessRngState = endlessSplitMixSeed(salt);
}

// The elite/champion tier random: its own seeded stream, per (seed, zone) in endlessResetElites;
// only the roll sequence is seed-fixed (notes.md §Seeded structure RNG).
Uint32 endlessEliteRand(void)
{
	return endlessSplitMixNext(&endlessEliteRngState);
}

void endlessSetSeed(const char *s)
{
	SDL_strlcpy(endlessRunSeed, (s != NULL) ? s : "", sizeof(endlessRunSeed));
	endlessSeedHash = endlessHashString(endlessRunSeed);
	endlessReseed(0);
}

const char *endlessSeedString(void)
{
	return endlessRunSeed;
}

// The pre-difficulty "choose your seed" screen (see endless.h), shown for a new Endless run.
// Styled after difficultySelect (the adjacent screen): pic-2 background, centered rows.
bool endlessSeedSelect(char *outSeed, size_t outN, bool *outHardcore)
{
	if (shopSpriteSheet.data == NULL)
		JE_loadCompShapes(&shopSpriteSheet, '1');  // mouse-pointer sprites (as difficultySelect does)

	char seed[ENDLESS_SEED_MAXLEN] = "";  // the seed being typed ("" => a random seed is rolled on Start)
	size_t len = 0;
	bool hardcore = false;  // the Hardcore toggle (default off); written to *outHardcore on Start

	enum { ROW_SEED, ROW_RANDOM, ROW_HARDCORE, ROW_START, ROW_COUNT };
	int selected = ROW_SEED;

	const int xCenter = 320 / 2;  // fixed 320 center (see difficultySelect / gameplaySelect rationale)
	const int yRows   = 82;
	const int dyRows  = 20;
	const int hRow    = 15;

	// Background + static titles: drawn once into VGAScreen2, copied to VGAScreen each frame.
	JE_loadPic(VGAScreen2, 2, false);
	draw_font_hv_shadow(VGAScreen2, xCenter, 20, "ENDLESS", large_font, centered, 15, -3, false, 2);
	draw_font_hv_shadow(VGAScreen2, xCenter, 54, "Type a seed for a repeatable run,",  small_font, centered, 15, 2, false, 1);
	draw_font_hv_shadow(VGAScreen2, xCenter, 64, "or leave it blank for a random one.", small_font, centered, 15, 2, false, 1);

	wait_noinput(true, true, true);

	bool first = true, done = false, commit = false;
	int prev_mx = mouse_x, prev_my = mouse_y;
	newkey = newmouse = new_text = false;  // input flags are consumed + cleared at the END of each pass
	while (!done)
	{
		memcpy(VGAScreen->pixels, VGAScreen2->pixels, (size_t)VGAScreen->pitch * VGAScreen->h);

		char seedRow[48];
		if (len > 0)
			snprintf(seedRow, sizeof(seedRow), "Seed: %s_", seed);
		else
			SDL_strlcpy(seedRow, "Seed: (random)", sizeof(seedRow));
		const char *label[ROW_COUNT] = { seedRow, "Randomize", hardcore ? "Hardcore: On" : "Hardcore: Off", "Start" };

		int rowW[ROW_COUNT];
		for (int i = 0; i < ROW_COUNT; ++i)
		{
			rowW[i] = JE_textWidth(label[i], normal_font);
			draw_font_hv_shadow(VGAScreen, xCenter, yRows + dyRows * i, label[i],
			                    normal_font, centered, 15, -4 + (i == selected ? 2 : 0), false, 2);
		}

		// A line under the rows spells out what the current Hardcore choice means, then the controls.
		draw_font_hv_shadow(VGAScreen, xCenter, yRows + dyRows * ROW_COUNT + 4,
		                    hardcore ? "Hardcore: no saving, and no second chances."
		                             : "Standard: save anytime; bail a level to re-outfit.",
		                    small_font, centered, 15, 2, false, 1);
		draw_font_hv_shadow(VGAScreen, xCenter, yRows + dyRows * ROW_COUNT + 18,
		                    "Up/Down Move    Enter Select    Esc Back", small_font, centered, 15, 4, false, 1);

		if (first)
		{
			fade_palette(colors, 10, 0, 255);
			first = false;
		}

		// Present at the render rate, smooth like every other menu: pump SDL events every frame and
		// end the pass the moment input arrives. JE_mouseStart calls service_SDL_events, so input
		// accumulates onto the edge flags (cleared only at the end of the pass, never mid-pass), so
		// no keystroke is dropped. setDelay bounds an idle pass; input ends it early.
		mouseCursor = MOUSE_POINTER_NORMAL;
		push_joysticks_as_keyboard();
		setDelay(1);
		for (;;)
		{
			JE_mouseStart();   // service_SDL_events(false): pump + accumulate, then prep the cursor
			JE_showVGA();
			JE_mouseReplace();
			if (newkey || newmouse || new_text || getDelayTicks() == 0)
				break;
			if (!output_vsync)
				limit_render_fps();
		}

		// Row hit-testing (centered rows): which row, if any, is the cursor over. Only a mouse
		// MOVE re-selects (so arrow-key navigation isn't yanked back to a resting cursor); a click
		// still acts on whatever row it lands on.
		int hover = -1;
		for (int i = 0; i < ROW_COUNT; ++i)
		{
			const int x0 = xCenter - rowW[i] / 2, x1 = xCenter + rowW[i] / 2;
			const int y0 = yRows + dyRows * i,    y1 = y0 + hRow;
			if (mouse_x >= x0 && mouse_x < x1 && mouse_y >= y0 && mouse_y < y1)
				hover = i;
		}
		const bool mouseMoved = (mouse_x != prev_mx || mouse_y != prev_my);
		prev_mx = mouse_x;
		prev_my = mouse_y;
		if (mouseMoved && hover >= 0 && hover != selected)
		{
			selected = hover;
			JE_playSampleNum(S_CURSOR);
		}

		bool activate = false;
		if (newmouse)
		{
			if (lastmouse_but == SDL_BUTTON_RIGHT)
			{
				JE_playSampleNum(S_SPRING);
				done = true;  // cancel
			}
			else if (hover >= 0)
			{
				selected = hover;
				activate = true;
			}
		}
		else if (newkey)
		{
			switch (lastkey_scan)
			{
			case SDL_SCANCODE_UP:
				selected = (selected == 0) ? ROW_COUNT - 1 : selected - 1;
				JE_playSampleNum(S_CURSOR);
				break;
			case SDL_SCANCODE_DOWN:
				selected = (selected + 1) % ROW_COUNT;
				JE_playSampleNum(S_CURSOR);
				break;
			case SDL_SCANCODE_BACKSPACE:
				if (len > 0)
					seed[--len] = '\0';
				selected = ROW_SEED;
				break;
			case SDL_SCANCODE_LEFT:
			case SDL_SCANCODE_RIGHT:
				// A natural left/right on the on/off Hardcore row flips it.
				if (selected == ROW_HARDCORE)
				{
					hardcore = !hardcore;
					JE_playSampleNum(S_CLICK);
				}
				break;
			case SDL_SCANCODE_RETURN:
			case SDL_SCANCODE_KP_ENTER:
				activate = true;
				break;
			case SDL_SCANCODE_ESCAPE:
				JE_playSampleNum(S_SPRING);
				done = true;  // cancel
				break;
			default:
				break;
			}
		}

		// Typed characters always edit the seed field -- any printable ASCII is a valid seed byte.
		if (new_text)
		{
			for (size_t ti = 0; last_text[ti] != '\0'; ++ti)
			{
				const unsigned char c = (unsigned char)last_text[ti];
				if (c >= 32 && c < 127 && len < sizeof(seed) - 1)
					seed[len++] = (char)c;
			}
			seed[len] = '\0';
			selected = ROW_SEED;
		}

		if (activate && !done)
		{
			if (selected == ROW_RANDOM)
			{
				snprintf(seed, sizeof(seed), "%lu", (unsigned long)(1u + mt_rand() % 999999999u));  // a fresh random seed to preview / share
				len = strlen(seed);
				JE_playSampleNum(S_SELECT);
			}
			else if (selected == ROW_HARDCORE)
			{
				hardcore = !hardcore;  // Enter / click on the toggle flips it, doesn't start the run
				JE_playSampleNum(S_CLICK);
			}
			else  // ROW_SEED or ROW_START: begin the run
			{
				if (len == 0)  // blank field => roll a random seed so a run always has one
					snprintf(seed, sizeof(seed), "%lu", (unsigned long)(1u + mt_rand() % 999999999u));
				SDL_strlcpy(outSeed, seed, outN);
				if (outHardcore)
					*outHardcore = hardcore;
				JE_playSampleNum(S_SELECT);
				commit = true;
				done = true;
			}
		}

		newkey = newmouse = new_text = false;  // consume this pass's input so nothing repeats next pass
	}

	fade_black(commit ? 10 : 15);  // fade out like difficultySelect, so the next screen fades in cleanly
	return commit;
}
