# Сборка release zip для Windows (фаза 7).
# Требует: cmake build с Qt, windeployqt уже выполнен post-build.

param(
    [string]$BuildDir = "build",
    [string]$OutDir = "dist/nyx-win64"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location (Join-Path $root "..")

if (-not (Test-Path "$BuildDir/nyx-app.exe")) {
    Write-Error "Сначала соберите проект: cmake --build $BuildDir"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$exes = @(
    "nyx-app.exe",
    "nyx-node.exe",
    "nyx-rendezvous.exe",
    "nyx-tests.exe",
    "nyx-appcore-tests.exe"
)

foreach ($e in $exes) {
    $src = Join-Path $BuildDir $e
    if (Test-Path $src) { Copy-Item $src $OutDir -Force }
}

# Qt runtime (windeployqt кладёт рядом с nyx-app)
Get-ChildItem $BuildDir -Filter "*.dll" | Copy-Item -Destination $OutDir -Force
if (Test-Path "$BuildDir/platforms") {
    Copy-Item "$BuildDir/platforms" "$OutDir/platforms" -Recurse -Force
}
if (Test-Path "$BuildDir/qml") {
    Copy-Item "$BuildDir/qml" "$OutDir/qml" -Recurse -Force
}

Copy-Item README.md $OutDir -Force
Copy-Item docs/APPLICATION.md "$OutDir/docs-APPLICATION.md" -Force

$zip = "dist/nyx-win64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$OutDir/*" -DestinationPath $zip -Force

Write-Host "Готово: $zip"
