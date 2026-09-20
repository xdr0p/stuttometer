#include "sound_cues.hpp"

#include <windows.h>
#include <mmsystem.h>

#include <vector>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <mutex>

namespace stuttometer::gui {

// Pure Tone Synthesizer for High-Precision Audio Cues (Zero disk files, zero latency, in-memory WASAPI/PlaySound)
std::vector<uint8_t> generate_sine_wav(float freq_hz, float duration_sec, float volume) {
    if (freq_hz <= 0.0f || duration_sec <= 0.0f || volume <= 0.0f) {
        return {};
    }
    volume = std::clamp(volume, 0.0f, 1.0f);

    uint32_t sample_rate = 44100;
    uint32_t total_samples = static_cast<uint32_t>(sample_rate * duration_sec);
    if (total_samples == 0) {
        return {};
    }
    uint32_t data_size = total_samples * sizeof(int16_t);
    uint32_t overall_size = 36 + data_size;

    std::vector<uint8_t> buffer(44 + data_size);
    uint8_t* p = buffer.data();

    // RIFF Header
    std::memcpy(p + 0, "RIFF", 4);
    std::memcpy(p + 4, &overall_size, 4);
    std::memcpy(p + 8, "WAVE", 4);

    // fmt chunk
    std::memcpy(p + 12, "fmt ", 4);
    uint32_t fmt_size = 16;
    uint16_t audio_format = 1; // PCM
    uint16_t num_channels = 1;
    uint32_t byte_rate = sample_rate * 2;
    uint16_t block_align = 2;
    uint16_t bits_per_sample = 16;

    std::memcpy(p + 16, &fmt_size, 4);
    std::memcpy(p + 20, &audio_format, 2);
    std::memcpy(p + 22, &num_channels, 2);
    std::memcpy(p + 24, &sample_rate, 4);
    std::memcpy(p + 28, &byte_rate, 4);
    std::memcpy(p + 32, &block_align, 2);
    std::memcpy(p + 34, &bits_per_sample, 2);

    // data chunk
    std::memcpy(p + 36, "data", 4);
    std::memcpy(p + 40, &data_size, 4);

    int16_t* samples = reinterpret_cast<int16_t*>(p + 44);
    uint32_t fade_samples = std::min(static_cast<uint32_t>(sample_rate * 0.008f), total_samples / 2); // anti-click fade envelope

    constexpr float pi = std::numbers::pi_v<float>;

    for (uint32_t i = 0; i < total_samples; ++i) {
        float gain = 1.0f;
        if (i < fade_samples) {
            gain = static_cast<float>(i) / fade_samples;
        } else if (i > total_samples - fade_samples) {
            gain = static_cast<float>(total_samples - i) / fade_samples;
        }
        float t = static_cast<float>(i) / sample_rate;
        float sample_val = std::sin(2.0f * pi * freq_hz * t);
        float decayed_gain = gain * (1.0f - 0.20f * (static_cast<float>(i) / total_samples));
        int16_t sample_i16 = static_cast<int16_t>(sample_val * decayed_gain * volume * 32767.0f);
        samples[i] = sample_i16;
    }

    return buffer;
}

static std::vector<uint8_t> g_wav_start;
static std::vector<uint8_t> g_wav_stop;
static std::once_flag g_wav_init_once;

// Pure Tone Player via memory PlaySoundW (Non-blocking async GDI thread safe)
void play_capture_sound(bool starting) {
    std::call_once(g_wav_init_once, []() {
        // Start: Crisp, comfortable continuous tone (1000 Hz, 100ms, 40% volume)
        g_wav_start = generate_sine_wav(1000.0f, 0.10f, 0.40f);
        // Stop: Softer, easily audible completion tone (700 Hz, 90ms, 28% volume)
        g_wav_stop  = generate_sine_wav(700.0f, 0.09f, 0.28f);
    });

    const auto& wav = starting ? g_wav_start : g_wav_stop;
    if (!wav.empty()) {
        // SND_MEMORY treats pszSound as a direct pointer to in-memory WAV byte data (cast via const void*)
        PlaySoundW(reinterpret_cast<LPCWSTR>(static_cast<const void*>(wav.data())), NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }
}

} // namespace stuttometer::gui
