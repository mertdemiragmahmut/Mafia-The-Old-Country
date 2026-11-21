function Register()
    return "80 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 48 8D 3D ?? ?? ?? ?? 48 89 F9 FF 15 ?? ?? ?? ?? C7 44 24 6C 00 00 00 00"
end

function OnMatchFound(MatchAddress)
    local LeaInstr = MatchAddress + 13
    local NextInstr = LeaInstr + 7
    local Disp = DerefToInt32(LeaInstr + 3)
    if Disp == nil then return 0 end
    return NextInstr + Disp
end
