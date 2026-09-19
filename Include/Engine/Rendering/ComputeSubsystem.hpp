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

	/**
	 * @brief Upload data into a buffer.
	 *
	 * Recorded as a copy in the current command list and serviced from the
	 * frame's upload heap: no per-call GPU allocation and no device stall, so it
	 * is safe to call several times per frame.
	 * @param buf  Destination buffer.
	 * @param data Source data.
	 * @param size Byte count.
	 */
	void updateBuffer(ComputeBuf buf, const void* data, UInt32 size);

	/**
	 * @brief Copy a buffer back to the CPU and wait for it.
	 *
	 * This is a genuine CPU/GPU synchronisation point: the device is allowed to
	 * catch up before the value is returned, so it stalls the frame. Only use it
	 * when the value is required on the CPU in the same frame (for example to
	 * build draw arguments); for statistics and HUD readouts prefer a
	 * fence-delayed read, which costs nothing.
	 * @param stagingBuf A USAGE_STAGING buffer of at least @p size bytes.
	 * @param gpuBuf     Source buffer.
	 * @param size       Byte count to copy.
	 * @param dst        CPU destination.
	 */
	void readback(ComputeBuf stagingBuf, ComputeBuf gpuBuf, UInt32 size, void* dst);

	/// @brief Get a shader resource view from a structured buffer.
	ComputeSRV getBufferSRV(ComputeBuf buf);

	// -- Culling pipeline --

	Result<void, CoreError> initCullingPipeline();

	/**
	 * @brief Frustum-cull instances and compact the visible ones.
	 *
	 * Writes a visibility mask, a compacted index list and the visible count. It
	 * also publishes that count straight into every entry of @p drawArgsBuf, so
	 * the draw arguments are complete when the dispatch returns and the CPU never
	 * has to read the count back. The caller must upload @p drawArgsBuf with
	 * `instanceCount == 0` before this call.
	 * @param instanceBuf  StructuredBuffer<CullingInstance> to test.
	 * @param cullingCB    Constant buffer filled by updateCullingCB().
	 * @param visibleMask  One uint per instance: 1 = visible.
	 * @param indicesBuf   Receives the compacted visible instance indices.
	 * @param counterBuf   Single-uint counter, cleared by the caller.
	 * @param drawArgsBuf  Indirect draw arguments written by the shader.
	 * @param instanceCount Number of instances to test.
	 */
	void dispatchCullingCompact(ComputeBuf instanceBuf, ComputeBuf cullingCB, ComputeBuf visibleMask,
	                            ComputeBuf indicesBuf, ComputeBuf counterBuf, ComputeBuf drawArgsBuf,
	                            UInt32 instanceCount);

	FrustumPlanes computeFrustumPlanes(const Mat4& viewProj);

	void updateCullingCB(ComputeBuf cullingCB, const FrustumPlanes& fp, UInt32 instanceCount);
	/**
	 * @brief Fill the culling constant buffer.
	 * @param cullingCB     Buffer created with createConstantBuffer(sizeof(FrustumPlanes) + sizeof(UInt32) * 8).
	 * @param fp            Frustum planes.
	 * @param instanceCount Instances to test.
	 * @param indexCount    Geometry of the mesh whose draw arguments are built.
	 * @param firstIndex    First index of that geometry.
	 * @param baseVertex    Base vertex of that geometry.
	 * @param meshCount     Number of indirect draw entries the shader publishes
	 *                      the visible count into.
	 */
	void updateCullingCB(ComputeBuf cullingCB, const FrustumPlanes& fp, UInt32 instanceCount,
	                     UInt32 indexCount, UInt32 firstIndex, UInt32 baseVertex, UInt32 meshCount);

protected:
	Result<void, CoreError> onInitialize() override;
	void onShutdown() override;

private:
	struct Impl;
	Uptr<Impl> m_impl;
};

EE_NAMESPACE_RENDERING_END
