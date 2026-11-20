#include "Blur.Common.hlsli"

minfloat4 main(PS_INPUT input) : SV_Target0
{
	minfloat4 background = 0;

	float4 fgBackground;
	float3 fgRimLight;
	LgPaneSample pane;
	LgCompose(input, fgBackground, fgRimLight, pane);
	background = minfloat4(fgBackground);

	minfloat coverage = saturate(background.a);
	minfloat luminance = dot(background.rgb, grayFactor);
	minfloat3 result = minfloat3(blurBalance.xxx) * background.rgb;
	result = luminance * minfloat3(afterglow.xyz) + result;
	result = coverage * minfloat3(color.xyz) + result;

	minfloat4 fallbackColor = minfloat4(fallback);
	result += minfloat(1.0f - coverage) * fallbackColor.rgb;
	minfloat4 material = minfloat4(
		result + minfloat3(fgRimLight) + coverage * minfloat(LgGrain(input.pos.xy)),
		coverage + minfloat(1.0f - coverage) * fallbackColor.a
	);
	return minfloat4(lerp(float4(material), pane.backdrop, pane.amount)) * minfloat(LgMask(input.pos.xy));
}
