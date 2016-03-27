// Emacs style mode select	 -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//		Movement, collision handling.
//		Shooting and aiming.
//
//-----------------------------------------------------------------------------

#include <stdlib.h>
#include <math.h>

#include "templates.h"

#include "m_bbox.h"
#include "m_random.h"
#include "i_system.h"
#include "c_dispatch.h"
#include "math/cmath.h"

#include "doomdef.h"
#include "p_local.h"
#include "p_spec.h"
#include "d_player.h"
#include "p_maputl.h"
#include "p_lnspec.h"
#include "p_effect.h"
#include "p_terrain.h"
#include "p_trace.h"
#include "p_checkposition.h"
#include "r_utility.h"

#include "s_sound.h"
#include "decallib.h"

// State.
#include "doomstat.h"
#include "r_state.h"

#include "gi.h"

#include "a_sharedglobal.h"
#include "p_conversation.h"
#include "r_data/r_translate.h"
#include "g_level.h"
#include "r_sky.h"

CVAR(Bool, cl_bloodsplats, true, CVAR_ARCHIVE)
CVAR(Int, sv_smartaim, 0, CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(Bool, cl_doautoaim, false, CVAR_ARCHIVE)

static void CheckForPushSpecial(line_t *line, int side, AActor *mobj, DVector2 * posforwindowcheck = NULL);
static void SpawnShootDecal(AActor *t1, const FTraceResults &trace);
static void SpawnDeepSplash(AActor *t1, const FTraceResults &trace, AActor *puff);

static FRandom pr_tracebleed("TraceBleed");
static FRandom pr_checkthing("CheckThing");
static FRandom pr_lineattack("LineAttack");
static FRandom pr_crunch("DoCrunch");

// keep track of special lines as they are hit,
// but don't process them until the move is proven valid
TArray<spechit_t> spechit;
TArray<spechit_t> portalhit;

// Temporary holder for thing_sectorlist threads
msecnode_t* sector_list = NULL;		// phares 3/16/98

//==========================================================================
//
// FindRefPoint
//
// Finds the point on the line closest to the given coordinate
//
//==========================================================================

static DVector2 FindRefPoint(line_t *ld, const DVector2 &pos)
{
	// If there's any chance of slopes getting in the way we need to get a proper refpoint, otherwise we can save the work.
	// Slopes can get in here when:
	// - the actual sector planes are sloped
	// - there's 3D floors in this sector
	// - there's a crossable floor portal (for which the dropoff needs to be calculated within P_LineOpening, and the lower sector can easily have slopes)
	//
	// Todo: check if this bootload of checks even helps or if it adds more than it saves
	//
	if 	(ld->frontsector->floorplane.isSlope() ||
			ld->backsector->floorplane.isSlope() ||
			ld->frontsector->ceilingplane.isSlope() ||
			ld->backsector->ceilingplane. isSlope() ||
			ld->backsector->e->XFloor.ffloors.Size() != 0 ||
			ld->frontsector->e->XFloor.ffloors.Size() != 0 ||
			!ld->frontsector->PortalBlocksMovement(sector_t::ceiling) ||
			!ld->frontsector->PortalBlocksMovement(sector_t::floor))
	{

		DVector2 v1 = ld->v1->fPos();
		DVector2 d = ld->Delta();
		double r = clamp(((pos.X - v1.X) * d.X + (pos.Y - v1.Y) * d.Y) / (d.X*d.X + d.Y*d.Y), 0., 1.);
		return v1 + d*r;
	}
	return pos;
}

//==========================================================================
//
// PIT_FindFloorCeiling
//
// only3d set means to only check against 3D floors and midtexes.
//
//==========================================================================
bool ffcf_verbose;

static bool PIT_FindFloorCeiling(FMultiBlockLinesIterator &mit, FMultiBlockLinesIterator::CheckResult &cres, const FBoundingBox &box, FCheckPosition &tmf, int flags)
{
	line_t *ld = cres.line;

	if (!box.inRange(ld) || box.BoxOnLineSide(ld) != -1)
		return true;

	// A line has been hit

	if (ffcf_verbose)
	{
		Printf("Hit line %d at position %f,%f, group %d\n",
			int(ld - lines), cres.Position.X, cres.Position.Y, ld->frontsector->PortalGroup);
	}

	if (!ld->backsector)
	{ // One sided line
		return true;
	}

	DVector2 refpoint = FindRefPoint(ld, cres.Position);
	FLineOpening open;

	P_LineOpening(open, tmf.thing, ld, refpoint, &cres.Position, flags);

	// adjust floor / ceiling heights
	if (!(flags & FFCF_NOCEILING))
	{
		if (open.top < tmf.ceilingz)
		{
			tmf.ceilingz = open.top;
			if (open.topsec != NULL) tmf.ceilingsector = open.topsec;
			if (ffcf_verbose) Printf("    Adjust ceilingz to %f\n", open.top);
			mit.StopUp();
		}
	}

	if (!(flags & FFCF_NOFLOOR))
	{
		if (open.bottom > tmf.floorz)
		{
			tmf.floorz = open.bottom;
			if (open.bottomsec != NULL) tmf.floorsector = open.bottomsec;
			tmf.touchmidtex = open.touchmidtex;
			tmf.abovemidtex = open.abovemidtex;
			if (ffcf_verbose) Printf("    Adjust floorz to %f\n", open.bottom);
			if (tmf.floorz > tmf.dropoffz + tmf.thing->MaxDropOffHeight) mit.StopDown();
		}
		else if (open.bottom == tmf.floorz)
		{
			tmf.touchmidtex |= open.touchmidtex;
			tmf.abovemidtex |= open.abovemidtex;
		}

		if (open.lowfloor < tmf.dropoffz && open.lowfloor > LINEOPEN_MIN)
		{
			tmf.dropoffz = open.lowfloor;
			if (ffcf_verbose) Printf("    Adjust dropoffz to %f\n", open.bottom);
			if (tmf.floorz > tmf.dropoffz + tmf.thing->MaxDropOffHeight) mit.StopDown();
		}
	}
	return true;
}


//==========================================================================
//
// calculates the actual floor and ceiling position at a given
// coordinate. Traverses through portals unless being told not to.
//
//==========================================================================

void P_GetFloorCeilingZ(FCheckPosition &tmf, int flags)
{
	sector_t *sec = (!(flags & FFCF_SAMESECTOR) || tmf.thing->Sector == NULL)? P_PointInSector(tmf.pos) : tmf.sector;
	F3DFloor *ffc, *fff;

	tmf.ceilingz = sec->NextHighestCeilingAt(tmf.pos.X, tmf.pos.Y, tmf.pos.Z, tmf.pos.Z + tmf.thing->Height, flags, &tmf.ceilingsector, &ffc);
	tmf.floorz = tmf.dropoffz = sec->NextLowestFloorAt(tmf.pos.X, tmf.pos.Y, tmf.pos.Z, flags, tmf.thing->MaxStepHeight, &tmf.floorsector, &fff);

	if (fff)
	{
		tmf.floorpic = *fff->top.texture;
		tmf.floorterrain = fff->model->GetTerrain(fff->top.isceiling);
	}
	else
	{
		tmf.floorpic = tmf.floorsector->GetTexture(sector_t::floor);
		tmf.floorterrain = tmf.floorsector->GetTerrain(sector_t::floor);
	}
	tmf.ceilingpic = ffc ? *ffc->bottom.texture : tmf.ceilingsector->GetTexture(sector_t::ceiling);
	tmf.sector = sec;

}

//==========================================================================
//
// P_FindFloorCeiling
//
//==========================================================================

void P_FindFloorCeiling(AActor *actor, int flags)
{
	FCheckPosition tmf;

	tmf.thing = actor;
	tmf.pos = actor->Pos();

	if (flags & FFCF_ONLYSPAWNPOS)
	{
		flags |= FFCF_3DRESTRICT;
	}
	if (flags & FFCF_SAMESECTOR)
	{
		tmf.sector = actor->Sector;
	}
	P_GetFloorCeilingZ(tmf, flags);
	assert(tmf.thing->Sector != NULL);

	actor->floorz = tmf.floorz;
	actor->dropoffz = tmf.dropoffz;
	actor->ceilingz = tmf.ceilingz;
	actor->floorpic = tmf.floorpic;
	actor->floorterrain = tmf.floorterrain;
	actor->floorsector = tmf.floorsector;
	actor->ceilingpic = tmf.ceilingpic;
	actor->ceilingsector = tmf.ceilingsector;
	if (ffcf_verbose) Printf("Starting with ceilingz = %f, floorz = %f\n", tmf.ceilingz, tmf.floorz);

	tmf.touchmidtex = false;
	tmf.abovemidtex = false;
	validcount++;

	FPortalGroupArray grouplist;
	FMultiBlockLinesIterator mit(grouplist, actor);
	FMultiBlockLinesIterator::CheckResult cres;

	// if we already have a valid floor/ceiling sector within the current sector, 
	// we do not need to iterate through plane portals to find a floor or ceiling.
	if (actor->floorsector == actor->Sector) mit.StopDown();
	if (actor->ceilingsector == actor->Sector) mit.StopUp();

	while ((mit.Next(&cres)))
	{
		PIT_FindFloorCeiling(mit, cres, mit.Box(), tmf, flags|cres.portalflags);
	}

	if (tmf.touchmidtex) tmf.dropoffz = tmf.floorz;

	bool usetmf = !(flags & FFCF_ONLYSPAWNPOS) || (tmf.abovemidtex && (tmf.floorz <= actor->Z()));

	// when actual floor or ceiling are beyond a portal plane we also need to use the result of the blockmap iterator, regardless of the flags being specified.
	if (usetmf || tmf.floorsector->PortalGroup != actor->Sector->PortalGroup)
	{
		actor->floorz = tmf.floorz;
		actor->dropoffz = tmf.dropoffz;
		actor->floorpic = tmf.floorpic;
		actor->floorterrain = tmf.floorterrain;
		actor->floorsector = tmf.floorsector;
	}

	if (usetmf || tmf.ceilingsector->PortalGroup != actor->Sector->PortalGroup)
	{
		actor->ceilingz = tmf.ceilingz;
		actor->ceilingpic = tmf.ceilingpic;
		actor->ceilingsector = tmf.ceilingsector;
	}
}

// Debug CCMD for checking errors in the MultiBlockLinesIterator (needs to be removed when this code is complete)
CCMD(ffcf)
{
	ffcf_verbose = true;
	P_FindFloorCeiling(players[0].mo, 0);
	ffcf_verbose = false;
}
//==========================================================================
//
// TELEPORT MOVE
// 

//
// P_TeleportMove
//
// [RH] Added telefrag parameter: When true, anything in the spawn spot
//		will always be telefragged, and the move will be successful.
//		Added z parameter. Originally, the thing's z was set *after* the
//		move was made, so the height checking I added for 1.13 could
//		potentially erroneously indicate the move was okay if the thing
//		was being teleported between two non-overlapping height ranges.
//
//==========================================================================

bool	P_TeleportMove(AActor* thing, const DVector3 &pos, bool telefrag, bool modifyactor) 
{
	FCheckPosition tmf;
	sector_t *oldsec = thing->Sector;

	// kill anything occupying the position


	// The base floor/ceiling is from the subsector that contains the point.
	// Any contacted lines the step closer together will adjust them.
	tmf.thing = thing;
	tmf.pos = pos;
	tmf.touchmidtex = false;
	tmf.abovemidtex = false;
	P_GetFloorCeilingZ(tmf, 0);

	bool StompAlwaysFrags = ((thing->flags2 & MF2_TELESTOMP) || (level.flags & LEVEL_MONSTERSTELEFRAG) || telefrag) && !(thing->flags7 & MF7_NOTELESTOMP);

	// P_LineOpening requires the thing's z to be the destination z in order to work.
	double savedz = thing->Z();
	thing->SetZ(pos.Z);
	sector_t *sector = P_PointInSector(pos);

	FPortalGroupArray grouplist;
	FMultiBlockLinesIterator mit(grouplist, pos.X, pos.Y, pos.Z, thing->Height, thing->radius, sector);
	FMultiBlockLinesIterator::CheckResult cres;

	while (mit.Next(&cres))
	{
		PIT_FindFloorCeiling(mit, cres, mit.Box(), tmf, 0);
	}
	thing->SetZ(savedz);

	if (tmf.touchmidtex) tmf.dropoffz = tmf.floorz;

	FMultiBlockThingsIterator mit2(grouplist, pos.X, pos.Y, pos.Z, thing->Height, thing->radius, false, sector);
	FMultiBlockThingsIterator::CheckResult cres2;

	while (mit2.Next(&cres2))
	{
		AActor *th = cres2.thing;

		if (!(th->flags & MF_SHOOTABLE))
			continue;

		// don't clip against self
		if (th == thing)
			continue;

		double blockdist = th->radius + tmf.thing->radius;
		if (fabs(th->X() - cres2.Position.X) >= blockdist || fabs(th->Y() - cres2.Position.Y) >= blockdist)
			continue;

		if ((th->flags2 | tmf.thing->flags2) & MF2_THRUACTORS)
			continue;

		if (tmf.thing->flags6 & MF6_THRUSPECIES && tmf.thing->GetSpecies() == th->GetSpecies())
			continue;

		// [RH] Z-Check
		// But not if not MF2_PASSMOBJ or MF3_DONTOVERLAP are set!
		// Otherwise those things would get stuck inside each other.
		if ((thing->flags2 & MF2_PASSMOBJ || th->flags4 & MF4_ACTLIKEBRIDGE) && !(i_compatflags & COMPATF_NO_PASSMOBJ))
		{
			if (!(th->flags3 & thing->flags3 & MF3_DONTOVERLAP))
			{
				if (pos.Z > th->Top() ||	// overhead
					pos.Z + thing->Height < th->Z())	// underneath
					continue;
			}
		}

		// monsters don't stomp things except on boss level
		// [RH] Some Heretic/Hexen monsters can telestomp
		// ... and some items can never be telefragged while others will be telefragged by everything that teleports upon them.
		if ((StompAlwaysFrags && !(th->flags6 & MF6_NOTELEFRAG)) || (th->flags7 & MF7_ALWAYSTELEFRAG))
		{
			// Don't actually damage if predicting a teleport
			if (thing->player == NULL || !(thing->player->cheats & CF_PREDICTING))
				P_DamageMobj(th, thing, thing, TELEFRAG_DAMAGE, NAME_Telefrag, DMG_THRUSTLESS);
			continue;
		}
		return false;
	}

	if (modifyactor)
	{
		// the move is ok, so link the thing into its new position
		thing->SetOrigin(pos, false);
		thing->floorz = tmf.floorz;
		thing->ceilingz = tmf.ceilingz;
		thing->floorsector = tmf.floorsector;
		thing->floorpic = tmf.floorpic;
		thing->floorterrain = tmf.floorterrain;
		thing->ceilingsector = tmf.ceilingsector;
		thing->ceilingpic = tmf.ceilingpic;
		thing->dropoffz = tmf.dropoffz;        // killough 11/98
		thing->BlockingLine = NULL;

		if (thing->flags2 & MF2_FLOORCLIP)
		{
			thing->AdjustFloorClip();
		}

		if (thing == players[consoleplayer].camera)
		{
			R_ResetViewInterpolation();
		}

		// If this teleport was caused by a move, P_TryMove() will handle the
		// sector transition messages better than we can here.
		if (!(thing->flags6 & MF6_INTRYMOVE))
		{
			thing->CheckSectorTransition(oldsec);
		}
	}

	return true;
}

//==========================================================================
//
// [RH] P_PlayerStartStomp
//
// Like P_TeleportMove, but it doesn't move anything, and only monsters and other
// players get telefragged.
//
//==========================================================================

void P_PlayerStartStomp(AActor *actor, bool mononly)
{
	FPortalGroupArray grouplist;
	FMultiBlockThingsIterator mit(grouplist, actor);
	FMultiBlockThingsIterator::CheckResult cres;

	while ((mit.Next(&cres)))
	{
		AActor *th = cres.thing;

		if (!(th->flags & MF_SHOOTABLE))
			continue;

		// don't clip against self, and don't kill your own voodoo dolls
		if (th == actor || (th->player == actor->player && th->player != NULL))
			continue;

		double blockdist = th->radius + actor->radius;
		if (fabs(th->X() - cres.Position.X) >= blockdist || fabs(th->Y() - cres.Position.Y) >= blockdist)
			continue;

		// only kill monsters and other players
		if (th->player == NULL && !(th->flags3 & MF3_ISMONSTER))
			continue;

		if (th->player != NULL && mononly)
			continue;

		if (actor->Z() > th->Top())
			continue;        // overhead
		if (actor->Top() < th->Z())
			continue;        // underneath

		P_DamageMobj(th, actor, actor, TELEFRAG_DAMAGE, NAME_Telefrag);
	}
}

//==========================================================================
//
// killough 8/28/98:
//
// P_GetFriction()
//
// Returns the friction associated with a particular mobj.
//
//==========================================================================

double P_GetFriction(const AActor *mo, double *frictionfactor)
{
	double friction = ORIG_FRICTION;
	double movefactor = ORIG_FRICTION_FACTOR;
	double newfriction;
	double newmf;

	const msecnode_t *m;
	sector_t *sec;

	if (mo->IsNoClip2())
	{
		// The default values are fine for noclip2 mode
	}
	else if (mo->flags2 & MF2_FLY && mo->flags & MF_NOGRAVITY)
	{
		friction = FRICTION_FLY;
	}
	else if ((!(mo->flags & MF_NOGRAVITY) && mo->waterlevel > 1) ||
		(mo->waterlevel == 1 && mo->Z() > mo->floorz+ 6))
	{
		friction = mo->Sector->GetFriction(sector_t::floor, &movefactor);
		movefactor *= 0.5;

		// Check 3D floors -- might be the source of the waterlevel
		for (unsigned i = 0; i < mo->Sector->e->XFloor.ffloors.Size(); i++)
		{
			F3DFloor *rover = mo->Sector->e->XFloor.ffloors[i];
			if (!(rover->flags & FF_EXISTS)) continue;
			if (!(rover->flags & FF_SWIMMABLE)) continue;

			if (mo->Z() > rover->top.plane->ZatPointF(mo) ||
				mo->Z() < rover->bottom.plane->ZatPointF(mo))
				continue;

			newfriction = rover->model->GetFriction(rover->top.isceiling, &newmf);
			if (newfriction < friction || friction == ORIG_FRICTION)
			{
				friction = newfriction;
				movefactor = newmf * 0.5;
			}
		}
	}
	else if (var_friction && !(mo->flags & (MF_NOCLIP | MF_NOGRAVITY)))
	{	// When the object is straddling sectors with the same
		// floor height that have different frictions, use the lowest
		// friction value (muddy has precedence over icy).

		for (m = mo->touching_sectorlist; m; m = m->m_tnext)
		{
			sec = m->m_sector;
			DVector3 pos = mo->PosRelative(sec);

			// 3D floors must be checked, too
			for (unsigned i = 0; i < sec->e->XFloor.ffloors.Size(); i++)
			{
				F3DFloor *rover = sec->e->XFloor.ffloors[i];
				if (!(rover->flags & FF_EXISTS)) continue;

				if (rover->flags & FF_SOLID)
				{
					// Must be standing on a solid floor
					if (mo->Z() != rover->top.plane->ZatPoint(pos)) continue;
				}
				else if (rover->flags & FF_SWIMMABLE)
				{
					// Or on or inside a swimmable floor (e.g. in shallow water)
					if (mo->Z() > rover->top.plane->ZatPoint(pos) ||
						(mo->Top()) < rover->bottom.plane->ZatPoint(pos))
						continue;
				}
				else
					continue;

				newfriction = rover->model->GetFriction(rover->top.isceiling, &newmf);
				if (newfriction < friction || friction == ORIG_FRICTION)
				{
					friction = newfriction;
					movefactor = newmf * 0.5;
				}
			}

			if (!(sec->Flags & SECF_FRICTION) &&
				Terrains[sec->GetTerrain(sector_t::floor)].Friction == 0)
			{
				continue;
			}
			newfriction = sec->GetFriction(sector_t::floor, &newmf);
			if ((newfriction < friction || friction == ORIG_FRICTION) &&
				(mo->Z() <= sec->floorplane.ZatPoint(pos) ||
				(sec->GetHeightSec() != NULL &&
				mo->Z() <= sec->heightsec->floorplane.ZatPoint(pos))))
			{
				friction = newfriction;
				movefactor = newmf;
			}
		}
	}

	if (mo->Friction != 1)
	{
		friction = clamp((friction * mo->Friction), 0., 1.);
		movefactor = FrictionToMoveFactor(friction);
	}

	if (frictionfactor)
		*frictionfactor = movefactor;

	return friction;
}

//==========================================================================
//
// phares 3/19/98
// P_GetMoveFactor() returns the value by which the x,y
// movements are multiplied to add to player movement.
//
// killough 8/28/98: rewritten
//
//==========================================================================

double P_GetMoveFactor(const AActor *mo, double *frictionp)
{
	double movefactor, friction;

	// If the floor is icy or muddy, it's harder to get moving. This is where
	// the different friction factors are applied to 'trying to move'. In
	// p_mobj.c, the friction factors are applied as you coast and slow down.

	if ((friction = P_GetFriction(mo, &movefactor)) < ORIG_FRICTION)
	{
		// phares 3/11/98: you start off slowly, then increase as
		// you get better footing

		double velocity = mo->VelXYToSpeed();

		if (velocity > MORE_FRICTION_VELOCITY * 4)
			movefactor *= 8;
		else if (velocity > MORE_FRICTION_VELOCITY * 2)
			movefactor *= 4;
		else if (velocity > MORE_FRICTION_VELOCITY)
			movefactor *= 2;
	}

	if (frictionp)
		*frictionp = friction;

	return movefactor;
}


//==========================================================================
//
// Checks if the line intersects with the actor
// returns 
// - 1 when above/below
// - 0 when intersecting
// - -1 when outside the portal
//
//==========================================================================

static int LineIsAbove(line_t *line, AActor *actor)
{
	AActor *point = line->frontsector->SkyBoxes[sector_t::floor];
	if (point == NULL) return -1;
	return point->specialf1 >= actor->Top();
}

static int LineIsBelow(line_t *line, AActor *actor)
{
	AActor *point = line->frontsector->SkyBoxes[sector_t::ceiling];
	if (point == NULL) return -1;
	return point->specialf1 <= actor->Z();
}

//
// MOVEMENT ITERATOR FUNCTIONS
//

//==========================================================================
//
//
// PIT_CheckLine
// Adjusts tmfloorz and tm_f_ceilingz() as lines are contacted
//
//
//==========================================================================

static // killough 3/26/98: make static
bool PIT_CheckLine(FMultiBlockLinesIterator &mit, FMultiBlockLinesIterator::CheckResult &cres, const FBoundingBox &box, FCheckPosition &tm)
{
	line_t *ld = cres.line;
	bool rail = false;

	if (!box.inRange(ld) || box.BoxOnLineSide(ld) != -1)
		return true;

	// A line has been hit
	/*
	=
	= The moving thing's destination position will cross the given line.
	= If this should not be allowed, return false.
	= If the line is special, keep track of it to process later if the move
	=       is proven ok.  NOTE: specials are NOT sorted by order, so two special lines
	=       that are only 8 pixels apart could be crossed in either order.
	*/

	if (!ld->backsector)
	{ // One sided line

		// Needed for polyobject portals.
		if (cres.line->isLinePortal())
		{
			spechit_t spec;
			spec.line = ld;
			spec.Refpos = cres.Position;
			spec.Oldrefpos = tm.thing->PosRelative(ld);
			portalhit.Push(spec);
			return true;
		}

		if (((cres.portalflags & FFCF_NOFLOOR) && LineIsAbove(cres.line, tm.thing) != 0) ||
			((cres.portalflags & FFCF_NOCEILING) && LineIsBelow(cres.line, tm.thing) != 0))
		{
			// this blocking line is in a different vertical layer and does not intersect with the actor that is being checked.
			// Since a one-sided line does not have an opening there's nothing left to do about it.
			return true;
		}
		if (tm.thing->flags2 & MF2_BLASTED)
		{
			P_DamageMobj(tm.thing, NULL, NULL, tm.thing->Mass >> 5, NAME_Melee);
		}
		tm.thing->BlockingLine = ld;
		CheckForPushSpecial(ld, 0, tm.thing);
		return false;
	}

	// MBF bouncers are treated as missiles here.
	bool Projectile = (tm.thing->flags & MF_MISSILE || tm.thing->BounceFlags & BOUNCE_MBF);
	// MBF considers that friendly monsters are not blocked by monster-blocking lines.
	// This is added here as a compatibility option. Note that monsters that are dehacked
	// into being friendly with the MBF flag automatically gain MF3_NOBLOCKMONST, so this
	// just optionally generalizes the behavior to other friendly monsters.
	bool NotBlocked = ((tm.thing->flags3 & MF3_NOBLOCKMONST)
		|| ((i_compatflags & COMPATF_NOBLOCKFRIENDS) && (tm.thing->flags & MF_FRIENDLY)));

	if (!(Projectile) || (ld->flags & (ML_BLOCKEVERYTHING | ML_BLOCKPROJECTILE)))
	{
		if (ld->flags & ML_RAILING)
		{
			rail = true;
		}
		else if ((ld->flags & (ML_BLOCKING | ML_BLOCKEVERYTHING)) || 				// explicitly blocking everything
			(!(NotBlocked) && (ld->flags & ML_BLOCKMONSTERS)) || 				// block monsters only
			(tm.thing->player != NULL && (ld->flags & ML_BLOCK_PLAYERS)) ||		// block players
			((Projectile) && (ld->flags & ML_BLOCKPROJECTILE)) ||				// block projectiles
			((tm.thing->flags & MF_FLOAT) && (ld->flags & ML_BLOCK_FLOATERS)))	// block floaters
		{
			if (cres.portalflags & FFCF_NOFLOOR)
			{
				int state = LineIsAbove(cres.line, tm.thing);
				if (state == -1) return true;
				if (state == 1)
				{
					// the line should not block but we should set the ceilingz to the portal boundary so that we can't float up into that line.
					double portalz = cres.line->frontsector->SkyBoxes[sector_t::floor]->specialf1;
					if (portalz < tm.ceilingz)
					{
						tm.ceilingz = portalz;
						tm.ceilingsector = cres.line->frontsector;
					}
					return true;
				}
			}
			else if (cres.portalflags & FFCF_NOCEILING)
			{
				// same, but for downward portals
				int state = LineIsBelow(cres.line, tm.thing);
				if (state == -1) return true;
				if (state == 1)
				{
					double portalz = cres.line->frontsector->SkyBoxes[sector_t::ceiling]->specialf1;
					if (portalz > tm.floorz)
					{
						tm.floorz = portalz;
						tm.floorsector = cres.line->frontsector;
						tm.floorterrain = 0;
					}
					return true;
				}
			}
			else
			{
				if (tm.thing->flags2 & MF2_BLASTED)
				{
					P_DamageMobj(tm.thing, NULL, NULL, tm.thing->Mass >> 5, NAME_Melee);
				}
				tm.thing->BlockingLine = ld;
				// Calculate line side based on the actor's original position, not the new one.
				CheckForPushSpecial(ld, P_PointOnLineSide(cres.Position, ld), tm.thing);
				return false;
			}
		}
	}
	DVector2 ref = FindRefPoint(ld, cres.Position);
	FLineOpening open;

	P_LineOpening(open, tm.thing, ld, ref, &cres.Position, cres.portalflags);

	// [RH] Steep sectors count as dropoffs, if the actor touches the boundary between a steep slope and something else
	if (!(tm.thing->flags & MF_DROPOFF) &&
		!(tm.thing->flags & (MF_NOGRAVITY | MF_NOCLIP)))
	{
		if ((open.frontfloorplane.c < STEEPSLOPE) != (open.backfloorplane.c < STEEPSLOPE))
		{
			// on the boundary of a steep slope
			return false;
		}
	}

	// If the floor planes on both sides match we should recalculate open.bottom at the actual position we are checking
	// This is to avoid bumpy movement when crossing a linedef with the same slope on both sides.
	if (open.frontfloorplane == open.backfloorplane && open.bottom > LINEOPEN_MIN)
	{
		open.bottom = open.frontfloorplane.ZatPoint(cres.Position);
	}

	if (rail &&
		// Eww! Gross! This check means the rail only exists if you stand on the
		// high side of the rail. So if you're walking on the low side of the rail,
		// it's possible to get stuck in the rail until you jump out. Unfortunately,
		// there is an area on Strife MAP04 that requires this behavior. Still, it's
		// better than Strife's handling of rails, which lets you jump into rails
		// from either side. How long until somebody reports this as a bug and I'm
		// forced to say, "It's not a bug. It's a feature?" Ugh.
		(!(level.flags2 & LEVEL2_RAILINGHACK) ||
		open.bottom == tm.thing->Sector->floorplane.ZatPoint(ref)))
	{
		open.bottom += 32;
	}

	// adjust floor / ceiling heights
	if (!(cres.portalflags & FFCF_NOCEILING))
	{
		if (open.top < tm.ceilingz)
		{
			tm.ceilingz = open.top;
			tm.ceilingsector = open.topsec;
			tm.ceilingpic = open.ceilingpic;
			tm.ceilingline = ld;
			tm.thing->BlockingLine = ld;
		}
	}

	if (!(cres.portalflags & FFCF_NOFLOOR))
	{
		if (open.bottom > tm.floorz)
		{
			tm.floorz = open.bottom;
			tm.floorsector = open.bottomsec;
			tm.floorpic = open.floorpic;
			tm.floorterrain = open.floorterrain;
			tm.touchmidtex = open.touchmidtex;
			tm.abovemidtex = open.abovemidtex;
			tm.thing->BlockingLine = ld;
		}
		else if (open.bottom == tm.floorz)
		{
			tm.touchmidtex |= open.touchmidtex;
			tm.abovemidtex |= open.abovemidtex;
		}

		if (open.lowfloor < tm.dropoffz)
		{
			tm.dropoffz = open.lowfloor;
		}
	}

	// if contacted a special line, add it to the list
	spechit_t spec;
	if (ld->special)
	{
		spec.line = ld;
		spec.Refpos = cres.Position;
		spec.Oldrefpos = tm.thing->PosRelative(ld);
		spechit.Push(spec);
	}
	if (ld->isLinePortal())
	{
		spec.line = ld;
		spec.Refpos = cres.Position;
		spec.Oldrefpos = tm.thing->PosRelative(ld);
		portalhit.Push(spec);
	}

	return true;
}

//==========================================================================
//
//
// PIT_CheckPortal
// This checks the destination side of a non-static line portal
// We cannot run a full P_CheckPosition there because it'd set 
// multiple fields to values that can cause problems in other
// parts of the code
// 
// What this does is starting a separate BlockLinesIterator
// and only taking the absolutely necessary information
// (i.e. floor and ceiling height plus terrain)
// 
//
//==========================================================================

static bool PIT_CheckPortal(FMultiBlockLinesIterator &mit, FMultiBlockLinesIterator::CheckResult cres, const FBoundingBox &box, FCheckPosition &tm)
{
	// if in another vertical section let's just ignore it.
	if (cres.portalflags & (FFCF_NOCEILING | FFCF_NOFLOOR)) return false;

	if (!box.inRange(cres.line) || box.BoxOnLineSide(cres.line) != -1)
		return false;

	line_t *lp = cres.line->getPortalDestination();
	double zofs = 0;

	P_TranslatePortalXY(cres.line, cres.Position.X, cres.Position.Y);
	P_TranslatePortalZ(cres.line, zofs);

	// fudge a bit with the portal line so that this gets included in the checks that normally only get run on two-sided lines
	sector_t *sec = lp->backsector;
	if (lp->backsector == NULL) lp->backsector = lp->frontsector;
	tm.thing->AddZ(zofs);

	FBoundingBox pbox(cres.Position.X, cres.Position.Y, tm.thing->radius);
	FBlockLinesIterator it(pbox);
	bool ret = false;
	line_t *ld;

	// Check all lines at the destination
	while ((ld = it.Next()))
	{
		if (!pbox.inRange(ld) || pbox.BoxOnLineSide(ld) != -1)
			continue;

		if (ld->backsector == NULL) 
			continue;

		DVector2 ref = FindRefPoint(ld, cres.Position);
		FLineOpening open;

		P_LineOpening(open, tm.thing, ld, ref, &cres.Position, 0);

		// adjust floor / ceiling heights
		if (open.top - zofs < tm.ceilingz)
		{
			tm.ceilingz = open.top - zofs;
			tm.ceilingpic = open.ceilingpic;
			/*
			tm.ceilingsector = open.topsec;
			tm.ceilingline = ld;
			tm.thing->BlockingLine = ld;
			*/
			ret = true;
		}

		if (open.bottom - zofs > tm.floorz)
		{
			tm.floorz = open.bottom - zofs;
			tm.floorpic = open.floorpic;
			tm.floorterrain = open.floorterrain;
			/*
			tm.floorsector = open.bottomsec;
			tm.touchmidtex = open.touchmidtex;
			tm.abovemidtex = open.abovemidtex;
			tm.thing->BlockingLine = ld;
			*/
			ret = true;
		}

		if (open.lowfloor - zofs < tm.dropoffz)
			tm.dropoffz = open.lowfloor - zofs;
	}
	tm.thing->AddZ(-zofs);
	lp->backsector = sec;

	return ret;
}


//==========================================================================
//
// Isolated to keep the code readable and fix the logic
//
//==========================================================================

static bool CheckRipLevel(AActor *victim, AActor *projectile)
{
	if (victim->RipLevelMin > 0 && projectile->RipperLevel < victim->RipLevelMin) return false;
	if (victim->RipLevelMax > 0 && projectile->RipperLevel > victim->RipLevelMax) return false;
	return true;
}


//==========================================================================
//
// Isolated to keep the code readable and allow reuse in other attacks
//
//==========================================================================

static bool CanAttackHurt(AActor *victim, AActor *shooter)
{
	// players are never subject to infighting settings and are always allowed
	// to harm / be harmed by anything.
	if (!victim->player && !shooter->player)
	{
		int infight = G_SkillProperty(SKILLP_Infight);

		if (infight < 0)
		{
			// -1: Monsters cannot hurt each other, but make exceptions for
			//     friendliness and hate status.
			if (shooter->flags & MF_SHOOTABLE)
			{
				// Question: Should monsters be allowed to shoot barrels in this mode?
				// The old code does not.
				if (victim->flags3 & MF3_ISMONSTER)
				{
					// Monsters that are clearly hostile can always hurt each other
					if (!victim->IsHostile(shooter))
					{
						// The same if the shooter hates the target
						if (victim->tid == 0 || shooter->TIDtoHate != victim->tid)
						{
							return false;
						}
					}
				}
			}
		}
		else if (infight == 0)
		{
			//  0: Monsters cannot hurt same species except 
			//     cases where they are clearly supposed to do that
			if (victim->IsFriend(shooter))
			{
				// Friends never harm each other, unless the shooter has the HARMFRIENDS set.
				if (!(shooter->flags7 & MF7_HARMFRIENDS)) return false;
			}
			else
			{
				if (victim->TIDtoHate != 0 && victim->TIDtoHate == shooter->TIDtoHate)
				{
					// [RH] Don't hurt monsters that hate the same victim as you do
					return false;
				}
				if (victim->GetSpecies() == shooter->GetSpecies() && !(victim->flags6 & MF6_DOHARMSPECIES))
				{
					// Don't hurt same species or any relative -
					// but only if the target isn't one's hostile.
					if (!victim->IsHostile(shooter))
					{
						// Allow hurting monsters the shooter hates.
						if (victim->tid == 0 || shooter->TIDtoHate != victim->tid)
						{
							return false;
						}
					}
				}
			}
		}
		// else if (infight==1) every shot hurts anything - no further tests needed
	}
	return true;
}

//==========================================================================
//
// PIT_CheckThing
//
//==========================================================================

bool PIT_CheckThing(FMultiBlockThingsIterator &it, FMultiBlockThingsIterator::CheckResult &cres, const FBoundingBox &box, FCheckPosition &tm)
{
	AActor *thing = cres.thing;
	double topz;
	bool 	solid;
	int 	damage;

	// don't clip against self
	if (thing == tm.thing)
		return true;

	if (!((thing->flags & (MF_SOLID | MF_SPECIAL | MF_SHOOTABLE)) || thing->flags6 & MF6_TOUCHY))
		return true;	// can't hit thing

	double blockdist = thing->radius + tm.thing->radius;
	if (fabs(thing->X() - cres.Position.X) >= blockdist || fabs(thing->Y() - cres.Position.Y) >= blockdist)
		return true;

	if ((thing->flags2 | tm.thing->flags2) & MF2_THRUACTORS)
		return true;

	if ((tm.thing->flags6 & MF6_THRUSPECIES) && (tm.thing->GetSpecies() == thing->GetSpecies()))
		return true;

	tm.thing->BlockingMobj = thing;
	topz = thing->Top();

	// Both things overlap in x or y direction
	bool unblocking = false;

	// walking on other actors and unblocking is too messy through restricted portal types so disable it.
	if (!(cres.portalflags & FFCF_RESTRICTEDPORTAL))
	{
		if (!(i_compatflags & COMPATF_NO_PASSMOBJ) && !(tm.thing->flags & (MF_FLOAT | MF_MISSILE | MF_SKULLFLY | MF_NOGRAVITY)) &&
			(thing->flags & MF_SOLID) && (thing->flags4 & MF4_ACTLIKEBRIDGE))
		{
			// [RH] Let monsters walk on actors as well as floors
			if ((tm.thing->flags3 & MF3_ISMONSTER) &&
				topz >= tm.floorz && topz <= tm.thing->Z() + tm.thing->MaxStepHeight)
			{
				// The commented-out if is an attempt to prevent monsters from walking off a
				// thing further than they would walk off a ledge. I can't think of an easy
				// way to do this, so I restrict them to only walking on bridges instead.
				// Uncommenting the if here makes it almost impossible for them to walk on
				// anything, bridge or otherwise.
				//			if (abs(thing->x - tmx) <= thing->radius &&
				//				abs(thing->y - tmy) <= thing->radius)
				{
					tm.stepthing = thing;
					tm.floorz = topz;
				}
			}
		}

		if (((tm.FromPMove || tm.thing->player != NULL) && thing->flags&MF_SOLID))
		{
			DVector3 oldpos = tm.thing->PosRelative(thing);
			// Both actors already overlap. To prevent them from remaining stuck allow the move if it
			// takes them further apart or the move does not change the position (when called from P_ChangeSector.)
			if (oldpos.X == thing->X() && oldpos.Y == thing->Y())
			{
				unblocking = true;
			}
			else if (fabs(thing->X() - oldpos.X) < (thing->radius + tm.thing->radius) &&
				fabs(thing->Y() - oldpos.Y) < (thing->radius + tm.thing->radius))

			{
				double newdist = thing->Distance2D(cres.Position.X, cres.Position.Y);
				double olddist = thing->Distance2D(oldpos.X, oldpos.Y);

				if (newdist > olddist)
				{
					// unblock only if there's already a vertical overlap (or both actors are flagged not to overlap)
					unblocking = (tm.thing->Top() > thing->Z() && tm.thing->Z() < topz) || (tm.thing->flags3 & thing->flags3 & MF3_DONTOVERLAP);
				}
			}
		}
	}

	// [RH] If the other thing is a bridge, then treat the moving thing as if it had MF2_PASSMOBJ, so
	// you can use a scrolling floor to move scenery items underneath a bridge.
	if ((tm.thing->flags2 & MF2_PASSMOBJ || thing->flags4 & MF4_ACTLIKEBRIDGE) && !(i_compatflags & COMPATF_NO_PASSMOBJ))
	{ // check if a mobj passed over/under another object
		if (!(tm.thing->flags & MF_MISSILE) ||
			!(tm.thing->flags2 & MF2_RIP) ||
			(thing->flags5 & MF5_DONTRIP) ||
			((tm.thing->flags6 & MF6_NOBOSSRIP) && (thing->flags2 & MF2_BOSS)))
		{
			if (tm.thing->flags3 & thing->flags3 & MF3_DONTOVERLAP)
			{ // Some things prefer not to overlap each other, if possible
				return unblocking;
			}
			if ((tm.thing->Z() >= topz) || (tm.thing->Top() <= thing->Z()))
				return true;
		}
	}

	if (tm.thing->player == NULL || !(tm.thing->player->cheats & CF_PREDICTING))
	{
		// touchy object is alive, toucher is solid
		if (thing->flags6 & MF6_TOUCHY && tm.thing->flags & MF_SOLID && thing->health > 0 &&
			// Thing is an armed mine or a sentient thing
			(thing->flags6 & MF6_ARMED || thing->IsSentient()) &&
			// either different classes or players
			(thing->player || thing->GetClass() != tm.thing->GetClass()) &&
			// or different species if DONTHARMSPECIES
			(!(thing->flags6 & MF6_DONTHARMSPECIES) || thing->GetSpecies() != tm.thing->GetSpecies()) &&
			// touches vertically
			topz >= tm.thing->Z() && tm.thing->Top() >= thing->Z() &&
			// prevents lost souls from exploding when fired by pain elementals
			(thing->master != tm.thing && tm.thing->master != thing))
			// Difference with MBF: MBF hardcodes the LS/PE check and lets actors of the same species
			// but different classes trigger the touchiness, but that seems less straightforwards.
		{
			thing->flags6 &= ~MF6_ARMED; // Disarm
			P_DamageMobj(thing, NULL, NULL, thing->health, NAME_None, DMG_FORCED);  // kill object
			return true;
		}

		// Check for MF6_BUMPSPECIAL
		// By default, only players can activate things by bumping into them
		if ((thing->flags6 & MF6_BUMPSPECIAL) && ((tm.thing->player != NULL)
			|| ((thing->activationtype & THINGSPEC_MonsterTrigger) && (tm.thing->flags3 & MF3_ISMONSTER))
			|| ((thing->activationtype & THINGSPEC_MissileTrigger) && (tm.thing->flags & MF_MISSILE))
			) && (level.maptime > thing->lastbump)) // Leave the bumper enough time to go away
		{
			if (P_ActivateThingSpecial(thing, tm.thing))
				thing->lastbump = level.maptime + TICRATE;
		}
	}

	// Check for skulls slamming into things
	if (tm.thing->flags & MF_SKULLFLY)
	{
		bool res = tm.thing->Slam(tm.thing->BlockingMobj);
		tm.thing->BlockingMobj = NULL;
		return res;
	}

	// [ED850] Player Prediction ends here. There is nothing else they could/should do.
	if (tm.thing->player != NULL && (tm.thing->player->cheats & CF_PREDICTING))
	{
		solid = (thing->flags & MF_SOLID) &&
			!(thing->flags & MF_NOCLIP) &&
			((tm.thing->flags & MF_SOLID) || (tm.thing->flags6 & MF6_BLOCKEDBYSOLIDACTORS));

		return !solid || unblocking;
	}

	// Check for blasted thing running into another
	if ((tm.thing->flags2 & MF2_BLASTED) && (thing->flags & MF_SHOOTABLE))
	{
		if (!(thing->flags2 & MF2_BOSS) && (thing->flags3 & MF3_ISMONSTER) && !(thing->flags3 & MF3_DONTBLAST))
		{
			// ideally this should take the mass factor into account
			thing->Vel += tm.thing->Vel.XY();
			if ((thing->Vel.X + thing->Vel.Y) > 3.)
			{
				int newdam;
				damage = (tm.thing->Mass / 100) + 1;
				newdam = P_DamageMobj(thing, tm.thing, tm.thing, damage, tm.thing->DamageType);
				P_TraceBleed(newdam > 0 ? newdam : damage, thing, tm.thing);
				damage = (thing->Mass / 100) + 1;
				newdam = P_DamageMobj(tm.thing, thing, thing, damage >> 2, tm.thing->DamageType);
				P_TraceBleed(newdam > 0 ? newdam : damage, tm.thing, thing);
			}
			return false;
		}
	}
	// Check for missile or non-solid MBF bouncer
	if (tm.thing->flags & MF_MISSILE || ((tm.thing->BounceFlags & BOUNCE_MBF) && !(tm.thing->flags & MF_SOLID)))
	{
		// Check for a non-shootable mobj
		if (thing->flags2 & MF2_NONSHOOTABLE)
		{
			return true;
		}
		// Check for passing through a ghost
		if ((thing->flags3 & MF3_GHOST) && (tm.thing->flags2 & MF2_THRUGHOST))
		{
			return true;
		}

		if ((tm.thing->flags6 & MF6_MTHRUSPECIES)
			&& tm.thing->target // NULL pointer check
			&& (tm.thing->target->GetSpecies() == thing->GetSpecies()))
			return true;

		// Check for rippers passing through corpses
		if ((thing->flags & MF_CORPSE) && (tm.thing->flags2 & MF2_RIP) && !(thing->flags & MF_SHOOTABLE))
		{
			return true;
		}

		double clipheight;

		if (thing->projectilepassheight > 0)
		{
			clipheight = thing->projectilepassheight;
		}
		else if (thing->projectilepassheight < 0 && (i_compatflags & COMPATF_MISSILECLIP))
		{
			clipheight = -thing->projectilepassheight;
		}
		else
		{
			clipheight = thing->Height;
		}

		// Check if it went over / under
		if (tm.thing->Z() > thing->Z() + clipheight)
		{ // Over thing
			return true;
		}
		if (tm.thing->Top() < thing->Z())
		{ // Under thing
			return true;
		}

		// [RH] What is the point of this check, again? In Hexen, it is unconditional,
		// but here we only do it if the missile's damage is 0.
		// MBF bouncer might have a non-0 damage value, but they must not deal damage on impact either.
		if ((tm.thing->BounceFlags & BOUNCE_Actors) && (tm.thing->Damage == 0 || !(tm.thing->flags & MF_MISSILE)))
		{
			return (tm.thing->target == thing || !(thing->flags & MF_SOLID));
		}

		switch (tm.thing->SpecialMissileHit(thing))
		{
		case 0:		return false;
		case 1:		return true;
		default:	break;
		}

		// [RH] Extend DeHacked infighting to allow for monsters
		// to never fight each other

		if (tm.thing->target != NULL)
		{
			if (thing == tm.thing->target)
			{ // Don't missile self
				return true;
			}

			if (!CanAttackHurt(thing, tm.thing->target))
			{
				return false;
			}
		}
		if (!(thing->flags & MF_SHOOTABLE))
		{ // Didn't do any damage
			return !(thing->flags & MF_SOLID);
		}
		if ((thing->flags4 & MF4_SPECTRAL) && !(tm.thing->flags4 & MF4_SPECTRAL))
		{
			return true;
		}

		if ((tm.DoRipping && !(thing->flags5 & MF5_DONTRIP)) && CheckRipLevel(thing, tm.thing))
		{
			if (!(tm.thing->flags6 & MF6_NOBOSSRIP) || !(thing->flags2 & MF2_BOSS))
			{
				bool *check = tm.LastRipped.CheckKey(thing);
				if (check == NULL || !*check)
				{
					tm.LastRipped[thing] = true;
					if (!(thing->flags & MF_NOBLOOD) &&
						!(thing->flags2 & MF2_REFLECTIVE) &&
						!(tm.thing->flags3 & MF3_BLOODLESSIMPACT) &&
						!(thing->flags2 & (MF2_INVULNERABLE | MF2_DORMANT)))
					{ // Ok to spawn blood
						P_RipperBlood(tm.thing, thing);
					}
					S_Sound(tm.thing, CHAN_BODY, "misc/ripslop", 1, ATTN_IDLE);

					// Do poisoning (if using new style poison)
					if (tm.thing->PoisonDamage > 0 && tm.thing->PoisonDuration != INT_MIN)
					{
						P_PoisonMobj(thing, tm.thing, tm.thing->target, tm.thing->PoisonDamage, tm.thing->PoisonDuration, tm.thing->PoisonPeriod, tm.thing->PoisonDamageType);
					}

					damage = tm.thing->GetMissileDamage(3, 2);
					int newdam = P_DamageMobj(thing, tm.thing, tm.thing->target, damage, tm.thing->DamageType);
					if (!(tm.thing->flags3 & MF3_BLOODLESSIMPACT))
					{
						P_TraceBleed(newdam > 0 ? newdam : damage, thing, tm.thing);
					}
					if (thing->flags2 & MF2_PUSHABLE
						&& !(tm.thing->flags2 & MF2_CANNOTPUSH))
					{ // Push thing
						if (thing->lastpush != tm.PushTime)
						{
							thing->Vel += tm.thing->Vel.XY() * thing->pushfactor;
							thing->lastpush = tm.PushTime;
						}
					}
				}
				spechit.Clear();
				return true;
			}
		}

		// Do poisoning (if using new style poison)
		if (tm.thing->PoisonDamage > 0 && tm.thing->PoisonDuration != INT_MIN)
		{
			P_PoisonMobj(thing, tm.thing, tm.thing->target, tm.thing->PoisonDamage, tm.thing->PoisonDuration, tm.thing->PoisonPeriod, tm.thing->PoisonDamageType);
		}

		// Do damage
		damage = tm.thing->GetMissileDamage((tm.thing->flags4 & MF4_STRIFEDAMAGE) ? 3 : 7, 1);
		if ((damage > 0) || (tm.thing->flags6 & MF6_FORCEPAIN) || (tm.thing->flags7 & MF7_CAUSEPAIN))
		{
			int newdam = P_DamageMobj(thing, tm.thing, tm.thing->target, damage, tm.thing->DamageType);
			if (damage > 0)
			{
				if ((tm.thing->flags5 & MF5_BLOODSPLATTER) &&
					!(thing->flags & MF_NOBLOOD) &&
					!(thing->flags2 & MF2_REFLECTIVE) &&
					!(thing->flags2 & (MF2_INVULNERABLE | MF2_DORMANT)) &&
					!(tm.thing->flags3 & MF3_BLOODLESSIMPACT) &&
					(pr_checkthing() < 192))
				{
					P_BloodSplatter(tm.thing->Pos(), thing, tm.thing->AngleTo(thing));
				}
				if (!(tm.thing->flags3 & MF3_BLOODLESSIMPACT))
				{
					P_TraceBleed(newdam > 0 ? newdam : damage, thing, tm.thing);
				}
			}
		}
		else
		{
			P_GiveBody(thing, -damage);
		}

		if ((thing->flags7 & MF7_THRUREFLECT) && (thing->flags2 & MF2_REFLECTIVE) && (tm.thing->flags & MF_MISSILE))
		{
			if (tm.thing->flags2 & MF2_SEEKERMISSILE)
			{
				tm.thing->tracer = tm.thing->target;
			}
			tm.thing->target = thing;
			return true;
		}
		return false;		// don't traverse any more
	}
	if (thing->flags2 & MF2_PUSHABLE && !(tm.thing->flags2 & MF2_CANNOTPUSH))
	{ // Push thing
		if (thing->lastpush != tm.PushTime)
		{
			thing->Vel += tm.thing->Vel.XY() * thing->pushfactor;
			thing->lastpush = tm.PushTime;
		}
	}
	solid = (thing->flags & MF_SOLID) &&
		!(thing->flags & MF_NOCLIP) &&
		((tm.thing->flags & MF_SOLID) || (tm.thing->flags6 & MF6_BLOCKEDBYSOLIDACTORS));

	// Check for special pickup
	if ((thing->flags & MF_SPECIAL) && (tm.thing->flags & MF_PICKUP)
		// [RH] The next condition is to compensate for the extra height
		// that gets added by P_CheckPosition() so that you cannot pick
		// up things that are above your true height.
		&& thing->Z() < tm.thing->Top() - tm.thing->MaxStepHeight)
	{ // Can be picked up by tmthing
		P_TouchSpecialThing(thing, tm.thing);	// can remove thing
	}

	// killough 3/16/98: Allow non-solid moving objects to move through solid
	// ones, by allowing the moving thing (tmthing) to move if it's non-solid,
	// despite another solid thing being in the way.
	// killough 4/11/98: Treat no-clipping things as not blocking

	return !solid || unblocking;

	// return !(thing->flags & MF_SOLID);	// old code -- killough
}



/*
===============================================================================

MOVEMENT CLIPPING

===============================================================================
*/

//==========================================================================
//
// P_CheckPosition
// This is purely informative, nothing is modified
// (except things picked up and missile damage applied).
// 
// in:
//	a AActor (can be valid or invalid)
//	a position to be checked
//	 (doesn't need to be related to the AActor->x,y)
//
// during:
//	special things are touched if MF_PICKUP
//	early out on solid lines?
//
// out:
//	newsubsec
//	floorz
//	ceilingz
//	tmdropoffz = the lowest point contacted (monsters won't move to a dropoff)
//	speciallines[]
//	numspeciallines
//  AActor *BlockingMobj = pointer to thing that blocked position (NULL if not
//   blocked, or blocked by a line).
//
//==========================================================================

bool P_CheckPosition(AActor *thing, const DVector2 &pos, FCheckPosition &tm, bool actorsonly)
{
	sector_t *newsec;
	AActor *thingblocker;
	double realHeight = thing->Height;

	tm.thing = thing;

	tm.pos.X = pos.X;
	tm.pos.Y = pos.Y;
	tm.pos.Z = thing->Z();

	newsec = tm.sector = P_PointInSector(pos);
	tm.ceilingline = thing->BlockingLine = NULL;

	// Retrieve the base floor / ceiling from the target location.
	// Any contacted lines the step closer together will adjust them.
	if (!thing->IsNoClip2())
	{
		P_GetFloorCeilingZ(tm, FFCF_SAMESECTOR);
	}
	else
	{
		// With noclip2, we must ignore 3D floors and go right to the uppermost ceiling and lowermost floor.
		tm.floorz = tm.dropoffz = newsec->LowestFloorAt(pos, &tm.floorsector);
		tm.ceilingz = newsec->HighestCeilingAt(pos, &tm.ceilingsector);
		tm.floorpic = tm.floorsector->GetTexture(sector_t::floor);
		tm.floorterrain = tm.floorsector->GetTerrain(sector_t::floor);
		tm.ceilingpic = tm.ceilingsector->GetTexture(sector_t::ceiling);
	}

	tm.touchmidtex = false;
	tm.abovemidtex = false;
	validcount++;

	if ((thing->flags & MF_NOCLIP) && !(thing->flags & MF_SKULLFLY))
		return true;

	// Check things first, possibly picking things up.
	thing->BlockingMobj = NULL;
	thingblocker = NULL;
	if (thing->player)
	{ // [RH] Fake taller height to catch stepping up into things.
		thing->Height = realHeight + thing->MaxStepHeight;
	}

	tm.stepthing = NULL;
	FBoundingBox box(pos.X, pos.Y, thing->radius);

	FPortalGroupArray pcheck;
	FMultiBlockThingsIterator it2(pcheck, pos.X, pos.Y, thing->Z(), thing->Height, thing->radius, false, newsec);
	FMultiBlockThingsIterator::CheckResult tcres;

	while ((it2.Next(&tcres)))
	{
		if (!PIT_CheckThing(it2, tcres, it2.Box(), tm))
		{ // [RH] If a thing can be stepped up on, we need to continue checking
			// other things in the blocks and see if we hit something that is
			// definitely blocking. Otherwise, we need to check the lines, or we
			// could end up stuck inside a wall.
			AActor *BlockingMobj = thing->BlockingMobj;

			// If this blocks through a restricted line portal, it will always completely block.
			if (BlockingMobj == NULL || (i_compatflags & COMPATF_NO_PASSMOBJ) || (tcres.portalflags & FFCF_RESTRICTEDPORTAL))
			{ // Thing slammed into something; don't let it move now.
				thing->Height = realHeight;
				return false;
			}
			else if (!BlockingMobj->player && !(thing->flags & (MF_FLOAT | MF_MISSILE | MF_SKULLFLY)) &&
				BlockingMobj->Top() - thing->Z() <= thing->MaxStepHeight)
			{
				if (thingblocker == NULL ||
					BlockingMobj->Z() > thingblocker->Z())
				{
					thingblocker = BlockingMobj;
				}
				thing->BlockingMobj = NULL;
			}
			else if (thing->player &&
				thing->Top() - BlockingMobj->Z() <= thing->MaxStepHeight)
			{
				if (thingblocker)
				{ // There is something to step up on. Return this thing as
					// the blocker so that we don't step up.
					thing->Height = realHeight;
					return false;
				}
				// Nothing is blocking us, but this actor potentially could
				// if there is something else to step on.
				thing->BlockingMobj = NULL;
			}
			else
			{ // Definitely blocking
				thing->Height = realHeight;
				return false;
			}
		}
	}

	// check lines

	// [RH] We need to increment validcount again, because a function above may
	// have already set some lines to equal the current validcount.
	//
	// Specifically, when DehackedPickup spawns a new item in its TryPickup()
	// function, that new actor will set the lines around it to match validcount
	// when it links itself into the world. If we just leave validcount alone,
	// that will give the player the freedom to walk through walls at will near
	// a pickup they cannot get, because their validcount will prevent them from
	// being considered for collision with the player.
	validcount++;

	thing->BlockingMobj = NULL;
	thing->Height = realHeight;
	if (actorsonly || (thing->flags & MF_NOCLIP))
		return (thing->BlockingMobj = thingblocker) == NULL;

	spechit.Clear();
	portalhit.Clear();

	FMultiBlockLinesIterator it(pcheck, pos.X, pos.Y, thing->Z(), thing->Height, thing->radius, newsec);
	FMultiBlockLinesIterator::CheckResult lcres;

	double thingdropoffz = tm.floorz;
	//bool onthing = (thingdropoffz != tmdropoffz);
	tm.floorz = tm.dropoffz;

	bool good = true;

	while (it.Next(&lcres))
	{
		bool thisresult = PIT_CheckLine(it, lcres, it.Box(), tm);
		good &= thisresult;
		if (thisresult)
		{
			FLinePortal *port = lcres.line->getPortal();
			if (port != NULL && port->mFlags & PORTF_PASSABLE && port->mType != PORTT_LINKED)
			{
				// Checking the other side of the portal completely is too costly,
				// but checking the portal's destination line is necessary to 
				// retrieve the proper sector heights on the other side.
				if (PIT_CheckPortal(it, lcres, it.Box(), tm))
				{
					tm.thing->BlockingLine = lcres.line;
				}
			}
		}
	}
	if (!good)
	{
		return false;
	}
	if (tm.ceilingz - tm.floorz < thing->Height)
	{
		return false;
	}
	if (tm.touchmidtex)
	{
		tm.dropoffz = tm.floorz;
	}
	else if (tm.stepthing != NULL)
	{
		tm.dropoffz = thingdropoffz;
	}

	return (thing->BlockingMobj = thingblocker) == NULL;
}

bool P_CheckPosition(AActor *thing, const DVector2 &pos, bool actorsonly)
{
	FCheckPosition tm;
	return P_CheckPosition(thing, pos, tm, actorsonly);
}

//----------------------------------------------------------------------------
//
// FUNC P_TestMobjLocation
//
// Returns true if the mobj is not blocked by anything at its current
// location, otherwise returns false.
//
//----------------------------------------------------------------------------

bool P_TestMobjLocation(AActor *mobj)
{
	ActorFlags flags;

	flags = mobj->flags;
	mobj->flags &= ~MF_PICKUP;
	if (P_CheckPosition(mobj, mobj->Pos()))
	{ // XY is ok, now check Z
		mobj->flags = flags;
		if ((mobj->Z() < mobj->floorz) || (mobj->Top() > mobj->ceilingz))
		{ // Bad Z
			return false;
		}
		return true;
	}
	mobj->flags = flags;
	return false;
}


//=============================================================================
//
// P_CheckOnmobj(AActor *thing)
//
//				Checks if the new Z position is legal
//=============================================================================

AActor *P_CheckOnmobj(AActor *thing)
{
	double oldz;
	bool good;
	AActor *onmobj;

	oldz = thing->Z();
	P_FakeZMovement(thing);
	good = P_TestMobjZ(thing, false, &onmobj);
	thing->SetZ(oldz);

	return good ? NULL : onmobj;
}

//=============================================================================
//
// P_TestMobjZ
//
//=============================================================================

bool P_TestMobjZ(AActor *actor, bool quick, AActor **pOnmobj)
{
	AActor *onmobj = NULL;
	if (actor->flags & MF_NOCLIP)
	{
		if (pOnmobj) *pOnmobj = NULL;
		return true;
	}

	FPortalGroupArray check;
	FMultiBlockThingsIterator it(check, actor, -1, true);
	FMultiBlockThingsIterator::CheckResult cres;

	while (it.Next(&cres))
	{
		AActor *thing = cres.thing;

		double blockdist = thing->radius + actor->radius;
		if (fabs(thing->X() - cres.Position.X) >= blockdist || fabs(thing->Y() - cres.Position.Y) >= blockdist)
		{
			continue;
		}
		if ((actor->flags2 | thing->flags2) & MF2_THRUACTORS)
		{
			continue;
		}
		if ((actor->flags6 & MF6_THRUSPECIES) && (thing->GetSpecies() == actor->GetSpecies()))
		{
			continue;
		}
		if (!(thing->flags & MF_SOLID))
		{ // Can't hit thing
			continue;
		}
		if (thing->flags & (MF_SPECIAL | MF_NOCLIP))
		{ // [RH] Specials and noclippers don't block moves
			continue;
		}
		if (thing->flags & (MF_CORPSE))
		{ // Corpses need a few more checks
			if (!(actor->flags & MF_ICECORPSE))
				continue;
		}
		if (!(thing->flags4 & MF4_ACTLIKEBRIDGE) && (actor->flags & MF_SPECIAL))
		{ // [RH] Only bridges block pickup items
			continue;
		}
		if (thing == actor)
		{ // Don't clip against self
			continue;
		}
		if ((actor->flags & MF_MISSILE) && (thing == actor->target))
		{ // Don't clip against whoever shot the missile.
			continue;
		}
		if (actor->Z() > thing->Top())
		{ // over thing
			continue;
		}
		else if (actor->Top() <= thing->Z())
		{ // under thing
			continue;
		}
		else if (!quick && onmobj != NULL && thing->Top() < onmobj->Top())
		{ // something higher is in the way
			continue;
		}
		onmobj = thing;
		if (quick) break;
	}

	if (pOnmobj) *pOnmobj = onmobj;
	return onmobj == NULL;
}

//=============================================================================
//
// P_FakeZMovement
//
//				Fake the zmovement so that we can check if a move is legal
//=============================================================================

void P_FakeZMovement(AActor *mo)
{
	//
	// adjust height
	//
	mo->AddZ(mo->Vel.Z);
	if ((mo->flags&MF_FLOAT) && mo->target)
	{ // float down towards target if too close
		if (!(mo->flags & MF_SKULLFLY) && !(mo->flags & MF_INFLOAT))
		{
			double dist = mo->Distance2D(mo->target);
			double delta = mo->target->Center() - mo->Z();
			if (delta < 0 && dist < -(delta * 3))
				mo->AddZ(-mo->FloatSpeed);
			else if (delta > 0 && dist < (delta * 3))
				mo->AddZ(mo->FloatSpeed);
		}
	}
	if (mo->player && mo->flags&MF_NOGRAVITY && (mo->Z() > mo->floorz) && !mo->IsNoClip2())
	{
		mo->AddZ(DAngle(4.5 * level.maptime).Sin());
	}

	//
	// clip movement
	//
	if (mo->Z() <= mo->floorz)
	{ // hit the floor
		mo->SetZ(mo->floorz);
	}

	if (mo->Top() > mo->ceilingz)
	{ // hit the ceiling
		mo->SetZ(mo->ceilingz - mo->Height);
	}
}

//===========================================================================
//
// CheckForPushSpecial
//
//===========================================================================

static void CheckForPushSpecial(line_t *line, int side, AActor *mobj, DVector2 *posforwindowcheck)
{
	if (line->special && !(mobj->flags6 & MF6_NOTRIGGER))
	{
		if (posforwindowcheck && !(ib_compatflags & BCOMPATF_NOWINDOWCHECK) && line->backsector != NULL)
		{ // Make sure this line actually blocks us and is not a window
			// or similar construct we are standing inside of.
			DVector3 pos = mobj->PosRelative(line);
			double fzt = line->frontsector->ceilingplane.ZatPoint(*posforwindowcheck);
			double fzb = line->frontsector->floorplane.ZatPoint(*posforwindowcheck);
			double bzt = line->backsector->ceilingplane.ZatPoint(*posforwindowcheck);
			double bzb = line->backsector->floorplane.ZatPoint(*posforwindowcheck);
			if (fzt >= mobj->Top() && bzt >= mobj->Top() &&
				fzb <= mobj->Z() && bzb <= mobj->Z())
			{
				// we must also check if some 3D floor in the backsector may be blocking
				for (auto rover : line->backsector->e->XFloor.ffloors)
				{
					if (!(rover->flags & FF_SOLID) || !(rover->flags & FF_EXISTS)) continue;

					double ff_bottom = rover->bottom.plane->ZatPoint(*posforwindowcheck);
					double ff_top = rover->top.plane->ZatPoint(*posforwindowcheck);

					if (ff_bottom < mobj->Top() && ff_top > mobj->Z())
					{
						goto isblocking;
					}
				}
				return;
			}
		}
	isblocking:
		if (mobj->flags2 & MF2_PUSHWALL)
		{
			P_ActivateLine(line, mobj, side, SPAC_Push);
		}
		else if (mobj->flags2 & MF2_IMPACT)
		{
			if ((level.flags2 & LEVEL2_MISSILESACTIVATEIMPACT) ||
				!(mobj->flags & MF_MISSILE) ||
				(mobj->target == NULL))
			{
				P_ActivateLine(line, mobj, side, SPAC_Impact);
			}
			else
			{
				P_ActivateLine(line, mobj->target, side, SPAC_Impact);
			}
		}
	}
}

//==========================================================================
//
// P_TryMove
// Attempt to move to a new position,
// crossing special lines unless MF_TELEPORT is set.
//
//==========================================================================

bool P_TryMove(AActor *thing, const DVector2 &pos,
	int dropoff, // killough 3/15/98: allow dropoff as option
	const secplane_t *onfloor, // [RH] Let P_TryMove keep the thing on the floor
	FCheckPosition &tm,
	bool missileCheck)	// [GZ] Fired missiles ignore the drop-off test
{
	sector_t	*oldsector;
	double		oldz;
	int 		side;
	int 		oldside;
	sector_t*	oldsec = thing->Sector;	// [RH] for sector actions
	sector_t*	newsec;

	tm.floatok = false;
	oldz = thing->Z();
	if (onfloor)
	{
		thing->SetZ(onfloor->ZatPoint(pos));
	}
	thing->flags6 |= MF6_INTRYMOVE;
	if (!P_CheckPosition(thing, pos, tm))
	{
		AActor *BlockingMobj = thing->BlockingMobj;
		// Solid wall or thing
		if (!BlockingMobj || BlockingMobj->player || !thing->player)
		{
			goto pushline;
		}
		else
		{
			if (BlockingMobj->player || !thing->player)
			{
				goto pushline;
			}
			else if (BlockingMobj->Top() - thing->Z() > thing->MaxStepHeight
				|| (BlockingMobj->Sector->ceilingplane.ZatPoint(pos) - (BlockingMobj->Top()) < thing->Height)
				|| (tm.ceilingz - (BlockingMobj->Top()) < thing->Height))
			{
				goto pushline;
			}
		}
		if (!(tm.thing->flags2 & MF2_PASSMOBJ) || (i_compatflags & COMPATF_NO_PASSMOBJ))
		{
			thing->SetZ(oldz);
			thing->flags6 &= ~MF6_INTRYMOVE;
			return false;
		}
	}

	if (thing->flags3 & MF3_FLOORHUGGER)
	{
		thing->SetZ(tm.floorz);
	}
	else if (thing->flags3 & MF3_CEILINGHUGGER)
	{
		thing->SetZ(tm.ceilingz - thing->Height);
	}

	if (onfloor && tm.floorsector == thing->floorsector)
	{
		thing->SetZ(tm.floorz);
	}
	if (!(thing->flags & MF_NOCLIP))
	{
		if (tm.ceilingz - tm.floorz < thing->Height)
		{
			goto pushline;		// doesn't fit
		}

		tm.floatok = true;

		if (!(thing->flags & MF_TELEPORT)
			&& tm.ceilingz < thing->Top()
			&& !(thing->flags3 & MF3_CEILINGHUGGER)
			&& (!(thing->flags2 & MF2_FLY) || !(thing->flags & MF_NOGRAVITY)))
		{
			goto pushline;		// mobj must lower itself to fit
		}
		if (thing->flags2 & MF2_FLY && thing->flags & MF_NOGRAVITY)
		{
#if 1
			if (thing->Top() > tm.ceilingz)
				goto pushline;
#else
			// When flying, slide up or down blocking lines until the actor
			// is not blocked.
			if (thing->Top() > tm.ceilingz)
			{
				thing->Vel.Z = -8;
				goto pushline;
			}
			else if (thing->Z() < tm.floorz && tm.floorz - tm.dropoffz > thing->MaxDropOffHeight)
			{
				thing->Vel.Z = 8;
				goto pushline;
			}
#endif
		}
		if (!(thing->flags & MF_TELEPORT) && !(thing->flags3 & MF3_FLOORHUGGER))
		{
			if ((thing->flags & MF_MISSILE) && !(thing->flags6 & MF6_STEPMISSILE) && tm.floorz > thing->Z())
			{ // [RH] Don't let normal missiles climb steps
				goto pushline;
			}
			if (tm.floorz - thing->Z() > thing->MaxStepHeight)
			{ // too big a step up
				goto pushline;
			}
			else if (thing->Z() < tm.floorz)
			{ // [RH] Check to make sure there's nothing in the way for the step up
				double savedz = thing->Z();
				bool good;
				thing->SetZ(tm.floorz);
				good = P_TestMobjZ(thing);
				thing->SetZ(savedz);
				if (!good)
				{
					goto pushline;
				}
				if (thing->flags6 & MF6_STEPMISSILE)
				{
					thing->SetZ(tm.floorz);
					// If moving down, cancel vertical component of the velocity
					if (thing->Vel.Z < 0)
					{
						// If it's a bouncer, let it bounce off its new floor, too.
						if (thing->BounceFlags & BOUNCE_Floors)
						{
							thing->FloorBounceMissile(tm.floorsector->floorplane);
						}
						else
						{
							thing->Vel.Z = 0;
						}
					}
				}
			}
		}

		// compatibility check: Doom originally did not allow monsters to cross dropoffs at all.
		// If the compatibility flag is on, only allow this when the velocity comes from a scroller
		if ((i_compatflags & COMPATF_CROSSDROPOFF) && !(thing->flags4 & MF4_SCROLLMOVE))
		{
			dropoff = false;
		}

		if (dropoff == 2 &&  // large jump down (e.g. dogs)
			(tm.floorz - tm.dropoffz > 128. || thing->target == NULL || thing->target->Z() >tm.dropoffz))
		{
			dropoff = false;
		}


		// killough 3/15/98: Allow certain objects to drop off
		if ((!dropoff && !(thing->flags & (MF_DROPOFF | MF_FLOAT | MF_MISSILE))) || (thing->flags5&MF5_NODROPOFF))
		{
			if (!(thing->flags5&MF5_AVOIDINGDROPOFF))
			{
				double floorz = tm.floorz;
				// [RH] If the thing is standing on something, use its current z as the floorz.
				// This is so that it does not walk off of things onto a drop off.
				if (thing->flags2 & MF2_ONMOBJ)
				{
					floorz = MAX(thing->Z(), tm.floorz);
				}

				if (floorz - tm.dropoffz > thing->MaxDropOffHeight &&
					!(thing->flags2 & MF2_BLASTED) && !missileCheck)
				{ // Can't move over a dropoff unless it's been blasted
					// [GZ] Or missile-spawned
					thing->SetZ(oldz);
					thing->flags6 &= ~MF6_INTRYMOVE;
					return false;
				}
			}
			else
			{
				// special logic to move a monster off a dropoff
				// this intentionally does not check for standing on things.
				if (thing->floorz - tm.floorz > thing->MaxDropOffHeight ||
					thing->dropoffz - tm.dropoffz > thing->MaxDropOffHeight)
				{
					thing->flags6 &= ~MF6_INTRYMOVE;
					return false;
				}
			}
		}
		if (thing->flags2 & MF2_CANTLEAVEFLOORPIC
			&& (tm.floorpic != thing->floorpic
			|| tm.floorz - thing->Z() != 0))
		{ // must stay within a sector of a certain floor type
			thing->SetZ(oldz);
			thing->flags6 &= ~MF6_INTRYMOVE;
			return false;
		}

		//Added by MC: To prevent bot from getting into dangerous sectors.
		if (thing->player && thing->player->Bot != NULL && thing->flags & MF_SHOOTABLE)
		{
			if (tm.sector != thing->Sector
				&& bglobal.IsDangerous(tm.sector))
			{
				thing->player->Bot->prev = thing->player->Bot->dest;
				thing->player->Bot->dest = NULL;
				thing->Vel.X = thing->Vel.Y = 0;
				thing->SetZ(oldz);
				thing->flags6 &= ~MF6_INTRYMOVE;
				return false;
			}
		}
	}

	// [RH] Check status of eyes against fake floor/ceiling in case
	// it slopes or the player's eyes are bobbing in and out.

	bool oldAboveFakeFloor, oldAboveFakeCeiling;
	double _viewheight = thing->player ? thing->player->viewheight : thing->Height / 2;
	oldAboveFakeFloor = oldAboveFakeCeiling = false;	// pacify GCC

	if (oldsec->heightsec)
	{
		double eyez = oldz + FIXED2DBL(viewheight);

		oldAboveFakeFloor = eyez > oldsec->heightsec->floorplane.ZatPoint(thing);
		oldAboveFakeCeiling = eyez > oldsec->heightsec->ceilingplane.ZatPoint(thing);
	}

	// Borrowed from MBF: 
	if (thing->BounceFlags & BOUNCE_MBF &&  // killough 8/13/98
		!(thing->flags & (MF_MISSILE | MF_NOGRAVITY)) &&
		!thing->IsSentient() && tm.floorz - thing->Z() > 16)
	{ // too big a step up for MBF bouncers under gravity
		thing->flags6 &= ~MF6_INTRYMOVE;
		return false;
	}


	// Check for crossed portals
	bool portalcrossed;
	portalcrossed = false;

	while (true)
	{
		double bestfrac = 1.1;
		spechit_t besthit;
		int besthitnum;
		// find the portal nearest to the crossing actor
		for (unsigned i = 0; i < portalhit.Size();i++)
		{
			auto &spec = portalhit[i];

			line_t *ld = spec.line;
			if (ld->frontsector->PortalGroup != thing->Sector->PortalGroup) continue;	// must be in the same group to be considered valid.

			// see if the line was crossed
			oldside = P_PointOnLineSide(spec.Oldrefpos, ld);
			side = P_PointOnLineSide(spec.Refpos, ld);
			if (oldside == 0 && side == 1)
			{
				divline_t dl2 = { ld->v1->fX(), ld->v1->fY(), ld->Delta().X, ld->Delta().Y };
				divline_t dl1 = { spec.Oldrefpos.X, spec.Oldrefpos.Y, spec.Refpos.X - spec.Oldrefpos.X, spec.Refpos.Y - spec.Oldrefpos.Y };
				double frac = P_InterceptVector(&dl1, &dl2);
				if (frac < bestfrac)
				{
					besthit = spec;
					bestfrac = frac;
					besthitnum = i;
				}
			}
		}

		if (bestfrac < 1.1)
		{
			portalhit.Delete(besthitnum);
			line_t *ld = besthit.line;
			FLinePortal *port = ld->getPortal();
			if (port->mType == PORTT_LINKED)
			{
				thing->UnlinkFromWorld();
				thing->SetXY(tm.pos + port->mDisplacement);
				thing->Prev += port->mDisplacement;
				thing->LinkToWorld();
				P_FindFloorCeiling(thing);
				portalcrossed = true;
			}
			else if (!portalcrossed)
			{
				DVector3 pos(tm.pos, thing->Z());
				DVector3 oldthingpos = thing->Pos();
				DVector2 thingpos = oldthingpos;
				
				P_TranslatePortalXY(ld, pos.X, pos.Y);
				P_TranslatePortalXY(ld, thingpos.X, thingpos.Y);
				P_TranslatePortalZ(ld, pos.Z);
				thing->SetXYZ(thingpos.X, thingpos.Y, pos.Z);
				if (!P_CheckPosition(thing, pos, true))	// check if some actor blocks us on the other side. (No line checks, because of the mess that'd create.)
				{
					thing->SetXYZ(oldthingpos);
					thing->flags6 &= ~MF6_INTRYMOVE;
					return false;
				}
				thing->UnlinkFromWorld();
				thing->SetXYZ(pos);
				P_TranslatePortalVXVY(ld, thing->Vel.X, thing->Vel.Y);
				P_TranslatePortalAngle(ld, thing->Angles.Yaw);
				thing->LinkToWorld();
				P_FindFloorCeiling(thing);
				thing->ClearInterpolation();
				portalcrossed = true;
			}
			// if this is the current camera we need to store the point where the portal was crossed and the exit
			// so that the renderer can properly calculate an interpolated position along the movement path.
			if (thing == players[consoleplayer].camera)
			{
				divline_t dl1 = { besthit.Oldrefpos.X,besthit.Oldrefpos.Y, besthit.Refpos.X - besthit.Oldrefpos.Y, besthit.Refpos.Y - besthit.Oldrefpos.Y };
				DVector3a hit = { {dl1.x + dl1.dx * bestfrac, dl1.y + dl1.dy * bestfrac, 0.},0. };

				R_AddInterpolationPoint(hit);
				if (port->mType == PORTT_LINKED)
				{
					hit.pos.X += port->mDisplacement.X;
					hit.pos.Y += port->mDisplacement.Y;
				}
				else
				{
					P_TranslatePortalXY(ld, hit.pos.X, hit.pos.Y);
					P_TranslatePortalZ(ld, hit.pos.Z);
					players[consoleplayer].viewz += hit.pos.Z;	// needs to be done here because otherwise the renderer will not catch the change.
					P_TranslatePortalAngle(ld, hit.angle);
				}
				R_AddInterpolationPoint(hit);
			}
			if (port->mType == PORTT_LINKED)
			{
				continue;
		}
		}
		break;
	}



	if (!portalcrossed)
	{
		// the move is ok, so link the thing into its new position
		thing->UnlinkFromWorld();

		oldsector = thing->Sector;
		thing->floorz = tm.floorz;
		thing->ceilingz= tm.ceilingz;
		thing->dropoffz = tm.dropoffz;		// killough 11/98: keep track of dropoffs
		thing->floorpic = tm.floorpic;
		thing->floorterrain = tm.floorterrain;
		thing->floorsector = tm.floorsector;
		thing->ceilingpic = tm.ceilingpic;
		thing->ceilingsector = tm.ceilingsector;
		thing->SetXY(pos);

		thing->LinkToWorld();
	}

	if (thing->flags2 & MF2_FLOORCLIP)
	{
		thing->AdjustFloorClip();
	}

	// if any special lines were hit, do the effect
	if (!(thing->flags & (MF_TELEPORT | MF_NOCLIP)))
	{
		spechit_t spec;
		DVector2 lastpos = thing->Pos();
		while (spechit.Pop(spec))
		{
			line_t *ld = spec.line;
			// see if the line was crossed

			// One more case of trying to preserve some side effects from the original:
			// If the reference position is the same as the actor's position before checking the spechits,
			// we use the thing's actual position, including all the side effects of the original.
			// If some portal transition has to be considered here, we cannot do that and use the reference position stored with the spechit.
			bool posisoriginal = (spec.Refpos == lastpos);
			side = posisoriginal? P_PointOnLineSide(thing->Pos(), ld) : P_PointOnLineSide(spec.Refpos, ld);
			oldside = P_PointOnLineSide(spec.Oldrefpos, ld);
			if (side != oldside && ld->special && !(thing->flags6 & MF6_NOTRIGGER))
			{
				if (thing->player && (thing->player->cheats & CF_PREDICTING))
				{
					P_PredictLine(ld, thing, oldside, SPAC_Cross);
				}
				else if (thing->player)
				{
					P_ActivateLine(ld, thing, oldside, SPAC_Cross);
				}
				else if (thing->flags2 & MF2_MCROSS)
				{
					P_ActivateLine(ld, thing, oldside, SPAC_MCross);
				}
				else if (thing->flags2 & MF2_PCROSS)
				{
					P_ActivateLine(ld, thing, oldside, SPAC_PCross);
				}
				else if ((ld->special == Teleport ||
					ld->special == Teleport_NoFog ||
					ld->special == Teleport_Line))
				{	// [RH] Just a little hack for BOOM compatibility
					P_ActivateLine(ld, thing, oldside, SPAC_MCross);
				}
				else
				{
					P_ActivateLine(ld, thing, oldside, SPAC_AnyCross);
				}
			}
		}
	}

	// [RH] Don't activate anything if just predicting
	if (thing->player && (thing->player->cheats & CF_PREDICTING))
	{
		thing->flags6 &= ~MF6_INTRYMOVE;
		return true;
	}

	// [RH] Check for crossing fake floor/ceiling
	newsec = thing->Sector;
	if (newsec->heightsec && oldsec->heightsec && newsec->SecActTarget)
	{
		const sector_t *hs = newsec->heightsec;
		double eyez = thing->Z() + FIXED2DBL(viewheight);
		double fakez = hs->floorplane.ZatPoint(pos);

		if (!oldAboveFakeFloor && eyez > fakez)
		{ // View went above fake floor
			newsec->SecActTarget->TriggerAction(thing, SECSPAC_EyesSurface);
		}
		else if (oldAboveFakeFloor && eyez <= fakez)
		{ // View went below fake floor
			newsec->SecActTarget->TriggerAction(thing, SECSPAC_EyesDive);
		}

		if (!(hs->MoreFlags & SECF_FAKEFLOORONLY))
		{
			fakez = hs->ceilingplane.ZatPoint(pos);
			if (!oldAboveFakeCeiling && eyez > fakez)
			{ // View went above fake ceiling
				newsec->SecActTarget->TriggerAction(thing, SECSPAC_EyesAboveC);
			}
			else if (oldAboveFakeCeiling && eyez <= fakez)
			{ // View went below fake ceiling
				newsec->SecActTarget->TriggerAction(thing, SECSPAC_EyesBelowC);
			}
		}
	}

	// [RH] If changing sectors, trigger transitions
	thing->CheckSectorTransition(oldsec);
	thing->flags6 &= ~MF6_INTRYMOVE;
	return true;

pushline:
	thing->flags6 &= ~MF6_INTRYMOVE;

	// [RH] Don't activate anything if just predicting
	if (thing->player && (thing->player->cheats & CF_PREDICTING))
	{
		return false;
	}

	thing->SetZ(oldz);
	if (!(thing->flags&(MF_TELEPORT | MF_NOCLIP)))
	{
		int numSpecHitTemp;

		if (tm.thing->flags2 & MF2_BLASTED)
		{
			P_DamageMobj(tm.thing, NULL, NULL, tm.thing->Mass >> 5, NAME_Melee);
		}
		numSpecHitTemp = (int)spechit.Size();
		while (numSpecHitTemp > 0)
		{
			// see which lines were pushed
			spechit_t &spec = spechit[--numSpecHitTemp];
			side = P_PointOnLineSide(spec.Refpos, spec.line);
			CheckForPushSpecial(spec.line, side, thing, &spec.Refpos);
		}
	}
	return false;
}

bool P_TryMove(AActor *thing, const DVector2 &pos,
	int dropoff, // killough 3/15/98: allow dropoff as option
	const secplane_t *onfloor) // [RH] Let P_TryMove keep the thing on the floor
{
	FCheckPosition tm;
	return P_TryMove(thing, pos, dropoff, onfloor, tm);
}



//==========================================================================
//
// P_CheckMove
// Similar to P_TryMove but doesn't actually move the actor. Used for polyobject crushing
//
//==========================================================================

bool P_CheckMove(AActor *thing, const DVector2 &pos)
{
	FCheckPosition tm;
	double		newz = thing->Z();

	if (!P_CheckPosition(thing, pos, tm))
	{
		return false;
	}

	if (thing->flags3 & MF3_FLOORHUGGER)
	{
		newz = tm.floorz;
	}
	else if (thing->flags3 & MF3_CEILINGHUGGER)
	{
		newz = tm.ceilingz - thing->Height;
	}

	if (!(thing->flags & MF_NOCLIP))
	{
		if (tm.ceilingz - tm.floorz < thing->Height)
		{
			return false;
		}

		if (!(thing->flags & MF_TELEPORT)
			&& tm.ceilingz - newz < thing->Height
			&& !(thing->flags3 & MF3_CEILINGHUGGER)
			&& (!(thing->flags2 & MF2_FLY) || !(thing->flags & MF_NOGRAVITY)))
		{
			return false;
		}
		if (thing->flags2 & MF2_FLY && thing->flags & MF_NOGRAVITY)
		{
			if (thing->Top() > tm.ceilingz)
				return false;
		}
		if (!(thing->flags & MF_TELEPORT) && !(thing->flags3 & MF3_FLOORHUGGER))
		{
			if (tm.floorz - newz > thing->MaxStepHeight)
			{ // too big a step up
				return false;
			}
			else if ((thing->flags & MF_MISSILE) && !(thing->flags6 & MF6_STEPMISSILE) && tm.floorz > newz)
			{ // [RH] Don't let normal missiles climb steps
				return false;
			}
			else if (newz < tm.floorz)
			{ // [RH] Check to make sure there's nothing in the way for the step up
				double savedz = thing->Z();
				thing->SetZ(newz = tm.floorz);
				bool good = P_TestMobjZ(thing);
				thing->SetZ(savedz);
				if (!good)
				{
					return false;
				}
			}
		}

		if (thing->flags2 & MF2_CANTLEAVEFLOORPIC
			&& (tm.floorpic != thing->floorpic
			|| tm.floorz - newz != 0))
		{ // must stay within a sector of a certain floor type
			return false;
		}
	}

	return true;
}



//==========================================================================
//
// SLIDE MOVE
// Allows the player to slide along any angled walls.
//
//==========================================================================

struct FSlide
{
	double 			bestSlidefrac;
	double 			secondSlidefrac;

	line_t* 		bestslideline;
	line_t* 		secondslideline;

	AActor* 		slidemo;

	DVector2		tmmove;

	void HitSlideLine(line_t *ld);
	void SlideTraverse(const DVector2 &start, const DVector2 &end);
	void SlideMove(AActor *mo, DVector2 tryp, int numsteps);

	// The bouncing code uses the same data structure
	bool BounceTraverse(const DVector2 &start, const DVector2 &end);
	bool BounceWall(AActor *mo);
};

//==========================================================================
//
// P_HitSlideLine
// Adjusts the xmove / ymove
// so that the next move will slide along the wall.
// If the floor is icy, then you can bounce off a wall.				// phares
//
//==========================================================================

void FSlide::HitSlideLine(line_t* ld)
{
	int 	side;

	DAngle lineangle;
	DAngle moveangle;
	DAngle deltaangle;

	double movelen;
	bool	icyfloor;	// is floor icy?							// phares
	//   |
	// Under icy conditions, if the angle of approach to the wall	//   V
	// is more than 45 degrees, then you'll bounce and lose half
	// your velocity. If less than 45 degrees, you'll slide along
	// the wall. 45 is arbitrary and is believable.

	// Check for the special cases of horz or vert walls.

	// killough 10/98: only bounce if hit hard (prevents wobbling)
	icyfloor =
		tmmove.LengthSquared() > 4*4 &&
		var_friction &&  // killough 8/28/98: calc friction on demand
		slidemo->Z() <= slidemo->floorz &&
		P_GetFriction(slidemo, NULL) > ORIG_FRICTION;

	if (ld->Delta().X == 0)
	{ // ST_VERTICAL
		if (icyfloor && (fabs(tmmove.X) > fabs(tmmove.Y)))
		{
			tmmove.X = -tmmove.X / 2;
			tmmove.Y /= 2; // absorb half the velocity
			if (slidemo->player && slidemo->health > 0 && !(slidemo->player->cheats & CF_PREDICTING))
			{
				S_Sound(slidemo, CHAN_VOICE, "*grunt", 1, ATTN_IDLE); // oooff!//   ^
			}
		}																		//   |
		else																	// phares
			tmmove.X = 0; // no more movement in the X direction
		return;
	}

	if (ld->Delta().Y == 0)
	{ // ST_HORIZONTAL
		if (icyfloor && (fabs(tmmove.Y) > fabs(tmmove.X)))
		{
			tmmove.X /= 2; // absorb half the velocity
			tmmove.Y = -tmmove.Y / 2;
			if (slidemo->player && slidemo->health > 0 && !(slidemo->player->cheats & CF_PREDICTING))
			{
				S_Sound(slidemo, CHAN_VOICE, "*grunt", 1, ATTN_IDLE); // oooff!
			}
		}
		else
			tmmove.Y = 0; // no more movement in the Y direction
		return;
	}

	// The wall is angled. Bounce if the angle of approach is		// phares
	// less than 45 degrees.										// phares

	DVector3 pos = slidemo->PosRelative(ld);
	side = P_PointOnLineSide(pos, ld);

	lineangle = ld->Delta().Angle();

	if (side == 1)
		lineangle += 180.;

	moveangle = tmmove.Angle();

	// prevents sudden path reversal due to rounding error |	// phares
	moveangle += 3600/65536.*65536.;		// Boom added 10 to the angle_t here.
	
	deltaangle = ::deltaangle(lineangle, moveangle);								//   V
	movelen = tmmove.Length();
	if (icyfloor && (deltaangle > 45) && (deltaangle < 135))
	{
		moveangle = ::deltaangle(deltaangle, lineangle);
		movelen /= 2; // absorb
		if (slidemo->player && slidemo->health > 0 && !(slidemo->player->cheats & CF_PREDICTING))
		{
			S_Sound(slidemo, CHAN_VOICE, "*grunt", 1, ATTN_IDLE); // oooff!
		}
		tmmove = moveangle.ToVector(movelen);
	}	
	else
	{	
		// The compatibility option that used to be here had to be removed because
		// with floating point math it was no longer possible to reproduce.

#if 0
		// with full precision this should work now. Needs some testing
		if (deltaangle < 0)	deltaangle += 180.;
		tmmove = lineangle.ToVector(movelen * deltaangle.Cos());
#else
		divline_t dll, dlv;
		double inter1, inter2, inter3;

		P_MakeDivline(ld, &dll);

		dlv.x = pos.X;
		dlv.y = pos.Y;
		dlv.dx = dll.dy;
		dlv.dy = -dll.dx;

		inter1 = P_InterceptVector(&dll, &dlv);

		dlv.dx = tmmove.X;
		dlv.dy = tmmove.Y;
		inter2 = P_InterceptVector(&dll, &dlv);
		inter3 = P_InterceptVector(&dlv, &dll);

		if (inter3 != 0)
		{
			tmmove.X = (inter2 - inter1) * dll.dx / inter3;
			tmmove.Y = (inter2 - inter1) * dll.dy / inter3;
		}
		else
		{
			tmmove.Zero();
		}
#endif
	}																// phares
}


//==========================================================================
//
// PTR_SlideTraverse
//
//==========================================================================

void FSlide::SlideTraverse(const DVector2 &start, const DVector2 &end)
{
	FLineOpening open;
	FPathTraverse it(start.X, start.Y, end.X, end.Y, PT_ADDLINES);
	intercept_t *in;

	while ((in = it.Next()))
	{
		line_t* 	li;

		if (!in->isaline)
		{
			// should never happen
			Printf("PTR_SlideTraverse: not a line?");
			continue;
		}

		li = in->d.line;

		if (!(li->flags & ML_TWOSIDED) || !li->backsector)
		{
			DVector3 pos = slidemo->PosRelative(li);
			if (P_PointOnLineSide(pos, li))
			{
				// don't hit the back side
				continue;
			}
			goto isblocking;
		}
		if (li->flags & (ML_BLOCKING | ML_BLOCKEVERYTHING))
		{
			goto isblocking;
		}
		if (li->flags & ML_BLOCK_PLAYERS && slidemo->player != NULL)
		{
			goto isblocking;
		}
		if (li->flags & ML_BLOCKMONSTERS && !((slidemo->flags3 & MF3_NOBLOCKMONST)
			|| ((i_compatflags & COMPATF_NOBLOCKFRIENDS) && (slidemo->flags & MF_FRIENDLY))))
		{
			goto isblocking;
		}

		// set openrange, opentop, openbottom
		P_LineOpening(open, slidemo, li, it.InterceptPoint(in));

		if (open.range < slidemo->Height)
			goto isblocking;				// doesn't fit

		if (open.top < slidemo->Top())
			goto isblocking;				// mobj is too high

		if (open.bottom - slidemo->Z() > slidemo->MaxStepHeight)
		{
			goto isblocking;				// too big a step up
		}
		else if (slidemo->Z() < open.bottom)
		{ // [RH] Check to make sure there's nothing in the way for the step up
			double savedz = slidemo->Z();
			slidemo->SetZ(open.bottom);
			bool good = P_TestMobjZ(slidemo);
			slidemo->SetZ(savedz);
			if (!good)
			{
				goto isblocking;
			}
		}

		// this line doesn't block movement
		continue;

		// the line does block movement,
		// see if it is closer than best so far
	isblocking:
		if (in->Frac < bestSlidefrac)
		{
			secondSlidefrac = bestSlidefrac;
			secondslideline = bestslideline;
			bestSlidefrac = in->Frac;
			bestslideline = li;
		}

		return;		// stop
	}
}



//==========================================================================
//
// P_SlideMove
//
// The vel.x / vel.y move is bad, so try to slide along a wall.
//
// Find the first line hit, move flush to it, and slide along it
//
// This is a kludgy mess.
//
//==========================================================================

void FSlide::SlideMove(AActor *mo, DVector2 tryp, int numsteps)
{
	DVector2 lead;
	DVector2 trail;
	DVector2 newpos;
	DVector2 move;
	const secplane_t * walkplane;
	int hitcount;

	hitcount = 3;
	slidemo = mo;

	if (mo->player && mo->player->mo == mo && mo->reactiontime > 0)
		return;	// player coming right out of a teleporter.

retry:
	if (!--hitcount)
		goto stairstep; 		// don't loop forever

	// trace along the three leading corners
	if (tryp.X > 0)
	{
		lead.X = mo->X() + mo->radius;
		trail.X = mo->X() - mo->radius;
	}
	else
	{
		lead.X = mo->X() - mo->radius;
		trail.X = mo->X() + mo->radius;
	}

	if (tryp.Y > 0)
	{
		lead.Y = mo->Y() + mo->radius;
		trail.Y = mo->Y() - mo->radius;
	}
	else
	{
		lead.Y = mo->Y() - mo->radius;
		trail.Y = mo->Y() + mo->radius;
	}

	bestSlidefrac = 1.01;

	SlideTraverse(lead, lead + tryp);
	SlideTraverse(DVector2(trail.X, lead.Y), tryp + DVector2(trail.X, lead.Y));
	SlideTraverse(DVector2(lead.X, trail.Y), tryp + DVector2(lead.X, trail.Y));

	// move up to the wall
	if (bestSlidefrac > 1)
	{
		// the move must have hit the middle, so stairstep
	stairstep:
		// killough 3/15/98: Allow objects to drop off ledges
		move = { 0, tryp.Y };
		walkplane = P_CheckSlopeWalk(mo, move);
		if (!P_TryMove(mo, mo->Pos() + move, true, walkplane))
		{
			move = { tryp.X, 0 };
			walkplane = P_CheckSlopeWalk(mo, move);
			P_TryMove(mo, mo->Pos() + move, true, walkplane);
		}
		return;
	}

	// fudge a bit to make sure it doesn't hit
	bestSlidefrac -= FRACUNIT / 32;
	if (bestSlidefrac > 0)
	{
		newpos = tryp * bestSlidefrac;

		// [BL] We need to abandon this function if we end up going through a teleporter
		const DVector2 startvel = mo->Vel.XY();

		// killough 3/15/98: Allow objects to drop off ledges
		if (!P_TryMove(mo, mo->Pos() + newpos, true))
			goto stairstep;

		if (mo->Vel.XY() != startvel)
			return;
	}

	// Now continue along the wall.
	bestSlidefrac = 1. - (bestSlidefrac + 1. / 32);	// remainder
	if (bestSlidefrac > 1)
		bestSlidefrac = 1;
	else if (bestSlidefrac <= 0)
		return;

	tryp = tmmove = tryp*bestSlidefrac;

	HitSlideLine(bestslideline); 	// clip the moves

	mo->Vel.X = tmmove.X * numsteps;
	mo->Vel.Y = tmmove.Y * numsteps;

	// killough 10/98: affect the bobbing the same way (but not voodoo dolls)
	if (mo->player && mo->player->mo == mo)
	{
		if (fabs(mo->player->Vel.X) > fabs(mo->Vel.X))
			mo->player->Vel.X = mo->Vel.X;
		if (fabs(mo->player->Vel.Y) > fabs(mo->Vel.Y))
			mo->player->Vel.Y = mo->Vel.Y;
	}

	walkplane = P_CheckSlopeWalk(mo, tmmove);

	// killough 3/15/98: Allow objects to drop off ledges
	if (!P_TryMove(mo, mo->Pos() + tmmove, true, walkplane))
	{
		goto retry;
	}
}

void P_SlideMove(AActor *mo, const DVector2 &pos, int numsteps)
{
	FSlide slide;
	slide.SlideMove(mo, pos, numsteps);
}

//============================================================================
//
// P_CheckSlopeWalk
//
//============================================================================

const secplane_t * P_CheckSlopeWalk(AActor *actor, DVector2 &move)
{
	static secplane_t copyplane;
	if (actor->flags & MF_NOGRAVITY)
	{
		return NULL;
	}

	DVector3 pos = actor->PosRelative(actor->floorsector);
	const secplane_t *plane = &actor->floorsector->floorplane;
	double planezhere = plane->ZatPoint(pos);

	for (auto rover : actor->floorsector->e->XFloor.ffloors)
	{
		if (!(rover->flags & FF_SOLID) || !(rover->flags & FF_EXISTS)) continue;

		double thisplanez = rover->top.plane->ZatPoint(pos);

		if (thisplanez > planezhere && thisplanez <= actor->Z() + actor->MaxStepHeight)
		{
			copyplane = *rover->top.plane;
			if (copyplane.c < 0) copyplane.FlipVert();
			plane = &copyplane;
			planezhere = thisplanez;
		}
	}

	if (actor->floorsector != actor->Sector)
	{
		for (auto rover : actor->Sector->e->XFloor.ffloors)
		{
			if (!(rover->flags & FF_SOLID) || !(rover->flags & FF_EXISTS)) continue;

			double thisplanez = rover->top.plane->ZatPointF(actor);

			if (thisplanez > planezhere && thisplanez <= actor->Z() + actor->MaxStepHeight)
			{
				copyplane = *rover->top.plane;
				if (copyplane.c < 0) copyplane.FlipVert();
				plane = &copyplane;
				planezhere = thisplanez;
			}
		}
	}

	if (actor->floorsector != actor->Sector)
	{
		// this additional check prevents sliding on sloped dropoffs
		if (planezhere>actor->floorz + 4)
			return NULL;
	}

	if (actor->Z() - planezhere > 1)
	{ // not on floor
		return NULL;
	}

	if (plane->isSlope())
	{
		DVector2 dest;
		double t;

		dest = actor->Pos() + move;
		t = plane->fA() * dest.X + plane->fB() * dest.Y + plane->fC() * actor->Z() + plane->fD();
		if (t < 0)
		{ // Desired location is behind (below) the plane
			// (i.e. Walking up the plane)
			if (plane->c < STEEPSLOPE)
			{ // Can't climb up slopes of ~45 degrees or more
				if (actor->flags & MF_NOCLIP)
				{
					return (actor->floorsector == actor->Sector) ? plane : NULL;
				}
				else
				{
					const msecnode_t *node;
					bool dopush = true;

					if (plane->c > STEEPSLOPE * 2 / 3)
					{
						for (node = actor->touching_sectorlist; node; node = node->m_tnext)
						{
							sector_t *sec = node->m_sector;
							if (sec->floorplane.c >= STEEPSLOPE)
							{
								DVector3 pos = actor->PosRelative(sec) +move;

								if (sec->floorplane.ZatPoint(pos) >= actor->Z() - actor->MaxStepHeight)
								{
									dopush = false;
									break;
								}
							}
						}
					}
					if (dopush)
					{
						actor->Vel.X = move.X = plane->fA() * 2;
						actor->Vel.Y = move.Y = plane->fB() * 2;
					}
					return (actor->floorsector == actor->Sector) ? plane : NULL;
				}
			}
			// Slide the desired location along the plane's normal
			// so that it lies on the plane's surface
			dest.X -= plane->fA() * t;
			dest.Y -= plane->fB() * t;
			move = dest - actor->Pos().XY();
			return (actor->floorsector == actor->Sector) ? plane : NULL;
		}
		else if (t > 0)
		{ // Desired location is in front of (above) the plane
			if (fabs(planezhere - actor->Z() < (1/65536.)))	// it is very important not to be too precise here.
			{ 
				// Actor's current spot is on/in the plane, so walk down it
				// Same principle as walking up, except reversed
				dest.X += plane->fA() * t;
				dest.Y += plane->fB() * t;
				move = dest - actor->Pos().XY();
				return (actor->floorsector == actor->Sector) ? plane : NULL;
			}
		}
	}
	return NULL;
}

//============================================================================
//
// PTR_BounceTraverse
//
//============================================================================

bool FSlide::BounceTraverse(const DVector2 &start, const DVector2 &end)
{
	FLineOpening open;
	FPathTraverse it(start.X, start.Y, end.X, end.Y, PT_ADDLINES);
	intercept_t *in;

	while ((in = it.Next()))
	{

		line_t  *li;

		if (!in->isaline)
		{
			Printf("PTR_BounceTraverse: not a line?");
			continue;
		}

		li = in->d.line;
		assert(((size_t)li - (size_t)lines) % sizeof(line_t) == 0);
		if (li->flags & ML_BLOCKEVERYTHING)
		{
			goto bounceblocking;
		}
		if (!(li->flags&ML_TWOSIDED) || !li->backsector)
		{
			if (P_PointOnLineSide(slidemo->_f_X(), slidemo->_f_Y(), li))
				continue;			// don't hit the back side
			goto bounceblocking;
		}


		P_LineOpening(open, slidemo, li, it._f_InterceptPoint(in));	// set openrange, opentop, openbottom
		if (open.range < slidemo->Height)
			goto bounceblocking;				// doesn't fit

		if (open.top < slidemo->Top())
			goto bounceblocking;				// mobj is too high

		if (open.bottom > slidemo->Z())
			goto bounceblocking;				// mobj is too low

		continue;			// this line doesn't block movement

		// the line does block movement, see if it is closer than best so far
	bounceblocking:
		if (in->Frac < bestSlidefrac)
		{
			secondSlidefrac = bestSlidefrac;
			secondslideline = bestslideline;
			bestSlidefrac = in->Frac;
			bestslideline = li;
		}
		return false;   // stop
	}
	return true;
}

//============================================================================
//
// P_BounceWall
//
//============================================================================

bool FSlide::BounceWall(AActor *mo)
{
	DVector2         lead;
	int             side;
	DAngle         lineangle, moveangle, deltaangle;
	double         movelen;
	line_t			*line;

	if (!(mo->BounceFlags & BOUNCE_Walls))
	{
		return false;
	}

	slidemo = mo;
	//
	// trace along the three leading corners
	//
	if (mo->Vel.X > 0)
	{
		lead.X = mo->X() + mo->radius;
	}
	else
	{
		lead.X = mo->X() - mo->radius;
	}
	if (mo->Vel.Y > 0)
	{
		lead.Y = mo->Y() + mo->radius;
	}
	else
	{
		lead.Y = mo->Y() - mo->radius;
	}
	bestSlidefrac = 1.01;
	bestslideline = mo->BlockingLine;
	if (BounceTraverse(lead, lead+mo->Vel.XY()) && mo->BlockingLine == NULL)
	{ // Could not find a wall, so bounce off the floor/ceiling instead.
		double floordist = mo->Z() - mo->floorz;
		double ceildist = mo->ceilingz - mo->Z();
		if (floordist <= ceildist)
		{
			mo->FloorBounceMissile(mo->Sector->floorplane);
			return true;
		}
		else
		{
			mo->FloorBounceMissile(mo->Sector->ceilingplane);
			return true;
		}
	}
	line = bestslideline;

	if (line->special == Line_Horizon)
	{
		mo->SeeSound = 0;	// it might make a sound otherwise
		mo->Destroy();
		return true;
	}

	// The amount of bounces is limited
	if (mo->bouncecount>0 && --mo->bouncecount == 0)
	{
		if (mo->flags & MF_MISSILE)
			P_ExplodeMissile(mo, line, NULL);
		else
			mo->Die(NULL, NULL);
		return true;
	}

	side = P_PointOnLineSide(mo->Pos(), line);
	lineangle = line->Delta().Angle();
	if (side == 1)
	{
		lineangle += 180;
	}
	moveangle = mo->Vel.Angle();
	deltaangle = (lineangle * 2) - moveangle;
	mo->Angles.Yaw = deltaangle;

	movelen = mo->Vel.XY().Length() * mo->wallbouncefactor;

	FBoundingBox box(mo->X(), mo->Y(), mo->radius);
	if (box.BoxOnLineSide(line) == -1)
	{
		DVector2 ofs = deltaangle.ToVector(mo->radius);
		DVector3 pos = mo->Vec3Offset(ofs.X, ofs.Y, 0.);
		mo->SetOrigin(pos, true);

	}
	if (movelen < 1)
	{
		movelen = 2;
	}
	DVector2 vel = deltaangle.ToVector(movelen);
	mo->Vel.X = vel.X;
	mo->Vel.Y = vel.Y;
	if (mo->BounceFlags & BOUNCE_UseBounceState)
	{
		FState *bouncestate = mo->FindState(NAME_Bounce, NAME_Wall);
		if (bouncestate != NULL)
		{
			mo->SetState(bouncestate);
		}
	}
	return true;
}

bool P_BounceWall(AActor *mo)
{
	FSlide slide;
	return slide.BounceWall(mo);
}

//==========================================================================
//
//
//
//==========================================================================

extern FRandom pr_bounce;
bool P_BounceActor(AActor *mo, AActor *BlockingMobj, bool ontop)
{
	//Don't go through all of this if the actor is reflective and wants things to pass through them.
	if (BlockingMobj && ((BlockingMobj->flags2 & MF2_REFLECTIVE) && (BlockingMobj->flags7 & MF7_THRUREFLECT))) return true;
	if (mo && BlockingMobj && ((mo->BounceFlags & BOUNCE_AllActors)
		|| ((mo->flags & MF_MISSILE) && (!(mo->flags2 & MF2_RIP) 
		|| (BlockingMobj->flags5 & MF5_DONTRIP) 
		|| ((mo->flags6 & MF6_NOBOSSRIP) && (BlockingMobj->flags2 & MF2_BOSS))) && (BlockingMobj->flags2 & MF2_REFLECTIVE))
		|| ((BlockingMobj->player == NULL) && (!(BlockingMobj->flags3 & MF3_ISMONSTER)))))
	{
		if (mo->bouncecount > 0 && --mo->bouncecount == 0) return false;

		if (mo->flags7 & MF7_HITTARGET)	mo->target = BlockingMobj;
		if (mo->flags7 & MF7_HITMASTER)	mo->master = BlockingMobj;
		if (mo->flags7 & MF7_HITTRACER)	mo->tracer = BlockingMobj;

		if (!ontop)
		{
			DAngle angle = BlockingMobj->AngleTo(mo) + ((pr_bounce() % 16) - 8);
			double speed = mo->VelXYToSpeed() * mo->wallbouncefactor; // [GZ] was 0.75, using wallbouncefactor seems more consistent
			mo->Angles.Yaw = ANGLE2DBL(angle);
			mo->VelFromAngle(speed);
			mo->PlayBounceSound(true);
			if (mo->BounceFlags & BOUNCE_UseBounceState)
			{
				FName names[] = { NAME_Bounce, NAME_Actor, NAME_Creature };
				FState *bouncestate;
				int count = 2;

				if ((BlockingMobj->flags & MF_SHOOTABLE) && !(BlockingMobj->flags & MF_NOBLOOD))
				{
					count = 3;
				}
				bouncestate = mo->FindState(count, names);
				if (bouncestate != NULL)
				{
					mo->SetState(bouncestate);
				}
			}
		}
		else
		{
			double dot = mo->Vel.Z;

			if (mo->BounceFlags & (BOUNCE_HereticType | BOUNCE_MBF))
			{
				mo->Vel.Z -= 2. / dot;
				if (!(mo->BounceFlags & BOUNCE_MBF)) // Heretic projectiles die, MBF projectiles don't.
				{
					mo->flags |= MF_INBOUNCE;
					mo->SetState(mo->FindState(NAME_Death));
					mo->flags &= ~MF_INBOUNCE;
					return false;
				}
				else
				{
					mo->Vel.Z *= mo->bouncefactor;
				}
			}
			else // Don't run through this for MBF-style bounces
			{
				// The reflected velocity keeps only about 70% of its original speed
				mo->Vel.Z = (mo->Vel.Z - 2. / dot) * mo->bouncefactor;
			}

			mo->PlayBounceSound(true);
			if (mo->BounceFlags & BOUNCE_MBF) // Bring it to rest below a certain speed
			{
				if (fabs(mo->Vel.Z) < mo->Mass * mo->GetGravity() / 64)
					mo->Vel.Z = 0;
			}
			else if (mo->BounceFlags & (BOUNCE_AutoOff | BOUNCE_AutoOffFloorOnly))
			{
				if (!(mo->flags & MF_NOGRAVITY) && (mo->Vel.Z < 3.))
					mo->BounceFlags &= ~BOUNCE_TypeMask;
			}
		}
		return true;
	}
	return false;
}

//============================================================================
//
// Aiming
//
//============================================================================

CVAR(Bool, aimdebug, false, 0)

struct AimTarget : public FTranslatedLineTarget
{
	DAngle pitch;
	double frac;

	void Clear()
	{
		memset(this, 0, sizeof(*this));
		frac = FLT_MAX;
	}
};

struct aim_t
{
	enum
	{
		aim_up = 1,
		aim_down = 2
	};

	DAngle			aimpitch;
	double			attackrange;
	double			shootz;			// Height if not aiming up or down
	double			limitz;			// height limit for portals to avoid bad setups
	AActor*			shootthing;
	AActor*			friender;		// actor to check friendliness again
	AActor*			aimtarget;		// if we want to aim at precisely this target.

	DAngle			toppitch, bottompitch;
	AimTarget		linetarget;
	AimTarget		thing_friend, thing_other;

	int				flags;
	sector_t *		lastsector;
	secplane_t *	lastfloorplane;
	secplane_t *	lastceilingplane;

	int				aimdir;
	DVector3		startpos;
	DVector2		aimtrace;
	double			startfrac;

	bool			crossedffloors;
	bool			unlinked;

	// Creates a clone of this structure with the basic info copied.
	aim_t Clone()
	{
		aim_t cloned;

		cloned.aimtrace = aimtrace;
		cloned.aimpitch = aimpitch;
		cloned.aimtarget = aimtarget;
		cloned.attackrange = attackrange;
		cloned.shootthing = shootthing;
		cloned.friender = friender;
		cloned.shootz = shootz;
		cloned.unlinked = unlinked;
		cloned.flags = flags;
		return cloned;
	}

	//============================================================================
	//
	// SetResult
	//
	//============================================================================

	void SetResult(AimTarget &res, double frac, AActor *th, DAngle pitch)
	{
		if (res.frac > frac)
		{
			res.linetarget = th;
			res.pitch = pitch;
			res.angleFromSource = (th->Pos() - startpos).Angle();
			res.unlinked = unlinked;
			res.frac = frac;
		}
	}

	void SetResult(AimTarget &res, AimTarget &set)
	{
		if (res.frac > set.frac)
		{
			res = set;
		}
	}

	//============================================================================
	//
	// Result
	//
	//============================================================================

	AimTarget *Result()
	{
		AimTarget *result = &linetarget;
		if (result->linetarget == NULL)
		{
			if (thing_other.linetarget != NULL)
			{
				result = &thing_other;
			}
			else if (thing_friend.linetarget != NULL)
			{
				result = &thing_friend;
			}
		}
		return result;
	}


	//============================================================================
	//
	// AimTraverse3DFloors
	//
	//============================================================================

	bool AimTraverse3DFloors(const divline_t &trace, intercept_t * in, int frontflag, int *planestocheck)
	{
		sector_t * nextsector;
		secplane_t * nexttopplane, *nextbottomplane;
		line_t * li = in->d.line;

		nextsector = NULL;
		nexttopplane = nextbottomplane = NULL;
		*planestocheck = aimdir;

		if (li->backsector == NULL) return true;	// shouldn't really happen but crashed once for me...
		if (li->frontsector->e->XFloor.ffloors.Size() || li->backsector->e->XFloor.ffloors.Size())
		{
			F3DFloor* rover;
			DAngle highpitch, lowpitch;

			double trX = trace.x + trace.dx * in->Frac;
			double trY = trace.y + trace.dy * in->Frac;
			double dist = attackrange * in->Frac;

			// 3D floor check. This is not 100% accurate but normally sufficient when
			// combined with a final sight check
			for (int i = 1; i <= 2; i++)
			{
				sector_t * s = i == 1 ? li->frontsector : li->backsector;

				for (unsigned k = 0; k < s->e->XFloor.ffloors.Size(); k++)
				{
					crossedffloors = true;
					rover = s->e->XFloor.ffloors[k];

					if ((rover->flags & FF_SHOOTTHROUGH) || !(rover->flags & FF_EXISTS)) continue;

					double ff_bottom = rover->bottom.plane->ZatPoint(trX, trY);
					double ff_top = rover->top.plane->ZatPoint(trX, trY);


					highpitch = -VecToAngle(dist, ff_top - shootz);
					lowpitch = -VecToAngle(dist, ff_bottom - shootz);

					if (highpitch <= toppitch)
					{
						// blocks completely
						if (lowpitch >= bottompitch) return false;
						// blocks upper edge of view
						if (lowpitch > toppitch)
						{
							toppitch = lowpitch;
							if (frontflag != i - 1)
							{
								nexttopplane = rover->bottom.plane;
								*planestocheck &= ~aim_up;
							}
						}
					}
					else if (lowpitch >= bottompitch)
					{
						// blocks lower edge of view
						if (highpitch < bottompitch)
						{
							bottompitch = highpitch;
							if (frontflag != i - 1)
							{
								nextbottomplane = rover->top.plane;
								*planestocheck &= ~aim_down;
							}
						}
					}
					// trace is leaving a sector with a 3d-floor

					if (frontflag == i - 1)
					{
						if (s == lastsector)
						{
							// upper slope intersects with this 3d-floor
							if (rover->bottom.plane == lastceilingplane && lowpitch > toppitch)
							{
								toppitch = lowpitch;
							}
							// lower slope intersects with this 3d-floor
							if (rover->top.plane == lastfloorplane && highpitch < bottompitch)
							{
								bottompitch = highpitch;
							}
						}
					}
					if (toppitch >= bottompitch) return false;		// stop
				}
			}
		}

		lastsector = nextsector;
		lastceilingplane = nexttopplane;
		lastfloorplane = nextbottomplane;
		return true;
	}

	//============================================================================
	//
	// traverses a sector portal
	//
	//============================================================================

	void EnterSectorPortal(int position, double frac, sector_t *entersec, DAngle newtoppitch, DAngle newbottompitch)
	{
		AActor *portal = entersec->SkyBoxes[position];

		if (position == sector_t::ceiling && portal->specialf1 < limitz) return;
		else if (position == sector_t::floor && portal->specialf1 > limitz) return;
		aim_t newtrace = Clone();


		newtrace.toppitch = newtoppitch;
		newtrace.bottompitch = newbottompitch;
		newtrace.aimdir = position == sector_t::ceiling? aim_t::aim_up : aim_t::aim_down;
		newtrace.startpos = startpos + portal->Scale;
		newtrace.startfrac = frac + 1. / attackrange;	// this is to skip the transition line to the portal which would produce a bogus opening
		newtrace.lastsector = P_PointInSector(newtrace.startpos + aimtrace * newtrace.startfrac);
		newtrace.limitz = portal->specialf1;
		if (aimdebug)
			Printf("-----Entering %s portal from sector %d to sector %d\n", position ? "ceiling" : "floor", lastsector->sectornum, newtrace.lastsector->sectornum);
		newtrace.AimTraverse();
		SetResult(linetarget, newtrace.linetarget);
		SetResult(thing_friend, newtrace.thing_friend);
		SetResult(thing_other, newtrace.thing_other);
		if (aimdebug)
			Printf("-----Exiting %s portal\n", position ? "ceiling" : "floor");
	}

	//============================================================================
	//
	// traverses a line portal
	//
	//============================================================================

	void EnterLinePortal(line_t *li, double frac)
	{
		aim_t newtrace = Clone();

		FLinePortal *port = li->getPortal();
		if (port->mType != PORTT_LINKED && (flags & ALF_PORTALRESTRICT)) return;

		newtrace.toppitch = toppitch;
		newtrace.bottompitch = bottompitch;
		newtrace.aimdir = aimdir;
		newtrace.unlinked = (port->mType != PORTT_LINKED);
		newtrace.startpos = startpos;
		newtrace.aimtrace = aimtrace;
		P_TranslatePortalXY(li, newtrace.startpos.X, newtrace.startpos.Y);
		P_TranslatePortalZ(li, newtrace.startpos.Z);
		P_TranslatePortalVXVY(li, newtrace.aimtrace.X, newtrace.aimtrace.Y);

		newtrace.startfrac = frac + 1 / attackrange;	// this is to skip the transition line to the portal which would produce a bogus opening

		DVector2 pos = newtrace.startpos + newtrace.aimtrace * newtrace.startfrac;

		newtrace.lastsector = P_PointInSector(pos);
		P_TranslatePortalZ(li, limitz);
		if (aimdebug)
			Printf("-----Entering line portal from sector %d to sector %d\n", lastsector->sectornum, newtrace.lastsector->sectornum);
		newtrace.AimTraverse();
		SetResult(linetarget, newtrace.linetarget);
		SetResult(thing_friend, newtrace.thing_friend);
		SetResult(thing_other, newtrace.thing_other);
	}


	//============================================================================
	//
	// PTR_AimTraverse
	// Sets linetaget and aimpitch when a target is aimed at.
	//
	//============================================================================

	void AimTraverse()
	{
		// for smart aiming
		linetarget.Clear();
		thing_friend.Clear();
		thing_other.Clear();
		crossedffloors = lastsector->e->XFloor.ffloors.Size() != 0;
		lastfloorplane = lastceilingplane = NULL;

		// check the initial sector for 3D-floors and portals
		bool ceilingportalstate = (aimdir & aim_t::aim_up) && toppitch < 0 && !lastsector->PortalBlocksMovement(sector_t::ceiling);
		bool floorportalstate = (aimdir & aim_t::aim_down) && bottompitch > 0 && !lastsector->PortalBlocksMovement(sector_t::floor);

		for (auto rover : lastsector->e->XFloor.ffloors)
		{
			if ((rover->flags & FF_SHOOTTHROUGH) || !(rover->flags & FF_EXISTS)) continue;

			double bottomz = rover->bottom.plane->ZatPoint(startpos);

			if (bottomz >= startpos.Z + shootthing->Height)
			{
				lastceilingplane = rover->bottom.plane;
				// no ceiling portal if below a 3D floor
				ceilingportalstate = false;
			}

			bottomz = rover->top.plane->ZatPoint(startpos);
			if (bottomz <= startpos.Z)
			{
				lastfloorplane = rover->top.plane;
				// no floor portal if above a 3D floor
				floorportalstate = false;
			}
		}
		if (ceilingportalstate) EnterSectorPortal(sector_t::ceiling, 0, lastsector, toppitch, MIN<DAngle>(0., bottompitch));
		if (floorportalstate) EnterSectorPortal(sector_t::floor, 0, lastsector, MAX<DAngle>(0., toppitch), bottompitch);

		FPathTraverse it(startpos.X, startpos.Y, aimtrace.X, aimtrace.Y, PT_ADDLINES | PT_ADDTHINGS | PT_COMPATIBLE | PT_DELTA, startfrac);
		intercept_t *in;

		if (aimdebug)
			Printf("Start AimTraverse, start = %f,%f,%f, vect = %f,%f\n",
				startpos.X / 65536., startpos.Y / 65536., startpos.Z / 65536.,
				aimtrace.X / 65536., aimtrace.Y / 65536.);
		
		while ((in = it.Next()))
		{
			line_t* 			li;
			AActor* 			th;
			DAngle	 			pitch;
			DAngle				thingtoppitch;
			DAngle 				thingbottompitch;
			double 				dist;
			DAngle				thingpitch;

			if (linetarget.linetarget != NULL && in->Frac > linetarget.frac) return;	// we already found something better in another portal section.

			if (in->isaline)
			{
				li = in->d.line;
				int frontflag = P_PointOnLineSidePrecise(startpos, li);

				if (aimdebug)
					Printf("Found line %d: ___toppitch = %f, ___bottompitch = %f\n", int(li - lines), toppitch.Degrees, bottompitch.Degrees);

				if (li->isLinePortal() && frontflag == 0)
				{
					EnterLinePortal(li, in->Frac);
					return;
				}


				if (!(li->flags & ML_TWOSIDED) || (li->flags & ML_BLOCKEVERYTHING))
					return;				// stop

				// Crosses a two sided line.
				// A two sided line will restrict the possible target ranges.
				FLineOpening open;
				P_LineOpening(open, NULL, li, it.InterceptPoint(in), (DVector2*)nullptr, FFCF_NODROPOFF);

				// The following code assumes that portals on the front of the line have already been processed.

				if (open.range <= 0 || open.bottom >= open.top)
					return;
					
				dist = attackrange * in->Frac;

				if (open.bottom != LINEOPEN_MIN)
				{
					pitch = -VecToAngle(dist, open.bottom - shootz);
					if (pitch < bottompitch) bottompitch = pitch;
				}

				if (open.top != LINEOPEN_MAX)
				{
					pitch = -VecToAngle(dist, open.top - shootz);
					if (pitch > toppitch) toppitch = pitch;
				}

				if (toppitch >= bottompitch)
					return;

				int planestocheck;
				if (!AimTraverse3DFloors(it.Trace(), in, frontflag, &planestocheck))
					return;

				if (aimdebug)
					Printf("After line %d: toppitch = %f, bottompitch = %f, planestocheck = %d\n", int(li - lines), toppitch.Degrees, bottompitch.Degrees, planestocheck);

				sector_t *entersec = frontflag ? li->frontsector : li->backsector;
				sector_t *exitsec = frontflag ? li->backsector : li->frontsector;
				lastsector = entersec;
				// check portal in backsector when aiming up/downward is possible, the line doesn't have portals on both sides and there's actually a portal in the backsector
				if ((planestocheck & aim_up) && toppitch < 0 && open.top != LINEOPEN_MAX && !entersec->PortalBlocksMovement(sector_t::ceiling))
				{
					EnterSectorPortal(sector_t::ceiling, in->Frac, entersec, toppitch, MIN<DAngle>(0., bottompitch));
				}
				if ((planestocheck & aim_down) && bottompitch > 0 && open.bottom != LINEOPEN_MIN && !entersec->PortalBlocksMovement(sector_t::floor))
				{
					EnterSectorPortal(sector_t::floor, in->Frac, entersec, MAX<DAngle>(0., toppitch), bottompitch);
				}
				continue;					// shot continues
			}

			// shoot a thing
			th = in->d.thing;
			if (th == shootthing)
				continue;					// can't shoot self

			if (aimtarget != NULL && th != aimtarget)
				continue;					// only care about target, and you're not it

			// If we want to start a conversation anything that has one should be
			// found, regardless of other settings.
			if (!(flags & ALF_CHECKCONVERSATION) || th->Conversation == NULL)
			{
				if (!(flags & ALF_CHECKNONSHOOTABLE))			// For info CCMD, ignore stuff about GHOST and SHOOTABLE flags
				{
					if (!(th->flags&MF_SHOOTABLE))
						continue;					// corpse or something

					// check for physical attacks on a ghost
					if ((th->flags3 & MF3_GHOST) &&
						shootthing->player &&	// [RH] Be sure shootthing is a player
						shootthing->player->ReadyWeapon &&
						(shootthing->player->ReadyWeapon->flags2 & MF2_THRUGHOST))
					{
						continue;
					}
				}
			}
			dist = attackrange * in->Frac;

			// Don't autoaim certain special actors
			if (!cl_doautoaim && th->flags6 & MF6_NOTAUTOAIMED)
			{
				continue;
			}

			// we must do one last check whether the trace has crossed a 3D floor
			if (lastsector == th->Sector && th->Sector->e->XFloor.ffloors.Size())
			{
				if (lastceilingplane)
				{
					double ff_top = lastceilingplane->ZatPointF(th);
					DAngle pitch = -VecToAngle(dist, ff_top - shootz);
					// upper slope intersects with this 3d-floor
					if (pitch > toppitch)
					{
						toppitch = pitch;
					}
				}
				if (lastfloorplane)
				{
					double ff_bottom = lastfloorplane->ZatPointF(th);
					DAngle pitch = -VecToAngle(dist, ff_bottom - shootz);
					// lower slope intersects with this 3d-floor
					if (pitch < bottompitch)
					{
						bottompitch = pitch;
					}
				}
			}

			// check angles to see if the thing can be aimed at

			thingtoppitch = -VecToAngle(dist, th->Top() - shootz);

			if (thingtoppitch > bottompitch)
				continue;					// shot over the thing

			thingbottompitch = -VecToAngle(dist, th->Z() - shootz);

			if (thingbottompitch < toppitch)
				continue;					// shot under the thing

			if (crossedffloors)
			{
				// if 3D floors were in the way do an extra visibility check for safety
				if (!unlinked && !P_CheckSight(shootthing, th, SF_IGNOREVISIBILITY | SF_IGNOREWATERBOUNDARY))
				{
					// the thing can't be seen so we can safely exclude its range from our aiming field
					if (thingtoppitch < toppitch)
					{
						if (thingbottompitch > toppitch) toppitch = thingbottompitch;
					}
					else if (thingbottompitch>bottompitch)
					{
						if (thingtoppitch < bottompitch) bottompitch = thingtoppitch;
					}
					if (toppitch < bottompitch) continue;
					else return;
				}
			}

			// this thing can be hit!
			if (thingtoppitch < toppitch)
				thingtoppitch = toppitch;

			if (thingbottompitch > bottompitch)
				thingbottompitch = bottompitch;

			thingpitch = thingtoppitch / 2 + thingbottompitch / 2;

			if (flags & ALF_CHECK3D)
			{
				// We need to do a 3D distance check here because this is nearly always used in
				// combination with P_LineAttack. P_LineAttack uses 3D distance but FPathTraverse
				// only 2D. This causes some problems with Hexen's weapons that use different
				// attack modes based on distance to target
				double cosine = thingpitch.Cos();
				if (cosine != 0)
				{
					double tracelen = DVector2(it.Trace().dx, it.Trace().dy).Length();
					double d3 = tracelen * in->Frac / cosine;
					if (d3 > attackrange)
					{
						return;
					}
				}
			}

			if ((flags & ALF_NOFRIENDS) && th->IsFriend(friender) && aimtarget == NULL)
			{
				continue;
			}
			else if (sv_smartaim != 0 && !(flags & ALF_FORCENOSMART) && aimtarget == NULL)
			{
				// try to be a little smarter about what to aim at!
				// In particular avoid autoaiming at friends and barrels.
				if (th->IsFriend(friender))
				{
					if (sv_smartaim < 2)
					{
						// friends don't aim at friends (except players), at least not first
						if (aimdebug)
							Printf("Hit friend %s at %f,%f,%f\n", th->GetClass()->TypeName.GetChars(), th->X(), th->Y(), th->Z());
						SetResult(thing_friend, in->Frac, th, thingpitch);
					}
				}
				else if (!(th->flags3 & MF3_ISMONSTER) && th->player == NULL)
				{
					if (sv_smartaim < 3)
					{
						// don't autoaim at barrels and other shootable stuff unless no monsters have been found
						if (aimdebug)
							Printf("Hit other %s at %f,%f,%f\n", th->GetClass()->TypeName.GetChars(), th->X(), th->Y(), th->Z());
						SetResult(thing_other, in->Frac, th, thingpitch);
					}
				}
				else
				{
					if (aimdebug)
						Printf("Hit target %s at %f,%f,%f\n", th->GetClass()->TypeName.GetChars(), th->X(), th->Y(), th->Z());
					SetResult(linetarget, in->Frac, th, thingpitch);
					return;
				}
			}
			else
			{
				if (aimdebug)
					Printf("Hit target %s at %f,%f,%f\n", th->GetClass()->TypeName.GetChars(), th->X(), th->Y(), th->Z());
				SetResult(linetarget, in->Frac, th, thingpitch);
				return;
			}
		}
	}
};

//============================================================================
//
// P_AimLineAttack
//
//============================================================================

DAngle P_AimLineAttack(AActor *t1, DAngle angle, double distance, FTranslatedLineTarget *pLineTarget, DAngle vrange,
	int flags, AActor *target, AActor *friender)
{
	double shootz = t1->Center() - t1->Floorclip;
	if (t1->player != NULL)
	{
		shootz += t1->player->mo->AttackZOffset * t1->player->crouchfactor;
	}
	else
	{
		shootz += 8;
	}

	// can't shoot outside view angles
	if (vrange == 0)
	{
		if (t1->player == NULL || !level.IsFreelookAllowed())
		{
			vrange = 35.;
		}
		else
		{
			// [BB] Disable autoaim on weapons with WIF_NOAUTOAIM.
			AWeapon *weapon = t1->player->ReadyWeapon;
			if (weapon && (weapon->WeaponFlags & WIF_NOAUTOAIM))
			{
				vrange = 0.5;
			}
			else
			{
				// 35 degrees is approximately what Doom used. You cannot have a
				// vrange of 0 degrees, because then toppitch and bottompitch will
				// be equal, and PTR_AimTraverse will never find anything to shoot at
				// if it crosses a line.
				vrange = clamp(t1->player->userinfo.GetAimDist(), 0.5, 35.);
			}
		}
	}

	aim_t aim;

	aim.flags = flags;
	aim.shootthing = t1;
	aim.friender = (friender == NULL) ? t1 : friender;
	aim.aimdir = aim_t::aim_up | aim_t::aim_down;
	aim.startpos = t1->Pos();
	aim.aimtrace = angle.ToVector(distance);
	aim.limitz = aim.shootz = shootz;
	aim.toppitch = t1->Angles.Pitch - vrange;
	aim.bottompitch = t1->Angles.Pitch + vrange;
	aim.attackrange = distance;
	aim.aimpitch = t1->Angles.Pitch;
	aim.lastsector = t1->Sector;
	aim.startfrac = 0;
	aim.unlinked = false;
	aim.aimtarget = target;

	aim.AimTraverse();

	AimTarget *result = aim.Result();

	if (pLineTarget)
	{
		*pLineTarget = *result;
	}
	return result->linetarget ? result->pitch : t1->Angles.Pitch;
}


//==========================================================================
//
// Helper stuff for P_LineAttack
//
//==========================================================================

struct Origin
{
	AActor *Caller;
	bool hitGhosts;
	bool hitSameSpecies;
};

static ETraceStatus CheckForActor(FTraceResults &res, void *userdata)
{
	if (res.HitType != TRACE_HitActor)
	{
		return TRACE_Stop;
	}

	Origin *data = (Origin *)userdata;

	// check for physical attacks on spectrals
	if (res.Actor->flags4 & MF4_SPECTRAL)
	{
		return TRACE_Skip;
	}

	if (data->hitSameSpecies && res.Actor->GetSpecies() == data->Caller->GetSpecies()) 
	{
		return TRACE_Skip;
	}
	if (data->hitGhosts && res.Actor->flags3 & MF3_GHOST)
	{
		return TRACE_Skip;
	}

	return TRACE_Stop;
}

//==========================================================================
//
// P_LineAttack
//
// if damage == 0, it is just a test trace that will leave linetarget set
//
//==========================================================================

AActor *P_LineAttack(AActor *t1, DAngle angle, double distance,
	DAngle pitch, int damage, FName damageType, PClassActor *pufftype, int flags, FTranslatedLineTarget*victim, int *actualdamage)
{
	DVector3 direction;
	double shootz;
	FTraceResults trace;
	Origin TData;
	TData.Caller = t1;
	bool killPuff = false;
	AActor *puff = NULL;
	int pflag = 0;
	int puffFlags = (flags & LAF_ISMELEEATTACK) ? PF_MELEERANGE : 0;
	if (flags & LAF_NORANDOMPUFFZ)
		puffFlags |= PF_NORANDOMZ;

	if (victim != NULL)
	{
		memset(victim, 0, sizeof(*victim));
	}
	if (actualdamage != NULL)
	{
		*actualdamage = 0;
	}

	double pc = pitch.Cos();

	direction = { pc * angle.Cos(), pc * angle.Sin(), -pitch.Sin() };
	shootz = t1->Center() - t1->Floorclip;

	if (t1->player != NULL)
	{
		shootz += t1->player->mo->AttackZOffset * t1->player->crouchfactor;
		if (damageType == NAME_Melee || damageType == NAME_Hitscan)
		{
			// this is coming from a weapon attack function which needs to transfer information to the obituary code,
			// We need to preserve this info from the damage type because the actual damage type can get overridden by the puff
			pflag = DMG_PLAYERATTACK;
		}
	}
	else
	{
		shootz += 8;
	}

	// We need to check the defaults of the replacement here
	AActor *puffDefaults = GetDefaultByType(pufftype->GetReplacement());

	TData.hitGhosts = (t1->player != NULL &&
		t1->player->ReadyWeapon != NULL &&
		(t1->player->ReadyWeapon->flags2 & MF2_THRUGHOST)) ||
		(puffDefaults && (puffDefaults->flags2 & MF2_THRUGHOST));

	TData.hitSameSpecies = (puffDefaults && (puffDefaults->flags6 & MF6_MTHRUSPECIES));

	// if the puff uses a non-standard damage type, this will override default, hitscan and melee damage type.
	// All other explicitly passed damage types (currenty only MDK) will be preserved.
	if ((damageType == NAME_None || damageType == NAME_Melee || damageType == NAME_Hitscan) &&
		puffDefaults != NULL && puffDefaults->DamageType != NAME_None)
	{
		damageType = puffDefaults->DamageType;
	}

	int tflags;
	if (puffDefaults != NULL && puffDefaults->flags6 & MF6_NOTRIGGER) tflags = TRACE_NoSky;
	else tflags = TRACE_NoSky | TRACE_Impact;

	if (!Trace(t1->PosAtZ(shootz), t1->Sector, direction, distance, MF_SHOOTABLE, 
		ML_BLOCKEVERYTHING | ML_BLOCKHITSCAN, t1, trace, tflags, CheckForActor, &TData))
	{ // hit nothing
		if (puffDefaults == NULL)
		{
		}
		else if (puffDefaults->ActiveSound)
		{ // Play miss sound
			S_Sound(t1, CHAN_WEAPON, puffDefaults->ActiveSound, 1, ATTN_NORM);
		}
		if (puffDefaults != NULL && puffDefaults->flags3 & MF3_ALWAYSPUFF)
		{ // Spawn the puff anyway
			puff = P_SpawnPuff(t1, pufftype, trace.HitPos, trace.SrcAngleFromTarget, trace.SrcAngleFromTarget, 2, puffFlags);
		}
		else
		{
			return NULL;
		}
	}
	else
	{
		if (trace.HitType != TRACE_HitActor)
		{
			// position a bit closer for puffs
			if (trace.HitType != TRACE_HitWall || trace.Line->special != Line_Horizon)
			{
				DVector2 pos = P_GetOffsetPosition(trace.HitPos.X, trace.HitPos.Y, -trace.HitVector.X * 4, -trace.HitVector.Y * 4);
				puff = P_SpawnPuff(t1, pufftype, DVector3(pos, trace.HitPos.Z - trace.HitVector.Z * 4), trace.SrcAngleFromTarget,
					trace.SrcAngleFromTarget - 90, 0, puffFlags);
				puff->radius = 1/65536.;
			}

			// [RH] Spawn a decal
			if (trace.HitType == TRACE_HitWall && trace.Line->special != Line_Horizon && !trace.Line->isVisualPortal() && !(flags & LAF_NOIMPACTDECAL) && !(puffDefaults->flags7 & MF7_NODECAL))
			{
				// [TN] If the actor or weapon has a decal defined, use that one.
				if (t1->DecalGenerator != NULL ||
					(t1->player != NULL && t1->player->ReadyWeapon != NULL && t1->player->ReadyWeapon->DecalGenerator != NULL))
				{
					// [ZK] If puff has FORCEDECAL set, do not use the weapon's decal
					if (puffDefaults->flags7 & MF7_FORCEDECAL && puff != NULL && puff->DecalGenerator)
						SpawnShootDecal(puff, trace);
					else
						SpawnShootDecal(t1, trace);
				}

				// Else, look if the bulletpuff has a decal defined.
				else if (puff != NULL && puff->DecalGenerator)
				{
					SpawnShootDecal(puff, trace);
				}

				else
				{
					SpawnShootDecal(t1, trace);
				}
			}
			else if (puff != NULL &&
				trace.CrossedWater == NULL &&
				trace.Sector->heightsec == NULL &&
				trace.HitType == TRACE_HitFloor)
			{
				P_HitWater(puff, trace.Sector, trace.HitPos);
			}
		}
		else
		{
			bool bloodsplatter = (t1->flags5 & MF5_BLOODSPLATTER) ||
				(t1->player != NULL &&	t1->player->ReadyWeapon != NULL &&
				(t1->player->ReadyWeapon->WeaponFlags & WIF_AXEBLOOD));

			bool axeBlood = (t1->player != NULL &&
				t1->player->ReadyWeapon != NULL &&
				(t1->player->ReadyWeapon->WeaponFlags & WIF_AXEBLOOD));

			// Hit a thing, so it could be either a puff or blood
			DVector3 bleedpos = trace.HitPos;
			// position a bit closer for puffs/blood if using compatibility mode.
			if (i_compatflags & COMPATF_HITSCAN)
			{
				DVector2 ofs = P_GetOffsetPosition(bleedpos.X, bleedpos.Y, -10 * trace.HitVector.X, -10 * trace.HitVector.Y);
				bleedpos.X = ofs.X;
				bleedpos.Y = ofs.Y;
				bleedpos.Z -= -10 * trace.HitVector.Z;
			}

			// Spawn bullet puffs or blood spots, depending on target type.
			if ((puffDefaults != NULL && puffDefaults->flags3 & MF3_PUFFONACTORS) ||
				(trace.Actor->flags & MF_NOBLOOD) ||
				(trace.Actor->flags2 & (MF2_INVULNERABLE | MF2_DORMANT)))
			{
				if (!(trace.Actor->flags & MF_NOBLOOD))
					puffFlags |= PF_HITTHINGBLEED;

				// We must pass the unreplaced puff type here 
				puff = P_SpawnPuff(t1, pufftype, bleedpos, trace.SrcAngleFromTarget, trace.SrcAngleFromTarget - 90, 2, puffFlags | PF_HITTHING, trace.Actor);
			}

			// Allow puffs to inflict poison damage, so that hitscans can poison, too.
			if (puffDefaults != NULL && puffDefaults->PoisonDamage > 0 && puffDefaults->PoisonDuration != INT_MIN)
			{
				P_PoisonMobj(trace.Actor, puff ? puff : t1, t1, puffDefaults->PoisonDamage, puffDefaults->PoisonDuration, puffDefaults->PoisonPeriod, puffDefaults->PoisonDamageType);
			}

			// [GZ] If MF6_FORCEPAIN is set, we need to call P_DamageMobj even if damage is 0!
			// Note: The puff may not yet be spawned here so we must check the class defaults, not the actor.
			int newdam = damage;
			if (damage || (puffDefaults != NULL && ((puffDefaults->flags6 & MF6_FORCEPAIN) || (puffDefaults->flags7 & MF7_CAUSEPAIN))))
			{
				int dmgflags = DMG_INFLICTOR_IS_PUFF | pflag;
				// Allow MF5_PIERCEARMOR on a weapon as well.
				if (t1->player != NULL && (dmgflags & DMG_PLAYERATTACK) && t1->player->ReadyWeapon != NULL &&
					t1->player->ReadyWeapon->flags5 & MF5_PIERCEARMOR)
				{
					dmgflags |= DMG_NO_ARMOR;
				}
				
				if (puff == NULL)
				{
					// Since the puff is the damage inflictor we need it here 
					// regardless of whether it is displayed or not.
					puff = P_SpawnPuff(t1, pufftype, bleedpos, 0., 0., 2, puffFlags | PF_HITTHING | PF_TEMPORARY);
					killPuff = true;
				}
				newdam = P_DamageMobj(trace.Actor, puff ? puff : t1, t1, damage, damageType, dmgflags|DMG_USEANGLE, trace.SrcAngleFromTarget);
				if (actualdamage != NULL)
				{
					*actualdamage = newdam;
				}
			}
			if (!(puffDefaults != NULL && puffDefaults->flags3&MF3_BLOODLESSIMPACT))
			{
				if (!bloodsplatter && !axeBlood &&
					!(trace.Actor->flags & MF_NOBLOOD) &&
					!(trace.Actor->flags2 & (MF2_INVULNERABLE | MF2_DORMANT)))
				{
					P_SpawnBlood(bleedpos, trace.SrcAngleFromTarget, newdam > 0 ? newdam : damage, trace.Actor);
				}

				if (damage)
				{
					if (bloodsplatter || axeBlood)
					{
						if (!(trace.Actor->flags&MF_NOBLOOD) &&
							!(trace.Actor->flags2&(MF2_INVULNERABLE | MF2_DORMANT)))
						{
							if (axeBlood)
							{
								P_BloodSplatter2(bleedpos, trace.Actor, trace.SrcAngleFromTarget);
							}
							if (pr_lineattack() < 192)
							{
								P_BloodSplatter(bleedpos, trace.Actor, trace.SrcAngleFromTarget);
							}
						}
					}
					// [RH] Stick blood to walls
					P_TraceBleed(newdam > 0 ? newdam : damage, trace.HitPos, trace.Actor, trace.SrcAngleFromTarget, pitch);
				}
			}
			if (victim != NULL)
			{
				victim->linetarget = trace.Actor;
				victim->angleFromSource = trace.SrcAngleFromTarget;
				victim->unlinked = trace.unlinked;
			}
		}
		if (trace.Crossed3DWater || trace.CrossedWater)
		{
			if (puff == NULL)
			{ // Spawn puff just to get a mass for the splash
				puff = P_SpawnPuff(t1, pufftype, trace.HitPos, 0., 0., 2, puffFlags | PF_HITTHING | PF_TEMPORARY);
				killPuff = true;
			}
			SpawnDeepSplash(t1, trace, puff);
		}
	}
	if (killPuff && puff != NULL)
	{
		puff->Destroy();
		puff = NULL;
	}
	return puff;
}

AActor *P_LineAttack(AActor *t1, DAngle angle, double distance,
	DAngle pitch, int damage, FName damageType, FName pufftype, int flags, FTranslatedLineTarget *victim, int *actualdamage)
{
	PClassActor *type = PClass::FindActor(pufftype);
	if (type == NULL)
	{
		if (victim != NULL)
		{
			memset(victim, 0, sizeof(*victim));
		}
		Printf("Attempt to spawn unknown actor type '%s'\n", pufftype.GetChars());
		return NULL;
	}
	else
	{
		return P_LineAttack(t1, angle, distance, pitch, damage, damageType, type, flags, victim, actualdamage);
	}
}

//==========================================================================
//
// P_LinePickActor
//
//==========================================================================

AActor *P_LinePickActor(AActor *t1, DAngle angle, double distance, DAngle pitch, ActorFlags actorMask, DWORD wallMask) 
{
	DVector3 direction;
	double shootz;

	double pc = pitch.Cos();
	direction = { pc * angle.Cos(), pc * angle.Sin(), -pitch.Sin() };
	shootz = t1->Center() - t1->Floorclip;

	if (t1->player != NULL)
	{
		shootz += t1->player->mo->AttackZOffset * t1->player->crouchfactor;
	}
	else
	{
		shootz += 8;
	}

	FTraceResults trace;
	Origin TData;
	
	TData.Caller = t1;
	TData.hitGhosts = true;
	
	if (Trace(t1->PosAtZ(shootz), t1->Sector, direction, distance,
		actorMask, wallMask, t1, trace, TRACE_NoSky | TRACE_PortalRestrict, CheckForActor, &TData))
	{
		if (trace.HitType == TRACE_HitActor)
		{
			return trace.Actor;
		}
	}

	return NULL;
}

//==========================================================================
//
//
//
//==========================================================================

void P_TraceBleed(int damage, const DVector3 &pos, AActor *actor, DAngle angle, DAngle pitch)
{
	if (!cl_bloodsplats)
		return;

	const char *bloodType = "BloodSplat";
	int count;
	double noise;


	if ((actor->flags & MF_NOBLOOD) ||
		(actor->flags5 & MF5_NOBLOODDECALS) ||
		(actor->flags2 & (MF2_INVULNERABLE | MF2_DORMANT)) ||
		(actor->player && actor->player->cheats & CF_GODMODE))
	{
		return;
	}

	if (damage < 15)
	{	// For low damages, there is a chance to not spray blood at all
		if (damage <= 10)
		{
			if (pr_tracebleed() < 160)
			{
				return;
			}
		}
		count = 1;
		noise = 11.25 / 256.;
	}
	else if (damage < 25)
	{
		count = 2;
		noise = 22.5 / 256.;
	}
	else
	{	// For high damages, there is a chance to spray just one big glob of blood
		if (pr_tracebleed() < 24)
		{
			bloodType = "BloodSmear";
			count = 1;
			noise = 45. / 256.;
		}
		else
		{
			count = 3;
			noise = 45. / 256.;
		}
	}

	for (; count; --count)
	{
		FTraceResults bleedtrace;

		DAngle bleedang = angle + (pr_tracebleed() - 128) * noise;
		DAngle bleedpitch = pitch + (pr_tracebleed() - 128) * noise;
		double cosp = bleedpitch.Cos();
		DVector3 vdir = DVector3(cosp * bleedang.Cos(), cosp * bleedang.Sin(), -bleedpitch.Sin());

		if (Trace(pos, actor->Sector, vdir, 172 * FRACUNIT, 0, ML_BLOCKEVERYTHING, actor, bleedtrace, TRACE_NoSky))
		{
			if (bleedtrace.HitType == TRACE_HitWall)
			{
				PalEntry bloodcolor = actor->GetBloodColor();
				if (bloodcolor != 0)
				{
					bloodcolor.r >>= 1;	// the full color is too bright for blood decals
					bloodcolor.g >>= 1;
					bloodcolor.b >>= 1;
					bloodcolor.a = 1;
				}

				DImpactDecal::StaticCreate(bloodType, bleedtrace.HitPos,
					bleedtrace.Line->sidedef[bleedtrace.Side], bleedtrace.ffloor, bloodcolor);
			}
		}
	}
}

void P_TraceBleed(int damage, AActor *target, DAngle angle, DAngle pitch)
{
	P_TraceBleed(damage, target->PosPlusZ(target->Height/2), target, angle, pitch);
}

//==========================================================================
//
//
//
//==========================================================================

void P_TraceBleed(int damage, AActor *target, AActor *missile)
{
	DAngle pitch;

	if (target == NULL || missile->flags3 & MF3_BLOODLESSIMPACT)
	{
		return;
	}

	if (missile->Vel.Z != 0)
	{
		double aim;

		aim = g_atan(missile->Vel.Z / target->Distance2D(missile));
		pitch = -ToDegrees(aim);
	}
	else
	{
		pitch = 0.;
	}
	P_TraceBleed(damage, target->PosPlusZ(target->Height/2), target, missile->AngleTo(target), pitch);
}

//==========================================================================
//
//
//
//==========================================================================

void P_TraceBleed(int damage, FTranslatedLineTarget *t, AActor *puff)
{
	if (t->linetarget == NULL || puff->flags3 & MF3_BLOODLESSIMPACT)
	{
		return;
	}

	DAngle pitch = (pr_tracebleed() - 128) * (360 / 65536.);
	P_TraceBleed(damage, t->linetarget->PosPlusZ(t->linetarget->Height/2), t->linetarget, t->angleFromSource, pitch);
}

//==========================================================================
//
//
//
//==========================================================================

void P_TraceBleed(int damage, AActor *target)
{
	if (target != NULL)
	{
		DAngle angle = pr_tracebleed() * (360 / 256.);
		DAngle pitch = (pr_tracebleed() - 128) * (360 / 65536.);
		P_TraceBleed(damage, target->PosPlusZ(target->Height / 2), target, angle, pitch);
	}
}

//==========================================================================
//
// [RH] Rail gun stuffage
//
//==========================================================================

struct SRailHit
{
	AActor *HitActor;
	DVector3 HitPos;
	DAngle HitAngle;
};
struct RailData
{
	AActor *Caller;
	TArray<SRailHit> RailHits;
	bool StopAtOne;
	bool StopAtInvul;
	bool ThruSpecies;
};

static ETraceStatus ProcessRailHit(FTraceResults &res, void *userdata)
{
	RailData *data = (RailData *)userdata;
	if (res.HitType != TRACE_HitActor)
	{
		return TRACE_Stop;
	}

	// Invulnerable things completely block the shot
	if (data->StopAtInvul && res.Actor->flags2 & MF2_INVULNERABLE)
	{
		return TRACE_Stop;
	}

	// Skip actors with the same species if the puff has MTHRUSPECIES.
	if (data->ThruSpecies && res.Actor->GetSpecies() == data->Caller->GetSpecies())
	{
		return TRACE_Skip;
	}

	// Save this thing for damaging later, and continue the trace
	SRailHit newhit;
	newhit.HitActor = res.Actor;
	newhit.HitPos = res.HitPos;
	newhit.HitAngle = res.SrcAngleFromTarget;
	if (i_compatflags & COMPATF_HITSCAN)
	{
		DVector2 ofs = P_GetOffsetPosition(newhit.HitPos.X, newhit.HitPos.Y, -10 * res.HitVector.X, -10 * res.HitVector.Y);
		newhit.HitPos.X = ofs.X;
		newhit.HitPos.Y = ofs.Y;
		newhit.HitPos.Z -= -10 * res.HitVector.Z;
	}
	data->RailHits.Push(newhit);

	return data->StopAtOne ? TRACE_Stop : TRACE_Continue;
}

//==========================================================================
//
//
//
//==========================================================================
void P_RailAttack(FRailParams *p)
{
	DVector3 start;
	FTraceResults trace;

	PClassActor *puffclass = p->puff;
	if (puffclass == NULL)
	{
		puffclass = PClass::FindActor(NAME_BulletPuff);
	}

	AActor *source = p->source;
	DAngle pitch = -source->Angles.Pitch + p->pitchoffset;
	DAngle angle = source->Angles.Yaw + p->angleoffset;

	DVector3 vec(DRotator(pitch, angle, angle));
	double shootz = source->Center() - source->FloatSpeed + p->offset_z;

	if (!(p->flags & RAF_CENTERZ))
	{
		if (source->player != NULL)
		{
			shootz += source->player->mo->AttackZOffset * source->player->crouchfactor;
		}
		else
		{
			shootz += 8;
		}
	}

	DVector2 xy = source->Vec2Angle(p->offset_xy, angle - 90.);

	RailData rail_data;
	rail_data.Caller = source;
	
	rail_data.StopAtOne = !!(p->flags & RAF_NOPIERCE);
	start.X = xy.X;
	start.Y = xy.Y;
	start.Z = shootz;

	int flags;

	assert(puffclass != NULL);		// Because we set it to a default above
	AActor *puffDefaults = GetDefaultByType(puffclass->GetReplacement()); //Contains all the flags such as FOILINVUL, etc.

	flags = (puffDefaults->flags6 & MF6_NOTRIGGER) ? 0 : TRACE_PCross | TRACE_Impact;
	rail_data.StopAtInvul = (puffDefaults->flags3 & MF3_FOILINVUL) ? false : true;
	rail_data.ThruSpecies = (puffDefaults->flags6 & MF6_MTHRUSPECIES) ? true : false;
	Trace(start, source->Sector, vec, p->distance, MF_SHOOTABLE, ML_BLOCKEVERYTHING, source, trace,	flags, ProcessRailHit, &rail_data);

	// Hurt anything the trace hit
	unsigned int i;
	FName damagetype = (puffDefaults == NULL || puffDefaults->DamageType == NAME_None) ? FName(NAME_Railgun) : puffDefaults->DamageType;

	// used as damage inflictor
	AActor *thepuff = NULL;

	if (puffclass != NULL) thepuff = Spawn(puffclass, source->Pos(), ALLOW_REPLACE);

	for (i = 0; i < rail_data.RailHits.Size(); i++)
	{
		bool spawnpuff;
		bool bleed = false;

		int puffflags = PF_HITTHING;
		AActor *hitactor = rail_data.RailHits[i].HitActor;
		DVector3 &hitpos = rail_data.RailHits[i].HitPos;
		DAngle hitangle = rail_data.RailHits[i].HitAngle;

		if ((hitactor->flags & MF_NOBLOOD) ||
			(hitactor->flags2 & MF2_DORMANT || ((hitactor->flags2 & MF2_INVULNERABLE) && !(puffDefaults->flags3 & MF3_FOILINVUL))))
		{
			spawnpuff = (puffclass != NULL);
		}
		else
		{
			spawnpuff = (puffclass != NULL && puffDefaults->flags3 & MF3_ALWAYSPUFF);
			puffflags |= PF_HITTHINGBLEED; // [XA] Allow for puffs to jump to XDeath state.
			if (!(puffDefaults->flags3 & MF3_BLOODLESSIMPACT))
			{
				bleed = true;
			}
		}
		if (spawnpuff)
		{
			P_SpawnPuff(source, puffclass, hitpos, hitangle, hitangle - 90, 1, puffflags, hitactor);
		}
		
		int dmgFlagPass = DMG_INFLICTOR_IS_PUFF;
		if (puffDefaults != NULL)	// is this even possible?
		{
			if (puffDefaults->PoisonDamage > 0 && puffDefaults->PoisonDuration != INT_MIN)
			{
				P_PoisonMobj(hitactor, thepuff ? thepuff : source, source, puffDefaults->PoisonDamage, puffDefaults->PoisonDuration, puffDefaults->PoisonPeriod, puffDefaults->PoisonDamageType);
			}
			if (puffDefaults->flags3 & MF3_FOILINVUL) dmgFlagPass |= DMG_FOILINVUL;
			if (puffDefaults->flags7 & MF7_FOILBUDDHA) dmgFlagPass |= DMG_FOILBUDDHA;
		}
		int newdam = P_DamageMobj(hitactor, thepuff ? thepuff : source, source, p->damage, damagetype, dmgFlagPass|DMG_USEANGLE, hitangle);

		if (bleed)
		{
			P_SpawnBlood(hitpos, hitangle, newdam > 0 ? newdam : p->damage, hitactor);
			P_TraceBleed(newdam > 0 ? newdam : p->damage, hitpos, hitactor, hitangle, ANGLE2DBL(pitch));
		}
	}

	// Spawn a decal or puff at the point where the trace ended.
	if (trace.HitType == TRACE_HitWall)
	{
		AActor* puff = NULL;

		if (puffclass != NULL && puffDefaults->flags3 & MF3_ALWAYSPUFF)
		{
			puff = P_SpawnPuff(source, puffclass, trace.HitPos, trace.SrcAngleFromTarget, trace.SrcAngleFromTarget - 90, 1, 0);
			if (puff && (trace.Line != NULL) && (trace.Line->special == Line_Horizon) && !(puff->flags3 & MF3_SKYEXPLODE))
				puff->Destroy();
		}
		if (puff != NULL && puffDefaults->flags7 & MF7_FORCEDECAL && puff->DecalGenerator)
			SpawnShootDecal(puff, trace);
		else
			SpawnShootDecal(source, trace);

	}
	if (trace.HitType == TRACE_HitFloor || trace.HitType == TRACE_HitCeiling)
	{
		AActor* puff = NULL;
		if (puffclass != NULL && puffDefaults->flags3 & MF3_ALWAYSPUFF)
		{
			puff = P_SpawnPuff(source, puffclass, trace.HitPos, trace.SrcAngleFromTarget, trace.SrcAngleFromTarget - 90, 1, 0);
			if (puff && !(puff->flags3 & MF3_SKYEXPLODE) &&
				(((trace.HitType == TRACE_HitFloor) && (puff->floorpic == skyflatnum)) ||
				((trace.HitType == TRACE_HitCeiling) && (puff->ceilingpic == skyflatnum))))
			{
				puff->Destroy();
			}
		}
	}
	if (thepuff != NULL)
	{
		if (trace.Crossed3DWater || trace.CrossedWater)
		{
			SpawnDeepSplash(source, trace, thepuff);
		}
		else if (trace.HitType == TRACE_HitFloor && trace.Sector->heightsec == NULL)
		{
			P_HitWater(thepuff, trace.Sector, trace.HitPos);
		}
		thepuff->Destroy();
	}

	// Draw the slug's trail.
	P_DrawRailTrail(source, start, trace.HitPos, p->color1, p->color2, p->maxdiff, p->flags, p->spawnclass, angle, p->duration, p->sparsity, p->drift, p->SpiralOffset);
}

//==========================================================================
//
// [RH] P_AimCamera
//
//==========================================================================

CVAR(Float, chase_height, -8.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, chase_dist, 90.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

void P_AimCamera(AActor *t1, DVector3 &campos, sector_t *&CameraSector, bool &unlinked)
{
	double distance = clamp<double>(chase_dist, 0, 30000);
	DAngle angle = t1->Angles.Yaw - 180;
	DAngle pitch = t1->Angles.Pitch;
	FTraceResults trace;
	DVector3 vvec;
	double sz;

	double pc = pitch.Cos();

	vvec = { pc * angle.Cos(), pc * angle.Sin(), -pitch.Sin() };
	sz = t1->Top() - t1->Floorclip + clamp<double>(chase_height, -1000, 1000);

	if (Trace(t1->PosAtZ(sz), t1->Sector, vvec, distance, 0, 0, NULL, trace) &&
		trace.Distance > 10)
	{
		// Position camera slightly in front of hit thing
		campos = t1->PosAtZ(sz) + vvec *(trace.Distance - 5);
	}
	else
	{
		campos = trace.HitPos;
	}
	CameraSector = trace.Sector;
	unlinked = trace.unlinked;
}


//==========================================================================
//
// P_TalkFacing
//
// Looks for something within 5.625 degrees left or right of the player
// to talk to.
//
//==========================================================================

bool P_TalkFacing(AActor *player)
{
	static const double angleofs[] = { 0, 90./16, -90./16 };
	FTranslatedLineTarget t;

	for (double angle : angleofs)
	{
		P_AimLineAttack(player, player->Angles.Yaw + angle, TALKRANGE, &t, 35., ALF_FORCENOSMART | ALF_CHECKCONVERSATION | ALF_PORTALRESTRICT);
		if (t.linetarget != NULL)
		{
			if (t.linetarget->health > 0 && // Dead things can't talk.
				!(t.linetarget->flags4 & MF4_INCOMBAT) && // Fighting things don't talk either.
				t.linetarget->Conversation != NULL)
			{
				// Give the NPC a chance to play a brief animation
				t.linetarget->ConversationAnimation(0);
				P_StartConversation(t.linetarget, player, true, true);
				return true;
			}
			return false;
		}
	}
	return false;
}

//==========================================================================
//
// USE LINES
//
//==========================================================================

bool P_UseTraverse(AActor *usething, const DVector2 &start, const DVector2 &end, bool &foundline)
{
	FPathTraverse it(start.X, start.Y, end.X, end.Y, PT_ADDLINES | PT_ADDTHINGS);
	intercept_t *in;
	DVector3 xpos = { start.X, start.Y, usething->Z() };

	while ((in = it.Next()))
	{
		// [RH] Check for things to talk with or use a puzzle item on
		if (!in->isaline)
		{
			if (usething == in->d.thing)
				continue;
			// Check thing

			// Check for puzzle item use or USESPECIAL flag
			// Extended to use the same activationtype mechanism as BUMPSPECIAL does
			if (in->d.thing->flags5 & MF5_USESPECIAL || in->d.thing->special == UsePuzzleItem)
			{
				if (P_ActivateThingSpecial(in->d.thing, usething))
					return true;
			}
			continue;
		}

		if (it.PortalRelocate(in, PT_ADDLINES | PT_ADDTHINGS, &xpos))
		{
			continue;
		}

		FLineOpening open;
		if (in->d.line->special == 0 || !(in->d.line->activation & (SPAC_Use | SPAC_UseThrough | SPAC_UseBack)))
		{
		blocked:
			if (in->d.line->flags & (ML_BLOCKEVERYTHING | ML_BLOCKUSE))
			{
				open.range = 0;
			}
			else
			{
				P_LineOpening(open, NULL, in->d.line, it.InterceptPoint(in));
			}
			if (open.range <= 0 ||
				(in->d.line->special != 0 && (i_compatflags & COMPATF_USEBLOCKING)))
			{
				// [RH] Give sector a chance to intercept the use

				sector_t * sec;

				sec = usething->Sector;

				if (sec->SecActTarget && sec->SecActTarget->TriggerAction(usething, SECSPAC_Use))
				{
					return true;
				}

				sec = P_PointOnLineSide(xpos, in->d.line) == 0 ?
					in->d.line->frontsector : in->d.line->backsector;

				if (sec != NULL && sec->SecActTarget &&
					sec->SecActTarget->TriggerAction(usething, SECSPAC_UseWall))
				{
					return true;
				}

				if (usething->player)
				{
					S_Sound(usething, CHAN_VOICE, "*usefail", 1, ATTN_IDLE);
				}
				return true;		// can't use through a wall
			}
			foundline = true;
			continue;			// not a special line, but keep checking
		}

		if (P_PointOnLineSide(xpos, in->d.line) == 1)
		{
			if (!(in->d.line->activation & SPAC_UseBack))
			{
				// [RH] continue traversal for two-sided lines
				//return in->d.line->backsector != NULL;		// don't use back side
				goto blocked;	// do a proper check for back sides of triggers
			}
			else
			{
				P_ActivateLine(in->d.line, usething, 1, SPAC_UseBack, &xpos);
				return true;
			}
		}
		else
		{
			if ((in->d.line->activation & (SPAC_Use | SPAC_UseThrough | SPAC_UseBack)) == SPAC_UseBack)
			{
				goto blocked; // Line cannot be used from front side so treat it as a non-trigger line
			}

			P_ActivateLine(in->d.line, usething, 0, SPAC_Use, &xpos);

			//WAS can't use more than one special line in a row
			//jff 3/21/98 NOW multiple use allowed with enabling line flag
			//[RH] And now I've changed it again. If the line is of type
			//	   SPAC_USE, then it eats the use. Everything else passes
			//	   it through, including SPAC_USETHROUGH.
			if (i_compatflags & COMPATF_USEBLOCKING)
			{
				if (in->d.line->activation & SPAC_UseThrough) continue;
				else return true;
			}
			else
			{
				if (!(in->d.line->activation & SPAC_Use)) continue;
				else return true;
			}

		}

	}
	return false;
}

//==========================================================================
//
// Returns false if a "oof" sound should be made because of a blocking
// linedef. Makes 2s middles which are impassable, as well as 2s uppers
// and lowers which block the player, cause the sound effect when the
// player tries to activate them. Specials are excluded, although it is
// assumed that all special linedefs within reach have been considered
// and rejected already (see P_UseLines).
//
// by Lee Killough
//
//==========================================================================

bool P_NoWayTraverse(AActor *usething, const DVector2 &start, const DVector2 &end)
{
	FPathTraverse it(start.X, start.Y, end.X, end.Y, PT_ADDLINES);
	intercept_t *in;

	while ((in = it.Next()))
	{
		line_t *ld = in->d.line;
		FLineOpening open;

		// [GrafZahl] de-obfuscated. Was I the only one who was unable to make sense out of
		// this convoluted mess?
		if (ld->special) continue;
		if (ld->isLinePortal()) return false;
		if (ld->flags&(ML_BLOCKING | ML_BLOCKEVERYTHING | ML_BLOCK_PLAYERS)) return true;
		P_LineOpening(open, NULL, ld, it.InterceptPoint(in));
		if (open.range <= 0 ||
			open.bottom > usething->Z() + usething->MaxStepHeight ||
			open.top < usething->Top()) return true;
	}
	return false;
}

//==========================================================================
//
// P_UseLines
//
// Looks for special lines in front of the player to activate
//
//==========================================================================

void P_UseLines(player_t *player)
{
	bool foundline = false;

	// If the player is transitioning a portal, use the group that is at its vertical center.
	DVector2 start = player->mo->GetPortalTransition(player->mo->Height / 2);
	// [NS] Now queries the Player's UseRange.
	DVector2 end = start + player->mo->Angles.Yaw.ToVector(player->mo->UseRange);

	// old code:
	// This added test makes the "oof" sound work on 2s lines -- killough:

	if (!P_UseTraverse(player->mo, start, end, foundline))
	{ // [RH] Give sector a chance to eat the use
		sector_t *sec = player->mo->Sector;
		int spac = SECSPAC_Use;
		if (foundline) spac |= SECSPAC_UseWall;
		if ((!sec->SecActTarget || !sec->SecActTarget->TriggerAction(player->mo, spac)) &&
			P_NoWayTraverse(player->mo, start, end))
		{
			S_Sound(player->mo, CHAN_VOICE, "*usefail", 1, ATTN_IDLE);
		}
	}
}

//==========================================================================
//
// P_UsePuzzleItem
//
// Returns true if the puzzle item was used on a line or a thing.
//
//==========================================================================

bool P_UsePuzzleItem(AActor *PuzzleItemUser, int PuzzleItemType)
{
	DVector2 start;
	DVector2 end;
	double usedist;

	// [NS] If it's a Player, get their UseRange.
	if (PuzzleItemUser->player)
		usedist = PuzzleItemUser->player->mo->UseRange;
	else
		usedist = USERANGE;

	start = PuzzleItemUser->GetPortalTransition(PuzzleItemUser->Height / 2);
	end = PuzzleItemUser->Angles.Yaw.ToVector(usedist);

	FPathTraverse it(start.X, start.Y, end.X, end.Y, PT_ADDLINES | PT_ADDTHINGS);
	intercept_t *in;

	while ((in = it.Next()))
	{
		AActor *mobj;
		FLineOpening open;

		if (in->isaline)
		{ // Check line
			if (in->d.line->special != UsePuzzleItem)
			{
				P_LineOpening(open, NULL, in->d.line, it.InterceptPoint(in));
				if (open.range <= 0)
				{
					return false; // can't use through a wall
				}
				continue;
			}
			if (P_PointOnLineSide(PuzzleItemUser->Pos(), in->d.line) == 1)
			{ // Don't use back sides
				return false;
			}
			if (PuzzleItemType != in->d.line->args[0])
			{ // Item type doesn't match
				return false;
			}
			int args[3] = { in->d.line->args[2], in->d.line->args[3], in->d.line->args[4] };
			P_StartScript(PuzzleItemUser, in->d.line, in->d.line->args[1], NULL, args, 3, ACS_ALWAYS);
			in->d.line->special = 0;
			return true;
		}
		// Check thing
		mobj = in->d.thing;
		if (mobj->special != UsePuzzleItem)
		{ // Wrong special
			continue;
		}
		if (PuzzleItemType != mobj->args[0])
		{ // Item type doesn't match
			continue;
		}
		int args[3] = { mobj->args[2], mobj->args[3], mobj->args[4] };
		P_StartScript(PuzzleItemUser, NULL, mobj->args[1], NULL, args, 3, ACS_ALWAYS);
		mobj->special = 0;
		return true;
	}
	return false;
}

//==========================================================================
//
// RADIUS ATTACK
//
//
//==========================================================================


// [RH] Damage scale to apply to thing that shot the missile.
static float selfthrustscale;

CUSTOM_CVAR(Float, splashfactor, 1.f, CVAR_SERVERINFO)
{
	if (self <= 0.f)
		self = 1.f;
	else
		selfthrustscale = 1.f / self;
}

//==========================================================================
//
// P_RadiusAttack
// Source is the creature that caused the explosion at spot.
//
//==========================================================================

void P_RadiusAttack(AActor *bombspot, AActor *bombsource, int bombdamage, int bombdistance, FName bombmod,
	int flags, int fulldamagedistance)
{
	if (bombdistance <= 0)
		return;
	fulldamagedistance = clamp<int>(fulldamagedistance, 0, bombdistance - 1);
	fixed_t bombdistfix = bombdistance << FRACBITS;

	double bombdistancefloat = 1.f / (double)(bombdistance - fulldamagedistance);
	double bombdamagefloat = (double)bombdamage;

	FPortalGroupArray grouplist(FPortalGroupArray::PGA_Full3d);
	FMultiBlockThingsIterator it(grouplist, bombspot->_f_X(), bombspot->_f_Y(), bombspot->_f_Z() - bombdistfix, bombspot->_f_height() + bombdistfix*2, bombdistfix, false, bombspot->Sector);
	FMultiBlockThingsIterator::CheckResult cres;

	if (flags & RADF_SOURCEISSPOT)
	{ // The source is actually the same as the spot, even if that wasn't what we received.
		bombsource = bombspot;
	}

	while ((it.Next(&cres)))
	{
		AActor *thing = cres.thing;
		// Vulnerable actors can be damaged by _f_radius() attacks even if not shootable
		// Used to emulate MBF's vulnerability of non-missile bouncers to explosions.
		if (!((thing->flags & MF_SHOOTABLE) || (thing->flags6 & MF6_VULNERABLE)))
			continue;

		// Boss spider and cyborg and Heretic's ep >= 2 bosses
		// take no damage from concussion.
		if (thing->flags3 & MF3_NORADIUSDMG && !(bombspot->flags4 & MF4_FORCERADIUSDMG))
			continue;

		if (!(flags & RADF_HURTSOURCE) && (thing == bombsource || thing == bombspot))
		{ // don't damage the source of the explosion
			continue;
		}

		// a much needed option: monsters that fire explosive projectiles cannot 
		// be hurt by projectiles fired by a monster of the same type.
		// Controlled by the DONTHARMCLASS and DONTHARMSPECIES flags.
		if ((bombsource && !thing->player) // code common to both checks
			&& ( // Class check first
			((bombsource->flags4 & MF4_DONTHARMCLASS) && (thing->GetClass() == bombsource->GetClass()))
			|| // Nigh-identical species check second
			((bombsource->flags6 & MF6_DONTHARMSPECIES) && (thing->GetSpecies() == bombsource->GetSpecies()))
			)
			)	continue;

		// Barrels always use the original code, since this makes
		// them far too "active." BossBrains also use the old code
		// because some user levels require they have a height of 16,
		// which can make them near impossible to hit with the new code.
		if ((flags & RADF_NODAMAGE) || !((bombspot->flags5 | thing->flags5) & MF5_OLDRADIUSDMG))
		{
			// [RH] New code. The bounding box only covers the
			// height of the thing and not the height of the map.
			double points;
			double len;
			fixed_t dx, dy;
			double boxradius;

			fixedvec2 vec = bombspot->_f_Vec2To(thing);
			dx = abs(vec.x);
			dy = abs(vec.y);
			boxradius = double(thing->_f_radius());

			// The damage pattern is square, not circular.
			len = double(dx > dy ? dx : dy);

			if (bombspot->_f_Z() < thing->_f_Z() || bombspot->_f_Z() >= thing->_f_Top())
			{
				double dz;

				if (bombspot->_f_Z() > thing->_f_Z())
				{
					dz = double(bombspot->_f_Z() - thing->_f_Top());
				}
				else
				{
					dz = double(thing->_f_Z() - bombspot->_f_Z());
				}
				if (len <= boxradius)
				{
					len = dz;
				}
				else
				{
					len -= boxradius;
					len = g_sqrt(len*len + dz*dz);
				}
			}
			else
			{
				len -= boxradius;
				if (len < 0.f)
					len = 0.f;
			}
			len /= FRACUNIT;
			len = clamp<double>(len - (double)fulldamagedistance, 0, len);
			points = bombdamagefloat * (1.f - len * bombdistancefloat);
			if (thing == bombsource)
			{
				points = points * splashfactor;
			}
			points *= thing->GetClass()->RDFactor;

			// points and bombdamage should be the same sign
			if (((points * bombdamage) > 0) && P_CheckSight(thing, bombspot, SF_IGNOREVISIBILITY | SF_IGNOREWATERBOUNDARY))
			{ // OK to damage; target is in direct path
				double vz;
				double thrust;
				int damage = abs((int)points);
				int newdam = damage;

				if (!(flags & RADF_NODAMAGE))
					newdam = P_DamageMobj(thing, bombspot, bombsource, damage, bombmod);
				else if (thing->player == NULL && (!(flags & RADF_NOIMPACTDAMAGE) && !(thing->flags7 & MF7_DONTTHRUST)))
					thing->flags2 |= MF2_BLASTED;

				if (!(thing->flags & MF_ICECORPSE))
				{
					if (!(flags & RADF_NODAMAGE) && !(bombspot->flags3 & MF3_BLOODLESSIMPACT))
						P_TraceBleed(newdam > 0 ? newdam : damage, thing, bombspot);

					if ((flags & RADF_NODAMAGE) || !(bombspot->flags2 & MF2_NODMGTHRUST))
					{
						if (bombsource == NULL || !(bombsource->flags2 & MF2_NODMGTHRUST))
						{
							if (!(thing->flags7 & MF7_DONTTHRUST))
							{
							
								thrust = points * 0.5 / (double)thing->Mass;
								if (bombsource == thing)
								{
									thrust *= selfthrustscale;
								}
								vz = (thing->Center() - bombspot->Z()) * thrust;
								if (bombsource != thing)
								{
									vz *= 0.5;
								}
								else
								{
									vz *= 0.8;
								}
								thing->Thrust(bombspot->AngleTo(thing), thrust);
								if (!(flags & RADF_NODAMAGE))
									thing->Vel.Z += vz;	// this really doesn't work well
							}
						}
					}
				}
			}
		}
		else
		{
			// [RH] Old code just for barrels
			fixed_t dx, dy, dist;

			fixedvec2 vec = bombspot->_f_Vec2To(thing);
			dx = abs(vec.x);
			dy = abs(vec.y);

			dist = dx>dy ? dx : dy;
			dist = (dist - thing->_f_radius()) >> FRACBITS;

			if (dist < 0)
				dist = 0;

			if (dist >= bombdistance)
				continue;  // out of range

			if (P_CheckSight(thing, bombspot, SF_IGNOREVISIBILITY | SF_IGNOREWATERBOUNDARY))
			{ // OK to damage; target is in direct path
				dist = clamp<int>(dist - fulldamagedistance, 0, dist);
				int damage = Scale(bombdamage, bombdistance - dist, bombdistance);

				double factor = splashfactor * thing->GetClass()->RDFactor;
				damage = int(damage * factor);
				if (damage > 0)
				{
					int newdam = P_DamageMobj(thing, bombspot, bombsource, damage, bombmod);
					P_TraceBleed(newdam > 0 ? newdam : damage, thing, bombspot);
				}
			}
		}
	}
}

//==========================================================================
//
// SECTOR HEIGHT CHANGING
// After modifying a sector's floor or ceiling height,
// call this routine to adjust the positions
// of all things that touch the sector.
//
// If anything doesn't fit anymore, true will be returned.
//
// [RH] If crushchange is non-negative, they will take the
//		specified amount of damage as they are being crushed.
//		If crushchange is negative, you should set the sector
//		height back the way it was and call P_ChangeSector()
//		again to undo the changes.
//		Note that this is very different from the original
//		true/false usage of crushchange! If you want regular
//		DOOM crushing behavior set crushchange to 10 or -1
//		if no crushing is desired.
//
//==========================================================================


struct FChangePosition
{
	sector_t *sector;
	int moveamt;
	int crushchange;
	bool nofit;
	bool movemidtex;
};

TArray<AActor *> intersectors;

EXTERN_CVAR(Int, cl_bloodtype)

//=============================================================================
//
// P_AdjustFloorCeil
//
//=============================================================================

bool P_AdjustFloorCeil(AActor *thing, FChangePosition *cpos)
{
	ActorFlags2 flags2 = thing->flags2 & MF2_PASSMOBJ;
	FCheckPosition tm;

	if ((thing->flags2 & MF2_PASSMOBJ) && (thing->flags3 & MF3_ISMONSTER))
	{
		tm.FromPMove = true;
	}

	if (cpos->movemidtex)
	{
		// From Eternity:
		// ALL things must be treated as PASSMOBJ when moving
		// 3DMidTex lines, otherwise you get stuck in them.
		thing->flags2 |= MF2_PASSMOBJ;
	}

	bool isgood = P_CheckPosition(thing, thing->Pos(), tm);
	thing->floorz = tm.floorz;
	thing->ceilingz = tm.ceilingz;
	thing->dropoffz = tm.dropoffz;		// killough 11/98: remember dropoffs
	thing->floorpic = tm.floorpic;
	thing->floorterrain = tm.floorterrain;
	thing->floorsector = tm.floorsector;
	thing->ceilingpic = tm.ceilingpic;
	thing->ceilingsector = tm.ceilingsector;

	// restore the PASSMOBJ flag but leave the other flags alone.
	thing->flags2 = (thing->flags2 & ~MF2_PASSMOBJ) | flags2;

	return isgood;
}

//=============================================================================
//
// P_FindAboveIntersectors
//
//=============================================================================

void P_FindAboveIntersectors(AActor *actor)
{
	if (actor->flags & MF_NOCLIP)
		return;

	if (!(actor->flags & MF_SOLID))
		return;

	FPortalGroupArray check;
	FMultiBlockThingsIterator it(check, actor);
	FMultiBlockThingsIterator::CheckResult cres;
	while (it.Next(&cres))
	{
		AActor *thing = cres.thing;
		fixed_t blockdist = actor->_f_radius() + thing->_f_radius();
		if (abs(thing->_f_X() - cres.position.x) >= blockdist || abs(thing->_f_Y() - cres.position.y) >= blockdist)
			continue;

		if (!(thing->flags & MF_SOLID))
		{ // Can't hit thing
			continue;
		}
		if (thing->flags & (MF_SPECIAL))
		{ // [RH] Corpses and specials don't block moves
			continue;
		}
		if (thing->flags & (MF_CORPSE))
		{ // Corpses need a few more checks
			if (!(actor->flags & MF_ICECORPSE))
				continue;
		}
		if (thing == actor)
		{ // Don't clip against self
			continue;
		}
		if (!((thing->flags2 | actor->flags2) & MF2_PASSMOBJ) && !((thing->flags3 | actor->flags3) & MF3_ISMONSTER))
		{
			// Don't bother if both things don't have MF2_PASSMOBJ set and aren't monsters.
			// These things would always block each other which in nearly every situation is 
			// not what is wanted here.
			continue;
		}
		if (thing->Z() >= actor->Z() &&
			thing->Z() <= actor->Top())
		{ // Thing intersects above the base
			intersectors.Push(thing);
		}
	}
}

//=============================================================================
//
// P_FindBelowIntersectors
//
//=============================================================================

void P_FindBelowIntersectors(AActor *actor)
{
	if (actor->flags & MF_NOCLIP)
		return;

	if (!(actor->flags & MF_SOLID))
		return;

	FPortalGroupArray check;
	FMultiBlockThingsIterator it(check, actor);
	FMultiBlockThingsIterator::CheckResult cres;
	while (it.Next(&cres))
	{
		AActor *thing = cres.thing;
		fixed_t blockdist = actor->_f_radius() + thing->_f_radius();
		if (abs(thing->_f_X() - cres.position.x) >= blockdist || abs(thing->_f_Y() - cres.position.y) >= blockdist)
			continue;

		if (!(thing->flags & MF_SOLID))
		{ // Can't hit thing
			continue;
		}
		if (thing->flags & (MF_SPECIAL))
		{ // [RH] Corpses and specials don't block moves
			continue;
		}
		if (thing->flags & (MF_CORPSE))
		{ // Corpses need a few more checks
			if (!(actor->flags & MF_ICECORPSE))
				continue;
		}
		if (thing == actor)
		{ // Don't clip against self
			continue;
		}
		if (!((thing->flags2 | actor->flags2) & MF2_PASSMOBJ) && !((thing->flags3 | actor->flags3) & MF3_ISMONSTER))
		{
			// Don't bother if both things don't have MF2_PASSMOBJ set and aren't monsters.
			// These things would always block each other which in nearly every situation is 
			// not what is wanted here.
			continue;
		}
		if (thing->Top() <= actor->Top() &&
			thing->Top() > actor->Z())
		{ // Thing intersects below the base
			intersectors.Push(thing);
		}
	}
}

//=============================================================================
//
// P_DoCrunch
//
//=============================================================================

void P_DoCrunch(AActor *thing, FChangePosition *cpos)
{
	if (!(thing && thing->Grind(true) && cpos)) return;
	cpos->nofit = true;

	if ((cpos->crushchange > 0) && !(level.maptime & 3))
	{
		int newdam = P_DamageMobj(thing, NULL, NULL, cpos->crushchange, NAME_Crush);

		// spray blood in a random direction
		if (!(thing->flags2&(MF2_INVULNERABLE | MF2_DORMANT)))
		{
			if (!(thing->flags&MF_NOBLOOD))
			{
				PalEntry bloodcolor = thing->GetBloodColor();
				PClassActor *bloodcls = thing->GetBloodType();
				
				P_TraceBleed (newdam > 0 ? newdam : cpos->crushchange, thing);
				if (bloodcls != NULL)
				{
					AActor *mo;

					mo = Spawn(bloodcls, thing->PosPlusZ(thing->Height / 2), ALLOW_REPLACE);

					mo->Vel.X = pr_crunch.Random2() / 16.;
					mo->Vel.Y = pr_crunch.Random2() / 16.;
					if (bloodcolor != 0 && !(mo->flags2 & MF2_DONTTRANSLATE))
					{
						mo->Translation = TRANSLATION(TRANSLATION_Blood, bloodcolor.a);
					}

					if (!(cl_bloodtype <= 1)) mo->renderflags |= RF_INVISIBLE;
				}

				DAngle an = (M_Random() - 128) * (360./256);
				if (cl_bloodtype >= 1)
				{
					P_DrawSplash2(32,  thing->PosPlusZ(thing->Height/2), an, 2, bloodcolor);
				}
			}
			if (thing->CrushPainSound != 0 && !S_GetSoundPlayingInfo(thing, thing->CrushPainSound))
			{
				S_Sound(thing, CHAN_VOICE, thing->CrushPainSound, 1.f, ATTN_NORM);
			}
		}
	}

	// keep checking (crush other things)
	return;
}

//=============================================================================
//
// P_PushUp
//
// Returns 0 if thing fits, 1 if ceiling got in the way, or 2 if something
// above it didn't fit.
//=============================================================================

int P_PushUp(AActor *thing, FChangePosition *cpos)
{
	unsigned int firstintersect = intersectors.Size();
	unsigned int lastintersect;
	int mymass = thing->Mass;

	if (thing->Top() > thing->ceilingz)
	{
		return 1;
	}
	// [GZ] Skip thing intersect test for THRUACTORS things.
	if (thing->flags2 & MF2_THRUACTORS)
		return 0;
	P_FindAboveIntersectors(thing);
	lastintersect = intersectors.Size();
	for (; firstintersect < lastintersect; firstintersect++)
	{
		AActor *intersect = intersectors[firstintersect];
		// [GZ] Skip this iteration for THRUSPECIES things
		// Should there be MF2_THRUGHOST / MF3_GHOST checks there too for consistency?
		// Or would that risk breaking established behavior? THRUGHOST, like MTHRUSPECIES,
		// is normally for projectiles which would have exploded by now anyway...
		if (thing->flags6 & MF6_THRUSPECIES && thing->GetSpecies() == intersect->GetSpecies())
			continue;
		if ((thing->flags & MF_MISSILE) && (intersect->flags2 & MF2_REFLECTIVE) && (intersect->flags7 & MF7_THRUREFLECT))
			continue;
		if (!(intersect->flags2 & MF2_PASSMOBJ) ||
			(!(intersect->flags3 & MF3_ISMONSTER) && intersect->Mass > mymass) ||
			(intersect->flags4 & MF4_ACTLIKEBRIDGE)
			)
		{
			// Can't push bridges or things more massive than ourself
			return 2;
		}
		fixed_t oldz;
		oldz = intersect->_f_Z();
		P_AdjustFloorCeil(intersect, cpos);
		intersect->_f_SetZ(thing->_f_Top() + 1);
		if (P_PushUp(intersect, cpos))
		{ // Move blocked
			P_DoCrunch(intersect, cpos);
			intersect->_f_SetZ(oldz);
			return 2;
		}
	}
	thing->CheckPortalTransition(true);
	return 0;
}

//=============================================================================
//
// P_PushDown
//
// Returns 0 if thing fits, 1 if floor got in the way, or 2 if something
// below it didn't fit.
//=============================================================================

int P_PushDown(AActor *thing, FChangePosition *cpos)
{
	unsigned int firstintersect = intersectors.Size();
	unsigned int lastintersect;
	int mymass = thing->Mass;

	if (thing->Z() <= thing->floorz)
	{
		return 1;
	}
	P_FindBelowIntersectors(thing);
	lastintersect = intersectors.Size();
	for (; firstintersect < lastintersect; firstintersect++)
	{
		AActor *intersect = intersectors[firstintersect];
		if (!(intersect->flags2 & MF2_PASSMOBJ) ||
			(!(intersect->flags3 & MF3_ISMONSTER) && intersect->Mass > mymass) ||
			(intersect->flags4 & MF4_ACTLIKEBRIDGE)
			)
		{
			// Can't push bridges or things more massive than ourself
			return 2;
		}
		fixed_t oldz = intersect->_f_Z();
		P_AdjustFloorCeil(intersect, cpos);
		if (oldz > thing->_f_Z() - intersect->_f_height())
		{ // Only push things down, not up.
			intersect->_f_SetZ(thing->_f_Z() - intersect->_f_height());
			if (P_PushDown(intersect, cpos))
			{ // Move blocked
				P_DoCrunch(intersect, cpos);
				intersect->_f_SetZ(oldz);
				return 2;
			}
		}
	}
	thing->CheckPortalTransition(true);
	return 0;
}

//=============================================================================
//
// PIT_FloorDrop
//
//=============================================================================

void PIT_FloorDrop(AActor *thing, FChangePosition *cpos)
{
	fixed_t oldfloorz = thing->_f_floorz();
	fixed_t oldz = thing->_f_Z();

	P_AdjustFloorCeil(thing, cpos);

	if (oldfloorz == thing->_f_floorz()) return;
	if (thing->flags4 & MF4_ACTLIKEBRIDGE) return; // do not move bridge things

	if (thing->_f_velz() == 0 &&
		(!(thing->flags & MF_NOGRAVITY) ||
		(thing->_f_Z() == oldfloorz && !(thing->flags & MF_NOLIFTDROP))))
	{
		fixed_t oldz = thing->_f_Z();

		if ((thing->flags & MF_NOGRAVITY) || (thing->flags5 & MF5_MOVEWITHSECTOR) ||
			(((cpos->sector->Flags & SECF_FLOORDROP) || cpos->moveamt < 9 * FRACUNIT)
			&& thing->_f_Z() - thing->_f_floorz() <= cpos->moveamt))
		{
			thing->SetZ(thing->floorz);
			P_CheckFakeFloorTriggers(thing, oldz);
		}
	}
	else if ((thing->_f_Z() != oldfloorz && !(thing->flags & MF_NOLIFTDROP)))
	{
		fixed_t oldz = thing->_f_Z();
		if ((thing->flags & MF_NOGRAVITY) && (thing->flags6 & MF6_RELATIVETOFLOOR))
		{
			thing->_f_AddZ(-oldfloorz + thing->_f_floorz());
			P_CheckFakeFloorTriggers(thing, oldz);
		}
	}
	if (thing->player && thing->player->mo == thing)
	{
		//thing->player->viewz += thing->_f_Z() - oldz;
	}
}

//=============================================================================
//
// PIT_FloorRaise
//
//=============================================================================

void PIT_FloorRaise(AActor *thing, FChangePosition *cpos)
{
	fixed_t oldfloorz = thing->_f_floorz();
	fixed_t oldz = thing->_f_Z();

	P_AdjustFloorCeil(thing, cpos);

	if (oldfloorz == thing->_f_floorz()) return;

	// Move things intersecting the floor up
	if (thing->Z() <= thing->floorz)
	{
		if (thing->flags4 & MF4_ACTLIKEBRIDGE)
		{
			cpos->nofit = true;
			return; // do not move bridge things
		}
		intersectors.Clear();
		thing->SetZ(thing->floorz);
	}
	else
	{
		if ((thing->flags & MF_NOGRAVITY) && (thing->flags6 & MF6_RELATIVETOFLOOR))
		{
			intersectors.Clear();
			thing->_f_AddZ(-oldfloorz + thing->_f_floorz());
		}
		else return;
	}
	switch (P_PushUp(thing, cpos))
	{
	default:
		P_CheckFakeFloorTriggers(thing, oldz);
		break;
	case 1:
		P_DoCrunch(thing, cpos);
		P_CheckFakeFloorTriggers(thing, oldz);
		break;
	case 2:
		P_DoCrunch(thing, cpos);
		thing->_f_SetZ(oldz);
		break;
	}
	if (thing->player && thing->player->mo == thing)
	{
		thing->player->viewz += thing->Z() - FIXED2DBL(oldz);
	}
}

//=============================================================================
//
// PIT_CeilingLower
//
//=============================================================================

void PIT_CeilingLower(AActor *thing, FChangePosition *cpos)
{
	bool onfloor;
	fixed_t oldz = thing->_f_Z();

	onfloor = thing->Z() <= thing->floorz;
	P_AdjustFloorCeil(thing, cpos);

	if (thing->Top() > thing->ceilingz)
	{
		if (thing->flags4 & MF4_ACTLIKEBRIDGE)
		{
			cpos->nofit = true;
			return; // do not move bridge things
		}
		intersectors.Clear();
		fixed_t oldz = thing->_f_Z();
		if (thing->ceilingz - thing->Height >= thing->floorz)
		{
			thing->SetZ(thing->ceilingz - thing->Height);
		}
		else
		{
			thing->SetZ(thing->floorz);
		}
		switch (P_PushDown(thing, cpos))
		{
		case 2:
			// intentional fall-through
		case 1:
			if (onfloor)
				thing->SetZ(thing->floorz);
			P_DoCrunch(thing, cpos);
			P_CheckFakeFloorTriggers(thing, oldz);
			break;
		default:
			P_CheckFakeFloorTriggers(thing, oldz);
			break;
		}
	}
	if (thing->player && thing->player->mo == thing)
	{
		thing->player->viewz += thing->Z() - FIXED2DBL(oldz);
	}
}

//=============================================================================
//
// PIT_CeilingRaise
//
//=============================================================================

void PIT_CeilingRaise(AActor *thing, FChangePosition *cpos)
{
	bool isgood = P_AdjustFloorCeil(thing, cpos);
	fixed_t oldz = thing->_f_Z();

	if (thing->flags4 & MF4_ACTLIKEBRIDGE) return; // do not move bridge things

	// For DOOM compatibility, only move things that are inside the floor.
	// (or something else?) Things marked as hanging from the ceiling will
	// stay where they are.
	if (thing->_f_Z() < thing->_f_floorz() &&
		thing->_f_Top() >= thing->_f_ceilingz() - cpos->moveamt &&
		!(thing->flags & MF_NOLIFTDROP))
	{
		fixed_t oldz = thing->_f_Z();
		thing->SetZ(thing->floorz);
		if (thing->Top() > thing->ceilingz)
		{
			thing->SetZ(thing->ceilingz - thing->Height);
		}
		P_CheckFakeFloorTriggers(thing, oldz);
	}
	else if ((thing->flags2 & MF2_PASSMOBJ) && !isgood && thing->Top() < thing->ceilingz)
	{
		AActor *onmobj;
		if (!P_TestMobjZ(thing, true, &onmobj) && onmobj->Z() <= thing->Z())
		{
			thing->SetZ(MIN(thing->ceilingz - thing->Height, onmobj->Top()));
		}
	}
	if (thing->player && thing->player->mo == thing)
	{
		thing->player->viewz += thing->Z() - FIXED2DBL(oldz);
	}
}

//=============================================================================
//
// P_ChangeSector	[RH] Was P_CheckSector in BOOM
//
// jff 3/19/98 added to just check monsters on the periphery
// of a moving sector instead of all in bounding box of the
// sector. Both more accurate and faster.
//
//=============================================================================

bool P_ChangeSector(sector_t *sector, int crunch, int amt, int floorOrCeil, bool isreset)
{
	FChangePosition cpos;
	void(*iterator)(AActor *, FChangePosition *);
	void(*iterator2)(AActor *, FChangePosition *) = NULL;
	msecnode_t *n;

	cpos.nofit = false;
	cpos.crushchange = crunch;
	cpos.moveamt = abs(amt);
	cpos.movemidtex = false;
	cpos.sector = sector;

	// Also process all sectors that have 3D floors transferred from the
	// changed sector.
	if (sector->e->XFloor.attached.Size() && floorOrCeil != 2)
	{
		unsigned       i;
		sector_t*      sec;


		// Use different functions for the four different types of sector movement.
		// for 3D-floors the meaning of floor and ceiling is inverted!!!
		if (floorOrCeil == 1)
		{
			iterator = (amt >= 0) ? PIT_FloorRaise : PIT_FloorDrop;
		}
		else
		{
			iterator = (amt >= 0) ? PIT_CeilingRaise : PIT_CeilingLower;
		}

		for (i = 0; i < sector->e->XFloor.attached.Size(); i++)
		{
			sec = sector->e->XFloor.attached[i];
			P_Recalculate3DFloors(sec);	// Must recalculate the 3d floor and light lists

			// no thing checks for attached sectors because of heightsec
			if (sec->heightsec == sector) continue;

			for (n = sec->touching_thinglist; n; n = n->m_snext) n->visited = false;
			do
			{
				for (n = sec->touching_thinglist; n; n = n->m_snext)
				{
					if (!n->visited)
					{
						n->visited = true;
						if (!(n->m_thing->flags & MF_NOBLOCKMAP) ||	//jff 4/7/98 don't do these
							(n->m_thing->flags5 & MF5_MOVEWITHSECTOR))
						{
							iterator(n->m_thing, &cpos);
						}
						break;
					}
				}
			} while (n);
			sec->CheckPortalPlane(!floorOrCeil);
		}
	}
	P_Recalculate3DFloors(sector);			// Must recalculate the 3d floor and light lists

	// [RH] Use different functions for the four different types of sector
	// movement.
	switch (floorOrCeil)
	{
	case 0:
		// floor
		iterator = (amt < 0) ? PIT_FloorDrop : PIT_FloorRaise;
		break;

	case 1:
		// ceiling
		iterator = (amt < 0) ? PIT_CeilingLower : PIT_CeilingRaise;
		break;

	case 2:
		// 3dmidtex
		// This must check both floor and ceiling 
		iterator = (amt < 0) ? PIT_FloorDrop : PIT_FloorRaise;
		iterator2 = (amt < 0) ? PIT_CeilingLower : PIT_CeilingRaise;
		cpos.movemidtex = true;
		break;

	default:
		// invalid
		assert(floorOrCeil > 0 && floorOrCeil < 2);
		return false;
	}

	// killough 4/4/98: scan list front-to-back until empty or exhausted,
	// restarting from beginning after each thing is processed. Avoids
	// crashes, and is sure to examine all things in the sector, and only
	// the things which are in the sector, until a steady-state is reached.
	// Things can arbitrarily be inserted and removed and it won't mess up.
	//
	// killough 4/7/98: simplified to avoid using complicated counter

	// Mark all things invalid

	for (n = sector->touching_thinglist; n; n = n->m_snext)
		n->visited = false;

	do
	{
		for (n = sector->touching_thinglist; n; n = n->m_snext)	// go through list
		{
			if (!n->visited)								// unprocessed thing found
			{
				n->visited = true; 							// mark thing as processed
				if (!(n->m_thing->flags & MF_NOBLOCKMAP) ||	//jff 4/7/98 don't do these
					(n->m_thing->flags5 & MF5_MOVEWITHSECTOR))
				{
					iterator(n->m_thing, &cpos);		 			// process it
					if (iterator2 != NULL) iterator2(n->m_thing, &cpos);
				}
				break;										// exit and start over
			}
		}
	} while (n);	// repeat from scratch until all things left are marked valid

	if (floorOrCeil != 2) sector->CheckPortalPlane(floorOrCeil);	// check for portal obstructions after everything is done.

	if (!cpos.nofit && !isreset /* && sector->MoreFlags & (SECF_UNDERWATERMASK)*/)
	{
		// If this is a control sector for a deep water transfer, all actors in affected
		// sectors need to have their waterlevel information updated and if applicable,
		// execute appropriate sector actions.
		// Only check if the sector move was successful.
		TArray<sector_t *> & secs = sector->e->FakeFloor.Sectors;
		for (unsigned i = 0; i < secs.Size(); i++)
		{
			sector_t * s = secs[i];

			for (n = s->touching_thinglist; n; n = n->m_snext)
				n->visited = false;

			do
			{
				for (n = s->touching_thinglist; n; n = n->m_snext)	// go through list
				{
					if (!n->visited && n->m_thing->Sector == s)		// unprocessed thing found
					{
						n->visited = true; 							// mark thing as processed

						n->m_thing->UpdateWaterLevel(false);
						P_CheckFakeFloorTriggers(n->m_thing, n->m_thing->_f_Z() - amt);
					}
				}
			} while (n);	// repeat from scratch until all things left are marked valid
		}

	}

	return cpos.nofit;
}

//=============================================================================
// phares 3/21/98
//
// Maintain a freelist of msecnode_t's to reduce memory allocs and frees.
//=============================================================================

msecnode_t *headsecnode = NULL;

//=============================================================================
//
// P_GetSecnode
//
// Retrieve a node from the freelist. The calling routine
// should make sure it sets all fields properly.
//
//=============================================================================

msecnode_t *P_GetSecnode()
{
	msecnode_t *node;

	if (headsecnode)
	{
		node = headsecnode;
		headsecnode = headsecnode->m_snext;
	}
	else
	{
		node = (msecnode_t *)M_Malloc(sizeof(*node));
	}
	return node;
}

//=============================================================================
//
// P_PutSecnode
//
// Returns a node to the freelist.
//
//=============================================================================

void P_PutSecnode(msecnode_t *node)
{
	node->m_snext = headsecnode;
	headsecnode = node;
}

//=============================================================================
// phares 3/16/98
//
// P_AddSecnode
//
// Searches the current list to see if this sector is
// already there. If not, it adds a sector node at the head of the list of
// sectors this object appears in. This is called when creating a list of
// nodes that will get linked in later. Returns a pointer to the new node.
//
//=============================================================================

msecnode_t *P_AddSecnode(sector_t *s, AActor *thing, msecnode_t *nextnode)
{
	msecnode_t *node;

	if (s == 0)
	{
		I_FatalError("AddSecnode of 0 for %s\n", thing->GetClass()->TypeName.GetChars());
	}

	node = nextnode;
	while (node)
	{
		if (node->m_sector == s)	// Already have a node for this sector?
		{
			node->m_thing = thing;	// Yes. Setting m_thing says 'keep it'.
			return nextnode;
		}
		node = node->m_tnext;
	}

	// Couldn't find an existing node for this sector. Add one at the head
	// of the list.

	node = P_GetSecnode();

	// killough 4/4/98, 4/7/98: mark new nodes unvisited.
	node->visited = 0;

	node->m_sector = s; 			// sector
	node->m_thing = thing; 		// mobj
	node->m_tprev = NULL;			// prev node on Thing thread
	node->m_tnext = nextnode;		// next node on Thing thread
	if (nextnode)
		nextnode->m_tprev = node;	// set back link on Thing

	// Add new node at head of sector thread starting at s->touching_thinglist

	node->m_sprev = NULL;			// prev node on sector thread
	node->m_snext = s->touching_thinglist; // next node on sector thread
	if (s->touching_thinglist)
		node->m_snext->m_sprev = node;
	s->touching_thinglist = node;
	return node;
}

//=============================================================================
//
// P_DelSecnode
//
// Deletes a sector node from the list of
// sectors this object appears in. Returns a pointer to the next node
// on the linked list, or NULL.
//
//=============================================================================

msecnode_t *P_DelSecnode(msecnode_t *node)
{
	msecnode_t* tp;  // prev node on thing thread
	msecnode_t* tn;  // next node on thing thread
	msecnode_t* sp;  // prev node on sector thread
	msecnode_t* sn;  // next node on sector thread

	if (node)
	{
		// Unlink from the Thing thread. The Thing thread begins at
		// sector_list and not from AActor->touching_sectorlist.

		tp = node->m_tprev;
		tn = node->m_tnext;
		if (tp)
			tp->m_tnext = tn;
		if (tn)
			tn->m_tprev = tp;

		// Unlink from the sector thread. This thread begins at
		// sector_t->touching_thinglist.

		sp = node->m_sprev;
		sn = node->m_snext;
		if (sp)
			sp->m_snext = sn;
		else
			node->m_sector->touching_thinglist = sn;
		if (sn)
			sn->m_sprev = sp;

		// Return this node to the freelist

		P_PutSecnode(node);
		return tn;
	}
	return NULL;
} 														// phares 3/13/98

//=============================================================================
//
// P_DelSector_List
//
// Deletes the sector_list and NULLs it.
//
//=============================================================================

void P_DelSector_List()
{
	if (sector_list != NULL)
	{
		P_DelSeclist(sector_list);
		sector_list = NULL;
	}
}

//=============================================================================
//
// P_DelSeclist
//
// Delete an entire sector list
//
//=============================================================================

void P_DelSeclist(msecnode_t *node)
{
	while (node)
		node = P_DelSecnode(node);
}

//=============================================================================
// phares 3/14/98
//
// P_CreateSecNodeList
//
// Alters/creates the sector_list that shows what sectors the object resides in
//
//=============================================================================

void P_CreateSecNodeList(AActor *thing, fixed_t x, fixed_t y)
{
	msecnode_t *node;

	// First, clear out the existing m_thing fields. As each node is
	// added or verified as needed, m_thing will be set properly. When
	// finished, delete all nodes where m_thing is still NULL. These
	// represent the sectors the Thing has vacated.

	node = sector_list;
	while (node)
	{
		node->m_thing = NULL;
		node = node->m_tnext;
	}

	FBoundingBox box(thing->_f_X(), thing->_f_Y(), thing->_f_radius());
	FBlockLinesIterator it(box);
	line_t *ld;

	while ((ld = it.Next()))
	{
		if (box.Right() <= ld->bbox[BOXLEFT] ||
			box.Left() >= ld->bbox[BOXRIGHT] ||
			box.Top() <= ld->bbox[BOXBOTTOM] ||
			box.Bottom() >= ld->bbox[BOXTOP])
			continue;

		if (box.BoxOnLineSide(ld) != -1)
			continue;

		// This line crosses through the object.

		// Collect the sector(s) from the line and add to the
		// sector_list you're examining. If the Thing ends up being
		// allowed to move to this position, then the sector_list
		// will be attached to the Thing's AActor at touching_sectorlist.

		sector_list = P_AddSecnode(ld->frontsector, thing, sector_list);

		// Don't assume all lines are 2-sided, since some Things
		// like MT_TFOG are allowed regardless of whether their radius takes
		// them beyond an impassable linedef.

		// killough 3/27/98, 4/4/98:
		// Use sidedefs instead of 2s flag to determine two-sidedness.

		if (ld->backsector)
			sector_list = P_AddSecnode(ld->backsector, thing, sector_list);
	}

	// Add the sector of the (x,y) point to sector_list.

	sector_list = P_AddSecnode(thing->Sector, thing, sector_list);

	// Now delete any nodes that won't be used. These are the ones where
	// m_thing is still NULL.

	node = sector_list;
	while (node)
	{
		if (node->m_thing == NULL)
		{
			if (node == sector_list)
				sector_list = node->m_tnext;
			node = P_DelSecnode(node);
		}
		else
		{
			node = node->m_tnext;
		}
	}
}

//==========================================================================
//
//
//
//==========================================================================

void SpawnShootDecal(AActor *t1, const FTraceResults &trace)
{
	FDecalBase *decalbase = NULL;

	if (t1->player != NULL && t1->player->ReadyWeapon != NULL)
	{
		decalbase = t1->player->ReadyWeapon->GetDefault()->DecalGenerator;
	}
	else
	{
		decalbase = t1->DecalGenerator;
	}
	if (decalbase != NULL)
	{
		DImpactDecal::StaticCreate(decalbase->GetDecal(),
			trace.HitPos, trace.Line->sidedef[trace.Side], trace.ffloor);
	}
}

//==========================================================================
//
//
//
//==========================================================================

static void SpawnDeepSplash(AActor *t1, const FTraceResults &trace, AActor *puff)
{
	const DVector3 *hitpos;
	if (trace.Crossed3DWater)
	{
		hitpos = &trace.Crossed3DWaterPos;
	}
	else if (trace.CrossedWater && trace.CrossedWater->heightsec)
	{
		hitpos = &trace.CrossedWaterPos;
	}
	else return;

	P_HitWater(puff != NULL ? puff : t1, P_PointInSector(*hitpos), *hitpos);
}

//=============================================================================
//
// P_ActivateThingSpecial
//
// Handles the code for things activated by death, USESPECIAL or BUMPSPECIAL
//
//=============================================================================

bool P_ActivateThingSpecial(AActor * thing, AActor * trigger, bool death)
{
	bool res = false;

	// Target switching mechanism
	if (thing->activationtype & THINGSPEC_ThingTargets)		thing->target = trigger;
	if (thing->activationtype & THINGSPEC_TriggerTargets)	trigger->target = thing;

	// State change mechanism. The thing needs to be not dead and to have at least one of the relevant flags
	if (!death && (thing->activationtype & (THINGSPEC_Activate | THINGSPEC_Deactivate | THINGSPEC_Switch)))
	{
		// If a switchable thing does not know whether it should be activated
		// or deactivated, the default is to activate it.
		if ((thing->activationtype & THINGSPEC_Switch)
			&& !(thing->activationtype & (THINGSPEC_Activate | THINGSPEC_Deactivate)))
		{
			thing->activationtype |= THINGSPEC_Activate;
		}
		// Can it be activated?
		if (thing->activationtype & THINGSPEC_Activate)
		{
			thing->activationtype &= ~THINGSPEC_Activate; // Clear flag
			if (thing->activationtype & THINGSPEC_Switch) // Set other flag if switching
				thing->activationtype |= THINGSPEC_Deactivate;
			thing->Activate(trigger);
			res = true;
		}
		// If not, can it be deactivated?
		else if (thing->activationtype & THINGSPEC_Deactivate)
		{
			thing->activationtype &= ~THINGSPEC_Deactivate; // Clear flag
			if (thing->activationtype & THINGSPEC_Switch)	// Set other flag if switching
				thing->activationtype |= THINGSPEC_Activate;
			thing->Deactivate(trigger);
			res = true;
		}
	}

	// Run the special, if any
	if (thing->special)
	{
		res = !!P_ExecuteSpecial(thing->special, NULL,
			// TriggerActs overrides the level flag, which only concerns thing activated by death
			(((death && level.flags & LEVEL_ACTOWNSPECIAL && !(thing->activationtype & THINGSPEC_TriggerActs))
			|| (thing->activationtype & THINGSPEC_ThingActs)) // Who triggers?
			? thing : trigger),
			false, thing->args[0], thing->args[1], thing->args[2], thing->args[3], thing->args[4]);

		// Clears the special if it was run on thing's death or if flag is set.
		if (death || (thing->activationtype & THINGSPEC_ClearSpecial && res)) thing->special = 0;
	}

	// Returns the result
	return res;
}
