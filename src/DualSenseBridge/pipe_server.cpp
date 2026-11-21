#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "pipe_server.hpp"
#include <sstream>
#include <algorithm>
#include <cstdio>

namespace ds {

void BridgeLog(const char* fmt, ...);

static constexpr const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\MafiaTOCDualSenseBridge";

PipeServer::PipeServer(DualSenseHidManager& hid, WasapiHapticsEngine& haptics)
    : hid_(hid), haptics_(haptics) {}

PipeServer::~PipeServer() {
    Stop();
}

void PipeServer::Start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    acceptThread_ = std::thread(&PipeServer::AcceptLoop, this);
}

void PipeServer::Stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;
    if (stopEvent_) {
        SetEvent(static_cast<HANDLE>(stopEvent_));
    }
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lk(clientsMutex_);
        threads.swap(clientThreads_);
    }
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
    if (stopEvent_) {
        CloseHandle(static_cast<HANDLE>(stopEvent_));
        stopEvent_ = nullptr;
    }
}

void PipeServer::AcceptLoop() {
    HANDLE stopH = static_cast<HANDLE>(stopEvent_);
    BridgeLog("IPC: Pipe server listening on \\\\.\\pipe\\MafiaTOCDualSenseBridge");

    while (running_) {
        HANDLE hPipe = CreateNamedPipeW(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            4096,
            4096,
            0,
            nullptr
        );
        if (hPipe == INVALID_HANDLE_VALUE) {
            BridgeLog("IPC: CreateNamedPipeW failed err=%lu", GetLastError());
            Sleep(200);
            continue;
        }

        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) {
            CloseHandle(hPipe);
            Sleep(100);
            continue;
        }

        BOOL connected = ConnectNamedPipe(hPipe, &ov);
        if (!connected) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                HANDLE waits[2] = {stopH, ov.hEvent};
                DWORD wr = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (wr == WAIT_OBJECT_0 || !running_) {
                    CancelIo(hPipe);
                    CloseHandle(ov.hEvent);
                    CloseHandle(hPipe);
                    break;
                }
                DWORD dummy = 0;
                connected = GetOverlappedResult(hPipe, &ov, &dummy, FALSE);
            } else if (err == ERROR_PIPE_CONNECTED) {
                connected = TRUE;
            }
        }
        CloseHandle(ov.hEvent);

        if (!connected || !running_) {
            CloseHandle(hPipe);
            continue;
        }

        std::lock_guard<std::mutex> lk(clientsMutex_);
        // Prune finished? We keep clean join at Stop(); limit growth by detaching? No, joinable vector is safe for <few dozen connections,
        // or cap/clean finished isn't needed unless hundreds connect; still safe.
        clientThreads_.emplace_back(&PipeServer::ClientThread, this, hPipe);
    }
}

void PipeServer::ClientThread(void* rawPipe) {
    HANDLE hPipe = static_cast<HANDLE>(rawPipe);
    HANDLE stopH = static_cast<HANDLE>(stopEvent_);
    HANDLE readEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    std::string accum;
    char buf[1024];

    while (running_ && readEv) {
        ResetEvent(readEv);
        OVERLAPPED ov{};
        ov.hEvent = readEv;
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(hPipe, buf, sizeof(buf), &bytesRead, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                HANDLE waits[2] = {stopH, readEv};
                DWORD wr = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (wr == WAIT_OBJECT_0 || !running_) {
                    CancelIo(hPipe);
                    break;
                }
                if (!GetOverlappedResult(hPipe, &ov, &bytesRead, FALSE) || bytesRead == 0) {
                    break;
                }
            } else {
                break; // ERROR_BROKEN_PIPE etc.
            }
        }
        if (bytesRead == 0) break;

        accum.append(buf, buf + bytesRead);
        size_t pos = 0;
        while ((pos = accum.find('\n')) != std::string::npos) {
            std::string line = accum.substr(0, pos);
            accum.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            std::string reply = ProcessCommand(line);
            if (!reply.empty()) {
                OVERLAPPED wov{};
                wov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (wov.hEvent) {
                    DWORD written = 0;
                    if (!WriteFile(hPipe, reply.data(), static_cast<DWORD>(reply.size()), &written, &wov)) {
                        if (GetLastError() == ERROR_IO_PENDING) {
                            WaitForSingleObject(wov.hEvent, 200);
                            GetOverlappedResult(hPipe, &wov, &written, FALSE);
                        }
                    }
                    CloseHandle(wov.hEvent);
                }
            }
        }
    }

    if (readEv) CloseHandle(readEv);
    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);
}

static int ToInt(const std::string& s, int defVal = 0) {
    try { return std::stoi(s); } catch (...) { return defVal; }
}

static float ToFloat(const std::string& s, float defVal = 0.0f) {
    try { return std::stof(s); } catch (...) { return defVal; }
}

std::string PipeServer::ProcessCommand(const std::string& line) {
    std::istringstream iss(line);
    std::vector<std::string> tok;
    std::string t;
    while (iss >> t) tok.push_back(t);
    if (tok.empty()) return "";

    const std::string& cmd = tok[0];

    if (cmd == "PING") {
        return "PONG\n";
    }
    if (cmd == "STATE") {
        char out[160];
        int conn = hid_.IsConnected() ? 1 : 0;
        int act = hid_.WasActiveRecently(4000) ? 1 : 0;
        int l2 = static_cast<int>(hid_.GetTrigger(0).mode);
        int r2 = static_cast<int>(hid_.GetTrigger(1).mode);
        int was = haptics_.IsActive() ? 1 : 0;
        std::snprintf(out, sizeof(out), "CONNECTED=%d ACTIVE=%d L2=%d R2=%d WASAPI=%d\n", conn, act, l2, r2, was);
        return std::string(out);
    }

    auto getSide = [&](size_t idx) -> uint8_t {
        if (idx >= tok.size()) return 2;
        return ClampU8(ToInt(tok[idx], 2), 0, 2);
    };

    if (cmd == "TRIG_OFF") {
        TriggerConfig cfg{};
        cfg.mode = TriggerMode::Off;
        hid_.SetTrigger(getSide(1), cfg);
        return "";
    }
    if (cmd == "TRIG_WEAPON" && tok.size() >= 5) {
        TriggerConfig cfg{};
        cfg.mode = TriggerMode::Weapon;
        cfg.p0 = ClampU8(ToInt(tok[2]));
        cfg.p1 = ClampU8(ToInt(tok[3]));
        cfg.p2 = ClampU8(ToInt(tok[4]));
        hid_.SetTrigger(getSide(1), cfg);
        return "";
    }
    if (cmd == "TRIG_FEEDBACK" && tok.size() >= 4) {
        TriggerConfig cfg{};
        cfg.mode = TriggerMode::Feedback;
        cfg.p0 = ClampU8(ToInt(tok[2]));
        cfg.p1 = ClampU8(ToInt(tok[3]));
        hid_.SetTrigger(getSide(1), cfg);
        return "";
    }
    if (cmd == "TRIG_SLOPE" && tok.size() >= 6) {
        TriggerConfig cfg{};
        cfg.mode = TriggerMode::SlopeFeedback;
        cfg.p0 = ClampU8(ToInt(tok[2]));
        cfg.p1 = ClampU8(ToInt(tok[3]));
        cfg.p2 = ClampU8(ToInt(tok[4]));
        cfg.p3 = ClampU8(ToInt(tok[5]));
        hid_.SetTrigger(getSide(1), cfg);
        return "";
    }
    if (cmd == "TRIG_VIB" && tok.size() >= 5) {
        TriggerConfig cfg{};
        cfg.mode = TriggerMode::Vibration;
        cfg.p0 = ClampU8(ToInt(tok[2]));
        cfg.p1 = ClampU8(ToInt(tok[3]));
        cfg.p2 = ClampU8(ToInt(tok[4]));
        hid_.SetTrigger(getSide(1), cfg);
        return "";
    }
    if (cmd == "TRIG_MULTI_FB" && tok.size() >= 3) {
        TriggerConfig cfg{};
        cfg.mode = TriggerMode::MultiPosFeedback;
        for (size_t i = 0; i < 10 && (2 + i) < tok.size(); ++i) {
            cfg.zones[i] = ClampU8(ToInt(tok[2 + i]));
        }
        hid_.SetTrigger(getSide(1), cfg);
        return "";
    }
    if (cmd == "TRIG_MULTI_VIB" && tok.size() >= 4) {
        TriggerConfig cfg{};
        cfg.mode = TriggerMode::MultiPosVibration;
        cfg.p0 = ClampU8(ToInt(tok[2]));
        for (size_t i = 0; i < 10 && (3 + i) < tok.size(); ++i) {
            cfg.zones[i] = ClampU8(ToInt(tok[3 + i]));
        }
        hid_.SetTrigger(getSide(1), cfg);
        return "";
    }
    if ((cmd == "LED_BASE" || cmd == "LED_SPEC") && tok.size() >= 7) {
        LedBehavior b{};
        int tVal = ToInt(tok[1], 0);
        b.type = (tVal == 1) ? LedEffectType::Flash : ((tVal == 2) ? LedEffectType::Pulse : LedEffectType::Default);
        b.r = ClampU8(ToInt(tok[2]));
        b.g = ClampU8(ToInt(tok[3]));
        b.b = ClampU8(ToInt(tok[4]));
        b.duration = std::max(0.0f, ToFloat(tok[5], 0.0f));
        b.interval = std::max(0.05f, ToFloat(tok[6], 1.0f));
        if (cmd == "LED_BASE") hid_.OverrideBaseLed(b);
        else hid_.ApplySpecialLed(b);
        return "";
    }
    if (cmd == "LED_STOP_SPEC") {
        hid_.StopSpecialLed();
        return "";
    }
    if (cmd == "LED_STOP_BASE") {
        hid_.StopBaseLedOverride();
        return "";
    }
    if (cmd == "LED_COLOR" && tok.size() >= 4) {
        hid_.SetDefaultColor(ClampU8(ToInt(tok[1])), ClampU8(ToInt(tok[2])), ClampU8(ToInt(tok[3])));
        return "";
    }
    if (cmd == "LED_PLAYER" && tok.size() >= 2) {
        hid_.SetPlayerLeds(ClampU8(ToInt(tok[1], 4)));
        return "";
    }
    if (cmd == "HAPTIC_BURST" && tok.size() >= 3) {
        float intensity = ToFloat(tok[2], 0.6f);
        float pan = (tok.size() >= 4) ? ToFloat(tok[3], 0.0f) : 0.0f;
        haptics_.TriggerNamedBurst(tok[1], intensity, pan);
        return "";
    }
    if (cmd == "HAPTIC_VALUE" && tok.size() >= 4) {
        float freq = ToFloat(tok[1], 0.0f);
        float amp = ToFloat(tok[2], 0.0f);
        uint8_t hand = ClampU8(ToInt(tok[3], 2), 0, 2);
        haptics_.SetContinuousHapticsValue(freq, amp, hand);
        return "";
    }
    if (cmd == "RUMBLE_ADD" && tok.size() >= 4) {
        uint8_t l = ClampU8(ToInt(tok[1], 0));
        uint8_t r = ClampU8(ToInt(tok[2], 0));
        uint32_t ms = static_cast<uint32_t>(std::max(10, ToInt(tok[3], 80)));
        hid_.AddExtraRumble(l, r, ms);
        return "";
    }

    return "";
}

} // namespace ds
