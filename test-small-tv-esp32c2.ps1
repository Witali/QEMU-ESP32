[CmdletBinding()]
param(
    [string]$YoRadioRepository = '',
    [string]$QemuExecutable = '',
    [string]$PythonExecutable = '',
    [string]$RomElf = '',
    [string]$StateDirectory = '',
    [ValidateSet('clock','offline')][string]$Network = 'clock',
    [ValidateRange(5,3600)][int]$TestSeconds = 30
)
$ErrorActionPreference = 'Stop'
if (!$StateDirectory) {
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')
    $StateDirectory = Join-Path $PSScriptRoot ".cache\small-tv-c2\test-runs\$stamp"
}
New-Item -ItemType Directory -Force -Path $StateDirectory | Out-Null
$results = @()
foreach ($variant in @('original','reconstructed')) {
    $variantState = Join-Path $StateDirectory $variant
    $runArguments = @{ Action='test'; Firmware=$variant; StateDirectory=$variantState; TestSeconds=$TestSeconds; Display='none'; Network=$Network }
    foreach ($name in @('YoRadioRepository','QemuExecutable','PythonExecutable','RomElf')) {
        $value = Get-Variable -Name $name -ValueOnly
        if ($value) { $runArguments[$name] = $value }
    }
    try {
        & (Join-Path $PSScriptRoot 'run-small-tv-esp32c2.ps1') @runArguments
        $report = Get-Content -Raw -LiteralPath (Join-Path $variantState 'test-report.json') | ConvertFrom-Json
        $results += @{ firmware=$variant; passed=$report.passed; report=(Join-Path $variantState 'test-report.json') }
    } catch {
        $results += @{ firmware=$variant; passed=$false; error="$($_.Exception.Message)" }
    }
}
$passed = @($results | Where-Object { !$_.passed }).Count -eq 0
$summary = @{ passed=$passed; results=$results; network=$Network; tested_at_utc=[DateTime]::UtcNow.ToString('o') }
$summaryPath = Join-Path $StateDirectory 'test-summary.json'
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $summaryPath -Encoding utf8
Write-Host "Both firmware reports saved: $summaryPath"
if (!$passed) { throw 'One or more Small TV VM tests failed; inspect the individual reports and UART logs.' }
