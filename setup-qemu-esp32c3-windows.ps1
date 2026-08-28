[CmdletBinding()]
param(
    [string]$MsysRoot,
    [string]$BuildDirectory = "build-esp32c3",
    [switch]$ForceBuild,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$repository = [IO.Path]::GetFullPath($PSScriptRoot)
$buildPath = [IO.Path]::GetFullPath((Join-Path $repository $BuildDirectory))
$runtimeBin = Join-Path $repository "bin"
$runtimeData = Join-Path $repository "share\qemu"
$qemu = Join-Path $runtimeBin "qemu-system-riscv32.exe"
$runtimeMarker = Join-Path $runtimeBin ".qemu-esp32c3-commit"
$sourceCommit = (& git -C $repository rev-parse HEAD).Trim()
$runtimeReady =
    (Test-Path -LiteralPath $qemu -PathType Leaf) -and
    (Test-Path -LiteralPath $runtimeMarker -PathType Leaf) -and
    ((Get-Content -Raw -LiteralPath $runtimeMarker) -eq $sourceCommit)
if ($runtimeReady -and -not $SkipBuild -and -not $ForceBuild) {
    & $qemu --version
    if ($LASTEXITCODE -ne 0) {
        throw "The cached ESP32-C3 QEMU executable smoke check failed."
    }
    Write-Host "Native Windows ESP32-C3 QEMU is ready: $qemu"
    exit 0
}

if (-not $MsysRoot) {
    $candidates = @(
        (Join-Path $repository "local_tools\msys2\msys64"),
        [IO.Path]::GetFullPath((Join-Path $repository `
            "..\HLV-codec\local_tools\msys2\msys64")),
        [IO.Path]::GetFullPath((Join-Path $repository `
            "..\..\..\HLV-codec\local_tools\msys2\msys64"))
    )
    $MsysRoot = $candidates |
        Where-Object {
            Test-Path -LiteralPath (Join-Path $_ "usr\bin\bash.exe") `
                -PathType Leaf
        } |
        Select-Object -First 1
}
if (-not $MsysRoot) {
    throw (
        "MSYS2 with the MinGW64 QEMU dependencies was not found. " +
        "Pass -MsysRoot or install it under local_tools\msys2\msys64."
    )
}
$MsysRoot = [IO.Path]::GetFullPath($MsysRoot)
$bash = Join-Path $MsysRoot "usr\bin\bash.exe"

function Quote-Bash {
    param([Parameter(Mandatory)][string]$Value)

    $singleQuote = [string][char]39
    $replacement = $singleQuote + '"' + $singleQuote + '"' + $singleQuote
    return $singleQuote + $Value.Replace($singleQuote, $replacement) +
        $singleQuote
}

$env:CHERE_INVOKING = "yes"
$env:MSYSTEM = "MINGW64"
$msysRepository = (& $bash -lc (
    "cygpath -a -u " + (Quote-Bash $repository)
) | Where-Object { $_ -match '^/' } | Select-Object -Last 1).Trim()
if (-not $msysRepository) {
    throw "MSYS2 returned an empty QEMU-ESP32 path."
}
$msysBuildPath = (& $bash -lc (
    "cygpath -a -u " + (Quote-Bash $buildPath)
) | Where-Object { $_ -match '^/' } | Select-Object -Last 1).Trim()
if (-not $msysBuildPath) {
    throw "MSYS2 returned an empty ESP32-C3 build path."
}
$quotedRepository = Quote-Bash $msysRepository
$quotedBuildPath = Quote-Bash $msysBuildPath
$temporaryPath = Join-Path $buildPath "tmp"
New-Item -ItemType Directory -Force -Path $temporaryPath | Out-Null
$msysTemporaryPath = (& $bash -lc (
    "cygpath -a -u " + (Quote-Bash $temporaryPath)
) | Where-Object { $_ -match '^/' } | Select-Object -Last 1).Trim()
if (-not $msysTemporaryPath) {
    throw "MSYS2 returned an empty ESP32-C3 temporary path."
}
$quotedTemporaryPath = Quote-Bash $msysTemporaryPath
$buildEnvironment =
    "export PATH=/mingw64/bin:/usr/bin; " +
    "export TMPDIR=$quotedTemporaryPath; " +
    "export TMP=$quotedTemporaryPath; " +
    "export TEMP=$quotedTemporaryPath; "

if (-not (Test-Path -LiteralPath (Join-Path $buildPath "build.ninja") `
        -PathType Leaf)) {
    if ($SkipBuild) {
        throw "ESP32-C3 build is not configured under $buildPath."
    }
    New-Item -ItemType Directory -Force -Path $buildPath | Out-Null
    Write-Host "Configuring native Windows riscv32-softmmu with SDL and SLIRP"
    $configure =
        $buildEnvironment +
        "cd $quotedBuildPath && $quotedRepository/configure " +
        "--bindir=bin --datadir=share/qemu " +
        "--enable-gcrypt --enable-pixman --enable-sdl --enable-slirp " +
        "--enable-stack-protector --disable-werror " +
        "--prefix=$msysRepository/install/qemu-esp32c3 --static " +
        "--target-list=riscv32-softmmu " +
        "--with-pkgversion='yoRadio ESP32-C3 QEMU' " +
        "--with-suffix= --without-default-features"
    & $bash -lc $configure
    if ($LASTEXITCODE -ne 0) {
        throw "Could not configure ESP32-C3 QEMU."
    }
}

if (-not $SkipBuild) {
    Write-Host "Building native qemu-system-riscv32.exe"
    & $bash -lc (
        $buildEnvironment + "cd $quotedRepository && " +
        "ninja -C $quotedBuildPath qemu-system-riscv32.exe"
    )
    if ($LASTEXITCODE -ne 0) {
        throw "Could not build ESP32-C3 QEMU."
    }
}

$builtQemu = Join-Path $buildPath "qemu-system-riscv32.exe"
if (-not (Test-Path -LiteralPath $builtQemu -PathType Leaf)) {
    throw "Built ESP32-C3 QEMU was not found: $builtQemu"
}
New-Item -ItemType Directory -Force -Path $runtimeBin | Out-Null
New-Item -ItemType Directory -Force -Path $runtimeData | Out-Null
Copy-Item -LiteralPath $builtQemu -Destination $qemu -Force
Copy-Item -LiteralPath (Join-Path $repository "pc-bios\esp32c3-rom.bin") `
    -Destination $runtimeData -Force
Set-Content -LiteralPath $runtimeMarker -Value $sourceCommit -NoNewline
& $qemu --version
if ($LASTEXITCODE -ne 0) {
    throw "The ESP32-C3 QEMU executable smoke check failed."
}
Write-Host "Native Windows ESP32-C3 QEMU is ready: $qemu"
