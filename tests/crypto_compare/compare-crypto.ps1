# Compare symmetric ciphers in rsa32.lib (reference) vs xrsa.lib (source).
param(
    [string]$RepoRoot = (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent)
)
$ErrorActionPreference = 'Stop'

$refExe = Join-Path $RepoRoot 'out/bin/Win32/Release/crypto_compare_ref.exe'
$xrsaExe = Join-Path $RepoRoot 'out/bin/Win32/Release/crypto_compare_xrsa.exe'
$refOut = Join-Path $RepoRoot 'out/crypto_compare_ref.txt'
$xrsaOut = Join-Path $RepoRoot 'out/crypto_compare_xrsa.txt'

function Invoke-CompareExe {
    param(
        [string]$ExePath,
        [string]$OutPath,
        [int]$TimeoutSec = 60
    )
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $ExePath
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $proc = [System.Diagnostics.Process]::Start($psi)
    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        $proc.Kill()
        throw "Timed out after ${TimeoutSec}s: $ExePath"
    }
    if ($proc.ExitCode -ne 0) {
        $err = $proc.StandardError.ReadToEnd()
        throw "Exit $($proc.ExitCode): $ExePath`n$err"
    }
    $proc.StandardOutput.ReadToEnd() | Set-Content -Encoding ascii $OutPath
}

$msb = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msb) { throw 'MSBuild not found' }

Write-Host 'Building crypto_compare_ref and crypto_compare_xrsa...' -ForegroundColor Cyan
& $msb (Join-Path $RepoRoot 'tests/crypto_compare/crypto_compare_ref.vcxproj') /p:Configuration=Release /p:Platform=Win32 /t:Rebuild /v:minimal
if ($LASTEXITCODE -ne 0) { throw 'crypto_compare_ref build failed' }
& $msb (Join-Path $RepoRoot 'tests/crypto_compare/crypto_compare_xrsa.vcxproj') /p:Configuration=Release /p:Platform=Win32 /t:Rebuild /v:minimal
if ($LASTEXITCODE -ne 0) { throw 'crypto_compare_xrsa build failed' }

Invoke-CompareExe -ExePath $refExe -OutPath $refOut -TimeoutSec 60
Invoke-CompareExe -ExePath $xrsaExe -OutPath $xrsaOut -TimeoutSec 60

$refLines = Get-Content $refOut
$xrsaLines = Get-Content $xrsaOut
$compareLines = @(
    'rc4',
    'rc4_odd',
    'des_enc',
    'des_dec',
    'des3_enc',
    'des3_dec',
    'cbc_enc',
    'cbc_dec',
    'desparity'
)
$failed = $false

foreach ($name in $compareLines) {
    $refLine = $refLines | Where-Object { $_ -like "${name}:*" } | Select-Object -First 1
    $xrsaLine = $xrsaLines | Where-Object { $_ -like "${name}:*" } | Select-Object -First 1
    if (-not $refLine -or -not $xrsaLine) {
        Write-Host "MISSING $name" -ForegroundColor Red
        $failed = $true
        continue
    }
    if ($refLine -eq $xrsaLine) {
        Write-Host "PASS $name" -ForegroundColor Green
    } else {
        Write-Host "FAIL $name" -ForegroundColor Red
        Write-Host "  ref:  $refLine"
        Write-Host "  xrsa: $xrsaLine"
        $failed = $true
    }
}

if ($failed) {
    throw 'crypto_compare mismatch'
}

Write-Host 'All xrsa symmetric-cipher vectors match rsa32.lib' -ForegroundColor Green
