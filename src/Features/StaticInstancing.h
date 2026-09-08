#pragma once

#include "Feature.h"
#include "Utils/Game.h"

#include <d3d11.h>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

namespace RE::BSGraphics
{
	class Renderer;
	struct TriShape;
}

/**
 * @brief Coalesces runs of identical static shadow-caster draws into one instanced draw.
 *
 * The render thread submits every shadow caster through BSBatchRenderer::RenderPassImmediately,
 * which costs about 1.5 us per draw on the engine and driver side regardless of mesh size.
 * Clones of the same NIF share their GPU vertex and index buffers, so identical placed meshes
 * inside one shadow-map pass group can be drawn once with N instances.
 *
 * The feature detours RenderPassImmediately at its entry, buckets eligible passes by
 * (renderer data, technique, alpha state, material identity), and at the end of the pass group
 * replays singles unchanged and batches through one representative pass. The engine sets up
 * technique, material and per-geometry state for the representative as usual; a detour on
 * Renderer::DrawTriShape then issues DrawIndexedInstanced instead of DrawIndexed. Instance world
 * matrices live in a per-frame structured buffer read by the Utility vertex shader under the
 * STATIC_INSTANCING define.
 *
 * Only shadow-map techniques are handled. Those passes are depth-only, so reordering inside a
 * pass group is safe. Depth pre-pass techniques are left alone because Terrain Blending
 * sequences them statefully.
 *
 * With coalescing off the feature still classifies every Utility pass and reports how many
 * draws would collapse, which is the number that decides whether the feature is worth enabling
 * for a given load order.
 */
struct StaticInstancing : Feature
{
	virtual inline std::string GetName() override { return "Static Instancing"; }
	virtual inline std::string GetShortName() override { return "StaticInstancing"; }
	virtual inline std::string_view GetShaderDefineName() override { return "STATIC_INSTANCING"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type t) override { return t == RE::BSShader::Type::Utility; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kUtility; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;

	struct Settings
	{
		/** Coalesce eligible draws. Off keeps the classification and counters running (measure only). */
		bool Enabled = true;
		/** Also coalesce alpha-tested casters (foliage, fences). Their key carries the material pointer. */
		bool IncludeAlphaTested = false;
		/** Debug: route every eligible draw through the instanced path with one instance. Validates the
		 *  matrix path independently of batching; costs a little more than vanilla. */
		bool InstanceSingles = false;
		/** Smallest bucket that is drawn instanced. */
		uint32_t MinBatchSize = 2;
		/** Capacity of the per-frame instance matrix ring (48 bytes each). */
		uint32_t MaxInstancesPerFrame = 65536;
	};
	Settings settings;

	/** Per-frame counters. `last` is the completed frame, `frame` is being filled. */
	struct Counters
	{
		uint32_t utilityPasses = 0;     // every Utility pass seen inside a RenderBatches loop
		uint32_t eligible = 0;          // passes that could be instanced (alpha-tested included)
		uint32_t eligibleAlpha = 0;     // subset of eligible that is alpha-tested
		uint32_t uniqueKeys = 0;        // distinct batch keys among eligible passes
		uint32_t collapsible = 0;       // eligible passes beyond the first of each key
		uint32_t deferred = 0;          // passes actually held back for batching
		uint32_t collapsed = 0;         // draws removed by instancing
		uint32_t instancedDraws = 0;    // DrawIndexedInstanced calls issued
		uint32_t singlesReplayed = 0;   // deferred passes replayed one by one
		uint32_t ringOverflow = 0;      // buckets replayed one by one because the ring was full
		uint32_t pendingDropped = 0;    // representative pass never reached DrawTriShape
		uint32_t excludedTechnique = 0;
		uint32_t excludedGeometry = 0;
		uint32_t excludedSkinned = 0;
		uint32_t excludedProperty = 0;
		uint32_t excludedAlpha = 0;     // alpha-tested while IncludeAlphaTested is off
	};

	virtual void SetupResources() override;
	virtual void Reset() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void PostPostLoad() override;

	/** @brief Snapshot of the last completed frame's counters plus the active settings, for devbench. */
	json GetStatsJson() const;

	struct Hooks
	{
		struct BSBatchRenderer_RenderPassImmediately
		{
			static void thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSBatchRenderer_RenderBatches
		{
			static bool thunk(void* a_renderer, uint32_t* a_currentPass, uint32_t* a_bucketIndex, void* a_passIndexList, uint32_t a_renderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Renderer_DrawTriShape
		{
			static void thunk(RE::BSGraphics::Renderer* a_renderer, RE::BSGraphics::TriShape* a_triShape, uint32_t a_startIndex, uint32_t a_count);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install();
	};

private:
	struct BatchKey
	{
		const void* rendererData = nullptr;
		const void* material = nullptr;
		uint32_t technique = 0;
		uint32_t renderFlags = 0;
		uint32_t alpha = 0;

		bool operator==(const BatchKey&) const = default;
	};

	struct BatchKeyHash
	{
		size_t operator()(const BatchKey& k) const noexcept;
	};

	struct DeferredPass
	{
		RE::BSRenderPass* pass;
		uint32_t technique;
		uint32_t renderFlags;
		bool alphaTest;
		uint32_t bucket;
	};

	struct Bucket
	{
		BatchKey key;
		std::vector<uint32_t> passes;  // indices into `deferred`
		bool drawn = false;
	};

	struct PendingBatch
	{
		bool active = false;
		uint32_t count = 0;
		uint32_t base = 0;
		const RE::BSGraphics::TriShape* rendererData = nullptr;
	};

	enum class Verdict
	{
		kNotUtility,
		kTechnique,
		kGeometry,
		kSkinned,
		kProperty,
		kAlphaExcluded,
		kEligible,
	};

	bool Active() const { return loaded; }
	void BeginFrameIfNeeded();
	Verdict Classify(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags, BatchKey& o_key);
	void OnRenderPass(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags);
	void Flush();
	bool WriteInstances(const Bucket& a_bucket, uint32_t& o_base);
	void DrawInstanced(RE::BSGraphics::TriShape* a_triShape, uint32_t a_startIndex, uint32_t a_count, uint32_t a_instanceCount, uint32_t a_base);
	void ReleaseResources();

	Counters frame{};
	Counters last{};
	Util::FrameChecker frameChecker;

	std::vector<DeferredPass> deferred;
	std::vector<Bucket> buckets;
	std::unordered_map<BatchKey, uint32_t, BatchKeyHash> bucketLookup;
	std::unordered_map<BatchKey, uint32_t, BatchKeyHash> measure;  // every eligible pass, for the counters

	PendingBatch pending;
	uint32_t batchDepth = 0;  // >0 while inside BSBatchRenderer::RenderBatches
	bool flushing = false;
	bool hooksInstalled = false;

	ID3D11Buffer* instanceBuffer = nullptr;
	ID3D11ShaderResourceView* instanceSRV = nullptr;
	ID3D11Buffer* batchCB = nullptr;
	ID3D11Buffer* disabledCB = nullptr;
	uint32_t ringCapacity = 0;
	uint32_t ringUsed = 0;
	bool ringDiscardPending = true;
	bool noOverwriteSupported = false;
};
