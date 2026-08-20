# EnderEngineCore

EnderEngineCore is a library that capsulates a set of low-level APIs and has some basic settings used for game development. **But don't use this library to develop games immediately!**

**If you are a NOVICE in this field, DO NOT USE THIS LIBRARY!**

## Getting Started
**This version doesn't have pre-compiled release at present, so you need to build from source.**

### Before Building
You need to comfirm that you have installed the following items:
- Microsoft Visual Studio 18 2026
- MinGW (for CGO)
- Xmake
- CMake
- zlib (Can be recognized by CMake)
- OpenSSL (Can be recognized by CMake)
- Golang
- Git
- PhysX 5.6.1
### Building
Clone first:
```cmd
git clone https://github.com/TheEnderStudio/EnderEngineCore.git
```
Then build the BuildHelper:
```cmd
cd Tools/BuildHelper
go mod tidy
go build -trimpath -ldflags "-s -w" -o BuildHelper.exe .
```
Then run BuildHelper.exe and configure the root directory of engine, the path of PhysX 5 (warning: if you use NVIDIA-Omniverse/PhysX, type <PhysX's root dir>/physx). Click `下载并构建全部` button and wait for completing.

Finally generate vsxmake project and build demo:
```cmd
cd ../..
xmake project -k vsxmake2026
```
Open the solution in `vsxmake2026` then build.

### After building
Copy the following files to `Binary/windows-xxx-xxx` (these files can be found in `Backends` and `ThirdParty`):
- EnderVFiles.dll
- freetyped.dll/freetype.dll
- glfw3.dll
- libcrypto-3-x64.dll
- OIS_d.dll
- phonon.dll
- PhysX_64.dll
- PhysXCommon_64.dll
- PhysXCooking_64.dll
- PhysXFoundation_64.dll
- spng.dll
- zd.dll/z.dll

These symbols also can be found (DEBUG ONLY):
- OIS_d.pdb
- LowLevel_static_64.pdb
- LowLevelAABB_static_64.pdb
- LowLevelDynamics_static_64.pdb
- PhysX_64.pdb
- PhysXCharacterKinematic_static_64.pdb
- PhysXCommon_64.pdb
- PhysXCooking_64.pdb
- PhysXExtensions_static_64.pdb
- PhysXFoundation_64.pdb
- PhysXPvdSDK_static_64.pdb
- PhysXTask_static_64.pdb
- PhysXVehicle2_static_64.pdb
- SceneQuery_static_64.pdb
- SimulationController_static_64.pdb
- phonon.pdb

If you need to run the demo, please download the zip file from the following link and unzip: `https://github.com/sally4953/Resources/releases/download/Tag0001/GameData.zip`

## License
This project is under MIT license, see `LICENSE` for more details.

## The Use of Third-Party Libraries

This project utilizes the following third-party libraries:
- Diligent Engine `https://github.com/DiligentGraphics/DiligentEngine`
- OIS `https://github.com/wgois/OIS`
- PhysX `https://github.com/NVIDIA-Omniverse/PhysX`
- SteamAudio `https://github.com/ValveSoftware/steam-audio`
- cxxopts `https://github.com/jarro2783/cxxopts`
- EnderVFiles `https://github.com/sally4953/EnderVFiles`
- fastgltf `https://github.com/spnda/fastgltf`
- FreeType `https://github.com/freetype/freetype`
- glfw `https://github.com/glfw/glfw`
- glm `https://github.com/g-truc/glm`
- gtest `https://github.com/google/googletest`
- libspng `https://github.com/randy408/libspng`
- spdlog `https://github.com/gabime/spdlog`
- stb `https://github.com/nothings/stb`
- uuid_v4 `https://github.com/crashoz/uuid_v4`

### Third-Party Licenses
See `ThirdPartyLicenses.txt` for more details.