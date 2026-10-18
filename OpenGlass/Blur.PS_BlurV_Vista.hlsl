#include "Blur.Common.hlsli"

minfloat4 main(PS_INPUT input) : SV_Target0
{
	minfloat4 background = 0;

	float4 lgBackground;
	float3 lgRimLight;
	LgCompose(input, lgBackground, lgRimLight);
	background = minfloat4(lgBackground);
	
	return minfloat4(
		minfloat3(color.xyz) + minfloat(1.0f - color.a) * background.xyz + minfloat3(lgRimLight),
		minfloat(color.a) + minfloat(1.0f - color.a) * background.a
	);
}
