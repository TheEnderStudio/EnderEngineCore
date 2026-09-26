#include <Engine/Core/Core.hpp>
#include <Engine/Core/Log.hpp>
#include <Engine/Core/Extension.hpp>
#include <Engine/Platform/Window.hpp>
#include <Engine/Rendering/RenderSubsystem.hpp>
#include <Engine/Rendering/DebugUISubsystem.hpp>
#include <Engine/Rendering/Render2DSubsystem.hpp>
#include <Engine/Input/InputSubsystem.hpp>
#include <Engine/Input/InputState.hpp>
#include <Engine/Jobs/JobSubsystem.hpp>
#include <Engine/Jobs/JobEvents.hpp>
#include <Engine/PostProcess/PostProcessSubsystem.hpp>
#include <Engine/UI/UISubsystem.hpp>
#include <Engine/Utilities/FontLoader.hpp>
#include <Engine/Utilities/ImageLoader.hpp>
#include <Engine/Physics/PhysicsBodySubsystem.hpp>
#include <Engine/Physics/PhysicsWorldSubsystem.hpp>
#include <Engine/Audio/AudioSubsystem.hpp>
#include <Engine/Resource/ResourcesManager.hpp>
#include <Engine/Rendering/ShadowSubsystem.hpp>
#include <Engine/Rendering/ComputeSubsystem.hpp>
#include <Engine/Rendering/RayTracingSubsystem.hpp>
#include <Engine/Rendering/DenoisingSubsystem.hpp>
#include <Engine/Rendering/MeshShaderSubsystem.hpp>
#include <Engine/Rendering/RenderPipeline.hpp>

#include <HelloWorld/HelloWorldExt.hpp>
#include <EngineExt/AdaptiveMusic.hpp>

#include <GLFW/glfw3.h>
#include <fstream>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <atomic>
#include <filesystem>
#include <random>
#include <ranges>

using namespace EnderEngine;
using namespace EnderEngine::Platform;
using namespace EnderEngine::Rendering;
using namespace EnderEngine::Input;
using namespace EnderEngine::Jobs;
using namespace EnderEngine::PostProcess;
using namespace EnderEngine::UI;
using namespace EnderEngine::Utilities;
using namespace EnderEngine::Physics;
using namespace EnderEngine::Audio;
using namespace EnderEngine::Extensions;
using namespace EnderEngine::Extensions::AdaptiveMusic;

// ===================================================================
// FlyCamera
// ===================================================================

struct FlyCamera {
	Vec3 pos = Vec3(0, 3, 4);
	F32 yaw = -90.0f, pitch = -15.0f;
	F32 moveSpeed = 10.0f, lookSpeed = 0.15f;
	bool boundToBody = true;   // F5 toggles between bound / free-fly
	Vec3 freePos = Vec3(0, 2, 4);

	void update(const InputSubsystem& input, F32 dt, PhysicsBodySubsystem* physics = nullptr) {
		F32 dx = 0.0f, dy = 0.0f;
		if (!input.isCursorVisible()) {
			dx = (F32)input.inputState().mouseDeltaX() + input.inputState().gamepadAxis(0, GamepadAxis::RightX);
			dy = (F32)input.inputState().mouseDeltaY() + input.inputState().gamepadAxis(0, GamepadAxis::RightY);
		}
		yaw += dx * lookSpeed;
		pitch -= dy * lookSpeed;
		pitch = Clamp(pitch, -89.0f, 89.0f);
		F32 yr = glm::radians(yaw), pr = glm::radians(pitch);
		Vec3 front(cos(pr) * cos(yr), 0, cos(pr) * sin(yr)); // horizontal only for movement
		Vec3 right = glm::normalize(glm::cross(front, Vec3(0, 1, 0)));

		Vec3 move(0);
		move += front * (input.actionMap().axisValue("MoveX") + input.inputState().gamepadAxis(0, GamepadAxis::LeftX));
		move += right * (input.actionMap().axisValue("MoveY") + input.inputState().gamepadAxis(0, GamepadAxis::LeftY));
		if (glm::dot(move, move) > 0.0001f) move = glm::normalize(move);

		if (physics && boundToBody) {
			F32 curVelY = physics->getLinearVelocity(cameraBody).y;
			Vec3 targetVel(move.x * moveSpeed, curVelY, move.z * moveSpeed);
			static bool jumpPressed = false;
			bool jumpNow = input.actionMap().isButtonDown("MoveUp") || input.inputState().isGamepadButtonDown(0, GamepadButton::A);
			F32 curY = physics->getPosition(cameraBody).y;
			if (jumpNow && !jumpPressed) {
				targetVel.y = 4.0f;
			}
			jumpPressed = jumpNow;
			physics->setLinearVelocity(cameraBody, targetVel);
			pos = physics->getPosition(cameraBody) + Vec3(0, 0.8f, 0);
			pos = physics->getPosition(cameraBody) + Vec3(0, 1.7f, 0);
		}
		else {
			// Free fly: full 3D movement
			Vec3 flyFront(cos(pr) * cos(yr), sin(pr), cos(pr) * sin(yr));
			Vec3 flyRight = glm::normalize(glm::cross(flyFront, Vec3(0, 1, 0)));
			Vec3 flyMove(0);
			flyMove += flyFront * (input.actionMap().axisValue("MoveX") + input.inputState().gamepadAxis(0, GamepadAxis::LeftX));
			flyMove += flyRight * (input.actionMap().axisValue("MoveY") + input.inputState().gamepadAxis(0, GamepadAxis::LeftY));
			if (input.actionMap().isButtonDown("MoveUp") || input.inputState().isGamepadButtonDown(0, GamepadButton::A))     flyMove.y += 1;
			if (input.actionMap().isButtonDown("MoveDown") || input.inputState().isGamepadButtonDown(0, GamepadButton::B))     flyMove.y -= 1;
			if (glm::dot(flyMove, flyMove) > 0.0001f) flyMove = glm::normalize(flyMove);
			freePos += flyMove * moveSpeed * dt;
			pos = freePos;
		}
	}

	RigidBodyHandle cameraBody = InvalidRigidBody;
	void bindPhysics(PhysicsBodySubsystem& physics) {
		RigidBodyDesc rd;
		rd.type = RigidBodyType::Dynamic;
		rd.position = pos;
		rd.mass = 1.0f;
		rd.linearDamping = 0.2f;
		rd.enableGravity = true;
		rd.friction = 0.5f;
		rd.restitution = 0.0f;
		cameraBody = physics.createRigidBody(rd);
		ColliderDesc cd;
		cd.shape = ColliderShape::Box;
		cd.halfExtents = Vec3(0.4f, 0.8f, 0.4f);
		physics.createCollider(cameraBody, cd);
		physics.setAngularLock(cameraBody, AngularLockFlag::LockX | AngularLockFlag::LockZ);
	}

	void toggleBind(PhysicsBodySubsystem& physics) {
		boundToBody = !boundToBody;
		if (boundToBody) {
			// Teleport body to camera position
			physics.setPosition(cameraBody, freePos);
			physics.setLinearVelocity(cameraBody, Vec3(0));
			physics.setAngularLock(cameraBody, AngularLockFlag::LockX | AngularLockFlag::LockZ);
			pos = freePos;
		}
		else {
			freePos = pos;
			physics.setLinearVelocity(cameraBody, Vec3(0));
			physics.setAngularLock(cameraBody, AngularLockFlag::None);
		}
	}

	CameraDesc toDesc(UInt32 w, UInt32 h) const {
		CameraDesc d; d.pos = pos; d.w = (F32)w; d.h = (F32)h;
		F32 yr = glm::radians(yaw), pr = glm::radians(pitch);
		d.target = pos + Vec3(cos(pr) * cos(yr), sin(pr), cos(pr) * sin(yr));
		d.up = Vec3(0, 1, 0);
		return d;
	}
};

// ===================================================================
// Main
// ===================================================================

#ifdef EE_DEBUG
int main(int argc, char** argv)
#else
//int main(int argc, char** argv)
int WinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int)
#endif
{
	// --- Engine ---
	Engine::initialize();

	// --- Resources Manager ---
	auto& resManager = ResourcesManager::getInstance();
	resManager.mount("GameData2.index", "mypassword");

	// --- Load Extension ---
	auto* extension = ExtensionLoader::load("HelloWorldExt");

	// --- Jobs ---
	JobSubsystem jobs;
	jobs.initialize();

	// --- Platform window ---
	Window window;
	WindowDesc wd; wd.title = "EnderEngine Demo"; wd.width = 1280; wd.height = 720;
	window.open(wd);
	window.setCursorVisible(false);

	// --- Input ---
	InputDesc inDesc;
	inDesc.windowHandle.handle = window.nativeHandle();
	InputSubsystem input(inDesc);
	input.initialize();

	input.actionMap().bindAxis("MoveX", KeyCode::W, KeyCode::S);
	input.actionMap().bindAxis("MoveY", KeyCode::D, KeyCode::A);
	input.actionMap().bindButton("MoveUp", KeyCode::Space);
	input.actionMap().bindButton("MoveDown", KeyCode::LeftShift);

	// --- Renderer ---
	RenderSubsystem renderer;
	renderer.setWindow(window.glfwWindow());
	renderer.setBackend(RenderBackendType::D3D12);
	renderer.setMSAASampleCount(16);
	{
		auto rerr = renderer.initialize();
		if (rerr.isErr()) {
			EError("Renderer initialization failed: {} - aborting.", ToString(rerr.error()));
			input.shutdown();
			jobs.shutdown();
			window.close();
			Engine::shutdown();
			return 1;
		}
	}

	// --- Ray tracing capability report ---
	{
		const auto rtCaps = renderer.rayTracingCaps();
		if (rtCaps != RayTracingCaps::None) {
			EInfo("Ray tracing supported: inline={} standalone={} | maxRecursion={} maxInstancesPerTLAS={}",
				renderer.supportsInlineRayTracing(), renderer.supportsStandaloneRayTracing(),
				renderer.maxRayRecursionDepth(), renderer.maxInstancesPerTLAS());
		}
		else {
			EInfo("Ray tracing NOT supported on this GPU/driver - running rasterization only.");
		}
	}

	// --- Debug UI ---
	DebugUISubsystem debugUI;
	debugUI.setDevice(renderer.getDevice());
	debugUI.setContext(renderer.getContext());
	debugUI.setWindowHandle(window.nativeHandle());
	auto error = debugUI.initialize();
	if (error.isErr()) { EError("DebugUI init failed: {}", ToString(error.error())); }

	// --- PostProcess ---
	PostProcessSubsystem postProcess;
	postProcess.setDevice(renderer.getDevice());
	postProcess.setContext(renderer.getContext());
	postProcess.setSwapChain(renderer.getSwapChain());
	postProcess.initialize();
	PostProcessConfig ppCfg; ppCfg.toneMap.mode = ToneMapMode::ACES; ppCfg.bloom.intensity = 0.3f;
	ppCfg.sampleCount = renderer.msaaSamples();
	postProcess.setConfig(ppCfg);

	// --- Render 2D ---
	Render2DSubsystem render2d;
	render2d.setDevice(renderer.getDevice());
	render2d.setContext(renderer.getContext());
	render2d.initialize();
	UInt32 fw, fh;
	window.getFramebufferSize(fw, fh);
	render2d.setScreenSize(fw, fh);
	render2d.setAfterPostProcess(false);

	// --- UI ---
	UISubsystem ui;
	{
		UInt32 ww, wh;
		window.getWindowSize(ww, wh);
		ui.initialize(&render2d, ww, wh);
	}
	// GLFW character input routing
	glfwSetWindowUserPointer(static_cast<GLFWwindow*>(window.glfwWindow()), &ui);
	glfwSetCharCallback(static_cast<GLFWwindow*>(window.glfwWindow()),
		[](GLFWwindow* win, unsigned int cp) {
			auto* u = static_cast<UISubsystem*>(glfwGetWindowUserPointer(win));
			u->inputChar(static_cast<char>(cp));
		});

	// Custom post-process shader (chromatic aberration + scanlines, retro CRT look)
	static const char* g_CustomPS = R"(
Texture2D g_HDR : register(t0); Texture2D g_Bloom : register(t1); SamplerState g_HDR_sampler : register(s0);
cbuffer PP : register(b0) { uint g_Mode; float g_Exp; float g_Gamma; float g_Vig; float g_Sat; float g_BloomI; float g_BloomT; float g_BloomR; };
float3 Reinhard(float3 c) { return c/(1.0+c); }
float3 Uncharted2(float3 c) { float3 a=c*(c*0.15+0.025)+0.004; float3 b=c*(c*0.15+0.5)+1.0; return a/b; }
float3 ACES(float3 c) { float3 a=c*(c*2.51+0.03); float3 b=c*(c*2.43+0.59)+0.14; return saturate(a/b); }
struct PSIn { float4 Pos : SV_POSITION; float2 UV : TEXCOORD0; };
float4 main(PSIn i) : SV_TARGET {
    // Chromatic aberration: sample R/G/B at slightly different UV offsets
    float2 d = (i.UV - 0.5) * 0.015;
    float r = g_HDR.Sample(g_HDR_sampler, i.UV + d).r;
    float g = g_HDR.Sample(g_HDR_sampler, i.UV).g;
    float b = g_HDR.Sample(g_HDR_sampler, i.UV - d).b;
    float3 hdr = float3(r,g,b) * g_Exp + g_Bloom.Sample(g_HDR_sampler, i.UV).rgb * g_BloomI;

    float3 c;
    if(g_Mode==1) c=Reinhard(hdr); else if(g_Mode==2) c=Uncharted2(hdr); else if(g_Mode==3) c=ACES(hdr); else c=hdr;

    // Scanlines
    float scanline = sin(i.UV.y * 800.0) * 0.03 + 0.97;
    c *= scanline;

    // Vignette
    float2 uv2 = i.UV * 2.0 - 1.0; c *= saturate(1.0 - dot(uv2,uv2) * g_Vig);

    // Gamma
    c = pow(max(c, 0.0), 1.0 / g_Gamma);

    // Saturation
    float lum2 = dot(c, float3(0.299,0.587,0.114));
    c = lerp(float3(lum2,lum2,lum2), c, g_Sat);

    return float4(c, 1.0);
}
)";

	// --- Camera ---
	FlyCamera fly;
	UInt32 ww = 1280, wh = 720;
	window.getFramebufferSize(ww, wh);
	postProcess.resize(ww, wh);
	auto camHandle = renderer.createCamera(fly.toDesc(ww, wh));
	if (camHandle.isOk()) renderer.setActiveCamera(camHandle.value());

	// --- Lighting ---
	LightHandle sunHandle;
	F32 sunYaw = -30.0f, sunPitch = 45.0f;
	{
		Vec3 sunDir = Vec3(cos(glm::radians(sunYaw)) * cos(glm::radians(sunPitch)),
			-sin(glm::radians(sunPitch)),
			sin(glm::radians(sunYaw)) * cos(glm::radians(sunPitch)));
		LightDesc sun; sun.type = LightType::Directional; sun.color = Vec3(1.0f, 0.98f, 0.9f); sun.intensity = 1.0f;
		sun.dir = glm::normalize(sunDir);
		auto r = renderer.createLight(sun); if (r.isOk()) sunHandle = r.value();
	}

	// --- Shadows ---
	ShadowSubsystem shadow;
	shadow.attachToRenderer(&renderer);
	shadow.setConfig(ShadowConfig{ .enabled = true, .resolution = 8192, .numCascades = 4, .partitioning = 0.95f });
	shadow.initialize();

	// --- Compute ---
	ComputeSubsystem compute;
	compute.attachToRenderer(&renderer);
	compute.initialize();

	// --- Ray tracing (hybrid, M3) ---
	RayTracingSubsystem rayTracing;
	rayTracing.attachToRenderer(&renderer);
	// --- NRD denoiser (REBLUR, GPU compute) ---
	DenoisingSubsystem denoising;
	denoising.attachToRenderer(&renderer);
	// --- Mesh shader (GPU-driven amplification + mesh pipeline, LOD) ---
	MeshShaderSubsystem meshShader;
	meshShader.attachToRenderer(&renderer);
	bool hybridRT = false; // F9 toggles between rasterization and hybrid ray tracing
	UInt32 rtDrawMode = 0; // compose debug mode (0=shaded ... 5=Fresnel)
	UInt32 rtShadowPCF = 4; // PCF shadow samples (1..16)
	F32 rtAoRadius = 1.5f;   // AO ray length
	UInt32 rtAoSamples = 4;  // AO rays per pixel (0 = off)
	F32 rtLightSize = 0.05f; // PCSS light size (0 = fixed-cone PCF)
	F32 rtReflectionBlur = 0.7f; // GGX reflection spread scale (0 = mirror, 1 = full roughness)
	UInt32 rtMaxBounces = 1;     // max reflection bounces (0 = single, 1 = two-bounce)
	F32 rtBounceRoughness = 1.0f; // second-bounce roughness threshold (1.0 = force all surfaces)
	bool rtDenoise = true;       // temporal + spatial denoiser (SVGF-lite) / OIDN GPU path
	F32 rtDenoiseStrength = 0.9f; // temporal history weight (0 = off, 1 = full history)
	bool rtOIDNAsync = true;     // OIDN async pipeline (1-frame latency) vs sync (0 latency)
	int  rtDenoiserSel = 0;      // 0 = Auto, 1 = NRD, 2 = OIDN, 3 = Temporal
	bool msTestGrid = false;     // M1 mesh shader test grid (GPU-driven AS/MS pipeline)
	bool msFrustumCull = true;   // toggle frustum culling in the test amplification shader
	bool msFurinaScene = true;  // M2: draw the 1000 Furina bodies through the mesh shader path
	bool msMeshesRegistered = false;
	// Index ranges into the registered mesh list, one per scene object type.
	UInt32 msTerrIdx = 0, msTerrCount = 0, msWallIdx = 0, msWallCount = 0;
	UInt32 msCubeIdx = 0, msCubeCount = 0, msFurinaIdx = 0, msFurinaCount = 0;
	UInt32 msMeshCount = 0, msGroupCount = 0;
	Vector<MeshHandle> msRegisteredMeshes;
	// Appends one instanced draw group for a mesh range (no-op when empty).
	auto msAddGroup = [&](Vector<MeshShaderSubsystem::MeshDrawGroup>& groups, UInt32 idx, UInt32 count, Vector<Mat4> instances) {
		if (count == 0 || instances.empty()) return;
		MeshShaderSubsystem::MeshDrawGroup g;
		g.meshIds.resize(count);
		for (UInt32 i = 0; i < count; ++i) g.meshIds[i] = idx + i;
		g.instanceMatrices = std::move(instances);
		groups.push_back(std::move(g));
	};
	F32  msLodScale = 0.25f;      // M3: LOD selection threshold, in pixels of projected error
	F32  msInstBudget = 1000.0f;   // debug: instance cap for the mesh shader scene (start small - opening at 1000 TDRs the GPU)
	F32  msClusterBudget = 262144.0f; // hard cap on cluster mesh groups dispatched per frame
	int  msDebugTri = 0;         // MS diagnostic: 0 = off, 1 = fixed triangle, 2 = albedo only
	int  msMeshFilter = 5;       // MS diagnostic: 0..4 = only that mesh, >4 = all
	UInt32 rtReflectionSamples = 8; // GGX VNDF reflection ray budget per pixel (1..8); smooth surfaces spend fewer
	F32 rtReflectionCone = 1.0f;    // ray-cone prefilter strength (0 = offset-free LOD0 sampling, 1 = lobe-matched mip)
	UInt32 rtReflectionShadowPCF = 1; // shadow rays per reflection hit (1 = single ray; >1 = PCSS+PCF)
	float rtResScale = 0.5f; // RT resolution scale (0.5 / 1.0)
	bool rtResAuto = true;   // auto-pick the scale from the distance to the nearest object
	TextureHandle gbufColor, gbufNormal, gbufEmissive, gbufDepth, rtTex;
	// RT-resolution albedo/normal written by the RT shader for the denoiser.
	TextureHandle rtAlbedo, rtNormal;
	// Single-sample resolve targets for the MSAA G-buffer (read by RT/compose).
	TextureHandle resColor, resNormal, resEmissive, resDepth;
	// (Re)create the ray traced output texture at the current rtResScale.
	// RGBA32F: the Open Image Denoise GPU path consumes 32-bit float images
	// (OIDN has no 16-bit format), so the shared buffers and the output match.
	// rtAlbedo/rtNormal are the RT-resolution albedo/normal the RT shader writes
	// for the denoiser (same size as rtTex).
	auto createRTTex = [&](UInt32 w, UInt32 h) {
		renderer.destroyTexture(rtTex);
		renderer.destroyTexture(rtAlbedo);
		renderer.destroyTexture(rtNormal);
		rtTex = rtAlbedo = rtNormal = TextureHandle{};
		TextureDesc td;
		td.fmt = TextureFormat::RGBA32_Float; td.asDepthStencil = false; td.asUAV = true;
		td.w = std::max<UInt32>(1, (UInt32)(w * rtResScale));
		td.h = std::max<UInt32>(1, (UInt32)(h * rtResScale));
		if (auto r = renderer.createTexture(td); r.isOk()) rtTex = r.value();
		if (auto r = renderer.createTexture(td); r.isOk()) rtAlbedo = r.value();
		if (auto r = renderer.createTexture(td); r.isOk()) rtNormal = r.value();
		};
	// (Re)create the window-sized G-buffer (always full resolution) + RT output.
	// The G-buffer targets are MSAA (same sample count as the renderer, so the
	// hybrid path gets real antialiasing at geometry edges); the RT trace and
	// compose passes read their single-sample resolves.
	auto createGBufferTextures = [&](UInt32 w, UInt32 h) {
		renderer.destroyTexture(gbufColor);
		renderer.destroyTexture(gbufNormal);
		renderer.destroyTexture(gbufEmissive);
		renderer.destroyTexture(gbufDepth);
		renderer.destroyTexture(resColor);
		renderer.destroyTexture(resNormal);
		renderer.destroyTexture(resEmissive);
		renderer.destroyTexture(resDepth);
		gbufColor = gbufNormal = gbufEmissive = gbufDepth = TextureHandle{};
		resColor = resNormal = resEmissive = resDepth = TextureHandle{};
		const UInt8 ms = renderer.msaaSamples();
		TextureDesc td;
		td.w = w; td.h = h; td.fmt = TextureFormat::RGBA8_UNorm_SRGB; td.asRenderTarget = true;
		td.sampleCount = ms;
		if (auto r = renderer.createTexture(td); r.isOk()) gbufColor = r.value();
		td.fmt = TextureFormat::RGBA16_Float;
		if (auto r = renderer.createTexture(td); r.isOk()) gbufNormal = r.value();
		if (auto r = renderer.createTexture(td); r.isOk()) gbufEmissive = r.value();
		td.fmt = TextureFormat::D32_Float; td.asRenderTarget = false; td.asDepthStencil = true;
		if (auto r = renderer.createTexture(td); r.isOk()) gbufDepth = r.value();
		// Single-sample resolve targets. Color/normal are RGBA32F (OIDN consumes
		// 32-bit float images; the resolve decodes the sRGB MSAA color to linear);
		// emissive stays RGBA16F; depth is R32F because a depth format cannot be
		// a UAV (the resolve writes the min depth).
		td.asDepthStencil = false; td.asRenderTarget = false; td.asUAV = true; td.sampleCount = 1;
		td.fmt = TextureFormat::RGBA32_Float;
		if (auto r = renderer.createTexture(td); r.isOk()) resColor = r.value();
		if (auto r = renderer.createTexture(td); r.isOk()) resNormal = r.value();
		td.fmt = TextureFormat::RGBA16_Float;
		if (auto r = renderer.createTexture(td); r.isOk()) resEmissive = r.value();
		td.fmt = TextureFormat::R32_Float;
		if (auto r = renderer.createTexture(td); r.isOk()) resDepth = r.value();
		createRTTex(w, h);
		};
	{
		if (renderer.supportsInlineRayTracing()) {
			auto r = rayTracing.initialize();
			if (r.isErr()) { EError("RayTracing init failed: {}", ToString(r.error())); }
			auto dr = denoising.initialize();
			if (dr.isErr()) { EError("Denoising init failed: {}", ToString(dr.error())); }
		}
		else {
			EInfo("Hybrid ray tracing disabled: device does not support inline ray tracing.");
		}
		createGBufferTextures(fw, fh);
	}
	{
		auto msr = meshShader.initialize();
		if (msr.isErr()) {
			// Expected when the device lacks mesh shader support; the DebugUI
			// shows the actual state (supported / disabled).
			if (!renderer.supportsMeshShaders()) EInfo("Mesh shaders: device does not support them - path disabled.");
			else EError("MeshShader init failed: {}", ToString(msr.error()));
		}
	}

	// ---- Render pipeline ----
	// Owns the entire frame as an explicit pass graph: shadows, geometry
	// submission and the frame tail. There is no inline fallback path any more,
	// so a build failure means nothing gets drawn and is reported loudly.
	RenderPipelineContext rpContext;
	JobExecutor rpExecutor{ jobs };
	Uptr<RenderPipeline> renderPipeline;
	{
		rpContext.renderer = &renderer;
		rpContext.shadow = &shadow;
		rpContext.meshShader = &meshShader;
		rpContext.rayTracing = &rayTracing;
		rpContext.denoising = &denoising;
		rpContext.postProcess = &postProcess;
		rpContext.ui = &ui;
		rpContext.debugUI = &debugUI;
		renderPipeline = std::make_unique<RenderPipeline>(rpExecutor, rpContext, "DemoFrame");
		if (!renderPipeline->isBuilt()) {
			EError("RenderPipeline: the pass graph failed to build; the pipeline path is unavailable.");
			renderPipeline.reset();
		}
		else {
			EInfo("RenderPipeline ready:\n{}", renderPipeline->describe());
		}
	}

	// ---- Emissive showcase: a self-illuminated cube that glows in the direct
	// view and appears lit inside ray traced reflections. ----
	MeshHandle emissiveCubeMesh;
	MaterialHandle emissiveCubeMat;
	static const Transform emissiveCubeTf{ .position = Vec3(0.0f, 2.5f, 4.0f) };
	{
		MaterialDesc mat;
		mat.name = "EmissiveLamp";
		mat.baseColorFactor = Vec4(0.92f, 0.92f, 0.86f, 1.0f);
		mat.metallicFactor = 0.0f;
		mat.roughnessFactor = 0.35f;
		mat.emissiveFactor = Vec3(2.6f, 1.0f, 0.2f); // warm lamp glow (HDR > 1)
		auto cr = renderer.createMaterial(mat, renderer.defaultPSO());
		if (cr.isOk()) emissiveCubeMat = cr.value();

		auto makeCube = [&](F32 s) -> MeshDesc {
			MeshDesc md;
			auto pushFace = [&](Vec3 n, Vec3 u, Vec3 v) {
				UInt32 base = (UInt32)md.vertices.size();
				for (int i = 0; i < 4; ++i) {
					F32 uu = (i == 1 || i == 2) ? 1.0f : 0.0f;
					F32 vv = (i == 2 || i == 3) ? 1.0f : 0.0f;
					Vertex vert;
					vert.position = (n * 0.5f + u * (uu - 0.5f) + v * (vv - 0.5f)) * s;
					vert.normal = n;
					vert.texCoord = Vec2(uu, vv);
					vert.tangent = Vec4(u, 1.0f);
					md.vertices.push_back(vert);
				}
				UInt32 idx[6] = { base, base + 1, base + 2, base, base + 2, base + 3 };
				for (UInt32 k = 0; k < 6; ++k) md.indices.push_back(idx[k]);
				};
			// u x v == n keeps the CCW (front-facing) winding used by the renderer
			// and by the RT ray queries (back-face culling).
			pushFace(Vec3(1, 0, 0), Vec3(0, 0, -1), Vec3(0, 1, 0));
			pushFace(Vec3(-1, 0, 0), Vec3(0, 0, 1), Vec3(0, 1, 0));
			pushFace(Vec3(0, 1, 0), Vec3(1, 0, 0), Vec3(0, 0, -1));
			pushFace(Vec3(0, -1, 0), Vec3(1, 0, 0), Vec3(0, 0, 1));
			pushFace(Vec3(0, 0, 1), Vec3(1, 0, 0), Vec3(0, 1, 0));
			pushFace(Vec3(0, 0, -1), Vec3(-1, 0, 0), Vec3(0, 1, 0));
			SubMesh sub; sub.indexOffset = 0; sub.indexCount = (UInt32)md.indices.size(); sub.vertexOffset = 0;
			sub.material = emissiveCubeMat;
			md.subMeshes.push_back(sub);
			return md;
			};
		MeshDesc md = makeCube(1.2f);
		auto mr = renderer.createMesh(md);
		if (mr.isOk()) emissiveCubeMesh = mr.value();
	}

	// GPU culling buffers
	ComputeBuf cullInstBuf = compute.createStructuredBuffer(1000, sizeof(CullingInstance), true);
	ComputeBuf cullVisBuf = compute.createStructuredBuffer(1000, sizeof(UInt32), true);
	ComputeBuf cullCBBuf = compute.createConstantBuffer(sizeof(FrustumPlanes) + sizeof(UInt32) * 8, true);
	ComputeBuf cullStgBuf = compute.createStagingBuffer(1000 * sizeof(UInt32));
	Vector<UInt32> visibleFlags(1000, 1);

	// Indirect draw buffers (kept for future use)
	ComputeBuf worldMatBuf = compute.createStructuredBuffer(1000, sizeof(Mat4), false);
	ComputeBuf indicesBuf = compute.createStructuredBuffer(1000, sizeof(UInt32), true);
	ComputeBuf argsBuf = compute.createIndirectArgsBuffer(8);
	ComputeBuf counterBuf = compute.createStructuredBuffer(1, sizeof(UInt32), true);
	ComputeSRV wmSRV = compute.getBufferSRV(worldMatBuf);
	ComputeSRV idxSRV = compute.getBufferSRV(indicesBuf);

	// Skybox with cubemap from 6 face images
	TextureHandle skyCubeTexHandle;
	// Skybox state shared between the renderer and the ray tracer (reflections).
	static int s_skyMode = 0;
	static Vec4 s_skyCorners[8] = {
		Vec4(0.3f, 0.5f, 0.9f, 1), Vec4(0.4f, 0.55f, 0.95f, 1),
		Vec4(0.4f, 0.55f, 0.95f, 1), Vec4(0.3f, 0.5f, 0.9f, 1),
		Vec4(0.5f, 0.55f, 0.6f, 1), Vec4(0.5f, 0.55f, 0.6f, 1),
		Vec4(0.5f, 0.55f, 0.6f, 1), Vec4(0.5f, 0.55f, 0.6f, 1),
	};
	{
		RenderSubsystem::CubemapFace skyFaces[6] = {
			{loadImage(ERes("skybox/right.png")), false, true},
			{loadImage(ERes("skybox/back.png")), false, true},
			{loadImage(ERes("skybox/top.png")), false, true},
			{loadImage(ERes("skybox/bottom.png")), false, true},
			{loadImage(ERes("skybox/front.png")), false, true},
			{loadImage(ERes("skybox/left.png")), false, true},
		};
		auto cr = renderer.createCubemapTexture(skyFaces);
		if (cr.isOk()) {
			skyCubeTexHandle = cr.value();
			RenderSubsystem::SkyboxDesc sd; sd.skyCubeTex = skyCubeTexHandle;
			renderer.setSkybox(sd);
			EInfo("Skybox: cubemap loaded from skybox/");
		}
	}

	PSOHandle pso;
	{ PipelineStateDesc pd; pd.name = "DemoPSO"; auto r = renderer.createPipelineState(pd); if (r.isOk()) pso = r.value(); }

	std::atomic<bool> modelLoadCompleted{ false };
	Object modelObj;
	jobs.registerObject(modelObj);
	jobs.subscribe<JobCompletedEvent>(modelObj, [&](const JobCompletedEvent& e) {
		if (e.owner != modelObj.guid()) {
			return;
		}
		EInfo("Model loaded and took {} ms!", (F32)e.durationNs / 1000000.0f);
		modelLoadCompleted.store(true, std::memory_order_release);
		});
	jobs.subscribe<JobFailedEvent>(modelObj, [&](const JobFailedEvent& e) {
		if (e.owner != modelObj.guid()) {
			return;
		}
		EError("Model loaded failed!");
		});
	auto mh = renderer.loadModelAsync(ERes("Furina.glb"), modelObj, jobs);
	if (mh.isErr()) {
		EError("Model load failed.");
	}
	Vector<MeshHandle> model{};

	// --- Physics ---
	PhysicsWorldSubsystem physicsWorld;
	physicsWorld.initialize(PhysicsWorldDesc{});

	PhysicsBodySubsystem physicsBodies;
	physicsBodies.initialize();
	physicsBodies.attachToWorld(&physicsWorld);

	// --- Audio ---
	AudioSubsystem audio;
	audio.initialize();

	// Collision audio: any dynamic↔static contact plays Collision.wav at contact point
	Object physicsObj;
	physicsWorld.registerObject(physicsObj);
	auto subId = physicsWorld.subscribe<PhysicsContactEvent>(physicsObj, [&](const PhysicsContactEvent& e) {
		if (!e.pair.aIsStatic && !e.pair.bIsStatic) return;
		if (e.pair.aIsStatic && e.pair.bIsStatic) return;
		audio.playFile(ERes("Collision.wav"), e.pair.position, 1.0f);
		});

	audio.bindScene(physicsWorld.scene());

	// --- AdaptiveMusic (extension) ---
	static const char* g_AdaptiveScript = R"(
; EnderEngine AdaptiveMusic demo script
PLAY TRACK intro VOL 0.8 FADEIN SEC 2
WAIT TRACK intro
;PLAY TRACK mid VOL 0.7 FADEIN SEC 3
;WAIT TRACK mid FADE
RANDOM climax1 climax2 climax3
climax1:
FADE OUT TRACK mid SEC 3
PLAY TRACK climax1 VOL 0.9 FADEIN SEC 2
WAIT TRACK climax1 FADE
WAIT TRACK climax1
PLAY TRACK end VOL 0.8 FADEIN SEC 2
WAIT TRACK end
HALT
climax2:
FADE OUT TRACK mid SEC 3
PLAY TRACK climax2 VOL 0.9 FADEIN SEC 2
WAIT TRACK climax2 FADE
WAIT TRACK climax2
PLAY TRACK end VOL 0.8 FADEIN SEC 2
WAIT TRACK end
HALT
climax3:
FADE OUT TRACK mid SEC 3
PLAY TRACK climax3 VOL 0.9 FADEIN SEC 2
WAIT TRACK climax3 FADE
WAIT TRACK climax3
PLAY TRACK end VOL 0.8 FADEIN SEC 2
WAIT TRACK end
HALT
)";

	Player musicPlayer;
	HighLevelController musicCtl;
	musicCtl.attachPlayer(&musicPlayer);
	if (!musicPlayer.initialize()) {
		EError("Player initialization failed");
	}

	TrackHandle introH = InvalidTrackHandle, midH = InvalidTrackHandle,
		c1H = InvalidTrackHandle, c2H = InvalidTrackHandle, c3H = InvalidTrackHandle, endH = InvalidTrackHandle;
	auto bindClip = [&](const char* name, const ResPath& path, TrackHandle& out) {
		auto clip = audio.decode(path);
		if (clip.isErr()) { EError("Failed to decode {}: {}", path.string(), ToString(clip.error())); return; }
		out = musicPlayer.addTrack(clip.value());
		musicCtl.bindTrack(name, out);
		const auto& ac = clip.value();
		F32 dur = (ac.channels > 0 && ac.sampleRate > 0)
			? (F32)ac.samples.size() / (F32)(ac.channels * ac.sampleRate) : 0.0f;
		EInfo("Track '{}' bound as {} ({}s)", name, out, dur);
		};
	bindClip("intro", ERes("bgm/Intro.wav"), introH);
	bindClip("mid", ERes("bgm/Mid.wav"), midH);
	bindClip("climax1", ERes("bgm/Climax1.wav"), c1H);
	bindClip("climax2", ERes("bgm/Climax2.wav"), c2H);
	bindClip("climax3", ERes("bgm/Climax3.wav"), c3H);
	bindClip("end", ERes("bgm/End.wav"), endH);

	HighLevelController::CompileError amErr;
	if (!musicCtl.compile(g_AdaptiveScript, &amErr)) {
		EError("Script compile failed at line {}: {}", amErr.lineNum, amErr.message);
	}
	else {
		musicCtl.start();
		EInfo("Script started");
	}

	// Bind camera to physics (collider capsule, no gravity)
	fly.bindPhysics(physicsBodies);

	// 1000 physics bodies at 10x10x10 grid with random rotations
	auto transv = Vector<Transform>(1000, Transform{});
	Vector<RigidBodyHandle> physHandles;
	physHandles.reserve(1000);
	{
		std::random_device rd;
		std::mt19937 gen(rd());
		std::uniform_real_distribution<F32> angleDist(0.0f, glm::two_pi<F32>());
		std::uniform_real_distribution<F32> axisDist(-1.0f, 1.0f);

		for (UInt16 x = 0; x < 10; x++) {
			for (UInt16 y = 0; y < 10; y++) {
				for (UInt16 z = 0; z < 10; z++) {
					RigidBodyDesc rd;
					rd.type = RigidBodyType::Dynamic;
					rd.position = Vec3(x * 2, y * 2 + 2, z * 2);
					rd.mass = 1.0f;
					rd.linearDamping = 0.1f;
					// Random rotation
					Vec3 axis = glm::normalize(Vec3(axisDist(gen), axisDist(gen), axisDist(gen)));
					rd.rotation = glm::angleAxis(angleDist(gen), axis);

					Transform trans;
					trans.position = rd.position;
					trans.rotation = rd.rotation;

					auto h = physicsBodies.createRigidBody(rd);
					ColliderDesc cd;
					cd.shape = ColliderShape::Capsule;
					cd.radius = 0.3f;
					cd.height = 1.7f;
					cd.localRot = glm::angleAxis(glm::radians(90.0f), Vec3(0, 0, 1));
					physicsBodies.createCollider(h, cd);
					physHandles.push_back(h);
					transv[z + y * 10 + x * 100] = trans;
				}
			}
		}
	}

	// --- Ground ---
	std::atomic<bool> terrianLoadCompleted{ false };
	Object terrianObj;
	jobs.registerObject(terrianObj);
	jobs.subscribe<JobCompletedEvent>(terrianObj, [&](const JobCompletedEvent& e) {
		if (terrianObj.guid() != e.owner) { return; }
		terrianLoadCompleted.store(true, std::memory_order_release);
		});
	jobs.subscribe<JobFailedEvent>(terrianObj, [&](const JobFailedEvent& e) {
		EError("Terrian loaded failed!");
		});
	auto terrianmh = renderer.loadModelAsync(ERes("Terrian.glb"), terrianObj, jobs);
	if (terrianmh.isErr()) {
		EError("Terrian loaded failed!");
	}
	Vector<MeshHandle> terrian;

	// --- Wall ---
	std::atomic<bool> wallLoadCompleted{ false };
	Object wallObj;
	jobs.registerObject(wallObj);
	jobs.subscribe<JobCompletedEvent>(wallObj, [&](const JobCompletedEvent& e) {
		if (wallObj.guid() != e.owner) { return; }
		wallLoadCompleted.store(true, std::memory_order_release);
		});
	jobs.subscribe<JobFailedEvent>(wallObj, [&](const JobFailedEvent& e) {
		if (wallObj.guid() != e.owner) { return; }
		EError("Model wall loaded failed: {}", ToString(e.error));
		});
	auto wallmh = renderer.loadModelAsync(ERes("Wall.glb"), wallObj, jobs);
	if (wallmh.isErr()) {
		EError("Model wall loaded failed: {}", ToString(wallmh.error()));
	}
	Vector<MeshHandle> wall;

	// --- Font ---
	FontData font = loadFont(ERes("msyh.ttc"), 24.0f, renderer.getDevice(), CharSet::ASCII | CharSet::CJK);

	// --- Layers ---
	UILayer hud;
	hud.name = "HUD";
	hud.bgColor = Vec4(0, 0, 0, 0);

	UILayer menu;
	menu.name = "Menu";
	menu.bgColor = Vec4(0.1f, 0.1f, 0.2f, 0.3f);
	menu.afterPostProcess = true;

	// --- UI ---
	UIButton button;
	UIButton button2;
	UIPicture picture;
	UICrosshair crosshair;
	crosshair.style = CrosshairStyle::Outline;
	crosshair.radius = 4.0f;
	crosshair.thickness = 1.0f;
	ui.setCrosshair(&crosshair);
	jobs.registerObject(button2);
	hud.addControl(&button);
	ui.pushLayer(hud);
	UIElementDesc btndesc;
	btndesc.anchor = UIAnchor::TopRight;
	btndesc.color = Vec4(0.8f, 0.3f, 0.3f, 0.7f);
	btndesc.position = Vec2(-10.0f, 10.0f);
	btndesc.size = Vec2(150.0f, 100.0f);
	button.desc = btndesc;
	button.label = "退出";
	button.fontSRV = font.atlasSRV;
	button.fontData = &font;
	ui.subscribe<UIClickEvent>(button, [&](const UIClickEvent& e) {
		if (!e.pressed) {
			if (e.control->guid() == button2.guid()) {
				button.label = "按下ESC退出！";
				ui.popLayer();
			}
			else if (e.control->guid() == button.guid()) {
				ui.pushLayer(menu);
			}
		}
		});
	menu.addControl(&button2);
	UIElementDesc btn2desc;
	btn2desc.anchor = UIAnchor::Center;
	btn2desc.color = Vec4(0.8f, 0.3f, 0.3f, 0.7f);
	btn2desc.position = Vec2(-10.0f, -10.0f);
	btn2desc.size = Vec2(150.0f, 100.0f);
	btn2desc.afterPostProcess = true;
	button2.desc = btn2desc;
	button2.label = "真的要退出吗？";
	button2.fontSRV = font.atlasSRV;
	button2.fontData = &font;
	menu.addControl(&picture);
	UIElementDesc picdesc;
	picdesc.anchor = UIAnchor::BotLeft;
	picdesc.size = Vec2(192.0f, 108.0f);
	picdesc.position = Vec2(10.0f, -10.0f);
	picdesc.afterPostProcess = true;
	picture.desc = picdesc;
	picture.textureSRV = loadTexture(ERes("14.jpg"), renderer.getDevice());

	UITextInput textInput;
	textInput.fontSRV = font.atlasSRV;
	textInput.fontData = &font;
	textInput.fontSize = 20.0f;
	textInput.textColor = Vec4(1, 1, 1, 1);
	UIElementDesc tidesc;
	tidesc.anchor = UIAnchor::Center;
	tidesc.size = Vec2(250.0f, 36.0f);
	tidesc.position = Vec2(0, 40.0f);
	tidesc.afterPostProcess = true;
	textInput.desc = tidesc;
	menu.addControl(&textInput);
	ui.subscribe<UITextSubmitEvent>(textInput, [&](const UITextSubmitEvent& e) {
		EInfo("Text submitted: {}", e.text);
		});

	// --- Debug UI ---
	Object debugUIObj;
	debugUI.registerObject(debugUIObj);
	static UInt32 gpuVisCount = 0;
	static float sunIntensity = 1.0;
	static float sunColorR = 1.0f;
	static float sunColorG = 0.98f;
	static float sunColorB = 0.9f;
	debugUI.subscribe<DebugUIRenderEvent>(debugUIObj, [&](const DebugUIRenderEvent&) {
		debugUI.beginWindow("Environment");
		debugUI.sliderFloat("Sun Intensity", &sunIntensity, 0.1f, 20.0f);
		debugUI.sliderFloat("Sun Color R", &sunColorR, 0, 1.0f);
		debugUI.sliderFloat("Sun Color G", &sunColorG, 0, 1.0f);
		debugUI.sliderFloat("Sun Color B", &sunColorB, 0, 1.0f);
		debugUI.endWindow();
		debugUI.beginWindow("Post-Process Settings");
		bool changed = false;
		bool customChanged = false;
		checkIfChanged<F32>(ppCfg.toneMap.exposure, changed, [&](F32& v) { debugUI.sliderFloat("Exposure", &v, 0.1f, 5.0f); });
		checkIfChanged<F32>(ppCfg.toneMap.vignette, changed, [&](F32& v) { debugUI.sliderFloat("Vignette", &v, 0, 1.0f); });
		checkIfChanged<F32>(ppCfg.toneMap.saturation, changed, [&](F32& v) { debugUI.sliderFloat("Saturation", &v, 0, 2.0f); });
		checkIfChanged<F32>(ppCfg.toneMap.contrast, changed, [&](F32& v) { debugUI.sliderFloat("Contrast", &v, 0.5f, 2.0f); });
		checkIfChanged<bool>(ppCfg.bloom.enabled, changed, [&](bool& v) { String str = fmt::format("Bloom: {}", v ? "ON" : "OFF"); if (debugUI.button(str.c_str())) { v = !v; } });
		checkIfChanged<F32>(ppCfg.bloom.intensity, changed, [&](F32& v) { debugUI.sliderFloat("Bloom Intensity", &v, 0, 2.0f); });
		checkIfChanged<F32>(ppCfg.bloom.radius, changed, [&](F32& v) { debugUI.sliderFloat("Bloom Radius", &v, 0, 8.0f); });
		debugUI.text("MSAA: {}x", (int)ppCfg.sampleCount);
		checkIfChanged<String>(ppCfg.customShader, customChanged, [&](String& v) { String str = fmt::format("Custom: {}", v.empty() ? "OFF" : "ON"); if (debugUI.button(str.c_str())) { v = v.empty() ? g_CustomPS : ""; } });
		if (debugUI.button("Reset")) {
			ppCfg.toneMap.exposure = 1.0f; ppCfg.toneMap.gamma = 2.2f;
			ppCfg.toneMap.vignette = 0; ppCfg.toneMap.saturation = 1.0f;
			ppCfg.bloom.enabled = true; ppCfg.bloom.intensity = 0.5f;
			ppCfg.bloom.radius = 0.01f; ppCfg.customShader = "";
			changed = true;
			customChanged = true;
		}
		if (changed || customChanged) {
			postProcess.setConfig(ppCfg);
		}
		if (customChanged) {
			postProcess.setCustomShader(ppCfg.customShader);
			postProcess.rebuildPSO();
		}
		debugUI.endWindow();

		debugUI.beginWindow("Camera");
		debugUI.text("Camera X: {}", fly.pos.x);
		debugUI.text("Camera Y: {}", fly.pos.y);
		debugUI.text("Camera Z: {}", fly.pos.z);
		debugUI.text("RayTracing: inline={} standalone={}", renderer.supportsInlineRayTracing(), renderer.supportsStandaloneRayTracing());
		debugUI.text("RT Subsystem: {}", rayTracing.isReady() ? "ready" : "disabled");
		debugUI.text("MeshShader: device={} subsystem={}", renderer.supportsMeshShaders() ? "supported" : "unsupported", meshShader.isReady() ? "ready" : "disabled");
		if (meshShader.isReady()) {
			if (msMeshesRegistered) {
				if (debugUI.button(fmt::format("MS Scene (all objects): {}", msFurinaScene ? "On" : "Off").c_str())) {
					msFurinaScene = !msFurinaScene;
				}
				if (debugUI.sliderFloat("MS Cluster Budget", &msClusterBudget, 256.0f, 262144.0f)) {
					meshShader.setClusterBudget((UInt32)msClusterBudget);
				}
				if (debugUI.sliderFloat("MS Inst Budget", &msInstBudget, 1.0f, 1000.0f)) {}
				if (debugUI.button(fmt::format("MS Debug: {}", msDebugTri == 0 ? "Off" : (msDebugTri == 1 ? "Triangle" : "Albedo")).c_str())) {
					msDebugTri = (msDebugTri + 1) % 3;
					meshShader.setDebugMode((UInt32)msDebugTri);
				}
				if (debugUI.button(fmt::format("MS Mesh Filter: {}", (msMeshFilter > (int)msMeshCount - 1) ? "All" : std::to_string(msMeshFilter)).c_str())) {
					msMeshFilter = (msMeshFilter > (int)msMeshCount - 1) ? 0 : (msMeshFilter + 1);
					meshShader.setMeshFilter((msMeshFilter > (int)msMeshCount - 1) ? 0xFFFFFFFFu : (UInt32)msMeshFilter);
				}
				if (msFurinaScene) {
					debugUI.text("MS visible tasks: {} (of {} groups x {} meshes)", meshShader.lastSceneVisibleTasks(), msGroupCount, msMeshCount);
					debugUI.text("MS visible clusters: {} / {} (demand {})", meshShader.lastSceneVisibleClusters(), (UInt32)msClusterBudget, meshShader.lastSceneClusterDemand());
					if (debugUI.sliderFloat("MS LOD Error (px)", &msLodScale, 0.25f, 64.0f)) {
						meshShader.setLodScale(msLodScale);
					}
				}
			}
			if (debugUI.button(fmt::format("MS Test Grid: {}", msTestGrid ? "On" : "Off").c_str())) {
				msTestGrid = !msTestGrid;
			}
			if (msTestGrid) {
				if (debugUI.button(fmt::format("MS Frustum Cull: {}", msFrustumCull ? "On" : "Off").c_str())) {
					msFrustumCull = !msFrustumCull;
					meshShader.setFrustumCulling(msFrustumCull);
				}
				debugUI.text("MS visible: {} / {}", meshShader.lastVisibleCount(), 32u * 32u);
			}
		}
		if (hybridRT) {
			debugUI.text("RT trace: {:.2f} ms", rayTracing.lastTraceMs());
		}

		// ---- Render pipeline: pass graph, CPU and GPU time per pass ----
		if (renderPipeline) {
			const auto& run = renderPipeline->lastRunStats();
			debugUI.text("  frame {:.2f} ms | run {} skip {} fail {} | {} lv, widest {}",
				run.totalMs, run.tasksRun, run.tasksSkipped, run.tasksFailed,
				run.levelCount, run.maxLevelWidth);
			// cpu = time spent recording the pass (all GPU passes are cheap here);
			// gpu = the work the pass actually put on the GPU, read one frame late.
			// 'present' has no gpu value: the frame is submitted and waited on
			// there, so its cpu time is where the whole GPU frame shows up.
			const auto& gpu = renderPipeline->passGpuStats();
			const auto& stats = renderPipeline->passStats();
			for (size_t i = 0; i < stats.size(); i++) {
				const auto& pass = stats[i];
				// A skipped pass has no measurement of its own (the timers never
				// ran), so keep its GPU column at zero instead of showing the last
				// frame it did run.
				const bool ran = pass.status == PipelineTaskStatus::Succeeded || pass.status == PipelineTaskStatus::Failed;
				const F64 gpuMs = (ran && i < gpu.size()) ? gpu[i].lastMs : 0.0;
				const F64 gpuMax = (ran && i < gpu.size()) ? gpu[i].maxMs : 0.0;
				debugUI.text("  {:<11} cpu {:>6.2f} | gpu {:>6.2f} (max {:>6.2f}) | cpu avg {:>6.2f} max {:>6.2f} | {}",
					pass.name, pass.lastMs, gpuMs, gpuMax, pass.averageMs, pass.maxMs, ToString(pass.status));
			}
			if (debugUI.button(fmt::format("GPU pass timers: {}", renderPipeline->gpuTiming() ? "On" : "Off").c_str())) {
				renderPipeline->setGpuTiming(!renderPipeline->gpuTiming());
			}
		}
		else {
			debugUI.text("Render pipeline: UNAVAILABLE (no frame is being drawn)");
		}
		debugUI.text("Mode: {} (F9)", hybridRT ? "Hybrid RT" : "Raster");
		if (hybridRT) {
			static const char* rtModeNames[] = { "Shaded", "GBufferColor", "GBufferNormal", "Diffuse", "Reflections", "Fresnel", "RTAlpha", "BackFacingNormals" };
			if (debugUI.button(fmt::format("RT View: {}", rtModeNames[rtDrawMode]).c_str())) {
				rtDrawMode = (rtDrawMode + 1) % 8;
			}
			static const char* rtResNames[] = { "Auto", "Full", "Half" };
			// Auto(0) / Full(1) / Half(2) cycle.
			int resMode = rtResAuto ? 0 : (rtResScale > 0.9f ? 1 : 2);
			if (debugUI.button(fmt::format("RT Res: {}", rtResNames[resMode]).c_str())) {
				resMode = (resMode + 1) % 3;
				if (resMode == 0) { rtResAuto = true; }
				else {
					rtResAuto = false;
					rtResScale = (resMode == 1) ? 1.0f : 0.5f;
				}
				createRTTex(ww, wh); // recreate only the RT texture (G-buffer stays full-res)
			}
			{
				float pcfF = (float)rtShadowPCF;
				if (debugUI.sliderFloat("Shadow PCF", &pcfF, 1.0f, 16.0f)) {
					rtShadowPCF = (UInt32)(pcfF + 0.5f);
				}
			}
			if (debugUI.sliderFloat("Light Size (PCSS)", &rtLightSize, 0.0f, 0.3f)) {}
			if (debugUI.sliderFloat("AO Radius", &rtAoRadius, 0.0f, 5.0f)) {}
			{
				float aoF = (float)rtAoSamples;
				if (debugUI.sliderFloat("AO Samples", &aoF, 0.0f, 8.0f)) {
					rtAoSamples = (UInt32)(aoF + 0.5f);
				}
			}
			if (debugUI.sliderFloat("Refl Blur", &rtReflectionBlur, 0.0f, 1.0f)) {}
			// Ray-cone prefiltering: 1 = every ray reads the mip that matches its
			// lobe (stable), 0 = raw level 0 (aliased, for comparison).
			if (debugUI.sliderFloat("Refl Cone", &rtReflectionCone, 0.0f, 1.0f)) {}
			// Shadow cost inside reflections: 1 = one ray per reflection hit, which
			// is what the lobe blur and the denoiser can actually use; higher values
			// restore the soft PCSS+PCF path (roughly doubles/triples trace()).
			{
				float rsf = (float)rtReflectionShadowPCF;
				if (debugUI.sliderFloat("Refl Shadow", &rsf, 1.0f, 8.0f)) {
					rtReflectionShadowPCF = (UInt32)(rsf + 0.5f);
				}
			}
			{
				float rsF = (float)rtReflectionSamples;
				if (debugUI.sliderFloat("Refl Max Rays", &rsF, 1.0f, 16.0f)) {
					rtReflectionSamples = (UInt32)(rsF + 0.5f);
				}
			}
			if (debugUI.button(fmt::format("Refl Bounce: {}", rtMaxBounces ? "2" : "1").c_str())) {
				rtMaxBounces = rtMaxBounces ? 0 : 1;
			}
			if (debugUI.sliderFloat("Bounce Rough", &rtBounceRoughness, 0.0f, 1.0f)) {}
			if (debugUI.button(fmt::format("RT Denoise: {}", rtDenoise ? "On" : "Off").c_str())) {
				rtDenoise = !rtDenoise;
			}
			{
				static const char* denoiserNames[] = { "Auto", "NRD", "OIDN", "Temporal" };
				if (debugUI.button(fmt::format("Denoiser: {}", denoiserNames[rtDenoiserSel]).c_str())) {
					rtDenoiserSel = (rtDenoiserSel + 1) % 4;
				}
				const bool activeNRD = denoising.isReady() && (rtDenoiserSel == 0 || rtDenoiserSel == 1);
				const bool activeOIDN = !activeNRD && rayTracing.oidnActive() && rtDenoiserSel != 3;
				if (activeNRD) {
					debugUI.text("Active: NRD (REBLUR)");
				}
				else if (activeOIDN) {
					if (debugUI.button(fmt::format("OIDN Async: {}", rtOIDNAsync ? "On" : "Off").c_str())) {
						rtOIDNAsync = !rtOIDNAsync;
						rayTracing.setOIDNAsync(rtOIDNAsync);
					}
					debugUI.text("Active: OIDN (GPU, zero-copy)");
				}
				else {
					debugUI.text("Active: temporal + spatial");
				}
			}
			if (debugUI.sliderFloat("Denoise Strength", &rtDenoiseStrength, 0.0f, 1.0f)) {}
		}
		{
			static size_t visibleCount = 0;
			// Use the GPU-computed count from args buffer readback (logged above)
			debugUI.text("GPU cull: {} / 1000 visible", gpuVisCount);
		}
		debugUI.endWindow();

		debugUI.beginWindow("Skybox");
		static const char* skyNames[] = { "Day", "Sunset", "Night", "Texture" };
		int totalModes = skyCubeTexHandle.isValid() ? 4 : 3;
		if (debugUI.button(fmt::format("Sky: {}", skyNames[s_skyMode]).c_str())) {
			s_skyMode = (s_skyMode + 1) % totalModes;
			RenderSubsystem::SkyboxDesc sd;
			if (s_skyMode == 3) {
				sd.skyCubeTex = skyCubeTexHandle;
			}
			else if (s_skyMode == 0) {
				// Day: blue top, gray horizon
				for (int i = 0; i < 4; i++) s_skyCorners[i] = Vec4(0.3f + (i & 1) * 0.1f, 0.5f + (i & 2) * 0.05f, 0.9f + ((i >> 1) & 1) * 0.05f, 1);
				for (int i = 4; i < 8; i++) s_skyCorners[i] = Vec4(0.5f, 0.55f, 0.6f, 1);
				memcpy(sd.corners, s_skyCorners, sizeof(s_skyCorners));
			}
			else if (s_skyMode == 1) {
				// Sunset: orange top, dark purple horizon
				for (int i = 0; i < 4; i++) s_skyCorners[i] = Vec4(0.9f, 0.4f + ((i & 2) >> 1) * 0.15f, 0.15f + ((i >> 1) & 1) * 0.1f, 1);
				for (int i = 4; i < 8; i++) s_skyCorners[i] = Vec4(0.3f, 0.15f, 0.25f, 1);
				memcpy(sd.corners, s_skyCorners, sizeof(s_skyCorners));
			}
			else {
				// Night: dark blue top, black bottom
				for (int i = 0; i < 4; i++) s_skyCorners[i] = Vec4(0.05f, 0.05f, 0.2f + (i & 1) * 0.05f, 1);
				for (int i = 4; i < 8; i++) s_skyCorners[i] = Vec4(0.02f, 0.02f, 0.05f, 1);
				memcpy(sd.corners, s_skyCorners, sizeof(s_skyCorners));
			}
			renderer.setSkybox(sd);
		}
		debugUI.endWindow();

		debugUI.beginWindow("AdaptiveMusic");
		debugUI.text("Script: {}", musicCtl.isRunning() ? "RUNNING" : "STOPPED");
		debugUI.text("intro:   {} {}", musicPlayer.isTrackPlaying(introH) ? "playing" : "-", musicPlayer.isTrackFinished(introH) ? "finished" : "");
		debugUI.text("mid:     {} {}", musicPlayer.isTrackPlaying(midH) ? "playing" : "-", musicPlayer.isTrackFinished(midH) ? "finished" : "");
		debugUI.text("climax1: {} {}", musicPlayer.isTrackPlaying(c1H) ? "playing" : "-", musicPlayer.isTrackFinished(c1H) ? "finished" : "");
		debugUI.text("climax2: {} {}", musicPlayer.isTrackPlaying(c2H) ? "playing" : "-", musicPlayer.isTrackFinished(c2H) ? "finished" : "");
		debugUI.text("climax3: {} {}", musicPlayer.isTrackPlaying(c3H) ? "playing" : "-", musicPlayer.isTrackFinished(c3H) ? "finished" : "");
		debugUI.text("end:     {} {}", musicPlayer.isTrackPlaying(endH) ? "playing" : "-", musicPlayer.isTrackFinished(endH) ? "finished" : "");
		debugUI.text("F7=restart F8=stop script");
		debugUI.endWindow();
		});

	//window.setMode(WindowMode::ExclusiveFullscreen);
	UInt64 gameTick = 0;

	// --- Main loop ---
	while (!window.shouldClose()) {
		gameTick++;

		auto now = std::chrono::high_resolution_clock::now();
		static auto prev = now;
		F32 dt = (F32)std::chrono::duration<F64>(now - prev).count();
		prev = now;
		if (dt > 0.1f) dt = 0.1f;

		window.pollEvents();

		if (modelLoadCompleted.load(std::memory_order_acquire) && model.size() == 0) {
			model = renderer.getModelMeshes(mh.value());
			// Load pre-cooked SDF mesh for camera
			auto r = ResourcesManager::getInstance().readFile(ERes("cooked/Furina.sdf"));
			if (r.isOk()) {
				if (fly.cameraBody != InvalidRigidBody) physicsBodies.destroyRigidBody(fly.cameraBody);
				fly.cameraBody = physicsBodies.createDynamicMeshFromCooked(r->data(), r->size(),
					fly.pos, Quat(1, 0, 0, 0), 70.0f);
				physicsBodies.setAngularLock(fly.cameraBody, AngularLockFlag::LockX | AngularLockFlag::LockZ);
				EInfo("Loaded cooked SDF mesh for camera ({} bytes)", r->size());
			}
		}
		if (terrianLoadCompleted.load(std::memory_order_acquire) && terrian.size() == 0) {
			terrian = renderer.getModelMeshes(terrianmh.value());
			// Load pre-cooked triangle mesh for terrain
			auto r = ResourcesManager::getInstance().readFile(ERes("cooked/Terrian.tri"));
			if (r.isOk()) {
				physicsBodies.createStaticMeshFromCooked(r->data(), r->size(), Vec3(0), Quat(1, 0, 0, 0));
				EInfo("Loaded cooked terrain mesh ({} bytes)", r->size());
			}
		}
		if (wallLoadCompleted.load(std::memory_order_acquire) && wall.size() == 0) {
			wall = renderer.getModelMeshes(wallmh.value());
			auto r = ResourcesManager::getInstance().readFile(ERes("cooked/Wall.tri"));
			if (r.isOk()) {
				physicsBodies.createStaticMeshFromCooked(r->data(), r->size(), Vec3(20.0f, -19.0f, 20.0f), Quat(1, 0, 0, 0));
				EInfo("Loaded cooked wall mesh ({} bytes)", r->size());
			}
		}

		// ---- Mesh shader scene registration -----------------------------
		// Every drawable object goes into one shared cluster pool; the index
		// ranges below let the draw build one instanced group per object type.
		// Re-registration happens whenever the set of loaded meshes changes
		// (the models arrive asynchronously), which also rebuilds the clusters.
		{
			Vector<MeshHandle> all;
			const UInt32 terrIdx = (UInt32)all.size();
			for (auto& m : terrian) all.push_back(m);
			const UInt32 wallIdx = (UInt32)all.size();
			for (auto& m : wall) all.push_back(m);
			const UInt32 cubeIdx = (UInt32)all.size();
			if (emissiveCubeMesh.isValid()) all.push_back(emissiveCubeMesh);
			const UInt32 furinaIdx = (UInt32)all.size();
			for (auto& m : model) all.push_back(m);

			if (meshShader.isReady() && !all.empty() && all != msRegisteredMeshes) {
				auto mr = meshShader.setMeshes(all);
				if (mr.isOk()) {
					msRegisteredMeshes = all;
					msTerrIdx = terrIdx;   msTerrCount   = wallIdx - terrIdx;
					msWallIdx = wallIdx;   msWallCount   = cubeIdx - wallIdx;
					msCubeIdx = cubeIdx;   msCubeCount   = furinaIdx - cubeIdx;
					msFurinaIdx = furinaIdx; msFurinaCount = (UInt32)all.size() - furinaIdx;
					msMeshesRegistered = true;
					msMeshCount = (UInt32)all.size();
					msGroupCount = (msTerrCount ? 1u : 0u) + (msWallCount ? 1u : 0u) + (msCubeCount ? 1u : 0u) + (msFurinaCount ? 1u : 0u) + 1u; // +1 for the camera body
					EInfo("MS: registered {} meshes (terrain {}, wall {}, cube {}, furina {})",
						all.size(), msTerrCount, msWallCount, msCubeCount, msFurinaCount);
				}
				else {
					EError("MeshShader: setMeshes failed: {}", ToString(mr.error()));
				}
			}
		}

		UInt32 fbW, fbH;
		window.getFramebufferSize(fbW, fbH);
		if (fbW != ww || fbH != wh) {
			ww = fbW; wh = fbH;
			renderer.resize(ww, wh);
			postProcess.resize(ww, wh);
			debugUI.resize(ww, wh);
			render2d.setScreenSize(ww, wh);
			ui.setScreenSize(ww, wh);
			createGBufferTextures(ww, wh);
		}

		input.update(dt);
		const InputState& st = input.inputState();

		if (st.wasKeyPressedThisFrame(KeyCode::Escape)) break;
		if (st.wasKeyPressedThisFrame(KeyCode::F1)) {
			bool w = !renderer.isWireframe(); renderer.setWireframe(w);
			EInfo("Wireframe: {}", w ? "ON" : "OFF");
		}
		if (st.wasKeyPressedThisFrame(KeyCode::F10)) {
			WindowMode cur = window.mode();
			window.setMode(cur == WindowMode::BorderlessFullscreen ? WindowMode::Windowed : WindowMode::BorderlessFullscreen);
		}
		if (st.wasKeyPressedThisFrame(KeyCode::F11)) {
			WindowMode cur = window.mode();
			window.setMode(cur == WindowMode::ExclusiveFullscreen ? WindowMode::Windowed : WindowMode::ExclusiveFullscreen);
		}
		if (st.wasKeyPressedThisFrame(KeyCode::F2)) {
			bool v = !window.isCursorVisible();
			window.setCursorVisible(v);
			input.setCursorVisible(v);
			EInfo("Cursor: {}", v ? "visible" : "hidden");
		}
		if (st.wasKeyPressedThisFrame(KeyCode::F5)) {
			fly.toggleBind(physicsBodies);
			EInfo("Camera: {}", fly.boundToBody ? "bound to cube" : "free fly");
		}
		if (st.wasKeyPressedThisFrame(KeyCode::F6)) {
			ppCfg.toneMap.mode = (ToneMapMode)(((UInt8)ppCfg.toneMap.mode + 1) % 4);
			postProcess.setConfig(ppCfg);
			EInfo("ToneMap: {}", (int)ppCfg.toneMap.mode);
		}
		if (st.wasKeyPressedThisFrame(KeyCode::F7)) {
			musicPlayer.stop();
			musicCtl.stop();
			musicCtl.start();
			EInfo("Restarted adaptive music script");
		}
		if (st.wasKeyPressedThisFrame(KeyCode::F8)) {
			musicCtl.stop();
			EInfo("Stopped script, tracks keep playing");
		}
		if (st.wasKeyPressedThisFrame(KeyCode::F9)) {
			if (!rayTracing.isReady()) {
				EInfo("Hybrid ray tracing unavailable on this device - stay in rasterization mode.");
			}
			else {
				hybridRT = !hybridRT;
				EInfo("Hybrid ray tracing: {}", hybridRT ? "ON" : "OFF");
			}
		}
		if (st.wasKeyPressedThisFrame(KeyCode::Num2)) {
			static bool pushed = true;
			pushed = !pushed;
			if (pushed) {
				ui.popMask();
			}
			else {
				ui.pushMask(Vec2(0, 200), Vec2(1920, 800));
			}
		}
		if (input.isCursorVisible()) {
			Vec2 pos = window.getCursorPos();
			debugUI.setMousePos(pos.x, pos.y);
			debugUI.setMouseButton(0, window.isMouseButtonDown(0));
			debugUI.setMouseButton(1, window.isMouseButtonDown(2));
			debugUI.setMouseButton(2, window.isMouseButtonDown(1));

			ui.setMousePos(pos.x, pos.y);
			ui.setMouseDown(window.isMouseButtonDown(0));
		}

		// Crosshair logic (always visible, not dependent on cursor state)
		if (!fly.boundToBody) {
			ui.setCrosshairStyle(CrosshairStyle::None);
		}
		else {
			F32 yr = glm::radians(fly.yaw), pr = glm::radians(fly.pitch);
			Vec3 camDir(cos(pr) * cos(yr), sin(pr), cos(pr) * sin(yr));
			RaycastHit hit = physicsWorld.raycast(fly.pos, camDir, 10.0f);
			bool onFurina = false;
			if (hit.hit) {
				for (auto& h : physHandles) { if (h == hit.body) { onFurina = true; break; } }
			}
			ui.setCrosshairStyle(onFurina ? CrosshairStyle::Filled : CrosshairStyle::Outline);
		}

		ui.setDeltaTime(dt);

		// Text input key handling (Backspace / Enter / Delete / Arrows)
		{
			GLFWwindow* gw = static_cast<GLFWwindow*>(window.glfwWindow());
			static bool bsWas = false, enterWas = false, delWas = false, leftWas = false, rightWas = false;
			bool bs = glfwGetKey(gw, GLFW_KEY_BACKSPACE) == GLFW_PRESS;
			bool en = glfwGetKey(gw, GLFW_KEY_ENTER) == GLFW_PRESS;
			bool dl = glfwGetKey(gw, GLFW_KEY_DELETE) == GLFW_PRESS;
			bool lf = glfwGetKey(gw, GLFW_KEY_LEFT) == GLFW_PRESS;
			bool rg = glfwGetKey(gw, GLFW_KEY_RIGHT) == GLFW_PRESS;
			if (bs && !bsWas) ui.inputBackspace();
			if (en && !enterWas) ui.inputSubmit();
			if (dl && !delWas) ui.inputDelete();
			if (lf && !leftWas) ui.inputCursorLeft();
			if (rg && !rightWas) ui.inputCursorRight();
			bsWas = bs; enterWas = en; delWas = dl; leftWas = lf; rightWas = rg;
		}

		// Rotate sun light with arrow keys
		{
			F32 rotSpeed = 60.0f * dt;
			if (st.isKeyDown(KeyCode::Left))  sunYaw -= rotSpeed;
			if (st.isKeyDown(KeyCode::Right)) sunYaw += rotSpeed;
			if (st.isKeyDown(KeyCode::Up))    sunPitch += rotSpeed;
			if (st.isKeyDown(KeyCode::Down))  sunPitch -= rotSpeed;
			Vec3 sunDir = Vec3(cos(glm::radians(sunYaw)) * cos(glm::radians(sunPitch)),
				-sin(glm::radians(sunPitch)),
				sin(glm::radians(sunYaw)) * cos(glm::radians(sunPitch)));
			LightDesc ld; ld.dir = glm::normalize(sunDir); ld.color = Vec3(sunColorR, sunColorG, sunColorB); ld.intensity = sunIntensity;
			renderer.updateLight(sunHandle, ld);
		}

		fly.update(input, dt, &physicsBodies);
		renderer.updateCamera(camHandle.value(), fly.toDesc(ww, wh));

		// The render pipeline owns every frame: shadow cascades, geometry
		// submission and the frame tail. There is no inline fallback, so report a
		// missing pipeline once instead of silently rendering nothing.
		if (renderPipeline == nullptr) {
			static bool rpMissingWarned = false;
			if (!rpMissingWarned) { EError("RenderPipeline is unavailable: nothing will be drawn"); rpMissingWarned = true; }
		}

		// Toggled with '1'.
		static bool fogEnabled = true;

		// Update audio listener
		{
			F32 yr = glm::radians(fly.yaw), pr = glm::radians(fly.pitch);
			Vec3 fwd(cos(pr) * cos(yr), sin(pr), cos(pr) * sin(yr));
			Vec3 up(0, 1, 0);
			audio.setListener(fly.pos, fwd, up);
		}
		audio.update(dt); // triggers occlusion raycasts on main thread
		musicCtl.update(dt); // AdaptiveMusic DSL 执行器

		// Physics: step simulation + build instance transforms
		if (modelLoadCompleted.load(std::memory_order_acquire)) {
			physicsWorld.step(dt);

			for (size_t i = 0; i < physHandles.size(); i++) {
				transv[i] = physicsBodies.getWorldTransform(physHandles[i]);
			}

			if (st.wasKeyPressedThisFrame(KeyCode::R)) {
				for (size_t i = 0; i < physHandles.size(); i++) {
					UInt16 x = (UInt16)(i / 100);
					UInt16 y = (UInt16)((i / 10) % 10);
					UInt16 z = (UInt16)(i % 10);
					physicsBodies.setPosition(physHandles[i], Vec3(x * 2, y * 2 + 2, z * 2));
					physicsBodies.setLinearVelocity(physHandles[i], Vec3(0));
				}
				EInfo("Physics: reset all bodies");
			}
		}

		debugUI.beginFrame(ww, wh);

		// Furina instance + GPU-cull data. It feeds the pipeline's indirect scene
		// draws (raster mode) and its indirect shadow pass, so it is refreshed
		// once per frame, before the geometry passes run.
		auto updateFurinaCull = [&](const Mat4& view, const Mat4& proj) {
			auto fp = compute.computeFrustumPlanes(proj * view);
			// Upload world matrices to GPU
			{
				Vector<Mat4> wm(1000);
				for (size_t i = 0; i < 1000; i++) wm[i] = transv[i].computeWorldMatrix();
				compute.updateBuffer(worldMatBuf, wm.data(), static_cast<UInt32>(1000 * sizeof(Mat4)));
			}
			// Upload culling instances
			{
				Vector<CullingInstance> insts(1000);
				for (size_t i = 0; i < 1000; i++) {
					insts[i].boundSphere = Vec4(transv[i].position, 0.8f);
					insts[i].drawIndex = static_cast<UInt32>(i);
				}
				compute.updateBuffer(cullInstBuf, insts.data(), static_cast<UInt32>(insts.size() * sizeof(CullingInstance)));
			}
			// Indirect draw arguments per mesh. instanceCount stays 0 here: the
			// culling shader accumulates it as it finds visible instances, so the
			// arguments are complete when the dispatch returns and no GPU->CPU
			// readback is needed to submit the draw.
			IndirectDrawArgs argsTmpl[8] = {};
			if (!model.empty()) {
				const UInt32 meshCount = static_cast<UInt32>(std::min<size_t>(model.size(), 8));
				for (UInt32 mi = 0; mi < meshCount; mi++) {
					auto sub = renderer.getSubMesh(model[mi], 0);
					argsTmpl[mi].indexCount = static_cast<UInt32>(sub.indexCount);
					argsTmpl[mi].firstIndex = static_cast<UInt32>(sub.indexOffset);
					argsTmpl[mi].baseVertex = static_cast<UInt32>(sub.vertexOffset);
				}
				compute.updateBuffer(argsBuf, argsTmpl, sizeof(argsTmpl));
				compute.updateCullingCB(cullCBBuf, fp, 1000,
					argsTmpl[0].indexCount, argsTmpl[0].firstIndex, argsTmpl[0].baseVertex, meshCount);
				// Clear the compaction counter before dispatch
				{ UInt32 zero = 0; compute.updateBuffer(counterBuf, &zero, sizeof(zero)); }
				compute.dispatchCullingCompact(cullInstBuf, cullCBBuf, cullVisBuf, indicesBuf, counterBuf, argsBuf, 1000);
				// The on-screen "GPU cull" counter still needs the value on the CPU.
				// Reading it is a device synchronisation, so refresh it every so
				// often instead of every frame; the draw itself does not wait.
				static UInt32 cullStatFrame = 0;
				if ((cullStatFrame++ % 30) == 0) {
					UInt32 visCount = 0;
					compute.readback(cullStgBuf, counterBuf, sizeof(UInt32), &visCount);
					gpuVisCount = visCount;
				}
			}
		};

		// ---- Render pipeline path ----
		if (renderPipeline) {
			RenderFrame frame;
			frame.deltaTime = dt;
			frame.timeSec = (F32)gameTick * 0.02f;
			frame.width = ww;
			frame.height = wh;
			renderer.getCameraMatrices(frame.view, frame.proj);
			frame.cameraPos = fly.pos;
			frame.hybrid = hybridRT;
			frame.denoise = rtDenoise;
			frame.denoiserSelection = rtDenoiserSel;
			frame.denoiseStrength = rtDenoiseStrength;

			// Shadow cascade fitting inputs (the pipeline runs the pass itself).
			{
				auto camDesc = fly.toDesc(ww, wh);
				frame.shadowLightDir = glm::normalize(Vec3(cos(glm::radians(sunYaw)) * cos(glm::radians(sunPitch)),
					-sin(glm::radians(sunPitch)), sin(glm::radians(sunYaw)) * cos(glm::radians(sunPitch))));
				F32 yr = glm::radians(fly.yaw), pr = glm::radians(fly.pitch);
				frame.shadowEye = fly.pos;
				frame.shadowCenter = fly.pos + Vec3(cos(pr) * cos(yr), sin(pr), cos(pr) * sin(yr));
				frame.shadowFov = glm::radians(camDesc.fov);
				frame.shadowAspect = (F32)ww / (F32)wh;
				frame.shadowNear = camDesc.nearP;
				frame.shadowFar = camDesc.farP;
			}

			// Scene geometry. One description feeds the colour pass and the shadow
			// pass, so a mesh can never be visible but unshadowed. The mesh shader
			// path submits it as draw groups (one DrawMesh for the whole scene);
			// everything else goes through the explicit list.
			frame.useMeshShaderScene = msFurinaScene && msMeshesRegistered && meshShader.isReady();
			if (frame.useMeshShaderScene) {
				msAddGroup(frame.meshGroups, msTerrIdx, msTerrCount, { Transform{}.computeWorldMatrix() });
				msAddGroup(frame.meshGroups, msWallIdx, msWallCount, { Transform{ .position = Vec3(20, -19, 20) }.computeWorldMatrix() });
				if (emissiveCubeMesh.isValid()) msAddGroup(frame.meshGroups, msCubeIdx, msCubeCount, { emissiveCubeTf.computeWorldMatrix() });
				{
					Vector<Mat4> wm(std::min<size_t>(1000, (size_t)msInstBudget));
					for (size_t i = 0; i < wm.size(); i++) wm[i] = transv[i].computeWorldMatrix();
					msAddGroup(frame.meshGroups, msFurinaIdx, msFurinaCount, wm);
				}
				if (!fly.boundToBody && fly.cameraBody != InvalidRigidBody && msFurinaCount > 0)
					msAddGroup(frame.meshGroups, msFurinaIdx, msFurinaCount, { physicsBodies.getWorldTransform(fly.cameraBody).computeWorldMatrix() });
			}

			// Explicit geometry: static meshes first, then the bodies.
			for (auto& terrMesh : terrian) {
				SceneDraw d; d.kind = SceneDraw::Kind::Single; d.mesh = terrMesh; frame.sceneDraws.push_back(std::move(d));
			}
			for (auto& wallMesh : wall) {
				SceneDraw d; d.kind = SceneDraw::Kind::Single; d.mesh = wallMesh;
				d.transform.position = Vec3(20, -19, 20);
				frame.sceneDraws.push_back(std::move(d));
			}
			if (emissiveCubeMesh.isValid()) {
				SceneDraw d; d.kind = SceneDraw::Kind::Single; d.mesh = emissiveCubeMesh;
				d.transform = emissiveCubeTf;
				frame.sceneDraws.push_back(std::move(d));
			}
			if (!model.empty() && modelLoadCompleted.load(std::memory_order_acquire)) {
				// The explicit submission path needs an entry per mesh. The raster
				// pass draws through the GPU-cull buffers, the hybrid pass draws
				// every body directly (its G-buffer pass does no culling of its
				// own). The mesh shader path already covers this geometry above,
				// but the shadow pass still walks this list.
				if (!hybridRT) {
					updateFurinaCull(frame.view, frame.proj);
					for (size_t mi = 0; mi < model.size(); mi++) {
						SceneDraw d; d.kind = SceneDraw::Kind::Indirect; d.mesh = model[mi];
						d.worldMatricesSRV = wmSRV;
						d.indicesSRV = idxSRV;
						d.indirectArgs = argsBuf;
						d.argsByteOffset = static_cast<UInt32>(mi * sizeof(IndirectDrawArgs));
						frame.sceneDraws.push_back(std::move(d));
					}
				}
				else if (!frame.useMeshShaderScene) {
					Vector<Mat4> wmats(std::min<size_t>(1000, (size_t)msInstBudget));
					for (size_t i = 0; i < wmats.size(); i++) wmats[i] = transv[i].computeWorldMatrix();
					for (size_t mi = 0; mi < model.size(); mi++) {
						SceneDraw d; d.kind = SceneDraw::Kind::Instanced; d.mesh = model[mi];
						d.matrices = wmats;
						frame.sceneDraws.push_back(std::move(d));
					}
				}
				// Free-fly camera body, when the explicit path is drawing the model.
				if (!frame.useMeshShaderScene && !fly.boundToBody && fly.cameraBody != InvalidRigidBody) {
					Vector<Mat4> camMat = { physicsBodies.getWorldTransform(fly.cameraBody).computeWorldMatrix() };
					for (size_t mi = 0; mi < model.size(); mi++) {
						SceneDraw d; d.kind = SceneDraw::Kind::Instanced; d.mesh = model[mi];
						d.matrices = camMat;
						frame.sceneDraws.push_back(std::move(d));
					}
				}
			}

			// Fog cloud around the camera.
			{
				if (st.wasKeyPressedThisFrame(KeyCode::Num1)) { fogEnabled = !fogEnabled; EInfo("Fog: {}", fogEnabled ? "ON" : "OFF"); }
				if (fogEnabled && !hybridRT) {
					static constexpr int N = 240;
					static Vector<RenderSubsystem::BillboardDesc> fogs(N);
					F32 t = (F32)(gameTick * 0.015);
					for (int i = 0; i < N; i++) {
						F32 phi = acosf(1.0f - 2.0f * ((F32)i + 0.5f) / N);
						F32 theta = glm::two_pi<F32>() * (F32)i * 1.61803398875f;
						F32 r = 3.0f + sinf(t * 0.7f + i * 0.5f) * 0.4f + sinf(i * 2.3f) * 0.6f;
						fogs[i].position = fly.pos + Vec3(sinf(phi) * cosf(theta) * r,
							cosf(phi) * r + sinf(t + i * 0.3f) * 0.2f, sinf(phi) * sinf(theta) * r);
						fogs[i].size = Vec2(2.5f + sinf(i * 1.7f + t) * 0.5f);
						fogs[i].color = Vec4(1, 1, 1, 0.3f + sinf(i * 2.6f + t) * 0.4f);
					}
					frame.billboards = fogs;
				}
			}

			// The M1 diagnostic grid lives inside the scene pass.
			if (msTestGrid && meshShader.isReady()) {
				frame.sceneExtras = [&](RenderSubsystem&) -> Result<void, CoreError> {
					Mat4 msView, msProj; renderer.getCameraMatrices(msView, msProj);
					const auto mg = meshShader.drawGrid(msView, msProj, (F32)gameTick * 0.02f);
					return mg.isErr() ? Result<void, CoreError>(CoreError::OperationFailed) : Result<void, CoreError>{};
				};
			}

			// Hybrid: G-buffer targets, RT scene and trace constants.
			frame.gBufferColor = gbufColor;
			frame.gBufferNormal = gbufNormal;
			frame.gBufferEmissive = gbufEmissive;
			frame.gBufferDepth = gbufDepth;
			frame.resolveColor = resColor;
			frame.resolveNormal = resNormal;
			frame.resolveEmissive = resEmissive;
			frame.resolveDepth = resDepth;
			frame.rtTex = rtTex;
			frame.rtAlbedo = rtAlbedo;
			frame.rtNormal = rtNormal;
			frame.rtDrawMode = (UInt32)rtDrawMode;
			frame.lightColor = Vec3(sunColorR, sunColorG, sunColorB);
			if (hybridRT) {
				rayTracing.setSkybox(s_skyMode == 3, skyCubeTexHandle, s_skyCorners);
				frame.rtConstants.lightDir = Vec4(glm::normalize(-frame.shadowLightDir), 0.0f);
				frame.rtConstants.maxRayLength = 100.0f;
				frame.rtConstants.ambientLight = 0.1f;
				frame.rtConstants.lightIntensity = sunIntensity;
				frame.rtConstants.lightColor = Vec4(sunColorR, sunColorG, sunColorB, 0.0f);
				frame.rtConstants.shadowPCF = rtShadowPCF;
				frame.rtConstants.aoRadius = rtAoRadius;
				frame.rtConstants.aoSamples = rtAoSamples;
				frame.rtConstants.lightSize = rtLightSize;
				frame.rtConstants.reflectionBlur = rtReflectionBlur;
				frame.rtConstants.maxBounces = rtMaxBounces;
				frame.rtConstants.bounceRoughness = rtBounceRoughness;
				frame.rtConstants.reflectionSamples = rtReflectionSamples;
				frame.rtConstants.reflectionCone = rtReflectionCone;
				frame.rtConstants.reflectionShadowPCF = rtReflectionShadowPCF;
				// One pixel's angular size: seeds the reflection ray cone whose
				// width at the hit picks the prefiltered texture / sky level.
				{
					auto camDesc = fly.toDesc(ww, wh);
					frame.rtConstants.rayConePixelAngle = 2.0f * std::tan(glm::radians(camDesc.fov) * 0.5f) / (F32)std::max(wh, 1u);
				}
				{
					static Vec4 s_disc[8];
					static bool s_discInit = false;
					if (!s_discInit) {
						memset(s_disc, 0, sizeof(s_disc));
						for (int i = 1; i < 16; ++i) {
							float r = std::sqrt((i + 0.5f) / 16.0f) * 0.8f;
							float a = i * 2.399963f;
							Vec2 p(cosf(a) * r, sinf(a) * r);
							s_disc[i / 2][(i % 2) * 2] = p.x;
							s_disc[i / 2][(i % 2) * 2 + 1] = p.y;
						}
						s_discInit = true;
					}
					memcpy(frame.rtConstants.discPoints, s_disc, sizeof(s_disc));
				}
				if (rtResAuto) {
					float minDist = 1e9f;
					for (const auto& t : transv) minDist = std::min(minDist, glm::length(t.position - fly.pos));
					float targetScale = rtResScale;
					if (rtResScale >= 1.0f && minDist > 28.0f) targetScale = 0.5f;
					else if (rtResScale <= 0.5f && minDist < 22.0f) targetScale = 1.0f;
					if (targetScale != rtResScale) { rtResScale = targetScale; createRTTex(ww, wh); }
				}
				frame.rtTex = rtTex;
				frame.rtAlbedo = rtAlbedo;
				frame.rtNormal = rtNormal;
				frame.rtWidth = std::max<UInt32>(1, (UInt32)(ww * rtResScale));
				frame.rtHeight = std::max<UInt32>(1, (UInt32)(wh * rtResScale));

				if (!terrian.empty()) {
					RayTracedObjectGroup g;
					for (auto& terrMesh : terrian) {
						RayTracedObject o; o.mesh = terrMesh;
						o.material = renderer.getSubMesh(terrMesh, 0).material;
						g.objects.push_back(o);
					}
					frame.rtGroups.push_back(std::move(g));
				}
				if (!wall.empty()) {
					RayTracedObjectGroup g;
					g.transform.position = Vec3(20, -19, 20);
					for (auto& wallMesh : wall) {
						RayTracedObject o; o.mesh = wallMesh;
						o.material = renderer.getSubMesh(wallMesh, 0).material;
						g.objects.push_back(o);
					}
					frame.rtGroups.push_back(std::move(g));
				}
				if (emissiveCubeMesh.isValid() && emissiveCubeMat.isValid()) {
					RayTracedObjectGroup g;
					g.transform = emissiveCubeTf;
					RayTracedObject o; o.mesh = emissiveCubeMesh; o.material = emissiveCubeMat;
					g.objects.push_back(o);
					frame.rtGroups.push_back(std::move(g));
				}
				if (!model.empty() && modelLoadCompleted.load(std::memory_order_acquire)) {
					for (size_t i = 0; i < physHandles.size(); ++i) {
						RayTracedObjectGroup g;
						g.transform = physicsBodies.getWorldTransform(physHandles[i]);
						for (size_t mi = 0; mi < model.size(); ++mi) {
							RayTracedObject o; o.mesh = model[mi];
							o.material = renderer.getSubMesh(model[mi], 0).material;
							g.objects.push_back(o);
						}
						frame.rtGroups.push_back(std::move(g));
					}
				}
			}

			const auto pr = renderPipeline->render(frame);
			if (pr.isErr()) {
				static bool rpWarned = false;
				if (!rpWarned) { EError("RenderPipeline frame failed: {}", ToString(pr.error())); rpWarned = true; }
			}
		}
		// The frame tail (UI layers, post-process execute, debug UI, present) is
		// owned by the render pipeline's uiPost / postExecute / uiLate / debugUi /
		// present passes.

		static F64 fpsTimer = 0; static UInt32 fpsCount = 0, lastFps = 0;
		fpsTimer += dt; fpsCount++;
		if (fpsTimer >= 2.0) { lastFps = (UInt32)(fpsCount / fpsTimer); fpsTimer = 0; fpsCount = 0; }
		static F64 titleTimer = 0; titleTimer += dt;
		if (titleTimer >= 0.5) {
			char buf[128]; snprintf(buf, sizeof(buf), "EnderEngine Demo | %u FPS | %u draw calls | sun yaw=%.0f pitch=%.0f | %s | %u frames",
				lastFps, renderer.lastFrameDrawCalls(), sunYaw, sunPitch, renderer.isWireframe() ? "WIRE" : "solid", gameTick);
			window.setTitle(buf); titleTimer = 0;
		}
	}

	audio.shutdown();
	rayTracing.shutdown();
	denoising.shutdown();
	meshShader.shutdown();
	renderer.shutdown();
	postProcess.shutdown();
	debugUI.shutdown();
	input.shutdown();
	jobs.shutdown();
	window.close();
	Engine::shutdown();
	return 0;
}
