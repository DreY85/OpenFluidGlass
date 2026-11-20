#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace OpenGlass::ExplorerPane
{
	// A window property is a single 32-bit snapshot, also readable across process bitness.
	inline constexpr wchar_t Property[] = L"OpenFluidGlass.ClearPane.v1";
	struct Rect { float left{}, top{}, right{}, bottom{}; };

	inline std::uint32_t Pack(Rect r)
	{
		auto byte = [](float v, bool low) {
			const float x = (std::clamp)(v, 0.f, 1.f) * 255.f;
			return static_cast<std::uint32_t>(low ? std::ceil(x) : std::floor(x));
		};
		const auto l = byte(r.left, true), t = byte(r.top, true);
		const auto h = byte(r.right, false), b = byte(r.bottom, false);
		return l < h && t < b ? l | (t << 8) | (h << 16) | (b << 24) : 0;
	}

	inline Rect Unpack(std::uint32_t v)
	{
		auto unit = [v](unsigned shift) { return static_cast<float>((v >> shift) & 255u) / 255.f; };
		Rect r{ unit(0), unit(8), unit(16), unit(24) };
		return r.left < r.right && r.top < r.bottom ? r : Rect{};
	}
}
