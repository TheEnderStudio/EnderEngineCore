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
#include <DiligentCore/Common/interface/RefCntAutoPtr.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace D = Diligent;

EE_NAMESPACE_RENDERING_BEGIN

namespace {

// ===================================================================
// Temporal accumulation for the volumetric result, shared by both backends.
//
// One pass over the full-resolution output: reconstruct the world position from the scene
// depth, project it with the previous frame's view-projection, clamp the reprojected history
// to the current 3x3 neighbourhood (which is what kills ghosting from moving objects, since
// there are no motion vectors here) and blend. The result is a running average, so a marching
// artefact - banding from a low step count, the quantisation of the froxel grid - averages
// out over a few frames even while the camera moves.
//
// Same depth handling as the backends: D3D12 has no "Texture2D or Texture2DMS", so the depth
// declaration is prepended and two variants are built.
// ===================================================================

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

static const char* g_ResolveBody = R"(
Texture2D<float4>             g_Current    : register(t1);
Texture2D<float4>             g_History    : register(t2);
SamplerState                  g_Sampler    : register(s0);
RWTexture2D<float4>           g_Out        : register(u0);


cbuffer ResolveCB : register(b0)
{
    float4x4 g_PrevViewProj;
    float4x4 g_ViewProjInv;
    float4   g_Params;   // x = history weight, y = relative depth reject, z = reset, w unused
};

float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

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
    const float4 current = g_Current.Load(int3(DTid, 0));

    // No history (first frame, resize, backend switch) means the current frame is the answer.
    if (g_Params.z > 0.5 || g_Params.x <= 0.0)
    {
        g_Out[DTid] = current;

        return;
    }

    // Reproject this pixel's world position into the previous frame.
    const float4 clip = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), depth, 1.0);
    float4 worldH = mul(g_ViewProjInv, clip);
    const float3 worldPos = worldH.xyz / max(worldH.w, 1e-6);
    const float4 prevClip = mul(g_PrevViewProj, float4(worldPos, 1.0));
    const float2 ndc = prevClip.xy / max(prevClip.w, 1e-5);
    const float2 prevUV = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    const bool inBounds = prevClip.w > 0.0 && all(prevUV > 0.0) && all(prevUV < 1.0);

    // 3x3 neighbourhood of the current frame: the clamp range that stops history from
    // dragging an old silhouette across a moving object.
    float4 mn = current, mx = current;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            const int2 q = clamp(int2(DTid) + int2(dx, dy), int2(0, 0), int2(dim) - 1);
            const float4 v = g_Current.Load(int3(q, 0));
            mn = min(mn, v);
            mx = max(mx, v);
        }

    float4 history = current;
    if (inBounds) history = g_History.SampleLevel(g_Sampler, prevUV, 0);

    // Depth rejection: the reprojected position is only valid if the history was written for
    // the same surface. A large depth difference means a disocclusion.
    const int2 pdp = min(int2(prevUV * float2(depthDim)), int2(depthDim) - 1);
    const float prevDepth = LoadSceneDepth(pdp);
    const float dz = abs(prevDepth - depth) / max(depth, 1e-3);
    const bool sameSurface = dz < max(g_Params.y, 1e-4);

    // Luminance rejection keeps the noise-level differences (which are what the accumulation
    // is supposed to average) and drops only large changes.
    const float4 clamped = clamp(history, mn, mx);
    const float rej = abs(Luminance(clamped.rgb) - Luminance(current.rgb)) / max(Luminance(current.rgb), 0.01);
    const float acceptance = (inBounds && sameSurface) ? saturate(1.0 - rej) : 0.0;
    const float alpha = saturate(g_Params.x) * acceptance;

    g_Out[DTid] = lerp(current, clamped, alpha);

}
)";

struct ResolveCBData {
	Mat4 prevViewProj;
	Mat4 viewProjInv;
	Vec4 params;   // x = history weight, y = depth reject, z = reset
};

/// @brief The shared temporal accumulation stage.
class TemporalResolve final : public Detail::ITemporalResolve {
public:
	bool initialize(RenderSubsystem* m_renderer) override;
	void shutdown() override;
	void* resolve(void* currentSRV, const VolumetricInput& input, F32 weight, bool reset) override;

	bool hasHistory() const override { return m_hasHistory; }

private:
	RenderSubsystem* m_renderer = nullptr;
	D::RefCntAutoPtr<D::IPipelineState> m_pso[2];
	D::RefCntAutoPtr<D::IShaderResourceBinding> m_srb[2];
	D::RefCntAutoPtr<D::IShader> m_shader[2];
	D::RefCntAutoPtr<D::IBuffer> m_cb;
	D::RefCntAutoPtr<D::ISampler> m_sampler;
	D::RefCntAutoPtr<D::ITexture> m_historyTex[2];
	D::RefCntAutoPtr<D::ITextureView> m_historySRV[2], m_historyUAV[2];


	UInt32 m_w = 0, m_h = 0;
	UInt32 m_flip = 0;
	Mat4 m_prevViewProj = Mat4(1.0f);
	bool m_hasHistory = false;
	bool m_selfChecked = false;


	D::IRenderDevice* device() const { return m_renderer ? static_cast<D::IRenderDevice*>(m_renderer->getDevice()) : nullptr; }
	D::IDeviceContext* ctx() const { return m_renderer ? static_cast<D::IDeviceContext*>(m_renderer->getContext()) : nullptr; }
	bool ensureTargets(UInt32 w, UInt32 h);
};

bool TemporalResolve::ensureTargets(UInt32 w, UInt32 h) {
	auto* dev = device();
	if (!dev || w == 0 || h == 0) return false;
	if (m_historyTex[0] && m_w == w && m_h == h) return true;
	for (int i = 0; i < 2; ++i) {
		D::TextureDesc td;
		td.Name = "VolumetricHistory";
		td.Type = D::RESOURCE_DIM_TEX_2D;
		td.Width = w;
		td.Height = h;
		td.Format = D::TEX_FORMAT_RGBA16_FLOAT;
		td.BindFlags = D::BIND_SHADER_RESOURCE | D::BIND_UNORDERED_ACCESS;
		td.Usage = D::USAGE_DEFAULT;
		m_historyTex[i].Release(); m_historySRV[i].Release(); m_historyUAV[i].Release();
		dev->CreateTexture(td, nullptr, &m_historyTex[i]);
		if (!m_historyTex[i]) return false;
		m_historySRV[i] = m_historyTex[i]->GetDefaultView(D::TEXTURE_VIEW_SHADER_RESOURCE);
		m_historyUAV[i] = m_historyTex[i]->GetDefaultView(D::TEXTURE_VIEW_UNORDERED_ACCESS);

	}
	m_w = w; m_h = h;
	m_hasHistory = false;
	return true;
}

bool TemporalResolve::initialize(RenderSubsystem* renderer) {
	m_renderer = renderer;
	auto* dev = device();
	if (!m_renderer || !dev) return false;

	const char* const prologues[2] = { g_DepthPrologueSingle, g_DepthPrologueMsaa };
	for (int variant = 0; variant < 2; ++variant) {
		String source = String(prologues[variant]) + g_ResolveBody;
		D::ShaderCreateInfo ci;
		ci.Desc.Name = "VolumetricResolveCS";
		ci.Desc.ShaderType = D::SHADER_TYPE_COMPUTE;
		ci.EntryPoint = "main";
		ci.SourceLanguage = D::SHADER_SOURCE_LANGUAGE_HLSL;
		ci.Source = source.c_str();
		ci.SourceLength = static_cast<D::Uint32>(source.size());
		dev->CreateShader(ci, &m_shader[variant]);
		if (!m_shader[variant] || m_shader[variant]->GetStatus() != D::SHADER_STATUS_READY) {
			EError("Volumetric: temporal resolve shader failed (depth variant {})", variant);
			return false;
		}
		D::ComputePipelineStateCreateInfo pci;
		pci.PSODesc.Name = "VolumetricResolvePSO";
		pci.PSODesc.PipelineType = D::PIPELINE_TYPE_COMPUTE;
		pci.PSODesc.ResourceLayout.DefaultVariableType = D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
		pci.pCS = m_shader[variant];
		dev->CreateComputePipelineState(pci, &m_pso[variant]);
		if (!m_pso[variant]) { EError("Volumetric: temporal resolve PSO failed"); return false; }
		m_pso[variant]->CreateShaderResourceBinding(&m_srb[variant], true);
	}

	{
		D::SamplerDesc sd;
		sd.MinFilter = D::FILTER_TYPE_LINEAR;
		sd.MagFilter = D::FILTER_TYPE_LINEAR;
		sd.MipFilter = D::FILTER_TYPE_LINEAR;
		sd.AddressU = sd.AddressV = sd.AddressW = D::TEXTURE_ADDRESS_CLAMP;
		dev->CreateSampler(sd, &m_sampler);
	}

	D::BufferDesc bd;
	bd.Name = "VolResolveCB";
	bd.Size = sizeof(ResolveCBData);
	bd.BindFlags = D::BIND_UNIFORM_BUFFER;
	bd.Usage = D::USAGE_DYNAMIC;
	bd.CPUAccessFlags = D::CPU_ACCESS_WRITE;
	dev->CreateBuffer(bd, nullptr, &m_cb);
	for (int variant = 0; variant < 2; ++variant) {
		if (m_srb[variant]) {
			if (auto* v = m_srb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "ResolveCB")) v->Set(m_cb);
			if (auto* v = m_srb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_Sampler")) v->Set(m_sampler);
		}
	}
	return true;
}

void TemporalResolve::shutdown() {
	for (int i = 0; i < 2; ++i) {
		m_srb[i].Release(); m_pso[i].Release(); m_shader[i].Release();
		m_historyTex[i].Release(); m_historySRV[i].Release(); m_historyUAV[i].Release();

	}
	m_cb.Release(); m_sampler.Release();
	m_w = m_h = 0;
	m_hasHistory = false;

}

void* TemporalResolve::resolve(void* currentSRV, const VolumetricInput& input, F32 weight, bool reset) {
	auto* context = ctx();
	auto* current = static_cast<D::ITextureView*>(currentSRV);
	if (!context || !current || !ensureTargets(input.width, input.height)) return currentSRV;
	const int variant = input.depthSamples > 1 ? 1 : 0;
	if (!m_pso[variant] || !m_srb[variant]) return currentSRV;

	const UInt32 prevIdx = m_flip;
	const UInt32 outIdx = m_flip ^ 1;

	ResolveCBData cb{};
	cb.prevViewProj = m_hasHistory ? m_prevViewProj : input.viewProj;
	cb.viewProjInv = input.viewProjInv;
	cb.params = Vec4(weight, 0.02f, (reset || !m_hasHistory) ? 1.0f : 0.0f, 0.0f);

	void* mapped = nullptr;
	context->MapBuffer(m_cb, D::MAP_WRITE, D::MAP_FLAG_DISCARD, mapped);
	if (mapped) { memcpy(mapped, &cb, sizeof(cb)); context->UnmapBuffer(m_cb, D::MAP_WRITE); }

	if (auto* v = m_srb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_Depth")) v->Set(static_cast<D::ITextureView*>(input.depthSRV), D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = m_srb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_Current")) v->Set(current, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = m_srb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_History")) v->Set(m_historySRV[prevIdx], D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	if (auto* v = m_srb[variant]->GetVariableByName(D::SHADER_TYPE_COMPUTE, "g_Out")) v->Set(m_historyUAV[outIdx], D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);


	context->SetPipelineState(m_pso[variant]);
	context->CommitShaderResources(m_srb[variant], D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	D::DispatchComputeAttribs da;
	da.ThreadGroupCountX = (input.width + 7) / 8;
	da.ThreadGroupCountY = (input.height + 7) / 8;
	da.ThreadGroupCountZ = 1;
	context->DispatchCompute(da);

	if (!m_selfChecked) {
		m_selfChecked = true;
		void* shaders[] = { m_shader[variant].RawPtr() };
		checkShaderBindings("VolumetricResolvePSO", m_pso[variant].RawPtr(), m_srb[variant].RawPtr(), shaders, 1);
	}

	m_flip = outIdx;
	m_prevViewProj = input.viewProj;
	m_hasHistory = true;
	return m_historySRV[outIdx].RawPtr();
}

} // namespace

namespace Detail {

Uptr<ITemporalResolve> createTemporalResolve() {
	return std::make_unique<TemporalResolve>();
}

} // namespace Detail

EE_NAMESPACE_RENDERING_END
