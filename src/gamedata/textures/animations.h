#pragma once

#include <stdint.h>
#include "name.h"
#include "textureid.h"
#include "tarray.h"
#include "s_soundinternal.h"
#include "firetexture.h"

struct FStandaloneAnimation
{
	double		SwitchTic;
	uint32_t	AnimIndex;
	uint16_t	CurFrame;
	bool		ok = false;
	uint8_t		AnimType;
};

static_assert(sizeof(FStandaloneAnimation) == sizeof(uint64_t)*2);

struct FFireTexture
{
	uint32_t Duration; // Duration before updates.
	uint64_t SwitchTime; // Absolute time before update.
	FGameTexture* texture;
};

struct FAnimDef
{
	struct FAnimFrame
	{
		uint32_t	SpeedMin;		// Speeds are in ms, not tics
		uint32_t	SpeedRange;
		FTextureID	FramePic;
	};

	FTextureID 	BasePic;
	uint16_t	NumFrames;
	uint16_t	CurFrame;
	uint8_t	AnimType;
	bool	bDiscrete;			// taken out of AnimType to have better control
	uint64_t	SwitchTime;			// Time to advance to next frame
	FAnimFrame* Frames;
	enum
	{
		ANIM_Forward,
		ANIM_Backward,
		ANIM_OscillateUp,
		ANIM_OscillateDown,
		ANIM_Random
	};

	void SetSwitchTime(uint64_t mstime);
};

struct FSwitchDef
{
	FTextureID PreTexture;		// texture to switch from
	FSwitchDef* PairDef;		// switch def to use to return to PreTexture
	uint16_t NumFrames;		// # of animation frames
	bool QuestPanel;	// Special texture for Strife mission
	FSoundID Sound;			// sound to play at start of animation. Changed to int to avoiud having to include s_sound here.
	struct frame		// Array of times followed by array of textures
	{					//   actual length of each array is <NumFrames>
		uint16_t TimeMin;
		uint16_t TimeRnd;
		FTextureID Texture;
	} frames[1];
};

struct FDoorAnimation
{
	FTextureID BaseTexture;
	FTextureID* TextureFrames;
	int NumTextureFrames;
	FName OpenSound;
	FName CloseSound;
};



class FTextureAnimator
{
	TMap<FTextureID, uint16_t> mAnimationIndices;
	TArray<FAnimDef> mAnimations;
	TArray<FSwitchDef*> mSwitchDefs;
	TArray<FDoorAnimation> mAnimatedDoors;
	TArray<FFireTexture> mFireTextures;

	void ParseAnim(FScanner& sc, ETextureType usetype);
	FAnimDef* ParseRangeAnim(FScanner& sc, FTextureID picnum, ETextureType usetype, bool missing);
	void ParsePicAnim(FScanner& sc, FTextureID picnum, ETextureType usetype, bool missing, TArray<FAnimDef::FAnimFrame>& frames);
	void ParseWarp(FScanner& sc);
	void ParseCanvasTexture(FScanner& sc);
	void ParseCameraTexture(FScanner& sc);
	void ParseFireTexture(FScanner& sc);
	FTextureID ParseFramenum(FScanner& sc, FTextureID basepicnum, ETextureType usetype, bool allowMissing);
	void ParseTime(FScanner& sc, uint32_t& min, uint32_t& max);

	void FixAnimations();
	void InitAnimated();
	void InitAnimDefs();
	void InitSwitchList();
	void ProcessSwitchDef(FScanner& sc);
	FSwitchDef* ParseSwitchDef(FScanner& sc, bool ignoreBad);
	void AddSwitchPair(FSwitchDef* def1, FSwitchDef* def2);
	void ParseAnimatedDoor(FScanner& sc);

public:

	~FTextureAnimator()
	{
		DeleteAll();
	}

	// Animation stuff
	FAnimDef* AddAnim(FAnimDef& anim);
	void DeleteAll();

	FAnimDef* AddSimpleAnim(FTextureID picnum, int animcount, uint32_t speedmin, uint32_t speedrange = 0);
	FAnimDef* AddComplexAnim(FTextureID picnum, const TArray<FAnimDef::FAnimFrame>& frames);

	FSwitchDef* FindSwitch(FTextureID texture);
	FDoorAnimation* FindAnimatedDoor(FTextureID picnum);
	void UpdateAnimations(uint64_t mstime);

	const TArray<FAnimDef>& GetAnimations() const { return mAnimations; }

	void Init()
	{
		DeleteAll();
		InitAnimated();
		InitAnimDefs();
		FixAnimations();
		InitSwitchList();
	}

	bool InitStandaloneAnimation(FStandaloneAnimation &animInfo, FTextureID tex, uint32_t curTic);
	FTextureID UpdateStandaloneAnimation(FStandaloneAnimation &animInfo, double curTic);
};

extern FTextureAnimator TexAnim;


