#pragma once

#include "Buffer.h"

#include <memory>
#include <winrt/base.h>

/** @brief Enhances water rendering with realistic caustics and underwater lighting effects. */
struct WaterEffects : Feature
{
public:
	winrt::com_ptr<ID3D11ShaderResourceView> causticsView;

	struct Settings
	{
		bool EnableUnderwaterDistortion = true;
		float BlurAmount = 1.0f;
		float BlurRange = 1.2f;
		float DistortionAmount = 1.0f;
		float DistortionScale = 16.0f;
		float DistortionSpeed = 8.0f;
		float VerticalAmount = 1.0f;
		float NoiseScale = 8.0f;
		float TransitionRange = 32.0f;
	};

	Settings settings;

	struct alignas(16) UnderwaterCB
	{
		float4 Params0;
		float4 Params1;
		float4 ScreenParams;
		float4 DynRes;
	};

	virtual inline std::string GetName() override { return "Water Effects"; }
	virtual std::string GetDisplayName() override { return T("feature.water_effects.name", "Water Effects"); }
	/** @brief Returns the short identifier used for file paths and logging. */
	virtual inline std::string GetShortName() override { return "WaterEffects"; }
	virtual inline std::string_view GetShaderDefineName() override { return "WATER_EFFECTS"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kWater; }

	/** @brief Returns a summary description and list of key features for the UI. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.water_effects.description", "Water Effects enhances water rendering with realistic caustics and underwater lighting effects.\nThis feature adds dynamic light patterns and improved water visual quality."),
			{ T("feature.water_effects.key_feature_1", "Realistic water caustics"),
				T("feature.water_effects.key_feature_2", "Enhanced underwater lighting"),
				T("feature.water_effects.key_feature_3", "Dynamic light patterns on water surfaces"),
				T("feature.water_effects.key_feature_4", "Improved water visual fidelity"),
				T("feature.water_effects.key_feature_5", "Atmospheric underwater effects") } };
	};

	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	/** @brief Loads the water caustics DDS texture and creates the underwater post-process resources. */
	virtual void SetupResources() override;

	/** @brief Releases the transient underwater render targets (recreated on demand). */
	virtual void Reset() override;

	/** @brief Binds the caustics texture SRV to the pixel shader for the current frame. */
	virtual void Prepass() override;

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void ClearShaderCache() override;

	/**
	 * @brief Applies the ENB-derived underwater distortion/blur post-process to the tonemapped image.
	 *
	 * Runs only while the camera is submerged. Reads a copy of the output target as the scene color,
	 * applies two depth-weighted distort/blur passes and a final surface-line composite, and writes
	 * the result back into the output render target.
	 * @param a_output The render target holding the final tonemapped color.
	 */
	void DrawUnderwater(RE::RENDER_TARGET a_output);

	virtual bool IsCore() const override { return true; };

private:
	ID3D11VertexShader* underwaterVS = nullptr;
	ID3D11PixelShader* underwaterBlurPS = nullptr;
	ID3D11PixelShader* underwaterDrawPS = nullptr;

	std::unique_ptr<Texture2D> sceneColorCopy;
	std::unique_ptr<Texture2D> blurTargetA;
	std::unique_ptr<Texture2D> blurTargetB;
	std::unique_ptr<ConstantBuffer> underwaterCB;

	winrt::com_ptr<ID3D11SamplerState> linearClampSampler;
	winrt::com_ptr<ID3D11SamplerState> pointClampSampler;
	winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
	winrt::com_ptr<ID3D11BlendState> opaqueBlendState;
	winrt::com_ptr<ID3D11DepthStencilState> noDepthState;

	uint32_t targetWidth = 0;
	uint32_t targetHeight = 0;
	DXGI_FORMAT targetFormat = DXGI_FORMAT_UNKNOWN;

	ID3D11VertexShader* GetUnderwaterVS();
	ID3D11PixelShader* GetUnderwaterBlurPS();
	ID3D11PixelShader* GetUnderwaterDrawPS();
	bool EnsureUnderwaterTargets(const D3D11_TEXTURE2D_DESC& a_outputDesc, DXGI_FORMAT a_srvFormat, DXGI_FORMAT a_rtvFormat);
};
