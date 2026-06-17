# Build RXDKTools.sln (Release|x64) and stage *.exe for CI artifacts.
param(
    [string]$Configuration = 'Release',
    [string]$Platform = 'x64',
    [string]$Solution = 'RXDKTools.sln',
    [string]$ArtifactDir = 'artifacts/rxdk-tools'
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $repoRoot

function Find-MsBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw 'vswhere.exe not found. Install Visual Studio with the Desktop development with C++ workload.'
    }

    $msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' |
        Select-Object -First 1
    if (-not $msbuild) {
        throw 'MSBuild not found. Install Visual Studio with the Desktop development with C++ workload.'
    }
    return $msbuild
}

function Find-Iscc {
    foreach ($candidate in @(
            (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
            (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')
        )) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    return $null
}

function Ensure-InnoSetup {
    if (Find-Iscc) { return }

    Write-Host 'Inno Setup 6 not found. Installing with Chocolatey...'
    $chocoCmd = Get-Command choco -ErrorAction SilentlyContinue
    if (-not $chocoCmd) {
        throw 'Inno Setup 6 is required for XboxNeighborhood-Setup.exe (xbshlext post-build). Install Inno Setup or Chocolatey on the runner.'
    }
    $choco = $chocoCmd.Source

    & $choco install innosetup -y --no-progress
    if ($LASTEXITCODE -ne 0) {
        throw "choco install innosetup failed (exit $LASTEXITCODE)"
    }

    if (-not (Find-Iscc)) {
        throw 'Inno Setup was installed but ISCC.exe was not found.'
    }
}

$msbuild = Find-MsBuild
Ensure-InnoSetup

Write-Host "MSBuild: $msbuild"
Write-Host "Building $Solution ($Configuration|$Platform)..."

& $msbuild (Join-Path $repoRoot $Solution) `
    /p:Configuration=$Configuration `
    /p:Platform=$Platform `
    /m `
    /v:m
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$binDir = Join-Path $repoRoot "out\bin\$Platform\$Configuration"
if (-not (Test-Path -LiteralPath $binDir)) {
    throw "Output directory not found: $binDir"
}

$stagingDir = Join-Path $repoRoot $ArtifactDir
if (Test-Path -LiteralPath $stagingDir) {
    Remove-Item -LiteralPath $stagingDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $stagingDir | Out-Null

$exes = @(Get-ChildItem -Path $binDir -Filter '*.exe' -File)
if ($exes.Count -eq 0) {
    throw "No .exe files found in $binDir"
}

$exes | Copy-Item -Destination $stagingDir -Force

Write-Host ''
Write-Host "Staged $($exes.Count) executable(s) in $stagingDir"
Get-ChildItem -Path $stagingDir -Filter '*.exe' -File |
    Sort-Object Name |
    ForEach-Object { Write-Host ("  {0,-28} {1,12:N0} bytes" -f $_.Name, $_.Length) }
