[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$HardwareProfile,
    [Parameter(Mandatory)][string]$FlashImage,
    [string]$QemuExecutable = '',
    [string]$PythonExecutable = '',
    [string]$RomDirectory = '',
    [ValidateSet('none','sdl')][string]$Display = 'none',
    [ValidateSet('offline','clock','setup')][string]$Network = 'offline',
    [ValidateRange(0,65535)][int]$ServicePort = 0,
    [string[]]$ServiceUrl = @(),
    [string[]]$ExtraArguments = @()
)
$ErrorActionPreference = 'Stop'
if (!$PythonExecutable) {
    $PythonExecutable = Join-Path $PSScriptRoot '..\yoRadio\.idf\python-3.12.10\tools\python.exe'
    if (!(Test-Path -LiteralPath $PythonExecutable)) { $PythonExecutable = (Get-Command python.exe).Source }
}
$profileArguments = @((Join-Path $PSScriptRoot 'tools\esp_board.py'), ([IO.Path]::GetFullPath($HardwareProfile)), '--network', $Network, '--service-port', "$ServicePort")
foreach ($url in $ServiceUrl) { $profileArguments += @('--service-url', $url) }
$json = & $PythonExecutable @profileArguments
if ($LASTEXITCODE -ne 0) { throw 'Invalid hardware profile' }
$config = ($json -join "`n") | ConvertFrom-Json
if (!$QemuExecutable) {
    $arch = if ($config.soc -eq 'esp32') { 'xtensa' } else { 'riscv32' }
    $QemuExecutable = Join-Path $PSScriptRoot "bin\qemu-system-$arch.exe"
}
if (!(Test-Path -LiteralPath $QemuExecutable)) { throw 'Build/install the matching QEMU executable first' }
if (!$RomDirectory) { $RomDirectory = Join-Path $PSScriptRoot 'pc-bios' }
$flash = ([IO.Path]::GetFullPath($FlashImage)).Replace(',',',,')
$arguments = @('-M', $config.machine, '-L', ([IO.Path]::GetFullPath($RomDirectory)), '-drive', "file=$flash,if=mtd,format=raw", '-snapshot', '-display', $Display)
if ($config.network_arguments.Count) { $arguments += $config.network_arguments } else { $arguments += @('-nic', 'none') }
$arguments += $config.arguments
$arguments += $ExtraArguments
& $QemuExecutable @arguments
if ($LASTEXITCODE -ne 0) { throw 'QEMU exited with an error' }
