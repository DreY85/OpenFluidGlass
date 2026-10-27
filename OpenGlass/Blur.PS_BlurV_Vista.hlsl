#include "Blur.Common.hlsli"

minfloat4 main(PS_INPUT input) : SV_Target0
{
	minfloat4 background = 0;

	float4 fgBackground;
	float3 fgRimLight;
	LgCompose(input, fgBackground, fgRimLight);
	background = minfloat4(fgBackground);
	
	return minfloat4(
		minfloat3(color.xyz) + minfloat(1.0f - color.a) * background.xyz + minfloat3(fgRimLight),
		minfloat(color.a) + minfloat(1.0f - color.a) * background.a
	) * minfloat(LgMask(input.pos.xy));
}
