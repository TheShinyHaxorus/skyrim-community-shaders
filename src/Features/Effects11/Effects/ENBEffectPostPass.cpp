#include "ENBEffectPostPass.h"

#include "../TextureManager.h"

void ENBEffectPostPass::Execute()
{
	auto& textureManager = TextureManager::GetSingleton();

	auto textureSDRTemp = textureManager.GetCommonTexture("TextureSDRTemp");
	auto textureSDRTemp2 = textureManager.GetCommonTexture("TextureSDRTemp2");

	if (!textureSDRTemp || !textureSDRTemp2) {
		return;
	}

	auto& scratch = effectTextureCache["TextureScratch"];
	if (!scratch.rtv) {
		return;
	}

	const auto& techniqueName = GetSelectedTechnique();

	uint32_t enabledTechniques = 0;
	if (auto it = techniques.find(techniqueName); it != techniques.end()) {
		for (auto& info : it->second) {
			if (info.technique && IsTechniqueEnabled(info))
				++enabledTechniques;
		}
	}

	auto [executed, inOutput] = ExecuteTechniqueSequence(techniqueName, textureSDRTemp->srv.get(), *textureSDRTemp2, scratch);

	if (!executed)
		return;

	if (inOutput) {
		textureManager.SwapTextures("TextureSDRTemp", "TextureSDRTemp2");
	} else if (enabledTechniques > 1) {
		auto* context = globals::d3d::context;
		ID3D11ShaderResourceView* nullSRVs[8] = {};
		context->PSSetShaderResources(0, 8, nullSRVs);
		context->CopyResource(textureSDRTemp->texture.get(), scratch.texture.get());
	}
}

void ENBEffectPostPass::CreateEffectTextures()
{
	auto* graphicsState = globals::game::graphicsState;
	if (!graphicsState)
		return;

	effectTextureCache["TextureScratch"] = CreateTexture(graphicsState->screenWidth, graphicsState->screenHeight,
		DXGI_FORMAT_R10G10B10A2_UNORM, "ENBEffectPostPass::TextureScratch");
}

void ENBEffectPostPass::UpdateEffectVariables()
{
	auto* textureSDRTemp = GetCachedCommonTexture("TextureSDRTemp");
	SetShaderResourceVariable("TextureOriginal", textureSDRTemp ? textureSDRTemp->srv.get() : nullptr);
}