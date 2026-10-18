#include "Blur.Common.hlsli"

minfloat4 main(PS_INPUT input) : SV_Target0
{
	minfloat4 background = 0;

	float4 lgBackground;
	float3 lgRimLight;
	LgCompose(input, lgBackground, lgRimLight);
	background = minfloat4(lgBackground);

	minfloat coverage = saturate(background.a);
	minfloat luminance = dot(background.rgb, grayFactor);
	minfloat3 result = minfloat3(blurBalance.xxx) * background.rgb;
	result = luminance * minfloat3(afterglow.xyz) + result;
	result = coverage * minfloat3(color.xyz) + result;

	minfloat4 fallbackColor = minfloat4(fallback);
	result += minfloat(1.0f - coverage) * fallbackColor.rgb;
	return minfloat4(
		result + minfloat3(lgRimLight),
		coverage + minfloat(1.0f - coverage) * fallbackColor.a
	);
}
