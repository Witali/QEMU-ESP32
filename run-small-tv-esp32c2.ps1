[CmdletBinding()]
param(
    [ValidateSet('run','prepare','status','stop','reset','screenshot','test')]
    [string]$Action = 'run',
    [ValidateSet('original','reconstructed')]
    [string]$Firmware = 'reconstructed',
    [string]$YoRadioRepository = '',
    [string]$QemuExecutable = '',
    [string]$PythonExecutable = '',
    [string]$RomElf = '',
    [string]$HardwareProfile = '',
    [string]$StateDirectory = '',
    [ValidateSet('sdl','none')][string]$Display = 'sdl',
    [ValidateSet('clock','setup','offline')][string]$Network = 'clock',
    [ValidateRange(-1,65535)][int]$HttpBasePort = 0,
    [Alias('WeatherPort')][ValidateRange(0,65535)][int]$ServicePort = 0,
    [string[]]$ServiceUrl = @(),
    [ValidateRange(5,3600)][int]$TestSeconds = 30,
    [ValidateRange(0,65535)][int]$GdbPort = 0,
    [string]$ScreenshotPath = '',
    [switch]$Persistent,
    [switch]$Paused
)
$ErrorActionPreference = 'Stop'
if (!$YoRadioRepository) {
    $YoRadioRepository = Join-Path (Split-Path -Parent $PSScriptRoot) 'yoRadio'
}
$YoRadioRepository = [IO.Path]::GetFullPath($YoRadioRepository)
if (!$StateDirectory) { $StateDirectory = Join-Path $PSScriptRoot ".cache\small-tv-c2\$Network-$Firmware" }
if (!$PythonExecutable) {
    $pythonCandidates = @(
        (Join-Path $YoRadioRepository '.idf\python-3.12.10\tools\python.exe'),
        (Join-Path $PSScriptRoot '..\HLV-codec\local_tools\msys2\msys64\mingw64\bin\python.exe')
    )
    $PythonExecutable = $pythonCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if (!$PythonExecutable) { $PythonExecutable = (Get-Command python.exe -ErrorAction Stop).Source }
}
if ($Action -in @('status','stop','reset','screenshot')) {
    $controlArguments = @((Join-Path $PSScriptRoot 'tools\small_tv_vm.py'), $Action, '--state', ([IO.Path]::GetFullPath($StateDirectory)))
    if ($ScreenshotPath) { $controlArguments += @('--screenshot', ([IO.Path]::GetFullPath($ScreenshotPath))) }
    & $PythonExecutable @controlArguments
    if ($LASTEXITCODE -ne 0) { throw "Small TV VM operation failed: $Action" }
    return
}
if (!$QemuExecutable) {
    $qemuCandidates = @('bin','build-esp32c2','build-riscv32','build') | ForEach-Object { Join-Path $PSScriptRoot "$_\qemu-system-riscv32.exe" }
    $QemuExecutable = $qemuCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
}
if (!$QemuExecutable) { throw 'Run setup-qemu-esp32c2-windows.ps1 first, or pass -QemuExecutable.' }
if (!$RomElf) {
    $romRoot = Join-Path $YoRadioRepository '.idf\tools-v6.1\tools\esp-rom-elfs'
    $RomElf = Get-ChildItem -LiteralPath $romRoot -Filter 'esp32c2_rev200_rom.elf' -Recurse -File -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (!$RomElf) { throw 'Pass -RomElf with the genuine esp32c2_rev200_rom.elf from the ESP-IDF esp-rom-elfs package.' }
$arguments = @(
    (Join-Path $PSScriptRoot 'tools\small_tv_vm.py'), $Action,
    '--firmware', $Firmware, '--yoradio', $YoRadioRepository,
    '--qemu', ([IO.Path]::GetFullPath($QemuExecutable)),
    '--rom-elf', ([IO.Path]::GetFullPath($RomElf)),
    '--state', ([IO.Path]::GetFullPath($StateDirectory)),
    '--network', $Network, '--http-base-port', "$HttpBasePort", '--service-port', "$ServicePort", '--display', $Display, '--seconds', "$TestSeconds", '--gdb-port', "$GdbPort"
)
foreach ($url in $ServiceUrl) { $arguments += @('--service-url', $url) }
if ($HardwareProfile) { $arguments += @('--hardware-profile', ([IO.Path]::GetFullPath($HardwareProfile))) }
if ($Persistent) { $arguments += '--persistent' }
if ($Paused) { $arguments += '--paused' }
if ($ScreenshotPath) { $arguments += @('--screenshot', ([IO.Path]::GetFullPath($ScreenshotPath))) }
if ($Action -eq 'test') { Write-Host "Testing $Firmware Small TV for $TestSeconds seconds, then reset and verify a second boot" }
& $PythonExecutable @arguments
if ($LASTEXITCODE -ne 0) { throw "Small TV VM operation failed: $Action" }
