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
#include "video.h"

#include "config.h"
#include "keyboard.h"
#include "mouse.h"
#include "opentyr.h"
#include "palette.h"
#include "video_scale.h"

#include "console_platform.h"  // console_get_output_size()

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

const char *const scaling_mode_names[ScalingMode_MAX] = {
	"Center",
	"Integer",
	"Widescreen",
	"Classic",
};

int fullscreen_display;
bool output_vsync = false;  // present in sync with the display's refresh rate (off by default)
ScalingMode scaling_mode = SCALE_WIDESCREEN;  // fill the screen at true 16:9 by default

// Sub-pixel supersampling factor; 0 = Auto (follow the scaler). See video.h.
int render_supersample = 0;

// Upscale filter for the supersampled present path; Sharp = crisp nearest pixels
// (the oldschool fullscreen look), Smooth = lightly antialiased, None = raw
// unfiltered nearest (the default). See video.h.
int render_supersample_filter = SS_FILTER_NONE;

static void update_native_scaler_dims(void);

// Resolve the configured supersample factor: Auto follows the scaler's integer
// factor (2x/Scale2x/hq2x -> 2, ...), so the sub-pixel buffer is exactly the
// resolution the user already chose to run the game at. The Native scaler's output
// ratio is fractional; round it UP so the sub-pixel buffer covers every screen
// pixel — the present pass averages the slight overshoot back down to exact size.
int effective_supersample(void)
{
	if (!smoothMotion)
		return 1;

#ifdef __vita__
	// The Vita GPU can't sustain the NxN sub-pixel present (render at N* then downscale). Force
	// 1x: Smooth Motion's render-rate interpolation still runs (even ship/background motion at
	// 60fps), just at native resolution, which the SGX can handle. Mirrors the cap the Switch
	// port used before its faster GPU made supersampling viable there.
	return 1;
#else
	int factor = render_supersample;
	if (factor == 0)
	{
		if (scaler_is_native(scaler))
		{
			update_native_scaler_dims();
			const int fw = (scalers[scaler].width + vga_width - 1) / vga_width;
			const int fh = (scalers[scaler].height + vga_height - 1) / vga_height;
			factor = fw > fh ? fw : fh;
		}
		else
			factor = scalers[scaler].width / vga_width;

		// A 1x scaler would resolve Auto to 1x — no sub-pixel motion at all, a
		// pointless state — so Auto always supersamples at least 2x.
		if (factor < 2)
			factor = 2;
	}
	if (factor < 1)
		factor = 1;
	else if (factor > RENDER_SUPERSAMPLE_MAX)
		factor = RENDER_SUPERSAMPLE_MAX;
	return factor;
#endif
}

bool show_fps = false;
int current_fps = 0;
static SDL_Rect last_output_rect = { 0, 0, vga_width, vga_height };

// Window size (the same quantity calc_dst_render_rect fits the frame into) at the last
// present. The event pump compares against this to notice a resolution change it must
// repaint for; see video_repaint_if_stale().
static int last_present_w = 0, last_present_h = 0;

SDL_Surface *VGAScreen, *VGAScreenSeg;
SDL_Surface *VGAScreen2;
SDL_Surface *game_screen;
static SDL_Surface* menu_screen;

static int current_x_offset = MENU_X_OFFSET;

SDL_Window *main_window = NULL;
static SDL_Renderer *main_window_renderer = NULL;
SDL_PixelFormat *main_window_tex_format = NULL;
static SDL_Texture *main_window_texture = NULL;

// Textures for the supersampled present path, recreated on size change; pass
// selection (prescale / halving / direct copy) is explained in present_hi().
static SDL_Texture *hi_texture = NULL;  // streaming: the palette-converted hi frame
static int hi_texture_w = 0, hi_texture_h = 0;
static SDL_Texture *hi_stage = NULL;    // render target for the prescale/halving pass
static int hi_stage_w = 0, hi_stage_h = 0;

static ScalerFunction scaler_function;

#ifndef __vita__  // side-gradient cache -- unused on Vita (plain black pillarbox)
static Uint8 gradient_cache[256][MENU_X_OFFSET];
static Uint32 last_gradient_palette[256];
static bool gradient_cache_valid = false;
#endif

static void init_renderer(void);
static void deinit_renderer(void);
static void init_texture(void);
static void deinit_texture(void);

static int window_get_display_index(void);
static void window_center_in_display(int display_index);
static void native_windowed_size(int *out_w, int *out_h);
static void calc_dst_render_rect(SDL_Surface *src_surface, SDL_Rect *dst_rect);
static void scale_and_flip(SDL_Surface *);
static void blit_with_offset(SDL_Surface* src, SDL_Surface* dst, int x_offset);
#ifndef __vita__
static Uint8 nearest_palette_index(Uint8 r, Uint8 g, Uint8 b);
static void update_gradient_cache(void);
#endif

void init_video(void)
{
	if (SDL_WasInit(SDL_INIT_VIDEO))
		return;

	if (SDL_InitSubSystem(SDL_INIT_VIDEO) == -1)
	{
		fprintf(stderr, "error: failed to initialize SDL video: %s\n", SDL_GetError());
		exit(1);
	}

	// Create the software surfaces that the game renders to. These are all 356x200x8 (16:9)
	// regardless of the window size or monitor resolution.
	VGAScreen = VGAScreenSeg = SDL_CreateRGBSurface(0, vga_width, vga_height, 8, 0, 0, 0, 0);
	VGAScreen2 = SDL_CreateRGBSurface(0, vga_width, vga_height, 8, 0, 0, 0, 0);
	game_screen = SDL_CreateRGBSurface(0, vga_width, vga_height, 8, 0, 0, 0, 0);
	menu_screen = SDL_CreateRGBSurface(0, vga_width, vga_height, 8, 0, 0, 0, 0);

	// The game code writes to surface->pixels directly without locking, so make sure that we
	// indeed created software surfaces that support this.
	assert(!SDL_MUSTLOCK(VGAScreen));
	assert(!SDL_MUSTLOCK(VGAScreen2));
	assert(!SDL_MUSTLOCK(game_screen));
	assert(!SDL_MUSTLOCK(menu_screen));

	JE_clr256(VGAScreen);

	// Create the window with a temporary initial size, hidden until we set up the
	// scaler and find the true window size
	int win_w = vga_width, win_h = vga_height;
#if defined(__SWITCH__) || defined(__vita__)
	// On the consoles the single app-layer always fills the panel, so the window IS the
	// screen: create it at the native output size (Switch 720p/1080p by dock state; Vita a
	// fixed 960x544) so the buffer matches the panel 1:1 from the first frame. Keeping the
	// window RESIZABLE (below) lets the switch-sdl2 driver re-track it on dock/undock; on
	// Vita the size is constant.
	console_get_output_size(&win_w, &win_h);
#endif
	main_window = SDL_CreateWindow(opentyrian_str,
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		win_w, win_h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);

	if (main_window == NULL)
	{
		fprintf(stderr, "error: failed to create window: %s\n", SDL_GetError());
		exit(EXIT_FAILURE);
	}

	reinit_fullscreen(fullscreen_display);
	init_renderer();
	init_texture();
	init_scaler(scaler);

	SDL_ShowWindow(main_window);

	SDL_SetRenderDrawColor(main_window_renderer, 0, 0, 0, 255);
	SDL_RenderClear(main_window_renderer);
	SDL_RenderPresent(main_window_renderer);

	// Seed the repaint watchdog with the starting window size so the first event-pump
	// poll doesn't see a spurious change (see video_repaint_if_stale()).
	SDL_GetWindowSize(main_window, &last_present_w, &last_present_h);
}

void deinit_video(void)
{
	deinit_texture();
	deinit_renderer();

	SDL_DestroyWindow(main_window);

	SDL_FreeSurface(VGAScreenSeg);
	SDL_FreeSurface(VGAScreen2);
	SDL_FreeSurface(game_screen);
	SDL_FreeSurface(menu_screen);

	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

static void init_renderer(void)
{
	Uint32 flags = output_vsync ? SDL_RENDERER_PRESENTVSYNC : 0;
	main_window_renderer = SDL_CreateRenderer(main_window, -1, flags);

	if (main_window_renderer == NULL && flags != 0)
	{
		// The driver may be unable to provide a vsync'd renderer; fall back.
		output_vsync = false;
		main_window_renderer = SDL_CreateRenderer(main_window, -1, 0);
	}

	if (main_window_renderer == NULL)
	{
		fprintf(stderr, "error: failed to create renderer: %s\n", SDL_GetError());
		exit(EXIT_FAILURE);
	}
}

// Toggle display-synced presentation. Recreates the renderer (and its texture)
// so the new vsync setting takes effect immediately.
void set_vsync(bool enabled)
{
	if (output_vsync == enabled && main_window_renderer != NULL)
		return;

	output_vsync = enabled;

	if (main_window_renderer != NULL)
	{
		deinit_texture();
		deinit_renderer();
		init_renderer();
		init_texture();
	}
}

static void deinit_renderer(void)
{
	if (main_window_renderer != NULL)
	{
		SDL_DestroyRenderer(main_window_renderer);
		main_window_renderer = NULL;
	}
}

static void init_texture(void)
{
	assert(main_window_renderer != NULL);

	// The Native scaler sizes its texture from the live window; refresh first.
	update_native_scaler_dims();

	int bpp = 32; // TODOSDL2
	Uint32 format = bpp == 32 ? SDL_PIXELFORMAT_RGB888 : SDL_PIXELFORMAT_RGB565;
	int scaler_w = scalers[scaler].width;
	int scaler_h = scalers[scaler].height;

	main_window_tex_format = SDL_AllocFormat(format);

	main_window_texture = SDL_CreateTexture(main_window_renderer, format, SDL_TEXTUREACCESS_STREAMING, scaler_w, scaler_h);

	if (main_window_texture == NULL)
	{
		fprintf(stderr, "error: failed to create scaler texture %dx%dx%s: %s\n", scaler_w, scaler_h, SDL_GetPixelFormatName(format), SDL_GetError());
		exit(EXIT_FAILURE);
	}
}

static void deinit_texture(void)
{
	if (main_window_texture != NULL)
	{
		SDL_DestroyTexture(main_window_texture);
		main_window_texture = NULL;
	}

	// The hi (supersample) textures belong to the same renderer; drop them too so a
	// renderer recreate (vsync toggle, scaler change) can't leave them dangling.
	if (hi_texture != NULL)
	{
		SDL_DestroyTexture(hi_texture);
		hi_texture = NULL;
		hi_texture_w = hi_texture_h = 0;
	}
	if (hi_stage != NULL)
	{
		SDL_DestroyTexture(hi_stage);
		hi_stage = NULL;
		hi_stage_w = hi_stage_h = 0;
	}

	if (main_window_tex_format != NULL)
	{
		SDL_FreeFormat(main_window_tex_format);
		main_window_tex_format = NULL;
	}
}

static int window_get_display_index(void)
{
	return SDL_GetWindowDisplayIndex(main_window);
}

static void window_center_in_display(int display_index)
{
	int win_w, win_h;
	SDL_GetWindowSize(main_window, &win_w, &win_h);

	SDL_Rect bounds;
	SDL_GetDisplayBounds(display_index, &bounds);

	SDL_SetWindowPosition(main_window, bounds.x + (bounds.w - win_w) / 2, bounds.y + (bounds.h - win_h) / 2);
}

void reinit_fullscreen(int new_display)
{
	fullscreen_display = new_display;

#if defined(__SWITCH__) || defined(__vita__)
	// Consoles have one always-fullscreen display and the SDL driver owns the window size.
	// Forcing FULLSCREEN_DESKTOP here pinned the Switch buffer to 1080p -- leave it untouched.
	// notes.md §Console ports.
	return;
#endif

	if (fullscreen_display >= SDL_GetNumVideoDisplays())
	{
		fullscreen_display = 0;
	}

	SDL_SetWindowFullscreen(main_window, SDL_FALSE);
	{
		// Native has no fixed output size (it tracks the window); give it the
		// largest clean integer-multiple window that fits the desktop instead.
		int win_w = scalers[scaler].width, win_h = scalers[scaler].height;
		if (scaler_is_native(scaler))
			native_windowed_size(&win_w, &win_h);
		SDL_SetWindowSize(main_window, win_w, win_h);
	}

	if (fullscreen_display == -1)
	{
		window_center_in_display(window_get_display_index());
	}
	else
	{
		window_center_in_display(fullscreen_display);

		if (SDL_SetWindowFullscreen(main_window, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
		{
			reinit_fullscreen(-1);
			return;
		}
	}
}

void video_on_win_resize(void)
{
#if defined(__SWITCH__) || defined(__vita__)
	// The console SDL driver owns the window size; the present path re-reads the live window
	// size every frame (calc_dst_render_rect), so there's nothing to reconcile. Snapping the
	// window back to a scaler-sized minimum here would just fight the driver (e.g. a 4x scaler
	// is wider than 720p) and re-break fullscreen.
	return;
#endif

	int w, h;
	int scaler_w, scaler_h;

	// Tell video to reinit if the window was manually resized by the user.
	// Also enforce a minimum size on the window.

	SDL_GetWindowSize(main_window, &w, &h);
	if (scaler_is_native(scaler))
	{
		// Native's output tracks the window, so it imposes no fixed minimum
		// beyond the logical screen itself.
		scaler_w = vga_width;
		scaler_h = vga_height;
	}
	else
	{
		scaler_w = scalers[scaler].width;
		scaler_h = scalers[scaler].height;
	}

	if (w < scaler_w || h < scaler_h)
	{
		w = w < scaler_w ? scaler_w : w;
		h = h < scaler_h ? scaler_h : h;

		SDL_SetWindowSize(main_window, w, h);
	}
}

void toggle_fullscreen(void)
{
#if defined(__SWITCH__) || defined(__vita__)
	return;  // always fullscreen on the consoles; nothing to toggle
#endif

	if (fullscreen_display != -1)
		reinit_fullscreen(-1);
	else
		reinit_fullscreen(SDL_GetWindowDisplayIndex(main_window));
}

bool init_scaler(unsigned int new_scaler)
{
	int bpp = main_window_tex_format->BitsPerPixel; // TODOSDL2

	scaler = new_scaler;

#if defined(__SWITCH__) || defined(__vita__)
	// On the consoles the window must stay at the panel's native size (the driver owns it);
	// only the scaler's intermediate texture, recreated by init_texture() below, changes.
	// The final present (calc_dst_render_rect) scales that texture to fill the window.
#else
	if (fullscreen_display == -1)
	{
		// Changing scalers, when not in fullscreen mode, forces the window
		// to resize to exactly match the scaler's output dimensions. Native
		// has none; give it the largest integer-multiple window that fits the
		// desktop. (Resize before init_texture: Native sizes its texture from
		// the window.)
		int w = scalers[scaler].width,
		    h = scalers[scaler].height;
		if (scaler_is_native(scaler))
			native_windowed_size(&w, &h);
		SDL_SetWindowSize(main_window, w, h);
		window_center_in_display(window_get_display_index());
	}
#endif

	deinit_texture();
	init_texture();

	switch (bpp)
	{
	case 32:
		scaler_function = scalers[scaler].scaler32;
		break;
	case 16:
		scaler_function = scalers[scaler].scaler16;
		break;
	default:
		scaler_function = NULL;
		break;
	}

	if (scaler_function == NULL)
	{
		assert(false);
		return false;
	}

	return true;
}

bool set_scaling_mode_by_name(const char *name)
{
	for (int i = 0; i < ScalingMode_MAX; ++i)
	{
		 if (strcmp(name, scaling_mode_names[i]) == 0)
		 {
			 scaling_mode = i;
			 return true;
		 }
	}
	return false;
}

void JE_clr256(SDL_Surface *screen)
{
	SDL_FillRect(screen, NULL, 0);
}

void JE_showVGA(void)
{
	if (current_x_offset != 0)
	{
		blit_with_offset(VGAScreen, menu_screen, current_x_offset);
		// Draw the cursor onto the composited frame (after the side gradient is
		// built from the clean content) so it doesn't smear into the pillarbox.
		JE_drawMouseToMenuScreen(menu_screen, current_x_offset);
		scale_and_flip(menu_screen);
	}
	else
	{
		scale_and_flip(VGAScreen);
	}
}

// Fit a centred rectangle of the given display aspect ratio (width / height)
// inside the window, preserving the ratio (letterbox or pillarbox as needed).
static void fit_rect_to_aspect(SDL_Rect *const r, int win_w, int win_h, float aspect)
{
	if ((float)win_h * aspect > (float)win_w)
	{
		r->w = win_w;
		r->h = (int)((float)win_w / aspect);
	}
	else
	{
		r->w = (int)((float)win_h * aspect);
		r->h = win_h;
	}
}

// The Native scaler's output size is not fixed: it tracks the window so the software
// scaler emits exactly one texel per screen pixel (in fullscreen, the exact size of
// the screen) and the final present never rescales. Refresh the table entry from the
// live window size and scaling mode; cheap, so callers run it before any use of the
// entry's dimensions.
static void update_native_scaler_dims(void)
{
	if (main_window == NULL)
		return;

	int win_w, win_h;
	SDL_GetWindowSize(main_window, &win_w, &win_h);

	SDL_Rect r = { 0, 0, vga_width, vga_height };
	const float pixel_aspect = (float)vga_width / (float)vga_height;

	switch (scaling_mode)
	{
	case SCALE_INTEGER:
		while (r.w + vga_width <= win_w && r.h + vga_height <= win_h)
		{
			r.w += vga_width;
			r.h += vga_height;
		}
		break;
	case SCALE_CLASSIC_PAR:
		fit_rect_to_aspect(&r, win_w, win_h, pixel_aspect * (5.f / 6.f));
		break;
	case SCALE_CENTER:  // no fixed size to center on; fill like Widescreen
	case SCALE_WIDESCREEN:
	default:
		fit_rect_to_aspect(&r, win_w, win_h, pixel_aspect);
		break;
	}

	if (r.w < vga_width)
		r.w = vga_width;
	if (r.h < vga_height)
		r.h = vga_height;

	scaler_set_native_size(r.w, r.h);
}

// Windowed size for the Native scaler (which has no fixed output size to restore):
// the largest integer multiple of the logical screen that fits the desktop's usable
// area — a big, clean window.
static void native_windowed_size(int *out_w, int *out_h)
{
	int factor = 1;

	int display = window_get_display_index();
	if (display < 0)
		display = 0;

	SDL_Rect bounds;
	if (SDL_GetDisplayUsableBounds(display, &bounds) == 0)
	{
		const int fw = bounds.w / vga_width,
		          fh = bounds.h / vga_height;
		factor = fw < fh ? fw : fh;
		if (factor < 1)
			factor = 1;
	}

	*out_w = vga_width * factor;
	*out_h = vga_height * factor;
}

static void calc_dst_render_rect(SDL_Surface *const src_surface, SDL_Rect *const dst_rect)
{
	// Decides how the logical output texture (after software scaling applied) will fit
	// in the window.

	int win_w, win_h;
	SDL_GetWindowSize(main_window, &win_w, &win_h);

	// Square-pixel ratio of the framebuffer itself (356:200 = 16:9).
	const float pixel_aspect = (float)src_surface->w / (float)src_surface->h;

	switch (scaling_mode)
	{
	case SCALE_CENTER:
		SDL_QueryTexture(main_window_texture, NULL, NULL, &dst_rect->w, &dst_rect->h);
		break;
	case SCALE_INTEGER:
		dst_rect->w = src_surface->w;
		dst_rect->h = src_surface->h;
		while (dst_rect->w + src_surface->w <= win_w && dst_rect->h + src_surface->h <= win_h)
		{
			dst_rect->w += src_surface->w;
			dst_rect->h += src_surface->h;
		}
		break;
	case SCALE_WIDESCREEN:
		// True widescreen: square pixels, i.e. the buffer's own ratio (16:9).
		fit_rect_to_aspect(dst_rect, win_w, win_h, pixel_aspect);
		break;
	case SCALE_CLASSIC_PAR:
		// Original DOS pixel aspect (PAR 5/6): taller pixels, ~3:2 overall.
		fit_rect_to_aspect(dst_rect, win_w, win_h, pixel_aspect * (5.f / 6.f));
		break;
	case ScalingMode_MAX:
		assert(false);
		break;
	}

	dst_rect->x = (win_w - dst_rect->w) / 2;
	dst_rect->y = (win_h - dst_rect->h) / 2;
}

// Sample the presented-frame rate once a second for the optional FPS counter.
// Shared by both present paths (scale_and_flip and present_hi).
static void sample_fps(void)
{
	static Uint32 fps_sample_start = 0;
	static int fps_frames = 0;

	++fps_frames;
	const Uint32 now = SDL_GetTicks();
	if (fps_sample_start == 0)
		fps_sample_start = now;
	else if (now - fps_sample_start >= 1000)
	{
		current_fps = (int)(fps_frames * 1000u / (now - fps_sample_start));
		fps_frames = 0;
		fps_sample_start = now;
	}
}

static void scale_and_flip(SDL_Surface *src_surface)
{
	assert(src_surface->format->BitsPerPixel == 8);

	// The Native scaler's output size tracks the window: if a resize, fullscreen
	// toggle or scaling-mode change moved it since the last present, recreate the
	// texture at the new exact size.
	if (scaler_is_native(scaler))
	{
		update_native_scaler_dims();

		int tw = 0, th = 0;
		if (main_window_texture != NULL)
			SDL_QueryTexture(main_window_texture, NULL, NULL, &tw, &th);
		if (tw != scalers[scaler].width || th != scalers[scaler].height)
		{
			SDL_Texture *fresh = SDL_CreateTexture(main_window_renderer,
				main_window_tex_format->format, SDL_TEXTUREACCESS_STREAMING,
				scalers[scaler].width, scalers[scaler].height);
			if (fresh != NULL)
			{
				if (main_window_texture != NULL)
					SDL_DestroyTexture(main_window_texture);
				main_window_texture = fresh;
			}
			// else: keep presenting through the old texture (stretched, not 1:1)
		}
	}

	// Do software scaling
	assert(scaler_function != NULL);
	scaler_function(src_surface, main_window_texture);

	SDL_Rect dst_rect;
	calc_dst_render_rect(src_surface, &dst_rect);

	// Clear the window and blit the output texture to it
	SDL_SetRenderDrawColor(main_window_renderer, 0, 0, 0, 255);
	SDL_RenderClear(main_window_renderer);
	SDL_RenderCopy(main_window_renderer, main_window_texture, NULL, &dst_rect);
	SDL_RenderPresent(main_window_renderer);

	sample_fps();

	// Save output rect to be used by mouse functions
	last_output_rect = dst_rect;
}

// Re-present the last composed frame without re-running the software scaler. Used to keep the
// display refreshing while a modal SYSTEM overlay is up -- specifically the Vita IME dialog,
// which the system compositor only draws while the app keeps presenting frames. Cheap: just
// re-copies the existing output texture. No-op before the first real present.
void video_repeat_last_present(void)
{
	if (main_window_renderer == NULL)
		return;

	SDL_SetRenderDrawColor(main_window_renderer, 0, 0, 0, 255);
	SDL_RenderClear(main_window_renderer);
	if (main_window_texture != NULL)
		SDL_RenderCopy(main_window_renderer, main_window_texture, NULL, &last_output_rect);
	SDL_RenderPresent(main_window_renderer);
}

// (Re)create the hi texture at the given size. Returns false on failure so the
// caller can fall back to the classic present path.
static bool ensure_hi_texture(int w, int h)
{
	if (hi_texture != NULL && hi_texture_w == w && hi_texture_h == h)
		return true;

	if (hi_texture != NULL)
	{
		SDL_DestroyTexture(hi_texture);
		hi_texture = NULL;
	}

	hi_texture = SDL_CreateTexture(main_window_renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, w, h);
	if (hi_texture == NULL)
	{
		hi_texture_w = hi_texture_h = 0;
		return false;
	}
	hi_texture_w = w;
	hi_texture_h = h;
	return true;
}

// (Re)create the intermediate render target for the sharp-bilinear prescale (or the
// minification halving pass). Returns NULL on failure; callers fall back to a direct
// linear copy — softer, never broken.
static SDL_Texture *ensure_hi_stage(int w, int h)
{
	if (hi_stage != NULL && hi_stage_w == w && hi_stage_h == h)
		return hi_stage;

	if (hi_stage != NULL)
	{
		SDL_DestroyTexture(hi_stage);
		hi_stage = NULL;
	}

	hi_stage = SDL_CreateTexture(main_window_renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_TARGET, w, h);
	if (hi_stage == NULL)
	{
		hi_stage_w = hi_stage_h = 0;
		return NULL;
	}
	SDL_SetTextureScaleMode(hi_stage, SDL_ScaleModeLinear);
	hi_stage_w = w;
	hi_stage_h = h;
	return hi_stage;
}

void present_hi(SDL_Surface *hi)
{
	assert(hi->format->BitsPerPixel == 8);

	if (!ensure_hi_texture(hi->w, hi->h))
	{
		// Can't build the hi texture (GPU limit / OOM): show the frame through the
		// classic path rather than nothing. It arrives NxN, which the scaler can't
		// take, so just present the logical screen — one soft frame, no crash.
		scale_and_flip(VGAScreen);
		return;
	}

	// Palette-convert the 8-bit hi frame 1:1 into the texture (rgb_palette is
	// already in the texture's pixel format).
	{
		void *tex_pixels = NULL;
		int tex_pitch = 0;
		if (SDL_LockTexture(hi_texture, NULL, &tex_pixels, &tex_pitch) != 0)
			return;

		const Uint8 *src_row = (const Uint8 *)hi->pixels;
		Uint8 *dst_row = (Uint8 *)tex_pixels;
		for (int y = 0; y < hi->h; ++y)
		{
			const Uint8 *src = src_row;
			Uint32 *dst = (Uint32 *)dst_row;
			for (int x = 0; x < hi->w; ++x)
				*dst++ = rgb_palette[*src++];
			src_row += hi->pitch;
			dst_row += tex_pitch;
		}

		SDL_UnlockTexture(hi_texture);
	}

	// Fit into the same on-screen rectangle the classic path would use (sized from
	// the LOGICAL screen + the scaler texture), so supersampling only adds detail —
	// it never zooms or resizes the output.
	SDL_Rect dst_rect;
	calc_dst_render_rect(VGAScreen, &dst_rect);

	// Pick the pass chain from the output/frame size ratio and the user's filter
	// (see video.h); on any stage failure fall back to the direct copy.
	SDL_Texture *final_tex = hi_texture;
	SDL_ScaleMode direct_mode = SDL_ScaleModeLinear;

	if (render_supersample_filter == SS_FILTER_NONE)
	{
		// None: no filtering at any ratio — point-sample the hi frame straight to the
		// output. The supersampled detail is dropped rather than blended (raw, aliased
		// pixels); for when zero smoothing is wanted over the antialias supersampling
		// normally buys. Skips both the magnify and minify special-casing below.
		direct_mode = SDL_ScaleModeNearest;
	}
	else if (dst_rect.w > hi->w || dst_rect.h > hi->h)
	{
		if (render_supersample_filter == SS_FILTER_SHARP)
		{
			// Sharp: plain nearest magnification — hard pixel blocks at any output
			// size, exactly the classic fullscreen look. (Magnification repeats
			// texels rather than dropping them, so there is no motion shimmer.)
			direct_mode = SDL_ScaleModeNearest;
		}
		else
		{
			// Smooth (sharp-bilinear): linear magnification alone would blur, so
			// nearest-prescale to the smallest integer multiple covering the output,
			// then let the final linear pass shrink the remainder (ratio in (0.5, 1]).
			const int kx = (dst_rect.w + hi->w - 1) / hi->w;
			const int ky = (dst_rect.h + hi->h - 1) / hi->h;
			const int k = kx > ky ? kx : ky;

			SDL_Texture *stage = ensure_hi_stage(hi->w * k, hi->h * k);
			if (stage != NULL && SDL_SetRenderTarget(main_window_renderer, stage) == 0)
			{
				SDL_SetTextureScaleMode(hi_texture, SDL_ScaleModeNearest);
				SDL_RenderCopy(main_window_renderer, hi_texture, NULL, NULL);
				SDL_SetRenderTarget(main_window_renderer, NULL);
				final_tex = stage;
			}
		}
	}
	else if (hi->w > dst_rect.w * 2 && dst_rect.w > 0)
	{
		// Output much smaller (beyond 2:1, e.g. a 4x frame in a 1x window): plain
		// bilinear minification only samples 2x2 texels and skips the rest (moving
		// shimmer), so linearly halve first; the final pass then sits within 2:1.
		SDL_Texture *stage = ensure_hi_stage(hi->w / 2, hi->h / 2);
		if (stage != NULL && SDL_SetRenderTarget(main_window_renderer, stage) == 0)
		{
			SDL_SetTextureScaleMode(hi_texture, SDL_ScaleModeLinear);
			SDL_RenderCopy(main_window_renderer, hi_texture, NULL, NULL);
			SDL_SetRenderTarget(main_window_renderer, NULL);
			final_tex = stage;
		}
	}

	if (final_tex == hi_texture)
		SDL_SetTextureScaleMode(hi_texture, direct_mode);

	SDL_SetRenderDrawColor(main_window_renderer, 0, 0, 0, 255);
	SDL_RenderClear(main_window_renderer);
	SDL_RenderCopy(main_window_renderer, final_tex, NULL, &dst_rect);
	SDL_RenderPresent(main_window_renderer);

	sample_fps();

	// Keep the mouse mapping in sync with what is actually on screen.
	last_output_rect = dst_rect;
}

// Re-present the current logical frame at the live window size, recovering the window after
// the drawable was invalidated (fullscreen toggle, window/scaler resize, Switch dock/undock,
// expose). The backbuffer goes stale on such a change, but the game may be parked in an
// input-wait loop that won't redraw on its own, so it would "sit on the frame" until the next
// keypress. Presents one 1x frame from the live VGAScreen; any in-game smooth/hi present loop
// resumes on the next iteration. notes.md §Console ports.
void video_repaint(void)
{
	if (main_window_renderer == NULL)
		return;
	JE_showVGA();
}

// Called from the event pump. Repaints when the window size no longer matches the last
// present (a resolution change / dock transition the game didn't initiate a redraw for),
// or when `force` is set (an EXPOSED / render-targets-reset event, where the size is
// unchanged but the backbuffer contents were lost). Cheap in the common case: one size
// query, no present -- so it's safe to call on every event-pump pass.
void video_repaint_if_stale(bool force)
{
	if (main_window == NULL || main_window_renderer == NULL)
		return;

	int w = 0, h = 0;
	SDL_GetWindowSize(main_window, &w, &h);

	if (force || w != last_present_w || h != last_present_h)
	{
		last_present_w = w;
		last_present_h = h;
		video_repaint();
	}
}

#ifndef __vita__  // gradient helpers -- unused on Vita, which draws a plain black pillarbox
static Uint8 nearest_palette_index(Uint8 r, Uint8 g, Uint8 b)
{
	int best = 0;
	int best_dist = INT_MAX;

	for (int i = 0; i < 256; ++i)
	{
		int dr = (int)colors[i].r - r;
		int dg = (int)colors[i].g - g;
		int db = (int)colors[i].b - b;
		int dist = dr * dr + dg * dg + db * db;
		if (dist < best_dist)
		{
			best_dist = dist;
			best = i;
			if (dist == 0)
				break;
		}
	}

	return (Uint8)best;
}

static void update_gradient_cache(void)
{
	if (!gradient_cache_valid || memcmp(last_gradient_palette, rgb_palette, sizeof(rgb_palette)) != 0)
	{
		memcpy(last_gradient_palette, rgb_palette, sizeof(rgb_palette));

		for (int c = 0; c < 256; ++c)
		{
			SDL_Color col = colors[c];
			gradient_cache[c][0] = 0;
			for (int i = 1; i < MENU_X_OFFSET; ++i)
			{
				float factor = (float)i / MENU_X_OFFSET;
				Uint8 r = (Uint8)(col.r * factor);
				Uint8 g = (Uint8)(col.g * factor);
				Uint8 b = (Uint8)(col.b * factor);
				gradient_cache[c][i] = nearest_palette_index(r, g, b);
			}
		}

		gradient_cache_valid = true;
	}
}
#endif  // !__vita__

static void blit_with_offset(SDL_Surface* src, SDL_Surface* dst, int x_offset)
{
#ifndef __vita__
	// The side gradient's nearest-palette-index rebuild (256-colour search per cell) reruns
	// every menu-fade frame and dominates the Vita's slow CPU, so Vita draws plain black bars.
	// notes.md §Console ports.
	update_gradient_cache();
#endif

	for (int y = 0; y < vga_height; ++y)
	{
		Uint8* src_row = (Uint8*)src->pixels + y * src->pitch;
		Uint8* dst_row = (Uint8*)dst->pixels + y * dst->pitch;

		memcpy(dst_row + x_offset, src_row, LEGACY_WIDTH);

#ifdef __vita__
		// Plain black pillarbox bars (index 0 = black, as the gradient's own edge column is).
		memset(dst_row, 0, x_offset);
		memset(dst_row + x_offset + LEGACY_WIDTH, 0, x_offset);
#else
		Uint8 left_color = src_row[0];
		for (int i = 0; i < x_offset; ++i)
		{
			dst_row[i] = gradient_cache[left_color][i];
		}

		Uint8 right_color = src_row[LEGACY_WIDTH - 1];
		for (int i = 0; i < x_offset; ++i)
		{
			dst_row[x_offset + LEGACY_WIDTH + i] = gradient_cache[right_color][x_offset - 1 - i];
		}
#endif
	}
}

void set_menu_centered(bool centered)
{
	current_x_offset = centered ? MENU_X_OFFSET : 0;
}

// Current pillarbox offset (0 when not centering a legacy-width screen); the mouse
// module uses this to decide whether to defer the cursor draw to the composited frame.
int video_get_menu_x_offset(void)
{
	return current_x_offset;
}

/** Maps a specified point in game screen coordinates to window coordinates. */
void mapScreenPointToWindow(Sint32 *const inout_x, Sint32 *const inout_y)
{
	Sint32 x = *inout_x + current_x_offset;
	*inout_x = (2 * x + 1) * last_output_rect.w / (2 * VGAScreen->w) + last_output_rect.x;
	*inout_y = (2 * *inout_y + 1) * last_output_rect.h / (2 * VGAScreen->h) + last_output_rect.y;
}

/** Maps a specified point in window coordinates to game screen coordinates. */
void mapWindowPointToScreen(Sint32 *const inout_x, Sint32 *const inout_y)
{
	*inout_x = (2 * (*inout_x - last_output_rect.x) + 1) * VGAScreen->w / (2 * last_output_rect.w) - current_x_offset;
	*inout_y = (2 * (*inout_y - last_output_rect.y) + 1) * VGAScreen->h / (2 * last_output_rect.h);
}

/** Scales a distance in window coordinates to game screen coordinates. */
void scaleWindowDistanceToScreen(Sint32 *const inout_x, Sint32 *const inout_y)
{
	*inout_x = (2 * *inout_x + 1) * VGAScreen->w / (2 * last_output_rect.w);
	*inout_y = (2 * *inout_y + 1) * VGAScreen->h / (2 * last_output_rect.h);
}

/** Float variant: no integer rounding, so callers that sample small deltas every
 *  render frame (rather than once per tick) don't lose fine/diagonal motion. */
void scaleWindowDistanceToScreenF(float *const inout_x, float *const inout_y)
{
	if (last_output_rect.w > 0)
		*inout_x = *inout_x * (float)VGAScreen->w / (float)last_output_rect.w;
	if (last_output_rect.h > 0)
		*inout_y = *inout_y * (float)VGAScreen->h / (float)last_output_rect.h;
}
