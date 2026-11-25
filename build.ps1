# Chroma-41 (41-EDO Spectral Pitch-Quantizer & Color Bass Resonator VST3)
# Build Script for Windows x64

$ErrorActionPreference = "Stop"
$GPP = ".\.toolchain\w64devkit\bin\g++.exe"

if (-not (Test-Path $GPP)) {
    Write-Host "Downloading portable w64devkit C++ toolchain..."
    New-Item -ItemType Directory -Force -Path ".toolchain" | Out-Null
    $url = "https://github.com/skeeto/w64devkit/releases/download/v2.11.0/w64devkit-x64-2.11.0.7z.exe"
    Invoke-WebRequest -Uri $url -OutFile ".toolchain\w64devkit.7z.exe"
    & "C:\Program Files\7-Zip\7z.exe" x ".toolchain\w64devkit.7z.exe" -o".toolchain" -y | Out-Null
}

New-Item -ItemType Directory -Force -Path "build\Chroma41.vst3\Contents\x86_64-win" | Out-Null

Write-Host "[1/3] Compiling Chroma41.vst3 plugin bundle..."
& $GPP -std=c++17 -O3 -shared -static-libgcc -static-libstdc++ -static `
    -I"external" `
    "src\vst3\Chroma41Plugin.cpp" `
    "external\pluginterfaces\base\funknown.cpp" `
    "external\pluginterfaces\base\coreiids.cpp" `
    "external\pluginterfaces\base\ustring.cpp" `
    "external\pluginterfaces\base\conststringtable.cpp" `
    -lgdi32 -luser32 -lole32 -luuid `
    -o "build\Chroma41.vst3\Contents\x86_64-win\Chroma41.vst3"

Write-Host "[2/3] Compiling Chroma41Standalone.exe (Interactive Preview + VST3 Test Host)..."
& $GPP -std=c++17 -O3 -static-libgcc -static-libstdc++ -static `
    -I"external" `
    "src\standalone\Chroma41Standalone.cpp" `
    "external\pluginterfaces\base\funknown.cpp" `
    "external\pluginterfaces\base\coreiids.cpp" `
    "external\pluginterfaces\base\ustring.cpp" `
    "external\pluginterfaces\base\conststringtable.cpp" `
    -lgdi32 -luser32 -lole32 -luuid -lwinmm `
    -o "build\Chroma41Standalone.exe"

Write-Host "[3/3] Running VST3 validation and rendering 41-EDO Color Bass demo WAVs..."
& ".\build\Chroma41Standalone.exe" --test-vst3

Write-Host "`nBuild succeeded! VST3 bundle ready at: build\Chroma41.vst3"
