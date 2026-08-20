set_project("EnderEngine")
set_xmakever("2.8.0")
add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate")

-- ---------------------------------------------------------------------------
-- Language & Standard
-- ---------------------------------------------------------------------------
set_languages("c++20", "c17")

-- ---------------------------------------------------------------------------
-- Platform Defines
-- ---------------------------------------------------------------------------
if is_plat("windows") then
	add_defines("EE_WINDOWS", "NOMINMAX", "WIN32_LEAN_AND_MEAN", "_CRT_SECURE_NO_WARNINGS", "PLATFORM_WIN32")
elseif is_plat("linux") then
	add_defines("EE_LINUX", "PLATFORM_LINUX")
end

-- ---------------------------------------------------------------------------
-- Output Directories
-- ---------------------------------------------------------------------------
set_targetdir("Binary/$(plat)-$(arch)-$(mode)")
set_objectdir("Object")

-- ---------------------------------------------------------------------------
-- Include Directories
-- ---------------------------------------------------------------------------
add_includedirs("Include")
add_includedirs("ThirdParty/uuid_v4")
add_includedirs("ThirdParty/glm")
add_includedirs("ThirdParty/spdlog/include")
add_includedirs("ThirdParty/glfw/include")
add_includedirs("ThirdParty/fastgltf/include")
add_includedirs("ThirdParty/gtest/include")
add_includedirs("ThirdParty/miniaudio")
add_includedirs("ThirdParty/stb")
add_includedirs("ThirdParty/libspng/include")
add_includedirs("ThirdParty/FreeType/include/freetype2")
add_includedirs("ThirdParty/EnderVFiles/include")
add_includedirs("Backends/SteamAudio/include")
add_includedirs("Backends/DiligentEngine/include")
add_includedirs("Backends/DiligentEngine/include/DiligentTools/Imgui/interface")
add_includedirs("Backends/DiligentEngine/include/DiligentTools/ThirdParty/imgui")
add_includedirs("Backends/DiligentEngine")
add_includedirs("Backends/OIS/include")
add_includedirs("Backends/PhysX/include")

-- ---------------------------------------------------------------------------
-- Mode Configuration
-- ---------------------------------------------------------------------------
if is_mode("debug") then
	add_defines("EE_DEBUG")
	set_symbols("debug")
	set_optimize("none")
	if is_plat("windows") then
		add_cxflags("/MDd", {force = true})
	end
else
	set_optimize("fastest")
	if is_plat("windows") then
		-- Release: disable RTTI and exceptions
		add_cxflags("/GR-", "/EHs-c-", "/MD", {force = true})
	elseif is_plat("linux") then
		add_cxflags("-fno-rtti", "-fno-exceptions", {force = true})
	end
end

-- ---------------------------------------------------------------------------
-- Platform System Links
-- ---------------------------------------------------------------------------
if is_plat("windows") then
	add_syslinks("user32", "dbghelp", "kernel32", "gdi32", "shell32", "comdlg32", "d3d11", "d3d12", "dxgi", "dxguid", "d3dcompiler")
	add_cxflags("/W4", "/utf-8", {force = true})
elseif is_plat("linux") then
	add_syslinks("pthread", "dl")
	add_cxflags("-Wall", "-Wextra", {force = true})
end

-- ---------------------------------------------------------------------------
-- Third-party Library Paths (mode-dependent)
-- ---------------------------------------------------------------------------
if is_mode("debug") then
	add_linkdirs("ThirdParty/spdlog/lib")
	add_linkdirs("ThirdParty/glfw/lib-vc2022")
	add_linkdirs("ThirdParty/fastgltf/lib")
	add_linkdirs("Backends/DiligentEngine/lib/DiligentCore/Debug")
	add_linkdirs("Backends/DiligentEngine/lib/DiligentTools/Debug")
	add_linkdirs("Backends/DiligentEngine/lib/DiligentFX/Debug")
	add_linkdirs("Backends/DiligentEngine/lib/NvApi/amd64")
	add_linkdirs("Backends/OIS/lib")
	add_linkdirs("Backends/PhysX/bin/win.x86_64.vc143.md/debug")
	add_linkdirs("Backends/SteamAudio/lib/windows-x64")
	add_linkdirs("ThirdParty/libspng/lib/Debug")
	add_linkdirs("ThirdParty/FreeType/lib")
	add_linkdirs("ThirdParty/EnderVFiles/lib")
else
	add_linkdirs("ThirdParty/spdlog/lib")
	add_linkdirs("ThirdParty/glfw/lib-vc2022")
	add_linkdirs("ThirdParty/fastgltf/lib")
	add_linkdirs("Backends/DiligentEngine/lib/DiligentCore/RELEASE")
	add_linkdirs("Backends/DiligentEngine/lib/DiligentTools/RELEASE")
	add_linkdirs("Backends/DiligentEngine/lib/DiligentFX/RELEASE")
	add_linkdirs("Backends/DiligentEngine/lib/NvApi/amd64")
	add_linkdirs("Backends/OIS/lib")
	add_linkdirs("Backends/PhysX/bin/win.x86_64.vc143.md/release")
	add_linkdirs("Backends/SteamAudio/lib/windows-x64")
	add_linkdirs("ThirdParty/libspng/lib/Release")
	add_linkdirs("ThirdParty/FreeType/lib")
	add_linkdirs("ThirdParty/EnderVFiles/lib")
end

-- ---------------------------------------------------------------------------
-- EnderEngineCore (Shared DLL)
-- ---------------------------------------------------------------------------
target("EnderEngineCore")
	set_kind("shared")
	add_defines("EE_EXPORTS", { private = true })

	add_includedirs("Include/Engine", { private = true })

	-- Headers
	add_headerfiles("Include/Engine/**/*.hpp")
	add_headerfiles("Include/Engine/**/*.h")

	-- Sources
	add_files("Source/Engine/**/*.cpp")
	add_files("Source/Engine/**/*.c")
	add_headerfiles("Source/Engine/**/*.hpp")

	-- Macros
	add_defines("EE_PHYSICS_BACKEND_PHYSX5")

	-- Third-party libraries
	if is_mode("debug") then
		add_links("spdlogd")
		add_links("glfw3dll")
		add_links("fastgltfd")
		add_links("OIS_d")
		add_links("spng")
		add_links("freetyped")
		add_links("DiligentCore")
		add_links("Diligent-Imgui")
		add_links("GraphicsEngineD3D11_64d")
		add_links("GraphicsEngineD3D12_64d")
		add_links("GraphicsEngineVk_64d")
		add_links("DiligentTools")
		add_links("DiligentFX")
		add_links("Archiver_64d")
		add_links("GenericCodeGend")
		add_links("glslangd")
		add_links("MachineIndependentd")
		add_links("OSDependentd")
		add_links("spirv-cross-cored")
		add_links("spirv-cross-glsld")
		add_links("SPIRV-Tools-opt")
		add_links("SPIRV-Tools")
		add_links("SPIRVd")
		add_links("LibJpeg")
		add_links("libpng16_staticd")
		add_links("LibTiff")
		add_links("ZLib")
		add_links("PhysX_64")
		add_links("PhysXCharacterKinematic_static_64")
		add_links("PhysXCommon_64")
		add_links("PhysXCooking_64")
		add_links("PhysXExtensions_static_64")
		add_links("PhysXFoundation_64")
		add_links("PhysXPvdSDK_static_64")
		add_links("PhysXTask_static_64")
		add_links("PhysXVehicle2_static_64")
		add_links("PVDRuntime_64")
		add_links("phonon")
		add_links("EnderVFiles")
	else
		add_links("spdlog")
		add_links("glfw3dll")
		add_links("fastgltf")
		add_links("OIS")
		add_links("spng")
		add_links("freetype")
		add_links("DiligentCore")
		add_links("Diligent-Imgui")
		add_links("GraphicsEngineD3D11_64r")
		add_links("GraphicsEngineD3D12_64r")
		add_links("GraphicsEngineVk_64r")
		add_links("DiligentTools")
		add_links("DiligentFX")
		add_links("Archiver_64r")
		add_links("GenericCodeGen")
		add_links("glslang")
		add_links("MachineIndependent")
		add_links("OSDependent")
		add_links("spirv-cross-core")
		add_links("spirv-cross-glsl")
		add_links("SPIRV-Tools-opt")
		add_links("SPIRV-Tools")
		add_links("SPIRV")
		add_links("LibJpeg")
		add_links("libpng16_static")
		add_links("LibTiff")
		add_links("ZLib")
		add_links("PhysX_64")
		add_links("PhysXCharacterKinematic_static_64")
		add_links("PhysXCommon_64")
		add_links("PhysXCooking_64")
		add_links("PhysXExtensions_static_64")
		add_links("PhysXFoundation_64")
		add_links("PhysXTask_static_64")
		add_links("PhysXVehicle2_static_64")
		add_links("phonon")
		add_links("EnderVFiles")
	end
target_end()

-- ---------------------------------------------------------------------------
-- TestCore (Test Executable)
-- ---------------------------------------------------------------------------
target("TestCore")
	set_kind("binary")
	add_deps("EnderEngineCore")
	add_files("Tests/*.cpp")

	if is_mode("debug") then
		add_linkdirs("ThirdParty/gtest/lib")
		add_links("gtestd", "gtest_maind")
	else
		add_linkdirs("ThirdParty/gtest/lib")
		add_links("gtest", "gtest_main")
	end

target_end()

-- ---------------------------------------------------------------------------
-- Demo (Optional Application)
-- ---------------------------------------------------------------------------
target("Demo")
	set_kind("binary")
	add_deps("EnderEngineCore", "EnderEngine.Extension.HelloWorld", "EnderEngine.Extension.AdaptiveMusic")
	add_files("Demo/*.cpp")

	-- Inherit include paths from EnderEngineCore
	add_includedirs("Include/Engine")
	add_includedirs("Include")
	add_includedirs("ThirdParty/glfw/include")
	add_includedirs("ThirdParty/glm")
	add_includedirs("Extensions")
	add_includedirs("Extensions/AdaptiveMusic/include")

	add_defines("EE_PHYSICS_BACKEND_PHYSX5")

	-- GLFW is already globally included and linked via EnderEngineCore,
	-- but the Demo needs to link against glfw3 directly for its own main().
	if is_mode("debug") then
		add_links("glfw3dll")
	else
		add_links("glfw3dll")
	end

	-- Windows system libs needed for GLFW and window creation
	add_syslinks("gdi32", "user32", "shell32", "d3d12", "dxgi", "d3dcompiler")

target_end()

-- ---------------------------------------------------------------------------
-- HelloWorld Extension (To show extension functions)
-- ---------------------------------------------------------------------------
target("EnderEngine.Extension.HelloWorld")
	set_kind("shared")
	add_defines("EEEXT_H0_EXPORTS", { private = true })
	add_deps("EnderEngineCore")
	add_headerfiles("Extensions/HelloWorld/HelloWorldExt.hpp")
	add_files("Extensions/HelloWorld/HelloWorldExt.cpp")
target_end()

target("EnderEngine.Extension.AdaptiveMusic")
	set_kind("shared")
	add_defines("EEEXT_A0_EXPORTS", { private = true })
	add_deps("EnderEngineCore")
	add_includedirs("Extensions/AdaptiveMusic/include")
	add_headerfiles("Extensions/AdaptiveMusic/include/EngineExt/AdaptiveMusic.hpp")
	add_files("Extensions/AdaptiveMusic/src/*.cpp")
target_end()

-- ---------------------------------------------------------------------------
-- MeshCooking (Offline tool for cooking collision meshes)
-- ---------------------------------------------------------------------------
target("MeshCooking")
	set_kind("binary")
	add_files("Tools/MeshCooking/*.cpp")
	add_includedirs("ThirdParty/fastgltf/include")
	add_includedirs("ThirdParty/cxxopts/include")
	add_includedirs("Backends/PhysX/include")
	if is_mode("debug") then
		add_linkdirs("ThirdParty/fastgltf/lib")
		add_linkdirs("Backends/PhysX/bin/win.x86_64.vc143.md/debug")
	else
		add_linkdirs("ThirdParty/fastgltf/lib")
		add_linkdirs("Backends/PhysX/bin/win.x86_64.vc143.md/release")
	end
	add_links("PhysX_64", "PhysXCommon_64", "PhysXFoundation_64", "PhysXCooking_64", "PhysXExtensions_static_64")
	if is_mode("debug") then
		add_links("fastgltfd")
	else
		add_links("fastgltf")
	end
target_end()

-- ---------------------------------------------------------------------------
-- ResourcePacker (Pack all resources to publish)
-- ---------------------------------------------------------------------------
target("ResourcePacker")
	set_kind("binary")
	add_files("Tools/ResourcePacker/*.cpp")
	add_includedirs("ThirdParty/EnderVFiles/include")
	add_linkdirs("ThirdParty/EnderVFiles/bin")
	add_links("EnderVFiles")

target_end()
