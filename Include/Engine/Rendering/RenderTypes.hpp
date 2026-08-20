#pragma once

#include <Engine/Core/Types.hpp>

EE_NAMESPACE_RENDERING_BEGIN

// ---------------------------------------------------------------------------
// Opaque type aliases (hides backend-specific pointer types)
// ---------------------------------------------------------------------------

using TextureSRV = void*;       ///< Opaque texture shader resource view
using TextureDSV = void*;       ///< Opaque depth-stencil view
using TextureRTV = void*;       ///< Opaque render target view
using ComputeBuf = void*;       ///< Opaque GPU buffer (structured, constant, indirect args, staging)
using ComputeSRV = void*;       ///< Opaque buffer shader resource view
using DevicePtr = void*;        ///< Opaque render device pointer
using ContextPtr = void*;       ///< Opaque device context pointer
using SwapChainPtr = void*;     ///< Opaque swap chain pointer

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

/// @brief Shader stage for pipeline compilation.
enum class ShaderStage : UInt8 { Vertex, Pixel, Geometry, Compute };
/// @brief Primitive topology for draw calls.
enum class PrimitiveTopology : UInt8 { TriangleList, TriangleStrip, LineList, PointList };
/// @brief Face culling mode.
enum class CullMode : UInt8 { None, Front, Back };
/// @brief Rasterizer fill mode.
enum class FillMode : UInt8 { Solid, Wireframe };
/// @brief Depth/stencil comparison function.
enum class CompareFunc : UInt8 { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
/// @brief Texture sampling filter mode.
enum class FilterMode : UInt8 { Point, Linear, Anisotropic };
/// @brief Texture coordinate addressing mode.
enum class AddressMode : UInt8 { Wrap, Mirror, Clamp, Border };
/// @brief Blend factor for alpha blending.
enum class BlendFactor : UInt8 { Zero, One, SrcColor, InvSrcColor, SrcAlpha, InvSrcAlpha, DestAlpha, InvDestAlpha, DestColor, InvDestColor };
/// @brief Blend operation for alpha blending.
enum class BlendOp : UInt8 { Add, Subtract, RevSubtract, Min, Max };
/// @brief Light type for scene illumination.
enum class LightType : UInt8 { Directional, Point, Spot };
/// @brief Texture pixel format.
enum class TextureFormat : UInt32 {
	Unknown = 0, RGBA8_UNorm, RGBA8_UNorm_SRGB, BGRA8_UNorm, BGRA8_UNorm_SRGB,
	R8_UNorm, RG8_UNorm, R32_Float, RG32_Float, RGBA32_Float, D32_Float, D24_UNorm_S8_UInt
};

/**
 * @brief Rendering backend type.
 */
enum class RenderBackendType : UInt8 {
	Auto,    ///< Try D3D12, then D3D11, then Vulkan
	D3D12,   ///< Direct3D 12
	D3D11,   ///< Direct3D 11
	Vulkan,  ///< Vulkan
};

// ---------------------------------------------------------------------------
// Handle types
// ---------------------------------------------------------------------------

/// @brief Sentinel index value indicating an invalid handle.
inline constexpr UInt32 InvalidIndex = 0xFFFFFFFF;

/// @brief Generic handle for GPU resources with index and generation fields.
template <typename Tag>
struct RenderHandle {
	UInt32 index = InvalidIndex;
	UInt32 generation = 0;
	bool isValid() const { return index != InvalidIndex; }
	bool operator==(const RenderHandle& o) const { return index == o.index && generation == o.generation; }
	bool operator!=(const RenderHandle& o) const { return !(*this == o); }
};

/// @brief Handle for a mesh resource.
using MeshHandle     = RenderHandle<struct MeshTag>;
/// @brief Handle for a material resource.
using MaterialHandle = RenderHandle<struct MaterialTag>;
/// @brief Handle for a texture resource.
using TextureHandle  = RenderHandle<struct TextureTag>;
/// @brief Handle for a shader resource.
using ShaderHandle   = RenderHandle<struct ShaderTag>;
/// @brief Handle for a pipeline state object.
using PSOHandle      = RenderHandle<struct PSOHandleTag>;
/// @brief Handle for a sampler resource.
using SamplerHandle  = RenderHandle<struct SamplerTag>;
/// @brief Handle for a camera resource.
using CameraHandle   = RenderHandle<struct CameraTag>;
/// @brief Handle for a light resource.
using LightHandle    = RenderHandle<struct LightTag>;
/// @brief Handle for a model resource.
using ModelHandle    = RenderHandle<struct ModelTag>;

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

/// @brief Vertex data with position, normal, texcoord and tangent.
struct Vertex {
	Vec3 position; Vec3 normal; Vec2 texCoord; Vec4 tangent;
};

/// @brief Descriptor for creating a pipeline state object.
struct PipelineStateDesc {
	String name;
	ShaderHandle vs; ShaderHandle ps;
	PrimitiveTopology topology = PrimitiveTopology::TriangleList;
	struct { FillMode fillMode = FillMode::Solid; CullMode cullMode = CullMode::Back; bool frontCounterClockwise = true; } rasterizer;
	struct { bool depthEnable = true; bool depthWrite = true; CompareFunc depthFunc = CompareFunc::Less; } depthStencil;
	struct { bool blendEnable = false; } blendTarget;
};

/// @brief Descriptor for creating a shader.
struct ShaderDesc {
	ShaderStage stage = ShaderStage::Vertex;
	String entryPoint = "main";
	String source;
	String filePath;
};

/// @brief Descriptor for creating a texture.
struct TextureDesc {
	UInt32 w = 0; UInt32 h = 0; TextureFormat fmt = TextureFormat::RGBA8_UNorm;
	UInt32 mipLevels = 1; const void* data = nullptr; UInt32 dataSize = 0;
};

/// @brief Descriptor for creating a sampler.
struct SamplerDesc {
	FilterMode minFilter = FilterMode::Linear; FilterMode magFilter = FilterMode::Linear;
	FilterMode mipFilter = FilterMode::Linear;
	AddressMode addressU = AddressMode::Wrap; AddressMode addressV = AddressMode::Wrap; AddressMode addressW = AddressMode::Wrap;
	UInt32 maxAnisotropy = 1; CompareFunc comparisonFunc = CompareFunc::Never;
};

/// @brief Descriptor for creating a material.
struct MaterialDesc {
	String name;
	Vec4 baseColorFactor = Vec4(1.0f);
	F32 metallicFactor = 0.0f; F32 roughnessFactor = 0.5f;
	Vec3 emissiveFactor = Vec3(0.0f);
	TextureHandle baseColorTexture; TextureHandle metallicRoughnessTexture;
	TextureHandle normalTexture; TextureHandle emissiveTexture; TextureHandle aoTexture;
};

/// @brief A sub-range of a mesh with its own material.
struct SubMesh { UInt32 indexOffset = 0; UInt32 indexCount = 0; UInt32 vertexOffset = 0; MaterialHandle material; };

/// @brief Descriptor for creating a mesh.
struct MeshDesc { Vector<Vertex> vertices; Vector<UInt32> indices; Vector<SubMesh> subMeshes; };

/// @brief Descriptor for creating a camera.
struct CameraDesc {
	Vec3 pos = Vec3(0, 0, 5); Vec3 target = Vec3(0, 0, 0); Vec3 up = Vec3(0, 1, 0);
	F32 fov = 60.0f; F32 nearP = 0.01f; F32 farP = 1000.0f; F32 w = 1280; F32 h = 720;
};

/// @brief Descriptor for creating a light.
struct LightDesc {
	LightType type = LightType::Directional;
	Vec3 color = Vec3(1.0f); F32 intensity = 1.0f;
	Vec3 dir = Vec3(0, -1, 0);
	Vec3 pos = Vec3(0, 0, 0); F32 range = 10.0f;
	F32 innerCone = 0.0f; F32 outerCone = 0.0f;
	bool shadowEnabled = false; ///< Enable cascaded shadow maps for this light.
};

/// @brief Cascaded shadow map configuration.
struct ShadowConfig {
	bool  enabled        = false;
	UInt32 resolution    = 2048;
	UInt32 numCascades   = 4;
	F32   partitioning   = 0.95f; ///< 0=linear, 1=logarithmic cascade split
	F32   depthBias      = 0.0025f;
	UInt32 filterSize    = 5;     ///< PCF filter kernel size (3/5/7)
	bool  visualizeCascades = false;
};

/// @brief Result from loading a model, containing meshes, materials and root transform.
struct ModelLoadResult { Vector<MeshHandle> meshes; Vector<MaterialHandle> materials; Transform rootTransform; };

// ---------------------------------------------------------------------------
// Constant buffers (16-byte aligned, matches HLSL cbuffer layout)
// ---------------------------------------------------------------------------

/// @brief Maximum number of lights supported in the constant buffer.
static constexpr UInt32 MaxLights = 8;

/// @brief Per-frame constant buffer (view-projection, camera position, ambient light).
struct alignas(16) FrameConstants {
	Mat4 viewProj;
	Vec4 cameraPos; Vec4 ambient;
	UInt32 lightCount; F32 _p0; F32 _p1; F32 _p2;
	Mat4 shadowMapUVDepth[4];
	Vec4 cascadeSplits;
};

/// @brief Single light data in the light constant buffer.
struct alignas(16) LightData {
	Vec4 CI; Vec4 DT; Vec4 PR; Vec4 CA;
};

/// @brief Light constant buffer containing all scene lights.
struct alignas(16) LightConstants { LightData lights[MaxLights]; };

/// @brief Per-object constant buffer (world matrix, normal matrix, base color, metallic/roughness).
struct alignas(16) ObjectConstants {
	Mat4 world; Mat4 normalMat;
	Vec4 baseColor; Vec4 metallicRough;
};

// ---------------------------------------------------------------------------
// GPU Frustum Culling
// ---------------------------------------------------------------------------

/// @brief Maximum number of cullable instances (matches instanceCB size).
static constexpr UInt32 MaxCullInstances = 4096;

/// @brief Per-instance data for GPU frustum culling (input to compute shader).
struct alignas(16) CullingInstance {
	Vec4 boundSphere;      ///< xyz = center, w = radius (world space)
	UInt32 drawIndex;       ///< Index into the draw list
	F32 _pad0; F32 _pad1; F32 _pad2;
};

/// @brief Six frustum planes packed for GPU constant buffer.
struct alignas(16) FrustumPlanes {
	Vec4 planes[6];         ///< Each: xyz = normal, w = distance
};

/// @brief GPU indirect draw arguments (matches D3D12 D3D12_DRAW_INDEXED_ARGUMENTS).
struct alignas(16) IndirectDrawArgs {
	UInt32 indexCount;      ///< NumIndices
	UInt32 instanceCount;   ///< NumInstances
	UInt32 firstIndex;      ///< StartIndexLocation
	UInt32 baseVertex;      ///< BaseVertexLocation
	UInt32 startInstance;   ///< StartInstanceLocation
	UInt32 _pad0; UInt32 _pad1; UInt32 _pad2; ///< Pad to 32 bytes
};

/// @brief GPU culling constant buffer.
struct alignas(16) CullingConstants {
	FrustumPlanes frustum;
	UInt32 instanceCount;   ///< Number of instances to test
	UInt32 _pad0; UInt32 _pad1; UInt32 _pad2;
};

EE_NAMESPACE_RENDERING_END
