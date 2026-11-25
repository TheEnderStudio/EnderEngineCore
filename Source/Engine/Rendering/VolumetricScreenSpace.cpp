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
// Screen-space backend: a view-ray march at a fraction of the render resolution, then a
// depth-guided upsample back to full resolution. The march is where the cost is (one shadow
// fetch per step), so halving its resolution per axis is the single biggest saving; the
// upsample uses the scene depth to keep silhouettes from bleeding fog across an edge.
//
// Both passes need the scene depth, which is either single-sample or multisampled depending
// on the renderer's MSAA setting - D3D12 has no "Texture2D or Texture2DMS" declaration, so
// the prologue is prepended to build a variant per depth flavour.
// ===================================================================

/// @brief Depth declaration + loader, prepended to both shader bodies.
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

/// @brief The march itself: one thread per (possibly reduced-resolution) pixel.
static const char* g_ScreenMarchBody = R"(
Texture2DArray                g_ShadowMap  : register(t1);
Texture3D<float>              g_Noise      : register(t2);
SamplerState                  g_NoiseSampler : register(s2);
SamplerComparisonState        g_ShadowSampler : register(s1);
RWTexture2D<float4>           g_Out        : register(u0);

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
    float4   g_Quality;       // x = steps, y = sunShafts, z = time, w = ambient strength
    float4   g_SkyCorners[8];
    float4   g_LightCount;    // x = number of local lights
    float4   g_Lights[4 * 8]; // 8 lights x (CI, DT, PR, CA), the shading shaders' packing
    float4   g_LocalShadow;   // x = enabled, y = steps
    float4   g_NoiseParams;   // x = strength, y = scale, z = time
};

#define MAX_LIGHTS 8

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

// Henyey-Greenstein phase function for the sun direction against the view ray.
float HGPhase(float cosTheta, float g)
{
    const float g2 = g * g;
    const float denom = max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4);
    return (1.0 - g2) / (4.0 * PI * denom * sqrt(denom));
}

// Local lights add in-scattering along the view ray. No shadowing (the pass has no per-light
// shadow data); the sun, which does, goes through SunVisibility.
// Screen-space occlusion along the ray towards the light (see the froxel backend's copy).
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
        if (type < 0.5) continue;
        const float3 toL = PR.xyz - p;
        const float d = length(toL);
        const float3 Ldir = toL / max(d, 1e-4);
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

// Sky irradiance along n: the same closed form the shading paths use, so the fog's ambient
// term carries the sky's colour and direction.
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

// Sun visibility from the cascaded shadow map: one comparison tap per step, because the fog
// integrates over dozens of steps and the per-step cost has to stay at one texel fetch.
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

// Interleaved gradient noise: a cheap, well-distributed per-pixel offset for the march's
// start, which turns step banding into high-frequency grain instead of visible rings.
float IgnoreGradientNoise(float2 p)
{
    return frac(52.9829189 * frac(0.06711056 * p.x + 0.00583715 * p.y));
}

[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
    uint2 dim;
    g_Out.GetDimensions(dim.x, dim.y);
    if (DTid.x >= dim.x || DTid.y >= dim.y) return;

    uint2 depthDim;
    GetSceneDepthSize(depthDim);

    const float2 uv = (float2(DTid) + 0.5) / float2(dim);
    const int2 dp = min(int2(uv * float2(depthDim)), int2(depthDim) - 1);
    const float depth = LoadSceneDepth(dp);

    const float3 camPos = g_CameraPos.xyz;
    const float maxDist = g_CameraPos.w;

    float4 clip = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), depth, 1.0);
    float4 worldH = mul(g_ViewProjInv, clip);
    const float3 worldPos = worldH.xyz / max(worldH.w, 1e-6);
    float3 rayDir = worldPos - camPos;
    float segLen = length(rayDir);
    const bool isSky = depth >= 1.0;
    if (segLen < 1e-4)
    {
        rayDir = float3(0.0, 0.0, 1.0);
        segLen = maxDist;
    }
    rayDir /= max(segLen, 1e-4);
    const float marchLen = isSky ? maxDist : min(segLen, maxDist);

    const int   steps = clamp((int)g_Quality.x, 1, 128);
    const float dt = marchLen / (float)steps;
    const float density = g_FogParams.x;
    const float baseHeight = g_FogParams.y;
    const float heightScale = max(g_FogParams.z, 1e-3);
    const float g = clamp(g_FogParams.w, -0.95, 0.95);
    const float sunScattering = g_SunDir.w;
    const float ambientScattering = g_SunColor.w;
    const float3 sunColor = g_SunColor.rgb;
    const float3 sunDir = normalize(-g_SunDir.xyz); // toward the light
    const float phase = HGPhase(dot(rayDir, sunDir), g);
    const bool shafts = g_Quality.y > 0.5;
    const float3 ambient = SkyIrradiance(float3(0.0, 1.0, 0.0)) * g_Quality.w * ambientScattering;

    const float jitter = IgnoreGradientNoise(float2(DTid) + g_Quality.z * 37.0);

    float3 scattering = float3(0, 0, 0);
    float transmittance = 1.0;
    float t = jitter * dt;
    for (int i = 0; i < steps; ++i)
    {
        if (t >= marchLen) break;
        const float3 p = camPos + rayDir * t;
        const float dens = density * exp(-(p.y - baseHeight) / heightScale) * MediumNoise(p);
        if (dens > 1e-6)
        {
            const float camZ = abs(mul(g_ViewProj, float4(p, 1.0)).w);
            const float vis = shafts ? SunVisibility(p, camZ) : 1.0;
            float3 inLight = sunColor * (sunScattering * phase * vis) + ambient;
            inLight += LocalLightScattering(p, rayDir, g);
            const float stepT = exp(-dens * dt);
            scattering += transmittance * inLight * (1.0 - stepT);
            transmittance *= stepT;
        }
        t += dt;
        if (transmittance < 0.002) break;
    }

    g_Out[DTid] = float4(scattering, saturate(transmittance));
}
)";

/// @brief Depth-guided upsample from the reduced march resolution to the frame.
static const char* g_ScreenUpsampleBody = R"(
Texture2D<float4>             g_March      : register(t1);
RWTexture2D<float4>           g_Out        : register(u0);

cbuffer UpCB : register(b0) { float4 g_UpsampleParams; }; // x = relative depth tolerance

[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
    uint2 dim;
    g_Out.GetDimensions(dim.x, dim.y);
    if (DTid.x >= dim.x || DTid.y >= dim.y) return;
    uint2 mdim;
    g_March.GetDimensions(mdim.x, mdim.y);
    uint2 depthDim;
    GetSceneDepthSize(depthDim);

    const float2 uv = (float2(DTid) + 0.5) / float2(dim);
    const int2 dp = min(int2(uv * float2(depthDim)), int2(depthDim) - 1);
    const float centerDepth = LoadSceneDepth(dp);
    const bool centerSky = centerDepth >= 1.0;

    // Bilinear position in march space, and the 2x2 texels around it.
    const float2 mv = uv * float2(mdim) - 0.5;
    const int2 base = int2(floor(mv));
    const float2 f = mv - float2(base);
    const float tolerance = max(g_UpsampleParams.x, 1e-4) * max(centerDepth, 1e-3);

    float4 acc = 0;
    float accW = 0;
    [unroll]
    for (int dy = 0; dy < 2; ++dy)
    {
        [unroll]
        for (int dx = 0; dx < 2; ++dx)
        {
            const int2 mt = clamp(base + int2(dx, dy), int2(0, 0), int2(mdim) - 1);
            // The depth the march used for that texel: its centre projected back to full res.
            const float2 muv = (float2(mt) + 0.5) / float2(mdim);
            const int2 fp = min(int2(muv * float2(depthDim)), int2(depthDim) - 1);
            const float d = LoadSceneDepth(fp);
            const bool sky = d >= 1.0;
            // A sky texel must not bleed into geometry and vice versa: that is what produces
            // the halo around silhouettes in a naive upsample.
            float w = (sky == centerSky) ? exp(-abs(d - centerDepth) / tolerance) : 0.0;
            w *= (dx == 0 ? (1.0 - f.x) : f.x) * (dy == 0 ? (1.0 - f.y) : f.y);
            acc += g_March.Load(int3(mt, 0)) * w;
            accW += w;
        }
    }
    g_Out[DTid] = (accW > 1e-5) ? acc / accW : g_March.Load(int3(min(int2(uv * float2(mdim)), int2(mdim) - 1), 0));
}
)";

/// @brief Must match both shaders' cbuffer layouts (VolCB keeps the larger one).
struct VolCBData {
	Mat4 viewProj;
	Mat4 viewProjInv;
	Vec4 cameraPos;      // w = maxDistance
	Vec4 sunDir;         // w = sunScattering
	Vec4 sunColor;       // rgb already scaled, w = ambientScattering
	Mat4 shadowUVDepth[4];
	Vec4 cascadeSplits;
	Vec4 fogParams;
	Vec4 quality;
	Vec4 skyCorners[8];
	Vec4 lightCount;
	Vec4 lights[32];
	Vec4 localShadow;
	Vec4 noiseParams;
};

constexpr UInt32 kVolCBAlign = 16;

/// @brief The view-ray march backend.
class ScreenSpaceBackend final : public Detail::IVolumetricBackend {
public:
	bool initialize(RenderSubsystem* renderer) override;
	void shutdown() override;
	Result<void, CoreError> render(const VolumetricInput& input, const VolumetricConfig& cfg) override;
	void* scatteringSRV() const override { return m_outSRV.RawPtr(); }
	F32 lastRenderMs() const override { return m_lastMs; }
	const char* name() const override { return "screen space"; }

private:
	RenderSubsystem* m_renderer = nullptr;
	/// March and upsample, one per depth flavour.
	D::RefCntAutoPtr<D::IPipelineState> m_marchPso[2];
	D::RefCntAutoPtr<D::IShaderResourceBinding> m_marchSrb[2];
	D::RefCntAutoPtr<D::IShader> m_marchShader[2];
	D::RefCntAutoPtr<D::IPipelineState> m_upsamplePso[2];
	D::RefCntAutoPtr<D::IShaderResourceBinding> m_upsampleSrb[2];
	D::RefCntAutoPtr<D::IShader> m_upsampleShader[2];
	D::RefCntAutoPtr<D::IBuffer> m_volCB;
	D::RefCntAutoPtr<D::IBuffer> m_upCB;
	D::RefCntAutoPtr<D::ISampler> m_cmpSampler;
	/// Wrapping sampler for the noise volume.
	D::RefCntAutoPtr<D::ISampler> m_noiseSampler;
	/// Reduced-resolution march result.
	D::RefCntAutoPtr<D::ITexture> m_marchTex;
	D::RefCntAutoPtr<D::ITextureView> m_marchSRV, m_marchUAV;
	UInt32 m_marchW = 0, m_marchH = 0;
	/// Full-resolution result the composite reads.
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
	bool ensureTargets(UInt32 width, UInt32 height, F32 scale);
};

bool ScreenSpaceBackend::createPipelines() {
	auto* dev = device();
	if (!dev) return false;

	const char* const prologues[2] = { g_DepthPrologueSingle, g_DepthPrologueMsaa };
	struct PassSpec {
		const char* prefix;
		const char* body;
		const char* psoName;
		UInt32 cbSize;
		bool isUpsample;
	};
	const PassSpec passes[2] = {
		{ "VolumetricScreenMarch", g_ScreenMarchBody, "VolumetricScreenMarchPSO", sizeof(VolCBData), false },
		{ "VolumetricScreenUpsample", g_ScreenUpsampleBody, "VolumetricScreenUpsamplePSO", 16, true },
	};

	// Comparison sampler for the shadow taps.
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

	for (int variant = 0; variant < 2; ++variant) {
		for (int pi = 0; pi < 2; ++pi) {
			const PassSpec& pass = passes[pi];
			String source = String(prologues[variant]) + pass.body;

			D::ShaderCreateInfo ci;
			ci.Desc.Name = pass.prefix;
			ci.Desc.ShaderType = D::SHADER_TYPE_COMPUTE;
			ci.EntryPoint = "main";
			ci.SourceLanguage = D::SHADER_SOURCE_LANGUAGE_HLSL;
			ci.Source = source.c_str();
			ci.SourceLength = static_cast<D::Uint32>(source.size());
			D::RefCntAutoPtr<D::IShader>& shader = pass.isUpsample ? m_upsampleShader[variant] : m_marchShader[variant];
			dev->CreateShader(ci, &shader);
			if (!shader || shader->GetStatus() != D::SHADER_STATUS_READY) {
				EError("Volumetric: {} shader failed (depth variant {})", pass.prefix, variant);
				return false;
			}

			D::ComputePipelineStateCreateInfo pci;
			pci.PSODesc.Name = pass.psoName;
			pci.PSODesc.PipelineType = D::PIPELINE_TYPE_COMPUTE;
			pci.PSODesc.ResourceLayout.DefaultVariableType = D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
			pci.pCS = shader;
			D::RefCntAutoPtr<D::IPipelineState>& pso = pass.isUpsample ? m_upsamplePso[variant] : m_marchPso[variant];
			D::RefCntAutoPtr<D::IShaderResourceBinding>& srb = pass.isUpsample ? m_upsampleSrb[variant] : m_marchSrb[variant];
			dev->CreateComputePipelineState(pci, &pso);
			if (!pso) {
				EError("Volumetric: {} PSO failed", pass.psoName);
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

	D::BufferDesc ub;
	ub.Name = "VolUpsampleCB";
	ub.Size = kVolCBAlign;
	ub.BindFlags = D::BIND_UNIFORM_BUFFER;
	ub.Usage = D::USAGE_DYNAMIC;
	ub.CPUAccessFlags = D::CPU_ACCESS_WRITE;
	dev->CreateBuffer(ub, nullptr, &m_upCB);

	for (int variant = 0; variant < 2; ++variant) {
		if (m_marchSrb[variant]) {
			if (auto* v = m_marchSrb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "VolCB")) v->Set(m_volCB);
			if (auto* v = m_marchSrb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_ShadowSampler")) v->Set(m_cmpSampler);
			if (auto* v = m_marchSrb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_NoiseSampler")) v->Set(m_noiseSampler);
		}
		if (m_upsampleSrb[variant]) {
			if (auto* v = m_upsampleSrb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "UpCB")) v->Set(m_upCB);
		}
	}

	D::QueryDesc qd(D::QUERY_TYPE_DURATION);
	dev->CreateQuery(qd, &m_query);
	return true;
}

bool ScreenSpaceBackend::ensureTargets(UInt32 width, UInt32 height, F32 scale) {
	auto* dev = device();
	if (!dev || width == 0 || height == 0) return false;
	const UInt32 mw = (UInt32)std::max(1.0f, std::round((F32)width * scale));
	const UInt32 mh = (UInt32)std::max(1.0f, std::round((F32)height * scale));

	if (!m_marchTex || m_marchW != mw || m_marchH != mh) {
		D::TextureDesc td;
		td.Name = "VolumetricMarch";
		td.Type = D::RESOURCE_DIM_TEX_2D;
		td.Width = mw;
		td.Height = mh;
		td.Format = D::TEX_FORMAT_RGBA16_FLOAT;
		td.BindFlags = D::BIND_SHADER_RESOURCE | D::BIND_UNORDERED_ACCESS;
		td.Usage = D::USAGE_DEFAULT;
		m_marchTex.Release(); m_marchSRV.Release(); m_marchUAV.Release();
		dev->CreateTexture(td, nullptr, &m_marchTex);
		if (!m_marchTex) return false;
		m_marchSRV = m_marchTex->GetDefaultView(D::TEXTURE_VIEW_SHADER_RESOURCE);
		m_marchUAV = m_marchTex->GetDefaultView(D::TEXTURE_VIEW_UNORDERED_ACCESS);
		m_marchW = mw; m_marchH = mh;
	}
	if (!m_outTex || m_outW != width || m_outH != height) {
		D::TextureDesc td;
		td.Name = "VolumetricScattering";
		td.Type = D::RESOURCE_DIM_TEX_2D;
		td.Width = width;
		td.Height = height;
		td.Format = D::TEX_FORMAT_RGBA16_FLOAT;
		td.BindFlags = D::BIND_SHADER_RESOURCE | D::BIND_UNORDERED_ACCESS;
		td.Usage = D::USAGE_DEFAULT;
		m_outTex.Release(); m_outSRV.Release(); m_outUAV.Release();
		dev->CreateTexture(td, nullptr, &m_outTex);
		if (!m_outTex) return false;
		m_outSRV = m_outTex->GetDefaultView(D::TEXTURE_VIEW_SHADER_RESOURCE);
		m_outUAV = m_outTex->GetDefaultView(D::TEXTURE_VIEW_UNORDERED_ACCESS);
		m_outW = width; m_outH = height;
	}
	return m_marchUAV && m_marchSRV && m_outUAV && m_outSRV;
}

bool ScreenSpaceBackend::initialize(RenderSubsystem* renderer) {
	m_renderer = renderer;
	if (!m_renderer) return false;
	return createPipelines();
}

void ScreenSpaceBackend::shutdown() {
	for (int i = 0; i < 2; ++i) {
		m_marchSrb[i].Release(); m_marchPso[i].Release(); m_marchShader[i].Release();
		m_upsampleSrb[i].Release(); m_upsamplePso[i].Release(); m_upsampleShader[i].Release();
	}
	m_volCB.Release(); m_upCB.Release(); m_cmpSampler.Release(); m_noiseSampler.Release(); m_query.Release();
	m_marchTex.Release(); m_marchSRV.Release(); m_marchUAV.Release();
	m_outTex.Release(); m_outSRV.Release(); m_outUAV.Release();
	m_marchW = m_marchH = m_outW = m_outH = 0;
	m_queryEnded = false;
	m_lastMs = 0.0f;
}


Result<void, CoreError> ScreenSpaceBackend::render(const VolumetricInput& input, const VolumetricConfig& cfg) {
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
	const F32 scale = std::clamp(cfg.renderScale, 0.25f, 1.0f);
	if (!ensureTargets(input.width, input.height, scale)) return CoreError::OperationFailed;

	const int variant = input.depthSamples > 1 ? 1 : 0;
	if (!m_marchPso[variant] || !m_marchSrb[variant] || !m_upsamplePso[variant] || !m_upsampleSrb[variant])
		return CoreError::OperationFailed;

	// Read the previous frame's measurement before starting a new one.
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
	cb.quality = Vec4((F32)cfg.steps, cfg.sunShafts ? 1.0f : 0.0f, std::fmod(input.timeSec, 64.0f), input.ambient.w);
	for (int i = 0; i < 8; ++i) cb.skyCorners[i] = input.skyCorners[i];
	const UInt32 localLights = std::min<UInt32>(input.lightCount, 8u);
	cb.lightCount = Vec4((F32)localLights, 0.0f, 0.0f, 0.0f);
	cb.localShadow = Vec4(cfg.localLightShadows ? 1.0f : 0.0f, (F32)cfg.localShadowSteps, 0.0f, 0.0f);
	cb.noiseParams = Vec4(cfg.noiseStrength, cfg.noiseScale, std::fmod(input.timeSec * cfg.noiseSpeed, 64.0f), 0.0f);
	for (UInt32 i = 0; i < localLights; ++i) {
		cb.lights[i * 4 + 0] = input.lights.lights[i].CI;
		cb.lights[i * 4 + 1] = input.lights.lights[i].DT;
		cb.lights[i * 4 + 2] = input.lights.lights[i].PR;
		cb.lights[i * 4 + 3] = input.lights.lights[i].CA;
	}

	void* mapped = nullptr;
	context->MapBuffer(m_volCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, mapped);
	if (mapped) { memcpy(mapped, &cb, sizeof(cb)); context->UnmapBuffer(m_volCB, D::MAP_WRITE); }

	// ---- pass 1: march at the reduced resolution ----
	if (auto* v = m_marchSrb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_Depth")) v->Set(static_cast<D::ITextureView*>(input.depthSRV), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = m_marchSrb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_ShadowMap")) v->Set(static_cast<D::ITextureView*>(input.shadowSRV), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = m_marchSrb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_Noise")) { if (input.noiseSRV) v->Set(static_cast<D::ITextureView*>(input.noiseSRV), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE); }
	if (auto* v = m_marchSrb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_Out")) v->Set(m_marchUAV, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);

	context->SetPipelineState(m_marchPso[variant]);
	context->CommitShaderResources(m_marchSrb[variant], D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	D::DispatchComputeAttribs march;
	march.ThreadGroupCountX = (m_marchW + 7) / 8;
	march.ThreadGroupCountY = (m_marchH + 7) / 8;
	march.ThreadGroupCountZ = 1;
	if (m_query) context->BeginQuery(m_query);
	context->DispatchCompute(march);
	if (m_query) { context->EndQuery(m_query); m_queryEnded = true; }

	// ---- pass 2: depth-guided upsample to the frame resolution ----
	{
		const F32 params[4] = { 0.02f, 0.0f, 0.0f, 0.0f }; // relative depth tolerance
		void* m2 = nullptr;
		context->MapBuffer(m_upCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m2);
		if (m2) { memcpy(m2, params, sizeof(params)); context->UnmapBuffer(m_upCB, D::MAP_WRITE); }
	}
	if (auto* v = m_upsampleSrb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_Depth")) v->Set(static_cast<D::ITextureView*>(input.depthSRV), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = m_upsampleSrb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_March")) v->Set(m_marchSRV, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = m_upsampleSrb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_Out")) v->Set(m_outUAV, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	context->SetPipelineState(m_upsamplePso[variant]);
	context->CommitShaderResources(m_upsampleSrb[variant], D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	D::DispatchComputeAttribs up;
	up.ThreadGroupCountX = (input.width + 7) / 8;
	up.ThreadGroupCountY = (input.height + 7) / 8;
	up.ThreadGroupCountZ = 1;
	context->DispatchCompute(up);

	if (!m_selfChecked) {
		m_selfChecked = true;
		void* shaders[] = { m_marchShader[variant].RawPtr() };
		checkShaderBindings("VolumetricScreenMarchPSO", m_marchPso[variant].RawPtr(), m_marchSrb[variant].RawPtr(), shaders, 1);
		void* upShaders[] = { m_upsampleShader[variant].RawPtr() };
		checkShaderBindings("VolumetricScreenUpsamplePSO", m_upsamplePso[variant].RawPtr(), m_upsampleSrb[variant].RawPtr(), upShaders, 1);
		checkResourceAvailable("volumetric fog (screen space)", "full-res scattering target", m_outSRV.RawPtr());
	}
	if (!m_loggedFirst) {
		m_loggedFirst = true;
		EInfo("Volumetric: screen-space march {}x{} -> {}x{} (depth samples {}, {} steps, scale {:.2f})",
			m_marchW, m_marchH, input.width, input.height, input.depthSamples, cfg.steps, scale);
	}

	return {};
}

} // namespace

namespace Detail {

Uptr<IVolumetricBackend> createScreenSpaceBackend() {
	return std::make_unique<ScreenSpaceBackend>();
}

} // namespace Detail

EE_NAMESPACE_RENDERING_END
