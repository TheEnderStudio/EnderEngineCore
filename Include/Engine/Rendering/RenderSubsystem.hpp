#pragma once

#include <Engine/Core/Subsystem.hpp>
#include <Engine/Jobs/JobTypes.hpp>
#include <Engine/Utilities/ImageLoader.hpp>
#include "Errors.hpp"
#include "RenderTypes.hpp"
#include "RenderEvents.hpp"

EE_NAMESPACE_JOBS_BEGIN
class JobSubsystem;
EE_NAMESPACE_JOBS_END

EE_NAMESPACE_RENDERING_BEGIN

/// @brief Core rendering subsystem managing GPU resources and scene rendering.
class EE_API RenderSubsystem : public Subsystem {
public:
	/// @brief Default constructor.
	RenderSubsystem();
	/// @brief Destructor.
	~RenderSubsystem() override;

	EE_NO_COPY(RenderSubsystem)
	EE_NO_MOVE(RenderSubsystem)

	// -------------------------------------------------------------------
	// Lifecycle & configuration
	// -------------------------------------------------------------------

	/// @brief Set the GLFW window handle for rendering.
	void setWindow(void* nativeWindow);
	/// @brief Check if the render backend has been initialized.
	bool isReady() const;

	/**
	 * @brief Select the rendering backend. Must be called before initialize().
	 * @param type The backend type (default: Auto).
	 */
	void setBackend(RenderBackendType type);

	/**
	 * @brief Get the currently selected backend type.
	 */
	RenderBackendType backend() const;

	/**
	 * @brief Enable or disable the ray tracing device feature.
	 *
	 * When enabled (default), D3D12 and Vulkan device creation requests the
	 * ray tracing feature. On GPUs/drivers without ray tracing support the
	 * device creation may fail and the engine falls back to the next backend
	 * (e.g. D3D11) as usual. Must be called before initialize().
	 * @param enable true to request ray tracing.
	 */
	void setRayTracingEnabled(bool enable);

	/// @brief Whether the ray tracing device feature was requested (not whether it is available).
	EE_NODISCARD bool isRayTracingEnabled() const;

	/**
	 * @brief Get the raw ray tracing capability flags reported by the device.
	 *
	 * Valid after initialize(). Zero means ray tracing is not available.
	 */
	EE_NODISCARD RayTracingCaps rayTracingCaps() const;

	/// @brief Whether the device supports inline ray tracing (RayQuery in compute shaders, DXR 1.1).
	EE_NODISCARD bool supportsInlineRayTracing() const;

	/// @brief Whether the device supports the full ray tracing pipeline (raygen / closest hit / miss shaders).
	EE_NODISCARD bool supportsStandaloneRayTracing() const;

	/// @brief Maximum supported ray recursion depth (0 if ray tracing is unavailable).
	EE_NODISCARD UInt32 maxRayRecursionDepth() const;

	/// @brief Maximum number of instances in a top-level acceleration structure (0 if ray tracing is unavailable).
	EE_NODISCARD UInt32 maxInstancesPerTLAS() const;

	/// @brief Set MSAA sample count (1=off, 2/4/8). Applied to all PSOs at creation.
	void setMSAASampleCount(UInt8 count);
	UInt8 msaaSamples() const;

	/// @brief Set the shadow map SRV for PBR rendering.
	void setShadowSRV(TextureSRV srv);
	/// @brief Set shadow map UV-depth matrices (one per cascade) and split distances.
	void setShadowData(const Mat4(&shadowMapUVDepth)[4], const Vec4& cascadeSplits);

	/// @brief Get active camera view and projection matrices.
	void getCameraMatrices(Mat4& view, Mat4& proj) const;

	// -------------------------------------------------------------------
	// Shadows
	// -------------------------------------------------------------------

	/// @brief Configure cascaded shadow maps. Call before initialize().
	void setShadowConfig(const ShadowConfig& cfg);

	/// @brief Get the shadow SRV (for PBR material setup).
	TextureSRV getShadowSRV() const;

	/// @brief Render shadow depth pass for all meshes using the given shadow subsystem.
	void renderShadowPass(class ShadowSubsystem& shadow, MeshHandle mesh, const Mat4& worldMatrix);
	/// @brief Render shadow pass with GPU-culled instances via indirect draw.
	void renderShadowPassIndirect(class ShadowSubsystem& shadow, MeshHandle mesh, ComputeSRV worldMatSRV, ComputeSRV indicesSRV, ComputeBuf argsBuf, UInt32 argsByteOffset);
	void clearShadowCascades(class ShadowSubsystem& shadow);

	// -------------------------------------------------------------------
	// Accessors for ray tracing integration
	// -------------------------------------------------------------------

	/**
	 * @brief Get the GPU geometry buffers of a mesh (for acceleration structure building).
	 * @param mesh Mesh handle.
	 * @param vertexBuffer Out: Diligent IBuffer* as void* (nullptr if the mesh is invalid).
	 * @param indexBuffer  Out: Diligent IBuffer* as void*.
	 * @param vertexCount  Out: number of vertices.
	 * @param indexCount   Out: number of indices.
	 */
	void getMeshGeometry(MeshHandle mesh, void*& vertexBuffer, void*& indexBuffer, UInt32& vertexCount, UInt32& indexCount) const;

	/**
	 * @brief Get the material descriptor (for bindless ray tracing data).
	 * @param material Material handle.
	 * @return The material descriptor, or NullOpt if the handle is invalid.
	 */
	EE_NODISCARD Optional<MaterialDesc> getMaterial(MaterialHandle material) const;

	/**
	 * @brief Get the shader resource view of a texture (Diligent ITextureView* as void*).
	 * @param texture Texture handle.
	 * @return The texture SRV, or nullptr if the handle is invalid.
	 */
	TextureSRV getTextureSRV(TextureHandle texture) const;

	/// @brief Destroy a texture created with createTexture() (its handle becomes invalid).
	void destroyTexture(TextureHandle texture);

	/// @brief Get the render target view of a texture (nullptr if not created as a render target).
	TextureRTV getTextureRTV(TextureHandle texture) const;

	/// @brief Get the unordered access view of a texture (nullptr if not created as a UAV).
	TextureUAV getTextureUAV(TextureHandle texture) const;

	/// @brief Get the depth-stencil view of a texture (nullptr if not created as a depth target).
	TextureDSV getTextureDSV(TextureHandle texture) const;

	/**
	 * @brief Resolve an MSAA G-buffer into single-sample targets with a compute pass.
	 *
	 * Color/normal/emissive are averaged over all samples, depth takes the nearest
	 * (min) sample. Runs only when the renderer MSAA sample count is > 1.
	 * @param colorSRV,normalSRV,emissiveSRV,depthSRV  SRVs of the MSAA G-buffer targets.
	 * @param colorUAV,normalUAV,emissiveUAV,depthUAV  UAVs of the 1x resolve targets.
	 * @param width,height G-buffer dimensions (dispatch size).
	 */
	Result<void, RenderError> resolveGBufferMSAA(void* colorSRV, void* normalSRV, void* emissiveSRV, void* depthSRV,
		void* colorUAV, void* normalUAV, void* emissiveUAV, void* depthUAV, UInt32 width, UInt32 height);

	/// @brief Get the renderer's internal (MSAA) depth-stencil view.
	TextureDSV getDepthStencil() const;

	// -------------------------------------------------------------------
	// G-buffer pass (for hybrid ray tracing)
	// -------------------------------------------------------------------

	/**
	 * @brief Begin a G-buffer pass.
	 *
	 * Subsequent drawMesh()/drawMeshInstanced() calls write albedo (color), world
	 * normal and emissive into the given render targets instead of the final
	 * shaded color, and the depth is written to depthDSV. Call endGBuffer() to
	 * restore normal shading.
	 * @param colorRT    Render target view for albedo (RGBA8_UNORM/SRGB).
	 * @param normalRT   Render target view for world normal (RGBA16_FLOAT).
	 * @param emissiveRT Render target view for emissive (RGBA16_FLOAT).
	 * @param depthDSV   Depth-stencil view (D32_FLOAT, non-MSAA recommended).
	 */
	void beginGBuffer(TextureRTV colorRT, TextureRTV normalRT, TextureRTV emissiveRT, TextureDSV depthDSV);

	/// @brief End the G-buffer pass and restore the default shading mode.
	void endGBuffer();

	/// @brief Whether a G-buffer pass is currently active.
	EE_NODISCARD bool isGBufferActive() const;

	// -------------------------------------------------------------------
	// Shader management
	// -------------------------------------------------------------------

	/// @brief Create a shader from a descriptor.
	Result<ShaderHandle, RenderError> createShader(const ShaderDesc& desc);
	/// @brief Create a shader by loading source from a file.
	Result<ShaderHandle, RenderError> createShaderFromFile(
		ShaderStage stage, const String& filePath, const String& entryPoint = "main");

	// -------------------------------------------------------------------
	// Pipeline state
	// -------------------------------------------------------------------

	/// @brief Create a pipeline state object from a descriptor.
	Result<PSOHandle, RenderError> createPipelineState(const PipelineStateDesc& desc);

	// -------------------------------------------------------------------
	// Buffer management
	// -------------------------------------------------------------------

	/// @brief Create a mesh from a descriptor (vertices, indices, submeshes).
	Result<MeshHandle, RenderError> createMesh(const MeshDesc& desc);

	// -------------------------------------------------------------------
	// Texture management
	// -------------------------------------------------------------------

	/// @brief Create a texture from a descriptor.
	Result<TextureHandle, RenderError> createTexture(const TextureDesc& desc);
	/// @brief Create a sampler from a descriptor.
	Result<SamplerHandle, RenderError> createSampler(const SamplerDesc& desc);

	// -------------------------------------------------------------------
	// Material management
	// -------------------------------------------------------------------

	/// @brief Create a material from a descriptor and pipeline state.
	Result<MaterialHandle, RenderError> createMaterial(const MaterialDesc& desc, PSOHandle pso);

	/// @brief Get the default PBR pipeline state handle (used by model materials).
	///        Handy for creating Demo-side materials (e.g. emissive showcase objects).
	PSOHandle defaultPSO() const;

	// -------------------------------------------------------------------
	// Camera management
	// -------------------------------------------------------------------

	/// @brief Create a camera from a descriptor.
	Result<CameraHandle, RenderError> createCamera(const CameraDesc& desc);
	/// @brief Update an existing camera's properties.
	Result<void, RenderError> updateCamera(CameraHandle handle, const CameraDesc& desc);
	/// @brief Set the currently active camera for rendering.
	void setActiveCamera(CameraHandle handle);
	/// @brief Get the currently active camera handle.
	CameraHandle activeCamera() const;

	// -------------------------------------------------------------------
	// Light management
	// -------------------------------------------------------------------

	/// @brief Create a light from a descriptor.
	Result<LightHandle, RenderError> createLight(const LightDesc& desc);
	/// @brief Update an existing light's properties.
	Result<void, RenderError> updateLight(LightHandle handle, const LightDesc& desc);
	/// @brief Set the ambient light color and intensity.
	void setAmbientLight(const Vec3& color, F32 intensity = 0.1f);

	// -------------------------------------------------------------------
	// Model loading
	// -------------------------------------------------------------------

	/// @brief Load a model synchronously from a file path.
	Result<ModelLoadResult, RenderError> loadModel(const String& filePath);
	/// @brief Load a model synchronously with an explicit root directory.
	Result<ModelLoadResult, RenderError> loadModel(const String& filePath, const String& rootDirectory);

	/// @brief Load a model from a resource archive (via ResourcesManager).
	Result<ModelLoadResult, RenderError> loadModel(const ResPath& filePath);

	/// @brief Load a model asynchronously from a file path and root directory.
	Result<ModelHandle, RenderError> loadModelAsync(const String& filePath, const String& rootDir,
		const Object& owner, class Jobs::JobSubsystem& jobs,
		Jobs::JobPriority priority = Jobs::JobPriority::Normal);

	/// @brief Load a model asynchronously from a file path.
	Result<ModelHandle, RenderError> loadModelAsync(const String& filePath,
		const Object& owner, class Jobs::JobSubsystem& jobs,
		Jobs::JobPriority priority = Jobs::JobPriority::Normal);

	/// @brief Load a model asynchronously from a resource archive.
	Result<ModelHandle, RenderError> loadModelAsync(const ResPath& filePath, const ResPath& rootDir,
		const Object& owner, class Jobs::JobSubsystem& jobs,
		Jobs::JobPriority priority = Jobs::JobPriority::Normal);

	/// @brief Load a model asynchronously from a resource archive (auto root dir).
	Result<ModelHandle, RenderError> loadModelAsync(const ResPath& filePath,
		const Object& owner, class Jobs::JobSubsystem& jobs,
		Jobs::JobPriority priority = Jobs::JobPriority::Normal);

	/**
	 * @brief Get loaded meshes from a model (sync or async).
	 * @param handle ModelHandle returned by loadModel/loadModelAsync.
	 * @return The mesh/material list, or empty vector if not yet loaded/invalid handle.
	 */
	Vector<MeshHandle> getModelMeshes(ModelHandle handle) const;

	// -------------------------------------------------------------------
	// Rendering
	// -------------------------------------------------------------------

	/**
	 * @brief Override the render target for scene rendering (e.g., HDR off-screen target).
	 *
	 * When set, beginFrame() renders to this RTV instead of the swap chain back buffer,
	 * and endFrame() skips Present(). Call reset after post-processing to resume normal flow.
	 * @param rtv Render target view (ITextureView*), or nullptr to use swap chain.
	 * @param dsv Depth-stencil view (ITextureView*), or nullptr to use internal DSV.
	 */
	void setRenderTarget(TextureRTV rtv, TextureDSV dsv = nullptr);

	/// @brief Begin a new frame (clear targets, set viewport).
	void beginFrame();
	/// @brief Draw a mesh at the given transform.
	void drawMesh(MeshHandle mesh, const Transform& transform);

	/**
	 * @brief Draw a mesh with instanced rendering (one draw call for all instances).
	 * @param mesh Mesh handle.
	 * @param worldMatrices Per-instance world transforms (max 1024).
	 */
	void drawMeshInstanced(MeshHandle mesh, const Vector<Mat4>& worldMatrices);
	/// @brief Draw instanced mesh with GPU-compacted data via indirect draw.
	void drawMeshInstancedIndirect(MeshHandle mesh, ComputeSRV worldMatricesSRV, ComputeSRV indicesSRV, ComputeBuf argsBuf, UInt32 argsByteOffset);
	/// @brief Get mesh sub-mesh info for indirect draw args.
	SubMesh getSubMesh(MeshHandle mesh, UInt32 subIdx) const;

	/// @brief Get CPU-side vertex data from a mesh (for physics cooking).
	const Vector<Vertex>* getMeshVertices(MeshHandle mesh) const;
	const Vector<UInt32>* getMeshIndices(MeshHandle mesh) const;

	/// @brief End the current frame and present to the swap chain.
	void endFrame();

	/// @brief Resize the viewport / swap chain backing buffers.
	void resize(UInt32 width, UInt32 height);
	/// @brief Get the current viewport dimensions.
	void getViewportSize(UInt32& width, UInt32& height) const;

	/**
	 * @brief Toggle wireframe rendering mode.
	 * @param enable true for wireframe, false for solid.
	 */
	void setWireframe(bool enable);

	/**
	 * @brief Check if wireframe mode is active.
	 */
	bool isWireframe() const;

	// -------------------------------------------------------------------
	// Statistics
	// -------------------------------------------------------------------

	/// @brief Get the number of draw calls in the last frame.
	UInt32 lastFrameDrawCalls() const;
	/// @brief Get the current frame number.
	UInt64 frameNumber() const;

	/// @brief Get Diligent device pointer (for PostProcess integration).
	DevicePtr getDevice() const;
	/// @brief Get Diligent immediate context pointer.
	ContextPtr getContext() const;
	/// @brief Get Diligent swap chain pointer.
	SwapChainPtr getSwapChain() const;

	// -------------------------------------------------------------------
	// Billboard / particle rendering
	// -------------------------------------------------------------------

	/// @brief Descriptor for a billboard (camera-facing quad).
	struct BillboardDesc {
		Vec3 position;
		Vec2 size;
		Vec4 color;
	};

	/// @brief Draw camera-facing billboard quads with alpha blending.
	void drawBillboards(const Vector<BillboardDesc>& billboards);

	// -------------------------------------------------------------------
	// Skybox
	// -------------------------------------------------------------------

	struct SkyboxDesc {
		Vec4  solidColor = Vec4(0.4f, 0.6f, 0.9f, 1.0f);
		TextureHandle cubemap;
		TextureHandle skyCubeTex; ///< Cubemap texture from createCubemapTexture.
		Vec4  corners[8] = {
			Vec4(0.3f,0.5f,0.9f,1), Vec4(0.4f,0.6f,0.95f,1), Vec4(0.4f,0.6f,0.95f,1), Vec4(0.3f,0.5f,0.9f,1),
			Vec4(0.5f,0.55f,0.6f,1), Vec4(0.5f,0.55f,0.6f,1), Vec4(0.5f,0.55f,0.6f,1), Vec4(0.5f,0.55f,0.6f,1),
		};
	};

	/// @brief Set skybox parameters (call before beginFrame).
	void setSkybox(const SkyboxDesc& desc);

	/// @brief Remove skybox.
	void clearSkybox();

	/// @brief A single cubemap face descriptor with optional flipping.
	struct CubemapFace {
		::EnderEngine::Utilities::DecodedImage image;
		bool   flipVertical   = false;
		bool   flipHorizontal = false;
	};

	/// @brief Create cubemap from 6 faces with per-face flip control. Order: right/left/top/bottom/front/back.
	Result<TextureHandle, RenderError> createCubemapTexture(const CubemapFace faces[6]);

protected:
	Result<void, CoreError> onInitialize() override;
	void onShutdown() override;
	void onUpdate(F64 deltaTime) override;
	bool onRecover() override;

private:
	struct RenderBackend;
	Uptr<RenderBackend> m_backend;
};

EE_NAMESPACE_RENDERING_END
