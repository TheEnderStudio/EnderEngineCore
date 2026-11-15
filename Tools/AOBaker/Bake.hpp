#pragma once
/**
 * @file Bake.hpp
 * @brief Ambient occlusion baking for one material's UV space.
 */
#include "Gltf.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aob {

/// @brief Baking settings, one-to-one with the command line.
struct BakeSettings {
	std::uint32_t size = 512;      ///< Output resolution (square).
	std::uint32_t samples = 32;    ///< Hemisphere rays per texel.
	float distance = 0.0f;         ///< Ray length in model units; 0 = unbounded.
	float bias = 0.0f;             ///< Ray origin offset along the normal; 0 = automatic.
	int blur = 2;                  ///< Box blur radius in texels, 0 = off.
	int dilate = 6;                ///< Padding passes into empty texels, 0 = off.
	float power = 1.0f;            ///< Contrast: ao = pow(ao, power).
	int threads = 0;               ///< Worker threads, 0 = hardware concurrency.
	bool flipV = false;            ///< Treat v as bottom-up (some exporters).
	bool verbose = false;
};

/// @brief Bakes occlusion for every material that has UV-mapped geometry.
///
/// The occluder set is the whole model (so a character's torso shades its own arm, and a
/// floor is shaded by whatever stands on it), while the texels come from the material's own
/// UV layout.
bool bakeAll(const ModelGeometry& model, const BakeSettings& settings,
	std::vector<AoMap>& out, std::string& error);

} // namespace aob
