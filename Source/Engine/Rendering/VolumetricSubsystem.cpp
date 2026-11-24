#include <Rendering/VolumetricSubsystem.hpp>
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

#include <cmath>
#include <cstring>

namespace D = Diligent;

EE_NAMESPACE_RENDERING_BEGIN

// ===================================================================
// Scattering compute shader.
//
// One thread per output pixel: reconstruct the view ray, march it through the height fog up
// to the scene depth (or the configured distance for the sky), and integrate single
// scattering. Sun visibility comes from the cascade the position falls into, so the shadow
// map's silhouettes become shafts. Output rgb = in-scattering, a = transmittance.
// ===================================================================
// The scene depth is either a single-sample or a multisampled target depending on the
// renderer's MSAA setting, and D3D12 has no "Texture2D or Texture2DMS" declaration, so the
// prologue is prepended to build two variants of the same shader.
static const char* g_DepthPrologueSingle = R"(
Texture2D<float>              g_Depth      : register(t0);
float LoadSceneDepth(int2 p) { return g_Depth.Load(int3(p, 0)).x; }
void GetSceneDepthSize(out uint2 dim) { g_Depth.GetDimensions(dim.x, dim.y); }
)";

static const char* g_DepthPrologueMsaa = R"(
Texture2DMS<float>             g_Depth      : register(t0);
float LoadSceneDepth(int2 p) { return g_Depth.Load(p, 0); }
void GetSceneDepthSize(out uint2 dim) { uint samples; g_Depth.GetDimensions(dim.x, dim.y, samples); }
)";

static const char* g_VolumetricBody = R"(
Texture2DArray                g_ShadowMap  : register(t1);
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
};

#define PI 3.14159265358979

// Henyey-Greenstein phase function for the sun direction against the view ray.
float HGPhase(float cosTheta, float g)
{
    const float g2 = g * g;
    const float denom = max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4);
    return (1.0 - g2) / (4.0 * PI * denom * sqrt(denom));
}

// Sky irradiance along n, the same closed form the shading paths use, so the fog's ambient
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

// Sun visibility from the cascaded shadow map. The cascade is chosen from the camera-space
// depth of the marched position, exactly like the shading paths, and the lookup is a single
// comparison tap: the fog integrates over dozens of steps, so the per-step cost has to stay
// at one texel fetch.
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
    // Same slope-independent bias the shading paths use: a small constant along the
    // normalized depth of the cascade.
    return g_ShadowMap.SampleCmpLevelZero(g_ShadowSampler, float3(sc.xy, (float)c), sc.z - 0.002).r;
}

// Interleaved gradient noise: a cheap, well-distributed per-pixel offset for the march's
// start, which turns the step banding into high-frequency noise the eye reads as grain.
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

    // Ray through the pixel, from the inverse view-projection.
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
    const float cosTheta = dot(rayDir, sunDir);
    const float phase = HGPhase(cosTheta, g);
    const bool shafts = g_Quality.y > 0.5;

    // Jitter the first sample: banding from a too-coarse march becomes noise instead of
    // visible rings, and a changing per-frame offset lets a future temporal pass average it.
    const float jitter = IgnoreGradientNoise(float2(DTid) + g_Quality.z * 37.0);

    float3 scattering = float3(0, 0, 0);
    float transmittance = 1.0;
    float t = jitter * dt;
    for (int i = 0; i < steps; ++i)
    {
        if (t >= marchLen) break;
        const float3 p = camPos + rayDir * t;
        // Exponential height density, and the ambient term folded in per step.
        const float dens = density * exp(-(p.y - baseHeight) / heightScale);
        if (dens > 1e-6)
        {
            const float camZ = abs(mul(g_ViewProj, float4(p, 1.0)).w);
            const float vis = shafts ? SunVisibility(p, camZ) : 1.0;
            const float3 ambient = SkyIrradiance(float3(0.0, 1.0, 0.0)) * g_Quality.w * ambientScattering;
            const float3 inLight = sunColor * (sunScattering * phase * vis) + ambient;
            // Single scattering with extinction: analytic integration of
            //   L(t) = L0 * exp(-sigma * t)  accumulated over the step.
            const float stepT = exp(-dens * dt);
            scattering += transmittance * inLight * (1.0 - stepT);
            transmittance *= stepT;
        }
        t += dt;
        if (transmittance < 0.002) break; // opaque: nothing behind it will show through
    }

    g_Out[DTid] = float4(scattering, saturate(transmittance));
}
)";

struct VolumetricSubsystem::Impl {
	RenderSubsystem* renderer = nullptr;
	VolumetricConfig cfg;

	D::RefCntAutoPtr<D::IShader> shader;
	/// One per depth flavour: single-sample and multisampled scene depth.
	D::RefCntAutoPtr<D::IPipelineState> pso[2];
	D::RefCntAutoPtr<D::IShaderResourceBinding> srb[2];
	D::RefCntAutoPtr<D::IBuffer> cb;
	D::RefCntAutoPtr<D::ISampler> cmpSampler;
	D::RefCntAutoPtr<D::ITexture> target;
	D::RefCntAutoPtr<D::ITextureView> targetSRV;
	D::RefCntAutoPtr<D::ITextureView> targetUAV;
	UInt32 targetW = 0, targetH = 0;
	F32 lastMs = 0.0f;
	bool loggedNotReady = false;
	bool loggedMissingInput = false;
	bool loggedFirstDispatch = false;
	D::RefCntAutoPtr<D::IQuery> query;
	bool queryEnded = false;
	bool ready = false;

	D::IRenderDevice* device() const { return renderer ? static_cast<D::IRenderDevice*>(renderer->getDevice()) : nullptr; }
	D::IDeviceContext* ctx() const { return renderer ? static_cast<D::IDeviceContext*>(renderer->getContext()) : nullptr; }

	bool createPso();
	bool createTarget(UInt32 w, UInt32 h);
};

bool VolumetricSubsystem::Impl::createPso() {
	auto* dev = device();
	if (!dev) return false;

	const char* const prologues[2] = { g_DepthPrologueSingle, g_DepthPrologueMsaa };
	for (int variant = 0; variant < 2; ++variant) {
		String source = String(prologues[variant]) + g_VolumetricBody;

		D::ShaderCreateInfo ci;
		ci.Desc.Name = variant == 0 ? "VolumetricCS" : "VolumetricCS_MSAA";
		ci.Desc.ShaderType = D::SHADER_TYPE_COMPUTE;
		ci.EntryPoint = "main";
		ci.SourceLanguage = D::SHADER_SOURCE_LANGUAGE_HLSL;
		ci.Source = source.c_str();
		ci.SourceLength = static_cast<D::Uint32>(source.size());
		D::RefCntAutoPtr<D::IShader> sh;
		dev->CreateShader(ci, &sh);
		if (!sh || sh->GetStatus() != D::SHADER_STATUS_READY) {
			EError("Volumetric: shader compilation failed (variant {})", variant);
			return false;
		}
		if (variant == 0) shader = sh;

		D::ComputePipelineStateCreateInfo pci;
		pci.PSODesc.Name = variant == 0 ? "VolumetricPSO" : "VolumetricPSO_MSAA";
		pci.PSODesc.PipelineType = D::PIPELINE_TYPE_COMPUTE;
		pci.PSODesc.ResourceLayout.DefaultVariableType = D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
		pci.pCS = sh;
		dev->CreateComputePipelineState(pci, &pso[variant]);
		if (!pso[variant]) {
			EError("Volumetric: PSO creation failed (variant {})", variant);
			return false;
		}
		pso[variant]->CreateShaderResourceBinding(&srb[variant], true);
	}

	D::BufferDesc bd;
	bd.Name = "VolCB";
	bd.Size = sizeof(Mat4) * 7 + sizeof(Vec4) * 14;
	bd.BindFlags = D::BIND_UNIFORM_BUFFER;
	bd.Usage = D::USAGE_DYNAMIC;
	bd.CPUAccessFlags = D::CPU_ACCESS_WRITE;
	dev->CreateBuffer(bd, nullptr, &cb);
	for (int variant = 0; variant < 2; ++variant) {
		if (!srb[variant]) continue;
		if (auto* v = srb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "VolCB")) v->Set(cb);
		if (auto* v = srb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_ShadowSampler")) v->Set(cmpSampler);
	}
	return true;
}

bool VolumetricSubsystem::Impl::createTarget(UInt32 w, UInt32 h) {
	auto* dev = device();
	if (!dev || w == 0 || h == 0) return false;
	D::TextureDesc td;
	td.Name = "VolumetricScattering";
	td.Type = D::RESOURCE_DIM_TEX_2D;
	td.Width = w;
	td.Height = h;
	td.Format = D::TEX_FORMAT_RGBA16_FLOAT;
	td.BindFlags = D::BIND_SHADER_RESOURCE | D::BIND_UNORDERED_ACCESS;
	td.Usage = D::USAGE_DEFAULT;
	target.Release(); targetSRV.Release(); targetUAV.Release();
	dev->CreateTexture(td, nullptr, &target);
	if (!target) return false;
	targetSRV = target->GetDefaultView(D::TEXTURE_VIEW_SHADER_RESOURCE);
	targetUAV = target->GetDefaultView(D::TEXTURE_VIEW_UNORDERED_ACCESS);
	targetW = w; targetH = h;
	return targetSRV && targetUAV;
}

VolumetricSubsystem::VolumetricSubsystem() : Subsystem("Volumetric"), m_impl(std::make_unique<Impl>()) {}
VolumetricSubsystem::~VolumetricSubsystem() = default;

void VolumetricSubsystem::attachToRenderer(RenderSubsystem* r) { m_impl->renderer = r; }
void VolumetricSubsystem::setConfig(const VolumetricConfig& cfg) { m_impl->cfg = cfg; }
const VolumetricConfig& VolumetricSubsystem::config() const { return m_impl->cfg; }
void* VolumetricSubsystem::getScatteringSRV() const { return m_impl->ready ? m_impl->targetSRV.RawPtr() : nullptr; }
F32 VolumetricSubsystem::lastRenderMs() const { return m_impl->lastMs; }

Result<void, CoreError> VolumetricSubsystem::onInitialize() {
	auto& p = *m_impl;
	if (!p.renderer) { EError("Volumetric: no renderer attached"); return CoreError::OperationFailed; }
	auto* dev = p.device();
	auto* ctx = p.ctx();
	if (!dev || !ctx) return CoreError::OperationFailed;

	D::SamplerDesc sd;
	sd.MinFilter = D::FILTER_TYPE_COMPARISON_LINEAR;
	sd.MagFilter = D::FILTER_TYPE_COMPARISON_LINEAR;
	sd.MipFilter = D::FILTER_TYPE_COMPARISON_LINEAR;
	sd.ComparisonFunc = D::COMPARISON_FUNC_LESS;
	sd.AddressU = D::TEXTURE_ADDRESS_CLAMP;
	sd.AddressV = D::TEXTURE_ADDRESS_CLAMP;
	sd.AddressW = D::TEXTURE_ADDRESS_CLAMP;
	dev->CreateSampler(sd, &p.cmpSampler);

	if (!p.createPso()) return CoreError::OperationFailed;

	// GPU timer for the march: a single query read back one frame later, the same pattern
	// the ray tracing pass uses, so the DebugUI readout means the same thing.
	D::QueryDesc qd(D::QUERY_TYPE_DURATION);
	dev->CreateQuery(qd, &p.query);
	p.ready = true;
	EInfo("Volumetric: single-scattering pass ready ({} steps, max {:.0f} units)", p.cfg.steps, p.cfg.maxDistance);
	return {};
}

void VolumetricSubsystem::onShutdown() {
	auto& p = *m_impl;
	p.ready = false;
	for (int i = 0; i < 2; ++i) { p.srb[i].Release(); p.pso[i].Release(); }
	p.shader.Release(); p.cb.Release(); p.cmpSampler.Release(); p.query.Release();
	p.target.Release(); p.targetSRV.Release(); p.targetUAV.Release();
	p.targetW = p.targetH = 0;
}

Result<void, CoreError> VolumetricSubsystem::render(const VolumetricInput& input) {
	auto& p = *m_impl;
	// One-shot diagnostics: a silent early return here is indistinguishable from "the fog does
	// nothing" from the outside, so each reason says so exactly once.
	if (!p.ready) {
		if (!p.loggedNotReady) { p.loggedNotReady = true; EError("Volumetric: render() called before the pass is ready (see the initialization error above)"); }
		return CoreError::OperationFailed;
	}
	if (!p.cfg.enabled) return {};
	if (!input.depthSRV || !input.shadowSRV || input.width == 0 || input.height == 0) {
		if (!p.loggedMissingInput) {
			p.loggedMissingInput = true;
			EError("Volumetric: missing input (depth={} shadow={} size={}x{})",
				input.depthSRV != nullptr, input.shadowSRV != nullptr, input.width, input.height);
		}
		return CoreError::InvalidArgument;
	}
	if (!p.target || p.targetW != input.width || p.targetH != input.height) {
		if (!p.createTarget(input.width, input.height)) {
			if (!p.loggedMissingInput) { p.loggedMissingInput = true; EError("Volumetric: could not create the {}x{} scattering target", input.width, input.height); }
			return CoreError::OperationFailed;
		}
	}
	if (!p.loggedFirstDispatch) {
		p.loggedFirstDispatch = true;
		EInfo("Volumetric: first dispatch at {}x{} (depth samples {}, {} steps, density {:.3f})",
			input.width, input.height, input.depthSamples, p.cfg.steps, p.cfg.density);
	}

	// Must match the shader's cbuffer member for member.
	struct VolCBData {
		Mat4 viewProj;
		Mat4 viewProjInv;
		Vec4 cameraPos;
		Vec4 sunDir;
		Vec4 sunColor;
		Mat4 shadowUVDepth[4];
		Vec4 cascadeSplits;
		Vec4 fogParams;
		Vec4 quality;
		Vec4 skyCorners[8];
	} data;
	data.viewProj = input.viewProj;
	data.viewProjInv = input.viewProjInv;
	data.cameraPos = Vec4(input.cameraPos, p.cfg.maxDistance);
	data.sunDir = Vec4(input.sunDir, p.cfg.sunScattering);
	data.sunColor = Vec4(Vec3(input.sunColor) * (input.sunColor.w * p.cfg.sunIntensity), p.cfg.ambientScattering);
	for (int i = 0; i < 4; ++i) data.shadowUVDepth[i] = input.shadowUVDepth[i];
	data.cascadeSplits = input.cascadeSplits;
	data.fogParams = Vec4(p.cfg.density, p.cfg.baseHeight, p.cfg.heightScale, p.cfg.anisotropy);
	data.quality = Vec4(static_cast<F32>(p.cfg.steps), p.cfg.sunShafts ? 1.0f : 0.0f,
		std::fmod(input.timeSec, 64.0f), input.ambient.w);
	for (int i = 0; i < 8; ++i) data.skyCorners[i] = input.skyCorners[i];

	auto* ctx = p.ctx();
	void* mapped = nullptr;
	ctx->MapBuffer(p.cb, D::MAP_WRITE, D::MAP_FLAG_DISCARD, mapped);
	if (mapped) { memcpy(mapped, &data, sizeof(data)); ctx->UnmapBuffer(p.cb, D::MAP_WRITE); }

	const int variant = input.depthSamples > 1 ? 1 : 0;
	if (!p.pso[variant] || !p.srb[variant]) return CoreError::OperationFailed;
	if (auto* v = p.srb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_Depth")) v->Set(static_cast<D::ITextureView*>(input.depthSRV), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = p.srb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_ShadowMap")) v->Set(static_cast<D::ITextureView*>(input.shadowSRV), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = p.srb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_Out")) v->Set(p.targetUAV, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);

	if (p.query && p.queryEnded) {
		D::QueryDataDuration qdur;
		if (p.query->GetData(&qdur, sizeof(qdur), false) && qdur.Frequency != 0) {
			p.lastMs = static_cast<F32>((double)qdur.Duration / (double)qdur.Frequency * 1000.0);
		}
	}

	ctx->SetPipelineState(p.pso[variant]);
	ctx->CommitShaderResources(p.srb[variant], D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	D::DispatchComputeAttribs da;
	da.ThreadGroupCountX = (input.width + 7) / 8;
	da.ThreadGroupCountY = (input.height + 7) / 8;
	if (p.query) ctx->BeginQuery(p.query);
	ctx->DispatchCompute(da);
	if (p.query) { ctx->EndQuery(p.query); p.queryEnded = true; }

	// Hand the framebuffer depth back in the state the next frame's scene pass expects. The
	// SRV binding above moved it to SHADER_RESOURCE, and Diligent otherwise reports
	// "requires texture 'Depth' to be transitioned to DEPTH_WRITE state" on every frame.
	if (input.restoreDepthState) {
		if (auto* view = static_cast<D::ITextureView*>(input.depthSRV)) {
			if (auto* tex = view->GetTexture()) {
				D::StateTransitionDesc barrier(tex, D::RESOURCE_STATE_SHADER_RESOURCE, D::RESOURCE_STATE_DEPTH_WRITE,
					D::STATE_TRANSITION_FLAG_UPDATE_STATE);
				ctx->TransitionResourceStates(1, &barrier);
			}
		}
	}
	return {};
}

EE_NAMESPACE_RENDERING_END
