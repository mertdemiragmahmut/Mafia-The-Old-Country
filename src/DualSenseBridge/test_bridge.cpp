#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xinput.h>
#include <cstdio>
#include <cstring>
#include <string>

#pragma pack(push, 1)
struct XInputCapabilitiesEx {
    XINPUT_CAPABILITIES Capabilities;
    WORD VendorId;
    WORD ProductId;
    WORD ProductVersion;
    WORD Unknown;
};
#pragma pack(pop)

using FnGetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using FnSetState = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
using FnGetCaps = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
using FnEnable = void(WINAPI*)(BOOL);
using FnGetBattery = DWORD(WINAPI*)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*);
using FnOrd100 = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using FnOrd108 = DWORD(WINAPI*)(DWORD, DWORD, DWORD, XInputCapabilitiesEx*);

static int g_failures = 0;

static void Check(bool cond, const char* msg) {
    if (cond) {
        std::printf("[PASS] %s\n", msg);
    } else {
        std::printf("[FAIL] %s\n", msg);
        ++g_failures;
    }
}

static bool PipeTransactWin32(HANDLE hPipe, const std::string& cmd, std::string* outReply) {
    DWORD written = 0;
    if (!WriteFile(hPipe, cmd.data(), static_cast<DWORD>(cmd.size()), &written, nullptr) || written != cmd.size()) {
        return false;
    }
    if (!outReply) return true;

    char buf[512]{};
    DWORD readBytes = 0;
    if (!ReadFile(hPipe, buf, sizeof(buf) - 1, &readBytes, nullptr) || readBytes == 0) {
        return false;
    }
    buf[readBytes] = '\0';
    *outReply = buf;
    while (!outReply->empty() && (outReply->back() == '\n' || outReply->back() == '\r')) {
        outReply->pop_back();
    }
    return true;
}

int main(int argc, char** argv) {
    std::string dllPath;
    if (argc >= 2 && argv[1] && argv[1][0]) {
        dllPath = argv[1];
    } else {
        char exePath[MAX_PATH]{};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string dir(exePath);
        size_t pos = dir.find_last_of("\\/");
        if (pos != std::string::npos) dir = dir.substr(0, pos);
        dllPath = dir + "\\..\\dist\\xinput1_4.dll";
    }

    std::printf("Loading bridge DLL: %s\n", dllPath.c_str());
    HMODULE hMod = LoadLibraryA(dllPath.c_str());
    Check(hMod != nullptr, "LoadLibraryA(xinput1_4.dll)");
    if (!hMod) {
        std::printf("GetLastError=%lu\n", GetLastError());
        return 1;
    }

    auto pGetState = reinterpret_cast<FnGetState>(GetProcAddress(hMod, "XInputGetState"));
    auto pSetState = reinterpret_cast<FnSetState>(GetProcAddress(hMod, "XInputSetState"));
    auto pGetCaps = reinterpret_cast<FnGetCaps>(GetProcAddress(hMod, "XInputGetCapabilities"));
    auto pEnable = reinterpret_cast<FnEnable>(GetProcAddress(hMod, "XInputEnable"));
    auto pGetBattery = reinterpret_cast<FnGetBattery>(GetProcAddress(hMod, "XInputGetBatteryInformation"));
    auto pOrd100 = reinterpret_cast<FnOrd100>(GetProcAddress(hMod, reinterpret_cast<LPCSTR>(100)));
    auto pOrd108 = reinterpret_cast<FnOrd108>(GetProcAddress(hMod, reinterpret_cast<LPCSTR>(108)));

    Check(pGetState && pSetState && pGetCaps && pEnable && pGetBattery && pOrd100 && pOrd108,
          "Resolved all XInput named + ordinal (100, 108) exports");

    if (pEnable) pEnable(TRUE);

    // Allow HID/WASAPI worker threads up to 2.5s to bring DualSense online
    XINPUT_STATE st{};
    DWORD gsRes = ERROR_DEVICE_NOT_CONNECTED;
    for (int i = 0; i < 25; ++i) {
        gsRes = pGetState ? pGetState(0, &st) : ERROR_INVALID_FUNCTION;
        if (gsRes == ERROR_SUCCESS) break;
        Sleep(100);
    }
    Check(gsRes == ERROR_SUCCESS, "XInputGetState(0) returns ERROR_SUCCESS");
    std::printf("  dwPacketNumber=%lu wButtons=0x%04X LX=%d LY=%d\n",
                st.dwPacketNumber, st.Gamepad.wButtons, st.Gamepad.sThumbLX, st.Gamepad.sThumbLY);

    XINPUT_STATE stEx{};
    DWORD ord100Res = pOrd100 ? pOrd100(0, &stEx) : ERROR_INVALID_FUNCTION;
    Check(ord100Res == ERROR_SUCCESS, "Ord100 (XInputGetStateEx) returns ERROR_SUCCESS");

    XINPUT_CAPABILITIES caps{};
    DWORD capsRes = pGetCaps ? pGetCaps(0, XINPUT_FLAG_GAMEPAD, &caps) : ERROR_INVALID_FUNCTION;
    Check(capsRes == ERROR_SUCCESS && caps.SubType == XINPUT_DEVSUBTYPE_GAMEPAD,
          "XInputGetCapabilities(0) returns ERROR_SUCCESS gamepad");

    XInputCapabilitiesEx capsEx{};
    DWORD ord108Res = pOrd108 ? pOrd108(1, 0, 0, &capsEx) : ERROR_INVALID_FUNCTION;
    Check(ord108Res == ERROR_SUCCESS && capsEx.VendorId == 0x054C && capsEx.ProductId == 0x0CE6,
          "Ord108 (XInputGetCapabilitiesEx) reports Sony VID=0x054C PID=0x0CE6");
    std::printf("  CapsEx VID=0x%04X PID=0x%04X Ver=0x%04X\n",
                capsEx.VendorId, capsEx.ProductId, capsEx.ProductVersion);

    XINPUT_BATTERY_INFORMATION bat{};
    DWORD batRes = pGetBattery ? pGetBattery(0, BATTERY_DEVTYPE_GAMEPAD, &bat) : ERROR_INVALID_FUNCTION;
    Check(batRes == ERROR_SUCCESS, "XInputGetBatteryInformation(0) returns ERROR_SUCCESS");

    XINPUT_VIBRATION vibOn{16000, 16000};
    XINPUT_VIBRATION vibOff{0, 0};
    Check(pSetState && pSetState(0, &vibOn) == ERROR_SUCCESS, "XInputSetState(0) rumble pulse");
    Sleep(120);
    Check(pSetState && pSetState(0, &vibOff) == ERROR_SUCCESS, "XInputSetState(0) rumble stop");

    // Test Win32 Named Pipe client
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 20; ++i) {
        hPipe = CreateFileW(L"\\\\.\\pipe\\MafiaTOCDualSenseBridge",
                            GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hPipe != INVALID_HANDLE_VALUE) break;
        Sleep(100);
    }
    Check(hPipe != INVALID_HANDLE_VALUE, "CreateFileW(\\\\.\\pipe\\MafiaTOCDualSenseBridge)");

    if (hPipe != INVALID_HANDLE_VALUE) {
        std::string reply;
        Check(PipeTransactWin32(hPipe, "PING\n", &reply) && reply == "PONG",
              "Pipe PING -> PONG");

        reply.clear();
        bool gotState = PipeTransactWin32(hPipe, "STATE\n", &reply);
        std::printf("  STATE reply: %s\n", reply.c_str());
        Check(gotState && reply.find("CONNECTED=1") != std::string::npos,
              "Pipe STATE reports CONNECTED=1");
        Check(gotState && reply.find("WASAPI=1") != std::string::npos,
              "Pipe STATE reports WASAPI=1");

        // Trigger modes exercise + verify reset
        Check(PipeTransactWin32(hPipe, "TRIG_WEAPON 1 2 6 8\n", nullptr), "Pipe TRIG_WEAPON R2");
        Sleep(80);
        Check(PipeTransactWin32(hPipe, "TRIG_FEEDBACK 0 3 6\n", nullptr), "Pipe TRIG_FEEDBACK L2");
        Sleep(80);
        Check(PipeTransactWin32(hPipe, "TRIG_SLOPE 2 1 8 2 7\n", nullptr), "Pipe TRIG_SLOPE Both");
        Sleep(80);
        Check(PipeTransactWin32(hPipe, "TRIG_VIB 1 2 25 6\n", nullptr), "Pipe TRIG_VIB R2");
        Sleep(80);
        Check(PipeTransactWin32(hPipe, "TRIG_MULTI_FB 0 1 2 3 4 5 6 7 8 7 6\n", nullptr), "Pipe TRIG_MULTI_FB L2");
        Sleep(80);
        Check(PipeTransactWin32(hPipe, "TRIG_MULTI_VIB 1 28 2 3 4 5 6 7 6 5 4 3\n", nullptr), "Pipe TRIG_MULTI_VIB R2");
        Sleep(80);
        Check(PipeTransactWin32(hPipe, "TRIG_OFF 2\n", nullptr), "Pipe TRIG_OFF Both");
        Sleep(50);

        reply.clear();
        PipeTransactWin32(hPipe, "STATE\n", &reply);
        Check(reply.find("L2=0 R2=0") != std::string::npos, "Pipe STATE confirms L2=0 R2=0 after TRIG_OFF");

        // Isolation test: verify LED & rumble updates do not clobber active R2 trigger state
        Check(PipeTransactWin32(hPipe, "TRIG_WEAPON 1 2 6 8\n", nullptr), "Pipe TRIG_WEAPON R2 isolation setup");
        Sleep(40);
        Check(PipeTransactWin32(hPipe, "LED_COLOR 24 72 168\n", nullptr), "Pipe LED_COLOR while R2 active");
        if (pSetState) {
            XINPUT_VIBRATION vibIso{8000, 8000};
            pSetState(0, &vibIso);
            Sleep(40);
            pSetState(0, &vibOff);
        }
        Sleep(40);
        reply.clear();
        PipeTransactWin32(hPipe, "STATE\n", &reply);
        Check(reply.find("L2=0 R2=2") != std::string::npos, "Pipe STATE preserves R2=2 across LED/rumble updates");
        Check(PipeTransactWin32(hPipe, "TRIG_OFF 2\n", nullptr), "Pipe TRIG_OFF isolation cleanup");
        Sleep(40);

        // LED effects exercise
        Check(PipeTransactWin32(hPipe, "LED_BASE 0 255 64 0 0 1\n", nullptr), "Pipe LED_BASE");
        Sleep(80);
        Check(PipeTransactWin32(hPipe, "LED_SPEC 1 255 0 0 0.4 0.12\n", nullptr), "Pipe LED_SPEC");
        Sleep(120);
        Check(PipeTransactWin32(hPipe, "LED_STOP_SPEC\n", nullptr), "Pipe LED_STOP_SPEC");
        Check(PipeTransactWin32(hPipe, "LED_STOP_BASE\n", nullptr), "Pipe LED_STOP_BASE");
        Check(PipeTransactWin32(hPipe, "LED_COLOR 0 85 200\n", nullptr), "Pipe LED_COLOR");
        Check(PipeTransactWin32(hPipe, "LED_PLAYER 4\n", nullptr), "Pipe LED_PLAYER");

        // WASAPI Haptics + extra rumble exercise
        Check(PipeTransactWin32(hPipe, "HAPTIC_BURST PISTOL 0.65 0.0\n", nullptr), "Pipe HAPTIC_BURST PISTOL");
        Sleep(80);
        Check(PipeTransactWin32(hPipe, "HAPTIC_VALUE 140.0 0.35 2\n", nullptr), "Pipe HAPTIC_VALUE on");
        Sleep(80);
        Check(PipeTransactWin32(hPipe, "HAPTIC_VALUE 0.0 0.0 2\n", nullptr), "Pipe HAPTIC_VALUE off");
        Check(PipeTransactWin32(hPipe, "RUMBLE_ADD 90 90 100\n", nullptr), "Pipe RUMBLE_ADD");
        Sleep(120);

        CloseHandle(hPipe);
    }

    // Test MSVC CRT fopen("r+b") compatibility (mirrors Lua io.open)
    FILE* fp = std::fopen("\\\\.\\pipe\\MafiaTOCDualSenseBridge", "r+b");
    Check(fp != nullptr, "CRT fopen(\"r+b\") opens named pipe (Lua io.open compatibility)");
    if (fp) {
        std::setvbuf(fp, nullptr, _IONBF, 0);
        std::fputs("PING\n", fp);
        std::fflush(fp);
        std::fseek(fp, 0, SEEK_CUR);
        char lineBuf[128]{};
        char* got = std::fgets(lineBuf, sizeof(lineBuf), fp);
        bool crtPong = (got && std::strncmp(got, "PONG", 4) == 0);
        Check(crtPong, "CRT fopen(\"r+b\") PING -> PONG roundtrip");

        // Also test write-only CRT "r+b" command batch without reading
        std::fseek(fp, 0, SEEK_CUR);
        std::fputs("LED_COLOR 0 85 200\nTRIG_OFF 2\n", fp);
        std::fflush(fp);
        std::fclose(fp);
    }

    BOOL freed = FreeLibrary(hMod);
    Check(freed != FALSE, "FreeLibrary(xinput1_4.dll) clean unload");

    std::printf("\nSummary: %s (failures=%d)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
