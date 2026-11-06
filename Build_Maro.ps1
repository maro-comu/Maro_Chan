[CmdletBinding()]
param(
    [string]$CocosRoot = $env:COCOS2DX_ROOT_PATH,
    [switch]$Run
)

$ErrorActionPreference = 'Stop'

function Invoke-MaroTool {
    param([string]$Tool, [string[]]$Arguments)

    & $Tool @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Tool failed with exit code $LASTEXITCODE."
    }
}

function Get-MaroArchive {
    param([string]$Url, [string]$Archive)

    if (!(Test-Path -LiteralPath $Archive -PathType Leaf)) {
        Write-Host "Downloading $([IO.Path]::GetFileName($Archive))"
        $partial = "$Archive.partial"
        Invoke-MaroTool -Tool 'curl.exe' -Arguments @('-fLsS', '--retry', '2', '-o', $partial, $Url)
        Move-Item -LiteralPath $partial -Destination $Archive -Force
    }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw 'Install Visual Studio with Desktop development with C++ and CMake tools for Windows.'
}

$vsJson = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json
$vs = $vsJson | ConvertFrom-Json | Select-Object -First 1
if (!$vs) {
    throw 'Install the Visual Studio Desktop development with C++ workload.'
}

$generator = switch (([version]$vs.installationVersion).Major) {
    18 { 'Visual Studio 18 2026' }
    17 { 'Visual Studio 17 2022' }
    16 { 'Visual Studio 16 2019' }
    default { throw 'Use Visual Studio 2019, 2022 or 2026 with MSVC x86 tools.' }
}

$cmake = Join-Path $vs.installationPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (!(Test-Path -LiteralPath $cmake -PathType Leaf)) {
    $cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if (!$cmakeCommand) {
        throw 'Install Visual Studio CMake tools for Windows or add CMake to PATH.'
    }
    $cmake = $cmakeCommand.Source
}

$local = Join-Path $env:LOCALAPPDATA 'MaroArena'
$stage = Join-Path $local 'project'
$build = Join-Path $local 'build'
New-Item -ItemType Directory -Path $stage -Force | Out-Null

if (!$CocosRoot) {
    $sdk = Join-Path $local 'sdk'
    $CocosRoot = Join-Path $sdk 'cocos2d-x-cocos2d-x-4.0'
    New-Item -ItemType Directory -Path $sdk -Force | Out-Null

    $engineReady = Join-Path $sdk 'cocos2d-x-4.0.ready'
    if (!(Test-Path -LiteralPath $engineReady -PathType Leaf)) {
        $engineArchive = Join-Path $sdk 'cocos2d-x-4.0.zip'
        Get-MaroArchive -Archive $engineArchive -Url 'https://codeload.github.com/cocos2d/cocos2d-x/zip/refs/tags/cocos2d-x-4.0'
        Invoke-MaroTool -Tool 'tar.exe' -Arguments @('-xf', $engineArchive, '-C', $sdk)
        New-Item -ItemType File -Path $engineReady -Force | Out-Null
    }

    $librariesReady = Join-Path $sdk 'metal-support-22.ready'
    if (!(Test-Path -LiteralPath $librariesReady -PathType Leaf)) {
        $librariesArchive = Join-Path $sdk 'metal-support-22.zip'
        Get-MaroArchive -Archive $librariesArchive -Url 'https://codeload.github.com/cocos2d/cocos2d-x-3rd-party-libs-bin/zip/refs/tags/metal-support-22'
        $external = Join-Path $CocosRoot 'external'
        Invoke-MaroTool -Tool 'tar.exe' -Arguments @('-xf', $librariesArchive, '--exclude', '*/linux-specific/*', '--strip-components', '1', '-C', $external)
        New-Item -ItemType File -Path $librariesReady -Force | Out-Null
    }
}

$engineSource = Join-Path $CocosRoot 'cocos\cocos2d.cpp'
$runtimeLibrary = Join-Path $CocosRoot 'external\win32-specific\gles\prebuilt\glew32.dll'
if (!(Test-Path -LiteralPath $engineSource -PathType Leaf) -or
    !(Select-String -LiteralPath $engineSource -SimpleMatch 'cocos2d-x-4.0' -Quiet) -or
    !(Test-Path -LiteralPath $runtimeLibrary -PathType Leaf)) {
    throw 'CocosRoot must contain the Cocos2d-x 4.0 SDK and its external Windows libraries.'
}
$CocosRoot = (Resolve-Path -LiteralPath $CocosRoot).Path

Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'CMakeLists_Maro.cmake') -Destination (Join-Path $stage 'CMakeLists.txt') -Force
Invoke-MaroTool -Tool $cmake -Arguments @(
    '-S', $stage, '-B', $build, '-G', $generator, '-A', 'Win32', '-Wno-deprecated',
    "-DCMAKE_GENERATOR_INSTANCE=$($vs.installationPath)",
    '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', "-DCOCOS2DX_ROOT_PATH=$CocosRoot", "-DMARO_SOURCE_DIR=$PSScriptRoot"
)
Invoke-MaroTool -Tool $cmake -Arguments @('--build', $build, '--config', 'Release', '--target', 'MaroArena', '--parallel', '3')

$release = Join-Path $PSScriptRoot 'Release'
Invoke-MaroTool -Tool $cmake -Arguments @('--install', $build, '--config', 'Release', '--prefix', $release, '--component', 'MaroArenaRuntime')

$exe = Join-Path $release 'MaroArena.exe'
Write-Host "Game ready: $exe"
if ($Run) {
    Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe)
}
