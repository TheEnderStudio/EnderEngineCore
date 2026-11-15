#pragma once
/**
 * @file Gltf.hpp
 * @brief Model geometry for the AO baker, and writing the baked maps back into the model.
 */
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace aob {

/// @brief One triangle in the model's space (node transforms applied), with its UVs.
struct Tri {
	glm::vec3 p[3] = {};
	glm::vec3 n[3] = {};
	glm::vec2 uv[3] = {};
	std::uint32_t material = UINT32_MAX; ///< Index into the glTF's materials; max = none.
	bool hasUv = false;
};

/// @brief Everything the baker needs to know about the model.
struct ModelGeometry {
	std::vector<Tri> tris;                    ///< Every triangle instance, world space.
	std::vector<std::string> materialNames;   ///< One name per glTF material.
	std::uint32_t materialCount = 0;
	glm::vec3 boundsMin = glm::vec3(0.0f);
	glm::vec3 boundsMax = glm::vec3(0.0f);
};

/// @brief Load the model's triangles (fastgltf), with node transforms applied.
bool loadGeometry(const std::string& path, ModelGeometry& out, std::string& err);

/// @brief One baked occlusion map, 8 bits per channel, RGB (R = G = B = occlusion).
struct AoMap {
	std::uint32_t material = 0;
	std::string name;                 ///< Image name written into the glTF.
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::vector<std::uint8_t> rgb;
};

/**
 * @brief Write @p inputPath's model to @p outputPath with @p maps attached.
 *
 * Each map becomes its material's `occlusionTexture`. For a .glb the PNG is appended to the
 * binary chunk and referenced through a buffer view, so the result stays a single file; for
 * a .gltf the PNG is written next to the output and referenced by URI. Everything else in
 * the model (buffers, images, materials, nodes) is preserved.
 */
bool writeModelWithAo(const std::string& inputPath, const std::string& outputPath,
	const std::vector<AoMap>& maps, std::string& err);

} // namespace aob
