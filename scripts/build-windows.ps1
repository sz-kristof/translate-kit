# Copyright 2026 Kristóf Szerencsi
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Builds translatekit.dll (x64, MSVC, static CRT) and runs the C-ABI smoke test
# against it. Requires Visual Studio 2022+ with the C++ workload (its bundled
# CMake and Ninja are used when none are on PATH) and Git for Windows' bash for
# the patch script.
#
#   git submodule update --init --checkout --recursive third_party/cld2
#   git submodule update --init --checkout third_party/translations
#   git -C third_party/translations submodule update --init --recursive `
#     inference/3rd_party/ssplit-cpp inference/marian-fork/src/3rd_party/sentencepiece `
#     inference/marian-fork/src/3rd_party/intgemm inference/marian-fork/src/3rd_party/ruy `
#     inference/marian-fork/src/3rd_party/simd_utils
#   bash scripts/apply-engine-patches.sh
#   bash scripts/fetch-models.sh            # optional: enables the live translation check
#   pwsh scripts/build-windows.ps1
#
# Output: build/windows/translatekit.dll + translatekit.lib (import library),
# with the public header at core/include/translate_kit/translate_kit.h.

param(
    [string]$BuildDir = "build/windows",
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

# Enter the MSVC x64 developer environment unless we're already in one.
if (-not $env:VCINSTALLDIR) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw "Visual Studio with the C++ workload was not found" }
    $vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
    cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
        if ($_ -match "^([^=]+)=(.*)$") { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }
}

cmake -S windows -B $BuildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Config"
if ($LASTEXITCODE) { throw "configure failed" }
cmake --build $BuildDir --target translatekit tk_smoke
if ($LASTEXITCODE) { throw "build failed" }

$model = Join-Path $repo "models/test-models/ende.student.tiny11"
if (Test-Path $model) { $env:TK_TEST_MODEL_DIR = $model }
& (Join-Path $BuildDir "tk_smoke.exe")
if ($LASTEXITCODE) { throw "smoke test failed" }

Write-Host "OK: $(Join-Path $BuildDir 'translatekit.dll')"
