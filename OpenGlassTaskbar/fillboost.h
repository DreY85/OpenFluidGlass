// Shared by pilltap.dll (the taskbar) and starttap.dll (Start and Search).
#pragma once
#include <windows.h>
#include <algorithm>
#include <vector>
#include <winrt/Windows.UI.Xaml.Media.h>

// The theme's hover, pressed and running app fills are a few percent of white (black when light), made for acrylic.
// On our clearer, brighter glass they all but vanished, so while the glass shows they are made stronger.
// A theme brush is shared by every part that uses it, so changing it once covers them all. It gets its own color back
// when the glass goes.
class FillBoost
{
public:
	static constexpr int Strength = 3;
	static constexpr BYTE Faint = 0x30;	// a stronger fill is no hover and stays as it is
	static constexpr BYTE Most = 0x48;

	// True when the brush is a faint fill not seen before.
	bool Add(const winrt::Windows::UI::Xaml::Media::Brush& brush)
	{
		const auto solid = brush ? brush.try_as<winrt::Windows::UI::Xaml::Media::SolidColorBrush>() : nullptr;
		if (!solid || std::any_of(m_fills.begin(), m_fills.end(), [&](const Fill& fill) { return fill.brush == solid; })) return false;
		const auto color = solid.Color();
		if (color.A == 0 || color.A > Faint) return false;
		m_fills.push_back({ solid, color });
		if (m_on) Set(m_fills.back(), true);
		return true;
	}

	// A hover fades in from a clear white. Faded to the darker shade it passed through a light gray on the way, which
	// flashed on every hover. A clear black fades without it and looks the same at rest. True when not seen before.
	bool AddClear(const winrt::Windows::UI::Xaml::Media::Brush& brush)
	{
		const auto solid = brush ? brush.try_as<winrt::Windows::UI::Xaml::Media::SolidColorBrush>() : nullptr;
		if (!solid || std::any_of(m_fills.begin(), m_fills.end(), [&](const Fill& fill) { return fill.brush == solid; })) return false;
		const auto color = solid.Color();
		if (color.A != 0 || color.R < 0xF0 || color.G < 0xF0 || color.B < 0xF0) return false;
		// Set turns a white fill into black of four times its alpha, which for a clear one is clear black.
		m_fills.push_back({ solid, color });
		if (m_on) Set(m_fills.back(), true);
		return true;
	}

	void Show(bool on)
	{
		if (on == m_on) return;
		m_on = on;
		for (const auto& fill : m_fills) Set(fill, on);
	}

	// The last fill added, for the log.
	winrt::Windows::UI::Color Last() const { return m_fills.empty() ? winrt::Windows::UI::Color{} : m_fills.back().original; }

private:
	struct Fill
	{
		winrt::Windows::UI::Xaml::Media::SolidColorBrush brush{ nullptr };
		winrt::Windows::UI::Color original{};
	};

	// A white fill belongs to the dark theme. More white hardly showed on the dark glass over a bright wallpaper, so
	// there it turns into a darker shade instead.
	static constexpr int DarkStrength = 4;
	static constexpr BYTE DarkMost = 0x50;

	static void Set(const Fill& fill, bool on)
	{
		auto color = fill.original;
		const bool dark = color.R >= 0xF0 && color.G >= 0xF0 && color.B >= 0xF0;
		if (on && dark) color = { static_cast<BYTE>((std::min)(color.A * DarkStrength, static_cast<int>(DarkMost))), 0, 0, 0 };
		else if (on) color.A = static_cast<BYTE>((std::min)(color.A * Strength, static_cast<int>(Most)));
		try { fill.brush.Color(color); }
		catch (...) {}
	}

	std::vector<Fill> m_fills;
	bool m_on{};
};
