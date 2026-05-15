# DirectXGI — DXGI 加载器/转发器

## 概述

`DirectXGI` 是 3DMigoto 的一个辅助模块，编译产物为 `dxgi.dll`。它的核心职责是**解决 DXGI 的加载顺序问题**，确保 3DMigoto 的 `d3d11.dll` 能在游戏初始化 DXGI 之前完成 Hook。

## 背景：为什么需要这个模块？

3DMigoto 的主要模块是 `d3d11.dll`，它通过拦截游戏对 Direct3D 11 API 的调用来实现着色器替换、覆盖层等功能。其中一部分 Hook 需要拦截 DXGI（DirectX Graphics Infrastructure）的调用。

正常情况下，`d3d11.dll` 在 DLL 加载时执行 Hook，此时 DXGI 尚未被游戏初始化，一切正常。

**但有些游戏（如《消逝的光芒》Dying Light）有一个奇怪的加载行为：** 它们会在 3DMigoto 的 `d3d11.dll` 有机会 Hook DXGI 之前，**抢先加载系统原版的 `dxgi.dll`**。这导致：
- 覆盖层（Overlay）无法工作
- 高级特性不可用
- 部分 Hook 失败

## 工作原理

```
游戏启动
   │
   ├──→ 加载 dxgi.dll（我们的）          ← 关键！冒充系统 DLL
   │         │
   │         ├──→ 链接器强制引用 D3D11CreateDevice
   │         │         │
   │         │         └──→ Windows 自动加载 d3d11.dll
   │         │                     │
   │         │                     └──→ 3DMigoto 执行 Hook
   │         │
   │         └──→ 导出 DXGI 函数给游戏调用
   │
   └──→ 游戏正常使用 DXGI
```

### 步骤详解

1. **冒充系统 `dxgi.dll`**：编译产物放在游戏目录下，Windows 会优先加载它而不是系统 `%SYSTEM32%\dxgi.dll`。
2. **强制加载 `d3d11.dll`**：通过链接器选项 `/INCLUDE:D3D11CreateDevice` 和 `d3d11.lib`，使这个 DLL 在导入表中引用了 `D3D11CreateDevice` 符号。Windows 加载该 DLL 时，会自动解析该符号，从而**提前加载我们的 `d3d11.dll`**。
3. **导出标准 DXGI 函数**：通过 `.def` 文件导出所有 DXGI 需要的函数（`CreateDXGIFactory`、`CreateDXGIFactory1`、`CreateDXGIFactory2`、`DXGID3D10CreateDevice` 等），让游戏正常运行。
4. **初始化与日志**：读取 `d3dx.ini` 配置，支持日志记录、调试器等待、CPU 亲和性设置等。

## 导出函数

通过 `d3dxgiWrapper.def` 导出的函数：

| 函数 | 说明 |
|------|------|
| `CreateDXGIFactory` | 创建 DXGI 工厂（Win7+） |
| `CreateDXGIFactory1` | 创建 DXGI 1.1 工厂（Win7+） |
| `CreateDXGIFactory2` | 创建 DXGI 1.2 工厂（Win10+） |
| `OpenAdapter10` / `OpenAdapter10_2` | 打开适配器 |
| `DXGID3D10CreateDevice` | D3D10 设备创建 |
| `D3DKMT*` 系列 | D3D Kernel Mode 函数（约 25 个） |
| `DXGIReportAdapterConfiguration` | 报告适配器配置 |

大部分导出函数是**空桩**（返回 `S_OK` 或 0），因为真正的 DXGI 功能由系统 DLL 完成。这个 DLL 的核心价值不在于实现这些函数，而在于**利用 Windows 的 DLL 加载机制提前拉起 `d3d11.dll`**。

## 配置（d3dx.ini）

读取 `d3dx.ini` 中的以下配置段：

### [Logging]

| 配置项 | 默认值 | 说明 |
|--------|:------:|------|
| `calls` | `1` | 启用日志，输出到 `dxgi_log.txt` |
| `debug` | `0` | 启用调试日志 |
| `unbuffered` | `0` | 无缓冲日志写入 |
| `waitfordebugger` | `0` | 启动时等待调试器附加（`=2` 时自动触发断点） |
| `force_cpu_affinity` | `0` | 强制 CPU 亲和性为核 0（调试用） |

## 历史演变

| 时间 | 变更 |
|------|------|
| 2015-04 | 最初作为 DXGI Hook 模块开发，后因 Win 8.1 兼容问题被禁用 |
| 2015-07 | 改为纯日志工具，用于分析游戏的 DXGI 使用模式 |
| 2015-11 | **重新启用** — 因《消逝的光芒》的加载顺序问题需要此 DLL 作为加载器 |
| 2018-03 | 添加 Win10 支持，在 `.def` 中增加 `CreateDXGIFactory2` 导出 |

## 与其他模块的关系

```
DirectXGI (dxgi.dll)
   │
   ├──→ 强制加载 ──→ d3d11.dll (DirectX11)
   │
   ├──→ 读取配置 ──→ d3dx.ini
   │
   └──→ 日志输出 ──→ dxgi_log.txt
```

- **DirectX11**：被 DirectXGI 通过导入表引用强制加载
- **d3dx.ini**：共享配置文件（位于 `config/d3dx.ini`）
- **Nektra**：不直接使用，但通过加载 `d3d11.dll` 间接支持 Nektra Hook

## 编译

作为 CMake 项目的一部分，自动构建：

```powershell
# 编译全部项目时自动包含
.\build_debug_x64.ps1
.\build_release_x64.ps1
```

产物位于 `build/cmake-*/src/DirectXGI/dxgi.dll`，构建脚本自动复制到 `x64/Debug/` 或 `x64/Release/`。

## 使用方式

1. 将 `dxgi.dll` 和 `d3d11.dll` 一起放入游戏目录
2. （可选）放置 `d3dx.ini` 配置文件
3. 启动游戏
4. 日志输出到游戏目录下的 `dxgi_log.txt`

## 是否必需？

**不是核心模块**。大多数游戏不需要它也能正常工作——只有那些在 `d3d11.dll` 之前就初始化 DXGI 的游戏才需要。具体表现为覆盖层（Overlay）无法显示，或者 Console 输出 "DXGI already initialized" 类错误。

如果不需要，可以从构建中排除：在根 `CMakeLists.txt` 中注释或删除 `add_subdirectory(src/DirectXGI)` 行。
