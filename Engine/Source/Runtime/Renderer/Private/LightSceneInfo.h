// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	LightSceneInfo.h: Light scene info definitions.
=============================================================================*/

#pragma once

#include "CoreMinimal.h"
#include "RenderResource.h"
#include "Templates/ScopedPointer.h"
#include "Math/GenericOctreePublic.h"
#include "SceneManagement.h"
#include "Math/GenericOctree.h"
#include "PrimitiveSceneProxy.h"
#include "Templates/UniquePtr.h"

class FLightPrimitiveInteraction;
class FLightSceneInfo;
class FPrimitiveSceneInfoCompact;
class FScene;
class FViewInfo;

/**
 * The information needed to cull a light-primitive interaction.
 */
class FLightSceneInfoCompact
{
public:
	// XYZ: origin, W:sphere radius
	VectorRegister BoundingSphereVector;
	FLinearColor Color;
	// must not be 0
	FLightSceneInfo* LightSceneInfo;
	// e.g. LightType_Directional, LightType_Point or LightType_Spot
	uint32 LightType : LightType_NumBits;	
	uint32 bCastDynamicShadow : 1;
	uint32 bCastStaticShadow : 1;
	uint32 bStaticLighting : 1;
	uint32 bAffectReflection : 1;
	uint32 bAffectGlobalIllumination : 1;
	uint32 bCastRaytracedShadow : 1;

	/** Initializes the compact scene info from the light's full scene info. */
	void Init(FLightSceneInfo* InLightSceneInfo);

	/** Default constructor. */
	FLightSceneInfoCompact():
		LightSceneInfo(NULL)
	{}

	/** Initialization constructor. */
	FLightSceneInfoCompact(FLightSceneInfo* InLightSceneInfo)
	{
		Init(InLightSceneInfo);
	}

	/**
	 * Tests whether this light affects the given primitive.  This checks both the primitive and light settings for light relevance
	 * and also calls AffectsBounds.
	 *
	 * @param CompactPrimitiveSceneInfo - The primitive to test.
	 * @return True if the light affects the primitive.
	 */
	bool AffectsPrimitive(const FBoxSphereBounds& PrimitiveBounds, const FPrimitiveSceneProxy* PrimitiveSceneProxy) const;
};

/** Information for sorting lights. */
struct FSortedLightSceneInfo
{
	union
	{
		struct
		{
			// Note: the order of these members controls the light sort order!
			// Currently bTiledDeferredNotSupported is the MSB and LightType is LSB
			/** The type of light. */
			uint32 LightType : LightType_NumBits;
			/** Whether the light has a texture profile. */
			uint32 bTextureProfile : 1;
			/** Whether the light uses a light function. */
			uint32 bLightFunction : 1;
			/** Whether the light casts shadows. */
			uint32 bShadowed : 1;
			/** Whether the light uses lighting channels. */
			uint32 bUsesLightingChannels : 1;
			/** Whether the light is NOT a simple light - they always support tiled/clustered but may want to be selected separately. */
			uint32 bIsNotSimpleLight : 1;
			/** True if the light doesn't support tiled deferred, logic is inverted so that lights that DO support tiled deferred will sort first in list */
			uint32 bTiledDeferredNotSupported : 1;
			/** 
			 * True if the light doesn't support clustered deferred, logic is inverted so that lights that DO support clustered deferred will sort first in list 
			 * Super-set of lights supporting tiled, so the tiled lights will end up in the first part of this range.
			 */
			uint32 bClusteredDeferredNotSupported : 1;
		} Fields;
		/** Sort key bits packed into an integer. */
		int32 Packed;
	} SortKey;

	const FLightSceneInfo* LightSceneInfo;
	int32 SimpleLightIndex;

	/** Initialization constructor. */
	explicit FSortedLightSceneInfo(const FLightSceneInfo* InLightSceneInfo)
		: LightSceneInfo(InLightSceneInfo),
		SimpleLightIndex(-1)
	{
		SortKey.Packed = 0;
		SortKey.Fields.bIsNotSimpleLight = 1;
	}

	explicit FSortedLightSceneInfo(int32 InSimpleLightIndex)
		: LightSceneInfo(nullptr),
		SimpleLightIndex(InSimpleLightIndex)
	{
		SortKey.Packed = 0;
		SortKey.Fields.bIsNotSimpleLight = 0;
	}
};

/** 
 * Stores info about sorted lights and ranges. 
 * The sort-key in FSortedLightSceneInfo gives rise to the following order:
 *  [SimpleLights,Tiled/Clustered,LightFunction/Shadow/LightChannels/TextureProfile]
 */
struct FSortedLightSetSceneInfo
{
	int SimpleLightsEnd;
	int TiledSupportedEnd;
	int ClusteredSupportedEnd;

	/** First light with shadow map or */
	int AttenuationLightStart;

	FSimpleLightArray SimpleLights;
	TArray<FSortedLightSceneInfo, SceneRenderingAllocator> SortedLights;
};

template <>
struct TUseBitwiseSwap<FSortedLightSceneInfo>
{
	enum { Value = false };
};

/** The type of the octree used by FScene to find lights. */
typedef TOctree<FLightSceneInfoCompact,struct FLightOctreeSemantics> FSceneLightOctree;

/**
 * The information used to render a light.  This is the rendering thread's mirror of the game thread's ULightComponent.
 * FLightSceneInfo is internal to the renderer module and contains internal scene state.
 */
class FLightSceneInfo : public FRenderResource
{
public:
	/** The light's scene proxy. */
	FLightSceneProxy* Proxy;

	/** The list of dynamic primitives affected by the light. */
	FLightPrimitiveInteraction* DynamicInteractionOftenMovingPrimitiveList;

	FLightPrimitiveInteraction* DynamicInteractionStaticPrimitiveList;

	/** If bVisible == true, this is the index of the primitive in Scene->Lights. */
	int32 Id;

	/** The identifier for the primitive in Scene->PrimitiveOctree. */
	FOctreeElementId OctreeId;

	/** Tile intersection buffer for distance field shadowing, stored on the light to avoid reallocating each frame. */
	mutable TUniquePtr<class FLightTileIntersectionResources> TileIntersectionResources;

	mutable FVertexBufferRHIRef ShadowCapsuleShapesVertexBuffer;
	mutable FShaderResourceViewRHIRef ShadowCapsuleShapesSRV;

protected:

	/** 
	 * ShadowMap channel assigned in the forward renderer when a movable shadow casting light is added to the scene. 
	 * Used to pack shadow projections into channels of the light attenuation texture which is read in the base pass.
	 */
	int32 DynamicShadowMapChannel;

	/** True if the light is built. */
	uint32 bPrecomputedLightingIsValid : 1;

public:

	/** 
	 * True if the light is visible.  
	 * False if the light is invisible but still needed for previewing, which can only happen in the editor.
	 */
	uint32 bVisible : 1;

	/** 
	 * Whether to render light shaft bloom from this light. 
	 * For directional lights, the color around the light direction will be blurred radially and added back to the scene.
	 * for point lights, the color on pixels closer than the light's SourceRadius will be blurred radially and added back to the scene.
	 */
	uint32 bEnableLightShaftBloom : 1;

	/** Scales the additive color. */
	float BloomScale;

	/** Scene color must be larger than this to create bloom in the light shafts. */
	float BloomThreshold;

	/** Multiplies against scene color to create the bloom color. */
	FColor BloomTint;

	/** Number of dynamic interactions with statically lit primitives. */
	int32 NumUnbuiltInteractions;

	/** Cached value from the light proxy's virtual function, since it is checked many times during shadow setup. */
	bool bCreatePerObjectShadowsForDynamicObjects;

	/** The scene the light is in. */
	FScene* Scene;

	/** Initialization constructor. */
	FLightSceneInfo(FLightSceneProxy* InProxy, bool InbVisible);
	virtual ~FLightSceneInfo();

	/** Adds the light to the scene. */
	void AddToScene();

	/**
	 * If the light affects the primitive, create an interaction, and process children 
	 * @param LightSceneInfoCompact Compact representation of the light
	 * @param PrimitiveSceneInfoCompact Compact representation of the primitive
	 */
	void CreateLightPrimitiveInteraction(const FLightSceneInfoCompact& LightSceneInfoCompact, const FPrimitiveSceneInfoCompact& PrimitiveSceneInfoCompact);

	/** Removes the light from the scene. */
	void RemoveFromScene();

	/** Detaches the light from the primitives it affects. */
	void Detach();

	/** Octree bounds setup. */
	FORCEINLINE FBoxCenterAndExtent GetBoundingBox() const
	{
		FSphere BoundingSphere = Proxy->GetBoundingSphere();
		return FBoxCenterAndExtent(BoundingSphere.Center, FVector(BoundingSphere.W, BoundingSphere.W, BoundingSphere.W));
	}

	bool ShouldRenderLight(const FViewInfo& View) const;

	/** Encapsulates all View-Independent reasons to have this light render. */
	bool ShouldRenderLightViewIndependent() const
	{
		return !Proxy->GetColor().IsAlmostBlack()
			// Only render lights with dynamic lighting or unbuilt static lights
			&& (!Proxy->HasStaticLighting() || !IsPrecomputedLightingValid());
	}

	/** Encapsulates all View-Independent reasons to render ViewIndependentWholeSceneShadows for this light */
	bool ShouldRenderViewIndependentWholeSceneShadows() const
	{
		bool bShouldRenderLight = ShouldRenderLightViewIndependent();
		bool bCastDynamicShadow = Proxy->CastsDynamicShadow();
		
		// Also create a whole scene shadow for lights with precomputed shadows that are unbuilt
		const bool bCreateShadowToPreviewStaticLight =
			Proxy->HasStaticShadowing()
			&& bCastDynamicShadow
			&& !IsPrecomputedLightingValid();

		bool bShouldRenderShadow = bShouldRenderLight && bCastDynamicShadow && (!Proxy->HasStaticLighting() || bCreateShadowToPreviewStaticLight);
		return bShouldRenderShadow;
	}

	bool IsPrecomputedLightingValid() const;

	void SetDynamicShadowMapChannel(int32 NewChannel)
	{
		if (Proxy->HasStaticShadowing())
		{
			// This ensure would trigger if several static shadowing light intersects eachother and have the same channel.
			// ensure(Proxy->GetPreviewShadowMapChannel() == NewChannel);
		}
		else
		{
			DynamicShadowMapChannel = NewChannel;
		}
	}

	int32 GetDynamicShadowMapChannel() const
	{
		if (Proxy->HasStaticShadowing())
		{
			// Stationary lights get a channel assigned by ReassignStationaryLightChannels
			return Proxy->GetPreviewShadowMapChannel();
		}

		// Movable lights get a channel assigned when they are added to the scene
		return DynamicShadowMapChannel;
	}

	/** Hash function. */
	friend uint32 GetTypeHash(const FLightSceneInfo* LightSceneInfo)
	{
		return (uint32)LightSceneInfo->Id;
	}

	// FRenderResource interface.
	virtual void ReleaseRHI();
};

/** Defines how the light is stored in the scene's light octree. */
struct FLightOctreeSemantics
{
	enum { MaxElementsPerLeaf = 16 };
	enum { MinInclusiveElementsPerNode = 7 };
	enum { MaxNodeDepth = 12 };

	typedef TInlineAllocator<MaxElementsPerLeaf> ElementAllocator;

	FORCEINLINE static FBoxCenterAndExtent GetBoundingBox(const FLightSceneInfoCompact& Element)
	{
		return Element.LightSceneInfo->GetBoundingBox();
	}

	FORCEINLINE static bool AreElementsEqual(const FLightSceneInfoCompact& A,const FLightSceneInfoCompact& B)
	{
		return A.LightSceneInfo == B.LightSceneInfo;
	}
	
	FORCEINLINE static void SetElementId(const FLightSceneInfoCompact& Element,FOctreeElementId Id)
	{
		Element.LightSceneInfo->OctreeId = Id;
	}

	FORCEINLINE static void ApplyOffset(FLightSceneInfoCompact& Element, FVector Offset)
	{
		VectorRegister OffsetReg = VectorLoadFloat3_W0(&Offset);
		Element.BoundingSphereVector = VectorAdd(Element.BoundingSphereVector, OffsetReg);
	}
};