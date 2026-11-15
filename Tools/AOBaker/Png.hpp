#pragma once
/**
 * @file Png.hpp
 * @brief PNG encoding for the baked occlusion maps (stb_image_write).
 */
#include <cstdint>
#include <vector>

namespace aob {

/// @brief Encode 8-bit RGB pixels as a PNG file in memory. Empty on failure.
std::vector<std::uint8_t> encodePng(std::uint32_t width, std::uint32_t height, const std::uint8_t* rgb);

} // namespace aob
