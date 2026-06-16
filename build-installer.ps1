# Build XboxNeighborhood-Setup.exe with Inno Setup 6.
param(
    [string]$IssPath = (Join-Path $PSScriptRoot 'setup.iss')
)
$ErrorActionPreference = 'Stop'

function Find-Iscc {
    foreach ($candidate in @(
            (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
            (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')
        )) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    return $null
}

if (-not (Test-Path -LiteralPath $IssPath)) {
    throw "Missing $IssPath"
}
$xbshlextDll = Join-Path $PSScriptRoot 'out\bin\x64\Release\xbshlext.dll'
if (-not (Test-Path -LiteralPath $xbshlextDll)) {
    throw "Missing $xbshlextDll - build xbshlext (Release|x64) before creating the installer"
}
foreach ($required in @('Icon.ico', 'WizardImage.bmp', 'WizardSmallImage.bmp')) {
    $path = Join-Path $PSScriptRoot $required
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Missing $path"
    }
}

$iscc = Find-Iscc
if (-not $iscc) {
    throw 'Inno Setup 6 not found. Install from https://jrsoftware.org/isinfo.php'
}

Write-Host "Building installer with $iscc" -ForegroundColor Cyan
& $iscc $IssPath
if ($LASTEXITCODE -ne 0) {
    throw "ISCC failed (exit $LASTEXITCODE)"
}

$output = Join-Path $PSScriptRoot 'output\XboxNeighborhood-Setup.exe'
Write-Host "Installer: $output" -ForegroundColor Green
