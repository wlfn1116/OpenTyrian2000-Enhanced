/* 
 * OpenTyrian: A modern cross-platform port of Tyrian
 * Copyright (C) 2007-2010  The OpenTyrian Development Team
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
#ifndef VIDEO_SCALE_H
#define VIDEO_SCALE_H

#include "opentyr.h"

#include "SDL.h"

typedef void (*ScalerFunction)(SDL_Surface *src, SDL_Texture *dst);

struct Scalers
{
	int width, height;
	ScalerFunction scaler16, scaler32;
	const char *name;
};

extern uint scaler;
// Non-const: the "Native" entry's width/height track the live output size
// (video.c refreshes them via scaler_set_native_size before any use).
extern struct Scalers scalers[];
extern const uint scalers_count;

void set_scaler_by_name(const char *name);
// Nearest-neighbour ("plain") scaler queries — the only scalers allowed while
// supersampling is enabled (the hi path bypasses scaler algorithms in-game).
bool scaler_is_plain(uint index);
uint scaler_plain_equivalent(uint index);
// "Native" fit-to-output scaler: no fixed factor — it renders at the exact
// output size (any ratio), one texel per screen pixel.
bool scaler_is_native(uint index);
void scaler_set_native_size(int w, int h);

#endif /* VIDEO_SCALE_H */
