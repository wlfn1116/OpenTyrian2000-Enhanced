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
#include "joystick.h"

#include "config.h"
#include "config_file.h"
#include "file.h"
#include "keyboard.h"
#include "nortsong.h"
#include "opentyr.h"
#include "params.h"
#include "varz.h"
#include "video.h"

#include <assert.h>
#include <ctype.h>
#include <string.h>

int joystick_axis_threshold(int j, int value);
int check_assigned(SDL_Joystick *joystick_handle, const Joystick_assignment assignment[2]);

const char *assignment_to_code(const Joystick_assignment *assignment);
void code_to_assignment(Joystick_assignment *assignment, const char *buffer);

int joystick_repeat_delay = 300; // milliseconds, repeat delay for buttons
bool joydown = false;            // any joystick buttons down, updated by poll_joysticks()
bool ignore_joystick = false;

int joysticks = 0;
Joystick *joystick = NULL;

static const int joystick_analog_max = 32767;

// eliminates axis movement below the threshold
int joystick_axis_threshold(int j, int value)
{
	assert(j < joysticks);
	
	bool negative = value < 0;
	if (negative)
		value = -value;
	
	if (value <= joystick[j].threshold * 1000)
		return 0;
	
	value -= joystick[j].threshold * 1000;
	
	return negative ? -value : value;
}

// converts joystick axis to sane Tyrian-usable value (based on sensitivity)
int joystick_axis_reduce(int j, int value)
{
	assert(j < joysticks);
	
	value = joystick_axis_threshold(j, value);
	
	if (value == 0)
		return 0;
	
	return value / (3000 - 200 * joystick[j].sensitivity);
}

// converts analog joystick axes to an angle
// returns false if axes are centered (there is no angle)
bool joystick_analog_angle(int j, float *angle)
{
	assert(j < joysticks);
	
	float x = joystick_axis_threshold(j, joystick[j].x), y = joystick_axis_threshold(j, joystick[j].y);
	
	if (x != 0)
	{
		*angle += atanf(-y / x);
		*angle += (x < 0) ? -M_PI_2 : M_PI_2;
		return true;
	}
	else if (y != 0)
	{
		*angle += y < 0 ? M_PI : 0;
		return true;
	}
	
	return false;
}

/* gives back value 0..joystick_analog_max indicating that one of the assigned
 * buttons has been pressed or that one of the assigned axes/hats has been moved
 * in the assigned direction
 */
int check_assigned(SDL_Joystick *joystick_handle, const Joystick_assignment assignment[2])
{
	int result = 0;
	
	for (int i = 0; i < 2; i++)
	{
		int temp = 0;
		
		switch (assignment[i].type)
		{
		case NONE:
			continue;
			
		case AXIS:
			temp = SDL_JoystickGetAxis(joystick_handle, assignment[i].num);
			
			if (assignment[i].negative_axis)
				temp = -temp;
			break;
		
		case BUTTON:
			temp = SDL_JoystickGetButton(joystick_handle, assignment[i].num) == 1 ? joystick_analog_max : 0;
			break;
		
		case HAT:
			temp = SDL_JoystickGetHat(joystick_handle, assignment[i].num);
			
			if (assignment[i].x_axis)
				temp &= SDL_HAT_LEFT | SDL_HAT_RIGHT;
			else
				temp &= SDL_HAT_UP | SDL_HAT_DOWN;
			
			if (assignment[i].negative_axis)
				temp &= SDL_HAT_LEFT | SDL_HAT_UP;
			else
				temp &= SDL_HAT_RIGHT | SDL_HAT_DOWN;
			
			temp = temp ? joystick_analog_max : 0;
			break;
		}
		
		if (temp > result)
			result = temp;
	}
	
	return result;
}

// updates joystick state
void poll_joystick(int j)
{
	assert(j < joysticks);
	
	if (joystick[j].handle == NULL)
		return;
	
	SDL_JoystickUpdate();
	
	// indicates that a direction/action was pressed since last poll
	joystick[j].input_pressed = false;
	
	// indicates that an direction/action has been held long enough to fake a repeat press
	bool repeat = joystick[j].joystick_delay < SDL_GetTicks();

#if defined(__SWITCH__) || defined(__vita__)
	// Make the RIGHT analog stick drive the ship exactly like the LEFT one. Both consoles'
	// SDL ports expose 4 axes (0/1 = left stick, 2/3 = right stick); the left-stick axis and
	// the d-pad button already occupy both assignment slots of every direction, so the right
	// stick can't be added as a normal binding -- fold its deflection into analog_direction[]
	// (which feeds both the digital direction[] and the analog x/y) in the loop below.
	const bool switch_right_stick = SDL_JoystickNumAxes(joystick[j].handle) >= 4;
#endif

	// update direction state
	for (uint d = 0; d < COUNTOF(joystick[j].direction); d++)
	{
		bool old = joystick[j].direction[d];

		joystick[j].analog_direction[d] = check_assigned(joystick[j].handle, joystick[j].assignment[d]);

#if defined(__SWITCH__) || defined(__vita__)
		if (switch_right_stick)
		{
			// direction d: 0=up, 1=right, 2=down, 3=left. The left stick binds axis (d+1)%2,
			// negated for up/left; the right stick is those same two axes + 2. Take whichever
			// stick is pushed further (mirrors check_assigned's max-over-slots semantics).
			int rs = SDL_JoystickGetAxis(joystick[j].handle, (d + 1) % 2 + 2);
			if (d == 0 || d == 3)
				rs = -rs;
			if (rs > joystick[j].analog_direction[d])
				joystick[j].analog_direction[d] = rs;
		}
#endif

		joystick[j].direction[d] = joystick[j].analog_direction[d] > (joystick_analog_max / 2);
		joydown |= joystick[j].direction[d];
		
		joystick[j].direction_pressed[d] = joystick[j].direction[d] && (!old || repeat);
		joystick[j].input_pressed |= joystick[j].direction_pressed[d];
	}
	
	joystick[j].x = -joystick[j].analog_direction[3] + joystick[j].analog_direction[1];
	joystick[j].y = -joystick[j].analog_direction[0] + joystick[j].analog_direction[2];
	
	// update action state
	for (uint d = 0; d < COUNTOF(joystick[j].action); d++)
	{
		bool old = joystick[j].action[d];
		
		joystick[j].action[d] = check_assigned(joystick[j].handle, joystick[j].assignment[d + COUNTOF(joystick[j].direction)]) > (joystick_analog_max / 2);
		joydown |= joystick[j].action[d];
		
		joystick[j].action_pressed[d] = joystick[j].action[d] && (!old || repeat);
		joystick[j].input_pressed |= joystick[j].action_pressed[d];
	}
	
	// "menu" (action[4]) acts as BACK / cancel in menus (Escape) -- matching how it toggles
	// the in-game menu closed and the B-is-back convention -- NOT as a second confirm. Only
	// "fire" confirms/selects; change-fire / menu / pause all cancel.
	joystick[j].confirm = joystick[j].action[0];
	joystick[j].cancel = joystick[j].action[1] || joystick[j].action[4] || joystick[j].action[5];
	
	// if new input, reset press-repeat delay
	if (joystick[j].input_pressed)
		joystick[j].joystick_delay = SDL_GetTicks() + joystick_repeat_delay;
}

// updates all joystick states
void poll_joysticks(void)
{
	joydown = false;
	
	for (int j = 0; j < joysticks; j++)
		poll_joystick(j);
}

// sends SDL KEYDOWN and KEYUP events for a key
void push_key(SDL_Scancode key)
{
	SDL_Event e;
	
	memset(&e.key.keysym, 0, sizeof(e.key.keysym));
	
	e.key.keysym.scancode = key;
	e.key.state = SDL_RELEASED;
	
	e.type = SDL_KEYDOWN;
	SDL_PushEvent(&e);
	
	e.type = SDL_KEYUP;
	SDL_PushEvent(&e);
}

// helps us be lazy by pretending joysticks are a keyboard (useful for menus)
void push_joysticks_as_keyboard(void)
{
	const SDL_Scancode confirm = SDL_SCANCODE_RETURN, cancel = SDL_SCANCODE_ESCAPE;
	const SDL_Scancode direction[4] = { SDL_SCANCODE_UP, SDL_SCANCODE_RIGHT, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT };
	
	poll_joysticks();
	
	for (int j = 0; j < joysticks; j++)
	{
		if (!joystick[j].input_pressed)
			continue;
		
		// A single button bound to BOTH a confirm action (fire/menu) and a cancel action
		// (change-fire/pause) -- e.g. the Switch B button left on its default "change fire"
		// (=cancel) while also bound to "menu" (=confirm) -- would otherwise push Return AND
		// Escape, making a menu select and go back at once (which can crash debug screens).
		// Fire only one; prefer cancel (back), which is non-destructive and matches the
		// B-is-back convention.
		if (joystick[j].cancel)
			push_key(cancel);
		else if (joystick[j].confirm)
			push_key(confirm);
		
		for (uint d = 0; d < COUNTOF(joystick[j].direction_pressed); d++)
		{
			if (joystick[j].direction_pressed[d])
				push_key(direction[d]);
		}
	}
}

// initializes SDL joystick system and loads assignments for joysticks found
void init_joysticks(void)
{
	if (ignore_joystick)
		return;
	
	if (SDL_InitSubSystem(SDL_INIT_JOYSTICK))
	{
		fprintf(stderr, "warning: failed to initialize joystick system: %s\n", SDL_GetError());
		ignore_joystick = true;
		return;
	}
	
	SDL_JoystickEventState(SDL_IGNORE);
	
	joysticks = SDL_NumJoysticks();

#ifdef __SWITCH__
	// switch-sdl2 always reports 8 slots but only slot 0 is real; saving the idle
	// placeholders clobbers slot 0's shared bindings, so use only slot 0. notes.md §Console ports.
	if (joysticks > 1)
		joysticks = 1;
#endif

	joystick = malloc(joysticks * sizeof(*joystick));
	
	for (int j = 0; j < joysticks; j++)
	{
		memset(&joystick[j], 0, sizeof(*joystick));
		
		joystick[j].handle = SDL_JoystickOpen(j);
		if (joystick[j].handle != NULL)
		{
			printf("joystick detected: %s ", SDL_JoystickName(joystick[j].handle));
			printf("(%d axes, %d buttons, %d hats)\n",
			       SDL_JoystickNumAxes(joystick[j].handle),
			       SDL_JoystickNumButtons(joystick[j].handle),
			       SDL_JoystickNumHats(joystick[j].handle));

#if defined(__SWITCH__) || defined(__vita__)
			// There is no visible stdout on the consoles; mirror the controller capabilities
			// to a file in the user dir so mapping problems can be diagnosed. Harmless anywhere.
			{
				FILE *jl = dir_fopen(get_user_directory(), "joystick_info.txt", j == 0 ? "w" : "a");
				if (jl)
				{
					fprintf(jl, "joystick %d: %s (%d axes, %d buttons, %d hats)\n",
					        j, SDL_JoystickName(joystick[j].handle),
					        SDL_JoystickNumAxes(joystick[j].handle),
					        SDL_JoystickNumButtons(joystick[j].handle),
					        SDL_JoystickNumHats(joystick[j].handle));
					fclose(jl);
				}
			}
#endif

			if (!load_joystick_assignments(&opentyrian_config, j))
				reset_joystick_assignments(j);
		}
	}
	
	if (joysticks == 0)
		printf("no joysticks detected\n");
}

// deinitializes SDL joystick system and saves joystick assignments
void deinit_joysticks(void)
{
	if (ignore_joystick)
		return;
	
	for (int j = 0; j < joysticks; j++)
	{
		if (joystick[j].handle != NULL)
		{
			save_joystick_assignments(&opentyrian_config, j);
			SDL_JoystickClose(joystick[j].handle);
		}
	}
	
	free(joystick);
	
	SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
}

void reset_joystick_assignments(int j)
{
	assert(j < joysticks);
	
	// defaults: first 2 axes, first hat, first 6 buttons
	for (uint a = 0; a < COUNTOF(joystick[j].assignment); a++)
	{
		// clear assignments
		for (uint i = 0; i < COUNTOF(joystick[j].assignment[a]); i++)
			joystick[j].assignment[a][i].type = NONE;
		
		if (a < 4)
		{
			if (SDL_JoystickNumAxes(joystick[j].handle) >= 2)
			{
				joystick[j].assignment[a][0].type = AXIS;
				joystick[j].assignment[a][0].num = (a + 1) % 2;
				joystick[j].assignment[a][0].negative_axis = (a == 0 || a == 3);
			}
			
			if (SDL_JoystickNumHats(joystick[j].handle) >= 1)
			{
				joystick[j].assignment[a][1].type = HAT;
				joystick[j].assignment[a][1].num = 0;
				joystick[j].assignment[a][1].x_axis = (a == 1 || a == 3);
				joystick[j].assignment[a][1].negative_axis = (a == 0 || a == 3);
			}
#if defined(__SWITCH__) || defined(__vita__)
			// The consoles expose the d-pad as BUTTONS (they report 0 hats), so the hat
			// default above never binds and the d-pad does nothing. Bind the d-pad buttons
			// into the free second slot so it works out of the box.
			// direction index a: 0=up, 1=right, 2=down, 3=left.
			else
			{
#if defined(__SWITCH__)
				// switch-sdl2 button order: 12=Left, 13=Up, 14=Right, 15=Down.
				static const int dpad_btn[4] = { 13, 14, 15, 12 };  // up, right, down, left
#else
				// Vita SDL button order: 6=Down, 7=Left, 8=Up, 9=Right.
				static const int dpad_btn[4] = { 8, 9, 6, 7 };      // up, right, down, left
#endif
				if (dpad_btn[a] < SDL_JoystickNumButtons(joystick[j].handle))
				{
					joystick[j].assignment[a][1].type = BUTTON;
					joystick[j].assignment[a][1].num = dpad_btn[a];
				}
			}
#endif
		}
		else
		{
			int btn = (int)(a - 4);
#if defined(__SWITCH__)
			// Buttons 4/5 are the analog-stick clicks on switch-sdl2 — poor defaults for
			// "menu" and "pause". Use Plus (10) and Minus (11), the natural system buttons.
			if (a == 8) btn = 10;       // assignment_names[8] = "menu"
			else if (a == 9) btn = 11;  // assignment_names[9] = "pause"
#elif defined(__vita__)
			// Vita SDL button order: 0 triangle,1 circle,2 cross,3 square,4 L1,5 R1,
			// 6 down,7 left,8 up,9 right,10 select,11 start. Map the actions (a-4) to a
			// comfortable scheme: cross=fire, circle=change fire/cancel, L1/R1=left/right
			// sidekick, start=menu, select=pause.
			static const int vita_action_btn[6] = { 2, 1, 4, 5, 11, 10 };
			btn = vita_action_btn[a - 4];
#endif
			if (btn < (int)SDL_JoystickNumButtons(joystick[j].handle))
			{
				joystick[j].assignment[a][0].type = BUTTON;
				joystick[j].assignment[a][0].num = btn;
			}
		}
	}
	
	joystick[j].analog = false;
	joystick[j].sensitivity = 5;
	joystick[j].threshold = 5;
}

static const char* const assignment_names[] =
{
	"up",
	"right",
	"down",
	"left",
	"fire",
	"change fire",
	"left sidekick",
	"right sidekick",
	"menu",
	"pause",
};

bool load_joystick_assignments(Config *config, int j)
{
	ConfigSection *section = config_find_section(config, "joystick", SDL_JoystickName(joystick[j].handle));
	if (section == NULL)
		return false;
	
	if (!config_get_bool_option(section, "analog", &joystick[j].analog))
		joystick[j].analog = false;
	
	joystick[j].sensitivity = config_get_or_set_int_option(section, "sensitivity", 5);

	joystick[j].threshold = config_get_or_set_int_option(section, "threshold", 5);
	
	for (size_t a = 0; a < COUNTOF(assignment_names); ++a)
	{
		for (unsigned int i = 0; i < COUNTOF(joystick[j].assignment[a]); ++i)
			joystick[j].assignment[a][i].type = NONE;
		
		ConfigOption *option = config_get_option(section, assignment_names[a]);
		if (option == NULL)
			continue;
		
		foreach_option_i_value(i, value, option)
		{
			if (i >= COUNTOF(joystick[j].assignment[a]))
				break;

			code_to_assignment(&joystick[j].assignment[a][i], value);
		}
	}

	// Configs predating the "menu"/"pause" actions load them unbound, so the pause/setup
	// menu is unreachable from a controller. Give any still-unbound one its default
	// button (matching reset_joystick_assignments).
	for (size_t a = 8; a <= 9; ++a)  // assignment_names: 8 = "menu", 9 = "pause"
	{
		bool bound = false;
		for (unsigned int i = 0; i < COUNTOF(joystick[j].assignment[a]); ++i)
			if (joystick[j].assignment[a][i].type != NONE)
				bound = true;

		if (!bound && (a - 4) < (unsigned)SDL_JoystickNumButtons(joystick[j].handle))
		{
			joystick[j].assignment[a][0].type = BUTTON;
			joystick[j].assignment[a][0].num = (int)(a - 4);
		}
	}

#if defined(__SWITCH__) || defined(__vita__)
	// Same idea for the d-pad: the consoles have no hat, so a config saved by an earlier
	// build (or first-run defaults) has only the analog-stick axis on each direction and
	// the d-pad does nothing. If a direction has no digital (button/hat) binding, back-fill
	// its matching d-pad button so the d-pad works without a manual reset.
	// direction a: 0=up, 1=right, 2=down, 3=left.
	{
#if defined(__SWITCH__)
		static const int dpad_btn[4] = { 13, 14, 15, 12 };  // switch-sdl2: 12=L,13=U,14=R,15=D
#else
		static const int dpad_btn[4] = { 8, 9, 6, 7 };      // Vita: 6=D,7=L,8=U,9=R
#endif
		for (size_t a = 0; a < 4; ++a)
		{
			bool has_digital = false;
			for (unsigned int i = 0; i < COUNTOF(joystick[j].assignment[a]); ++i)
				if (joystick[j].assignment[a][i].type == BUTTON || joystick[j].assignment[a][i].type == HAT)
					has_digital = true;

			if (!has_digital && joystick[j].assignment[a][1].type == NONE
			    && dpad_btn[a] < SDL_JoystickNumButtons(joystick[j].handle))
			{
				joystick[j].assignment[a][1].type = BUTTON;
				joystick[j].assignment[a][1].num = dpad_btn[a];
			}
		}
	}
#endif

	return true;
}

bool save_joystick_assignments(Config *config, int j)
{
	ConfigSection *section = config_find_or_add_section(config, "joystick", SDL_JoystickName(joystick[j].handle));
	if (section == NULL)
		exit(EXIT_FAILURE);  // out of memory
	
	config_set_bool_option(section, "analog", joystick[j].analog, NO_YES);
	
	config_set_int_option(section, "sensitivity", joystick[j].sensitivity);
	
	config_set_int_option(section, "threshold", joystick[j].threshold);
	
	for (size_t a = 0; a < COUNTOF(assignment_names); ++a)
	{
		ConfigOption *option = config_set_option(section, assignment_names[a], NULL);
		if (option == NULL)
			exit(EXIT_FAILURE);  // out of memory
		
		option = config_set_value(option, NULL);
		if (option == NULL)
			exit(EXIT_FAILURE);  // out of memory

		for (size_t i = 0; i < COUNTOF(joystick[j].assignment[a]); ++i)
		{
			if (joystick[j].assignment[a][i].type == NONE)
				continue;
			
			option = config_add_value(option, assignment_to_code(&joystick[j].assignment[a][i]));
			if (option == NULL)
				exit(EXIT_FAILURE);  // out of memory
		}
	}
	
	return true;
}

// fills buffer with comma separated list of assigned joystick functions
void joystick_assignments_to_string(char *buffer, size_t buffer_len, const Joystick_assignment *assignments)
{
	strncpy(buffer, "", buffer_len);
	
	bool comma = false;
	for (uint i = 0; i < COUNTOF(*joystick->assignment); ++i)
	{
		if (assignments[i].type == NONE)
			continue;
		
		size_t len = snprintf(buffer, buffer_len, "%s%s",
		                      comma ? ", " : "",
		                      assignment_to_code(&assignments[i]));
		buffer += len;
		buffer_len -= len;
		
		comma = true;
	}
}

// reverse of assignment_to_code()
void code_to_assignment(Joystick_assignment *assignment, const char *buffer)
{
	memset(assignment, 0, sizeof(*assignment));
	
	char axis = 0, direction = 0;
	
	if (sscanf(buffer, " AX %d%c", &assignment->num, &direction) == 2)
		assignment->type = AXIS;
	else if (sscanf(buffer, " BTN %d", &assignment->num) == 1)
		assignment->type = BUTTON;
	else if (sscanf(buffer, " H %d%c%c", &assignment->num, &axis, &direction) == 3)
		assignment->type = HAT;
	
	if (assignment->num == 0)
		assignment->type = NONE;
	else
		--assignment->num;
	
	assignment->x_axis = (toupper(axis) == 'X');
	assignment->negative_axis = (toupper(direction) == '-');
}

/* gives the short (6 or less characters) identifier for a joystick assignment
 * 
 * two of these per direction/action is all that can fit on the joystick config screen,
 * assuming two digits for the axis/button/hat number
 */
const char *assignment_to_code(const Joystick_assignment *assignment)
{
	static char name[7];
	
	switch (assignment->type)
	{
	case NONE:
		strcpy(name, "");
		break;
		
	case AXIS:
		snprintf(name, sizeof(name), "AX %d%c",
		         assignment->num + 1,
		         assignment->negative_axis ? '-' : '+');
		break;
		
	case BUTTON:
		snprintf(name, sizeof(name), "BTN %d",
		         assignment->num + 1);
		break;
		
	case HAT:
		snprintf(name, sizeof(name), "H %d%c%c",
		         assignment->num + 1,
		         assignment->x_axis ? 'X' : 'Y',
		         assignment->negative_axis ? '-' : '+');
		break;
	}
	
	return name;
}

// captures joystick input for configuring assignments
// returns false if non-joystick input was detected
// TODO: input from joystick other than the one being configured probably should not be ignored
bool detect_joystick_assignment(int j, Joystick_assignment *assignment)
{
	// get initial joystick state to compare against to see if anything was pressed
	
	const int axes = SDL_JoystickNumAxes(joystick[j].handle);
	Sint16 *axis = malloc(axes * sizeof(*axis));
	for (int i = 0; i < axes; i++)
		axis[i] = SDL_JoystickGetAxis(joystick[j].handle, i);
	
	const int buttons = SDL_JoystickNumButtons(joystick[j].handle);
	Uint8 *button = malloc(buttons * sizeof(*button));
	for (int i = 0; i < buttons; i++)
		button[i] = SDL_JoystickGetButton(joystick[j].handle, i);
	
	const int hats = SDL_JoystickNumHats(joystick[j].handle);
	Uint8 *hat = malloc(hats * sizeof(*hat));
	for (int i = 0; i < hats; i++)
		hat[i] = SDL_JoystickGetHat(joystick[j].handle, i);
	
	bool detected = false;

	// The controller "confirm" that opened this prompt pushed a synthetic RETURN
	// (push_joysticks_as_keyboard) that is still queued. Without draining it, the first
	// service_SDL_events() in the loop below sets newkey and aborts detection on frame 1,
	// making it impossible to bind anything with a controller. Consume it first.
	service_SDL_events(true);
	newkey = newmouse = false;

	do
	{
		setDelay(1);

		SDL_JoystickUpdate();
		
		for (int i = 0; i < axes; ++i)
		{
			Sint16 temp = SDL_JoystickGetAxis(joystick[j].handle, i);
			
			if (abs(temp - axis[i]) > joystick_analog_max * 2 / 3)
			{
				assignment->type = AXIS;
				assignment->num = i;
				assignment->negative_axis = temp < axis[i];
				detected = true;
				break;
			}
		}
		
		for (int i = 0; i < buttons; ++i)
		{
			Uint8 new_button = SDL_JoystickGetButton(joystick[j].handle, i),
			      changed = button[i] ^ new_button;
			
			if (!changed)
				continue;
			
			if (new_button == 0) // button was released
			{
				button[i] = new_button;
			}
			else                 // button was pressed
			{
				assignment->type = BUTTON;
				assignment->num = i;
				detected = true;
				break;
			}
		}
		
		for (int i = 0; i < hats; ++i)
		{
			Uint8 new_hat = SDL_JoystickGetHat(joystick[j].handle, i),
			      changed = hat[i] ^ new_hat;
			
			if (!changed)
				continue;
			
			if ((new_hat & changed) == SDL_HAT_CENTERED) // hat was centered
			{
				hat[i] = new_hat;
			}
			else
			{
				assignment->type = HAT;
				assignment->num = i;
				assignment->x_axis = changed & (SDL_HAT_LEFT | SDL_HAT_RIGHT);
				assignment->negative_axis = changed & (SDL_HAT_LEFT | SDL_HAT_UP);
				detected = true;
			}
		}
		
		service_SDL_events(true);
		JE_showVGA();
		
		wait_delay();
	} while (!detected && !newkey && !newmouse);
	
	free(axis);
	free(button);
	free(hat);
	
	return detected;
}

// compares relevant parts of joystick assignments for equality
bool joystick_assignment_cmp(const Joystick_assignment *a, const Joystick_assignment *b)
{
	if (a->type == b->type)
	{
		switch (a->type)
		{
		case NONE:
			return true;
		case AXIS:
			return (a->num == b->num) &&
			       (a->negative_axis == b->negative_axis);
		case BUTTON:
			return (a->num == b->num);
		case HAT:
			return (a->num == b->num) &&
			       (a->x_axis == b->x_axis) &&
			       (a->negative_axis == b->negative_axis);
		}
	}
	
	return false;
}
