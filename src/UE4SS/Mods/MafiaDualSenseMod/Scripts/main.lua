-- Mafia: The Old Country - Native Wired DualSense Integration Mod (UE4SS)
-- Connects Unreal Engine 5 / Hangar 13 subsystems to the xinput1_4.dll DualSense bridge
-- over Named Pipe \\.\pipe\MafiaTOCDualSenseBridge for prompts, adaptive triggers,
-- WASAPI 4-channel haptics, and lightbar feedback.

local UEHelpers = require("UEHelpers")

local PIPE_PATH = "\\\\.\\pipe\\MafiaTOCDualSenseBridge"
local LOG_PREFIX = "[MafiaDualSense] "

local function log(fmt, ...)
    local ok, msg = pcall(string.format, fmt, ...)
    if ok then
        print(LOG_PREFIX .. msg .. "\n")
    else
        print(LOG_PREFIX .. tostring(fmt) .. "\n")
    end
end

-- ============================================================
-- Named Pipe IPC Client (Lua io.open "r+b" + unbuffered)
-- ============================================================
local pipeHandle = nil
local pipeLastAttemptMs = 0
local pipeConnectedOnce = false

local bridgeState = {
    reachable = false,
    connected = false,
    active = false,
    wasapi = false,
    l2Mode = 0,
    r2Mode = 0,
    lastQueryMs = 0,
}

local function nowMs()
    return math.floor(os.clock() * 1000)
end

local function closePipe()
    if pipeHandle then
        pcall(function() pipeHandle:close() end)
        pipeHandle = nil
    end
end

local function ensurePipe()
    if pipeHandle then return true end
    local t = nowMs()
    if (t - pipeLastAttemptMs) < 300 then
        return false
    end
    pipeLastAttemptMs = t
    local f = io.open(PIPE_PATH, "r+b")
    if not f then
        return false
    end
    pcall(function() f:setvbuf("no") end)
    pipeHandle = f
    if not pipeConnectedOnce then
        pipeConnectedOnce = true
        log("IPC pipe connected: %s", PIPE_PATH)
    end
    return true
end

local function sendCmd(cmd)
    if not cmd or cmd == "" then return false end
    for attempt = 1, 2 do
        if not ensurePipe() then return false end
        local ok = pcall(function()
            pipeHandle:seek("cur", 0)
            pipeHandle:write(cmd .. "\n")
            pipeHandle:flush()
        end)
        if ok then
            return true
        end
        closePipe()
    end
    return false
end

local function queryCmd(cmd)
    for attempt = 1, 2 do
        if not ensurePipe() then return nil end
        local ok, line = pcall(function()
            pipeHandle:seek("cur", 0)
            pipeHandle:write(cmd .. "\n")
            pipeHandle:flush()
            pipeHandle:seek("cur", 0)
            local res = pipeHandle:read("*l")
            pipeHandle:seek("cur", 0)
            return res
        end)
        if ok and line and #line > 0 then
            return line:gsub("[\r\n]+$", "")
        end
        closePipe()
    end
    return nil
end

local function refreshBridgeState(force)
    local t = nowMs()
    if not force and (t - bridgeState.lastQueryMs) < 500 then
        return bridgeState.reachable
    end
    bridgeState.lastQueryMs = t
    local reply = queryCmd("STATE")
    if not reply then
        bridgeState.reachable = false
        bridgeState.connected = false
        bridgeState.active = false
        bridgeState.wasapi = false
        return false
    end
    local conn = tonumber(reply:match("CONNECTED=(%d+)")) or 0
    local act  = tonumber(reply:match("ACTIVE=(%d+)")) or 0
    local l2   = tonumber(reply:match("L2=(%d+)")) or 0
    local r2   = tonumber(reply:match("R2=(%d+)")) or 0
    local was  = tonumber(reply:match("WASAPI=(%d+)")) or 0
    local prevConn = bridgeState.connected
    local prevWas  = bridgeState.wasapi
    bridgeState.reachable = true
    bridgeState.connected = (conn == 1)
    bridgeState.active = (act == 1)
    bridgeState.l2Mode = l2
    bridgeState.r2Mode = r2
    bridgeState.wasapi = (was == 1)
    if bridgeState.connected ~= prevConn or bridgeState.wasapi ~= prevWas then
        log("Bridge STATE: %s", reply)
    end
    return true
end

-- ============================================================
-- Safe UE Helpers & FName cache
-- ============================================================
local fnameCache = {}
local function getFName(str)
    if fnameCache[str] then return fnameCache[str] end
    local ok, fn = pcall(UEHelpers.FindOrAddFName, str)
    if ok and fn then
        fnameCache[str] = fn
        return fn
    end
    local ok2, fn2 = pcall(FName, str)
    if ok2 and fn2 then
        fnameCache[str] = fn2
        return fn2
    end
    return nil
end

local function safeStr(v)
    if v == nil then return "" end
    local okToStr, s1 = pcall(function() return v:ToString() end)
    if okToStr and type(s1) == "string" and s1 ~= "" then return s1 end
    local ok, s = pcall(tostring, v)
    return ok and s or ""
end

local function clamp(x, lo, hi)
    if x < lo then return lo end
    if x > hi then return hi end
    return x
end

local function safeRound(x, lo, hi)
    return clamp(math.floor((tonumber(x) or 0) + 0.5), lo, hi)
end

local noesisLibCDO = nil
local function getNoesisLib()
    if noesisLibCDO and noesisLibCDO:IsValid() then return noesisLibCDO end
    local obj = StaticFindObject("/Script/NoesisRuntime.Default__NoesisFunctionLibrary")
    if obj and obj:IsValid() then
        noesisLibCDO = obj
        return obj
    end
    return nil
end

local function noesisNotify(owner, propNameStr)
    local lib = getNoesisLib()
    local fn = getFName(propNameStr)
    if lib and fn and owner and owner:IsValid() then
        pcall(function() lib:NotifyChanged(owner, fn) end)
    end
end

-- ============================================================
-- PlayStation Prompt Enforcement (CommonInput + Noesis VMs)
-- ============================================================
local psGamepadNameStr = "PS5"
local discoveredGamepadsLogged = false
local loggedPromptOverride = false
local isApplyingPromptOverride = false

local function isPlayStationGamepadName(nameStr)
    local low = (nameStr or ""):lower()
    return low:find("ps5", 1, true) ~= nil
        or low:find("dualsense", 1, true) ~= nil
        or low:find("playstation", 1, true) ~= nil
        or low:find("sony", 1, true) ~= nil
        or low:find("ps4", 1, true) ~= nil
        or low:find("dualshock", 1, true) ~= nil
end

local function discoverRegisteredGamepads()
    if discoveredGamepadsLogged then return end
    discoveredGamepadsLogged = true
    local names = {}
    local seen = {}

    local function addName(n)
        local cur = n
        for _ = 1, 3 do
            if type(cur) == "userdata" and tostring(cur):find("RemoteUnrealParam", 1, true) then
                local okG, inner = pcall(function() return cur:get() end)
                if okG and inner ~= nil then
                    cur = inner
                else
                    break
                end
            else
                break
            end
        end
        local s = safeStr(cur)
        if s ~= "" and s ~= "None" and not s:find("RemoteUnrealParam", 1, true) and not seen[s] then
            seen[s] = true
            table.insert(names, s)
        end
    end

    pcall(function()
        local cdo = StaticFindObject("/Script/CommonInput.Default__CommonInputBaseControllerData")
        if cdo and cdo:IsValid() then
            local arr = cdo:GetRegisteredGamepads()
            if arr then
                local num = 0
                pcall(function() num = arr:GetArrayNum() end)
                if num > 0 then
                    pcall(function()
                        arr:ForEach(function(idx, elem)
                            local v = elem
                            pcall(function() v = elem:get() end)
                            addName(v)
                        end)
                    end)
                end
            end
        end
    end)

    pcall(function()
        local allData = FindAllOf("CommonInputBaseControllerData")
        if allData then
            for _, obj in ipairs(allData) do
                if obj:IsValid() then
                    addName(obj.GamepadName)
                end
            end
        end
    end)

    pcall(function()
        local settings = FindAllOf("CommonInputPlatformSettings")
        if settings then
            for _, ps in ipairs(settings) do
                if ps:IsValid() then
                    local okD, defN = pcall(function() return safeStr(ps.DefaultGamepadName) end)
                    local nCls, nSoft = 0, 0
                    pcall(function() nCls = ps.ControllerDataClasses:GetArrayNum() end)
                    pcall(function() nSoft = ps.ControllerData:GetArrayNum() end)
                    log("CommonInputPlatformSettings(%s): DefaultGamepadName='%s', Classes=%d, SoftAssets=%d",
                        safeStr(ps:GetFName()), okD and defN or "?", nCls, nSoft)
                    pcall(function()
                        ps.ControllerDataClasses:ForEach(function(_, clsElem)
                            local cls = clsElem:get()
                            if cls and cls:IsValid() then
                                local cdo = cls:GetCDO()
                                if cdo and cdo:IsValid() then
                                    addName(cdo.GamepadName)
                                end
                            end
                        end)
                    end)
                end
            end
        end
    end)

    if #names > 0 then
        log("Registered CommonInput gamepads (%d): %s", #names, table.concat(names, ", "))
        local priorities = { "ps5", "dualsense", "sony", "playstation", "ps4", "dualshock" }
        for _, pat in ipairs(priorities) do
            for _, candidate in ipairs(names) do
                if candidate:lower():find(pat, 1, true) then
                    psGamepadNameStr = candidate
                    log("Selected PlayStation GamepadInputType: '%s'", psGamepadNameStr)
                    return
                end
            end
        end
    end
    log("Using GamepadInputType '%s' + NeroUI PrimeView EInputType::GamepadPS (2)", psGamepadNameStr)
end

local function enforcePlayStationPrompts()
    if not bridgeState.connected then return end
    if isApplyingPromptOverride then return end
    isApplyingPromptOverride = true

    discoverRegisteredGamepads()
    local psFName = getFName(psGamepadNameStr)

    -- 1. CommonInputSubsystem: set GamepadInputType to PS5/DualSense
    local cisInputIsGamepad = false
    pcall(function()
        local allCIS = FindAllOf("CommonInputSubsystem")
        if not allCIS then return end
        for _, cis in ipairs(allCIS) do
            if cis:IsValid() then
                local curType = tonumber(cis:GetCurrentInputType()) or 0
                if curType == 1 then
                    cisInputIsGamepad = true
                end
                local curPad = ""
                pcall(function() curPad = safeStr(cis:GetCurrentGamepadName():ToString()) end)
                if curPad == "" then
                    pcall(function() curPad = safeStr(cis.GamepadInputType:ToString()) end)
                end
                if psFName and curPad ~= psGamepadNameStr and not isPlayStationGamepadName(curPad) then
                    cis:SetGamepadInputType(psFName)
                    if curType == 1 then
                        pcall(function() cis:BroadcastInputMethodChanged() end)
                    end
                    if not loggedPromptOverride then
                        log("CommonInputSubsystem GamepadInputType changed '%s' -> '%s'", curPad, psGamepadNameStr)
                    end
                end
            end
        end
    end)

    -- 2. PrimeView (Noesis master UI view): force EInputType::GamepadPS (2) when gamepad is active or Xbox prompts shown
    local gamepadActive = cisInputIsGamepad or bridgeState.active
    pcall(function()
        local allPV = FindAllOf("PrimeView")
        if not allPV then return end
        for _, pv in ipairs(allPV) do
            if pv:IsValid() then
                local isPad = false
                pcall(function() isPad = (pv.bIsGamepad == true) end)
                local curInput = tonumber(pv.InputType) or 0
                -- Flip Xbox (1) -> PS (2) immediately; also flip if gamepad is active (isPad or recent controller activity)
                if curInput == 1 or ((isPad or gamepadActive) and curInput ~= 2) then
                    pv.bIsGamepad = true
                    pv.InputType = 2 -- EInputType::GamepadPS
                    noesisNotify(pv, "bIsGamepad")
                    noesisNotify(pv, "InputType")
                    if not loggedPromptOverride then
                        loggedPromptOverride = true
                        log("PrimeView UI prompt forced to PlayStation (InputType=%d -> 2)", curInput)
                    end
                end
            end
        end
    end)

    -- 3. PlayerHUDViewModel: flip PS5Controller / XboxController flags when gamepad is active
    pcall(function()
        local allHUD = FindAllOf("PlayerHUDViewModel")
        if not allHUD then return end
        for _, vm in ipairs(allHUD) do
            if vm:IsValid() then
                local isXB = (vm.XboxController == true)
                local isPS = (vm.PS5Controller == true)
                local isKB = (vm.Keyboard == true)
                if isXB or ((gamepadActive or not isKB) and not isPS) then
                    vm.XboxController = false
                    vm.PS5Controller = true
                    vm.Keyboard = false
                    noesisNotify(vm, "XboxController")
                    noesisNotify(vm, "PS5Controller")
                    noesisNotify(vm, "Keyboard")
                end
            end
        end
    end)

    -- 4. ControllerInputPresetScreenViewModel: always show PlayStation DualSense diagram in controller settings
    pcall(function()
        local allPreset = FindAllOf("ControllerInputPresetScreenViewModel")
        if not allPreset then return end
        for _, vm in ipairs(allPreset) do
            if vm:IsValid() and vm.bIsPSInput ~= true then
                vm.bIsPSInput = true
                noesisNotify(vm, "bIsPSInput")
            end
        end
    end)

    isApplyingPromptOverride = false
end

-- ============================================================
-- Adaptive Trigger Command Helpers + Ownership State Machine
-- ============================================================
local lastSentTrigCmd = { [0] = "", [1] = "" }
local nativeTriggersObserved = false
local nativeSideActive = { [0] = false, [1] = false }
local fallbackSideActive = { [0] = false, [1] = false }
local nativeTriggerLogCount = 0
local prevInMenuOrNonGameplay = false

local function sendTriggerSideCmd(side, cmd)
    if side == 2 then
        if lastSentTrigCmd[0] == cmd and lastSentTrigCmd[1] == cmd then return end
        lastSentTrigCmd[0] = cmd
        lastSentTrigCmd[1] = cmd
    else
        local s = clamp(side, 0, 1)
        if lastSentTrigCmd[s] == cmd then return end
        lastSentTrigCmd[s] = cmd
    end
    sendCmd(cmd)
end

local function markNativeSide(side, active)
    if side == 2 then
        nativeSideActive[0] = active
        nativeSideActive[1] = active
        if active then
            fallbackSideActive[0] = false
            fallbackSideActive[1] = false
        end
    else
        local s = clamp(side, 0, 1)
        nativeSideActive[s] = active
        if active then
            fallbackSideActive[s] = false
        end
    end
end

local function sendNativeTrigger(side, cmd)
    nativeTriggersObserved = true
    markNativeSide(side, true)
    sendTriggerSideCmd(side, cmd)
    if nativeTriggerLogCount < 12 then
        nativeTriggerLogCount = nativeTriggerLogCount + 1
        log("Native Trigger -> %s", cmd)
    end
end

local function clearNativeTrigger(side)
    nativeTriggersObserved = true
    local sd = clamp(side, 0, 2)
    markNativeSide(sd, false)
    sendTriggerSideCmd(sd, string.format("TRIG_OFF %d", sd))
end

local function sendFallbackTrigger(side, cmd)
    local s = clamp(side, 0, 1)
    if nativeSideActive[s] then return end
    fallbackSideActive[s] = true
    sendTriggerSideCmd(s, cmd)
end

local function releaseFallbackSide(side)
    local s = clamp(side, 0, 1)
    if fallbackSideActive[s] and not nativeSideActive[s] then
        fallbackSideActive[s] = false
        sendTriggerSideCmd(s, string.format("TRIG_OFF %d", s))
    end
end

local function releaseAllFallbackTriggers()
    releaseFallbackSide(0)
    releaseFallbackSide(1)
end

-- ============================================================
-- Lightbar Command Helpers + Contextual Fallback
-- ============================================================
local lastSentBaseLedCmd = ""
local lastBaseLedReason = ""
local nativeBaseLedOverride = false
local nativeSpecialLedActive = false
local loggedTriggerHookHit = false
local loggedLedHookHit = false
local loggedHapticHookHit = false

local function sendBaseLed(typeInt, r, g, b, dur, interval, reason)
    local cmd = string.format("LED_BASE %d %d %d %d %.2f %.2f",
        typeInt, safeRound(r, 0, 255), safeRound(g, 0, 255), safeRound(b, 0, 255),
        tonumber(dur) or 0.0, math.max(0.05, tonumber(interval) or 1.0))
    if cmd ~= lastSentBaseLedCmd then
        lastSentBaseLedCmd = cmd
        sendCmd(cmd)
        if reason and reason ~= lastBaseLedReason then
            lastBaseLedReason = reason
            log("Lightbar -> %s", reason)
        end
    end
end

-- ============================================================
-- Haptics Rate Limiter & Asset Name Classifier
-- ============================================================
local lastHapticBurstMs = {}

local function fireHapticBurst(preset, intensity, pan, minCooldownMs)
    if not bridgeState.connected then return end
    local t = nowMs()
    local cd = minCooldownMs or 40
    if lastHapticBurstMs[preset] and (t - lastHapticBurstMs[preset]) < cd then
        return
    end
    lastHapticBurstMs[preset] = t
    local inten = clamp(tonumber(intensity) or 0.6, 0.05, 1.0)
    local p = clamp(tonumber(pan) or 0.0, -1.0, 1.0)
    sendCmd(string.format("HAPTIC_BURST %s %.3f %.2f", preset, inten, p))
end

local function classifyAssetToPreset(assetStr)
    local s = (assetStr or ""):lower()
    if s:find("shotgun") or s:find("lupara") then return "SHOTGUN", 0.88 end
    if s:find("rifle") or s:find("carbine") or s:find("bolt") then return "RIFLE", 0.78 end
    if s:find("revolver") then return "REVOLVER", 0.75 end
    if s:find("pistol") or s:find("handgun") or s:find("gun") or s:find("fire") or s:find("shoot") then return "PISTOL", 0.70 end
    if s:find("reload") then return "RELOAD", 0.60 end
    if s:find("dry") or s:find("empty") then return "DRYFIRE", 0.55 end
    if s:find("knife") or s:find("melee") or s:find("punch") or s:find("stiletto") or s:find("slash") then return "MELEE", 0.72 end
    if s:find("horse") or s:find("gallop") or s:find("hoof") then return "HORSE_STEP", 0.50 end
    if s:find("gear") or s:find("shift") then return "CAR_SHIFT", 0.55 end
    if s:find("backfire") or s:find("exhaust") then return "CAR_BACKFIRE", 0.65 end
    if s:find("hit") or s:find("damage") or s:find("hurt") then return "HIT_REACT", 0.75 end
    if s:find("ui") or s:find("menu") or s:find("click") then return "UI_TICK", 0.45 end
    return "IMPACT", 0.55
end

-- ============================================================
-- Contextual Gameplay Fallback (Triggers + Lightbar)
-- ============================================================
local function findLiveInstance(className)
    local list = FindAllOf(className)
    if not list then return nil end
    for _, obj in ipairs(list) do
        if obj and obj:IsValid() then
            local ok, fn = pcall(function() return safeStr(obj:GetFullName()) end)
            if ok and fn ~= "" and not fn:find("Default__", 1, true) then
                return obj
            end
        end
    end
    return nil
end

local function readHudStateMask()
    local mask = 0
    local found = false
    pcall(function()
        local comp = findLiveInstance("HUDStateManagerComponent")
        if comp then
            local st = comp:GetCurrentHUDState()
            if st then
                mask = tonumber(st.HUDState) or 0
                found = true
            end
        end
    end)
    return found, mask
end

local function isCinematicPlaying()
    local playing = false
    pcall(function()
        local sub = findLiveInstance("H13CinematicSubsystem")
        if sub then
            local seq = sub.CurrentLevelSequence
            if seq and seq:IsValid() then
                playing = true
            end
        end
    end)
    return playing
end

local VEHICLE_R2_FIXED_CMD = "TRIG_FEEDBACK 1 2 2"
local VEHICLE_L2_FIXED_CMD = "TRIG_FEEDBACK 0 2 4"
local playerDrivingVehicle = false
local prevHudInVehicle = false
local activeVehicleRef = nil

local function applyFixedVehicleTriggers()
    if not bridgeState.connected then return end
    sendFallbackTrigger(1, VEHICLE_R2_FIXED_CMD)
    sendFallbackTrigger(0, VEHICLE_L2_FIXED_CMD)
end

local function setPlayerDrivingState(driving, vehObj, reason)
    if driving then
        activeVehicleRef = vehObj or activeVehicleRef
        if not playerDrivingVehicle then
            playerDrivingVehicle = true
            log("Vehicle trigger -> ON (%s: R2='%s', L2='%s')", reason or "enter", VEHICLE_R2_FIXED_CMD, VEHICLE_L2_FIXED_CMD)
        end
        if not prevInMenuOrNonGameplay then
            applyFixedVehicleTriggers()
        end
    else
        activeVehicleRef = nil
        if playerDrivingVehicle then
            playerDrivingVehicle = false
            releaseAllFallbackTriggers()
            log("Vehicle trigger -> OFF (%s)", reason or "exit")
        end
    end
end

local function checkAndApplyVehicleState(vehObj, reason)
    if not bridgeState.connected then return end
    pcall(function()
        if vehObj and vehObj:IsValid() and vehObj:IsPlayerDriving() == true then
            setPlayerDrivingState(true, vehObj, reason)
            return
        end
        if activeVehicleRef and activeVehicleRef:IsValid() and activeVehicleRef:IsPlayerDriving() == true then
            setPlayerDrivingState(true, activeVehicleRef, reason)
            return
        end
        local hudFound, hudMask = readHudStateMask()
        if hudFound and (hudMask & 1) ~= 0 then
            setPlayerDrivingState(true, nil, reason)
            return
        end
        setPlayerDrivingState(false, nil, reason)
    end)
end

local function refreshVehicleStateFromEvent(vehParam, reason)
    local unwrapped = nil
    if vehParam then
        local okG, obj = pcall(function() return vehParam:get() end)
        if okG and obj and obj:IsValid() then
            unwrapped = obj
        elseif vehParam.IsValid and vehParam:IsValid() then
            unwrapped = vehParam
        end
    end
    -- Check immediately and once more after 120ms in case UE updates HUD/seat state post-hook
    checkAndApplyVehicleState(unwrapped, reason)
    ExecuteWithDelay(120, function()
        ExecuteInGameThread(function()
            checkAndApplyVehicleState(nil, reason .. "+post")
        end)
    end)
end

local function updateGameplayTriggersAndLightbar()
    if not bridgeState.connected then return end
    local hudFound, hudMask = readHudStateMask()
    local hudInVehicle = hudFound and ((hudMask & 1) ~= 0)
    if hudInVehicle ~= prevHudInVehicle then
        prevHudInVehicle = hudInVehicle
        setPlayerDrivingState(hudInVehicle, nil, hudInVehicle and "HUDState:Vehicle=ON" or "HUDState:Vehicle=OFF")
    end
    -- EHUDState flags: Vehicle=1, Duel=2, Cinematic=4, Loading=8, Screen=16, Dialog=32, Photography=64, Fence=128
    local inMenuOrNonGameplay = (not hudFound)
        or ((hudMask & (4 | 8 | 16 | 32 | 64)) ~= 0)
        or isCinematicPlaying()

    -- Contextual Lightbar (unless Hangar 13's native H13HardwareLightEffectsSubsystem is overriding)
    if not nativeBaseLedOverride then
        if inMenuOrNonGameplay then
            if hudFound and (hudMask & 8) ~= 0 then
                -- Loading screen: solid warm Sicilian amber
                sendBaseLed(0, 180, 110, 25, 0.0, 1.0, "Loading (amber)")
            elseif (hudFound and (hudMask & 4) ~= 0) or isCinematicPlaying() then
                -- Cutscene: subtle dim midnight blue
                sendBaseLed(0, 8, 18, 45, 0.0, 1.0, "Cinematic (dim blue)")
            else
                -- Menu / idle: calm solid Mediterranean cobalt blue
                sendBaseLed(0, 18, 60, 155, 0.0, 1.0, "Menu/Idle (blue)")
            end
        else
            local burning, inGas = false, false
            local inCombat, isBandagePulsing = false, false
            local trespassState = 0

            pcall(function()
                local dmg = findLiveInstance("DamageFeedbackViewModel")
                if dmg then
                    burning = (dmg.bIsBurning == true)
                    inGas = (dmg.bIsInGas == true)
                end
            end)
            pcall(function()
                local hud = findLiveInstance("PlayerHUDViewModel")
                if hud then
                    inCombat = (hud.bInCombat == true)
                    isBandagePulsing = (hud.bIsBandagePulsing == true)
                end
            end)
            pcall(function()
                local tr = findLiveInstance("TrespassHUDViewModel")
                if tr then
                    trespassState = tonumber(tr:GetCurrentTrespassState()) or 0
                end
            end)

            if burning then
                sendBaseLed(0, 255, 85, 0, 0.0, 1.0, "Burning (orange)")
            elseif inGas then
                sendBaseLed(0, 55, 200, 35, 0.0, 1.0, "Gas (green)")
            elseif isBandagePulsing then
                sendBaseLed(2, 220, 18, 18, 0.0, 1.1, "Critical Health Bandage (red pulse)")
            elseif inCombat or trespassState == 2 then
                sendBaseLed(0, 195, 20, 20, 0.0, 1.0, "Combat/Hostile (crimson)")
            elseif trespassState == 1 then
                sendBaseLed(0, 215, 140, 18, 0.0, 1.0, "Trespassing (amber)")
            else
                sendBaseLed(0, 24, 72, 168, 0.0, 1.0, "Explore (Sicilian blue)")
            end
        end
    end

    -- Adaptive Triggers: respect native TriggerEffectBPLibrary ownership without ever cancelling native effects
    local enteringMenu = inMenuOrNonGameplay and (not prevInMenuOrNonGameplay)
    prevInMenuOrNonGameplay = inMenuOrNonGameplay

    if inMenuOrNonGameplay then
        releaseAllFallbackTriggers()
        if enteringMenu and hudFound and (nativeSideActive[0] or nativeSideActive[1]) then
            clearNativeTrigger(2)
        end
        return
    end

    -- Event-driven fixed vehicle triggers (managed by H13Vehicle & HUDStateManagerComponent hooks; no polling)
    if playerDrivingVehicle then
        applyFixedVehicleTriggers()
        return
    end

    -- Check horseback riding
    local ridingActive = false
    pcall(function()
        local rider = findLiveInstance("NeroHorseRiderComponent")
        if rider and rider.IsHorseMountedByPlayer == true then
            sendFallbackTrigger(1, "TRIG_SLOPE 1 2 8 1 4")
            sendFallbackTrigger(0, "TRIG_SLOPE 0 2 8 1 3")
            ridingActive = true
        end
    end)
    if ridingActive then return end

    -- Check aiming / weapon fallback ONLY if Hangar 13 native TriggerEffectBPLibrary has never fired
    local aimingActive = false
    if not nativeTriggersObserved then
        pcall(function()
            local aim = findLiveInstance("NeroPlayerAimComponent")
            if aim then
                local aiming = (aim:IsAiming() == true) or (aim:IsHipFireAiming() == true)
                if aiming then
                    local rStart, rEnd, rStr = 2, 6, 6
                    pcall(function()
                        local items = FindAllOf("BPI_Item_C")
                        if items then
                            for _, it in ipairs(items) do
                                if it:IsValid() and it.GetAdaptiveTriggerSettings then
                                    local okG, s, e, st = pcall(function() return it:GetAdaptiveTriggerSettings() end)
                                    if okG and tonumber(st) and tonumber(st) > 0 then
                                        rStart = safeRound(s, 0, 8)
                                        rEnd = safeRound(e, rStart + 1, 9)
                                        rStr = safeRound(st, 1, 8)
                                        break
                                    end
                                end
                            end
                        end
                    end)
                    sendFallbackTrigger(0, "TRIG_FEEDBACK 0 2 4")
                    sendFallbackTrigger(1, string.format("TRIG_WEAPON 1 %d %d %d", rStart, rEnd, rStr))
                    aimingActive = true
                end
            end
        end)
    end
    if aimingActive then return end

    -- Only release fallback-owned sides; never wipe native TriggerEffectBPLibrary states!
    releaseAllFallbackTriggers()
end

-- ============================================================
-- Hook Registration Helper (idempotent, pcall-safe)
-- ============================================================
local hookStats = { attached = 0, deferred = 0 }
local hookedMap = {}

local function safeHook(path, fn)
    if hookedMap[path] then return true end
    local ok, preId, postId = pcall(RegisterHook, path, fn)
    if ok then
        hookedMap[path] = { preId, postId }
        hookStats.attached = hookStats.attached + 1
        return true
    end
    return false
end

local function extractByteArray10(arrParam)
    local out = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
    pcall(function()
        local arr = arrParam:get()
        if not arr then return end
        local idx = 1
        local iterOk = pcall(function()
            arr:ForEach(function(_, elem)
                if idx <= 10 then
                    out[idx] = safeRound(elem:get(), 0, 8)
                    idx = idx + 1
                end
            end)
        end)
        if not iterOk or idx == 1 then
            for i = 1, 10 do
                local val = arr[i]
                if val == nil then break end
                out[i] = safeRound(val, 0, 8)
            end
        end
    end)
    return out
end

-- ============================================================
-- Core C++ Hooks (TriggerEffect, LightEffects, CommonInput, PlayerController, Vehicle, Horse)
-- ============================================================
local function registerCoreHooks()
    -- CommonInput hooks: intercept controller name/type swaps cleanly
    safeHook("/Script/CommonInput.CommonInputSubsystem:SetGamepadInputType", function(_, padParam)
        if not bridgeState.connected or isApplyingPromptOverride then return end
        pcall(function()
            local requested = safeStr(padParam:get():ToString())
            if not isPlayStationGamepadName(requested) then
                local psFN = getFName(psGamepadNameStr)
                if psFN then padParam:set(psFN) end
            end
        end)
    end)

    safeHook("/Script/CommonInput.CommonInputSubsystem:SetCurrentInputType", function()
        if bridgeState.connected then
            ExecuteWithDelay(50, function()
                ExecuteInGameThread(enforcePlayStationPrompts)
            end)
        end
    end)

    -- TriggerEffectBPLibrary: full native Sony DualSense trigger integration
    local function isDualSenseCtrlType(ctrlParam)
        if not ctrlParam then return true end
        local ct = 0
        local ok = pcall(function() ct = tonumber(ctrlParam:get()) or 0 end)
        if ok and ct == 1 then return false end -- ETriggerEffectControllerType::Sense (PSVR2) only
        return true
    end

    safeHook("/Script/TriggerEffect.TriggerEffectBPLibrary:TriggerEffectSetWeaponMode",
        function(_, _, startPos, endPos, strength, side, ctrlType)
            if not bridgeState.connected or not isDualSenseCtrlType(ctrlType) then return end
            pcall(function()
                local sd = safeRound(side:get(), 0, 2)
                local st = safeRound(strength:get(), 0, 8)
                if st <= 0 then
                    clearNativeTrigger(sd)
                else
                    sendNativeTrigger(sd, string.format("TRIG_WEAPON %d %d %d %d",
                        sd, safeRound(startPos:get(), 0, 9), safeRound(endPos:get(), 0, 9), st))
                end
                if not loggedTriggerHookHit then
                    loggedTriggerHookHit = true
                    log("Native TriggerEffectSetWeaponMode hook active!")
                end
            end)
        end)

    safeHook("/Script/TriggerEffect.TriggerEffectBPLibrary:TriggerEffectSetFeedbackMode",
        function(_, _, pos, strength, side, ctrlType)
            if not bridgeState.connected or not isDualSenseCtrlType(ctrlType) then return end
            pcall(function()
                local sd = safeRound(side:get(), 0, 2)
                local st = safeRound(strength:get(), 0, 8)
                if st <= 0 then
                    clearNativeTrigger(sd)
                else
                    sendNativeTrigger(sd, string.format("TRIG_FEEDBACK %d %d %d",
                        sd, safeRound(pos:get(), 0, 9), st))
                end
                if not loggedTriggerHookHit then
                    loggedTriggerHookHit = true
                    log("Native TriggerEffectSetFeedbackMode hook active!")
                end
            end)
        end)

    safeHook("/Script/TriggerEffect.TriggerEffectBPLibrary:TriggerEffectSetSlopeFeedbackMode",
        function(_, _, startPos, endPos, startStr, endStr, side, ctrlType)
            if not bridgeState.connected or not isDualSenseCtrlType(ctrlType) then return end
            pcall(function()
                local sd = safeRound(side:get(), 0, 2)
                local s0 = safeRound(startStr:get(), 0, 8)
                local s1 = safeRound(endStr:get(), 0, 8)
                if s0 <= 0 and s1 <= 0 then
                    clearNativeTrigger(sd)
                else
                    sendNativeTrigger(sd, string.format("TRIG_SLOPE %d %d %d %d %d",
                        sd, safeRound(startPos:get(), 0, 9), safeRound(endPos:get(), 0, 9), s0, s1))
                end
            end)
        end)

    safeHook("/Script/TriggerEffect.TriggerEffectBPLibrary:TriggerEffectSetVibrationMode",
        function(_, _, pos, freq, amp, side, ctrlType)
            if not bridgeState.connected or not isDualSenseCtrlType(ctrlType) then return end
            pcall(function()
                local sd = safeRound(side:get(), 0, 2)
                local a = safeRound(amp:get(), 0, 8)
                if a <= 0 then
                    clearNativeTrigger(sd)
                else
                    sendNativeTrigger(sd, string.format("TRIG_VIB %d %d %d %d",
                        sd, safeRound(pos:get(), 0, 9), safeRound(freq:get(), 0, 255), a))
                end
            end)
        end)

    safeHook("/Script/TriggerEffect.TriggerEffectBPLibrary:TriggerEffectSetMultipositionFeedbackMode",
        function(_, _, strengths, side, ctrlType)
            if not bridgeState.connected or not isDualSenseCtrlType(ctrlType) then return end
            pcall(function()
                local sd = safeRound(side:get(), 0, 2)
                local z = extractByteArray10(strengths)
                sendNativeTrigger(sd, string.format("TRIG_MULTI_FB %d %d %d %d %d %d %d %d %d %d %d",
                    sd, z[1], z[2], z[3], z[4], z[5], z[6], z[7], z[8], z[9], z[10]))
            end)
        end)

    safeHook("/Script/TriggerEffect.TriggerEffectBPLibrary:TriggerEffectSetMultipositionVibrationMode",
        function(_, _, freq, amplitudes, side, ctrlType)
            if not bridgeState.connected or not isDualSenseCtrlType(ctrlType) then return end
            pcall(function()
                local sd = safeRound(side:get(), 0, 2)
                local z = extractByteArray10(amplitudes)
                sendNativeTrigger(sd, string.format("TRIG_MULTI_VIB %d %d %d %d %d %d %d %d %d %d %d %d",
                    sd, safeRound(freq:get(), 0, 255), z[1], z[2], z[3], z[4], z[5], z[6], z[7], z[8], z[9], z[10]))
            end)
        end)

    safeHook("/Script/TriggerEffect.TriggerEffectBPLibrary:TriggerEffectSetOffMode",
        function(_, _, side, ctrlType)
            if not bridgeState.connected or not isDualSenseCtrlType(ctrlType) then return end
            pcall(function()
                local sd = safeRound(side:get(), 0, 2)
                clearNativeTrigger(sd)
            end)
        end)

    safeHook("/Script/TriggerEffect.TriggerEffectBPLibrary:TriggerEffectSetOffModeBothControllers",
        function()
            if not bridgeState.connected then return end
            clearNativeTrigger(2)
        end)

    -- Hangar 13 adaptive triggers component reset hook (ApplyWeaponTriggerEffect calls TriggerEffectSetWeaponMode internally)
    safeHook("/Script/NeroAdaptiveTriggersRuntime.NeroAdaptiveTriggersComponent:ResetTriggerEffect",
        function()
            if not bridgeState.connected then return end
            clearNativeTrigger(2)
        end)

    -- H13HardwareLightEffectsSubsystem: native game lightbar integration
    local function handleLightBehavior(cmdName, behaviorParam, isBase)
        if not bridgeState.connected then return end
        pcall(function()
            local b = behaviorParam:get()
            local lt = tonumber(b.LightType) or 0
            local col = b.LightColor
            local r = tonumber(col and col.R) or 0
            local g = tonumber(col and col.G) or 0
            local bl = tonumber(col and col.B) or 0
            local dur = tonumber(b.Duration) or 0.0
            local interval = math.max(0.05, tonumber(b.Interval) or 1.0)
            if isBase then
                nativeBaseLedOverride = true
                lastSentBaseLedCmd = ""
            else
                nativeSpecialLedActive = true
            end
            sendCmd(string.format("%s %d %d %d %d %.2f %.2f",
                cmdName, safeRound(lt, 0, 2), safeRound(r, 0, 255), safeRound(g, 0, 255), safeRound(bl, 0, 255), dur, interval))
            if not loggedLedHookHit then
                loggedLedHookHit = true
                log("Native H13HardwareLightEffects hook fired (%s R=%d G=%d B=%d)", cmdName, r, g, bl)
            end
        end)
    end

    safeHook("/Script/H13HardwareLightEffects.H13HardwareLightEffectsSubsystem:OverrideBaseEffect",
        function(_, behaviorParam) handleLightBehavior("LED_BASE", behaviorParam, true) end)

    safeHook("/Script/H13HardwareLightEffects.H13HardwareLightEffectsSubsystem:ApplySpecialEffect",
        function(_, behaviorParam) handleLightBehavior("LED_SPEC", behaviorParam, false) end)

    safeHook("/Script/H13HardwareLightEffects.H13HardwareLightEffectsSubsystem:StopBaseEffectOverride",
        function()
            nativeBaseLedOverride = false
            lastSentBaseLedCmd = ""
            if bridgeState.connected then sendCmd("LED_STOP_BASE") end
        end)

    safeHook("/Script/H13HardwareLightEffects.H13HardwareLightEffectsSubsystem:StopSpecialEffect",
        function()
            nativeSpecialLedActive = false
            if bridgeState.connected then sendCmd("LED_STOP_SPEC") end
        end)

    safeHook("/Script/Engine.PlayerController:SetControllerLightColor", function(_, colParam)
        if not bridgeState.connected then return end
        pcall(function()
            local col = colParam:get()
            local r = safeRound(col.R, 0, 255)
            local g = safeRound(col.G, 0, 255)
            local b = safeRound(col.B, 0, 255)
            nativeBaseLedOverride = true
            sendCmd(string.format("LED_COLOR %d %d %d", r, g, b))
        end)
    end)

    safeHook("/Script/Engine.PlayerController:ResetControllerLightColor", function()
        nativeBaseLedOverride = false
        lastSentBaseLedCmd = ""
        if bridgeState.connected then sendCmd("LED_STOP_BASE") end
    end)

    -- Engine Haptics & ForceFeedback -> WASAPI DualSense Haptic Voice Coils
    safeHook("/Script/Engine.PlayerController:SetHapticsByValue", function(_, freq, amp, hand)
        if not bridgeState.connected then return end
        pcall(function()
            sendCmd(string.format("HAPTIC_VALUE %.3f %.3f %d",
                clamp(tonumber(freq:get()) or 0, 0.0, 500.0),
                clamp(tonumber(amp:get()) or 0, 0.0, 1.0),
                safeRound(hand:get(), 0, 2)))
            if not loggedHapticHookHit then
                loggedHapticHookHit = true
                log("Native SetHapticsByValue hook active!")
            end
        end)
    end)

    safeHook("/Script/Engine.PlayerController:StopHapticEffect", function(_, hand)
        if not bridgeState.connected then return end
        pcall(function()
            sendCmd(string.format("HAPTIC_VALUE 0.000 0.000 %d", safeRound(hand:get(), 0, 2)))
        end)
    end)

    safeHook("/Script/Engine.PlayerController:PlayHapticEffect", function(_, effectParam, handParam, scaleParam)
        if not bridgeState.connected then return end
        pcall(function()
            local eff = effectParam:get()
            local name = (eff and eff:IsValid()) and eff:GetFullName() or ""
            local preset, baseI = classifyAssetToPreset(name)
            local sc = clamp(tonumber(scaleParam:get()) or 1.0, 0.1, 1.5)
            local h = tonumber(handParam:get()) or 2
            local pan = (h == 0) and -0.65 or ((h == 1) and 0.65 or 0.0)
            fireHapticBurst(preset, baseI * sc, pan, 35)
        end)
    end)

    safeHook("/Script/Engine.PlayerController:PlayDynamicForceFeedback",
        function(_, intenParam, _, bLL, _, bRL, _, actionParam)
            if not bridgeState.connected then return end
            pcall(function()
                local act = tonumber(actionParam:get()) or 0
                local inten = tonumber(intenParam:get()) or 0
                if act == 2 then -- EDynamicForceFeedbackAction::Stop
                    sendCmd("HAPTIC_VALUE 0.0 0.0 2")
                elseif inten >= 0.12 then
                    local pan = 0.0
                    if bLL:get() == true and bRL:get() ~= true then pan = -0.6 end
                    if bRL:get() == true and bLL:get() ~= true then pan = 0.6 end
                    fireHapticBurst("IMPACT", inten * 0.8, pan, 45)
                end
            end)
        end)

    safeHook("/Script/Engine.PlayerController:ClientPlayForceFeedback_Internal", function(_, effectParam)
        if not bridgeState.connected then return end
        pcall(function()
            local eff = effectParam:get()
            local name = (eff and eff:IsValid()) and eff:GetFullName() or ""
            local preset, baseI = classifyAssetToPreset(name)
            fireHapticBurst(preset, baseI, 0.0, 45)
        end)
    end)

    -- Vehicle C++ Haptic Hooks
    safeHook("/Script/H13Vehicles.H13Vehicle:OnCollatedImpact", function(vehSelf, velParam)
        if not bridgeState.connected then return end
        pcall(function()
            if vehSelf and vehSelf:IsValid() and vehSelf:IsPlayerDriving() == true then
                local vel = math.abs(tonumber(velParam:get()) or 0.0)
                local inten = clamp(vel / 1200.0, 0.25, 1.0)
                fireHapticBurst("IMPACT", inten, 0.0, 90)
            end
        end)
    end)

    safeHook("/Script/H13Vehicles.H13Vehicle:ReceiveGearShiftBegin", function(vehSelf)
        if not bridgeState.connected then return end
        pcall(function()
            if vehSelf and vehSelf:IsValid() and vehSelf:IsPlayerDriving() == true then
                fireHapticBurst("CAR_SHIFT", 0.52, 0.0, 120)
            end
        end)
    end)

    safeHook("/Script/H13Vehicles.H13Vehicle:ReceiveExhaustBackfire", function(vehSelf)
        if not bridgeState.connected then return end
        pcall(function()
            if vehSelf and vehSelf:IsValid() and vehSelf:IsPlayerDriving() == true then
                fireHapticBurst("CAR_BACKFIRE", 0.65, 0.0, 140)
            end
        end)
    end)

    -- Event-driven Vehicle Enter/Exit Trigger Hooks (zero polling loop)
    safeHook("/Script/H13Vehicles.H13Vehicle:OnDriverEntered", function(vehSelf)
        refreshVehicleStateFromEvent(vehSelf, "OnDriverEntered")
    end)

    safeHook("/Script/H13Vehicles.H13Vehicle:ReceiveCharacterEntered", function(vehSelf)
        refreshVehicleStateFromEvent(vehSelf, "ReceiveCharacterEntered")
    end)

    safeHook("/Script/H13Vehicles.H13Vehicle:ReceiveCharacterLeft", function(vehSelf)
        refreshVehicleStateFromEvent(vehSelf, "ReceiveCharacterLeft")
    end)

    safeHook("/Script/H13Vehicles.H13Vehicle:OnCharacterChangedSeat", function(vehSelf)
        refreshVehicleStateFromEvent(vehSelf, "OnCharacterChangedSeat")
    end)

    safeHook("/Script/NeroUI.HUDStateManagerComponent:UpdateCurrentHUDState", function()
        refreshVehicleStateFromEvent(nil, "UpdateCurrentHUDState")
    end)

    safeHook("/Script/NeroUI.HUDStateManagerComponent:AddOrRemoveState", function()
        refreshVehicleStateFromEvent(nil, "AddOrRemoveState")
    end)

    safeHook("/Script/NeroUI.HUDStateManagerComponent:AddOrRemoveAdditionalState", function()
        refreshVehicleStateFromEvent(nil, "AddOrRemoveAdditionalState")
    end)

    -- Horse C++ Haptic Hooks
    safeHook("/Script/Nero.NeroHorseRiderComponent:OnHorseSprint", function(comp)
        if not bridgeState.connected then return end
        pcall(function()
            if comp and comp:IsValid() and comp.IsHorseMountedByPlayer == true then
                fireHapticBurst("HORSE_BOOST", 0.60, 0.0, 200)
            end
        end)
    end)

    safeHook("/Script/Nero.NeroHorseRiderComponent:OnHorseBoost", function(comp)
        if not bridgeState.connected then return end
        pcall(function()
            if comp and comp:IsValid() and comp.IsHorseMountedByPlayer == true then
                fireHapticBurst("HORSE_BOOST", 0.72, 0.0, 150)
            end
        end)
    end)

    safeHook("/Script/Nero.NeroHorseRiderComponent:OnHorseLanded", function(comp)
        if not bridgeState.connected then return end
        pcall(function()
            if comp and comp:IsValid() and comp.IsHorseMountedByPlayer == true then
                fireHapticBurst("IMPACT", 0.68, 0.0, 120)
            end
        end)
    end)

    safeHook("/Script/Nero.NeroHorseEffectComponent:OnAnimalStep", function(_, footIdxParam)
        if not bridgeState.connected then return end
        pcall(function()
            local rider = FindFirstOf("NeroHorseRiderComponent")
            if rider and rider:IsValid() and rider.IsHorseMountedByPlayer == true then
                local idx = tonumber(footIdxParam:get()) or 0
                local pan = ((idx % 2) == 0) and -0.45 or 0.45
                fireHapticBurst("HORSE_STEP", 0.42, pan, 80)
            end
        end)
    end)
end

-- ============================================================
-- Deferred Blueprint Weapon/HUD Hooks (loaded after gameplay map opens)
-- ============================================================
local bpHooksRegistered = false
local bpWeaponTypePreset = "PISTOL"

local function tryRegisterGameplayBPHooks()
    if bpHooksRegistered then return true end
    local anyAttached = false

    if safeHook("/NeroWeapons/BPFL_Player_HUD.BPFL_Player_HUD_C:HUD_SetWeapon", function(_, weapIdParam)
        pcall(function()
            local wid = safeStr(weapIdParam:get():ToString())
            bpWeaponTypePreset = classifyAssetToPreset(wid)
        end)
    end) then anyAttached = true end

    if safeHook("/NeroWeapons/BPFL_Player_HUD.BPFL_Player_HUD_C:HUD_TriggerWeaponFired", function()
        if bridgeState.connected then
            fireHapticBurst(bpWeaponTypePreset, 0.78, 0.0, 35)
        end
    end) then anyAttached = true end

    if safeHook("/NeroWeapons/BPFL_Player_HUD.BPFL_Player_HUD_C:HUD_TriggerWeaponHit", function()
        if bridgeState.connected then
            fireHapticBurst("UI_TICK", 0.55, 0.0, 35)
        end
    end) then anyAttached = true end

    if safeHook("/NeroWeapons/BPFL_Player_HUD.BPFL_Player_HUD_C:HUD_TriggerHitReact", function(_, _, intenParam)
        if bridgeState.connected then
            local inten = 0.65
            pcall(function() inten = clamp(tonumber(intenParam:get()) or 0.65, 0.35, 1.0) end)
            fireHapticBurst("HIT_REACT", inten, 0.0, 60)
        end
    end) then anyAttached = true end

    if safeHook("/NeroWeapons/BPI_Weapon.BPI_Weapon_C:Fire", function(wSelf)
        if bridgeState.connected then
            local name = (wSelf and wSelf:IsValid()) and wSelf:GetFullName() or ""
            local preset, baseI = classifyAssetToPreset(name)
            fireHapticBurst(preset, baseI, 0.0, 35)
        end
    end) then anyAttached = true end

    if safeHook("/NeroWeapons/BPI_Weapon.BPI_Weapon_C:FireNoAmmo", function()
        if bridgeState.connected then
            fireHapticBurst("DRYFIRE", 0.60, 0.0, 80)
        end
    end) then anyAttached = true end

    if safeHook("/NeroWeapons/BPI_Weapon.BPI_Weapon_C:Reload", function()
        if bridgeState.connected then
            fireHapticBurst("RELOAD", 0.62, 0.0, 150)
        end
    end) then anyAttached = true end

    if safeHook("/NeroWeapons/BPI_Weapon.BPI_Weapon_C:OnMeleeHit", function()
        if bridgeState.connected then
            fireHapticBurst("MELEE", 0.78, 0.0, 60)
        end
    end) then anyAttached = true end

    if hookedMap["/NeroWeapons/BPFL_Player_HUD.BPFL_Player_HUD_C:HUD_TriggerWeaponFired"]
       and hookedMap["/NeroWeapons/BPI_Weapon.BPI_Weapon_C:Fire"] then
        bpHooksRegistered = true
        log("Deferred /NeroWeapons Blueprint hooks attached successfully!")
    end
    return anyAttached
end

-- ============================================================
-- Startup & Background Loop
-- ============================================================
log("Initializing Mafia: The Old Country DualSense Mod...")

-- Run registration inside game thread safely
ExecuteInGameThread(function()
    registerCoreHooks()
    tryRegisterGameplayBPHooks()

    -- Re-check prompts and deferred BP hooks whenever player spawns into a level
    safeHook("/Script/Engine.PlayerController:ClientRestart", function()
        ExecuteWithDelay(300, function()
            ExecuteInGameThread(function()
                tryRegisterGameplayBPHooks()
                refreshBridgeState(true)
                enforcePlayStationPrompts()
                playerDrivingVehicle = false
                prevHudInVehicle = false
                activeVehicleRef = nil
                refreshVehicleStateFromEvent(nil, "ClientRestart")
                updateGameplayTriggersAndLightbar()
            end)
        end)
    end)

    log("Core C++ hooks attached: %d", hookStats.attached)
end)

-- Asynchronous trigger to pump IPC & contextual feedback strictly on Game Thread (avoids concurrent pipe I/O)
local loopTick = 0
local tickInFlight = false
local startupBannerLogged = false
local loggedInitialPromptState = false
local loggedPostPromptVerify = false

LoopAsync(150, function()
    if tickInFlight then
        return false
    end
    tickInFlight = true
    ExecuteInGameThread(function()
        pcall(function()
            loopTick = loopTick + 1
            local forceQuery = (not bridgeState.reachable) or ((loopTick % 4) == 0)
            if forceQuery then
                local ok = refreshBridgeState(true)
                if ok and not startupBannerLogged then
                    startupBannerLogged = true
                    local pong = queryCmd("PING")
                    log("DualSenseBridge IPC ready (PING=%s, connected=%s, wasapi=%s)",
                        tostring(pong), tostring(bridgeState.connected), tostring(bridgeState.wasapi))
                end
            end

            if bridgeState.connected then
                if not loggedInitialPromptState then
                    pcall(function()
                        local cis = FindFirstOf("CommonInputSubsystem")
                        local pv = FindFirstOf("PrimeView")
                        local cisType = (cis and cis:IsValid()) and tonumber(cis:GetCurrentInputType()) or -1
                        local cisPad = (cis and cis:IsValid()) and safeStr(cis:GetCurrentGamepadName()) or "?"
                        local pvType = (pv and pv:IsValid()) and tonumber(pv.InputType) or -1
                        local pvPad = (pv and pv:IsValid()) and tostring(pv.bIsGamepad) or "?"
                        if cis or pv then
                            loggedInitialPromptState = true
                            log("Initial UI state: CIS(type=%d, pad='%s'), PrimeView(InputType=%d, bIsGamepad=%s)",
                                cisType, cisPad, pvType, pvPad)
                        end
                    end)
                end

                if (loopTick % 3) == 1 then
                    enforcePlayStationPrompts()
                end
                if not loggedPostPromptVerify and loopTick >= 22 then
                    pcall(function()
                        local cis = FindFirstOf("CommonInputSubsystem")
                        local pv = FindFirstOf("PrimeView")
                        local cisPad = (cis and cis:IsValid()) and safeStr(cis:GetCurrentGamepadName()) or "?"
                        local pvType = (pv and pv:IsValid()) and tonumber(pv.InputType) or -1
                        local pvPad = (pv and pv:IsValid()) and tostring(pv.bIsGamepad) or "?"
                        if cis or pv then
                            loggedPostPromptVerify = true
                            log("UI prompt verified (~3s): CIS(pad='%s'), PrimeView(InputType=%d, bIsGamepad=%s)",
                                cisPad, pvType, pvPad)
                        end
                    end)
                end
                if not bpHooksRegistered and (loopTick % 15) == 0 then
                    tryRegisterGameplayBPHooks()
                end
                updateGameplayTriggersAndLightbar()
            end
        end)
        tickInFlight = false
    end)

    return false -- keep running
end)
