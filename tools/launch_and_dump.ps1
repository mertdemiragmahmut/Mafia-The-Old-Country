$ErrorActionPreference = "Continue"
$Win64 = "c:\Program Files (x86)\Steam\steamapps\common\Mafia The Old Country\MafiaTheOldCountry\Binaries\Win64"
$DumpDir = "c:\Program Files (x86)\Steam\steamapps\common\Mafia The Old Country\DualSenseModDev\dumps"
$Ue4ssLog = Join-Path $Win64 "ue4ss\UE4SS.log"

# Clear old flags
foreach ($f in @("init.flag", "done.flag", "error.flag")) {
    $p = Join-Path $DumpDir $f
    if (Test-Path $p) { Remove-Item $p -Force }
}
if (Test-Path $Ue4ssLog) { Remove-Item $Ue4ssLog -Force }

# Ensure game isn't already running
Get-Process -Name "MafiaTheOldCountry" -ErrorAction SilentlyContinue | Stop-Process -Force

Write-Host "Launching Mafia: The Old Country via Steam..."
Start-Process "steam://rungameid/1941540"

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$timeoutSec = 180
$sawProc = $false
$sawLog = $false
$sawInit = $false

while ($sw.Elapsed.TotalSeconds -lt $timeoutSec) {
    $proc = Get-Process -Name "MafiaTheOldCountry" -ErrorAction SilentlyContinue
    if ($proc -and -not $sawProc) {
        $sawProc = $true
        Write-Host ("[{0:F1}s] Process started PID={1}" -f $sw.Elapsed.TotalSeconds, ($proc.Id -join ","))
    }
    if ((Test-Path $Ue4ssLog) -and -not $sawLog) {
        $sawLog = $true
        Write-Host ("[{0:F1}s] UE4SS.log detected!" -f $sw.Elapsed.TotalSeconds)
    }
    if ((Test-Path (Join-Path $DumpDir "init.flag")) -and -not $sawInit) {
        $sawInit = $true
        Write-Host ("[{0:F1}s] init.flag detected!" -f $sw.Elapsed.TotalSeconds)
    }
    if (Test-Path (Join-Path $DumpDir "done.flag")) {
        Write-Host ("[{0:F1}s] done.flag detected!" -f $sw.Elapsed.TotalSeconds)
        Get-Content (Join-Path $DumpDir "done.flag")
        break
    }
    if (Test-Path (Join-Path $DumpDir "error.flag")) {
        Write-Host ("[{0:F1}s] error.flag detected!" -f $sw.Elapsed.TotalSeconds)
        Get-Content (Join-Path $DumpDir "error.flag")
        break
    }
    if ($sawProc -and -not $proc -and $sw.Elapsed.TotalSeconds -gt 15) {
        Write-Host ("[{0:F1}s] Process exited unexpectedly!" -f $sw.Elapsed.TotalSeconds)
        break
    }
    Start-Sleep -Seconds 2
}

if (-not (Test-Path (Join-Path $DumpDir "done.flag"))) {
    Write-Host ("Timeout or early exit at {0:F1}s. sawProc={1} sawLog={2} sawInit={3}" -f $sw.Elapsed.TotalSeconds, $sawProc, $sawLog, $sawInit)
}

Write-Host "Stopping MafiaTheOldCountry if still running..."
Get-Process -Name "MafiaTheOldCountry" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2
if (Test-Path $Ue4ssLog) {
    Write-Host "=== Tail of UE4SS.log (first 60 + last 40 lines) ==="
    $lines = Get-Content $Ue4ssLog
    $lines | Select-Object -First 60
    if ($lines.Count -gt 60) {
        Write-Host "... ($($lines.Count) total lines) ..."
        $lines | Select-Object -Last 40
    }
} else {
    Write-Host "WARNING: UE4SS.log was NOT created."
}
