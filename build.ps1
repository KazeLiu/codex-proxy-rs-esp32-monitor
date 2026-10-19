param([string]$Port = 'COM3', [switch]$Flash, [switch]$Configure, [string]$IdfProfile)
$ErrorActionPreference = 'Stop'
# Preserve the original workstation entry point; other machines use an exported IDF shell.
if (-not $env:IDF_PATH -and -not $IdfProfile -and (Test-Path -LiteralPath 'D:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1')) {
    $IdfProfile = 'D:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
}
if ($IdfProfile) { . $IdfProfile }
if (-not $env:IDF_PATH) { throw 'Open an ESP-IDF 6.1 PowerShell terminal, or pass -IdfProfile with your ESP-IDF profile path.' }
$idf = Join-Path $env:IDF_PATH 'tools\idf.py'
$env:PYTHONUTF8 = '1'
& (Join-Path $PSScriptRoot 'enter-build-key.ps1')
Push-Location (Join-Path $PSScriptRoot 'firmware')
try {
    if ($Configure) {
        & python $idf menuconfig
        if ($LASTEXITCODE -ne 0) { throw 'Firmware configuration failed.' }
    }
    & python $idf build
    if ($LASTEXITCODE -ne 0) { throw 'Firmware build failed.' }
    if ($Flash) {
        & python $idf -p $Port flash
        if ($LASTEXITCODE -ne 0) { throw 'Firmware flash failed.' }
    }
    Remove-Item -LiteralPath (Join-Path $PSScriptRoot 'firmware\.private\api-key.txt')
} finally {
    Pop-Location
}
