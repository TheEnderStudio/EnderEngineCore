#pragma once
/**
 * @file VolumetricBackend.hpp
 * @brief Internal interface behind VolumetricSubsystem: one implementation per algorithm.
 *
 * Two algorithms compute the same thing - in-scattering and transmittance for the fog - and
 * the subsystem picks between them at run time:
 *
 *  * @b ScreenSpace marches each pixel's view ray directly (optionally at a reduced
 *    resolution, upsampled with a depth-guided filter). One shadow-map fetch per step, no
 *    extra memory, and it handles a long view distance well.
 *  * @b Froxel fills a view-space 3D grid once (one shadow fetch per froxel, not per step),
 *    then integrates the grid per pixel column. It costs a 3D texture and an extra pass but
 *    makes per-froxel work - local lights above all - independent of the march length.
 *
 * Both write a full-resolution `rgb = in-scattering, a = transmittance` target, so the
 * composite in the post-process chain, and everything above this interface, is shared.
 */
#include <Rendering/VolumetricSubsystem.hpp>

EE_NAMESPACE_RENDERING_BEGIN

namespace Detail {

/// @brief One volumetric algorithm.
class IVolumetricBackend {
public:
	virtual ~IVolumetricBackend() = default;

	/// @brief Creates the pipeline states and resources. False disables the backend.
	virtual bool initialize(RenderSubsystem* renderer) = 0;
	virtual void shutdown() = 0;

	/// @brief Renders the scattering/transmittance buffer for one frame.
	virtual Result<void, CoreError> render(const VolumetricInput& input, const VolumetricConfig& cfg) = 0;

	/// @brief rgb = in-scattering, a = transmittance, full resolution. Null when not ready.
	virtual void* scatteringSRV() const = 0;

	/// @brief Last frame's GPU time in milliseconds (0 when no measurement is available).
	virtual F32 lastRenderMs() const = 0;

	/// @brief Name used in the log and the self-check, e.g. "screen space".
	virtual const char* name() const = 0;
};

/**
 * @brief Temporal accumulation shared by both backends.
 *
 * Reprojects the previous resolved frame with the camera's motion, clamps it to the current
 * frame's neighbourhood and blends. Applied to the backend's output, so whichever algorithm
 * runs benefits - and a converged pixel can afford fewer march steps, which is what the
 * adaptive quality governor trades against.
 */
class ITemporalResolve {
public:
	virtual ~ITemporalResolve() = default;
	virtual bool initialize(RenderSubsystem* renderer) = 0;
	virtual void shutdown() = 0;

	/**
	 * @brief Blends @p currentSRV into the history and returns the resolved SRV.
	 *
	 * @param currentSRV  rgb = in-scattering, a = transmittance, full resolution.
	 * @param input       The frame's inputs (depth, matrices, size).
	 * @param weight      History weight in [0, 1); 0 disables the blend for this frame.
	 * @param reset       Drops the history (backend switch, resize, first frame).
	 */
	virtual void* resolve(void* currentSRV, const VolumetricInput& input, F32 weight, bool reset) = 0;

	/// @brief Per-pixel confidence written by the last resolve (0..1), for the governor.

	/// @brief True when no history exists yet (first frame or after a reset).
	virtual bool hasHistory() const = 0;
};

/// @brief Direct view-ray march (optionally at a reduced resolution).
Uptr<IVolumetricBackend> createScreenSpaceBackend();

/// @brief View-space froxel volume: inject into a 3D grid, then integrate per column.
Uptr<IVolumetricBackend> createFroxelBackend();

/// @brief The temporal accumulation stage used in front of the composite.
Uptr<ITemporalResolve> createTemporalResolve();

} // namespace Detail

EE_NAMESPACE_RENDERING_END
