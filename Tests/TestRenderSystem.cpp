#include <Engine/Rendering/RenderSubsystem.hpp>
#include <Engine/Rendering/Errors.hpp>
#include <Engine/Rendering/RenderTypes.hpp>
#include <Engine/Rendering/RenderEvents.hpp>
#include <Engine/Core/Log.hpp>
#include <gtest/gtest.h>

using namespace EnderEngine;
using namespace EnderEngine::Rendering;

// ===================================================================
// Basic lifecycle tests
// ===================================================================

TEST(RenderSubsystemTest, ConstructAndDestruct) {
	RenderSubsystem renderer;
	EXPECT_EQ(renderer.name(), "Rendering");
	EXPECT_FALSE(renderer.isReady());
}

TEST(RenderSubsystemTest, IsNotReadyBeforeInitialize) {
	RenderSubsystem renderer;
	EXPECT_FALSE(renderer.isReady());
}

// ===================================================================
// Resource creation without initialization (should fail)
// ===================================================================

TEST(RenderSubsystemTest, CreateShaderBeforeInitializeFails) {
	RenderSubsystem renderer;
	ShaderDesc desc;
	desc.stage = ShaderStage::Vertex;
	desc.source = "float4 main() : SV_POSITION { return float4(0,0,0,1); }";
	auto result = renderer.createShader(desc);
	EXPECT_TRUE(result.isErr());
	EXPECT_EQ(result.error(), RenderError::NotInitialized);
}

TEST(RenderSubsystemTest, CreateMeshBeforeInitializeFails) {
	RenderSubsystem renderer;
	MeshDesc desc;
	auto result = renderer.createMesh(desc);
	EXPECT_TRUE(result.isErr());
	EXPECT_EQ(result.error(), RenderError::NotInitialized);
}

TEST(RenderSubsystemTest, CreateTextureBeforeInitializeFails) {
	RenderSubsystem renderer;
	TextureDesc desc;
	auto result = renderer.createTexture(desc);
	EXPECT_TRUE(result.isErr());
	EXPECT_EQ(result.error(), RenderError::NotInitialized);
}

TEST(RenderSubsystemTest, CreateMaterialBeforeInitializeFails) {
	RenderSubsystem renderer;
	MaterialDesc desc;
	PSOHandle invalidPso;
	auto result = renderer.createMaterial(desc, invalidPso);
	EXPECT_TRUE(result.isErr());
	EXPECT_EQ(result.error(), RenderError::NotInitialized);
}

TEST(RenderSubsystemTest, CreateCameraBeforeInitializeFails) {
	RenderSubsystem renderer;
	CameraDesc desc;
	auto result = renderer.createCamera(desc);
	EXPECT_TRUE(result.isErr());
	EXPECT_EQ(result.error(), RenderError::NotInitialized);
}

TEST(RenderSubsystemTest, CreateLightBeforeInitializeFails) {
	RenderSubsystem renderer;
	LightDesc desc;
	auto result = renderer.createLight(desc);
	EXPECT_TRUE(result.isErr());
	EXPECT_EQ(result.error(), RenderError::NotInitialized);
}

// ===================================================================
// Handle validation tests
// ===================================================================

TEST(RenderSubsystemTest, InvalidHandleBehavior) {
	RenderSubsystem renderer;
	CameraHandle invalidHandle;
	EXPECT_FALSE(invalidHandle.isValid());

	auto result = renderer.updateCamera(invalidHandle, CameraDesc{});
	EXPECT_TRUE(result.isErr());
}

// ===================================================================
// Events test (no window needed)
// ===================================================================

TEST(RenderSubsystemTest, EventTypesExist) {
	FrameBeginEvent beginEvent;
	beginEvent.frameNumber = 1;
	EXPECT_EQ(beginEvent.frameNumber, 1U);

	FrameEndEvent endEvent;
	endEvent.frameNumber = 1;
	EXPECT_EQ(endEvent.frameNumber, 1U);

	SwapChainResizeEvent resizeEvent;
	resizeEvent.w = 800;
	resizeEvent.h = 600;
	EXPECT_EQ(resizeEvent.w, 800U);
	EXPECT_EQ(resizeEvent.h, 600U);
}

// ===================================================================
// Types tests
// ===================================================================

TEST(RenderSubsystemTest, RenderHandleEquality) {
	MeshHandle h1{ 1, 1 };
	MeshHandle h2{ 1, 1 };
	MeshHandle h3{ 2, 1 };
	MeshHandle h4{ 1, 2 };

	EXPECT_TRUE(h1 == h2);
	EXPECT_FALSE(h1 == h3);
	EXPECT_FALSE(h1 == h4);
	EXPECT_TRUE(h1.isValid());
}

TEST(RenderSubsystemTest, InvalidHandle) {
	MeshHandle invalid;
	EXPECT_FALSE(invalid.isValid());
	EXPECT_EQ(invalid.index, 0xFFFFFFFFU);
}

// ===================================================================
// Error code test
// ===================================================================

TEST(RenderSubsystemTest, ErrorToString) {
	EXPECT_STREQ(ToString(RenderError::None), "None");
	EXPECT_STREQ(ToString(RenderError::DeviceCreationFailed), "DeviceCreationFailed");
	EXPECT_STREQ(ToString(RenderError::NotInitialized), "NotInitialized");
	EXPECT_STREQ(ToString(RenderError::InvalidHandle), "InvalidHandle");
}

// ===================================================================
// Subsystem lifecycle with window
// (requires GLFW + Diligent, so this is an integration test)
// ===================================================================

TEST(RenderSubsystemTest, SetWindowBeforeInitialize) {
	RenderSubsystem renderer;
	// Setting window is valid before initialize
	renderer.setWindow(nullptr); // null window is OK before init
	EXPECT_FALSE(renderer.isReady());
}
