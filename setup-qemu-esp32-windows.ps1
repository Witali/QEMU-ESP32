[CmdletBinding()]
param(
    [string]$MsysRoot
)

$ErrorActionPreference = "Stop"
$repository = $PSScriptRoot
$runtimeBin = Join-Path $repository "bin"
$runtimeData = Join-Path $repository "share\qemu"
$qemu = Join-Path $runtimeBin "qemu-system-xtensa.exe"

if (-not $MsysRoot) {
    $candidates = @(
        (Join-Path $repository "local_tools\msys2\msys64"),
        [IO.Path]::GetFullPath((Join-Path $repository `
            "..\HLV-codec\local_tools\msys2\msys64"))
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
) | Select-Object -First 1).Trim()
if ($LASTEXITCODE -ne 0) {
    throw "Could not convert the QEMU-ESP32 path for MSYS2."
}
$quotedRepository = Quote-Bash $msysRepository

if (-not (Test-Path -LiteralPath (Join-Path $repository `
    "build\build.ninja") -PathType Leaf)) {
    Write-Host "Configuring native Windows xtensa-softmmu with SDL"
    $configure =
        "export PATH=/mingw64/bin:/usr/bin; " +
        "cd $quotedRepository && ./configure " +
        "--bindir=bin --datadir=share/qemu " +
        "--enable-gcrypt --enable-pixman --enable-sdl --enable-slirp " +
        "--enable-stack-protector --disable-werror " +
        "--prefix=$msysRepository/install/qemu --static " +
        "--target-list=xtensa-softmmu " +
        "--with-pkgversion='DosToEsp QEMU-ESP32' " +
        "--with-suffix= --without-default-features"
    & $bash -lc $configure
    if ($LASTEXITCODE -ne 0) {
        throw "Could not configure QEMU-ESP32."
    }
}

Write-Host "Building native qemu-system-xtensa.exe"
& $bash -lc (
    "export PATH=/mingw64/bin:/usr/bin; cd $quotedRepository && " +
    "ninja -C build qemu-system-xtensa.exe"
)
if ($LASTEXITCODE -ne 0) {
    throw "Could not build QEMU-ESP32."
}

New-Item -ItemType Directory -Force -Path $runtimeBin | Out-Null
New-Item -ItemType Directory -Force -Path $runtimeData | Out-Null
Copy-Item -LiteralPath (Join-Path $repository `
    "build\qemu-system-xtensa.exe") -Destination $runtimeBin -Force
foreach ($rom in @("esp32-v3-rom.bin", "esp32-v3-rom-app.bin")) {
    Copy-Item -LiteralPath (Join-Path $repository "pc-bios\$rom") `
        -Destination $runtimeData -Force
}
& $qemu --version
if ($LASTEXITCODE -ne 0) {
    throw "The QEMU-ESP32 executable smoke check failed."
}
Write-Host "Native Windows QEMU-ESP32 is ready: $qemu"
