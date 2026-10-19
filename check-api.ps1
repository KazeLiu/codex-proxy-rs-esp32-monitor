param([string]$BaseUrl)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
if (-not $BaseUrl) { $BaseUrl = Read-Host 'CPR base URL (https://...)' }
$baseUrl = $BaseUrl.TrimEnd('/')

Write-Host "CPR: $baseUrl"
$apiKey = Read-Host 'Admin API Key' -AsSecureString
$headers = @{ 'x-api-key' = [System.Net.NetworkCredential]::new('', $apiKey).Password }
$asOf = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()

function Get-CprData([string]$Path) {
    $response = Invoke-WebRequest -Uri "$baseUrl$Path" -Headers $headers -UseBasicParsing
    $json = [System.Text.Encoding]::UTF8.GetString($response.RawContentStream.ToArray())
    ($json | ConvertFrom-Json).data
}

Write-Host "`n=== Overview, health timeline and slots (today) ===" -ForegroundColor Cyan
$summary = Get-CprData "/api/admin/dashboard/summary?period=today&kind=usage&asOf=$asOf"
$summary |
    Select-Object asOf, asOfDisplay, analytics, cards, trend, healthTimeline, capacityInfo |
    ConvertTo-Json -Depth 20

foreach ($dimension in 'model', 'account', 'apiKey', 'failureClass') {
    Write-Host "`n=== Diagnostics: $dimension (today) ===" -ForegroundColor Cyan
    $diagnostics = Get-CprData "/api/admin/usage/insights/diagnostics?period=today&asOf=$asOf&dimension=$dimension"
    if ($dimension -eq 'account') {
        $notesById = @{}
        $page = 1
        do {
            $accounts = Get-CprData "/api/admin/accounts?page=$page&pageSize=20"
            foreach ($account in $accounts.items) {
                $notesById[$account.id] = $account.notes
            }
            $page++
        } while ($page -le $accounts.page.totalPages)
        foreach ($item in $diagnostics.items) {
            $item | Add-Member -NotePropertyName accountNotes -NotePropertyValue $notesById[$item.key]
        }
    }
    $diagnostics | ConvertTo-Json -Depth 20
}

Write-Host "`n=== Latest 10 requests, all outcomes (today) ===" -ForegroundColor Cyan
$records = Get-CprData "/api/admin/usage/records?period=today&asOf=$asOf&currentPage=1&pageSize=10&status=all"
$records | ConvertTo-Json -Depth 20

Write-Host "`n=== Latest 10 operation errors for new-error notices (today) ===" -ForegroundColor Cyan
$failures = Get-CprData "/api/admin/operations/errors?period=today&asOf=$asOf&currentPage=1&pageSize=10"
$failures | ConvertTo-Json -Depth 20

Write-Host "`nDone." -ForegroundColor Green
