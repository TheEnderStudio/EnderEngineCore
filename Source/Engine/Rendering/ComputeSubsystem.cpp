#include <Rendering/ComputeSubsystem.hpp>
#include <Rendering/RenderSubsystem.hpp>
#include <Core/Log.hpp>
#include <vector>

#include <DiligentCore/Graphics/GraphicsEngine/interface/RenderDevice.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/DeviceContext.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Buffer.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Shader.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/PipelineState.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/ShaderResourceBinding.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Fence.h>
#include <DiligentCore/Common/interface/RefCntAutoPtr.hpp>

namespace D = Diligent;

EE_NAMESPACE_RENDERING_BEGIN

// ===================================================================
// Embedded compute shader: Frustum culling
// ===================================================================
static const char* g_CullingCS = R"(
struct CullingInstance {
    float4 boundSphere;
    uint drawIndex;
    float _p0, _p1, _p2;
};

cbuffer CullingCB : register(b0) {
    float4 g_FrustumPlanes[6];
    uint g_InstanceCount;
    uint g_IndexCount;
    uint g_FirstIndex;
    uint g_BaseVertex;
};

StructuredBuffer<CullingInstance> g_Input  : register(t0);
RWStructuredBuffer<uint>          g_Visible : register(u0);
RWStructuredBuffer<uint>          g_Indices : register(u1);
RWStructuredBuffer<uint>          g_Counter : register(u2);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= g_InstanceCount) return;
    float4 s = g_Input[i].boundSphere;
    bool visible = true;
    [unroll]
    for (int p = 0; p < 6; p++) {
        float4 plane = g_FrustumPlanes[p];
        float d = dot(plane.xyz, s.xyz) + plane.w;
        if (d < -s.w) { visible = false; break; }
    }
    g_Visible[i] = visible ? 1 : 0;
    if (visible) {
        uint idx; InterlockedAdd(g_Counter[0], 1, idx);
        g_Indices[idx] = i;
    }
}
)";

struct ComputeSubsystem::Impl {
	RenderSubsystem* renderer = nullptr;
	bool ok = false;

	// Culling pipeline
	D::RefCntAutoPtr<D::IPipelineState> cullingPSO;
	D::RefCntAutoPtr<D::IShaderResourceBinding> cullingSRB;

	// Fence for GPU→CPU sync
	D::RefCntAutoPtr<D::IFence> fence;
	D::Uint64 nextFenceValue = 1;

	// Helper: get Diligent device
	D::IRenderDevice* device() const {
		return renderer ? static_cast<D::IRenderDevice*>(renderer->getDevice()) : nullptr;
	}
	D::IDeviceContext* ctx() const {
		return renderer ? static_cast<D::IDeviceContext*>(renderer->getContext()) : nullptr;
	}
};

ComputeSubsystem::ComputeSubsystem() : Subsystem("Compute"), m_impl(std::make_unique<Impl>()) {}
ComputeSubsystem::~ComputeSubsystem() = default;

void ComputeSubsystem::attachToRenderer(RenderSubsystem* r) { m_impl->renderer = r; m_impl->ok = r != nullptr; }
bool ComputeSubsystem::isReady() const { return m_impl->ok; }

void* ComputeSubsystem::createStructuredBuffer(UInt32 count, UInt32 stride, bool bindUAV) {
	auto* dev = m_impl->device(); if (!dev) return nullptr;
	D::BufferDesc bd;
	bd.Name = "CS_StructuredBuf";
	bd.Usage = D::USAGE_DEFAULT;
	bd.BindFlags = D::BIND_SHADER_RESOURCE;
	if (bindUAV) bd.BindFlags |= D::BIND_UNORDERED_ACCESS;
	bd.Mode = D::BUFFER_MODE_STRUCTURED;
	bd.ElementByteStride = stride;
	bd.Size = static_cast<D::Uint64>(count) * stride;
	D::RefCntAutoPtr<D::IBuffer> buf;
	dev->CreateBuffer(bd, nullptr, &buf);
	return buf.Detach();
}

void* ComputeSubsystem::createIndirectArgsBuffer(UInt32 maxDrawCount) {
	auto* dev = m_impl->device(); if (!dev) return nullptr;
	D::BufferDesc bd;
	bd.Name = "CS_IndirectArgs";
	bd.Usage = D::USAGE_DEFAULT;
	bd.BindFlags = D::BIND_INDIRECT_DRAW_ARGS | D::BIND_UNORDERED_ACCESS;
	bd.Mode = D::BUFFER_MODE_STRUCTURED;
	bd.ElementByteStride = sizeof(IndirectDrawArgs);
	bd.Size = static_cast<D::Uint64>(maxDrawCount) * sizeof(IndirectDrawArgs);
	D::RefCntAutoPtr<D::IBuffer> buf;
	dev->CreateBuffer(bd, nullptr, &buf);
	return buf.Detach();
}

void* ComputeSubsystem::createConstantBuffer(UInt32 size, bool dynamic) {
	auto* dev = m_impl->device(); if (!dev) return nullptr;
	D::BufferDesc bd;
	bd.Name = "CS_ConstBuf";
	bd.Size = size;
	bd.BindFlags = D::BIND_UNIFORM_BUFFER;
	if (dynamic) { bd.Usage = D::USAGE_DYNAMIC; bd.CPUAccessFlags = D::CPU_ACCESS_WRITE; }
	else bd.Usage = D::USAGE_DEFAULT;
	D::RefCntAutoPtr<D::IBuffer> buf;
	dev->CreateBuffer(bd, nullptr, &buf);
	return buf.Detach();
}

void* ComputeSubsystem::createStagingBuffer(UInt32 size) {
	auto* dev = m_impl->device(); if (!dev) return nullptr;
	D::BufferDesc bd;
	bd.Name = "CS_StagingBuf";
	bd.Usage = D::USAGE_STAGING;
	bd.CPUAccessFlags = D::CPU_ACCESS_READ;
	bd.Size = size;
	D::RefCntAutoPtr<D::IBuffer> buf;
	dev->CreateBuffer(bd, nullptr, &buf);
	return buf.Detach();
}

void ComputeSubsystem::readback(void* stagingBuf, void* gpuBuf, UInt32 size, void* dst) {
	auto* ctx = m_impl->ctx(); if (!ctx || !stagingBuf || !gpuBuf || !dst) return;
	auto* stg = static_cast<D::IBuffer*>(stagingBuf);
	auto* gpu = static_cast<D::IBuffer*>(gpuBuf);
	ctx->CopyBuffer(gpu, 0, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
	                stg, 0, size, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	ctx->WaitForIdle();
	D::PVoid pData = nullptr;
	ctx->MapBuffer(stg, D::MAP_READ, D::MAP_FLAG_DO_NOT_WAIT, pData);
	if (pData) {
		memcpy(dst, pData, size);
		ctx->UnmapBuffer(stg, D::MAP_READ);
	}
}

ComputeSRV ComputeSubsystem::getBufferSRV(ComputeBuf buf) {
	if (!buf) return nullptr;
	return static_cast<D::IBuffer*>(buf)->GetDefaultView(D::BUFFER_VIEW_SHADER_RESOURCE);
}

FrustumPlanes ComputeSubsystem::computeFrustumPlanes(const Mat4& vp) {
	FrustumPlanes r;
	r.planes[0] = Vec4(vp[0][3] + vp[0][0], vp[1][3] + vp[1][0], vp[2][3] + vp[2][0], vp[3][3] + vp[3][0]);
	r.planes[1] = Vec4(vp[0][3] - vp[0][0], vp[1][3] - vp[1][0], vp[2][3] - vp[2][0], vp[3][3] - vp[3][0]);
	r.planes[2] = Vec4(vp[0][3] + vp[0][1], vp[1][3] + vp[1][1], vp[2][3] + vp[2][1], vp[3][3] + vp[3][1]);
	r.planes[3] = Vec4(vp[0][3] - vp[0][1], vp[1][3] - vp[1][1], vp[2][3] - vp[2][1], vp[3][3] - vp[3][1]);
	r.planes[4] = Vec4(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
	r.planes[5] = Vec4(vp[0][3] - vp[0][2], vp[1][3] - vp[1][2], vp[2][3] - vp[2][2], vp[3][3] - vp[3][2]);
	for (int i = 0; i < 6; i++) {
		F32 l = sqrtf(r.planes[i].x * r.planes[i].x + r.planes[i].y * r.planes[i].y + r.planes[i].z * r.planes[i].z);
		if (l > 0.0001f) { r.planes[i].x /= l; r.planes[i].y /= l; r.planes[i].z /= l; r.planes[i].w /= l; }
	}
	return r;
}

void ComputeSubsystem::updateCullingCB(void* cullingCB, const FrustumPlanes& fp, UInt32 instanceCount, UInt32 indexCount, UInt32 firstIndex, UInt32 baseVertex) {
	auto* ctx = m_impl->ctx(); if (!ctx || !cullingCB) return;
	auto* buf = static_cast<D::IBuffer*>(cullingCB);
	D::PVoid d = nullptr; ctx->MapBuffer(buf, D::MAP_WRITE, D::MAP_FLAG_DISCARD, d);
	if (d) {
		struct { float p[6][4]; UInt32 cnt, icnt, fidx, bvtx; } cb;
		for (int i = 0; i < 6; i++)
			for (int j = 0; j < 4; j++) cb.p[i][j] = fp.planes[i][j];
		cb.cnt = instanceCount; cb.icnt = indexCount; cb.fidx = firstIndex; cb.bvtx = baseVertex;
		memcpy(d, &cb, sizeof(cb)); ctx->UnmapBuffer(buf, D::MAP_WRITE);
	}
}

void ComputeSubsystem::updateCullingCB(void* cullingCB, const FrustumPlanes& fp, UInt32 instanceCount) {
	updateCullingCB(cullingCB, fp, instanceCount, 0, 0, 0);
}

Result<void, CoreError> ComputeSubsystem::initCullingPipeline() {
	auto& p = *m_impl;
	auto* dev = p.device(); if (!dev) return CoreError::OperationFailed;

	// Create compute shader
	D::ShaderCreateInfo sci;
	sci.Desc.ShaderType = D::SHADER_TYPE_COMPUTE;
	sci.Desc.Name = "FrustumCullingCS";
	sci.EntryPoint = "main";
	sci.SourceLanguage = D::SHADER_SOURCE_LANGUAGE_HLSL;
	sci.Source = g_CullingCS;
	sci.SourceLength = static_cast<D::Uint32>(strlen(g_CullingCS));
	D::RefCntAutoPtr<D::IShader> cs;
	dev->CreateShader(sci, &cs);
	if (!cs || cs->GetStatus() != D::SHADER_STATUS_READY) {
		EError("Compute: failed to create culling shader");
		return CoreError::OperationFailed;
	}

	// Create compute PSO
	D::ComputePipelineStateCreateInfo ci;
	ci.PSODesc.Name = "FrustumCullingPSO";
	ci.PSODesc.PipelineType = D::PIPELINE_TYPE_COMPUTE;
	ci.PSODesc.ResourceLayout.DefaultVariableType = D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
	ci.pCS = cs;
	dev->CreateComputePipelineState(ci, &p.cullingPSO);
	if (!p.cullingPSO) {
		EError("Compute: failed to create culling PSO");
		return CoreError::OperationFailed;
	}

	p.cullingPSO->CreateShaderResourceBinding(&p.cullingSRB, true);
	p.ok = true;
	EInfo("Compute: frustum culling pipeline ready");
	return {};
}

void ComputeSubsystem::updateBuffer(void* buf, const void* data, UInt32 size) {
	auto* ctx = m_impl->ctx(); if (!ctx || !buf || !data) return;
	auto* b = static_cast<D::IBuffer*>(buf);
	// Create temporary staging buffer for upload
	D::BufferDesc sd; sd.Name = "CS_TempStaging"; sd.Usage = D::USAGE_STAGING;
	sd.CPUAccessFlags = D::CPU_ACCESS_WRITE; sd.Size = size;
	D::RefCntAutoPtr<D::IBuffer> stg;
	m_impl->device()->CreateBuffer(sd, nullptr, &stg);
	if (!stg) return;
	D::PVoid d = nullptr; ctx->MapBuffer(stg, D::MAP_WRITE, D::MAP_FLAG_DISCARD, d);
	if (d) { memcpy(d, data, size); ctx->UnmapBuffer(stg, D::MAP_WRITE); }
	ctx->CopyBuffer(stg, 0, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
	                b, 0, size, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	ctx->WaitForIdle();
}

void ComputeSubsystem::dispatchCulling(void* instanceBuf, void* cullingCB, void* visibleMask, UInt32 instanceCount) {
	dispatchCullingCompact(instanceBuf, cullingCB, visibleMask, nullptr, nullptr, instanceCount);
}

void ComputeSubsystem::dispatchCullingCompact(void* instanceBuf, void* cullingCB, void* visibleMask,
                                              void* indicesBuf, void* argsBuf, UInt32 instanceCount) {
	auto& p = *m_impl;
	if (!p.ok || !p.cullingPSO || !instanceBuf || !cullingCB || !visibleMask) return;
	auto* ctx = p.ctx();
	auto* srb = p.cullingSRB.RawPtr();
	if (auto* v = srb->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_Input"))
		v->Set(static_cast<D::IBuffer*>(instanceBuf)->GetDefaultView(D::BUFFER_VIEW_SHADER_RESOURCE));
	if (auto* v = srb->GetVariableByName(D::SHADER_TYPE_COMPUTE, "CullingCB"))
		v->Set(static_cast<D::IBuffer*>(cullingCB));
	if (auto* v = srb->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_Visible"))
		v->Set(static_cast<D::IBuffer*>(visibleMask)->GetDefaultView(D::BUFFER_VIEW_UNORDERED_ACCESS));
	if (auto* v = srb->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_Indices"))
		v->Set(indicesBuf ? static_cast<D::IBuffer*>(indicesBuf)->GetDefaultView(D::BUFFER_VIEW_UNORDERED_ACCESS) : nullptr);
	if (auto* v = srb->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_Counter"))
		v->Set(argsBuf ? static_cast<D::IBuffer*>(argsBuf)->GetDefaultView(D::BUFFER_VIEW_UNORDERED_ACCESS) : nullptr);
	ctx->SetPipelineState(p.cullingPSO);
	ctx->CommitShaderResources(srb, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	D::DispatchComputeAttribs da;
	da.ThreadGroupCountX = (instanceCount + 63) / 64;
	ctx->DispatchCompute(da);
}

Result<void, CoreError> ComputeSubsystem::onInitialize() {
	if (!m_impl->renderer) {
		EError("Compute: no renderer attached");
		return CoreError::OperationFailed;
	}
	return initCullingPipeline();
}

void ComputeSubsystem::onShutdown() {
	m_impl->cullingPSO.Release();
	m_impl->cullingSRB.Release();
	m_impl->fence.Release();
	m_impl->ok = false;
}

EE_NAMESPACE_RENDERING_END
