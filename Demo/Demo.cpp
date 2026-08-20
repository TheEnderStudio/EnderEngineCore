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
	F32 moveSpeed = 6.0f, lookSpeed = 0.15f;
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
	renderer.setMSAASampleCount(4);
	renderer.initialize();

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
	ppCfg.sampleCount = 4; // MSAA 4x
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

	// GPU culling buffers
	ComputeBuf cullInstBuf = compute.createStructuredBuffer(1000, sizeof(CullingInstance), true);
	ComputeBuf cullVisBuf = compute.createStructuredBuffer(1000, sizeof(UInt32), true);
	ComputeBuf cullCBBuf = compute.createConstantBuffer(sizeof(FrustumPlanes) + sizeof(UInt32) * 4, true);
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
	bindClip("intro",   ERes("bgm/Intro.wav"),   introH);
	bindClip("mid",     ERes("bgm/Mid.wav"),     midH);
	bindClip("climax1", ERes("bgm/Climax1.wav"), c1H);
	bindClip("climax2", ERes("bgm/Climax2.wav"), c2H);
	bindClip("climax3", ERes("bgm/Climax3.wav"), c3H);
	bindClip("end",     ERes("bgm/End.wav"),     endH);

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
	debugUI.subscribe<DebugUIRenderEvent>(debugUIObj, [&](const DebugUIRenderEvent&) {
		debugUI.beginWindow("Post-Process Settings");
		bool changed = false;
		bool customChanged = false;
		checkIfChanged<F32>(ppCfg.toneMap.exposure, changed, [&](F32& v) { debugUI.sliderFloat("Exposure", &v, 0.1f, 5.0f); });
		checkIfChanged<F32>(ppCfg.toneMap.vignette, changed, [&](F32& v) { debugUI.sliderFloat("Vignette", &v, 0, 1.0f); });
		checkIfChanged<F32>(ppCfg.toneMap.saturation, changed, [&](F32& v) { debugUI.sliderFloat("Saturation", &v, 0, 2.0f); });
		checkIfChanged<F32>(ppCfg.toneMap.contrast, changed, [&](F32& v) { debugUI.sliderFloat("Contrast", &v, 0.5f, 2.0f); });
		checkIfChanged<bool>(ppCfg.bloom.enabled, changed, [&](bool& v) { String str = fmt::format("Bloom: {}", v ? "ON" : "OFF"); if (debugUI.button(str.c_str())) { v = !v; } });
		checkIfChanged<F32>(ppCfg.bloom.intensity, changed, [&](F32& v) { debugUI.sliderFloat("Bloom Intensity", &v, 0, 1.0f); });
		checkIfChanged<F32>(ppCfg.bloom.radius, changed, [&](F32& v) { debugUI.sliderFloat("Bloom Radius", &v, 0, 1.0f); });
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
		{
			static size_t visibleCount = 0;
			// Use the GPU-computed count from args buffer readback (logged above)
			debugUI.text("GPU cull: {} / 1000 visible", gpuVisCount);
		}
		debugUI.endWindow();

		debugUI.beginWindow("Skybox");
		static int skyMode = 0;
		static const char* skyNames[] = { "Day", "Sunset", "Night", "Texture" };
		int totalModes = skyCubeTexHandle.isValid() ? 4 : 3;
		if (debugUI.button(fmt::format("Sky: {}", skyNames[skyMode]).c_str())) {
			skyMode = (skyMode + 1) % totalModes;
			RenderSubsystem::SkyboxDesc sd;
			if (skyMode == 3) {
				sd.skyCubeTex = skyCubeTexHandle;
			}
			else if (skyMode == 0) {
				// Day: blue top, gray horizon
				for (int i = 0; i < 4; i++) sd.corners[i] = Vec4(0.3f + (i & 1) * 0.1f, 0.5f + (i & 2) * 0.05f, 0.9f + ((i >> 1) & 1) * 0.05f, 1);
				for (int i = 4; i < 8; i++) sd.corners[i] = Vec4(0.5f, 0.55f, 0.6f, 1);
			}
			else if (skyMode == 1) {
				// Sunset: orange top, dark purple horizon
				for (int i = 0; i < 4; i++) sd.corners[i] = Vec4(0.9f, 0.4f + ((i & 2) >> 1) * 0.15f, 0.15f + ((i >> 1) & 1) * 0.1f, 1);
				for (int i = 4; i < 8; i++) sd.corners[i] = Vec4(0.3f, 0.15f, 0.25f, 1);
			}
			else {
				// Night: dark blue top, black bottom
				for (int i = 0; i < 4; i++) sd.corners[i] = Vec4(0.05f, 0.05f, 0.2f + (i & 1) * 0.05f, 1);
				for (int i = 4; i < 8; i++) sd.corners[i] = Vec4(0.02f, 0.02f, 0.05f, 1);
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

		UInt32 fbW, fbH;
		window.getFramebufferSize(fbW, fbH);
		if (fbW != ww || fbH != wh) {
			ww = fbW; wh = fbH;
			renderer.resize(ww, wh);
			postProcess.resize(ww, wh);
			debugUI.resize(ww, wh);
			render2d.setScreenSize(ww, wh);
			ui.setScreenSize(ww, wh);
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
			LightDesc ld; ld.dir = glm::normalize(sunDir); ld.color = Vec3(1.0f, 0.98f, 0.9f); ld.intensity = 2.0f;
			renderer.updateLight(sunHandle, ld);
		}

		fly.update(input, dt, &physicsBodies);
		renderer.updateCamera(camHandle.value(), fly.toDesc(ww, wh));

		// Distribute shadow cascades + bind SRV
		{
			auto camDesc = fly.toDesc(ww, wh);
			Vec3 sunDirNorm = glm::normalize(Vec3(cos(glm::radians(sunYaw)) * cos(glm::radians(sunPitch)),
				-sin(glm::radians(sunPitch)), sin(glm::radians(sunYaw)) * cos(glm::radians(sunPitch))));
			F32 yr = glm::radians(fly.yaw), pr = glm::radians(fly.pitch);
			Vec3 fwd(cos(pr) * cos(yr), sin(pr), cos(pr) * sin(yr));
			shadow.distribute(sunDirNorm, fly.pos, fly.pos + fwd, Vec3(0, 1, 0), glm::radians(camDesc.fov), (F32)ww / (F32)wh, camDesc.nearP, camDesc.farP);
			renderer.clearShadowCascades(shadow);
			for (auto& terrMesh : terrian)
				renderer.renderShadowPass(shadow, terrMesh, Transform{}.computeWorldMatrix());
			for (auto& wallMesh : wall)
				renderer.renderShadowPass(shadow, wallMesh, Transform{ .position = Vec3(20, -19, 20) }.computeWorldMatrix());
			// Indirect shadow for Furinas (all instances, GPU-culled)
			for (size_t mi = 0; mi < model.size(); mi++)
				renderer.renderShadowPassIndirect(shadow, model[mi], wmSRV, idxSRV, argsBuf, static_cast<UInt32>(mi * sizeof(IndirectDrawArgs)));
		renderer.setShadowSRV(shadow.getSRV());
		{
			Mat4 uv[4];
			for (UInt32 ci = 0; ci < 4; ci++) uv[ci] = shadow.getWorldToShadowMapUVDepth(ci);
			renderer.setShadowData(uv, shadow.getCascadeSplitDistances());
		}
		}

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
		renderer.setRenderTarget(postProcess.getHDRRTV());
		renderer.beginFrame();

		// GPU frustum culling + indirect draw
		{
			Mat4 view, proj; renderer.getCameraMatrices(view, proj);
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
			// Pre-fill indirect args per mesh
			IndirectDrawArgs argsTmpl[8] = {};
			if (!model.empty()) {
				for (size_t mi = 0; mi < model.size() && mi < 8; mi++) {
					auto sub = renderer.getSubMesh(model[mi], 0);
					argsTmpl[mi].indexCount = static_cast<UInt32>(sub.indexCount);
					argsTmpl[mi].firstIndex = static_cast<UInt32>(sub.indexOffset);
					argsTmpl[mi].baseVertex = static_cast<UInt32>(sub.vertexOffset);
				}
				compute.updateBuffer(argsBuf, argsTmpl, sizeof(argsTmpl));
				compute.updateCullingCB(cullCBBuf, fp, 1000,
					argsTmpl[0].indexCount, argsTmpl[0].firstIndex, argsTmpl[0].baseVertex);
			}
			// Clear counter before dispatch
			{ UInt32 zero = 0; compute.updateBuffer(counterBuf, &zero, sizeof(zero)); }
			compute.dispatchCullingCompact(cullInstBuf, cullCBBuf, cullVisBuf, indicesBuf, counterBuf, 1000);
			// Copy instanceCount to all mesh entries
			{
				UInt32 visCount = 0;
				compute.readback(cullStgBuf, counterBuf, sizeof(UInt32), &visCount);
				gpuVisCount = visCount;
				for (size_t mi = 0; mi < model.size() && mi < 8; mi++) {
					argsTmpl[mi].instanceCount = visCount;
				}
				compute.updateBuffer(argsBuf, argsTmpl, sizeof(argsTmpl));
			}
			for (size_t mi = 0; mi < model.size(); mi++)
				renderer.drawMeshInstancedIndirect(model[mi], wmSRV, idxSRV, argsBuf, static_cast<UInt32>(mi * sizeof(IndirectDrawArgs)));
		}

		for (auto& terrMesh : terrian) {
			static const Transform trans{ .position = Vec3(0, 0, 0) };
			renderer.drawMesh(terrMesh, trans);
		}
		for (auto& wallMesh : wall) {
			static const Transform trans{ .position = Vec3(20, -19, 20) };
			renderer.drawMesh(wallMesh, trans);
		}

		// Draw camera body model when free-fly (F5 detached)
		if (!fly.boundToBody && fly.cameraBody != InvalidRigidBody && !model.empty()) {
			Transform ct = physicsBodies.getWorldTransform(fly.cameraBody);
			Mat4 cameraWorld = ct.computeWorldMatrix();
			Vector<Mat4> camMats = { cameraWorld };
			for (auto& meh : model) {
				renderer.drawMeshInstanced(meh, camMats);
			}
		}

		// Fog cloud around camera (toggle with '1')
		{
			static bool fogEnabled = true;
			if (st.wasKeyPressedThisFrame(KeyCode::Num1)) { fogEnabled = !fogEnabled; EInfo("Fog: {}", fogEnabled ? "ON" : "OFF"); }
			if (fogEnabled) {
				static constexpr int N = 240;
				static Vector<RenderSubsystem::BillboardDesc> fogs(N);
				F32 t = (F32)(gameTick * 0.015);
				for (int i = 0; i < N; i++) {
					F32 phi = acosf(1.0f - 2.0f * ((F32)i + 0.5f) / N);
					F32 theta = glm::two_pi<F32>() * (F32)i * 1.61803398875f;
					F32 r = 3.0f + sinf(t * 0.7f + i * 0.5f) * 0.4f + sinf(i * 2.3f) * 0.6f;
					fogs[i].position = fly.pos + Vec3(
						sinf(phi) * cosf(theta) * r,
						cosf(phi) * r + sinf(t + i * 0.3f) * 0.2f,
						sinf(phi) * sinf(theta) * r);
					fogs[i].size = Vec2(2.5f + sinf(i * 1.7f + t * 0.5f) * 1.0f);
					fogs[i].color = Vec4(1, 1, 1, 0.3f + sinf(i * 2.6f + t) * 0.4f);
				}
				renderer.drawBillboards(fogs);
			}
		}

		renderer.endFrame();
		renderer.setRenderTarget(nullptr);

		// Render UI layers that receive post-processing (before execute)
		ui.beginFrame(false);
		ui.endFrame();

		postProcess.execute();

		// Render UI layers that skip post-processing (after execute, before debug UI)
		ui.beginFrame(true);
		ui.endFrame();

		debugUI.endFrameAndRender();
		postProcess.present();

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
	renderer.shutdown();
	postProcess.shutdown();
	debugUI.shutdown();
	input.shutdown();
	jobs.shutdown();
	window.close();
	Engine::shutdown();
	return 0;
}
