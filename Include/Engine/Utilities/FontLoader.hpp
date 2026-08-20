#pragma once

#include <Core/Types.hpp>

EE_NAMESPACE_UTILITIES_BEGIN

/// @brief Metrics and UV coordinates for a single character glyph in a font atlas.
struct Glyph {
	Vec2 uvMin, uvMax; Vec2 size; Vec2 bearing; F32 advance;
};

/// @brief Bitmask flags for selecting character ranges when loading a font.
enum class CharSet : UInt32 {
	None    = 0,
	ASCII   = 1u << 0,   ///< 0x20-0x7E (Basic Latin)
	Latin1  = 1u << 1,   ///< 0xA0-0xFF (Latin-1 Supplement)
	CJK     = 1u << 2,   ///< 0x4E00-0x9FFF (CJK Unified Ideographs, ~21k chars)
	Custom  = 1u << 3,   ///< Explicit string (use extraChars parameter)
};
inline CharSet operator|(CharSet a, CharSet b) { return CharSet((UInt32)a | (UInt32)b); }
inline CharSet operator&(CharSet a, CharSet b) { return CharSet((UInt32)a & (UInt32)b); }
inline bool operator!(CharSet a) { return (UInt32)a == 0; }

/// @brief Holds a loaded font's atlas texture and glyph lookup table.
struct FontData {
	void* atlasSRV = nullptr;
	HashMap<UInt32, Glyph> glyphs;
	UInt32 atlasWidth=0, atlasHeight=0; F32 fontSize=16, lineHeight=0;
	bool isValid() const { return atlasSRV != nullptr; }
};

/**
 * @brief Load a font with specified character sets.
 * @param fontPath  Path to .ttf/.otf.
 * @param fontSize  Size in pixels.
 * @param device    Diligent IRenderDevice*.
 * @param sets      Character sets to load (use | to combine, e.g. CharSet::ASCII | CharSet::CJK).
 * @param extra     Additional characters (only used when CharSet::Custom is set).
 */
FontData EE_API loadFont(const String& fontPath, F32 fontSize, void* device,
	CharSet sets = CharSet::ASCII, const String& extra = "");

/// @brief Load a font from a resource archive.
FontData EE_API loadFont(const ResPath& fontPath, F32 fontSize, void* device,
	CharSet sets = CharSet::ASCII, const String& extra = "");

EE_NAMESPACE_UTILITIES_END
