#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>

#include "dualsense_hid.hpp"
#include "wasapi_haptics.hpp"
#include "pipe_server.hpp"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <atomic>

namespace ds {

static std::mutex g_logMutex;
static FILE* g_logFile = nullptr;
static std::string g_logPath;

void InitBridgeLog(HMODULE hSelf) {
    wchar_t modPath[MAX_PATH]{};
    GetModuleFileNameW(hSelf, modPath, MAX_PATH);
    std::wstring dir(modPath);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir = dir.substr(0, slash);

    // Prefer <Binaries\Win64>\ue4ss\DualSenseBridge.log if ue4ss folder exists, else alongside DLL
    std::wstring ue4ssDir = dir + L"\\ue4ss";
    DWORD attr = GetFileAttributesW(ue4ssDir.c_str());
    std::wstring logW = (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
        ? (ue4ssDir + L"\\DualSenseBridge.log")
        : (dir + L"\\DualSenseBridge.log");

    int needed = WideCharToMultiByte(CP_UTF8, 0, logW.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed > 0) {
        g_logPath.resize(needed - 1);
        WideCharToMultiByte(CP_UTF8, 0, logW.c_str(), -1, &g_logPath[0], needed, nullptr, nullptr);
    }
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_logFile = _wfopen(logW.c_str(), L"w");
}

void BridgeLog(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (!g_logFile) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::fprintf(g_logFile, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_logFile, fmt, args);
    va_end(args);
    std::fprintf(g_logFile, "\n");
    std::fflush(g_logFile);
}

void CloseBridgeLog() {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logFile) {
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
}

struct XInputCapabilitiesEx {
    XINPUT_CAPABILITIES Capabilities;
    WORD VendorId;
    WORD ProductId;
    WORD ProductVersion;
    WORD Unknown;
};

static HMODULE g_realXInput = nullptr;
static std::once_flag g_initOnce;
static std::atomic<bool> g_started{false};
static std::atomic<bool> g_shuttingDown{false};
static std::atomic<bool> g_enabled{true};

static DualSenseHidManager* g_hid = nullptr;
static WasapiHapticsEngine* g_haptics = nullptr;
static PipeServer* g_pipe = nullptr;

// Original System32 function pointers
using FnGetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using FnSetState = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
using FnGetCaps = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
using FnEnable = void(WINAPI*)(BOOL);
using FnGetBattery = DWORD(WINAPI*)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*);
using FnGetKeystroke = DWORD(WINAPI*)(DWORD, DWORD, PXINPUT_KEYSTROKE);
using FnGetAudioIds = DWORD(WINAPI*)(DWORD, LPWSTR, UINT*, LPWSTR, UINT*);
using FnOrd100 = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using FnOrd101 = DWORD(WINAPI*)(DWORD, DWORD, LPVOID);
using FnOrd102 = DWORD(WINAPI*)(DWORD);
using FnOrd103 = DWORD(WINAPI*)(DWORD);
using FnOrd104 = DWORD(WINAPI*)(DWORD, LPVOID);
using FnOrd108 = DWORD(WINAPI*)(DWORD, DWORD, DWORD, XInputCapabilitiesEx*);
using FnOrd109 = DWORD(WINAPI*)(DWORD, LPVOID);

static FnGetState pfnGetState = nullptr;
static FnSetState pfnSetState = nullptr;
static FnGetCaps pfnGetCaps = nullptr;
static FnEnable pfnEnable = nullptr;
static FnGetBattery pfnGetBattery = nullptr;
static FnGetKeystroke pfnGetKeystroke = nullptr;
static FnGetAudioIds pfnGetAudioIds = nullptr;
static FnOrd100 pfnOrd100 = nullptr;
static FnOrd101 pfnOrd101 = nullptr;
static FnOrd102 pfnOrd102 = nullptr;
static FnOrd103 pfnOrd103 = nullptr;
static FnOrd104 pfnOrd104 = nullptr;
static FnOrd108 pfnOrd108 = nullptr;
static FnOrd109 pfnOrd109 = nullptr;

static std::atomic<bool> g_loggedGetState{false};
static std::atomic<bool> g_loggedGetStateEx{false};
static std::atomic<bool> g_loggedCaps{false};
static std::atomic<bool> g_loggedCapsEx{false};
static std::atomic<bool> g_loggedSetState{false};

static void EnsureInitialized() {
    if (g_shuttingDown.load()) return;
    std::call_once(g_initOnce, []() {
        wchar_t sysDir[MAX_PATH]{};
        GetSystemDirectoryW(sysDir, MAX_PATH);
        std::wstring realPath = std::wstring(sysDir) + L"\\xinput1_4.dll";
        g_realXInput = LoadLibraryW(realPath.c_str());
        if (g_realXInput) {
            pfnGetState = reinterpret_cast<FnGetState>(GetProcAddress(g_realXInput, "XInputGetState"));
            pfnSetState = reinterpret_cast<FnSetState>(GetProcAddress(g_realXInput, "XInputSetState"));
            pfnGetCaps = reinterpret_cast<FnGetCaps>(GetProcAddress(g_realXInput, "XInputGetCapabilities"));
            pfnEnable = reinterpret_cast<FnEnable>(GetProcAddress(g_realXInput, "XInputEnable"));
            pfnGetBattery = reinterpret_cast<FnGetBattery>(GetProcAddress(g_realXInput, "XInputGetBatteryInformation"));
            pfnGetKeystroke = reinterpret_cast<FnGetKeystroke>(GetProcAddress(g_realXInput, "XInputGetKeystroke"));
            pfnGetAudioIds = reinterpret_cast<FnGetAudioIds>(GetProcAddress(g_realXInput, "XInputGetAudioDeviceIds"));
            pfnOrd100 = reinterpret_cast<FnOrd100>(GetProcAddress(g_realXInput, MAKEINTRESOURCEA(100)));
            pfnOrd101 = reinterpret_cast<FnOrd101>(GetProcAddress(g_realXInput, MAKEINTRESOURCEA(101)));
            pfnOrd102 = reinterpret_cast<FnOrd102>(GetProcAddress(g_realXInput, MAKEINTRESOURCEA(102)));
            pfnOrd103 = reinterpret_cast<FnOrd103>(GetProcAddress(g_realXInput, MAKEINTRESOURCEA(103)));
            pfnOrd104 = reinterpret_cast<FnOrd104>(GetProcAddress(g_realXInput, MAKEINTRESOURCEA(104)));
            pfnOrd108 = reinterpret_cast<FnOrd108>(GetProcAddress(g_realXInput, MAKEINTRESOURCEA(108)));
            pfnOrd109 = reinterpret_cast<FnOrd109>(GetProcAddress(g_realXInput, MAKEINTRESOURCEA(109)));
        }

        g_hid = new DualSenseHidManager();
        g_haptics = new WasapiHapticsEngine();
        g_pipe = new PipeServer(*g_hid, *g_haptics);
        g_hid->Start();
        g_haptics->Start();
        g_pipe->Start();
        g_started = true;
        BridgeLog("Bridge: Subsystems started (realXInput=%p)", g_realXInput);
    });
}

static void FillDualSenseXInputState(XINPUT_STATE* outState, bool includeGuide) {
    DualSenseInputState st = g_hid->GetInputState();
    outState->dwPacketNumber = st.packetNumber;
    if (!g_enabled.load()) {
        std::memset(&outState->Gamepad, 0, sizeof(outState->Gamepad));
        return;
    }
    outState->Gamepad.wButtons = st.buttons;
    if (includeGuide && st.guidePressed) {
        outState->Gamepad.wButtons |= 0x0400;
    }
    outState->Gamepad.bLeftTrigger = st.leftTrigger;
    outState->Gamepad.bRightTrigger = st.rightTrigger;
    outState->Gamepad.sThumbLX = st.thumbLX;
    outState->Gamepad.sThumbLY = st.thumbLY;
    outState->Gamepad.sThumbRX = st.thumbRX;
    outState->Gamepad.sThumbRY = st.thumbRY;
}

static void FillDualSenseCaps(XINPUT_CAPABILITIES* caps) {
    std::memset(caps, 0, sizeof(XINPUT_CAPABILITIES));
    caps->Type = XINPUT_DEVTYPE_GAMEPAD;
    caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    caps->Flags = 0;
    caps->Gamepad.wButtons = 0xF3FF;
    caps->Gamepad.bLeftTrigger = 0xFF;
    caps->Gamepad.bRightTrigger = 0xFF;
    caps->Gamepad.sThumbLX = -1;
    caps->Gamepad.sThumbLY = -1;
    caps->Gamepad.sThumbRX = -1;
    caps->Gamepad.sThumbRY = -1;
    caps->Vibration.wLeftMotorSpeed = 0xFFFF;
    caps->Vibration.wRightMotorSpeed = 0xFFFF;
}

} // namespace ds

extern "C" {

DWORD WINAPI XInputGetState(DWORD dwUserIndex, XINPUT_STATE* pState) WIN_NOEXCEPT {
    ds::EnsureInitialized();
    if (!pState) return ERROR_BAD_ARGUMENTS;
    if (!ds::g_loggedGetState.exchange(true)) {
        ds::BridgeLog("XInputGetState first call (slot=%lu, dsConnected=%d)",
                      dwUserIndex, (ds::g_hid && ds::g_hid->IsConnected()) ? 1 : 0);
    }
    if (dwUserIndex == 0 && ds::g_hid && ds::g_hid->IsConnected()) {
        ds::FillDualSenseXInputState(pState, false);
        return ERROR_SUCCESS;
    }
    return ds::pfnGetState ? ds::pfnGetState(dwUserIndex, pState) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION* pVibration) WIN_NOEXCEPT {
    ds::EnsureInitialized();
    if (!pVibration) return ERROR_BAD_ARGUMENTS;
    if (!ds::g_loggedSetState.exchange(true)) {
        ds::BridgeLog("XInputSetState first call (slot=%lu, L=%u R=%u)",
                      dwUserIndex, pVibration->wLeftMotorSpeed, pVibration->wRightMotorSpeed);
    }
    if (dwUserIndex == 0 && ds::g_hid && ds::g_hid->IsConnected()) {
        uint16_t l = ds::g_enabled.load() ? pVibration->wLeftMotorSpeed : 0;
        uint16_t r = ds::g_enabled.load() ? pVibration->wRightMotorSpeed : 0;
        ds::g_hid->SetXInputRumble(l, r);
        if (ds::g_haptics) ds::g_haptics->SetContinuousRumble(l, r);
        return ERROR_SUCCESS;
    }
    return ds::pfnSetState ? ds::pfnSetState(dwUserIndex, pVibration) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetCapabilities(DWORD dwUserIndex, DWORD dwFlags, XINPUT_CAPABILITIES* pCapabilities) WIN_NOEXCEPT {
    ds::EnsureInitialized();
    if (!pCapabilities) return ERROR_BAD_ARGUMENTS;
    if (!ds::g_loggedCaps.exchange(true)) {
        ds::BridgeLog("XInputGetCapabilities first call (slot=%lu, flags=0x%lX)", dwUserIndex, dwFlags);
    }
    if (dwUserIndex == 0 && ds::g_hid && ds::g_hid->IsConnected()) {
        ds::FillDualSenseCaps(pCapabilities);
        return ERROR_SUCCESS;
    }
    return ds::pfnGetCaps ? ds::pfnGetCaps(dwUserIndex, dwFlags, pCapabilities) : ERROR_DEVICE_NOT_CONNECTED;
}

#pragma warning(push)
#pragma warning(disable: 4995)
void WINAPI XInputEnable(BOOL enable) WIN_NOEXCEPT {
    ds::EnsureInitialized();
    ds::g_enabled = (enable != FALSE);
    if (!enable && ds::g_hid) {
        ds::g_hid->SetXInputRumble(0, 0);
        if (ds::g_haptics) ds::g_haptics->SetContinuousRumble(0, 0);
    }
    if (ds::pfnEnable) ds::pfnEnable(enable);
}
#pragma warning(pop)

DWORD WINAPI XInputGetBatteryInformation(DWORD dwUserIndex, BYTE devType, XINPUT_BATTERY_INFORMATION* pBatteryInformation) WIN_NOEXCEPT {
    ds::EnsureInitialized();
    if (!pBatteryInformation) return ERROR_BAD_ARGUMENTS;
    if (dwUserIndex == 0 && ds::g_hid && ds::g_hid->IsConnected()) {
        pBatteryInformation->BatteryType = BATTERY_TYPE_WIRED;
        pBatteryInformation->BatteryLevel = BATTERY_LEVEL_FULL;
        return ERROR_SUCCESS;
    }
    return ds::pfnGetBattery ? ds::pfnGetBattery(dwUserIndex, devType, pBatteryInformation) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetKeystroke(DWORD dwUserIndex, DWORD dwReserved, PXINPUT_KEYSTROKE pKeystroke) WIN_NOEXCEPT {
    ds::EnsureInitialized();
    return ds::pfnGetKeystroke ? ds::pfnGetKeystroke(dwUserIndex, dwReserved, pKeystroke) : ERROR_EMPTY;
}

DWORD WINAPI XInputGetAudioDeviceIds(DWORD dwUserIndex, LPWSTR pRenderDeviceId, UINT* pRenderCount,
                                     LPWSTR pCaptureDeviceId, UINT* pCaptureCount) WIN_NOEXCEPT {
    ds::EnsureInitialized();
    return ds::pfnGetAudioIds
        ? ds::pfnGetAudioIds(dwUserIndex, pRenderDeviceId, pRenderCount, pCaptureDeviceId, pCaptureCount)
        : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Ord100(DWORD dwUserIndex, XINPUT_STATE* pState) {
    ds::EnsureInitialized();
    if (!pState) return ERROR_BAD_ARGUMENTS;
    if (!ds::g_loggedGetStateEx.exchange(true)) {
        ds::BridgeLog("Ord100 (XInputGetStateEx) first call (slot=%lu, dsConnected=%d)",
                      dwUserIndex, (ds::g_hid && ds::g_hid->IsConnected()) ? 1 : 0);
    }
    if (dwUserIndex == 0 && ds::g_hid && ds::g_hid->IsConnected()) {
        ds::FillDualSenseXInputState(pState, true);
        return ERROR_SUCCESS;
    }
    return ds::pfnOrd100 ? ds::pfnOrd100(dwUserIndex, pState) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Ord101(DWORD a, DWORD b, LPVOID c) {
    ds::EnsureInitialized();
    return ds::pfnOrd101 ? ds::pfnOrd101(a, b, c) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Ord102(DWORD a) {
    ds::EnsureInitialized();
    return ds::pfnOrd102 ? ds::pfnOrd102(a) : ERROR_SUCCESS;
}

DWORD WINAPI Ord103(DWORD a) {
    ds::EnsureInitialized();
    return ds::pfnOrd103 ? ds::pfnOrd103(a) : ERROR_SUCCESS;
}

DWORD WINAPI Ord104(DWORD a, LPVOID b) {
    ds::EnsureInitialized();
    return ds::pfnOrd104 ? ds::pfnOrd104(a, b) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Ord108(DWORD unk1, DWORD dwUserIndex, DWORD dwFlags, ds::XInputCapabilitiesEx* pCapsEx) {
    ds::EnsureInitialized();
    if (!ds::g_loggedCapsEx.exchange(true)) {
        ds::BridgeLog("Ord108 (XInputGetCapabilitiesEx) first call (unk1=%lu slot=%lu flags=0x%lX)",
                      unk1, dwUserIndex, dwFlags);
    }
    if (unk1 == 1 && dwUserIndex == 0 && pCapsEx && ds::g_hid && ds::g_hid->IsConnected()) {
        ds::DualSenseInputState st = ds::g_hid->GetInputState();
        ds::FillDualSenseCaps(&pCapsEx->Capabilities);
        pCapsEx->VendorId = ds::SONY_VID;
        pCapsEx->ProductId = st.productId ? st.productId : ds::DUALSENSE_PID;
        pCapsEx->ProductVersion = st.versionNumber;
        pCapsEx->Unknown = 0;
        return ERROR_SUCCESS;
    }
    return ds::pfnOrd108 ? ds::pfnOrd108(unk1, dwUserIndex, dwFlags, pCapsEx) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Ord109(DWORD a, LPVOID b) {
    ds::EnsureInitialized();
    return ds::pfnOrd109 ? ds::pfnOrd109(a, b) : ERROR_DEVICE_NOT_CONNECTED;
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        ds::InitBridgeLog(hModule);
        ds::BridgeLog("Bridge: xinput1_4.dll attached to process PID=%lu", GetCurrentProcessId());
        // Spawn worker initialization asynchronously from Loader Lock, or initialize lazily on first XInput call;
        // starting a lightweight init thread is also safe if XInput isn't called immediately on startup.
        HANDLE hInit = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            ds::EnsureInitialized();
            return 0;
        }, nullptr, 0, nullptr);
        if (hInit) CloseHandle(hInit);
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        ds::g_shuttingDown = true;
        if (lpReserved == nullptr && ds::g_started.load()) {
            ds::BridgeLog("Bridge: DLL_PROCESS_DETACH clean shutdown...");
            if (ds::g_pipe) { ds::g_pipe->Stop(); delete ds::g_pipe; ds::g_pipe = nullptr; }
            if (ds::g_haptics) { ds::g_haptics->Stop(); delete ds::g_haptics; ds::g_haptics = nullptr; }
            if (ds::g_hid) { ds::g_hid->Stop(); delete ds::g_hid; ds::g_hid = nullptr; }
            if (ds::g_realXInput) { FreeLibrary(ds::g_realXInput); ds::g_realXInput = nullptr; }
            ds::BridgeLog("Bridge: Shutdown complete.");
            ds::CloseBridgeLog();
        } else if (ds::g_hid) {
            // Process terminating via ExitProcess: send lock-free HID neutral reset without joining threads
            ds::g_hid->ForceNeutralOnProcessExit();
            ds::BridgeLog("Bridge: Process exit neutral reset sent.");
            ds::CloseBridgeLog();
        }
    }
    return TRUE;
}
