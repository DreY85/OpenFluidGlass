#ifndef BLUR_COMMON_HLSLI
#define BLUR_COMMON_HLSLI

#include "Common.hlsli"

cbuffer BlurVSConstants : register(b0)
{
	float2 viewportSize;
	float2 _vsPadding;
};

cbuffer BlurPSConstants : register(b0)
{
	float4 conv[8];
	float4 afterglow;
	float4 blurBalance;
	float4 color;
	float4 fallback;
	float4 lgRect;		// left, top, right, bottom of the glass, back-buffer pixels
	float4 lgTex;		// uv per pixel (x, y), corner radius in pixels, enabled
	float4 lgOptics;	// index of refraction, bezel width, glass thickness, refraction scale
	float4 lgLook;		// dispersion, saturation, readability, glass-to-background gap
	float4 lgRim;		// rim light strength, shine exponent, light angle in radians, bezel profile
	float4 lgFull;		// uv per pixel of the full-resolution copy (x, y), clarity, softness in pixels
};

Texture2D<float4> texture0 : register(t0);
Texture2D<float4> texture1 : register(t1);	// Liquid Glass: full-resolution copy of the backdrop
SamplerState sampler0 : register(s0);

struct VS_INPUT
{
	float2 pos : POSITION;
	float2 tex0 : TEXCOORD0;
	float4 tex1 : TEXCOORD1; // (uv0.xy, uv1.xy)
	float4 tex2 : TEXCOORD2; // (uv2.xy, uv3.xy)
	float4 tex3 : TEXCOORD3; // (uv4.xy, uv5.xy)
	float4 tex4 : TEXCOORD4; // (uv6.xy, uv7.xy)
};

struct PS_INPUT
{
	float4 pos : SV_POSITION0;
	float2 tex0 : TEXCOORD0;
	float4 tex1 : TEXCOORD1;
	float4 tex2 : TEXCOORD2;
	float4 tex3 : TEXCOORD3;
	float4 tex4 : TEXCOORD4;
};

static const minfloat3 grayFactor = minfloat3(0.2126, 0.7152, 0.0722);

//----------------------------------------------------------------------------------
// Liquid Glass
//
// The edge of the glass is a convex bezel. A ray coming straight from the eye refracts
// where it enters the glass (Snell's law, air 1.0 to glass ~1.5) and travels down through
// the glass thickness. Where it lands on the background, compared with where it would have
// landed without glass, is the displacement. A convex bezel pulls rays inward, so the
// edge shows the background from further inside, compressed, as on Apple's Liquid Glass.
//----------------------------------------------------------------------------------

float LgDistance(float2 p)
{
	float2 halfSize = (lgRect.zw - lgRect.xy) * 0.5f;
	float r = min(lgTex.z, min(halfSize.x, halfSize.y));
	float2 q = abs(p - (lgRect.xy + lgRect.zw) * 0.5f) - halfSize + r;
	return length(max(q, 0.0f)) + min(max(q.x, q.y), 0.0f) - r;
}

// Bezel height from the outer edge (u = 0) to the flat top (u = 1).
float LgConvex(float u)
{
	float k = 1.0f - u;
	return lgRim.w > 0.5f && lgRim.w < 1.5f ? sqrt(max(1.0f - k * k, 0.0f)) : pow(max(1.0f - k * k * k * k, 0.0f), 0.25f);
}
float LgHeight(float u)
{
	if (lgRim.w > 1.5f)
	{
		float s = u * u * u * (u * (u * 6.0f - 15.0f) + 10.0f);
		return lerp(LgConvex(u), 1.0f - LgConvex(u), s);
	}
	return LgConvex(u);
}

// How far, sideways, a vertical ray lands after refracting at a surface with normal N
// and travelling depth pixels down.
float LgLateral(float2 N, float eta, float depth)
{
	float2 r = refract(float2(0.0f, -1.0f), N, eta);
	return r.y < -1e-3f ? r.x / -r.y * depth : 0.0f;
}

float4 LgBlurAt(PS_INPUT input, float2 duv)
{
	float4 s = 0;
	s += conv[0].x * texture0.Sample(sampler0, input.tex1.xy + duv);
	s += conv[1].x * texture0.Sample(sampler0, input.tex1.zw + duv);
	s += conv[2].x * texture0.Sample(sampler0, input.tex2.xy + duv);
	s += conv[3].x * texture0.Sample(sampler0, input.tex2.zw + duv);
	s += conv[4].x * texture0.Sample(sampler0, input.tex3.xy + duv);
	s += conv[5].x * texture0.Sample(sampler0, input.tex3.zw + duv);
	s += conv[6].x * texture0.Sample(sampler0, input.tex4.xy + duv);
	s += conv[7].x * texture0.Sample(sampler0, input.tex4.zw + duv);
	return s;
}

// The sharp backdrop at a full-resolution uv, softened by a small cross of samples.
float4 LgSharpAt(float2 uv)
{
	float2 o = lgFull.xy * lgFull.w;
	float4 s = texture1.Sample(sampler0, uv) * 0.4f;
	s += texture1.Sample(sampler0, uv + float2(o.x, 0.0f)) * 0.15f;
	s += texture1.Sample(sampler0, uv - float2(o.x, 0.0f)) * 0.15f;
	s += texture1.Sample(sampler0, uv + float2(0.0f, o.y)) * 0.15f;
	s += texture1.Sample(sampler0, uv - float2(0.0f, o.y)) * 0.15f;
	return s;
}

// The blurred background seen through the glass, plus the light caught by the rim.
void LgCompose(PS_INPUT input, out float4 background, out float3 rimLight)
{
	background = 0;
	rimLight = 0;
	if (lgTex.w < 0.5f)
	{
		background = LgBlurAt(input, 0);
		return;
	}

	float2 p = input.pos.xy;
	float d = LgDistance(p);
	// Edge normal, pointing out of the glass.
	float2 e = float2(1.0f, 0.0f);
	float2 n = normalize(float2(LgDistance(p + e.xy) - LgDistance(p - e.xy), LgDistance(p + e.yx) - LgDistance(p - e.yx)) + 1e-6f);

	float bezel = max(lgOptics.y, 1.0f);
	float u = saturate(-d / bezel);
	float slope = u >= 1.0f ? 0.0f : lgOptics.z / bezel * (LgHeight(min(u + 0.002f, 1.0f)) - LgHeight(max(u - 0.002f, 0.0f))) / 0.004f;
	// Cross-section through the edge: x points inward, y points up. The surface normal leans outward.
	float2 N = normalize(float2(-slope, 1.0f));
	float depth = lgOptics.z * LgHeight(u) + lgLook.w;
	float ior = lgOptics.x;
	// Sideways shift in pixels for each channel; dispersion bends red less and blue more.
	float2 pg = -n * LgLateral(N, 1.0f / ior, depth) * lgOptics.w;
	float2 pr = -n * LgLateral(N, 1.0f / max(ior - lgLook.x, 1.0001f), depth) * lgOptics.w;
	float2 pb = -n * LgLateral(N, 1.0f / (ior + lgLook.x), depth) * lgOptics.w;

	float4 g = LgBlurAt(input, pg * lgTex.xy);
	background = float4(LgBlurAt(input, pr * lgTex.xy).r, g.g, LgBlurAt(input, pb * lgTex.xy).b, g.a);
	if (lgFull.z > 0.0f)
	{
		float2 uv = p * lgFull.xy;
		float4 sg = LgSharpAt(uv + pg * lgFull.xy);
		float3 sharp = float3(LgSharpAt(uv + pr * lgFull.xy).r, sg.g, LgSharpAt(uv + pb * lgFull.xy).b);
		background.rgb = lerp(background.rgb, sharp, lgFull.z);
	}

	float luminance = dot(background.rgb, float3(0.2126f, 0.7152f, 0.0722f));
	background.rgb = max(lerp(luminance.xxx, background.rgb, lgLook.y), 0.0f);
	// Readability: where the background is too bright for white text, the glass darkens it.
	luminance = dot(background.rgb, float3(0.2126f, 0.7152f, 0.0722f));
	background.rgb *= lerp(1.0f, min(1.0f, 0.22f / max(luminance, 1e-3f)), lgLook.z);

	// Rim light: how steep the bezel is and how much it faces the light.
	// The opposite edge catches a weaker reflection, like light leaving through the far side.
	float2 L = float2(cos(lgRim.z), sin(lgRim.z));
	float facing = pow(max(dot(n, L), 0.0f), lgRim.y) + 0.4f * pow(max(dot(n, -L), 0.0f), lgRim.y);
	rimLight = ((1.0f - N.y) * facing * lgRim.x).xxx;
}

#endif // BLUR_COMMON_HLSLI
