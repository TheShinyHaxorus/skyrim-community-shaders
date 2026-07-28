Texture2D<float4> SourceTexture : register(t0);
SamplerState LinearSampler : register(s0);

struct PS_INPUT
{
	float4 pos : SV_POSITION;
	float2 txcoord0 : TEXCOORD0;
};

// The target is R11G11B10_FLOAT, whose 10-bit channel saturates at 64512. Scene values above
// that (or a stray NaN) encode as INF here, and GenerateMips then smears it over the whole mip
// chain that feeds bloom and the temporal adaptation loop, which cannot recover on its own.
static const float MaxDownsampleValue = 64512.0;

float4 main(PS_INPUT input) : SV_Target
{
	float4 color = SourceTexture.SampleLevel(LinearSampler, input.txcoord0.xy, 0);
	return isfinite(color) ? clamp(color, 0.0, MaxDownsampleValue) : 0.0;
}
