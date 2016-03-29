/*
** a_dynlight.cpp
** Implements actors representing dynamic lights (hardware independent)
**
**---------------------------------------------------------------------------
** Copyright 2003 Timothy Stump
** Copyright 2005 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
** 4. Full disclosure of the entire project's source code, except for third
**    party libraries is mandatory. (NOTE: This clause is non-negotiable!)
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#include "gl/system/gl_system.h"

#include "templates.h"
#include "m_random.h"
#include "p_local.h"
#include "c_dispatch.h"
#include "g_level.h"
#include "thingdef/thingdef.h"
#include "i_system.h"
#include "templates.h"
#include "doomdata.h"
#include "r_utility.h"
#include "portal.h"


#include "gl/renderer/gl_renderer.h"
#include "gl/data/gl_data.h"
#include "gl/dynlights/gl_dynlight.h"
#include "gl/utility/gl_convert.h"
#include "gl/utility/gl_templates.h"

EXTERN_CVAR (Float, gl_lights_size);
EXTERN_CVAR (Bool, gl_lights_additive);
EXTERN_CVAR(Int, vid_renderer)


//==========================================================================
//
//==========================================================================
DEFINE_CLASS_PROPERTY(type, S, DynamicLight)
{
	PROP_STRING_PARM(str, 0);
	static const char * ltype_names[]={
		"Point","Pulse","Flicker","Sector","RandomFlicker", "ColorPulse", "ColorFlicker", "RandomColorFlicker", NULL};

	static const int ltype_values[]={
		PointLight, PulseLight, FlickerLight, SectorLight, RandomFlickerLight, ColorPulseLight, ColorFlickerLight, RandomColorFlickerLight };

	int style = MatchString(str, ltype_names);
	if (style < 0) I_Error("Unknown light type '%s'", str);
	defaults->lighttype = ltype_values[style];
}

//==========================================================================
//
// Actor classes
//
// For flexibility all functionality has been packed into a single class
// which is controlled by flags
//
//==========================================================================
IMPLEMENT_CLASS (ADynamicLight)
IMPLEMENT_CLASS (AVavoomLight)
IMPLEMENT_CLASS (AVavoomLightWhite)
IMPLEMENT_CLASS (AVavoomLightColor)

void AVavoomLight::BeginPlay ()
{
	// This must not call Super::BeginPlay!
	ChangeStatNum(STAT_DLIGHT);
	if (Sector) AddZ(-Sector->floorplane.ZatPoint(this), false);
	lighttype = PointLight;
}

void AVavoomLightWhite::BeginPlay ()
{
	m_intensity[0] = args[0] * 4;
	args[LIGHT_RED] = 128;
	args[LIGHT_GREEN] = 128;
	args[LIGHT_BLUE] = 128;

	Super::BeginPlay();
}

void AVavoomLightColor::BeginPlay ()
{
	int l_args[5];
	memcpy(l_args, args, sizeof(l_args));
	memset(args, 0, sizeof(args));
	m_intensity[0] = l_args[0] * 4;
	args[LIGHT_RED] = l_args[1] >> 1;
	args[LIGHT_GREEN] = l_args[2] >> 1;
	args[LIGHT_BLUE] = l_args[3] >> 1;

	Super::BeginPlay();
}

static FRandom randLight;

//==========================================================================
//
// Base class
//
//==========================================================================

//==========================================================================
//
//
//
//==========================================================================
void ADynamicLight::Serialize(FArchive &arc)
{
	Super::Serialize (arc);
	arc << lightflags << lighttype;
	arc << m_tickCount << m_currentIntensity;
	arc << m_intensity[0] << m_intensity[1];

	if (lighttype == PulseLight) arc << m_lastUpdate << m_cycler;
	if (arc.IsLoading()) LinkLight();

}


//==========================================================================
//
//
//
//==========================================================================
void ADynamicLight::BeginPlay()
{
	//Super::BeginPlay();
	ChangeStatNum(STAT_DLIGHT);

	m_intensity[0] = args[LIGHT_INTENSITY];
	m_intensity[1] = args[LIGHT_SECONDARY_INTENSITY];
}

//==========================================================================
//
//
//
//==========================================================================
void ADynamicLight::PostBeginPlay()
{
	Super::PostBeginPlay();
	
	if (!(SpawnFlags & MTF_DORMANT))
	{
		Activate (NULL);
	}

	subsector = R_PointInSubsector(_f_X(), _f_Y());
}


//==========================================================================
//
//
//
//==========================================================================
void ADynamicLight::Activate(AActor *activator)
{
	//Super::Activate(activator);
	flags2&=~MF2_DORMANT;	

	m_currentIntensity = float(m_intensity[0]);
	m_tickCount = 0;

	if (lighttype == PulseLight)
	{
		float pulseTime = Angles.Yaw.Degrees / TICRATE;
		
		m_lastUpdate = level.maptime;
		m_cycler.SetParams(float(m_intensity[1]), float(m_intensity[0]), pulseTime);
		m_cycler.ShouldCycle(true);
		m_cycler.SetCycleType(CYCLE_Sin);
		m_currentIntensity = (BYTE)m_cycler.GetVal();
	}
}


//==========================================================================
//
//
//
//==========================================================================
void ADynamicLight::Deactivate(AActor *activator)
{
	//Super::Deactivate(activator);
	flags2|=MF2_DORMANT;	
}


//==========================================================================
//
//
//
//==========================================================================
void ADynamicLight::Tick()
{
	if (vid_renderer == 0)
	{
		return;
	}
	if (IsOwned())
	{
		if (!target || !target->state)
		{
			this->Destroy();
			return;
		}
		if (target->flags & MF_UNMORPHED) return;
	}

	// Don't bother if the light won't be shown
	if (!IsActive()) return;

	// I am doing this with a type field so that I can dynamically alter the type of light
	// without having to create or maintain multiple objects.
	switch(lighttype)
	{
	case PulseLight:
	{
		float diff = (level.maptime - m_lastUpdate) / (float)TICRATE;
		
		m_lastUpdate = level.maptime;
		m_cycler.Update(diff);
		m_currentIntensity = m_cycler.GetVal();
		break;
	}

	case FlickerLight:
	{
		BYTE rnd = randLight();
		float pct = Angles.Yaw.Degrees / 360.f;
		
		m_currentIntensity = float(m_intensity[rnd >= pct * 255]);
		break;
	}

	case RandomFlickerLight:
	{
		int flickerRange = m_intensity[1] - m_intensity[0];
		float amt = randLight() / 255.f;
		
		m_tickCount++;
		
		if (m_tickCount > Angles.Yaw.Degrees)
		{
			m_currentIntensity = float(m_intensity[0] + (amt * flickerRange));
			m_tickCount = 0;
		}
		break;
	}

#if 0
	// These need some more work elsewhere
	case ColorFlickerLight:
	{
		BYTE rnd = randLight();
		float pct = Angles.Yaw.Degrees/360.f;
		
		m_currentIntensity = m_intensity[rnd >= pct * 255];
		break;
	}

	case RandomColorFlickerLight:
	{
		int flickerRange = m_intensity[1] - m_intensity[0];
		float amt = randLight() / 255.f;
		
		m_tickCount++;
		
		if (m_tickCount > Angles.Yaw.Degrees)
		{
			m_currentIntensity = m_intensity[0] + (amt * flickerRange);
			m_tickCount = 0;
		}
		break;
	}
#endif

	case SectorLight:
	{
		float intensity;
		float scale = args[LIGHT_SCALE] / 8.f;
		
		if (scale == 0.f) scale = 1.f;
		
		intensity = Sector->lightlevel * scale;
		intensity = clamp<float>(intensity, 0.f, 255.f);
		
		m_currentIntensity = intensity;
		break;
	}

	case PointLight:
		m_currentIntensity = float(m_intensity[0]);
		break;
	}

	UpdateLocation();
}




//==========================================================================
//
//
//
//==========================================================================
void ADynamicLight::UpdateLocation()
{
	double oldx= X();
	double oldy= Y();
	double oldradius= radius;
	float intensity;

	if (IsActive())
	{
		if (target)
		{
			DAngle angle = target->Angles.Yaw;
			double s = angle.Sin();
			double c = angle.Cos();

			DVector3 pos = target->Vec3Offset(m_off.X * c + m_off.Y * s, m_off.X * s - m_off.Y * c, m_off.Z + target->GetBobOffset());
			SetXYZ(pos); // attached lights do not need to go into the regular blockmap
			Prev = target->Pos();
			subsector = R_PointInSubsector(target->_f_X(), target->_f_Y());
			Sector = subsector->sector;
		}


		// The radius being used here is always the maximum possible with the
		// current settings. This avoids constant relinking of flickering lights

		if (lighttype == FlickerLight || lighttype == RandomFlickerLight)
		{
			intensity = float(MAX(m_intensity[0], m_intensity[1]));
		}
		else
		{
			intensity = m_currentIntensity;
		}
		radius = intensity * 2.0f * gl_lights_size;

		if (X() != oldx || Y() != oldy || radius != oldradius)
		{
			//Update the light lists
			LinkLight();
		}
	}
}


//==========================================================================
//
//
//
//==========================================================================

void ADynamicLight::SetOrigin(fixed_t x, fixed_t y, fixed_t z, bool moving)
{
	Super::SetOrigin(x, y, z, moving);
	LinkLight();
}

//==========================================================================
//
//
//
//==========================================================================

void ADynamicLight::SetOffset(const DVector3 &pos)
{
	m_off = pos;
	UpdateLocation();
}


//==========================================================================
//
// The target pointer in dynamic lights should never be substituted unless 
// notOld is NULL (which indicates that the object was destroyed by force.)
//
//==========================================================================
size_t ADynamicLight::PointerSubstitution (DObject *old, DObject *notOld)
{
	AActor *saved_target = target;
	size_t ret = Super::PointerSubstitution(old, notOld);
	if (notOld != NULL) target = saved_target;
	return ret;
}

//=============================================================================
//
// These have been copied from the secnode code and modified for the light links
//
// P_AddSecnode() searches the current list to see if this sector is
// already there. If not, it adds a sector node at the head of the list of
// sectors this object appears in. This is called when creating a list of
// nodes that will get linked in later. Returns a pointer to the new node.
//
//=============================================================================

FLightNode * AddLightNode(FLightNode ** thread, void * linkto, ADynamicLight * light, FLightNode *& nextnode)
{
	FLightNode * node;

	node = nextnode;
	while (node)
    {
		if (node->targ==linkto)   // Already have a node for this sector?
		{
			node->lightsource = light; // Yes. Setting m_thing says 'keep it'.
			return(nextnode);
		}
		node = node->nextTarget;
    }

	// Couldn't find an existing node for this sector. Add one at the head
	// of the list.
	
	node = new FLightNode;
	
	node->targ = linkto;
	node->lightsource = light; 

	node->prevTarget = &nextnode; 
	node->nextTarget = nextnode;

	if (nextnode) nextnode->prevTarget = &node->nextTarget;
	
	// Add new node at head of sector thread starting at s->touching_thinglist
	
	node->prevLight = thread;  	
	node->nextLight = *thread; 
	if (node->nextLight) node->nextLight->prevLight=&node->nextLight;
	*thread = node;
	return(node);
}


//=============================================================================
//
// P_DelSecnode() deletes a sector node from the list of
// sectors this object appears in. Returns a pointer to the next node
// on the linked list, or NULL.
//
//=============================================================================

static FLightNode * DeleteLightNode(FLightNode * node)
{
	FLightNode * tn;  // next node on thing thread
	
	if (node)
    {
		
		*node->prevTarget = node->nextTarget;
		if (node->nextTarget) node->nextTarget->prevTarget=node->prevTarget;

		*node->prevLight = node->nextLight;
		if (node->nextLight) node->nextLight->prevLight=node->prevLight;
		
		// Return this node to the freelist
		tn=node->nextTarget;
		delete node;
		return(tn);
    }
	return(NULL);
}                             // phares 3/13/98



//==========================================================================
//
// Gets the light's distance to a line
//
//==========================================================================

double ADynamicLight::DistToSeg(const fixedvec3 &pos, seg_t *seg)
{
	double u, px, py;

	double seg_dx = seg->v2->fX() - seg->v1->fX();
	double seg_dy = seg->v2->fY() - seg->v1->fY();
	double seg_length_sq = seg_dx * seg_dx + seg_dy * seg_dy;

	u = ((FIXED2DBL(pos.x) - seg->v1->fX()) * seg_dx + (FIXED2DBL(pos.y) - seg->v1->fY()) * seg_dy) / seg_length_sq;
	if (u < 0.) u = 0.; // clamp the test point to the line segment
	if (u > 1.) u = 1.;

	px = seg->v1->fX() + (u * seg_dx);
	py = seg->v1->fY() + (u * seg_dy);

	px -= FIXED2DBL(pos.x);
	py -= FIXED2DBL(pos.y);

	return (px*px) + (py*py);
}


//==========================================================================
//
// Collect all touched sidedefs and subsectors
// to sidedefs and sector parts.
//
//==========================================================================

void ADynamicLight::CollectWithinRadius(const fixedvec3 &pos, subsector_t *subSec, float radius)
{
	if (!subSec) return;

	subSec->validcount = ::validcount;

	touching_subsectors = AddLightNode(&subSec->lighthead, subSec, this, touching_subsectors);
	if (subSec->sector->validcount != ::validcount)
	{
		touching_sector = AddLightNode(&subSec->render_sector->lighthead, subSec->sector, this, touching_sector);
		subSec->sector->validcount = ::validcount;
	}

	for (unsigned int i = 0; i < subSec->numlines; i++)
	{
		seg_t * seg = subSec->firstline + i;

		if (seg->sidedef && seg->linedef && seg->linedef->validcount!=::validcount)
		{
			// light is in front of the seg
			if (DMulScale32(pos.y - seg->v1->fixY(), seg->v2->fixX() - seg->v1->fixX(), seg->v1->fixX() - pos.x, seg->v2->fixY() - seg->v1->fixY()) <= 0)
			{
				seg->linedef->validcount = validcount;
				touching_sides = AddLightNode(&seg->sidedef->lighthead, seg->sidedef, this, touching_sides);
			}
		}
		if (seg->linedef)
		{
			FLinePortal *port = seg->linedef->getPortal();
			if (port && port->mType == PORTT_LINKED)
			{
				if (DistToSeg(pos, seg) <= radius)
				{
					line_t *other = port->mDestination;
					if (other->validcount != ::validcount)
					{
						subsector_t *othersub = R_PointInSubsector(other->v1->fixX() + other->fixDx() / 2, other->v1->fixY() + other->fixDy() / 2);
						if (othersub->validcount != ::validcount) CollectWithinRadius(_f_PosRelative(other), othersub, radius);
					}
				}
			}
		}

		seg_t *partner = seg->PartnerSeg;
		if (partner)
		{
			subsector_t *sub = partner->Subsector;
			if (sub != NULL && sub->validcount!=::validcount)
			{
				// check distance from x/y to seg and if within radius add opposing subsector (lather/rinse/repeat)
				if (DistToSeg(pos, seg) <= radius)
				{
					CollectWithinRadius(pos, sub, radius);
				}
			}
		}
	}
	if (!subSec->sector->PortalBlocksSight(sector_t::ceiling))
	{
		line_t *other = subSec->firstline->linedef;
		AActor *sb = subSec->sector->SkyBoxes[sector_t::ceiling];
		if (sb->specialf1 < Z() + radius)
		{
			fixedvec2 refpos = { other->v1->fixX() + other->fixDx() / 2 + FLOAT2FIXED(sb->Scale.X), other->v1->fixY() + other->fixDy() / 2 + FLOAT2FIXED(sb->Scale.Y) };
			subsector_t *othersub = R_PointInSubsector(refpos.x, refpos.y);
			if (othersub->validcount != ::validcount) CollectWithinRadius(_f_PosRelative(othersub->sector), othersub, radius);
		}
	}
	if (!subSec->sector->PortalBlocksSight(sector_t::floor))
	{
		line_t *other = subSec->firstline->linedef;
		AActor *sb = subSec->sector->SkyBoxes[sector_t::floor];
		if (sb->specialf1 > Z() - radius)
		{
			fixedvec2 refpos = { other->v1->fixX() + other->fixDx() / 2 + FLOAT2FIXED(sb->Scale.X), other->v1->fixY() + other->fixDy() / 2 + FLOAT2FIXED(sb->Scale.Y) };
			subsector_t *othersub = R_PointInSubsector(refpos.x, refpos.y);
			if (othersub->validcount != ::validcount) CollectWithinRadius(_f_PosRelative(othersub->sector), othersub, radius);
		}
	}
}

//==========================================================================
//
// Link the light into the world
//
//==========================================================================

void ADynamicLight::LinkLight()
{
	// mark the old light nodes
	FLightNode * node;
	
	node = touching_sides;
	while (node)
    {
		node->lightsource = NULL;
		node = node->nextTarget;
    }
	node = touching_subsectors;
	while (node)
    {
		node->lightsource = NULL;
		node = node->nextTarget;
    }
	node = touching_sector;
	while (node)
	{
		node->lightsource = NULL;
		node = node->nextTarget;
	}

	if (radius>0)
	{
		// passing in radius*radius allows us to do a distance check without any calls to sqrtf
		subsector_t * subSec = R_PointInSubsector(_f_X(), _f_Y());
		::validcount++;
		CollectWithinRadius(_f_Pos(), subSec, radius*radius);

	}
		
	// Now delete any nodes that won't be used. These are the ones where
	// m_thing is still NULL.
	
	node = touching_sides;
	while (node)
	{
		if (node->lightsource == NULL)
		{
			node = DeleteLightNode(node);
		}
		else
			node = node->nextTarget;
	}

	node = touching_subsectors;
	while (node)
	{
		if (node->lightsource == NULL)
		{
			node = DeleteLightNode(node);
		}
		else
			node = node->nextTarget;
	}

	node = touching_sector;
	while (node)
	{
		if (node->lightsource == NULL)
		{
			node = DeleteLightNode(node);
		}
		else
			node = node->nextTarget;
	}
}


//==========================================================================
//
// Deletes the link lists
//
//==========================================================================
void ADynamicLight::UnlinkLight ()
{
	if (owned && target != NULL)
	{
		// Delete reference in owning actor
		for(int c=target->dynamiclights.Size()-1; c>=0; c--)
		{
			if (target->dynamiclights[c] == this)
			{
				target->dynamiclights.Delete(c);
				break;
			}
		}
	}
	while (touching_sides) touching_sides = DeleteLightNode(touching_sides);
	while (touching_subsectors) touching_subsectors = DeleteLightNode(touching_subsectors);
	while (touching_sector) touching_sector = DeleteLightNode(touching_sector);
}

void ADynamicLight::Destroy()
{
	UnlinkLight();
	Super::Destroy();
}


//==========================================================================
//
// Needed for garbage collection
//
//==========================================================================

size_t AActor::PropagateMark()
{
	for (unsigned i=0; i<dynamiclights.Size(); i++)
	{
		GC::Mark(dynamiclights[i]);
	}
	return Super::PropagateMark();
}



CCMD(listlights)
{
	int walls, sectors, subsecs;
	int allwalls=0, allsectors=0, allsubsecs = 0;
	int i=0;
	ADynamicLight * dl;
	TThinkerIterator<ADynamicLight> it;

	while ((dl=it.Next()))
	{
		walls=0;
		sectors=0;
		subsecs = 0;
		Printf("%s at (%f, %f, %f), color = 0x%02x%02x%02x, radius = %f ",
			dl->target? dl->target->GetClass()->TypeName.GetChars() : dl->GetClass()->TypeName.GetChars(),
			dl->X(), dl->Y(), dl->Z(), dl->args[LIGHT_RED], 
			dl->args[LIGHT_GREEN], dl->args[LIGHT_BLUE], dl->radius);
		i++;

		if (dl->target)
		{
			FTextureID spr = gl_GetSpriteFrame(dl->target->sprite, dl->target->frame, 0, 0, NULL);
			Printf(", frame = %s ", TexMan[spr]->Name.GetChars());
		}


		FLightNode * node;

		node=dl->touching_sides;

		while (node)
		{
			walls++;
			allwalls++;
			node = node->nextTarget;
		}

		node=dl->touching_subsectors;

		while (node)
		{
			allsubsecs++;
			subsecs++;
			node = node->nextTarget;
		}

		node = dl->touching_sector;

		while (node)
		{
			allsectors++;
			sectors++;
			node = node->nextTarget;
		}
		Printf("- %d walls, %d subsectors, %d sectors\n", walls, subsecs, sectors);

	}
	Printf("%i dynamic lights, %d walls, %d subsectors, %d sectors\n\n\n", i, allwalls, allsubsecs, allsectors);
}

CCMD(listsublights)
{
	for(int i=0;i<numsubsectors;i++)
	{
		subsector_t *sub = &subsectors[i];
		int lights = 0;

		FLightNode * node = sub->lighthead;
		while (node != NULL)
		{
			lights++;
			node = node->nextLight;
		}

		Printf(PRINT_LOG, "Subsector %d - %d lights\n", i, lights);
	}
}


