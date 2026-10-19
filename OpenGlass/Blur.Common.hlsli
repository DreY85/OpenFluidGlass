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
	float4 lgLimit;		// longest refraction shift in pixels, 1 for a pill cut to its rounded outline
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

// Depth inside the glass outline, in pixels; zero on the outline.
// Each contour at depth t is the outline shrunk by t with its corner radius grown to r + t.
// Plain distance keeps the corner radius shrinking to zero, so with a bezel wider than the
// window's corner radius the contours meet at a sharp diagonal and the refraction tears along it.
// A pill's bezel reaches its middle, where a growing radius would outgrow the shrinking contour
// (r + t > shortHalf - t) and the top and bottom contours would meet in a crease along the middle line.
// So from that depth on its contours keep the radius they can hold, shortHalf - t: concentric round ends.
float LgDepth(float2 p)
{
	float2 halfSize = (lgRect.zw - lgRect.xy) * 0.5f;
	float shortHalf = min(halfSize.x, halfSize.y);
	float r = min(lgTex.z, shortHalf);
	float2 offset = abs(p - (lgRect.xy + lgRect.zw) * 0.5f);
	float2 A = offset - halfSize + r;
	// Straight sides: the contour radius cancels out.
	float t = r - max(A.x, A.y);
	if (min(A.x, A.y) + 2.0f * t > 0.0f)
	{
		// Corner arc: |A + 2t| = r + t, a quadratic in t; the larger root is the one at zero on the outline.
		float qa = 7.0f;
		float qb = 4.0f * (A.x + A.y) - 2.0f * r;
		float qc = dot(A, A) - r * r;
		t = (-qb + sqrt(max(qb * qb - 4.0f * qa * qc, 0.0f))) / (2.0f * qa);
	}
	if (lgLimit.y > 0.5f && t > (shortHalf - r) * 0.5f)
	{
		float2 B = offset - halfSize + shortHalf;
		t = shortHalf - (length(max(B, 0.0f)) + min(max(B.x, B.y), 0.0f));
	}
	return t;
}
// How much of the pixel lies inside a pill's rounded outline; 1 for any other glass.
// Windows draws the glass over the whole window rectangle, so a pill fades it out over
// one pixel outside its outline and the blend keeps the back buffer in the corners.
float LgMask(float2 p)
{
	if (lgLimit.y < 0.5f)
	{
		return 1.0f;
	}
	float2 halfSize = (lgRect.zw - lgRect.xy) * 0.5f;
	float r = min(lgTex.z, min(halfSize.x, halfSize.y));
	float2 q = abs(p - (lgRect.xy + lgRect.zw) * 0.5f) - halfSize + r;
	float outside = length(max(q, 0.0f)) + min(max(q.x, q.y), 0.0f) - r;
	return saturate(0.5f - outside);
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

// The full-resolution backdrop, blurred with a radius of lgFull.w pixels.
// Twelve taps on a golden-angle spiral with Gaussian weights cover the disc evenly.
float4 LgSharpAt(float2 uv)
{
	float4 center = texture1.Sample(sampler0, uv);
	float4 sum = center;
	float weight = 1.0f;
	[unroll] for (int i = 0; i < 12; ++i)
	{
		float r = sqrt((i + 0.5f) / 12.0f);
		float a = i * 2.39996323f;
		float w = exp(-2.0f * r * r);
		sum += texture1.Sample(sampler0, uv + float2(cos(a), sin(a)) * r * lgFull.w * lgFull.xy) * w;
		weight += w;
	}
	return lgFull.w < 0.05f ? center : sum / weight;
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
	// The bezel ends on the flat top at the latest where the glass is deepest. A wider bezel than
	// half of a short glass (a 48 px pill) left a slope at the middle, whose normal flips there,
	// so the refraction tore along a line at half height.
	float2 halfSize = (lgRect.zw - lgRect.xy) * 0.5f;
	float bezel = clamp(lgOptics.y, 1.0f, max(min(halfSize.x, halfSize.y), 1.0f));
	float d = -max(LgDepth(p), 0.0f);
	// Edge normal, pointing out of the glass.
	float2 e = float2(1.0f, 0.0f);
	float2 n = normalize(float2(LgDepth(p - e.xy) - LgDepth(p + e.xy), LgDepth(p - e.yx) - LgDepth(p + e.yx)) + 1e-6f);

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
	// Only the redrawn region plus the blur expansion holds current pixels, so the shift stays inside it.
	pg = clamp(pg, -lgLimit.x, lgLimit.x);
	pr = clamp(pr, -lgLimit.x, lgLimit.x);
	pb = clamp(pb, -lgLimit.x, lgLimit.x);

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
	// A rounded corner is the only place whose normal can face a diagonal light head-on, which made it a
	// hot spot. Cap the facing at what the best lit straight side gets, so the corner matches the sides.
	float sideFacing = max(abs(L.x), abs(L.y));
	float facing = pow(clamp(dot(n, L), 0.0f, sideFacing), lgRim.y) + 0.4f * pow(clamp(dot(n, -L), 0.0f, sideFacing), lgRim.y);
	rimLight = ((1.0f - N.y) * facing * lgRim.x).xxx;
}

#endif // BLUR_COMMON_HLSLI
