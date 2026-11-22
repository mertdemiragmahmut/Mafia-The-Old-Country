#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>

#include "dualsense_hid.hpp"
#include <string>
#include <algorithm>
#include <cmath>

// Link hints (also passed in build.bat)
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

namespace ds {

// Logging helper declared in dllmain.cpp
void BridgeLog(const char* fmt, ...);

static uint64_t NowMs() {
    return GetTickCount64();
}

static uint16_t TranslateButtons(const uint8_t* r, bool& outGuide, bool& outTouchpad) {
    uint16_t btns = 0;
    uint8_t b8 = r[8];
    uint8_t b9 = r[9];
    uint8_t b10 = r[10];

    // D-pad hat in low nibble of byte 8
    uint8_t dpad = b8 & 0x0F;
    switch (dpad) {
    case 0: btns |= 0x0001; break;                   // Up
    case 1: btns |= 0x0001 | 0x0008; break;          // Up + Right
    case 2: btns |= 0x0008; break;                   // Right
    case 3: btns |= 0x0002 | 0x0008; break;          // Down + Right
    case 4: btns |= 0x0002; break;                   // Down
    case 5: btns |= 0x0002 | 0x0004; break;          // Down + Left
    case 6: btns |= 0x0004; break;                   // Left
    case 7: btns |= 0x0001 | 0x0004; break;          // Up + Left
    default: break;
    }

    // Face buttons (PS -> XInput bit positions)
    if (b8 & 0x10) btns |= 0x4000; // Square -> X
    if (b8 & 0x20) btns |= 0x1000; // Cross -> A
    if (b8 & 0x40) btns |= 0x2000; // Circle -> B
    if (b8 & 0x80) btns |= 0x8000; // Triangle -> Y

    // Shoulders / Stick clicks / Create / Options
    if (b9 & 0x01) btns |= 0x0100; // L1 -> LB
    if (b9 & 0x02) btns |= 0x0200; // R1 -> RB
    if (b9 & 0x10) btns |= 0x0020; // Create -> BACK
    if (b9 & 0x20) btns |= 0x0010; // Options -> START
    if (b9 & 0x40) btns |= 0x0040; // L3 -> LEFT_THUMB
    if (b9 & 0x80) btns |= 0x0080; // R3 -> RIGHT_THUMB

    outGuide = (b10 & 0x01) != 0;
    outTouchpad = (b10 & 0x02) != 0;
    if (outTouchpad) {
        btns |= 0x0020; // Touchpad click also triggers Map/Journal (BACK)
    }
    return btns;
}

DualSenseHidManager::~DualSenseHidManager() {
    Stop();
}

void DualSenseHidManager::Start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    workerThread_ = std::thread(&DualSenseHidManager::WorkerLoop, this);
}

void DualSenseHidManager::Stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

bool DualSenseHidManager::IsConnected() const {
    std::lock_guard<std::mutex> lk(stateMutex_);
    return inputState_.connected;
}

bool DualSenseHidManager::WasActiveRecently(uint32_t withinMs) const {
    std::lock_guard<std::mutex> lk(stateMutex_);
    if (!inputState_.connected) return false;
    uint64_t now = NowMs();
    return (now - lastActivityMs_) <= withinMs;
}

DualSenseInputState DualSenseHidManager::GetInputState() const {
    std::lock_guard<std::mutex> lk(stateMutex_);
    return inputState_;
}

void DualSenseHidManager::SetXInputRumble(uint16_t leftMotor, uint16_t rightMotor) {
    std::lock_guard<std::mutex> lk(outMutex_);
    if (xinputLeft_ != leftMotor || xinputRight_ != rightMotor) {
        xinputLeft_ = leftMotor;
        xinputRight_ = rightMotor;
        outputDirty_ = true;
    }
}

void DualSenseHidManager::AddExtraRumble(uint8_t left, uint8_t right, uint32_t durationMs) {
    std::lock_guard<std::mutex> lk(outMutex_);
    extraRumbleLeft_ = left;
    extraRumbleRight_ = right;
    extraRumbleUntilMs_ = NowMs() + durationMs;
    outputDirty_ = true;
}

void DualSenseHidManager::SetTrigger(uint8_t side, const TriggerConfig& cfg) {
    uint8_t gate = ComputeWeaponGateThreshold(cfg);
    std::lock_guard<std::mutex> lk(outMutex_);
    if (side == 0 || side == 2) {
        triggerL2_ = cfg;
        uint8_t prev = weaponGateL2_.exchange(gate, std::memory_order_relaxed);
        if (prev != gate) {
            BridgeLog("HID: L2 weapon trigger gate -> %u (mode=%u p0=%u p1=%u p2=%u)",
                      gate, static_cast<unsigned>(cfg.mode), cfg.p0, cfg.p1, cfg.p2);
        }
    }
    if (side == 1 || side == 2) {
        triggerR2_ = cfg;
        uint8_t prev = weaponGateR2_.exchange(gate, std::memory_order_relaxed);
        if (prev != gate) {
            BridgeLog("HID: R2 weapon trigger gate -> %u (mode=%u p0=%u p1=%u p2=%u)",
                      gate, static_cast<unsigned>(cfg.mode), cfg.p0, cfg.p1, cfg.p2);
        }
    }
    outputDirty_ = true;
}

TriggerConfig DualSenseHidManager::GetTrigger(uint8_t side) const {
    std::lock_guard<std::mutex> lk(outMutex_);
    return (side == 0) ? triggerL2_ : triggerR2_;
}

void DualSenseHidManager::SetDefaultColor(uint8_t r, uint8_t g, uint8_t b) {
    std::lock_guard<std::mutex> lk(outMutex_);
    defaultLed_.type = LedEffectType::Default;
    defaultLed_.r = r;
    defaultLed_.g = g;
    defaultLed_.b = b;
    outputDirty_ = true;
}

void DualSenseHidManager::OverrideBaseLed(const LedBehavior& b) {
    std::lock_guard<std::mutex> lk(outMutex_);
    hasBaseOverride_ = true;
    baseOverride_ = b;
    baseStartMs_ = NowMs();
    outputDirty_ = true;
}

void DualSenseHidManager::StopBaseLedOverride() {
    std::lock_guard<std::mutex> lk(outMutex_);
    hasBaseOverride_ = false;
    outputDirty_ = true;
}

void DualSenseHidManager::ApplySpecialLed(const LedBehavior& b) {
    std::lock_guard<std::mutex> lk(outMutex_);
    hasSpecialLed_ = true;
    specialLed_ = b;
    specialStartMs_ = NowMs();
    outputDirty_ = true;
}

void DualSenseHidManager::StopSpecialLed() {
    std::lock_guard<std::mutex> lk(outMutex_);
    hasSpecialLed_ = false;
    outputDirty_ = true;
}

void DualSenseHidManager::SetPlayerLeds(uint8_t mask) {
    std::lock_guard<std::mutex> lk(outMutex_);
    playerLeds_ = mask & 0x1F;
    outputDirty_ = true;
}

static void EvalLedBehavior(const LedBehavior& b, uint64_t elapsedMs, uint8_t& r, uint8_t& g, uint8_t& bl) {
    if (b.type == LedEffectType::Default || b.interval <= 0.01f) {
        r = b.r; g = b.g; bl = b.b;
        return;
    }
    float tSec = static_cast<float>(elapsedMs) / 1000.0f;
    float phase = std::fmod(tSec, b.interval) / b.interval;
    float mult = 1.0f;
    if (b.type == LedEffectType::Flash) {
        mult = (phase < 0.5f) ? 1.0f : 0.08f;
    } else if (b.type == LedEffectType::Pulse) {
        mult = 0.5f + 0.5f * std::sin(phase * 6.2831853f - 1.5707963f);
        mult = 0.12f + 0.88f * mult;
    }
    r = static_cast<uint8_t>(std::lround(b.r * mult));
    g = static_cast<uint8_t>(std::lround(b.g * mult));
    bl = static_cast<uint8_t>(std::lround(b.b * mult));
}

void DualSenseHidManager::ComputeCurrentLed(uint64_t nowMs, uint8_t& outR, uint8_t& outG, uint8_t& outB) {
    if (hasSpecialLed_) {
        uint64_t elapsed = nowMs - specialStartMs_;
        if (specialLed_.duration > 0.0f && static_cast<float>(elapsed) / 1000.0f >= specialLed_.duration) {
            hasSpecialLed_ = false;
        } else {
            EvalLedBehavior(specialLed_, elapsed, outR, outG, outB);
            return;
        }
    }
    if (hasBaseOverride_) {
        uint64_t elapsed = nowMs - baseStartMs_;
        if (baseOverride_.duration > 0.0f && static_cast<float>(elapsed) / 1000.0f >= baseOverride_.duration) {
            hasBaseOverride_ = false;
        } else {
            EvalLedBehavior(baseOverride_, elapsed, outR, outG, outB);
            return;
        }
    }
    EvalLedBehavior(defaultLed_, nowMs, outR, outG, outB);
}

bool DualSenseHidManager::FindAndOpenWiredDualSense(void*& outHandle, uint16_t& outPid, uint16_t& outVer) {
    outHandle = INVALID_HANDLE_VALUE;
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        return false;
    }

    SP_DEVICE_INTERFACE_DATA ifData{};
    ifData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
    for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, &hidGuid, idx, &ifData); ++idx) {
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &reqSize, nullptr);
        if (reqSize == 0 || reqSize > 4096) continue;

        uint8_t detailBuf[4096]{};
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuf);
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, reqSize, nullptr, nullptr)) {
            continue;
        }

        std::wstring path = detail->DevicePath;
        std::wstring lower = path;
        for (auto& ch : lower) ch = static_cast<wchar_t>(towlower(ch));
        if (lower.find(L"vid_054c") == std::wstring::npos) {
            continue;
        }

        HANDLE h = CreateFileW(
            path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr
        );
        if (h == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attrs{};
        attrs.Size = sizeof(HIDD_ATTRIBUTES);
        bool match = false;
        if (HidD_GetAttributes(h, &attrs) &&
            attrs.VendorID == SONY_VID &&
            (attrs.ProductID == DUALSENSE_PID || attrs.ProductID == DUALSENSE_EDGE_PID)) {
            PHIDP_PREPARSED_DATA ppd = nullptr;
            if (HidD_GetPreparsedData(h, &ppd)) {
                HIDP_CAPS caps{};
                if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
                    // Wired USB DualSense exposes 64-byte input / 48-byte output
                    if (caps.UsagePage == 0x01 && caps.InputReportByteLength == USB_INPUT_REPORT_LEN &&
                        caps.OutputReportByteLength == USB_OUTPUT_REPORT_LEN) {
                        match = true;
                        outPid = attrs.ProductID;
                        outVer = attrs.VersionNumber;
                    }
                }
                HidD_FreePreparsedData(ppd);
            }
        }
        if (match) {
            outHandle = h;
            SetupDiDestroyDeviceInfoList(devInfo);
            return true;
        }
        CloseHandle(h);
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return false;
}

static bool OverlappedWrite(HANDLE h, const uint8_t* data, DWORD len, DWORD timeoutMs) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, len, &written, &ov);
    if (!ok) {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            CloseHandle(ov.hEvent);
            return false;
        }
        DWORD w = WaitForSingleObject(ov.hEvent, timeoutMs);
        if (w != WAIT_OBJECT_0 || !GetOverlappedResult(h, &ov, &written, FALSE)) {
            CancelIo(h);
            CloseHandle(ov.hEvent);
            return false;
        }
    }
    CloseHandle(ov.hEvent);
    return written == len;
}

void DualSenseHidManager::SendNeutralReset(void* handle) {
    if (!handle || handle == INVALID_HANDLE_VALUE) return;
    uint8_t buf[USB_OUTPUT_REPORT_LEN]{};
    TriggerConfig off{};
    BuildUsbOutputReport(buf, 0, 0, off, off, 0, 0, 0, 0x00);
    OverlappedWrite(static_cast<HANDLE>(handle), buf, USB_OUTPUT_REPORT_LEN, 250);
}

void DualSenseHidManager::ForceNeutralOnProcessExit() {
    running_ = false;
    void* h = activeHandle_.exchange(nullptr);
    if (h && h != INVALID_HANDLE_VALUE) {
        SendNeutralReset(h);
    }
}

static uint8_t ApplyWeaponTriggerGate(uint8_t raw, uint8_t fireThreshold, bool& latched) {
    if (fireThreshold == 0) {
        latched = false;
        return raw;
    }
    uint8_t releaseThreshold = (fireThreshold > 22) ? static_cast<uint8_t>(fireThreshold - 22) : 0;
    if (!latched) {
        if (raw >= fireThreshold) {
            latched = true;
            return 255;
        }
        return 0;
    } else {
        if (raw <= releaseThreshold) {
            latched = false;
            return 0;
        }
        return 255;
    }
}

void DualSenseHidManager::ReadThreadLoop(void* rawHandle, std::atomic<bool>& deviceAlive) {
    HANDLE h = static_cast<HANDLE>(rawHandle);
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        deviceAlive = false;
        return;
    }

    bool l2Latched = false;
    bool r2Latched = false;
    uint8_t buf[USB_INPUT_REPORT_LEN]{};
    while (running_ && deviceAlive) {
        ResetEvent(ov.hEvent);
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(h, buf, USB_INPUT_REPORT_LEN, &bytesRead, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                deviceAlive = false;
                break;
            }
            DWORD wait = WaitForSingleObject(ov.hEvent, 100);
            if (wait == WAIT_TIMEOUT) {
                CancelIo(h);
                // Wait briefly for cancel to finish cleanly
                GetOverlappedResult(h, &ov, &bytesRead, TRUE);
                continue;
            }
            if (wait != WAIT_OBJECT_0 || !GetOverlappedResult(h, &ov, &bytesRead, FALSE)) {
                deviceAlive = false;
                break;
            }
        }

        if (bytesRead >= 12 && buf[0] == 0x01) {
            bool guide = false, touch = false;
            uint16_t btns = TranslateButtons(buf, guide, touch);
            int16_t lx = ScaleAxis(buf[1], false);
            int16_t ly = ScaleAxis(buf[2], true);
            int16_t rx = ScaleAxis(buf[3], false);
            int16_t ry = ScaleAxis(buf[4], true);
            uint8_t rawL2 = buf[5];
            uint8_t rawR2 = buf[6];
            uint8_t l2 = ApplyWeaponTriggerGate(rawL2, weaponGateL2_.load(std::memory_order_relaxed), l2Latched);
            uint8_t r2 = ApplyWeaponTriggerGate(rawR2, weaponGateR2_.load(std::memory_order_relaxed), r2Latched);

            bool significantInput =
                (btns != 0) || guide || touch || (rawL2 > 15) || (rawR2 > 15) ||
                (std::abs(lx) > 6000) || (std::abs(ly) > 6000) ||
                (std::abs(rx) > 6000) || (std::abs(ry) > 6000);

            std::lock_guard<std::mutex> lk(stateMutex_);
            bool changed =
                (inputState_.buttons != btns) ||
                (inputState_.guidePressed != guide) ||
                (inputState_.leftTrigger != l2) ||
                (inputState_.rightTrigger != r2) ||
                (inputState_.thumbLX != lx) || (inputState_.thumbLY != ly) ||
                (inputState_.thumbRX != rx) || (inputState_.thumbRY != ry);

            inputState_.connected = true;
            inputState_.buttons = btns;
            inputState_.guidePressed = guide;
            inputState_.touchpadPressed = touch;
            inputState_.leftTrigger = l2;
            inputState_.rightTrigger = r2;
            inputState_.thumbLX = lx;
            inputState_.thumbLY = ly;
            inputState_.thumbRX = rx;
            inputState_.thumbRY = ry;
            if (changed) {
                inputState_.packetNumber++;
            }
            if (significantInput) {
                lastActivityMs_ = NowMs();
            }
        }
    }

    CancelIo(h);
    CloseHandle(ov.hEvent);
}

void DualSenseHidManager::WorkerLoop() {
    while (running_) {
        HANDLE h = INVALID_HANDLE_VALUE;
        uint16_t pid = 0, ver = 0;
        if (!FindAndOpenWiredDualSense(reinterpret_cast<void*&>(h), pid, ver)) {
            {
                std::lock_guard<std::mutex> lk(stateMutex_);
                inputState_.connected = false;
            }
            for (int i = 0; i < 10 && running_; ++i) Sleep(50);
            continue;
        }

        BridgeLog("HID: Wired DualSense connected (VID=0x054C PID=0x%04X Ver=0x%04X)", pid, ver);
        activeHandle_ = h;
        {
            std::lock_guard<std::mutex> lk(stateMutex_);
            inputState_.connected = true;
            inputState_.productId = pid;
            inputState_.versionNumber = ver;
            inputState_.packetNumber++;
            lastActivityMs_ = NowMs();
        }
        outputDirty_ = true;

        std::atomic<bool> deviceAlive{true};
        std::thread reader(&DualSenseHidManager::ReadThreadLoop, this, h, std::ref(deviceAlive));

        bool firstSent = false;
        uint8_t lastRL = 0, lastRR = 0, lastR = 0, lastG = 0, lastB = 0, lastPL = 0;
        TriggerConfig lastL2{}, lastR2{};

        while (running_ && deviceAlive) {
            uint64_t now = NowMs();
            uint8_t report[USB_OUTPUT_REPORT_LEN]{};
            bool animatedLed = false;
            bool needSend = false;

            uint8_t curRL = 0, curRR = 0, curR = 0, curG = 0, curB = 0, curPL = 0;
            TriggerConfig curL2{}, curR2{};

            {
                std::lock_guard<std::mutex> lk(outMutex_);
                curRL = static_cast<uint8_t>(xinputLeft_ >> 8);
                curRR = static_cast<uint8_t>(xinputRight_ >> 8);
                if (now < extraRumbleUntilMs_) {
                    curRL = std::max(curRL, extraRumbleLeft_);
                    curRR = std::max(curRR, extraRumbleRight_);
                }
                ComputeCurrentLed(now, curR, curG, curB);
                curL2 = triggerL2_;
                curR2 = triggerR2_;
                curPL = playerLeds_;
                animatedLed = (hasSpecialLed_ && specialLed_.type != LedEffectType::Default) ||
                              (hasBaseOverride_ && baseOverride_.type != LedEffectType::Default);
                outputDirty_ = false;
            }

            bool sendRumble = !firstSent || (curRL != lastRL) || (curRR != lastRR);
            bool sendR2 = !firstSent || (curR2 != lastR2);
            bool sendL2 = !firstSent || (curL2 != lastL2);
            bool sendLed = !firstSent || (curR != lastR) || (curG != lastG) || (curB != lastB);
            bool sendPlayerLeds = !firstSent || (curPL != lastPL);

            needSend = sendRumble || sendR2 || sendL2 || sendLed || sendPlayerLeds;
            if (needSend) {
                BuildUsbOutputReport(report, curRL, curRR, curL2, curR2, curR, curG, curB, curPL,
                                     sendRumble, sendR2, sendL2, sendLed, sendPlayerLeds);
                if (!OverlappedWrite(h, report, USB_OUTPUT_REPORT_LEN, 250)) {
                    BridgeLog("HID: Output WriteFile failed, reconnecting...");
                    deviceAlive = false;
                    break;
                }
                firstSent = true;
                lastRL = curRL;
                lastRR = curRR;
                lastL2 = curL2;
                lastR2 = curR2;
                lastR = curR;
                lastG = curG;
                lastB = curB;
                lastPL = curPL;
            }

            Sleep(animatedLed ? 16 : 8);
        }

        deviceAlive = false;
        if (reader.joinable()) reader.join();

        void* closedH = activeHandle_.exchange(nullptr);
        SendNeutralReset(closedH ? closedH : h);
        CloseHandle(h);
        BridgeLog("HID: DualSense handle closed cleanly (neutral reset sent).");
        {
            std::lock_guard<std::mutex> lk(stateMutex_);
            inputState_.connected = false;
        }
        if (running_) Sleep(200);
    }
}

} // namespace ds
