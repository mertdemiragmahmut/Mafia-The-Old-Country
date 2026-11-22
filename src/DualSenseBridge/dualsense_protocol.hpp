#pragma once

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace ds {

constexpr uint16_t SONY_VID = 0x054C;
constexpr uint16_t DUALSENSE_PID = 0x0CE6;
constexpr uint16_t DUALSENSE_EDGE_PID = 0x0DF2;

constexpr size_t USB_INPUT_REPORT_LEN = 64;
constexpr size_t USB_OUTPUT_REPORT_LEN = 48;

enum class TriggerMode : uint8_t {
    Off = 0,
    Feedback = 1,
    Weapon = 2,
    SlopeFeedback = 3,
    Vibration = 4,
    MultiPosFeedback = 5,
    MultiPosVibration = 6,
};

struct TriggerConfig {
    TriggerMode mode = TriggerMode::Off;
    uint8_t p0 = 0;   // position / startPos / frequency
    uint8_t p1 = 0;   // endPos / strength / amplitude
    uint8_t p2 = 0;   // strength / startStrength
    uint8_t p3 = 0;   // endStrength
    uint8_t zones[10] = {};

    bool operator==(const TriggerConfig& o) const {
        return mode == o.mode && p0 == o.p0 && p1 == o.p1 && p2 == o.p2 && p3 == o.p3 &&
               std::memcmp(zones, o.zones, sizeof(zones)) == 0;
    }
    bool operator!=(const TriggerConfig& o) const {
        return !(*this == o);
    }
};

enum class LedEffectType : uint8_t {
    Default = 0, // Solid
    Flash = 1,
    Pulse = 2,
};

struct LedBehavior {
    LedEffectType type = LedEffectType::Default;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    float duration = 0.0f; // 0 = indefinite
    float interval = 1.0f; // cycle period in seconds
};

struct DualSenseInputState {
    bool connected = false;
    bool activeRecently = false;
    uint16_t buttons = 0;
    bool guidePressed = false;
    bool touchpadPressed = false;
    uint8_t leftTrigger = 0;
    uint8_t rightTrigger = 0;
    int16_t thumbLX = 0;
    int16_t thumbLY = 0;
    int16_t thumbRX = 0;
    int16_t thumbRY = 0;
    uint32_t packetNumber = 0;
    uint16_t productId = 0;
    uint16_t versionNumber = 0;
};

inline uint8_t ClampU8(int v, int lo = 0, int hi = 255) {
    if (v < lo) return static_cast<uint8_t>(lo);
    if (v > hi) return static_cast<uint8_t>(hi);
    return static_cast<uint8_t>(v);
}

inline int16_t ScaleAxis(uint8_t raw, bool invertY) {
    int centered = static_cast<int>(raw) - 128;
    // Tiny 3-unit inner jitter filter so resting stick reports clean 0
    if (centered >= -3 && centered <= 3) {
        return 0;
    }
    if (invertY) {
        centered = -centered;
    }
    int scaled = centered * 257;
    if (scaled > 32767) scaled = 32767;
    if (scaled < -32768) scaled = -32768;
    return static_cast<int16_t>(scaled);
}

inline uint8_t NormalizePos09(uint8_t v) {
    if (v <= 9) return v;
    return ClampU8((static_cast<int>(v) * 9 + 127) / 255, 0, 9);
}

inline uint8_t NormalizeStrength08(uint8_t v) {
    if (v <= 8) return v;
    return ClampU8((static_cast<int>(v) * 8 + 127) / 255, 1, 8);
}

inline uint8_t ComputeWeaponGateThreshold(const TriggerConfig& cfg) {
    if (cfg.mode != TriggerMode::Weapon) return 0;
    if (NormalizeStrength08(cfg.p2) == 0) return 0;
    uint8_t start = ClampU8(NormalizePos09(cfg.p0), 0, 7);
    uint8_t end = ClampU8(NormalizePos09(cfg.p1), start + 1, 8);
    int thresh = (static_cast<int>(end) * 255 + 4) / 9 + 8;
    return ClampU8(thresh, 140, 242);
}

inline void EncodeTriggerBlock(uint8_t* out11, const TriggerConfig& cfg) {
    std::memset(out11, 0, 11);
    switch (cfg.mode) {
    case TriggerMode::Off: {
        out11[0] = 0x05;
        break;
    }
    case TriggerMode::Feedback: {
        uint8_t pos = NormalizePos09(cfg.p0);
        uint8_t str = NormalizeStrength08(cfg.p1);
        if (str == 0) { out11[0] = 0x05; break; }
        uint8_t force = ClampU8(str - 1, 0, 7);
        uint16_t activeZones = 0;
        uint32_t forceZones = 0;
        for (int i = pos; i <= 9; ++i) {
            activeZones |= static_cast<uint16_t>(1u << i);
            forceZones |= static_cast<uint32_t>(force & 0x7u) << (3 * i);
        }
        out11[0] = 0x21;
        out11[1] = static_cast<uint8_t>(activeZones & 0xFF);
        out11[2] = static_cast<uint8_t>((activeZones >> 8) & 0xFF);
        out11[3] = static_cast<uint8_t>((forceZones >> 0) & 0xFF);
        out11[4] = static_cast<uint8_t>((forceZones >> 8) & 0xFF);
        out11[5] = static_cast<uint8_t>((forceZones >> 16) & 0xFF);
        out11[6] = static_cast<uint8_t>((forceZones >> 24) & 0xFF);
        break;
    }
    case TriggerMode::Weapon: {
        uint8_t start = ClampU8(NormalizePos09(cfg.p0), 0, 7);
        uint8_t end = ClampU8(NormalizePos09(cfg.p1), start + 1, 8);
        uint8_t str = NormalizeStrength08(cfg.p2);
        if (str == 0) { out11[0] = 0x05; break; }
        uint16_t zones = static_cast<uint16_t>((1u << start) | (1u << end));
        out11[0] = 0x25;
        out11[1] = static_cast<uint8_t>(zones & 0xFF);
        out11[2] = static_cast<uint8_t>((zones >> 8) & 0xFF);
        out11[3] = ClampU8(str - 1, 0, 7);
        break;
    }
    case TriggerMode::SlopeFeedback: {
        uint8_t start = ClampU8(NormalizePos09(cfg.p0), 0, 8);
        uint8_t end = ClampU8(NormalizePos09(cfg.p1), start + 1, 9);
        uint8_t s0 = NormalizeStrength08(cfg.p2);
        uint8_t s1 = NormalizeStrength08(cfg.p3);
        if (s0 == 0 && s1 == 0) { out11[0] = 0x05; break; }
        uint16_t activeZones = 0;
        uint32_t forceZones = 0;
        int span = std::max(1, static_cast<int>(end) - static_cast<int>(start));
        for (int i = start; i <= 9; ++i) {
            float t = std::min(1.0f, static_cast<float>(i - start) / static_cast<float>(span));
            int interp = static_cast<int>(std::lround((1.0f - t) * s0 + t * s1));
            if (interp <= 0) continue;
            uint8_t f = ClampU8(interp - 1, 0, 7);
            activeZones |= static_cast<uint16_t>(1u << i);
            forceZones |= static_cast<uint32_t>(f & 0x7u) << (3 * i);
        }
        if (activeZones == 0) { out11[0] = 0x05; break; }
        out11[0] = 0x21;
        out11[1] = static_cast<uint8_t>(activeZones & 0xFF);
        out11[2] = static_cast<uint8_t>((activeZones >> 8) & 0xFF);
        out11[3] = static_cast<uint8_t>((forceZones >> 0) & 0xFF);
        out11[4] = static_cast<uint8_t>((forceZones >> 8) & 0xFF);
        out11[5] = static_cast<uint8_t>((forceZones >> 16) & 0xFF);
        out11[6] = static_cast<uint8_t>((forceZones >> 24) & 0xFF);
        break;
    }
    case TriggerMode::Vibration: {
        uint8_t pos = NormalizePos09(cfg.p0);
        uint8_t freq = ClampU8(cfg.p1, 1, 255);
        uint8_t amp = NormalizeStrength08(cfg.p2);
        if (amp == 0) { out11[0] = 0x05; break; }
        uint8_t force = ClampU8(amp - 1, 0, 7);
        uint16_t activeZones = 0;
        uint32_t ampZones = 0;
        for (int i = pos; i <= 9; ++i) {
            activeZones |= static_cast<uint16_t>(1u << i);
            ampZones |= static_cast<uint32_t>(force & 0x7u) << (3 * i);
        }
        out11[0] = 0x26;
        out11[1] = static_cast<uint8_t>(activeZones & 0xFF);
        out11[2] = static_cast<uint8_t>((activeZones >> 8) & 0xFF);
        out11[3] = static_cast<uint8_t>((ampZones >> 0) & 0xFF);
        out11[4] = static_cast<uint8_t>((ampZones >> 8) & 0xFF);
        out11[5] = static_cast<uint8_t>((ampZones >> 16) & 0xFF);
        out11[6] = static_cast<uint8_t>((ampZones >> 24) & 0xFF);
        out11[9] = freq;
        break;
    }
    case TriggerMode::MultiPosFeedback: {
        uint16_t activeZones = 0;
        uint32_t forceZones = 0;
        for (int i = 0; i < 10; ++i) {
            uint8_t str = NormalizeStrength08(cfg.zones[i]);
            if (str == 0) continue;
            uint8_t f = ClampU8(str - 1, 0, 7);
            activeZones |= static_cast<uint16_t>(1u << i);
            forceZones |= static_cast<uint32_t>(f & 0x7u) << (3 * i);
        }
        if (activeZones == 0) { out11[0] = 0x05; break; }
        out11[0] = 0x21;
        out11[1] = static_cast<uint8_t>(activeZones & 0xFF);
        out11[2] = static_cast<uint8_t>((activeZones >> 8) & 0xFF);
        out11[3] = static_cast<uint8_t>((forceZones >> 0) & 0xFF);
        out11[4] = static_cast<uint8_t>((forceZones >> 8) & 0xFF);
        out11[5] = static_cast<uint8_t>((forceZones >> 16) & 0xFF);
        out11[6] = static_cast<uint8_t>((forceZones >> 24) & 0xFF);
        break;
    }
    case TriggerMode::MultiPosVibration: {
        uint8_t freq = ClampU8(cfg.p0, 1, 255);
        uint16_t activeZones = 0;
        uint32_t ampZones = 0;
        for (int i = 0; i < 10; ++i) {
            uint8_t amp = NormalizeStrength08(cfg.zones[i]);
            if (amp == 0) continue;
            uint8_t f = ClampU8(amp - 1, 0, 7);
            activeZones |= static_cast<uint16_t>(1u << i);
            ampZones |= static_cast<uint32_t>(f & 0x7u) << (3 * i);
        }
        if (activeZones == 0) { out11[0] = 0x05; break; }
        out11[0] = 0x26;
        out11[1] = static_cast<uint8_t>(activeZones & 0xFF);
        out11[2] = static_cast<uint8_t>((activeZones >> 8) & 0xFF);
        out11[3] = static_cast<uint8_t>((ampZones >> 0) & 0xFF);
        out11[4] = static_cast<uint8_t>((ampZones >> 8) & 0xFF);
        out11[5] = static_cast<uint8_t>((ampZones >> 16) & 0xFF);
        out11[6] = static_cast<uint8_t>((ampZones >> 24) & 0xFF);
        out11[9] = freq;
        break;
    }
    }
}

inline void BuildUsbOutputReport(
    uint8_t* buf48,
    uint8_t rumbleLeft,
    uint8_t rumbleRight,
    const TriggerConfig& l2,
    const TriggerConfig& r2,
    uint8_t red,
    uint8_t green,
    uint8_t blue,
    uint8_t playerLeds,
    bool sendRumble = true,
    bool sendR2 = true,
    bool sendL2 = true,
    bool sendLed = true,
    bool sendPlayerLeds = true)
{
    std::memset(buf48, 0, USB_OUTPUT_REPORT_LEN);
    buf48[0] = 0x02; // USB Report ID
    buf48[1] = (sendRumble ? 0x03 : 0x00) | (sendR2 ? 0x04 : 0x00) | (sendL2 ? 0x08 : 0x00);
    buf48[2] = (sendLed ? 0x04 : 0x00) | (sendPlayerLeds ? 0x10 : 0x00);
    if (sendRumble) {
        buf48[3] = rumbleRight;
        buf48[4] = rumbleLeft;
    }
    if (sendR2) {
        EncodeTriggerBlock(&buf48[11], r2);
    }
    if (sendL2) {
        EncodeTriggerBlock(&buf48[22], l2);
    }
    buf48[42] = 0x00; // lightbar_setup = 0 (do not set LIGHT_OUT 0x02)
    buf48[43] = 0x00;
    if (sendPlayerLeds) {
        buf48[44] = playerLeds & 0x1F;
    }
    if (sendLed) {
        buf48[45] = red;
        buf48[46] = green;
        buf48[47] = blue;
    }
}

} // namespace ds
