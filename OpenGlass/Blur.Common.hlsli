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
	float4 fgRect;		// left, top, right, bottom of the glass, back-buffer pixels
	float4 fgTex;		// uv per pixel (x, y), corner radius in pixels, enabled
	float4 fgOptics;	// index of refraction, bezel width, glass thickness, refraction scale
	float4 fgLook;		// dispersion, saturation, readability, glass-to-background gap
	float4 fgRim;		// rim light strength, shine exponent, light angle in radians, bezel profile
	float4 fgFull;		// uv per pixel of the full-resolution copy (x, y), clarity, softness in pixels
	float4 fgClearPane;  // normalized empty part of the file view, or zeros
	float4 fgLimit;		// longest refraction shift in pixels, 1 for a pill cut to its rounded outline, grain strength,
						// 1 for the shadow lift in light mode
};

Texture2D<float4> texture0 : register(t0);
Texture2D<float4> texture1 : register(t1);	// Fluid Glass: full-resolution copy of the backdrop
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
// Fluid Glass
//
// The edge of the glass is a convex bezel. A ray coming straight from the eye refracts
// where it enters the glass (Snell's law, air 1.0 to glass ~1.5) and travels down through
// the glass thickness. Where it lands on the background, compared with where it would have
// landed without glass, is the displacement. A convex bezel pulls rays inward, so the
// edge shows the background from further inside, compressed, as on Apple's glass design.
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
	float2 halfSize = (fgRect.zw - fgRect.xy) * 0.5f;
	float shortHalf = min(halfSize.x, halfSize.y);
	float r = min(fgTex.z, shortHalf);
	float2 offset = abs(p - (fgRect.xy + fgRect.zw) * 0.5f);
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
	if (fgLimit.y > 0.5f && t > (shortHalf - r) * 0.5f)
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
	if (fgLimit.y < 0.5f)
	{
		return 1.0f;
	}
	float2 halfSize = (fgRect.zw - fgRect.xy) * 0.5f;
	float r = min(fgTex.z, min(halfSize.x, halfSize.y));
	float2 q = abs(p - (fgRect.xy + fgRect.zw) * 0.5f) - halfSize + r;
	float outside = length(max(q, 0.0f)) + min(max(q.x, q.y), 0.0f) - r;
	return saturate(0.5f - outside);
}
// Bezel height from the outer edge (u = 0) to the flat top (u = 1).
float LgConvex(float u)
{
	float k = 1.0f - u;
	return fgRim.w > 0.5f && fgRim.w < 1.5f ? sqrt(max(1.0f - k * k, 0.0f)) : pow(max(1.0f - k * k * k * k, 0.0f), 0.25f);
}
float LgHeight(float u)
{
	if (fgRim.w > 1.5f)
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
	s += conv[0].x * texture0.SampleLevel(sampler0, input.tex1.xy + duv, 0.0f);
	s += conv[1].x * texture0.SampleLevel(sampler0, input.tex1.zw + duv, 0.0f);
	s += conv[2].x * texture0.SampleLevel(sampler0, input.tex2.xy + duv, 0.0f);
	s += conv[3].x * texture0.SampleLevel(sampler0, input.tex2.zw + duv, 0.0f);
	s += conv[4].x * texture0.SampleLevel(sampler0, input.tex3.xy + duv, 0.0f);
	s += conv[5].x * texture0.SampleLevel(sampler0, input.tex3.zw + duv, 0.0f);
	s += conv[6].x * texture0.SampleLevel(sampler0, input.tex4.xy + duv, 0.0f);
	s += conv[7].x * texture0.SampleLevel(sampler0, input.tex4.zw + duv, 0.0f);
	return s;
}

// The full-resolution backdrop, blurred with a radius of fgFull.w pixels.
// Twelve taps on a golden-angle spiral with Gaussian weights cover the disc evenly.
float4 LgSharpAt(float2 uv)
{
	float4 center = texture1.SampleLevel(sampler0, uv, 0.0f);
	// Below the threshold only the center is used. Branch before the 12 taps so they are not
	// fetched and thrown away, which the compiler did with the old select at the end.
	[branch] if (fgFull.w < 0.05f)
	{
		return center;
	}
	float4 sum = center;
	float weight = 1.0f;
	[unroll] for (int i = 0; i < 12; ++i)
	{
		float r = sqrt((i + 0.5f) / 12.0f);
		float a = i * 2.39996323f;
		float w = exp(-2.0f * r * r);
		sum += texture1.SampleLevel(sampler0, uv + float2(cos(a), sin(a)) * r * fgFull.w * fgFull.xy, 0.0f) * w;
		weight += w;
	}
	return sum / weight;
}

// The blurred background seen through the glass, plus the light caught by the rim.
// Grain: a fine static noise, like frosted glass. It is tied to the glass, not to the screen, so it moves with the
// window instead of the window sliding over a fixed pattern. Returns a signed brightness offset, 0 when grain is off.
float LgGrain(float2 p)
{
	uint2 q = uint2(max(p - fgRect.xy, 0.0f));
	uint h = q.x * 1973u + q.y * 9277u + 0x68E31DA4u;
	h = (h ^ (h >> 15)) * 0x2C1B3C6Du;
	h = (h ^ (h >> 12)) * 0x297A2D39u;
	h ^= h >> 15;
	return ((h & 0xFFFFu) / 65535.0f - 0.5f) * 0.16f * fgLimit.z;
}

struct LgPaneSample
{
	float4 backdrop;
	float amount;
};

// Only the file-view layout determines where the ordinary material yields to clear glass.
// Bezel width changes the optics of both materials, never the size of the empty space.
float LgPaneWeight(float2 p)
{
	float weight = 0.0f;
	[branch] if (fgLimit.w > 0.5f && all(fgClearPane.zw > fgClearPane.xy))
	{
		float2 extent = fgRect.zw - fgRect.xy;
		float2 start = fgRect.xy + fgClearPane.xy * extent;
		float2 end = fgRect.xy + fgClearPane.zw * extent;
		float side = min(p.x - start.x, end.x - p.x);
		float rowFade = clamp(extent.y * 0.14f, 32.0f, 96.0f);
		float edgeFade = clamp(extent.y * 0.065f, 16.0f, 48.0f);
		weight = smoothstep(0.0f, rowFade, p.y - start.y)
			* smoothstep(0.0f, edgeFade, side)
			* smoothstep(0.0f, edgeFade, end.y - p.y);
	}
	return weight;
}

void LgCompose(PS_INPUT input, out float4 background, out float3 rimLight, out LgPaneSample pane)
{
	background = 0;
	rimLight = 0;
	pane = (LgPaneSample)0;
	if (fgTex.w < 0.5f)
	{
		background = LgBlurAt(input, 0);
		return;
	}

	float2 p = input.pos.xy;
	// The bezel ends on the flat top at the latest where the glass is deepest. A wider bezel than
	// half of a short glass (a 48 px pill) left a slope at the middle, whose normal flips there,
	// so the refraction tore along a line at half height.
	float2 halfSize = (fgRect.zw - fgRect.xy) * 0.5f;
	float bezel = clamp(fgOptics.y, 1.0f, max(min(halfSize.x, halfSize.y), 1.0f));
	float d = -max(LgDepth(p), 0.0f);
	float u = saturate(-d / bezel);
	// Past the bezel the glass is flat: the normal points straight up, nothing refracts and the rim catches no light.
	// Those are the values the optics below produce there, so skipping them leaves every output bit unchanged.
	float2 n = 0, N = float2(0.0f, 1.0f), pg = 0, pr = 0, pb = 0;
	[branch] if (u < 1.0f)
	{
		// Edge normal, pointing out of the glass.
		float2 e = float2(1.0f, 0.0f);
		n = normalize(float2(LgDepth(p - e.xy) - LgDepth(p + e.xy), LgDepth(p - e.yx) - LgDepth(p + e.yx)) + 1e-6f);

		float slope = fgOptics.z / bezel * (LgHeight(min(u + 0.002f, 1.0f)) - LgHeight(max(u - 0.002f, 0.0f))) / 0.004f;
		// Cross-section through the edge: x points inward, y points up. The surface normal leans outward.
		N = normalize(float2(-slope, 1.0f));
		float depth = fgOptics.z * LgHeight(u) + fgLook.w;
		float ior = fgOptics.x;
		// Sideways shift in pixels for each channel; dispersion bends red less and blue more.
		pg = -n * LgLateral(N, 1.0f / ior, depth) * fgOptics.w;
		pr = -n * LgLateral(N, 1.0f / max(ior - fgLook.x, 1.0001f), depth) * fgOptics.w;
		pb = -n * LgLateral(N, 1.0f / (ior + fgLook.x), depth) * fgOptics.w;
		// Only the redrawn region plus the blur expansion holds current pixels, so the shift stays inside it.
		pg = clamp(pg, -fgLimit.x, fgLimit.x);
		pr = clamp(pr, -fgLimit.x, fgLimit.x);
		pb = clamp(pb, -fgLimit.x, fgLimit.x);
	}

	// The three channels bend alike on the flat top and without dispersion. There one set of taps serves them all.
	// The taps use SampleLevel to stay legal inside the branch. The textures have a single mip, so they read the same.
	bool split = any(pr != pg) || any(pb != pg);
	float paneWeight = LgPaneWeight(p);
	[branch] if (paneWeight > 0.0f)
	{
		// Take the clear sample at the same refracted coordinates as the ordinary glass.
		pane.backdrop = texture1.SampleLevel(sampler0, (p + pg) * fgFull.xy, 0.0f);
		[branch] if (split)
		{
			pane.backdrop.r = texture1.SampleLevel(sampler0, (p + pr) * fgFull.xy, 0.0f).r;
			pane.backdrop.b = texture1.SampleLevel(sampler0, (p + pb) * fgFull.xy, 0.0f).b;
		}
		pane.amount = paneWeight * saturate(pane.backdrop.a);
	}
	background = LgBlurAt(input, pg * fgTex.xy);
	[branch] if (split)
	{
		background.r = LgBlurAt(input, pr * fgTex.xy).r;
		background.b = LgBlurAt(input, pb * fgTex.xy).b;
	}
	// Keep the broad backdrop colour for the shadow lift before mixing in sharp detail.
	float3 neighborhood = background.rgb;
	if (fgFull.z > 0.0f)
	{
		float2 uv = p * fgFull.xy;
		float3 sharp = LgSharpAt(uv + pg * fgFull.xy).rgb;
		[branch] if (split)
		{
			sharp.r = LgSharpAt(uv + pr * fgFull.xy).r;
			sharp.b = LgSharpAt(uv + pb * fgFull.xy).b;
		}
		background.rgb = lerp(background.rgb, sharp, fgFull.z);
	}

	float luminance = dot(background.rgb, float3(0.2126f, 0.7152f, 0.0722f));
	background.rgb = max(lerp(luminance.xxx, background.rgb, fgLook.y), 0.0f);
	// Readability: where the background is too bright for white text, the glass darkens it. A negative value means
	// dark text (light mode): there the glass lightens a background that is too dark, the mirror of the same rule.
	luminance = dot(background.rgb, float3(0.2126f, 0.7152f, 0.0722f));
	if (fgLook.z >= 0.0f)
	{
		background.rgb *= lerp(1.0f, min(1.0f, 0.22f / max(luminance, 1e-3f)), fgLook.z);
	}
	else if (fgLimit.w > 0.5f)
	{
		// Shadow lift: only the dark parts of the backdrop get light, tinted toward the surrounding wallpaper, so the
		// glass stays clear over bright areas instead of turning milky. The lift fades across the curved bezel and
		// the flat interior receives its full strength.
		// The strength comes from Readability in light mode. Higher values lift more and reach further into the mid
		// tones: at 0.5 the lift is 0.4 on black, at 1 it reaches white on black and most of the way on mid tones.
		const float localTintStrength = 0.32f;
		float strength = saturate(-fgLook.z);
		float interior = smoothstep(0.0f, 1.0f, u);
		float coverage = saturate(background.a);
		float backdropLuma = luminance / max(coverage, 1e-4f);
		float shadow = saturate(1.0f - backdropLuma / 0.95f);
		float lift = strength * (0.6f + 0.4f * strength) * interior * pow(shadow, 1.5f - strength);

		// Steer the added light towards the surrounding wallpaper. On a grey the bias is zero.
		float localLuma = dot(neighborhood, float3(0.2126f, 0.7152f, 0.0722f));
		float localPeak = max(neighborhood.r, max(neighborhood.g, neighborhood.b));
		float3 localChroma = (neighborhood - localLuma) / max(localPeak, 0.12f);
		float3 addedLight = coverage * lift * (1.0f + localTintStrength * clamp(localChroma, -1.0f, 1.0f));

		// Limit only the addition: transparent pixels get no light, existing HDR highlights keep their range.
		background.rgb += min(addedLight, max(coverage - background.rgb, 0.0f));

		// Near the top of the slider the glass turns almost opaque: the backdrop fades into a light frost tinted
		// toward the wallpaper, up to 90% at 1. Below 0.6 nothing changes here.
		float frost = 0.9f * smoothstep(0.6f, 1.0f, strength) * interior;
		float3 frostColor = coverage * saturate(0.9f + 0.1f * clamp(localChroma, -1.0f, 1.0f));
		background.rgb = lerp(background.rgb, frostColor, frost);
	}
	else
	{
		// Only the luminance rises, up to a floor that grows with the setting, so mid tones keep their contrast.
		// The hue and as much of the color as fits stay, so the glass brightens instead of turning milky.
		float3 unlifted = background.rgb;
		float floorLuminance = 0.78f * -fgLook.z;
		float knee = 0.25f * floorLuminance;
		float lifted = min(0.5f * (luminance + floorLuminance + sqrt((luminance - floorLuminance) * (luminance - floorLuminance) + knee * knee)), 1.0f);
		float3 chroma = background.rgb - luminance;
		float peak = max(max(chroma.r, chroma.g), chroma.b);
		// Never above white, nor above a channel that was already brighter.
		float fit = min(1.0f, (max(1.0f, luminance + peak) - lifted) / max(peak, 1e-4f));
		background.rgb = max(lifted + chroma * fit, 0.0f);
		// The lift follows the background's coverage. Where nothing is behind the glass (alpha 0, as in a DWM
		// thumbnail or Alt+Tab), it used to add colour without alpha, which those views showed as a white sheet.
		background.rgb = lerp(unlifted, background.rgb, saturate(background.a));
	}

	// Rim light: how steep the bezel is and how much it faces the light.
	// The opposite edge catches a weaker reflection, like light leaving through the far side.
	float2 L = float2(cos(fgRim.z), sin(fgRim.z));
	// A rounded corner is the only place whose normal can face a diagonal light head-on, which made it a
	// hot spot. Cap the facing at what the best lit straight side gets, so the corner matches the sides.
	float sideFacing = max(abs(L.x), abs(L.y));
	// On the two corners side-on to the light the normal turns from the lit face to the far face. A low focus
	// (an exponent near 0.1) makes both terms jump there, so the corner showed a hard diagonal line. Near that
	// turn the lit and the far reflection now blend smoothly. Past it, both read what they did before.
	const float turn = 0.25f;
	float toLight = dot(n, L);
	float litFace = pow(clamp(max(toLight, turn), 0.0f, sideFacing), fgRim.y);
	float farFace = 0.4f * pow(clamp(max(-toLight, turn), 0.0f, sideFacing), fgRim.y);
	float facing = lerp(farFace, litFace, smoothstep(-turn, turn, toLight));
	rimLight = ((1.0f - N.y) * facing * fgRim.x).xxx;
	pane.backdrop.rgb += rimLight * saturate(pane.backdrop.a);
}

#endif // BLUR_COMMON_HLSLI
