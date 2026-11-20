#include "Blur.Common.hlsli"

minfloat4 main(PS_INPUT input) : SV_Target0
{
	minfloat4 background = 0;

	float4 fgBackground;
	float3 fgRimLight;
	LgPaneSample pane;
	LgCompose(input, fgBackground, fgRimLight, pane);
	background = minfloat4(fgBackground);
	
	minfloat4 material = minfloat4(
		minfloat3(color.xyz) + minfloat(1.0f - color.a) * background.xyz + minfloat3(fgRimLight) + minfloat(saturate(fgBackground.a) * LgGrain(input.pos.xy)),
		minfloat(color.a) + minfloat(1.0f - color.a) * background.a
	);
	return minfloat4(lerp(float4(material), pane.backdrop, pane.amount)) * minfloat(LgMask(input.pos.xy));
}
