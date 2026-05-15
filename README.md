
# 3DMigoto DX12 - 开发指南

## 📋 环境要求

| 依赖 | 说明 |
|------|------|
| **Visual Studio 2022+** | 需要安装 **C++ 桌面开发** 工作负载（MSVC、Windows SDK） |
| **CMake 3.20+** | 构建系统生成器（推荐 v4.3+） |
| **Ninja** | 高速构建工具（可选，推荐） |
| **VS Code** | 推荐安装 **C++ Extension Pack** (`ms-vscode.cpptools-extension-pack`) |

---

## 🔧 编译方法

### 方式一：PowerShell 脚本

```powershell
# Debug x64（默认）
.\scripts\build-cmake.ps1 -Configuration Debug -Platform x64

# Release x64
.\scripts\build-cmake.ps1 -Configuration Release -Platform x64

# Debug Win32
.\scripts\build-cmake.ps1 -Configuration Debug -Platform Win32
```

编译输出目录：`x64\Debug\`、`x64\Release\`、`x32\Debug\`、`x32\Release\`

### 方式二：直接使用 CMake + Ninja

```powershell
# 在 VS 开发者命令提示符中执行：
cmake -S . -B build/debug-x64 -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug-x64
```

### 方式三：CMake Presets

```powershell
cmake --preset default          # Debug x64
cmake --build --preset default
cmake --preset release          # Release x64
cmake --build --preset release
```

---

## 🐛 调试

1. 编译 Debug 版本
2. 将生成的 `x64\Debug\d3d11.dll` 复制到游戏目录
3. 启动游戏
4. 在 VS Code 中按 `F5`，选择 **Attach to Game (x64)**
5. 在弹出的进程列表中选择游戏进程

---

## 📁 项目结构

```
3DMigotoDX12/
├── CMakeLists.txt              # 根 CMake 构建文件
├── CMakePresets.json           # CMake 预设配置
├── cmake/                      # CMake 模块
│   ├── CompilerOptions.cmake   # 统一编译器选项
│   └── FindNektra.cmake        # Nektra 预编译库查找
├── src/                        # 源代码
│   ├── common/                 # 公共共享文件 (log.h, util.*, version.h...)
│   ├── DirectX11/              # d3d11.dll — 主模块 (API Hook)
│   ├── DirectXGI/              # dxgi.dll — DXGI 包装器
│   ├── D3DCompiler_46/         # d3dcompiler_47.dll — D3DCompiler 包装器
│   ├── D3DCompiler/            # D3DCompiler 共享代码
│   ├── BinaryDecompiler/       # 静态库 — 二进制着色器反编译
│   ├── D3D_Shaders/            # 独立工具 — 着色器汇编/反汇编
│   ├── HLSLDecompiler/         # HLSL 反编译器
│   ├── Injector/               # 3DMigoto Loader.exe
│   └── InjectorLib/            # 3dmloader.dll (x64) / .exe (Win32)
├── 3rdparty/                   # 第三方依赖
│   ├── crc32c-hw-1.0.5/       # CRC32C 库 (自带 CMake)
│   ├── DirectXTK/              # DirectX Tool Kit (自带 CMake)
│   ├── pcre2-10.30/            # PCRE2 正则库 (自带 CMake, 可选)
│   ├── pcre2/                  # PCRE2 预编译库 (当前使用)
│   └── Nektra/                 # Nektra Hook Library (预编译 .lib)
├── resources/                  # 资源文件
│   ├── fonts/                  # 字体 (.spritefont)
│   └── shaders/                # 测试着色器
├── config/                     # 配置文件
│   └── d3dx.ini
├── Dependencies/               # 运行时依赖
│   └── d3dcompiler_47.dll
├── scripts/                    # 构建脚本
│   ├── build-all.ps1           # 旧 MSBuild 脚本（兼容）
│   └── build-cmake.ps1         # 新 CMake 构建脚本（推荐）
└── x64/ x32/                   # 编译输出目录

---

## ⚡ 小提示

- CMake 构建产出在 `build/cmake-*/` 目录中，最终产物自动拷贝到 `x64\Debug\` 等目录
- 如需在 VS Code 中使用 CMake 集成，安装 `ms-vscode.cmake-tools` 扩展并选择 CMake Preset
- 旧版 MSBuild `.vcxproj` 文件仍然保留在原始位置（根目录子文件夹），可作为备份
- 首次使用 CMake 构建前，请确保已安装 CMake 和 Ninja：`winget install Kitware.CMake Ninja-build.Ninja`