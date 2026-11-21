#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <avrt.h>

#include "wasapi_haptics.hpp"
#include <algorithm>
#include <cmath>
#include <cwctype>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

namespace ds {

void BridgeLog(const char* fmt, ...);

static const PROPERTYKEY kPkeyFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    14
};

WasapiHapticsEngine::~WasapiHapticsEngine() {
    Stop();
}

void WasapiHapticsEngine::Start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    audioThread_ = std::thread(&WasapiHapticsEngine::AudioLoop, this);
}

void WasapiHapticsEngine::Stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;
    if (audioThread_.joinable()) {
        audioThread_.join();
    }
    endpointActive_ = false;
}

void WasapiHapticsEngine::SetContinuousRumble(uint16_t leftMotor, uint16_t rightMotor) {
    rumbleLeft_ = static_cast<float>(leftMotor) / 65535.0f;
    rumbleRight_ = static_cast<float>(rightMotor) / 65535.0f;
}

void WasapiHapticsEngine::SetContinuousHapticsValue(float freqHz, float amp, uint8_t hand) {
    float f = std::max(0.0f, std::min(300.0f, freqHz));
    float a = std::max(0.0f, std::min(1.0f, amp));
    if (hand == 0 || hand == 2) { directFreqL_ = f; directAmpL_ = a; }
    if (hand == 1 || hand == 2) { directFreqR_ = f; directAmpR_ = a; }
}

static float DurationForBurst(HapticBurstType t) {
    switch (t) {
    case HapticBurstType::Pistol:      return 0.11f;
    case HapticBurstType::Revolver:    return 0.14f;
    case HapticBurstType::Rifle:       return 0.16f;
    case HapticBurstType::Shotgun:     return 0.24f;
    case HapticBurstType::DryFire:     return 0.045f;
    case HapticBurstType::Reload:      return 0.12f;
    case HapticBurstType::Melee:       return 0.15f;
    case HapticBurstType::HitReact:    return 0.22f;
    case HapticBurstType::Impact:      return 0.26f;
    case HapticBurstType::CarShift:    return 0.09f;
    case HapticBurstType::CarBackfire: return 0.18f;
    case HapticBurstType::HorseStep:   return 0.065f;
    case HapticBurstType::HorseBoost:  return 0.20f;
    case HapticBurstType::UiTick:      return 0.03f;
    }
    return 0.1f;
}

void WasapiHapticsEngine::TriggerBurst(HapticBurstType type, float intensity, float pan) {
    float clamped = std::max(0.05f, std::min(1.0f, intensity));
    float p = std::max(-1.0f, std::min(1.0f, pan));
    float pL = std::min(1.0f, 1.0f - std::max(0.0f, p));
    float pR = std::min(1.0f, 1.0f + std::min(0.0f, p));

    std::lock_guard<std::mutex> lk(burstMutex_);
    int targetIdx = 0;
    float maxProgress = -1.0f;
    for (int i = 0; i < MAX_BURSTS; ++i) {
        if (!bursts_[i].active) {
            targetIdx = i;
            break;
        }
        float prog = bursts_[i].elapsed / std::max(0.01f, bursts_[i].duration);
        if (prog > maxProgress) {
            maxProgress = prog;
            targetIdx = i;
        }
    }
    bursts_[targetIdx].active = true;
    bursts_[targetIdx].type = type;
    bursts_[targetIdx].elapsed = 0.0f;
    bursts_[targetIdx].duration = DurationForBurst(type);
    bursts_[targetIdx].intensity = clamped;
    bursts_[targetIdx].panLeft = pL;
    bursts_[targetIdx].panRight = pR;
}

void WasapiHapticsEngine::TriggerNamedBurst(const std::string& name, float intensity, float pan) {
    HapticBurstType t = HapticBurstType::Pistol;
    if (name == "REVOLVER") t = HapticBurstType::Revolver;
    else if (name == "RIFLE") t = HapticBurstType::Rifle;
    else if (name == "SHOTGUN") t = HapticBurstType::Shotgun;
    else if (name == "DRYFIRE") t = HapticBurstType::DryFire;
    else if (name == "RELOAD") t = HapticBurstType::Reload;
    else if (name == "MELEE") t = HapticBurstType::Melee;
    else if (name == "HIT_REACT") t = HapticBurstType::HitReact;
    else if (name == "IMPACT") t = HapticBurstType::Impact;
    else if (name == "CAR_SHIFT") t = HapticBurstType::CarShift;
    else if (name == "CAR_BACKFIRE") t = HapticBurstType::CarBackfire;
    else if (name == "HORSE_STEP") t = HapticBurstType::HorseStep;
    else if (name == "HORSE_BOOST") t = HapticBurstType::HorseBoost;
    else if (name == "UI_TICK") t = HapticBurstType::UiTick;
    TriggerBurst(t, intensity, pan);
}

static float EvalBurstWaveform(const HapticBurstSlot& s, float t, bool isRight) {
    float norm = t / std::max(0.001f, s.duration);
    if (norm >= 1.0f) return 0.0f;
    constexpr float twoPi = 6.2831853f;
    float env = std::exp(-5.5f * norm) * std::sin(std::min(1.0f, norm * 12.0f) * 1.5707963f);
    float sideDetune = isRight ? 1.07f : 1.0f;

    switch (s.type) {
    case HapticBurstType::Pistol:
        return env * (0.7f * std::sin(twoPi * 95.0f * sideDetune * t) + 0.4f * std::sin(twoPi * 195.0f * t));
    case HapticBurstType::Revolver:
        return env * (0.75f * std::sin(twoPi * 72.0f * sideDetune * t) + 0.45f * std::sin(twoPi * 165.0f * t));
    case HapticBurstType::Rifle:
        return env * (0.8f * std::sin(twoPi * 60.0f * sideDetune * t) + 0.4f * std::sin(twoPi * 145.0f * t));
    case HapticBurstType::Shotgun:
        return env * (0.85f * std::sin(twoPi * 44.0f * sideDetune * t) + 0.55f * std::sin(twoPi * 92.0f * t));
    case HapticBurstType::DryFire:
        return env * std::sin(twoPi * 240.0f * sideDetune * t);
    case HapticBurstType::Reload: {
        // Double mechanical latch click
        float click1 = std::exp(-28.0f * norm) * std::sin(twoPi * 175.0f * t);
        float click2 = (norm > 0.45f) ? std::exp(-28.0f * (norm - 0.45f)) * std::sin(twoPi * 210.0f * t) : 0.0f;
        return 0.8f * (click1 + click2);
    }
    case HapticBurstType::Melee:
        return env * (0.7f * std::sin(twoPi * 55.0f * sideDetune * t) + 0.3f * std::sin(twoPi * 130.0f * t));
    case HapticBurstType::HitReact:
        return env * (0.75f * std::sin(twoPi * 48.0f * sideDetune * t) + 0.35f * std::sin(twoPi * 115.0f * t));
    case HapticBurstType::Impact:
        return env * (0.85f * std::sin(twoPi * 38.0f * sideDetune * t) + 0.45f * std::sin(twoPi * 88.0f * t));
    case HapticBurstType::CarShift:
        return env * std::sin(twoPi * 68.0f * sideDetune * t);
    case HapticBurstType::CarBackfire:
        return env * (0.7f * std::sin(twoPi * 52.0f * t) + 0.5f * std::sin(twoPi * 155.0f * t));
    case HapticBurstType::HorseStep:
        return env * std::sin(twoPi * 64.0f * sideDetune * t);
    case HapticBurstType::HorseBoost:
        return env * (0.65f * std::sin(twoPi * 75.0f * t) + 0.35f * std::sin(twoPi * 140.0f * t));
    case HapticBurstType::UiTick:
        return env * std::sin(twoPi * 210.0f * t);
    }
    return 0.0f;
}

void WasapiHapticsEngine::SynthesizeFrames(float* outInterleaved, uint32_t frames, uint32_t channels, uint32_t sampleRate) {
    const double dt = 1.0 / static_cast<double>(sampleRate);
    constexpr double twoPi = 6.283185307179586;

    float rL = rumbleLeft_.load();
    float rR = rumbleRight_.load();
    float dFL = directFreqL_.load();
    float dAL = directAmpL_.load();
    float dFR = directFreqR_.load();
    float dAR = directAmpR_.load();

    HapticBurstSlot localBursts[MAX_BURSTS];
    {
        std::lock_guard<std::mutex> lk(burstMutex_);
        for (int i = 0; i < MAX_BURSTS; ++i) localBursts[i] = bursts_[i];
    }

    for (uint32_t f = 0; f < frames; ++f) {
        // Continuous XInput rumble -> dual-band tactile synthesis
        double lowL = std::sin(phaseLowL_) * 0.65 + std::sin(phaseLowL_ * 2.15) * 0.25;
        double lowR = std::sin(phaseLowR_) * 0.65 + std::sin(phaseLowR_ * 2.15) * 0.25;
        double hiL = std::sin(phaseHighL_);
        double hiR = std::sin(phaseHighR_);

        float sampL = static_cast<float>(lowL * rL * 0.48 + hiL * rR * 0.22);
        float sampR = static_cast<float>(lowR * rL * 0.38 + hiR * rR * 0.32);

        if (dAL > 0.005f && dFL > 1.0f) {
            sampL += static_cast<float>(std::sin(phaseDirectL_)) * dAL * 0.55f;
        }
        if (dAR > 0.005f && dFR > 1.0f) {
            sampR += static_cast<float>(std::sin(phaseDirectR_)) * dAR * 0.55f;
        }

        phaseLowL_ = std::fmod(phaseLowL_ + twoPi * 48.0 * dt, twoPi * 100.0);
        phaseLowR_ = std::fmod(phaseLowR_ + twoPi * 52.0 * dt, twoPi * 100.0);
        phaseHighL_ = std::fmod(phaseHighL_ + twoPi * 165.0 * dt, twoPi);
        phaseHighR_ = std::fmod(phaseHighR_ + twoPi * 182.0 * dt, twoPi);
        phaseDirectL_ = std::fmod(phaseDirectL_ + twoPi * std::max(20.0f, dFL) * dt, twoPi);
        phaseDirectR_ = std::fmod(phaseDirectR_ + twoPi * std::max(20.0f, dFR) * dt, twoPi);

        // Transient bursts
        for (int i = 0; i < MAX_BURSTS; ++i) {
            if (!localBursts[i].active) continue;
            float t = localBursts[i].elapsed;
            float amp = localBursts[i].intensity * 0.72f;
            sampL += EvalBurstWaveform(localBursts[i], t, false) * amp * localBursts[i].panLeft;
            sampR += EvalBurstWaveform(localBursts[i], t, true) * amp * localBursts[i].panRight;
            localBursts[i].elapsed += static_cast<float>(dt);
            if (localBursts[i].elapsed >= localBursts[i].duration) {
                localBursts[i].active = false;
            }
        }

        sampL = std::max(-0.85f, std::min(0.85f, sampL));
        sampR = std::max(-0.85f, std::min(0.85f, sampR));

        float* framePtr = outInterleaved + f * channels;
        for (uint32_t c = 0; c < channels; ++c) {
            if (c == 2) framePtr[c] = sampL;      // Left Haptic Voice Coil
            else if (c == 3) framePtr[c] = sampR; // Right Haptic Voice Coil
            else framePtr[c] = 0.0f;              // Speaker L/R silent
        }
    }

    {
        std::lock_guard<std::mutex> lk(burstMutex_);
        for (int i = 0; i < MAX_BURSTS; ++i) {
            // Only overwrite if slot wasn't replaced with a fresh burst during synthesis
            if (bursts_[i].active && bursts_[i].type == localBursts[i].type &&
                bursts_[i].elapsed <= localBursts[i].elapsed) {
                bursts_[i] = localBursts[i];
            }
        }
    }
}

static bool FindDualSenseAudioDevice(IMMDevice** outDev) {
    *outDev = nullptr;
    IMMDeviceEnumerator* enumr = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumr));
    if (FAILED(hr) || !enumr) return false;

    IMMDeviceCollection* col = nullptr;
    hr = enumr->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
    if (SUCCEEDED(hr) && col) {
        UINT count = 0;
        col->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* dev = nullptr;
            if (SUCCEEDED(col->Item(i, &dev)) && dev) {
                IPropertyStore* ps = nullptr;
                if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && ps) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (SUCCEEDED(ps->GetValue(kPkeyFriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
                        std::wstring name = pv.pwszVal;
                        if (name.find(L"DualSense") != std::wstring::npos) {
                            *outDev = dev;
                            dev = nullptr; // ownership transferred
                            PropVariantClear(&pv);
                            ps->Release();
                            break;
                        }
                    }
                    PropVariantClear(&pv);
                    ps->Release();
                }
                if (dev) dev->Release();
            }
        }
        col->Release();
    }
    enumr->Release();
    return *outDev != nullptr;
}

void WasapiHapticsEngine::AudioLoop() {
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool needUninit = SUCCEEDED(hrCom);

    while (running_) {
        IMMDevice* dev = nullptr;
        if (!FindDualSenseAudioDevice(&dev)) {
            endpointActive_ = false;
            for (int i = 0; i < 20 && running_; ++i) Sleep(50);
            continue;
        }

        IAudioClient* ac = nullptr;
        HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ac));
        if (FAILED(hr) || !ac) {
            dev->Release();
            Sleep(500);
            continue;
        }

        WAVEFORMATEX* pwfx = nullptr;
        hr = ac->GetMixFormat(&pwfx);
        if (FAILED(hr) || !pwfx) {
            ac->Release();
            dev->Release();
            Sleep(500);
            continue;
        }

        bool isFloat32 = false;
        if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && pwfx->wBitsPerSample == 32) {
            isFloat32 = true;
        } else if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pwfx->cbSize >= 22) {
            auto* wfxe = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx);
            if (IsEqualGUID(wfxe->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && pwfx->wBitsPerSample == 32) {
                isFloat32 = true;
            }
        }

        if (!isFloat32 || pwfx->nChannels < 4) {
            BridgeLog("WASAPI: Unsupported DualSense mix format ch=%u rate=%u bits=%u tag=0x%X",
                      pwfx->nChannels, pwfx->nSamplesPerSec, pwfx->wBitsPerSample, pwfx->wFormatTag);
            CoTaskMemFree(pwfx);
            ac->Release();
            dev->Release();
            for (int i = 0; i < 40 && running_; ++i) Sleep(50);
            continue;
        }

        const UINT32 channels = pwfx->nChannels;
        const UINT32 sampleRate = pwfx->nSamplesPerSec;
        hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 400000 /* 40ms */, 0, pwfx, nullptr);
        CoTaskMemFree(pwfx);

        if (FAILED(hr)) {
            BridgeLog("WASAPI: IAudioClient::Initialize failed hr=0x%08X", hr);
            ac->Release();
            dev->Release();
            Sleep(500);
            continue;
        }

        UINT32 bufFrames = 0;
        ac->GetBufferSize(&bufFrames);
        IAudioRenderClient* rc = nullptr;
        hr = ac->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&rc));
        if (FAILED(hr) || !rc) {
            ac->Release();
            dev->Release();
            Sleep(500);
            continue;
        }

        hr = ac->Start();
        if (FAILED(hr)) {
            rc->Release();
            ac->Release();
            dev->Release();
            Sleep(500);
            continue;
        }

        endpointActive_ = true;
        BridgeLog("WASAPI: DualSense 4ch haptic audio endpoint active (%u ch, %u Hz, buf=%u frames)",
                  channels, sampleRate, bufFrames);

        while (running_) {
            UINT32 padding = 0;
            hr = ac->GetCurrentPadding(&padding);
            if (FAILED(hr)) {
                break; // likely AUDCLNT_E_DEVICE_INVALIDATED
            }
            UINT32 avail = (bufFrames > padding) ? (bufFrames - padding) : 0;
            // Write in small chunks (~10ms) to keep latency low
            UINT32 target = std::min(avail, sampleRate / 50);
            if (target > 0) {
                BYTE* pData = nullptr;
                hr = rc->GetBuffer(target, &pData);
                if (FAILED(hr)) break;
                SynthesizeFrames(reinterpret_cast<float*>(pData), target, channels, sampleRate);
                hr = rc->ReleaseBuffer(target, 0);
                if (FAILED(hr)) break;
            }
            Sleep(5);
        }

        ac->Stop();
        endpointActive_ = false;
        rc->Release();
        ac->Release();
        dev->Release();
        if (running_) Sleep(250);
    }

    if (needUninit) CoUninitialize();
}

} // namespace ds
