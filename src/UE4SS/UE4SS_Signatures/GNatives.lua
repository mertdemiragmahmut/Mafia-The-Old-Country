function Register()
    return "48 8B 4A 18 48 8B 42 20 48 8D 50 01 48 89 56 20 0F B6 00 4C 8D 3D ?? ?? ?? ?? 48 89 F2 45 31 C0 41 FF 14 C7"
end

function OnMatchFound(MatchAddress)
    local LeaInstr = MatchAddress + 19
    local NextInstr = LeaInstr + 7
    local Disp = DerefToInt32(LeaInstr + 3)
    if Disp == nil then return 0 end
    return NextInstr + Disp
end
