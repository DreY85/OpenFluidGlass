#pragma once

namespace Engraving
{
	inline constexpr int Strength = 42; // percent
	// Upper shadow and lower reflection for a shallow cut under overhead light.
	inline constexpr int UpperStrength(bool darkInk) { return darkInk ? 25 : 46; }
	inline constexpr int LowerStrength(bool darkInk, bool lightTheme)
	{
		if (darkInk) return 48;
		return lightTheme ? 23 : 35;
	}
	inline constexpr bool LightEdge(int red, int green, int blue)
	{
		return 299 * red + 587 * green + 114 * blue < 128000;
	}
	// The part of a glyph mask that its neighbour one row away covers and this pixel does not: the row above for the
	// edge under the letters, the row below for the edge above them.
	inline constexpr unsigned char Edge(int neighbour, int current, int percent)
	{
		return static_cast<unsigned char>(neighbour > current ? (neighbour - current) * percent / 100 : 0);
	}
}
