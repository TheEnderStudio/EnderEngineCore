#pragma once

#include <Engine/Core/Types.hpp>

EE_NAMESPACE_RENDERING_BEGIN

// ---------------------------------------------------------------------------
// Opaque type aliases (hides backend-specific pointer types)
// ---------------------------------------------------------------------------

using TextureSRV = void*;       ///< Opaque texture shader resource view
using TextureDSV = void*;       ///< Opaque depth-stencil view
using TextureRTV = void*;       ///< Opaque render target view
using TextureUAV = void*;       ///< Opaque texture unordered access view
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
	R8_UNorm, RG8_UNorm, R32_Float, RG32_Float, RGBA16_Float, RGBA32_Float, D32_Float, D24_UNorm_S8_UInt
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

/**
 * @brief Ray tracing capability flags (bitmask).
 *
 * Mirrors the Diligent RAY_TRACING_CAP_FLAGS values so the raw flags can be
 * compared 1:1 with the backend report.
 */
enum class RayTracingCaps : UInt8 {
	None               = 0x00,
	StandaloneShaders  = 0x01, ///< Full ray tracing pipeline (raygen / closest hit / miss / intersection shaders).
	InlineRayTracing   = 0x02, ///< Inline ray tracing via RayQuery in compute shaders (DXR 1.1).
	IndirectRayTracing = 0x04, ///< Indirect ray tracing (TraceRaysIndirect).
};

/// @brief Check whether a ray tracing capability flag is set.
EE_NODISCARD inline bool hasRayTracingCap(RayTracingCaps flags, RayTracingCaps cap) {
	return (static_cast<UInt8>(flags) & static_cast<UInt8>(cap)) != 0;
}

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
/// @brief Handle for a bottom-level acceleration structure (BLAS) resource.
using BLASHandle     = RenderHandle<struct BLASHandleTag>;
/// @brief Handle for a top-level acceleration structure (TLAS) resource.
using TLASHandle     = RenderHandle<struct TLASHandleTag>;

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

/// @brief A single instance in a top-level acceleration structure (TLAS).
struct TLASInstance {
	BLASHandle blas;                     ///< BLAS to reference (must be valid).
	Mat4 transform = Mat4(1.0f);         ///< World transform (column-major glm::mat4).
	UInt32 mask = 0xFF;                  ///< Instance inclusion mask used during ray traversal.
	UInt32 customId = 0;                 ///< User-defined ID, exposed as InstanceID() in hit/query shaders.
	String name;                         ///< Optional instance name (used for SBT binding / debugging).
};

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
	UInt8 sampleCount = 1;       ///< MSAA sample count (1 = no MSAA).
	bool asRenderTarget = false; ///< Also bind as a render target (BIND_RENDER_TARGET, usage DEFAULT).
	bool asUAV = false;          ///< Also bind as an unordered access view (BIND_UNORDERED_ACCESS).
	bool asDepthStencil = false; ///< Also bind as a depth-stencil target (BIND_DEPTH_STENCIL).
	/// Generate a full CPU mip chain for `data` (box filter in linear light).
	/// Reflection rays pick a level from the ray cone instead of aliasing level 0.
	bool mipChain = false;
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
struct MeshDesc {
	String name; ///< Optional; used in diagnostics (e.g. "this asset has inverted normals").
	Vector<Vertex> vertices; Vector<UInt32> indices; Vector<SubMesh> subMeshes;
};

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
	/**
	 * @brief World distance added to each cascade's box on every side, for shadow casters.
	 *
	 * A cascade's orthographic box is fitted to the camera slice, and a caster need not sit
	 * inside that slice: geometry between the light and the receiver, or off to the side of
	 * it, projects outside the box and is clipped out of the shadow map. The shadow then
	 * disappears exactly when the camera gets close, because the near cascades have the
	 * tightest boxes. This grows every cascade's box by this many world units on each side,
	 * trading shadow-map resolution for casters that would otherwise be missing.
	 * 0 disables it and leaves the fitted box untouched.
	 */
	F32   casterExtension = 15.0f;
};

/**
 * @brief Volumetric fog parameters (see Rendering::VolumetricSubsystem).
 *
 * The medium is an exponential height fog: the extinction at a world height y is
 * density * exp(-(y - baseHeight) / heightScale). The march then accumulates single
 * scattering with a Henyey-Greenstein phase function and the sun's shadow visibility, which
 * is what turns the shadow map into visible shafts.
 */
struct VolumetricConfig {
	/// @brief Which algorithm the fog runs (see Rendering::VolumetricSubsystem).
	enum class Mode : UInt8 {
		ScreenSpace = 0, ///< March each pixel's view ray (optionally at reduced resolution).
		Froxel = 1,      ///< Fill a view-space 3D grid, then integrate per pixel column.
	};

	bool  enabled      = false;
	F32   density      = 0.02f;   ///< Extinction at baseHeight (1 / world unit).
	F32   baseHeight   = 0.0f;    ///< Height at which the medium has full density.
	F32   heightScale  = 12.0f;   ///< e-folding height of the density falloff (world units).
	F32   anisotropy   = 0.6f;    ///< Henyey-Greenstein g: > 0 scatters forward (sun halo).
	F32   maxDistance  = 120.0f;  ///< Longest marched distance (sky included, as fog over sky).
	UInt32 steps       = 24;      ///< Ray march steps (more = smoother shafts, more expensive).
	F32   sunScattering = 1.0f;   ///< Scale of the sun's in-scattering.
	F32   ambientScattering = 0.25f; ///< Scale of the sky irradiance in-scattering.
	bool  sunShafts    = true;    ///< Sample the shadow map per step (off = uniform fog).
	F32   sunIntensity = 1.0f;    ///< Multiplied into the sun colour (set by the caller each frame).

	// ---- algorithm selection, and the knobs only one of the two uses --------------
	/// Default is the froxel volume: measured at roughly a third of the screen-space march's
	/// cost in the Demo scene, and its integration pass does not grow with the march length.
	Mode  mode = Mode::Froxel;
	/// Fraction of the render resolution the screen-space march runs at (0.25..1). Its result
	/// is upsampled with a depth-guided filter, so 0.5 halves the march cost per axis.
	F32   renderScale = 0.5f;
	/// Depth slices of the froxel volume. The injection pass costs
	/// width * height * froxelSlices / 64 threads, the integration one thread per pixel.
	UInt32 froxelSlices = 64;

	// ---- local light shadows -------------------------------------------------------
	/**
	 * @brief Whether local lights are occluded by geometry.
	 *
	 * Implemented as a short ray march toward the light, projected into the scene depth
	 * buffer (screen-space occlusion): it needs no per-light shadow map and works in both
	 * render modes, but only occluders inside the view take part - a wall behind the camera
	 * does not block the light. That is why the result is a soft factor, not a hard shadow.
	 */
	bool  localLightShadows = true;
	/// Samples along the light ray per froxel (or per march step). 4 is enough for a halo
	/// that stops at a silhouette; the cost is one depth fetch per sample.
	UInt32 localShadowSteps = 4;

	// ---- temporal accumulation and adaptive quality -------------------------------
	/// Blend the reprojected previous frame into this one. The accumulation is shared by both
	/// backends and averages banding (low step count) and the froxel grid's quantisation.
	bool  temporal = true;
	/// History weight: 0 disables the blend, 0.9 keeps a long average. A disocclusion (a
	/// depth mismatch after reprojection) drops the history regardless of this value.
	F32   temporalWeight = 0.9f;
	/// Scale the step/slice counts to hold a frame-time target (see targetMs).
	bool  adaptiveQuality = false;
	/// Target GPU time of the scattering pass in milliseconds; 0 disables the governor.
	F32   targetMs = 1.0f;

	// ---- medium detail and debugging ----------------------------------------------
	/// Break the medium up with a tileable 3D noise field, so shafts and cones get a natural
	/// texture instead of being perfectly smooth volumes. 0 disables it.
	F32   noiseStrength = 0.4f;
	/// World-space frequency of the noise: larger values give smaller wisps.
	F32   noiseScale = 0.08f;
	/// How fast the noise scrolls, in world units per second (it drifts along +Y).
	F32   noiseSpeed = 0.35f;
	/// Debug view composited instead of the lit scene: 0 = off, 1 = in-scattering,
	/// 2 = transmittance, 3 = both side by side.
	UInt32 debugMode = 0;
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
	/// Skybox corner colours (bit0=x+, bit1=y+, bit2=z+). The ambient light is the
	/// sky's irradiance, which is derived from them in the shaders; appended at the
	/// end so shaders that do not read them keep working unchanged.
	Vec4 skyCorners[8];
	/// Specular image-based lighting parameters: x = enabled, y = roughness -> mip
	/// scale of the prefiltered environment cube, z = debug (shade with the raw
	/// environment sample), w = intensity. Appended at the end like the corners, so
	/// shaders that do not read them keep working unchanged.
	Vec4 envParams = Vec4(0.0f, 1.0f, 0.0f, 1.0f);
	/// Cascaded shadow quality and bias: x = PCF radius in shadow map texels (0 = only
	/// the hardware's own 2x2 comparison filter), y = cascade blend fraction (how much
	/// of the far end of a cascade fades into the next one; 0 = the hard switch this
	/// used to be), z = depth bias base, w = slope-bias scale (the bias then grows with
	/// tan(angle between the normal and the light), which is where the depth error
	/// grows too). Appended at the end like the fields above.
	Vec4 shadowParams = Vec4(1.0f, 0.15f, 0.002f, 2.0f);
	/// Contact shadows and normal offset: x = normal offset in shadow map texels (the
	/// lookup is pushed that far off the surface along the normal, which removes
	/// self-shadowing at its source and, being in world units derived from the
	/// cascade, stays scale-correct), y = contact ray length in texels, z = contact
	/// strength, w = distance from the camera at which the contact term has faded out
	/// completely (0 disables it). Appended at the end like the fields above.
	Vec4 contactParams = Vec4(1.5f, 24.0f, 0.5f, 50.0f);
	/// Diffuse wrap of the direct lights: x = wrap amount (0 = Lambert, the physically
	/// correct falloff, 1 = the half-Lambert this used to be, which keeps every shadow
	/// at half brightness or more), y/z/w reserved. Appended at the end like the fields
	/// above.
	Vec4 lightParams = Vec4(0.0f);
};

/// @brief Single light data in the light constant buffer.
struct alignas(16) LightData {
	Vec4 CI; Vec4 DT; Vec4 PR; Vec4 CA;
};

/// @brief Light constant buffer containing all scene lights.
struct alignas(16) LightConstants { LightData lights[MaxLights]; };

/// @brief Per-object constant buffer (world matrix, normal matrix, base color, metallic/roughness, emissive).
struct alignas(16) ObjectConstants {
	Mat4 world; Mat4 normalMat;
	Vec4 baseColor; Vec4 metallicRough;
	Vec4 emissive; ///< Emissive color (rgb) + intensity (w).
	/// Which PBR maps this material actually has, so the shaders only sample what
	/// exists instead of relying on a fallback texture: x = normal map, y = MR map,
	/// z = emissive map, w = raw-sample debug (RenderSubsystem::setNormalMapDebug).
	/// (The mesh shader path carries the same flags in its material constant, which
	/// is why it never had this problem.)
	Vec4 mapFlags = Vec4(0.0f);
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
