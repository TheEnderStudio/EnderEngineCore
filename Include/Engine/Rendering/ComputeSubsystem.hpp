#pragma once

#include <Core/Subsystem.hpp>
#include "RenderTypes.hpp"

EE_NAMESPACE_RENDERING_BEGIN

/// @brief Manages compute shader pipelines, dispatch, and GPU buffer resources.
class EE_API ComputeSubsystem : public Subsystem {
public:
	ComputeSubsystem();
	~ComputeSubsystem() override;

	EE_NO_COPY(ComputeSubsystem)
	EE_NO_MOVE(ComputeSubsystem)

	void attachToRenderer(class RenderSubsystem* renderer);
	bool isReady() const;

	// -- Buffer management --

	ComputeBuf createStructuredBuffer(UInt32 count, UInt32 stride, bool bindUAV);
	ComputeBuf createIndirectArgsBuffer(UInt32 maxDrawCount);
	ComputeBuf createConstantBuffer(UInt32 size, bool dynamic);
	ComputeBuf createStagingBuffer(UInt32 size);

	void updateBuffer(ComputeBuf buf, const void* data, UInt32 size);
	void readback(ComputeBuf stagingBuf, ComputeBuf gpuBuf, UInt32 size, void* dst);

	/// @brief Get a shader resource view from a structured buffer.
	ComputeSRV getBufferSRV(ComputeBuf buf);

	// -- Culling pipeline --

	Result<void, CoreError> initCullingPipeline();

	void dispatchCulling(ComputeBuf instanceBuf, ComputeBuf cullingCB, ComputeBuf visibleMask, UInt32 instanceCount);
	void dispatchCullingCompact(ComputeBuf instanceBuf, ComputeBuf cullingCB, ComputeBuf visibleMask,
	                            ComputeBuf indicesBuf, ComputeBuf argsBuf, UInt32 instanceCount);

	FrustumPlanes computeFrustumPlanes(const Mat4& viewProj);

	void updateCullingCB(ComputeBuf cullingCB, const FrustumPlanes& fp, UInt32 instanceCount);
	void updateCullingCB(ComputeBuf cullingCB, const FrustumPlanes& fp, UInt32 instanceCount,
	                     UInt32 indexCount, UInt32 firstIndex, UInt32 baseVertex);

protected:
	Result<void, CoreError> onInitialize() override;
	void onShutdown() override;

private:
	struct Impl;
	Uptr<Impl> m_impl;
};

EE_NAMESPACE_RENDERING_END
