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

/**
 * @brief A full mip chain built from an image (level 0 first).
 */
struct MipChain {
	Vector<Vector<UInt8>> levels; ///< RGBA8 pixels per level (4 bytes per texel).
	UInt32 width = 0;             ///< Base level width.
	UInt32 height = 0;            ///< Base level height.
	bool isValid() const { return !levels.empty(); }
};

/**
 * @brief Build a full mip chain for an RGBA8 image with a 2x2 box filter.
 *
 * The chain is what lets a shader sample a texture at a level that matches its
 * footprint instead of aliasing the base level: the ray tracer picks the level
 * from the reflection lobe (ray cone) and the raster path gets it from the
 * hardware derivatives for free.
 *
 * sRGB data is averaged in linear light (decode, average, re-encode). Filtering
 * the encoded values directly would darken every level, which is visible as a
 * darker/blotchy texture as soon as anything samples a mip.
 *
 * @param image Base level; must be valid (RGBA8, 4 bytes per texel). Taken by
 *              value so the caller can move its pixels in and avoid a copy.
 * @param srgb  True when the pixels are sRGB-encoded (base color, emissive).
 * @return The chain, or an empty chain when the image is not valid.
 */
MipChain EE_API buildMipChain(DecodedImage image, bool srgb);

EE_NAMESPACE_UTILITIES_END
