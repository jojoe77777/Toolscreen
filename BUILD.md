# Building Toolscreen

## Prerequisites

- Visual Studio 2022 (or Build Tools)
- Windows 10/11
- 7-Zip or Java JDK

## Dependencies

- MinHook - https://github.com/TsudaKageyu/minhook
- ImGui - https://github.com/ocornut/imgui
- GLEW - https://glew.sourceforge.net/
- toml++ - https://github.com/marzer/tomlplusplus
- nlohmann/json - https://github.com/nlohmann/json

Recommended: Use vcpkg for dependency management.

## Setup

### Install dependencies

```batch
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
bootstrap-vcpkg.bat
vcpkg integrate install
vcpkg install imgui[opengl3-binding,win32-binding]:x64-windows
vcpkg install tomlplusplus:x64-windows
vcpkg install nlohmann-json:x64-windows
vcpkg install glew:x64-windows
```

### Create wrapper headers

vcpkg installs headers in different paths than the code expects. Create these wrapper files:

**toml.hpp** (project root):
```cpp
#include <toml++/toml.hpp>
```

**json.hpp** (project root):
```cpp
#include <nlohmann/json.hpp>
```

**include/imgui/imgui.h**:
```cpp
#include <imgui.h>
```

**include/imgui/backends/imgui_impl_opengl3.h**:
```cpp
#include <imgui_impl_opengl3.h>
```

**include/imgui/backends/imgui_impl_win32.h**:
```cpp
#include <imgui_impl_win32.h>
```

## Build

```batch
MSBuild Toolscreen.vcxproj /p:Configuration=Release /p:Platform=x64
```

Output: `bin\Release\Toolscreen.dll`

## Create JAR

1. Download a release JAR from GitHub
2. Extract: `7z x Toolscreen-X.X.X.jar -ojar_temp`
3. Replace DLL: `copy /Y bin\Release\Toolscreen.dll jar_temp\dlls\Toolscreen.dll`
4. Repack: `cd jar_temp && 7z a -tzip ..\Toolscreen-custom.jar *`
5. Test: Place JAR in Minecraft instance folder and run
