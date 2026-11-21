#pragma once

#include <atomic>
#include <mutex>
#include <thread>
#include <cstdint>
#include <string>

namespace ds {

enum class HapticBurstType : uint8_t {
    Pistol = 0,
    Revolver,
    Rifle,
    Shotgun,
    DryFire,
    Reload,
    Melee,
    HitReact,
    Impact,
    CarShift,
    CarBackfire,
    HorseStep,
    HorseBoost,
    UiTick,
};

struct HapticBurstSlot {
    bool active = false;
    HapticBurstType type = HapticBurstType::Pistol;
    float elapsed = 0.0f;
    float duration = 0.1f;
    float intensity = 0.6f;
    float panLeft = 1.0f;
    float panRight = 1.0f;
};

class WasapiHapticsEngine {
public:
    WasapiHapticsEngine() = default;
    ~WasapiHapticsEngine();

    void Start();
    void Stop();

    bool IsActive() const { return endpointActive_.load(); }
    void SetContinuousRumble(uint16_t leftMotor, uint16_t rightMotor);
    void SetContinuousHapticsValue(float freqHz, float amp, uint8_t hand); // 0=L,1=R,2=Both
    void TriggerBurst(HapticBurstType type, float intensity, float pan = 0.0f);
    void TriggerNamedBurst(const std::string& name, float intensity, float pan = 0.0f);

private:
    void AudioLoop();
    void SynthesizeFrames(float* outInterleaved, uint32_t frames, uint32_t channels, uint32_t sampleRate);

    std::atomic<bool> running_{false};
    std::atomic<bool> endpointActive_{false};
    std::thread audioThread_;

    std::atomic<float> rumbleLeft_{0.0f};
    std::atomic<float> rumbleRight_{0.0f};
    std::atomic<float> directFreqL_{0.0f};
    std::atomic<float> directAmpL_{0.0f};
    std::atomic<float> directFreqR_{0.0f};
    std::atomic<float> directAmpR_{0.0f};

    std::mutex burstMutex_;
    static constexpr int MAX_BURSTS = 8;
    HapticBurstSlot bursts_[MAX_BURSTS]{};

    double phaseLowL_ = 0.0;
    double phaseLowR_ = 0.0;
    double phaseHighL_ = 0.0;
    double phaseHighR_ = 0.0;
    double phaseDirectL_ = 0.0;
    double phaseDirectR_ = 0.0;
};

} // namespace ds
