#pragma once

#include "dualsense_protocol.hpp"
#include <atomic>
#include <mutex>
#include <thread>
#include <cstdint>

namespace ds {

class DualSenseHidManager {
public:
    DualSenseHidManager() = default;
    ~DualSenseHidManager();

    void Start();
    void Stop();

    bool IsConnected() const;
    bool WasActiveRecently(uint32_t withinMs = 4000) const;
    DualSenseInputState GetInputState() const;

    // XInput rumble (0..65535)
    void SetXInputRumble(uint16_t leftMotor, uint16_t rightMotor);
    // Extra rumble pulse from mod (0..255 with durationMs)
    void AddExtraRumble(uint8_t left, uint8_t right, uint32_t durationMs);

    // Triggers (side: 0=Left, 1=Right, 2=Both)
    void SetTrigger(uint8_t side, const TriggerConfig& cfg);
    TriggerConfig GetTrigger(uint8_t side) const;

    // Lightbar controls
    void SetDefaultColor(uint8_t r, uint8_t g, uint8_t b);
    void OverrideBaseLed(const LedBehavior& b);
    void StopBaseLedOverride();
    void ApplySpecialLed(const LedBehavior& b);
    void StopSpecialLed();
    void SetPlayerLeds(uint8_t mask);
    void ForceNeutralOnProcessExit();

private:
    void WorkerLoop();
    void ReadThreadLoop(void* handle, std::atomic<bool>& deviceAlive);
    void ComputeCurrentLed(uint64_t nowMs, uint8_t& outR, uint8_t& outG, uint8_t& outB);
    void SendNeutralReset(void* handle);
    bool FindAndOpenWiredDualSense(void*& outHandle, uint16_t& outPid, uint16_t& outVer);

    std::atomic<bool> running_{false};
    std::thread workerThread_;

    mutable std::mutex stateMutex_;
    DualSenseInputState inputState_{};
    uint64_t lastActivityMs_ = 0;

    mutable std::mutex outMutex_;
    uint16_t xinputLeft_ = 0;
    uint16_t xinputRight_ = 0;
    uint8_t extraRumbleLeft_ = 0;
    uint8_t extraRumbleRight_ = 0;
    uint64_t extraRumbleUntilMs_ = 0;

    TriggerConfig triggerL2_{};
    TriggerConfig triggerR2_{};

    // Lightbar layering: default -> baseOverride -> specialEffect
    LedBehavior defaultLed_{LedEffectType::Default, 18, 56, 130, 0.0f, 1.0f}; // subtle Sicilian night blue
    bool hasBaseOverride_ = false;
    LedBehavior baseOverride_{};
    uint64_t baseStartMs_ = 0;

    bool hasSpecialLed_ = false;
    LedBehavior specialLed_{};
    uint64_t specialStartMs_ = 0;

    uint8_t playerLeds_ = 0x04; // center LED (Player 1)
    std::atomic<bool> outputDirty_{true};
    std::atomic<void*> activeHandle_{nullptr};
};

} // namespace ds
