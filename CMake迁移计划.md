# 3DMigoto DX12 → CMake 迁移计划

> 调研日期：2026-05-15
> 当前构建系统：MSBuild（.vcxproj）+ PowerShell 脚本

---

## 1. 项目总览

| 子项目 | 类型 | 产物 | 依赖关系 |
|--------|------|------|----------|
| `BinaryDecompiler` | 静态库 | `BinaryDecompiler.lib` | 无（纯 C++） |
| `DirectXTK` | 静态库 | `DirectXTK.lib` | Windows SDK |
| `D3D_Shaders` | 控制台应用（独立工具） | `D3D_Shaders.exe` | `d3dcompiler.lib` |
| `DirectX11` | DLL | `d3d11.dll` | BinaryDecompiler, DirectXTK, crc32c-hw, HLSLDecompiler, Nektra, pcre2 |
| `DirectXGI` | DLL | `dxgi.dll` | Nektra, DirectXTK(Inc) |
| `D3DCompiler_46` | DLL | `D3DCompiler_46.dll` | 无 |
| `Injector` | EXE | `3DMigoto Loader.exe` | 无 |
| `InjectorLib` | DLL(x64) / EXE(Win32) | `3dmloader.exe` | 无 |

---

## 2. 外部依赖分析

| 依赖 | 位置 | 构建方式 | 是否自带 CMake |
|------|------|----------|:---:|
| **crc32c-hw-1.0.5** | `crc32c-hw-1.0.5/src/` | 源文件直接编译 | ❌ |
| **pcre2-10.30** | `pcre2-10.30/` | 当前使用预编译 `.lib` | ✅ 自带 `CMakeLists.txt` |
| **Nektra** | `Nektra/` | 预编译 `.lib` | ❌ 保持原样 |
| **DirectXTK** | `DirectXTK/` | 当前通过 `.vcxproj` 编译 | ❌ 需要新增 |
| **HLSLDecompiler** | `HLSLDecompiler/` | 源文件直接编译 | ❌ |

---

## 3. 迁移策略

### 3.1 目录结构建议（CMake 化后）

```
3DMigotoDX12/
├── CMakeLists.txt              # 根 CMake（顶层项目）
├── cmake/
│   ├── CompilerOptions.cmake   # 统一编译器选项
│   └── FindNektra.cmake        # FindNektra 模块
├── src/                        # [可选] 根目录公共源码保持原位
├── BinaryDecompiler/
│   ├── CMakeLists.txt
│   └── ...
├── D3D_Shaders/
│   ├── CMakeLists.txt
│   └── ...
├── D3DCompiler_46/
│   ├── CMakeLists.txt
│   └── ...
├── D3DCompiler/                # 共享代码，被 D3DCompiler_xx 引用
├── DirectX11/
│   ├── CMakeLists.txt
│   └── ...
├── DirectXGI/
│   ├── CMakeLists.txt
│   └── ...
├── Injector/
│   ├── CMakeLists.txt
│   └── ...
├── InjectorLib/
│   ├── CMakeLists.txt
│   └── ...
├── DirectXTK/
│   ├── CMakeLists.txt          # 新增
│   └── ...
├── pcre2-10.30/
│   └── CMakeLists.txt          # 已有，直接使用
├── crc32c-hw-1.0.5/
│   ├── CMakeLists.txt          # 新增
│   └── ...
├── HLSLDecompiler/
│   ├── CMakeLists.txt          # 新增
│   └── ...
├── Nektra/                     # 预编译库，不构建
└── scripts/
    └── build-cmake.ps1         # 新的构建脚本
```

### 3.2 阶段划分

#### 阶段一：基础设施（预计 1-2 天）

1. **创建根 `CMakeLists.txt`**
   - 设定 CMake 最低版本要求（推荐 `cmake_minimum_required(VERSION 3.20)`）
   - 定义项目名 `3DMigotoDX12`
   - 配置 C++ 标准为 C++14
   - 设置 C 标准为 C17
   - 添加 `add_compile_definitions()` 统一全局宏

2. **创建 `cmake/CompilerOptions.cmake`**
   - 统一的编译选项设置：
     - 警告级别 `/W3`
     - 异常处理 `/EHsc`（对应现有 `/EHac`，Async模式）
     - Debug: `/MTd` (MultiThreadedDebug), Release: `/MT` (MultiThreaded)
     - Release 启用 `/O2`, `/GL` (WholeProgramOptimization), `/LTCG`
     - Debug 禁用优化 `/Od`
     - 缓冲区安全检查 Debug 开、Release 关

3. **创建 `cmake/FindNektra.cmake`**
   - 定位 `Nektra/` 目录下的预编译 `.lib` 文件
   - 区分 x86/x64 和 Debug/Release

#### 阶段二：独立库迁移（预计 1-2 天）

4. **`BinaryDecompiler/CMakeLists.txt`**
   - `add_library(BinaryDecompiler STATIC ...)`
   - 源文件：`decode.cpp`, `decodeDX9.cpp`, `reflect.cpp`
   - Include 目录：`include/`, `internal_includes/`, 项目根目录
   - 注意：该库在现有项目中作为 StaticLibrary 构建（MultiThreaded / MultiThreadedDebug）

5. **`DirectXTK/CMakeLists.txt`**
   - 由于 DirectXTK 源文件很多（约 40+ .cpp），建议直接写新的 CMakeLists.txt
   - 包含 `Inc/` 和 `Src/` 目录
   - 作为静态库构建
   - 参考现有 `.vcxproj` 中的 `ClCompile` 列表

6. **`crc32c-hw-1.0.5/CMakeLists.txt`**
   - `add_library(crc32c STATIC ...)`
   - 源文件：`src/crc32c.cpp`, `src/generated-constants.cpp`
   - 定义宏：`CRC32C_STATIC`

7. **`HLSLDecompiler/CMakeLists.txt`**
   - `add_library(HLSLDecompiler OBJECT ...)` 或直接纳入 DirectX11
   - 源文件：`DecompileHLSL.cpp`

#### 阶段三：可执行文件/DLL 迁移（预计 1-2 天）

8. **`DirectX11/CMakeLists.txt`**
   - `add_library(d3d11 SHARED ...)`
   - 链接 BinaryDecompiler, DirectXTK, crc32c, HLSLDecompiler, Nektra, pcre2
   - Windows SDK 库：`d3dcompiler`, `dxgi`, `XINPUT9_1_0`, `Shlwapi`, `Dbghelp`
   - 模块定义文件：`d3d11Wrapper.def`
   - 预处理定义：`MIGOTO_DX=11`, `_USRDLL`, `_WINDOWS` 等
   - 资源文件：`DirectX11.rc`
   - **特别注意**：现有项目将 `crc32c.cpp`, `D3D_Shaders/Assembler.cpp`, `D3D_Shaders/SignatureParser.cpp`, `HLSLDecompiler/DecompileHLSL.cpp`, `iid.cpp`, `ini_parser_lite.cpp`, `util.cpp` 等根目录/外部源文件直接编译进 DirectX11，CMake 中也需要包含这些源文件

9. **`D3DCompiler_46/CMakeLists.txt`**
   - `add_library(D3DCompiler_46 SHARED ...)`
   - 源文件：`../D3DCompiler/d3dcWrapper.cpp`
   - 模块定义文件：`D3DCompiler_46.def`

10. **`DirectXGI/CMakeLists.txt`**
    - `add_library(dxgi SHARED ...)`
    - 源文件：`DXGIWrapper.cpp`
    - 模块定义文件：`d3dxgiWrapper.def`
    - 链接 Nektra
    - Windows SDK 库：`dxgi`, `d3d11`, `d3dcompiler`, `comctl32`, `dinput8` 等

11. **`Injector/CMakeLists.txt`**
    - `add_executable(3DMigoto\ Loader WIN32 ...)`
    - 源文件：`Injector.cpp`
    - 链接 `version.lib`

12. **`InjectorLib/CMakeLists.txt`**
    - x64: `add_library(3dmloader SHARED ...)`（当前 x64 Debug/Release 为 DLL）
    - Win32: `add_executable(3dmloader WIN32 ...)`（当前 Win32 为 EXE）
    - CMake 可以通过 `WIN32` 或 `SHARED` + Generator expressions 处理
    - 源文件：`Injector.cpp`, `../ini_parser_lite.cpp`
    - 链接 `version.lib`

13. **`D3D_Shaders/CMakeLists.txt`**
    - `add_executable(D3D_Shaders ...)`
    - 源文件：`Assembler.cpp`, `Shaders.cpp`, `SignatureParser.cpp`
    - 链接 `d3dcompiler.lib`

#### 阶段四：后处理与脚本（预计 1 天）

14. **Post-Build 事件迁移**
    - `DirectX11` 的 post-build：复制 `Dependencies/` 目录到输出目录
    - 复制 `d3dcompiler_47.dll`（来自 Windows SDK）

15. **构建脚本**
    - 创建 `scripts/build-cmake.ps1`
    - 使用 `cmake --build` 替代 MSBuild 直接调用
    - 支持 Debug/Release + x64/Win32

16. **VS Code tasks.json 更新**
    - 将 MSBuild 任务替换为 CMake 任务

#### 阶段五：pcre2 集成（可选，预计 0.5 天）

17. **使用 pcre2-10.30 原生 CMake 构建**
    - 通过 `add_subdirectory(pcre2-10.30)` 引入
    - 使用 `find_package` 或直接 target_link
    - 配置 `PCRE2_STATIC`, `PCRE2_CODE_UNIT_WIDTH=8`
    - 这将替代当前 `pcre2/` 下的预编译 `.lib`

---

## 4. 关键配置对照表

### 4.1 预处理器定义

| 宏 | 作用域 | Debug | Release |
|---|--------|:-----:|:-------:|
| `CRC32C_STATIC=1` | DirectX11 | ✅ | ✅ |
| `PCRE2_STATIC` | DirectX11 | ✅ | ✅ |
| `PCRE2_CODE_UNIT_WIDTH=8` | DirectX11 | ✅ | ✅ |
| `_CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES=1` | DirectX11, D3DCompiler | ✅ | ✅ |
| `_WINDOWS` | DLL 项目 | ✅ | ✅ |
| `_USRDLL` | DLL 项目 | ✅ | ✅ |
| `MIGOTO_DX=11` | DirectX11 | ✅ | ✅ |
| `NDEBUG` | - | ❌ | ✅ |
| `_DEBUG` | - | ✅ | ❌ |

### 4.2 运行时库对照

| 配置 | MSVC 标志 | CMake 变量 |
|------|-----------|-----------|
| Debug | `/MTd` | `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDebug` |
| Release | `/MT` | `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` |

### 4.3 输出目录

| 架构 | 配置 | 当前路径 | CMake 建议 |
|------|------|----------|-----------|
| x64 | Debug | `x64/Debug/` | `build/x64/Debug/` 或保持 `x64/Debug/` |
| x64 | Release | `x64/Release/` | `build/x64/Release/` |
| Win32 | Debug | `x32/Debug/` | `build/Win32/Debug/` 或 `build/x86/Debug/` |
| Win32 | Release | `x32/Release/` | `build/Win32/Release/` |

> ⚠️ 当前路径硬编码在 `.vcxproj` 的 `<OutDir>` 中。CMake 可以通过 `CMAKE_RUNTIME_OUTPUT_DIRECTORY`, `CMAKE_LIBRARY_OUTPUT_DIRECTORY`, `CMAKE_ARCHIVE_OUTPUT_DIRECTORY` 设置。

---

## 5. 预计工作量

| 阶段 | 内容 | 预估工时 | 风险 |
|------|------|:--------:|:----:|
| 一 | 基础设施（根 CMake + cmake/ 工具链） | 4h | 低 |
| 二 | 独立库（BinaryDecompiler, DirectXTK, crc32c, HLSLDecompiler） | 8h | 中（DirectXTK 文件多） |
| 三 | 主项目（DirectX11, Injector 等） | 12h | 中（.def 文件、链接顺序） |
| 四 | 后处理 + 脚本 | 4h | 低 |
| 五 | pcre2 原生 CMake 集成 | 2h | 低 |
| **总计** | | **~30h** | |

---

## 6. 风险与注意事项

### 6.1 已知风险

1. **`Zip Release` 配置**：现有项目有一个特殊的 `Zip Release` 配置（无 Debug Info），CMake 可以通过 **预设（Presets）** 或自定义 Configuration 处理。
2. **`InjectorLib` 跨平台差异**：Win32 下是 EXE，x64 下是 DLL，CMake 需要用 `if(WIN32)` / `if(CMAKE_SIZEOF_VOID_P EQUAL 8)` 做条件判断。
3. **`D3D_Shaders` 的 `_LIB` 宏与 `IgnoreAllDefaultLibraries`**：Release 配置中有 `_LIB` 宏和特殊的链接器设置，需要注意。
4. **模块定义文件（.def）**：DLL 项目依赖 `.def` 文件控制导出符号，CMake 通过 `target_sources(PROJECT PRIVATE file.def)` 或 `set_target_properties(PROPERTIES LINK_FLAGS "/DEF:file.def")` 处理。推荐用 `target_sources` + `SHARED` 库的方式（CMake 3.4+ 支持 `.def` 文件自动处理）。
5. **Nektra 预编译库**：需要区分架构和配置（`NktHookLib.lib` vs `NktHookLib64.lib`, `NktHookLib_Debug.lib` vs `NktHookLib64_Debug.lib`），可以用 Generator Expressions：`$<$<CONFIG:Debug>:...>$<$<CONFIG:Release>:...>`。
6. **pcre2 预编译库**：当前使用 `pcre2/pcre2-8-XX.lib` 预编译文件。若迁移到 pcre2-10.30 的 CMake 构建，需确保与当前预编译库 ABI 兼容。
7. **`ForceSymbolReferences`**：DirectX11 中有 `ForceSymbolReferences` 连接器选项，CMake 中需用 `target_link_options` 配合 `/INCLUDE:` 实现。
8. **资源文件**：`DirectX11.rc` 引用了根目录的 `version.h`，以及 `courierbold.spritefont` 等文件，CMake 需正确设置依赖路径。

### 6.2 建议

- **不要求一蹴而就**：建议先让 `BinaryDecompiler`（最简单的静态库）跑通 CMake，再逐步添加其他子项目。
- **保持双构建系统并行**：新 CMake 构建与旧 MSBuild 并行存在一段时间，用 `build-cmake/` 作为 CMake 输出目录以避免冲突。
- **使用 CMake Presets**：推荐使用 `CMakePresets.json` 定义 Debug/Release + x64/Win32 的配置组合，方便 `cmake --preset` 一键配置。
- **条件编译处理 `Zip Release`**：若仍需保留，可以通过自定义 `CMAKE_CONFIGURATION_TYPES` 实现。
- **ninja 构建器**：推荐使用 `ninja` 作为 CMake 的生成器，比 MSBuild 快很多，且 VS Code + Ninja 配合良好。

---

## 7. 根 CMakeLists.txt 骨架（参考）

```cmake
cmake_minimum_required(VERSION 3.20)
project(3DMigotoDX12
    VERSION 1.3.16
    LANGUAGES C CXX
)

# C++ 标准
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 运行时库 - 对应 /MT (Release) 和 /MTd (Debug)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# 输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY
    ${CMAKE_SOURCE_DIR}/x64/$<CONFIG>)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY
    ${CMAKE_SOURCE_DIR}/x64/$<CONFIG>)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY
    ${CMAKE_SOURCE_DIR}/x64/$<CONFIG>)

# 全局编译选项
include(cmake/CompilerOptions.cmake)

# 子项目
add_subdirectory(BinaryDecompiler)
add_subdirectory(DirectXTK)
add_subdirectory(crc32c-hw-1.0.5)
add_subdirectory(HLSLDecompiler)
add_subdirectory(DirectX11)
add_subdirectory(D3DCompiler_46)
add_subdirectory(DirectXGI)
add_subdirectory(Injector)
add_subdirectory(InjectorLib)
add_subdirectory(D3D_Shaders)

# 可选：pcre2 原生构建
# add_subdirectory(pcre2-10.30)
```

---

## 8. 结论

**可行性**：✅ **完全可以迁移。** 该项目的构建逻辑不复杂，所有 `.vcxproj` 都是标准的 MSBuild 配置，无自定义 Target，无复杂的 MSBuild 逻辑。

**推荐程度**：⭐ **强烈推荐。** CMake 将带来：
- 跨 VS 版本兼容（无需绑定 VS 2022 v143 工具集）
- 更好的 VS Code + Ninja 体验
- 更快的增量构建
- CI/CD 兼容性提高（GitHub Actions, Azure Pipelines 原生支持 CMake）
- 可选的 pcre2 自动构建（无需维护预编译库）

**优先建议**：如果时间有限，可以先从 `BinaryDecompiler` → `DirectX11` 通路开始，这是项目的核心链路。
