#include "VolumetricBackend.hpp"
#include <Rendering/RenderSelfCheck.hpp>
#include <Rendering/RenderSubsystem.hpp>
#include <Core/Log.hpp>

#include <DiligentCore/Graphics/GraphicsEngine/interface/RenderDevice.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/DeviceContext.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Shader.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/PipelineState.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Buffer.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Texture.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Sampler.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Query.h>
#include <DiligentCore/Common/interface/RefCntAutoPtr.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace D = Diligent;

EE_NAMESPACE_RENDERING_BEGIN

namespace {

// ===================================================================
// Froxel backend.
//
// The view frustum is divided into a 3D grid - one cell per 8x8 screen pixels, `slices` cells
// deep with an exponential distance distribution - and the medium is evaluated once per cell
// (density, sky irradiance, sun visibility). A second pass walks each pixel's column
// front-to-back and integrates the cells it can see, clipped to the scene depth.
//
// Compared with the view-ray march this trades a 3D texture and an injection pass for a much
// cheaper integrate step: the shadow map is sampled once per *cell* instead of once per pixel
// per step, and a cell can carry any number of light contributions without making the
// per-pixel loop more expensive - which is what makes local lights affordable (V2's next
// step). The cost is resolution: the fog is quantised to the grid, so shafts are softer.
// ===================================================================

/// @brief Depth declaration + loader, prepended to both bodies (see the screen-space file).
static const char* g_DepthPrologueSingle = R"(
Texture2D<float>              g_Depth      : register(t0);
float LoadSceneDepth(int2 p) { return g_Depth.Load(int3(p, 0)).x; }
void GetSceneDepthSize(out uint2 dim) { g_Depth.GetDimensions(dim.x, dim.y); }
)";

static const char* g_DepthPrologueMsaa = R"(
Texture2DMS<float>             g_Depth      : register(t0);
float LoadSceneDepth(int2 p) { return g_Depth.Load(p, 0).x; }
void GetSceneDepthSize(out uint2 dim) { uint samples; g_Depth.GetDimensions(dim.x, dim.y, samples); }
)";

/// @brief Constants and the medium helpers shared by both passes.
static const char* g_FroxelCommon = R"(
Texture2DArray                g_ShadowMap  : register(t1);
Texture3D<float>              g_Noise      : register(t2);
SamplerState                  g_NoiseSampler : register(s2);
SamplerComparisonState        g_ShadowSampler : register(s1);

cbuffer VolCB : register(b0)
{
    float4x4 g_ViewProj;
    float4x4 g_ViewProjInv;
    float4   g_CameraPos;     // xyz, w = maxDistance
    float4   g_SunDir;        // xyz = direction the light travels, w = sunScattering
    float4   g_SunColor;      // rgb * intensity, w = ambientScattering
    float4x4 g_ShadowMapUVDepth[4];
    float4   g_CascadeSplits;
    float4   g_FogParams;     // x = density, y = baseHeight, z = heightScale, w = anisotropy
    float4   g_Quality;       // x = slices, y = sunShafts, z = time, w = ambient strength
    float4   g_SkyCorners[8];
    float4   g_FroxelParams;  // x = slices, y = near distance, z = tile size, w = max distance
    float4   g_LightCount;    // x = number of local lights
    // Same packed layout the shading shaders use: CI = (colour, intensity), DT = (dir, type),
    // PR = (pos, range), CA = (inner cone, outer cone).
    float4   g_Lights[4 * 8];
    float4   g_LocalShadow;   // x = enabled, y = steps
    float4   g_NoiseParams;   // x = strength, y = scale, z = time
};


#define PI 3.14159265358979

// Medium detail: a tileable 3D noise field modulating the density, so shafts and cones get a
// natural texture instead of being perfectly smooth. The field scrolls along +Y over time; at
// strength 0 the factor is exactly 1 and the medium is uniform.
float MediumNoise(float3 p)
{
    if (g_NoiseParams.x <= 0.0) return 1.0;
    const float3 uv = p * max(g_NoiseParams.y, 1e-4) + float3(0.0, g_NoiseParams.z, 0.0);
    const float n = g_Noise.SampleLevel(g_NoiseSampler, uv, 0).r;
    return lerp(1.0, saturate(n * 1.6), saturate(g_NoiseParams.x));
}

float HGPhase(float cosTheta, float g)
{
    const float g2 = g * g;
    const float denom = max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4);
    return (1.0 - g2) / (4.0 * PI * denom * sqrt(denom));
}

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

float SunVisibility(float3 wp, float camZ)
{
    uint c = 0;
    if (camZ > g_CascadeSplits.x) c = 1;
    if (camZ > g_CascadeSplits.y) c = 2;
    if (camZ > g_CascadeSplits.z) c = 3;
    float4 sc = mul(g_ShadowMapUVDepth[c], float4(wp, 1.0));
    sc.xyz /= max(sc.w, 1e-6);
    if (sc.x < 0.0 || sc.x > 1.0 || sc.y < 0.0 || sc.y > 1.0 || sc.z < 0.0 || sc.z > 1.0)
        return 1.0;
    return g_ShadowMap.SampleCmpLevelZero(g_ShadowSampler, float3(sc.xy, (float)c), sc.z - 0.002).r;
}

// Slice boundaries are exponential, so the near field - where the fog changes fastest and
// detail matters most - gets as much depth resolution as the far field.
float SliceDistance(float sliceIndex, float nearDist, float farDist, float slices)
{
    const float k = max(farDist / max(nearDist, 1e-3), 1.0001);
    return nearDist * pow(k, saturate(sliceIndex / max(slices, 1.0)));
}

#define MAX_LIGHTS 8

// Local lights add in-scattering along the cell's view ray. No shadowing: the volumetric pass
// has no per-light shadow data, so a point light lights the fog around it but does not cast
// shafts through geometry (that needs the light's own shadow map; the sun, which has one,
// goes through SunVisibility instead).
// Screen-space occlusion along the ray towards the light: samples are projected into the
// current scene depth and count as blocked when they land behind it. Cheap (one depth fetch
// per sample, no per-light shadow map) and it works in both render modes, but only occluders
// inside the view take part - a wall behind the camera does not block the light.
float LightScreenVisibility(float3 p, float3 lightPos)
{
    if (g_LocalShadow.x < 0.5) return 1.0;
    const float3 d = lightPos - p;
    const float len = length(d);
    if (len < 1e-4) return 1.0;
    const float3 dir = d / len;
    const int steps = clamp((int)g_LocalShadow.y, 1, 8);
    uint2 dim;
    GetSceneDepthSize(dim);
    int blocked = 0, total = 0;
    [loop]
    for (int i = 1; i <= steps; ++i)
    {
        // Stop short of the light itself: the light usually sits inside geometry or in air.
        const float t = (float)i / (float)steps * len * 0.9;
        const float4 clip = mul(g_ViewProj, float4(p + dir * t, 1.0));
        if (clip.w <= 0.0) continue;
        const float2 uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
        if (any(uv < 0.0) || any(uv > 1.0)) continue;
        const int2 dp = min(int2(uv * float2(dim)), int2(dim) - 1);
        const float sceneDepth = LoadSceneDepth(dp);
        if (sceneDepth >= 1.0) continue;
        ++total;
        if (sceneDepth < clip.z / clip.w - 0.002) ++blocked;
    }
    return total > 0 ? saturate(1.0 - (float)blocked / (float)total) : 1.0;
}

float3 LocalLightScattering(float3 p, float3 rayDir, float g)
{
    float3 sum = float3(0, 0, 0);
    const uint count = min((uint)max(g_LightCount.x, 0.0), (uint)MAX_LIGHTS);
    for (uint i = 0; i < count; ++i)
    {
        const float4 CI = g_Lights[i * 4 + 0];
        const float4 DT = g_Lights[i * 4 + 1];
        const float4 PR = g_Lights[i * 4 + 2];
        const float4 CA = g_Lights[i * 4 + 3];
        const float type = DT.a;
        if (type < 0.5) continue; // directional: the sun, handled separately (it has shadows)
        const float3 toL = PR.xyz - p;
        const float d = length(toL);
        const float3 Ldir = toL / max(d, 1e-4);
        // The same falloff the forward shading uses, so the halo matches the lit geometry.
        float att = 1.0 / (1.0 + 0.09 * d + 0.032 * d * d);
        if (type > 1.5)
        {
            const float ca = dot(normalize(-DT.xyz), -Ldir);
            att *= saturate((ca - CA.y) / max(CA.x - CA.y, 1e-3));
        }
        att *= LightScreenVisibility(p, PR.xyz);
        sum += CI.rgb * (CI.a * att) * HGPhase(dot(rayDir, Ldir), g);
    }
    return sum;
}
)";

/// @brief Injection: one thread per froxel cell.
static const char* g_FroxelInjectBody = R"(
RWTexture3D<float4>           g_Volume     : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint3 dim;
    g_Volume.GetDimensions(dim.x, dim.y, dim.z);
    if (DTid.x >= dim.x || DTid.y >= dim.y || DTid.z >= dim.z) return;

    const float slices = max(g_FroxelParams.x, 1.0);
    const float nearDist = max(g_FroxelParams.y, 1e-3);
    const float maxDist = g_FroxelParams.w;
    const float z0 = SliceDistance((float)DTid.z, nearDist, maxDist, slices);
    const float z1 = SliceDistance((float)DTid.z + 1.0, nearDist, maxDist, slices);
    const float zMid = 0.5 * (z0 + z1);

    // The cell's view direction: the centre of its 8x8 pixel tile.
    const float2 uv = (float2(DTid.xy * 8u) + 4.0) / float2(dim.xy * 8u);
    float4 clip = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    float4 worldH = mul(g_ViewProjInv, clip);
    const float3 camPos = g_CameraPos.xyz;
    const float3 rayDir = normalize(worldH.xyz / max(worldH.w, 1e-6) - camPos);
    const float3 p = camPos + rayDir * zMid;

    const float density = g_FogParams.x;
    const float baseHeight = g_FogParams.y;
    const float heightScale = max(g_FogParams.z, 1e-3);
    const float g = clamp(g_FogParams.w, -0.95, 0.95);
    const float phase = HGPhase(dot(rayDir, normalize(-g_SunDir.xyz)), g);
    const float dens = density * exp(-(p.y - baseHeight) / heightScale) * MediumNoise(p);

    // Ambient plus sun, with the shadow visibility evaluated once for the whole cell.
    float3 inLight = SkyIrradiance(float3(0.0, 1.0, 0.0)) * (g_Quality.w * g_SunColor.w);
    float sunVis = 1.0;
    if (g_Quality.y > 0.5)
    {
        const float camZ = abs(mul(g_ViewProj, float4(p, 1.0)).w);
        sunVis = SunVisibility(p, camZ);
    }
    inLight += g_SunColor.rgb * (g_SunDir.w * phase * sunVis);
    inLight += LocalLightScattering(p, rayDir, g);

    // rgb = in-scattered radiance per unit length, a = extinction.
    g_Volume[DTid] = float4(inLight * dens, dens);
}
)";

/// @brief Integration: one thread per pixel, walking its froxel column.
static const char* g_FroxelIntegrateBody = R"(
Texture3D<float4>             g_Volume     : register(t2);
RWTexture2D<float4>           g_Out        : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
    uint2 dim;
    g_Out.GetDimensions(dim.x, dim.y);
    if (DTid.x >= dim.x || DTid.y >= dim.y) return;
    uint3 vdim;
    g_Volume.GetDimensions(vdim.x, vdim.y, vdim.z);
    uint2 depthDim;
    GetSceneDepthSize(depthDim);

    const float2 uv = (float2(DTid) + 0.5) / float2(dim);
    const int2 dp = min(int2(uv * float2(depthDim)), int2(depthDim) - 1);
    const float depth = LoadSceneDepth(dp);

    // Which froxel column this pixel falls into (the inject pass built one cell per 8x8 tile).
    const int2 tile = min(int2(DTid / 8u), int2(vdim.xy) - 1);

    // Clip the integration to the scene depth: everything behind it is invisible.
    const float farDist = g_FroxelParams.w;
    float maxDist = farDist;
    if (depth < 1.0)
    {
        const float4 clip = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), depth, 1.0);
        float4 worldH = mul(g_ViewProjInv, clip);
        const float3 worldPos = worldH.xyz / max(worldH.w, 1e-6);
        maxDist = min(maxDist, length(worldPos - g_CameraPos.xyz));
    }

    const float slices = max(g_FroxelParams.x, 1.0);
    const float nearDist = max(g_FroxelParams.y, 1e-3);

    float3 scattering = float3(0, 0, 0);
    float transmittance = 1.0;
    for (uint z = 0u; z < (uint)vdim.z; ++z)
    {
        const float z0 = SliceDistance((float)z, nearDist, farDist, slices);
        if (z0 >= maxDist) break;
        const float z1 = SliceDistance((float)z + 1.0, nearDist, farDist, slices);
        const float len = max(min(z1, maxDist) - z0, 0.0);
        const float4 cell = g_Volume.Load(int4(tile, (int)z, 0));
        const float stepT = exp(-cell.a * len);
        scattering += transmittance * cell.rgb * len;
        transmittance *= stepT;
        if (transmittance < 0.002) break;
    }

    g_Out[DTid] = float4(scattering, saturate(transmittance));
}
)";

struct VolCBData {
	Mat4 viewProj;
	Mat4 viewProjInv;
	Vec4 cameraPos;      // w = maxDistance
	Vec4 sunDir;         // w = sunScattering
	Vec4 sunColor;       // rgb already scaled, w = ambientScattering
	Mat4 shadowUVDepth[4];
	Vec4 cascadeSplits;
	Vec4 fogParams;
	Vec4 quality;        // x = slices, y = sunShafts, z = time, w = ambient strength
	Vec4 skyCorners[8];
	Vec4 froxelParams;   // x = slices, y = near, z = tile size, w = maxDistance
	Vec4 lightCount;     // x = number of local lights
	Vec4 lights[32];     // 8 lights x 4 vec4, the same packing the shading shaders use
	Vec4 localShadow;    // x = enabled, y = steps
	Vec4 noiseParams;    // x = strength, y = scale, z = time
};

constexpr UInt32 kTileSize = 8;
constexpr F32 kNearDistance = 1.0f;

/// @brief The froxel-volume backend.
class FroxelBackend final : public Detail::IVolumetricBackend {
public:
	bool initialize(RenderSubsystem* renderer) override;
	void shutdown() override;
	Result<void, CoreError> render(const VolumetricInput& input, const VolumetricConfig& cfg) override;
	void* scatteringSRV() const override { return m_outSRV.RawPtr(); }
	F32 lastRenderMs() const override { return m_lastMs; }
	const char* name() const override { return "froxel"; }

private:
	RenderSubsystem* m_renderer = nullptr;
	/// Inject and integrate, one per depth flavour.
	D::RefCntAutoPtr<D::IPipelineState> m_injectPso[2], m_integratePso[2];
	D::RefCntAutoPtr<D::IShaderResourceBinding> m_injectSrb[2], m_integrateSrb[2];
	D::RefCntAutoPtr<D::IShader> m_injectShader[2], m_integrateShader[2];
	D::RefCntAutoPtr<D::IBuffer> m_volCB;
	D::RefCntAutoPtr<D::ISampler> m_cmpSampler;
	/// Wrapping sampler for the noise volume.
	D::RefCntAutoPtr<D::ISampler> m_noiseSampler;
	D::RefCntAutoPtr<D::ITexture> m_volumeTex;
	D::RefCntAutoPtr<D::ITextureView> m_volumeSRV, m_volumeUAV;
	UInt32 m_volumeX = 0, m_volumeY = 0, m_volumeZ = 0;
	D::RefCntAutoPtr<D::ITexture> m_outTex;
	D::RefCntAutoPtr<D::ITextureView> m_outSRV, m_outUAV;
	UInt32 m_outW = 0, m_outH = 0;
	D::RefCntAutoPtr<D::IQuery> m_query;
	bool m_queryEnded = false;
	F32 m_lastMs = 0.0f;
	bool m_selfChecked = false;
	bool m_loggedFirst = false;

	D::IRenderDevice* device() const { return m_renderer ? static_cast<D::IRenderDevice*>(m_renderer->getDevice()) : nullptr; }
	D::IDeviceContext* ctx() const { return m_renderer ? static_cast<D::IDeviceContext*>(m_renderer->getContext()) : nullptr; }

	bool createPipelines();
	bool ensureTargets(const VolumetricInput& input, const VolumetricConfig& cfg);
};

bool FroxelBackend::createPipelines() {
	auto* dev = device();
	if (!dev) return false;

	D::SamplerDesc sd;
	sd.MinFilter = D::FILTER_TYPE_COMPARISON_LINEAR;
	sd.MagFilter = D::FILTER_TYPE_COMPARISON_LINEAR;
	sd.MipFilter = D::FILTER_TYPE_COMPARISON_LINEAR;
	sd.ComparisonFunc = D::COMPARISON_FUNC_LESS;
	sd.AddressU = sd.AddressV = sd.AddressW = D::TEXTURE_ADDRESS_CLAMP;
	dev->CreateSampler(sd, &m_cmpSampler);

	D::SamplerDesc nsd;
	nsd.MinFilter = D::FILTER_TYPE_LINEAR;
	nsd.MagFilter = D::FILTER_TYPE_LINEAR;
	nsd.MipFilter = D::FILTER_TYPE_LINEAR;
	nsd.AddressU = nsd.AddressV = nsd.AddressW = D::TEXTURE_ADDRESS_WRAP;
	dev->CreateSampler(nsd, &m_noiseSampler);

	const char* const prologues[2] = { g_DepthPrologueSingle, g_DepthPrologueMsaa };
	for (int variant = 0; variant < 2; ++variant) {
		struct Pass { const char* name; const char* body; int slot; };
		const Pass passes[2] = {
			{ "VolumetricFroxelInject", g_FroxelInjectBody, 0 },
			{ "VolumetricFroxelIntegrate", g_FroxelIntegrateBody, 1 },
		};
		for (const Pass& pass : passes) {
			String source = String(prologues[variant]) + g_FroxelCommon + pass.body;
			D::ShaderCreateInfo ci;
			ci.Desc.Name = pass.name;
			ci.Desc.ShaderType = D::SHADER_TYPE_COMPUTE;
			ci.EntryPoint = "main";
			ci.SourceLanguage = D::SHADER_SOURCE_LANGUAGE_HLSL;
			ci.Source = source.c_str();
			ci.SourceLength = static_cast<D::Uint32>(source.size());
			D::RefCntAutoPtr<D::IShader>& shader = pass.slot == 0 ? m_injectShader[variant] : m_integrateShader[variant];
			dev->CreateShader(ci, &shader);
			if (!shader || shader->GetStatus() != D::SHADER_STATUS_READY) {
				EError("Volumetric: {} shader failed (depth variant {})", pass.name, variant);
				return false;
			}

			D::ComputePipelineStateCreateInfo pci;
			pci.PSODesc.Name = pass.name;
			pci.PSODesc.PipelineType = D::PIPELINE_TYPE_COMPUTE;
			pci.PSODesc.ResourceLayout.DefaultVariableType = D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
			pci.pCS = shader;
			D::RefCntAutoPtr<D::IPipelineState>& pso = pass.slot == 0 ? m_injectPso[variant] : m_integratePso[variant];
			D::RefCntAutoPtr<D::IShaderResourceBinding>& srb = pass.slot == 0 ? m_injectSrb[variant] : m_integrateSrb[variant];
			dev->CreateComputePipelineState(pci, &pso);
			if (!pso) {
				EError("Volumetric: {} PSO failed", pass.name);
				return false;
			}
			pso->CreateShaderResourceBinding(&srb, true);
		}
	}

	D::BufferDesc bd;
	bd.Name = "VolCB";
	bd.Size = sizeof(VolCBData);
	bd.BindFlags = D::BIND_UNIFORM_BUFFER;
	bd.Usage = D::USAGE_DYNAMIC;
	bd.CPUAccessFlags = D::CPU_ACCESS_WRITE;
	dev->CreateBuffer(bd, nullptr, &m_volCB);

	for (int variant = 0; variant < 2; ++variant) {
		for (auto* srb : { m_injectSrb[variant].RawPtr(), m_integrateSrb[variant].RawPtr() }) {
			if (!srb) continue;
			if (auto* v = srb->GetVariableByName(D::SHADER_TYPE_COMPUTE, "VolCB")) v->Set(m_volCB);
			if (auto* v = srb->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_ShadowSampler")) v->Set(m_cmpSampler);
			if (auto* v = srb->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_NoiseSampler")) v->Set(m_noiseSampler);
		}
	}

	D::QueryDesc qd(D::QUERY_TYPE_DURATION);
	dev->CreateQuery(qd, &m_query);
	return true;
}

bool FroxelBackend::ensureTargets(const VolumetricInput& input, const VolumetricConfig& cfg) {
	auto* dev = device();
	if (!dev || input.width == 0 || input.height == 0) return false;

	const UInt32 slices = std::clamp(cfg.froxelSlices, 8u, 512u);
	const UInt32 vx = (input.width + kTileSize - 1) / kTileSize;
	const UInt32 vy = (input.height + kTileSize - 1) / kTileSize;

	if (!m_volumeTex || m_volumeX != vx || m_volumeY != vy || m_volumeZ != slices) {
		D::TextureDesc td;
		td.Name = "VolumetricFroxels";
		td.Type = D::RESOURCE_DIM_TEX_3D;
		td.Width = vx;
		td.Height = vy;
		td.Depth = slices;
		td.Format = D::TEX_FORMAT_RGBA16_FLOAT;
		td.BindFlags = D::BIND_SHADER_RESOURCE | D::BIND_UNORDERED_ACCESS;
		td.Usage = D::USAGE_DEFAULT;
		m_volumeTex.Release(); m_volumeSRV.Release(); m_volumeUAV.Release();
		dev->CreateTexture(td, nullptr, &m_volumeTex);
		if (!m_volumeTex) return false;
		m_volumeSRV = m_volumeTex->GetDefaultView(D::TEXTURE_VIEW_SHADER_RESOURCE);
		m_volumeUAV = m_volumeTex->GetDefaultView(D::TEXTURE_VIEW_UNORDERED_ACCESS);
		m_volumeX = vx; m_volumeY = vy; m_volumeZ = slices;
	}
	if (!m_outTex || m_outW != input.width || m_outH != input.height) {
		D::TextureDesc td;
		td.Name = "VolumetricScattering";
		td.Type = D::RESOURCE_DIM_TEX_2D;
		td.Width = input.width;
		td.Height = input.height;
		td.Format = D::TEX_FORMAT_RGBA16_FLOAT;
		td.BindFlags = D::BIND_SHADER_RESOURCE | D::BIND_UNORDERED_ACCESS;
		td.Usage = D::USAGE_DEFAULT;
		m_outTex.Release(); m_outSRV.Release(); m_outUAV.Release();
		dev->CreateTexture(td, nullptr, &m_outTex);
		if (!m_outTex) return false;
		m_outSRV = m_outTex->GetDefaultView(D::TEXTURE_VIEW_SHADER_RESOURCE);
		m_outUAV = m_outTex->GetDefaultView(D::TEXTURE_VIEW_UNORDERED_ACCESS);
		m_outW = input.width; m_outH = input.height;
	}
	return m_volumeUAV && m_volumeSRV && m_outUAV && m_outSRV;
}

bool FroxelBackend::initialize(RenderSubsystem* renderer) {
	m_renderer = renderer;
	if (!m_renderer) return false;
	return createPipelines();
}

void FroxelBackend::shutdown() {
	for (int i = 0; i < 2; ++i) {
		m_injectSrb[i].Release(); m_injectPso[i].Release(); m_injectShader[i].Release();
		m_integrateSrb[i].Release(); m_integratePso[i].Release(); m_integrateShader[i].Release();
	}
	m_volCB.Release(); m_cmpSampler.Release(); m_noiseSampler.Release(); m_query.Release();
	m_volumeTex.Release(); m_volumeSRV.Release(); m_volumeUAV.Release();
	m_outTex.Release(); m_outSRV.Release(); m_outUAV.Release();
	m_volumeX = m_volumeY = m_volumeZ = m_outW = m_outH = 0;
	m_queryEnded = false;
	m_lastMs = 0.0f;
}


Result<void, CoreError> FroxelBackend::render(const VolumetricInput& input, const VolumetricConfig& cfg) {
	auto* context = ctx();
	if (!context) return CoreError::OperationFailed;
	if (!input.depthSRV || !input.shadowSRV || input.width == 0 || input.height == 0) {
		if (!m_loggedFirst) {
			m_loggedFirst = true;
			EError("Volumetric: missing input (depth={} shadow={} size={}x{})",
				input.depthSRV != nullptr, input.shadowSRV != nullptr, input.width, input.height);
		}
		return CoreError::InvalidArgument;
	}
	if (!ensureTargets(input, cfg)) return CoreError::OperationFailed;

	const int variant = input.depthSamples > 1 ? 1 : 0;
	if (!m_injectPso[variant] || !m_injectSrb[variant] || !m_integratePso[variant] || !m_integrateSrb[variant])
		return CoreError::OperationFailed;

	if (m_query && m_queryEnded) {
		D::QueryDataDuration data;
		if (m_query->GetData(&data, sizeof(data), false) && data.Frequency != 0) {
			m_lastMs = static_cast<F32>((double)data.Duration / (double)data.Frequency * 1000.0);
		}
	}

	VolCBData cb{};
	cb.viewProj = input.viewProj;
	cb.viewProjInv = input.viewProjInv;
	cb.cameraPos = Vec4(input.cameraPos, cfg.maxDistance);
	cb.sunDir = Vec4(input.sunDir, cfg.sunScattering);
	cb.sunColor = Vec4(Vec3(input.sunColor) * (input.sunColor.w * cfg.sunIntensity), cfg.ambientScattering);
	for (int i = 0; i < 4; ++i) cb.shadowUVDepth[i] = input.shadowUVDepth[i];
	cb.cascadeSplits = input.cascadeSplits;
	cb.fogParams = Vec4(cfg.density, cfg.baseHeight, cfg.heightScale, cfg.anisotropy);
	cb.quality = Vec4((F32)cfg.froxelSlices, cfg.sunShafts ? 1.0f : 0.0f, std::fmod(input.timeSec, 64.0f), input.ambient.w);
	for (int i = 0; i < 8; ++i) cb.skyCorners[i] = input.skyCorners[i];
	cb.froxelParams = Vec4((F32)cfg.froxelSlices, kNearDistance, (F32)kTileSize, cfg.maxDistance);
	// Local lights: the first eight as the shading paths packed them (CI/DT/PR/CA per light).
	const UInt32 lightCount = std::min<UInt32>(input.lightCount, 8u);
	cb.lightCount = Vec4((F32)lightCount, 0.0f, 0.0f, 0.0f);
	cb.localShadow = Vec4(cfg.localLightShadows ? 1.0f : 0.0f, (F32)cfg.localShadowSteps, 0.0f, 0.0f);
	cb.noiseParams = Vec4(cfg.noiseStrength, cfg.noiseScale, std::fmod(input.timeSec * cfg.noiseSpeed, 64.0f), 0.0f);
	for (UInt32 i = 0; i < lightCount; ++i) {
		cb.lights[i * 4 + 0] = input.lights.lights[i].CI;
		cb.lights[i * 4 + 1] = input.lights.lights[i].DT;
		cb.lights[i * 4 + 2] = input.lights.lights[i].PR;
		cb.lights[i * 4 + 3] = input.lights.lights[i].CA;
	}

	void* mapped = nullptr;
	context->MapBuffer(m_volCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, mapped);
	if (mapped) { memcpy(mapped, &cb, sizeof(cb)); context->UnmapBuffer(m_volCB, D::MAP_WRITE); }

	// ---- pass 1: inject the medium into the volume ----
	if (auto* v = m_injectSrb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_ShadowMap")) v->Set(static_cast<D::ITextureView*>(input.shadowSRV), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = m_injectSrb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_Noise")) { if (input.noiseSRV) v->Set(static_cast<D::ITextureView*>(input.noiseSRV), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE); }
	if (auto* v = m_injectSrb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_Volume")) v->Set(m_volumeUAV, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = m_injectSrb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_Depth")) v->Set(static_cast<D::ITextureView*>(input.depthSRV), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	context->SetPipelineState(m_injectPso[variant]);
	context->CommitShaderResources(m_injectSrb[variant], D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	D::DispatchComputeAttribs inject;
	inject.ThreadGroupCountX = (m_volumeX + 7) / 8;
	inject.ThreadGroupCountY = (m_volumeY + 7) / 8;
	inject.ThreadGroupCountZ = m_volumeZ;
	if (m_query) context->BeginQuery(m_query);
	context->DispatchCompute(inject);
	if (m_query) { context->EndQuery(m_query); m_queryEnded = true; }

	// ---- pass 2: integrate each pixel's column ----
	if (auto* v = m_integrateSrb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_Depth")) v->Set(static_cast<D::ITextureView*>(input.depthSRV), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = m_integrateSrb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_Volume")) v->Set(m_volumeSRV, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = m_integrateSrb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_Out")) v->Set(m_outUAV, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	context->SetPipelineState(m_integratePso[variant]);
	context->CommitShaderResources(m_integrateSrb[variant], D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	D::DispatchComputeAttribs integrate;
	integrate.ThreadGroupCountX = (input.width + 7) / 8;
	integrate.ThreadGroupCountY = (input.height + 7) / 8;
	integrate.ThreadGroupCountZ = 1;
	context->DispatchCompute(integrate);

	if (!m_selfChecked) {
		m_selfChecked = true;
		void* injectShaders[] = { m_injectShader[variant].RawPtr() };
		checkShaderBindings("VolumetricFroxelInjectPSO", m_injectPso[variant].RawPtr(), m_injectSrb[variant].RawPtr(), injectShaders, 1);
		void* integrateShaders[] = { m_integrateShader[variant].RawPtr() };
		checkShaderBindings("VolumetricFroxelIntegratePSO", m_integratePso[variant].RawPtr(), m_integrateSrb[variant].RawPtr(), integrateShaders, 1);
		checkResourceAvailable("volumetric fog (froxel)", "full-res scattering target", m_outSRV.RawPtr());
	}
	if (!m_loggedFirst) {
		m_loggedFirst = true;
		EInfo("Volumetric: froxel volume {}x{}x{} over {}x{} (depth samples {}, {} slices)",
			m_volumeX, m_volumeY, m_volumeZ, input.width, input.height, input.depthSamples, cfg.froxelSlices);
	}

	return {};
}

} // namespace

namespace Detail {

Uptr<IVolumetricBackend> createFroxelBackend() {
	return std::make_unique<FroxelBackend>();
}

} // namespace Detail

EE_NAMESPACE_RENDERING_END
