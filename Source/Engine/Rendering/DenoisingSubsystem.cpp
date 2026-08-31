#include <Rendering/DenoisingSubsystem.hpp>
#include <Rendering/RenderSubsystem.hpp>
#include <Core/Log.hpp>

#include <DiligentCore/Graphics/GraphicsEngine/interface/RenderDevice.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/DeviceContext.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Shader.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/PipelineState.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Buffer.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Sampler.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/GraphicsTypes.h>
#include <DiligentCore/Common/interface/RefCntAutoPtr.hpp>

#include <NRD.h>
#include <DenoisingSubsystemShaders.generated.hpp>
#include <NRDShaderResources.generated.hpp>

#include <cstring>

namespace D = Diligent;

EE_NAMESPACE_RENDERING_BEGIN

using namespace NRDShaders;

// ===================================================================
// Prep compute shader: RT output (RGBA32F, rgb = reflection, a = lighting
// factor) + RT-res normal + full-res depth -> NRD 4.18 REBLUR inputs at the
// RT resolution:
//   - IN_MV                 RG16F  prevUv - curUv (UV units, camera only)
//   - IN_VIEWZ              R32F   positive view-space depth
//   - IN_NORMAL_ROUGHNESS   R10G10B10A2_UNORM (NRD_NORMAL_ENCODING=2 oct-pack)
//   - IN_*_RADIANCE_HITDIST RGBA16F (YCoCg radiance + normalized hit distance)
// ===================================================================
static const char* g_PrepCS = R"(
Texture2D<float4> g_InColor  : register(t0); // RT output (RGBA32F)
Texture2D<float4> g_InNormal : register(t1); // RT-res world normal + roughness (a)
Texture2D<float>  g_InDepth  : register(t2); // full-res depth (R32F)
RWTexture2D<float2> g_OutMv        : register(u0); // RG16F: prevUv - curUv (UV units)
RWTexture2D<float>  g_OutViewZ     : register(u1); // R32F: positive view-space Z
RWTexture2D<float4> g_OutNormalRgh : register(u2); // R10G10B10A2: oct-packed normal + roughness
RWTexture2D<float4> g_OutSpecRad   : register(u3); // RGBA16F: YCoCg(spec) + normHitDist
RWTexture2D<float4> g_OutDiffRad   : register(u4); // RGBA16F: YCoCg(diff) + normHitDist
cbuffer PrepCB : register(b0) {
    float4x4 g_ViewProjInv;    // inverse(proj * view)
    float4x4 g_View;           // world -> view
    float4x4 g_PrevViewProj;   // previous frame proj * view
    float4    g_HitDistParams; // x = A, y = B, z = C
    uint2     g_RTDim;
    uint2     g_FullDim;
    float     g_DenoisingRange;
    float     _pad;
};

float3 Unproject(float2 uv, float depth)
{
    float4 clip = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), depth, 1.0);
    float4 w    = mul(g_ViewProjInv, clip);
    return w.xyz / w.w;
}

// NRD 4.18 normal+roughness packing for NRD_NORMAL_ENCODING=R10G10B10A2_UNORM
// (improved oct-packing, roughness + n.z sign in the z channel).
float3 EncodeNormalRoughness101010(float3 n, float roughness)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    float3 r;
    r.y = n.y * 0.5 + 0.5;
    r.x = n.x * 0.5 + r.y;
    r.y -= n.x * 0.5;
    roughness = max(roughness, 1.5 / 512.0); // can't be 0 to not ruin the n.z sign bit
    float s = n.z < 0 ? -roughness : roughness;
    r.z = s * 0.5 + 0.5;
    return r;
}

// NRD 4.18 YCoCg color space for radiance (matches _NRD_LinearToYCoCg).
float3 LinearToYCoCg(float3 c)
{
    float Y  = dot(c, float3(0.25, 0.5, 0.25));
    float Co = dot(c, float3(0.5, 0.0, -0.5));
    float Cg = dot(c, float3(-0.25, 0.5, -0.25));
    return float3(Y, Co, Cg);
}

// REBLUR normalized hit distance: saturate(hitDist / f), f = (A + |viewZ|*B) * lerp(C, 1, smc)
float GetNormHitDist(float hitDist, float viewZ, float roughness)
{
    float smc = (1.0 - exp2(-200.0 * roughness * roughness)) * pow(saturate(roughness), 0.5);
    float f   = (g_HitDistParams.x + abs(viewZ) * g_HitDistParams.y) * lerp(g_HitDistParams.z, 1.0, smc);
    return saturate(hitDist / max(f, 1e-6));
}

[numthreads(8, 8, 1)]
void CSMain(uint2 DTid : SV_DispatchThreadID)
{
    uint2 dim; g_InColor.GetDimensions(dim.x, dim.y);
    if (DTid.x >= dim.x || DTid.y >= dim.y) return;

    // Depth-guided 2x2 nearest (matches the RT trace shader's half-res sampling).
    int2 fpBest;
    float depth;
    if (dim.x >= g_FullDim.x) // full resolution: 1:1
    {
        fpBest = min(int2(DTid), int2(g_FullDim) - 1);
        depth  = g_InDepth.Load(int3(fpBest, 0)).x;
    }
    else // half resolution: 2x2 footprint
    {
        int2 maxc = int2(g_FullDim) - 1;
        int2 base = int2(DTid * 2);
        int2 p00 = min(base + int2(0, 0), maxc);
        int2 p10 = min(base + int2(1, 0), maxc);
        int2 p01 = min(base + int2(0, 1), maxc);
        int2 p11 = min(base + int2(1, 1), maxc);
        fpBest = p00; depth = g_InDepth.Load(int3(p00, 0)).x;
        float d = g_InDepth.Load(int3(p10, 0)).x; if (d < depth) { depth = d; fpBest = p10; }
        d = g_InDepth.Load(int3(p01, 0)).x; if (d < depth) { depth = d; fpBest = p01; }
        d = g_InDepth.Load(int3(p11, 0)).x; if (d < depth) { depth = d; fpBest = p11; }
    }

    float2 uv = (float2(DTid) + 0.5) / float2(dim);
    if (depth >= 1.0)
    {
        // Background: mark as outside the denoising range.
        g_OutMv[DTid]        = float2(0, 0);
        g_OutViewZ[DTid]     = g_DenoisingRange * 2.0;
        g_OutNormalRgh[DTid] = float4(EncodeNormalRoughness101010(float3(0, 0, 1), 1.0), 0.0);
        g_OutSpecRad[DTid]   = float4(0, 0, 0, 0);
        g_OutDiffRad[DTid]   = float4(0, 0, 0, 0);
        return;
    }

    float4 c  = g_InColor.Load(int3(DTid, 0));
    float4 nm = g_InNormal.Load(int3(DTid, 0));
    float3 N  = normalize(nm.xyz);
    float roughness = nm.a;

    float3 wpos  = Unproject(uv, depth);
    float4 vp    = mul(g_View, float4(wpos, 1.0));
    float  viewZ = abs(vp.z); // positive view-space depth

    // Hit distance proxy: the pixel's own view depth (we don't track ray hit
    // distances for the reflection/shadow signals).
    float normHitDist = GetNormHitDist(viewZ, viewZ, roughness);

    // Screen-space motion: previous UV - current UV (camera reprojection only).
    float4 prevClip = mul(g_PrevViewProj, float4(wpos, 1.0));
    float2 prevUv = float2(prevClip.x * 0.5 + 0.5, 0.5 - prevClip.y * 0.5) / max(prevClip.w, 1e-6);
    float2 mv = prevClip.w > 0.0 ? (prevUv - uv) : float2(0, 0);

    g_OutMv[DTid]        = mv;
    g_OutViewZ[DTid]     = viewZ;
    g_OutNormalRgh[DTid] = float4(EncodeNormalRoughness101010(N, roughness), 0.0);
    g_OutSpecRad[DTid]   = float4(LinearToYCoCg(c.rgb), normHitDist);
    g_OutDiffRad[DTid]   = float4(LinearToYCoCg(c.aaa), normHitDist);
}
)";

// ===================================================================
// Pack compute shader: blend REBLUR OUT spec/diff (RGBA16F, YCoCg radiance)
// with the raw RT output by strength, -> compose texture (RGBA32F:
// rgb = blended reflection, a = blended diffuse).
// ===================================================================
static const char* g_PackCS = R"(
Texture2D<float4> g_InSpec : register(t0);
Texture2D<float4> g_InDiff : register(t1);
Texture2D<float4> g_InRaw  : register(t2); // RT output (RGBA32F: rgb = reflection, a = lighting)
RWTexture2D<float4> g_Out  : register(u0);
cbuffer PackCB : register(b0) {
    float g_Strength; // 0 = raw RT output, 1 = fully denoised
    float3 _pad;
};

// NRD 4.18 YCoCg decode (matches _NRD_YCoCgToLinear).
float3 YCoCgToLinear(float3 c)
{
    float t = c.x - c.z;
    float3 r;
    r.y = c.x + c.z;
    r.x = t + c.y;
    r.z = t - c.y;
    return max(r, 0.0);
}

[numthreads(8, 8, 1)]
void CSMain(uint2 DTid : SV_DispatchThreadID)
{
    uint2 dim; g_InSpec.GetDimensions(dim.x, dim.y);
    if (DTid.x >= dim.x || DTid.y >= dim.y) return;
    float4 s = g_InSpec.Load(int3(DTid, 0));
    float4 d = g_InDiff.Load(int3(DTid, 0));
    float4 raw = g_InRaw.Load(int3(DTid, 0));
    float3 denSpec = YCoCgToLinear(s.rgb);
    float3 denDiff = YCoCgToLinear(d.rgb);
    float3 outSpec = lerp(raw.rgb, denSpec, saturate(g_Strength));
    float  outDiff = lerp(raw.a, denDiff.r, saturate(g_Strength));
    g_Out[DTid] = float4(outSpec, outDiff);
}
)";

// ===================================================================
// Implementation
// ===================================================================

static D::TEXTURE_FORMAT toDiligentFormat(nrd::Format f) {
	switch (f) {
	case nrd::Format::R8_UNORM: return D::TEX_FORMAT_R8_UNORM;
	case nrd::Format::R8_SNORM: return D::TEX_FORMAT_R8_SNORM;
	case nrd::Format::R8_UINT: return D::TEX_FORMAT_R8_UINT;
	case nrd::Format::R8_SINT: return D::TEX_FORMAT_R8_SINT;
	case nrd::Format::RG8_UNORM: return D::TEX_FORMAT_RG8_UNORM;
	case nrd::Format::RG8_SNORM: return D::TEX_FORMAT_RG8_SNORM;
	case nrd::Format::RG8_UINT: return D::TEX_FORMAT_RG8_UINT;
	case nrd::Format::RG8_SINT: return D::TEX_FORMAT_RG8_SINT;
	case nrd::Format::RGBA8_UNORM: return D::TEX_FORMAT_RGBA8_UNORM;
	case nrd::Format::RGBA8_SNORM: return D::TEX_FORMAT_RGBA8_SNORM;
	case nrd::Format::RGBA8_UINT: return D::TEX_FORMAT_RGBA8_UINT;
	case nrd::Format::RGBA8_SINT: return D::TEX_FORMAT_RGBA8_SINT;
	case nrd::Format::RGBA8_SRGB: return D::TEX_FORMAT_RGBA8_UNORM_SRGB;
	case nrd::Format::R16_UNORM: return D::TEX_FORMAT_R16_UNORM;
	case nrd::Format::R16_SNORM: return D::TEX_FORMAT_R16_SNORM;
	case nrd::Format::R16_UINT: return D::TEX_FORMAT_R16_UINT;
	case nrd::Format::R16_SINT: return D::TEX_FORMAT_R16_SINT;
	case nrd::Format::R16_SFLOAT: return D::TEX_FORMAT_R16_FLOAT;
	case nrd::Format::RG16_UNORM: return D::TEX_FORMAT_RG16_UNORM;
	case nrd::Format::RG16_SNORM: return D::TEX_FORMAT_RG16_SNORM;
	case nrd::Format::RG16_UINT: return D::TEX_FORMAT_RG16_UINT;
	case nrd::Format::RG16_SINT: return D::TEX_FORMAT_RG16_SINT;
	case nrd::Format::RG16_SFLOAT: return D::TEX_FORMAT_RG16_FLOAT;
	case nrd::Format::RGBA16_UNORM: return D::TEX_FORMAT_RGBA16_UNORM;
	case nrd::Format::RGBA16_SNORM: return D::TEX_FORMAT_RGBA16_SNORM;
	case nrd::Format::RGBA16_UINT: return D::TEX_FORMAT_RGBA16_UINT;
	case nrd::Format::RGBA16_SINT: return D::TEX_FORMAT_RGBA16_SINT;
	case nrd::Format::RGBA16_SFLOAT: return D::TEX_FORMAT_RGBA16_FLOAT;
	case nrd::Format::R32_UINT: return D::TEX_FORMAT_R32_UINT;
	case nrd::Format::R32_SINT: return D::TEX_FORMAT_R32_SINT;
	case nrd::Format::R32_SFLOAT: return D::TEX_FORMAT_R32_FLOAT;
	case nrd::Format::RG32_UINT: return D::TEX_FORMAT_RG32_UINT;
	case nrd::Format::RG32_SINT: return D::TEX_FORMAT_RG32_SINT;
	case nrd::Format::RG32_SFLOAT: return D::TEX_FORMAT_RG32_FLOAT;
	case nrd::Format::RGBA32_UINT: return D::TEX_FORMAT_RGBA32_UINT;
	case nrd::Format::RGBA32_SINT: return D::TEX_FORMAT_RGBA32_SINT;
	case nrd::Format::RGBA32_SFLOAT: return D::TEX_FORMAT_RGBA32_FLOAT;
	case nrd::Format::R10_G10_B10_A2_UNORM: return D::TEX_FORMAT_RGB10A2_UNORM;
	case nrd::Format::R10_G10_B10_A2_UINT: return D::TEX_FORMAT_RGB10A2_UINT;
	case nrd::Format::R11_G11_B10_UFLOAT: return D::TEX_FORMAT_R11G11B10_FLOAT;
	case nrd::Format::R9_G9_B9_E5_UFLOAT: return D::TEX_FORMAT_RGB9E5_SHAREDEXP;
	default:
		EError("Denoising: unmapped NRD format index {}", (int)f);
		return D::TEX_FORMAT_UNKNOWN;
	}
}

struct DenoisingSubsystem::Impl {
	RenderSubsystem* renderer = nullptr;
	bool ok = false;

	D::IRenderDevice* device() const { return renderer ? static_cast<D::IRenderDevice*>(renderer->getDevice()) : nullptr; }
	D::IDeviceContext* ctx() const { return renderer ? static_cast<D::IDeviceContext*>(renderer->getContext()) : nullptr; }

	// NRD
	nrd::Instance* instance = nullptr;
	UInt32 frameIndex = 0;
	bool  firstFrame = true;
	Mat4  prevView = Mat4(1.0f), prevProj = Mat4(1.0f);
	UInt32 w = 0, h = 0;
	nrd::ReblurSettings reblurSettings;

	// Prep pass
	D::RefCntAutoPtr<D::IPipelineState> prepPSO;
	D::RefCntAutoPtr<D::IShaderResourceBinding> prepSRB;
	D::RefCntAutoPtr<D::IBuffer> prepCB;
	D::RefCntAutoPtr<D::ITexture> mvTex, viewZTex, normalTex, specRadTex, diffRadTex;
	D::RefCntAutoPtr<D::ITextureView> mvSRV, viewZSRV, normalSRV, specRadSRV, diffRadSRV;
	D::RefCntAutoPtr<D::ITextureView> mvUAV, viewZUAV, normalUAV, specRadUAV, diffRadUAV;

	// Denoise strength (0 = raw RT output, 1 = fully denoised).
	float strength = 1.0f;
	D::RefCntAutoPtr<D::IBuffer> packCB;

	// NRD pools + pipelines
	Vector<D::RefCntAutoPtr<D::ITexture>> permanentPool, transientPool;
	Vector<D::RefCntAutoPtr<D::ITextureView>> permanentSRV, permanentUAV;
	Vector<D::RefCntAutoPtr<D::ITextureView>> transientSRV, transientUAV;
	Vector<D::RefCntAutoPtr<D::IPipelineState>> nrdPSOs;
	Vector<D::RefCntAutoPtr<D::IShaderResourceBinding>> nrdSRBs;
	Vector<const char*> nrdCBNames;        ///< Per-pipeline constant buffer name (from shader reflection).
	Vector<Vector<const char*>> nrdResNames; ///< Per-pipeline resource names, dispatch-resources order.
	Vector<bool> nrdHasSamplers;           ///< Whether the pipeline shader declares NRD samplers.
	D::RefCntAutoPtr<D::IBuffer> nrdCB;

	// REBLUR outputs + pack pass
	D::RefCntAutoPtr<D::ITexture> outSpecTex, outDiffTex, denoisedTex;
	D::RefCntAutoPtr<D::ITextureView> outSpecUAV, outDiffUAV, outSpecSRV, outDiffSRV, denoisedSRV;
	D::RefCntAutoPtr<D::IPipelineState> packPSO;
	D::RefCntAutoPtr<D::IShaderResourceBinding> packSRB;

	~Impl() { releaseNRD(); }

	void releaseNRD() {
		if (instance) { nrd::DestroyInstance(*instance); instance = nullptr; }
		frameIndex = 0; firstFrame = true; w = h = 0; strength = 1.0f;
		prepPSO.Release(); prepSRB.Release(); prepCB.Release();
		packCB.Release();
		mvTex.Release(); viewZTex.Release(); normalTex.Release(); specRadTex.Release(); diffRadTex.Release();
		mvSRV.Release(); viewZSRV.Release(); normalSRV.Release(); specRadSRV.Release(); diffRadSRV.Release();
		mvUAV.Release(); viewZUAV.Release(); normalUAV.Release(); specRadUAV.Release(); diffRadUAV.Release();
		permanentPool.clear(); permanentSRV.clear(); permanentUAV.clear();
		transientPool.clear(); transientSRV.clear(); transientUAV.clear();
		nrdPSOs.clear(); nrdSRBs.clear(); nrdCBNames.clear(); nrdResNames.clear(); nrdHasSamplers.clear(); nrdCB.Release();
		outSpecTex.Release(); outDiffTex.Release(); denoisedTex.Release();
		outSpecUAV.Release(); outDiffUAV.Release(); outSpecSRV.Release(); outDiffSRV.Release(); denoisedSRV.Release();
		packPSO.Release(); packSRB.Release();
	}

	bool createTex(D::RefCntAutoPtr<D::ITexture>& t, D::RefCntAutoPtr<D::ITextureView>& srv,
		D::RefCntAutoPtr<D::ITextureView>& uav, UInt32 ww, UInt32 hh, D::TEXTURE_FORMAT fmt, const char* name) {
		D::TextureDesc td; td.Name = name; td.Type = D::RESOURCE_DIM_TEX_2D;
		td.Width = ww; td.Height = hh; td.Format = fmt;
		td.BindFlags = D::BIND_SHADER_RESOURCE | D::BIND_UNORDERED_ACCESS; td.Usage = D::USAGE_DEFAULT;
		device()->CreateTexture(td, nullptr, &t);
		if (!t) return false;
		srv = t->GetDefaultView(D::TEXTURE_VIEW_SHADER_RESOURCE);
		uav = t->GetDefaultView(D::TEXTURE_VIEW_UNORDERED_ACCESS);
		return srv && uav;
	}

	bool createComputePSO(D::RefCntAutoPtr<D::IPipelineState>& pso, D::RefCntAutoPtr<D::IShaderResourceBinding>& srb,
		const char* name, const char* entry, const void* bytecode, size_t bytecodeSize, bool withSamplers) {
		D::ShaderCreateInfo ci;
		ci.Desc.ShaderType = D::SHADER_TYPE_COMPUTE;
		ci.Desc.Name = name;
		ci.EntryPoint = entry;
		ci.SourceLanguage = D::SHADER_SOURCE_LANGUAGE_DEFAULT; // precompiled bytecode
		ci.ByteCode = bytecode;
		ci.ByteCodeSize = (D::Uint32)bytecodeSize;
		D::RefCntAutoPtr<D::IShader> cs;
		device()->CreateShader(ci, &cs);
		if (!cs || cs->GetStatus() != D::SHADER_STATUS_READY) { EError("Denoising: failed to create compute shader '{}'", name); return false; }
		D::ComputePipelineStateCreateInfo cci;
		cci.PSODesc.Name = name;
		cci.PSODesc.PipelineType = D::PIPELINE_TYPE_COMPUTE;
		cci.PSODesc.ResourceLayout.DefaultVariableType = D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
		// NRD shaders sample with gNearestClamp (s0) / gLinearClamp (s1) in space 1.
		D::ImmutableSamplerDesc samplers[2] = {};
		if (withSamplers) {
			samplers[0].ShaderStages = D::SHADER_TYPE_COMPUTE;
			samplers[0].SamplerOrTextureName = "gNearestClamp";
			samplers[0].Desc.MinFilter = D::FILTER_TYPE_POINT;
			samplers[0].Desc.MagFilter = D::FILTER_TYPE_POINT;
			samplers[0].Desc.MipFilter = D::FILTER_TYPE_POINT;
			samplers[0].Desc.AddressU = D::TEXTURE_ADDRESS_CLAMP;
			samplers[0].Desc.AddressV = D::TEXTURE_ADDRESS_CLAMP;
			samplers[0].Desc.AddressW = D::TEXTURE_ADDRESS_CLAMP;
			samplers[1].ShaderStages = D::SHADER_TYPE_COMPUTE;
			samplers[1].SamplerOrTextureName = "gLinearClamp";
			samplers[1].Desc.MinFilter = D::FILTER_TYPE_LINEAR;
			samplers[1].Desc.MagFilter = D::FILTER_TYPE_LINEAR;
			samplers[1].Desc.MipFilter = D::FILTER_TYPE_LINEAR;
			samplers[1].Desc.AddressU = D::TEXTURE_ADDRESS_CLAMP;
			samplers[1].Desc.AddressV = D::TEXTURE_ADDRESS_CLAMP;
			samplers[1].Desc.AddressW = D::TEXTURE_ADDRESS_CLAMP;
			cci.PSODesc.ResourceLayout.NumImmutableSamplers = 2;
			cci.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
		}
		cci.pCS = cs;
		device()->CreateComputePipelineState(cci, &pso);
		if (!pso) { EError("Denoising: failed to create compute PSO '{}'", name); return false; }
		pso->CreateShaderResourceBinding(&srb, true);
		return srb != nullptr;
	}
};

DenoisingSubsystem::DenoisingSubsystem() : Subsystem("Denoising"), m_impl(std::make_unique<Impl>()) {}
DenoisingSubsystem::~DenoisingSubsystem() = default;

void DenoisingSubsystem::attachToRenderer(RenderSubsystem* r) { m_impl->renderer = r; }
void DenoisingSubsystem::setStrength(F32 strength) { m_impl->strength = glm::clamp(strength, 0.0f, 1.0f); }
bool DenoisingSubsystem::isReady() const { return m_impl->ok; }

Result<void, CoreError> DenoisingSubsystem::onInitialize() {
	m_impl->ok = true;
	EInfo("Denoising subsystem ready (NRD REBLUR, lazy init on first frame).");
	return {};
}

void DenoisingSubsystem::onShutdown() {
	m_impl->releaseNRD();
	m_impl->ok = false;
}

// ===================================================================
// Lazy NRD init (needs the RT resolution).
// ===================================================================
Result<void, RenderError> DenoisingSubsystem::initNRD(Impl& p, UInt32 width, UInt32 height) {
	p.releaseNRD();
	p.w = width; p.h = height;

	// 1. Instance with the REBLUR diffuse+specular denoiser.
	nrd::DenoiserDesc ddesc{};
	ddesc.identifier = 0;
	ddesc.denoiser = nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;
	nrd::InstanceCreationDesc icd{};
	icd.denoisers = &ddesc;
	icd.denoisersNum = 1;
	if (nrd::CreateInstance(icd, p.instance) != nrd::Result::SUCCESS || !p.instance) {
		EError("Denoising: nrd::CreateInstance failed"); return RenderError::OperationFailed;
	}
	const nrd::LibraryDesc* libDesc = nrd::GetLibraryDesc();
	EInfo("Denoising: NRD {} {}.{} normalEncoding={} roughnessEncoding={}", libDesc->versionMajor, libDesc->versionMinor, libDesc->versionBuild,
		(int)libDesc->normalEncoding, (int)libDesc->roughnessEncoding);
	const nrd::InstanceDesc* instDesc = nrd::GetInstanceDesc(*p.instance);
	if (!instDesc) { EError("Denoising: GetInstanceDesc failed"); p.releaseNRD(); return RenderError::OperationFailed; }

	// 2. REBLUR settings (real-time friendly).
	p.reblurSettings = nrd::ReblurSettings{};
	p.reblurSettings.maxAccumulatedFrameNum = (uint32_t)(nrd::REBLUR_DEFAULT_ACCUMULATION_TIME * 60.0f + 0.5f);
	p.reblurSettings.maxFastAccumulatedFrameNum = 6;
	p.reblurSettings.historyFixFrameNum = 3;
	p.reblurSettings.enableAntiFirefly = true;
	p.reblurSettings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::OFF;
	if (nrd::SetDenoiserSettings(*p.instance, 0, &p.reblurSettings) != nrd::Result::SUCCESS) {
		EError("Denoising: SetDenoiserSettings failed"); p.releaseNRD(); return RenderError::OperationFailed;
	}

	// 3. Permanent + transient pools.
	auto createPool = [&](const nrd::TextureDesc* descs, uint32_t num, Vector<D::RefCntAutoPtr<D::ITexture>>& pool,
		Vector<D::RefCntAutoPtr<D::ITextureView>>& srvs, Vector<D::RefCntAutoPtr<D::ITextureView>>& uavs) {
		for (uint32_t i = 0; i < num; ++i) {
			D::TEXTURE_FORMAT fmt = toDiligentFormat(descs[i].format);
			UInt32 pw = std::max<UInt32>(1, width / std::max<uint16_t>(1, descs[i].downsampleFactor));
			UInt32 ph = std::max<UInt32>(1, height / std::max<uint16_t>(1, descs[i].downsampleFactor));
			D::RefCntAutoPtr<D::ITexture> tex; D::RefCntAutoPtr<D::ITextureView> srv, uav;
			D::TextureDesc td; td.Name = "NRD Pool"; td.Type = D::RESOURCE_DIM_TEX_2D;
			td.Width = pw; td.Height = ph; td.Format = fmt;
			td.BindFlags = D::BIND_SHADER_RESOURCE | D::BIND_UNORDERED_ACCESS; td.Usage = D::USAGE_DEFAULT;
			p.device()->CreateTexture(td, nullptr, &tex);
			if (!tex) { EError("Denoising: failed to create NRD pool texture"); return false; }
			srv = tex->GetDefaultView(D::TEXTURE_VIEW_SHADER_RESOURCE);
			uav = tex->GetDefaultView(D::TEXTURE_VIEW_UNORDERED_ACCESS);
			pool.push_back(std::move(tex)); srvs.push_back(std::move(srv)); uavs.push_back(std::move(uav));
		}
		return true;
	};
	if (!createPool(instDesc->permanentPool, instDesc->permanentPoolSize, p.permanentPool, p.permanentSRV, p.permanentUAV) ||
		!createPool(instDesc->transientPool, instDesc->transientPoolSize, p.transientPool, p.transientSRV, p.transientUAV)) {
		p.releaseNRD(); return RenderError::OperationFailed;
	}

	// 4. Prep/pack passes (compiled from source with DXC, like the RT shaders).
	auto* dev = p.device();
	{
		D::ShaderCreateInfo ci;
		ci.Desc.ShaderType = D::SHADER_TYPE_COMPUTE; ci.Desc.Name = "Denoise Prep CS";
		ci.EntryPoint = "CSMain";
		ci.SourceLanguage = D::SHADER_SOURCE_LANGUAGE_HLSL;
		ci.ShaderCompiler = D::SHADER_COMPILER_DXC;
		ci.HLSLVersion = { 6, 5 };
		ci.Source = g_PrepCS; ci.SourceLength = (D::Uint32)strlen(g_PrepCS);
		D::RefCntAutoPtr<D::IShader> cs;
		dev->CreateShader(ci, &cs);
		if (!cs || cs->GetStatus() != D::SHADER_STATUS_READY) { EError("Denoising: failed to compile prep shader"); p.releaseNRD(); return RenderError::ShaderCompilationFailed; }
		D::ComputePipelineStateCreateInfo cci;
		cci.PSODesc.Name = "Denoise Prep PSO"; cci.PSODesc.PipelineType = D::PIPELINE_TYPE_COMPUTE;
		cci.PSODesc.ResourceLayout.DefaultVariableType = D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
		cci.pCS = cs;
		dev->CreateComputePipelineState(cci, &p.prepPSO);
		if (!p.prepPSO) { EError("Denoising: failed to create prep PSO"); p.releaseNRD(); return RenderError::PipelineStateCreationFailed; }
		p.prepPSO->CreateShaderResourceBinding(&p.prepSRB, true);
	}
	{
		D::ShaderCreateInfo ci;
		ci.Desc.ShaderType = D::SHADER_TYPE_COMPUTE; ci.Desc.Name = "Denoise Pack CS";
		ci.EntryPoint = "CSMain";
		ci.SourceLanguage = D::SHADER_SOURCE_LANGUAGE_HLSL;
		ci.ShaderCompiler = D::SHADER_COMPILER_DXC;
		ci.HLSLVersion = { 6, 5 };
		ci.Source = g_PackCS; ci.SourceLength = (D::Uint32)strlen(g_PackCS);
		D::RefCntAutoPtr<D::IShader> cs;
		dev->CreateShader(ci, &cs);
		if (!cs || cs->GetStatus() != D::SHADER_STATUS_READY) { EError("Denoising: failed to compile pack shader"); p.releaseNRD(); return RenderError::ShaderCompilationFailed; }
		D::ComputePipelineStateCreateInfo cci;
		cci.PSODesc.Name = "Denoise Pack PSO"; cci.PSODesc.PipelineType = D::PIPELINE_TYPE_COMPUTE;
		cci.PSODesc.ResourceLayout.DefaultVariableType = D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
		cci.pCS = cs;
		dev->CreateComputePipelineState(cci, &p.packPSO);
		if (!p.packPSO) { EError("Denoising: failed to create pack PSO"); p.releaseNRD(); return RenderError::PipelineStateCreationFailed; }
		p.packPSO->CreateShaderResourceBinding(&p.packSRB, true);
	}

	// Prep CB (232-byte payload, keep the buffer 256-aligned).
	{
		D::BufferDesc bd; bd.Name = "Denoise Prep CB"; bd.Size = 256;
		bd.BindFlags = D::BIND_UNIFORM_BUFFER; bd.Usage = D::USAGE_DYNAMIC; bd.CPUAccessFlags = D::CPU_ACCESS_WRITE;
		dev->CreateBuffer(bd, nullptr, &p.prepCB);
		if (p.prepSRB) { if (auto* v = p.prepSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "PrepCB")) v->Set(p.prepCB); }
	}

	// Pack CB (blend strength).
	{
		D::BufferDesc bd; bd.Name = "Denoise Pack CB"; bd.Size = 256;
		bd.BindFlags = D::BIND_UNIFORM_BUFFER; bd.Usage = D::USAGE_DYNAMIC; bd.CPUAccessFlags = D::CPU_ACCESS_WRITE;
		dev->CreateBuffer(bd, nullptr, &p.packCB);
		if (p.packSRB) { if (auto* v = p.packSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "PackCB")) v->Set(p.packCB); }
	}

	// Prep intermediate textures (RT resolution).
	if (!p.createTex(p.mvTex, p.mvSRV, p.mvUAV, width, height, D::TEX_FORMAT_RG16_FLOAT, "NRD MV") ||
		!p.createTex(p.viewZTex, p.viewZSRV, p.viewZUAV, width, height, D::TEX_FORMAT_R32_FLOAT, "NRD ViewZ") ||
		!p.createTex(p.normalTex, p.normalSRV, p.normalUAV, width, height, D::TEX_FORMAT_RGB10A2_UNORM, "NRD NormalRgh") ||
		!p.createTex(p.specRadTex, p.specRadSRV, p.specRadUAV, width, height, D::TEX_FORMAT_RGBA16_FLOAT, "NRD SpecRad") ||
		!p.createTex(p.diffRadTex, p.diffRadSRV, p.diffRadUAV, width, height, D::TEX_FORMAT_RGBA16_FLOAT, "NRD DiffRad")) {
		p.releaseNRD(); return RenderError::TextureCreationFailed;
	}

	// REBLUR outputs + compose texture.
	if (!p.createTex(p.outSpecTex, p.outSpecSRV, p.outSpecUAV, width, height, D::TEX_FORMAT_RGBA16_FLOAT, "NRD OutSpec") ||
		!p.createTex(p.outDiffTex, p.outDiffSRV, p.outDiffUAV, width, height, D::TEX_FORMAT_RGBA16_FLOAT, "NRD OutDiff")) {
		p.releaseNRD(); return RenderError::TextureCreationFailed;
	}
	{
		D::TextureDesc td; td.Name = "NRD Denoised"; td.Type = D::RESOURCE_DIM_TEX_2D;
		td.Width = width; td.Height = height; td.Format = D::TEX_FORMAT_RGBA32_FLOAT;
		td.BindFlags = D::BIND_SHADER_RESOURCE | D::BIND_UNORDERED_ACCESS; td.Usage = D::USAGE_DEFAULT;
		dev->CreateTexture(td, nullptr, &p.denoisedTex);
		if (!p.denoisedTex) { p.releaseNRD(); return RenderError::TextureCreationFailed; }
		p.denoisedSRV = p.denoisedTex->GetDefaultView(D::TEXTURE_VIEW_SHADER_RESOURCE);
	}

	// NRD constant buffer (per-dispatch constants from the library).
	{
		D::BufferDesc bd; bd.Name = "NRD CB"; bd.Size = std::max<UInt64>(256, instDesc->constantBufferMaxDataSize);
		bd.BindFlags = D::BIND_UNIFORM_BUFFER; bd.Usage = D::USAGE_DYNAMIC; bd.CPUAccessFlags = D::CPU_ACCESS_WRITE;
		dev->CreateBuffer(bd, nullptr, &p.nrdCB);
		if (!p.nrdCB) { p.releaseNRD(); return RenderError::BufferCreationFailed; }
	}

	// 5. NRD dispatch pipelines (one PSO + SRB per pipeline). The bytecode
	// shipped inside NRD.dll is compiled with --stripReflection (no resource
	// names), which Diligent cannot build PSOs from (it binds by name). We
	// recompiled the NRD 4.18 shaders ourselves WITHOUT stripping (see
	// Tools/nrd_compile_shaders.ps1) and use those blobs instead.
	size_t blobCount = 0;
	const auto* blobs = NRDShaders::GetShaderBlobs(blobCount);
	size_t resCount = 0;
	const auto* resTable = NRDShaders::GetShaderResources(resCount);
	for (uint32_t i = 0; i < instDesc->pipelinesNum; ++i) {
		const auto& sd = instDesc->pipelines[i].computeShaderDXIL;
		const char* identifier = instDesc->pipelines[i].shaderIdentifier;
		const void* bc = sd.bytecode; size_t bcSize = sd.size;
		const NRDShaderBlob* blob = nullptr;
		for (size_t b = 0; b < blobCount; ++b) {
			if (strcmp(blobs[b].identifier, identifier) == 0) { blob = &blobs[b]; break; }
		}
		if (blob) { bc = blob->data; bcSize = blob->size; }
		else { EError("Denoising: no compiled shader for pipeline '{}' - falling back to NRD.dll bytecode (PSO creation will likely fail)", identifier); }
		D::RefCntAutoPtr<D::IPipelineState> pso;
		D::RefCntAutoPtr<D::IShaderResourceBinding> srb;
		const NRDShaderResources* res = nullptr;
		for (size_t r = 0; r < resCount; ++r) {
			if (strcmp(resTable[r].identifier, identifier) == 0) { res = &resTable[r]; break; }
		}
		if (!p.createComputePSO(pso, srb, "NRD Dispatch PSO", "main", bc, bcSize, res ? res->hasSamplers : false)) {
			p.releaseNRD(); return RenderError::PipelineStateCreationFailed;
		}
		p.nrdPSOs.push_back(std::move(pso));
		p.nrdSRBs.push_back(std::move(srb));
		// Store the CB name + ordered resource names for this pipeline.
		p.nrdCBNames.push_back(res ? res->cbName : "gConstants");
		p.nrdHasSamplers.push_back(res ? res->hasSamplers : false);
		Vector<const char*> names;
		if (res) for (size_t n = 0; n < res->namesNum; ++n) names.push_back(res->names[n]);
		p.nrdResNames.push_back(std::move(names));
	}

	const nrd::Identifier id = 0;
	const nrd::DispatchDesc* dispatchDescs = nullptr;
	uint32_t dispatchNum = 0;
	if (nrd::GetComputeDispatches(*p.instance, &id, 1, dispatchDescs, dispatchNum) != nrd::Result::SUCCESS) {
		EError("Denoising: GetComputeDispatches failed"); p.releaseNRD(); return RenderError::OperationFailed;
	}
	EInfo("Denoising: REBLUR ready at {}x{} ({} dispatches, permanent pool {}, transient pool {})",
		width, height, dispatchNum, instDesc->permanentPoolSize, instDesc->transientPoolSize);

	p.ok = true;
	p.firstFrame = true;
	return {};
}

// ===================================================================
// Per-frame denoise.
// ===================================================================
Result<void, RenderError> DenoisingSubsystem::denoise(
	void* colorSRV, void* normalSRV, void* depthSRV,
	const Mat4& view, const Mat4& proj, UInt32 width, UInt32 height) {
	auto& p = *m_impl;
	if (!p.ok) return RenderError::OperationFailed;
	if (!colorSRV || !normalSRV || !depthSRV || width == 0 || height == 0) return RenderError::InvalidArgument;
	if (p.w != width || p.h != height || !p.instance) {
		auto r = initNRD(p, width, height);
		if (r.isErr()) return r.error();
	}
	auto* ctx = p.ctx(); if (!ctx) return RenderError::NotInitialized;
	const auto& td = static_cast<D::ITextureView*>(colorSRV)->GetTexture()->GetDesc();

	// ---- Prep pass ----
	{
		Mat4 viewProj = proj * view;
		Mat4 vpInv = glm::inverse(viewProj);
		Mat4 prevVP = p.prevProj * p.prevView;
		Vec4 hitDistParams = Vec4(p.reblurSettings.hitDistanceParameters.A,
			p.reblurSettings.hitDistanceParameters.B, p.reblurSettings.hitDistanceParameters.C, 0);
		struct { Mat4 viewProjInv; Mat4 view; Mat4 prevViewProj; Vec4 hitDistParams; UInt32 rtDim[2]; UInt32 fullDim[2]; float denoisingRange; float pad; } cb;
		cb.viewProjInv = vpInv;
		cb.view = view;
		cb.prevViewProj = prevVP;
		cb.hitDistParams = hitDistParams;
		cb.rtDim[0] = width; cb.rtDim[1] = height;
		cb.fullDim[0] = td.Width; cb.fullDim[1] = td.Height;
		cb.denoisingRange = 500.0f;
		cb.pad = 0;
		void* m = nullptr;
		ctx->MapBuffer(p.prepCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m);
		if (m) { memcpy(m, &cb, sizeof(cb)); ctx->UnmapBuffer(p.prepCB, D::MAP_WRITE); }
	}
	const auto setVar = [&](D::IShaderResourceBinding* srb, D::SHADER_TYPE stage, const char* name, D::IDeviceObject* obj) {
		if (auto* v = srb->GetVariableByName(stage, name)) v->Set(obj, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	};
	{
		setVar(p.prepSRB, D::SHADER_TYPE_COMPUTE, "g_InColor", static_cast<D::ITextureView*>(colorSRV));
		setVar(p.prepSRB, D::SHADER_TYPE_COMPUTE, "g_InNormal", static_cast<D::ITextureView*>(normalSRV));
		setVar(p.prepSRB, D::SHADER_TYPE_COMPUTE, "g_InDepth", static_cast<D::ITextureView*>(depthSRV));
		setVar(p.prepSRB, D::SHADER_TYPE_COMPUTE, "g_OutMv", p.mvUAV);
		setVar(p.prepSRB, D::SHADER_TYPE_COMPUTE, "g_OutViewZ", p.viewZUAV);
		setVar(p.prepSRB, D::SHADER_TYPE_COMPUTE, "g_OutNormalRgh", p.normalUAV);
		setVar(p.prepSRB, D::SHADER_TYPE_COMPUTE, "g_OutSpecRad", p.specRadUAV);
		setVar(p.prepSRB, D::SHADER_TYPE_COMPUTE, "g_OutDiffRad", p.diffRadUAV);
		D::DispatchComputeAttribs da;
		da.ThreadGroupCountX = (width + 7) / 8;
		da.ThreadGroupCountY = (height + 7) / 8;
		ctx->SetPipelineState(p.prepPSO);
		ctx->CommitShaderResources(p.prepSRB, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		ctx->DispatchCompute(da);
	}

	// ---- NRD: common settings + dispatches ----
	{
		nrd::CommonSettings cs{};
		memcpy(cs.viewToClipMatrix, &proj, sizeof(cs.viewToClipMatrix));
		memcpy(cs.viewToClipMatrixPrev, &p.prevProj, sizeof(cs.viewToClipMatrixPrev));
		memcpy(cs.worldToViewMatrix, &view, sizeof(cs.worldToViewMatrix));
		memcpy(cs.worldToViewMatrixPrev, &p.prevView, sizeof(cs.worldToViewMatrixPrev));
		cs.resourceSize[0] = (uint16_t)width; cs.resourceSize[1] = (uint16_t)height;
		cs.resourceSizePrev[0] = cs.resourceSize[0]; cs.resourceSizePrev[1] = cs.resourceSize[1];
		cs.rectSize[0] = (uint16_t)width; cs.rectSize[1] = (uint16_t)height;
		cs.rectSizePrev[0] = cs.rectSize[0]; cs.rectSizePrev[1] = cs.rectSize[1];
		cs.motionVectorScale[0] = 1.0f; cs.motionVectorScale[1] = 1.0f; cs.motionVectorScale[2] = 0.0f;
		cs.denoisingRange = 500.0f;
		cs.frameIndex = p.frameIndex;
		cs.accumulationMode = p.firstFrame ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;
		if (nrd::SetCommonSettings(*p.instance, cs) != nrd::Result::SUCCESS) {
			EError("Denoising: SetCommonSettings failed"); return RenderError::OperationFailed;
		}
	}
	const nrd::Identifier id = 0;
	const nrd::DispatchDesc* dispatchDescs = nullptr;
	uint32_t dispatchNum = 0;
	if (nrd::GetComputeDispatches(*p.instance, &id, 1, dispatchDescs, dispatchNum) != nrd::Result::SUCCESS) {
		EError("Denoising: GetComputeDispatches failed"); return RenderError::OperationFailed;
	}
	for (uint32_t di = 0; di < dispatchNum; ++di) {
		const nrd::DispatchDesc& dd = dispatchDescs[di];
		if (dd.pipelineIndex >= p.nrdPSOs.size()) continue;
		auto& pso = p.nrdPSOs[dd.pipelineIndex];
		auto& srb = p.nrdSRBs[dd.pipelineIndex];
		if (!pso || !srb) continue;

		// Resolve a dispatch resource to the corresponding Diligent view.
		const auto resolve = [&](const nrd::ResourceDesc& rd, bool uav) -> D::IDeviceObject* {
			using RT = nrd::ResourceType;
			auto pick = [](D::RefCntAutoPtr<D::ITextureView>& srv, D::RefCntAutoPtr<D::ITextureView>& uav, bool u) -> D::IDeviceObject* {
				return (u ? uav : srv).RawPtr();
			};
			switch (rd.type) {
			case RT::IN_MV: return pick(p.mvSRV, p.mvUAV, uav);
			case RT::IN_VIEWZ: return pick(p.viewZSRV, p.viewZUAV, uav);
			case RT::IN_NORMAL_ROUGHNESS: return pick(p.normalSRV, p.normalUAV, uav);
			case RT::IN_DIFF_RADIANCE_HITDIST: return pick(p.diffRadSRV, p.diffRadUAV, uav);
			case RT::IN_SPEC_RADIANCE_HITDIST: return pick(p.specRadSRV, p.specRadUAV, uav);
			// Dummy inputs (isHistoryConfidenceAvailable/isDisocclusionThresholdMixAvailable = false)
			case RT::IN_DIFF_CONFIDENCE:
			case RT::IN_SPEC_CONFIDENCE:
			case RT::IN_DISOCCLUSION_THRESHOLD_MIX:
				return pick(p.viewZSRV, p.viewZUAV, uav);
			case RT::OUT_DIFF_RADIANCE_HITDIST: return pick(p.outDiffSRV, p.outDiffUAV, uav);
			case RT::OUT_SPEC_RADIANCE_HITDIST: return pick(p.outSpecSRV, p.outSpecUAV, uav);
			case RT::PERMANENT_POOL:
				if (rd.indexInPool < p.permanentPool.size()) return pick(p.permanentSRV[rd.indexInPool], p.permanentUAV[rd.indexInPool], uav);
				return nullptr;
			case RT::TRANSIENT_POOL:
				if (rd.indexInPool < p.transientPool.size()) return pick(p.transientSRV[rd.indexInPool], p.transientUAV[rd.indexInPool], uav);
				return nullptr;
			default: return nullptr;
			}
		};

		// Bind resources by name (names come from the reflection of the shader
		// we compiled - dispatch resource order == shader register order).
		const char* const* names = nullptr;
		size_t namesNum = 0;
		if (dd.pipelineIndex < p.nrdResNames.size()) {
			names = p.nrdResNames[dd.pipelineIndex].data();
			namesNum = p.nrdResNames[dd.pipelineIndex].size();
		}
		for (uint32_t ri = 0; ri < dd.resourcesNum && ri < namesNum; ++ri) {
			const nrd::ResourceDesc& rd = dd.resources[ri];
			const bool uav = (rd.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE);
			D::IDeviceObject* obj = resolve(rd, uav);
			if (!obj) { static bool warned = false; if (!warned) { EError("Denoising: cannot resolve dispatch resource type {}", (int)rd.type); warned = true; } continue; }
			auto* v = srb->GetVariableByName(D::SHADER_TYPE_COMPUTE, names[ri]);
			if (!v) { static bool warned2 = false; if (!warned2) { EError("Denoising: NRD shader resource '{}' not found", names[ri]); warned2 = true; } continue; }
			v->Set(obj, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
		}
		// Constants (upload the per-dispatch constant buffer from the library).
		if (dd.constantBufferDataSize > 0 && dd.constantBufferData && p.nrdCB) {
			void* m = nullptr;
			ctx->MapBuffer(p.nrdCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m);
			if (m) { memcpy(m, dd.constantBufferData, dd.constantBufferDataSize); ctx->UnmapBuffer(p.nrdCB, D::MAP_WRITE); }
			const char* cbName = (dd.pipelineIndex < p.nrdCBNames.size()) ? p.nrdCBNames[dd.pipelineIndex] : "gConstants";
			if (auto* v = srb->GetVariableByName(D::SHADER_TYPE_COMPUTE, cbName)) v->Set(p.nrdCB, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
		}
		ctx->SetPipelineState(pso);
		ctx->CommitShaderResources(srb, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		D::DispatchComputeAttribs da;
		da.ThreadGroupCountX = std::max<UInt32>(1, dd.gridWidth);
		da.ThreadGroupCountY = std::max<UInt32>(1, dd.gridHeight);
		ctx->DispatchCompute(da);
	}

	// ---- Pack pass: blend REBLUR outputs with the raw RT output -> compose texture ----
	{
		struct { float strength; float pad[3]; } cb;
		cb.strength = p.strength; cb.pad[0] = cb.pad[1] = cb.pad[2] = 0.0f;
		void* m = nullptr;
		ctx->MapBuffer(p.packCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m);
		if (m) { memcpy(m, &cb, sizeof(cb)); ctx->UnmapBuffer(p.packCB, D::MAP_WRITE); }
	}
	setVar(p.packSRB, D::SHADER_TYPE_COMPUTE, "g_InSpec", p.outSpecSRV);
	setVar(p.packSRB, D::SHADER_TYPE_COMPUTE, "g_InDiff", p.outDiffSRV);
	setVar(p.packSRB, D::SHADER_TYPE_COMPUTE, "g_InRaw", static_cast<D::ITextureView*>(colorSRV));
	if (auto* v = p.packSRB->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_Out")) v->Set(p.denoisedTex->GetDefaultView(D::TEXTURE_VIEW_UNORDERED_ACCESS), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	D::DispatchComputeAttribs da;
	da.ThreadGroupCountX = (width + 7) / 8;
	da.ThreadGroupCountY = (height + 7) / 8;
	ctx->SetPipelineState(p.packPSO);
	ctx->CommitShaderResources(p.packSRB, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	ctx->DispatchCompute(da);

	p.prevView = view;
	p.prevProj = proj;
	p.frameIndex++;
	p.firstFrame = false;
	return {};
}

void* DenoisingSubsystem::getDenoisedSRV() const {
	auto& p = *m_impl;
	return p.ok && p.denoisedSRV ? p.denoisedSRV.RawPtr() : nullptr;
}

EE_NAMESPACE_RENDERING_END
