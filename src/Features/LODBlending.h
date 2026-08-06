#pragma once

#include "Buffer.h"

struct LODBlending : Feature
{
	virtual inline std::string GetName() override { return "LOD Blending"; }
	virtual std::string GetDisplayName() override { return T("feature.lod_blending.name", "LOD Blending"); }
	virtual inline std::string GetShortName() override { return "LODBlending"; }
	virtual inline std::string_view GetShaderDefineName() override { return "LOD_BLENDING"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLandscapeAndTextures; }
	/** @brief Returns a localized description and list of key features for the UI summary panel. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.lod_blending.description", "Provides seamless visual transitions between Level of Detail (LOD) objects and full-detail objects, eliminating harsh transitions and creating smooth visual continuity."),
			{ T("feature.lod_blending.key_feature_1", "Smooth LOD object brightness blending"),
				T("feature.lod_blending.key_feature_2", "Enhanced terrain LOD appearance matching"),
				T("feature.lod_blending.key_feature_3", "Snow-specific LOD brightness adjustment"),
				T("feature.lod_blending.key_feature_4", "Optional terrain vertex color modification"),
				T("feature.lod_blending.key_feature_5", "Seamless transition between detail levels") } };
	};

	virtual inline bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	struct Settings
	{
		float LODTerrainBrightness = 1;
		float LODObjectBrightness = 1;
		float LODObjectSnowBrightness = 1;
		uint DisableTerrainVertexColors = false;
		float LODTerrainGamma = 1;
		float LODObjectGamma = 1;
		float LODObjectSnowGamma = 1;
		uint EnableTreeWind = true;
		float TreeWindAmplitude = 0.04f;
		float TreeWindSpeed = 1.f;
		uint EnableLODObjectTreeWind = true;
		float LODObjectTreeWindAmplitude = 20.f;
		float LODObjectTreeHeight = 1200.f;
	};

	Settings settings;

	/** @brief Layout mirrored by SharedData::LODBlendingSettings; keep field order in sync. */
	struct BufferData
	{
		Settings settings;
		float TreeWindTimer;
		float TreeWindPreviousTimer;
		float LODBlendingPad0;
	};

	static_assert(sizeof(BufferData) % 16 == 0);
	static_assert(sizeof(BufferData) == 64);

	/** @brief Advances the wind timer history at most once per frame. */
	void StepWindTimer();

	/** @brief Returns the packed shader-visible data, advancing the wind timer once per frame. */
	BufferData GetCommonBufferData();

	/** @brief Binds the shared/feature cbuffers to the vertex stage for distant tree draws. */
	void Update();

	/** @brief Layout mirrored by the LODTreeWind cbuffer (b4) in Lighting.hlsl. */
	struct alignas(16) LODTreeWindData
	{
		float Enabled;
		float InvTreeHeight;
		float Amplitude;
		float Speed;

		float Timer;
		float PreviousTimer;
		float Pad0;
		float Pad1;

		float2 MapScale;
		float2 MapOffset;

		float2 MapZRange;
		float2 MapDimensions;
	};

	static_assert(sizeof(LODTreeWindData) == 64);

	/**
	 * @brief Sways DynDOLOD 3D/Ultra tree LOD, which is merged into the object LOD.
	 *
	 * Those shapes never reach DistantTree.hlsl, carry no per-instance data and share one
	 * merged shape per block, so the per-vertex wind weight comes from the vertex's height
	 * above the Terrain Shadows worldspace heightmap rather than from anything per-shape.
	 */
	void UpdateLODObjectTreeWind(RE::BSRenderPass* a_pass);

	void SetupLODTreeWindResources();

	/** @brief Draws the ImGui settings UI for LOD brightness, gamma, and vertex color configuration. */
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void RestoreDefaultSettings() override;

	virtual void PostPostLoad() override;

	virtual bool IsCore() const override { return true; };

	float windTimer = 0;
	float previousWindTimer = 0;
	uint lastWindFrame = std::numeric_limits<uint>::max();

	std::unique_ptr<ConstantBuffer> lodTreeWindCB;
	bool lodTreeWindActive = false;
	bool lodTreeWindEverBound = false;

	struct Hooks
	{
		struct BSDistantTreeShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct LODBlending_BSLightingShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_vfunc<0x6, BSDistantTreeShader_SetupGeometry>(RE::VTABLE_BSDistantTreeShader[0]);
			stl::write_vfunc<0x6, LODBlending_BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
			logger::info("[LOD BLENDING] Installed hooks");
		}
	};
};
