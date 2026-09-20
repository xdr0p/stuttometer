#pragma once

#include <vector>
#include <cstdint>

namespace stuttometer::gui {

/// Synthesize in-memory PCM 16-bit 44.1kHz mono WAV audio byte buffer for pure tones.
std::vector<uint8_t> generate_sine_wav(float freq_hz, float duration_sec, float volume);

/// Play a synthesized, non-blocking asynchronous audio cue for capture start/stop.
void play_capture_sound(bool starting);

} // namespace stuttometer::gui
