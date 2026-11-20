#pragma once
#include <array>
#include <list>

// Only the uncoloured, unpositioned glyph coverage is reusable. Colours, effects,
// clipping and destination pixels are evaluated on every draw.
namespace TextMaskCache
{
	inline constexpr size_t MaxEntries = 48, MaxPixels = 32768, MaxLetters = 512;
	inline std::atomic<unsigned> displayEpoch{};
	inline std::array<std::atomic<unsigned>, 128> themeEpochs{};

	inline auto& ThemeEpoch(HTHEME theme)
	{
		const uintptr_t h = reinterpret_cast<uintptr_t>(theme);
		return themeEpochs[((h >> 3) ^ (h >> 12)) % themeEpochs.size()];
	}

	struct Key
	{
		HTHEME theme{};
		HGDIOBJ font{};
		LOGFONTW description{};
		int part{}, state{}, width{}, height{}, pad{}, dpiX{}, dpiY{};
		DWORD flags{}, layout{};
		unsigned display{}, generation{};
		bool Equals(const Key& other) const
		{
			return theme == other.theme && font == other.font && part == other.part && state == other.state &&
				width == other.width && height == other.height && pad == other.pad && dpiX == other.dpiX &&
				dpiY == other.dpiY && flags == other.flags && layout == other.layout && display == other.display &&
				generation == other.generation && memcmp(&description, &other.description, sizeof(description)) == 0;
		}
	};

	struct Entry
	{
		Key key;
		std::wstring text;
		RECT bounds;
		std::vector<BYTE> alpha;
	};

	inline thread_local std::list<Entry> entries;
	inline bool Find(const Key& key, std::wstring_view text, RECT& bounds, std::vector<BYTE>& alpha)
	{
		for (auto it = entries.begin(); it != entries.end(); ++it)
		{
			if (it->key.Equals(key) && it->text == text)
			{
				bounds = it->bounds;
				alpha = it->alpha;
				entries.splice(entries.begin(), entries, it);
				return true;
			}
		}
		return false;
	}

	inline void Store(const Key& key, std::wstring_view text, const RECT& bounds, const std::vector<BYTE>& alpha)
	{
		if (entries.size() >= MaxEntries) entries.pop_back();
		entries.push_front({ key, std::wstring(text), bounds, alpha });
	}
}
