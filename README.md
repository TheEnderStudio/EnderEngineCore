# EnderEngineCore

EnderEngineCore is a library that capsulates a set of low-level APIs and has some basic settings used for game development. **But don't use this library to develop games immediately!**

**If you are a NOVICE in this field, DO NOT USE THIS LIBRARY!**

## Getting Started
**This version doesn't have pre-compiled release at present, so you need to build from source.**

### Before Building
**Please ensure that there is at least 25GB of space available on the disk you are using.**
You need to confirm that you have installed the following items:
- Microsoft Visual Studio 18 2026 `https://visualstudio.microsoft.com/downloads/`
- MinGW (for CGO) `https://www.mingw-w64.org/downloads/`
- Xmake `https://github.com/xmake-io/xmake/`
- CMake `https://cmake.org/download/`
- zlib (Can be recognized by CMake) `https://github.com/madler/zlib`
- OpenSSL (Can be recognized by CMake) `https://github.com/openssl/openssl`
- Golang `https://go.dev/dl/`
- Git `https://git-scm.com/install/`
- PhysX 5.6.1 `https://github.com/NVIDIA-Omniverse/PhysX`
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
- Diligent Engine `https://github.com/DiligentGraphics/DiligentEngine` (Apache-2.0 license)
- OIS `https://github.com/wgois/OIS` (Zlib license)
- PhysX `https://github.com/NVIDIA-Omniverse/PhysX` (BSD-3-Claude license)
- SteamAudio `https://github.com/ValveSoftware/steam-audio` (Apache-2.0 license)
- cxxopts `https://github.com/jarro2783/cxxopts` (MIT license)
- EnderVFiles `https://github.com/sally4953/EnderVFiles` (MIT license)
- fastgltf `https://github.com/spnda/fastgltf` (MIT license)
- FreeType `https://github.com/freetype/freetype` (FreeType License)
- glfw `https://github.com/glfw/glfw` (Zlib license)
- glm `https://github.com/g-truc/glm` (MIT license)
- gtest `https://github.com/google/googletest` (BSD-3-Claude license)
- libspng `https://github.com/randy408/libspng` (BSD-2-Claude license)
- spdlog `https://github.com/gabime/spdlog` (MIT license)
- stb `https://github.com/nothings/stb` (MIT license)
- uuid_v4 `https://github.com/crashoz/uuid_v4` (MIT license)

### Third-Party Licenses
See `ThirdPartyLicenses.txt` for more details.
