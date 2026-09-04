<#
.SYNOPSIS
    Ensures a local, repo-scoped copy of vcpkg exists and that the Azure IoT SDK
    (azure-iot-sdk-c, declared in vcpkg.json) is installed for the requested triplet.

    This script is invoked automatically as an MSBuild pre-build step
    (see Directory.Build.targets), so a fresh clone of this repo can be built on a
    brand-new machine with no manual vcpkg setup: no VcpkgRoot environment variable,
    no "vcpkg integrate install", nothing pre-installed except Git and Visual Studio's
    C++ build tools.

.PARAMETER SolutionDir
    Path to the solution root (where vcpkg.json and the "vcpkg" folder live).

.PARAMETER Triplet
    The vcpkg triplet to install for, e.g. x64-windows or x86-windows.
#>
param(
    [Parameter(Mandatory = $true)][string]$SolutionDir,
    [Parameter(Mandatory = $true)][string]$Triplet
)

$ErrorActionPreference = "Stop"

function Fail($message) {
    Write-Error $message
    exit 1
}

$SolutionDir = (Resolve-Path $SolutionDir).Path
if (-not $SolutionDir.EndsWith('\\')) { $SolutionDir += '\\' }

$vcpkgDir = Join-Path $SolutionDir "vcpkg"
$vcpkgExe = Join-Path $vcpkgDir "vcpkg.exe"

$solutionDirHash = [System.BitConverter]::ToString(
    [System.Security.Cryptography.SHA256]::Create().ComputeHash(
        [System.Text.Encoding]::UTF8.GetBytes($SolutionDir.ToLowerInvariant())
    )
).Replace('-', '').Substring(0, 16)
$mutexName = "Global\AzureIotVcpkgBootstrap_$solutionDirHash"
$bootstrapMutex = New-Object System.Threading.Mutex($false, $mutexName)
$hasMutex = $false

try {
    Write-Host "==> Waiting for vcpkg bootstrap lock ..."
    $hasMutex = $bootstrapMutex.WaitOne([TimeSpan]::FromMinutes(30))
    if (-not $hasMutex) {
        Fail "Timed out waiting for vcpkg bootstrap lock. Another build may still be preparing dependencies."
    }

    # 1. Make sure git is available; we need it to fetch vcpkg itself.
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        Fail "Git is required to download vcpkg/the Azure IoT SDK but was not found on PATH. Install Git for Windows (https://git-scm.com/download/win) and build again."
    }

    # 2. Clone vcpkg into the repo if it isn't there yet.
    if (-not (Test-Path (Join-Path $vcpkgDir ".git"))) {
        Write-Host "==> Cloning vcpkg into '$vcpkgDir' ..."
        if (Test-Path $vcpkgDir) { Remove-Item $vcpkgDir -Recurse -Force }
        & git clone --depth 1 https://github.com/microsoft/vcpkg.git $vcpkgDir
        if ($LASTEXITCODE -ne 0) { Fail "git clone of vcpkg failed." }
    }

    # 3. Bootstrap the vcpkg executable if needed.
    if (-not (Test-Path $vcpkgExe)) {
        Write-Host "==> Bootstrapping vcpkg ..."
        & (Join-Path $vcpkgDir "bootstrap-vcpkg.bat") -disableMetrics
        if ($LASTEXITCODE -ne 0) { Fail "vcpkg bootstrap failed." }
    }

    # 4. Install the manifest's dependencies (azure-iot-sdk-c) for the requested triplet.
    #    Installing into vcpkg's own "installed" folder keeps the classic-mode layout
    #    ($(VcpkgRoot)\installed\<triplet>\...) that the .vcxproj files expect.
    $installRoot = Join-Path $vcpkgDir "installed"
    $marker = Join-Path $installRoot "$Triplet\include\azureiot\iothub.h"

    if (-not (Test-Path $marker)) {
        Write-Host "==> Installing Azure IoT SDK (azure-iot-sdk-c) for triplet '$Triplet' -- this can take a while the first time ..."
        & $vcpkgExe install `
            "--triplet=$Triplet" `
            "--x-manifest-root=$($SolutionDir.TrimEnd('\\'))" `
            "--x-install-root=$($installRoot.TrimEnd('\\'))" `
            --disable-metrics
        if ($LASTEXITCODE -ne 0) { Fail "vcpkg install failed." }
    }

    if (-not (Test-Path $marker)) {
        Fail "vcpkg install completed but iothub.h was not found at '$marker'. Check the vcpkg output above for errors."
    }

    Write-Host "==> Azure IoT SDK is ready ($Triplet)."
}
finally {
    if ($hasMutex) {
        $bootstrapMutex.ReleaseMutex() | Out-Null
    }
    $bootstrapMutex.Dispose()
}
