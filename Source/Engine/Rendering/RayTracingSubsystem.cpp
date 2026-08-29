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
	F32 roughness;            ///< Material roughness (drives reflection attenuation).
	F32 metallic;             ///< Material metallic factor.
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
    float  Roughness;
    float  Metallic;
};
struct RTConstants {
    float4x4 ViewProjInv;
    float4   LightDir;
    float4   CameraPos;
    float    MaxRayLength;
    float    AmbientLight;
    uint     ShadowPCF;
    float    LightIntensity;
    float4   LightColor;
    float4   DiscPoints[8];
    float4   SkyCorners[8];
    uint     SkyMode;
    uint     _padSky0;
    uint     _padSky1;
    uint     _padSky2;
    float    AoRadius;
    uint     AoSamples;
    float    LightSize;
    float    ReflectionBlur;
    uint     MaxBounces;
    float    BounceRoughness;
    float    _padB0;
    float    _padB1;
};

RaytracingAccelerationStructure g_TLAS            : register(t0);
StructuredBuffer<ObjectAttribs>   g_ObjectAttribs : register(t1);
StructuredBuffer<MaterialAttribs> g_MaterialAttribs : register(t2);
StructuredBuffer<Vertex>          g_VertexBuffer  : register(t3);
StructuredBuffer<uint>            g_IndexBuffer   : register(t4);
Texture2D<float4>                 g_GBufferNormal : register(t5);
Texture2D<float>                  g_GBufferDepth  : register(t6);
Texture2D<float4>                 g_Textures[16]  : register(t7);
TextureCube<float4>               g_SkyCube       : register(t23);
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

// Percentage-closer soft shadows with optional PCSS (penumbra scales with the
// blocker distance). Samples are taken from the packed DiscPoints array.
float CastShadowPCF(float3 Origin, float3 LightDir, float MaxRayLength, float3 Norm, float DistToCam)
{
    // Depth-reconstruction error grows ~z^2 (perspective), so the origin bias
    // must scale with distance and with the slope (grazing angle) to keep the
    // ray start outside the surface; otherwise the start point falls inside the
    // mesh and the ray self-hits, producing regular "triangle" dark artifacts.
    float ndl = max(dot(LightDir, Norm), 0.1);
    float bias = max(0.002, 0.001 * DistToCam) / ndl;
    float3 origin = Origin + Norm * bias + LightDir * (bias * 0.5);

    float3 T = normalize(cross(abs(LightDir.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0), LightDir));
    float3 B = cross(LightDir, T);
    int    n = clamp((int)g_RTConstants.ShadowPCF, 1, 16);

    // PCSS: estimate the penumbra from the distance to the nearest blocker.
    float radius = 0.015;
    if (g_RTConstants.LightSize > 0.0)
    {
        RayDesc blockerRay;
        blockerRay.Origin    = origin;
        blockerRay.Direction = LightDir;
        blockerRay.TMin      = 0.0;
        blockerRay.TMax      = MaxRayLength;
        RayQuery<RAY_FLAG_CULL_FRONT_FACING_TRIANGLES> bq;
        bq.TraceRayInline(g_TLAS, RAY_FLAG_NONE, ~0u, blockerRay);
        bq.Proceed();
        if (bq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            float blockerDist = bq.CommittedRayT();
            // penumbra ~ (blockerDist - surfaceDist) * lightSize / blockerDist
            float penumbra = saturate((blockerDist - 0.001) * g_RTConstants.LightSize / max(blockerDist, 0.001));
            radius = clamp(penumbra * 0.05, 0.005, 0.05);
        }
    }

    float s = 0.0;
    for (int j = 0; j < 16; ++j)
    {
        if (j >= n)
            break;
        float4 dp = g_RTConstants.DiscPoints[j / 2];
        float2 d  = (j % 2 == 0) ? dp.xy : dp.zw;
        float3 dir = normalize(LightDir + (T * d.x + B * d.y) * radius);
        s += CastShadow(origin, dir, MaxRayLength);
    }
    return s / float(n);
}

// Screen-space-style ambient occlusion: short rays in a hemisphere around the
// normal, using the fixed disc directions. Returns 1 (unoccluded) .. 0 (fully
// occluded). AoSamples == 0 disables it.
float ComputeAO(float3 Origin, float3 Norm, float DistToCam)
{
    int samples = (int)g_RTConstants.AoSamples;
    if (samples <= 0)
        return 1.0;
    samples = clamp(samples, 1, 16);

    float3 T = normalize(cross(abs(Norm.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0), Norm));
    float3 B = cross(Norm, T);
    float bias = max(0.01, 0.001 * DistToCam);
    float3 aoOrigin = Origin + Norm * bias;

    float occluded = 0.0;
    for (int i = 0; i < samples; ++i)
    {
        float4 dp = g_RTConstants.DiscPoints[i / 2];
        float2 d  = (i % 2 == 0) ? dp.xy : dp.zw;
        // Hemisphere direction around the normal.
        float3 dir = normalize(Norm + (T * d.x + B * d.y) * 0.8);

        RayDesc ray;
        ray.Origin    = aoOrigin;
        ray.Direction = dir;
        ray.TMin      = 0.0;
        ray.TMax      = g_RTConstants.AoRadius;

        RayQuery<RAY_FLAG_CULL_FRONT_FACING_TRIANGLES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
        q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, ~0u, ray);
        q.Proceed();
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
            occluded += 1.0;
    }
    return 1.0 - occluded / float(samples);
}

float4 GetSkyColor(float3 Dir)
{
    // Sample the actual skybox so reflections match the rendered background:
    // either the cubemap texture (SkyMode=1) or the corner gradient (SkyMode=0,
    // trilinear interpolation of the 8 corner colors).
    if (g_RTConstants.SkyMode == 1)
    {
        return float4(g_SkyCube.SampleLevel(g_Sampler, Dir, 0).rgb, 1.0);
    }
    float3 w = saturate(Dir * 0.5 + 0.5);
    float3 c0  = lerp(g_RTConstants.SkyCorners[0].rgb, g_RTConstants.SkyCorners[1].rgb, w.x);
    float3 c1  = lerp(g_RTConstants.SkyCorners[2].rgb, g_RTConstants.SkyCorners[3].rgb, w.x);
    float3 c2  = lerp(g_RTConstants.SkyCorners[4].rgb, g_RTConstants.SkyCorners[5].rgb, w.x);
    float3 c3  = lerp(g_RTConstants.SkyCorners[6].rgb, g_RTConstants.SkyCorners[7].rgb, w.x);
    float3 c01 = lerp(c0, c1, w.y);
    float3 c23 = lerp(c2, c3, w.y);
    return float4(lerp(c01, c23, w.z), 1.0);
}

struct ReflectionResult {
    float4 BaseColor;
    float  NdotL;
    bool   Found;
};

// Trace one reflection ray and shade the hit point (material + shadow).
// When bounce < MaxBounces and the hit surface is smooth, casts a second
// reflection ray from the hit point (two-bounce reflections).
ReflectionResult Reflect(float3 Origin, float3 ReflDir, float MaxReflLen, float MaxShadowLen, float3 CameraPos, float3 LightDir, uint bounce)
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
        // CustomId is the ObjectAttribs start index of the instance; the
        // geometry index selects the mesh within the instance's BLAS.
        uint          InstId = q.CommittedInstanceID();
        uint          GeoId  = q.CommittedGeometryIndex();
        ObjectAttribs Obj    = g_ObjectAttribs[InstId + GeoId];
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
        // Roughness-driven reflection attenuation: rougher surfaces reflect less.
        res.BaseColor *= (1.0 - Mtr.Roughness * g_RTConstants.ReflectionBlur);
        res.NdotL     = max(0.0, dot(LightDir, Norm));
        res.Found     = true;

        if (res.NdotL > 0.0)
        {
            float3 HitPos = Origin + ReflDir * q.CommittedRayT();
            res.NdotL *= CastShadowPCF(HitPos, LightDir, MaxShadowLen, Norm, length(HitPos - CameraPos));
        }

        // Two-bounce: reflect again from the hit point on surfaces at or below
        // the roughness threshold (BounceRoughness = 1.0 forces all surfaces).
        if (res.Found && bounce < g_RTConstants.MaxBounces && Mtr.Roughness <= g_RTConstants.BounceRoughness)
        {
            float3 HitPos = Origin + ReflDir * q.CommittedRayT();
            float3 Refl2  = reflect(ReflDir, Norm);
            float  bias2  = max(0.002, 0.001 * length(HitPos - CameraPos));
            RayDesc ray2;
            ray2.Origin    = HitPos + Norm * bias2;
            ray2.Direction = Refl2;
            ray2.TMin      = 0.0;
            ray2.TMax      = MaxReflLen;

            RayQuery<RAY_FLAG_CULL_BACK_FACING_TRIANGLES> q2;
            q2.TraceRayInline(g_TLAS, RAY_FLAG_NONE, ~0u, ray2);
            q2.Proceed();

            float3 col2;
            if (q2.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
            {
                // Simplified second-bounce shading: material base color only.
                uint          InstId2 = q2.CommittedInstanceID();
                uint          GeoId2  = q2.CommittedGeometryIndex();
                ObjectAttribs   Obj2   = g_ObjectAttribs[InstId2 + GeoId2];
                MaterialAttribs Mtr2   = g_MaterialAttribs[Obj2.MaterialId];
                col2 = Mtr2.BaseColorMask.rgb * (1.0 - Mtr2.Roughness * g_RTConstants.ReflectionBlur);
            }
            else
            {
                col2 = GetSkyColor(Refl2).rgb;
            }
            res.BaseColor = lerp(res.BaseColor, float4(col2, res.BaseColor.a), 0.5);
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
    g_OutRT.GetDimensions(Dim.x, Dim.y); // half-resolution output
    if (DTid.x >= Dim.x || DTid.y >= Dim.y)
        return;

    // Depth-guided sampling: pick the G-buffer texel with the nearest depth
    // (the visible surface). At half resolution we scan the 2x2 footprint of
    // this output pixel; at full resolution it is the 1:1 texel. This avoids
    // blending depth/normal across geometry edges (bilinear "triangle"
    // artifacts) while keeping surface detail (vs. always sampling one center
    // texel, which produces blocky/banded half-res lighting).
    uint2 FullDim;
    g_GBufferDepth.GetDimensions(FullDim.x, FullDim.y);
    int2  fpBest;
    float bestD;
    if (Dim.x >= FullDim.x) // full resolution: 1:1
    {
        fpBest = min(int2(DTid), int2(FullDim) - 1);
        bestD  = g_GBufferDepth.Load(int3(fpBest, 0)).x;
    }
    else // half resolution: 2x2 footprint
    {
        int2 maxc = int2(FullDim) - 1;
        int2 base = int2(DTid * 2);
        int2 p00 = min(base + int2(0, 0), maxc);
        int2 p10 = min(base + int2(1, 0), maxc);
        int2 p01 = min(base + int2(0, 1), maxc);
        int2 p11 = min(base + int2(1, 1), maxc);
        float d00 = g_GBufferDepth.Load(int3(p00, 0)).x;
        float d10 = g_GBufferDepth.Load(int3(p10, 0)).x;
        float d01 = g_GBufferDepth.Load(int3(p01, 0)).x;
        float d11 = g_GBufferDepth.Load(int3(p11, 0)).x;
        fpBest = p00; bestD = d00;
        if (d10 < bestD) { bestD = d10; fpBest = p10; }
        if (d01 < bestD) { bestD = d01; fpBest = p01; }
        if (d11 < bestD) { bestD = d11; fpBest = p11; }
    }

    float Depth = bestD;
    if (Depth >= 1.0)
    {
        g_OutRT[DTid] = float4(0, 0, 0, 1);
        return;
    }

    float2 BestUV  = (float2(fpBest) + 0.5) / float2(FullDim);
    float3 WPos    = ScreenPosToWorldPos(BestUV, Depth, g_RTConstants.ViewProjInv);
    float3 LightDir = g_RTConstants.LightDir.xyz;
    float3 WNormal = normalize(g_GBufferNormal.Load(int3(fpBest, 0)).xyz);
    float  DisToCam = length(WPos - g_RTConstants.CameraPos.xyz);
    float3 ViewDir = (WPos - g_RTConstants.CameraPos.xyz) / max(DisToCam, 0.0001);

    float NdotL = max(0.0, dot(LightDir, WNormal));
    if (NdotL > 0.0)
        NdotL *= CastShadowPCF(WPos, LightDir, g_RTConstants.MaxRayLength, WNormal, DisToCam);

    // Ambient occlusion (short hemisphere rays).
    float ao = ComputeAO(WPos, WNormal, DisToCam);
    // Current pixel roughness (from the G-buffer normal alpha) for the sky miss.
    float roughness = g_GBufferNormal.Load(int3(fpBest, 0)).a;

    float4 Color = float4(0, 0, 0, 1);
    {
        // Reflect from a point biased above the surface to avoid self-hit. The
        // bias scales with distance (depth-reconstruction error grows ~z^2), the
        // same fix that removed the shadow "triangle" artifacts.
        float bias = max(0.002, 0.001 * DisToCam);
        ReflectionResult refl = Reflect(WPos + WNormal * bias,
                                        reflect(ViewDir, WNormal),
                                        g_RTConstants.MaxRayLength,
                                        g_RTConstants.MaxRayLength,
                                        g_RTConstants.CameraPos.xyz,
                                        LightDir,
                                        0);
        if (refl.Found)
        {
            // Reflection point lighting: ambient and direct are both tinted by the
            // light color (consistent with the compose diffuse term).
            float3 lightColor = g_RTConstants.LightColor.rgb;
            float3 lit = (g_RTConstants.AmbientLight + max(0.0, refl.NdotL) * g_RTConstants.LightIntensity) * lightColor;
            Color = float4(refl.BaseColor.rgb * lit, 1.0);
        }
        else
            Color = GetSkyColor(reflect(ViewDir, WNormal)) * (1.0 - roughness * g_RTConstants.ReflectionBlur);
    }

    // Lighting factor: ambient (AO-shaded) + direct (shadowed, intensity-scaled).
    // Compose uses Color * RT.a for the diffuse term.
    Color.a = g_RTConstants.AmbientLight * ao + NdotL * g_RTConstants.LightIntensity;
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
SamplerState      g_Sampler       : register(s0);
cbuffer ComposeCB : register(b0) {
    float4x4 g_ViewProjInv;
    float4   g_CameraPos;
    uint     g_DrawMode;
    float3   g_Pad;
    float4   g_LightColor;
};
struct PSIn { float4 Pos : SV_POSITION; float2 UV : TEXCOORD0; };

#define RENDER_MODE_SHADED           0
#define RENDER_MODE_G_BUFFER_COLOR   1
#define RENDER_MODE_G_BUFFER_NORMAL  2
#define RENDER_MODE_DIFFUSE_LIGHTING 3
#define RENDER_MODE_REFLECTIONS      4
#define RENDER_MODE_FRESNEL_TERM     5
#define RENDER_MODE_RT_ALPHA         6

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
    // The ray traced texture is half resolution; bilinear upsample.
    float4 RT     = g_RayTracedTex.SampleLevel(g_Sampler, UV, 0);

    float3 WPos    = ScreenPosToWorldPos(UV, Depth, g_ViewProjInv);
    float3 ViewDir = normalize(g_CameraPos.xyz - WPos);
    float  NdotV   = saturate(dot(Normal, ViewDir));
    float  R       = lerp(0.04, 1.0, pow(1.0 - NdotV, 5.0));

    switch (g_DrawMode)
    {
        case RENDER_MODE_G_BUFFER_COLOR:   return Color;
        case RENDER_MODE_G_BUFFER_NORMAL:  return float4(abs(Normal), 1.0);
        case RENDER_MODE_DIFFUSE_LIGHTING: return float4(Color.rgb * RT.a, 1.0);
        case RENDER_MODE_REFLECTIONS:      return float4(RT.rgb, 1.0);
        case RENDER_MODE_FRESNEL_TERM:     return float4(R, R, R, 1.0);
        case RENDER_MODE_RT_ALPHA:         return float4(RT.a, RT.a, RT.a, 1.0);
        case RENDER_MODE_SHADED:
        default:
        {
            // Diffuse: albedo * lighting factor * light color.
            float3 Shaded = Color.rgb * RT.a * g_LightColor.rgb;
            float3 Final  = lerp(Shaded, RT.rgb, R);
            return float4(Final, 1.0);
        }
    }
}
)";

// ===================================================================
// Acceleration structure data (owned by this subsystem).
// ===================================================================
struct BLASData { D::RefCntAutoPtr<D::IBottomLevelAS> blas; Vector<MeshHandle> sourceMeshes; };
struct TLASData {
	D::RefCntAutoPtr<D::ITopLevelAS> tlas;
	D::RefCntAutoPtr<D::IBuffer> scratch;
	D::RefCntAutoPtr<D::IBuffer> instanceBuf;
	UInt32 maxInstances = 0;
	bool allowUpdate = true;
	bool built = false;
	Vector<String> names; ///< Cached instance names (Diligent requires non-null; avoids per-frame allocations).
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
	Vector<BLASHandle> blasCache;       ///< BLAS per unique group mesh-set.
	Vector<Vector<MeshHandle>> blasCacheMeshSet; ///< The mesh-set each cached BLAS was built from.

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
	D::RefCntAutoPtr<D::ITextureView> whiteCubeSRV;      ///< 1x1 white cubemap fallback for g_SkyCube.
	D::RefCntAutoPtr<D::ISampler> linearSampler;

	// RT compute pipeline
	D::RefCntAutoPtr<D::IPipelineState> rtPSO;
	D::RefCntAutoPtr<D::IShaderResourceBinding> rtSRB;
	D::RefCntAutoPtr<D::IBuffer> constantsBuf;
	D::RefCntAutoPtr<D::IQuery> traceQuery; ///< GPU duration query for trace().
	bool traceQueryEnded = false;          ///< Whether the query was ended at least once (GetData guard).
	float lastTraceMs = 0.0f;

	// Skybox for the reflection miss shader (matches the rendered background).
	bool skyUseCubemap = false;
	TextureHandle skyCubemap;
	Vec4 skyCorners[8] = { Vec4(0.3f, 0.5f, 0.9f, 1) };

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
float RayTracingSubsystem::lastTraceMs() const { return m_impl->lastTraceMs; }

void RayTracingSubsystem::setSkybox(bool useCubemap, TextureHandle cubemap, const Vec4 corners[8]) {
	auto& p = *m_impl;
	p.skyUseCubemap = useCubemap;
	p.skyCubemap = cubemap;
	if (corners) {
		for (int i = 0; i < 8; ++i) p.skyCorners[i] = corners[i];
	}
}

// ===================================================================
// Acceleration structures
// ===================================================================

Result<BLASHandle, RenderError> RayTracingSubsystem::createBLAS(MeshHandle mesh) {
	Vector<MeshHandle> meshes{ mesh };
	return createBLAS(meshes);
}

Result<BLASHandle, RenderError> RayTracingSubsystem::createBLAS(const Vector<MeshHandle>& meshes) {
	auto& p = *m_impl;
	auto* dev = p.device(); if (!dev) return RenderError::NotInitialized;
	auto* ctx = p.ctx(); if (!ctx) return RenderError::NotInitialized;
	if (meshes.empty()) return RenderError::InvalidArgument;

	// Geometry names must be stable for the BLAS lifetime (used by the build data).
	Vector<String> geoNames;
	geoNames.reserve(meshes.size());
	Vector<D::BLASTriangleDesc> tris(meshes.size());
	Vector<D::IBuffer*> vbs(meshes.size());
	Vector<D::IBuffer*> ibs(meshes.size());
	Vector<UInt32> vcs(meshes.size());
	Vector<UInt32> ics(meshes.size());
	UInt32 totalPrims = 0;
	for (size_t i = 0; i < meshes.size(); ++i) {
		void* vb = nullptr; void* ib = nullptr; UInt32 vc = 0; UInt32 ic = 0;
		p.renderer->getMeshGeometry(meshes[i], vb, ib, vc, ic);
		if (!vb || !ib || ic == 0) return RenderError::InvalidHandle;
		geoNames.emplace_back("Geo" + std::to_string(i));
		vbs[i] = static_cast<D::IBuffer*>(vb);
		ibs[i] = static_cast<D::IBuffer*>(ib);
		vcs[i] = vc;
		ics[i] = ic;
		D::BLASTriangleDesc& tri = tris[i];
		tri.GeometryName         = geoNames.back().c_str();
		tri.MaxVertexCount       = vc;
		tri.VertexValueType      = D::VT_FLOAT32;
		tri.VertexComponentCount = 3;
		tri.MaxPrimitiveCount    = ic / 3;
		tri.IndexType            = D::VT_UINT32;
		totalPrims += ic / 3;
	}

	D::BottomLevelASDesc ad;
	ad.Name          = "EE_RT_BLAS";
	ad.Flags         = D::RAYTRACING_BUILD_AS_PREFER_FAST_TRACE;
	ad.pTriangles    = tris.data();
	ad.TriangleCount = (D::Uint32)tris.size();
	D::RefCntAutoPtr<D::IBottomLevelAS> blas;
	dev->CreateBLAS(ad, &blas);
	if (!blas) { EError("RayTracing: BLAS creation failed ({} geometries)", meshes.size()); return RenderError::AccelerationStructureCreationFailed; }

	{
		const auto sz = blas->GetScratchBufferSizes().Build;
		if (!p.blasScratch || p.blasScratch->GetDesc().Size < sz) {
			D::BufferDesc sd; sd.Name = "RT BLAS Scratch"; sd.Usage = D::USAGE_DEFAULT; sd.BindFlags = D::BIND_RAY_TRACING; sd.Size = sz;
			p.blasScratch.Release();
			dev->CreateBuffer(sd, nullptr, &p.blasScratch);
			if (!p.blasScratch) return RenderError::BufferCreationFailed;
		}
	}

	Vector<D::BLASBuildTriangleData> tds(meshes.size());
	for (size_t i = 0; i < meshes.size(); ++i) {
		D::BLASBuildTriangleData& td = tds[i];
		td.GeometryName         = geoNames[i].c_str();
		td.pVertexBuffer        = vbs[i];
		td.VertexStride         = sizeof(Vertex);
		td.VertexCount          = vcs[i];
		td.VertexValueType      = tris[i].VertexValueType;
		td.VertexComponentCount = tris[i].VertexComponentCount;
		td.pIndexBuffer         = ibs[i];
		td.PrimitiveCount       = tris[i].MaxPrimitiveCount;
		td.IndexType            = tris[i].IndexType;
		td.Flags                = D::RAYTRACING_GEOMETRY_FLAG_OPAQUE;
	}

	D::BuildBLASAttribs ba;
	ba.pBLAS             = blas;
	ba.pTriangleData     = tds.data();
	ba.TriangleDataCount = (D::Uint32)tds.size();
	ba.pScratchBuffer    = p.blasScratch;
	ba.BLASTransitionMode          = D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
	ba.GeometryTransitionMode      = D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
	ba.ScratchBufferTransitionMode = D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
	ctx->BuildBLAS(ba);

	auto a = p.blases.allocate();
	auto* dd = p.blases.getUnchecked(a.index);
	dd->blas = std::move(blas);
	dd->sourceMeshes = meshes;
	EInfo("RayTracing: BLAS created ({} geometries, {} primitives)", meshes.size(), totalPrims);
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

	// Cache instance names (rebuilt only when the instance count changes).
	if (dd.names.size() != instances.size()) {
		dd.names.clear();
		dd.names.reserve(instances.size()); // reserve so c_str() pointers stay valid
		for (size_t i = 0; i < instances.size(); ++i)
			dd.names.emplace_back(instances[i].name.empty() ? "Instance" + std::to_string(i) : instances[i].name);
	}

	Vector<D::TLASBuildInstanceData> insts;
	insts.reserve(instances.size());
	for (size_t i = 0; i < instances.size(); ++i) {
		const TLASInstance& in = instances[i];
		auto* bd = p.blases.get(in.blas.index, in.blas.generation);
		if (!bd || !bd->blas) { EError("RayTracing: invalid BLAS handle {}:{}", in.blas.index, in.blas.generation); return RenderError::InvalidHandle; }
		D::TLASBuildInstanceData di;
		di.InstanceName = dd.names[i].c_str();
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

Result<void, RenderError> RayTracingSubsystem::updateScene(const Vector<RayTracedObjectGroup>& groups) {
	auto& p = *m_impl;
	auto* dev = p.device(); if (!dev) return RenderError::NotInitialized;
	auto* ctx = p.ctx(); if (!ctx) return RenderError::NotInitialized;
	if (groups.empty() || !p.rtPSO) return RenderError::InvalidArgument;

	// 1. Ensure a BLAS (one geometry per mesh) exists for every unique group mesh-set.
	Vector<BLASHandle> groupBLAS(groups.size());
	for (size_t g = 0; g < groups.size(); ++g) {
		Vector<MeshHandle> meshes;
		meshes.reserve(groups[g].objects.size());
		for (auto& o : groups[g].objects) meshes.push_back(o.mesh);
		BLASHandle bh;
		for (size_t c = 0; c < p.blasCacheMeshSet.size(); ++c) {
			if (p.blasCacheMeshSet[c].size() != meshes.size()) continue;
			bool same = true;
			for (size_t i = 0; i < meshes.size(); ++i) {
				if (p.blasCacheMeshSet[c][i].index != meshes[i].index ||
					p.blasCacheMeshSet[c][i].generation != meshes[i].generation) { same = false; break; }
			}
			if (same) { bh = p.blasCache[c]; break; }
		}
		if (!bh.isValid()) {
			auto r = createBLAS(meshes);
			if (r.isErr()) { EError("RayTracing: createBLAS failed: {}", ToString(r.error())); return r.error(); }
			bh = r.value();
			p.blasCache.push_back(bh);
			p.blasCacheMeshSet.push_back(std::move(meshes));
		}
		groupBLAS[g] = bh;
	}

	// 1.5. Rebuild the shared vertex/index buffers when the mesh set changes.
	// All unique scene meshes are merged into one vertex/index buffer pair.
	Vector<MeshHandle> allMeshes;
	for (auto& set : p.blasCacheMeshSet)
		for (auto& mh : set)
			if (std::find(allMeshes.begin(), allMeshes.end(), mh) == allMeshes.end())
				allMeshes.push_back(mh);
	bool meshSetChanged = allMeshes.size() != p.sceneMeshKeys.size();
	if (!meshSetChanged) {
		for (size_t i = 0; i < allMeshes.size(); ++i) {
			if (allMeshes[i].index != p.sceneMeshKeys[i].index ||
				allMeshes[i].generation != p.sceneMeshKeys[i].generation) { meshSetChanged = true; break; }
		}
	}
	if (meshSetChanged) {
		Vector<Vertex> allVerts;
		Vector<UInt32> allIdx;
		Vector<UInt32> firstVertex, firstIndex;
		for (auto& mh : allMeshes) {
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
		p.sceneMeshKeys = allMeshes;
		p.sceneMeshFirstVertex = std::move(firstVertex);
		p.sceneMeshFirstIndex = std::move(firstIndex);
		EInfo("RayTracing: shared geometry rebuilt ({} meshes, {} verts, {} indices)", p.sceneMeshKeys.size(), allVerts.size(), allIdx.size());
	}

	// 2. Rebuild material attribs (deduplicated by material handle) + texture registry.
	// Traversal order matches the ObjectAttribs expansion in step 3.
	UInt32 totalObjects = 0;
	for (auto& g : groups) totalObjects += (UInt32)g.objects.size();
	Vector<RTMaterialAttribs> newMaterials;
	Vector<MaterialHandle> materialKeys;
	Vector<UInt32> objMaterialId(totalObjects);
	Vector<TextureHandle> texKeys;
	Vector<D::ITextureView*> newSRVs;
	UInt32 oi = 0;
	for (auto& g : groups) {
		for (auto& o : g.objects) {
			const MaterialHandle mat = o.material;
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
				ma.roughness = desc->roughnessFactor;
				ma.metallic = desc->metallicFactor;
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
			objMaterialId[oi++] = mid;
		}
	}

	// 3. Build ObjectAttribs (expanded per geometry; FirstIndex/FirstVertex into the shared buffers).
	Vector<RTObjectAttribs> objAttribs(totalObjects);
	Vector<UInt32> groupStart(groups.size());
	oi = 0;
	for (size_t g = 0; g < groups.size(); ++g) {
		groupStart[g] = oi;
		const Mat4 wm = groups[g].transform.computeWorldMatrix();
		const Mat4 nm = groups[g].transform.computeNormalMatrix();
		for (auto& o : groups[g].objects) {
			UInt32 mi = InvalidIndex;
			for (size_t m = 0; m < p.sceneMeshKeys.size(); ++m) {
				if (p.sceneMeshKeys[m].index == o.mesh.index && p.sceneMeshKeys[m].generation == o.mesh.generation) { mi = (UInt32)m; break; }
			}
			RTObjectAttribs& oa = objAttribs[oi];
			oa.modelMat  = wm;
			oa.normalMat = nm;
			oa.materialId = objMaterialId[oi];
			oa.firstIndex = (mi != InvalidIndex) ? p.sceneMeshFirstIndex[mi] : 0;
			oa.firstVertex = (mi != InvalidIndex) ? p.sceneMeshFirstVertex[mi] : 0;
			oa.meshId = mi;
			++oi;
		}
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

	// 6. Build/update the internal TLAS (one instance per group).
	Vector<TLASInstance> instances(groups.size());
	for (size_t g = 0; g < groups.size(); ++g) {
		TLASInstance& in = instances[g];
		in.blas = groupBLAS[g];
		in.transform = groups[g].transform.computeWorldMatrix();
		in.mask = 0xFF;
		in.customId = groupStart[g]; // ObjectAttribs start index of this instance
	}
	if (!p.sceneTLAS.tlas || p.sceneTLAS.maxInstances < groups.size()) {
		p.sceneTLAS = TLASData{}; // Recreate with the new capacity.
		D::TopLevelASDesc td;
		td.Name = "EE_RT_SceneTLAS";
		td.MaxInstanceCount = (UInt32)groups.size();
		td.Flags = D::RAYTRACING_BUILD_AS_PREFER_FAST_TRACE | D::RAYTRACING_BUILD_AS_ALLOW_UPDATE;
		dev->CreateTLAS(td, &p.sceneTLAS.tlas);
		if (!p.sceneTLAS.tlas) return RenderError::AccelerationStructureCreationFailed;
		p.sceneTLAS.maxInstances = (UInt32)groups.size();
		p.sceneTLAS.allowUpdate = true;
		EInfo("RayTracing: scene TLAS created ({} instances)", groups.size());
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

	// Upload constants (skybox data is merged in from the subsystem state).
	{
		RayTraceConstants cc = c;
		cc.skyMode = p.skyUseCubemap ? 1 : 0;
		memcpy(cc.skyCorners, p.skyCorners, sizeof(p.skyCorners));
		void* m = nullptr;
		ctx->MapBuffer(p.constantsBuf, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m);
		if (m) { memcpy(m, &cc, sizeof(cc)); ctx->UnmapBuffer(p.constantsBuf, D::MAP_WRITE); }
	}

	if (auto* v = p.rtSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_GBufferNormal")) v->Set(static_cast<D::ITextureView*>(gBufferNormal), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = p.rtSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_GBufferDepth")) v->Set(static_cast<D::ITextureView*>(gBufferDepth), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = p.rtSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_OutRT")) v->Set(static_cast<D::ITextureView*>(outRT), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	// Bind the skybox cubemap (or the white CUBEMAP fallback when not in cubemap mode).
	if (auto* v = p.rtSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_SkyCube")) {
		D::ITextureView* srv = p.whiteCubeSRV.RawPtr();
		if (p.skyUseCubemap && p.skyCubemap.isValid()) {
			if (auto* s = static_cast<D::ITextureView*>(p.renderer->getTextureSRV(p.skyCubemap))) srv = s;
		}
		v->Set(srv, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	}

	ctx->SetPipelineState(p.rtPSO);
	ctx->CommitShaderResources(p.rtSRB, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

	// Read the previous frame's duration result (if ready).
	if (p.traceQuery && p.traceQueryEnded) {
		D::QueryDataDuration qd;
		if (p.traceQuery->GetData(&qd, sizeof(qd), false) && qd.Frequency != 0) {
			p.lastTraceMs = (float)((double)qd.Duration / (double)qd.Frequency * 1000.0);
		}
	}

	D::DispatchComputeAttribs da;
	da.ThreadGroupCountX = (width + 7) / 8;
	da.ThreadGroupCountY = (height + 7) / 8;
	if (p.traceQuery) ctx->BeginQuery(p.traceQuery);
	ctx->DispatchCompute(da);
	if (p.traceQuery) { ctx->EndQuery(p.traceQuery); p.traceQueryEnded = true; }
	return {};
}

Result<void, RenderError> RayTracingSubsystem::compose(void* gBufferColor, void* gBufferNormal, void* gBufferDepth,
	void* rtTex, void* outputRTV, void* depthDSV, UInt32 drawMode,
	const Mat4& viewProjInv, const Vec3& cameraPos, const Vec3& lightColor,
	UInt32 width, UInt32 height) {
	auto& p = *m_impl;
	auto* ctx = p.ctx(); if (!ctx) return RenderError::NotInitialized;
	if (!p.ok || !p.composePSO || !p.composeSRB) return RenderError::OperationFailed;
	if (!gBufferColor || !gBufferNormal || !gBufferDepth || !rtTex || !outputRTV) return RenderError::InvalidArgument;

	{
		// ComposeCB: float4x4 + float4 + uint + 3 floats padding + lightColor (16-byte aligned).
		struct { Mat4 viewProjInv; Vec4 cameraPos; UInt32 drawMode; UInt32 _pad[3]; Vec4 lightColor; } cb;
		cb.viewProjInv = viewProjInv;
		cb.cameraPos = Vec4(cameraPos, 1.0f);
		cb.drawMode = drawMode;
		cb._pad[0] = cb._pad[1] = cb._pad[2] = 0;
		cb.lightColor = Vec4(lightColor, 0.0f);
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

	// 1x1 white CUBEMAP (fallback for g_SkyCube so the bound dimension always
	// matches the TextureCube the shader expects, even in corner-gradient mode).
	{
		D::TextureDesc td; td.Name = "RT WhiteCube"; td.Type = D::RESOURCE_DIM_TEX_CUBE; td.Width = 1; td.Height = 1;
		td.ArraySize = 6; td.MipLevels = 1;
		td.Format = D::TEX_FORMAT_RGBA8_UNORM; td.BindFlags = D::BIND_SHADER_RESOURCE; td.Usage = D::USAGE_IMMUTABLE;
		UInt32 white = 0xFFFFFFFF;
		D::TextureSubResData srd[6];
		for (int i = 0; i < 6; ++i) { srd[i].pData = &white; srd[i].Stride = 4; }
		D::TextureData tdata; tdata.pSubResources = srd; tdata.NumSubresources = 6;
		D::RefCntAutoPtr<D::ITexture> tex;
		dev->CreateTexture(td, &tdata, &tex);
		if (tex) p.whiteCubeSRV = tex->GetDefaultView(D::TEXTURE_VIEW_SHADER_RESOURCE);
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

	// GPU duration query for trace().
	{
		D::QueryDesc qd(D::QUERY_TYPE_DURATION);
		dev->CreateQuery(qd, &p.traceQuery);
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
		D::ImmutableSamplerDesc csam[] = { { D::SHADER_TYPE_PIXEL, "g_Sampler", D::SamplerDesc{} } };
		gci.PSODesc.ResourceLayout.ImmutableSamplers = csam;
		gci.PSODesc.ResourceLayout.NumImmutableSamplers = 1;
		gci.pVS = vs; gci.pPS = ps;
		gci.GraphicsPipeline.NumRenderTargets = 1;
		// Compose outputs into the RGBA16_FLOAT HDR target (matches the renderer's
		// scene PSOs and the PostProcess HDR texture).
		gci.GraphicsPipeline.RTVFormats[0] = D::TEX_FORMAT_RGBA16_FLOAT;
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
		D::BufferDesc bd; bd.Name = "RT Compose CB"; bd.Size = sizeof(Mat4) + sizeof(Vec4) + 16 + 16; // + drawMode/padding + lightColor
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
	p.traceQuery.Release(); p.traceQueryEnded = false;
	p.composePSO.Release(); p.composeSRB.Release(); p.composeCB.Release();
	p.objectAttribsBuf.Release(); p.materialAttribsBuf.Release();
	p.sceneTLAS = TLASData{};
	p.blases.reset(); p.tlases.reset();
	p.blasScratch.Release();
	p.blasCache.clear(); p.blasCacheMeshSet.clear();
	p.sharedVB.Release(); p.sharedIB.Release();
	p.sceneMeshKeys.clear(); p.sceneMeshFirstVertex.clear(); p.sceneMeshFirstIndex.clear();
	p.sceneTextureSRVs.clear();
	p.whiteSRV.Release(); p.whiteCubeSRV.Release(); p.linearSampler.Release();
	p.ok = false;
	EInfo("RayTracing subsystem shut down.");
}

EE_NAMESPACE_RENDERING_END
