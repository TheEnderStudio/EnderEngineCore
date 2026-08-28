#include <Rendering/Render2DSubsystem.hpp>
#include <Core/Log.hpp>

#include <DiligentCore/Graphics/GraphicsEngine/interface/RenderDevice.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/DeviceContext.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/PipelineState.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Buffer.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Shader.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/GraphicsTypes.h>
#include <DiligentCore/Common/interface/RefCntAutoPtr.hpp>

namespace D = Diligent;

EE_NAMESPACE_RENDERING_BEGIN

static const char* g_VS2D = R"(
cbuffer Constants:register(b0){float4x4 g_Proj;};
struct VSIn{float2 Pos:ATTRIB0;float2 UV:ATTRIB1;float4 Col:ATTRIB2;};
struct VSOut{float4 Pos:SV_POSITION;float2 UV:TEXCOORD0;float4 Col:TEXCOORD1;};
VSOut main(VSIn i){VSOut o;o.Pos=mul(g_Proj,float4(i.Pos,0,1));o.UV=i.UV;o.Col=i.Col;return o;}
)";

static const char* g_PS2DTex = R"(
Texture2D g_Tex:register(t0);SamplerState g_Tex_sampler:register(s0);
struct PSIn{float4 Pos:SV_POSITION;float2 UV:TEXCOORD0;float4 Col:TEXCOORD1;};
float4 main(PSIn i):SV_TARGET{return g_Tex.Sample(g_Tex_sampler,i.UV)*i.Col;}
)";

static const char* g_PS2DSolid = R"(
struct PSIn{float4 Pos:SV_POSITION;float2 UV:TEXCOORD0;float4 Col:TEXCOORD1;};
float4 main(PSIn i):SV_TARGET{return i.Col;}
)";

struct Render2DSubsystem::Impl {
	static constexpr UInt32 MaxVertices=65536, MaxIndices=98304;
	D::RefCntAutoPtr<D::IRenderDevice> device; D::RefCntAutoPtr<D::IDeviceContext> ctx;
	D::RefCntAutoPtr<D::IPipelineState> psoTexDS, psoTexNoDS, psoSolidDS, psoSolidNoDS;
	D::RefCntAutoPtr<D::IShaderResourceBinding> srbSolidDS, srbSolidNoDS;
	D::RefCntAutoPtr<D::IBuffer> cb, vb, ib;
	UInt32 screenW=1280,screenH=720; bool ok=false; bool afterPost=true;
	Vector<Vertex2D> verts; Vector<UInt32> idx;
	Int32 scissorX=0, scissorY=0, scissorW=0, scissorH=0; bool scissorEnabled=false;

	D::RefCntAutoPtr<D::IShader> mkShader(const char* s, D::SHADER_TYPE t, const char* n) {
		D::ShaderCreateInfo ci; ci.Desc.ShaderType=t; ci.Desc.Name=n; ci.Desc.UseCombinedTextureSamplers=true;
		ci.EntryPoint="main"; ci.SourceLanguage=D::SHADER_SOURCE_LANGUAGE_HLSL; ci.Source=s; ci.SourceLength=(D::Uint32)strlen(s);
		D::RefCntAutoPtr<D::IShader> sh; device->CreateShader(ci,&sh); return sh;
	}

	void createPSO(D::RefCntAutoPtr<D::IPipelineState>& pso, D::RefCntAutoPtr<D::IShaderResourceBinding>& srb,
	               const char* psSrc, bool hasTexture, D::TEXTURE_FORMAT dsvFmt) {
		auto vs=mkShader(g_VS2D,D::SHADER_TYPE_VERTEX,"VS2D");
		auto ps=mkShader(psSrc,D::SHADER_TYPE_PIXEL,hasTexture?"PS2DTex":"PS2DSolid");
		D::GraphicsPipelineStateCreateInfo ci;
		ci.PSODesc.Name=hasTexture?"R2DTex":"R2DSolid"; ci.PSODesc.PipelineType=D::PIPELINE_TYPE_GRAPHICS;
		ci.pVS=vs; ci.pPS=ps; ci.GraphicsPipeline.NumRenderTargets=1;
		ci.GraphicsPipeline.RTVFormats[0]=D::TEX_FORMAT_RGBA8_UNORM_SRGB;
		ci.GraphicsPipeline.PrimitiveTopology=D::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		ci.GraphicsPipeline.RasterizerDesc.CullMode=D::CULL_MODE_NONE;
		ci.GraphicsPipeline.RasterizerDesc.ScissorEnable=true;
		ci.GraphicsPipeline.DepthStencilDesc.DepthEnable=false;
		ci.GraphicsPipeline.DSVFormat=dsvFmt;
		auto& b=ci.GraphicsPipeline.BlendDesc.RenderTargets[0]; b.BlendEnable=true;
		b.SrcBlend=D::BLEND_FACTOR_SRC_ALPHA; b.DestBlend=D::BLEND_FACTOR_INV_SRC_ALPHA;
		D::LayoutElement le[]={
			{0,0,2,D::VT_FLOAT32,false,0,sizeof(Vertex2D),D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX},
			{1,0,2,D::VT_FLOAT32,false,8,sizeof(Vertex2D),D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX},
			{2,0,4,D::VT_FLOAT32,false,16,sizeof(Vertex2D),D::INPUT_ELEMENT_FREQUENCY_PER_VERTEX},
		};
	ci.GraphicsPipeline.InputLayout.NumElements=EE_ARRAY_SIZE(le); ci.GraphicsPipeline.InputLayout.LayoutElements=le;
	ci.PSODesc.ResourceLayout.DefaultVariableType=D::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
	if(hasTexture) {
		static const D::ShaderResourceVariableDesc vv[]={{D::SHADER_TYPE_PIXEL,"g_Tex",D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE}};
		ci.PSODesc.ResourceLayout.Variables=vv; ci.PSODesc.ResourceLayout.NumVariables=EE_ARRAY_SIZE(vv);
		static D::SamplerDesc smp2; smp2.MinFilter=D::FILTER_TYPE_LINEAR; smp2.MagFilter=D::FILTER_TYPE_LINEAR;
		static const D::ImmutableSamplerDesc im[]={{D::SHADER_TYPE_PIXEL,"g_Tex",smp2}};
		ci.PSODesc.ResourceLayout.ImmutableSamplers=im; ci.PSODesc.ResourceLayout.NumImmutableSamplers=EE_ARRAY_SIZE(im);
	}
	device->CreateGraphicsPipelineState(ci,&pso);
	pso->GetStaticVariableByName(D::SHADER_TYPE_VERTEX,"Constants")->Set(cb);
	pso->CreateShaderResourceBinding(&srb,true);
	}

	void createAllPSOs() {
		auto vs=mkShader(g_VS2D,D::SHADER_TYPE_VERTEX,"VS2D");
		// Common buffers
		{ D::BufferDesc bd; bd.Name="R2DCB"; bd.Size=sizeof(Mat4); bd.BindFlags=D::BIND_UNIFORM_BUFFER; bd.Usage=D::USAGE_DYNAMIC; bd.CPUAccessFlags=D::CPU_ACCESS_WRITE; device->CreateBuffer(bd,nullptr,&cb); }
		{ D::BufferDesc bd; bd.Name="R2DVB"; bd.Size=sizeof(Vertex2D)*MaxVertices; bd.BindFlags=D::BIND_VERTEX_BUFFER; bd.Usage=D::USAGE_DYNAMIC; bd.CPUAccessFlags=D::CPU_ACCESS_WRITE; device->CreateBuffer(bd,nullptr,&vb); }
		{ D::BufferDesc bd; bd.Name="R2DIB"; bd.Size=sizeof(UInt32)*MaxIndices; bd.BindFlags=D::BIND_INDEX_BUFFER; bd.Usage=D::USAGE_DYNAMIC; bd.CPUAccessFlags=D::CPU_ACCESS_WRITE; device->CreateBuffer(bd,nullptr,&ib); }
		// afterPost=false 2D is drawn together with the scene, so the PSO declares
		// the scene depth format (D32) - the DSV is set by the renderer before UI.
		// The afterPost=true variants declare UNKNOWN (post-process leaves no DSV).
		{ D::RefCntAutoPtr<D::IShaderResourceBinding> tmp; createPSO(psoTexNoDS,tmp,g_PS2DTex,true,D::TEX_FORMAT_UNKNOWN); }
		{ D::RefCntAutoPtr<D::IShaderResourceBinding> tmp; createPSO(psoTexDS,tmp,g_PS2DTex,true,D::TEX_FORMAT_D32_FLOAT); }
	createPSO(psoSolidNoDS,srbSolidNoDS,g_PS2DSolid,false,D::TEX_FORMAT_UNKNOWN);
	createPSO(psoSolidDS,srbSolidDS,g_PS2DSolid,false,D::TEX_FORMAT_D32_FLOAT);
	}

	void flushSolid() {
		if(verts.empty()) return;
		auto& pso = afterPost ? psoSolidNoDS : psoSolidDS;
		auto& srb = afterPost ? srbSolidNoDS : srbSolidDS;
		uploadAndDraw(pso, &srb);
	}

	void uploadAndDraw(D::IPipelineState* pso, D::IShaderResourceBinding*const* srb) {
		{ void*m=nullptr; ctx->MapBuffer(vb,D::MAP_WRITE,D::MAP_FLAG_DISCARD,m);
		  if(m){memcpy(m,verts.data(),verts.size()*sizeof(Vertex2D)); ctx->UnmapBuffer(vb,D::MAP_WRITE);} }
		{ void*m=nullptr; ctx->MapBuffer(ib,D::MAP_WRITE,D::MAP_FLAG_DISCARD,m);
		  if(m){memcpy(m,idx.data(),idx.size()*sizeof(UInt32)); ctx->UnmapBuffer(ib,D::MAP_WRITE);} }
		{ Mat4 proj=glm::ortho(0.0f,(float)screenW,(float)screenH,0.0f);
		  void*m=nullptr; ctx->MapBuffer(cb,D::MAP_WRITE,D::MAP_FLAG_DISCARD,m);
		  if(m){memcpy(m,&proj,sizeof(Mat4)); ctx->UnmapBuffer(cb,D::MAP_WRITE);} }
		D::Uint64 vo=0; D::IBuffer* vbs[]={vb.RawPtr()};
		ctx->SetVertexBuffers(0,1,vbs,&vo,D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,D::SET_VERTEX_BUFFERS_FLAG_RESET);
		ctx->SetIndexBuffer(ib,0,D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		ctx->SetPipelineState(pso);
		if (scissorEnabled) { D::Rect r={scissorX,scissorY,scissorX+scissorW,scissorY+scissorH}; ctx->SetScissorRects(1,&r,(D::Uint32)screenW,(D::Uint32)screenH); }
		else { D::Rect r={0,0,(Int32)screenW,(Int32)screenH}; ctx->SetScissorRects(1,&r,(D::Uint32)screenW,(D::Uint32)screenH); }
		if(srb) ctx->CommitShaderResources(*srb,D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
		D::DrawIndexedAttribs da; da.IndexType=D::VT_UINT32; da.NumIndices=(UInt32)idx.size(); da.Flags=D::DRAW_FLAG_VERIFY_ALL;
		ctx->DrawIndexed(da);
		verts.clear(); idx.clear();
	}

	void addQuad(Vec2 pos, Vec2 size, Vec4 color, Vec2 uv0=Vec2(0,0), Vec2 uv1=Vec2(1,1), float rot=0) {
		UInt32 base=(UInt32)verts.size();
		float hw=size.x*0.5f,hh=size.y*0.5f,cx=pos.x+hw,cy=pos.y+hh;
		auto rp=[&](float x,float y){float rx=x-cx,ry=y-cy; float c=cos(rot),s=sin(rot); return Vec2(cx+rx*c-ry*s,cy+rx*s+ry*c);};
		Vec2 p0=rp(pos.x,pos.y),p1=rp(pos.x+size.x,pos.y),p2=rp(pos.x+size.x,pos.y+size.y),p3=rp(pos.x,pos.y+size.y);
		verts.push_back({p0,uv0,color}); verts.push_back({p1,Vec2(uv1.x,uv0.y),color});
		verts.push_back({p2,uv1,color}); verts.push_back({p3,Vec2(uv0.x,uv1.y),color});
		idx.insert(idx.end(),{base,base+1,base+2,base,base+2,base+3});
	}

	void drawRectInternal(Vec2 pos, Vec2 size, Vec4 color) { addQuad(pos,size,color); flushSolid(); }
	void drawFilledCircle(Vec2 c, F32 r, Vec4 col, UInt32 seg) {
		UInt32 base = (UInt32)verts.size();
		verts.push_back({c, Vec2(0,0), col});
		for (UInt32 i = 0; i <= seg; i++) {
			F32 a = (F32)i / (F32)seg * glm::two_pi<F32>();
			verts.push_back({Vec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r), Vec2(0,0), col});
		}
		for (UInt32 i = 1; i <= seg; i++)
			idx.insert(idx.end(), {base, base + i + 1, base + i});
		flushSolid();
	}
	void drawOutlineCircle(Vec2 c, F32 r, F32 t, Vec4 col, UInt32 seg) {
		F32 ir = r - t;
		UInt32 base = (UInt32)verts.size();
		for (UInt32 i = 0; i <= seg; i++) {
			F32 a = (F32)i / (F32)seg * glm::two_pi<F32>();
			F32 sa = std::sin(a), ca = std::cos(a);
			verts.push_back({Vec2(c.x + ca * r, c.y + sa * r), Vec2(0,0), col});
			verts.push_back({Vec2(c.x + ca * ir, c.y + sa * ir), Vec2(0,0), col});
		}
		for (UInt32 i = 0; i < seg; i++) {
			UInt32 a = base + i * 2, b = a + 1, c = a + 2, d = a + 3;
			idx.insert(idx.end(), {a, b, c, b, d, c});
		}
		flushSolid();
	}
	void drawTexQuad(D::ITextureView* tex, Vec2 pos, Vec2 size, Vec4 color, Vec2 uv0, Vec2 uv1, float rot) {
		addQuad(pos,size,color,uv0,uv1,rot);
		if(verts.empty()) return;
		auto* pso = afterPost ? psoTexNoDS.RawPtr() : psoTexDS.RawPtr();
		D::RefCntAutoPtr<D::IShaderResourceBinding> srb;
		pso->CreateShaderResourceBinding(&srb, true);
		if(tex){auto*var=srb->GetVariableByName(D::SHADER_TYPE_PIXEL,"g_Tex");if(var)var->Set(tex);}
		uploadAndDraw(pso, &srb);
	}
};

Render2DSubsystem::Render2DSubsystem() : Subsystem("Render2D"), m_impl(std::make_unique<Impl>()) {}
Render2DSubsystem::~Render2DSubsystem() = default;
void Render2DSubsystem::setDevice(void* d) { m_impl->device=static_cast<D::IRenderDevice*>(d); }
void Render2DSubsystem::setContext(void* c) { m_impl->ctx=static_cast<D::IDeviceContext*>(c); }
void Render2DSubsystem::setScreenSize(UInt32 w, UInt32 h) { m_impl->screenW=w; m_impl->screenH=h; }
void Render2DSubsystem::setAfterPostProcess(bool a) { m_impl->afterPost=a; }
void Render2DSubsystem::begin() { m_impl->verts.clear(); m_impl->idx.clear(); }
void Render2DSubsystem::drawSprite(const SpriteDesc& d) {
	m_impl->drawTexQuad(static_cast<D::ITextureView*>(d.texture), d.position, d.size, d.color, d.uvMin, d.uvMax, d.rotation);
}
void Render2DSubsystem::drawSpriteUV(const SpriteDesc& d) { drawSprite(d); }
void Render2DSubsystem::drawRect(Vec2 pos, Vec2 size, Vec4 color) { m_impl->drawRectInternal(pos, size, color); }
void Render2DSubsystem::drawFilledCircle(Vec2 c, F32 r, Vec4 col, UInt32 seg) { m_impl->drawFilledCircle(c, r, col, seg); }
void Render2DSubsystem::drawOutlineCircle(Vec2 c, F32 r, F32 t, Vec4 col, UInt32 seg) { m_impl->drawOutlineCircle(c, r, t, col, seg); }
void Render2DSubsystem::end() {}
void Render2DSubsystem::setScissorRect(Vec2 pos, Vec2 size) {
	if (size.x <= 0 || size.y <= 0) { m_impl->scissorEnabled = false; return; }
	m_impl->scissorX = (Int32)pos.x; m_impl->scissorY = (Int32)pos.y;
	m_impl->scissorW = (Int32)size.x; m_impl->scissorH = (Int32)size.y;
	m_impl->scissorEnabled = true;
}

Result<void,CoreError> Render2DSubsystem::onInitialize() {
	if(!m_impl->device||!m_impl->ctx){EError("Render2D: need device+ctx");return CoreError::OperationFailed;}
	m_impl->createAllPSOs(); m_impl->ok=true; EInfo("Render2D ready"); return {};
}
void Render2DSubsystem::onShutdown() { m_impl->ok=false; }

EE_NAMESPACE_RENDERING_END
