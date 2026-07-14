/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 *
 * Sony PlayStation Vita (VitaSDK) platform glue -- see vita_platform.h.
 */
#include "vita_platform.h"

#ifdef __vita__

#include "SDL.h"
#include "video.h"             // main_window, video_repeat_last_present()

#include <psp2/io/stat.h>      // sceIoMkdir
#include <psp2/sysmodule.h>    // sceSysmoduleLoadModule, SCE_SYSMODULE_IME
#include <psp2/common_dialog.h>// SceCommonDialogStatus
#include <psp2/ime_dialog.h>   // sceImeDialogGetStatus / sceImeDialogAbort / sceImeDialogTerm

void vita_platform_init(void)
{
	// app0:data (the read-only data bundled in the VPK) is auto-mounted by the loader,
	// and ux0:data always exists, so we only need to create our writable subfolder.
	// sceIoMkdir no-ops (EEXIST) if it is already there.
	sceIoMkdir(VITA_USER_DIR, 0777);

	// SDL's Vita text-input support drives the system IME dialog; make sure its sysmodule
	// is resident before the first SDL_StartTextInput(). Harmless if SDL also loads it.
	sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
}

bool vita_swkbd(char *out, size_t out_size, size_t max_len,
                const char *initial, const char *guide, bool numeric)
{
	(void)guide;
	(void)numeric;   // SDL's Vita IME exposes no type/guide option; it is a plain keyboard.

	if (out == NULL || out_size == 0)
		return false;

	// Start from the initial text so a cancel leaves the value unchanged, matching the
	// Switch keyboard's pre-fill behaviour. Names here are short; 256 is ample.
	char text[256];
	text[0] = '\0';
	if (initial != NULL)
		SDL_strlcpy(text, initial, sizeof(text));

	// Drain stale events so a queued keypress isn't mistaken for the IME's result.
	SDL_Event ev;
	while (SDL_PollEvent(&ev)) { }

	// The Vita IME is full-screen; still set a rect (some SDL paths gate showing on it).
	SDL_Rect rect = { 0, 0, 960, 544 };
	SDL_SetTextInputRect(&rect);

	// The IME is a Vita system common dialog; it holds the control pad until terminated, so drive
	// the loop off its native status and force it down afterwards. notes.md §Console ports / pitfalls.
	SDL_StartTextInput();

	bool everRunning = false;
	bool confirmed = false;
	const int maxFrames = 60 * 60;   // ~60s absolute safety cap, so we can never hard-hang

	for (int frame = 0; frame < maxFrames; ++frame)
	{
		video_repeat_last_present();

		while (SDL_PollEvent(&ev))
		{
			if (ev.type == SDL_TEXTINPUT)
				SDL_strlcpy(text, ev.text.text, sizeof(text));
			else if (ev.type == SDL_KEYDOWN &&
			         (ev.key.keysym.scancode == SDL_SCANCODE_RETURN ||
			          ev.key.keysym.scancode == SDL_SCANCODE_KP_ENTER))
				confirmed = true;   // Enter only; a cancel/close sends no RETURN
		}

		const SceCommonDialogStatus st = sceImeDialogGetStatus();
		if (st == SCE_COMMON_DIALOG_STATUS_RUNNING)
			everRunning = true;
		else if (everRunning || frame > 90)  // closed (confirm/cancel), or never opened
			break;

		SDL_Delay(16);
	}

	SDL_StopTextInput();

	if (sceImeDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING)
		sceImeDialogAbort();
	if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_NONE)
		sceImeDialogTerm();

	while (SDL_PollEvent(&ev)) { }   // drop the dialog's synthetic keys so they don't leak into the game

	if (max_len > 0 && SDL_strlen(text) > max_len)
		text[max_len] = '\0';
	SDL_strlcpy(out, text, out_size);

	return confirmed;
}

void vita_get_output_size(int *w, int *h)
{
	if (w != NULL)
		*w = 960;
	if (h != NULL)
		*h = 544;
}

#endif // __vita__
