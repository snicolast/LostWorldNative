// Model 3 sound board: a 68000 (Musashi) driving two SCSPs. The PowerPC talks
// to it through the MIDI port at 0xF0080000. See docs/sound_spec.md.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "audio_stream.h"
#include "scsp.h"
#include "weather_sound.h"

class AudioOut;

class SoundBoard {
public:
    bool init(std::vector<uint8_t> program, std::vector<uint8_t> samples, AudioOut* out);
    void midi_write(uint8_t b) { scsp_[0].midi_in(b); }
    // Runs the sound board for one PowerPC frame (keeps 44.1 kHz in sync)
    void run_frame(double frame_hz);
    // Debugging: also write everything generated to a 16-bit stereo WAV file
    void dump_wav(const char* path);
    void close_wav();
    void set_volume(float gain) { gain_ = gain; }
    // Level of the rear pair (slave SCSP), in dB, quieter or louder: on the cabinet those
    // speakers sit in the seat by the player's head, and the T-Rex roars and
    // many effects play there
    void set_rear_boost(int db) { rear_gain_ = std::pow(10.0f, float(db) / 20.0f); }
    void set_dsp_enabled(bool on) { scsp_[0].set_dsp_enabled(on); scsp_[1].set_dsp_enabled(on); }
    // Muted: the board keeps running but nothing reaches the output (the game
    // fast-forwarding behind a loading screen); unmuted, it fades in over 20 ms
    void set_muted(bool m) { muted_ = m; }
    // The rain and thunder of our weather, mixed over the game's sound
    WeatherSound& weather() { return weather_; }
    uint64_t frames_out() const { return frames_out_; }   // frames sent to the output so far
    // Game states (boss mode, runtime.cpp): the 68000, the SCSPs and their RAM.
    // save_state leaves `out` empty when the board is not running.
    void save_state(std::vector<uint8_t>& out) const;
    bool load_state(const std::vector<uint8_t>& in);

    // 68000 bus (called from the Musashi callbacks)
    uint8_t read8(uint32_t a);
    uint16_t read16(uint32_t a);
    void write8(uint32_t a, uint8_t v);
    void write16(uint32_t a, uint16_t v);

private:
    std::vector<uint8_t> program_, samples_;
    std::vector<uint8_t> ram_[2];
    Scsp scsp_[2];
    WeatherSound weather_;
    uint8_t ctrl_ = 0;
    AudioOut* out_ = nullptr;
    double sample_acc_ = 0;
    int overshoot_ = 0;
    std::vector<float> buffer_;
    bool ready_ = false;
    float rear_gain_ = 2.0f;   // +6 dB
    float gain_ = 5.0f;   // the original mix peaks around a quarter of full scale
    std::atomic<bool> muted_{false};
    bool was_muted_ = false;
    int fade_ = 0;        // samples of fade-in left
    std::atomic<uint64_t> frames_out_{0};
    FILE* wav_ = nullptr;
    uint32_t wav_frames_ = 0;
    Limiter wav_limiter_;
};
