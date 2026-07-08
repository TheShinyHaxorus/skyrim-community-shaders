namespace TerrainShadows
{
	Texture2D<float2> ShadowHeightTexture : register(t60);

	float2 GetTerrainShadowUV(float2 xy)
	{
		return xy * SharedData::terraOccSettings.Scale.xy + SharedData::terraOccSettings.Offset.xy;
	}

	float GetTerrainZ(float norm_z)
	{
		return lerp(SharedData::terraOccSettings.ZRange.x, SharedData::terraOccSettings.ZRange.y, norm_z) - 256;
	}

	float2 GetTerrainZ(float2 norm_z)
	{
		return float2(GetTerrainZ(norm_z.x), GetTerrainZ(norm_z.y));
	}

	float GetTerrainShadow(const float3 worldPos, SamplerState samp)
	{
		if (!SharedData::terraOccSettings.EnableTerrainShadow)
			return 1.0;
		float2 rawHeight = ShadowHeightTexture.SampleLevel(samp, GetTerrainShadowUV(worldPos.xy), 0);
		// channels are equal on lit ridge/peak texels; the naive penumbra divide is then 0/0 -> saturate(NaN) flickers black
		if (rawHeight.x - rawHeight.y < 1e-4)
			return 1.0;
		float2 shadowHeight = GetTerrainZ(rawHeight);
		return saturate((worldPos.z - shadowHeight.y) / (shadowHeight.x - shadowHeight.y));
	}
}
