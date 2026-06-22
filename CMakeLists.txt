# ============================================================
# 3DMigotoDX12 — CMake 构建系统
# ============================================================
cmake_minimum_required(VERSION 3.20)
project(3DMigotoDX12
    VERSION 1.3.16
    DESCRIPTION "3Dmigoto - DirectX API Hooking and Shader Manipulation Tool"
    LANGUAGES C CXX
)

# ---- C++ / C 标准 ----
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---- 输出目录 ----
# 保持与旧 MSBuild 一致的输出路径
foreach(CONF Debug Release)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONF}
        ${PROJECT_SOURCE_DIR}/x64/${CONF})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONF}
        ${PROJECT_SOURCE_DIR}/x64/${CONF})
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONF}
        ${PROJECT_SOURCE_DIR}/x64/${CONF})
endforeach()

# ---- 包含 cmake 模块路径 ----
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ---- 加载编译选项模块 ----
include(CompilerOptions)

# ---- 查找 Nektra 预编译库 ----
find_package(Nektra)

# ---- 第三方依赖 ----
add_subdirectory(3rdparty/crc32c-hw-1.0.5)
add_subdirectory(3rdparty/DirectXTK)

# ---- 子项目 ----
add_subdirectory(src/Core)
add_subdirectory(src/BinaryDecompiler)
add_subdirectory(src/D3D_Shaders)
add_subdirectory(src/D3DCompiler_46)
add_subdirectory(src/DirectXGI)
add_subdirectory(src/DirectX11)
add_subdirectory(src/DirectX12)
add_subdirectory(src/Injector)
add_subdirectory(src/InjectorLib)
