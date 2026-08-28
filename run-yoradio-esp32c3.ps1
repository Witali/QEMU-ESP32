[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$FirmwareBuild = $env:YORADIO_ESP32C3_BUILD,
    [string]$QemuExecutable = "",
    [string]$StateDirectory = "",
    [string]$PythonExecutable = "",
    [string]$SpiffsGenerator = "",
    [ValidateSet("ideal", "hardware", "weak", "congested", "offline")]
    [string]$NetworkProfile = "hardware",
    [string]$Ssid = "yoRadio-QEMU",
    [string]$Password = "",
    [ValidateSet("dsound", "wav", "none")]
    [string]$AudioBackend = "dsound",
    [string]$AudioFile = "",
    [ValidateSet("sdl", "none")]
    [string]$Display = "sdl",
    [ValidateSet("stdio", "none")]
    [string]$Serial = "stdio",
    [string]$Monitor = "none",
    [ValidateRange(0, 65535)]
    [int]$WebPort = 8080,
    [switch]$RestrictedNetwork,
    [switch]$Snapshot,
    [switch]$ResetState,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$AdditionalQemuArguments
)

$ErrorActionPreference = "Stop"
$repository = [IO.Path]::GetFullPath($PSScriptRoot)

function Resolve-ExistingFile {
    param([Parameter(Mandatory)][string]$Path, [string]$Description = "File")

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description was not found: $Path"
    }
    return [IO.Path]::GetFullPath($Path)
}

function Resolve-ExistingDirectory {
    param([Parameter(Mandatory)][string]$Path, [string]$Description = "Directory")

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Description was not found: $Path"
    }
    return [IO.Path]::GetFullPath($Path)
}

if ([string]::IsNullOrWhiteSpace($FirmwareBuild)) {
    $repositoryParent = Split-Path -Parent $repository
    $workspaceCandidates = @($repositoryParent)
    if ((Split-Path -Leaf $repositoryParent) -eq ".worktree") {
        $workspaceCandidates += Split-Path -Parent (
            Split-Path -Parent $repositoryParent)
    }
    $firmwareCandidates = foreach ($workspace in $workspaceCandidates) {
        Join-Path $workspace `
            "yoRadio\idf\esp32c3-oled-native\build-production"
        Join-Path $workspace `
            "yoRadio\.worktree\esp32c3-qemu-profile\idf\esp32c3-oled-native\build-production"
    }
    $FirmwareBuild = $firmwareCandidates |
        Where-Object {
            Test-Path -LiteralPath (Join-Path $_ `
                "qemu-production-flash.bin") -PathType Leaf
        } |
        Select-Object -First 1
}
if ([string]::IsNullOrWhiteSpace($FirmwareBuild)) {
    throw "Pass -FirmwareBuild or set YORADIO_ESP32C3_BUILD."
}
$FirmwareBuild = Resolve-ExistingDirectory $FirmwareBuild `
    "yoRadio production build"

if ([string]::IsNullOrWhiteSpace($QemuExecutable)) {
    $qemuCandidates = @(
        (Join-Path $repository "bin\qemu-system-riscv32.exe"),
        (Join-Path $repository "build\qemu-system-riscv32.exe")
    )
    $QemuExecutable = $qemuCandidates |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
}
if ([string]::IsNullOrWhiteSpace($QemuExecutable)) {
    throw "Pass -QemuExecutable or build qemu-system-riscv32.exe."
}
$QemuExecutable = Resolve-ExistingFile $QemuExecutable "QEMU executable"

if ([string]::IsNullOrWhiteSpace($StateDirectory)) {
    $StateDirectory = Join-Path $repository "build\yoradio-esp32c3"
}
$StateDirectory = [IO.Path]::GetFullPath($StateDirectory)
New-Item -ItemType Directory -Force -Path $StateDirectory | Out-Null

if ($Ssid.Length -eq 0 -or $Ssid.Length -gt 31 -or
    $Ssid.IndexOfAny([char[]]"`t`r`n") -ge 0) {
    throw "SSID must contain 1..31 characters without tabs or newlines."
}
if ($Password.Length -gt 63 -or
    $Password.IndexOfAny([char[]]"`t`r`n") -ge 0) {
    throw "Password must contain at most 63 characters without tabs or newlines."
}

$baseFlash = Resolve-ExistingFile (Join-Path $FirmwareBuild `
    "qemu-production-flash.bin") "Combined production flash"
$stateFlash = Join-Path $StateDirectory "yoradio-esp32c3-flash.bin"
$spiffsImage = Join-Path $StateDirectory "spiffs.bin"
$spiffsRoot = Join-Path $StateDirectory "spiffs-root"
$spiffsSource = Resolve-ExistingDirectory (Join-Path $FirmwareBuild `
    "esp-idf\main\spiffs-root") "Production SPIFFS source"

$createState = $ResetState -or
    -not (Test-Path -LiteralPath $stateFlash -PathType Leaf)
if ($createState) {
    $statePrefix = $StateDirectory.TrimEnd([IO.Path]::DirectorySeparatorChar) +
        [IO.Path]::DirectorySeparatorChar
    $resolvedSpiffsRoot = [IO.Path]::GetFullPath($spiffsRoot)
    if (-not $resolvedSpiffsRoot.StartsWith(
            $statePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to recreate SPIFFS outside the state directory."
    }
    if (Test-Path -LiteralPath $spiffsRoot) {
        Remove-Item -LiteralPath $spiffsRoot -Recurse -Force
    }
    Copy-Item -LiteralPath $spiffsSource -Destination $spiffsRoot -Recurse
    $wifiPath = Join-Path $spiffsRoot "data\wifi.csv"
    [IO.File]::WriteAllText(
        $wifiPath, "$Ssid`t$Password`n", [Text.UTF8Encoding]::new($false))

    $buildNinja = Join-Path $FirmwareBuild "build.ninja"
    if ([string]::IsNullOrWhiteSpace($PythonExecutable) -or
        [string]::IsNullOrWhiteSpace($SpiffsGenerator)) {
        $spiffsRule = Select-String -LiteralPath $buildNinja `
            -SimpleMatch "spiffsgen.py 0x40000" |
            Select-Object -First 1
        if ($spiffsRule -and $spiffsRule.Line -match
            '(?<python>[A-Za-z]:\\\S+python\.exe)\s+(?<generator>[A-Za-z]:[/\\]\S+spiffsgen\.py)') {
            if ([string]::IsNullOrWhiteSpace($PythonExecutable)) {
                $PythonExecutable = $Matches.python
            }
            if ([string]::IsNullOrWhiteSpace($SpiffsGenerator)) {
                $SpiffsGenerator = $Matches.generator
            }
        }
    }
    $PythonExecutable = Resolve-ExistingFile $PythonExecutable `
        "ESP-IDF Python executable"
    $SpiffsGenerator = Resolve-ExistingFile $SpiffsGenerator `
        "ESP-IDF SPIFFS generator"

    & $PythonExecutable $SpiffsGenerator 0x40000 $spiffsRoot $spiffsImage `
        --page-size=256 --obj-name-len=32 --meta-len=0 `
        --use-magic --use-magic-len
    if ($LASTEXITCODE -ne 0) {
        throw "SPIFFS image generation failed."
    }
    $spiffsBytes = [IO.File]::ReadAllBytes($spiffsImage)
    if ($spiffsBytes.Length -ne 0x40000) {
        throw "SPIFFS image has an unexpected size: $($spiffsBytes.Length)."
    }

    Copy-Item -LiteralPath $baseFlash -Destination $stateFlash -Force
    $flashStream = [IO.File]::Open(
        $stateFlash, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite,
        [IO.FileShare]::None)
    try {
        if ($flashStream.Length -lt 0x3f0000) {
            throw "Combined flash is too small for the SPIFFS partition."
        }
        $null = $flashStream.Seek(0x3b0000, [IO.SeekOrigin]::Begin)
        $flashStream.Write($spiffsBytes, 0, $spiffsBytes.Length)
    } finally {
        $flashStream.Dispose()
    }
    Write-Host "Created persistent yoRadio state: $stateFlash"
} else {
    Write-Host "Reusing persistent yoRadio state: $stateFlash"
}

$profilesPath = Join-Path $repository `
    "configs\devices\esp32c3-yoradio-wifi.psd1"
$profiles = Import-PowerShellDataFile -LiteralPath $profilesPath
$profile = $profiles[$NetworkProfile]
if (-not $profile) {
    throw "Unknown network profile: $NetworkProfile"
}

$qemuArguments = @(
    "-M", $(if ($AudioBackend -eq "none") {
        "esp32c3"
    } else {
        "esp32c3,audiodev=audio0"
    }),
    "-display", $Display,
    "-serial", $Serial,
    "-monitor", $Monitor,
    "-no-reboot",
    "-global", "esp32c3-wifi.bandwidth-bps=$($profile.BandwidthBps)",
    "-global", "esp32c3-wifi.latency-ms=$($profile.LatencyMs)",
    "-global", "esp32c3-wifi.jitter-ms=$($profile.JitterMs)",
    "-global", "esp32c3-wifi.loss-ppm=$($profile.LossPpm)",
    "-global", "esp32c3-wifi.queue-limit=$($profile.QueueLimit)",
    "-global", "esp32c3-wifi.link-down=$(if ($profile.LinkDown) { 'on' } else { 'off' })"
)
if ($Snapshot) {
    $qemuArguments += "-snapshot"
}
if ($AudioBackend -eq "dsound") {
    $qemuArguments += @(
        "-audiodev", "dsound,id=audio0,out.frequency=48000"
    )
} elseif ($AudioBackend -eq "wav") {
    if ([string]::IsNullOrWhiteSpace($AudioFile)) {
        $AudioFile = Join-Path $StateDirectory "yoradio-audio.wav"
    }
    $AudioFile = [IO.Path]::GetFullPath($AudioFile)
    $qemuArguments += @(
        "-audiodev", "wav,id=audio0,path=$AudioFile,out.frequency=48000"
    )
}

$network = "user,model=esp32c3-wifi"
if ($RestrictedNetwork) {
    $network += ",restrict=on"
}
if ($WebPort -ne 0) {
    $network += ",hostfwd=tcp:127.0.0.1:$WebPort-10.0.2.15:80"
}
$qemuArguments += @(
    "-nic", $network,
    "-drive", "file=$stateFlash,if=mtd,format=raw"
)
if ($AdditionalQemuArguments) {
    $qemuArguments += $AdditionalQemuArguments
}

Write-Host "Starting unmodified yoRadio production firmware"
Write-Host "  QEMU: $QemuExecutable"
Write-Host "  Wi-Fi: $Ssid; profile: $NetworkProfile"
if ($WebPort -ne 0) {
    Write-Host "  Web UI: http://127.0.0.1:$WebPort/"
}
& $QemuExecutable @qemuArguments
exit $LASTEXITCODE
