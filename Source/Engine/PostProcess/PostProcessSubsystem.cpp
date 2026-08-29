#include <PostProcess/PostProcessSubsystem.hpp>
#include <Core/Log.hpp>

#include <DiligentCore/Graphics/GraphicsEngine/interface/RenderDevice.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/DeviceContext.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/SwapChain.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/PipelineState.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Texture.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Shader.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/GraphicsTypes.h>
#include <DiligentCore/Common/interface/RefCntAutoPtr.hpp>

namespace D = Diligent;

EE_NAMESPACE_POSTPROCESS_BEGIN

static const char* g_FS_VS = R"(
struct VSOut { float4 Pos : SV_POSITION; float2 UV : TEXCOORD0; };
VSOut main(uint vid : SV_VertexID) {
    VSOut o; float2 uv = float2((vid << 1) & 2, vid & 2);
    o.Pos = float4(uv * float2(2.0,-2.0) + float2(-1.0,1.0), 0.0, 1.0); o.UV = uv; return o;
}
)";

static const char* g_ToneMapPS = R"(
Texture2D g_HDR : register(t0); Texture2D g_Bloom : register(t1); SamplerState g_HDR_sampler : register(s0);
cbuffer PP : register(b0) { uint g_Mode; float g_Exp; float g_Gamma; float g_Vig; float g_Sat; float g_Contrast; float g_BloomI; float g_BloomT; float g_BloomR; };
float3 Reinhard(float3 c) { return c/(1.0+c); }
float3 Uncharted2(float3 c) { float3 a=c*(c*0.15+0.025)+0.004; float3 b=c*(c*0.15+0.5)+1.0; return a/b; }
float3 ACES(float3 c) { float3 a=c*(c*2.51+0.03); float3 b=c*(c*2.43+0.59)+0.14; return saturate(a/b); }
struct PSIn { float4 Pos : SV_POSITION; float2 UV : TEXCOORD0; };
float4 main(PSIn i) : SV_TARGET {
    float3 c = g_HDR.Sample(g_HDR_sampler,i.UV).rgb*g_Exp + g_Bloom.Sample(g_HDR_sampler,i.UV).rgb*g_BloomI;
    if(g_Mode==1)c=Reinhard(c);else if(g_Mode==2)c=Uncharted2(c);else if(g_Mode==3)c=ACES(c);
    float lum=dot(c,float3(0.299,0.587,0.114)); c=lerp(float3(lum,lum,lum),c,g_Sat);
    c = (c - 0.5) * g_Contrast + 0.5;
    c=pow(max(c,0.0),1.0/g_Gamma);
    float2 uv=i.UV*2.0-1.0; c*=saturate(1.0-dot(uv,uv)*g_Vig);
    return float4(c,1.0);
}
)";

static const char* g_BrightPS = R"(
Texture2D g_In : register(t0); SamplerState g_In_sampler : register(s0);
cbuffer BP : register(b0) { float g_Thresh; float3 _pad; };
struct PSIn { float4 Pos : SV_POSITION; float2 UV : TEXCOORD0; };
float4 main(PSIn i) : SV_TARGET { float3 c=g_In.Sample(g_In_sampler,i.UV).rgb; float br=dot(c,float3(0.299,0.587,0.114)); return float4(c*saturate((br-g_Thresh)/max(0.001,1.0-g_Thresh)),1.0); }
)";

static const char* g_BlurPS = R"(
Texture2D g_In : register(t0); SamplerState g_In_sampler : register(s0);
cbuffer BP : register(b0) { float2 g_TS; float2 _p; };
struct PSIn { float4 Pos : SV_POSITION; float2 UV : TEXCOORD0; };
float4 main(PSIn i) : SV_TARGET {
    float2 t=g_TS; float3 c=0;
    c+=g_In.Sample(g_In_sampler,i.UV+float2(-t.x,-t.y)).rgb*0.0625;
    c+=g_In.Sample(g_In_sampler,i.UV+float2(0,-t.y)).rgb*0.125;
    c+=g_In.Sample(g_In_sampler,i.UV+float2(t.x,-t.y)).rgb*0.0625;
    c+=g_In.Sample(g_In_sampler,i.UV+float2(-t.x,0)).rgb*0.125;
    c+=g_In.Sample(g_In_sampler,i.UV).rgb*0.25;
    c+=g_In.Sample(g_In_sampler,i.UV+float2(t.x,0)).rgb*0.125;
    c+=g_In.Sample(g_In_sampler,i.UV+float2(-t.x,t.y)).rgb*0.0625;
    c+=g_In.Sample(g_In_sampler,i.UV+float2(0,t.y)).rgb*0.125;
    c+=g_In.Sample(g_In_sampler,i.UV+float2(t.x,t.y)).rgb*0.0625;
    return float4(c,1.0);
}
)";

struct PostProcessSubsystem::Impl {
	D::RefCntAutoPtr<D::IRenderDevice>  dev;
	D::RefCntAutoPtr<D::IDeviceContext> ctx;
	D::RefCntAutoPtr<D::ISwapChain>     sc;

	D::RefCntAutoPtr<D::ITexture>       hdrTex, bloomTex, blurTex, resTex;
	D::RefCntAutoPtr<D::ITextureView>   hdrRTV, hdrSRV, bloomRTV, bloomSRV, blurRTV, blurSRV, resRTV, resSRV;

	D::RefCntAutoPtr<D::IPipelineState> psoTM, psoBright, psoBlur;
	D::RefCntAutoPtr<D::IShaderResourceBinding> srbTM, srbBright, srbBlur;
	D::RefCntAutoPtr<D::IBuffer>        cbTM, cbBloom, cbBlur;

	UInt32 w=0, h=0; PostProcessConfig cfg; bool ok=false;

	D::RefCntAutoPtr<D::IShader> mkShader(const char* s, D::SHADER_TYPE t, const char* n) {
		D::ShaderCreateInfo ci; ci.Desc.ShaderType=t; ci.Desc.Name=n; ci.Desc.UseCombinedTextureSamplers=true;
		ci.EntryPoint="main"; ci.SourceLanguage=D::SHADER_SOURCE_LANGUAGE_HLSL; ci.Source=s; ci.SourceLength=(D::Uint32)strlen(s);
		D::RefCntAutoPtr<D::IShader> sh; dev->CreateShader(ci, &sh); return sh;
	}

	D::GraphicsPipelineStateCreateInfo basePSO(const char* name, D::IShader* vs, D::IShader* ps, int nvars, D::ShaderResourceVariableDesc* vars, D::ImmutableSamplerDesc* ims, int nims, UInt8 sampleCount = 1) {
		D::GraphicsPipelineStateCreateInfo ci;
		ci.PSODesc.Name=name; ci.PSODesc.PipelineType=D::PIPELINE_TYPE_GRAPHICS;
		ci.pVS=vs; ci.pPS=ps; ci.GraphicsPipeline.NumRenderTargets=1;
		ci.GraphicsPipeline.RTVFormats[0]=D::TEX_FORMAT_RGBA8_UNORM_SRGB;
		ci.GraphicsPipeline.PrimitiveTopology=D::PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
		ci.GraphicsPipeline.RasterizerDesc.CullMode=D::CULL_MODE_NONE;
		ci.GraphicsPipeline.DepthStencilDesc.DepthEnable=false;
		ci.GraphicsPipeline.SmplDesc.Count = sampleCount;
		ci.PSODesc.ResourceLayout.DefaultVariableType=D::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
		ci.PSODesc.ResourceLayout.Variables=vars; ci.PSODesc.ResourceLayout.NumVariables=nvars;
		ci.PSODesc.ResourceLayout.ImmutableSamplers=ims; ci.PSODesc.ResourceLayout.NumImmutableSamplers=nims;
		return ci;
	}

	void createAllPSOs() {
		auto vs = mkShader(g_FS_VS, D::SHADER_TYPE_VERTEX, "FS_VS");
		{	const char* psSrc = cfg.customShader.empty() ? g_ToneMapPS : cfg.customShader.c_str();
		const char* psName = cfg.customShader.empty() ? "ToneMapPS" : "CustomPS";
		auto ps = mkShader(psSrc, D::SHADER_TYPE_PIXEL, psName);
			D::ShaderResourceVariableDesc vv[] = {{D::SHADER_TYPE_PIXEL,"g_HDR",D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},{D::SHADER_TYPE_PIXEL,"g_Bloom",D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE}};
			D::SamplerDesc smp; smp.MinFilter=D::FILTER_TYPE_LINEAR; smp.MagFilter=D::FILTER_TYPE_LINEAR;
			D::ImmutableSamplerDesc im[] = {{D::SHADER_TYPE_PIXEL,"g_HDR",smp}};
			psoTM.Release();
		auto ci = basePSO("ToneMapPSO",vs,ps,EE_ARRAY_SIZE(vv),vv,im,EE_ARRAY_SIZE(im));
			dev->CreateGraphicsPipelineState(ci, &psoTM);
			if (!psoTM) { EError("Failed to create ToneMap PSO"); return; }
			D::BufferDesc bd; bd.Name="ppCB"; bd.Size=48; bd.BindFlags=D::BIND_UNIFORM_BUFFER; bd.Usage=D::USAGE_DYNAMIC; bd.CPUAccessFlags=D::CPU_ACCESS_WRITE;
			cbTM.Release();
		dev->CreateBuffer(bd,nullptr,&cbTM);
			auto* sv = psoTM->GetStaticVariableByName(D::SHADER_TYPE_PIXEL,"PP");
			if (sv) sv->Set(cbTM);
			srbTM.Release();
		psoTM->CreateShaderResourceBinding(&srbTM,true);
		}
		{	auto ps = mkShader(g_BrightPS, D::SHADER_TYPE_PIXEL, "BrightPS");
			D::ShaderResourceVariableDesc vv[] = {{D::SHADER_TYPE_PIXEL,"g_In",D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE}};
			D::SamplerDesc smp; smp.MinFilter=D::FILTER_TYPE_LINEAR; smp.MagFilter=D::FILTER_TYPE_LINEAR;
			D::ImmutableSamplerDesc im[] = {{D::SHADER_TYPE_PIXEL,"g_In",smp}};
			psoBright.Release();
		auto ci = basePSO("BrightPSO",vs,ps,EE_ARRAY_SIZE(vv),vv,im,EE_ARRAY_SIZE(im));
			dev->CreateGraphicsPipelineState(ci, &psoBright);
			D::BufferDesc bd; bd.Name="bCB"; bd.Size=16; bd.BindFlags=D::BIND_UNIFORM_BUFFER; bd.Usage=D::USAGE_DYNAMIC; bd.CPUAccessFlags=D::CPU_ACCESS_WRITE;
			cbBloom.Release();
		dev->CreateBuffer(bd,nullptr,&cbBloom);
			psoBright->GetStaticVariableByName(D::SHADER_TYPE_PIXEL,"BP")->Set(cbBloom);
			srbBright.Release();
		psoBright->CreateShaderResourceBinding(&srbBright,true);
		}
		{	auto ps = mkShader(g_BlurPS, D::SHADER_TYPE_PIXEL, "BlurPS");
			D::ShaderResourceVariableDesc vv[] = {{D::SHADER_TYPE_PIXEL,"g_In",D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE}};
			D::SamplerDesc smp; smp.MinFilter=D::FILTER_TYPE_LINEAR; smp.MagFilter=D::FILTER_TYPE_LINEAR;
			D::ImmutableSamplerDesc im[] = {{D::SHADER_TYPE_PIXEL,"g_In",smp}};
			psoBlur.Release();
		auto ci = basePSO("BlurPSO",vs,ps,EE_ARRAY_SIZE(vv),vv,im,EE_ARRAY_SIZE(im));
			dev->CreateGraphicsPipelineState(ci, &psoBlur);
			D::BufferDesc bd; bd.Name="blCB"; bd.Size=16; bd.BindFlags=D::BIND_UNIFORM_BUFFER; bd.Usage=D::USAGE_DYNAMIC; bd.CPUAccessFlags=D::CPU_ACCESS_WRITE;
			cbBlur.Release();
		dev->CreateBuffer(bd,nullptr,&cbBlur);
			psoBlur->GetStaticVariableByName(D::SHADER_TYPE_PIXEL,"BP")->Set(cbBlur);
			srbBlur.Release();
		psoBlur->CreateShaderResourceBinding(&srbBlur,true);
		}
	}

	void makeTex(D::RefCntAutoPtr<D::ITexture>& t, D::RefCntAutoPtr<D::ITextureView>& r, D::RefCntAutoPtr<D::ITextureView>& s, UInt8 sampleCount, D::TEXTURE_FORMAT fmt) {
		if(w==0||h==0) return;
		D::TextureDesc td; td.Type=D::RESOURCE_DIM_TEX_2D; td.Width=w; td.Height=h;
		td.Format=fmt; td.BindFlags=D::BIND_SHADER_RESOURCE|D::BIND_RENDER_TARGET;
		td.SampleCount = sampleCount;
		r.Release(); s.Release(); t.Release(); dev->CreateTexture(td,nullptr,&t);
		r=t->GetDefaultView(D::TEXTURE_VIEW_RENDER_TARGET); s=t->GetDefaultView(D::TEXTURE_VIEW_SHADER_RESOURCE);
	}

	void createTextures() {
		UInt8 msaaSamples = cfg.sampleCount;
		if (msaaSamples < 1) msaaSamples = 1;
		// HDR target and its single-sample resolve use RGBA16_FLOAT so that
		// bright reflections/highlights are not clamped to 8-bit before tone
		// mapping. Bloom intermediates stay 8-bit.
		makeTex(hdrTex,hdrRTV,hdrSRV, msaaSamples, D::TEX_FORMAT_RGBA16_FLOAT);
		makeTex(resTex,resRTV,resSRV, 1, D::TEX_FORMAT_RGBA16_FLOAT);
		makeTex(bloomTex,bloomRTV,bloomSRV, 1, D::TEX_FORMAT_RGBA8_UNORM_SRGB);
		makeTex(blurTex,blurRTV,blurSRV, 1, D::TEX_FORMAT_RGBA8_UNORM_SRGB);
		if(srbTM){auto*pv=srbTM->GetVariableByName(D::SHADER_TYPE_PIXEL,"g_HDR");if(pv)pv->Set(resSRV,D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);pv=srbTM->GetVariableByName(D::SHADER_TYPE_PIXEL,"g_Bloom");if(pv)pv->Set(blurSRV,D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);}
		if(srbBright){auto*pv=srbBright->GetVariableByName(D::SHADER_TYPE_PIXEL,"g_In");if(pv)pv->Set(resSRV,D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);}
		if(srbBlur){auto*pv=srbBlur->GetVariableByName(D::SHADER_TYPE_PIXEL,"g_In");if(pv)pv->Set(bloomSRV,D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);}
	}

	void exeBloom() {
		auto* rtvP = blurRTV.RawPtr();
		if(!cfg.bloom.enabled) {
			float cc[]={0,0,0,1}; ctx->SetRenderTargets(1,&rtvP,nullptr,D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
			ctx->ClearRenderTarget(rtvP,cc,D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION); return;
		}
		{ float t=cfg.bloom.threshold; void*m=nullptr; ctx->MapBuffer(cbBloom,D::MAP_WRITE,D::MAP_FLAG_DISCARD,m);
		  if(m){float b[4]={t,0,0,0}; memcpy(m,b,16); ctx->UnmapBuffer(cbBloom,D::MAP_WRITE);} }
		auto* brP=bloomRTV.RawPtr();
		ctx->SetRenderTargets(1,&brP,nullptr,D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		ctx->SetPipelineState(psoBright); ctx->CommitShaderResources(srbBright,D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		D::DrawAttribs da; da.NumVertices=3; da.Flags=D::DRAW_FLAG_VERIFY_ALL; ctx->Draw(da);
		{ float ts[2]={cfg.bloom.radius/(float)w,cfg.bloom.radius/(float)h}; void*m=nullptr;
		  ctx->MapBuffer(cbBlur,D::MAP_WRITE,D::MAP_FLAG_DISCARD,m);
		  if(m){memcpy(m,ts,8); ctx->UnmapBuffer(cbBlur,D::MAP_WRITE);} }
		ctx->SetRenderTargets(1,&rtvP,nullptr,D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		ctx->SetPipelineState(psoBlur); ctx->CommitShaderResources(srbBlur,D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		ctx->Draw(da);
	}
};

PostProcessSubsystem::PostProcessSubsystem() : Subsystem("PostProcess"), m_impl(std::make_unique<Impl>()) {}
PostProcessSubsystem::~PostProcessSubsystem() = default;
void PostProcessSubsystem::setDevice(void* d) { m_impl->dev=static_cast<D::IRenderDevice*>(d); }
void PostProcessSubsystem::setSwapChain(void* s) { m_impl->sc=static_cast<D::ISwapChain*>(s); }
void PostProcessSubsystem::setContext(void* c) { m_impl->ctx=static_cast<D::IDeviceContext*>(c); }
void PostProcessSubsystem::setConfig(const PostProcessConfig& c) { m_impl->cfg=c; }
const PostProcessConfig& PostProcessSubsystem::config() const { return m_impl->cfg; }
void PostProcessSubsystem::setCustomShader(const String& s) { m_impl->cfg.customShader = s; }
void PostProcessSubsystem::rebuildPSO() {
	if(!m_impl->ok) return;
	m_impl->psoTM.Release(); m_impl->srbTM.Release(); m_impl->cbTM.Release();
	m_impl->psoBright.Release(); m_impl->srbBright.Release(); m_impl->cbBloom.Release();
	m_impl->psoBlur.Release(); m_impl->srbBlur.Release(); m_impl->cbBlur.Release();
	m_impl->createAllPSOs();
	m_impl->createTextures();
}
void* PostProcessSubsystem::getHDRSRV() const { return m_impl->hdrSRV.RawPtr(); }
void* PostProcessSubsystem::getHDRRTV() const { return m_impl->hdrRTV.RawPtr(); }

void PostProcessSubsystem::resize(UInt32 ww, UInt32 wh) {
	if(ww==m_impl->w&&wh==m_impl->h) return;
	m_impl->w=ww; m_impl->h=wh;
	if(ww>0&&wh>0) m_impl->createTextures();
}

void PostProcessSubsystem::execute() {
	auto&p=*m_impl; if(!p.ok||!p.hdrRTV) return;
	auto* rtvP = p.sc->GetCurrentBackBufferRTV();
	if(!p.cfg.enabled) {
		// Passthrough: copy HDR target to swap chain without effects
		p.ctx->SetRenderTargets(1, &rtvP, nullptr, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		p.ctx->SetPipelineState(p.psoTM);
		{ void*m=nullptr; p.ctx->MapBuffer(p.cbTM, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m);
		  if(m){float b[12]={0,1.0f,2.2f,0,1.0f,1.0f,0,0,0,0,0,0}; memcpy(m,b,48); p.ctx->UnmapBuffer(p.cbTM, D::MAP_WRITE);} }
	p.ctx->CommitShaderResources(p.srbTM, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	D::DrawAttribs da; da.NumVertices=3; da.Flags=D::DRAW_FLAG_VERIFY_ALL; p.ctx->Draw(da);
		return;
	}
	// Resolve MSAA HDR to single-sample before post-process
	if (p.cfg.sampleCount > 1 && p.hdrTex && p.resTex) {
		D::ResolveTextureSubresourceAttribs ra;
		p.ctx->ResolveTextureSubresource(p.hdrTex.RawPtr(), p.resTex.RawPtr(), ra);
	}
	// When MSAA is off, bind hdrSRV directly (no resolve needed)
	if (p.cfg.sampleCount <= 1 && p.srbTM) {
		auto* pv = p.srbTM->GetVariableByName(D::SHADER_TYPE_PIXEL, "g_HDR");
		if (pv) pv->Set(p.hdrSRV, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
		pv = p.srbBright->GetVariableByName(D::SHADER_TYPE_PIXEL, "g_In");
		if (pv) pv->Set(p.hdrSRV, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	}
	if (p.cfg.sampleCount > 1 && p.srbTM) {
		auto* pv = p.srbTM->GetVariableByName(D::SHADER_TYPE_PIXEL, "g_HDR");
		if (pv) pv->Set(p.resSRV, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
		pv = p.srbBright->GetVariableByName(D::SHADER_TYPE_PIXEL, "g_In");
		if (pv) pv->Set(p.resSRV, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
	}
	p.exeBloom();
	{ void*m=nullptr; p.ctx->MapBuffer(p.cbTM,D::MAP_WRITE,D::MAP_FLAG_DISCARD,m);
		if(m){float b[12]={(float)(UInt32)p.cfg.toneMap.mode,p.cfg.toneMap.exposure,p.cfg.toneMap.gamma,p.cfg.toneMap.vignette,p.cfg.toneMap.saturation,p.cfg.toneMap.contrast,p.cfg.bloom.intensity,p.cfg.bloom.threshold,p.cfg.bloom.radius,0,0,0}; memcpy(m,b,48); p.ctx->UnmapBuffer(p.cbTM,D::MAP_WRITE);} }
	p.ctx->SetRenderTargets(1,&rtvP,nullptr,D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	p.ctx->SetPipelineState(p.psoTM); p.ctx->CommitShaderResources(p.srbTM,D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	D::DrawAttribs da; da.NumVertices=3; da.Flags=D::DRAW_FLAG_VERIFY_ALL; p.ctx->Draw(da);
}

void PostProcessSubsystem::present() {
	if (m_impl->ok && m_impl->sc) m_impl->sc->Present(1);
}

Result<void,CoreError> PostProcessSubsystem::onInitialize() {
	if(!m_impl->dev||!m_impl->ctx){EError("PostProcess: no dev/ctx");return CoreError::OperationFailed;}
	m_impl->createAllPSOs(); if(m_impl->w>0) m_impl->createTextures();
	m_impl->ok=true; EInfo("PostProcess ready (tone map + bloom)"); return {};
}
void PostProcessSubsystem::onShutdown() { m_impl->ok=false; }
void PostProcessSubsystem::onUpdate(F64) {}
bool PostProcessSubsystem::onRecover() { onShutdown(); return onInitialize().isOk(); }

EE_NAMESPACE_POSTPROCESS_END
