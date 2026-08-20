#pragma once

#include <Core/Types.hpp>

EE_NAMESPACE_POSTPROCESS_BEGIN

/// @brief Tone mapping operator selection.
enum class ToneMapMode : UInt8 { None, Reinhard, Uncharted2, ACES };

/**
 * @brief Describes a single post-processing effect in the stack.
 */
enum class EffectType : UInt8 {
	ToneMap,   ///< Tone mapping + vignette + saturation + gamma.
	Bloom,     ///< Bloom / glow effect (bright pass + blur + composite).
	Custom,    ///< User-provided HLSL pixel shader.
};

/// @brief Configuration for the bloom post-process effect.
struct BloomConfig {
	bool  enabled    = true;
	F32   threshold  = 0.8f;    ///< Minimum brightness to bloom.
	F32   intensity  = 0.5f;    ///< Bloom blend strength.
	F32   radius     = 0.01f;   ///< Blur sample offset in UV space.
};

/// @brief Configuration for tone mapping, exposure, gamma, vignette, and saturation.
struct ToneMapConfig {
	ToneMapMode mode       = ToneMapMode::ACES;
	F32         exposure   = 1.0f;
	F32         gamma      = 2.2f;
	F32         vignette   = 0.0f;
	F32         saturation = 1.0f;
	F32         contrast   = 1.0f; ///< Contrast multiplier (1.0 = neutral).
};

/// @brief Top-level post-process configuration combining tone mapping, bloom, custom shaders, and MSAA.
struct PostProcessConfig {
	ToneMapConfig toneMap;
	BloomConfig   bloom;
	bool          enabled     = true;   ///< Skip entire post-process when false.
	String        customShader;         ///< HLSL source for Custom effect (entry: "customMain").
	UInt8         sampleCount = 4;      ///< MSAA sample count for the HDR render target (1=off, 2/4/8).
};

EE_NAMESPACE_POSTPROCESS_END
