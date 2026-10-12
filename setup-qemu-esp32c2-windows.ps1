[CmdletBinding()]
param(
    [string]$MsysRoot = '',
    [string]$BuildDirectory = 'build-esp32c2',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
# C2 and C3 share the native riscv32-softmmu build and its Windows installer.
$buildArguments = @{ BuildDirectory = $BuildDirectory; ForceBuild = $true }
if ($MsysRoot) { $buildArguments.MsysRoot = $MsysRoot }
if ($SkipBuild) { $buildArguments.SkipBuild = $true }
& (Join-Path $PSScriptRoot 'setup-qemu-esp32c3-windows.ps1') @buildArguments
$qemu = Join-Path $PSScriptRoot 'bin\qemu-system-riscv32.exe'
$machines = & $qemu -machine help
if ($LASTEXITCODE -ne 0 -or !($machines -match '^esp32c2\s')) {
    throw 'The installed QEMU binary does not contain the esp32c2 machine.'
}
$displayBackends = & $qemu -display help
if ($LASTEXITCODE -ne 0 -or !($displayBackends -match '^sdl$')) {
    throw 'This binary has no SDL backend. Configure its build with -Dsdl=enabled, or use a fresh default build-esp32c2 directory.'
}
Write-Host "ESP32-C2 Small TV machine with SDL ready: $qemu"
