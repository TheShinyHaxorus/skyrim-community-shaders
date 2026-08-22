#pragma once
// Disable warning about structure padding due to alignment specifier
#pragma warning(disable: 4324)

/**
 * Motion Blur Effect
 *
 * Three-pass compute shader approach based on CoD:AW:
 * 1. Reduction: Horizontal pass to [grid×height], then vertical to [grid×grid]
 * 2. Neighbor: Calculate neighborhood max velocities in 3×3 grid groups
 * 3. Blur: Apply motion blur using neighborhood velocities and depth comparison
 */

#include "../../Buffer.h"
#include "PostProcessFeature.h"
#include <EASTL/unique_ptr.h>

struct MotionBlur : public PostProcessFeature
{
	// Feature interface
	inline std::string GetType() const override { return "Motion Blur"; }
	inline std::string GetDisplayName() const override { return T("feature.post_processing.motion_blur.name", "Motion Blur"); }
	inline std::string GetDesc() const override
	{
		return T("feature.post_processing.motion_blur.description", "Creates cinematic motion blur based on camera and object movement.");
	}
	virtual inline bool DisableInMainLoadingMenu() const override { return true; }

	// Constants
	static constexpr float MaxBlurRadiusAt1080p = 40.0f;
	static constexpr float ReferenceHeight = 1080.0f;
	static constexpr int FixedGridSize = 20;

	// Shutter presets
	enum class MotionScale
	{
		VeryShort = 0,
		Short = 1,
		Medium = 2,
		Long = 3,
		VeryLong = 4
	};

	// Settings
	struct Settings
	{
		int SampleCount = 8;  // Doubled internally before sending to shader
		MotionScale ScalePreset = MotionScale::Medium;
	};
	Settings settings;

	float GetShutterFromPreset(MotionScale preset) const
	{
		switch (preset) {
		case MotionScale::VeryShort:
			return 0.125f;
		case MotionScale::Short:
			return 0.25f;
		case MotionScale::Medium:
			return 0.5f;
		case MotionScale::Long:
			return 0.75f;
		case MotionScale::VeryLong:
			return 1.0f;
		default:
			return 0.5f;
		}
	}

	// D3D Resources
	winrt::com_ptr<ID3D11SamplerState> linearSampler;
	winrt::com_ptr<ID3D11SamplerState> pointSampler;

	// Compute shaders
	winrt::com_ptr<ID3D11ComputeShader> horizontalPassShader;   // Pass 1a
	winrt::com_ptr<ID3D11ComputeShader> verticalPassShader;     // Pass 1b
	winrt::com_ptr<ID3D11ComputeShader> neighborMaxPassShader;  // Pass 2
	winrt::com_ptr<ID3D11ComputeShader> blurPassShader;         // Pass 3

	// ConstantBuffer wrapper objects
	eastl::unique_ptr<ConstantBuffer> blurConstantBufferObj;
	eastl::unique_ptr<ConstantBuffer> reductionPassConstantBufferObj;

	// Textures
	eastl::unique_ptr<Texture2D> horizontalPassTexture;  // [grid x height]
	eastl::unique_ptr<Texture2D> verticalPassTexture;    // [grid x grid]
	eastl::unique_ptr<Texture2D> neighborMaxTexture;     // [grid x grid]
	eastl::unique_ptr<Texture2D> blurOutputTexture;      // Full resolution

	// Dimensions tracking
	uint32_t lastWidth = 0;
	uint32_t lastHeight = 0;
	uint32_t dynamicWidth = 0;   // Dynamic resolution width for dispatch (not for resource sizing)
	uint32_t dynamicHeight = 0;  // Dynamic resolution height for dispatch (not for resource sizing)

	// Constant buffer structs
	struct alignas(16) MotionBlurConstantBuffer
	{
		float4 VelocityParams;    // x: shutter fraction, yz: motion-vector source scale, w: max blur radius in px
		float4 TargetResolution;  // xy: rendered area of the color target, in pixels
		float4 DepthParams;       // Util::GetCameraData(): far, near, far - near, far * near
		int32_t SampleCount;
		float Padding[3] = {};
	};

	struct alignas(16) ReductionPassConstantBuffer
	{
		float4 VelocityParams;    // x: shutter fraction, yz: motion-vector source scale, w: max blur radius in px
		float4 TargetResolution;  // xy: rendered area of the color target, in pixels
	};

	// CB instances
	MotionBlurConstantBuffer motionBlurCB;
	ReductionPassConstantBuffer reductionPassCB;

	// Cache for optimization
	MotionBlurConstantBuffer lastMotionBlurCB = {};
	ReductionPassConstantBuffer lastReductionPassCB = {};

	// Interface methods
	void SetupResources() override;
	void ClearShaderCache() override;
	void RestoreDefaultSettings() override;
	void LoadSettings(json&) override;
	void SaveSettings(json&) override;
	void DrawSettings() override;
	void Draw(TextureInfo&) override;

	// Helper methods
	void CompileComputeShaders();
	bool CheckAndResizeResources(const TextureInfo& inout_tex);
	bool UpdateConstantBuffers();
	void SetupComputePass(ID3D11ComputeShader* shader,
		ID3D11ShaderResourceView** srvs,
		uint32_t srvCount,
		ID3D11UnorderedAccessView* uav,
		ID3D11Buffer* constantBuffer);
	void ClearComputeResources(uint32_t srvCount = 1);
	void ExecuteHorizontalPass();
	void ExecuteVerticalPass();
	void ExecuteNeighborMaxPass();
	void ExecuteBlurPass(TextureInfo& inout_tex);
};
