#pragma once

#include <Core/Subsystem.hpp>
#include "Errors.hpp"
#include "PostProcessTypes.hpp"

EE_NAMESPACE_POSTPROCESS_BEGIN

/// @brief Post-process subsystem managing tone mapping, bloom, and custom HLSL effects.
class EE_API PostProcessSubsystem : public Subsystem {
public:
	/// @brief Construct the post-process subsystem.
	PostProcessSubsystem();
	/// @brief Destroy the post-process subsystem.
	~PostProcessSubsystem() override;

	EE_NO_COPY(PostProcessSubsystem)
	EE_NO_MOVE(PostProcessSubsystem)

	/// @brief Set the D3D11 device pointer.
	void setDevice(void* device);
	/// @brief Set the D3D11 swap chain pointer.
	void setSwapChain(void* swapChain);
	/// @brief Set the D3D11 device context pointer.
	void setContext(void* context);
	/// @brief Apply a full post-process configuration.
	void setConfig(const PostProcessConfig& config);
	/// @brief Get the current post-process configuration.
	const PostProcessConfig& config() const;

	/**
	 * @brief Set a custom HLSL shader source.
	 * @param source HLSL PS with entry point "customMain":
	 *   float4 customMain(float3 color, float2 uv, Texture2D inputTex, SamplerState smp) : SV_TARGET.
	 *   Empty string disables the custom effect.
	 */
	void setCustomShader(const String& source);

	/**
	 * @brief Trigger a full PSO rebuild (e.g., after changing custom shader).
	 */
	void rebuildPSO();

	/// @brief Resize internal render targets.
	void resize(UInt32 width, UInt32 height);
	/// @brief Get the HDR shader resource view.
	void* getHDRSRV() const;
	/// @brief Get the HDR render target view.
	void* getHDRRTV() const;
	/// @brief Execute the post-process pipeline.
	void execute();
	/// @brief Present the final result to the swap chain.
	void present();

protected:
	Result<void, CoreError> onInitialize() override;
	void onShutdown() override;
	void onUpdate(F64 deltaTime) override;
	bool onRecover() override;

private:
	struct Impl;
	Uptr<Impl> m_impl;
};

EE_NAMESPACE_POSTPROCESS_END
