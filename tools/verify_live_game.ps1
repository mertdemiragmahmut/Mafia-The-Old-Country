param(
    [int]$LaunchTimeoutSec = 60,
    [int]$InitTimeoutSec = 65
)

$ErrorActionPreference = "Continue"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$BinDir = Join-Path $Root "MafiaTheOldCountry\Binaries\Win64"
$Ue4ssDir = Join-Path $BinDir "ue4ss"
$Ue4ssLog = Join-Path $Ue4ssDir "UE4SS.log"
$BridgeLog = Join-Path $Ue4ssDir "DualSenseBridge.log"

Write-Host "=== Mafia: The Old Country - Live DualSense Mod Verification ===" -ForegroundColor Cyan
Write-Host "Game Root: $Root"

function Stop-GameProcesses {
    $procs = Get-Process | Where-Object { $_.ProcessName -like "*MafiaTheOldCountry*" }
    if ($procs) {
        Write-Host "Stopping existing game process(es) gracefully..."
        foreach ($p in $procs) {
            try { $null = $p.CloseMainWindow() } catch {}
        }
        for ($i = 0; $i -lt 24; $i++) {
            Start-Sleep -Milliseconds 500
            $alive = Get-Process | Where-Object { $_.ProcessName -like "*MafiaTheOldCountry*" }
            if (-not $alive) {
                Write-Host "Game exited cleanly after $(($i + 1) * 0.5)s."
                return
            }
        }
        $procs = Get-Process | Where-Object { $_.ProcessName -like "*MafiaTheOldCountry*" }
        foreach ($p in $procs) {
            Write-Host "Force-closing unresponsive process PID=$($p.Id)..." -ForegroundColor Yellow
            try { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } catch {}
        }
        Start-Sleep -Seconds 2
    }
}

Stop-GameProcesses

# Clean previous logs
Remove-Item $Ue4ssLog -Force -ErrorAction SilentlyContinue
Remove-Item $BridgeLog -Force -ErrorAction SilentlyContinue

Write-Host "Launching Mafia: The Old Country via Steam (steam://rungameid/1941540)..."
Start-Process "steam://rungameid/1941540"

$gameProc = $null
$swLaunch = [System.Diagnostics.Stopwatch]::StartNew()
while ($swLaunch.Elapsed.TotalSeconds -lt $LaunchTimeoutSec) {
    $procs = Get-Process | Where-Object { $_.ProcessName -like "*MafiaTheOldCountry*" }
    if ($procs) {
        $gameProc = $procs | Sort-Object WorkingSet64 -Descending | Select-Object -First 1
        Write-Host ("Game process detected: {0} (PID={1}) after {2:N1}s" -f $gameProc.ProcessName, $gameProc.Id, $swLaunch.Elapsed.TotalSeconds)
        break
    }
    Start-Sleep -Seconds 1
}

if (-not $gameProc) {
    Write-Host "[FAIL] Game process did not start within ${LaunchTimeoutSec}s." -ForegroundColor Red
    exit 1
}

$swInit = [System.Diagnostics.Stopwatch]::StartNew()
$bridgeReady = $false
$luaReady = $false
$uiLogged = $false

while ($swInit.Elapsed.TotalSeconds -lt $InitTimeoutSec) {
    $stillRunning = Get-Process -Id $gameProc.Id -ErrorAction SilentlyContinue
    if (-not $stillRunning) {
        Write-Host "[WARN] Game process exited unexpectedly at $($swInit.Elapsed.TotalSeconds.ToString('N1'))s." -ForegroundColor Yellow
        break
    }

    if ((Test-Path $BridgeLog) -and -not $bridgeReady) {
        $bTxt = Get-Content $BridgeLog -Raw -ErrorAction SilentlyContinue
        if ($bTxt -match "HID: Wired DualSense connected" -and $bTxt -match "WASAPI: DualSense 4ch haptic") {
            $bridgeReady = $true
            Write-Host ("DualSenseBridge ready at {0:N1}s" -f $swInit.Elapsed.TotalSeconds) -ForegroundColor Green
        }
    }

    if (Test-Path $Ue4ssLog) {
        $uTxt = Get-Content $Ue4ssLog -Raw -ErrorAction SilentlyContinue
        if (-not $luaReady -and $uTxt -match "DualSenseBridge IPC ready") {
            $luaReady = $true
            Write-Host ("MafiaDualSenseMod IPC ready at {0:N1}s" -f $swInit.Elapsed.TotalSeconds) -ForegroundColor Green
        }
        if (-not $uiLogged -and ($uTxt -match "Initial UI state:" -or $uTxt -match "Registered CommonInput gamepads")) {
            $uiLogged = $true
            Write-Host ("UI/CommonInput state captured at {0:N1}s" -f $swInit.Elapsed.TotalSeconds) -ForegroundColor Green
        }
    }

    if ($bridgeReady -and $luaReady -and $uiLogged) {
        Write-Host "All startup markers observed; letting game run 6s more for XInput polling & stability..."
        Start-Sleep -Seconds 6
        break
    }

    Start-Sleep -Seconds 1
}

Write-Host "Closing game gracefully..."
Stop-GameProcesses
Start-Sleep -Seconds 2

$failures = 0
function Check-Match($label, $text, $pattern) {
    if ($text -and ($text -match $pattern)) {
        Write-Host "[PASS] $label" -ForegroundColor Green
        return $true
    } else {
        Write-Host "[FAIL] $label" -ForegroundColor Red
        $script:failures++
        return $false
    }
}

Write-Host "`n--- Verification Checks ---" -ForegroundColor Cyan
$bridgeContent = if (Test-Path $BridgeLog) { Get-Content $BridgeLog -Raw -ErrorAction SilentlyContinue } else { "" }
$ue4ssContent  = if (Test-Path $Ue4ssLog)  { Get-Content $Ue4ssLog  -Raw -ErrorAction SilentlyContinue } else { "" }

Check-Match "DualSenseBridge.log exists" ($bridgeContent.Length -gt 0).ToString() "True"
Check-Match "Bridge attached (xinput1_4.dll)" $bridgeContent "Bridge: xinput1_4\.dll attached"
Check-Match "Wired DualSense HID connected" $bridgeContent "HID: Wired DualSense connected \(VID=0x054C"
Check-Match "WASAPI 4ch Haptics active" $bridgeContent "WASAPI: DualSense 4ch haptic audio endpoint active"
Check-Match "Named Pipe IPC listening" $bridgeContent "IPC: Pipe server listening"
Check-Match "Game called XInputGetState (or Ex)" $bridgeContent "XInputGetState(Ex)?.*first call|Ord100"

Check-Match "UE4SS.log exists" ($ue4ssContent.Length -gt 0).ToString() "True"
Check-Match "MafiaDualSenseMod started" $ue4ssContent "\[MafiaDualSense\] Initializing"
Check-Match "Core C++ hooks attached" $ue4ssContent "\[MafiaDualSense\] Core C\+\+ hooks attached:\s*[1-9]\d*"
Check-Match "Lua IPC ready (PING=PONG, connected=true)" $ue4ssContent "DualSenseBridge IPC ready \(PING=PONG, connected=true"
Check-Match "Lua confirmed WASAPI=1 & CONNECTED=1" $ue4ssContent "Bridge STATE:.*CONNECTED=1.*WASAPI=1"

# Check for Lua errors (excluding UE4SS's built-in FObjectPtrProperty startup notice)
$luaErrLines = @()
if ($ue4ssContent) {
    $luaErrLines = ($ue4ssContent -split "`r?`n") | Where-Object {
        ($_ -notmatch "FObjectPtrProperty") -and
        (
            ($_ -match "MafiaDualSenseMod.*(Error|error|attempt to|stack traceback)") -or
            ($_ -match "\[Lua\].*(Lua Error|stack traceback|attempt to|syntax error|Failed to load)") -or
            ($_ -match "\[MafiaDualSense\].*(Error|FAIL)")
        )
    }
}
if ($luaErrLines.Count -gt 0) {
    Write-Host "[FAIL] Lua errors detected in UE4SS.log:" -ForegroundColor Red
    $luaErrLines | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    $failures++
} else {
    Write-Host "[PASS] Zero MafiaDualSense Lua errors in UE4SS.log" -ForegroundColor Green
}

Write-Host "`n--- DualSenseBridge.log ---" -ForegroundColor Cyan
if ($bridgeContent) { Write-Host $bridgeContent.TrimEnd() } else { Write-Host "(missing)" }

Write-Host "`n--- [MafiaDualSense] lines in UE4SS.log ---" -ForegroundColor Cyan
if ($ue4ssContent) {
    ($ue4ssContent -split "`r?`n") | Where-Object { $_ -match "MafiaDualSense|Starting mods|MafiaDualSenseMod" } | ForEach-Object { Write-Host $_ }
} else {
    Write-Host "(missing)"
}

Write-Host "`nSummary: failures=$failures" -ForegroundColor $(if ($failures -eq 0) { "Green" } else { "Red" })
if ($failures -ne 0) { exit 1 } else { exit 0 }
