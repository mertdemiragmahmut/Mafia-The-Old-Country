$ErrorActionPreference = "Stop"

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$BinSrc = Join-Path $Root "MafiaTheOldCountry\Binaries\Win64"
$Ue4ssSrc = Join-Path $BinSrc "ue4ss"
$DistDir = Join-Path $Root "DualSenseModDev\dist"
$BuildDir = Join-Path $Root "DualSenseModDev\build"
$StageDir = Join-Path $BuildDir "stage_payload"
$ZipPath = Join-Path $BuildDir "MafiaTOCDualSensePayload.zip"
$CsPath = Join-Path $Root "DualSenseModDev\src\Installer\Program.cs"
$DistExe = Join-Path $DistDir "MafiaTheOldCountry_DualSenseMod_Kur_Kaldir.exe"
$RootExe = Join-Path $Root "MafiaTheOldCountry_DualSenseMod_Kur_Kaldir.exe"

$NetDir = "C:\Windows\Microsoft.NET\Framework64\v4.0.30319"
$Csc = Join-Path $NetDir "csc.exe"
if (-not (Test-Path $Csc)) { throw "csc.exe not found at $Csc" }

Write-Host "=== Building Single-File Turkish DualSense Mod Installer ===" -ForegroundColor Cyan

# Clean and recreate staging directories
if (Test-Path $StageDir) { Remove-Item $StageDir -Recurse -Force }
if (Test-Path $ZipPath)  { Remove-Item $ZipPath -Force }
New-Item -ItemType Directory -Path $StageDir -Force | Out-Null
New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
New-Item -ItemType Directory -Path $DistDir  -Force | Out-Null

$payloadMap = @(
    @{ Src = (Join-Path $BinSrc "dwmapi.dll"); Rel = "MafiaTheOldCountry\Binaries\Win64\dwmapi.dll" },
    @{ Src = (Join-Path $DistDir "xinput1_4.dll"); Rel = "MafiaTheOldCountry\Binaries\Win64\xinput1_4.dll" },
    @{ Src = (Join-Path $DistDir "hid.dll"); Rel = "MafiaTheOldCountry\Binaries\Win64\hid.dll" },
    @{ Src = (Join-Path $Ue4ssSrc "UE4SS.dll"); Rel = "MafiaTheOldCountry\Binaries\Win64\ue4ss\UE4SS.dll" },
    @{ Src = (Join-Path $Ue4ssSrc "LICENSE"); Rel = "MafiaTheOldCountry\Binaries\Win64\ue4ss\LICENSE" },
    @{ Src = (Join-Path $Ue4ssSrc "UE4SS-settings.ini"); Rel = "MafiaTheOldCountry\Binaries\Win64\ue4ss\UE4SS-settings.ini" },
    @{ Src = (Join-Path $Ue4ssSrc "MemberVariableLayout.ini"); Rel = "MafiaTheOldCountry\Binaries\Win64\ue4ss\MemberVariableLayout.ini" },
    @{ Src = (Join-Path $Ue4ssSrc "Mods\mods.txt"); Rel = "MafiaTheOldCountry\Binaries\Win64\ue4ss\Mods\mods.txt" },
    @{ Src = (Join-Path $Ue4ssSrc "Mods\shared\UEHelpers\UEHelpers.lua"); Rel = "MafiaTheOldCountry\Binaries\Win64\ue4ss\Mods\shared\UEHelpers\UEHelpers.lua" },
    @{ Src = (Join-Path $Ue4ssSrc "Mods\MafiaDualSenseMod\enabled.txt"); Rel = "MafiaTheOldCountry\Binaries\Win64\ue4ss\Mods\MafiaDualSenseMod\enabled.txt" },
    @{ Src = (Join-Path $Ue4ssSrc "Mods\MafiaDualSenseMod\Scripts\main.lua"); Rel = "MafiaTheOldCountry\Binaries\Win64\ue4ss\Mods\MafiaDualSenseMod\Scripts\main.lua" }
)

# Add all verified custom UE4SS signatures
$sigFiles = Get-ChildItem (Join-Path $Ue4ssSrc "UE4SS_Signatures\*.lua")
if ($sigFiles.Count -eq 0) { throw "No UE4SS_Signatures\*.lua files found!" }
foreach ($sf in $sigFiles) {
    $payloadMap += @{ Src = $sf.FullName; Rel = ("MafiaTheOldCountry\Binaries\Win64\ue4ss\UE4SS_Signatures\" + $sf.Name) }
}

foreach ($item in $payloadMap) {
    if (-not (Test-Path $item.Src)) {
        throw "Missing payload source file: $($item.Src)"
    }
    $dest = Join-Path $StageDir $item.Rel
    $parent = Split-Path $dest -Parent
    if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    Copy-Item $item.Src $dest -Force
    Write-Host ("  Staged: {0} ({1:N0} bytes)" -f $item.Rel, (Get-Item $dest).Length)
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $StageDir,
    $ZipPath,
    [System.IO.Compression.CompressionLevel]::Optimal,
    $false
)
Write-Host ("Created embedded payload archive: {0} ({1:N0} bytes)" -f $ZipPath, (Get-Item $ZipPath).Length) -ForegroundColor Green

$cscArgs = @(
    "/nologo",
    "/target:exe",
    "/platform:x64",
    "/optimize+",
    "/utf8output",
    "/r:$NetDir\System.IO.Compression.dll",
    "/r:$NetDir\System.IO.Compression.FileSystem.dll",
    "/resource:$ZipPath,MafiaTOCDualSensePayload.zip",
    "/out:$DistExe",
    $CsPath
)

Write-Host "Compiling with csc.exe..."
& $Csc @cscArgs
if ($LASTEXITCODE -ne 0) {
    throw "csc.exe failed with exit code $LASTEXITCODE"
}

Copy-Item $DistExe $RootExe -Force
Write-Host ("Build succeeded:`n  {0} ({1:N0} bytes)`n  {2}" -f $DistExe, (Get-Item $DistExe).Length, $RootExe) -ForegroundColor Green
