#include <Rendering/RayTracingSubsystem.hpp>
#include <Rendering/RenderSubsystem.hpp>
#include <Core/Log.hpp>

#include <DiligentCore/Graphics/GraphicsEngine/interface/RenderDevice.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/DeviceContext.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/BottomLevelAS.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/TopLevelAS.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Shader.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/PipelineState.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Buffer.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Sampler.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/GraphicsTypes.h>
#include <DiligentCore/Common/interface/RefCntAutoPtr.hpp>
#include <DiligentCore/Common/interface/AdvancedMath.hpp>

#include "ResourcePool.hpp"

#include <cstring>

namespace D = Diligent;

EE_NAMESPACE_RENDERING_BEGIN

// ===================================================================
// Bindless scene structures (must match the HLSL layouts below).
// glm matrices are column-major and uploaded as-is (mul(matrix, vector)).
// ===================================================================

/// @brief Per-object ray tracing attribs (HLSL: ObjectAttribs).
struct alignas(16) RTObjectAttribs {
	Mat4 modelMat;            ///< Object-to-world (column-major).
	Mat4 normalMat;           ///< Normal transform (column-major).
	UInt32 materialId;        ///< Index into the material attribs buffer.
	UInt32 firstIndex;        ///< First index in the (per-mesh) index buffer (0 for engine meshes).
	UInt32 firstVertex;       ///< First vertex in the (per-mesh) vertex buffer (0 for engine meshes).
	UInt32 meshId;            ///< Reserved.
};
static_assert(sizeof(RTObjectAttribs) % 16 == 0, "RTObjectAttribs must be 16-byte aligned");

/// @brief Per-material ray tracing attribs (HLSL: MaterialAttribs).
struct alignas(16) RTMaterialAttribs {
	Vec4 baseColorMask;       ///< Base color tint.
	UInt32 sampInd;           ///< Index into the sampler array (always 0 for now).
	UInt32 baseColorTexInd;   ///< Index into the texture array.
	UInt32 padding0;
	UInt32 padding1;
};
static_assert(sizeof(RTMaterialAttribs) % 16 == 0, "RTMaterialAttribs must be 16-byte aligned");

static constexpr UInt32 MaxSceneTextures = 16;

// ===================================================================
// Inline ray tracing compute shader (RayQuery, DXR 1.1 / SM 6.5).
// Engine convention: column-major matrices, mul(matrix, vector).
// ===================================================================
static const char* g_RayQueryCS = R"(
struct Vertex {
    float3 pos;
    float3 norm;
    float2 uv;
    float4 tangent;
};
struct ObjectAttribs {
    float4x4 ModelMat;
    float4x4 NormalMat;
    uint MaterialId;
    uint FirstIndex;
    uint FirstVertex;
    uint MeshId;
};
struct MaterialAttribs {
    float4 BaseColorMask;
    uint   SampInd;
    uint   BaseColorTexInd;
    uint   padding0;
    uint   padding1;
};
struct RTConstants {
    float4x4 ViewProjInv;
    float4   LightDir;
    float4   CameraPos;
    float    MaxRayLength;
    float    AmbientLight;
    float    _pad0;
    float    _pad1;
};

RaytracingAccelerationStructure g_TLAS            : register(t0);
StructuredBuffer<ObjectAttribs>   g_ObjectAttribs : register(t1);
StructuredBuffer<MaterialAttribs> g_MaterialAttribs : register(t2);
StructuredBuffer<Vertex>          g_VertexBuffer  : register(t3);
StructuredBuffer<uint>            g_IndexBuffer   : register(t4);
Texture2D<float4>                 g_GBufferNormal : register(t5);
Texture2D<float>                  g_GBufferDepth  : register(t6);
Texture2D<float4>                 g_Textures[16]  : register(t7);
RWTexture2D<float4>               g_OutRT         : register(u0);
SamplerState                      g_Sampler       : register(s0);
ConstantBuffer<RTConstants>       g_RTConstants   : register(b0);

#define SMALL_OFFSET 0.0001

// Returns 0 when an occluder is found, 1 otherwise.
float CastShadow(float3 Origin, float3 RayDir, float MaxRayLength)
{
    RayDesc ray;
    ray.Origin    = Origin;
    ray.Direction = RayDir;
    ray.TMin      = 0.0;
    ray.TMax      = MaxRayLength;

    RayQuery<RAY_FLAG_CULL_FRONT_FACING_TRIANGLES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, ~0u, ray);
    q.Proceed();
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}

float4 GetSkyColor(float3 Dir, float3 LightDir)
{
    Dir.y += 0.075;
    Dir = normalize(Dir);
    float CosTheta = dot(Dir, LightDir);
    float Scatter  = pow(saturate(0.5 * (1.0 - CosTheta)), 0.2);
    float3 Sky     = pow(saturate(CosTheta - 0.02), 50.0) * saturate(LightDir.y * 5.0);
    float3 Dome    = float3(0.07, 0.11, 0.23) * lerp(max(Scatter, 0.1), 1.0, saturate(Dir.y)) / max(Dir.y, 0.01);
    Dome *= 13.0 / max(length(Dome), 13.0);
    float3 Horizon = pow(Dome, float3(1.0, 1.0, 1.0) - Dome);
    Sky += lerp(Horizon, Dome / (Dome + 0.5), saturate(Dir.y * 2.0));
    Sky *= 1.0 + pow(1.0 - Scatter, 10.0) * 10.0;
    Sky *= 1.0 - abs(1.0 - Dir.y) * 0.5;
    return float4(Sky, 1.0);
}

struct ReflectionResult {
    float4 BaseColor;
    float  NdotL;
    bool   Found;
};

// Trace one reflection ray and shade the hit point (material + shadow).
ReflectionResult Reflect(float3 Origin, float3 ReflDir, float MaxReflLen, float MaxShadowLen, float3 CameraPos, float3 LightDir)
{
    RayDesc ray;
    ray.Origin    = Origin;
    ray.Direction = ReflDir;
    ray.TMin      = 0.0;
    ray.TMax      = MaxReflLen;

    // Rasterization uses back-face culling; keep the same for reflections.
    RayQuery<RAY_FLAG_CULL_BACK_FACING_TRIANGLES> q;
    q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, ~0u, ray);
    q.Proceed();

    ReflectionResult res;
    res.BaseColor = float4(0, 0, 0, 0);
    res.NdotL     = 0.0;
    res.Found     = false;

    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        uint          InstId = q.CommittedInstanceID();
        ObjectAttribs Obj    = g_ObjectAttribs[InstId];
        MaterialAttribs Mtr  = g_MaterialAttribs[Obj.MaterialId];

        uint  PrimInd     = q.CommittedPrimitiveIndex();
        uint3 TriangleInd = uint3(g_IndexBuffer[Obj.FirstIndex + PrimInd * 3 + 0],
                                  g_IndexBuffer[Obj.FirstIndex + PrimInd * 3 + 1],
                                  g_IndexBuffer[Obj.FirstIndex + PrimInd * 3 + 2]);
        Vertex Vert0 = g_VertexBuffer[TriangleInd.x + Obj.FirstVertex];
        Vertex Vert1 = g_VertexBuffer[TriangleInd.y + Obj.FirstVertex];
        Vertex Vert2 = g_VertexBuffer[TriangleInd.z + Obj.FirstVertex];

        float3 Bary;
        Bary.yz = q.CommittedTriangleBarycentrics();
        Bary.x  = 1.0 - Bary.y - Bary.z;

        float2 UV   = Vert0.uv * Bary.x + Vert1.uv * Bary.y + Vert2.uv * Bary.z;
        float3 Norm = Vert0.norm * Bary.x + Vert1.norm * Bary.y + Vert2.norm * Bary.z;
        Norm = normalize(mul((float3x3)Obj.NormalMat, Norm));

        res.BaseColor = Mtr.BaseColorMask * g_Textures[NonUniformResourceIndex(Mtr.BaseColorTexInd)].SampleLevel(g_Sampler, UV, 0);
        res.NdotL     = max(0.0, dot(LightDir, Norm));
        res.Found     = true;

        if (res.NdotL > 0.0)
        {
            float3 HitPos = Origin + ReflDir * q.CommittedRayT();
            res.NdotL *= CastShadow(HitPos + Norm * SMALL_OFFSET * length(HitPos - CameraPos), LightDir, MaxShadowLen);
        }
    }
    return res;
}

float3 ScreenPosToWorldPos(float2 uv, float depth, float4x4 vpInv)
{
    float4 clip = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), depth, 1.0);
    float4 w    = mul(vpInv, clip);
    return w.xyz / w.w;
}

[numthreads(8, 8, 1)]
void CSMain(uint2 DTid : SV_DispatchThreadID)
{
    uint2 Dim;
    g_OutRT.GetDimensions(Dim.x, Dim.y);
    if (DTid.x >= Dim.x || DTid.y >= Dim.y)
        return;

    float Depth = g_GBufferDepth.Load(int3(DTid, 0)).x;
    if (Depth >= 1.0)
    {
        g_OutRT[DTid] = float4(0, 0, 0, 1);
        return;
    }

    float2 UV        = (float2(DTid) + 0.5) / float2(Dim);
    float3 WPos      = ScreenPosToWorldPos(UV, Depth, g_RTConstants.ViewProjInv);
    float3 LightDir  = g_RTConstants.LightDir.xyz;
    float3 WNormal   = normalize(g_GBufferNormal.Load(int3(DTid, 0)).xyz);
    float  DisToCam  = length(WPos - g_RTConstants.CameraPos.xyz);
    float3 ViewDir   = (WPos - g_RTConstants.CameraPos.xyz) / max(DisToCam, 0.0001);

    float NdotL = max(0.0, dot(LightDir, WNormal));
    if (NdotL > 0.0)
        NdotL *= CastShadow(WPos + WNormal * SMALL_OFFSET * DisToCam, LightDir, g_RTConstants.MaxRayLength);

    float4 Color = float4(0, 0, 0, 1);
    {
        ReflectionResult refl = Reflect(WPos + WNormal * SMALL_OFFSET * DisToCam,
                                        reflect(ViewDir, WNormal),
                                        g_RTConstants.MaxRayLength,
                                        g_RTConstants.MaxRayLength,
                                        g_RTConstants.CameraPos.xyz,
                                        LightDir);
        if (refl.Found)
            Color = refl.BaseColor * max(g_RTConstants.AmbientLight, refl.NdotL);
        else
            Color = GetSkyColor(reflect(ViewDir, WNormal), LightDir);
    }

    Color.a = max(g_RTConstants.AmbientLight, NdotL);
    g_OutRT[DTid] = Color;
}
)";

// ===================================================================
// Compose pass (fullscreen triangle, alpha blend preserves the skybox).
// ===================================================================
static const char* g_ComposeVS = R"(
struct PSIn { float4 Pos : SV_POSITION; float2 UV : TEXCOORD0; };
void main(uint vid : SV_VertexID, out PSIn o) {
    o.UV  = float2(vid >> 1, vid & 1) * 2.0;
    o.Pos = float4(o.UV * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* g_ComposePS = R"(
Texture2D<float4> g_GBufferColor  : register(t0);
Texture2D<float4> g_GBufferNormal : register(t1);
Texture2D<float>  g_GBufferDepth  : register(t2);
Texture2D<float4> g_RayTracedTex  : register(t3);
cbuffer ComposeCB : register(b0) {
    float4x4 g_ViewProjInv;
    float4   g_CameraPos;
};
struct PSIn { float4 Pos : SV_POSITION; float2 UV : TEXCOORD0; };

float3 ScreenPosToWorldPos(float2 uv, float depth, float4x4 vpInv)
{
    float4 clip = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), depth, 1.0);
    float4 w    = mul(vpInv, clip);
    return w.xyz / w.w;
}

float4 main(PSIn i) : SV_Target
{
    float2 Dim;
    g_GBufferColor.GetDimensions(Dim.x, Dim.y);
    float2 UV = float2(i.UV.x, 1.0 - i.UV.y); // flip Y: fullscreen UV -> texture UV

    int3 tc  = int3(UV * Dim, 0);
    float Depth = g_GBufferDepth.Load(tc).x;
    if (Depth >= 1.0)
        return float4(0, 0, 0, 0); // background: alpha 0 preserves the skybox

    float4 Color  = g_GBufferColor.Load(tc);
    float3 Normal = normalize(g_GBufferNormal.Load(tc).xyz);
    float4 RT     = g_RayTracedTex.Load(tc);

    float3 WPos    = ScreenPosToWorldPos(UV, Depth, g_ViewProjInv);
    float3 ViewDir = normalize(g_CameraPos.xyz - WPos);
    float  NdotV   = saturate(dot(Normal, ViewDir));
    float  R       = lerp(0.04, 1.0, pow(1.0 - NdotV, 5.0));

    float3 Shaded = Color.rgb * RT.a;
    float3 Final  = lerp(Shaded, RT.rgb, R);
    return float4(Final, 1.0);
}
)";

// ===================================================================
// Acceleration structure data (owned by this subsystem).
// ===================================================================
struct BLASData { D::RefCntAutoPtr<D::IBottomLevelAS> blas; MeshHandle sourceMesh; };
struct TLASData {
	D::RefCntAutoPtr<D::ITopLevelAS> tlas;
	D::RefCntAutoPtr<D::IBuffer> scratch;
	D::RefCntAutoPtr<D::IBuffer> instanceBuf;
	UInt32 maxInstances = 0;
	bool allowUpdate = true;
	bool built = false;
};

struct RayTracingSubsystem::Impl {
	RenderSubsystem* renderer = nullptr;
	bool ok = false;

	D::IRenderDevice* device() const { return renderer ? static_cast<D::IRenderDevice*>(renderer->getDevice()) : nullptr; }
	D::IDeviceContext* ctx() const { return renderer ? static_cast<D::IDeviceContext*>(renderer->getContext()) : nullptr; }

	// Acceleration structures
	Detail::ResourcePool<BLASData> blases{ 4096 };
	Detail::ResourcePool<TLASData> tlases{ 8 };
	D::RefCntAutoPtr<D::IBuffer> blasScratch;
	TLASData sceneTLAS;                 ///< Internal TLAS managed by updateScene().
	Vector<BLASHandle> blasCache;       ///< BLAS per unique mesh (parallel to blasCacheMesh).
	Vector<MeshHandle> blasCacheMesh;

	// Shared scene geometry (all meshes merged, for the ray query shader).
	D::RefCntAutoPtr<D::IBuffer> sharedVB;
	D::RefCntAutoPtr<D::IBuffer> sharedIB;
	Vector<MeshHandle> sceneMeshKeys;      ///< Meshes the shared buffers were built for.
	Vector<UInt32> sceneMeshFirstVertex;   ///< First vertex in sharedVB per mesh.
	Vector<UInt32> sceneMeshFirstIndex;    ///< First index in sharedIB per mesh.

	// Bindless scene
	D::RefCntAutoPtr<D::IBuffer> objectAttribsBuf;
	D::RefCntAutoPtr<D::IBuffer> materialAttribsBuf;
	Vector<D::ITextureView*> sceneTextureSRVs;           ///< SRVs bound into the compute shader.
	D::RefCntAutoPtr<D::ITextureView> whiteSRV;
	D::RefCntAutoPtr<D::ISampler> linearSampler;

	// RT compute pipeline
	D::RefCntAutoPtr<D::IPipelineState> rtPSO;
	D::RefCntAutoPtr<D::IShaderResourceBinding> rtSRB;
	D::RefCntAutoPtr<D::IBuffer> constantsBuf;

	// Compose pipeline
	D::RefCntAutoPtr<D::IPipelineState> composePSO;
	D::RefCntAutoPtr<D::IShaderResourceBinding> composeSRB;
	D::RefCntAutoPtr<D::IBuffer> composeCB;

	Result<void, RenderError> buildTLAS(TLASData& dd, const Vector<TLASInstance>& instances);
};

RayTracingSubsystem::RayTracingSubsystem() : Subsystem("RayTracing"), m_impl(std::make_unique<Impl>()) {}
RayTracingSubsystem::~RayTracingSubsystem() = default;

void RayTracingSubsystem::attachToRenderer(RenderSubsystem* r) { m_impl->renderer = r; }
bool RayTracingSubsystem::isReady() const { return m_impl->ok; }

// ===================================================================
// Acceleration structures
// ===================================================================

Result<BLASHandle, RenderError> RayTracingSubsystem::createBLAS(MeshHandle mesh) {
	auto& p = *m_impl;
	auto* dev = p.device(); if (!dev) return RenderError::NotInitialized;
	auto* ctx = p.ctx(); if (!ctx) return RenderError::NotInitialized;

	void* vb = nullptr; void* ib = nullptr; UInt32 vc = 0; UInt32 ic = 0;
	p.renderer->getMeshGeometry(mesh, vb, ib, vc, ic);
	if (!vb || !ib || ic == 0) return RenderError::InvalidHandle;

	D::BLASTriangleDesc tri;
	tri.GeometryName         = "Mesh";
	tri.MaxVertexCount       = vc;
	tri.VertexValueType      = D::VT_FLOAT32;
	tri.VertexComponentCount = 3;
	tri.MaxPrimitiveCount    = ic / 3;
	tri.IndexType            = D::VT_UINT32;

	D::BottomLevelASDesc ad;
	ad.Name          = "EE_RT_BLAS";
	ad.Flags         = D::RAYTRACING_BUILD_AS_PREFER_FAST_TRACE;
	ad.pTriangles    = &tri;
	ad.TriangleCount = 1;
	D::RefCntAutoPtr<D::IBottomLevelAS> blas;
	dev->CreateBLAS(ad, &blas);
	if (!blas) { EError("RayTracing: BLAS creation failed for mesh {}", mesh.index); return RenderError::AccelerationStructureCreationFailed; }

	{
		const auto sz = blas->GetScratchBufferSizes().Build;
		if (!p.blasScratch || p.blasScratch->GetDesc().Size < sz) {
			D::BufferDesc sd; sd.Name = "RT BLAS Scratch"; sd.Usage = D::USAGE_DEFAULT; sd.BindFlags = D::BIND_RAY_TRACING; sd.Size = sz;
			p.blasScratch.Release();
			dev->CreateBuffer(sd, nullptr, &p.blasScratch);
			if (!p.blasScratch) return RenderError::BufferCreationFailed;
		}
	}

	D::BLASBuildTriangleData td;
	td.GeometryName         = tri.GeometryName;
	td.pVertexBuffer        = static_cast<D::IBuffer*>(vb);
	td.VertexStride         = sizeof(Vertex);
	td.VertexCount          = vc;
	td.VertexValueType      = tri.VertexValueType;
	td.VertexComponentCount = tri.VertexComponentCount;
	td.pIndexBuffer         = static_cast<D::IBuffer*>(ib);
	td.PrimitiveCount       = tri.MaxPrimitiveCount;
	td.IndexType            = tri.IndexType;
	td.Flags                = D::RAYTRACING_GEOMETRY_FLAG_OPAQUE;

	D::BuildBLASAttribs ba;
	ba.pBLAS             = blas;
	ba.pTriangleData     = &td;
	ba.TriangleDataCount = 1;
	ba.pScratchBuffer    = p.blasScratch;
	ba.BLASTransitionMode          = D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
	ba.GeometryTransitionMode      = D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
	ba.ScratchBufferTransitionMode = D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
	ctx->BuildBLAS(ba);

	auto a = p.blases.allocate();
	auto* dd = p.blases.getUnchecked(a.index);
	dd->blas = std::move(blas);
	dd->sourceMesh = mesh;
	EInfo("RayTracing: BLAS created for mesh {} ({} verts, {} primitives)", mesh.index, vc, ic / 3);
	return BLASHandle{ a.index, a.generation };
}

Result<TLASHandle, RenderError> RayTracingSubsystem::createTLAS(UInt32 maxInstances, bool allowUpdate) {
	auto& p = *m_impl;
	auto* dev = p.device(); if (!dev) return RenderError::NotInitialized;
	if (maxInstances == 0) return RenderError::InvalidArgument;

	D::TopLevelASDesc td;
	td.Name             = "EE_RT_TLAS";
	td.MaxInstanceCount = maxInstances;
	td.Flags            = D::RAYTRACING_BUILD_AS_PREFER_FAST_TRACE;
	if (allowUpdate) td.Flags |= D::RAYTRACING_BUILD_AS_ALLOW_UPDATE;
	D::RefCntAutoPtr<D::ITopLevelAS> tlas;
	dev->CreateTLAS(td, &tlas);
	if (!tlas) { EError("RayTracing: TLAS creation failed"); return RenderError::AccelerationStructureCreationFailed; }

	auto a = p.tlases.allocate();
	auto* dd = p.tlases.getUnchecked(a.index);
	dd->tlas = std::move(tlas);
	dd->maxInstances = maxInstances;
	dd->allowUpdate = allowUpdate;
	EInfo("RayTracing: TLAS created (maxInstances={}, allowUpdate={})", maxInstances, allowUpdate);
	return TLASHandle{ a.index, a.generation };
}

Result<void, RenderError> RayTracingSubsystem::buildTLAS(TLASHandle h, const Vector<TLASInstance>& instances) {
	auto& p = *m_impl;
	auto* dd = p.tlases.get(h.index, h.generation);
	if (!dd || !dd->tlas) return RenderError::InvalidHandle;
	return p.buildTLAS(*dd, instances);
}

Result<void, RenderError> RayTracingSubsystem::Impl::buildTLAS(TLASData& dd, const Vector<TLASInstance>& instances) {
	auto& p = *this;
	auto* dev = p.device(); if (!dev) return RenderError::NotInitialized;
	auto* ctx = p.ctx(); if (!ctx) return RenderError::NotInitialized;
	if (instances.empty()) return RenderError::InvalidArgument;
	if (instances.size() > dd.maxInstances) { EError("RayTracing: {} instances exceed TLAS capacity {}", instances.size(), dd.maxInstances); return RenderError::InvalidArgument; }

	if (!dd.scratch) {
		D::BufferDesc sd; sd.Name = "RT TLAS Scratch"; sd.Usage = D::USAGE_DEFAULT; sd.BindFlags = D::BIND_RAY_TRACING;
		sd.Size = std::max(dd.tlas->GetScratchBufferSizes().Build, dd.tlas->GetScratchBufferSizes().Update);
		dev->CreateBuffer(sd, nullptr, &dd.scratch);
		if (!dd.scratch) return RenderError::BufferCreationFailed;
	}
	if (!dd.instanceBuf) {
		D::BufferDesc id; id.Name = "RT TLAS Instances"; id.Usage = D::USAGE_DEFAULT; id.BindFlags = D::BIND_RAY_TRACING;
		id.Size = static_cast<D::Uint64>(D::TLAS_INSTANCE_DATA_SIZE) * dd.maxInstances;
		dev->CreateBuffer(id, nullptr, &dd.instanceBuf);
		if (!dd.instanceBuf) return RenderError::BufferCreationFailed;
	}

	Vector<D::TLASBuildInstanceData> insts;
	insts.reserve(instances.size());
	Vector<String> names;
	names.reserve(instances.size()); // reserve so c_str() pointers stay valid
	for (const auto& in : instances) {
		auto* bd = p.blases.get(in.blas.index, in.blas.generation);
		if (!bd || !bd->blas) { EError("RayTracing: invalid BLAS handle {}:{}", in.blas.index, in.blas.generation); return RenderError::InvalidHandle; }
		D::TLASBuildInstanceData di;
		// Diligent requires a non-null instance name.
		names.push_back(in.name.empty() ? "Instance" + std::to_string(insts.size()) : in.name);
		di.InstanceName = names.back().c_str();
		di.CustomId     = in.customId;
		di.Mask         = in.mask;
		di.pBLAS        = bd->blas;
		// Diligent's InstanceMatrix::SetRotation reads the source matrix in
		// COLUMN-major order (data[r][c] = pMatrix[c*RowSize + r]), which matches
		// glm's storage directly - pass the raw glm matrix (value_ptr). The
		// translation is the 4th column of the column-major matrix (m[3][0..2]).
		di.Transform.SetRotation(glm::value_ptr(in.transform), 4);
		di.Transform.SetTranslation(in.transform[3][0], in.transform[3][1], in.transform[3][2]);
		insts.push_back(di);
	}

	D::BuildTLASAttribs ba;
	ba.pTLAS            = dd.tlas;
	ba.Update           = dd.built && dd.allowUpdate;
	ba.pInstances       = insts.data();
	ba.InstanceCount    = static_cast<D::Uint32>(insts.size());
	ba.pInstanceBuffer  = dd.instanceBuf;
	ba.pScratchBuffer   = dd.scratch;
	ba.TLASTransitionMode          = D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
	ba.BLASTransitionMode          = D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
	ba.InstanceBufferTransitionMode = D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
	ba.ScratchBufferTransitionMode  = D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
	ctx->BuildTLAS(ba);
	dd.built = true;
	return {};
}

// ===================================================================
// Bindless scene
// ===================================================================

Result<void, RenderError> RayTracingSubsystem::updateScene(const Vector<RayTracedObject>& objects) {
	auto& p = *m_impl;
	auto* dev = p.device(); if (!dev) return RenderError::NotInitialized;
	auto* ctx = p.ctx(); if (!ctx) return RenderError::NotInitialized;
	if (objects.empty() || !p.rtPSO) return RenderError::InvalidArgument;

	// 1. Ensure a BLAS exists for every unique mesh.
	Vector<BLASHandle> objBLAS(objects.size());
	for (size_t i = 0; i < objects.size(); ++i) {
		const MeshHandle mh = objects[i].mesh;
		BLASHandle bh;
		bool found = false;
		for (size_t c = 0; c < p.blasCacheMesh.size(); ++c) {
			if (p.blasCacheMesh[c].index == mh.index && p.blasCacheMesh[c].generation == mh.generation) {
				bh = p.blasCache[c]; found = true; break;
			}
		}
		if (!found) {
			auto r = createBLAS(mh);
			if (r.isErr()) { EError("RayTracing: createBLAS failed for mesh {}: {}", mh.index, ToString(r.error())); return r.error(); }
			bh = r.value();
			p.blasCache.push_back(bh);
			p.blasCacheMesh.push_back(mh);
		}
		objBLAS[i] = bh;
	}

	// 1.5. Rebuild the shared vertex/index buffers when the mesh set changes.
	// All scene meshes are merged into one vertex/index buffer pair; ObjectAttribs
	// stores the per-mesh offset into these buffers (same approach as the reference
	// hybrid renderer). Mesh geometry is static, so this only runs on set changes.
	bool meshSetChanged = p.blasCacheMesh.size() != p.sceneMeshKeys.size();
	if (!meshSetChanged) {
		for (size_t i = 0; i < p.blasCacheMesh.size(); ++i) {
			if (p.blasCacheMesh[i].index != p.sceneMeshKeys[i].index ||
				p.blasCacheMesh[i].generation != p.sceneMeshKeys[i].generation) { meshSetChanged = true; break; }
		}
	}
	if (meshSetChanged) {
		Vector<Vertex> allVerts;
		Vector<UInt32> allIdx;
		Vector<UInt32> firstVertex, firstIndex;
		for (auto& mh : p.blasCacheMesh) {
			const Vector<Vertex>* verts = p.renderer->getMeshVertices(mh);
			const Vector<UInt32>* idxs = p.renderer->getMeshIndices(mh);
			if (!verts || !idxs) { EError("RayTracing: cannot access mesh {} geometry", mh.index); return RenderError::InvalidHandle; }
			firstVertex.push_back((UInt32)allVerts.size());
			firstIndex.push_back((UInt32)allIdx.size());
			allVerts.insert(allVerts.end(), verts->begin(), verts->end());
			allIdx.insert(allIdx.end(), idxs->begin(), idxs->end());
		}
		if (allVerts.empty() || allIdx.empty()) return RenderError::InvalidArgument;
		const auto uploadStatic = [&](D::RefCntAutoPtr<D::IBuffer>& buf, const void* data, UInt32 count, UInt32 stride, const char* name) {
			D::BufferDesc bd; bd.Name = name; bd.Usage = D::USAGE_IMMUTABLE; bd.BindFlags = D::BIND_SHADER_RESOURCE;
			bd.Size = static_cast<UInt64>(count) * stride; bd.Mode = D::BUFFER_MODE_STRUCTURED; bd.ElementByteStride = stride;
			D::BufferData bdata; bdata.pData = data; bdata.DataSize = bd.Size;
			buf.Release();
			dev->CreateBuffer(bd, &bdata, &buf);
			return buf != nullptr;
		};
		if (!uploadStatic(p.sharedVB, allVerts.data(), (UInt32)allVerts.size(), (UInt32)sizeof(Vertex), "RT SharedVB")) return RenderError::BufferCreationFailed;
		if (!uploadStatic(p.sharedIB, allIdx.data(), (UInt32)allIdx.size(), (UInt32)sizeof(UInt32), "RT SharedIB")) return RenderError::BufferCreationFailed;
		p.sceneMeshKeys = p.blasCacheMesh;
		p.sceneMeshFirstVertex = std::move(firstVertex);
		p.sceneMeshFirstIndex = std::move(firstIndex);
		EInfo("RayTracing: shared geometry rebuilt ({} meshes, {} verts, {} indices)", p.sceneMeshKeys.size(), allVerts.size(), allIdx.size());
	}

	// 2. Rebuild material attribs (deduplicated by material handle) + texture registry.
	Vector<RTMaterialAttribs> newMaterials;
	Vector<MaterialHandle> materialKeys;
	Vector<UInt32> objMaterialId(objects.size());
	Vector<TextureHandle> texKeys;
	Vector<D::ITextureView*> newSRVs;
	for (size_t i = 0; i < objects.size(); ++i) {
		const MaterialHandle mat = objects[i].material;
		UInt32 mid = InvalidIndex;
		for (size_t m = 0; m < materialKeys.size(); ++m) {
			if (materialKeys[m].index == mat.index && materialKeys[m].generation == mat.generation) { mid = (UInt32)m; break; }
		}
		if (mid == InvalidIndex) {
			auto desc = p.renderer->getMaterial(mat);
			if (!desc.has_value()) { EError("RayTracing: invalid material handle {}:{}", mat.index, mat.generation); return RenderError::InvalidHandle; }
			RTMaterialAttribs ma;
			ma.baseColorMask = desc->baseColorFactor;
			ma.sampInd = 0;
			ma.baseColorTexInd = 0;
			if (desc->baseColorTexture.isValid()) {
				bool texFound = false;
				for (size_t t = 0; t < texKeys.size(); ++t) {
					if (texKeys[t].index == desc->baseColorTexture.index) { ma.baseColorTexInd = (UInt32)t; texFound = true; break; }
				}
				if (!texFound) {
					ma.baseColorTexInd = (UInt32)texKeys.size();
					texKeys.push_back(desc->baseColorTexture);
					auto* srv = static_cast<D::ITextureView*>(p.renderer->getTextureSRV(desc->baseColorTexture));
					newSRVs.push_back(srv ? srv : p.whiteSRV.RawPtr());
				}
			}
			mid = (UInt32)materialKeys.size();
			materialKeys.push_back(mat);
			newMaterials.push_back(ma);
		}
		objMaterialId[i] = mid;
	}

	// 3. Build ObjectAttribs (FirstIndex/FirstVertex point into the shared buffers).
	Vector<RTObjectAttribs> objAttribs(objects.size());
	for (size_t i = 0; i < objects.size(); ++i) {
		UInt32 mi = InvalidIndex;
		for (size_t m = 0; m < p.sceneMeshKeys.size(); ++m) {
			if (p.sceneMeshKeys[m].index == objects[i].mesh.index && p.sceneMeshKeys[m].generation == objects[i].mesh.generation) { mi = (UInt32)m; break; }
		}
		RTObjectAttribs& oa = objAttribs[i];
		oa.modelMat  = objects[i].transform.computeWorldMatrix();
		oa.normalMat = objects[i].transform.computeNormalMatrix();
		oa.materialId = objMaterialId[i];
		oa.firstIndex = (mi != InvalidIndex) ? p.sceneMeshFirstIndex[mi] : 0;
		oa.firstVertex = (mi != InvalidIndex) ? p.sceneMeshFirstVertex[mi] : 0;
		oa.meshId = mi;
	}

	// 4. Upload buffers (recreate when the size grows).
	const auto upload = [&](D::RefCntAutoPtr<D::IBuffer>& buf, const void* data, UInt32 count, UInt32 stride, const char* name) {
		const UInt32 size = count * stride;
		if (!buf || buf->GetDesc().Size < size) {
			D::BufferDesc bd; bd.Name = name; bd.Usage = D::USAGE_DEFAULT; bd.BindFlags = D::BIND_SHADER_RESOURCE;
			bd.Size = size; bd.Mode = D::BUFFER_MODE_STRUCTURED; bd.ElementByteStride = stride;
			buf.Release();
			dev->CreateBuffer(bd, nullptr, &buf);
			if (!buf) return false;
		}
		ctx->UpdateBuffer(buf, 0, size, data, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		return true;
	};
	if (!upload(p.objectAttribsBuf, objAttribs.data(), (UInt32)objAttribs.size(), (UInt32)sizeof(RTObjectAttribs), "RT ObjectAttribs")) return RenderError::BufferCreationFailed;
	if (!upload(p.materialAttribsBuf, newMaterials.data(), (UInt32)newMaterials.size(), (UInt32)sizeof(RTMaterialAttribs), "RT MaterialAttribs")) return RenderError::BufferCreationFailed;

	// 5. Rebind scene resources (textures, attribs, geometry, TLAS).
	if (p.rtSRB) {
		if (auto* v = p.rtSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_ObjectAttribs")) v->Set(p.objectAttribsBuf->GetDefaultView(D::BUFFER_VIEW_SHADER_RESOURCE), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
		if (auto* v = p.rtSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_MaterialAttribs")) v->Set(p.materialAttribsBuf->GetDefaultView(D::BUFFER_VIEW_SHADER_RESOURCE), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
		if (p.sharedVB) { if (auto* v = p.rtSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_VertexBuffer")) v->Set(p.sharedVB->GetDefaultView(D::BUFFER_VIEW_SHADER_RESOURCE), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE); }
		if (p.sharedIB) { if (auto* v = p.rtSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_IndexBuffer")) v->Set(p.sharedIB->GetDefaultView(D::BUFFER_VIEW_SHADER_RESOURCE), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE); }
		// Bind the full texture array, filling the remainder with white.
		Vector<D::IDeviceObject*> srvs(MaxSceneTextures, p.whiteSRV.RawPtr());
		for (size_t i = 0; i < newSRVs.size() && i < MaxSceneTextures; ++i) srvs[i] = newSRVs[i];
		if (auto* v = p.rtSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_Textures")) v->SetArray(srvs.data(), 0, MaxSceneTextures, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	}

	// 6. Build/update the internal TLAS.
	Vector<TLASInstance> instances(objects.size());
	for (size_t i = 0; i < objects.size(); ++i) {
		TLASInstance& in = instances[i];
		in.blas = objBLAS[i];
		in.transform = objects[i].transform.computeWorldMatrix();
		in.mask = 0xFF;
		in.customId = (UInt32)i; // CustomId == ObjectAttribs index
	}
	if (!p.sceneTLAS.tlas || p.sceneTLAS.maxInstances < objects.size()) {
		p.sceneTLAS = TLASData{}; // Recreate with the new capacity.
		D::TopLevelASDesc td;
		td.Name = "EE_RT_SceneTLAS";
		td.MaxInstanceCount = (UInt32)objects.size();
		td.Flags = D::RAYTRACING_BUILD_AS_PREFER_FAST_TRACE | D::RAYTRACING_BUILD_AS_ALLOW_UPDATE;
		dev->CreateTLAS(td, &p.sceneTLAS.tlas);
		if (!p.sceneTLAS.tlas) return RenderError::AccelerationStructureCreationFailed;
		p.sceneTLAS.maxInstances = (UInt32)objects.size();
		p.sceneTLAS.allowUpdate = true;
		EInfo("RayTracing: scene TLAS created ({} instances)", objects.size());
	}
	auto r = p.buildTLAS(p.sceneTLAS, instances);
	if (r.isErr()) { EError("RayTracing: scene TLAS build failed: {}", ToString(r.error())); return r.error(); }

	// 7. Bind the TLAS into the compute SRB.
	if (p.rtSRB) {
		if (auto* v = p.rtSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_TLAS")) v->Set(p.sceneTLAS.tlas, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	}
	return {};
}

// ===================================================================
// Ray tracing
// ===================================================================

Result<void, RenderError> RayTracingSubsystem::trace(const RayTraceConstants& c,
	void* gBufferNormal, void* gBufferDepth, void* outRT, UInt32 width, UInt32 height) {
	auto& p = *m_impl;
	auto* ctx = p.ctx(); if (!ctx) return RenderError::NotInitialized;
	if (!p.ok || !p.rtPSO || !p.rtSRB) return RenderError::OperationFailed;
	if (!gBufferNormal || !gBufferDepth || !outRT || width == 0 || height == 0) return RenderError::InvalidArgument;

	// Upload constants.
	{
		void* m = nullptr;
		ctx->MapBuffer(p.constantsBuf, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m);
		if (m) { memcpy(m, &c, sizeof(c)); ctx->UnmapBuffer(p.constantsBuf, D::MAP_WRITE); }
	}

	if (auto* v = p.rtSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_GBufferNormal")) v->Set(static_cast<D::ITextureView*>(gBufferNormal), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = p.rtSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_GBufferDepth")) v->Set(static_cast<D::ITextureView*>(gBufferDepth), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = p.rtSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_OutRT")) v->Set(static_cast<D::ITextureView*>(outRT), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);

	ctx->SetPipelineState(p.rtPSO);
	ctx->CommitShaderResources(p.rtSRB, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

	D::DispatchComputeAttribs da;
	da.ThreadGroupCountX = (width + 7) / 8;
	da.ThreadGroupCountY = (height + 7) / 8;
	ctx->DispatchCompute(da);
	return {};
}

Result<void, RenderError> RayTracingSubsystem::compose(void* gBufferColor, void* gBufferNormal, void* gBufferDepth,
	void* rtTex, void* outputRTV, void* depthDSV,
	const Mat4& viewProjInv, const Vec3& cameraPos,
	UInt32 width, UInt32 height) {
	auto& p = *m_impl;
	auto* ctx = p.ctx(); if (!ctx) return RenderError::NotInitialized;
	if (!p.ok || !p.composePSO || !p.composeSRB) return RenderError::OperationFailed;
	if (!gBufferColor || !gBufferNormal || !gBufferDepth || !rtTex || !outputRTV) return RenderError::InvalidArgument;

	{
		struct { Mat4 viewProjInv; Vec4 cameraPos; } cb;
		cb.viewProjInv = viewProjInv;
		cb.cameraPos = Vec4(cameraPos, 1.0f);
		void* m = nullptr;
		ctx->MapBuffer(p.composeCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m);
		if (m) { memcpy(m, &cb, sizeof(cb)); ctx->UnmapBuffer(p.composeCB, D::MAP_WRITE); }
	}

	if (auto* v = p.composeSRB->GetVariableByName(D::SHADER_TYPE_PIXEL, "g_GBufferColor")) v->Set(static_cast<D::ITextureView*>(gBufferColor), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = p.composeSRB->GetVariableByName(D::SHADER_TYPE_PIXEL, "g_GBufferNormal")) v->Set(static_cast<D::ITextureView*>(gBufferNormal), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = p.composeSRB->GetVariableByName(D::SHADER_TYPE_PIXEL, "g_GBufferDepth")) v->Set(static_cast<D::ITextureView*>(gBufferDepth), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = p.composeSRB->GetVariableByName(D::SHADER_TYPE_PIXEL, "g_RayTracedTex")) v->Set(static_cast<D::ITextureView*>(rtTex), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);

	D::ITextureView* rtv = static_cast<D::ITextureView*>(outputRTV);
	D::ITextureView* dsv = depthDSV ? static_cast<D::ITextureView*>(depthDSV) : nullptr;
	ctx->SetRenderTargets(1, &rtv, dsv, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	ctx->SetPipelineState(p.composePSO);
	ctx->CommitShaderResources(p.composeSRB, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	ctx->Draw(D::DrawAttribs{ 3, D::DRAW_FLAG_VERIFY_ALL });
	return {};
}

// ===================================================================
// Lifecycle
// ===================================================================

Result<void, CoreError> RayTracingSubsystem::onInitialize() {
	auto& p = *m_impl;
	if (!p.renderer) { EError("RayTracing: no renderer attached"); return CoreError::OperationFailed; }
	if (!p.renderer->supportsInlineRayTracing()) {
		EInfo("RayTracing: inline ray tracing not supported by this device - subsystem disabled.");
		return CoreError::OperationFailed;
	}
	auto* dev = p.device(); if (!dev) return CoreError::OperationFailed;
	auto* ctx = p.ctx(); if (!ctx) return CoreError::OperationFailed;

	// 1x1 white texture (fallback for materials without a base color texture).
	{
		D::TextureDesc td; td.Name = "RT White"; td.Type = D::RESOURCE_DIM_TEX_2D; td.Width = 1; td.Height = 1;
		td.Format = D::TEX_FORMAT_RGBA8_UNORM; td.BindFlags = D::BIND_SHADER_RESOURCE; td.Usage = D::USAGE_IMMUTABLE;
		UInt32 white = 0xFFFFFFFF;
		D::TextureSubResData srd; srd.pData = &white; srd.Stride = 4;
		D::TextureData tdata; tdata.pSubResources = &srd; tdata.NumSubresources = 1;
		D::RefCntAutoPtr<D::ITexture> tex;
		dev->CreateTexture(td, &tdata, &tex);
		if (tex) p.whiteSRV = tex->GetDefaultView(D::TEXTURE_VIEW_SHADER_RESOURCE);
	}

	// Linear sampler (immutable in the RT PSO).
	{
		D::SamplerDesc sd;
		sd.MinFilter = D::FILTER_TYPE_LINEAR; sd.MagFilter = D::FILTER_TYPE_LINEAR; sd.MipFilter = D::FILTER_TYPE_LINEAR;
		sd.AddressU = D::TEXTURE_ADDRESS_WRAP; sd.AddressV = D::TEXTURE_ADDRESS_WRAP; sd.AddressW = D::TEXTURE_ADDRESS_WRAP;
		dev->CreateSampler(sd, &p.linearSampler);
	}

	// Constants buffer.
	{
		D::BufferDesc bd; bd.Name = "RT Constants"; bd.Size = sizeof(RayTraceConstants);
		bd.BindFlags = D::BIND_UNIFORM_BUFFER; bd.Usage = D::USAGE_DYNAMIC; bd.CPUAccessFlags = D::CPU_ACCESS_WRITE;
		dev->CreateBuffer(bd, nullptr, &p.constantsBuf);
	}

	// Ray tracing compute shader (DXR 1.1 inline, SM 6.5).
	D::RefCntAutoPtr<D::IShader> rtCS;
	{
		D::ShaderCreateInfo ci;
		ci.Desc.ShaderType = D::SHADER_TYPE_COMPUTE;
		ci.Desc.Name = "RT RayQuery CS";
		ci.Desc.UseCombinedTextureSamplers = false;
		ci.EntryPoint = "CSMain";
		ci.SourceLanguage = D::SHADER_SOURCE_LANGUAGE_HLSL;
		ci.ShaderCompiler = D::SHADER_COMPILER_DXC;
		ci.HLSLVersion = { 6, 5 };
		ci.Source = g_RayQueryCS;
		ci.SourceLength = (D::Uint32)strlen(g_RayQueryCS);
		dev->CreateShader(ci, &rtCS);
		if (!rtCS || rtCS->GetStatus() != D::SHADER_STATUS_READY) { EError("RayTracing: failed to compile RayQuery compute shader"); return CoreError::OperationFailed; }
	}

	// RT compute PSO.
	{
		D::ComputePipelineStateCreateInfo ci;
		ci.PSODesc.Name = "RT RayQuery PSO";
		ci.PSODesc.PipelineType = D::PIPELINE_TYPE_COMPUTE;
		ci.PSODesc.ResourceLayout.DefaultVariableType = D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
		D::ImmutableSamplerDesc im[] = { { D::SHADER_TYPE_COMPUTE, "g_Sampler", D::SamplerDesc{} } };
		ci.PSODesc.ResourceLayout.ImmutableSamplers = im;
		ci.PSODesc.ResourceLayout.NumImmutableSamplers = 1;
		ci.pCS = rtCS;
		dev->CreateComputePipelineState(ci, &p.rtPSO);
		if (!p.rtPSO) { EError("RayTracing: failed to create RayQuery PSO"); return CoreError::OperationFailed; }
		p.rtPSO->CreateShaderResourceBinding(&p.rtSRB, true);
		if (p.rtSRB) {
			if (auto* v = p.rtSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_RTConstants")) v->Set(p.constantsBuf);
		}
	}

	// Compose shaders + PSO.
	{
		D::ShaderCreateInfo ci;
		ci.Desc.UseCombinedTextureSamplers = false;
		ci.EntryPoint = "main";
		ci.SourceLanguage = D::SHADER_SOURCE_LANGUAGE_HLSL;
		ci.ShaderCompiler = D::SHADER_COMPILER_DXC;

		D::RefCntAutoPtr<D::IShader> vs, ps;
		{
			ci.Desc.ShaderType = D::SHADER_TYPE_VERTEX; ci.Desc.Name = "RT Compose VS";
			ci.Source = g_ComposeVS; ci.SourceLength = (D::Uint32)strlen(g_ComposeVS);
			dev->CreateShader(ci, &vs);
		}
		{
			ci.Desc.ShaderType = D::SHADER_TYPE_PIXEL; ci.Desc.Name = "RT Compose PS";
			ci.Source = g_ComposePS; ci.SourceLength = (D::Uint32)strlen(g_ComposePS);
			dev->CreateShader(ci, &ps);
		}
		if (!vs || !ps || vs->GetStatus() != D::SHADER_STATUS_READY || ps->GetStatus() != D::SHADER_STATUS_READY) {
			EError("RayTracing: failed to compile compose shaders");
			return CoreError::OperationFailed;
		}

		D::GraphicsPipelineStateCreateInfo gci;
		gci.PSODesc.Name = "RT Compose PSO";
		gci.PSODesc.PipelineType = D::PIPELINE_TYPE_GRAPHICS;
		gci.PSODesc.ResourceLayout.DefaultVariableType = D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
		gci.pVS = vs; gci.pPS = ps;
		gci.GraphicsPipeline.NumRenderTargets = 1;
		gci.GraphicsPipeline.RTVFormats[0] = D::TEX_FORMAT_RGBA8_UNORM_SRGB;
		// The compose pass is bound with the renderer's MSAA depth (to keep the
		// DSV state consistent for later 2D/UI passes), so the PSO must declare it.
		gci.GraphicsPipeline.DSVFormat = D::TEX_FORMAT_D32_FLOAT;
		gci.GraphicsPipeline.PrimitiveTopology = D::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		gci.GraphicsPipeline.RasterizerDesc.CullMode = D::CULL_MODE_NONE;
		gci.GraphicsPipeline.DepthStencilDesc.DepthEnable = false;
		auto& b = gci.GraphicsPipeline.BlendDesc.RenderTargets[0];
		b.BlendEnable = true; b.SrcBlend = D::BLEND_FACTOR_SRC_ALPHA; b.DestBlend = D::BLEND_FACTOR_INV_SRC_ALPHA;
		dev->CreateGraphicsPipelineState(gci, &p.composePSO);
		if (!p.composePSO) { EError("RayTracing: failed to create compose PSO"); return CoreError::OperationFailed; }
		p.composePSO->CreateShaderResourceBinding(&p.composeSRB, true);
	}

	// Compose constant buffer.
	{
		D::BufferDesc bd; bd.Name = "RT Compose CB"; bd.Size = sizeof(Mat4) + sizeof(Vec4);
		bd.BindFlags = D::BIND_UNIFORM_BUFFER; bd.Usage = D::USAGE_DYNAMIC; bd.CPUAccessFlags = D::CPU_ACCESS_WRITE;
		dev->CreateBuffer(bd, nullptr, &p.composeCB);
		if (p.composeSRB) {
			if (auto* v = p.composeSRB->GetVariableByName(D::SHADER_TYPE_PIXEL, "ComposeCB")) v->Set(p.composeCB);
		}
	}

	p.ok = true;
	EInfo("RayTracing subsystem initialized (inline ray tracing ready).");
	return {};
}

void RayTracingSubsystem::onShutdown() {
	auto& p = *m_impl;
	p.rtPSO.Release(); p.rtSRB.Release(); p.constantsBuf.Release();
	p.composePSO.Release(); p.composeSRB.Release(); p.composeCB.Release();
	p.objectAttribsBuf.Release(); p.materialAttribsBuf.Release();
	p.sceneTLAS = TLASData{};
	p.blases.reset(); p.tlases.reset();
	p.blasScratch.Release();
	p.blasCache.clear(); p.blasCacheMesh.clear();
	p.sharedVB.Release(); p.sharedIB.Release();
	p.sceneMeshKeys.clear(); p.sceneMeshFirstVertex.clear(); p.sceneMeshFirstIndex.clear();
	p.sceneTextureSRVs.clear();
	p.whiteSRV.Release(); p.linearSampler.Release();
	p.ok = false;
	EInfo("RayTracing subsystem shut down.");
}

EE_NAMESPACE_RENDERING_END
