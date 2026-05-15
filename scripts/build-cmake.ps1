# ============================================================
# build-cmake.ps1 — CMake 构建脚本
# 用法: .\scripts\build-cmake.ps1 [-Configuration Debug|Release] [-Platform x64|Win32]
# ============================================================
param(
    [ValidateSet('Debug','Release')]
    [string]$Configuration = 'Debug',

    [ValidateSet('x64','Win32')]
    [string]$Platform = 'x64'
)

$root = Split-Path -Parent $PSScriptRoot
$cmake = "C:\tools\cmake\cmake-4.3.2-windows-x86_64\bin\cmake.exe"
$buildDir = "$root\build\cmake-$Platform-$($Configuration.ToLower())"

# VS 版本检测
$vsPath = "C:\Program Files\Microsoft Visual Studio\18\Community"
$vcvars = if ($Platform -eq 'x64') {
    "$vsPath\VC\Auxiliary\Build\vcvars64.bat"
} else {
    "$vsPath\VC\Auxiliary\Build\vcvars32.bat"
}

# 检查 CMake
if (-not (Test-Path $cmake)) {
    Write-Host "ERROR: CMake not found at $cmake" -ForegroundColor Red
    exit 1
}

# 检查 VS
if (-not (Test-Path $vcvars)) {
    Write-Host "ERROR: Visual Studio not found at $vsPath" -ForegroundColor Red
    exit 1
}

# CMake 配置
Write-Host "===== Configuring CMake ($Configuration|$Platform) =====" -ForegroundColor Cyan
$genArgs = @(
    "-S", $root
    "-B", $buildDir
    "-G", "Ninja"
    "-DCMAKE_BUILD_TYPE=$Configuration"
)

# x64 架构设置
if ($Platform -eq 'x64') {
    $genArgs += @()
}

$configureCmd = "call `"$vcvars`" > nul 2>&1 && & `"$cmake`" $genArgs"
cmd /c $configureCmd 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAILED: Configure" -ForegroundColor Red
    exit $LASTEXITCODE
}

# CMake 构建
Write-Host "`n===== Building ($Configuration|$Platform) =====" -ForegroundColor Cyan
$buildCmd = "call `"$vcvars`" > nul 2>&1 && & `"$cmake`" --build `"$buildDir`""
cmd /c $buildCmd 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAILED: Build" -ForegroundColor Red
    exit $LASTEXITCODE
}

# 复制产物到统一输出目录
$outDir = if ($Platform -eq 'x64') { "$root\x64\$Configuration" } else { "$root\x32\$Configuration" }
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir -Force | Out-Null }

$files = @(
    "src/DirectX11/d3d11.dll",
    "src/D3DCompiler_46/d3dcompiler_47.dll",
    "src/DirectXGI/dxgi.dll",
    "src/Injector/3DMigoto Loader.exe",
    "src/InjectorLib/3dmloader.dll",
    "src/D3D_Shaders/D3D_Shaders.exe"
)
foreach ($f in $files) {
    $src = "$buildDir/$f"
    $dst = "$outDir/$(Split-Path -Leaf $f)"
    if (Test-Path $src) {
        Copy-Item $src $dst -Force
        Write-Host "  Copied: $dst" -ForegroundColor Gray
    }
}

Write-Host "`n===== All projects built successfully =====" -ForegroundColor Green
Write-Host "Output directory: $outDir"
