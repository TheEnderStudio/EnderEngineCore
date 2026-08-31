#pragma once

#include <Engine/Core/Subsystem.hpp>
#include <Engine/Core/Types.hpp>
#include <Engine/Rendering/RenderTypes.hpp>
#include <Engine/Rendering/Errors.hpp>

EE_NAMESPACE_RENDERING_BEGIN

class RenderSubsystem;

/**
 * @brief NVIDIA Real-Time Denoiser (NRD) subsystem.
 *
 * Denoises the hybrid ray tracing output with REBLUR (diffuse + specular) on
 * the GPU (vendor-agnostic compute shaders; NRD's core API is device-agnostic
 * - this subsystem binds Diligent D3D12 resources and executes NRD's compute
 * dispatches on the renderer's command list). No CPU participation, no
 * external-memory interop.
 *
 * Pipeline per frame:
 *   1. prep pass   - RT output (RGBA32F: rgb = reflection, a = lighting factor)
 *                    + RT-res normal + full-res depth -> NRD inputs (motion
 *                    vectors from camera reprojection, view Z, packed
 *                    normal+roughness, spec/diff radiance + norm. hit distance).
 *   2. REBLUR      - NRD's dispatches (REBLUR_DIFFUSE_SPECULAR).
 *   3. pack pass   - OUT spec/diff radiance -> compose texture (RGBA32F).
 */
class EE_API DenoisingSubsystem final : public Subsystem {
public:
	DenoisingSubsystem();
	~DenoisingSubsystem() override;

	void attachToRenderer(RenderSubsystem* r);

	/// @brief Strength of the denoised result vs. the raw RT output (0 = raw, 1 = fully denoised).
	void setStrength(F32 strength);

	/// @brief Whether NRD initialized successfully (GPU compute available).
	EE_NODISCARD bool isReady() const;

	/**
	 * @brief Denoise the current RT output (REBLUR diffuse+specular).
	 *
	 * Runs the prep pass, NRD's REBLUR dispatches and the pack pass, all on the
	 * renderer's command list in the current frame. compose() should read
	 * getDenoisedSRV() afterwards.
	 * @param colorSRV  SRV of the RT output (RGBA32F: rgb = reflection, a = lighting).
	 * @param normalSRV SRV of the RT-res world normal + roughness (RGBA32F, a = roughness).
	 * @param depthSRV  SRV of the full-res depth (R32F).
	 * @param view      World->view matrix (current frame).
	 * @param proj      View->clip matrix (current frame).
	 * @param width,height RT output dimensions.
	 */
	Result<void, RenderError> denoise(
		void* colorSRV, void* normalSRV, void* depthSRV,
		const Mat4& view, const Mat4& proj,
		UInt32 width, UInt32 height);

	/// @brief SRV of the latest denoised frame (nullptr until denoise() runs).
	void* getDenoisedSRV() const;

protected:
	Result<void, CoreError> onInitialize() override;
	void onShutdown() override;

private:
	struct Impl;
	static Result<void, RenderError> initNRD(Impl& p, UInt32 width, UInt32 height); ///< Lazy NRD init (needs the RT resolution).
	Uptr<Impl> m_impl;
};

EE_NAMESPACE_RENDERING_END
