function Register()
    return "44 89 E8 C1 E8 10 41 0F B7 CD 48 8B 15 ?? ?? ?? ?? 4C 8D 34 89 49 C1 E6 02 4C 03 34 C2 C1 E5 17 4D 8B 26 4D 85 E4"
end

function OnMatchFound(MatchAddress)
    local MovInstr = MatchAddress + 10
    local NextInstr = MovInstr + 7
    local Disp = DerefToInt32(MovInstr + 3)
    if Disp == nil then return 0 end
    -- Mov loads FUObjectArray.ObjObjects.Objects (+0x10 from GUObjectArray)
    return NextInstr + Disp - 0x10
end
