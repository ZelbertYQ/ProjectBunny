param(
    [Parameter(Mandatory)]
    [ValidateSet('Debug','Release','Zip Release')]
    [string]$Configuration,

    [Parameter(Mandatory)]
    [ValidateSet('x64','Win32')]
    [string]$Platform
)

$MSBuild = if ($Platform -eq 'x64') {
    'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
} else {
    'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
}

$Projects = @(
    'DirectX11\DirectX11.vcxproj'
    'D3DCompiler_46\D3DCompiler_46.vcxproj'
    'Injector\Injector.vcxproj'
    'InjectorLib\InjectorLib.vcxproj'
)

$root = Split-Path -Parent $PSScriptRoot

# SolutionDir 用于 vcxproj 中引用 $(SolutionDir) 的路径
# 必须以反斜杠结尾，MSBuild 才会正确解析
$solutionDir = $root + '\'

foreach ($proj in $Projects) {
    Write-Host "`n===== Building $proj ($Configuration|$Platform) =====" -ForegroundColor Cyan
    $projPath = Join-Path $root $proj
    & $MSBuild $projPath "/p:Configuration=$Configuration" "/p:Platform=$Platform" "/p:SolutionDir=$solutionDir" "/m"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAILED: $proj" -ForegroundColor Red
        exit $LASTEXITCODE
    }
}

Write-Host "`n===== All projects built successfully =====" -ForegroundColor Green
