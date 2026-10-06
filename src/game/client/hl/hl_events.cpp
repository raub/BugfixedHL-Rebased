/***
*
*	Copyright (c) 1996-2002, Valve LLC. All rights reserved.
*	
*	This product contains software technology licensed from Id 
*	Software, Inc. ("Id Technology").  Id Technology (c) 1996 Id Software, Inc. 
*	All Rights Reserved.
*
*   Use, distribution, and modification of this source code and/or resulting
*   object code is restricted to non-commercial enhancements to products from
*   Valve LLC.  All other use, distribution, or modification is prohibited
*   without written permission from Valve LLC.
*
****/
#include "../hud.h"
#include "../cl_util.h"
#include "event_api.h"
#include "ir/ir_producer.h"

extern "C"
{
	// HLDM
	void EV_FireGlock1(struct event_args_s *args);
	void EV_FireGlock2(struct event_args_s *args);
	void EV_FireShotGunSingle(struct event_args_s *args);
	void EV_FireShotGunDouble(struct event_args_s *args);
	void EV_FireMP5(struct event_args_s *args);
	void EV_FireMP52(struct event_args_s *args);
	void EV_FirePython(struct event_args_s *args);
	void EV_FireGauss(struct event_args_s *args);
	void EV_SpinGauss(struct event_args_s *args);
	void EV_Crowbar(struct event_args_s *args);
	void EV_FireCrossbow(struct event_args_s *args);
	void EV_FireCrossbow2(struct event_args_s *args);
	void EV_FireRpg(struct event_args_s *args);
	void EV_EgonFire(struct event_args_s *args);
	void EV_EgonStop(struct event_args_s *args);
	void EV_HornetGunFire(struct event_args_s *args);
	void EV_TripmineFire(struct event_args_s *args);
	void EV_SnarkFire(struct event_args_s *args);

	void EV_TrainPitchAdjust(struct event_args_s *args);
	void EV_VehiclePitchAdjust(struct event_args_s *args);
}

/*
======================
Game_HookEvents

Associate script file name with callback functions.  Callback's must be extern "C" so
 the engine doesn't get confused about name mangling stuff.  Note that the format is
 always the same.  Of course, a clever mod team could actually embed parameters, behavior
 into the actual .sc files and create a .sc file parser and hook their functionality through
 that.. i.e., a scripting system.

That was what we were going to do, but we ran out of time...oh well.
======================
*/
// IR Bot: hl-cl-producer reports other players' weapon events as sounds, so every event is
// registered through its wrapper.
static void HookEvent(const char *name, void (*fn)(struct event_args_s *))
{
	gEngfuncs.pfnHookEvent(name, ir_producer::WrapEvent(name, fn));
}

void Game_HookEvents(void)
{
	HookEvent("events/glock1.sc", EV_FireGlock1);
	HookEvent("events/glock2.sc", EV_FireGlock2);
	HookEvent("events/shotgun1.sc", EV_FireShotGunSingle);
	HookEvent("events/shotgun2.sc", EV_FireShotGunDouble);
	HookEvent("events/mp5.sc", EV_FireMP5);
	HookEvent("events/mp52.sc", EV_FireMP52);
	HookEvent("events/python.sc", EV_FirePython);
	HookEvent("events/gauss.sc", EV_FireGauss);
	HookEvent("events/gaussspin.sc", EV_SpinGauss);
	HookEvent("events/train.sc", EV_TrainPitchAdjust);
	HookEvent("events/vehicle.sc", EV_VehiclePitchAdjust);
	HookEvent("events/crowbar.sc", EV_Crowbar);
	HookEvent("events/crossbow1.sc", EV_FireCrossbow);
	HookEvent("events/crossbow2.sc", EV_FireCrossbow2);
	HookEvent("events/rpg.sc", EV_FireRpg);
	HookEvent("events/egon_fire.sc", EV_EgonFire);
	HookEvent("events/egon_stop.sc", EV_EgonStop);
	HookEvent("events/firehornet.sc", EV_HornetGunFire);
	HookEvent("events/tripfire.sc", EV_TripmineFire);
	HookEvent("events/snarkfire.sc", EV_SnarkFire);
}
