#pragma once
#include <algorithm>
#include <cstdint>

namespace OpenGlass::TaskbarGlass
{
	inline constexpr wchar_t CustomKey[] = L"FluidGlassTaskbarCustom";
	inline constexpr wchar_t BlurKey[] = L"FluidGlassTaskbarBlur";
	inline constexpr wchar_t DarkKey[] = L"FluidGlassTaskbarReadabilityDark";
	inline constexpr wchar_t LightKey[] = L"FluidGlassTaskbarReadabilityLight";
	struct Settings
	{
		bool custom{};
		std::uint32_t blur100{150}, dark{50}, light{80};
	};

	template<typename Read> Settings Load(Read read)
	{
		Settings s{};
		s.custom = read(CustomKey, 0u) != 0;
		s.blur100 = (std::min)(std::uint32_t(read(BlurKey, s.blur100)), 2000u);
		s.dark = (std::min)(std::uint32_t(read(DarkKey, s.dark)), 100u);
		s.light = (std::min)(std::uint32_t(read(LightKey, s.light)), 100u);
		return s;
	}

	struct Material { float clarity, blur, readability; bool shadowLift; };
	inline Material Resolve(const Settings& s, bool pill, bool light, Material inherited)
	{
		if (!pill || !s.custom) return inherited;
		// Sample the refracted backdrop at the selected radius. The luminance floor works across
		// the bevel of a short pill, even where Explorer's interior shadow lift would fade out.
		return { 1.f, s.blur100 / 100.f, light ? -float(s.light) / 100.f : float(s.dark) / 100.f, false };
	}
}
