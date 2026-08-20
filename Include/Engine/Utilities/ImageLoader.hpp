#pragma once

#include <Engine/Core/Types.hpp>

EE_NAMESPACE_UTILITIES_BEGIN

/**
 * @brief Decoded image result with raw pixel data.
 */
struct DecodedImage {
	Vector<UInt8> pixels;  ///< RGBA8 pixel data (4 bytes per pixel).
	UInt32 width = 0;
	UInt32 height = 0;
	bool isValid() const { return !pixels.empty(); }
};

/**
 * @brief Decode a PNG image from memory using libspng.
 *
 * Falls back to stb_image if libspng fails (e.g., non-PNG data
 * misidentified as PNG, or corrupted PNG).
 *
 * @param data   Pointer to raw image bytes.
 * @param size   Size of the data in bytes.
 * @return Decoded image in RGBA8 format, or empty on failure.
 */
DecodedImage EE_API decodePNG(const void* data, Size size);

/**
 * @brief Decode any supported image format from memory using stb_image.
 * @param data   Pointer to raw image bytes.
 * @param size   Size of the data in bytes.
 * @return Decoded image in RGBA8 format, or empty on failure.
 */
DecodedImage EE_API decodeImage(const void* data, Size size);

/**
 * @brief Load a texture from file (PNG/JPEG/etc.) via DiligentTools.
 * @param filePath  Path to the image file.
 * @param device    Diligent IRenderDevice*.
 * @param sRGB      Load as sRGB (default true).
 * @return ITextureView* as void*, or nullptr on failure.
 */
EE_API void* loadTexture(const String& filePath, void* device, bool sRGB = true);

/// @brief Load a texture from a resource archive.
EE_API void* loadTexture(const ResPath& filePath, void* device, bool sRGB = true);

DecodedImage EE_API loadImage(const Path& path);
DecodedImage EE_API loadImage(const ResPath& path);

EE_NAMESPACE_UTILITIES_END
