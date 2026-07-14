/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 * Copyright (C) 2007-2013  The OpenTyrian Development Team
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
#include "shots.h"

#include "config.h"
#include "endless.h"
#include "player.h"
#include "render_list.h"
#include "sprite.h"
#include "video.h"
#include "varz.h"

// I'm pretty sure the last extra entry is never used.
PlayerShotDataType playerShotData[MAX_PWEAPON + 1]; /* [1..MaxPWeapon+1] */
JE_byte shotAvail[MAX_PWEAPON]; /* [1..MaxPWeapon] */   /*0:Avail 1-255:Duration left*/

void simulate_player_shots(void)
{
	/* Player Shot Images */
	for (int z = 0; z < MAX_PWEAPON; z++)
	{
		if (shotAvail[z] != 0)
		{
			shotAvail[z]--;
			if (z != MAX_PWEAPON - 1)
			{
				PlayerShotDataType* shot = &playerShotData[z];

				// Entry position: the tick's delta becomes the render-list extrapolation
				// velocity (as in player_shot_move_and_draw), for the smooth shop preview.
				const int rl_shot_old_x = shot->shotX, rl_shot_old_y = shot->shotY;

				shot->shotXM += shot->shotXC;

				if (shot->shotXM <= 100)
					shot->shotX += shot->shotXM;

				shot->shotYM += shot->shotYC;
				shot->shotY += shot->shotYM;

				if (shot->shotYM > 100)
				{
					shot->shotY -= 120;
					shot->shotY += player[0].delta_y_shot_move;
				}

				if (shot->shotComplicated != 0)
				{
					shot->shotDevX += shot->shotDirX;
					shot->shotX += shot->shotDevX;

					if (abs(shot->shotDevX) == shot->shotCirSizeX)
						shot->shotDirX = -shot->shotDirX;

					shot->shotDevY += shot->shotDirY;
					shot->shotY += shot->shotDevY;

					if (abs(shot->shotDevY) == shot->shotCirSizeY)
						shot->shotDirY = -shot->shotDirY;
					/*Double Speed Circle Shots - add a second copy of above loop*/
				}

				int tempShotX = shot->shotX;
				int tempShotY = shot->shotY;

				if (shot->shotX < 0 || shot->shotX > 140 ||
				    shot->shotY < 0 || shot->shotY > 170)
				{
					shotAvail[z] = 0;
					goto draw_player_shot_loop_end;
				}

/*				if (shot->shotTrail != 255)
				{
					if (shot->shotTrail == 98)
					{
						JE_setupExplosion(shot->shotX - shot->shotXM, shot->shotY - shot->shotYM, shot->shotTrail);
					}
					else
					{
						JE_setupExplosion(shot->shotX, shot->shotY, shot->shotTrail);
					}
				}*/

				JE_word anim_frame = shot->shotGr + shot->shotAni;
				if (++shot->shotAni == shot->shotAniMax)
					shot->shotAni = 0;

				if (anim_frame < 60000)
				{
					rl_current_id = RL_ID_PSHOT_BASE + z;
					rl_current_vel_x = tempShotX - rl_shot_old_x;
					rl_current_vel_y = tempShotY - rl_shot_old_y;
					rl_current_acc_x = shot->shotXC;
					rl_current_acc_y = shot->shotYC;
					if (anim_frame > 1000)
					{
						// Match the in-game shot draw (player_shot_move_and_draw): a shot graphic
						// > 1000 leaves a superspark trail (colour bank = the thousands digit; see
						// JE_doSP). Without this the weapon-sim previews (shop + custom weapon
						// creator) silently dropped the superspark / sparky custom-bullet trail.
						JE_doSP(tempShotX+1 + 6, tempShotY + 6, 5, 3, (anim_frame / 1000) << 4, superSparkCapForSprite(anim_frame % 1000));
						anim_frame = anim_frame % 1000;
					}
					if (anim_frame > 500)
						blit_sprite2(VGAScreen, tempShotX+1, tempShotY, spriteSheet12, anim_frame - 500);
					else
						blit_sprite2(VGAScreen, tempShotX+1, tempShotY, spriteSheet8, anim_frame);
					rl_current_id = 0;
					rl_current_vel_x = 0;
					rl_current_vel_y = 0;
					rl_current_acc_x = 0;
					rl_current_acc_y = 0;
				}
				else if (anim_frame > 60000)
				{
					// Blended "special" sprite (e.g. the Plasma Storm cloud tiles). Mirror
					// player_shot_move_and_draw so the weapon-simulator preview (custom
					// weapon creator) shows these looks instead of silently skipping them.
					rl_current_id = RL_ID_PSHOT_BASE + z;
					rl_current_vel_x = tempShotX - rl_shot_old_x;
					rl_current_vel_y = tempShotY - rl_shot_old_y;
					rl_current_acc_x = shot->shotXC;
					rl_current_acc_y = shot->shotYC;
					blit_sprite_blend(VGAScreen, tempShotX+1, tempShotY, OPTION_SHAPES, anim_frame - 60001);
					rl_current_id = 0;
					rl_current_vel_x = 0;
					rl_current_vel_y = 0;
					rl_current_acc_x = 0;
					rl_current_acc_y = 0;
				}
			}

draw_player_shot_loop_end:
			;
		}
	}
}

static const JE_word linkMultiGr[17] /* [0..16] */ =
	{77,221,183,301,1,282,164,202,58,201,163,281,39,300,182,220,77};
static const JE_word linkSonicGr[17] /* [0..16] */ =
	{85,242,131,303,47,284,150,223,66,224,149,283,9,302,130,243,85};
static const JE_word linkMult2Gr[17] /* [0..16] */ =
	{78,299,295,297,2,278,276,280,59,279,275,277,40,296,294,298,78};

void player_shot_set_direction(JE_integer shot_id, uint weapon_id, JE_real direction)
{
	PlayerShotDataType* shot = &playerShotData[shot_id];

	shot->shotXM = -roundf(sinf(direction) * shot->shotYM);
	shot->shotYM = -roundf(cosf(direction) * shot->shotYM);

	// Some weapons have sprites for each direction, use those.
	int rounded_dir;

	switch (weapon_id)
	{
	case 27:
	case 32:
	case 10:
		rounded_dir = roundf(direction * (16 / (2 * M_PI)));  /*16 directions*/
		shot->shotGr = linkMultiGr[rounded_dir];
		break;
	case 28:
	case 33:
	case 11:
		rounded_dir = roundf(direction * (16 / (2 * M_PI)));  /*16 directions*/
		shot->shotGr = linkSonicGr[rounded_dir];
		break;
	case 30:
	case 35:
	case 14:
		if (direction > M_PI_2 && direction < M_PI + M_PI_2)
		{
			shot->shotYC = 1;
		}
		break;
	case 38:
	case 22:
		rounded_dir = roundf(direction * (16 / (2 * M_PI)));  /*16 directions*/
		shot->shotGr = linkMult2Gr[rounded_dir];
		break;
	}
}

bool player_shot_move_and_draw(
		int shot_id, bool* out_is_special,
		int* out_shotx, int* out_shoty,
		JE_integer* out_shot_damage, JE_byte* out_blast_filter,
		JE_byte* out_chain, JE_byte* out_playerNum,
		JE_word* out_special_radiusw, JE_word* out_special_radiush)
{
	PlayerShotDataType* shot = &playerShotData[shot_id];

	shotAvail[shot_id]--;
	if (shot_id != MAX_PWEAPON - 1)
	{
		// Entry position: the tick's delta becomes the render-list extrapolation
		// velocity (rl_current_vel_*, set at the blit below).
		const int rl_shot_old_x = shot->shotX, rl_shot_old_y = shot->shotY;

		shot->shotXM += shot->shotXC;
		shot->shotX += shot->shotXM;
		JE_integer tmp_shotXM = shot->shotXM;

		if (shot->shotXM > 100)
		{
			if (shot->shotXM == 101)
			{
				shot->shotX -= 101;
				shot->shotX += player[shot->playerNumber-1].delta_x_shot_move;
				shot->shotY += player[shot->playerNumber-1].delta_y_shot_move;
			}
			else
			{
				shot->shotX -= 120;
				shot->shotX += player[shot->playerNumber-1].delta_x_shot_move;
			}
		}

		shot->shotYM += shot->shotYC;
		shot->shotY += shot->shotYM;

		if (shot->shotYM > 100)
		{
			shot->shotY -= 120;
			shot->shotY += player[shot->playerNumber-1].delta_y_shot_move;
		}

		if (shot->shotComplicated != 0)
		{
			shot->shotDevX += shot->shotDirX;
			shot->shotX += shot->shotDevX;

			if (abs(shot->shotDevX) == shot->shotCirSizeX)
				shot->shotDirX = -shot->shotDirX;

			shot->shotDevY += shot->shotDirY;
			shot->shotY += shot->shotDevY;

			if (abs(shot->shotDevY) == shot->shotCirSizeY)
				shot->shotDirY = -shot->shotDirY;

			/*Double Speed Circle Shots - add a second copy of above loop*/
		}

		*out_shotx = shot->shotX;
		*out_shoty = shot->shotY;

		// Top cull margin widened -15 -> -40, so interpolation still gets recorded ticks
		// past the edge and a fast shot's exit animates off-screen instead of popping.
		// Ascending shots only: a decelerating shot (e.g. Vulcan Cannon) apexing between
		// -15 and -40 would fall back on-screen, so past -15 cull once it stops ascending
		// (shotYM >= 0).
		if (shot->shotX < -34 || shot->shotX > PLAYFIELD_WIDTH + 34 ||
			shot->shotY < -40 || shot->shotY > 190 ||
			(shot->shotY < -15 && shot->shotYM >= 0))
		{
			shotAvail[shot_id] = 0;
			return false;
		}

		if (shot->shotTrail != 255)
		{
			if (shot->shotTrail == 98 || shot->shotTrail == 198)
				JE_setupExplosion(shot->shotX - shot->shotXM, shot->shotY - shot->shotYM, 0, shot->shotTrail, false, false);
			else
				JE_setupExplosion(shot->shotX, shot->shotY, 0, shot->shotTrail, false, false);
		}

		if (shot->aimAtEnemy != 0)
		{
			if (--shot->aimDelay == 0)
			{
				shot->aimDelay = shot->aimDelayMax;

				if (enemyAvail[shot->aimAtEnemy - 1] != 1)
				{
					if (shot->shotX < enemy[shot->aimAtEnemy - 1].ex)
						shot->shotXM++;
					else
						shot->shotXM--;

					if (shot->shotY < enemy[shot->aimAtEnemy - 1].ey)
						shot->shotYM++;
					else
						shot->shotYM--;
				}
				else
				{
					if (shot->shotXM > 0)
						shot->shotXM++;
					else
						shot->shotXM--;
				}
			}
		}

		JE_word sprite_frame = shot->shotGr + shot->shotAni;
		if (++shot->shotAni == shot->shotAniMax)
			shot->shotAni = 0;

		*out_shot_damage = shot->shotDmg;
		*out_blast_filter = shot->shotBlastFilter;
		*out_chain = shot->chainReaction;
		*out_playerNum = shot->playerNumber;

		*out_is_special = sprite_frame > 60000;

		// Ship-tracking shots (shotXM/shotYM > 100 sentinels) draw at the ship's render-
		// rate position on the attached axis so the beam base stays on the gun; the
		// travelling axis extrapolates (see ship_attach in render_list.c). Velocity is
		// this tick's real on-screen delta (post-accel, post-circle, post ship-lock), so
		// the exact motion extrapolates — fast shots exit the top cleanly, recycled slots
		// never streak.
		rl_current_id = RL_ID_PSHOT_BASE + shot_id;
		rl_current_vel_x = shot->shotX - rl_shot_old_x;
		rl_current_vel_y = shot->shotY - rl_shot_old_y;
		// Acceleration lets the travelling axis extrapolate a decelerating shot
		// (e.g. Vulcan Cannon) without overshooting and snapping back; the attached
		// axis ignores it (orbit math uses pure velocity).
		rl_current_acc_x = shot->shotXC;
		rl_current_acc_y = shot->shotYC;
		rl_shot_attach = (shot->shotXM > 100 ? 1 : 0)
		               | (shot->shotYM > 100 ? 2 : 0)
		               | ((shot->playerNumber - 1) << 2);
		if (*out_is_special)
		{
			blit_sprite_blend(VGAScreen, *out_shotx+1, *out_shoty, OPTION_SHAPES, sprite_frame - 60001);

			*out_special_radiusw = sprite(OPTION_SHAPES, sprite_frame - 60001)->width / 2;
			*out_special_radiush = sprite(OPTION_SHAPES, sprite_frame - 60001)->height / 2;
		}
		else
		{
			if (sprite_frame > 1000)
			{
				JE_doSP(*out_shotx+1 + 6, *out_shoty + 6, 5, 3, (sprite_frame / 1000) << 4, superSparkCapForSprite(sprite_frame % 1000));
				sprite_frame = sprite_frame % 1000;
			}
			if (sprite_frame > 500)
			{
				if (background2 && *out_shoty + shadowYDist < 190 && tmp_shotXM < 100)
					blit_sprite2_darken(VGAScreen, *out_shotx+1, *out_shoty + shadowYDist, spriteSheet12, sprite_frame - 500);
				blit_sprite2(VGAScreen, *out_shotx+1, *out_shoty, spriteSheet12, sprite_frame - 500);
			}
			else
			{
				if (background2 && *out_shoty + shadowYDist < 190 && tmp_shotXM < 100)
					blit_sprite2_darken(VGAScreen, *out_shotx+1, *out_shoty + shadowYDist, spriteSheet8, sprite_frame);
				blit_sprite2(VGAScreen, *out_shotx+1, *out_shoty, spriteSheet8, sprite_frame);
			}
		}
		rl_current_id = 0;
		rl_current_vel_x = 0;
		rl_current_vel_y = 0;
		rl_current_acc_x = 0;
		rl_current_acc_y = 0;
		rl_shot_attach = 0;
	}

	return true;
}

JE_integer player_shot_create(JE_word portNum, uint bay_i, JE_word PX, JE_word PY, JE_word mouseX, JE_word mouseY, JE_word wpNum, JE_byte playerNum)
{
	static const JE_byte soundChannel[11] /* [1..11] */ = {0, 2, 4, 4, 2, 2, 5, 5, 1, 4, 1};

	// Bounds check
	if (portNum > PORT_NUM || wpNum <= 0 || wpNum > WEAP_NUM)
		return MAX_PWEAPON;

	const JE_WeaponType* weapon = &weapons[wpNum];

	uint power_use = weaponPort[portNum].poweruse;
	if (expertMode)
		power_use = power_use * expertEnergyPct / 100;

	// Endless "Efficient Coils" perk: trim the generator draw per main-weapon shot (100 = normal),
	// so a given generator sustains heavier fire. Scales the base cost before the special/boost zeroing.
	if (endlessMode)
		power_use = power_use * endlessPerkPowerUsePercent() / 100;

	// Special-weapon shots are paid for upfront in shield/armor; they must not also
	// drain the generator, or a sustained special (e.g. an active Minefield) starves
	// the main weapon of power.
	if (bay_i == SHOT_SPECIAL || bay_i == SHOT_SPECIAL2)
		power_use = 0;

	// Endless kill-fire BOON (Turbodrive / Overdrive): the boosted fire rate must not drain the
	// generator faster, so shots fired during the window are power-free. An evil curse fires SLOWER,
	// so it gets no such break -- normal power cost applies.
	if (endlessMode && endlessTurbodriveActive() && !endlessKillFireIsEvil())
		power_use = 0;

	if (!cheatInfiniteGenerator)
	{
		if (power < power_use)
			return MAX_PWEAPON;
		power -= power_use;
	}

	if (weapon->sound > 0)
		soundQueue[soundChannel[bay_i]] = weapon->sound;

	int shot_id = MAX_PWEAPON;
	/*Rot*/
	for (int multi_i = 1; multi_i <= weapon->multi; multi_i++)
	{
		for (shot_id = 0; shot_id < MAX_PWEAPON; shot_id++)
			if (shotAvail[shot_id] == 0)
				break;
		if (shot_id == MAX_PWEAPON)
			return MAX_PWEAPON;

		// Fire-cursor wrap must test >=, not ==: the cursor is a persistent per-bay global, so a
		// weapon swap mid-cycle can leave it above the new max, silently killing the gun. notes.md §Weapons.
		if ((weapon->max != 0 && shotMultiPos[bay_i] >= weapon->max) ||
		    shotMultiPos[bay_i] >= WEAPON_MULTI_MAX ||
		    (weapon->max == 0 && shotMultiPos[bay_i] > 8))
			shotMultiPos[bay_i] = 1;
		else
			shotMultiPos[bay_i]++;

		PlayerShotDataType* shot = &playerShotData[shot_id];
		shot->chainReaction = 0;

		shot->playerNumber = playerNum;

		shot->shotAni = 0;

		shot->shotComplicated = weapon->circlesize != 0;

		if (weapon->circlesize == 0)
		{
			shot->shotDevX = 0;
			shot->shotDirX = 0;
			shot->shotDevY = 0;
			shot->shotDirY = 0;
			shot->shotCirSizeX = 0;
			shot->shotCirSizeY = 0;
		}
		else
		{
			JE_byte circsize = weapon->circlesize;

			if (circsize > 19)
			{
				JE_byte circsize_mod20 = circsize % 20;
				shot->shotCirSizeX = circsize_mod20;
				shot->shotDevX = circsize_mod20 >> 1;

				circsize = circsize / 20;
				shot->shotCirSizeY = circsize;
				shot->shotDevY = circsize >> 1;
			}
			else
			{
				shot->shotCirSizeX = circsize;
				shot->shotCirSizeY = circsize;
				shot->shotDevX = circsize >> 1;
				shot->shotDevY = circsize >> 1;
			}
			shot->shotDirX = 1;
			shot->shotDirY = -1;
		}

		shot->shotTrail = weapon->trail;

		// Tyrian 2000: I'm not sure precisely how it does it in the original game, but trail 198 results in
		// only the Flying Punch's center tile having a trail. This replicates that behavior
		if (shot->shotTrail == 198 && shotMultiPos[bay_i] > 1)
		{
			shot->shotTrail = 255;
		}

		if (weapon->attack[shotMultiPos[bay_i]-1] > 99 && weapon->attack[shotMultiPos[bay_i]-1] < 250)
		{
			shot->chainReaction = weapon->attack[shotMultiPos[bay_i]-1] - 100;
			shot->shotDmg = 1;
		}
		else
		{
			shot->shotDmg = weapon->attack[shotMultiPos[bay_i]-1];
		}

		shot->shotBlastFilter = weapon->shipblastfilter;

		JE_integer tmp_by = weapon->by[shotMultiPos[bay_i]-1];

		/*Note: Only front selection used for player shots...*/

		shot->shotX = PX + weapon->bx[shotMultiPos[bay_i]-1];

		shot->shotY = PY + tmp_by;
		shot->shotYC = -weapon->acceleration;
		shot->shotXC = weapon->accelerationx;

		shot->shotXM = weapon->sx[shotMultiPos[bay_i]-1];

		// Not sure what this field does exactly.
		JE_byte del = weapon->del[shotMultiPos[bay_i]-1];

		if (del == 121)
		{
			shot->shotTrail = 0;
			del = 255;
		}

		shot->shotGr = weapon->sg[shotMultiPos[bay_i]-1];
		if (shot->shotGr == 0)
			shotAvail[shot_id] = 0;
		else
			shotAvail[shot_id] = del;

		if (del > 100 && del < 120)
			shot->shotAniMax = (del - 100 + 1);
		else
			shot->shotAniMax = weapon->weapani + 1;

		if (del == 99 || del == 98)
		{
			tmp_by = PX - mouseX;
			if (tmp_by < -5)
				tmp_by = -5;
			else if (tmp_by > 5)
				tmp_by = 5;
			shot->shotXM += tmp_by;
		}

		if (del == 99 || del == 100)
		{
			tmp_by = PY - mouseY - weapon->sy[shotMultiPos[bay_i]-1];
			if (tmp_by < -4)
				tmp_by = -4;
			else if (tmp_by > 4)
				tmp_by = 4;
			shot->shotYM = tmp_by;
		}
		else if (weapon->sy[shotMultiPos[bay_i]-1] == 98)
		{
			shot->shotYM = 0;
			shot->shotYC = -1;
		}
		else if (weapon->sy[shotMultiPos[bay_i]-1] > 100)
		{
			shot->shotYM = weapon->sy[shotMultiPos[bay_i]-1];
			shot->shotY -= player[shot->playerNumber-1].delta_y_shot_move;
		}
		else
		{
			shot->shotYM = -weapon->sy[shotMultiPos[bay_i]-1];
		}

		if (weapon->sx[shotMultiPos[bay_i]-1] > 100)
		{
			shot->shotXM = weapon->sx[shotMultiPos[bay_i]-1];
			shot->shotX -= player[shot->playerNumber-1].delta_x_shot_move;
			if (shot->shotXM == 101)
				shot->shotY -= player[shot->playerNumber-1].delta_y_shot_move;
		}

		if (weapon->aim > 5)  /*Guided Shot*/
		{
			uint best_dist = 65000;
			JE_byte closest_enemy = 0;
			/*Find Closest Enemy*/
			for (x = 0; x < 100; x++)
			{
				if (enemyAvail[x] != 1 && !enemy[x].scoreitem)
				{
					y = abs(enemy[x].ex - shot->shotX) + abs(enemy[x].ey - shot->shotY);
					if (y < best_dist)
					{
						best_dist = y;
						closest_enemy = x + 1;
					}
				}
			}
			shot->aimAtEnemy = closest_enemy;
			shot->aimDelay = 5;
			shot->aimDelayMax = weapon->aim - 5;
		}
		else
		{
			shot->aimAtEnemy = 0;
		}

		shotRepeat[bay_i] = weapon->shotrepeat;
		// Endless Evil Turbodrive/Overdrive curse: JAM the guns by lengthening the cooldown as the
		// kill combo climbs (0 unless an evil kill-fire window is up). Main/sidekick guns only -- the
		// special bays run their own cadence (see varz.c). shotRepeat is a byte, so clamp.
		if (endlessMode && bay_i != SHOT_SPECIAL && bay_i != SHOT_SPECIAL2)
		{
			const int jam = endlessKillFireJamTicks();
			if (jam > 0)
			{
				const int v = shotRepeat[bay_i] + jam;
				shotRepeat[bay_i] = (v > 250) ? 250 : (JE_byte)v;
			}
		}
	}

	return shot_id;
}
