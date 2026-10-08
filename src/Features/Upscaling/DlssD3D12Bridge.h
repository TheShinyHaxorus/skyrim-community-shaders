#pragma once

// D3D11 to D3D12 bridge for DLSS 5 Neural Rendering, which is an NGX D3D12 feature.
//
// The pass runs at render resolution, one to one, on the colour input BEFORE super resolution
// (the FO4 reference order): the render-resolution rectangle of the game's colour is copied into a
// shared texture of exactly that size, the model rewrites it in place on a D3D12 command list, the
// rectangle is copied back into the game's colour, and DLSS then upscales it on D3D11 as usual.
// It uses the frame generation swap chain's D3D12 device when one exists and creates one otherwise,
// so neural rendering does not depend on frame generation.
//
// Modelled on FidelityFX::DispatchRuntimeUpscalerSingle: one shared fence pair, a small ring of
// command allocators/lists, and D3D11 waits on the D3D12 signal before it reads the output.

#include <array>
#include <cstdint>
#include <d3d11_4.h>
#include <d3d12.h>
#include <string>
#include <winrt/base.h>

class WrappedResource;

class DlssD3D12Bridge
{
public:
	struct Inputs
	{
		ID3D11Resource* dlssInput = nullptr;  ///< Game colour (kMAIN); its top-left render-resolution rectangle is rewritten in place
		ID3D11Resource* depth = nullptr;      ///< Typed R32_FLOAT depth (render-resolution region)
		ID3D11Resource* motionVectors = nullptr;
		uint32_t renderWidth = 0;
		uint32_t renderHeight = 0;
		bool resetHistory = false;
		float jitterDeltaX = 0.0f;  ///< Current minus previous frame's render-resolution jitter, pixels; for the neural rendering model
		float jitterDeltaY = 0.0f;
	};

	~DlssD3D12Bridge();

	/** @brief True after the D3D12 device, fences and command ring exist. */
	bool IsReady() const;

	/** @brief Runs neural rendering for this frame. Returns true when the game's colour holds the rewritten rectangle. */
	bool Dispatch(const Inputs& a_inputs);

	/** @brief Drops shared textures (size or format change). The device and fences stay. */
	void ReleaseSharedResources(bool a_waitForIdle = true);

	/** @brief Full teardown. */
	void Destroy();

	/** @brief Set when the last dispatch faulted; the bridge refuses further work this session. */
	bool IsQuarantined() const { return quarantined; }
	const std::string& GetQuarantineReason() const { return quarantineReason; }

private:
	static constexpr uint32_t kCommandContextCount = 4;
	struct CommandContext
	{
		winrt::com_ptr<ID3D12CommandAllocator> allocator;
		winrt::com_ptr<ID3D12GraphicsCommandList4> list;
		uint64_t fenceValue = 0;
	};

	bool EnsureInterop();
	bool EnsureCommandContexts();
	CommandContext* AcquireCommandContext();
	bool WaitForFence(uint64_t a_value);
	void WaitForIdle();
	bool EnsureSharedResources(const Inputs& a_inputs);
	void Quarantine(const std::string& a_reason);

	/// A caller-owned D3D11 texture created with D3D11_RESOURCE_MISC_SHARED_NTHANDLE, opened on
	/// the D3D12 device so the bridge reads or writes it in place. Falls back to the copy path
	/// (resource stays null) when the texture is not shareable.
	struct Alias
	{
		ID3D11Resource* source = nullptr;
		winrt::com_ptr<ID3D12Resource> resource;
	};
	bool OpenAlias(Alias& a_alias, ID3D11Resource* a_source);
	Alias aliasDepth, aliasMotion;

	winrt::com_ptr<ID3D11Fence> d3d11Fence;
	winrt::com_ptr<ID3D12Fence> d3d12Fence;
	uint64_t fenceValue = 1;
	std::array<CommandContext, kCommandContextCount> commandContexts{};
	uint32_t commandContextCursor = 0;

	WrappedResource* colorInShared = nullptr;
	WrappedResource* depthShared = nullptr;
	WrappedResource* motionShared = nullptr;

	D3D11_TEXTURE2D_DESC colorInDesc{};
	D3D11_TEXTURE2D_DESC depthDesc{};
	D3D11_TEXTURE2D_DESC motionDesc{};

	bool quarantined = false;
	std::string quarantineReason;
};
