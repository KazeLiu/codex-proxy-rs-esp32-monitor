$ErrorActionPreference = 'Stop'
$directory = Join-Path $PSScriptRoot 'firmware\.private'
[System.IO.Directory]::CreateDirectory($directory) | Out-Null
$key = Read-Host 'Enter CPR admin API Key for this firmware build' -AsSecureString
$text = [System.Net.NetworkCredential]::new('', $key).Password
if ([string]::IsNullOrWhiteSpace($text)) { throw 'API Key is required.' }
[System.IO.File]::WriteAllText((Join-Path $directory 'api-key.txt'), $text, [System.Text.UTF8Encoding]::new($false))
$text = $null
Write-Host 'Key is ready for the build. This window will close.'
Start-Sleep -Seconds 2
