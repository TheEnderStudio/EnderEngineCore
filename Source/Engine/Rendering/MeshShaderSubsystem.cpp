#include <Rendering/MeshShaderSubsystem.hpp>
#include <Rendering/RenderSubsystem.hpp>
#include <Core/Log.hpp>

#include <DiligentCore/Graphics/GraphicsEngine/interface/RenderDevice.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/DeviceContext.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Shader.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/PipelineState.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Buffer.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Fence.h>
#include <DiligentCore/Common/interface/RefCntAutoPtr.hpp>

#include <glm/glm.hpp>

#include <meshoptimizer.h>
#include <clusterlod.h>

#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace D = Diligent;

EE_NAMESPACE_RENDERING_BEGIN

// ===================================================================
// M1: Tutorial20-style test grid (animated cubes) driven by an
// amplification shader (frustum culling + screen-space LOD coloring +
// GPU visible-count statistics) and a mesh shader (whole-cube mesh tasks).
// All matrices are column-major (glm layout), shaders use mul(M, v).
// ===================================================================

namespace {

constexpr UInt32 kGridGroupSize = 32;  // AS threads per group (tasks per group)
constexpr UInt32 kCubeThreads   = 24;  // MS threads per group (cube vertices)
constexpr UInt32 kCubeTris      = 12;
constexpr UInt32 kGridDim       = 32;  // grid: kGridDim x kGridDim cubes
constexpr UInt32 kTaskCount     = kGridDim * kGridDim;
constexpr UInt32 kStatRingSize  = 4;   // frames of statistics history

struct GridConstants { // 240 B payload -> 256 B buffer
	Mat4 view;         // 64
	Mat4 viewProj;     // 64
	Vec4 frustum[6];   // 96
	F32  cotHalfFov = 1.0f;
	F32  timeSec    = 0.0f;
	UInt32 frustumCulling = 1;
	UInt32 pad = 0;
};

struct DrawTask {
	Vec2 basePos;
	F32  scale;
	F32  timeOffset;
};

struct CubeData { // CPU/HLSL layout must match (all members 16-byte)
	Vec4      sphereRadius; // circumscribed-sphere radius, w unused
	Vec4      positions[24];
	Vec4      uvs[24];
	glm::uvec4 indices[12]; // 3 indices per element + pad
};

// Matches cbuffer cbConstants in the HLSL below.
static const char* g_MeshAS = R"(
struct DrawTask { float2 BasePos; float Scale; float TimeOffset; };
StructuredBuffer<DrawTask> DrawTasks;
RWByteAddressBuffer Statistics;

struct Payload {
    float PosX[32];
    float PosY[32];
    float PosZ[32];
    float Scale[32];
    float LODs[32];
    uint  Count; // number of valid entries (0 for the empty safety group)
};

cbuffer cbConstants : register(b0) {
    float4x4 g_View;
    float4x4 g_ViewProj;
    float4   g_Frustum[6];
    float    g_CoTanHalfFov;
    float    g_CurrTime;
    uint     g_FrustumCulling;
    uint     g_Pad;
};
cbuffer cbCubeData : register(b1) {
    float4 g_SphereRadius;
    float4 g_Positions[24];
    float4 g_UVs[24];
    uint4  g_Indices[12];
};

groupshared Payload s_Payload;
groupshared uint  s_TaskCount;

bool IsVisible(float3 cubeCenter, float radius) {
    float4 center = float4(cubeCenter, 1.0);
    for (int i = 0; i < 6; ++i) {
        if (dot(g_Frustum[i], center) < -radius)
            return false;
    }
    return true;
}

float CalcDetailLevel(float3 cubeCenter, float radius) {
    float3 pos = mul(g_View, float4(cubeCenter, 1.0)).xyz;
    float  dist2 = dot(pos, pos);
    float  size  = g_CoTanHalfFov * radius / sqrt(max(dist2 - radius * radius, 1e-6));
    return clamp(1.0 - size, 0.0, 1.0);
}

[numthreads(32, 1, 1)]
void main(in uint I  : SV_GroupIndex,
          in uint wg : SV_GroupID)
{
    if (I == 0) s_TaskCount = 0;
    GroupMemoryBarrierWithGroupSync();

    const uint   gid  = wg * 32 + I;
    DrawTask     task = DrawTasks[gid];
    float3       pos  = float3(task.BasePos, 0.0);
    float        scale = task.Scale;
    pos.y = sin(g_CurrTime + task.TimeOffset);

    if (g_FrustumCulling == 0 || IsVisible(pos, g_SphereRadius.x * scale)) {
        uint index;
        InterlockedAdd(s_TaskCount, 1, index);
        s_Payload.PosX[index] = pos.x;
        s_Payload.PosY[index] = pos.y;
        s_Payload.PosZ[index] = pos.z;
        s_Payload.Scale[index] = scale;
        s_Payload.LODs[index]  = CalcDetailLevel(pos, g_SphereRadius.x * scale);
    }

    GroupMemoryBarrierWithGroupSync();

    if (I == 0) {
        uint orig;
        Statistics.InterlockedAdd(0, s_TaskCount, orig);
        s_Payload.Count = s_TaskCount;
    }

    // Dispatch at least one group; extra/empty groups exit without output but
    // still call SetMeshOutputCounts (see the mesh shader).
    DispatchMesh(max(s_TaskCount, 1), 1, 1, s_Payload);
}
)";

// Matches the payload struct written by the amplification shader.
static const char* g_MeshMS = R"(
struct PSInput {
    float4 Pos   : SV_POSITION;
    float4 Color : COLOR;
};
struct Payload {
    float PosX[32];
    float PosY[32];
    float PosZ[32];
    float Scale[32];
    float LODs[32];
    uint  Count; // number of valid entries (0 for the empty safety group)
};

cbuffer cbConstants : register(b0) {
    float4x4 g_View;
    float4x4 g_ViewProj;
    float4   g_Frustum[6];
    float    g_CoTanHalfFov;
    float    g_CurrTime;
    uint     g_FrustumCulling;
    uint     g_Pad;
};
cbuffer cbCubeData : register(b1) {
    float4 g_SphereRadius;
    float4 g_Positions[24];
    float4 g_UVs[24];
    uint4  g_Indices[12];
};

float4 Rainbow(float factor) {
    float  h   = factor / 1.35;
    float3 col = float3(abs(h * 6.0 - 3.0) - 1.0, 2.0 - abs(h * 6.0 - 2.0), 2.0 - abs(h * 6.0 - 4.0));
    return float4(clamp(col, float3(0, 0, 0), float3(1, 1, 1)), 1.0);
}

[numthreads(24, 1, 1)]
[outputtopology("triangle")]
void main(in uint I      : SV_GroupIndex,
          in uint gid    : SV_GroupID,
          in payload Payload payload,
          out indices    uint3   tris[12],
          out vertices   PSInput verts[24])
{
    // Extra (empty) safety groups have Count == 0: they must still call
    // SetMeshOutputCounts, just with no outputs.
    const bool valid = (gid < payload.Count);
    SetMeshOutputCounts(valid ? 24u : 0u, valid ? 12u : 0u);
    if (!valid) return;

    float3 pos   = float3(payload.PosX[gid], payload.PosY[gid], payload.PosZ[gid]);
    float  scale = payload.Scale[gid];
    float  LOD   = payload.LODs[gid];

    verts[I].Pos = mul(g_ViewProj, float4(pos + g_Positions[I].xyz * scale, 1.0));
    verts[I].Color = Rainbow(LOD);

    if (I < 12) {
        tris[I] = g_Indices[I].xyz;
    }
}
)";

static const char* g_MeshPS = R"(
struct PSInput {
    float4 Pos   : SV_POSITION;
    float4 Color : COLOR;
};
float4 main(in PSInput i) : SV_TARGET {
    return i.Color;
}
)";

void extractFrustumPlanes(const Mat4& viewProj, Vec4 out[6]) {
	// Plane from the combined view-projection matrix (clip space |w|<=w style, LH depth = z/w):
	// rows of the matrix (column-major storage -> row i is m[col][i]).
	auto row = [&](int i) -> Vec4 {
		return Vec4(viewProj[0][i], viewProj[1][i], viewProj[2][i], viewProj[3][i]);
	};
	const Vec4 rows[4] = { row(0), row(1), row(2), row(3) };
	// left/right/bottom/top/near/far (normal facing inward)
	Vec4 planes[6] = {
		rows[3] + rows[0],
		rows[3] - rows[0],
		rows[3] + rows[1],
		rows[3] - rows[1],
		rows[3] + rows[2],
		rows[3] - rows[2],
	};
	for (int i = 0; i < 6; ++i) {
		Vec3 n(planes[i]);
		const F32 len = glm::length(n);
		if (len > 1e-6f) { planes[i] /= len; }
		out[i] = planes[i];
	}
}

void makeCubeGeometry(CubeData& out) {
	// Axis-aligned unit cube (positions in [-1, 1]), 6 faces x 4 vertices.
	static const int faceAxis[6] = { 0, 0, 1, 1, 2, 2 };
	static const float faceSign[6] = { -1, 1, -1, 1, -1, 1 };
	int vi = 0, ii = 0;
	for (int f = 0; f < 6; ++f) {
		const int a = faceAxis[f];
		const float s = faceSign[f];
		// The two tangent axes in a right-handed order so that the quad
		// triangle winding is consistent (culling is disabled anyway).
		const int b = (a + 1) % 3, c = (a + 2) % 3;
		for (int v = 0; v < 4; ++v) {
			const float bu = (v == 1 || v == 2) ? 1.0f : -1.0f;
			const float cv = (v == 2 || v == 3) ? 1.0f : -1.0f;
			glm::vec3 p(0);
			p[a] = s;
			p[b] = bu;
			p[c] = cv;
			out.positions[vi] = Vec4(p, 1.0f);
			out.uvs[vi] = Vec4((bu * 0.5f + 0.5f), (cv * 0.5f + 0.5f), 0, 0);
			++vi;
		}
		const UInt32 base = f * 4;
		out.indices[ii++] = glm::uvec4(base, base + 1, base + 2, 0);
		out.indices[ii++] = glm::uvec4(base, base + 2, base + 3, 0);
	}
	out.sphereRadius = Vec4(1.7320508f, 0, 0, 0); // half diagonal of a 2x2x2 cube
}

} // namespace

// Task entry consumed by the amplification shaders (SceneTask in HLSL).
struct SceneTask { // 16 B
	UInt32 meshId;
	UInt32 instanceId;   // index into the flat per-frame instance array
	UInt32 groupId;      // index into the per-frame draw-group bounds table
	UInt32 pad;
};

// Per-draw-group bounding sphere (used for frustum culling and the too-small
// drop test). One per MeshDrawGroup, in that group's local space.
struct MeshGroupGPU { // 32 B
	Vec4   center;
	F32    radius;
	UInt32 pad[3];
};

// Cluster (M4) mesh shader output budget. The D3D12 mesh shader spec guarantees
// at least 256 output vertices and 256 output primitives per group, and at most
// 32k of output data. meshoptimizer's clusterlod is configured with
// max_vertices = max_triangles = 128, but the flexible builder may merge a few
// extra triangles into a cluster, so the primitive array is sized to the
// guaranteed 256 while the vertex array matches the configured maximum.
// CRITICAL: every triangle index written by the mesh shader must be strictly
// below the vertex count passed to SetMeshOutputCounts - otherwise the
// rasterizer reads unwritten output vertices, which shows up as garbage
// triangles and eventually removes the device (TDR).
constexpr UInt32 kClusterMaxVerts = 128;
constexpr UInt32 kClusterMaxTris  = 256;
// Visible clusters an amplification group can hand to its mesh shaders through
// the payload (must match CLUSTER_PER_GROUP in g_ClusterAS). The payload is the
// only spec-guaranteed AS -> MS channel, and it counts against the 32k
// amplification / 28k mesh groupshared limits: 384 * 8 + 8 = 3080 bytes.
constexpr UInt32 kClusterPerGroup = 384;
constexpr UInt32 kClusterMaxBudget = 1u << 18; // upper bound accepted by setClusterBudget()

// Albedo texture palette size for the cluster pixel shader (must match
// MAX_MATERIALS in g_ClusterPS). Every slot is always bound (white fallback), so
// a model with more distinct materials than this falls back to white.
constexpr UInt32 kMaxMaterials = 32;

// ===================================================================
// Implementation
// ===================================================================

struct MeshShaderSubsystem::Impl {
	RenderSubsystem* renderer = nullptr;
	bool ok = false;

	// Pipeline (created lazily on the first drawGrid with the renderer's sample count)
	D::RefCntAutoPtr<D::IPipelineState> pso;
	D::RefCntAutoPtr<D::IShaderResourceBinding> srb;
	UInt8 psoSampleCount = 0;

	// Buffers
	D::RefCntAutoPtr<D::IBuffer> constantsCB;  // GridConstants
	D::RefCntAutoPtr<D::IBuffer> cubeDataCB;   // CubeData (immutable)
	D::RefCntAutoPtr<D::IBuffer> drawTasks;    // StructuredBuffer<DrawTask>
	D::RefCntAutoPtr<D::IBuffer> statsBuf;     // Raw UAV visible-count
	D::RefCntAutoPtr<D::IBuffer> statsStaging; // Readback ring
	D::RefCntAutoPtr<D::IFence>  statsFence;
	UInt64 frame = 0;
	UInt32 visibleCount = 0;   // last read result
	bool   frustumCulling = true;
	F32    lodScale = 4.0f;    // M3/M4: projected-error threshold in pixels for LOD selection
	F32    adaptiveDrop = 0.02f; // adaptive too-small threshold (model-sphere based)
	UInt32 meshletCap = 384;   // per-AS-group mesh group budget (GPU overload protection)
	UInt32 meshFilter = 0xFFFFFFFFu; // debug: draw only this mesh id (0xFFFFFFFF = all)
	Vec3   bodyCenter = Vec3(0); // whole-model bounding sphere (all registered meshes)
	F32    bodyRadius = 1.0f;
	UInt32 debugMode = 0;      // 1 = MS emits a fixed triangle (diagnostic)

	// ---- M2: GPU-driven scene (meshlet pipeline) ----
	D::RefCntAutoPtr<D::IPipelineState> scenePso;
	D::RefCntAutoPtr<D::IShaderResourceBinding> sceneSrb;
	UInt8 scenePsoSampleCount = 0;
	Vector<MeshHandle> sceneMeshList;
	D::RefCntAutoPtr<D::IBuffer> sceneCB;          // SceneConstants
	D::RefCntAutoPtr<D::IBuffer> sceneInstances;   // StructuredBuffer<float4x4>
	D::RefCntAutoPtr<D::IBuffer> scenePos;         // StructuredBuffer<float4>
	D::RefCntAutoPtr<D::IBuffer> sceneNorm;
	D::RefCntAutoPtr<D::IBuffer> sceneMeshlets;    // StructuredBuffer<SceneMeshlet>
	D::RefCntAutoPtr<D::IBuffer> sceneMeshletVerts;// uint pool
	D::RefCntAutoPtr<D::IBuffer> sceneMeshletTris; // uint pool
	D::RefCntAutoPtr<D::IBuffer> sceneMeshInfo;    // StructuredBuffer<SceneMeshInfo>
	D::RefCntAutoPtr<D::IBuffer> sceneTasks;       // StructuredBuffer<SceneTask>
	D::RefCntAutoPtr<D::IBuffer> sceneStatsBuf;
	D::RefCntAutoPtr<D::IBuffer> sceneStatsStaging;
	D::RefCntAutoPtr<D::IFence>  sceneStatsFence;
	UInt64 sceneFrame = 0;
	UInt32 sceneVisibleTasks = 0;
	UInt32 sceneMeshCount = 0;
	UInt32 sceneTaskCapacity = 0;             // meshCount * kSceneMaxInstances
	std::vector<SceneTask> sceneTaskScratch;  // per-frame (mesh x instance) list

	// ---- M4: meshoptimizer cluster-LOD pools ----
	D::RefCntAutoPtr<D::IBuffer> clPos, clNorm, clUV;    // pooled original vertices
	D::RefCntAutoPtr<D::IBuffer> clClusters;             // StructuredBuffer<ClusterGPU>
	D::RefCntAutoPtr<D::IBuffer> clClusterVerts;         // uint pool
	D::RefCntAutoPtr<D::IBuffer> clClusterTris;          // uint pool (3 per triangle)
	D::RefCntAutoPtr<D::IBuffer> clGroups;               // StructuredBuffer<ClusterGroupGPU>
	D::RefCntAutoPtr<D::IBuffer> clMeshInfo;             // StructuredBuffer<ClusterMeshInfo>
	D::RefCntAutoPtr<D::IBuffer> clMaterials;            // StructuredBuffer<MaterialGPU>
	D::RefCntAutoPtr<D::IBuffer> clCounter;              // raw UAV: global cluster budget allocator
	D::RefCntAutoPtr<D::IBuffer> clGroupBounds;          // StructuredBuffer<MeshGroupGPU> (per frame)
	// Per-mesh local bounding spheres (CPU copy of ClusterMeshInfo::center/radius),
	// used to derive each draw group's sphere.
	struct MeshBound { Vec3 center; F32 radius; };
	std::vector<MeshBound> meshBounds;
	// Flat per-frame instance matrices across all draw groups.
	std::vector<Mat4> groupInstancesScratch;
	std::vector<MeshGroupGPU> groupBoundsScratch;
	UInt32 clusterBudget = 16384;                        // hard per-frame cap on dispatched mesh groups
	UInt32 sceneVisibleClusters = 0;                     // last readback: dispatched cluster groups
	UInt32 sceneClusterDemand = 0;                       // last readback: wanted clusters before the clamp
	D::RefCntAutoPtr<D::IPipelineState> clusterPso;
	D::RefCntAutoPtr<D::IShaderResourceBinding> clusterSrb;
	// Hybrid ray tracing variant: same AS/MS, writes the G-buffer instead.
	D::RefCntAutoPtr<D::IPipelineState> clusterGBufferPso;
	D::RefCntAutoPtr<D::IShaderResourceBinding> clusterGBufferSrb;
	UInt8 clusterPsoSampleCount = 0;

	// ---- M5: materials (texture palette) ----
	D::RefCntAutoPtr<D::ITexture>     whiteTex;          // 1x1 white fallback (keeps the SRV alive)
	D::RefCntAutoPtr<D::ITextureView> whiteTexSRV;
	D::RefCntAutoPtr<D::ITexture>     flatNormalTex, flatMRTex;
	D::RefCntAutoPtr<D::ITextureView> flatNormalSRV, flatMRSRV;
	D::RefCntAutoPtr<D::ISampler>     materialSampler;
	std::vector<D::IDeviceObject*>    materialSRVs;      // albedo   (baseColorTexture)
	std::vector<D::IDeviceObject*>    normalSRVs;        // normal   (normalTexture)
	std::vector<D::IDeviceObject*>    mrSRVs;            // metallicRoughnessTexture
	std::vector<D::IDeviceObject*>    emissiveSRVs;      // emissiveTexture
	UInt32                            materialCount = 0;

	// ---- Shadows (raster path only; the hybrid path uses ray traced shadows) ----
	D::RefCntAutoPtr<D::ISampler>     shadowSampler;      // comparison sampler
	D::RefCntAutoPtr<D::ITexture>     shadowDummyTex;     // 1x1x4 "fully lit" fallback
	D::RefCntAutoPtr<D::ITextureView> shadowDummySRV;
	D::RefCntAutoPtr<D::ITextureView> shadowMapSRV;       // borrowed from the renderer
	Mat4                              shadowUV[4] = {};
	Vec4                              cascadeSplits = Vec4(0);
	bool                              shadowValid = false;
};

MeshShaderSubsystem::MeshShaderSubsystem() : Subsystem("MeshShader"), m_impl(std::make_unique<Impl>()) {}
MeshShaderSubsystem::~MeshShaderSubsystem() = default;

void MeshShaderSubsystem::attachToRenderer(RenderSubsystem* r) { m_impl->renderer = r; }
bool MeshShaderSubsystem::isReady() const { return m_impl->ok; }

void MeshShaderSubsystem::setFrustumCulling(bool enable) { m_impl->frustumCulling = enable; }
void MeshShaderSubsystem::setLodScale(F32 scale) { m_impl->lodScale = std::max(scale, 0.05f); }
void MeshShaderSubsystem::setMeshletCap(UInt32 cap) {
	// The driver/hardware shows a cliff around ~512 mesh groups derived from a
	// single amplification group, so the per-group budget stays well below it.
	m_impl->meshletCap = std::clamp<UInt32>(cap, 1u, 384u);
}
void MeshShaderSubsystem::setDebugMode(UInt32 mode) { m_impl->debugMode = mode; }
void MeshShaderSubsystem::setMeshFilter(UInt32 meshId) { m_impl->meshFilter = meshId; }
UInt32 MeshShaderSubsystem::lastVisibleCount() const { return m_impl->visibleCount; }
UInt32 MeshShaderSubsystem::lastSceneVisibleTasks() const { return m_impl->sceneVisibleTasks; }
UInt32 MeshShaderSubsystem::lastSceneVisibleClusters() const { return m_impl->sceneVisibleClusters; }
UInt32 MeshShaderSubsystem::lastSceneClusterDemand() const { return m_impl->sceneClusterDemand; }
void MeshShaderSubsystem::setClusterBudget(UInt32 budget) {
	m_impl->clusterBudget = std::clamp<UInt32>(budget, 256u, kClusterMaxBudget);
}

void MeshShaderSubsystem::setShadowMap(TextureSRV shadowMap, const Mat4 worldToShadowUV[4], const Vec4& cascadeSplits) {
	auto& p = *m_impl;
	p.shadowMapSRV = shadowMap ? static_cast<D::ITextureView*>(shadowMap) : nullptr;
	p.shadowValid = p.shadowMapSRV != nullptr;
	for (int i = 0; i < 4; ++i) p.shadowUV[i] = worldToShadowUV ? worldToShadowUV[i] : Mat4(1.0f);
	p.cascadeSplits = cascadeSplits;
	// The binding already exists once the pipeline has been built; refresh it so
	// a shadow map created after the first draw still takes effect.
	if (p.clusterSrb) {
		if (auto* v = p.clusterSrb->GetVariableByName(D::SHADER_TYPE_PIXEL, "g_ShadowMap")) {
			auto* srv = p.shadowMapSRV ? p.shadowMapSRV.RawPtr() : (p.shadowDummySRV ? p.shadowDummySRV.RawPtr() : nullptr);
			if (srv) v->Set(srv, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
		}
	}
}

Result<void, CoreError> MeshShaderSubsystem::onInitialize() {
	auto& p = *m_impl;
	if (!p.renderer) { EError("MeshShader: no renderer attached"); return CoreError::OperationFailed; }
	if (!p.renderer->supportsMeshShaders()) {
		EInfo("MeshShader: the device does not support mesh shaders - subsystem disabled.");
		return CoreError::OperationFailed;
	}
	p.ok = true;
	EInfo("MeshShader subsystem ready (amplification + mesh shader pipeline).");
	return {};
}

void MeshShaderSubsystem::onShutdown() {
	auto& p = *m_impl;
	p.ok = false;
	p.pso.Release(); p.srb.Release(); p.constantsCB.Release(); p.cubeDataCB.Release();
	p.drawTasks.Release(); p.statsBuf.Release(); p.statsStaging.Release(); p.statsFence.Release();
	p.scenePso.Release(); p.sceneSrb.Release(); p.sceneCB.Release(); p.sceneInstances.Release();
	p.scenePos.Release(); p.sceneNorm.Release(); p.sceneMeshlets.Release();
	p.sceneMeshletVerts.Release(); p.sceneMeshletTris.Release(); p.sceneMeshInfo.Release();
	p.sceneTasks.Release(); p.sceneStatsBuf.Release(); p.sceneStatsStaging.Release(); p.sceneStatsFence.Release();
	p.clPos.Release(); p.clNorm.Release(); p.clClusters.Release(); p.clClusterVerts.Release();
	p.clClusterTris.Release(); p.clGroups.Release(); p.clMeshInfo.Release(); p.clCounter.Release();
	p.clMaterials.Release(); p.clGroupBounds.Release(); p.meshBounds.clear();
	p.clusterGBufferPso.Release(); p.clusterGBufferSrb.Release();
	p.clUV.Release(); p.whiteTex.Release(); p.whiteTexSRV.Release(); p.materialSampler.Release();
	p.flatNormalTex.Release(); p.flatNormalSRV.Release();
	p.flatMRTex.Release(); p.flatMRSRV.Release();
	p.shadowSampler.Release(); p.shadowDummyTex.Release(); p.shadowDummySRV.Release(); p.shadowMapSRV.Release();
	p.materialSRVs.clear(); p.normalSRVs.clear(); p.mrSRVs.clear(); p.emissiveSRVs.clear();
	p.materialCount = 0;
	p.sceneMeshList.clear();
}

// ===================================================================
// Lazy pipeline + buffer creation.
// ===================================================================
Result<void, RenderError> MeshShaderSubsystem::ensurePipeline(Impl& p, UInt32 sampleCount) {
	auto* dev = p.renderer->getDevice() ? static_cast<D::IRenderDevice*>(p.renderer->getDevice()) : nullptr;
	auto* ctx = p.renderer->getContext() ? static_cast<D::IDeviceContext*>(p.renderer->getContext()) : nullptr;
	if (!dev || !ctx) return RenderError::NotInitialized;

	if (p.pso && p.psoSampleCount == sampleCount) {
		return {};
	}
	p.pso.Release(); p.srb.Release();

	// ---- Shaders (DXC; mesh/amplification need SM 6.5) ----
	auto makeShader = [&](D::SHADER_TYPE type, const char* name, const char* src) -> D::RefCntAutoPtr<D::IShader> {
		D::ShaderCreateInfo ci;
		ci.Desc.ShaderType = type;
		ci.Desc.Name = name;
		ci.EntryPoint = "main";
		ci.SourceLanguage = D::SHADER_SOURCE_LANGUAGE_HLSL;
		ci.ShaderCompiler = D::SHADER_COMPILER_DXC;
		ci.HLSLVersion = { 6, 5 };
		ci.Source = src;
		ci.SourceLength = (D::Uint32)strlen(src);
		D::RefCntAutoPtr<D::IShader> s;
		dev->CreateShader(ci, &s);
		if (!s || s->GetStatus() != D::SHADER_STATUS_READY) { EError("MeshShader: failed to compile {}", name); return {}; }
		return s;
	};

	auto as = makeShader(D::SHADER_TYPE_AMPLIFICATION, "MS Test AS", g_MeshAS);
	auto ms = makeShader(D::SHADER_TYPE_MESH, "MS Test MS", g_MeshMS);
	auto ps = makeShader(D::SHADER_TYPE_PIXEL, "MS Test PS", g_MeshPS);
	if (!as || !ms || !ps) return RenderError::ShaderCompilationFailed;

	D::GraphicsPipelineStateCreateInfo ci;
	ci.PSODesc.Name = "Mesh Shader Test Grid";
	ci.PSODesc.PipelineType = D::PIPELINE_TYPE_MESH;
	ci.PSODesc.ResourceLayout.DefaultVariableType = D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
	// Topology is defined by the mesh shader ([outputtopology("triangle")]).
	ci.GraphicsPipeline.PrimitiveTopology = D::PRIMITIVE_TOPOLOGY_UNDEFINED;
	ci.GraphicsPipeline.NumRenderTargets = 1;
	ci.GraphicsPipeline.RTVFormats[0] = D::TEX_FORMAT_RGBA16_FLOAT;  // matches the scene HDR target
	ci.GraphicsPipeline.DSVFormat = D::TEX_FORMAT_D32_FLOAT;
	ci.GraphicsPipeline.SmplDesc.Count = static_cast<D::Uint8>(sampleCount);
	ci.GraphicsPipeline.RasterizerDesc.CullMode = D::CULL_MODE_NONE;
	ci.GraphicsPipeline.RasterizerDesc.FillMode = D::FILL_MODE_SOLID;
	ci.GraphicsPipeline.DepthStencilDesc.DepthEnable = true;
	ci.GraphicsPipeline.DepthStencilDesc.DepthWriteEnable = true;
	ci.GraphicsPipeline.DepthStencilDesc.DepthFunc = D::COMPARISON_FUNC_LESS;
	ci.pAS = as; ci.pMS = ms; ci.pPS = ps;

	dev->CreateGraphicsPipelineState(ci, &p.pso);
	if (!p.pso) { EError("MeshShader: failed to create the mesh pipeline"); return RenderError::PipelineStateCreationFailed; }
	p.psoSampleCount = static_cast<UInt8>(sampleCount);
	p.pso->CreateShaderResourceBinding(&p.srb, true);
	if (!p.srb) { p.pso.Release(); return RenderError::PipelineStateCreationFailed; }

	// ---- Buffers ----
	if (!p.constantsCB) {
		D::BufferDesc bd; bd.Name = "MS Constants"; bd.Size = 256;
		bd.BindFlags = D::BIND_UNIFORM_BUFFER; bd.Usage = D::USAGE_DYNAMIC; bd.CPUAccessFlags = D::CPU_ACCESS_WRITE;
		dev->CreateBuffer(bd, nullptr, &p.constantsCB);
		if (!p.constantsCB) { p.pso.Release(); return RenderError::BufferCreationFailed; }
	}
	if (!p.cubeDataCB) {
		CubeData cube{};
		makeCubeGeometry(cube);
		D::BufferDesc bd; bd.Name = "MS CubeData"; bd.Size = sizeof(cube);
		bd.BindFlags = D::BIND_UNIFORM_BUFFER; bd.Usage = D::USAGE_IMMUTABLE;
		D::BufferData data; data.pData = &cube; data.DataSize = sizeof(cube);
		dev->CreateBuffer(bd, &data, &p.cubeDataCB);
		if (!p.cubeDataCB) { p.pso.Release(); return RenderError::BufferCreationFailed; }
	}
	if (!p.drawTasks) {
		struct TaskUpload { DrawTask tasks[kTaskCount]; };
		TaskUpload tu{};
		for (UInt32 y = 0; y < kGridDim; ++y) {
			for (UInt32 x = 0; x < kGridDim; ++x) {
				const UInt32 idx = y * kGridDim + x;
				const F32 half = (F32)kGridDim * 0.5f;
				tu.tasks[idx].basePos = Vec2(((F32)x - half) * 5.0f, ((F32)y - half) * 5.0f);
				tu.tasks[idx].scale = 0.5f + 0.7f * std::abs(std::sin((F32)idx * 12.9898f)) * 0.5f;
				tu.tasks[idx].timeOffset = (F32)(idx % 17) * 0.3f;
			}
		}
		D::BufferDesc bd; bd.Name = "MS DrawTasks"; bd.Size = sizeof(tu);
		bd.BindFlags = D::BIND_SHADER_RESOURCE; bd.Mode = D::BUFFER_MODE_STRUCTURED;
		bd.ElementByteStride = sizeof(DrawTask); bd.Usage = D::USAGE_IMMUTABLE;
		D::BufferData data; data.pData = &tu; data.DataSize = sizeof(tu);
		dev->CreateBuffer(bd, &data, &p.drawTasks);
		if (!p.drawTasks) { p.pso.Release(); return RenderError::BufferCreationFailed; }
	}
	if (!p.statsBuf) {
		D::BufferDesc bd; bd.Name = "MS Statistics"; bd.Size = 16;
		bd.BindFlags = D::BIND_UNORDERED_ACCESS; bd.Mode = D::BUFFER_MODE_RAW; bd.Usage = D::USAGE_DEFAULT;
		dev->CreateBuffer(bd, nullptr, &p.statsBuf);
		if (!p.statsBuf) { p.pso.Release(); return RenderError::BufferCreationFailed; }
	}
	if (!p.statsStaging) {
		D::BufferDesc bd; bd.Name = "MS Statistics Staging"; bd.Size = 16 * kStatRingSize;
		bd.Usage = D::USAGE_STAGING; bd.CPUAccessFlags = D::CPU_ACCESS_READ;
		dev->CreateBuffer(bd, nullptr, &p.statsStaging);
		if (!p.statsStaging) { p.pso.Release(); return RenderError::BufferCreationFailed; }
	}
	if (!p.statsFence) {
		D::FenceDesc fd; fd.Name = "MS Statistics fence";
		dev->CreateFence(fd, &p.statsFence);
	}

	// ---- SRB bindings ----
	p.srb->GetVariableByName(D::SHADER_TYPE_AMPLIFICATION, "DrawTasks")->Set(p.drawTasks->GetDefaultView(D::BUFFER_VIEW_SHADER_RESOURCE));
	p.srb->GetVariableByName(D::SHADER_TYPE_AMPLIFICATION, "Statistics")->Set(p.statsBuf->GetDefaultView(D::BUFFER_VIEW_UNORDERED_ACCESS));
	p.srb->GetVariableByName(D::SHADER_TYPE_AMPLIFICATION, "cbConstants")->Set(p.constantsCB);
	p.srb->GetVariableByName(D::SHADER_TYPE_AMPLIFICATION, "cbCubeData")->Set(p.cubeDataCB);
	p.srb->GetVariableByName(D::SHADER_TYPE_MESH, "cbConstants")->Set(p.constantsCB);
	p.srb->GetVariableByName(D::SHADER_TYPE_MESH, "cbCubeData")->Set(p.cubeDataCB);

	return {};
}

// ===================================================================
// Per-frame draw of the test grid.
// ===================================================================
Result<void, RenderError> MeshShaderSubsystem::drawGrid(const Mat4& view, const Mat4& proj, F32 timeSec) {
	auto& p = *m_impl;
	if (!p.ok) return RenderError::OperationFailed;
	auto* dev = p.renderer->getDevice() ? static_cast<D::IRenderDevice*>(p.renderer->getDevice()) : nullptr;
	auto* ctx = p.renderer->getContext() ? static_cast<D::IDeviceContext*>(p.renderer->getContext()) : nullptr;
	if (!dev || !ctx) return RenderError::NotInitialized;

	const UInt8 sampleCount = p.renderer->msaaSamples();
	auto r = ensurePipeline(p, sampleCount);
	if (r.isErr()) return r.error();
	if (!p.pso || !p.srb) return RenderError::NotInitialized;

	// ---- Constants ----
	{
		GridConstants cb{};
		cb.view = view;
		cb.viewProj = proj * view;
		extractFrustumPlanes(cb.viewProj, cb.frustum);
		cb.cotHalfFov = proj[1][1];
		cb.timeSec = timeSec;
		cb.frustumCulling = p.frustumCulling ? 1u : 0u;
		void* m = nullptr;
		ctx->MapBuffer(p.constantsCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m);
		if (m) { memcpy(m, &cb, sizeof(cb)); ctx->UnmapBuffer(p.constantsCB, D::MAP_WRITE); }
	}

	// Reset the statistics counter (raw UAV, first dword).
	{
		const UInt32 zero = 0;
		ctx->UpdateBuffer(p.statsBuf, 0, sizeof(zero), &zero, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	}

	ctx->SetPipelineState(p.pso);
	ctx->CommitShaderResources(p.srb, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

	D::DrawMeshAttribs drawAttrs(kTaskCount / kGridGroupSize, D::DRAW_FLAG_VERIFY_ALL);
	ctx->DrawMesh(drawAttrs);

	// ---- Statistics readback (delayed by one frame) ----
	{
		ctx->CopyBuffer(p.statsBuf, 0, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
			p.statsStaging, static_cast<UInt32>(p.frame % kStatRingSize) * 16, 16,
			D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		ctx->EnqueueSignal(p.statsFence, p.frame);
		const UInt64 avail = p.statsFence->GetCompletedValue();
		if (p.frame > kStatRingSize && avail >= p.frame - kStatRingSize) {
			const UInt64 slot = (avail) % kStatRingSize;
			void* m = nullptr;
			ctx->MapBuffer(p.statsStaging, D::MAP_READ, D::MAP_FLAG_DO_NOT_WAIT, m);
			if (m) {
				const UInt8* base = static_cast<const UInt8*>(m);
				memcpy(&p.visibleCount, base + slot * 16, sizeof(p.visibleCount));
				ctx->UnmapBuffer(p.statsStaging, D::MAP_READ);
			}
		}
		++p.frame;
	}

	return {};
}

// ===================================================================
// M2: GPU-driven real meshes (meshlet pipeline)
// ---------------------------------------------------------------------
// Per registered mesh the CPU builds meshlets (greedy clustering, <=64
// vertices / <=96 triangles each). The geometry of all meshes is then
// concatenated into GPU pools:
//   ScenePositions / SceneNormals - vertex streams (float4, pool offset per mesh)
//   SceneMeshlets                 - one record per meshlet
//   MeshletVerts                  - per meshlet: global pool vertex indices
//   MeshletTris                   - per meshlet: triangle local-vertex indices
//   SceneMeshInfos                - per mesh: bounding sphere, pool bases, color
//   SceneTasks                    - static (mesh x instance) task list
// Amplification shader: 1 thread per task, instance frustum culling, group
// prefix of visible meshlet counts, DispatchMesh(total visible meshlets).
// Mesh shader: 1 group per meshlet, emits vertices/triangles from the pools.
// ===================================================================

namespace {

constexpr UInt32 kSceneGroupSize   = 32;  // AS threads per group
constexpr UInt32 kSceneTasksPerGroup = 4; // scene tasks per AS group: keeps the mesh
                                          // groups dispatched by one amplification group
                                          // well below the ~512 driver cliff, so the
                                          // per-group budget never truncates geometry
// Meshlet size doubles as the mesh shader group's output budget. Larger
// meshlets = far fewer dispatched mesh groups (each group has a fixed cost on
// the GPU), which is what keeps a full-scene DrawMesh within the TDR budget.
constexpr UInt32 kMeshletMaxVerts  = 128;
constexpr UInt32 kMeshletMaxTris   = 160;
constexpr UInt32 kSceneMsThreads   = 128; // mesh shader threads per group
// Flat per-frame instance array across every draw group (Furina's bodies plus
// the single-instance terrain/wall/cube groups).
constexpr UInt32 kSceneMaxInstances = 4096;
// Upper bound on MeshDrawGroups per frame.
constexpr UInt32 kSceneMaxGroups = 64;

struct SceneConstants { // 264 B payload (buffer 320 B; layout matches the HLSL cbuffer)
	Mat4 view;          // 0..64
	Mat4 viewProj;      // 64..128
	Vec4 frustum[6];    // 128..224
	F32  cotHalfFov = 1.0f;   // 224
	F32  timeSec    = 0.0f;   // 228
	UInt32 frustumCulling = 1; // 232
	UInt32 activeInstances = 0; // 236
	Vec4 lightDir = Vec4(-0.5f, -0.9f, 0.3f, 0); // 240..256 (16-aligned)
	F32  lodScale  = 1.0f;  // 256
	F32  dropSize  = 0.06f; // 260: instances projecting below this are skipped
	UInt32 meshletCap = 384; // 264: hard cap of mesh groups a single AS group may dispatch
	UInt32 debugMode = 0;    // 268: 1 = mesh shader emits a fixed triangle without reading geometry
	Vec4   bodyCenter = Vec4(0, 0, 0, 1); // 272: whole-model bounding sphere (all meshes share
	F32    bodyRadius = 1.0f;             // 288: one instance transform -> used for culling)
	UInt32 meshFilter = 0xFFFFFFFFu;      // 292: draw only this mesh id (0xFFFFFFFF = all)
	F32    screenHeight = 1080.0f;        // 296: viewport height (projected error -> pixels)
	UInt32 clusterCapacity = 0;           // 300: visible cluster list capacity
	Vec4   cameraPos = Vec4(0, 0, 0, 1);  // 304: world-space camera position (PBR view vector)
	Vec4   ambient = Vec4(0.30f, 0.30f, 0.35f, 1.0f); // 320: rgb = ambient colour, a = intensity
	Vec4   lightColor = Vec4(1, 1, 1, 1); // 336: rgb = sun colour, a = sun intensity
	Mat4   shadowMapUVDepth[4] = {};      // 352: world -> shadow UV/depth, one per cascade
	Vec4   cascadeSplits = Vec4(0);       // 608: camera-space far distance of cascades 0..2
	Vec4   skyCorners[8] = {};            // 624: skybox corner colours (bit0=x+, bit1=y+, bit2=z+)
	                                      //      -> the ambient term is derived from them, see SkyIrradiance
}; // 752 B (buffer 1024 B)
static_assert(sizeof(SceneConstants) == 752, "SceneConstants must match the HLSL cbuffer layout");

struct SceneMeshInfo { // 96 B (16-byte aligned; layout must match the HLSL struct)
	Vec4   center;           // 0  mesh-local LOD0 bounding-sphere center
	Vec4   color;            // 16 per-mesh tint for the debug shading
	F32    radius;           // 32 LOD0 bounding-sphere radius (local space)
	UInt32 lodCount;         // 36 number of LOD levels (3)
	UInt32 vertBase[3];      // 40 per-LOD first pooled vertex
	UInt32 meshletBase[3];   // 52 per-LOD first pooled meshlet
	UInt32 meshletCount[3];  // 64 per-LOD meshlet count (0 = level not generated)
	F32    lodRadius[3];     // 76 per-LOD bounding-sphere radius
	UInt32 pad;              // 88
	UInt32 pad2;             // 92 -> struct size 96 (HLSL structures pad float4 alignment to 16)
};
static_assert(sizeof(SceneMeshInfo) == 96, "SceneMeshInfo must match the HLSL 16-byte-aligned layout");

struct SceneMeshlet { // 48 B
	UInt32 vertsOffset;   // into MeshletVerts
	UInt32 vertsCount;
	UInt32 trisOffset;    // into MeshletTris (uint per index)
	UInt32 triCount;
	Vec4   center;        // meshlet-local bounding sphere center
	F32    radius;
	UInt32 pad[3];
};

// ---- M4: meshoptimizer cluster-LOD data ----------------------------------
struct ClusterGPU { // 64 B (layout must match the HLSL struct)
	UInt32 vertexIdsOffset; // into ClusterVertexIds
	UInt32 vertexCount;
	UInt32 triOffset;       // into ClusterTris (3 uints per triangle)
	UInt32 triCount;
	Vec4   center;          // cluster bounds, mesh-local space
	F32    radius;
	F32    error;           // simplification error (mesh units)
	int    groupId;         // group this cluster belongs to
	int    refinedGroup;    // finer group that produced it (-1 = original geometry)
	UInt32 meshVertBase;    // pooled vertex base of the owning mesh
	UInt32 meshId;          // owning mesh
	UInt32 materialId;      // index into the albedo texture palette
	UInt32 pad1;
};

struct ClusterGroupGPU { // 32 B
	Vec4   center;          // simplified bounds of the group's children
	F32    radius;
	F32    error;
	int    depth;           // DAG level
	UInt32 pad;
};

struct ClusterMeshInfo { // 48 B
	Vec4   center;          // mesh bounding sphere (mesh-local)
	F32    radius;
	UInt32 vertBase;        // pooled vertex base
	UInt32 clusterBase;     // first cluster of this mesh in the pool
	UInt32 clusterCount;
	Vec4   color;
};

// M5: per-material constants, indexed by ClusterGPU::materialId.
struct MaterialGPU { // 48 B
	Vec4 baseColor;         // glTF baseColorFactor
	Vec4 emissive = Vec4(0, 0, 0, 1);  // rgb = emissiveFactor, a = emissive strength
	Vec4 params = Vec4(1.0f, 1.0f, 0.0f, 0.0f); // x = metallic, y = roughness,
	                                            // z = 1 when a normal map is bound,
	                                            // w = 1 when a metallicRoughness map is bound
};

// M3: vertex-clustering mesh simplification. Merges close vertices into cubic
// cells (cell size chosen to approach ~frac of the vertex count), averages
// positions/normals per cell and rebuilds the triangle list (degenerate or
// flipped triangles dropped / fixed). Operates in local mesh space.
void buildClusteredLOD(const Vector<Vertex>& verts, const Vector<UInt32>& idxs,
	float frac, std::vector<Vec4>& outPos, std::vector<Vec4>& outNorm, std::vector<UInt32>& outIdx) {
	const size_t nV = verts.size();
	if (nV == 0 || idxs.size() < 3) return;
	Vec3 mn(1e30f), mx(-1e30f);
	for (const auto& v : verts) { mn = glm::min(mn, v.position); mx = glm::max(mx, v.position); }
	const Vec3 ext = mx - mn;
	const float diag = glm::length(ext);
	if (diag < 1e-9f) return;
	const size_t target = std::max<size_t>(1, (size_t)(nV * frac));

	// Quantization: cell index keyed per axis over a packed grid.
	std::vector<UInt32> cellOf(nV, 0xFFFFFFFF); // filled below on the accepted pass
	std::vector<Vec3> cellSumP, cellSumN;
	std::vector<UInt32> cellCount;
	std::vector<Vec3> cellP; // centroid (final)

	auto runCluster = [&](float s) -> size_t {
		// grid dims per axis
		const UInt32 NX = std::max<UInt32>(1, (UInt32)(ext.x / s) + 2);
		const UInt32 NY = std::max<UInt32>(1, (UInt32)(ext.y / s) + 2);
		const UInt32 NZ = std::max<UInt32>(1, (UInt32)(ext.z / s) + 2);
		const UInt64 step = (UInt64)NX * NY;
		std::unordered_map<UInt64, UInt32> cellIdx;
		cellSumP.clear(); cellSumN.clear(); cellCount.clear();
		cellSumP.reserve(nV); cellSumN.reserve(nV); cellCount.reserve(nV);
		std::vector<UInt32> localCell(nV);
		for (size_t i = 0; i < nV; ++i) {
			const Vec3 q = (verts[i].position - mn) / s;
			UInt32 ix = (UInt32)q.x, iy = (UInt32)q.y, iz = (UInt32)q.z;
			if (ix >= NX) ix = NX - 1; if (iy >= NY) iy = NY - 1; if (iz >= NZ) iz = NZ - 1;
			const UInt64 key = (UInt64)ix + (UInt64)iy * NX + (UInt64)iz * step;
			auto it = cellIdx.find(key);
			UInt32 c;
			if (it == cellIdx.end()) {
				c = (UInt32)cellIdx.size();
				cellIdx.emplace(key, c);
				cellSumP.push_back(Vec3(0)); cellSumN.push_back(Vec3(0)); cellCount.push_back(0);
			} else c = it->second;
			cellSumP[c] += verts[i].position;
			cellSumN[c] += verts[i].normal;
			cellCount[c]++;
			localCell[i] = c;
		}
		const size_t n = cellIdx.size();
		// Stash the final remap if this cell size is used.
		if (cellP.size() == n) {
			for (size_t i = 0; i < nV; ++i) cellOf[i] = localCell[i];
		}
		return n;
	};

	// Adjust the cell size towards the target count (few fixed-point iterations).
	float s = diag / std::cbrt((float)target);
	for (int iter = 0; iter < 10; ++iter) {
		const size_t n = runCluster(s);
		if (n > (size_t)(target * 1.10f)) s *= 1.25f;
		else if (n < (size_t)(target * 0.80f)) s *= 0.8f;
		else break;
	}
	const size_t nC = runCluster(s); // final authoritative pass
	if (nC == 0) return;

	// Centroids.
	cellP.assign(nC, Vec3(0));
	cellOf.assign(nV, 0xFFFFFFFF);
	{
		// rerun with the accepted cell size to fill cellOf/cellSum again
		const UInt32 NX = std::max<UInt32>(1, (UInt32)(ext.x / s) + 2);
		const UInt32 NY = std::max<UInt32>(1, (UInt32)(ext.y / s) + 2);
		const UInt32 NZ = std::max<UInt32>(1, (UInt32)(ext.z / s) + 2);
		const UInt64 step = (UInt64)NX * NY;
		std::unordered_map<UInt64, UInt32> cellIdx;
		cellSumP.assign(nC, Vec3(0)); cellSumN.assign(nC, Vec3(0)); cellCount.assign(nC, 0);
		for (size_t i = 0; i < nV; ++i) {
			const Vec3 q = (verts[i].position - mn) / s;
			UInt32 ix = (UInt32)q.x, iy = (UInt32)q.y, iz = (UInt32)q.z;
			if (ix >= NX) ix = NX - 1; if (iy >= NY) iy = NY - 1; if (iz >= NZ) iz = NZ - 1;
			const UInt64 key = (UInt64)ix + (UInt64)iy * NX + (UInt64)iz * step;
			auto it = cellIdx.find(key);
			UInt32 c;
			if (it == cellIdx.end()) { c = (UInt32)cellIdx.size(); cellIdx.emplace(key, c); }
			else c = it->second;
			cellSumP[c] += verts[i].position;
			cellSumN[c] += verts[i].normal;
			cellCount[c]++;
			cellOf[i] = c;
		}
	}
	for (size_t c = 0; c < nC; ++c) {
		cellP[c] = cellSumP[c] / (float)std::max<UInt32>(1, cellCount[c]);
	}

	outPos.clear(); outNorm.clear(); outIdx.clear();
	outPos.reserve(nC); outNorm.reserve(nC);
	for (size_t c = 0; c < nC; ++c) {
		const Vec3 nn = glm::normalize(cellSumN[c]);
		outPos.push_back(Vec4(cellP[c], 1.0f));
		outNorm.push_back(Vec4(nn, 0.0f));
	}

	const UInt32 triN = (UInt32)(idxs.size() / 3);
	for (UInt32 t = 0; t < triN; ++t) {
		UInt32 a = cellOf[idxs[t * 3 + 0]];
		UInt32 b = cellOf[idxs[t * 3 + 1]];
		UInt32 c0 = cellOf[idxs[t * 3 + 2]];
		if (a == b || b == c0 || a == c0) continue; // degenerate
		// keep the winding consistent with the original triangle normal
		const Vec3 p0 = cellP[a], p1 = cellP[b], p2 = cellP[c0];
		Vec3 gn = glm::cross(p1 - p0, p2 - p0);
		if (glm::dot(gn, gn) < 1e-16f) continue;
		Vec3 ref(0);
		for (int k = 0; k < 3; ++k) ref += verts[idxs[t * 3 + k]].normal;
		ref = glm::normalize(ref);
		if (glm::dot(gn, ref) < 0.0f) std::swap(b, c0);
		outIdx.push_back(a); outIdx.push_back(b); outIdx.push_back(c0);
	}
}

float lodBoundingRadius(const std::vector<Vec4>& pos, const Vec3& center) {
	float r = 0;
	for (const auto& p : pos) r = std::max(r, glm::length(Vec3(p) - center));
	return r;
}

} // namespace

// Matches the structs above (also declared in the mesh shader below).
static const char* g_SceneAS = R"(
struct SceneTask { uint meshId; uint instanceId; };
struct SceneMeshInfo {
    float4 center;
    float4 color;
    float  radius;
    uint   lodCount;
    uint   vertBase[3];
    uint   meshletBase[3];
    uint   meshletCount[3];
    float  lodRadius[3];
    uint   pad;
};
struct MeshletGPU {
    uint vertsOffset;
    uint vertsCount;
    uint trisOffset;
    uint triCount;
    float4 center;
    float radius;
    uint p0, p1, p2;
};
StructuredBuffer<SceneTask>      SceneTasks;
StructuredBuffer<float4x4>       SceneInstances;
StructuredBuffer<SceneMeshInfo>  SceneMeshInfos;
StructuredBuffer<MeshletGPU>     SceneMeshlets;
StructuredBuffer<uint>           MeshletVerts;
StructuredBuffer<uint>           MeshletTris;
StructuredBuffer<float4>         ScenePositions;
StructuredBuffer<float4>         SceneNormals;
RWByteAddressBuffer              SceneStats;

cbuffer cbSceneConstants : register(b0) {
    float4x4 g_View;
    float4x4 g_ViewProj;
    float4   g_Frustum[6];
    float    g_CoTanHalfFov;
    float    g_CurrTime;
    uint     g_FrustumCulling;
    uint     g_ActiveInstances;
    float4   g_LightDir;
    float    g_LodScale;
    float    g_DropSize;
    uint     g_MeshletCap;
    uint     g_DebugMode;
    float4   g_BodyCenter;
    float    g_BodyRadius;
    uint     g_MeshFilter;
    float    g_ScreenHeight;
    uint     g_ClusterCapacity;
    float4   g_CameraPos;
    float4   g_Ambient;
    float4   g_LightColor;
    float4x4 g_ShadowMapUVDepth[4];
    float4   g_CascadeSplits;
};

struct ScenePayload {
    uint starts[32];
    uint counts[32];
    uint baseIds[32];
    uint meshIds[32];
    uint instanceIds[32];
    uint count;
    uint pad[3];
};

groupshared uint s_VisCount;
groupshared uint s_VisMesh[32];
groupshared uint s_VisInst[32];
groupshared uint s_VisLod[32];
groupshared uint s_TotalMeshlets;
groupshared ScenePayload s_Payload; // the group's payload (built by thread 0)

bool IsVisible(float3 sphereCenter, float radius) {
    float4 center = float4(sphereCenter, 1.0);
    for (int i = 0; i < 6; ++i) {
        if (dot(g_Frustum[i], center) < -radius)
            return false;
    }
    return true;
}

[numthreads(32, 1, 1)]
void main(in uint I  : SV_GroupIndex,
          in uint wg : SV_GroupID)
{
    if (I == 0) s_VisCount = 0;
    GroupMemoryBarrierWithGroupSync();

    // One task per amplification group: a single task's meshlet count is far
    // below the per-group budget, so geometry is never truncated.
    const bool inGroup = (I == 0);
    const uint taskId  = wg;
    SceneTask  task    = { 0u, 0xFFFFFFFFu };
    if (inGroup) task = SceneTasks[taskId];
    const bool valid   = inGroup && (task.instanceId < g_ActiveInstances) &&
                         (g_MeshFilter == 0xFFFFFFFFu || task.meshId == g_MeshFilter);

    uint visLod = 0;
    if (valid) {
        SceneMeshInfo mi  = SceneMeshInfos[task.meshId];
        float4x4      M   = SceneInstances[task.instanceId];
        const float   scale = length(float3(M[0][0], M[0][1], M[0][2]));

        // Culling uses the whole-model sphere: a small mesh part must never be
        // culled just because its own projected size is tiny.
        float4 bc    = mul(M, float4(g_BodyCenter.xyz, 1.0));
        float  brad  = g_BodyRadius * scale;
        float3 bv    = mul(g_View, float4(bc.xyz, 1.0)).xyz;
        float  bdist = max(length(bv), 1e-3);
        float  bsize = g_CoTanHalfFov * brad / bdist * g_LodScale;
        const bool bodyVisible = (g_FrustumCulling == 0 || IsVisible(bc.xyz, brad)) && (bsize >= g_DropSize);

        if (bodyVisible) {
            // LOD is chosen per mesh part (its own projected size).
            float4 mc    = mul(M, float4(mi.center.xyz, 1.0));
            float3 mv    = mul(g_View, float4(mc.xyz, 1.0)).xyz;
            float  mdist = max(length(mv), 1e-3);
            float  msize = g_CoTanHalfFov * (mi.radius * scale) / mdist * g_LodScale;
            uint lod = (msize >= 1.0) ? 0 : ((msize >= 0.3) ? 1 : 2);
            if (lod >= mi.lodCount) lod = mi.lodCount - 1;
            while (lod > 0 && mi.meshletCount[lod] == 0) --lod;
            visLod = lod;
            if (mi.meshletCount[lod] > 0) {
                uint slot;
                InterlockedAdd(s_VisCount, 1, slot);
                s_VisMesh[slot] = task.meshId;
                s_VisInst[slot] = task.instanceId;
                s_VisLod[slot]  = visLod;
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();

    // Payload + prefix built by thread 0. It also enforces the per-group
    // meshlet budget (g_MeshletCap): a heavy scene must not dispatch an
    // unbounded number of mesh groups in one DrawMesh (drives TDR otherwise).
    if (I == 0) {
        uint total = 0, k = 0;
        for (uint j = 0; j < s_VisCount; ++j) {
            const uint c = SceneMeshInfos[s_VisMesh[j]].meshletCount[s_VisLod[j]];
            if (k >= 32 || total + c > g_MeshletCap) break;
            s_Payload.starts[k]      = total;
            s_Payload.counts[k]      = c;
            s_Payload.baseIds[k]     = SceneMeshInfos[s_VisMesh[j]].meshletBase[s_VisLod[j]];
            s_Payload.meshIds[k]     = s_VisMesh[j];
            s_Payload.instanceIds[k] = s_VisInst[j];
            total += c;
            ++k;
        }
        s_Payload.count = k;
        s_Payload.pad[0] = s_Payload.pad[1] = s_Payload.pad[2] = 0;
        s_TotalMeshlets = total;

        uint orig;
        SceneStats.InterlockedAdd(0, s_VisCount, orig);
    }

    GroupMemoryBarrierWithGroupSync();

    // Must be reachable from every thread (exactly-once semantics handled by HW).
    DispatchMesh(max(s_TotalMeshlets, 1), 1, 1, s_Payload);
}
)";

static const char* g_SceneMS = R"(
struct SceneMeshInfo {
    float4 center;
    float4 color;
    float  radius;
    uint   lodCount;
    uint   vertBase[3];
    uint   meshletBase[3];
    uint   meshletCount[3];
    float  lodRadius[3];
    uint   pad;
};
struct MeshletGPU {
    uint vertsOffset;
    uint vertsCount;
    uint trisOffset;
    uint triCount;
    float4 center;
    float radius;
    uint p0, p1, p2;
};
StructuredBuffer<float4x4>       SceneInstances;
StructuredBuffer<SceneMeshInfo>  SceneMeshInfos;
StructuredBuffer<MeshletGPU>     SceneMeshlets;
StructuredBuffer<uint>           MeshletVerts;
StructuredBuffer<uint>           MeshletTris;
StructuredBuffer<float4>         ScenePositions;
StructuredBuffer<float4>         SceneNormals;

cbuffer cbSceneConstants : register(b0) {
    float4x4 g_View;
    float4x4 g_ViewProj;
    float4   g_Frustum[6];
    float    g_CoTanHalfFov;
    float    g_CurrTime;
    uint     g_FrustumCulling;
    uint     g_ActiveInstances;
    float4   g_LightDir;
    float    g_LodScale;
    float    g_DropSize;
    uint     g_MeshletCap;
    uint     g_DebugMode;
    float4   g_BodyCenter;
    float    g_BodyRadius;
    uint     g_MeshFilter;
    float    g_ScreenHeight;
    uint     g_ClusterCapacity;
    float4   g_CameraPos;
    float4   g_Ambient;
    float4   g_LightColor;
    float4x4 g_ShadowMapUVDepth[4];
    float4   g_CascadeSplits;
};

struct PSInput {
    float4 Pos    : SV_POSITION;
    float3 Normal : NORMAL;
    float4 Color  : COLOR;
};
struct ScenePayload {
    uint starts[32];
    uint counts[32];
    uint baseIds[32];
    uint meshIds[32];
    uint instanceIds[32];
    uint count;
    uint pad[3];
};

[numthreads(128, 1, 1)]
[outputtopology("triangle")]
void main(in uint I      : SV_GroupIndex,
          in uint gid    : SV_GroupID,
          in payload ScenePayload payload,
          out indices    uint3    tris[160],
          out vertices   PSInput  verts[128])
{
    // Resolve the dispatch-local group index gid to (task slot, pool meshlet).
    // The AS dispatches one extra group when nothing is visible; that group has
    // an empty payload but must still call SetMeshOutputCounts (with 0,0).
    uint remaining = gid;
    uint slot = 0;
    for (; slot < payload.count; ++slot) {
        if (remaining < payload.counts[slot]) break;
        remaining -= payload.counts[slot];
    }
    const bool valid = (slot < payload.count);

    uint meshId = 0, instId = 0, meshletId = 0, vertCount = 0, triCount = 0;
    if (valid) {
        meshId    = payload.meshIds[slot];
        instId    = payload.instanceIds[slot];
        meshletId = payload.baseIds[slot] + remaining; // global pool meshlet index
        MeshletGPU ml0 = SceneMeshlets[meshletId];
        // Defensive clamp: the declared output counts must never exceed the
        // output arrays / the group's thread count (violating that hangs the GPU).
        vertCount = min(ml0.vertsCount, 128u);
        triCount  = min(ml0.triCount, 160u);
    }

    // Diagnostic mode: emit a fixed screen-space triangle without reading any
    // geometry buffer (separates "buffer binding/read" issues from "dispatch /
    // output mechanics" issues).
    if (g_DebugMode != 0) { vertCount = 3; triCount = 1; }

    // Single dominating call - every derived mesh group (including the empty
    // safety group) must call it exactly once.
    SetMeshOutputCounts(vertCount, triCount);

    if (g_DebugMode != 0) {
        if (I < 3) {
            // Large, closest-to-camera triangle so it is unmistakably visible
            // (clip-space z near 0 survives the depth test over the scene).
            const float2 p[3] = { float2(-0.9, -0.8), float2(0.9, -0.8), float2(0.0, 0.9) };
            verts[I].Pos    = float4(p[I] * 0.6, 0.02, 1.0);
            verts[I].Normal = float3(0, 0, 1);
            verts[I].Color  = float4(1.0, 0.0, 0.0, 1.0);
        }
        if (I == 0) tris[0] = uint3(0, 1, 2);
        return;
    }

    if (!valid) return;

    MeshletGPU ml = SceneMeshlets[meshletId];
    const float4x4 M   = SceneInstances[instId];
    const float4   col = SceneMeshInfos[meshId].color;

    for (uint v = I; v < ml.vertsCount; v += 128) {
        const uint gv   = MeshletVerts[ml.vertsOffset + v];
        const float4 wp = mul(M, ScenePositions[gv]);
        const float3 wn = mul((float3x3)M, SceneNormals[gv].xyz);
        verts[v].Pos    = mul(g_ViewProj, wp);
        verts[v].Normal = wn;
        verts[v].Color  = col;
    }
    // A meshlet can hold more triangles than the group has threads: every
    // thread emits (I, I+128, ...) so no declared primitive is left with an
    // uninitialized index (undefined behaviour / driver hang).
    for (uint t = I; t < ml.triCount; t += 128) {
        tris[t] = uint3(MeshletTris[ml.trisOffset + t * 3 + 0],
                        MeshletTris[ml.trisOffset + t * 3 + 1],
                        MeshletTris[ml.trisOffset + t * 3 + 2]);
    }
}
)";

static const char* g_ScenePS = R"(
cbuffer cbSceneConstants : register(b0) {
    float4x4 g_View;
    float4x4 g_ViewProj;
    float4   g_Frustum[6];
    float    g_CoTanHalfFov;
    float    g_CurrTime;
    uint     g_FrustumCulling;
    uint     g_ActiveInstances;
    float4   g_LightDir;
    float    g_LodScale;
    float    g_DropSize;
    uint     g_MeshletCap;
    uint     g_DebugMode;
    float4   g_BodyCenter;
    float    g_BodyRadius;
    uint     g_MeshFilter;
    float    g_ScreenHeight;
    uint     g_ClusterCapacity;
    float4   g_CameraPos;
    float4   g_Ambient;
    float4   g_LightColor;
    float4x4 g_ShadowMapUVDepth[4];
    float4   g_CascadeSplits;
};
struct PSInput {
    float4 Pos    : SV_POSITION;
    float3 Normal : NORMAL;
    float4 Color  : COLOR;
};
float4 main(in PSInput i) : SV_TARGET {
    float3 n = normalize(i.Normal);
    float  d = max(dot(n, -g_LightDir.xyz), 0.0);
    float3 c = i.Color.rgb * (0.12 + 0.88 * d);
    return float4(c, 1.0);
}
)";

// ===================================================================
// M4: cluster (meshoptimizer clusterlod) rendering shaders
//   AS: one task (mesh x instance) per group; 32 threads scan the mesh's
//       clusters, do frustum culling + error-based LOD selection, then append
//       the visible ones to a global list (single atomic allocation per group).
//   MS: one cluster per group (128 threads, <=128 verts / 128 tris).
// ===================================================================
static const char* g_ClusterAS = R"(
struct ClusterTask { uint meshId; uint instanceId; uint groupId; uint pad; };
struct ClusterMeshInfo {
    float4 center;
    float  radius;
    uint   vertBase;
    uint   clusterBase;
    uint   clusterCount;
    float4 color;
};
struct ClusterGPU {
    uint   vertexIdsOffset;
    uint   vertexCount;
    uint   triOffset;
    uint   triCount;
    float4 center;
    float  radius;
    float  error;
    int    groupId;
    int    refinedGroup;
    uint   meshVertBase;
    uint   meshId;
    uint   materialId;
    uint   pad1;
};
struct ClusterGroupGPU {
    float4 center;
    float  radius;
    float  error;
    int    depth;
    uint   pad;
};
struct MeshGroupGPU {
    float4 center;
    float  radius;
    uint   p0;
    uint   p1;
    uint   p2;
};
StructuredBuffer<ClusterTask>     Tasks;
StructuredBuffer<float4x4>        Instances;
StructuredBuffer<ClusterMeshInfo> MeshInfos;
StructuredBuffer<ClusterGPU>      Clusters;
StructuredBuffer<ClusterGroupGPU> Groups;
StructuredBuffer<MeshGroupGPU>    GroupBounds;
RWByteAddressBuffer               ClusterCounter;
RWByteAddressBuffer               SceneStats;

cbuffer cbSceneConstants : register(b0) {
    float4x4 g_View;
    float4x4 g_ViewProj;
    float4   g_Frustum[6];
    float    g_CoTanHalfFov;
    float    g_CurrTime;
    uint     g_FrustumCulling;
    uint     g_ActiveInstances;
    float4   g_LightDir;
    float    g_LodScale;
    float    g_DropSize;
    uint     g_MeshletCap;
    uint     g_DebugMode;
    float4   g_BodyCenter;
    float    g_BodyRadius;
    uint     g_MeshFilter;
    float    g_ScreenHeight;
    uint     g_ClusterCapacity;
    float4   g_CameraPos;
    float4   g_Ambient;
    float4   g_LightColor;
    float4x4 g_ShadowMapUVDepth[4];
    float4   g_CascadeSplits;
};

// The visible-cluster list is handed to the child mesh shader groups through the
// *payload*, never through global memory: DispatchMesh only implies a
// GroupMemoryBarrierWithGroupSync() (groupshared scope), so a UAV written by the
// amplification shader is not guaranteed to be visible to its mesh shaders. That
// hazard showed up as flickering garbage triangles followed by a hard device
// hang. The M1 test grid uses the same payload hand-off and has never hung.
#define CLUSTER_PER_GROUP 384
struct ClusterPayload {
    uint  count;
    uint2 list[CLUSTER_PER_GROUP];
};

groupshared uint s_Counts[32];
groupshared uint s_Base;
groupshared uint s_Total;
groupshared uint s_Count;
groupshared ClusterPayload s_Payload;

bool IsVisible(float3 sphereCenter, float radius) {
    float4 center = float4(sphereCenter, 1.0);
    for (int i = 0; i < 6; ++i) {
        if (dot(g_Frustum[i], center) < -radius)
            return false;
    }
    return true;
}

// Approximate screen-space error in pixels (meshoptimizer's clusterlod formula).
float ProjError(float3 c, float r, float err) {
    float3 v = mul(g_View, float4(c, 1.0)).xyz;
    float  d = max(length(v) - r, 0.05);
    return err / d * (g_CoTanHalfFov * 0.5) * g_ScreenHeight;
}

// clusterlod selection rule (see the clodGroup documentation): a cluster is
// rendered iff the simplification that produced its group is acceptable AND the
// next simplification (away from its group) is not - i.e. this is the finest
// level whose error is still within the threshold.
bool ClusterSelected(uint clusterIndex, float4x4 M, float scale) {
    ClusterGPU cl = Clusters[clusterIndex];

    float4 wc = mul(M, float4(cl.center.xyz, 1.0));
    float  wr = cl.radius * scale;
    if (g_FrustumCulling != 0 && !IsVisible(wc.xyz, wr)) return false;

    // 1) the group this cluster belongs to must still be needed (its further
    //    simplification would be too coarse).
    if (cl.groupId >= 0) {
        ClusterGroupGPU g = Groups[cl.groupId];
        float4 gc = mul(M, float4(g.center.xyz, 1.0));
        if (ProjError(gc.xyz, g.radius * scale, g.error * scale) <= g_LodScale) return false;
    }
    // 2) the simplification from the refined (finer) group into this one must be
    //    acceptable; otherwise the finer clusters are drawn instead.
    if (cl.refinedGroup >= 0) {
        ClusterGroupGPU rg = Groups[cl.refinedGroup];
        float4 rc = mul(M, float4(rg.center.xyz, 1.0));
        if (ProjError(rc.xyz, rg.radius * scale, rg.error * scale) > g_LodScale) return false;
    }
    return true;
}

[numthreads(32, 1, 1)]
void main(in uint I  : SV_GroupIndex,
          in uint wg : SV_GroupID)
{
    ClusterTask t = Tasks[wg];
    const bool validTask = (t.instanceId < g_ActiveInstances) &&
                           (g_MeshFilter == 0xFFFFFFFFu || t.meshId == g_MeshFilter);
    ClusterMeshInfo mi = MeshInfos[t.meshId];
    const uint instIdx = validTask ? t.instanceId : 0u;
    float4x4 M = Instances[instIdx];
    const float scale = length(float3(M[0][0], M[0][1], M[0][2]));
    const uint  capacity = g_ClusterCapacity;

    bool bodyVisible = false;
    if (validTask) {
        // Per draw-group sphere: culling and the too-small drop test follow the
        // object the task belongs to (terrain, a wall, one character, ...), not
        // a single scene-wide sphere.
        MeshGroupGPU gb = GroupBounds[t.groupId];
        float4 bc = mul(M, float4(gb.center.xyz, 1.0));
        float  br = gb.radius * scale;
        float3 bv = mul(g_View, float4(bc.xyz, 1.0)).xyz;
        float  bd = max(length(bv), 0.05);
        float  bsize = g_CoTanHalfFov * br / bd;
        bodyVisible = (g_FrustumCulling == 0 || IsVisible(bc.xyz, br)) && (bsize >= g_DropSize);
    }

    // First pass: count the visible clusters this thread is responsible for.
    uint count = 0;
    if (bodyVisible) {
        for (uint c = I; c < mi.clusterCount; c += 32) {
            if (ClusterSelected(mi.clusterBase + c, M, scale)) ++count;
        }
    }
    s_Counts[I] = count;
    GroupMemoryBarrierWithGroupSync();

    uint offset = 0, total = 0;
    for (uint j = 0; j < 32; ++j) {
        if (j < I) offset += s_Counts[j];
        total += s_Counts[j];
    }
    if (I == 0) {
        // Global budget accounting only (never used to index anything that the
        // child mesh shader groups read back).
        uint base = 0;
        ClusterCounter.InterlockedAdd(0, total, base);
        const uint room = (base < capacity) ? (capacity - base) : 0u;
        s_Base  = base;
        s_Total = total;
        s_Count = min(total, min((uint)CLUSTER_PER_GROUP, room));
    }
    GroupMemoryBarrierWithGroupSync();

    // Second pass: append this thread's clusters to the payload list.
    uint idx = 0;
    if (bodyVisible) {
        for (uint c = I; c < mi.clusterCount; c += 32) {
            if (ClusterSelected(mi.clusterBase + c, M, scale)) {
                const uint slot = offset + idx;
                if (slot < CLUSTER_PER_GROUP) s_Payload.list[slot] = uint2(mi.clusterBase + c, instIdx);
                ++idx;
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (I == 0) {
        s_Payload.count = s_Count;
        // Statistics (offsets 0/4/8 of the raw stats buffer): visible
        // (mesh, instance) tasks, clusters actually dispatched, and the raw
        // cluster demand before the budget clamp. All are read back by the CPU
        // on a later frame, so no shader-side ordering is needed here.
        uint prev;
        if (bodyVisible) SceneStats.InterlockedAdd(0, 1u, prev);
        SceneStats.InterlockedAdd(4, s_Count, prev);
        SceneStats.InterlockedAdd(8, s_Total, prev);
    }
    GroupMemoryBarrierWithGroupSync();

    // Must be reachable from every thread; the payload was built above.
    DispatchMesh(max(s_Payload.count, 1), 1, 1, s_Payload);
}
)";

static const char* g_ClusterMS = R"(
struct ClusterMeshInfo {
    float4 center;
    float  radius;
    uint   vertBase;
    uint   clusterBase;
    uint   clusterCount;
    float4 color;
};
struct ClusterGPU {
    uint   vertexIdsOffset;
    uint   vertexCount;
    uint   triOffset;
    uint   triCount;
    float4 center;
    float  radius;
    float  error;
    int    groupId;
    int    refinedGroup;
    uint   meshVertBase;
    uint   meshId;
    uint   materialId;
    uint   pad1;
};
struct MaterialGPU {
    float4 baseColor;
    float4 emissive;
    float4 params;   // x = metallic, y = roughness, z = has normal map, w = has MR map
};
StructuredBuffer<float4x4>        Instances;
StructuredBuffer<ClusterMeshInfo> MeshInfos;
StructuredBuffer<ClusterGPU>      Clusters;
StructuredBuffer<uint>            ClusterVertexIds;
StructuredBuffer<uint>            ClusterTris;
StructuredBuffer<float4>          ScenePositions;
StructuredBuffer<float4>          SceneNormals;
StructuredBuffer<float4>          SceneUVs;
StructuredBuffer<MaterialGPU>     Materials;

cbuffer cbSceneConstants : register(b0) {
    float4x4 g_View;
    float4x4 g_ViewProj;
    float4   g_Frustum[6];
    float    g_CoTanHalfFov;
    float    g_CurrTime;
    uint     g_FrustumCulling;
    uint     g_ActiveInstances;
    float4   g_LightDir;
    float    g_LodScale;
    float    g_DropSize;
    uint     g_MeshletCap;
    uint     g_DebugMode;
    float4   g_BodyCenter;
    float    g_BodyRadius;
    uint     g_MeshFilter;
    float    g_ScreenHeight;
    uint     g_ClusterCapacity;
    float4   g_CameraPos;
    float4   g_Ambient;
    float4   g_LightColor;
    float4x4 g_ShadowMapUVDepth[4];
    float4   g_CascadeSplits;
};

struct PSInput {
    float4 Pos    : SV_POSITION;
    float3 WorldPos : TEXCOORD2;
    float3 Normal : NORMAL;
    float4 Color  : COLOR;
    float2 UV     : TEXCOORD0;
    nointerpolation uint MatId : TEXCOORD1;
};
// Must match the payload built by g_ClusterAS (see CLUSTER_PER_GROUP there).
#define CLUSTER_PER_GROUP 384
struct ClusterPayload {
    uint  count;
    uint2 list[CLUSTER_PER_GROUP];
};

[numthreads(128, 1, 1)]
[outputtopology("triangle")]
void main(in uint I   : SV_GroupIndex,
          in uint gid : SV_GroupID,
          in payload ClusterPayload payload,
          out indices  uint3   tris[256],
          out vertices PSInput verts[128])
{
    uint vertexCount = 0, triCount = 0;
    ClusterGPU cl;
    float4x4   M;
    float4     col = float4(1, 1, 1, 1);

    // Diagnostic: g_DebugMode == 1 makes the first group ignore the culling
    // results and emit a single fixed triangle (proves the pipeline itself
    // rasterizes even when nothing is classified as visible).
    const bool debugTri = (g_DebugMode == 1) && (gid == 0);
    const bool valid = !debugTri && (gid < payload.count);
    if (valid) {
        uint2 entry = payload.list[gid];
        cl  = Clusters[entry.x];
        M   = Instances[entry.y];
        col = Materials[cl.materialId].baseColor;
        vertexCount = min(cl.vertexCount, 128u);
        triCount    = min(cl.triCount, 256u);
    } else if (debugTri) {
        vertexCount = 3;
        triCount    = 1;
    }

    // Every derived mesh group (including the empty safety group) must call this.
    SetMeshOutputCounts(vertexCount, triCount);

    if (debugTri) {
        const float2 p[3] = { float2(-0.6, -0.5), float2(0.6, -0.5), float2(0.0, 0.6) };
        for (uint i = I; i < 3; i += 128) {
            verts[i].Pos    = float4(p[i], 0.5, 1.0);
            verts[i].Normal = float3(0, 0, -1);
            verts[i].Color  = float4(1, 0, 1, 1);
            verts[i].UV     = float2(0, 0);
            verts[i].MatId  = 0u;
            verts[i].WorldPos = float3(0, 0, 0);
        }
        if (I == 0) tris[0] = uint3(0, 1, 2);
        return;
    }
    if (!valid) return;

    for (uint v = I; v < vertexCount; v += 128) {
        const uint gv   = ClusterVertexIds[cl.vertexIdsOffset + v] + cl.meshVertBase;
        const float4 wp = mul(M, ScenePositions[gv]);
        const float3 wn = mul((float3x3)M, SceneNormals[gv].xyz);
        verts[v].Pos    = mul(g_ViewProj, wp);
        verts[v].Normal = wn;
        verts[v].Color  = col;
        verts[v].UV     = SceneUVs[gv].xy;
        verts[v].MatId  = cl.materialId;
        verts[v].WorldPos = wp.xyz;
    }
    for (uint t = I; t < triCount; t += 128) {
        tris[t] = uint3(ClusterTris[cl.triOffset + t * 3 + 0],
                        ClusterTris[cl.triOffset + t * 3 + 1],
                        ClusterTris[cl.triOffset + t * 3 + 2]);
    }
}
)";

// ===================================================================
// M5: cluster pixel shader - glTF material textures + the engine's lighting
// model (half-Lambert diffuse + Blinn specular + ambient + emissive), so the
// mesh shader path matches the forward path instead of having its own look.
// ===================================================================
static const char* g_ClusterPS = R"(
// Must match kMaxMaterials in MeshShaderSubsystem.cpp.
#define MAX_MATERIALS 32

cbuffer cbSceneConstants : register(b0) {
    float4x4 g_View;
    float4x4 g_ViewProj;
    float4   g_Frustum[6];
    float    g_CoTanHalfFov;
    float    g_CurrTime;
    uint     g_FrustumCulling;
    uint     g_ActiveInstances;
    float4   g_LightDir;
    float    g_LodScale;
    float    g_DropSize;
    uint     g_MeshletCap;
    uint     g_DebugMode;
    float4   g_BodyCenter;
    float    g_BodyRadius;
    uint     g_MeshFilter;
    float    g_ScreenHeight;
    uint     g_ClusterCapacity;
    float4   g_CameraPos;
    float4   g_Ambient;
    float4   g_LightColor;
    float4x4 g_ShadowMapUVDepth[4];
    float4   g_CascadeSplits;
    float4   g_SkyCorners[8];   // 8 skybox corner colours, see SkyIrradiance below
};

// Diffuse irradiance of the analytic sky.
//
// The 8-corner skybox is trilinear in the direction, which to first order is
// A + dot(B, l), with A the mean of the corners and B_i the mean of the corners
// that have the i-th bit set minus A. The cosine-weighted average of such a
// function over the hemisphere around n is A + (2/3) * dot(B, n) - the 2/3 is the
// standard cosine-weighted mean of a linear function - so the ambient becomes both
// directional (bright from the sky, ground bounce from below) and coloured by the
// sky, instead of a flat grey that lights every surface identically.
// When no skybox is set the corners are filled with the ambient colour, which
// makes this fall back to exactly the old behaviour.
float3 SkyIrradiance(float3 n)
{
    float3 A = float3(0, 0, 0);
    float3 B[3] = { float3(0, 0, 0), float3(0, 0, 0), float3(0, 0, 0) };
    [unroll]
    for (int j = 0; j < 8; ++j)
    {
        const float3 c = g_SkyCorners[j].rgb;
        A += c * 0.125;
        [unroll]
        for (int i = 0; i < 3; ++i)
            if (((j >> i) & 1) != 0) B[i] += c * 0.25;
    }
    [unroll]
    for (int i = 0; i < 3; ++i) B[i] -= A;
    return max(A + (2.0 / 3.0) * (B[0] * n.x + B[1] * n.y + B[2] * n.z), 0.0);
}
struct MaterialGPU {
    float4 baseColor;
    float4 emissive;
    float4 params;   // x = metallic, y = roughness, z/w unused
};

Texture2D    g_Albedo[MAX_MATERIALS]   : register(t0);
Texture2D    g_NormalMap[MAX_MATERIALS] : register(t32);
Texture2D    g_MetalRough[MAX_MATERIALS] : register(t64);
Texture2D    g_EmissiveMap[MAX_MATERIALS] : register(t96);
// Cascaded shadow map, exactly as the forward PBR shader declares it (t4/s4
// there; here the material palettes already own t0..t127).
Texture2DArray g_ShadowMap : register(t128);
SamplerComparisonState g_ShadowMapSampler : register(s1);
SamplerState g_AlbedoSampler : register(s0);
StructuredBuffer<MaterialGPU> Materials;

struct PSInput {
    float4 Pos      : SV_POSITION;
    float3 WorldPos : TEXCOORD2;
    float3 Normal   : NORMAL;
    float4 Color    : COLOR;
    float2 UV       : TEXCOORD0;
    nointerpolation uint MatId : TEXCOORD1;
};

// Tangent frame from screen-space derivatives (no TANGENT attribute is loaded
// by the glTF importer, and this is robust across the whole mesh).
// Tangent-frame sign for the normal map, one factor per tangent axis:
//   x -> the red channel (tangent / +u), y -> the green channel (bitangent).
//
// This is NOT an asset-format quirk that can be reasoned away: the frame below is
// built from screen-space derivatives (a cotangent frame) instead of the glTF
// TANGENT attribute - which this loader does not use yet, it writes a dummy
// tangent - and the resulting handedness depends on the screen/UV orientation, so
// the conversion from the asset's tangent-space convention into *this* frame is a
// sign that has to be verified once, visually:
//
//   put a directional normal map (bricks, rock, tiles) on a surface, light it from
//   the side, and check that the bumps are lit on the side facing the light. If
//   the relief looks inverted left/right, flip kNormalMapSign.x; if it is inverted
//   up/down, flip kNormalMapSign.y. Current setting matches the glTF (+Y up /
//   OpenGL-style) maps that the loader feeds in.
//
// Loading the real glTF TANGENT_0 attribute (or generating MikkTSpace tangents, as
// the spec asks when they are absent) removes the ambiguity entirely: the frame
// would then come from the asset exactly as the spec defines it.
static const float2 kNormalMapSign = float2(1.0, -1.0);

float3 NormalMapped(float3 N, float3 wp, float2 uv, float3 sampledNormal) {
    float3 dp1 = ddx(wp);
    float3 dp2 = ddy(wp);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);
    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    // Epsilon: degenerate UVs (or the debug triangle) give a zero frame, and
    // rsqrt(0) would produce NaN normals.
    float invmax = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
    float3 n = sampledNormal * float3(kNormalMapSign, 1.0);
    return normalize(mul(n, float3x3(T * invmax, B * invmax, N)));
}

float4 main(in PSInput i) : SV_TARGET {
    // MatId is always < MAX_MATERIALS (validated on the CPU before upload), but
    // clamp anyway so a bad value can never index outside the descriptor array.
    const uint id = min(i.MatId, (uint)(MAX_MATERIALS - 1));
    const MaterialGPU mat = Materials[id];

    // Unused maps are bound to neutral 1x1 textures (flat normal, white ORM,
    // black emissive) so the samples below are correct without any branching.
    float4 tex = g_Albedo[id].Sample(g_AlbedoSampler, i.UV);
    float3 bc  = tex.rgb * mat.baseColor.rgb;
    float  a   = tex.a * mat.baseColor.a;

    // glTF ORM packing: G = roughness, B = metallic.
    float4 orm = g_MetalRough[id].Sample(g_AlbedoSampler, i.UV);
    float  m = saturate(orm.b * mat.params.x);
    float  r = saturate(orm.g * mat.params.y);

    float3 N = normalize(i.Normal);
    float3 sn = g_NormalMap[id].Sample(g_AlbedoSampler, i.UV).xyz * 2.0 - 1.0;
    N = NormalMapped(N, i.WorldPos, i.UV, sn);
    float3 V = normalize(g_CameraPos.xyz - i.WorldPos);

    // Same shading model as the forward PBR shader: ambient + one directional
    // light with half-Lambert diffuse and a Blinn specular lobe, attenuated by
    // the cascaded shadow map. The ambient is the sky's irradiance, so it carries
    // the sky's colour and direction (g_Ambient.a still scales it).
    float3 col = SkyIrradiance(N) * g_Ambient.a * bc;
    {
        float3 Ldir = normalize(-g_LightDir.xyz);
        float  NdotL = dot(N, Ldir) * 0.5 + 0.5;
        float3 H = normalize(Ldir + V);
        float  specExp = max(1.0, (1.0 - r) * 256.0);
        float  spec = pow(max(dot(N, H), 0.001), specExp);
        float3 diff = bc * (1.0 - m);
        float3 specC = lerp(float3(0.04, 0.04, 0.04), bc, m);

        // Cascade selection + PCF comparison, matching g_PS_Forward's DoLight.
        // Without this every object drawn through the mesh shader path would be
        // lit as if nothing occluded it.
        float shadow = 1.0;
        if (g_LightColor.a > 0.0) {
            const float camZ = abs(mul(g_ViewProj, float4(i.WorldPos, 1.0)).w);
            uint c = 0;
            if (camZ > g_CascadeSplits.x) c = 1;
            if (camZ > g_CascadeSplits.y) c = 2;
            if (camZ > g_CascadeSplits.z) c = 3;
            float4 sc = mul(g_ShadowMapUVDepth[c], float4(i.WorldPos, 1.0));
            sc.xyz /= max(sc.w, 1e-6);
            const float bias = 0.005 + 0.01 * (1.0 - NdotL);
            shadow = g_ShadowMap.SampleCmpLevelZero(g_ShadowMapSampler, float3(sc.xy, (float)c), sc.z - bias);
        }

        col += (diff * NdotL + specC * spec) * g_LightColor.rgb * g_LightColor.a * shadow;
    }

    // Emissive is self-emission: added after lighting (not tinted by the light).
    float3 em = g_EmissiveMap[id].Sample(g_AlbedoSampler, i.UV).rgb * mat.emissive.rgb * mat.emissive.a;
    col += em;

    // Albedo-only diagnostic (MS Debug: Albedo).
    if (g_DebugMode == 2) col = bc;

    return float4(col, a);
}
)";

// ===================================================================
// M6: cluster G-buffer pixel shader - the hybrid ray tracing path rasterizes
// the scene into albedo / world-normal / emissive and lets the RT compose pass
// do the lighting, so this variant writes the same three targets (with normal
// mapping) instead of shaded colour. It must produce exactly what
// g_PS_GBuffer does, or the denoiser and RT reflections will disagree with the
// forward path.
// ===================================================================
static const char* g_ClusterGBufferPS = R"(
#define MAX_MATERIALS 32

cbuffer cbSceneConstants : register(b0) {
    float4x4 g_View;
    float4x4 g_ViewProj;
    float4   g_Frustum[6];
    float    g_CoTanHalfFov;
    float    g_CurrTime;
    uint     g_FrustumCulling;
    uint     g_ActiveInstances;
    float4   g_LightDir;
    float    g_LodScale;
    float    g_DropSize;
    uint     g_MeshletCap;
    uint     g_DebugMode;
    float4   g_BodyCenter;
    float    g_BodyRadius;
    uint     g_MeshFilter;
    float    g_ScreenHeight;
    uint     g_ClusterCapacity;
    float4   g_CameraPos;
    float4   g_Ambient;
    float4   g_LightColor;
    float4x4 g_ShadowMapUVDepth[4];
    float4   g_CascadeSplits;
};

struct MaterialGPU {
    float4 baseColor;
    float4 emissive;
    float4 params;   // x = metallic, y = roughness
};

Texture2D    g_Albedo[MAX_MATERIALS]      : register(t0);
Texture2D    g_NormalMap[MAX_MATERIALS]   : register(t32);
Texture2D    g_MetalRough[MAX_MATERIALS]  : register(t64);
Texture2D    g_EmissiveMap[MAX_MATERIALS] : register(t96);
SamplerState g_AlbedoSampler : register(s0);
StructuredBuffer<MaterialGPU> Materials;

struct PSInput {
    float4 Pos      : SV_POSITION;
    float3 WorldPos : TEXCOORD2;
    float3 Normal   : NORMAL;
    float4 Color    : COLOR;
    float2 UV       : TEXCOORD0;
    nointerpolation uint MatId : TEXCOORD1;
};

struct PSOut {
    float4 Color : SV_Target0;  // albedo (RGBA8_SRGB)
    float4 Norm  : SV_Target1;  // world normal, roughness in alpha (RGBA16_FLOAT)
    float4 Emis  : SV_Target2;  // emissive (RGBA16_FLOAT)
};

// See the note on kNormalMapSign in the cluster G-buffer shader above: the sign
// belongs to the derivative-built tangent frame, not to the asset, and is meant to
// be verified visually once (flip x for a left/right inversion, y for up/down).
static const float2 kNormalMapSign = float2(1.0, -1.0);

float3 NormalMapped(float3 N, float3 wp, float2 uv, float3 sampledNormal) {
    float3 dp1 = ddx(wp);
    float3 dp2 = ddy(wp);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);
    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
    float3 n = sampledNormal * float3(kNormalMapSign, 1.0);
    return normalize(mul(n, float3x3(T * invmax, B * invmax, N)));
}

PSOut main(in PSInput i) {
    const uint id = min(i.MatId, (uint)(MAX_MATERIALS - 1));
    const MaterialGPU mat = Materials[id];

    PSOut o;
    o.Color = g_Albedo[id].Sample(g_AlbedoSampler, i.UV) * mat.baseColor;

    float4 orm = g_MetalRough[id].Sample(g_AlbedoSampler, i.UV);
    float  r = saturate(orm.g * mat.params.y);

    float3 N = normalize(i.Normal);
    float3 sn = g_NormalMap[id].Sample(g_AlbedoSampler, i.UV).xyz * 2.0 - 1.0;
    N = NormalMapped(N, i.WorldPos, i.UV, sn);

    o.Norm = float4(N, r);
    o.Emis = float4(g_EmissiveMap[id].Sample(g_AlbedoSampler, i.UV).rgb * mat.emissive.rgb * mat.emissive.a, 1.0);
    return o;
}
)";

// ===================================================================
// Scene pipeline (lazily created)
// ===================================================================
Result<void, RenderError> MeshShaderSubsystem::ensureScenePipeline(Impl& p, UInt32 sampleCount) {
	auto* dev = p.renderer->getDevice() ? static_cast<D::IRenderDevice*>(p.renderer->getDevice()) : nullptr;
	auto* ctx = p.renderer->getContext() ? static_cast<D::IDeviceContext*>(p.renderer->getContext()) : nullptr;
	if (!dev || !ctx) return RenderError::NotInitialized;
	if (p.scenePso && p.scenePsoSampleCount == sampleCount) return {};
	p.scenePso.Release(); p.sceneSrb.Release();

	auto makeShader = [&](D::SHADER_TYPE type, const char* name, const char* src) -> D::RefCntAutoPtr<D::IShader> {
		D::ShaderCreateInfo ci;
		ci.Desc.ShaderType = type;
		ci.Desc.Name = name;
		ci.EntryPoint = "main";
		ci.SourceLanguage = D::SHADER_SOURCE_LANGUAGE_HLSL;
		ci.ShaderCompiler = D::SHADER_COMPILER_DXC;
		ci.HLSLVersion = { 6, 5 };
		ci.Source = src;
		ci.SourceLength = (D::Uint32)strlen(src);
		D::RefCntAutoPtr<D::IShader> s;
		dev->CreateShader(ci, &s);
		if (!s || s->GetStatus() != D::SHADER_STATUS_READY) { EError("MeshShader: failed to compile {}", name); return {}; }
		return s;
	};
	auto as = makeShader(D::SHADER_TYPE_AMPLIFICATION, "Scene MS AS", g_SceneAS);
	auto ms = makeShader(D::SHADER_TYPE_MESH, "Scene MS MS", g_SceneMS);
	auto ps = makeShader(D::SHADER_TYPE_PIXEL, "Scene MS PS", g_ScenePS);
	if (!as || !ms || !ps) return RenderError::ShaderCompilationFailed;

	D::GraphicsPipelineStateCreateInfo ci;
	ci.PSODesc.Name = "Mesh Shader Scene";
	ci.PSODesc.PipelineType = D::PIPELINE_TYPE_MESH;
	ci.PSODesc.ResourceLayout.DefaultVariableType = D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
	ci.GraphicsPipeline.PrimitiveTopology = D::PRIMITIVE_TOPOLOGY_UNDEFINED;
	ci.GraphicsPipeline.NumRenderTargets = 1;
	ci.GraphicsPipeline.RTVFormats[0] = D::TEX_FORMAT_RGBA16_FLOAT;
	ci.GraphicsPipeline.DSVFormat = D::TEX_FORMAT_D32_FLOAT;
	ci.GraphicsPipeline.SmplDesc.Count = static_cast<D::Uint8>(sampleCount);
	ci.GraphicsPipeline.RasterizerDesc.CullMode = D::CULL_MODE_BACK;
	ci.GraphicsPipeline.RasterizerDesc.FillMode = D::FILL_MODE_SOLID;
	// The engine's own pipelines treat counter-clockwise winding as front facing
	// (see RenderTypes.hpp); Diligent defaults to clockwise, which would cull the
	// front faces of every mesh drawn through this path.
	ci.GraphicsPipeline.RasterizerDesc.FrontCounterClockwise = true;
	ci.GraphicsPipeline.DepthStencilDesc.DepthEnable = true;
	ci.GraphicsPipeline.DepthStencilDesc.DepthWriteEnable = true;
	ci.GraphicsPipeline.DepthStencilDesc.DepthFunc = D::COMPARISON_FUNC_LESS;
	ci.pAS = as; ci.pMS = ms; ci.pPS = ps;
	dev->CreateGraphicsPipelineState(ci, &p.scenePso);
	if (!p.scenePso) { EError("MeshShader: failed to create the scene mesh pipeline"); return RenderError::PipelineStateCreationFailed; }
	p.scenePsoSampleCount = static_cast<UInt8>(sampleCount);
	p.scenePso->CreateShaderResourceBinding(&p.sceneSrb, true);
	if (!p.sceneSrb) { p.scenePso.Release(); return RenderError::PipelineStateCreationFailed; }

	// Per-frame dynamic constants.
	if (!p.sceneCB) {
		D::BufferDesc bd; bd.Name = "MS Scene CB"; bd.Size = 1024;
		bd.BindFlags = D::BIND_UNIFORM_BUFFER; bd.Usage = D::USAGE_DYNAMIC; bd.CPUAccessFlags = D::CPU_ACCESS_WRITE;
		dev->CreateBuffer(bd, nullptr, &p.sceneCB);
		if (!p.sceneCB) { p.scenePso.Release(); return RenderError::BufferCreationFailed; }
	}
	// Instances (updated every frame with UpdateBuffer).
	if (!p.sceneInstances) {
		D::BufferDesc bd; bd.Name = "MS Scene Instances"; bd.Size = kSceneMaxInstances * sizeof(Mat4);
		bd.BindFlags = D::BIND_SHADER_RESOURCE; bd.Mode = D::BUFFER_MODE_STRUCTURED;
		bd.ElementByteStride = sizeof(Mat4); bd.Usage = D::USAGE_DEFAULT;
		dev->CreateBuffer(bd, nullptr, &p.sceneInstances);
		if (!p.sceneInstances) { p.scenePso.Release(); return RenderError::BufferCreationFailed; }
	}
	// Scene statistics (visible task count).
	if (!p.sceneStatsBuf) {
		D::BufferDesc bd; bd.Name = "MS Scene Statistics"; bd.Size = 16;
		bd.BindFlags = D::BIND_UNORDERED_ACCESS; bd.Mode = D::BUFFER_MODE_RAW; bd.Usage = D::USAGE_DEFAULT;
		dev->CreateBuffer(bd, nullptr, &p.sceneStatsBuf);
		if (!p.sceneStatsBuf) { p.scenePso.Release(); return RenderError::BufferCreationFailed; }
	}
	if (!p.sceneStatsStaging) {
		D::BufferDesc bd; bd.Name = "MS Scene Statistics Staging"; bd.Size = 16 * kStatRingSize;
		bd.Usage = D::USAGE_STAGING; bd.CPUAccessFlags = D::CPU_ACCESS_READ;
		dev->CreateBuffer(bd, nullptr, &p.sceneStatsStaging);
	}
	if (!p.sceneStatsFence) {
		D::FenceDesc fd; fd.Name = "MS Scene fence";
		dev->CreateFence(fd, &p.sceneStatsFence);
	}

	// Static geometry / task pools (uploaded once in setMeshes).
	auto bindSrv = [&](D::SHADER_TYPE stage, const char* name, D::IDeviceObject* obj) {
		if (obj) if (auto* v = p.sceneSrb->GetVariableByName(stage, name)) v->Set(obj, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	};
	bindSrv(D::SHADER_TYPE_AMPLIFICATION, "SceneTasks", p.sceneTasks ? p.sceneTasks->GetDefaultView(D::BUFFER_VIEW_SHADER_RESOURCE) : nullptr);
	bindSrv(D::SHADER_TYPE_AMPLIFICATION, "SceneInstances", p.sceneInstances->GetDefaultView(D::BUFFER_VIEW_SHADER_RESOURCE));
	bindSrv(D::SHADER_TYPE_AMPLIFICATION, "SceneMeshInfos", p.sceneMeshInfo ? p.sceneMeshInfo->GetDefaultView(D::BUFFER_VIEW_SHADER_RESOURCE) : nullptr);
	bindSrv(D::SHADER_TYPE_AMPLIFICATION, "SceneMeshlets", p.sceneMeshlets ? p.sceneMeshlets->GetDefaultView(D::BUFFER_VIEW_SHADER_RESOURCE) : nullptr);
	bindSrv(D::SHADER_TYPE_AMPLIFICATION, "MeshletVerts", p.sceneMeshletVerts ? p.sceneMeshletVerts->GetDefaultView(D::BUFFER_VIEW_SHADER_RESOURCE) : nullptr);
	bindSrv(D::SHADER_TYPE_AMPLIFICATION, "MeshletTris", p.sceneMeshletTris ? p.sceneMeshletTris->GetDefaultView(D::BUFFER_VIEW_SHADER_RESOURCE) : nullptr);
	bindSrv(D::SHADER_TYPE_AMPLIFICATION, "ScenePositions", p.scenePos ? p.scenePos->GetDefaultView(D::BUFFER_VIEW_SHADER_RESOURCE) : nullptr);
	bindSrv(D::SHADER_TYPE_AMPLIFICATION, "SceneNormals", p.sceneNorm ? p.sceneNorm->GetDefaultView(D::BUFFER_VIEW_SHADER_RESOURCE) : nullptr);
	bindSrv(D::SHADER_TYPE_AMPLIFICATION, "SceneStats", p.sceneStatsBuf->GetDefaultView(D::BUFFER_VIEW_UNORDERED_ACCESS));
	if (auto* v = p.sceneSrb->GetVariableByName(D::SHADER_TYPE_AMPLIFICATION, "cbSceneConstants")) v->Set(p.sceneCB);
	// Mesh + pixel share the instance/pool bindings.
	bindSrv(D::SHADER_TYPE_MESH, "SceneInstances", p.sceneInstances->GetDefaultView(D::BUFFER_VIEW_SHADER_RESOURCE));
	bindSrv(D::SHADER_TYPE_MESH, "SceneMeshInfos", p.sceneMeshInfo ? p.sceneMeshInfo->GetDefaultView(D::BUFFER_VIEW_SHADER_RESOURCE) : nullptr);
	bindSrv(D::SHADER_TYPE_MESH, "SceneMeshlets", p.sceneMeshlets ? p.sceneMeshlets->GetDefaultView(D::BUFFER_VIEW_SHADER_RESOURCE) : nullptr);
	bindSrv(D::SHADER_TYPE_MESH, "MeshletVerts", p.sceneMeshletVerts ? p.sceneMeshletVerts->GetDefaultView(D::BUFFER_VIEW_SHADER_RESOURCE) : nullptr);
	bindSrv(D::SHADER_TYPE_MESH, "MeshletTris", p.sceneMeshletTris ? p.sceneMeshletTris->GetDefaultView(D::BUFFER_VIEW_SHADER_RESOURCE) : nullptr);
	bindSrv(D::SHADER_TYPE_MESH, "ScenePositions", p.scenePos ? p.scenePos->GetDefaultView(D::BUFFER_VIEW_SHADER_RESOURCE) : nullptr);
	bindSrv(D::SHADER_TYPE_MESH, "SceneNormals", p.sceneNorm ? p.sceneNorm->GetDefaultView(D::BUFFER_VIEW_SHADER_RESOURCE) : nullptr);
	if (auto* v = p.sceneSrb->GetVariableByName(D::SHADER_TYPE_MESH, "cbSceneConstants")) v->Set(p.sceneCB);
	if (auto* v = p.sceneSrb->GetVariableByName(D::SHADER_TYPE_PIXEL, "cbSceneConstants")) v->Set(p.sceneCB);

	return {};
}

// ===================================================================
// M4: cluster-LOD pipeline (meshoptimizer clusterlod) - lazily created.
// ===================================================================
Result<void, RenderError> MeshShaderSubsystem::ensureClusterPipeline(Impl& p, UInt32 sampleCount) {
	auto* dev = p.renderer->getDevice() ? static_cast<D::IRenderDevice*>(p.renderer->getDevice()) : nullptr;
	auto* ctx = p.renderer->getContext() ? static_cast<D::IDeviceContext*>(p.renderer->getContext()) : nullptr;
	if (!dev || !ctx) return RenderError::NotInitialized;
	if (p.clusterPso && p.clusterPsoSampleCount == sampleCount) return {};
	p.clusterPso.Release(); p.clusterSrb.Release();
	p.clusterGBufferPso.Release(); p.clusterGBufferSrb.Release();

	auto makeShader = [&](D::SHADER_TYPE type, const char* name, const char* src) -> D::RefCntAutoPtr<D::IShader> {
		D::ShaderCreateInfo ci;
		ci.Desc.ShaderType = type;
		ci.Desc.Name = name;
		ci.EntryPoint = "main";
		ci.SourceLanguage = D::SHADER_SOURCE_LANGUAGE_HLSL;
		ci.ShaderCompiler = D::SHADER_COMPILER_DXC;
		ci.HLSLVersion = { 6, 5 };
		ci.Source = src;
		ci.SourceLength = (D::Uint32)strlen(src);
		D::RefCntAutoPtr<D::IShader> s;
		dev->CreateShader(ci, &s);
		if (!s || s->GetStatus() != D::SHADER_STATUS_READY) { EError("MeshShader: failed to compile {}", name); return {}; }
		return s;
	};
	auto as = makeShader(D::SHADER_TYPE_AMPLIFICATION, "Cluster MS AS", g_ClusterAS);
	auto ms = makeShader(D::SHADER_TYPE_MESH, "Cluster MS MS", g_ClusterMS);
	auto ps = makeShader(D::SHADER_TYPE_PIXEL, "Cluster MS PS", g_ClusterPS);
	auto psG = makeShader(D::SHADER_TYPE_PIXEL, "Cluster MS GBuffer PS", g_ClusterGBufferPS);
	if (!as || !ms || !ps || !psG) return RenderError::ShaderCompilationFailed;

	// Shared PSO state; the shaded and the G-buffer variant differ only in their
	// pixel shader and render targets.
	auto makeClusterPso = [&](const char* name, D::IShader* pixelShader, UInt32 numRTs,
		bool gbuffer, D::RefCntAutoPtr<D::IPipelineState>& outPso,
		D::RefCntAutoPtr<D::IShaderResourceBinding>& outSrb) -> Result<void, RenderError> {
		D::GraphicsPipelineStateCreateInfo ci;
		ci.PSODesc.Name = name;
		ci.PSODesc.PipelineType = D::PIPELINE_TYPE_MESH;
		ci.PSODesc.ResourceLayout.DefaultVariableType = D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
		ci.GraphicsPipeline.PrimitiveTopology = D::PRIMITIVE_TOPOLOGY_UNDEFINED;
		ci.GraphicsPipeline.NumRenderTargets = numRTs;
		// The shaded pass writes the HDR target; the hybrid RT path writes the
		// G-buffer (8-bit sRGB albedo + two 16F targets), matching the engine.
		ci.GraphicsPipeline.RTVFormats[0] = gbuffer ? D::TEX_FORMAT_RGBA8_UNORM_SRGB : D::TEX_FORMAT_RGBA16_FLOAT;
		if (gbuffer) {
			ci.GraphicsPipeline.RTVFormats[1] = D::TEX_FORMAT_RGBA16_FLOAT;
			ci.GraphicsPipeline.RTVFormats[2] = D::TEX_FORMAT_RGBA16_FLOAT;
		}
		ci.GraphicsPipeline.DSVFormat = D::TEX_FORMAT_D32_FLOAT;
		ci.GraphicsPipeline.SmplDesc.Count = static_cast<D::Uint8>(sampleCount);
		ci.GraphicsPipeline.RasterizerDesc.CullMode = D::CULL_MODE_BACK;
		ci.GraphicsPipeline.RasterizerDesc.FillMode = D::FILL_MODE_SOLID;
		// Must match the engine's front-face convention (CCW), see RenderTypes.hpp.
		ci.GraphicsPipeline.RasterizerDesc.FrontCounterClockwise = true;
		ci.GraphicsPipeline.DepthStencilDesc.DepthEnable = true;
		ci.GraphicsPipeline.DepthStencilDesc.DepthWriteEnable = true;
		ci.GraphicsPipeline.DepthStencilDesc.DepthFunc = D::COMPARISON_FUNC_LESS;
		ci.pAS = as; ci.pMS = ms; ci.pPS = pixelShader;
		dev->CreateGraphicsPipelineState(ci, &outPso);
		if (!outPso) { EError("MeshShader: failed to create the cluster-LOD pipeline '{}'", name); return RenderError::PipelineStateCreationFailed; }
		outPso->CreateShaderResourceBinding(&outSrb, true);
		if (!outSrb) { outPso.Release(); return RenderError::PipelineStateCreationFailed; }
		return {};
	};

	auto pr = makeClusterPso("Mesh Shader Cluster LOD", ps, 1, false, p.clusterPso, p.clusterSrb);
	if (pr.isErr()) return pr.error();
	auto gr = makeClusterPso("Mesh Shader Cluster LOD (G-buffer)", psG, 3, true, p.clusterGBufferPso, p.clusterGBufferSrb);
	if (gr.isErr()) return gr.error();
	p.clusterPsoSampleCount = static_cast<UInt8>(sampleCount);

	// Shared frame buffers (identical to the ones the old scene path uses; the
	// cluster path is self-sufficient so it creates them when it runs alone).
	if (!p.sceneCB) {
		D::BufferDesc bd; bd.Name = "MS Scene CB"; bd.Size = 1024;
		bd.BindFlags = D::BIND_UNIFORM_BUFFER; bd.Usage = D::USAGE_DYNAMIC; bd.CPUAccessFlags = D::CPU_ACCESS_WRITE;
		dev->CreateBuffer(bd, nullptr, &p.sceneCB);
		if (!p.sceneCB) { p.clusterPso.Release(); return RenderError::BufferCreationFailed; }
	}
	if (!p.sceneInstances) {
		D::BufferDesc bd; bd.Name = "MS Scene Instances"; bd.Size = kSceneMaxInstances * sizeof(Mat4);
		bd.BindFlags = D::BIND_SHADER_RESOURCE; bd.Mode = D::BUFFER_MODE_STRUCTURED;
		bd.ElementByteStride = sizeof(Mat4); bd.Usage = D::USAGE_DEFAULT;
		dev->CreateBuffer(bd, nullptr, &p.sceneInstances);
		if (!p.sceneInstances) { p.clusterPso.Release(); return RenderError::BufferCreationFailed; }
	}
	if (!p.sceneStatsBuf) {
		D::BufferDesc bd; bd.Name = "MS Scene Statistics"; bd.Size = 16;
		bd.BindFlags = D::BIND_UNORDERED_ACCESS; bd.Mode = D::BUFFER_MODE_RAW; bd.Usage = D::USAGE_DEFAULT;
		dev->CreateBuffer(bd, nullptr, &p.sceneStatsBuf);
		if (!p.sceneStatsBuf) { p.clusterPso.Release(); return RenderError::BufferCreationFailed; }
	}
	if (!p.sceneStatsStaging) {
		D::BufferDesc bd; bd.Name = "MS Scene Statistics Staging"; bd.Size = 16 * kStatRingSize;
		bd.Usage = D::USAGE_STAGING; bd.CPUAccessFlags = D::CPU_ACCESS_READ;
		dev->CreateBuffer(bd, nullptr, &p.sceneStatsStaging);
	}
	if (!p.sceneStatsFence) {
		D::FenceDesc fd; fd.Name = "MS Scene fence";
		dev->CreateFence(fd, &p.sceneStatsFence);
	}

	auto srv = [](D::IBuffer* b) { return b ? b->GetDefaultView(D::BUFFER_VIEW_SHADER_RESOURCE) : nullptr; };
	auto uav = [](D::IBuffer* b) { return b ? b->GetDefaultView(D::BUFFER_VIEW_UNORDERED_ACCESS) : nullptr; };

	// The shaded and G-buffer pipelines share the amplification/mesh shaders and
	// every resource; only the pixel shader differs. Bind once per SRB.
	auto bindCluster = [&](D::IShaderResourceBinding* srb) {
		auto bindS = [&](D::SHADER_TYPE stage, const char* name, D::IDeviceObject* obj) {
			if (obj) if (auto* v = srb->GetVariableByName(stage, name)) v->Set(obj, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
		};

		// Amplification: task list, clusters, LOD groups, culling constants.
		bindS(D::SHADER_TYPE_AMPLIFICATION, "Tasks", srv(p.sceneTasks));
		bindS(D::SHADER_TYPE_AMPLIFICATION, "Instances", srv(p.sceneInstances));
		bindS(D::SHADER_TYPE_AMPLIFICATION, "MeshInfos", srv(p.clMeshInfo));
		bindS(D::SHADER_TYPE_AMPLIFICATION, "Clusters", srv(p.clClusters));
		bindS(D::SHADER_TYPE_AMPLIFICATION, "Groups", srv(p.clGroups));
		bindS(D::SHADER_TYPE_AMPLIFICATION, "GroupBounds", srv(p.clGroupBounds));
		bindS(D::SHADER_TYPE_AMPLIFICATION, "ClusterCounter", uav(p.clCounter));
		bindS(D::SHADER_TYPE_AMPLIFICATION, "SceneStats", uav(p.sceneStatsBuf));
		if (auto* v = srb->GetVariableByName(D::SHADER_TYPE_AMPLIFICATION, "cbSceneConstants")) v->Set(p.sceneCB);

		// Mesh: resolves one visible cluster per derived group.
		bindS(D::SHADER_TYPE_MESH, "Instances", srv(p.sceneInstances));
		bindS(D::SHADER_TYPE_MESH, "MeshInfos", srv(p.clMeshInfo));
		bindS(D::SHADER_TYPE_MESH, "Clusters", srv(p.clClusters));
		bindS(D::SHADER_TYPE_MESH, "ClusterVertexIds", srv(p.clClusterVerts));
		bindS(D::SHADER_TYPE_MESH, "ClusterTris", srv(p.clClusterTris));
		bindS(D::SHADER_TYPE_MESH, "ScenePositions", srv(p.clPos));
		bindS(D::SHADER_TYPE_MESH, "SceneNormals", srv(p.clNorm));
		bindS(D::SHADER_TYPE_MESH, "SceneUVs", srv(p.clUV));
		bindS(D::SHADER_TYPE_MESH, "Materials", srv(p.clMaterials));
		if (auto* v = srb->GetVariableByName(D::SHADER_TYPE_MESH, "cbSceneConstants")) v->Set(p.sceneCB);

		// Pixel: the material table and the four texture palettes (mutable arrays
		// - every slot of every array must be set, hence the neutral fallbacks).
		if (auto* v = srb->GetVariableByName(D::SHADER_TYPE_PIXEL, "cbSceneConstants")) v->Set(p.sceneCB);
		bindS(D::SHADER_TYPE_PIXEL, "Materials", srv(p.clMaterials));
		if (p.whiteTexSRV) {
			auto fixup = [&](std::vector<D::IDeviceObject*>& arr, D::IDeviceObject* fallback) {
				if (arr.size() != kMaxMaterials) arr.assign(kMaxMaterials, fallback);
			};
			D::IDeviceObject* const white = p.whiteTexSRV.RawPtr();
			fixup(p.materialSRVs, white);
			fixup(p.normalSRVs,   p.flatNormalSRV ? p.flatNormalSRV.RawPtr() : white);
			fixup(p.mrSRVs,       p.flatMRSRV     ? p.flatMRSRV.RawPtr()     : white);
			fixup(p.emissiveSRVs, white);

			auto bindArray = [&](const char* name, std::vector<D::IDeviceObject*>& arr) {
				if (arr.size() != kMaxMaterials) return;
				if (auto* v = srb->GetVariableByName(D::SHADER_TYPE_PIXEL, name))
					v->SetArray(arr.data(), 0, kMaxMaterials);
			};
			bindArray("g_Albedo", p.materialSRVs);
			bindArray("g_NormalMap", p.normalSRVs);
			bindArray("g_MetalRough", p.mrSRVs);
			bindArray("g_EmissiveMap", p.emissiveSRVs);
		}
		if (auto* v = srb->GetVariableByName(D::SHADER_TYPE_PIXEL, "g_AlbedoSampler"))
			if (p.materialSampler) v->Set(p.materialSampler);

		// Shadows (only the raster pixel shader declares these; the G-buffer
		// variant leaves the light transport to the RT compose pass).
		{
			D::ITextureView* srv = p.shadowValid && p.shadowMapSRV ? p.shadowMapSRV.RawPtr()
				: (p.shadowDummySRV ? p.shadowDummySRV.RawPtr() : nullptr);
			if (srv) if (auto* v = srb->GetVariableByName(D::SHADER_TYPE_PIXEL, "g_ShadowMap"))
				v->Set(srv, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
			if (auto* v = srb->GetVariableByName(D::SHADER_TYPE_PIXEL, "g_ShadowMapSampler"))
				if (p.shadowSampler) v->Set(p.shadowSampler);
		}
	};

	bindCluster(p.clusterSrb);
	bindCluster(p.clusterGBufferSrb);

	return {};
}

// ===================================================================
// Register meshes: CPU meshletization + pool uploads.
// ===================================================================
Result<void, RenderError> MeshShaderSubsystem::setMeshes(const Vector<MeshHandle>& meshes) {
	auto& p = *m_impl;
	if (!p.ok) return RenderError::OperationFailed;
	auto* dev = p.renderer->getDevice() ? static_cast<D::IRenderDevice*>(p.renderer->getDevice()) : nullptr;
	if (!dev) return RenderError::NotInitialized;
	if (meshes.empty()) return RenderError::InvalidArgument;

	// Release previous geometry pools.
	//
	// Every pool that setMeshes recreates has to be released here: the call runs
	// again whenever the mesh set changes, and Diligent asserts when a reference
	// that is already set is overwritten ("Overwriting reference to existing
	// object may cause memory leaks"). The cluster pools are created further down
	// by createImmutable(), so they must be dropped here as well - a re-entrant
	// call (the Demo re-registers once the asynchronously loaded model arrives)
	// otherwise trips that assert on "MS ClPos".
	p.scenePos.Release(); p.sceneNorm.Release(); p.sceneMeshlets.Release();
	p.sceneMeshletVerts.Release(); p.sceneMeshletTris.Release(); p.sceneMeshInfo.Release(); p.sceneTasks.Release();
	p.clPos.Release(); p.clNorm.Release(); p.clUV.Release();
	p.clClusters.Release(); p.clClusterVerts.Release(); p.clClusterTris.Release();
	p.clGroups.Release(); p.clMeshInfo.Release(); p.clMaterials.Release();
	p.sceneMeshList = meshes;
	p.sceneMeshCount = (UInt32)meshes.size();

	// ---- CPU meshletization over the concatenated pools ----
	std::vector<Vec4> poolPos, poolNorm;
	std::vector<SceneMeshlet> poolMeshlets;
	std::vector<UInt32> poolMeshletVerts, poolMeshletTris;
	std::vector<SceneMeshInfo> poolInfo;

	static const Vec4 meshColors[6] = {
		Vec4(0.92f, 0.90f, 0.88f, 1), Vec4(0.85f, 0.70f, 0.55f, 1), Vec4(0.60f, 0.80f, 0.95f, 1),
		Vec4(0.90f, 0.85f, 0.65f, 1), Vec4(0.75f, 0.55f, 0.85f, 1), Vec4(0.65f, 0.90f, 0.75f, 1),
	};

	// Whole-model bounds (all meshes of the model are instanced together, so
	// culling must use the model sphere, not the individual mesh sphere).
	Vec3 bodyMin(1e30f), bodyMax(-1e30f);

	for (size_t mi = 0; mi < meshes.size(); ++mi) {
		const auto* verts = p.renderer->getMeshVertices(meshes[mi]);
		const auto* idxs  = p.renderer->getMeshIndices(meshes[mi]);
		if (!verts || !idxs || verts->empty() || idxs->empty()) return RenderError::InvalidArgument;
		const UInt32 nVerts = (UInt32)verts->size();

		// LOD0 bounding sphere.
		Vec3 minA(1e30f), maxA(-1e30f);
		for (const auto& v : *verts) { minA = glm::min(minA, v.position); maxA = glm::max(maxA, v.position); }
		const Vec3 sphereC = (minA + maxA) * 0.5f;
		float sphereR = 0;
		for (const auto& v : *verts) sphereR = std::max(sphereR, glm::length(v.position - sphereC));
		bodyMin = glm::min(bodyMin, minA);
		bodyMax = glm::max(bodyMax, maxA);

		// Greedy meshletizer for one LOD geometry (vertices already pooled at vBase).
		auto addLod = [&](UInt32 vBase, UInt32 nV, const std::vector<UInt32>& srcIdx, UInt32& outStart, UInt32& outCount) {
			outStart = (UInt32)poolMeshlets.size();
			outCount = 0;
			const UInt32 triTotal = (UInt32)(srcIdx.size() / 3);
			if (triTotal == 0 || nV == 0) return;
			std::vector<UInt32> mlVertsGlobal;
			std::vector<std::array<UInt32, 3>> mlTris;
			std::vector<UInt32> seen(nV, 0xFFFFFFFF);
			auto flush = [&]() {
				if (mlTris.empty()) return;
				SceneMeshlet out{};
				out.vertsOffset = (UInt32)poolMeshletVerts.size();
				out.vertsCount = (UInt32)mlVertsGlobal.size();
				out.trisOffset = (UInt32)poolMeshletTris.size();
				out.triCount = (UInt32)mlTris.size();
				poolMeshletVerts.insert(poolMeshletVerts.end(), mlVertsGlobal.begin(), mlVertsGlobal.end());
				for (const auto& t : mlTris) { poolMeshletTris.push_back(t[0]); poolMeshletTris.push_back(t[1]); poolMeshletTris.push_back(t[2]); }
				Vec3 c(0); for (UInt32 gi : mlVertsGlobal) c += Vec3(poolPos[gi]); c /= (float)mlVertsGlobal.size();
				float r = 0; for (UInt32 gi : mlVertsGlobal) r = std::max(r, glm::length(Vec3(poolPos[gi]) - c));
				out.center = Vec4(c, 1); out.radius = r;
				poolMeshlets.push_back(out);
				mlVertsGlobal.clear(); mlTris.clear();
				for (auto& s : seen) s = 0xFFFFFFFF;
			};
			auto localOf = [&](UInt32 g) -> UInt32 {
				if (seen[g] == 0xFFFFFFFF) { seen[g] = (UInt32)mlVertsGlobal.size(); mlVertsGlobal.push_back(vBase + g); }
				return seen[g];
			};
			for (UInt32 t = 0; t < triTotal; ++t) {
				const UInt32 i0 = srcIdx[t * 3 + 0], i1 = srcIdx[t * 3 + 1], i2 = srcIdx[t * 3 + 2];
				if (i0 >= nV || i1 >= nV || i2 >= nV) continue;
				bool overflow = (mlTris.size() >= kMeshletMaxTris);
				if (!overflow) {
					// A triangle may introduce up to 3 new vertices, so the check
					// must account for the ones that would actually be added -
					// otherwise the meshlet can exceed kMeshletMaxVerts (which is
					// also the mesh shader's output/thread limit).
					UInt32 fresh = (seen[i0] == 0xFFFFFFFF) ? 1u : 0u;
					if (i1 != i0 && seen[i1] == 0xFFFFFFFF) ++fresh;
					if (i2 != i0 && i2 != i1 && seen[i2] == 0xFFFFFFFF) ++fresh;
					if (mlVertsGlobal.size() + fresh > kMeshletMaxVerts) overflow = true;
				}
				if (overflow) flush();
				mlTris.push_back({ localOf(i0), localOf(i1), localOf(i2) });
			}
			flush();
			outCount = (UInt32)poolMeshlets.size() - outStart;
		};
		auto radiusOfRange = [&](size_t from, size_t count, const Vec3& c) {
			float r = 0;
			for (size_t i = from; i < from + count; ++i) r = std::max(r, glm::length(Vec3(poolPos[i]) - c));
			return r;
		};

		SceneMeshInfo info{};
		info.color = meshColors[mi % 6];
		info.lodCount = 3;
		info.center = Vec4(sphereC, 1);
		info.radius = sphereR;
		info.pad = 0;

		// LOD0: original geometry.
		{
			const UInt32 vBase = (UInt32)poolPos.size();
			for (const auto& v : *verts) { poolPos.push_back(Vec4(v.position, 1.0f)); poolNorm.push_back(Vec4(v.normal, 0.0f)); }
			std::vector<UInt32> srcIdx(idxs->begin(), idxs->end());
			addLod(vBase, nVerts, srcIdx, info.meshletBase[0], info.meshletCount[0]);
			info.vertBase[0] = vBase;
			info.lodRadius[0] = radiusOfRange(vBase, nVerts, sphereC);
		}
		// LOD1 / LOD2: vertex-clustering simplification (approx 55% / 30% of the vertices).
		for (int li = 1; li <= 2; ++li) {
			std::vector<Vec4> sPos, sNorm; std::vector<UInt32> sIdx;
			buildClusteredLOD(*verts, *idxs, (li == 1) ? 0.55f : 0.30f, sPos, sNorm, sIdx);
			if (sIdx.size() >= 3 && !sPos.empty()) {
				const UInt32 vBase = (UInt32)poolPos.size();
				poolPos.insert(poolPos.end(), sPos.begin(), sPos.end());
				poolNorm.insert(poolNorm.end(), sNorm.begin(), sNorm.end());
				addLod(vBase, (UInt32)sPos.size(), sIdx, info.meshletBase[li], info.meshletCount[li]);
				info.vertBase[li] = vBase;
				info.lodRadius[li] = radiusOfRange(vBase, sPos.size(), sphereC);
			} else {
				info.meshletBase[li] = (UInt32)poolMeshlets.size();
				info.meshletCount[li] = 0;
			}
		}
		poolInfo.push_back(info);
	}

	// ---- Self-check: catch meshlet pool inconsistencies before they reach the GPU
	// (an out-of-range vertex/primitive index in a mesh shader can hang the device).
	{
		bool ok = true;
		for (size_t i = 0; i < poolMeshlets.size() && ok; ++i) {
			const auto& ml = poolMeshlets[i];
			if ((size_t)ml.vertsOffset + ml.vertsCount > poolMeshletVerts.size()) {
				EError("MeshShader: meshlet {} vertex range out of bounds ({}+{} > {})", i, ml.vertsOffset, ml.vertsCount, poolMeshletVerts.size()); ok = false; break;
			}
			if ((size_t)ml.trisOffset + (size_t)ml.triCount * 3 > poolMeshletTris.size()) {
				EError("MeshShader: meshlet {} primitive range out of bounds ({}+{} > {})", i, ml.trisOffset, ml.triCount * 3, poolMeshletTris.size()); ok = false; break;
			}
			for (UInt32 v = 0; v < ml.vertsCount && ok; ++v) {
				if (poolMeshletVerts[ml.vertsOffset + v] >= poolPos.size()) {
					EError("MeshShader: meshlet {} vertex index {} out of pool range ({})", i, v, poolMeshletVerts[ml.vertsOffset + v]); ok = false; break;
				}
			}
			for (UInt32 t = 0; t < ml.triCount * 3 && ok; ++t) {
				if (poolMeshletTris[ml.trisOffset + t] >= ml.vertsCount) {
					EError("MeshShader: meshlet {} local index {} >= vertsCount {}", i, poolMeshletTris[ml.trisOffset + t], ml.vertsCount); ok = false; break;
				}
			}
			if (ml.vertsCount > kMeshletMaxVerts || ml.triCount > kMeshletMaxTris) {
				EError("MeshShader: meshlet {} exceeds the output limits ({} verts, {} tris)", i, ml.vertsCount, ml.triCount); ok = false; break;
			}
		}
		for (size_t m = 0; m < poolInfo.size() && ok; ++m) {
			const auto& in = poolInfo[m];
			for (int l = 0; l < 3 && ok; ++l) {
				if ((size_t)in.meshletBase[l] + in.meshletCount[l] > poolMeshlets.size()) {
					EError("MeshShader: mesh {} lod {} meshlet range out of bounds ({}+{} > {})", m, l, in.meshletBase[l], in.meshletCount[l], poolMeshlets.size()); ok = false; break;
				}
				if (in.vertBase[l] > poolPos.size()) {
					EError("MeshShader: mesh {} lod {} vertex base out of bounds ({})", m, l, in.vertBase[l]); ok = false; break;
				}
			}
		}
		if (ok) EInfo("MeshShader: meshlet pool self-check passed ({} meshlets, {} primitives, {} pooled vertices)",
			poolMeshlets.size(), poolMeshletTris.size() / 3, poolPos.size());
		for (size_t m = 0; m < poolInfo.size(); ++m) {
			const auto& in = poolInfo[m];
			EInfo("MeshShader: mesh {} -> lod0 {} meshlets / lod1 {} / lod2 {}",
				m, in.meshletCount[0], in.meshletCount[1], in.meshletCount[2]);
			EInfo("MeshShader: mesh {} vertBase {}/{}/{} meshletBase {}/{}/{} center ({:.2f},{:.2f},{:.2f}) r={:.3f}",
				m, in.vertBase[0], in.vertBase[1], in.vertBase[2],
				in.meshletBase[0], in.meshletBase[1], in.meshletBase[2],
				in.center.x, in.center.y, in.center.z, in.radius);
		}
		p.bodyCenter = (bodyMin + bodyMax) * 0.5f;
		float br = 0;
		for (const auto& v : *p.renderer->getMeshVertices(meshes[0]))
			br = std::max(br, glm::length(v.position - p.bodyCenter));
		for (size_t m = 1; m < meshes.size(); ++m) {
			const auto* vs = p.renderer->getMeshVertices(meshes[m]);
			if (!vs) continue;
			for (const auto& v : *vs) br = std::max(br, glm::length(v.position - p.bodyCenter));
		}
		p.bodyRadius = std::max(br, 0.01f);
		EInfo("MeshShader: model sphere center=({:.2f}, {:.2f}, {:.2f}) radius={:.2f}",
			p.bodyCenter.x, p.bodyCenter.y, p.bodyCenter.z, p.bodyRadius);
	}

	// ---- M4 step 1: build the meshoptimizer cluster-LOD hierarchy (CPU only
	// for now - the GPU path still uses the meshlet pools above). This gives us
	// proper error-based clusters with bounds; the rendering path will switch to
	// these clusters next.
	std::vector<ClusterGPU> clClusters;
	std::vector<ClusterGroupGPU> clGroups;
	std::vector<UInt32> clClusterVerts, clClusterTris;
	std::vector<ClusterMeshInfo> clMeshInfo;
	std::vector<Vec4> clPos; // pooled original vertices (positions)
	std::vector<Vec4> clNorm;
	std::vector<Vec4> clUV;  // pooled texture coordinates (xy)

	// ---- Material palette -------------------------------------------------
	// One slot per distinct (albedo texture, base colour) pair. A glTF mesh is
	// merged from all of its primitives, so one mesh can carry several
	// sub-meshes with different materials; each sub-mesh owns a contiguous range
	// of the mesh's vertices, which is what lets a cluster pick the material of
	// the region it was built from.
	struct MatSlot  { MaterialHandle mat; MaterialDesc desc; bool valid = false; };
	struct SubRange { UInt32 vBegin, vEnd, mat; };
	std::vector<MatSlot> matSlots;
	std::vector<std::vector<SubRange>> meshSubs(meshes.size());
	{
		// Slot 0 is the white fallback so an out-of-range id is always safe.
		matSlots.push_back(MatSlot{});
		// Materials are deduplicated by their handle (identical materials share
		// one handle), so a model with many meshes but few materials uses few
		// palette slots and few texture descriptors.
		auto paletteId = [&](MaterialHandle handle, const MaterialDesc& md) -> UInt32 {
			for (UInt32 i = 0; i < (UInt32)matSlots.size(); ++i)
				if (matSlots[i].valid && matSlots[i].mat == handle) return i;
			if (matSlots.size() >= kMaxMaterials) {
				EWarn("MeshShader: material palette is full ({} slots); extra materials use the fallback", kMaxMaterials);
				return 0;
			}
			MatSlot s; s.mat = handle; s.desc = md; s.valid = true;
			matSlots.push_back(s);
			return (UInt32)matSlots.size() - 1;
		};

		size_t rangeCount = 0;
		for (size_t mi = 0; mi < meshes.size(); ++mi) {
			const auto* verts = p.renderer->getMeshVertices(meshes[mi]);
			const UInt32 totalVerts = verts ? (UInt32)verts->size() : 0u;

			std::vector<SubMesh> subs;
			for (UInt32 s = 0; s < 32; ++s) {
				const SubMesh sm = p.renderer->getSubMesh(meshes[mi], s);
				if (sm.indexCount == 0) break;
				subs.push_back(sm);
			}
			if (subs.size() == 32) EWarn("MeshShader: mesh {} has 32+ sub-meshes; the extras are ignored", mi);
			if (subs.empty()) {
				EWarn("MeshShader: mesh {} reports no sub-mesh (material/tint only)", mi);
				continue;
			}
			auto& ranges = meshSubs[mi];
			for (size_t s = 0; s < subs.size(); ++s) {
				UInt32 mat = 0;
				if (subs[s].material.isValid()) {
					if (const auto md = p.renderer->getMaterial(subs[s].material)) {
						mat = paletteId(subs[s].material, *md);
					}
				}
				const UInt32 vBegin = subs[s].vertexOffset;
				const UInt32 vEnd   = (s + 1 < subs.size()) ? subs[s + 1].vertexOffset : totalVerts;
				if (vEnd > vBegin) { ranges.push_back(SubRange{ vBegin, vEnd, mat }); ++rangeCount; }
			}
		}
		EInfo("MeshShader: {} material slot(s), {} sub-mesh range(s) over {} meshes",
			matSlots.size(), rangeCount, meshes.size());
	}

	// GPU-side material table, padded to the full palette so an out-of-range id
	// still reads a valid (white) entry.
	std::vector<MaterialGPU> clMaterials(kMaxMaterials);
	for (size_t i = 0; i < matSlots.size() && i < kMaxMaterials; ++i) {
		if (!matSlots[i].valid) continue;
		const MaterialDesc& md = matSlots[i].desc;
		MaterialGPU& g = clMaterials[i];
		g.baseColor = md.baseColorFactor;
		// glTF: emission = emissiveFactor * emissiveTexture. The emissive map's
		// fallback is white (the multiplicative identity) rather than black, so a
		// material that carries only a factor - the Demo's lamp cube - still
		// glows; this matches the forward path's `emissive = emissiveFactor`.
		g.emissive = Vec4(md.emissiveFactor, 1.0f);
		g.params = Vec4(
			md.metallicFactor,
			md.roughnessFactor,
			md.normalTexture.isValid() ? 1.0f : 0.0f,
			md.metallicRoughnessTexture.isValid() ? 1.0f : 0.0f);
	}
	{
		for (size_t mi = 0; mi < meshes.size(); ++mi) {
			const auto* verts = p.renderer->getMeshVertices(meshes[mi]);
			const auto* idxs  = p.renderer->getMeshIndices(meshes[mi]);
			if (!verts || !idxs || verts->empty() || idxs->empty()) continue;

			const UInt32 vertBase = (UInt32)clPos.size();
			for (const auto& v : *verts) {
				clPos.push_back(Vec4(v.position, 1.0f));
				clNorm.push_back(Vec4(v.normal, 0.0f));
				clUV.push_back(Vec4(v.texCoord, 0.0f, 0.0f));
			}

			std::vector<float> positions(verts->size() * 3);
			for (size_t i = 0; i < verts->size(); ++i) {
				positions[i * 3 + 0] = (*verts)[i].position.x;
				positions[i * 3 + 1] = (*verts)[i].position.y;
				positions[i * 3 + 2] = (*verts)[i].position.z;
			}
			clodMesh cm{};
			cm.indices = idxs->data();
			cm.index_count = idxs->size();
			cm.vertex_count = verts->size();
			cm.vertex_positions = positions.data();
			cm.vertex_positions_stride = sizeof(float) * 3;

			const clodConfig cfg = clodDefaultConfig(128);

			const UInt32 clusterBase = (UInt32)clClusters.size();
			size_t clusters = 0, maxVerts = 0, maxTris = 0, groupCount = 0;
			size_t droppedClusters = 0;
			int maxDepth = 0;
			std::vector<unsigned int> localVerts;
			std::vector<unsigned char> localTris;
			clodBuild(cfg, cm, [&](clodGroup group, const clodCluster* cl, size_t count) -> int {
				maxDepth = std::max(maxDepth, group.depth);
				const int groupId = (int)clGroups.size();
				ClusterGroupGPU g{};
				g.center = Vec4(group.simplified.center[0], group.simplified.center[1], group.simplified.center[2], 1.0f);
				g.radius = group.simplified.radius;
				g.error  = group.simplified.error;
				g.depth  = group.depth;
				g.pad    = 0;
				clGroups.push_back(g);

				for (size_t i = 0; i < count; ++i) {
					const clodCluster& c = cl[i];
					localVerts.resize(c.vertex_count);
					localTris.resize(c.index_count);
					const size_t vc = clodLocalIndices(localVerts.data(), localTris.data(), c.indices, c.index_count);

					// Keep only the triangles the mesh shader can actually emit,
					// and derive the vertex count from the *kept* indices. This
					// guarantees every emitted index is below the vertex count
					// passed to SetMeshOutputCounts - an index above it makes the
					// rasterizer read unwritten output vertices (garbage triangles
					// followed by device removal / TDR).
					size_t keep = 0;
					UInt32 maxLocal = 0;
					const size_t triTotal = c.index_count / 3;
					for (size_t t = 0; t < triTotal; ++t) {
						const UInt32 a = localTris[t * 3 + 0];
						const UInt32 b = localTris[t * 3 + 1];
						const UInt32 d = localTris[t * 3 + 2];
						if (t >= kClusterMaxTris) break;
						const UInt32 m = std::max(a, std::max(b, d));
						if (m >= kClusterMaxVerts) break;
						maxLocal = std::max(maxLocal, m);
						++keep;
					}
					if (keep < triTotal) ++droppedClusters;

					ClusterGPU out{};
					out.vertexIdsOffset = (UInt32)clClusterVerts.size();
					out.vertexCount     = maxLocal + 1;   // == 0 when the cluster emitted nothing
					out.triOffset       = (UInt32)clClusterTris.size();
					out.triCount        = (UInt32)keep;
					out.center          = Vec4(c.bounds.center[0], c.bounds.center[1], c.bounds.center[2], 1.0f);
					out.radius          = c.bounds.radius;
					out.error           = c.bounds.error;
					out.groupId         = groupId;
					out.refinedGroup    = c.refined;
					out.meshVertBase    = vertBase;
					out.meshId          = (UInt32)mi;

					// Material of the sub-mesh this cluster mostly came from. A
					// cluster can straddle a sub-mesh seam (and coarse LOD levels
					// mix several), so take the majority of its referenced
					// vertices rather than the first one.
					{
						const auto& ranges = meshSubs[mi];
						UInt32 votes[32] = {};
						for (UInt32 v = 0; v < out.vertexCount; ++v) {
							const UInt32 gv0 = localVerts[v];
							for (size_t r = 0; r < ranges.size() && r < 32; ++r) {
								if (gv0 >= ranges[r].vBegin && gv0 < ranges[r].vEnd) { ++votes[r]; break; }
							}
						}
						UInt32 best = 0, bestVotes = 0;
						for (size_t r = 0; r < ranges.size() && r < 32; ++r) {
							if (votes[r] > bestVotes) { bestVotes = votes[r]; best = ranges[r].mat; }
						}
						out.materialId = best;
					}
					out.pad1 = 0;
					clClusterVerts.insert(clClusterVerts.end(), localVerts.begin(), localVerts.end());
					for (size_t t = 0; t < keep * 3; ++t) clClusterTris.push_back(localTris[t]);
					clClusters.push_back(out);

					maxVerts = std::max(maxVerts, vc);
					maxTris  = std::max(maxTris, triTotal);
					++clusters;
				}
				++groupCount;
				return groupId; // referenced by coarser clusters as 'refined'
			});

			ClusterMeshInfo info{};
			info.center = Vec4(poolInfo[mi].center);
			info.radius = poolInfo[mi].radius;
			info.vertBase = vertBase;
			info.clusterBase = clusterBase;
			info.clusterCount = (UInt32)clusters;
			// Kept for the legacy meshlet path / debugging; the cluster pixel
			// shader takes its tint from the per-cluster material table instead.
			info.color = Vec4(1.0f);
			clMeshInfo.push_back(info);

			EInfo("MeshShader: mesh {} clusterlod -> {} clusters ({} groups, depth {}, maxVerts {}, maxTris {}, truncated {})",
				mi, clusters, groupCount, maxDepth + 1, maxVerts, maxTris, droppedClusters);
			if (droppedClusters > 0) {
				EWarn("MeshShader: mesh {} has {} cluster(s) exceeding the mesh shader output budget "
					"({} verts / {} prims) - those triangles are dropped",
					mi, droppedClusters, kClusterMaxVerts, kClusterMaxTris);
			}
			if (clusters > kClusterPerGroup) {
				// The payload list is fixed-size, so a single (mesh, instance)
				// task cannot dispatch more than this many clusters.
				EError("MeshShader: mesh {} has {} clusters but a single task can only "
					"dispatch {} - the excess is dropped (raise kClusterPerGroup)",
					mi, clusters, kClusterPerGroup);
			}
		}
		EInfo("MeshShader: clusterlod total {} clusters / {} groups", clClusters.size(), clGroups.size());
	}

	// ---- Self-check the cluster pools. An out-of-range vertex/primitive index
	// reaching the mesh shader shows up as garbage triangles and can remove the
	// device, so it is validated on the CPU first (mirrors the meshlet check).
	{
		bool ok = true;
		for (size_t i = 0; i < clClusters.size() && ok; ++i) {
			const auto& cl = clClusters[i];
			if (cl.vertexCount > kClusterMaxVerts || cl.triCount > kClusterMaxTris) {
				EError("MeshShader: cluster {} exceeds the mesh shader output limits ({} verts, {} prims)",
					i, cl.vertexCount, cl.triCount); ok = false; break;
			}
			if ((size_t)cl.vertexIdsOffset + cl.vertexCount > clClusterVerts.size()) {
				EError("MeshShader: cluster {} vertex range out of bounds ({}+{} > {})",
					i, cl.vertexIdsOffset, cl.vertexCount, clClusterVerts.size()); ok = false; break;
			}
			if (cl.materialId >= kMaxMaterials) {
				EError("MeshShader: cluster {} material {} out of range ({})",
					i, cl.materialId, kMaxMaterials); ok = false; break;
			}
			if ((size_t)cl.triOffset + (size_t)cl.triCount * 3 > clClusterTris.size()) {
				EError("MeshShader: cluster {} primitive range out of bounds ({}+{} > {})",
					i, cl.triOffset, cl.triCount * 3, clClusterTris.size()); ok = false; break;
			}
			for (UInt32 v = 0; v < cl.vertexCount && ok; ++v) {
				const UInt32 gv = clClusterVerts[cl.vertexIdsOffset + v];
				if ((size_t)cl.meshVertBase + gv >= clPos.size()) {
					EError("MeshShader: cluster {} vertex {} out of the pooled range ({}+{} >= {})",
						i, v, cl.meshVertBase, gv, clPos.size()); ok = false; break;
				}
			}
			for (UInt32 t = 0; t < cl.triCount * 3 && ok; ++t) {
				if (clClusterTris[cl.triOffset + t] >= cl.vertexCount) {
					EError("MeshShader: cluster {} local index {} >= vertexCount {} (garbage triangles!)",
						i, clClusterTris[cl.triOffset + t], cl.vertexCount); ok = false; break;
				}
			}
			if (cl.groupId < 0 || (size_t)cl.groupId >= clGroups.size()) {
				EError("MeshShader: cluster {} group id {} out of range ({})", i, cl.groupId, clGroups.size()); ok = false; break;
			}
			if (cl.refinedGroup >= (int)clGroups.size()) {
				EError("MeshShader: cluster {} refined group {} out of range ({})", i, cl.refinedGroup, clGroups.size()); ok = false; break;
			}
		}
		if (ok) {
			EInfo("MeshShader: cluster pool self-check passed ({} clusters, {} primitives, {} cluster vertices, {} groups)",
				clClusters.size(), clClusterTris.size() / 3, clClusterVerts.size(), clGroups.size());
		} else {
			return RenderError::OperationFailed;
		}
	}

	// ---- Task list: rewritten every frame with the *live* (mesh x instance)
	// pairs only, so no empty amplification groups are ever launched. ----
	p.sceneTaskCapacity = (UInt32)(meshes.size() * kSceneMaxInstances);
	p.sceneTaskScratch.resize(p.sceneTaskCapacity);

	// ---- Upload pools (immutable) ----
	auto createImmutable = [&](const char* name, const void* data, UInt64 size, UInt32 stride, D::RefCntAutoPtr<D::IBuffer>& out) -> bool {
		// Never hand a set reference to CreateBuffer: Diligent treats that as an
		// overwrite and asserts on it in debug builds. Dropping it here keeps a
		// re-registration safe even if a caller forgets to release the pool.
		out.Release();
		if (size == 0 || !data) return true;
		D::BufferDesc bd; bd.Name = name; bd.Size = size;
		bd.BindFlags = D::BIND_SHADER_RESOURCE;
		if (stride) { bd.Mode = D::BUFFER_MODE_STRUCTURED; bd.ElementByteStride = stride; }
		bd.Usage = D::USAGE_IMMUTABLE;
		D::BufferData bdata; bdata.pData = data; bdata.DataSize = size;
		dev->CreateBuffer(bd, &bdata, &out);
		return out != nullptr;
	};
	if (!createImmutable("MS ScenePos", poolPos.data(), poolPos.size() * sizeof(Vec4), sizeof(Vec4), p.scenePos) ||
		!createImmutable("MS SceneNorm", poolNorm.data(), poolNorm.size() * sizeof(Vec4), sizeof(Vec4), p.sceneNorm) ||
		!createImmutable("MS SceneMeshlets", poolMeshlets.data(), poolMeshlets.size() * sizeof(SceneMeshlet), sizeof(SceneMeshlet), p.sceneMeshlets) ||
		!createImmutable("MS MeshletVerts", poolMeshletVerts.data(), poolMeshletVerts.size() * sizeof(UInt32), sizeof(UInt32), p.sceneMeshletVerts) ||
		!createImmutable("MS MeshletTris", poolMeshletTris.data(), poolMeshletTris.size() * sizeof(UInt32), sizeof(UInt32), p.sceneMeshletTris) ||
		!createImmutable("MS SceneMeshInfo", poolInfo.data(), poolInfo.size() * sizeof(SceneMeshInfo), sizeof(SceneMeshInfo), p.sceneMeshInfo)) {
		EError("MeshShader: failed to create scene pool buffers");
		return RenderError::BufferCreationFailed;
	}

	// ---- M4: cluster pools (meshoptimizer cluster LOD) ----
	if (!createImmutable("MS ClPos", clPos.data(), clPos.size() * sizeof(Vec4), sizeof(Vec4), p.clPos) ||
		!createImmutable("MS ClNorm", clNorm.data(), clNorm.size() * sizeof(Vec4), sizeof(Vec4), p.clNorm) ||
		!createImmutable("MS ClUV", clUV.data(), clUV.size() * sizeof(Vec4), sizeof(Vec4), p.clUV) ||
		!createImmutable("MS Clusters", clClusters.data(), clClusters.size() * sizeof(ClusterGPU), sizeof(ClusterGPU), p.clClusters) ||
		!createImmutable("MS ClusterVerts", clClusterVerts.data(), clClusterVerts.size() * sizeof(UInt32), sizeof(UInt32), p.clClusterVerts) ||
		!createImmutable("MS ClusterTris", clClusterTris.data(), clClusterTris.size() * sizeof(UInt32), sizeof(UInt32), p.clClusterTris) ||
		!createImmutable("MS ClusterGroups", clGroups.data(), clGroups.size() * sizeof(ClusterGroupGPU), sizeof(ClusterGroupGPU), p.clGroups) ||
		!createImmutable("MS ClusterMeshInfo", clMeshInfo.data(), clMeshInfo.size() * sizeof(ClusterMeshInfo), sizeof(ClusterMeshInfo), p.clMeshInfo) ||
		!createImmutable("MS ClusterMaterials", clMaterials.data(), clMaterials.size() * sizeof(MaterialGPU), sizeof(MaterialGPU), p.clMaterials)) {
		EError("MeshShader: failed to create cluster pool buffers");
		return RenderError::BufferCreationFailed;
	}

	// ---- M5: material textures ----
	{
		// 1x1 white fallback: every palette slot must have a valid SRV, because
		// Diligent refuses to commit an SRB with an unbound mutable array element.
		if (!p.whiteTex) {
			const UInt8 white[4] = { 255, 255, 255, 255 };
			D::TextureDesc td;
			td.Name = "MS White"; td.Type = D::RESOURCE_DIM_TEX_2D; td.Width = 1; td.Height = 1;
			td.MipLevels = 1; td.Format = D::TEX_FORMAT_RGBA8_UNORM;
			td.BindFlags = D::BIND_SHADER_RESOURCE; td.Usage = D::USAGE_IMMUTABLE;
			D::TextureSubResData sub; sub.pData = white; sub.Stride = sizeof(white);
			D::TextureData tdata(&sub, 1);
			dev->CreateTexture(td, &tdata, &p.whiteTex);
			if (!p.whiteTex) { EError("MeshShader: failed to create the white fallback texture"); return RenderError::TextureCreationFailed; }
			p.whiteTexSRV = p.whiteTex->GetDefaultView(D::TEXTURE_VIEW_SHADER_RESOURCE);
		}
		if (!p.materialSampler) {
			D::SamplerDesc sd;
			sd.MinFilter = D::FILTER_TYPE_LINEAR; sd.MagFilter = D::FILTER_TYPE_LINEAR; sd.MipFilter = D::FILTER_TYPE_LINEAR;
			sd.AddressU = D::TEXTURE_ADDRESS_WRAP; sd.AddressV = D::TEXTURE_ADDRESS_WRAP; sd.AddressW = D::TEXTURE_ADDRESS_WRAP;
			sd.MaxAnisotropy = 8;
			dev->CreateSampler(sd, &p.materialSampler);
			if (!p.materialSampler) { EError("MeshShader: failed to create the material sampler"); return RenderError::SamplerCreationFailed; }
		}
		// Shadow comparison sampler + a 1x1x4 "nothing occludes" fallback, so the
		// mutable shadow variables are always bound even without a shadow pass.
		if (!p.shadowSampler) {
			D::SamplerDesc sd;
			sd.MinFilter = D::FILTER_TYPE_COMPARISON_LINEAR; sd.MagFilter = D::FILTER_TYPE_COMPARISON_LINEAR;
			sd.MipFilter = D::FILTER_TYPE_COMPARISON_LINEAR;
			sd.AddressU = D::TEXTURE_ADDRESS_CLAMP; sd.AddressV = D::TEXTURE_ADDRESS_CLAMP; sd.AddressW = D::TEXTURE_ADDRESS_CLAMP;
			sd.ComparisonFunc = D::COMPARISON_FUNC_LESS;
			dev->CreateSampler(sd, &p.shadowSampler);
			if (!p.shadowSampler) { EError("MeshShader: failed to create the shadow sampler"); return RenderError::SamplerCreationFailed; }
		}
		if (!p.shadowDummyTex) {
			// Depth 1.0 in every slice => every comparison passes => fully lit.
			// (nb: not named `far`, which windef.h defines as an empty macro.)
			const UInt16 farDepth = 0xFFFF;
			D::TextureDesc td;
			td.Name = "MS ShadowDummy"; td.Type = D::RESOURCE_DIM_TEX_2D_ARRAY;
			td.Width = 1; td.Height = 1; td.ArraySize = 4; td.MipLevels = 1;
			td.Format = D::TEX_FORMAT_R16_UNORM;
			td.BindFlags = D::BIND_SHADER_RESOURCE; td.Usage = D::USAGE_IMMUTABLE;
			std::vector<D::TextureSubResData> sub(4);
			std::vector<UInt16> far4(4, farDepth);
			for (int i = 0; i < 4; ++i) { sub[i].pData = &far4[i]; sub[i].Stride = sizeof(UInt16); }
			D::TextureData tdata(sub.data(), 4);
			dev->CreateTexture(td, &tdata, &p.shadowDummyTex);
			if (p.shadowDummyTex) p.shadowDummySRV = p.shadowDummyTex->GetDefaultView(D::TEXTURE_VIEW_SHADER_RESOURCE);
		}

		// Neutral defaults are the multiplicative identity for each map, so the
		// same shader works with or without a texture:
		//   albedo / metalRough / emissive -> white (1,1,1)
		//   normal                         -> flat (0.5, 0.5, 1)
		// Emissive in particular must NOT default to black: emission is
		// emissiveFactor * emissiveTexture, and materials that carry only a
		// factor (the Demo's lamp cube) would otherwise lose their glow.
		p.materialSRVs.assign(kMaxMaterials, p.whiteTexSRV.RawPtr());
		p.normalSRVs.assign(kMaxMaterials, p.whiteTexSRV.RawPtr());
		p.mrSRVs.assign(kMaxMaterials, p.whiteTexSRV.RawPtr());
		p.emissiveSRVs.assign(kMaxMaterials, p.whiteTexSRV.RawPtr());
		p.materialCount = (UInt32)matSlots.size();

		auto mkFlat = [&](const UInt8 rgba[4], D::RefCntAutoPtr<D::ITexture>& tex, D::RefCntAutoPtr<D::ITextureView>& srv) {
			if (tex) return;
			D::TextureDesc td;
			td.Name = "MS Flat"; td.Type = D::RESOURCE_DIM_TEX_2D; td.Width = 1; td.Height = 1;
			td.MipLevels = 1; td.Format = D::TEX_FORMAT_RGBA8_UNORM;
			td.BindFlags = D::BIND_SHADER_RESOURCE; td.Usage = D::USAGE_IMMUTABLE;
			D::TextureSubResData sub; sub.pData = rgba; sub.Stride = 4;
			D::TextureData tdata(&sub, 1);
			dev->CreateTexture(td, &tdata, &tex);
			if (tex) srv = tex->GetDefaultView(D::TEXTURE_VIEW_SHADER_RESOURCE);
		};
		const UInt8 flatNormal[4] = { 128, 128, 255, 255 };
		const UInt8 flatMR[4]     = { 255, 255, 255, 255 }; // G = roughness 1, B = metallic 1
		mkFlat(flatNormal, p.flatNormalTex, p.flatNormalSRV);
		mkFlat(flatMR,     p.flatMRTex,     p.flatMRSRV);
		if (p.flatNormalSRV) p.normalSRVs.assign(kMaxMaterials, p.flatNormalSRV.RawPtr());
		if (p.flatMRSRV)     p.mrSRVs.assign(kMaxMaterials, p.flatMRSRV.RawPtr());

		UInt32 boundAlbedo = 0, boundNormal = 0, boundMR = 0, boundEmissive = 0;
		auto resolve = [&](TextureHandle h, std::vector<D::IDeviceObject*>& dst, UInt32 i, UInt32& counter) {
			if (!h.isValid()) return;
			if (auto* srv = static_cast<D::IDeviceObject*>(p.renderer->getTextureSRV(h))) { dst[i] = srv; ++counter; }
			else EWarn("MeshShader: material slot {} references a texture with no SRV.", i);
		};
		for (UInt32 i = 0; i < p.materialCount && i < kMaxMaterials; ++i) {
			if (!matSlots[i].valid) continue;           // slot 0 = fallback
			const MaterialDesc& md = matSlots[i].desc;
			resolve(md.baseColorTexture,          p.materialSRVs, i, boundAlbedo);
			resolve(md.normalTexture,             p.normalSRVs,   i, boundNormal);
			resolve(md.metallicRoughnessTexture,  p.mrSRVs,       i, boundMR);
			resolve(md.emissiveTexture,           p.emissiveSRVs, i, boundEmissive);
		}
		EInfo("MeshShader: {} material slot(s); textures bound - albedo {}, normal {}, metalRough {}, emissive {}",
			p.materialCount, boundAlbedo, boundNormal, boundMR, boundEmissive);
	}
	if (!p.clCounter) {
		D::BufferDesc bd; bd.Name = "MS ClusterCounter"; bd.Size = 16;
		bd.BindFlags = D::BIND_UNORDERED_ACCESS; bd.Mode = D::BUFFER_MODE_RAW; bd.Usage = D::USAGE_DEFAULT;
		dev->CreateBuffer(bd, nullptr, &p.clCounter);
		if (!p.clCounter) { EError("MeshShader: failed to create the cluster counter"); return RenderError::BufferCreationFailed; }
	}
	// Task list (one entry per mesh x instance, rewritten every frame).
	{
		p.sceneTasks.Release();
		D::BufferDesc bd; bd.Name = "MS Scene Tasks";
		bd.Size = (UInt64)p.sceneTaskCapacity * sizeof(SceneTask);
		bd.BindFlags = D::BIND_SHADER_RESOURCE; bd.Mode = D::BUFFER_MODE_STRUCTURED;
		bd.ElementByteStride = sizeof(SceneTask); bd.Usage = D::USAGE_DEFAULT;
		dev->CreateBuffer(bd, nullptr, &p.sceneTasks);
		if (!p.sceneTasks) { EError("MeshShader: failed to create the task buffer"); return RenderError::BufferCreationFailed; }
	}
	// Per-frame draw-group bounds (one entry per MeshDrawGroup).
	{
		p.clGroupBounds.Release();
		D::BufferDesc bd; bd.Name = "MS GroupBounds";
		bd.Size = (UInt64)kSceneMaxGroups * sizeof(MeshGroupGPU);
		bd.BindFlags = D::BIND_SHADER_RESOURCE; bd.Mode = D::BUFFER_MODE_STRUCTURED;
		bd.ElementByteStride = sizeof(MeshGroupGPU); bd.Usage = D::USAGE_DEFAULT;
		dev->CreateBuffer(bd, nullptr, &p.clGroupBounds);
		if (!p.clGroupBounds) { EError("MeshShader: failed to create the group bounds buffer"); return RenderError::BufferCreationFailed; }
	}
	// CPU copy of the per-mesh local bounds for building group spheres.
	p.meshBounds.clear();
	p.meshBounds.reserve(clMeshInfo.size());
	for (const auto& mi : clMeshInfo) p.meshBounds.push_back(Impl::MeshBound{ Vec3(mi.center), mi.radius });
	EInfo("MeshShader: cluster pools uploaded ({} clusters, {} cluster verts, {} indices)",
		clClusters.size(), clClusterVerts.size(), clClusterTris.size());
	EInfo("MeshShader: {} meshes registered -> {} pooled vertices, {} meshlets",
		poolInfo.size(), poolPos.size(), poolMeshlets.size());

	// The pools were just re-uploaded: force the (lazily created) cluster
	// pipeline/SRB to be rebuilt so it binds the new buffers.
	p.clusterPsoSampleCount = 0;
	return {};
}

// ===================================================================
// Draw every registered mesh once per instance (GPU-driven, one DrawMesh).
// Shared by the shaded (raster) and G-buffer (hybrid ray tracing) variants;
// they differ only in the pipeline state and the render targets the caller has
// already bound.
// ===================================================================
Result<void, RenderError> MeshShaderSubsystem::drawSceneImpl(const Vector<MeshDrawGroup>& groups,
	const Mat4& view, const Mat4& proj, F32 timeSec, bool gbuffer) {
	auto& p = *m_impl;
	if (!p.ok) return RenderError::OperationFailed;
	if (groups.empty() || groups.size() > kSceneMaxGroups) return RenderError::InvalidArgument;
	if (p.sceneMeshList.empty()) return RenderError::NotInitialized;
	auto* dev = p.renderer->getDevice() ? static_cast<D::IRenderDevice*>(p.renderer->getDevice()) : nullptr;
	auto* ctx = p.renderer->getContext() ? static_cast<D::IDeviceContext*>(p.renderer->getContext()) : nullptr;
	if (!dev || !ctx) return RenderError::NotInitialized;

	const UInt8 sampleCount = p.renderer->msaaSamples();
	auto r = ensureClusterPipeline(p, sampleCount);
	if (r.isErr()) return r.error();
	auto* pso = gbuffer ? p.clusterGBufferPso.RawPtr() : p.clusterPso.RawPtr();
	auto* srb = gbuffer ? p.clusterGBufferSrb.RawPtr() : p.clusterSrb.RawPtr();
	if (!pso || !srb) return RenderError::NotInitialized;

	// ---- Per-frame draw groups ------------------------------------------
	// Build one flat instance array (group instances concatenated) plus the
	// (mesh, instance, group) task list, so the whole scene is one DrawMesh.
	p.groupInstancesScratch.clear();
	p.groupBoundsScratch.clear();
	UInt32 taskCount = 0;
	for (const auto& g : groups) {
		if (g.meshIds.empty() || g.instanceMatrices.empty()) continue;
		for (UInt32 id : g.meshIds) if (id >= p.sceneMeshCount) return RenderError::InvalidArgument;
		taskCount += (UInt32)(g.meshIds.size() * g.instanceMatrices.size());
	}
	if (taskCount == 0) return RenderError::InvalidArgument;
	if (taskCount > p.sceneTaskCapacity) {
		EError("MeshShader: {} tasks exceed the task buffer capacity {}", taskCount, p.sceneTaskCapacity);
		return RenderError::InvalidArgument;
	}
	{
		SceneTask* dst = p.sceneTaskScratch.data();
		UInt32 k = 0, groupId = 0;
		for (const auto& g : groups) {
			if (g.meshIds.empty() || g.instanceMatrices.empty()) continue;
			if (p.groupInstancesScratch.size() + g.instanceMatrices.size() > kSceneMaxInstances) {
				EError("MeshShader: more than {} instances across all draw groups", kSceneMaxInstances);
				return RenderError::InvalidArgument;
			}
			const UInt32 instanceBase = (UInt32)p.groupInstancesScratch.size();
			p.groupInstancesScratch.insert(p.groupInstancesScratch.end(),
				g.instanceMatrices.begin(), g.instanceMatrices.end());

			// Group sphere: enclose the local bounds of every mesh in the group.
			Vec3 lo(1e30f), hi(-1e30f);
			for (UInt32 id : g.meshIds) {
				const auto& b = p.meshBounds[id];
				lo = glm::min(lo, b.center - Vec3(b.radius));
				hi = glm::max(hi, b.center + Vec3(b.radius));
			}
			MeshGroupGPU gb{};
			if (lo.x <= hi.x) {
				const Vec3 c = (lo + hi) * 0.5f;
				gb.center = Vec4(c, 1.0f);
				gb.radius = glm::length(hi - c);
			} else {
				gb.center = Vec4(p.bodyCenter, 1.0f);
				gb.radius = p.bodyRadius;
			}
			p.groupBoundsScratch.push_back(gb);

			for (UInt32 i = 0; i < (UInt32)g.instanceMatrices.size(); ++i) {
				for (UInt32 m = 0; m < (UInt32)g.meshIds.size(); ++m) {
					dst[k].meshId = g.meshIds[m];
					dst[k].instanceId = instanceBase + i;
					dst[k].groupId = groupId;
					dst[k].pad = 0;
					++k;
				}
			}
			++groupId;
		}
		const UInt32 realGroups = (UInt32)p.groupBoundsScratch.size();
		if (realGroups > kSceneMaxGroups) return RenderError::InvalidArgument;
		ctx->UpdateBuffer(p.sceneInstances, 0, (UInt64)p.groupInstancesScratch.size() * sizeof(Mat4),
			p.groupInstancesScratch.data(), D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		ctx->UpdateBuffer(p.sceneTasks, 0, (UInt64)taskCount * sizeof(SceneTask), dst,
			D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		ctx->UpdateBuffer(p.clGroupBounds, 0, (UInt64)realGroups * sizeof(MeshGroupGPU),
			p.groupBoundsScratch.data(), D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	}
	const UInt32 N = (UInt32)p.groupInstancesScratch.size();

	// Adaptive overload protection, driven by the *raw* cluster demand (not the
	// clamped dispatch count). Driving it from the clamped value made the drop
	// threshold oscillate and whole instances flicker: the count saturates at
	// the budget forever, so the threshold ramps up and down without settling.
	// With the demand and a wide dead band the loop is monotone: dropping
	// instances really does reduce the demand.
	{
		const UInt32 hi = (UInt32)(p.clusterBudget * 0.95f);
		const UInt32 lo = (UInt32)(p.clusterBudget * 0.35f);
		if (p.sceneFrame < 2)                    p.adaptiveDrop = 0.06f;
		else if (p.sceneClusterDemand > hi)      p.adaptiveDrop = std::min(0.5f, p.adaptiveDrop * 1.15f);
		else if (p.sceneClusterDemand < lo)      p.adaptiveDrop = std::max(0.02f, p.adaptiveDrop / 1.15f);
	}

	// Constants.
	{
		UInt32 vpW = 0, vpH = 0;
		p.renderer->getViewportSize(vpW, vpH);
		SceneConstants cb{};
		cb.view = view;
		cb.viewProj = proj * view;
		extractFrustumPlanes(cb.viewProj, cb.frustum);
		cb.cotHalfFov = proj[1][1];
		cb.timeSec = timeSec;
		cb.frustumCulling = p.frustumCulling ? 1u : 0u;
		cb.activeInstances = N;
		cb.lodScale = p.lodScale;
		cb.dropSize = p.adaptiveDrop;
		cb.meshletCap = p.meshletCap;
		cb.debugMode = p.debugMode;
		cb.bodyCenter = Vec4(p.bodyCenter, 1.0f);
		cb.bodyRadius = p.bodyRadius;
		cb.meshFilter = p.meshFilter;
		cb.screenHeight = (F32)(vpH ? vpH : 1080u);
		cb.clusterCapacity = p.clusterBudget;
		// Camera position / sun / ambient come from the renderer so the cluster
		// path is lit exactly like the forward path (the sun is user-controlled).
		cb.cameraPos = Vec4(Vec3(glm::inverse(view)[3]), 1.0f);
		cb.ambient = p.renderer->getAmbientLight();
		// Cascaded shadows (see setShadowMap). Without a shadow pass the cascade
		// splits stay 0 so the last slice of the always-lit dummy is used.
		for (int i = 0; i < 4; ++i) cb.shadowMapUVDepth[i] = p.shadowUV[i];
		cb.cascadeSplits = p.cascadeSplits;
		// Sky colours feed the ambient irradiance (SkyIrradiance in the pixel
		// shader). Without a skybox the corners are filled with the ambient colour,
		// which degenerates the irradiance back to exactly the old flat ambient.
		{
			Vec4 skyCorners[8];
			const bool haveSky = p.renderer->getSkyboxCorners(skyCorners);
			for (int i = 0; i < 8; ++i) cb.skyCorners[i] = haveSky ? skyCorners[i] : cb.ambient;
		}
		Vec3 sunDir(0); Vec4 sunColor(1, 1, 1, 1);
		if (p.renderer->getPrimaryDirectionalLight(sunDir, sunColor)) {
			cb.lightDir = Vec4(glm::normalize(sunDir), 0.0f);
			cb.lightColor = sunColor;
		} else {
			cb.lightDir = Vec4(-0.45f, -0.85f, 0.28f, 0);
			cb.lightColor = Vec4(1, 1, 1, 1);
		}
		void* m = nullptr;
		ctx->MapBuffer(p.sceneCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m);
		if (m) { memcpy(m, &cb, sizeof(cb)); ctx->UnmapBuffer(p.sceneCB, D::MAP_WRITE); }
	}

	// Reset the visible-task counter and the global cluster allocator.
	{
		const UInt32 zero[4] = { 0, 0, 0, 0 };
		ctx->UpdateBuffer(p.sceneStatsBuf, 0, sizeof(zero), zero, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		ctx->UpdateBuffer(p.clCounter, 0, sizeof(zero), zero, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	}

	// ---- Per-frame draw groups ------------------------------------------
	// Build one flat instance array (group instances concatenated) plus the
	// (mesh, instance, group) task list, so the whole scene is one DrawMesh.

	ctx->SetPipelineState(pso);
	ctx->CommitShaderResources(srb, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

	// One task per (mesh, instance) pair; the AS derives one mesh group per
	// visible cluster (see g_ClusterAS).
	D::DrawMeshAttribs drawAttrs(taskCount, D::DRAW_FLAG_VERIFY_ALL);
	ctx->DrawMesh(drawAttrs);

	// Statistics readback (delayed by one frame).
	{
		ctx->CopyBuffer(p.sceneStatsBuf, 0, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
			p.sceneStatsStaging, static_cast<UInt32>(p.sceneFrame % kStatRingSize) * 16, 16,
			D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		ctx->EnqueueSignal(p.sceneStatsFence, p.sceneFrame);
		const UInt64 avail = p.sceneStatsFence->GetCompletedValue();
		// Strictly less than the ring size: with `avail == sceneFrame - kStatRingSize`
		// the slot picked below is the one this very frame's copy is writing, and
		// mapping it here races the GPU.
		if (p.sceneFrame >= kStatRingSize && avail + kStatRingSize > p.sceneFrame) {
			const UInt64 slot = avail % kStatRingSize;
			void* m = nullptr;
			ctx->MapBuffer(p.sceneStatsStaging, D::MAP_READ, D::MAP_FLAG_DO_NOT_WAIT, m);
			if (m) {
				const UInt8* base = static_cast<const UInt8*>(m);
				memcpy(&p.sceneVisibleTasks, base + slot * 16, sizeof(p.sceneVisibleTasks));
				memcpy(&p.sceneVisibleClusters, base + slot * 16 + 4, sizeof(p.sceneVisibleClusters));
				memcpy(&p.sceneClusterDemand, base + slot * 16 + 8, sizeof(p.sceneClusterDemand));
				ctx->UnmapBuffer(p.sceneStatsStaging, D::MAP_READ);
			}
		}
		++p.sceneFrame;
	}

	return {};
}

Result<void, RenderError> MeshShaderSubsystem::drawScene(const Vector<MeshDrawGroup>& groups,
	const Mat4& view, const Mat4& proj, F32 timeSec) {
	return drawSceneImpl(groups, view, proj, timeSec, false);
}

Result<void, RenderError> MeshShaderSubsystem::drawSceneGBuffer(const Vector<MeshDrawGroup>& groups,
	const Mat4& view, const Mat4& proj, F32 timeSec) {
	return drawSceneImpl(groups, view, proj, timeSec, true);
}

// Convenience single-group overloads: every registered mesh drawn with the given
// instance matrices (the original Furina-only behaviour).
Result<void, RenderError> MeshShaderSubsystem::drawScene(const Vector<Mat4>& instanceMatrices,
	const Mat4& view, const Mat4& proj, F32 timeSec) {
	return drawSceneImpl(singleGroup(instanceMatrices), view, proj, timeSec, false);
}

Result<void, RenderError> MeshShaderSubsystem::drawSceneGBuffer(const Vector<Mat4>& instanceMatrices,
	const Mat4& view, const Mat4& proj, F32 timeSec) {
	return drawSceneImpl(singleGroup(instanceMatrices), view, proj, timeSec, true);
}

Vector<MeshShaderSubsystem::MeshDrawGroup> MeshShaderSubsystem::singleGroup(const Vector<Mat4>& instanceMatrices) const {
	MeshDrawGroup g;
	g.instanceMatrices = instanceMatrices;
	g.meshIds.resize(m_impl->sceneMeshCount);
	for (UInt32 i = 0; i < m_impl->sceneMeshCount; ++i) g.meshIds[i] = i;
	Vector<MeshDrawGroup> out;
	out.push_back(std::move(g));
	return out;
}

EE_NAMESPACE_RENDERING_END

