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
    float3 col = g_Ambient.rgb * g_Ambient.a * bc;
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
        float3 specC = lerp(float3(0.04, 0.04, 0.04), bc, m);
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

float4 main(PSIn i) : SV_TARGET
{
    float4 tex = t_BC.Sample(t_BC_sampler, i.UV);
    float3 bc = tex.rgb * g_BaseColor.rgb;
    float  a  = tex.a * g_BaseColor.a;
    float3 N = normalize(i.N);
    float3 V = normalize(g_CameraPos.xyz - i.WP);
    float m = g_MetallicRough.x;
    float r = g_MetallicRough.y;
    float3 lit = DoLight(i.WP, N, V, bc, m, r);
    return float4(lit, a);
}
)";

// G-buffer pixel shader: writes albedo (SV_Target0) and world normal (SV_Target1).
// g_ShadowMap is declared (but unused) so the resource layout matches the PBR PSOs.
static const char* g_PS_GBuffer = R"(
Texture2D    t_BC         : register(t0);
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
};

PSOut main(PSIn i)
{
    PSOut o;
    o.Color = t_BC.Sample(t_BC_sampler, i.UV) * g_BaseColor;
    o.Norm  = float4(normalize(i.N), g_MetallicRough.y);
    return o;
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
	switch (severity)
	{
	default:
		EDebug("Unknown message severity from Diligent Engine: {}", static_cast<UInt32>(severity));
	case D::DEBUG_MESSAGE_SEVERITY_INFO:
#ifdef EE_DEBUG
		Log::info(file, line, function, "[DiligentEngine] {}", message);
#endif
		break;
	case D::DEBUG_MESSAGE_SEVERITY_WARNING:
		Log::warn(file, line, function, "[DiligentEngine] {}", message); break;
	case D::DEBUG_MESSAGE_SEVERITY_ERROR:
		Log::error(file, line, function, "[DiligentEngine] {}", message); break;
	case D::DEBUG_MESSAGE_SEVERITY_FATAL_ERROR:
		Log::critical(file, line, function, "[DiligentEngine] {}", message); break;
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
	TextureHandle defTex, fogTex;
	MeshHandle    bboardMesh;
	D::RefCntAutoPtr<D::ITextureView> whiteSRV, fogSRV;
	MeshData      skyMesh;
	D::RefCntAutoPtr<D::IBuffer> skyCB;
	D::RefCntAutoPtr<D::ITexture> skyCubeTex;
	D::RefCntAutoPtr<D::ITextureView> skyCubeSRV;
	Optional<RenderSubsystem::SkyboxDesc> skyDesc;

	CamData cam; CameraHandle camHandle;
	Vec4 ambient = Vec4(0.30f, 0.30f, 0.35f, 1.0f);
	LightConstants lcBuf;
	Vector<LightHandle> activeLights;

	D::RefCntAutoPtr<D::IBuffer> frameCB, lightCB, instanceCB, bboardVB, bboardIB;
	UInt64 fn = 0; UInt32 dc = 0; bool ok = false; bool wireframe = false;
	bool gBufferActive = false; ///< When true, draw()/drawInstanced() write the G-buffer instead of shaded color.
	UInt8 msaaSamples = 1;
	// Ray tracing device feature request + reported capabilities.
	bool requestRayTracing = true;
	bool rtFeatureEnabled = false; ///< Whether the device actually enabled the ray tracing feature.
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
				// First attempt requests the ray tracing feature (if enabled).
				{
					D::EngineD3D12CreateInfo ci; ci.SetValidationLevel(D::VALIDATION_LEVEL_DISABLED);
					if (requestRayTracing) ci.Features.RayTracing = D::DEVICE_FEATURE_STATE_ENABLED;
					f->CreateDeviceAndContextsD3D12(ci, &device, &ctx);
				}
				// If the GPU/driver does not support ray tracing, retry without it so that
				// rasterization keeps working on non-RT hardware.
				if (!device && requestRayTracing) {
					EWarn("D3D12 device creation with ray tracing failed; retrying without ray tracing.");
					device.Release(); ctx.Release();
					D::EngineD3D12CreateInfo ci; ci.SetValidationLevel(D::VALIDATION_LEVEL_DISABLED);
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
					f->CreateDeviceAndContextsVk(ci, &device, &ctx);
				}
				if (!device && requestRayTracing) {
					EWarn("Vulkan device creation with ray tracing failed; retrying without ray tracing.");
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
					f->CreateDeviceAndContextsVk(ci, &device, &ctx);
				}
				if (!device && requestRayTracing) {
					EWarn("Vulkan device creation with ray tracing failed; retrying without ray tracing.");
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
		}

		D::TextureDesc td; td.Name = "Depth"; td.Type = D::RESOURCE_DIM_TEX_2D; td.Width = scd.Width; td.Height = scd.Height; td.Format = scd.DepthBufferFormat; td.BindFlags = D::BIND_DEPTH_STENCIL; td.Usage = D::USAGE_DEFAULT; td.SampleCount = msaaSamples;
		D::RefCntAutoPtr<D::ITexture> dt; device->CreateTexture(td, nullptr, &dt); if (dt) dsv = dt->GetDefaultView(D::TEXTURE_VIEW_DEPTH_STENCIL);
		return {};
	}

	Result<void, RenderError> createDefaults() {
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
				ci.GraphicsPipeline.RTVFormats[0] = D::TEX_FORMAT_RGBA8_UNORM_SRGB; ci.GraphicsPipeline.DSVFormat = D::TEX_FORMAT_D32_FLOAT;
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
					{D::SHADER_TYPE_VERTEX | D::SHADER_TYPE_PIXEL, "Object", D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
					{D::SHADER_TYPE_PIXEL, "g_ShadowMap", D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
					{D::SHADER_TYPE_VERTEX, "g_WorldMatrices", D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
					{D::SHADER_TYPE_VERTEX, "g_Indices", D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
				};
				ci.PSODesc.ResourceLayout.Variables = Vars; ci.PSODesc.ResourceLayout.NumVariables = EE_ARRAY_SIZE(Vars);
				D::SamplerDesc cmpSamp; cmpSamp.MinFilter = D::FILTER_TYPE_COMPARISON_LINEAR; cmpSamp.MagFilter = D::FILTER_TYPE_COMPARISON_LINEAR; cmpSamp.MipFilter = D::FILTER_TYPE_COMPARISON_LINEAR; cmpSamp.ComparisonFunc = D::COMPARISON_FUNC_LESS; cmpSamp.AddressU = D::TEXTURE_ADDRESS_CLAMP; cmpSamp.AddressV = D::TEXTURE_ADDRESS_CLAMP; cmpSamp.AddressW = D::TEXTURE_ADDRESS_CLAMP;
				D::ImmutableSamplerDesc ImtblSamps[] = {
					{D::SHADER_TYPE_PIXEL, "t_BC", D::SamplerDesc{}},
					{D::SHADER_TYPE_PIXEL, "g_ShadowMap", cmpSamp},
				};
				ci.PSODesc.ResourceLayout.ImmutableSamplers = ImtblSamps; ci.PSODesc.ResourceLayout.NumImmutableSamplers = EE_ARRAY_SIZE(ImtblSamps);
				D::RefCntAutoPtr<D::IPipelineState> p; device->CreateGraphicsPipelineState(ci, &p);
				if (p) {
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
				ci.GraphicsPipeline.RTVFormats[0] = D::TEX_FORMAT_RGBA8_UNORM_SRGB; ci.GraphicsPipeline.DSVFormat = D::TEX_FORMAT_D32_FLOAT;
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
				ci.GraphicsPipeline.RTVFormats[0] = D::TEX_FORMAT_RGBA8_UNORM_SRGB; ci.GraphicsPipeline.DSVFormat = D::TEX_FORMAT_D32_FLOAT;
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
				ci.GraphicsPipeline.RTVFormats[0] = D::TEX_FORMAT_RGBA8_UNORM_SRGB; ci.GraphicsPipeline.DSVFormat = D::TEX_FORMAT_D32_FLOAT;
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
			ci.GraphicsPipeline.NumRenderTargets = gbuffer ? 2 : 1;
			ci.GraphicsPipeline.RTVFormats[0] = D::TEX_FORMAT_RGBA8_UNORM_SRGB;
			if (gbuffer) ci.GraphicsPipeline.RTVFormats[1] = D::TEX_FORMAT_RGBA16_FLOAT;
			ci.GraphicsPipeline.DSVFormat = D::TEX_FORMAT_D32_FLOAT;
			// G-buffer pass uses non-MSAA targets (read back by the ray tracing compute shader).
			ci.GraphicsPipeline.SmplDesc.Count = gbuffer ? 1 : msaaSamples;
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

		D::ShaderResourceVariableDesc Vars[] = {
			{D::SHADER_TYPE_PIXEL, "t_BC",   D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
			{D::SHADER_TYPE_VERTEX | D::SHADER_TYPE_PIXEL, "Object", D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
			{D::SHADER_TYPE_PIXEL, "g_ShadowMap", D::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
		};
		ci.PSODesc.ResourceLayout.Variables = Vars; ci.PSODesc.ResourceLayout.NumVariables = EE_ARRAY_SIZE(Vars);

		D::SamplerDesc cmpSamp; cmpSamp.MinFilter = D::FILTER_TYPE_COMPARISON_LINEAR; cmpSamp.MagFilter = D::FILTER_TYPE_COMPARISON_LINEAR; cmpSamp.MipFilter = D::FILTER_TYPE_COMPARISON_LINEAR; cmpSamp.ComparisonFunc = D::COMPARISON_FUNC_LESS; cmpSamp.AddressU = D::TEXTURE_ADDRESS_CLAMP; cmpSamp.AddressV = D::TEXTURE_ADDRESS_CLAMP; cmpSamp.AddressW = D::TEXTURE_ADDRESS_CLAMP;
		D::ImmutableSamplerDesc ImtblSamps[] = {
			{D::SHADER_TYPE_PIXEL, "t_BC", D::SamplerDesc{}},
			{D::SHADER_TYPE_PIXEL, "g_ShadowMap", cmpSamp},
		};
		ci.PSODesc.ResourceLayout.ImmutableSamplers = ImtblSamps; ci.PSODesc.ResourceLayout.NumImmutableSamplers = EE_ARRAY_SIZE(ImtblSamps);

		D::RefCntAutoPtr<D::IPipelineState> p; device->CreateGraphicsPipelineState(ci, &p);
		if (!p) return RenderError::PipelineStateCreationFailed;

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
		auto a = meshes.allocate(); auto* dd = meshes.getUnchecked(a.index);
		dd->vc = (UInt32)d.vertices.size(); dd->ic = (UInt32)d.indices.size(); dd->sub = d.subMeshes;
		dd->cpuVertices = d.vertices; dd->cpuIndices = d.indices;
		// BIND_RAY_TRACING allows the buffers to be read during BLAS build operations,
		// but is only valid when the ray tracing device feature is enabled.
		{ D::BufferDesc bd; bd.Name = "VB"; bd.Size = d.vertices.size() * sizeof(Vertex); bd.BindFlags = D::BIND_VERTEX_BUFFER; if (rtFeatureEnabled) bd.BindFlags |= D::BIND_RAY_TRACING; bd.Usage = D::USAGE_IMMUTABLE; D::BufferData bdata; bdata.pData = d.vertices.data(); bdata.DataSize = bd.Size; D::RefCntAutoPtr<D::IBuffer> b; device->CreateBuffer(bd, &bdata, &b); if (!b) return RenderError::BufferCreationFailed; dd->vb = std::move(b); }
		{ D::BufferDesc bd; bd.Name = "IB"; bd.Size = d.indices.size() * sizeof(UInt32); bd.BindFlags = D::BIND_INDEX_BUFFER; if (rtFeatureEnabled) bd.BindFlags |= D::BIND_RAY_TRACING; bd.Usage = D::USAGE_IMMUTABLE; D::BufferData bdata; bdata.pData = d.indices.data(); bdata.DataSize = bd.Size; D::RefCntAutoPtr<D::IBuffer> b; device->CreateBuffer(bd, &bdata, &b); if (!b) return RenderError::BufferCreationFailed; dd->ib = std::move(b); }
		return MeshHandle{ a.index, a.generation };
	}

	Result<TextureHandle, RenderError> mkTex(const TextureDesc& d) {
		const bool renderTargetLike = d.asRenderTarget || d.asUAV || d.asDepthStencil;
		D::TextureDesc td; td.Name = "Tex"; td.Type = D::RESOURCE_DIM_TEX_2D; td.Width = d.w; td.Height = d.h; td.Format = toDFmt(d.fmt); td.MipLevels = d.mipLevels;
		td.BindFlags = D::BIND_SHADER_RESOURCE;
		if (d.asRenderTarget) td.BindFlags |= D::BIND_RENDER_TARGET;
		if (d.asUAV) td.BindFlags |= D::BIND_UNORDERED_ACCESS;
		if (d.asDepthStencil) td.BindFlags |= D::BIND_DEPTH_STENCIL;
		td.Usage = renderTargetLike ? D::USAGE_DEFAULT : D::USAGE_IMMUTABLE;
		D::TextureSubResData srd; srd.pData = d.data; srd.Stride = d.w * 4; D::TextureData tdata; tdata.pSubResources = d.data ? &srd : nullptr; tdata.NumSubresources = d.data ? 1 : 0;
		D::RefCntAutoPtr<D::ITexture> t; device->CreateTexture(td, d.data ? &tdata : nullptr, &t); if (!t) return RenderError::TextureCreationFailed;
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
			if (auto* pv = srb->GetVariableByName(D::SHADER_TYPE_PIXEL, "t_BC")) {
				D::ITextureView* srv = whiteSRV.RawPtr();
				if (d.baseColorTexture.isValid()) {
					if (auto* td = textures.get(d.baseColorTexture.index, d.baseColorTexture.generation)) srv = td->srv.RawPtr();
				}
				pv->Set(srv, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
			}
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
		{ FrameConstants fc{}; fc.viewProj = cam.proj * cam.view; fc.cameraPos = Vec4(cam.desc.pos, 1.0f); fc.ambient = ambient; fc.lightCount = (UInt32)activeLights.size(); for(int i=0;i<4;i++) fc.shadowMapUVDepth[i]=shadowMapUVDepth[i]; fc.cascadeSplits = cascadeSplits; void* m = nullptr; ctx->MapBuffer(frameCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m); if (m) { memcpy(m, &fc, sizeof(fc)); ctx->UnmapBuffer(frameCB, D::MAP_WRITE); } }
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
			if (mt && mt->objCB) { ObjectConstants oc; oc.world = wm; oc.normalMat = nm; oc.baseColor = mt->desc.baseColorFactor; oc.metallicRough = Vec4(mt->desc.metallicFactor, mt->desc.roughnessFactor, 0, 0); void* m = nullptr; ctx->MapBuffer(mt->objCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m); if (m) { memcpy(m, &oc, sizeof(oc)); ctx->UnmapBuffer(mt->objCB, D::MAP_WRITE); } }
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
			FrameConstants fc{}; fc.viewProj = cam.proj * cam.view; fc.cameraPos = Vec4(camPos, 1.0f); fc.ambient = ambient; fc.lightCount = (UInt32)activeLights.size(); for(int i=0;i<4;i++) fc.shadowMapUVDepth[i]=shadowMapUVDepth[i]; fc.cascadeSplits = cascadeSplits;
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
		{ FrameConstants fc{}; fc.viewProj = cam.proj * cam.view; fc.cameraPos = Vec4(cam.desc.pos, 1.0f); fc.ambient = ambient; fc.lightCount = (UInt32)activeLights.size(); for(int i=0;i<4;i++) fc.shadowMapUVDepth[i]=shadowMapUVDepth[i]; fc.cascadeSplits = cascadeSplits; void* m = nullptr; ctx->MapBuffer(frameCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m); if (m) { memcpy(m, &fc, sizeof(fc)); ctx->UnmapBuffer(frameCB, D::MAP_WRITE); } }
		{ void* m = nullptr; ctx->MapBuffer(lightCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m); if (m) { memcpy(m, &lcBuf, sizeof(lcBuf)); ctx->UnmapBuffer(lightCB, D::MAP_WRITE); } }
		D::Uint64 offsets[] = { 0, 0 };
		D::IBuffer* pBuffs[] = { md->vb.RawPtr(), instanceCB.RawPtr() };
		ctx->SetVertexBuffers(0, 2, pBuffs, offsets, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION, D::SET_VERTEX_BUFFERS_FLAG_RESET);
		ctx->SetIndexBuffer(md->ib, 0, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

		for (auto& s : md->sub) {
			auto* mt = materials.get(s.material.index, s.material.generation);
			if (mt && mt->objCB) { ObjectConstants oc; oc.world = Mat4(1.0f); oc.normalMat = Mat4(1.0f); oc.baseColor = mt->desc.baseColorFactor; oc.metallicRough = Vec4(mt->desc.metallicFactor, mt->desc.roughnessFactor, 0, 0); void* m = nullptr; ctx->MapBuffer(mt->objCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m); if (m) { memcpy(m, &oc, sizeof(oc)); ctx->UnmapBuffer(mt->objCB, D::MAP_WRITE); } }
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
		{ FrameConstants fc{}; fc.viewProj = cam.proj * cam.view; fc.cameraPos = Vec4(cam.desc.pos, 1.0f); fc.ambient = ambient; fc.lightCount = (UInt32)activeLights.size(); for(int i=0;i<4;i++) fc.shadowMapUVDepth[i]=shadowMapUVDepth[i]; fc.cascadeSplits = cascadeSplits; void* m = nullptr; ctx->MapBuffer(frameCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m); if (m) { memcpy(m, &fc, sizeof(fc)); ctx->UnmapBuffer(frameCB, D::MAP_WRITE); } }
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
			// Mutable variables must be explicitly bound on fresh SRB
			{
				// Try to get material's texture, fall back to white
				D::IDeviceObject* texObj = whiteSRV.RawPtr();
				if (mt && mt->srb) {
					auto* src = mt->srb->GetVariableByName(D::SHADER_TYPE_PIXEL, "t_BC");
					if (src) { auto* obj = src->Get(); if (obj) texObj = obj; }
				}
				if (auto* v = srb->GetVariableByName(D::SHADER_TYPE_PIXEL, "t_BC"))
					v->Set(texObj, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
			}
			if (auto* v = srb->GetVariableByName(D::SHADER_TYPE_PIXEL, "g_ShadowMap"))
				v->Set(shadowSRV ? shadowSRV : shadowDummy, D::SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);

			if (mt) {
				void* m = nullptr; ctx->MapBuffer(mt->objCB, D::MAP_WRITE, D::MAP_FLAG_DISCARD, m);
				if (m) { ObjectConstants oc; oc.world = Mat4(1.0f); oc.normalMat = Mat4(1.0f); oc.baseColor = mt->desc.baseColorFactor; oc.metallicRough = Vec4(mt->desc.metallicFactor, mt->desc.roughnessFactor, 0, 0); memcpy(m, &oc, sizeof(oc)); ctx->UnmapBuffer(mt->objCB, D::MAP_WRITE); }
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
			FrameConstants fc{}; fc.viewProj = cam.proj * cam.view; fc.cameraPos = Vec4(cam.desc.pos, 1.0f); fc.ambient = ambient; fc.lightCount = 0; for(int i=0;i<4;i++) fc.shadowMapUVDepth[i]=shadowMapUVDepth[i]; fc.cascadeSplits = cascadeSplits;
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
				loadInfo.IsSRGB = true;
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
							TextureDesc td; td.fmt = TextureFormat::RGBA8_UNorm_SRGB; td.w = decoded.width; td.h = decoded.height;
							td.data = decoded.pixels.data(); td.dataSize = (UInt32)decoded.pixels.size();
							auto tr = mkTex(td); if (tr.isOk()) th = tr.value();
						}
					}
					}, buffer.data);
			}
			else if (auto* arr = std::get_if<fastgltf::sources::Array>(&img.data)) {
				srcType = "Array";
				auto decoded = Utilities::decodeImage(arr->bytes.data(), arr->bytes.size());
				if (decoded.isValid()) {
					TextureDesc td; td.fmt = TextureFormat::RGBA8_UNorm_SRGB; td.w = decoded.width; td.h = decoded.height;
					td.data = decoded.pixels.data(); td.dataSize = (UInt32)decoded.pixels.size();
					auto tr = mkTex(td); if (tr.isOk()) th = tr.value();
				}
			}
			else if (auto* bv2 = std::get_if<fastgltf::sources::ByteView>(&img.data)) {
				srcType = "ByteView";
				auto decoded = Utilities::decodeImage(bv2->bytes.data(), bv2->bytes.size());
				if (decoded.isValid()) {
					TextureDesc td; td.fmt = TextureFormat::RGBA8_UNorm_SRGB; td.w = decoded.width; td.h = decoded.height;
					td.data = decoded.pixels.data(); td.dataSize = (UInt32)decoded.pixels.size();
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
			// Resolve baseColorTexture through Texture��Image mapping
			if (m.pbrData.baseColorTexture.has_value()) {
				auto& texInfo = m.pbrData.baseColorTexture.value();
				if (texInfo.textureIndex < asset.textures.size()) {
					auto& tex = asset.textures[texInfo.textureIndex];
					if (tex.imageIndex.has_value()) {
						auto it = texMap.find((UInt32)tex.imageIndex.value());
						if (it != texMap.end()) { md.baseColorTexture = it->second; hasTex = true; }
					}
				}
			}
			ETrace("Material[{}] '{}': baseColor=({:.2f},{:.2f},{:.2f},{:.2f}) metal={:.2f} rough={:.2f} tex={}",
				i, md.name, md.baseColorFactor.x, md.baseColorFactor.y, md.baseColorFactor.z, md.baseColorFactor.w,
				md.metallicFactor, md.roughnessFactor, hasTex ? "yes" : "no");
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
				UInt32 base = (UInt32)allVerts.size();
				for (UInt32 v = 0; v < vc; ++v) { allVerts.push_back({ pos[v], nrm[v], uv[v], Vec4(1,0,0,1) }); }
				if (prim.indicesAccessor.has_value()) readIndices(asset, asset.accessors[prim.indicesAccessor.value()], allIdx, base);
				sub.indexCount = (UInt32)allIdx.size() - sub.indexOffset;
				if (prim.materialIndex.has_value()) { auto it = matMap.find((UInt32)prim.materialIndex.value()); if (it != matMap.end()) sub.material = it->second; else if (!matMap.empty()) sub.material = matMap.begin()->second; }
				subMeshes.push_back(sub);
			}
			if (!allVerts.empty() && !allIdx.empty()) {
				MeshDesc md; md.vertices = std::move(allVerts); md.indices = std::move(allIdx); md.subMeshes = std::move(subMeshes);
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
RayTracingCaps RenderSubsystem::rayTracingCaps() const { return m_backend->rtCaps; }
bool RenderSubsystem::supportsInlineRayTracing() const { return hasRayTracingCap(m_backend->rtCaps, RayTracingCaps::InlineRayTracing); }
bool RenderSubsystem::supportsStandaloneRayTracing() const { return hasRayTracingCap(m_backend->rtCaps, RayTracingCaps::StandaloneShaders); }
UInt32 RenderSubsystem::maxRayRecursionDepth() const { return m_backend->rtMaxRecursionDepth; }
UInt32 RenderSubsystem::maxInstancesPerTLAS() const { return m_backend->rtMaxInstancesPerTLAS; }

void RenderSubsystem::setMSAASampleCount(UInt8 c) { m_backend->msaaSamples = c; }
UInt8 RenderSubsystem::msaaSamples() const { return m_backend->msaaSamples; }

void RenderSubsystem::setShadowSRV(TextureSRV srv) { m_backend->shadowSRV = static_cast<D::ITextureView*>(srv); }
void RenderSubsystem::setShadowData(const Mat4(&uv)[4], const Vec4& splits) { auto& b=*m_backend; for(int i=0;i<4;i++) b.shadowMapUVDepth[i]=uv[i]; b.cascadeSplits=splits; }

Result<ShaderHandle, RenderError> RenderSubsystem::createShader(const ShaderDesc& d) { if (!m_backend->ok) return RenderError::NotInitialized; return m_backend->mkShader(d); }
Result<ShaderHandle, RenderError> RenderSubsystem::createShaderFromFile(ShaderStage s, const String& fp, const String& ep) { ShaderDesc d; d.stage = s; d.filePath = fp; d.entryPoint = ep; return createShader(d); }
Result<PSOHandle, RenderError> RenderSubsystem::createPipelineState(const PipelineStateDesc& d) { if (!m_backend->ok) return RenderError::NotInitialized; return m_backend->mkPSO(d); }
Result<MeshHandle, RenderError> RenderSubsystem::createMesh(const MeshDesc& d) { if (!m_backend->ok) return RenderError::NotInitialized; return m_backend->mkMesh(d); }
Result<TextureHandle, RenderError> RenderSubsystem::createTexture(const TextureDesc& d) { if (!m_backend->ok) return RenderError::NotInitialized; return m_backend->mkTex(d); }
Result<SamplerHandle, RenderError> RenderSubsystem::createSampler(const SamplerDesc& d) { if (!m_backend->ok) return RenderError::NotInitialized; return m_backend->mkSampler(d); }
Result<MaterialHandle, RenderError> RenderSubsystem::createMaterial(const MaterialDesc& d, PSOHandle p) { if (!m_backend->ok) return RenderError::NotInitialized; return m_backend->mkMat(d, p); }

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
void RenderSubsystem::setAmbientLight(const Vec3& c, F32 i) { m_backend->ambient = Vec4(c, i); }

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
void RenderSubsystem::setSkybox(const SkyboxDesc& d) { m_backend->skyDesc = d; }
void RenderSubsystem::clearSkybox() { m_backend->skyDesc.reset(); }

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

	D::TextureDesc td; td.Name = "SkyCube"; td.Type = D::RESOURCE_DIM_TEX_CUBE;
	td.Width = w; td.Height = h; td.ArraySize = 6; td.MipLevels = 1;
	td.Format = D::TEX_FORMAT_RGBA8_UNORM_SRGB;
	td.BindFlags = D::BIND_SHADER_RESOURCE; td.Usage = D::USAGE_IMMUTABLE;

	// Cubemap face order (D3D12): 0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z
	D::TextureSubResData subRes[6];
	for (int i = 0; i < 6; i++) {
		subRes[i].pData = imgs[i].pixels.data();
		subRes[i].Stride = (D::Uint32)(imgs[i].width * 4);
	}
	D::TextureData tdata; tdata.pSubResources = subRes; tdata.NumSubresources = 6;
	D::RefCntAutoPtr<D::ITexture> tex;
	b.device->CreateTexture(td, &tdata, &tex);
	if (!tex) { EError("Cubemap creation failed"); return RenderError::TextureCreationFailed; }
	auto a = b.textures.allocate();
	auto* dd = b.textures.getUnchecked(a.index);
	dd->tex = std::move(tex); dd->srv = dd->tex->GetDefaultView(D::TEXTURE_VIEW_SHADER_RESOURCE);
	EInfo("Skybox cubemap created: {}x{}", w, h);
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

TextureDSV RenderSubsystem::getDepthStencil() const { return m_backend->dsv.RawPtr(); }

void RenderSubsystem::beginGBuffer(TextureRTV colorRT, TextureRTV normalRT, TextureDSV depthDSV) {
	if (!m_backend->ok || !colorRT || !normalRT || !depthDSV) return;
	auto& b = *m_backend;
	b.gBufferActive = true;
	auto* cRTV = static_cast<D::ITextureView*>(colorRT);
	auto* nRTV = static_cast<D::ITextureView*>(normalRT);
	auto* dDSV = static_cast<D::ITextureView*>(depthDSV);
	D::ITextureView* rtvs[2] = { cRTV, nRTV };
	b.ctx->SetRenderTargets(2, rtvs, dDSV, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	const float cc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	b.ctx->ClearRenderTarget(cRTV, cc, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
	b.ctx->ClearRenderTarget(nRTV, cc, D::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
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
void RenderSubsystem::onShutdown() { m_backend->ok = false; m_backend->device.Release(); m_backend->ctx.Release(); m_backend->sc.Release(); m_backend->factory.Release(); EInfo("Rendering shut down"); }

void RenderSubsystem::onUpdate(F64) {}

bool RenderSubsystem::onRecover() { EInfo("Recovering renderer..."); onShutdown(); return onInitialize().isOk(); }

EE_NAMESPACE_RENDERING_END
