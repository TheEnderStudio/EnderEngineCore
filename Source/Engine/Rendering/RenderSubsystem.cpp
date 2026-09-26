#include <Rendering/RenderSubsystem.hpp>
#include <Core/Log.hpp>
#include <Core/Crash.h>
#include <Rendering/Errors.hpp>
#include <Rendering/RenderEvents.hpp>
#include <Rendering/ShadowSubsystem.hpp>
#include <Resource/ResourcesManager.hpp>
#include <Utilities/ImageLoader.hpp>

#include <GLFW/glfw3.h>
#ifdef EE_WINDOWS
#	define GLFW_EXPOSE_NATIVE_WIN32
#	include <GLFW/glfw3native.h>
#endif

#include <DiligentCore/Graphics/GraphicsEngine/interface/RenderDevice.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/DeviceContext.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/SwapChain.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/EngineFactory.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/PipelineState.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Buffer.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Texture.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Shader.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Sampler.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/GraphicsTypes.h>
#include <DiligentCore/Graphics/GraphicsEngineD3D11/interface/EngineFactoryD3D11.h>
#include <DiligentCore/Graphics/GraphicsEngineD3D12/interface/EngineFactoryD3D12.h>
#include <DiligentCore/Graphics/GraphicsEngineVulkan/interface/EngineFactoryVk.h>
#include <DiligentCore/Common/interface/RefCntAutoPtr.hpp>
#include <DiligentCore/Common/interface/AdvancedMath.hpp>
#include <DiligentTools/TextureLoader/interface/TextureUtilities.h>
#include <imgui.h>

#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>
#include <fastgltf/tools.hpp>

#include <stb_image.h>
#include <algorithm>
#include <Jobs/JobSubsystem.hpp>
#include <Jobs/JobTypes.hpp>
#include <Utilities/ImageLoader.hpp>

#include "ResourcePool.hpp"

namespace D = Diligent;

EE_NAMESPACE_RENDERING_BEGIN

// ===================================================================
// Embedded HLSL shaders (column-major, mul(matrix,vector) convention)
// ===================================================================

static const char* g_VS = R"(
cbuffer Frame : register(b0)
{
    float4x4 g_ViewProj;
    float4   g_CameraPos;
    float4   g_Ambient;
uint     g_LightCount;
    float    _p0;
    float    _p1;
    float    _p2;
    float4x4 g_ShadowMapUVDepth[4]; float4 g_CascadeSplits;
};

cbuffer Object : register(b2)
{
    float4x4 g_World;
    float4x4 g_Normal;
    float4   g_BaseColor;
    float4   g_MetallicRough;
};

struct VSIn
{
    float3 Pos : ATTRIB0;
    float3 Nrm : ATTRIB1;
    float2 UV  : ATTRIB2;
    float4 Tan : ATTRIB3;
};

struct VSOut
{
    float4 Pos : SV_POSITION;
    float3 WP  : TEXCOORD0;
    float3 N   : TEXCOORD1;
    float2 UV  : TEXCOORD2;
};

VSOut main(VSIn i)
{
    VSOut o;
    float4 worldPos = mul(g_World, float4(i.Pos, 1.0));
    o.WP = worldPos.xyz;
    o.Pos = mul(g_ViewProj, worldPos);
    o.N = normalize(mul((float3x3)g_Normal, i.Nrm));
    o.UV = i.UV;
    return o;
}
)";

// Instanced vertex shader: per-instance world matrix COLUMNS from slot 1 (ATTRIB4-7)
// Same column-vector convention as non-instanced: mul(matrix, vector)
static const char* g_VS_Inst = R"(
cbuffer Frame : register(b0) { float4x4 g_ViewProj; float4 g_CameraPos; float4 g_Ambient; uint g_LightCount; float _p0; float _p1; float _p2; float4x4 g_ShadowMapUVDepth[4]; float4 g_CascadeSplits; };
cbuffer Object : register(b2) { float4x4 g_World; float4x4 g_Normal; float4 g_BaseColor; float4 g_MetallicRough; };
struct VSIn {
    float3 Pos : ATTRIB0; float3 Nrm : ATTRIB1; float2 UV : ATTRIB2; float4 Tan : ATTRIB3;
    float4 IW0 : ATTRIB4; float4 IW1 : ATTRIB5; float4 IW2 : ATTRIB6; float4 IW3 : ATTRIB7;
};
struct VSOut { float4 Pos : SV_POSITION; float3 WP : TEXCOORD0; float3 N : TEXCOORD1; float2 UV : TEXCOORD2; };
VSOut main(VSIn i) {
    VSOut o;
    // HLSL float4x4{} treats args as rows. IW0-3 = glm columns.
    // transpose recovers the correct column-major matrix.
    float4x4 iw = { i.IW0, i.IW1, i.IW2, i.IW3 };
    iw = transpose(iw);
    float4 worldPos = mul(iw, float4(i.Pos, 1));
    o.WP = worldPos.xyz;
    o.Pos = mul(g_ViewProj, worldPos);
    o.N = normalize(mul((float3x3)iw, i.Nrm));
    o.UV = i.UV;
    return o;
}
)";

// Indirect instanced VS: reads world matrices from StructuredBuffer (GPU-culled)
static const char* g_VS_Indirect = R"(
cbuffer Frame : register(b0) { float4x4 g_ViewProj; float4 g_CameraPos; float4 g_Ambient; uint g_LightCount; float _p0; float _p1; float _p2; float4x4 g_ShadowMapUVDepth[4]; float4 g_CascadeSplits; };
cbuffer Object : register(b2) { float4x4 g_World; float4x4 g_Normal; float4 g_BaseColor; float4 g_MetallicRough; };
StructuredBuffer<float4x4> g_WorldMatrices : register(t5);
StructuredBuffer<uint> g_Indices : register(t6);
struct VSIn { float3 Pos : ATTRIB0; float3 Nrm : ATTRIB1; float2 UV : ATTRIB2; float4 Tan : ATTRIB3; };
struct VSOut { float4 Pos : SV_POSITION; float3 WP : TEXCOORD0; float3 N : TEXCOORD1; float2 UV : TEXCOORD2; };
VSOut main(VSIn i, uint instID : SV_InstanceID) {
    VSOut o;
    uint origIdx = g_Indices[instID];
    float4x4 iw = g_WorldMatrices[origIdx];
    float4 worldPos = mul(iw, float4(i.Pos, 1));
    o.WP = worldPos.xyz;
    o.Pos = mul(g_ViewProj, worldPos);
    o.N = normalize(mul((float3x3)iw, i.Nrm));
    o.UV = i.UV;
    return o;
}
)";

// Shadow indirect VS: depth-only, world matrices from StructuredBuffer
static const char* g_VS_ShadowIndirect = R"(
cbuffer Frame : register(b0) { float4x4 g_ViewProj; float4 g_CameraPos; float4 g_Ambient; uint g_LightCount; float _p0; float _p1; float _p2; float4x4 g_ShadowMapUVDepth[4]; float4 g_CascadeSplits; };
StructuredBuffer<float4x4> g_WorldMatrices : register(t5);
StructuredBuffer<uint> g_Indices : register(t6);
struct VSIn { float3 Pos : ATTRIB0; float3 Nrm : ATTRIB1; float2 UV : ATTRIB2; float4 Tan : ATTRIB3; };
struct VSOut { float4 Pos : SV_POSITION; };
VSOut main(VSIn i, uint instID : SV_InstanceID) {
    VSOut o;
    float4x4 iw = g_WorldMatrices[g_Indices[instID]];
    o.Pos = mul(g_ViewProj, mul(iw, float4(i.Pos, 1)));
    return o;
}
)";

// Billboard vertex shader: pre-computed quad vertices
static const char* g_VS_Billboard = R"(
cbuffer Frame : register(b0) { float4x4 g_ViewProj; float4 g_CameraPos; float4 g_Ambient; uint g_LightCount; float _p0; float _p1; float _p2; float4x4 g_ShadowMapUVDepth[4]; float4 g_CascadeSplits; };
struct VSIn  { float3 Pos : ATTRIB0; float2 UV : ATTRIB1; float4 Col : ATTRIB2; };
struct VSOut { float4 Pos : SV_POSITION; float2 UV : TEXCOORD0; float4 Col : TEXCOORD1; };
VSOut main(VSIn i) {
	VSOut o;
	o.Pos = mul(g_ViewProj, float4(i.Pos, 1));
	o.UV  = i.UV;
	o.Col = i.Col;
	return o;
}
)";

static const char* g_PS_Billboard = R"(
Texture2D    g_BillTex      : register(t0);
SamplerState g_BillSampler  : register(s0);
struct PSIn { float4 Pos : SV_POSITION; float2 UV : TEXCOORD0; float4 Col : TEXCOORD1; };
float4 main(PSIn i) : SV_TARGET { return g_BillTex.Sample(g_BillSampler, i.UV) * i.Col; }
)";

// Skybox vertex shader: w=0 strips translation, xyww forces far depth, corner-color lookup
static const char* g_VS_Skybox = R"(
cbuffer Frame : register(b0)
{
    float4x4 g_ViewProj;
    float4   g_CameraPos;
    float4   g_Ambient;
    uint     g_LightCount;
    float    _p0;
    float    _p1;
    float    _p2;
    float4x4 g_ShadowMapUVDepth[4]; float4 g_CascadeSplits;
};
cbuffer SkyColors : register(b1) { float4 g_Corners[8]; };
struct VSIn  { float3 Pos : ATTRIB0; };
struct VSOut { float4 Pos : SV_POSITION; float4 Col : TEXCOORD0; float3 TexCoord : TEXCOORD1; };
VSOut main(VSIn i) {
	VSOut o;
	o.Pos = mul(g_ViewProj, float4(i.Pos, 0.0f)).xyww;
	int idx = (i.Pos.x > 0.0f ? 1 : 0) + (i.Pos.y > 0.0f ? 2 : 0) + (i.Pos.z > 0.0f ? 4 : 0);
	o.Col = g_Corners[idx];
	o.TexCoord = i.Pos;
	return o;
}
)";

static const char* g_PS_Skybox = R"(
struct PSIn { float4 Pos : SV_POSITION; float4 Col : TEXCOORD0; };
float4 main(PSIn i) : SV_TARGET { return i.Col; }
)";

static const char* g_PS_SkyboxCube = R"(
TextureCube g_SkyTex : register(t0);
SamplerState g_SkySamp : register(s0);
struct PSIn { float4 Pos : SV_POSITION; float4 Col : TEXCOORD0; float3 TexCoord : TEXCOORD1; };
float4 main(PSIn i) : SV_TARGET { return g_SkyTex.Sample(g_SkySamp, i.TexCoord); }
)";

static const char* g_PS = R"(
Texture2D    t_BC         : register(t0);
Texture2D    t_NormalMap  : register(t1);
Texture2D    t_MR         : register(t2);
Texture2D    t_EmissiveMap: register(t3);
SamplerState t_BC_sampler : register(s0);
Texture2DArray g_ShadowMap        : register(t4);
SamplerComparisonState g_ShadowMap_sampler : register(s4);
// Prefiltered sky environment (see RenderSubsystem::prepareEnvironment): one mip per
// roughness level, built by the GGX prefilter compute shader.
TextureCube  g_SkyEnv     : register(t5);
SamplerState g_SkyEnv_sampler : register(s5);


cbuffer Frame : register(b0)
{
    float4x4 g_ViewProj;
    float4   g_CameraPos;
    float4   g_Ambient;
    uint     g_LightCount;
    float    _p0;
    float    _p1;
    float    _p2;
    float4x4 g_ShadowMapUVDepth[4]; float4 g_CascadeSplits;
    float4   g_SkyCorners[8];   // skybox corners -> ambient irradiance, see SkyIrradiance
    float4   g_EnvParams;       // x = sky IBL on, y = mip scale, z = debug, w = intensity
};

// Diffuse irradiance of the analytic sky (the same closed form the mesh shader and
// ray tracing paths use). The 8-corner sky is trilinear in the direction, so to
// first order it is A + dot(B, l) with A the corner mean and B_i the mean of the
// corners carrying the i-th bit minus A; its cosine-weighted average around n is
// A + (2/3) dot(B, n). That turns the flat ambient into environment lighting that
// is bright and cool from above, ground-tinted from below.
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

// ---------------------------------------------------------------------------
// Specular IBL (split-sum)
// ---------------------------------------------------------------------------
// Karis' analytic fit of the second half of the split-sum approximation: the BRDF
// integral against a white environment, as a (scale, bias) pair applied to F0. It
// replaces a precomputed BRDF LUT with a handful of multiply-adds.
float2 EnvBRDFApprox(float NdotV, float roughness)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4( 1.0,  0.0425,  1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return float2(-1.04, 1.04) * a004 + r.zw;
}

// Radiance the specular lobe gathers from the environment around the reflection
// vector.
//
// The cube's mip chain is a plain box blur, so there is no per-roughness level to
// look up: the level is chosen by matching the lobe's solid angle against a texel's,
// which is the same rule the ray tracing path uses for its sky hits
// (SkyLodForLobe). The GGX lobe's half-angle is about alpha = roughness^2.
// The first half of the split-sum (that prefiltered radiance) times the second half
// (EnvBRDFApprox) is the whole approximation. g_EnvParams.y scales the mip (a look
// knob), .w the intensity, .x the enable.
float3 SkySpecular(float3 N, float3 V, float3 F0, float roughness)
{
    if (g_EnvParams.x < 0.5)
        return 0.0;
    // TextureCube has no 3-output GetDimensions: the four-argument form is
    // (MipLevel, Width, Height, NumberOfLevels), so the level to query comes first.
    uint w = 1, h = 1, levels = 1;
    g_SkyEnv.GetDimensions(0, w, h, levels);
    const float alpha = max(roughness * roughness, 1e-3);
    const float texelSolid = 2.0 / max((float)w * (float)w, 1.0); // ~4*pi/6 / (w*w)
    const float lobeSolid = 3.14159265358979 * alpha * alpha;
    float lod = 0.5 * log2(max(lobeSolid / texelSolid, 1.0));
    lod = clamp(lod * g_EnvParams.y, 0.0, max((float)levels - 1.0, 0.0));
    const float3 R = reflect(-V, N);
    const float3 pre = g_SkyEnv.SampleLevel(g_SkyEnv_sampler, R, lod).rgb;
    const float2 ab = EnvBRDFApprox(saturate(dot(N, V)), max(roughness, 0.002));
    return pre * (F0 * ab.x + ab.y) * g_EnvParams.w;
}

struct Light
{
    float4 CI;
    float4 DT;
    float4 PR;
    float4 CA;
};

cbuffer Lights : register(b1)
{
    Light g_Lights[8];
};

cbuffer Object : register(b2)
{
    float4x4 g_World;
    float4x4 g_Normal;
    float4   g_BaseColor;
    float4   g_MetallicRough;
    float4   g_Emissive;
    float4   g_MapFlags;      // x = has normal map, y = has MR map, z = has emissive map, w = raw-sample debug
    // Per-channel sign applied to the decoded tangent-space normal map (xyz used,
    // z stays 1). A run-time value rather than a compile-time constant so the
    // convention can be dialled in live (DebugUI "NM Flip X/Y") instead of by
    // rebuilding - the tangent frame here is built from screen-space derivatives,
    // so the sign is a property of that frame, not of the asset.
    float4   g_NMSign;
};

struct PSIn
{
    float4 Pos : SV_POSITION;
    float3 WP  : TEXCOORD0;
    float3 N   : TEXCOORD1;
    float2 UV  : TEXCOORD2;
};

float3 DoLight(float3 wp, float3 N, float3 V, float3 bc, float m, float r)
{
    // F0 of the dielectric/metal mix, used by the direct specular lobe and by the
    // environment below.
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), bc, m);
    // Ambient: the sky's irradiance along the normal (environment lighting instead
    // of a flat colour), plus the specular lobe's share of the environment. g_Ambient.a
    // still scales both. The specular term is what lights a metal: the irradiance
    // term above only ever fed the diffuse lobe, so metals and polished dielectrics
    // had no ambient specular at all.
    float3 col = SkyIrradiance(N) * g_Ambient.a * bc + SkySpecular(N, V, F0, r) * g_Ambient.a;
    for (uint i = 0; i < g_LightCount && i < 8; i++)
    {
        Light L = g_Lights[i];
        float3 lc = L.CI.rgb;
        float  intensity = L.CI.a;
        float  lt = L.DT.a;
        float3 Ldir;
        float  att = 1.0;

        if (lt < 0.5)
        {
            Ldir = normalize(-L.DT.xyz);
        }
        else if (lt < 1.5)
        {
            float3 toL = L.PR.xyz - wp;
            float d = length(toL);
            Ldir = toL / max(d, 0.001);
            att = 1.0 / (1.0 + 0.09 * d + 0.032 * d * d);
        }
        else
        {
            float3 toL = L.PR.xyz - wp;
            float d = length(toL);
            Ldir = toL / max(d, 0.001);
            float ca = dot(normalize(-L.DT.xyz), -Ldir);
            float inner = L.CA.x;
            float outer = L.CA.y;
            float sf = saturate((ca - outer) / max(inner - outer, 0.001));
            att = sf / (1.0 + 0.09 * d + 0.032 * d * d);
        }

	float NdotL = dot(N, Ldir) * 0.5 + 0.5;
		float3 H = normalize(Ldir + V);
		float specExp = max(1.0, (1.0 - r) * 256.0);
		float spec = pow(max(dot(N, H), 0.001), specExp);
        float3 diff = bc * (1.0 - m);
        float3 specC = F0;
        float shadow = 1.0;
        if (lt < 0.5) {
            float camZ = abs(mul(g_ViewProj, float4(wp, 1.0)).w);
            uint c = 0;
            if (camZ > g_CascadeSplits.x) c = 1;
            if (camZ > g_CascadeSplits.y) c = 2;
            if (camZ > g_CascadeSplits.z) c = 3;
            if (c < 4) {
                float4 sc = mul(g_ShadowMapUVDepth[c], float4(wp, 1.0));
                sc.xyz /= sc.w;
                float bias = 0.005 + 0.01 * (1.0 - NdotL);
                shadow = g_ShadowMap.SampleCmpLevelZero(g_ShadowMap_sampler, float3(sc.xy, c), sc.z - bias).r;
            }
        }
        col += (diff * NdotL + specC * spec) * lc * intensity * att * shadow;
    }
    return col;
}

// Normal mapping from the sampled tangent-space normal.
//
// The tangent frame is built from screen-space derivatives (a cotangent frame)
// rather than from the mesh's TANGENT attribute, so the sign that converts the
// asset's tangent-space convention into this frame is not derivable from the asset
// alone: it comes from g_NMSign, which the DebugUI exposes live ("NM Flip X/Y").
// The mesh shader path (MeshShaderSubsystem) carries the same note.
float3 NormalMapped(float3 N, float3 wp, float2 uv, float3 sampledNormal) {
    float3 dp1 = ddx(wp);
    float3 dp2 = ddy(wp);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);
    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    // Epsilon: degenerate UVs give a zero frame and rsqrt(0) would make the normal
    // NaN, which renders as black.
    float invmax = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
    float3 n = sampledNormal * g_NMSign.xyz;
    return normalize(mul(n, float3x3(T * invmax, B * invmax, N)));
}

float4 main(PSIn i) : SV_TARGET
{
    float4 tex = t_BC.Sample(t_BC_sampler, i.UV);
    float3 bc = tex.rgb * g_BaseColor.rgb;
    float  a  = tex.a * g_BaseColor.a;

    // Diagnostic (toggled by RenderSubsystem::setNormalMapDebug): output the raw
    // normal-map sample and nothing else. No tangent frame, no lighting - so this
    // alone answers whether the classic path samples the right texture. A flat
    // (128,128,255) blue is the neutral fallback, a recognisable normal map means
    // the binding is correct, noise means it is not.
    if (g_MapFlags.w > 0.5) {
        return float4(t_NormalMap.Sample(t_BC_sampler, i.UV).xyz, 1.0);
    }

    float3 N = normalize(i.N);
    if (g_MapFlags.x > 0.5) {
        // Decode the texel to tangent space, then rotate it into the frame the
        // derivative-built tangent/bitangent spans.
        float3 sn = t_NormalMap.Sample(t_BC_sampler, i.UV).xyz * 2.0 - 1.0;
        N = NormalMapped(N, i.WP, i.UV, sn);
    }

    float3 V = normalize(g_CameraPos.xyz - i.WP);
    // Debug: the environment cube itself (a mirror ball), so its orientation can be
    // compared against the skybox on screen without any lighting in the way.
    if (g_EnvParams.z > 0.5) {
        return float4(g_SkyEnv.SampleLevel(g_SkyEnv_sampler, reflect(-V, N), 0).rgb, 1.0);
    }
    float m = g_MetallicRough.x;
    float r = g_MetallicRough.y;
    float3 lit = DoLight(i.WP, N, V, bc, m, r);
    // Emissive is self-emission: added after lighting (not tinted by the light).
    return float4(lit + g_Emissive.rgb * g_Emissive.w, a);
}
)";

// G-buffer pixel shader: writes albedo (SV_Target0) and world normal (SV_Target1).
// g_ShadowMap is declared (but unused) so the resource layout matches the PBR PSOs.
static const char* g_PS_GBuffer = R"(
Texture2D    t_BC         : register(t0);
Texture2D    t_NormalMap  : register(t1);
Texture2D    t_MR         : register(t2);
Texture2D    t_EmissiveMap: register(t3);
SamplerState t_BC_sampler : register(s0);
Texture2DArray g_ShadowMap        : register(t4);
SamplerComparisonState g_ShadowMap_sampler : register(s4);

cbuffer Frame : register(b0)
{
    float4x4 g_ViewProj;
    float4   g_CameraPos;
    float4   g_Ambient;
    uint     g_LightCount;
    float    _p0;
    float    _p1;
    float    _p2;
    float4x4 g_ShadowMapUVDepth[4]; float4 g_CascadeSplits;
};

cbuffer Object : register(b2)
{
    float4x4 g_World;
    float4x4 g_Normal;
    float4   g_BaseColor;
    float4   g_MetallicRough;
    float4   g_Emissive;
    float4   g_MapFlags;      // x = has normal map, y = has MR map, z = has emissive map, w = raw-sample debug
    // Per-channel sign applied to the decoded tangent-space normal map (xyz used,
    // z stays 1). A run-time value rather than a compile-time constant so the
    // convention can be dialled in live (DebugUI "NM Flip X/Y") instead of by
    // rebuilding - the tangent frame here is built from screen-space derivatives,
    // so the sign is a property of that frame, not of the asset.
    float4   g_NMSign;
};

struct PSIn
{
    float4 Pos : SV_POSITION;
    float3 WP  : TEXCOORD0;
    float3 N   : TEXCOORD1;
    float2 UV  : TEXCOORD2;
};

struct PSOut
{
    float4 Color : SV_Target0; ///< Albedo (RGBA8_SRGB).
    float4 Norm  : SV_Target1; ///< World normal in rgb, roughness in alpha (RGBA16_FLOAT).
    float4 Emis  : SV_Target2; ///< Emissive color * intensity (RGBA16_FLOAT, rgb used).
};

// Same note as in the forward shader: the sign belongs to the derivative-built
// tangent frame, not to the asset, and comes from g_NMSign at run time.
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
    float3 n = sampledNormal * g_NMSign.xyz;
    return normalize(mul(n, float3x3(T * invmax, B * invmax, N)));
}

PSOut main(PSIn i)
{
    PSOut o;
    o.Color = t_BC.Sample(t_BC_sampler, i.UV) * g_BaseColor;
    // Diagnostic (RenderSubsystem::setNormalMapDebug): write the raw normal-map
    // sample into the normal and emissive targets. The normal target is linear
    // RGBA16F, and the hybrid compose adds the emissive on top, so both the G-buffer
    // debug view and the final image show it unmodified. No tangent frame is
    // involved, which is the point: this isolates the sampling from the TBN.
    if (g_MapFlags.w > 0.5) {
        float4 raw = float4(t_NormalMap.Sample(t_BC_sampler, i.UV).xyz, 1.0);
        o.Norm = raw;
        o.Emis = raw;
        return o;
    }
    float3 N = normalize(i.N);
    if (g_MapFlags.x > 0.5) {
        float3 sn = t_NormalMap.Sample(t_BC_sampler, i.UV).xyz * 2.0 - 1.0;
        N = NormalMapped(N, i.WP, i.UV, sn);
    }
    o.Norm  = float4(N, g_MetallicRough.y);
    o.Emis  = float4(g_Emissive.rgb * g_Emissive.w, 1.0);
    return o;
}
)";

// ===================================================================
// G-buffer MSAA resolve (compute). Replaces the hardware
// ResolveSubresource, which only supports a subset of formats (classic
// D3D12 ResolveSubresource is color-only; depth/sRGB paths are
// backend-dependent and fail command-list close with "Failed to close
// the command list"). Color is decoded sRGB->linear and averaged over
// all samples (matching the hardware AVERAGE resolve), normal/emissive
// are averaged in linear, depth takes the nearest sample (min).
// Outputs: color/normal/emissive = RGBA16F, depth = R32F (depth cannot
// be a UAV in D3D12, and sRGB formats cannot be UAVs either).
// ===================================================================
static const char* g_ResolveCS = R"(
Texture2DMS<float4, 16> g_MSAAColor    : register(t0);
Texture2DMS<float4, 16> g_MSAANormal   : register(t1);
Texture2DMS<float4, 16> g_MSAAEmissive : register(t2);
Texture2DMS<float,  16> g_MSAADepth    : register(t3);
RWTexture2D<float4> g_OutColor    : register(u0);
RWTexture2D<float4> g_OutNormal   : register(u1);
RWTexture2D<float4> g_OutEmissive : register(u2);
RWTexture2D<float>  g_OutDepth    : register(u3);

float3 SRGBToLinear(float3 c)
{
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

[numthreads(8, 8, 1)]
void CSMain(uint2 DTid : SV_DispatchThreadID)
{
    uint2 Dim; uint Samples;
    g_MSAAColor.GetDimensions(Dim.x, Dim.y, Samples);
    if (DTid.x >= Dim.x || DTid.y >= Dim.y) return;
    int3 tc = int3(DTid, 0);
    float4 cSum = float4(0, 0, 0, 0);
    float4 nSum = float4(0, 0, 0, 0);
    float4 eSum = float4(0, 0, 0, 0);
    float  dMin = 1.0;
    float  covered = 0.0;
    [loop]
    for (uint s = 0; s < min(Samples, 16u); ++s)
    {
        // Coverage matters: samples the geometry did not touch still hold the
        // clear value - black for every colour target - so averaging them in
        // darkened each silhouette pixel by the uncovered fraction (at 8x MSAA a
        // half covered edge pixel lost half of its albedo). That is the dark seam
        // that showed up around every object in the hybrid path, since only there
        // is the resolved G-buffer actually shaded. Only samples that carry
        // geometry contribute to the resolve.
        const float d = g_MSAADepth.Load(tc, s);
        dMin = min(dMin, d);
        if (d >= 1.0)
            continue; // background sample

        float4 c = g_MSAAColor.Load(tc, s);
        cSum.rgb += SRGBToLinear(c.rgb); // MSAA storage is sRGB-encoded; average in linear
        cSum.a   += c.a;
        nSum += g_MSAANormal.Load(tc, s);
        eSum += g_MSAAEmissive.Load(tc, s);
        covered += 1.0;
    }
    // No covered sample at all: the pixel is background, and reporting depth 1
    // lets the compose pass leave the skybox untouched.
    const float inv = (covered > 0.0) ? (1.0 / covered) : 0.0;
    g_OutColor[DTid]    = cSum * inv;
    g_OutNormal[DTid]   = nSum * inv;
    g_OutEmissive[DTid] = eSum * inv;
    g_OutDepth[DTid]    = (covered > 0.0) ? dMin : 1.0;
}
)";

// ===================================================================
// Conversion helpers
// ===================================================================

static D::TEXTURE_FORMAT toDFmt(TextureFormat f) {
	switch (f) {
	case TextureFormat::RGBA8_UNorm: return D::TEX_FORMAT_RGBA8_UNORM;
	case TextureFormat::RGBA8_UNorm_SRGB: return D::TEX_FORMAT_RGBA8_UNORM_SRGB;
	case TextureFormat::BGRA8_UNorm: return D::TEX_FORMAT_BGRA8_UNORM;
	case TextureFormat::BGRA8_UNorm_SRGB: return D::TEX_FORMAT_BGRA8_UNORM_SRGB;
	case TextureFormat::R8_UNorm: return D::TEX_FORMAT_R8_UNORM;
	case TextureFormat::RG8_UNorm: return D::TEX_FORMAT_RG8_UNORM;
	case TextureFormat::R32_Float: return D::TEX_FORMAT_R32_FLOAT;
	case TextureFormat::RG32_Float: return D::TEX_FORMAT_RG32_FLOAT;
	case TextureFormat::RGBA16_Float: return D::TEX_FORMAT_RGBA16_FLOAT;
	case TextureFormat::RGBA32_Float: return D::TEX_FORMAT_RGBA32_FLOAT;
	case TextureFormat::D32_Float: return D::TEX_FORMAT_D32_FLOAT;
	case TextureFormat::D24_UNorm_S8_UInt: return D::TEX_FORMAT_D24_UNORM_S8_UINT;
	default: return D::TEX_FORMAT_UNKNOWN;
	}
}
static D::SHADER_TYPE toDST(ShaderStage s) {
	switch (s) {
	case ShaderStage::Vertex: return D::SHADER_TYPE_VERTEX; case ShaderStage::Pixel: return D::SHADER_TYPE_PIXEL;
	case ShaderStage::Geometry: return D::SHADER_TYPE_GEOMETRY; case ShaderStage::Compute: return D::SHADER_TYPE_COMPUTE;
	} return D::SHADER_TYPE_VERTEX;
}
static D::PRIMITIVE_TOPOLOGY toDTopo(PrimitiveTopology t) {
	switch (t) {
	case PrimitiveTopology::TriangleList: return D::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	case PrimitiveTopology::TriangleStrip: return D::PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	case PrimitiveTopology::LineList: return D::PRIMITIVE_TOPOLOGY_LINE_LIST;
	case PrimitiveTopology::PointList: return D::PRIMITIVE_TOPOLOGY_POINT_LIST;
	} return D::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}
static D::CULL_MODE toDCull(CullMode m) {
	switch (m) { case CullMode::None: return D::CULL_MODE_NONE; case CullMode::Front: return D::CULL_MODE_FRONT; case CullMode::Back: return D::CULL_MODE_BACK; } return D::CULL_MODE_BACK;
}
static D::FILL_MODE toDFill(FillMode m) { return m == FillMode::Wireframe ? D::FILL_MODE_WIREFRAME : D::FILL_MODE_SOLID; }
static D::COMPARISON_FUNCTION toDCmp(CompareFunc f) {
	switch (f) {
	case CompareFunc::Never: return D::COMPARISON_FUNC_NEVER; case CompareFunc::Less: return D::COMPARISON_FUNC_LESS;
	case CompareFunc::Equal: return D::COMPARISON_FUNC_EQUAL; case CompareFunc::LessEqual: return D::COMPARISON_FUNC_LESS_EQUAL;
	case CompareFunc::Greater: return D::COMPARISON_FUNC_GREATER; case CompareFunc::NotEqual: return D::COMPARISON_FUNC_NOT_EQUAL;
	case CompareFunc::GreaterEqual: return D::COMPARISON_FUNC_GREATER_EQUAL; case CompareFunc::Always: return D::COMPARISON_FUNC_ALWAYS;
	} return D::COMPARISON_FUNC_LESS;
}
static D::FILTER_TYPE toDFilt(FilterMode m) { return m == FilterMode::Point ? D::FILTER_TYPE_POINT : m == FilterMode::Anisotropic ? D::FILTER_TYPE_ANISOTROPIC : D::FILTER_TYPE_LINEAR; }
static D::TEXTURE_ADDRESS_MODE toDAddr(AddressMode m) {
	switch (m) { case AddressMode::Wrap: return D::TEXTURE_ADDRESS_WRAP; case AddressMode::Mirror: return D::TEXTURE_ADDRESS_MIRROR; case AddressMode::Clamp: return D::TEXTURE_ADDRESS_CLAMP; case AddressMode::Border: return D::TEXTURE_ADDRESS_BORDER; } return D::TEXTURE_ADDRESS_WRAP;
}

// ===================================================================
// Diligent Engine Callbacks
// ===================================================================

static void DILIGENT_CALL_TYPE DiligentDebugMsgCallback(enum D::DEBUG_MESSAGE_SEVERITY severity, const D::Char* message, const D::Char* function, const D::Char* file, int line) {
	static bool initialized = false;
	static const char* diligentName = "DiligentEngine";

	if (!initialized) [[unlikely]] {
		if (!Log::isInitialized() || !Log::getLogger()) {
			ECrash("Logger is unavailable");
		}
		auto logger = Log::getLogger()->clone(diligentName);
		spdlog::register_logger(logger);
		initialized = true;
	}

	switch (severity)
	{
	default:
		EDebug("Unknown message severity from Diligent Engine: {}", static_cast<UInt32>(severity));
	case D::DEBUG_MESSAGE_SEVERITY_INFO:
#ifdef EE_DEBUG
		spdlog::get(diligentName)->log(spdlog::source_loc{ file, line, function }, spdlog::level::info, message);
#endif
		break;
	case D::DEBUG_MESSAGE_SEVERITY_WARNING:
		spdlog::get(diligentName)->log(spdlog::source_loc{ file, line, function }, spdlog::level::warn, message); break;
	case D::DEBUG_MESSAGE_SEVERITY_ERROR:
		spdlog::get(diligentName)->log(spdlog::source_loc{ file, line, function }, spdlog::level::err, message); break;
	case D::DEBUG_MESSAGE_SEVERITY_FATAL_ERROR:
		spdlog::get(diligentName)->log(spdlog::source_loc{ file, line, function }, spdlog::level::critical, message); break;
	}
}

// ===================================================================
// Resource data structs
// ===================================================================

struct ShaderData { D::RefCntAutoPtr<D::IShader> shader; ShaderStage stage = ShaderStage::Vertex; };
struct PSOData { D::RefCntAutoPtr<D::IPipelineState> pso; D::RefCntAutoPtr<D::IShaderResourceBinding> srb; PipelineStateDesc desc; };
struct MeshData { D::RefCntAutoPtr<D::IBuffer> vb; D::RefCntAutoPtr<D::IBuffer> ib; UInt32 vc = 0; UInt32 ic = 0; Vector<SubMesh> sub; Vector<Vertex> cpuVertices; Vector<UInt32> cpuIndices; };
struct TexData { D::RefCntAutoPtr<D::ITexture> tex; D::RefCntAutoPtr<D::ITextureView> srv; D::RefCntAutoPtr<D::ITextureView> rtv; D::RefCntAutoPtr<D::ITextureView> uav; D::RefCntAutoPtr<D::ITextureView> dsv; TextureDesc desc; };
struct SamplerData { D::RefCntAutoPtr<D::ISampler> sampler; SamplerDesc desc; };
struct MatData { MaterialDesc desc; PSOHandle pso; D::RefCntAutoPtr<D::IShaderResourceBinding> srb; D::RefCntAutoPtr<D::IShaderResourceBinding> gbufSRB; D::RefCntAutoPtr<D::IBuffer> objCB; };
struct CamData { CameraDesc desc; Mat4 view; Mat4 proj; F32 aspect = 16.0f / 9.0f; };
struct RenderLightData { LightDesc desc; };
struct ModelData { ModelLoadResult result; };

// ===================================================================
// Mesh preprocessing
// ===================================================================

UInt32 RenderSubsystem::orientNormalsToWinding(Vector<Vertex>& vertices, const Vector<UInt32>& indices) {
	const Size vertexCount = vertices.size();
	if (vertexCount == 0 || indices.size() < 3) return 0;

	// Area-weighted (unnormalized cross product) so large triangles dominate and
	// slivers cannot outvote them.
	Vector<Vec3> winding(vertexCount, Vec3(0.0f));
	for (Size i = 0; i + 2 < indices.size(); i += 3) {
		const UInt32 i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];
		if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) continue;
		const Vec3 n = glm::cross(vertices[i1].position - vertices[i0].position,
		                          vertices[i2].position - vertices[i0].position);
		winding[i0] += n; winding[i1] += n; winding[i2] += n;
	}

	UInt32 flipped = 0;
	for (Size v = 0; v < vertexCount; ++v) {
		// Incident triangles that cancel out (a folded fan, a degenerate vertex)
		// carry no orientation information: leave the normal untouched.
		if (glm::dot(winding[v], winding[v]) < 1e-12f) continue;
		if (glm::dot(vertices[v].normal, winding[v]) < 0.0f) {
			vertices[v].normal = -vertices[v].normal;
			++flipped;
		}
	}
	return flipped;
}

// ===================================================================
// RenderBackend
// ===================================================================

struct RenderSubsystem::RenderBackend {
	D::RefCntAutoPtr<D::IEngineFactory> factory;
	D::RefCntAutoPtr<D::IRenderDevice>  device;
	D::RefCntAutoPtr<D::IDeviceContext> ctx;
	D::RefCntAutoPtr<D::ISwapChain>     sc;
	D::RefCntAutoPtr<D::ITextureView>   dsv;
	void* wnd = nullptr;
	UInt32 w = 1280, h = 720;
	RenderBackendType backendType = RenderBackendType::Auto;

	Detail::ResourcePool<ShaderData>       shaders{ 256 };
	Detail::ResourcePool<PSOData>          psos{ 256 };
	Detail::ResourcePool<MeshData>         meshes{ 4096 };
	Detail::ResourcePool<TexData>          textures{ 4096 };
	Detail::ResourcePool<SamplerData>      samplers{ 64 };
	Detail::ResourcePool<MatData>          materials{ 4096 };
	Detail::ResourcePool<CamData>          cameras{ 16 };
	Detail::ResourcePool<RenderLightData>  lights{ 16 };
	Detail::ResourcePool<ModelData>        models{ 128 };
	Mutex m_loadMutex; ///< Serializes async model loading (pool + GPU resource creation)

	ShaderHandle  defVS, defPS, defVS_Inst, defVS_Indirect, defVS_ShadowIndirect, bboardVS, bboardPS, skyVS, skyPS, skyCubePS, gbufPS;
	PSOHandle     defPSO, defPSO_wire, defPSO_Inst, defPSO_Inst_wire, defPSO_Indirect, defPSO_ShadowIndirect, bboardPSO, skyPSO, skyCubePSO, shadowPSO, gbufPSO, gbufPSO_Inst;
	SamplerHandle defSamp, bboardSamp;
	TextureHandle defTex, fogTex, flatNormalTex;
	MeshHandle    bboardMesh;
	D::RefCntAutoPtr<D::ITextureView> whiteSRV, fogSRV, flatNormalSRV;
	MeshData      skyMesh;
	D::RefCntAutoPtr<D::IBuffer> skyCB;
	D::RefCntAutoPtr<D::ITexture> skyCubeTex;
	D::RefCntAutoPtr<D::ITextureView> skyCubeSRV;
	Optional<RenderSubsystem::SkyboxDesc> skyDesc;

	/// @brief Copy the skybox corners into the frame constants.
	///
	/// The shaders derive the ambient light from them (SkyIrradiance); when no
	/// skybox is set they are filled with the ambient colour, which degenerates the
	/// irradiance back to the previous flat ambient instead of turning it black.

	/// @brief Debug: shade with the raw normal-map sample of the classic path.
	///
	/// Written into the per-object mapFlags.w so g_PS / g_PS_GBuffer can output the
	/// sampled texel directly, with no tangent frame involved. That answers "does
	/// this path sample the right texture at all" without the TBN in the way: a flat
	/// (128,128,255) blue is the neutral fallback, a recognisable normal map means
	/// the binding is right, noise means it is not.
	bool normalMapDebug = false;
	/// @brief Sign applied to the classic path's decoded tangent-space normal map.
	///
	/// The tangent frame there is built from screen-space derivatives, so the sign
	/// that maps an asset's tangent-space normal into that frame is a convention
	/// (glTF's +Y-up / +V-down textures give (1,-1)) rather than something derivable
	/// from the asset. Kept as data so it can be dialled in live.
	Vec4 nmSign = Vec4(1.0f, -1.0f, 1.0f, 0.0f);

	// ---- Sky environment (specular IBL) --------------------------------
	//
	// The sky is drawn into the faces of envCube - through the ordinary sky shaders,
	// so it is the same sky the visible skybox shows - and its mip chain is generated
	// from there. The render targets have to be around from createDefaults() on, before
	// any shading PSO exists, because those bind the cube as a static variable at
	// creation time; an unbound resource would be sampled as whatever the descriptor
	// heap holds (see the note in mkPSO). Only the *content* is rebuilt, and until the
	// first build the cube is zero, so the specular term simply contributes nothing.
	static constexpr UInt32 kEnvCubeSize = 128;
	static constexpr UInt32 kEnvCubeMips = 8; // 128 -> 1
	D::RefCntAutoPtr<D::ITexture>     envCube;
	D::RefCntAutoPtr<D::ITextureView> envCubeSRV;
	D::RefCntAutoPtr<D::ITextureView> envCubeRTV[6];
	PSOHandle envCornerPSO, envCubeSkyPSO;
	bool envViewsOk = true;     ///< False when a cube face view could not be created.
	bool envBuildWarned = false;///< One-shot warning when the environment cannot be built yet.
	bool envDirty = true;       ///< Sky changed: rebuild the environment cube before the next frame.
	bool envEnabled = true;     ///< Specular IBL on/off (FrameConstants::envParams.x).
	F32  envMipScale = 1.0f;    ///< Roughness -> mip scale (envParams.y).
	bool envDebug = false;      ///< Shade with the raw environment sample (envParams.z).
	F32  envIntensity = 1.0f;   ///< Specular IBL intensity (envParams.w).
	/// Cube-map orientation fixes (see envFaceMatrix): which mirrors the built
	/// environment needs so that it matches the visible sky. Kept as data rather than
	/// as a baked-in correction until the right combination is confirmed on screen.
	bool envFlipU = false;      ///< Mirror every face horizontally.
	bool envFlipV = false;      ///< Mirror every face vertically.
	bool envMirrorY = false;    ///< Mirror the sky about the world's up axis.

	/// @brief Fill the frame constants the shaders derive lighting from.
	///
	/// The skybox corners drive the ambient irradiance (SkyIrradiance); when no skybox
	/// is set they are filled with the ambient colour, which degenerates the irradiance
	/// back to the previous flat ambient instead of turning it black. The same call
	/// carries the specular IBL parameters, because every site that builds a
	/// FrameConstants for shading needs both.
	void applyEnvironment(FrameConstants& fc) const {
		for (int i = 0; i < 8; ++i) fc.skyCorners[i] = skyDesc.has_value() ? skyDesc->corners[i] : fc.ambient;
		fc.envParams = Vec4(envEnabled ? 1.0f : 0.0f, envMipScale, envDebug ? 1.0f : 0.0f, envIntensity);
	}

	/// @brief View-projection that maps a direction onto cube face @p face.
	///
	/// Written out by hand instead of built from a look-at plus a projection: the
	/// cube addressing convention fixes the sign of the two screen axes per face
	/// (for +X: u = -z and v = -y), and the skybox vertex shader only uses x, y and w
	/// of the result (it forces z = w), so there is nothing a camera matrix would add
	/// except room for a handedness mistake. The faces are (D3D/GL order) +X, -X, +Y,
	/// -Y, +Z, -Z: the addressing convention fixed by the cube map.
	///
	/// The three optional mirrors are the escape hatch for exactly that convention:
	/// getting a sign wrong there produces an environment that is mirrored, which no
	/// amount of shader-side care can fix, and it is far cheaper to dial in live (the
	/// DebugUI toggles) than to reason about. They compose as F_u * F_v * M * S, i.e.
	/// the world mirror first, then the two face-space mirrors.
	static Mat4 envFaceMatrix(UInt32 face, bool flipU, bool flipV, bool mirrorY) {
		Mat4 m(0.0f);
		// Clip x = u_cube * |major|, clip y = v_cube * |major|, clip w = major, so w
		// stays positive in front of the face and the back half is clipped away.
		switch (face) {
		case 0: m[2][0] = -1.0f; m[1][1] = -1.0f; m[0][3] = 1.0f; break;  // +X: u=-z, v=-y
		case 1: m[2][0] = 1.0f;  m[1][1] = -1.0f; m[0][3] = -1.0f; break; // -X: u= z, v=-y
		case 2: m[0][0] = 1.0f;  m[2][1] = 1.0f;  m[1][3] = 1.0f; break;  // +Y: u= x, v= z
		case 3: m[0][0] = 1.0f;  m[2][1] = -1.0f; m[1][3] = -1.0f; break; // -Y: u= x, v=-z
		case 4: m[0][0] = 1.0f;  m[1][1] = -1.0f; m[2][3] = 1.0f; break;  // +Z: u= x, v=-y
		default: m[0][0] = -1.0f; m[1][1] = -1.0f; m[2][3] = -1.0f; break; // -Z: u=-x, v=-y
		}
		if (mirrorY) { Mat4 s(1.0f); s[1][1] = -1.0f; m = m * s; } // mirror the sky about the world's up axis
		if (flipU) { Mat4 f(1.0f); f[0][0] = -1.0f; m = f * m; }   // mirror every face horizontally
		if (flipV) { Mat4 f(1.0f); f[1][1] = -1.0f; m = f * m; }   // mirror every face vertically
		return m;
	}

	/// @brief GGX sample count for a prefilter level: few samples where the lobe is
	/// huge and the target is tiny, many where the level is still sharp.

	CamData cam; CameraHandle camHandle;
	Vec4 ambient = Vec4(0.30f, 0.30f, 0.35f, 1.0f);
	LightConstants lcBuf;
	Vector<LightHandle> activeLights;

	D::RefCntAutoPtr<D::IBuffer> frameCB, lightCB, instanceCB, bboardVB, bboardIB;
	UInt64 fn = 0; UInt32 dc = 0; bool ok = false; bool wireframe = false;
	bool gBufferActive = false; ///< When true, draw()/drawInstanced() write the G-buffer instead of shaded color.
	UInt8 msaaSamples = 1;
	// G-buffer MSAA resolve (compute pass, created lazily).
	D::RefCntAutoPtr<D::IPipelineState> resolvePSO;
	D::RefCntAutoPtr<D::IShaderResourceBinding> resolveSRB;
	bool resolvePSOAttempted = false; ///< Guard: never retry a failed lazy creation per frame.
	// Ray tracing device feature request + reported capabilities.
	bool requestRayTracing = true;
	bool rtFeatureEnabled = false; ///< Whether the device actually enabled the ray tracing feature.
	bool requestMeshShaders = true;
	bool msFeatureEnabled = false; ///< Whether the device actually enabled the mesh shader feature.
	RayTracingCaps rtCaps = RayTracingCaps::None;
	UInt32 rtMaxRecursionDepth = 0;
	UInt32 rtMaxInstancesPerTLAS = 0;
	D::ITextureView* shadowDummy = nullptr;
	D::ITextureView* shadowSRV = nullptr;
	Mat4 shadowMapUVDepth[4]; Vec4 cascadeSplits = Vec4(0);
	TextureRTV overrideRTV = nullptr;
	TextureDSV overrideDSV = nullptr;
	static constexpr UInt32 MaxInstances = 1024;

	// --------------------------------------------------------------
	// Init
	// --------------------------------------------------------------

	Result<void, RenderError> init() {
		if (!wnd) return RenderError::WindowNotBound;
		int fw, fh; glfwGetFramebufferSize((GLFWwindow*)wnd, &fw, &fh);
		w = (UInt32)fw; h = (UInt32)fh;

		D::SwapChainDesc scd;
		scd.Width = w; scd.Height = h;
		scd.ColorBufferFormat = D::TEX_FORMAT_RGBA8_UNORM_SRGB;
		scd.DepthBufferFormat = D::TEX_FORMAT_D32_FLOAT;

		bool tryD3D12 = (backendType == RenderBackendType::Auto || backendType == RenderBackendType::D3D12);
		bool tryD3D11 = (backendType == RenderBackendType::Auto || backendType == RenderBackendType::D3D11);
		bool tryVk = (backendType == RenderBackendType::Auto || backendType == RenderBackendType::Vulkan);

#ifdef EE_WINDOWS
		HWND hwnd = glfwGetWin32Window((GLFWwindow*)wnd);
		if (tryD3D12) {
			auto* f = D::GetEngineFactoryD3D12();
			if (f) {
				// Debug builds enable the D3D12 validation layer so raw
				// interop commands (e.g. the OIDN zero-copy copies) report the
				// exact validation error instead of a generic close failure.
#ifdef EE_DEBUG
				const D::VALIDATION_LEVEL vlevel = D::VALIDATION_LEVEL_1;
#else
				const D::VALIDATION_LEVEL vlevel = D::VALIDATION_LEVEL_DISABLED;
#endif
				// First attempt requests the ray tracing and mesh shader features (if enabled).
				{
					D::EngineD3D12CreateInfo ci; ci.SetValidationLevel(vlevel);
					if (requestRayTracing) ci.Features.RayTracing = D::DEVICE_FEATURE_STATE_ENABLED;
					if (requestMeshShaders) ci.Features.MeshShaders = D::DEVICE_FEATURE_STATE_ENABLED;
					f->CreateDeviceAndContextsD3D12(ci, &device, &ctx);
				}
				// If the GPU/driver does not support the optional features, retry without
				// them so that rasterization keeps working on older hardware.
				if (!device && (requestRayTracing || requestMeshShaders)) {
					EWarn("D3D12 device creation with ray tracing / mesh shaders failed; retrying without optional features.");
					device.Release(); ctx.Release();
					D::EngineD3D12CreateInfo ci; ci.SetValidationLevel(vlevel);
					f->CreateDeviceAndContextsD3D12(ci, &device, &ctx);
				}
				if (device) { f->CreateSwapChainD3D12(device, ctx, scd, D::FullScreenModeDesc{}, D::NativeWindow{ hwnd }, &sc); factory = f; backendType = RenderBackendType::D3D12; }
			}
		}
		if (!device && tryD3D11) {
			auto* f = D::GetEngineFactoryD3D11();
			if (f) { D::EngineD3D11CreateInfo ci; ci.SetValidationLevel(D::VALIDATION_LEVEL_DISABLED); f->CreateDeviceAndContextsD3D11(ci, &device, &ctx); if (device) { f->CreateSwapChainD3D11(device, ctx, scd, D::FullScreenModeDesc{}, D::NativeWindow{ hwnd }, &sc); factory = f; backendType = RenderBackendType::D3D11; } }
		}
		if (!device && tryVk) {
			auto* f = D::GetEngineFactoryVk();
			if (f) {
				{
					D::EngineVkCreateInfo ci;
					if (requestRayTracing) ci.Features.RayTracing = D::DEVICE_FEATURE_STATE_ENABLED;
					if (requestMeshShaders) ci.Features.MeshShaders = D::DEVICE_FEATURE_STATE_ENABLED;
					f->CreateDeviceAndContextsVk(ci, &device, &ctx);
				}
				if (!device && (requestRayTracing || requestMeshShaders)) {
					EWarn("Vulkan device creation with ray tracing / mesh shaders failed; retrying without optional features.");
					device.Release(); ctx.Release();
					D::EngineVkCreateInfo ci;
					f->CreateDeviceAndContextsVk(ci, &device, &ctx);
				}
				if (device) { f->CreateSwapChainVk(device, ctx, scd, D::NativeWindow{ hwnd }, &sc); factory = f; backendType = RenderBackendType::Vulkan; }
			}
		}
#else
		if (tryVk) {
			auto* f = D::GetEngineFactoryVk();
			if (f) {
				{
					D::EngineVkCreateInfo ci;
					if (requestRayTracing) ci.Features.RayTracing = D::DEVICE_FEATURE_STATE_ENABLED;
					if (requestMeshShaders) ci.Features.MeshShaders = D::DEVICE_FEATURE_STATE_ENABLED;
					f->CreateDeviceAndContextsVk(ci, &device, &ctx);
				}
				if (!device && (requestRayTracing || requestMeshShaders)) {
					EWarn("Vulkan device creation with ray tracing / mesh shaders failed; retrying without optional features.");
					device.Release(); ctx.Release();
					D::EngineVkCreateInfo ci;
					f->CreateDeviceAndContextsVk(ci, &device, &ctx);
				}
				if (device) { f->CreateSwapChainVk(device, ctx, scd, D::NativeWindow{}, &sc); factory = f; backendType = RenderBackendType::Vulkan; }
			}
		}
#endif

		if (!device) return RenderError::DeviceCreationFailed;
		if (!sc) return RenderError::SwapChainCreationFailed;

		// Read ray tracing capabilities reported by the created device.
		{
			const auto& adapter = device->GetAdapterInfo();
			const auto& rt = adapter.RayTracing;
			rtFeatureEnabled = (adapter.Features.RayTracing == D::DEVICE_FEATURE_STATE_ENABLED);
			rtCaps = static_cast<RayTracingCaps>(static_cast<UInt8>(rt.CapFlags));
			rtMaxRecursionDepth = rt.MaxRecursionDepth;
			rtMaxInstancesPerTLAS = rt.MaxInstancesPerTLAS;
			msFeatureEnabled = (adapter.Features.MeshShaders == D::DEVICE_FEATURE_STATE_ENABLED);
			EInfo("RenderBackend: ray tracing {} (inline={}), mesh shaders {}",
				rtFeatureEnabled ? "enabled" : "disabled",
				hasRayTracingCap(rtCaps, RayTracingCaps::InlineRayTracing),
				msFeatureEnabled ? "enabled" : "disabled");
		}

		// Clamp the requested MSAA sample count to what the device supports for
		// the formats used by the scene and the G-buffer. The depth format
		// (D32F) is usually the tightest constraint - e.g. RDNA2 caps depth at
		// 8x while RGBA8 supports 16x, so requesting 16x would fail texture and
		// pipeline creation.
		if (msaaSamples > 1) {
			const auto maxSamplesFor = [&](D::TEXTURE_FORMAT f) -> UInt8 {
				const auto& fi = device->GetTextureFormatInfoExt(f);
				if (fi.SampleCounts & D::SAMPLE_COUNT_32) return 32;
				if (fi.SampleCounts & D::SAMPLE_COUNT_16) return 16;
				if (fi.SampleCounts & D::SAMPLE_COUNT_8)  return 8;
				if (fi.SampleCounts & D::SAMPLE_COUNT_4)  return 4;
				if (fi.SampleCounts & D::SAMPLE_COUNT_2)  return 2;
				return 1;
			};
			UInt8 cap = maxSamplesFor(scd.DepthBufferFormat);
			cap = std::min(cap, maxSamplesFor(D::TEX_FORMAT_RGBA8_UNORM_SRGB));
			cap = std::min(cap, maxSamplesFor(D::TEX_FORMAT_RGBA16_FLOAT));
			if (msaaSamples > cap) {
				EWarn("MSAA {}x requested but the device supports at most {}x for the used formats - clamped to {}x.",
					(int)msaaSamples, (int)cap, (int)cap);
				msaaSamples = cap;
			}
		}

		D::TextureDesc td; td.Name = "Depth"; td.Type = D::RESOURCE_DIM_TEX_2D; td.Width = scd.Width; td.Height = scd.Height; td.Format = scd.DepthBufferFormat; td.BindFlags = D::BIND_DEPTH_STENCIL; td.Usage = D::USAGE_DEFAULT; td.SampleCount = msaaSamples;
		D::RefCntAutoPtr<D::ITexture> dt; device->CreateTexture(td, nullptr, &dt); if (dt) dsv = dt->GetDefaultView(D::TEXTURE_VIEW_DEPTH_STENCIL);
		return {};
	}

	Result<void, RenderError> createDefaults() {
		// ---- Sky environment cube (specular IBL) -------------------------
		//
		// One cube, drawn face by face through the ordinary sky shaders and then
		// mip-mapped. Created before any shading PSO, which binds it as a static
		// variable at creation time (and it cannot be created lazily: a resource that
		// was never bound is sampled as whatever the descriptor heap holds).
		//
		// The blur of the rougher levels is a plain box mip chain. For this sky - a
		// smooth gradient between eight corner colours - that is a good stand-in for
		// the GGX prefilter, and the consumer's level selection accounts for it by
		// matching the lobe's solid angle (see SkySpecular in g_PS). A true GGX
		// importance-sampled prefilter would only differ for a high-frequency sky
		// (a sun disk, clouds) and can replace this without touching the shaders.
		{
			D::TextureDesc td;
			td.Name = "EnvCube";
			td.Type = D::RESOURCE_DIM_TEX_CUBE;
			td.Width = kEnvCubeSize; td.Height = kEnvCubeSize;
			td.Format = D::TEX_FORMAT_RGBA16_FLOAT;
			td.ArraySize = 6;
			td.MipLevels = kEnvCubeMips;
			td.BindFlags = D::BIND_RENDER_TARGET | D::BIND_SHADER_RESOURCE;
			td.MiscFlags = D::MISC_TEXTURE_FLAG_GENERATE_MIPS;
			td.Usage = D::USAGE_DEFAULT;
			device->CreateTexture(td, nullptr, &envCube);
			if (!envCube) return RenderError::TextureCreationFailed;
			envCubeSRV = envCube->GetDefaultView(D::TEXTURE_VIEW_SHADER_RESOURCE);
			for (UInt32 f = 0; f < 6; ++f) {
				// One render target per face. A cube face is a 2D-array slice, which is
				// what a render target view of it has to describe.
				D::TextureViewDesc vd;
				vd.Name = "EnvFace";
				vd.TextureDim = D::RESOURCE_DIM_TEX_2D_ARRAY;
				vd.ViewType = D::TEXTURE_VIEW_RENDER_TARGET;
				vd.Format = D::TEX_FORMAT_RGBA16_FLOAT;
				vd.MostDetailedMip = 0; vd.NumMipLevels = 1;
				vd.FirstArraySlice = f; vd.NumArraySlices = 1;
				envCube->CreateView(vd, &envCubeRTV[f]);
				if (!envCubeRTV[f]) envViewsOk = false;
			}
			// The per-face render targets are the only part of this that depends on how
			// a backend exposes the subresources of a cube. If one cannot be created,
			// fall back to "no specular IBL" (an unwritten cube is zero) instead of
			// failing the whole renderer - the cube and its SRV exist, so the PSO
			// bindings stay valid either way.
			if (!envViewsOk) {
				EWarn("RenderSubsystem: the sky environment cube face views could not be created; specular IBL is disabled.");
				envEnabled = false;
			}
		}

		ShaderDesc sd; sd.entryPoint = "main";
		{ sd.stage = ShaderStage::Vertex; sd.source = g_VS; auto r = mkShader(sd); if (r.isErr()) return r.error(); defVS = r.value(); }
		{ sd.stage = ShaderStage::Vertex; sd.source = g_VS_Inst; auto r = mkShader(sd); if (r.isErr()) return r.error(); defVS_Inst = r.value(); }
		{ sd.stage = ShaderStage::Vertex; sd.source = g_VS_Indirect; auto r = mkShader(sd); if (r.isErr()) return r.error(); defVS_Indirect = r.value(); }
		{ sd.stage = ShaderStage::Vertex; sd.source = g_VS_ShadowIndirect; auto r = mkShader(sd); if (r.isErr()) return r.error(); defVS_ShadowIndirect = r.value(); }
		{ sd.stage = ShaderStage::Pixel;  sd.source = g_PS; auto r = mkShader(sd); if (r.isErr()) return r.error(); defPS = r.value(); }
		{ SamplerDesc smd; smd.minFilter = smd.magFilter = smd.mipFilter = FilterMode::Linear; auto r = mkSampler(smd); if (r.isErr()) return r.error(); defSamp = r.value(); }
		{
			TextureDesc tdd; tdd.w = 1; tdd.h = 1; tdd.fmt = TextureFormat::RGBA8_UNorm; UInt32 wh = 0xFFFFFFFF; tdd.data = &wh; tdd.dataSize = 4; auto r = mkTex(tdd); if (r.isErr()) return r.error(); defTex = r.value();
			auto* td2 = textures.get(defTex.index, defTex.generation); if (td2) whiteSRV = td2->srv;
		}
		{
			// Flat tangent-space normal (128,128,255) -> (0,0,1): the fallback for
			// materials without a normal map, so the forward/G-buffer shaders can
			// always sample one and keep the interpolated vertex normal.
			// Packed little-endian as R | G<<8 | B<<16 | A<<24.
			TextureDesc tdd; tdd.w = 1; tdd.h = 1; tdd.fmt = TextureFormat::RGBA8_UNorm; UInt32 flat = 0xFFFF8080u; tdd.data = &flat; tdd.dataSize = 4;
			auto r = mkTex(tdd); if (r.isErr()) return r.error(); flatNormalTex = r.value();
			auto* td2 = textures.get(flatNormalTex.index, flatNormalTex.generation); if (td2) flatNormalSRV = td2->srv;
		}
		{ D::BufferDesc bd; bd.Name = "FrameCB"; bd.Size = sizeof(FrameConstants); bd.BindFlags = D::BIND_UNIFORM_BUFFER; bd.Usage = D::USAGE_DYNAMIC; bd.CPUAccessFlags = D::CPU_ACCESS_WRITE; device->CreateBuffer(bd, nullptr, &frameCB); }
		{ D::BufferDesc bd; bd.Name = "LightCB"; bd.Size = sizeof(LightConstants); bd.BindFlags = D::BIND_UNIFORM_BUFFER; bd.Usage = D::USAGE_DYNAMIC; bd.CPUAccessFlags = D::CPU_ACCESS_WRITE; device->CreateBuffer(bd, nullptr, &lightCB); memset(&lcBuf, 0, sizeof(lcBuf)); }
		{ D::BufferDesc bd; bd.Name = "InstCB"; bd.Size = sizeof(Mat4) * MaxInstances; bd.BindFlags = D::BIND_VERTEX_BUFFER; bd.Usage = D::USAGE_DEFAULT; device->CreateBuffer(bd, nullptr, &instanceCB); }
		// Create PBR PSOs �� then fix up shadow vars on all four
		{ PipelineStateDesc pd; pd.name = "DefPSO"; pd.vs = defVS; pd.ps = defPS; auto r = mkPSO(pd); if (r.isErr()) return r.error(); defPSO = r.value(); }
		{ PipelineStateDesc pd; pd.name = "WirePSO"; pd.vs = defVS; pd.ps = defPS; pd.rasterizer.fillMode = FillMode::Wireframe; auto r = mkPSO(pd); if (r.isErr()) return r.error(); defPSO_wire = r.value(); }
		{ PipelineStateDesc pd; pd.name = "InstPSO"; pd.vs = defVS_Inst; pd.ps = defPS; auto r = mkPSO(pd, true); if (r.isErr()) return r.error(); defPSO_Inst = r.value(); }
		{ PipelineStateDesc pd; pd.name = "InstWirePSO"; pd.vs = defVS_Inst; pd.ps = defPS; pd.rasterizer.fillMode = FillMode::Wireframe; auto r = mkPSO(pd, true); if (r.isErr()) return r.error(); defPSO_Inst_wire = r.value(); }
		// G-buffer shaders + PSOs (hybrid ray tracing): MRT albedo + world normal, non-MSAA.
		{
			ShaderDesc sd; sd.stage = ShaderStage::Pixel; sd.source = g_PS_GBuffer;
			auto r = mkShader(sd); if (r.isErr()) return r.error(); gbufPS = r.value();
		}
		{
			PipelineStateDesc pd; pd.name = "GBufPSO"; pd.vs = defVS; pd.ps = gbufPS;
			auto r = mkPSO(pd, false, true); if (r.isErr()) return r.error(); gbufPSO = r.value();
		}
		{
			PipelineStateDesc pd; pd.name = "GBufInstPSO"; pd.vs = defVS_Inst; pd.ps = gbufPS;
			auto r = mkPSO(pd, true, true); if (r.isErr()) return r.error(); gbufPSO_Inst = r.value();
		}
		// Indirect instanced PSO (world matrices from StructuredBuffer t5, indices from t6)
		{
			auto* vs = shaders.get(defVS_Indirect.index, defVS_Indirect.generation);
			auto* ps = shaders.get(defPS.index, defPS.generation);
			if (vs && ps) {
				D::GraphicsPipelineStateCreateInfo ci; ci.PSODesc.Name = "IndirectPSO"; ci.PSODesc.PipelineType = D::PIPELINE_TYPE_GRAPHICS;
				ci.pVS = vs->shader; ci.pPS = ps->shader; ci.GraphicsPipeline.NumRenderTargets = 1;
				ci.GraphicsPipeline.RTVFormats[0] = D::TEX_FORMAT_RGBA16_FLOAT; ci.GraphicsPipeline.DSVFormat = D::TEX_FORMAT_D32_FLOAT;
				ci.GraphicsPipeline.SmplDesc.Count = msaaSamples;
				ci.GraphicsPipeline.PrimitiveTopology = D::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
				ci.GraphicsPipeline.RasterizerDesc.CullMode = D::CULL_MODE_NONE;
				ci.GraphicsPipeline.DepthStencilDesc.DepthEnable = true;
				ci.GraphicsPipeline.DepthStencilDesc.DepthWriteEnable = true;
				ci.GraphicsPipeline.DepthStencilDesc.DepthFunc = D::COMPARISON_FUNC_LESS;
				D::LayoutElement le[] = {
					{0,0,3,D::VT_FLOAT32,false,offsetof(Vertex,position),sizeof(Vertex),D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX},
					{1,0,3,D::VT_FLOAT32,false,offsetof(Vertex,normal),sizeof(Vertex),D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX},
					{2,0,2,D::VT_FLOAT32,false,offsetof(Vertex,texCoord),sizeof(Vertex),D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX},
					{3,0,4,D::VT_FLOAT32,false,offsetof(Vertex,tangent),sizeof(Vertex),D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX},
				};
				ci.GraphicsPipeline.InputLayout.NumElements = 4; ci.GraphicsPipeline.InputLayout.LayoutElements = le;
				ci.PSODesc.ResourceLayout.DefaultVariableType = D::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
				ci.PSODesc.ResourceLayout.DefaultVariableMergeStages = D::SHADER_TYPE_VERTEX | D::SHADER_TYPE_PIXEL;
				D::ShaderResourceVariableDesc Vars[] = {
					{D::SHADER_TYPE_PIXEL, "t_BC", D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
					{D::SHADER_TYPE_PIXEL, "t_NormalMap", D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
					{D::SHADER_TYPE_PIXEL, "t_MR", D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
					{D::SHADER_TYPE_PIXEL, "t_EmissiveMap", D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
					{D::SHADER_TYPE_VERTEX | D::SHADER_TYPE_PIXEL, "Object", D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
					{D::SHADER_TYPE_PIXEL, "g_ShadowMap", D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
					// Pinned to the pixel stage so it matches its immutable sampler (see
					// the same note in mkPSO).
					{D::SHADER_TYPE_PIXEL, "g_SkyEnv", D::SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
					{D::SHADER_TYPE_VERTEX, "g_WorldMatrices", D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
					{D::SHADER_TYPE_VERTEX, "g_Indices", D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
				};
				ci.PSODesc.ResourceLayout.Variables = Vars; ci.PSODesc.ResourceLayout.NumVariables = EE_ARRAY_SIZE(Vars);
				D::SamplerDesc cmpSamp; cmpSamp.MinFilter = D::FILTER_TYPE_COMPARISON_LINEAR; cmpSamp.MagFilter = D::FILTER_TYPE_COMPARISON_LINEAR; cmpSamp.MipFilter = D::FILTER_TYPE_COMPARISON_LINEAR; cmpSamp.ComparisonFunc = D::COMPARISON_FUNC_LESS; cmpSamp.AddressU = D::TEXTURE_ADDRESS_CLAMP; cmpSamp.AddressV = D::TEXTURE_ADDRESS_CLAMP; cmpSamp.AddressW = D::TEXTURE_ADDRESS_CLAMP;
				D::ImmutableSamplerDesc ImtblSamps[] = {
					{D::SHADER_TYPE_PIXEL, "t_BC", D::SamplerDesc{}},
					{D::SHADER_TYPE_PIXEL, "g_ShadowMap", cmpSamp},
					{D::SHADER_TYPE_PIXEL, "g_SkyEnv", D::SamplerDesc{}},
				};
				ci.PSODesc.ResourceLayout.ImmutableSamplers = ImtblSamps; ci.PSODesc.ResourceLayout.NumImmutableSamplers = EE_ARRAY_SIZE(ImtblSamps);
				D::RefCntAutoPtr<D::IPipelineState> p; device->CreateGraphicsPipelineState(ci, &p);
				if (p) {
					if (envCubeSRV) { auto* v = p->GetStaticVariableByName(D::SHADER_TYPE_PIXEL, "g_SkyEnv"); if (v) v->Set(envCubeSRV); }
					if (frameCB) { auto* v = p->GetStaticVariableByName(D::SHADER_TYPE_VERTEX, "Frame"); if (v) v->Set(frameCB); }
					if (frameCB) { auto* v = p->GetStaticVariableByName(D::SHADER_TYPE_PIXEL, "Frame"); if (v) v->Set(frameCB); }
					if (lightCB) { auto* v = p->GetStaticVariableByName(D::SHADER_TYPE_PIXEL, "Lights"); if (v) v->Set(lightCB); }
					auto a = psos.allocate(); auto* dd = psos.getUnchecked(a.index); dd->pso = p; dd->desc = PipelineStateDesc{};
					p->CreateShaderResourceBinding(&dd->srb, true);
					if (dd->srb && defTex.isValid()) {
						auto* td = textures.get(defTex.index, defTex.generation);
						if (td) { auto* pv = dd->srb->GetVariableByName(D::SHADER_TYPE_PIXEL, "t_BC"); if (pv) pv->Set(td->srv.RawPtr(), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE); }
					}
					defPSO_Indirect = PSOHandle{a.index, a.generation};
				}
			}
		}
		// Shadow indirect PSO (depth-only, structured buffer world matrices)
		{
			auto* vs = shaders.get(defVS_ShadowIndirect.index, defVS_ShadowIndirect.generation);
			if (vs) {
				D::GraphicsPipelineStateCreateInfo ci; ci.PSODesc.Name = "ShadowIndirectPSO"; ci.PSODesc.PipelineType = D::PIPELINE_TYPE_GRAPHICS;
				ci.pVS = vs->shader; ci.GraphicsPipeline.NumRenderTargets = 0;
				ci.GraphicsPipeline.DSVFormat = D::TEX_FORMAT_D16_UNORM;
				ci.GraphicsPipeline.PrimitiveTopology = D::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
				ci.GraphicsPipeline.RasterizerDesc.CullMode = D::CULL_MODE_FRONT;
				ci.GraphicsPipeline.DepthStencilDesc.DepthEnable = true;
				ci.GraphicsPipeline.DepthStencilDesc.DepthWriteEnable = true;
				ci.GraphicsPipeline.DepthStencilDesc.DepthFunc = D::COMPARISON_FUNC_LESS;
				D::LayoutElement le[] = {
					{0,0,3,D::VT_FLOAT32,false,offsetof(Vertex,position),sizeof(Vertex),D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX},
					{1,0,3,D::VT_FLOAT32,false,offsetof(Vertex,normal),sizeof(Vertex),D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX},
					{2,0,2,D::VT_FLOAT32,false,offsetof(Vertex,texCoord),sizeof(Vertex),D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX},
					{3,0,4,D::VT_FLOAT32,false,offsetof(Vertex,tangent),sizeof(Vertex),D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX},
				};
				ci.GraphicsPipeline.InputLayout.NumElements = 4; ci.GraphicsPipeline.InputLayout.LayoutElements = le;
				ci.PSODesc.ResourceLayout.DefaultVariableType = D::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
				D::ShaderResourceVariableDesc Vars[] = {
					{D::SHADER_TYPE_VERTEX, "g_WorldMatrices", D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
					{D::SHADER_TYPE_VERTEX, "g_Indices", D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
				};
				ci.PSODesc.ResourceLayout.Variables = Vars; ci.PSODesc.ResourceLayout.NumVariables = EE_ARRAY_SIZE(Vars);
				D::RefCntAutoPtr<D::IPipelineState> p; device->CreateGraphicsPipelineState(ci, &p);
				if (p) {
					if (frameCB) { auto* v = p->GetStaticVariableByName(D::SHADER_TYPE_VERTEX, "Frame"); if (v) v->Set(frameCB); }
					auto a = psos.allocate(); auto* dd = psos.getUnchecked(a.index); dd->pso = p; dd->desc = PipelineStateDesc{};
					defPSO_ShadowIndirect = PSOHandle{a.index, a.generation};
				}
			}
		}
		// Depth-only shadow PSO (no pixel shader, D16 depth, no RT)
		{
			auto* vs = shaders.get(defVS.index, defVS.generation);
			if (vs) {
				D::GraphicsPipelineStateCreateInfo ci; ci.PSODesc.Name = "ShadowPSO"; ci.PSODesc.PipelineType = D::PIPELINE_TYPE_GRAPHICS;
				ci.pVS = vs->shader; ci.GraphicsPipeline.NumRenderTargets = 0;
				ci.GraphicsPipeline.DSVFormat = D::TEX_FORMAT_D16_UNORM;
				ci.GraphicsPipeline.PrimitiveTopology = D::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
				ci.GraphicsPipeline.RasterizerDesc.CullMode = D::CULL_MODE_NONE;
				ci.GraphicsPipeline.DepthStencilDesc.DepthEnable = true;
				ci.GraphicsPipeline.DepthStencilDesc.DepthWriteEnable = true;
				ci.GraphicsPipeline.DepthStencilDesc.DepthFunc = D::COMPARISON_FUNC_LESS;
				ci.GraphicsPipeline.SmplDesc.Count = 1;
				D::LayoutElement le[] = {
					{0,0,3,D::VT_FLOAT32,false,offsetof(Vertex,position),sizeof(Vertex),D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX},
					{1,0,3,D::VT_FLOAT32,false,offsetof(Vertex,normal),sizeof(Vertex),D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX},
					{2,0,2,D::VT_FLOAT32,false,offsetof(Vertex,texCoord),sizeof(Vertex),D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX},
					{3,0,4,D::VT_FLOAT32,false,offsetof(Vertex,tangent),sizeof(Vertex),D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX},
				};
				ci.GraphicsPipeline.InputLayout.NumElements = 4; ci.GraphicsPipeline.InputLayout.LayoutElements = le;
				ci.PSODesc.ResourceLayout.DefaultVariableType = D::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
				D::RefCntAutoPtr<D::IPipelineState> p; device->CreateGraphicsPipelineState(ci, &p);
				if (p) {
					if (frameCB) p->GetStaticVariableByName(D::SHADER_TYPE_VERTEX, "Frame")->Set(frameCB);
					// Bind Object CB once (identity world, normal matrix unused)
					{ D::BufferDesc bd; bd.Name = "ShadowObjCB"; bd.Size = sizeof(ObjectConstants); bd.BindFlags = D::BIND_UNIFORM_BUFFER; bd.Usage = D::USAGE_DEFAULT;
					  ObjectConstants oc{}; oc.world = Mat4(1.0f); oc.normalMat = Mat4(1.0f);
					  D::BufferData bdata; bdata.pData = &oc; bdata.DataSize = sizeof(oc);
					  D::RefCntAutoPtr<D::IBuffer> objCB; device->CreateBuffer(bd, &bdata, &objCB);
					  if (objCB) p->GetStaticVariableByName(D::SHADER_TYPE_VERTEX, "Object")->Set(objCB); }
					auto a = psos.allocate(); auto* dd = psos.getUnchecked(a.index); dd->pso = p;
					p->CreateShaderResourceBinding(&dd->srb, true);
					shadowPSO = PSOHandle{a.index, a.generation};
				}
			}
		}
		// Dummy 1x1 white texture for default shadow map binding (prevents black when unset)
		{
			D::TextureDesc td; td.Name = "ShadowDummy"; td.Type = D::RESOURCE_DIM_TEX_2D_ARRAY; td.Width = 1; td.Height = 1; td.ArraySize = 4; td.MipLevels = 1; td.Format = D::TEX_FORMAT_R16_UNORM; td.BindFlags = D::BIND_SHADER_RESOURCE; td.Usage = D::USAGE_IMMUTABLE;
			D::TextureSubResData srd[4]; UInt16 white = 0xFFFF;
			for (int i = 0; i < 4; i++) { srd[i].pData = &white; srd[i].Stride = 2; }
			D::TextureData tdata; tdata.pSubResources = srd; tdata.NumSubresources = 4;
			D::RefCntAutoPtr<D::ITexture> t; device->CreateTexture(td, &tdata, &t);
			if (t) shadowDummy = t->GetDefaultView(D::TEXTURE_VIEW_SHADER_RESOURCE);
		}
		// Recreate SRBs for PBR PSOs with shadow vars as MUTABLE
		for (auto* ph : {&defPSO, &defPSO_wire, &defPSO_Inst, &defPSO_Inst_wire}) {
			auto* dpd = psos.get(ph->index, ph->generation);
			if (!dpd || !dpd->pso) continue;
			dpd->srb.Release();
			dpd->pso->CreateShaderResourceBinding(&dpd->srb, true);
		}

		// Billboard shaders (manual: UseCombinedTextureSamplers = false)
		{
			auto mkBBShader = [&](D::SHADER_TYPE st, const char* src, ShaderHandle& out) {
				D::ShaderCreateInfo ci; ci.Desc.ShaderType = st; ci.Desc.Name = (st == D::SHADER_TYPE_VERTEX) ? "BBoardVS" : "BBoardPS";
				ci.Desc.UseCombinedTextureSamplers = false;
				ci.EntryPoint = "main"; ci.SourceLanguage = D::SHADER_SOURCE_LANGUAGE_HLSL;
				ci.Source = src; ci.SourceLength = (D::Uint32)strlen(src);
				D::RefCntAutoPtr<D::IShader> s; device->CreateShader(ci, &s);
				if (!s || s->GetStatus() != D::SHADER_STATUS_READY) return RenderError::ShaderCompilationFailed;
				auto a = shaders.allocate(); shaders.getUnchecked(a.index)->shader = std::move(s);
				shaders.getUnchecked(a.index)->stage = (st == D::SHADER_TYPE_VERTEX) ? ShaderStage::Vertex : ShaderStage::Pixel;
				out = ShaderHandle{ a.index, a.generation };
				return RenderError::None;
				};
			if (auto r = mkBBShader(D::SHADER_TYPE_VERTEX, g_VS_Billboard, bboardVS); r != RenderError::None) return r;
			if (auto r = mkBBShader(D::SHADER_TYPE_PIXEL, g_PS_Billboard, bboardPS); r != RenderError::None) return r;
		}
		{ SamplerDesc smd; smd.minFilter = smd.magFilter = smd.mipFilter = FilterMode::Linear; smd.addressU = smd.addressV = smd.addressW = AddressMode::Clamp; auto r = mkSampler(smd); if (r.isErr()) return r.error(); bboardSamp = r.value(); }
		// Fog texture (must be created before PSO for static binding)
		{
			const UInt32 sz = 128; std::vector<UInt32> pixels(sz * sz);
			for (UInt32 y = 0; y < sz; y++) for (UInt32 x = 0; x < sz; x++) {
				float dx = (float)x / sz - 0.5f, dy = (float)y / sz - 0.5f;
				float d = sqrtf(dx * dx + dy * dy) * 2.0f;
				UInt8 a = (UInt8)Clamp((UInt32)(255.0f * expf(-d * d * 2.0f)), 0u, 255u);
				pixels[y * sz + x] = 0x00FFFFFF | ((UInt32)a << 24);
			}
			TextureDesc td; td.w = sz; td.h = sz; td.fmt = TextureFormat::RGBA8_UNorm; td.data = pixels.data(); td.dataSize = (UInt32)(pixels.size() * 4);
			auto r = mkTex(td); if (r.isErr()) return r.error(); fogTex = r.value();
			auto* td2 = textures.get(fogTex.index, fogTex.generation); if (td2) fogSRV = td2->srv;
		}
		// Billboard PSO (needs fogSRV + Frame CB as static variables)
		{
			auto* vs = shaders.get(bboardVS.index, bboardVS.generation);
			auto* ps = shaders.get(bboardPS.index, bboardPS.generation);
			if (vs && ps && fogSRV) {
				D::GraphicsPipelineStateCreateInfo ci; 				ci.PSODesc.Name = "BBoardPSO"; ci.PSODesc.PipelineType = D::PIPELINE_TYPE_GRAPHICS;
				ci.pVS = vs->shader; ci.pPS = ps->shader; ci.GraphicsPipeline.NumRenderTargets = 1;
				ci.GraphicsPipeline.RTVFormats[0] = D::TEX_FORMAT_RGBA16_FLOAT; ci.GraphicsPipeline.DSVFormat = D::TEX_FORMAT_D32_FLOAT;
				ci.GraphicsPipeline.SmplDesc.Count = msaaSamples;
				ci.GraphicsPipeline.PrimitiveTopology = D::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
				ci.GraphicsPipeline.RasterizerDesc.CullMode = D::CULL_MODE_NONE;
				ci.GraphicsPipeline.DepthStencilDesc.DepthEnable = false;
				ci.GraphicsPipeline.DepthStencilDesc.DepthWriteEnable = false;
				auto& b = ci.GraphicsPipeline.BlendDesc.RenderTargets[0]; b.BlendEnable = true;
				b.SrcBlend = D::BLEND_FACTOR_SRC_ALPHA; b.DestBlend = D::BLEND_FACTOR_INV_SRC_ALPHA;
				b.SrcBlendAlpha = D::BLEND_FACTOR_ONE; b.DestBlendAlpha = D::BLEND_FACTOR_ONE;
				D::LayoutElement le[] = {
					{0,0,3,D::VT_FLOAT32,false,0, 36,D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX},
					{1,0,2,D::VT_FLOAT32,false,12,36,D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX},
					{2,0,4,D::VT_FLOAT32,false,20,36,D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX},
				};
				ci.GraphicsPipeline.InputLayout.NumElements = 3; ci.GraphicsPipeline.InputLayout.LayoutElements = le;
				ci.PSODesc.ResourceLayout.DefaultVariableType = D::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
				D::ShaderResourceVariableDesc svd[] = {
					{D::SHADER_TYPE_VERTEX | D::SHADER_TYPE_PIXEL, "Frame", D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
					{D::SHADER_TYPE_PIXEL, "g_BillTex", D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
					{D::SHADER_TYPE_PIXEL, "g_BillSampler", D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
				};
				ci.PSODesc.ResourceLayout.Variables = svd; ci.PSODesc.ResourceLayout.NumVariables = 3;
				D::RefCntAutoPtr<D::IPipelineState> pso;
				device->CreateGraphicsPipelineState(ci, &pso);
				if (pso) {
					auto a = psos.allocate();
					auto* pd2 = psos.getUnchecked(a.index); pd2->pso = pso;
					pso->CreateShaderResourceBinding(&pd2->srb, true);
					bboardPSO = PSOHandle{ a.index, a.generation };
					EInfo("BBoardPSO created");
				}
			}
		}
		// Dynamic VB/IB for billboards
		{ D::BufferDesc bd; bd.Name = "BBoardVB"; bd.Size = 65536; bd.BindFlags = D::BIND_VERTEX_BUFFER; bd.Usage = D::USAGE_DYNAMIC; bd.CPUAccessFlags = D::CPU_ACCESS_WRITE; device->CreateBuffer(bd, nullptr, &bboardVB); }
		{ D::BufferDesc bd; bd.Name = "BBoardIB"; bd.Size = 65536;  bd.BindFlags = D::BIND_INDEX_BUFFER;  bd.Usage = D::USAGE_DYNAMIC; bd.CPUAccessFlags = D::CPU_ACCESS_WRITE; device->CreateBuffer(bd, nullptr, &bboardIB); }
		// Skybox
		{
			// Compile both with and without cubemap
			D::ShaderCreateInfo ci; ci.Desc.ShaderType = D::SHADER_TYPE_VERTEX; ci.Desc.Name = "SkyVS";
			ci.Desc.UseCombinedTextureSamplers = false; ci.EntryPoint = "main"; ci.SourceLanguage = D::SHADER_SOURCE_LANGUAGE_HLSL;
			ci.Source = g_VS_Skybox; ci.SourceLength = (D::Uint32)strlen(g_VS_Skybox);
			D::RefCntAutoPtr<D::IShader> vs; device->CreateShader(ci, &vs);
			if (!vs || vs->GetStatus() != D::SHADER_STATUS_READY) return RenderError::ShaderCompilationFailed;
			{ auto a = shaders.allocate(); shaders.getUnchecked(a.index)->shader = std::move(vs); shaders.getUnchecked(a.index)->stage = ShaderStage::Vertex; skyVS = ShaderHandle{ a.index, a.generation }; }
		}
		{
			D::ShaderCreateInfo ci; ci.Desc.ShaderType = D::SHADER_TYPE_PIXEL; ci.Desc.Name = "SkyPS";
			ci.Desc.UseCombinedTextureSamplers = false; ci.EntryPoint = "main"; ci.SourceLanguage = D::SHADER_SOURCE_LANGUAGE_HLSL;
			ci.Source = g_PS_Skybox; ci.SourceLength = (D::Uint32)strlen(g_PS_Skybox);
			D::RefCntAutoPtr<D::IShader> ps; device->CreateShader(ci, &ps);
			if (!ps || ps->GetStatus() != D::SHADER_STATUS_READY) return RenderError::ShaderCompilationFailed;
			{ auto a = shaders.allocate(); shaders.getUnchecked(a.index)->shader = std::move(ps); shaders.getUnchecked(a.index)->stage = ShaderStage::Pixel; skyPS = ShaderHandle{ a.index, a.generation }; }
		}
		// Unit cube mesh (position only)
		{
			struct { float x, y, z; } cubeVerts[] = {
				{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1},{-1,1,-1},{1,1,-1},{1,1,1},{-1,1,1},{-1,-1,-1},{1,-1,-1},{1,-1,1},{-1,-1,1},{-1,-1,-1},{-1,1,-1},{-1,1,1},{-1,-1,1},{1,-1,-1},{1,1,-1},{1,1,1},{1,-1,1},
			};
			UInt32 idxs[] = {
				0,1,2,2,3,0,  4,5,6,6,7,4,  8,9,10,10,11,8,
				12,13,14,14,15,12,  16,17,18,18,19,16,  20,21,22,22,23,20,
			};
			D::BufferDesc vbd; vbd.Name = "SkyVB"; vbd.Size = sizeof(cubeVerts); vbd.BindFlags = D::BIND_VERTEX_BUFFER; vbd.Usage = D::USAGE_IMMUTABLE;
			D::BufferDesc ibd; ibd.Name = "SkyIB"; ibd.Size = sizeof(idxs); ibd.BindFlags = D::BIND_INDEX_BUFFER; ibd.Usage = D::USAGE_IMMUTABLE;
			D::BufferData vbd2; vbd2.pData = cubeVerts; vbd2.DataSize = sizeof(cubeVerts);
			D::BufferData ibd2; ibd2.pData = idxs; ibd2.DataSize = sizeof(idxs);
			device->CreateBuffer(vbd, &vbd2, &skyMesh.vb);
			device->CreateBuffer(ibd, &ibd2, &skyMesh.ib);
		}
		{ D::BufferDesc bd; bd.Name = "SkyColors"; bd.Size = 8 * 16; bd.BindFlags = D::BIND_UNIFORM_BUFFER; bd.Usage = D::USAGE_DYNAMIC; bd.CPUAccessFlags = D::CPU_ACCESS_WRITE; device->CreateBuffer(bd, nullptr, &skyCB); }
		// Skybox PSO
		{
			auto* vs = shaders.get(skyVS.index, skyVS.generation);
			auto* ps = shaders.get(skyPS.index, skyPS.generation);
			if (vs && ps) {
				D::GraphicsPipelineStateCreateInfo ci; ci.PSODesc.Name = "SkyPSO"; ci.PSODesc.PipelineType = D::PIPELINE_TYPE_GRAPHICS;
				ci.pVS = vs->shader; ci.pPS = ps->shader; ci.GraphicsPipeline.NumRenderTargets = 1;
				ci.GraphicsPipeline.RTVFormats[0] = D::TEX_FORMAT_RGBA16_FLOAT; ci.GraphicsPipeline.DSVFormat = D::TEX_FORMAT_D32_FLOAT;
				ci.GraphicsPipeline.PrimitiveTopology = D::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
				ci.GraphicsPipeline.RasterizerDesc.CullMode = D::CULL_MODE_NONE;
				ci.GraphicsPipeline.DepthStencilDesc.DepthEnable = true;
				ci.GraphicsPipeline.DepthStencilDesc.DepthWriteEnable = false;
				ci.GraphicsPipeline.DepthStencilDesc.DepthFunc = D::COMPARISON_FUNC_LESS_EQUAL;
				ci.GraphicsPipeline.SmplDesc.Count = msaaSamples;
				D::LayoutElement le[] = { {0,0,3,D::VT_FLOAT32,false,0,12,D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX} };
				ci.GraphicsPipeline.InputLayout.NumElements = 1; ci.GraphicsPipeline.InputLayout.LayoutElements = le;
				ci.PSODesc.ResourceLayout.DefaultVariableType = D::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
				D::RefCntAutoPtr<D::IPipelineState> pso; device->CreateGraphicsPipelineState(ci, &pso);
				if (pso) {
					if (frameCB) { auto* v = pso->GetStaticVariableByName(D::SHADER_TYPE_VERTEX, "Frame"); if (v) v->Set(frameCB); }
					if (skyCB) { auto* v = pso->GetStaticVariableByName(D::SHADER_TYPE_VERTEX, "SkyColors"); if (v) v->Set(skyCB); }
					auto a = psos.allocate(); auto* pd = psos.getUnchecked(a.index); pd->pso = pso;
					pso->CreateShaderResourceBinding(&pd->srb, true);
					skyPSO = PSOHandle{ a.index, a.generation };
				}
			}
		}
		// Cubemap skybox PS + PSO
		{
			D::ShaderCreateInfo ci; ci.Desc.ShaderType = D::SHADER_TYPE_PIXEL; ci.Desc.Name = "SkyCubePS";
			ci.Desc.UseCombinedTextureSamplers = false; ci.EntryPoint = "main"; ci.SourceLanguage = D::SHADER_SOURCE_LANGUAGE_HLSL;
			ci.Source = g_PS_SkyboxCube; ci.SourceLength = (D::Uint32)strlen(g_PS_SkyboxCube);
			D::RefCntAutoPtr<D::IShader> ps; device->CreateShader(ci, &ps);
			if (!ps || ps->GetStatus() != D::SHADER_STATUS_READY) return RenderError::ShaderCompilationFailed;
			{ auto a = shaders.allocate(); shaders.getUnchecked(a.index)->shader = std::move(ps); shaders.getUnchecked(a.index)->stage = ShaderStage::Pixel; skyCubePS = ShaderHandle{ a.index, a.generation }; }
		}
		{
			auto* vs = shaders.get(skyVS.index, skyVS.generation);
			auto* ps = shaders.get(skyCubePS.index, skyCubePS.generation);
			if (vs && ps) {
				D::GraphicsPipelineStateCreateInfo ci; ci.PSODesc.Name = "SkyCubePSO"; ci.PSODesc.PipelineType = D::PIPELINE_TYPE_GRAPHICS;
				ci.pVS = vs->shader; ci.pPS = ps->shader; ci.GraphicsPipeline.NumRenderTargets = 1;
				ci.GraphicsPipeline.RTVFormats[0] = D::TEX_FORMAT_RGBA16_FLOAT; ci.GraphicsPipeline.DSVFormat = D::TEX_FORMAT_D32_FLOAT;
				ci.GraphicsPipeline.PrimitiveTopology = D::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
				ci.GraphicsPipeline.RasterizerDesc.CullMode = D::CULL_MODE_NONE;
				ci.GraphicsPipeline.DepthStencilDesc.DepthEnable = true;
				ci.GraphicsPipeline.DepthStencilDesc.DepthWriteEnable = false;
				ci.GraphicsPipeline.DepthStencilDesc.DepthFunc = D::COMPARISON_FUNC_LESS_EQUAL;
				ci.GraphicsPipeline.SmplDesc.Count = msaaSamples;
				D::LayoutElement le[] = { {0,0,3,D::VT_FLOAT32,false,0,12,D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX} };
				ci.GraphicsPipeline.InputLayout.NumElements = 1; ci.GraphicsPipeline.InputLayout.LayoutElements = le;
				ci.PSODesc.ResourceLayout.DefaultVariableType = D::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
				// g_SkyTex + g_SkySamp both mutable (bound per-frame)
				D::ShaderResourceVariableDesc vv[] = {
					{D::SHADER_TYPE_PIXEL,"g_SkyTex",D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
					{D::SHADER_TYPE_PIXEL,"g_SkySamp",D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
				};
				ci.PSODesc.ResourceLayout.Variables = vv; ci.PSODesc.ResourceLayout.NumVariables = 2;
				D::RefCntAutoPtr<D::IPipelineState> pso; device->CreateGraphicsPipelineState(ci, &pso);
				if (pso) {
					if (frameCB) { auto* v = pso->GetStaticVariableByName(D::SHADER_TYPE_VERTEX, "Frame"); if (v) v->Set(frameCB); }
					if (skyCB) { auto* v = pso->GetStaticVariableByName(D::SHADER_TYPE_VERTEX, "SkyColors"); if (v) v->Set(skyCB); }
					auto a = psos.allocate(); auto* pd = psos.getUnchecked(a.index); pd->pso = pso;
					pso->CreateShaderResourceBinding(&pd->srb, true);
					skyCubePSO = PSOHandle{ a.index, a.generation };
				}
			}
		}

		// PSOs that draw the sky into the environment cube: the sky shaders again, but
		// at one sample and without a depth buffer (the cube has neither), which is why
		// the sky PSOs of the visible pass cannot be reused.
		{
			auto* vs = shaders.get(skyVS.index, skyVS.generation);
			auto makeEnvPso = [&](const char* name, ShaderHandle psH, bool cubeSky, PSOHandle& out) -> Result<void, RenderError> {
				auto* ps = shaders.get(psH.index, psH.generation);
				if (!vs || !ps) return RenderError::InvalidHandle;
				D::GraphicsPipelineStateCreateInfo ci;
				ci.PSODesc.Name = name; ci.PSODesc.PipelineType = D::PIPELINE_TYPE_GRAPHICS;
				ci.pVS = vs->shader; ci.pPS = ps->shader;
				ci.GraphicsPipeline.NumRenderTargets = 1;
				ci.GraphicsPipeline.RTVFormats[0] = D::TEX_FORMAT_RGBA16_FLOAT;
				ci.GraphicsPipeline.DSVFormat = D::TEX_FORMAT_UNKNOWN;
				ci.GraphicsPipeline.PrimitiveTopology = D::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
				ci.GraphicsPipeline.RasterizerDesc.CullMode = D::CULL_MODE_NONE;
				ci.GraphicsPipeline.DepthStencilDesc.DepthEnable = false;
				ci.GraphicsPipeline.SmplDesc.Count = 1;
				D::LayoutElement le[] = { {0,0,3,D::VT_FLOAT32,false,0,12,D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX} };
				ci.GraphicsPipeline.InputLayout.NumElements = 1; ci.GraphicsPipeline.InputLayout.LayoutElements = le;
				ci.PSODesc.ResourceLayout.DefaultVariableType = D::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
				D::ShaderResourceVariableDesc vv[] = {
					{D::SHADER_TYPE_PIXEL,"g_SkyTex",D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
					{D::SHADER_TYPE_PIXEL,"g_SkySamp",D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
				};
				if (cubeSky) { ci.PSODesc.ResourceLayout.Variables = vv; ci.PSODesc.ResourceLayout.NumVariables = EE_ARRAY_SIZE(vv); }
				D::RefCntAutoPtr<D::IPipelineState> pso; device->CreateGraphicsPipelineState(ci, &pso);
				if (!pso) return RenderError::PipelineStateCreationFailed;
				if (frameCB) { auto* v = pso->GetStaticVariableByName(D::SHADER_TYPE_VERTEX, "Frame"); if (v) v->Set(frameCB); }
				if (skyCB) { auto* v = pso->GetStaticVariableByName(D::SHADER_TYPE_VERTEX, "SkyColors"); if (v) v->Set(skyCB); }
				auto a = psos.allocate(); auto* pd = psos.getUnchecked(a.index); pd->pso = pso;
				pso->CreateShaderResourceBinding(&pd->srb, true);
				out = PSOHandle{ a.index, a.generation };
				return {};
			};
			auto r1 = makeEnvPso("EnvCornerPSO", skyPS, false, envCornerPSO); if (r1.isErr()) return r1.error();
			auto r2 = makeEnvPso("EnvCubeSkyPSO", skyCubePS, true, envCubeSkyPSO); if (r2.isErr()) return r2.error();
		}
		return {};
	}

	// --------------------------------------------------------------
	// Resource creation
	// --------------------------------------------------------------

	Result<ShaderHandle, RenderError> mkShader(const ShaderDesc& d) {
		D::ShaderCreateInfo ci; ci.Desc.ShaderType = toDST(d.stage); ci.Desc.Name = "S"; ci.Desc.UseCombinedTextureSamplers = true;
		ci.EntryPoint = d.entryPoint.c_str(); ci.SourceLanguage = D::SHADER_SOURCE_LANGUAGE_HLSL;
		if (!d.filePath.empty()) ci.FilePath = d.filePath.c_str(); else { ci.Source = d.source.c_str(); ci.SourceLength = (D::Uint32)d.source.length(); }
		D::RefCntAutoPtr<D::IShader> s; device->CreateShader(ci, &s);
		if (!s || s->GetStatus() != D::SHADER_STATUS_READY) return RenderError::ShaderCompilationFailed;
		auto a = shaders.allocate(); shaders.getUnchecked(a.index)->shader = std::move(s); shaders.getUnchecked(a.index)->stage = d.stage;
		return ShaderHandle{ a.index, a.generation };
	}

	Result<PSOHandle, RenderError> mkPSO(const PipelineStateDesc& d, bool instanced = false, bool gbuffer = false) {
		auto* vs = shaders.get(d.vs.index, d.vs.generation); auto* ps = shaders.get(d.ps.index, d.ps.generation);
		if (!vs || !ps) return RenderError::InvalidHandle;
		D::GraphicsPipelineStateCreateInfo ci; ci.PSODesc.Name = d.name.c_str(); ci.PSODesc.PipelineType = D::PIPELINE_TYPE_GRAPHICS;
			ci.pVS = vs->shader; ci.pPS = ps->shader;
			ci.GraphicsPipeline.NumRenderTargets = gbuffer ? 3 : 1;
			// Scene passes render into the (RGBA16_FLOAT) HDR target; the G-buffer
			// pass keeps its 8-bit albedo target + RGBA16F normal/emissive targets.
			ci.GraphicsPipeline.RTVFormats[0] = gbuffer ? D::TEX_FORMAT_RGBA8_UNORM_SRGB : D::TEX_FORMAT_RGBA16_FLOAT;
			if (gbuffer) ci.GraphicsPipeline.RTVFormats[1] = D::TEX_FORMAT_RGBA16_FLOAT;
			if (gbuffer) ci.GraphicsPipeline.RTVFormats[2] = D::TEX_FORMAT_RGBA16_FLOAT;
			ci.GraphicsPipeline.DSVFormat = D::TEX_FORMAT_D32_FLOAT;
			// The G-buffer pass runs at the configured MSAA sample count too; the
			// Demo resolves it to 1x before the ray tracing compute pass reads it.
			ci.GraphicsPipeline.SmplDesc.Count = msaaSamples;
		ci.GraphicsPipeline.PrimitiveTopology = toDTopo(d.topology);
		ci.GraphicsPipeline.RasterizerDesc.FillMode = toDFill(d.rasterizer.fillMode);
		ci.GraphicsPipeline.RasterizerDesc.CullMode = toDCull(d.rasterizer.cullMode);
		ci.GraphicsPipeline.RasterizerDesc.FrontCounterClockwise = d.rasterizer.frontCounterClockwise;
		ci.GraphicsPipeline.DepthStencilDesc.DepthEnable = d.depthStencil.depthEnable;
		ci.GraphicsPipeline.DepthStencilDesc.DepthWriteEnable = d.depthStencil.depthWrite;
		ci.GraphicsPipeline.DepthStencilDesc.DepthFunc = toDCmp(d.depthStencil.depthFunc);
		if (d.blendTarget.blendEnable) { auto& b = ci.GraphicsPipeline.BlendDesc.RenderTargets[0]; b.BlendEnable = true; b.SrcBlend = D::BLEND_FACTOR_SRC_ALPHA; b.DestBlend = D::BLEND_FACTOR_INV_SRC_ALPHA; b.SrcBlendAlpha = D::BLEND_FACTOR_ONE; b.DestBlendAlpha = D::BLEND_FACTOR_INV_SRC_ALPHA; }
		D::LayoutElement le[8];
		UInt32 leCount = 0;
		le[leCount++] = D::LayoutElement(0, 0, 3, D::VT_FLOAT32, false, offsetof(Vertex, position), sizeof(Vertex), D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX);
		le[leCount++] = D::LayoutElement(1, 0, 3, D::VT_FLOAT32, false, offsetof(Vertex, normal), sizeof(Vertex), D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX);
		le[leCount++] = D::LayoutElement(2, 0, 2, D::VT_FLOAT32, false, offsetof(Vertex, texCoord), sizeof(Vertex), D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX);
		le[leCount++] = D::LayoutElement(3, 0, 4, D::VT_FLOAT32, false, offsetof(Vertex, tangent), sizeof(Vertex), D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX);
		if (instanced) {
			le[leCount++] = D::LayoutElement(4, 1, 4, D::VT_FLOAT32, false, D::INPUT_ELEMENT_FREQUENCY_PER_INSTANCE);
			le[leCount++] = D::LayoutElement(5, 1, 4, D::VT_FLOAT32, false, D::INPUT_ELEMENT_FREQUENCY_PER_INSTANCE);
			le[leCount++] = D::LayoutElement(6, 1, 4, D::VT_FLOAT32, false, D::INPUT_ELEMENT_FREQUENCY_PER_INSTANCE);
			le[leCount++] = D::LayoutElement(7, 1, 4, D::VT_FLOAT32, false, D::INPUT_ELEMENT_FREQUENCY_PER_INSTANCE);
		}
		ci.GraphicsPipeline.InputLayout.NumElements = leCount;
		ci.GraphicsPipeline.InputLayout.LayoutElements = le;

		ci.PSODesc.ResourceLayout.DefaultVariableType = D::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
		ci.PSODesc.ResourceLayout.DefaultVariableMergeStages = D::SHADER_TYPE_VERTEX | D::SHADER_TYPE_PIXEL;

		// Every per-material texture has to be declared MUTABLE here: anything left
		// to DefaultVariableType (STATIC) is owned by the PSO, and
		// IShaderResourceBinding::GetVariableByName() does not return it, so the
		// per-material Set() in mkMat() silently did nothing and the shader sampled
		// whatever descriptor happened to sit in that heap slot - which is why all
		// objects used to show one and the same (and flickering) normal map. The
		// mesh shader path never had this problem because it drives its maps through
		// a mutable texture array.
		D::ShaderResourceVariableDesc Vars[] = {
			{D::SHADER_TYPE_PIXEL, "t_BC",          D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
			{D::SHADER_TYPE_PIXEL, "t_NormalMap",   D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
			{D::SHADER_TYPE_PIXEL, "t_MR",          D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
			{D::SHADER_TYPE_PIXEL, "t_EmissiveMap", D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
			{D::SHADER_TYPE_VERTEX | D::SHADER_TYPE_PIXEL, "Object", D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
			{D::SHADER_TYPE_PIXEL, "g_ShadowMap", D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
			// Explicit, and deliberately not left to DefaultVariableMergeStages: an
			// automatically detected resource is merged across the default stages
			// (VERTEX|PIXEL here), and Diligent then rejects the PIXEL-only immutable
			// sampler below - "a resource present in multiple shader stages cannot be
			// combined with different immutable samplers in different stages". The
			// stage list of a resource has to match its immutable sampler's.
			{D::SHADER_TYPE_PIXEL, "g_SkyEnv", D::SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
		};
		// Only the shaded pixel shader declares the environment cube - the G-buffer
		// variant leaves the lighting to the RT compose pass - so its entry (and its
		// immutable sampler, which must be the last one) is only part of the layout
		// when the shader actually has the resource.
		const D::Uint32 numVars = gbuffer ? EE_ARRAY_SIZE(Vars) - 1u : EE_ARRAY_SIZE(Vars);
		ci.PSODesc.ResourceLayout.Variables = Vars; ci.PSODesc.ResourceLayout.NumVariables = numVars;

		D::SamplerDesc cmpSamp; cmpSamp.MinFilter = D::FILTER_TYPE_COMPARISON_LINEAR; cmpSamp.MagFilter = D::FILTER_TYPE_COMPARISON_LINEAR; cmpSamp.MipFilter = D::FILTER_TYPE_COMPARISON_LINEAR; cmpSamp.ComparisonFunc = D::COMPARISON_FUNC_LESS; cmpSamp.AddressU = D::TEXTURE_ADDRESS_CLAMP; cmpSamp.AddressV = D::TEXTURE_ADDRESS_CLAMP; cmpSamp.AddressW = D::TEXTURE_ADDRESS_CLAMP;
		D::ImmutableSamplerDesc ImtblSamps[] = {
			{D::SHADER_TYPE_PIXEL, "t_BC", D::SamplerDesc{}},
			{D::SHADER_TYPE_PIXEL, "g_ShadowMap", cmpSamp},
			{D::SHADER_TYPE_PIXEL, "g_SkyEnv", D::SamplerDesc{}},
		};
		ci.PSODesc.ResourceLayout.ImmutableSamplers = ImtblSamps;
		ci.PSODesc.ResourceLayout.NumImmutableSamplers = gbuffer ? EE_ARRAY_SIZE(ImtblSamps) - 1u : EE_ARRAY_SIZE(ImtblSamps);

		D::RefCntAutoPtr<D::IPipelineState> p; device->CreateGraphicsPipelineState(ci, &p);
		if (!p) return RenderError::PipelineStateCreationFailed;

		// The environment cube is shared by every object (it is the sky, not a
		// material), so it is a static variable bound once here. The texture exists
		// from createDefaults() on, even before the first build, so this never leaves
		// the variable unbound - see the note on the mutable map variables above.
		if (envCubeSRV) { auto* v = p->GetStaticVariableByName(D::SHADER_TYPE_PIXEL, "g_SkyEnv"); if (v) v->Set(envCubeSRV); }

		if (frameCB) {
			{ auto* v = p->GetStaticVariableByName(D::SHADER_TYPE_VERTEX, "Frame"); if (v) v->Set(frameCB); }
			{ auto* v = p->GetStaticVariableByName(D::SHADER_TYPE_PIXEL, "Frame"); if (v) v->Set(frameCB); }
		}
		if (lightCB) {
			{ auto* v = p->GetStaticVariableByName(D::SHADER_TYPE_PIXEL, "Lights"); if (v) v->Set(lightCB); }
		}
		auto a = psos.allocate(); auto* dd = psos.getUnchecked(a.index); dd->pso = std::move(p); dd->desc = d;
		dd->pso->CreateShaderResourceBinding(&dd->srb, true);
		if (dd->srb && defTex.isValid()) {
			auto* td = textures.get(defTex.index, defTex.generation);
			if (td) { auto* pv = dd->srb->GetVariableByName(D::SHADER_TYPE_PIXEL, "t_BC"); if (pv) pv->Set(td->srv.RawPtr(), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE); }
		}
		return PSOHandle{ a.index, a.generation };
	}

	Result<MeshHandle, RenderError> mkMesh(const MeshDesc& d) {
		if (d.vertices.empty() || d.indices.empty()) return RenderError::InvalidArgument;

		// Correct normals that disagree with the winding before anything reads them
		// (see orientNormalsToWinding). This is deliberately an engine-side safety
		// net: the source asset should be fixed too, which is why the warning names
		// the numbers.
		Vector<Vertex> verts = d.vertices;
		if (const UInt32 flipped = orientNormalsToWinding(verts, d.indices); flipped > 0) {
			EWarn("Mesh '{}' ({} vertices): {} normals disagreed with the triangle winding and were flipped. "
				"The asset has inverted normals - reflections, AO and the G-buffer would otherwise shade the wrong side.",
				d.name.empty() ? "<unnamed>" : d.name.c_str(), (UInt32)verts.size(), flipped);
		}

		auto a = meshes.allocate(); auto* dd = meshes.getUnchecked(a.index);
		dd->vc = (UInt32)verts.size(); dd->ic = (UInt32)d.indices.size(); dd->sub = d.subMeshes;
		dd->cpuVertices = verts; dd->cpuIndices = d.indices;
		// BIND_RAY_TRACING allows the buffers to be read during BLAS build operations,
		// but is only valid when the ray tracing device feature is enabled.
		{ D::BufferDesc bd; bd.Name = "VB"; bd.Size = verts.size() * sizeof(Vertex); bd.BindFlags = D::BIND_VERTEX_BUFFER; if (rtFeatureEnabled) bd.BindFlags |= D::BIND_RAY_TRACING; bd.Usage = D::USAGE_IMMUTABLE; D::BufferData bdata; bdata.pData = verts.data(); bdata.DataSize = bd.Size; D::RefCntAutoPtr<D::IBuffer> b; device->CreateBuffer(bd, &bdata, &b); if (!b) return RenderError::BufferCreationFailed; dd->vb = std::move(b); }
		{ D::BufferDesc bd; bd.Name = "IB"; bd.Size = d.indices.size() * sizeof(UInt32); bd.BindFlags = D::BIND_INDEX_BUFFER; if (rtFeatureEnabled) bd.BindFlags |= D::BIND_RAY_TRACING; bd.Usage = D::USAGE_IMMUTABLE; D::BufferData bdata; bdata.pData = d.indices.data(); bdata.DataSize = bd.Size; D::RefCntAutoPtr<D::IBuffer> b; device->CreateBuffer(bd, &bdata, &b); if (!b) return RenderError::BufferCreationFailed; dd->ib = std::move(b); }
		return MeshHandle{ a.index, a.generation };
	}

	Result<TextureHandle, RenderError> mkTex(const TextureDesc& d) {
		const bool renderTargetLike = d.asRenderTarget || d.asUAV || d.asDepthStencil;

		// Optional CPU mip chain: filtered in linear light (sRGB formats) and
		// uploaded as extra subresources of the same immutable texture. Immutable
		// + prebuilt levels keeps texture creation thread safe (the async model
		// loader creates textures off the render thread, where recording a
		// GenerateMips command would race with the main context).
		Utilities::MipChain chain;
		if (d.mipChain && d.data && !renderTargetLike && d.w > 0 && d.h > 0 && d.dataSize >= (Size)d.w * d.h * 4) {
			Utilities::DecodedImage base;
			base.width = d.w; base.height = d.h;
			base.pixels.assign(static_cast<const UInt8*>(d.data), static_cast<const UInt8*>(d.data) + (Size)d.w * d.h * 4);
			chain = Utilities::buildMipChain(std::move(base), d.fmt == TextureFormat::RGBA8_UNorm_SRGB);
		}

		D::TextureDesc td; td.Name = "Tex"; td.Type = D::RESOURCE_DIM_TEX_2D; td.Width = d.w; td.Height = d.h; td.Format = toDFmt(d.fmt);
		td.MipLevels = chain.isValid() ? (D::Uint32)chain.levels.size() : d.mipLevels;
		td.SampleCount = d.sampleCount;
		td.BindFlags = D::BIND_SHADER_RESOURCE;
		if (d.asRenderTarget) td.BindFlags |= D::BIND_RENDER_TARGET;
		if (d.asUAV) td.BindFlags |= D::BIND_UNORDERED_ACCESS;
		if (d.asDepthStencil) td.BindFlags |= D::BIND_DEPTH_STENCIL;
		td.Usage = renderTargetLike ? D::USAGE_DEFAULT : D::USAGE_IMMUTABLE;

		Vector<D::TextureSubResData> subRes;
		D::TextureData tdata;
		if (chain.isValid()) {
			UInt32 mw = chain.width;
			subRes.reserve(chain.levels.size());
			for (const auto& level : chain.levels) {
				D::TextureSubResData srd; srd.pData = level.data(); srd.Stride = mw * 4;
				subRes.push_back(srd);
				mw = std::max(1u, mw / 2);
			}
			tdata.pSubResources = subRes.data();
			tdata.NumSubresources = (D::Uint32)subRes.size();
		}
		else {
			D::TextureSubResData srd; srd.pData = d.data; srd.Stride = d.w * 4;
			if (d.data) subRes.push_back(srd);
			tdata.pSubResources = d.data ? subRes.data() : nullptr;
			tdata.NumSubresources = d.data ? 1 : 0;
		}
		D::RefCntAutoPtr<D::ITexture> t; device->CreateTexture(td, (tdata.pSubResources || tdata.NumSubresources) ? &tdata : nullptr, &t); if (!t) return RenderError::TextureCreationFailed;
		auto a = textures.allocate(); auto* dd = textures.getUnchecked(a.index); dd->tex = std::move(t); dd->desc = d;
		dd->srv = dd->tex->GetDefaultView(D::TEXTURE_VIEW_SHADER_RESOURCE);
		if (d.asRenderTarget) dd->rtv = dd->tex->GetDefaultView(D::TEXTURE_VIEW_RENDER_TARGET);
		if (d.asUAV) dd->uav = dd->tex->GetDefaultView(D::TEXTURE_VIEW_UNORDERED_ACCESS);
		if (d.asDepthStencil) dd->dsv = dd->tex->GetDefaultView(D::TEXTURE_VIEW_DEPTH_STENCIL);
		return TextureHandle{ a.index, a.generation };
	}

	Result<SamplerHandle, RenderError> mkSampler(const SamplerDesc& d) {
		D::SamplerDesc sd; sd.MinFilter = toDFilt(d.minFilter); sd.MagFilter = toDFilt(d.magFilter); sd.MipFilter = toDFilt(d.mipFilter); sd.AddressU = toDAddr(d.addressU); sd.AddressV = toDAddr(d.addressV); sd.AddressW = toDAddr(d.addressW); sd.MaxAnisotropy = d.maxAnisotropy;
		D::RefCntAutoPtr<D::ISampler> s; device->CreateSampler(sd, &s); if (!s) return RenderError::SamplerCreationFailed;
		auto a = samplers.allocate(); samplers.getUnchecked(a.index)->sampler = std::move(s); samplers.getUnchecked(a.index)->desc = d;
		return SamplerHandle{ a.index, a.generation };
	}

	Result<MaterialHandle, RenderError> mkMat(const MaterialDesc& d, PSOHandle po) {
		auto* p = psos.get(po.index, po.generation); if (!p) return RenderError::InvalidHandle;
		auto a = materials.allocate(); auto* dd = materials.getUnchecked(a.index); dd->desc = d; dd->pso = po;
		{ D::BufferDesc bd; bd.Name = "ObjCB"; bd.Size = sizeof(ObjectConstants); bd.BindFlags = D::BIND_UNIFORM_BUFFER; bd.Usage = D::USAGE_DYNAMIC; bd.CPUAccessFlags = D::CPU_ACCESS_WRITE; device->CreateBuffer(bd, nullptr, &dd->objCB); }
		auto bindSRB = [&](D::RefCntAutoPtr<D::IShaderResourceBinding>& srb, D::IPipelineState* pso, bool bindShadowDummy) {
			pso->CreateShaderResourceBinding(&srb, true);
			if (!srb) return;
			if (dd->objCB) {
				if (auto* pv = srb->GetVariableByName(D::SHADER_TYPE_VERTEX, "Object")) pv->Set(dd->objCB.RawPtr());
				if (auto* pv = srb->GetVariableByName(D::SHADER_TYPE_PIXEL, "Object")) pv->Set(dd->objCB.RawPtr());
			}
			auto bindTex = [&](const char* name, TextureHandle handle, D::ITextureView* fallback) {
				if (auto* pv = srb->GetVariableByName(D::SHADER_TYPE_PIXEL, name)) {
					D::ITextureView* srv = fallback;
					if (handle.isValid()) {
						if (auto* td = textures.get(handle.index, handle.generation)) srv = td->srv.RawPtr();
					}
					pv->Set(srv, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
				}
			};
			// Every map has a neutral 1x1 fallback so the shaders keep working (and
			// the mutable variables stay bound) for materials without them: white for
			// albedo/MR/emissive, flat (128,128,255) for the normal.
			bindTex("t_BC",          d.baseColorTexture,         whiteSRV.RawPtr());
			bindTex("t_NormalMap",   d.normalTexture,            flatNormalSRV.RawPtr());
			bindTex("t_MR",          d.metallicRoughnessTexture, whiteSRV.RawPtr());
			bindTex("t_EmissiveMap", d.emissiveTexture,          whiteSRV.RawPtr());
			// The G-buffer shader declares (but does not use) g_ShadowMap; bind a dummy so
			// the mutable variable is never left unbound when committing the G-buffer SRB.
			// The regular PBR SRB leaves it unbound here - draw() binds the real shadow map
			// (with ALLOW_OVERWRITE) right before the draw call.
			if (bindShadowDummy) {
				if (auto* sv = srb->GetVariableByName(D::SHADER_TYPE_PIXEL, "g_ShadowMap"))
					sv->Set(shadowDummy, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
			}
		};
		bindSRB(dd->srb, p->pso, false);
		// G-buffer SRB (only meaningful when the G-buffer PSOs exist, i.e. always after createDefaults).
		if (gbufPSO.isValid()) {
			if (auto* gp = psos.get(gbufPSO.index, gbufPSO.generation)) bindSRB(dd->gbufSRB, gp->pso, true);
		}
		return MaterialHandle{ a.index, a.generation };
	}

	// --------------------------------------------------------------
	// Drawing
	// --------------------------------------------------------------

	void begin() {
		auto* r = overrideRTV ? static_cast<D::ITextureView*>(overrideRTV) : sc->GetCurrentBackBufferRTV();
		auto* d = overrideDSV ? static_cast<D::ITextureView*>(overrideDSV) : dsv.RawPtr();
		float cc[] = { 0.1f,0.1f,0.15f,1.0f };
		ctx->SetRenderTargets(1, &r, d, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		ctx->ClearRenderTarget(r, cc, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		ctx->ClearDepthStencil(d, D::CLEAR_DEPTH_FLAG, 1.0f, 0, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		dc = 0;
		drawSkybox();
	}

	void draw(MeshHandle mh, const Transform& tr) {
		auto* md = meshes.get(mh.index, mh.generation); if (!md || md->sub.empty()) return;
		PSOHandle activePSO = wireframe ? defPSO_wire : defPSO;
		if (gBufferActive) activePSO = gbufPSO;
		auto* pd = psos.get(activePSO.index, activePSO.generation);
		static int wfLog = 0;
		if (!pd && wireframe && wfLog++ < 5) { EInfo("WirePSO lookup failed: idx={} gen={}", defPSO_wire.index, defPSO_wire.generation); }
		if (!pd) return;
		ctx->SetPipelineState(pd->pso);
		// Update frame CB: ViewProj = Proj * View (column-major, for mul(g_ViewProj, worldPos))
		{ FrameConstants fc{}; fc.viewProj = cam.proj * cam.view; fc.cameraPos = Vec4(cam.desc.pos, 1.0f); fc.ambient = ambient; applyEnvironment(fc); fc.lightCount = (UInt32)activeLights.size(); for(int i=0;i<4;i++) fc.shadowMapUVDepth[i]=shadowMapUVDepth[i]; fc.cascadeSplits = cascadeSplits; void* m = nullptr; ctx->MapBuffer(frameCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m); if (m) { memcpy(m, &fc, sizeof(fc)); ctx->UnmapBuffer(frameCB, D::MAP_WRITE); } }
		{ void* m = nullptr; ctx->MapBuffer(lightCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m); if (m) { memcpy(m, &lcBuf, sizeof(lcBuf)); ctx->UnmapBuffer(lightCB, D::MAP_WRITE); } }
		D::Uint64 vo = 0; D::IBuffer* vbs[] = { md->vb.RawPtr() };
		ctx->SetVertexBuffers(0, 1, vbs, &vo, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION, D::SET_VERTEX_BUFFERS_FLAG_RESET);
		ctx->SetIndexBuffer(md->ib, 0, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		Mat4 wm = tr.computeWorldMatrix();
		// g_Normal = transpose(inverse(world)) in column-major (for mul(g_Normal, normal) convention)
		Mat4 nm = tr.computeNormalMatrix();
		for (UInt32 si = 0; si < (UInt32)md->sub.size(); si++) {
			auto& s = md->sub[si];
			auto* mt = materials.get(s.material.index, s.material.generation);
			if (mt && mt->objCB) { ObjectConstants oc; oc.world = wm; oc.normalMat = nm; oc.baseColor = mt->desc.baseColorFactor; oc.metallicRough = Vec4(mt->desc.metallicFactor, mt->desc.roughnessFactor, 0, 0); oc.emissive = Vec4(mt->desc.emissiveFactor, 1.0f); oc.nmSign = nmSign; oc.mapFlags = Vec4(mt->desc.normalTexture.isValid() ? 1.0f : 0.0f, mt->desc.metallicRoughnessTexture.isValid() ? 1.0f : 0.0f, mt->desc.emissiveTexture.isValid() ? 1.0f : 0.0f, normalMapDebug ? 1.0f : 0.0f); void* m = nullptr; ctx->MapBuffer(mt->objCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m); if (m) { memcpy(m, &oc, sizeof(oc)); ctx->UnmapBuffer(mt->objCB, D::MAP_WRITE); } }
			D::IShaderResourceBinding* srb = mt ? mt->srb.RawPtr() : nullptr;
			if (gBufferActive && mt) srb = mt->gbufSRB.RawPtr();
			if (mt && srb) {
				if (!gBufferActive) {
					auto* sv = srb->GetVariableByName(D::SHADER_TYPE_PIXEL, "g_ShadowMap");
					if (sv) sv->Set(shadowSRV ? shadowSRV : shadowDummy, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
				}
				ctx->CommitShaderResources(srb, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
			}
			D::DrawIndexedAttribs da; da.IndexType = D::VT_UINT32; da.NumIndices = s.indexCount; da.FirstIndexLocation = s.indexOffset; da.BaseVertex = (D::Uint32)s.vertexOffset; da.Flags = D::DRAW_FLAG_VERIFY_ALL;
			ctx->DrawIndexed(da); dc++;
		}
	}

	void drawBillboards(const Vector<RenderSubsystem::BillboardDesc>& bbs) {
		if (bbs.empty() || !bboardVB || !bboardIB) return;
		auto* pd = psos.get(bboardPSO.index, bboardPSO.generation); if (!pd) return;
		auto& camPos = cam.desc.pos;

		struct BVert { Vec3 pos; Vec2 uv; Vec4 col; };
		std::vector<BVert> verts; std::vector<UInt32> idxs;
		verts.reserve(bbs.size() * 4); idxs.reserve(bbs.size() * 6);

		for (auto& bb : bbs) {
			Vec3 toEye = normalize(camPos - bb.position);
			Vec3 right = normalize(cross(Vec3(0, 1, 0), toEye));
			if (dot(right, right) < 0.001f) right = normalize(cross(Vec3(0, 0, 1), toEye));
			Vec3 up = cross(toEye, right);
			F32 hw = bb.size.x * 0.5f, hh = bb.size.y * 0.5f;
			Vec3 c[4] = { bb.position - right * hw - up * hh, bb.position + right * hw - up * hh, bb.position + right * hw + up * hh, bb.position - right * hw + up * hh };
			UInt32 base = (UInt32)verts.size();
			verts.push_back({ c[0], Vec2(0,0), bb.color }); verts.push_back({ c[1], Vec2(1,0), bb.color });
			verts.push_back({ c[2], Vec2(1,1), bb.color }); verts.push_back({ c[3], Vec2(0,1), bb.color });
			idxs.insert(idxs.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
		}

		void* m = nullptr;
		ctx->MapBuffer(bboardVB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m); if (m) { memcpy(m, verts.data(), verts.size() * sizeof(BVert)); ctx->UnmapBuffer(bboardVB, D::MAP_WRITE); }
		ctx->MapBuffer(bboardIB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m); if (m) { memcpy(m, idxs.data(), idxs.size() * sizeof(UInt32)); ctx->UnmapBuffer(bboardIB, D::MAP_WRITE); }

		ctx->SetPipelineState(pd->pso);
		{
			FrameConstants fc{}; fc.viewProj = cam.proj * cam.view; fc.cameraPos = Vec4(camPos, 1.0f); fc.ambient = ambient; applyEnvironment(fc); fc.lightCount = (UInt32)activeLights.size(); for(int i=0;i<4;i++) fc.shadowMapUVDepth[i]=shadowMapUVDepth[i]; fc.cascadeSplits = cascadeSplits;
			void* m2 = nullptr; ctx->MapBuffer(frameCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m2); if (m2) { memcpy(m2, &fc, sizeof(fc)); ctx->UnmapBuffer(frameCB, D::MAP_WRITE); }
			D::IShaderResourceVariable* fv = pd->srb->GetVariableByName(D::SHADER_TYPE_VERTEX, "Frame");
			if (fv) fv->Set(frameCB, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
			D::IShaderResourceVariable* tv = pd->srb->GetVariableByName(D::SHADER_TYPE_PIXEL, "g_BillTex");
			if (tv) tv->Set(fogSRV, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
			D::IShaderResourceVariable* sv = pd->srb->GetVariableByName(D::SHADER_TYPE_PIXEL, "g_BillSampler");
			auto* sd = samplers.get(bboardSamp.index, bboardSamp.generation);
			if (sv && sd) sv->Set(sd->sampler, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
		}
		ctx->SetVertexBuffers(0, 1, &bboardVB, nullptr, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION, D::SET_VERTEX_BUFFERS_FLAG_RESET);
		ctx->SetIndexBuffer(bboardIB, 0, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		ctx->CommitShaderResources(pd->srb, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		D::DrawIndexedAttribs da; da.IndexType = D::VT_UINT32; da.NumIndices = (UInt32)idxs.size(); da.Flags = D::DRAW_FLAG_VERIFY_ALL;
		ctx->DrawIndexed(da); dc++;
	}

	void end() { if (!overrideRTV) ctx->Flush(); if (!overrideRTV) sc->Present(1); fn++; }

	void drawInstanced(MeshHandle mh, const Vector<Mat4>& worldMatrices) {
		if (worldMatrices.empty()) return;
		auto* md = meshes.get(mh.index, mh.generation); if (!md || md->sub.empty()) return;
		PSOHandle activePSO = wireframe ? defPSO_Inst_wire : defPSO_Inst;
		if (gBufferActive) activePSO = gbufPSO_Inst;
		auto* pd = psos.get(activePSO.index, activePSO.generation); if (!pd) return;
		UInt32 count = (UInt32)Min(worldMatrices.size(), (Size)MaxInstances);

		// Upload instance world matrices (column-major, directly from glm)
		ctx->UpdateBuffer(instanceCB, 0, count * (D::Uint32)sizeof(Mat4), worldMatrices.data(), D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

		ctx->SetPipelineState(pd->pso);
		{ FrameConstants fc{}; fc.viewProj = cam.proj * cam.view; fc.cameraPos = Vec4(cam.desc.pos, 1.0f); fc.ambient = ambient; applyEnvironment(fc); fc.lightCount = (UInt32)activeLights.size(); for(int i=0;i<4;i++) fc.shadowMapUVDepth[i]=shadowMapUVDepth[i]; fc.cascadeSplits = cascadeSplits; void* m = nullptr; ctx->MapBuffer(frameCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m); if (m) { memcpy(m, &fc, sizeof(fc)); ctx->UnmapBuffer(frameCB, D::MAP_WRITE); } }
		{ void* m = nullptr; ctx->MapBuffer(lightCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m); if (m) { memcpy(m, &lcBuf, sizeof(lcBuf)); ctx->UnmapBuffer(lightCB, D::MAP_WRITE); } }
		D::Uint64 offsets[] = { 0, 0 };
		D::IBuffer* pBuffs[] = { md->vb.RawPtr(), instanceCB.RawPtr() };
		ctx->SetVertexBuffers(0, 2, pBuffs, offsets, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION, D::SET_VERTEX_BUFFERS_FLAG_RESET);
		ctx->SetIndexBuffer(md->ib, 0, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

		for (auto& s : md->sub) {
			auto* mt = materials.get(s.material.index, s.material.generation);
			if (mt && mt->objCB) { ObjectConstants oc; oc.world = Mat4(1.0f); oc.normalMat = Mat4(1.0f); oc.baseColor = mt->desc.baseColorFactor; oc.metallicRough = Vec4(mt->desc.metallicFactor, mt->desc.roughnessFactor, 0, 0); oc.emissive = Vec4(mt->desc.emissiveFactor, 1.0f); oc.nmSign = nmSign; oc.mapFlags = Vec4(mt->desc.normalTexture.isValid() ? 1.0f : 0.0f, mt->desc.metallicRoughnessTexture.isValid() ? 1.0f : 0.0f, mt->desc.emissiveTexture.isValid() ? 1.0f : 0.0f, normalMapDebug ? 1.0f : 0.0f); void* m = nullptr; ctx->MapBuffer(mt->objCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m); if (m) { memcpy(m, &oc, sizeof(oc)); ctx->UnmapBuffer(mt->objCB, D::MAP_WRITE); } }
			D::IShaderResourceBinding* srb = mt ? mt->srb.RawPtr() : nullptr;
			if (gBufferActive && mt) srb = mt->gbufSRB.RawPtr();
			if (mt && srb) {
				if (!gBufferActive) {
					auto* sv = srb->GetVariableByName(D::SHADER_TYPE_PIXEL, "g_ShadowMap");
					if (sv) sv->Set(shadowSRV ? shadowSRV : shadowDummy, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
				}
				ctx->CommitShaderResources(srb, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
			}
			D::DrawIndexedAttribs da; da.IndexType = D::VT_UINT32; da.NumIndices = s.indexCount; da.FirstIndexLocation = s.indexOffset; da.BaseVertex = (D::Uint32)s.vertexOffset; da.NumInstances = count; da.Flags = D::DRAW_FLAG_VERIFY_ALL;
			ctx->DrawIndexed(da); dc++;
		}
	}

	void drawInstancedIndirect(MeshHandle mh, ComputeSRV worldMatSRV, ComputeSRV indicesSRV, ComputeBuf indirectArgsBuf, UInt32 argsByteOffset) {
		auto* md = meshes.get(mh.index, mh.generation); if (!md || md->sub.empty()) return;
		auto* pd = psos.get(defPSO_Indirect.index, defPSO_Indirect.generation); if (!pd) return;

		ctx->SetPipelineState(pd->pso);
		{ FrameConstants fc{}; fc.viewProj = cam.proj * cam.view; fc.cameraPos = Vec4(cam.desc.pos, 1.0f); fc.ambient = ambient; applyEnvironment(fc); fc.lightCount = (UInt32)activeLights.size(); for(int i=0;i<4;i++) fc.shadowMapUVDepth[i]=shadowMapUVDepth[i]; fc.cascadeSplits = cascadeSplits; void* m = nullptr; ctx->MapBuffer(frameCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m); if (m) { memcpy(m, &fc, sizeof(fc)); ctx->UnmapBuffer(frameCB, D::MAP_WRITE); } }
		{ void* m = nullptr; ctx->MapBuffer(lightCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m); if (m) { memcpy(m, &lcBuf, sizeof(lcBuf)); ctx->UnmapBuffer(lightCB, D::MAP_WRITE); } }
		D::Uint64 vo = 0; D::IBuffer* vbs[] = { md->vb.RawPtr() };
		ctx->SetVertexBuffers(0, 1, vbs, &vo, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION, D::SET_VERTEX_BUFFERS_FLAG_RESET);
		ctx->SetIndexBuffer(md->ib, 0, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

		for (auto& s : md->sub) {
			auto* mt = materials.get(s.material.index, s.material.generation);
			D::RefCntAutoPtr<D::IShaderResourceBinding> srb;
			pd->pso->CreateShaderResourceBinding(&srb, true);
			if (!srb) continue;

			if (worldMatSRV) { auto* v = srb->GetVariableByName(D::SHADER_TYPE_VERTEX, "g_WorldMatrices"); if (v) v->Set(static_cast<D::IDeviceObject*>(worldMatSRV)); }
			if (indicesSRV) { auto* v = srb->GetVariableByName(D::SHADER_TYPE_VERTEX, "g_Indices"); if (v) v->Set(static_cast<D::IDeviceObject*>(indicesSRV)); }
			// Mutable variables have to be bound explicitly on a fresh SRB, and an
			// unbound one does not fail loudly - it samples whatever descriptor sits in
			// that heap slot. Copy *every* map binding from the material's SRB, with the
			// same neutral fallbacks mkMat() uses, so this path shades the same textures
			// as the regular draw() path.
			{
				const char* mapNames[4] = { "t_BC", "t_NormalMap", "t_MR", "t_EmissiveMap" };
				D::IDeviceObject* fallbacks[4] = { whiteSRV.RawPtr(), flatNormalSRV.RawPtr(), whiteSRV.RawPtr(), whiteSRV.RawPtr() };
				for (int mi = 0; mi < 4; ++mi) {
					auto* v = srb->GetVariableByName(D::SHADER_TYPE_PIXEL, mapNames[mi]);
					if (!v) continue;
					D::IDeviceObject* obj = fallbacks[mi];
					if (mt && mt->srb) {
						if (auto* src = mt->srb->GetVariableByName(D::SHADER_TYPE_PIXEL, mapNames[mi])) {
							if (auto* bound = src->Get()) obj = bound;
						}
					}
					v->Set(obj, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
				}
			}
			if (auto* v = srb->GetVariableByName(D::SHADER_TYPE_PIXEL, "g_ShadowMap"))
				v->Set(shadowSRV ? shadowSRV : shadowDummy, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);

			if (mt) {
				void* m = nullptr; ctx->MapBuffer(mt->objCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m);
				if (m) { ObjectConstants oc; oc.world = Mat4(1.0f); oc.normalMat = Mat4(1.0f); oc.baseColor = mt->desc.baseColorFactor; oc.metallicRough = Vec4(mt->desc.metallicFactor, mt->desc.roughnessFactor, 0, 0); oc.emissive = Vec4(mt->desc.emissiveFactor, 1.0f); oc.nmSign = nmSign; oc.mapFlags = Vec4(mt->desc.normalTexture.isValid() ? 1.0f : 0.0f, mt->desc.metallicRoughnessTexture.isValid() ? 1.0f : 0.0f, mt->desc.emissiveTexture.isValid() ? 1.0f : 0.0f, normalMapDebug ? 1.0f : 0.0f); memcpy(m, &oc, sizeof(oc)); ctx->UnmapBuffer(mt->objCB, D::MAP_WRITE); }
				if (auto* v = srb->GetVariableByName(D::SHADER_TYPE_VERTEX, "Object"))
					v->Set(mt->objCB, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
			}
			ctx->CommitShaderResources(srb, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
			D::DrawIndexedIndirectAttribs dia; dia.IndexType = D::VT_UINT32; dia.pAttribsBuffer = static_cast<D::IBuffer*>(indirectArgsBuf); dia.Flags = D::DRAW_FLAG_VERIFY_ALL; dia.DrawCount = 1; dia.DrawArgsOffset = argsByteOffset;
			ctx->DrawIndexedIndirect(dia); dc++;
		}
	}

	void drawShadowIndirect(class ShadowSubsystem& sh, MeshHandle mh, ComputeSRV worldMatSRV, ComputeSRV indicesSRV, ComputeBuf indirectArgsBuf, UInt32 argsByteOffset) {
		auto* md = meshes.get(mh.index, mh.generation); if (!md) return;
		auto* pd = psos.get(defPSO_ShadowIndirect.index, defPSO_ShadowIndirect.generation); if (!pd) return;

		ctx->SetPipelineState(pd->pso);
		D::Uint64 vo = 0; D::IBuffer* vbs[] = { md->vb.RawPtr() };
		ctx->SetVertexBuffers(0, 1, vbs, &vo, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION, D::SET_VERTEX_BUFFERS_FLAG_RESET);
		ctx->SetIndexBuffer(md->ib, 0, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		D::Viewport vp; vp.Width = (float)sh.config().resolution; vp.Height = (float)sh.config().resolution; vp.MinDepth = 0; vp.MaxDepth = 1;
		ctx->SetViewports(1, &vp, sh.config().resolution, sh.config().resolution);

		for (UInt32 c = 0; c < sh.config().numCascades; c++) {
			auto* dsv = static_cast<D::ITextureView*>(sh.getCascadeDSV(c)); if (!dsv) continue;
			D::ITextureView* nr = nullptr;
			ctx->SetRenderTargets(0, &nr, dsv, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
			Mat4 vpMat = sh.getCascadeTransform(c);
			FrameConstants fc{}; fc.viewProj = vpMat; fc.cameraPos = Vec4(0,0,0,1); fc.ambient = Vec4(0); fc.lightCount = 0; for(int i=0;i<4;i++) fc.shadowMapUVDepth[i]=shadowMapUVDepth[i]; fc.cascadeSplits = cascadeSplits;
			void* m = nullptr; ctx->MapBuffer(frameCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m);
			if (m) { memcpy(m, &fc, sizeof(fc)); ctx->UnmapBuffer(frameCB, D::MAP_WRITE); }

			D::RefCntAutoPtr<D::IShaderResourceBinding> srb;
			pd->pso->CreateShaderResourceBinding(&srb, true);
			if (!srb) continue;
			if (auto* v = srb->GetVariableByName(D::SHADER_TYPE_VERTEX, "g_WorldMatrices"))
				v->Set(static_cast<D::IDeviceObject*>(worldMatSRV));
			if (auto* v = srb->GetVariableByName(D::SHADER_TYPE_VERTEX, "g_Indices"))
				v->Set(static_cast<D::IDeviceObject*>(indicesSRV));
			ctx->CommitShaderResources(srb, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

			D::DrawIndexedIndirectAttribs dia; dia.IndexType = D::VT_UINT32; dia.pAttribsBuffer = static_cast<D::IBuffer*>(indirectArgsBuf); dia.Flags = D::DRAW_FLAG_VERIFY_ALL; dia.DrawCount = 1; dia.DrawArgsOffset = argsByteOffset;
			ctx->DrawIndexedIndirect(dia); dc++;
		}
	}

	void drawSkybox() {
		if (!skyDesc.has_value() || !skyMesh.vb) return;
		bool useCubemap = skyDesc->skyCubeTex.isValid() && skyCubePSO.isValid();
		auto* psoH = useCubemap ? &skyCubePSO : &skyPSO;
		if (!psoH->isValid()) return;
		auto* pd = psos.get(psoH->index, psoH->generation); if (!pd) return;

		ctx->SetPipelineState(pd->pso);
		{
			FrameConstants fc{}; fc.viewProj = cam.proj * cam.view; fc.cameraPos = Vec4(cam.desc.pos, 1.0f); fc.ambient = ambient; applyEnvironment(fc); fc.lightCount = 0; for(int i=0;i<4;i++) fc.shadowMapUVDepth[i]=shadowMapUVDepth[i]; fc.cascadeSplits = cascadeSplits;
			void* m = nullptr; ctx->MapBuffer(frameCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m); if (m) { memcpy(m, &fc, sizeof(fc)); ctx->UnmapBuffer(frameCB, D::MAP_WRITE); }
		}
		{
			void* m = nullptr; ctx->MapBuffer(skyCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m);
			if (m) { memcpy(m, skyDesc->corners, sizeof(skyDesc->corners)); ctx->UnmapBuffer(skyCB, D::MAP_WRITE); }
		}

		if (useCubemap) {
			auto* td = textures.get(skyDesc->skyCubeTex.index, skyDesc->skyCubeTex.generation);
			auto* sd = samplers.get(defSamp.index, defSamp.generation);
			if (td) {
				auto* tv = pd->srb->GetVariableByName(D::SHADER_TYPE_PIXEL, "g_SkyTex");
				if (tv) tv->Set(td->srv, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
			}
			if (sd) {
				auto* sv = pd->srb->GetVariableByName(D::SHADER_TYPE_PIXEL, "g_SkySamp");
				if (sv) sv->Set(sd->sampler);
			}
		}

		D::Uint64 vo = 0; D::IBuffer* vbs[] = { skyMesh.vb.RawPtr() };
		ctx->SetVertexBuffers(0, 1, vbs, &vo, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION, D::SET_VERTEX_BUFFERS_FLAG_RESET);
		ctx->SetIndexBuffer(skyMesh.ib, 0, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		ctx->CommitShaderResources(pd->srb, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		D::DrawIndexedAttribs da; da.IndexType = D::VT_UINT32; da.NumIndices = 36; da.Flags = D::DRAW_FLAG_VERIFY_ALL;
		ctx->DrawIndexed(da); dc++;
	}

	/// @brief Draw the sky into the six faces of envCube and build its mip chain.
	///
	/// Recording-side, so the caller has to be inside a frame; runs once per sky
	/// change (see envDirty). Six small draws plus one mip generation - a couple of
	/// milliseconds at worst, and only when the sky or its colours change.
	void buildEnvCube() {
		if (!ok || !skyMesh.vb || !envCornerPSO.isValid()) {
			if (!envBuildWarned) {
				envBuildWarned = true;
				EWarn("RenderSubsystem: the sky environment cannot be built yet (ok={}, skyMesh={}, cornerPSO={}); specular IBL stays black.",
					ok, skyMesh.vb != nullptr, envCornerPSO.isValid());
			}
			return;
		}

		const bool useCubemap = skyDesc.has_value() && skyDesc->skyCubeTex.isValid() && envCubeSkyPSO.isValid();
		PSOHandle srcPso = useCubemap ? envCubeSkyPSO : envCornerPSO;
		auto* pd = psos.get(srcPso.index, srcPso.generation);
		if (!pd) return;

		// The colours the visible skybox uses, with the same fallback to the ambient
		// colour that applyEnvironment() gives the shading paths.
		{
			Vec4 corners[8];
			for (int i = 0; i < 8; ++i) corners[i] = skyDesc.has_value() ? skyDesc->corners[i] : ambient;
			void* m = nullptr;
			ctx->MapBuffer(skyCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m);
			if (m) { memcpy(m, corners, sizeof(corners)); ctx->UnmapBuffer(skyCB, D::MAP_WRITE); }
		}
		if (useCubemap) {
			if (auto* td = textures.get(skyDesc->skyCubeTex.index, skyDesc->skyCubeTex.generation)) {
				if (auto* v = pd->srb->GetVariableByName(D::SHADER_TYPE_PIXEL, "g_SkyTex")) v->Set(td->srv, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
			}
			if (auto* sd = samplers.get(defSamp.index, defSamp.generation)) {
				if (auto* v = pd->srb->GetVariableByName(D::SHADER_TYPE_PIXEL, "g_SkySamp")) v->Set(sd->sampler);
			}
		}

		ctx->SetPipelineState(pd->pso);
		D::Viewport vp; vp.Width = (F32)kEnvCubeSize; vp.Height = (F32)kEnvCubeSize; vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
		ctx->SetViewports(1, &vp, kEnvCubeSize, kEnvCubeSize);
		D::Uint64 vo = 0; D::IBuffer* vbs[] = { skyMesh.vb.RawPtr() };
		ctx->SetVertexBuffers(0, 1, vbs, &vo, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION, D::SET_VERTEX_BUFFERS_FLAG_RESET);
		ctx->SetIndexBuffer(skyMesh.ib, 0, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		for (UInt32 f = 0; f < 6; ++f) {
			if (!envCubeRTV[f]) continue;
			D::ITextureView* rtv = envCubeRTV[f];
			ctx->SetRenderTargets(1, &rtv, nullptr, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
			// The sky vertex shader strips translation and forces z = w, so the face
			// matrix is all the frame constants have to carry.
			FrameConstants fc{};
			fc.viewProj = envFaceMatrix(f, envFlipU, envFlipV, envMirrorY);
			fc.cameraPos = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
			fc.ambient = ambient;
			applyEnvironment(fc);
			void* m = nullptr;
			ctx->MapBuffer(frameCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m);
			if (m) { memcpy(m, &fc, sizeof(fc)); ctx->UnmapBuffer(frameCB, D::MAP_WRITE); }
			ctx->CommitShaderResources(pd->srb, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
			D::DrawIndexedAttribs da; da.IndexType = D::VT_UINT32; da.NumIndices = 36; da.Flags = D::DRAW_FLAG_VERIFY_ALL;
			ctx->DrawIndexed(da); dc++;
		}

		// Mip chain: the levels the shading paths pick from by roughness. The texture
		// carries MISC_TEXTURE_FLAG_GENERATE_MIPS, so the view has to be an SRV that
		// covers them all - which the default view does.
		ctx->GenerateMips(envCubeSRV);

		// The graphics passes that follow expect the full-window viewport back.
		ctx->SetRenderTargets(0, nullptr, nullptr, D::RESOURCE_STATE_TRANSITION_MODE_NONE);
		D::Viewport full; full.Width = (F32)w; full.Height = (F32)h; full.MinDepth = 0.0f; full.MaxDepth = 1.0f;
		ctx->SetViewports(1, &full, w, h);

		envDirty = false;
		EInfo("RenderSubsystem: sky environment rebuilt ({}x{}, {} mip levels, {}{}).",
			kEnvCubeSize, kEnvCubeSize, kEnvCubeMips, useCubemap ? "cubemap sky" : "corner sky",
			envEnabled ? "" : ", specular IBL off");
	}

	// --------------------------------------------------------------
	// glTF model loading (fastgltf)
	// --------------------------------------------------------------

	static void readIndices(const fastgltf::Asset& asset, const fastgltf::Accessor& acc, Vector<UInt32>& out, UInt32 base) {
		switch (acc.componentType) {
		case fastgltf::ComponentType::UnsignedByte: fastgltf::iterateAccessorWithIndex<UInt8>(asset, acc, [&](UInt8 v, size_t) { out.push_back(base + (UInt32)v); }); break;
		case fastgltf::ComponentType::UnsignedShort: fastgltf::iterateAccessorWithIndex<UInt16>(asset, acc, [&](UInt16 v, size_t) { out.push_back(base + (UInt32)v); }); break;
		case fastgltf::ComponentType::UnsignedInt: fastgltf::iterateAccessorWithIndex<UInt32>(asset, acc, [&](UInt32 v, size_t) { out.push_back(base + v); }); break;
		case fastgltf::ComponentType::Short: fastgltf::iterateAccessorWithIndex<Int16>(asset, acc, [&](Int16 v, size_t) { out.push_back(base + (UInt32)v); }); break;
		case fastgltf::ComponentType::Int: fastgltf::iterateAccessorWithIndex<Int32>(asset, acc, [&](Int32 v, size_t) { out.push_back(base + (UInt32)v); }); break;
		default: break;
		}
	}

	Result<ModelLoadResult, RenderError> loadModelInternal(const std::filesystem::path& filePath, const std::filesystem::path& rootDir) {
		auto dataBuf = fastgltf::GltfDataBuffer::FromPath(filePath);
		if (dataBuf.error() != fastgltf::Error::None) { EWarn("fastgltf read: {}, file: '{}'", fastgltf::getErrorName(dataBuf.error()), filePath.string()); return RenderError::ModelLoadFailed; }
		return loadModelFromBuffer(dataBuf.get(), rootDir, filePath.string());
	}

	Result<ModelLoadResult, RenderError> loadModelFromMemory(const Byte* data, Size size, const std::filesystem::path& rootDir) {
		auto dataBuf = fastgltf::GltfDataBuffer::FromBytes(reinterpret_cast<const std::byte*>(data), size);
		if (dataBuf.error() != fastgltf::Error::None) { EWarn("fastgltf read from memory: {}", fastgltf::getErrorName(dataBuf.error())); return RenderError::ModelLoadFailed; }
		return loadModelFromBuffer(dataBuf.get(), rootDir, "(memory)");
	}

	Result<ModelLoadResult, RenderError> loadModelFromBuffer(fastgltf::GltfDataGetter& dataBuf, const std::filesystem::path& rootDir, const String& label) {
		using Ext = fastgltf::Extensions;
		fastgltf::Parser parser(Ext::KHR_texture_transform);
		auto assetRes = parser.loadGltf(dataBuf, rootDir, fastgltf::Options::LoadExternalBuffers | fastgltf::Options::LoadExternalImages);
		if (assetRes.error() != fastgltf::Error::None) { EWarn("fastgltf parse: {}, file: '{}'", fastgltf::getErrorName(assetRes.error()), label); return RenderError::ModelLoadFailed; }
		auto& asset = assetRes.get();
		ModelLoadResult result;

		EInfo("glTF: {} images, {} textures, {} materials, {} meshes",
			asset.images.size(), asset.textures.size(), asset.materials.size(), asset.meshes.size());

		// ---- Classify the images by role before creating them ----------------
		//
		// A glTF image is either *colour* (base colour, emissive) or *data*
		// (normal, metallic-roughness, occlusion). Colour images are sRGB-encoded
		// and must be created with an sRGB format so the sampler decodes them to
		// linear; data images are already linear and must NOT be decoded. Every
		// image used to go through the same RGBA8_UNORM_SRGB path, which warped
		// normal vectors and turned a stored roughness of 0.5 into ~0.21 - and the
		// mip chain then filtered it in the wrong space as well.
		//
		// The role is only known from the material slots, so the materials are
		// scanned first. An image that is never referenced, or that appears in both
		// kinds of slot, keeps the sRGB view (the safe default for textures).
		HashMap<UInt32, UInt8> imageSrgb; // image index -> 1 = sRGB colour, 0 = linear data
		auto imageIndexOf = [&](const auto& info) -> Int32 {
			if (!info.has_value()) return -1;
			const size_t ti = (size_t)info->textureIndex;
			if (ti >= asset.textures.size()) return -1;
			if (!asset.textures[ti].imageIndex.has_value()) return -1;
			return (Int32)asset.textures[ti].imageIndex.value();
		};
		auto markImage = [&](const auto& info, bool srgb, const char* role) {
			const Int32 img = imageIndexOf(info);
			if (img < 0) return;
			auto it = imageSrgb.find((UInt32)img);
			if (it == imageSrgb.end()) { imageSrgb[(UInt32)img] = srgb ? 1 : 0; return; }
			if ((it->second != 0) != srgb) {
				EWarn("glTF image {} is referenced both as colour and as data ({}); keeping the sRGB view - author the two uses as separate images.", img, role);
			}
		};
		for (auto& m : asset.materials) {
			markImage(m.pbrData.baseColorTexture, true, "baseColor");
			markImage(m.emissiveTexture, true, "emissive");
			markImage(m.pbrData.metallicRoughnessTexture, false, "metallicRoughness");
			markImage(m.normalTexture, false, "normal");
			markImage(m.occlusionTexture, false, "occlusion");
		}
		auto imageIsSrgb = [&](UInt32 img) -> bool {
			auto it = imageSrgb.find(img);
			return it == imageSrgb.end() ? true : (it->second != 0);
		};
		auto imageFormat = [&](UInt32 img) {
			return imageIsSrgb(img) ? TextureFormat::RGBA8_UNorm_SRGB : TextureFormat::RGBA8_UNorm;
		};

		// Load textures from glTF images
		HashMap<UInt32, TextureHandle> texMap;
		for (size_t i = 0; i < asset.images.size(); ++i) {
			auto& img = asset.images[i];
			TextureHandle th;
			const char* srcType = "none";
			// External texture file (URI source)
			if (auto* uriSrc = std::get_if<fastgltf::sources::URI>(&img.data)) {
				srcType = "URI";
				String imgPath = (rootDir / uriSrc->uri.fspath()).string();
				D::TextureLoadInfo loadInfo;
				loadInfo.IsSRGB = imageIsSrgb((UInt32)i);
				D::RefCntAutoPtr<D::ITexture> tex;
				D::CreateTextureFromFile(imgPath.c_str(), loadInfo, device.RawPtr(), &tex);
				if (tex) {
					auto a = textures.allocate(); auto* dd = textures.getUnchecked(a.index);
					dd->tex = std::move(tex); dd->srv = dd->tex->GetDefaultView(D::TEXTURE_VIEW_SHADER_RESOURCE);
					th = TextureHandle{ a.index, a.generation };
				}
				else { EWarn("Failed to load glTF texture: {}", imgPath); }
			}
			else if (auto* bv = std::get_if<fastgltf::sources::BufferView>(&img.data)) {
				srcType = "BufferView";
				auto& bufferView = asset.bufferViews[bv->bufferViewIndex];
				auto& buffer = asset.buffers[bufferView.bufferIndex];
				std::visit([&](auto& srcData) {
					using T = std::decay_t<decltype(srcData)>;
					const void* s = nullptr; int len = 0;
					if constexpr (std::is_same_v<T, fastgltf::sources::Array> || std::is_same_v<T, fastgltf::sources::Vector>) {
						s = reinterpret_cast<const uint8_t*>(srcData.bytes.data()) + bufferView.byteOffset;
						len = (int)(srcData.bytes.size() - bufferView.byteOffset);
					}
					else if constexpr (std::is_same_v<T, fastgltf::sources::ByteView>) {
						s = reinterpret_cast<const uint8_t*>(srcData.bytes.data()) + bufferView.byteOffset;
						len = (int)(srcData.bytes.size() - bufferView.byteOffset);
					}
					if (s && len > 0) {
						auto decoded = Utilities::decodeImage(s, (Size)len);
						if (decoded.isValid()) {
							TextureDesc td; td.fmt = imageFormat((UInt32)i); td.w = decoded.width; td.h = decoded.height;
							td.data = decoded.pixels.data(); td.dataSize = (UInt32)decoded.pixels.size();
							// Full mip chain: the ray tracer samples the reflection
							// hit at a level matched to the reflection lobe, and the
							// raster path gets derivative-based filtering for free.
							td.mipChain = true;
							auto tr = mkTex(td); if (tr.isOk()) th = tr.value();
						}
					}
					}, buffer.data);
			}
			else if (auto* arr = std::get_if<fastgltf::sources::Array>(&img.data)) {
				srcType = "Array";
				auto decoded = Utilities::decodeImage(arr->bytes.data(), arr->bytes.size());
				if (decoded.isValid()) {
					TextureDesc td; td.fmt = imageFormat((UInt32)i); td.w = decoded.width; td.h = decoded.height;
					td.data = decoded.pixels.data(); td.dataSize = (UInt32)decoded.pixels.size();
					td.mipChain = true; // prefiltered levels for ray-traced reflections
					auto tr = mkTex(td); if (tr.isOk()) th = tr.value();
				}
			}
			else if (auto* bv2 = std::get_if<fastgltf::sources::ByteView>(&img.data)) {
				srcType = "ByteView";
				auto decoded = Utilities::decodeImage(bv2->bytes.data(), bv2->bytes.size());
				if (decoded.isValid()) {
					TextureDesc td; td.fmt = imageFormat((UInt32)i); td.w = decoded.width; td.h = decoded.height;
					td.data = decoded.pixels.data(); td.dataSize = (UInt32)decoded.pixels.size();
					td.mipChain = true; // prefiltered levels for ray-traced reflections
					auto tr = mkTex(td); if (tr.isOk()) th = tr.value();
				}
			}
			// Every image slot gets at least a placeholder (1x1 white)
			if (!th.isValid()) {
				UInt32 white = 0xFFFFFFFF;
				TextureDesc td; td.fmt = TextureFormat::RGBA8_UNorm_SRGB; td.w = 1; td.h = 1; td.data = &white; td.dataSize = 4;
				auto tr = mkTex(td); if (tr.isOk()) th = tr.value();
			}
			ETrace("Image[{}] src={} -> texHandle={} {}", i, srcType, th.index,
				th.isValid() ? (th.index != 0xFFFFFFFF ? "OK" : "placeholder") : "FAIL");
			texMap[(UInt32)i] = th;
		}

		HashMap<UInt32, MaterialHandle> matMap;
		for (size_t i = 0; i < asset.materials.size(); ++i) {
			auto& m = asset.materials[i];
			MaterialDesc md; md.name = !m.name.empty() ? String(m.name.begin(), m.name.end()) : "Mat";
			auto& c = m.pbrData.baseColorFactor; md.baseColorFactor = Vec4(c.x(), c.y(), c.z(), c.w());
			md.metallicFactor = m.pbrData.metallicFactor;
			md.roughnessFactor = m.pbrData.roughnessFactor;
			bool hasTex = false;
			// Every PBR map goes through the same Texture -> Image mapping. The data
			// maps used to not be resolved at all, so imported assets were shaded
			// from the base colour texture plus the scalar factors alone: no normal
			// detail, no per-texel roughness/metallic, no emissive map.
			auto resolveMap = [&](const auto& info) -> TextureHandle {
				const Int32 img = imageIndexOf(info);
				if (img < 0) return TextureHandle{};
				auto it = texMap.find((UInt32)img);
				return it != texMap.end() ? it->second : TextureHandle{};
			};
			md.baseColorTexture         = resolveMap(m.pbrData.baseColorTexture);
			md.metallicRoughnessTexture = resolveMap(m.pbrData.metallicRoughnessTexture);
			md.normalTexture            = resolveMap(m.normalTexture);
			md.emissiveTexture          = resolveMap(m.emissiveTexture);
			md.aoTexture                = resolveMap(m.occlusionTexture);
			hasTex = md.baseColorTexture.isValid();
			// glTF emissiveFactor: self-emission color (also feeds RT reflections).
			{
				auto& ef = m.emissiveFactor;
				md.emissiveFactor = Vec3(ef[0], ef[1], ef[2]);
			}
			ETrace("Material[{}] '{}': baseColor=({:.2f},{:.2f},{:.2f},{:.2f}) metal={:.2f} rough={:.2f} tex={}",
				i, md.name, md.baseColorFactor.x, md.baseColorFactor.y, md.baseColorFactor.z, md.baseColorFactor.w,
				md.metallicFactor, md.roughnessFactor, hasTex ? "yes" : "no");
			// Diagnostic for the classic path's PBR maps: the mesh shader path renders
			// every one of these correctly, so if a material's map resolves to a
			// handle that looks wrong here, that is the bug to chase (see the notes in
			// g_PS / g_PS_GBuffer about why the sampling is not enabled yet).
			EInfo("Material[{}] '{}': maps base={} normal={} mr={} emissive={} ao={}",
				i, md.name,
				md.baseColorTexture.isValid() ? md.baseColorTexture.index : ~0u,
				md.normalTexture.isValid() ? md.normalTexture.index : ~0u,
				md.metallicRoughnessTexture.isValid() ? md.metallicRoughnessTexture.index : ~0u,
				md.emissiveTexture.isValid() ? md.emissiveTexture.index : ~0u,
				md.aoTexture.isValid() ? md.aoTexture.index : ~0u);
			auto mr = mkMat(md, defPSO); if (mr.isOk()) { matMap[(UInt32)i] = mr.value(); result.materials.push_back(mr.value()); }
		}

		for (size_t mi = 0; mi < asset.meshes.size(); ++mi) {
			auto& mesh = asset.meshes[mi];
			Vector<Vertex> allVerts; Vector<UInt32> allIdx; Vector<SubMesh> subMeshes;
			for (size_t pi = 0; pi < mesh.primitives.size(); ++pi) {
				auto& prim = mesh.primitives[pi];
				SubMesh sub; sub.vertexOffset = (UInt32)allVerts.size(); sub.indexOffset = (UInt32)allIdx.size();
				auto posIt = prim.findAttribute("POSITION"); if (posIt == prim.attributes.end()) continue;
				auto& posAcc = asset.accessors[posIt->accessorIndex]; UInt32 vc = (UInt32)posAcc.count; if (vc == 0) continue;
				Vector<Vec3> pos(vc, Vec3(0)); fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(asset, posAcc, [&](fastgltf::math::fvec3 p, size_t j) { pos[(UInt32)j] = Vec3(p.x(), p.y(), p.z()); });
				Vector<Vec3> nrm(vc, Vec3(0, 1, 0));
				auto normIt = prim.findAttribute("NORMAL"); if (normIt != prim.attributes.end()) { auto& acc = asset.accessors[normIt->accessorIndex]; fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(asset, acc, [&](fastgltf::math::fvec3 n, size_t j) { nrm[(UInt32)j] = Vec3(n.x(), n.y(), n.z()); }); }
				Vector<Vec2> uv(vc, Vec2(0));
				auto tcIt = prim.findAttribute("TEXCOORD_0");
				if (tcIt != prim.attributes.end()) { auto& acc = asset.accessors[tcIt->accessorIndex]; fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(asset, acc, [&](fastgltf::math::fvec2 t, size_t j) { uv[(UInt32)j] = Vec2(t.x(), t.y()); }); }
				ETrace("  Prim[{}]: {}v {}i uvs={}", pi, vc, prim.indicesAccessor.has_value() ? "indexed" : "none", tcIt != prim.attributes.end() ? "yes" : "no");
				// glTF TANGENT_0 is optional. The shaders currently build their tangent
				// frame from screen-space derivatives, but the attribute is carried
				// through (rather than the dummy (1,0,0,1) every vertex used to get) so
				// it can be used - and so the mesh round-trips - once the frames switch
				// to the asset's tangent, which is what the spec defines.
				Vector<Vec4> tan(vc, Vec4(1, 0, 0, 1));
				auto tanIt = prim.findAttribute("TANGENT");
				if (tanIt != prim.attributes.end()) { auto& acc = asset.accessors[tanIt->accessorIndex]; fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(asset, acc, [&](fastgltf::math::fvec4 t, size_t j) { tan[(UInt32)j] = Vec4(t.x(), t.y(), t.z(), t.w()); }); }
				UInt32 base = (UInt32)allVerts.size();
				for (UInt32 v = 0; v < vc; ++v) { allVerts.push_back({ pos[v], nrm[v], uv[v], tan[v] }); }
				if (prim.indicesAccessor.has_value()) readIndices(asset, asset.accessors[prim.indicesAccessor.value()], allIdx, base);
				sub.indexCount = (UInt32)allIdx.size() - sub.indexOffset;
				if (prim.materialIndex.has_value()) { auto it = matMap.find((UInt32)prim.materialIndex.value()); if (it != matMap.end()) sub.material = it->second; else if (!matMap.empty()) sub.material = matMap.begin()->second; }
				subMeshes.push_back(sub);
			}
			if (!allVerts.empty() && !allIdx.empty()) {
				MeshDesc md; md.name = String(mesh.name); md.vertices = std::move(allVerts); md.indices = std::move(allIdx); md.subMeshes = std::move(subMeshes);
				auto mres = mkMesh(md); if (mres.isOk()) result.meshes.push_back(mres.value());
			}
		}
		return result;
	}

	void drawShadowPass(ShadowSubsystem& sh, MeshHandle mh, const Mat4& wm) {
		auto* md = meshes.get(mh.index, mh.generation); if (!md) return;
		auto* pd = psos.get(shadowPSO.index, shadowPSO.generation); if (!pd) return;
		ctx->SetPipelineState(pd->pso);
		ctx->CommitShaderResources(pd->srb, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		D::Uint64 vo = 0; D::IBuffer* vbs[] = { md->vb.RawPtr() };
		ctx->SetVertexBuffers(0, 1, vbs, &vo, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION, D::SET_VERTEX_BUFFERS_FLAG_RESET);
		ctx->SetIndexBuffer(md->ib, 0, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		D::Viewport vp; vp.Width = (float)sh.config().resolution; vp.Height = (float)sh.config().resolution; vp.MinDepth = 0; vp.MaxDepth = 1;
		ctx->SetViewports(1, &vp, sh.config().resolution, sh.config().resolution);
		for (UInt32 c = 0; c < sh.config().numCascades; c++) {
			auto* dsv = static_cast<D::ITextureView*>(sh.getCascadeDSV(c)); if (!dsv) continue;
			D::ITextureView* nr = nullptr;
			ctx->SetRenderTargets(0, &nr, dsv, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
			Mat4 vpMat = sh.getCascadeTransform(c);
			FrameConstants fc{}; fc.viewProj = vpMat * wm; fc.cameraPos = Vec4(0,0,0,1); fc.ambient = Vec4(0); fc.lightCount = 0; for(int i=0;i<4;i++) fc.shadowMapUVDepth[i]=shadowMapUVDepth[i]; fc.cascadeSplits = cascadeSplits;
			void* m = nullptr; ctx->MapBuffer(frameCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m);
			if (m) { memcpy(m, &fc, sizeof(fc)); ctx->UnmapBuffer(frameCB, D::MAP_WRITE); }
			for (auto& s : md->sub) { D::DrawIndexedAttribs da; da.IndexType = D::VT_UINT32; da.NumIndices = s.indexCount; da.FirstIndexLocation = s.indexOffset; da.BaseVertex = (D::Uint32)s.vertexOffset; da.Flags = D::DRAW_FLAG_VERIFY_ALL; ctx->DrawIndexed(da); }
		}
	}
};

// ===================================================================
// RenderSubsystem public API
// ===================================================================

RenderSubsystem::RenderSubsystem() : Subsystem("Rendering"), m_backend(std::make_unique<RenderBackend>()) {}
RenderSubsystem::~RenderSubsystem() = default;

void RenderSubsystem::setWindow(void* w) { m_backend->wnd = w; }
bool RenderSubsystem::isReady() const { return m_backend->ok; }
void RenderSubsystem::setBackend(RenderBackendType t) { m_backend->backendType = t; }
RenderBackendType RenderSubsystem::backend() const { return m_backend->backendType; }

void RenderSubsystem::setRayTracingEnabled(bool enable) { m_backend->requestRayTracing = enable; }
bool RenderSubsystem::isRayTracingEnabled() const { return m_backend->requestRayTracing; }

void RenderSubsystem::setMeshShadersEnabled(bool enable) { m_backend->requestMeshShaders = enable; }
bool RenderSubsystem::isMeshShadersEnabled() const { return m_backend->requestMeshShaders; }
bool RenderSubsystem::supportsMeshShaders() const { return m_backend->msFeatureEnabled; }
RayTracingCaps RenderSubsystem::rayTracingCaps() const { return m_backend->rtCaps; }
bool RenderSubsystem::supportsInlineRayTracing() const { return hasRayTracingCap(m_backend->rtCaps, RayTracingCaps::InlineRayTracing); }
bool RenderSubsystem::supportsStandaloneRayTracing() const { return hasRayTracingCap(m_backend->rtCaps, RayTracingCaps::StandaloneShaders); }
UInt32 RenderSubsystem::maxRayRecursionDepth() const { return m_backend->rtMaxRecursionDepth; }
UInt32 RenderSubsystem::maxInstancesPerTLAS() const { return m_backend->rtMaxInstancesPerTLAS; }

void RenderSubsystem::setMSAASampleCount(UInt8 c) { m_backend->msaaSamples = c; }
UInt8 RenderSubsystem::msaaSamples() const { return m_backend->msaaSamples; }

void RenderSubsystem::setShadowSRV(TextureSRV srv) { m_backend->shadowSRV = static_cast<D::ITextureView*>(srv); }
TextureSRV RenderSubsystem::getShadowSRV() const { return m_backend->shadowSRV; }
void RenderSubsystem::setShadowData(const Mat4(&uv)[4], const Vec4& splits) { auto& b=*m_backend; for(int i=0;i<4;i++) b.shadowMapUVDepth[i]=uv[i]; b.cascadeSplits=splits; }

Result<ShaderHandle, RenderError> RenderSubsystem::createShader(const ShaderDesc& d) { if (!m_backend->ok) return RenderError::NotInitialized; return m_backend->mkShader(d); }
Result<ShaderHandle, RenderError> RenderSubsystem::createShaderFromFile(ShaderStage s, const String& fp, const String& ep) { ShaderDesc d; d.stage = s; d.filePath = fp; d.entryPoint = ep; return createShader(d); }
Result<PSOHandle, RenderError> RenderSubsystem::createPipelineState(const PipelineStateDesc& d) { if (!m_backend->ok) return RenderError::NotInitialized; return m_backend->mkPSO(d); }
Result<MeshHandle, RenderError> RenderSubsystem::createMesh(const MeshDesc& d) { if (!m_backend->ok) return RenderError::NotInitialized; return m_backend->mkMesh(d); }
Result<TextureHandle, RenderError> RenderSubsystem::createTexture(const TextureDesc& d) { if (!m_backend->ok) return RenderError::NotInitialized; return m_backend->mkTex(d); }
Result<SamplerHandle, RenderError> RenderSubsystem::createSampler(const SamplerDesc& d) { if (!m_backend->ok) return RenderError::NotInitialized; return m_backend->mkSampler(d); }
Result<MaterialHandle, RenderError> RenderSubsystem::createMaterial(const MaterialDesc& d, PSOHandle p) { if (!m_backend->ok) return RenderError::NotInitialized; return m_backend->mkMat(d, p); }

PSOHandle RenderSubsystem::defaultPSO() const { return m_backend->ok ? m_backend->defPSO : PSOHandle{}; }

Result<CameraHandle, RenderError> RenderSubsystem::createCamera(const CameraDesc& d) {
	if (!m_backend->ok) return RenderError::NotInitialized;
	CamData cd; cd.desc = d; cd.aspect = d.w / d.h; cd.proj = glm::perspective(glm::radians(d.fov), cd.aspect, d.nearP, d.farP); cd.view = glm::lookAt(d.pos, d.target, d.up);
	auto a = m_backend->cameras.allocate(); *m_backend->cameras.getUnchecked(a.index) = cd; CameraHandle h{ a.index, a.generation };
	if (!m_backend->camHandle.isValid()) { m_backend->camHandle = h; m_backend->cam = cd; }
	return h;
}
Result<void, RenderError> RenderSubsystem::updateCamera(CameraHandle h, const CameraDesc& d) {
	auto* c = m_backend->cameras.get(h.index, h.generation); if (!c) return RenderError::CameraNotFound;
	c->desc = d; c->proj = glm::perspective(glm::radians(d.fov), c->aspect, d.nearP, d.farP); c->view = glm::lookAt(d.pos, d.target, d.up);
	if (h == m_backend->camHandle) m_backend->cam = *c;
	return {};
}
void RenderSubsystem::setActiveCamera(CameraHandle h) { auto* c = m_backend->cameras.get(h.index, h.generation); if (c) { m_backend->camHandle = h; m_backend->cam = *c; } }
CameraHandle RenderSubsystem::activeCamera() const { return m_backend->camHandle; }

Result<LightHandle, RenderError> RenderSubsystem::createLight(const LightDesc& d) {
	if (!m_backend->ok) return RenderError::NotInitialized;
	if (m_backend->activeLights.size() >= MaxLights) return RenderError::PoolExhausted;
	RenderLightData ld; ld.desc = d; auto a = m_backend->lights.allocate(); *m_backend->lights.getUnchecked(a.index) = ld;
	LightHandle h{ a.index, a.generation }; m_backend->activeLights.push_back(h);
	UInt32 i = (UInt32)m_backend->activeLights.size() - 1;
	auto& lc = m_backend->lcBuf.lights[i]; lc.CI = Vec4(d.color, d.intensity); lc.DT = Vec4(d.dir, (F32)(UInt8)d.type); lc.PR = Vec4(d.pos, d.range); lc.CA = Vec4(d.innerCone, d.outerCone, 0, 0);
	return h;
}
Result<void, RenderError> RenderSubsystem::updateLight(LightHandle h, const LightDesc& d) {
	auto* ld = m_backend->lights.get(h.index, h.generation); if (!ld) return RenderError::LightNotFound;
	ld->desc = d; for (size_t i = 0; i < m_backend->activeLights.size(); ++i) { if (m_backend->activeLights[i] == h) { auto& lc = m_backend->lcBuf.lights[i]; lc.CI = Vec4(d.color, d.intensity); lc.DT = Vec4(d.dir, (F32)(UInt8)d.type); lc.PR = Vec4(d.pos, d.range); lc.CA = Vec4(d.innerCone, d.outerCone, 0, 0); break; } }
	return {};
}
void RenderSubsystem::setAmbientLight(const Vec3& c, F32 i) {
	const Vec4 next(c, i);
	if (next == m_backend->ambient) return;
	m_backend->ambient = next;
	// Without a skybox the environment cube is filled with the ambient colour, so a
	// change to it has to rebuild the cube too (with a skybox the corners drive it
	// and this is a no-op).
	if (!m_backend->skyDesc.has_value()) m_backend->envDirty = true;
}

Vec4 RenderSubsystem::getAmbientLight() const { return m_backend->ambient; }

bool RenderSubsystem::getPrimaryDirectionalLight(Vec3& dir, Vec4& color) const {
	if (!m_backend->ok) return false;
	for (size_t i = 0; i < m_backend->activeLights.size(); ++i) {
		const LightData& lc = m_backend->lcBuf.lights[i];
		if ((UInt8)lc.DT.w == (UInt8)LightType::Directional) {
			dir = Vec3(lc.DT);
			color = lc.CI;
			return true;
		}
	}
	return false;
}

Result<ModelLoadResult, RenderError> RenderSubsystem::loadModel(const String& fp) {
	if (!m_backend->ok) return RenderError::NotInitialized;
	using namespace std::filesystem;
	path filePath = path(fp);
	if (!exists(filePath)) {
		EError("Cannot find model file '{}'", fp);
		return RenderError::InvalidArgument;
	}
	return m_backend->loadModelInternal(filePath, filePath.parent_path().string());
}

Result<ModelLoadResult, RenderError> RenderSubsystem::loadModel(const ResPath& fp) {
	if (!m_backend->ok) return RenderError::NotInitialized;
	auto& rm = EnderEngine::ResourcesManager::getInstance();
	auto r = rm.readFile(fp);
	if (r.isErr()) { EError("Resource read failed: {}", fp.path.string()); return RenderError::ModelLoadFailed; }
	auto& data = r.value();
	Path rd = Path(fp.path).parent_path();
	if (rd == "") rd = "./";
	return m_backend->loadModelFromMemory(data.data(), data.size(), rd);
}
Result<ModelLoadResult, RenderError> RenderSubsystem::loadModel(const String& fp, const String& rd) {
	if (!m_backend->ok) return RenderError::NotInitialized;
	using namespace std::filesystem;
	path filePath = path(fp);
	if (!exists(filePath)) {
		EError("Cannot find model file '{}'", fp);
		return RenderError::InvalidArgument;
	}
	path rootDir = path(rd);
	if (!exists(rootDir) || !is_directory(rootDir)) {
		EError("Path '{}' doesn't exist or is not a directory", rd);
		return RenderError::InvalidArgument;
	}
	return m_backend->loadModelInternal(filePath, rootDir);
}
Result<ModelHandle, RenderError> RenderSubsystem::loadModelAsync(const String& fp, const String& rd, const Object& owner,
	Jobs::JobSubsystem& jobs, Jobs::JobPriority priority)
{
	if (!m_backend->ok) return RenderError::NotInitialized;

	using namespace std::filesystem;
	path filePath = path(fp);
	path rootDir = path(rd);

	// Pre-allocate a ModelHandle for the result
	auto a = m_backend->models.allocate();
	ModelHandle mh{ a.index, a.generation };
	EInfo("Async load queued: {} -> modelSlot={}", filePath.filename().string(), mh.index);

	auto* backend = m_backend.get();
	auto jr = jobs.dispatchFor(owner,
		[backend, filePath, rootDir, mh](const Jobs::JobContext& ctx) -> Result<void, Jobs::JobError> {
			EInfo("[worker {}] Async load begin: {}", ctx.workerIndex, filePath.filename().string());
			std::lock_guard<std::mutex> lock(backend->m_loadMutex);
			EInfo("[worker {}] Async load parsing glTF...", ctx.workerIndex);
			auto result = backend->loadModelInternal(filePath, rootDir);
			if (result.isErr()) {
				EWarn("Async model load failed for {}: {}", filePath.string(), ToString(result.error()));
				return Jobs::JobError::OperationFailed;
			}
			// Store result in the pre-allocated model slot
			auto* slot = backend->models.get(mh.index, mh.generation);
			if (slot) {
				slot->result = result.value();
				EInfo("Async model loaded: {} ({} meshes)", filePath.string(), slot->result.meshes.size());
			}
			else {
				EWarn("[worker] Model slot {} invalid after load!", mh.index);
			}
			EInfo("[worker] Async load job returning success (event will fire now)");
			return {};
		}, priority);

	if (jr.isErr()) {
		m_backend->models.release(mh.index);
		EError("Failed to dispatch async model load: {}", ToString(jr.error()));
		return RenderError::OperationFailed;
	}
	return mh;
}

Result<ModelHandle, RenderError> RenderSubsystem::loadModelAsync(const String& fp, const Object& owner,
	Jobs::JobSubsystem& jobs, Jobs::JobPriority priority) {
	auto rd = std::filesystem::path(fp).parent_path().string();
	if (rd == "") rd = "./";
	return loadModelAsync(fp, rd, owner, jobs, priority);
}

Result<ModelHandle, RenderError> RenderSubsystem::loadModelAsync(const ResPath& fp, const ResPath& rd, const Object& owner,
	Jobs::JobSubsystem& jobs, Jobs::JobPriority priority)
{
	if (!m_backend->ok) return RenderError::NotInitialized;

	auto& rm = EnderEngine::ResourcesManager::getInstance();
	auto r = rm.readFile(fp);
	if (r.isErr()) { EError("Resource read failed: {}, file: '{}'", ToString(r.error()), fp.path.string()); return RenderError::ModelLoadFailed; }
	auto data = std::make_shared<std::vector<Byte>>(std::move(r.value()));

	auto a = m_backend->models.allocate();
	ModelHandle mh{ a.index, a.generation };
	EInfo("Async resource load queued: {} -> modelSlot={}", fp.path.filename().string(), mh.index);

	auto* backend = m_backend.get();
	Path rootDir(rd.path);
	if (rootDir == "") rootDir = "./";
	auto jr = jobs.dispatchFor(owner,
		[backend, mh, data, rootDir](const Jobs::JobContext& ctx) -> Result<void, Jobs::JobError> {
			EInfo("[worker {}] Async resource load begin: in-memory", ctx.workerIndex);
			std::lock_guard<std::mutex> lock(backend->m_loadMutex);
			auto result = backend->loadModelFromMemory(data->data(), data->size(), rootDir);
			if (result.isErr()) {
				EWarn("Async resource load failed: {}", ToString(result.error()));
				return Jobs::JobError::OperationFailed;
			}
			auto* slot = backend->models.get(mh.index, mh.generation);
			if (slot) {
				slot->result = result.value();
				EInfo("Async resource loaded: ({} meshes)", slot->result.meshes.size());
			}
			return {};
		}, priority);

	if (jr.isErr()) {
		m_backend->models.release(mh.index);
		EError("Failed to dispatch async resource load: {}", ToString(jr.error()));
		return RenderError::OperationFailed;
	}
	return mh;
}

Result<ModelHandle, RenderError> RenderSubsystem::loadModelAsync(const ResPath& fp, const Object& owner,
	Jobs::JobSubsystem& jobs, Jobs::JobPriority priority)
{
	ResPath rd(Path(fp.path.parent_path()));
	return loadModelAsync(fp, rd, owner, jobs, priority);
}

Vector<MeshHandle> RenderSubsystem::getModelMeshes(ModelHandle handle) const {
	auto* data = m_backend->models.get(handle.index, handle.generation);
	if (!data) return {};
	return data->result.meshes;
}

void RenderSubsystem::setRenderTarget(TextureRTV rtv, TextureDSV dsv) { m_backend->overrideRTV = (D::ITextureView*)rtv; m_backend->overrideDSV = (D::ITextureView*)dsv; }
void RenderSubsystem::beginFrame() { if (m_backend->ok) m_backend->begin(); }

void RenderSubsystem::drawMesh(MeshHandle m, const Transform& t) { if (m_backend->ok) m_backend->draw(m, t); }

void RenderSubsystem::drawMeshInstanced(MeshHandle m, const Vector<Mat4>& wm) { if (m_backend->ok) m_backend->drawInstanced(m, wm); }
void RenderSubsystem::drawMeshInstancedIndirect(MeshHandle m, ComputeSRV ws, ComputeSRV is, ComputeBuf ir, UInt32 off) { if (m_backend->ok) m_backend->drawInstancedIndirect(m, ws, is, ir, off); }
SubMesh RenderSubsystem::getSubMesh(MeshHandle mh, UInt32 subIdx) const {
	auto* md = m_backend->meshes.get(mh.index, mh.generation);
	if (!md || subIdx >= md->sub.size()) return {};
	return md->sub[subIdx];
}

void RenderSubsystem::drawBillboards(const Vector<BillboardDesc>& bbs) { if (m_backend->ok) m_backend->drawBillboards(bbs); }
void RenderSubsystem::renderShadowPass(ShadowSubsystem& sh, MeshHandle mh, const Mat4& wm) { if (m_backend->ok) m_backend->drawShadowPass(sh, mh, wm); }
void RenderSubsystem::renderShadowPassIndirect(ShadowSubsystem& sh, MeshHandle mh, ComputeSRV ws, ComputeSRV is, ComputeBuf ir, UInt32 off) { if (m_backend->ok) m_backend->drawShadowIndirect(sh, mh, ws, is, ir, off); }

void RenderSubsystem::clearShadowCascades(ShadowSubsystem& sh) {
	auto& b = *m_backend;
	if (!b.ok || !sh.config().enabled) return;
	for (UInt32 c = 0; c < sh.config().numCascades; c++) {
		auto* dsv = static_cast<D::ITextureView*>(sh.getCascadeDSV(c));
		if (dsv) {
			b.ctx->SetRenderTargets(0, nullptr, dsv, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
			b.ctx->ClearDepthStencil(dsv, D::CLEAR_DEPTH_FLAG, 1.0f, 0, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		}
	}
}

void RenderSubsystem::getCameraMatrices(Mat4& view, Mat4& proj) const { view = m_backend->cam.view; proj = m_backend->cam.proj; }
void RenderSubsystem::setSkybox(const SkyboxDesc& d) { m_backend->skyDesc = d; m_backend->envDirty = true; }
void RenderSubsystem::clearSkybox() { m_backend->skyDesc.reset(); m_backend->envDirty = true; }
void RenderSubsystem::setNormalMapDebug(bool enable) { m_backend->normalMapDebug = enable; }
bool RenderSubsystem::normalMapDebug() const { return m_backend->normalMapDebug; }
void RenderSubsystem::setNormalMapSign(Vec2 sign) { m_backend->nmSign = Vec4(sign.x, sign.y, 1.0f, 0.0f); }
Vec2 RenderSubsystem::normalMapSign() const { return Vec2(m_backend->nmSign.x, m_backend->nmSign.y); }
void RenderSubsystem::setSkyIBLEnabled(bool enable) { m_backend->envEnabled = enable; }
bool RenderSubsystem::skyIBLEnabled() const { return m_backend->envEnabled; }
void RenderSubsystem::setSkyIBLMipScale(F32 scale) { m_backend->envMipScale = std::clamp(scale, 0.0f, 4.0f); }
F32 RenderSubsystem::skyIBLMipScale() const { return m_backend->envMipScale; }
void RenderSubsystem::setSkyIBLDebug(bool enable) { m_backend->envDebug = enable; }
bool RenderSubsystem::skyIBLDebug() const { return m_backend->envDebug; }
void RenderSubsystem::setSkyEnvFlip(bool flipU, bool flipV, bool mirrorY) {
	auto& b = *m_backend;
	if (b.envFlipU == flipU && b.envFlipV == flipV && b.envMirrorY == mirrorY) return;
	b.envFlipU = flipU; b.envFlipV = flipV; b.envMirrorY = mirrorY;
	// The mirrors are baked into the faces as they are drawn, so the cube has to be
	// rebuilt. That is six small draws plus a mip chain - cheap enough to do while a
	// checkbox is being clicked.
	b.envDirty = true;
}
bool RenderSubsystem::skyEnvFlipU() const { return m_backend->envFlipU; }
bool RenderSubsystem::skyEnvFlipV() const { return m_backend->envFlipV; }
bool RenderSubsystem::skyEnvMirrorY() const { return m_backend->envMirrorY; }
Vec4 RenderSubsystem::skyEnvParams() const {
	return Vec4(m_backend->envEnabled ? 1.0f : 0.0f, m_backend->envMipScale,
	            m_backend->envDebug ? 1.0f : 0.0f, m_backend->envIntensity);
}
TextureSRV RenderSubsystem::getSkyEnvSRV() const { return m_backend->envCubeSRV.RawPtr(); }
void RenderSubsystem::prepareEnvironment() { if (m_backend->envDirty) m_backend->buildEnvCube(); }
bool RenderSubsystem::getSkyboxCorners(Vec4 outCorners[8]) const {
	if (!m_backend->skyDesc.has_value()) return false;
	for (int i = 0; i < 8; ++i) outCorners[i] = m_backend->skyDesc->corners[i];
	return true;
}

Result<TextureHandle, RenderError> RenderSubsystem::createCubemapTexture(const CubemapFace faces[6]) {
	auto& b = *m_backend;

	std::vector<Utilities::DecodedImage> imgs(6);
	UInt32 w = 0, h = 0;
	for (int i = 0; i < 6; i++) {
		imgs[i] = faces[i].image;
		if (!imgs[i].isValid()) { EError("Image {} is invalid", i); return RenderError::InvalidArgument; }
		if (w == 0) { w = imgs[i].width; h = imgs[i].height; }
		else if (imgs[i].width != w || imgs[i].height != h) {
			EError("Skybox face {} size mismatch: {}x{} != {}x{}", i, imgs[i].width, imgs[i].height, w, h);
			return RenderError::InvalidArgument;
		}
		// Apply flipping
		if (faces[i].flipVertical || faces[i].flipHorizontal) {
			Vector<UInt8> flipped(imgs[i].pixels.size());
			for (UInt32 y = 0; y < h; y++) {
				UInt32 sy = faces[i].flipVertical ? (h - 1 - y) : y;
				for (UInt32 x = 0; x < w; x++) {
					UInt32 sx = faces[i].flipHorizontal ? (w - 1 - x) : x;
					memcpy(&flipped[(y * w + x) * 4], &imgs[i].pixels[(sy * w + sx) * 4], 4);
				}
			}
			imgs[i].pixels = std::move(flipped);
		}
	}

	// Full mip chain per face. The ray tracer samples the cube at the level that
	// matches the reflection lobe (a wide lobe must not read a sharp sky texel),
	// which is the difference between a stable and a sparkling rough reflection.
	Vector<Utilities::MipChain> chains(6);
	UInt32 levels = 1;
	for (int i = 0; i < 6; i++) {
		chains[i] = Utilities::buildMipChain(std::move(imgs[i]), true);
		if (!chains[i].isValid()) { EError("Skybox face {}: mip chain failed", i); return RenderError::TextureCreationFailed; }
		if (i == 0) levels = (UInt32)chains[i].levels.size();
		else if ((UInt32)chains[i].levels.size() != levels) { EError("Skybox face {}: mip count mismatch", i); return RenderError::TextureCreationFailed; }
	}

	D::TextureDesc td; td.Name = "SkyCube"; td.Type = D::RESOURCE_DIM_TEX_CUBE;
	td.Width = w; td.Height = h; td.ArraySize = 6; td.MipLevels = (D::Uint32)levels;
	td.Format = D::TEX_FORMAT_RGBA8_UNORM_SRGB;
	td.BindFlags = D::BIND_SHADER_RESOURCE; td.Usage = D::USAGE_IMMUTABLE;

	// Cubemap face order (D3D12): 0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z. Subresources must
	// be in D3D12 order, i.e. slice-major: (face0, mip0..N), (face1, mip0..N), ...
	// (Diligent asserts on the count and feeds the array straight to
	// UpdateSubresources, whose index is MipSlice + ArraySlice * MipLevels.)
	Vector<D::TextureSubResData> subRes;
	subRes.reserve((Size)levels * 6);
	for (int i = 0; i < 6; i++) {
		for (UInt32 mip = 0; mip < levels; mip++) {
			D::TextureSubResData srd; srd.pData = chains[i].levels[mip].data();
			srd.Stride = (D::Uint32)(std::max(1u, w >> mip) * 4);
			subRes.push_back(srd);
		}
	}
	D::TextureData tdata; tdata.pSubResources = subRes.data(); tdata.NumSubresources = (D::Uint32)subRes.size();
	D::RefCntAutoPtr<D::ITexture> tex;
	b.device->CreateTexture(td, &tdata, &tex);
	if (!tex) { EError("Cubemap creation failed"); return RenderError::TextureCreationFailed; }
	auto a = b.textures.allocate();
	auto* dd = b.textures.getUnchecked(a.index);
	dd->tex = std::move(tex); dd->srv = dd->tex->GetDefaultView(D::TEXTURE_VIEW_SHADER_RESOURCE);
	EInfo("Skybox cubemap created: {}x{} ({} mips)", w, h, levels);
	return TextureHandle{ a.index, a.generation };
}

const Vector<Vertex>* RenderSubsystem::getMeshVertices(MeshHandle m) const {
	if (!m_backend->ok) return nullptr;
	auto* md = m_backend->meshes.get(m.index, m.generation);
	return md ? &md->cpuVertices : nullptr;
}
const Vector<UInt32>* RenderSubsystem::getMeshIndices(MeshHandle m) const {
	if (!m_backend->ok) return nullptr;
	auto* md = m_backend->meshes.get(m.index, m.generation);
	return md ? &md->cpuIndices : nullptr;
}

void RenderSubsystem::getMeshGeometry(MeshHandle mesh, void*& vertexBuffer, void*& indexBuffer, UInt32& vertexCount, UInt32& indexCount) const {
	vertexBuffer = nullptr; indexBuffer = nullptr; vertexCount = 0; indexCount = 0;
	auto* md = m_backend->meshes.get(mesh.index, mesh.generation);
	if (!md) return;
	vertexBuffer = md->vb.RawPtr();
	indexBuffer  = md->ib.RawPtr();
	vertexCount  = md->vc;
	indexCount   = md->ic;
}

Optional<MaterialDesc> RenderSubsystem::getMaterial(MaterialHandle material) const {
	auto* md = m_backend->materials.get(material.index, material.generation);
	if (!md) return NullOpt;
	return md->desc;
}

TextureSRV RenderSubsystem::getTextureSRV(TextureHandle texture) const {
	auto* td = m_backend->textures.get(texture.index, texture.generation);
	return td ? td->srv.RawPtr() : nullptr;
}

void RenderSubsystem::destroyTexture(TextureHandle texture) {
	m_backend->textures.release(texture.index);
}

TextureRTV RenderSubsystem::getTextureRTV(TextureHandle texture) const {
	auto* td = m_backend->textures.get(texture.index, texture.generation);
	return td ? td->rtv.RawPtr() : nullptr;
}

TextureUAV RenderSubsystem::getTextureUAV(TextureHandle texture) const {
	auto* td = m_backend->textures.get(texture.index, texture.generation);
	return td ? td->uav.RawPtr() : nullptr;
}

TextureDSV RenderSubsystem::getTextureDSV(TextureHandle texture) const {
	auto* td = m_backend->textures.get(texture.index, texture.generation);
	return td ? td->dsv.RawPtr() : nullptr;
}

Result<void, RenderError> RenderSubsystem::resolveGBufferMSAA(void* colorSRV, void* normalSRV, void* emissiveSRV, void* depthSRV,
	void* colorUAV, void* normalUAV, void* emissiveUAV, void* depthUAV, UInt32 width, UInt32 height) {
	auto& b = *m_backend;
	if (!b.ok) return RenderError::NotInitialized;
	if (!colorSRV || !normalSRV || !emissiveSRV || !depthSRV || !colorUAV || !normalUAV || !emissiveUAV || !depthUAV) return RenderError::InvalidArgument;
	if (b.msaaSamples <= 1) return {}; // single-sample G-buffer needs no resolve

	// Lazy-create the resolve compute pipeline (DXC). Attempt once even on
	// failure so a broken shader does not get recompiled every frame.
	if (!b.resolvePSO) {
		if (b.resolvePSOAttempted) return RenderError::ShaderCompilationFailed;
		b.resolvePSOAttempted = true;
		D::ShaderCreateInfo ci;
		ci.Desc.ShaderType = D::SHADER_TYPE_COMPUTE;
		ci.Desc.Name = "GBuffer Resolve CS";
		ci.Desc.UseCombinedTextureSamplers = false;
		ci.EntryPoint = "CSMain";
		ci.SourceLanguage = D::SHADER_SOURCE_LANGUAGE_HLSL;
		ci.ShaderCompiler = D::SHADER_COMPILER_DXC;
		ci.HLSLVersion = { 6, 5 };
		ci.Source = g_ResolveCS;
		ci.SourceLength = (D::Uint32)strlen(g_ResolveCS);
		D::RefCntAutoPtr<D::IShader> cs;
		b.device->CreateShader(ci, &cs);
		if (!cs || cs->GetStatus() != D::SHADER_STATUS_READY) {
			EError("RenderSubsystem: failed to compile G-buffer resolve compute shader");
			return RenderError::ShaderCompilationFailed;
		}
		D::ComputePipelineStateCreateInfo cci;
		cci.PSODesc.Name = "GBuffer Resolve PSO";
		cci.PSODesc.PipelineType = D::PIPELINE_TYPE_COMPUTE;
		cci.PSODesc.ResourceLayout.DefaultVariableType = D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
		cci.pCS = cs;
		b.device->CreateComputePipelineState(cci, &b.resolvePSO);
		if (!b.resolvePSO) {
			EError("RenderSubsystem: failed to create G-buffer resolve PSO");
			return RenderError::PipelineStateCreationFailed;
		}
		b.resolvePSO->CreateShaderResourceBinding(&b.resolveSRB, true);
		EInfo("RenderSubsystem: G-buffer MSAA resolve ready ({}x -> 1x).", (int)b.msaaSamples);
	}

	if (auto* v = b.resolveSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_MSAAColor")) v->Set(static_cast<D::ITextureView*>(colorSRV), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = b.resolveSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_MSAANormal")) v->Set(static_cast<D::ITextureView*>(normalSRV), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = b.resolveSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_MSAAEmissive")) v->Set(static_cast<D::ITextureView*>(emissiveSRV), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = b.resolveSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_MSAADepth")) v->Set(static_cast<D::ITextureView*>(depthSRV), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = b.resolveSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_OutColor")) v->Set(static_cast<D::ITextureView*>(colorUAV), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = b.resolveSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_OutNormal")) v->Set(static_cast<D::ITextureView*>(normalUAV), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = b.resolveSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_OutEmissive")) v->Set(static_cast<D::ITextureView*>(emissiveUAV), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = b.resolveSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_OutDepth")) v->Set(static_cast<D::ITextureView*>(depthUAV), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);

	D::DispatchComputeAttribs da;
	da.ThreadGroupCountX = (width + 7) / 8;
	da.ThreadGroupCountY = (height + 7) / 8;
	b.ctx->SetPipelineState(b.resolvePSO);
	b.ctx->CommitShaderResources(b.resolveSRB, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	b.ctx->DispatchCompute(da);
	return {};
}

TextureDSV RenderSubsystem::getDepthStencil() const { return m_backend->dsv.RawPtr(); }

void RenderSubsystem::beginGBuffer(TextureRTV colorRT, TextureRTV normalRT, TextureRTV emissiveRT, TextureDSV depthDSV) {
	if (!m_backend->ok || !colorRT || !normalRT || !emissiveRT || !depthDSV) return;
	auto& b = *m_backend;
	b.gBufferActive = true;
	auto* cRTV = static_cast<D::ITextureView*>(colorRT);
	auto* nRTV = static_cast<D::ITextureView*>(normalRT);
	auto* eRTV = static_cast<D::ITextureView*>(emissiveRT);
	auto* dDSV = static_cast<D::ITextureView*>(depthDSV);
	D::ITextureView* rtvs[3] = { cRTV, nRTV, eRTV };
	b.ctx->SetRenderTargets(3, rtvs, dDSV, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	const float cc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	b.ctx->ClearRenderTarget(cRTV, cc, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	b.ctx->ClearRenderTarget(nRTV, cc, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	b.ctx->ClearRenderTarget(eRTV, cc, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	b.ctx->ClearDepthStencil(dDSV, D::CLEAR_DEPTH_FLAG, 1.0f, 0, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	b.dc = 0;
}

void RenderSubsystem::endGBuffer() {
	if (m_backend->ok) m_backend->gBufferActive = false;
}

bool RenderSubsystem::isGBufferActive() const { return m_backend->gBufferActive; }

void RenderSubsystem::endFrame() { if (m_backend->ok) m_backend->end(); }

void RenderSubsystem::resize(UInt32 ww, UInt32 wh) {
	if (!m_backend->ok || ww == 0 || wh == 0) return;
	m_backend->w = ww; m_backend->h = wh;
	m_backend->sc->Resize(ww, wh);

	// Recreate depth buffer to match new size
	{
		D::TextureDesc td; td.Name = "Depth"; td.Type = D::RESOURCE_DIM_TEX_2D;
		td.Width = ww; td.Height = wh; td.Format = D::TEX_FORMAT_D32_FLOAT;
		td.BindFlags = D::BIND_DEPTH_STENCIL; td.Usage = D::USAGE_DEFAULT;
		td.SampleCount = m_backend->msaaSamples;
		D::RefCntAutoPtr<D::ITexture> dt;
		m_backend->device->CreateTexture(td, nullptr, &dt);
		m_backend->dsv.Release();
		if (dt) m_backend->dsv = dt->GetDefaultView(D::TEXTURE_VIEW_DEPTH_STENCIL);
	}

	if (m_backend->camHandle.isValid()) { auto* c = m_backend->cameras.get(m_backend->camHandle.index, m_backend->camHandle.generation); if (c) { c->aspect = (F32)ww / (F32)wh; c->desc.w = (F32)ww; c->desc.h = (F32)wh; c->proj = glm::perspective(glm::radians(c->desc.fov), c->aspect, c->desc.nearP, c->desc.farP); m_backend->cam = *c; } }
	SwapChainResizeEvent e; e.w = ww; e.h = wh; emit(e);
}

void RenderSubsystem::getViewportSize(UInt32& ww, UInt32& wh) const { ww = m_backend->w; wh = m_backend->h; }

UInt32 RenderSubsystem::lastFrameDrawCalls() const { return m_backend->dc; }

UInt64 RenderSubsystem::frameNumber() const { return m_backend->fn; }

DevicePtr RenderSubsystem::getDevice() const { return m_backend->device.RawPtr(); }
ContextPtr RenderSubsystem::getContext() const { return m_backend->ctx.RawPtr(); }
SwapChainPtr RenderSubsystem::getSwapChain() const { return m_backend->sc.RawPtr(); }

void RenderSubsystem::setWireframe(bool enable) { m_backend->wireframe = enable; }

bool RenderSubsystem::isWireframe() const { return m_backend->wireframe; }

Result<void, CoreError> RenderSubsystem::onInitialize() {
	D::SetDebugMessageCallback(DiligentDebugMsgCallback);
	auto r = m_backend->init(); if (r.isErr()) { EError("Render init: {}", ToString(r.error())); return CoreError::OperationFailed; }
	auto dr = m_backend->createDefaults(); if (dr.isErr()) { EError("Render defaults: {}", ToString(dr.error())); return CoreError::OperationFailed; }
	m_backend->ok = true;
	const char* bn = "?"; switch (m_backend->backendType) { case RenderBackendType::D3D12: bn = "D3D12"; break; case RenderBackendType::D3D11: bn = "D3D11"; break; case RenderBackendType::Vulkan: bn = "Vulkan"; break; default: bn = "Auto"; break; }
	EInfo("Rendering ready ({}x{}, {})", m_backend->w, m_backend->h, bn);
	if (m_backend->rtCaps != RayTracingCaps::None) {
		EInfo("Ray tracing available on {}: inline={} standalone={} maxRecursion={} maxInstancesPerTLAS={}",
			bn,
			hasRayTracingCap(m_backend->rtCaps, RayTracingCaps::InlineRayTracing),
			hasRayTracingCap(m_backend->rtCaps, RayTracingCaps::StandaloneShaders),
			m_backend->rtMaxRecursionDepth,
			m_backend->rtMaxInstancesPerTLAS);
	}
	else {
		EInfo("Ray tracing is NOT available on this device/driver{}; rasterization only.",
			m_backend->requestRayTracing ? " (feature was requested)" : " (feature was disabled by application)");
	}
	return {};
}
void RenderSubsystem::onShutdown() { m_backend->ok = false;
	m_backend->device.Release(); m_backend->ctx.Release();
	m_backend->sc.Release(); m_backend->factory.Release();
	D::SetDebugMessageCallback(nullptr);
	EInfo("Rendering shut down"); }

void RenderSubsystem::onUpdate(F64) {}

bool RenderSubsystem::onRecover() { EInfo("Recovering renderer..."); onShutdown(); return onInitialize().isOk(); }

EE_NAMESPACE_RENDERING_END
