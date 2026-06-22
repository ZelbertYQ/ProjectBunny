# build_release_x64.ps1 — 快捷构建 Release x64
param(
    [string]$GameId = ""
)

$root = Split-Path -Parent $PSCommandPath
& "$root\scripts\build-cmake.ps1" -Configuration Release -Platform x64 -GameId $GameId
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
