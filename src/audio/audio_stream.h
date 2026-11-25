// Queue between the sound board (44.1 kHz, emulation thread) and the audio
// device (its own rate, output thread). The game is paced by the CPU clock and
// the device by its own crystal, so they never agree exactly: the reading side
// resamples with a windowed sinc and nudges the ratio (at most 0.5 %) to keep
// the queue near a fixed level. Gaps and overflows fade out and back in over a
// few milliseconds instead of cutting, so there are no clicks.
// Portable C++ (no Windows): tests/audio_stream_test.cpp drives it directly.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

// Soft limiter on a full-scale-1 signal: linear up to 3/4, then compresses
// smoothly so a raised volume never clips hard
float soft_limit(float v);

// Limiter: when a frame would pass the linear range of soft_limit, the whole
// frame is turned down at once and comes back up over ~0.2 s. Unlike clipping
// each sample, a loud sound (a roar over the music) keeps its shape and stays
// on top instead of being flattened into the rest.
class Limiter {
public:
    static constexpr float kLimit = 0.75f;   // where soft_limit starts to bend
    void set_rate(int rate);
    // Scales `n` samples of one frame in place
    void process(float* frame, int n);

private:
    float gain_ = 1, release_ = 0;
};

class AudioStream {
public:
    static constexpr int kInRate = 44100;
    static constexpr int kInChannels = 4;   // front L, R (master SCSP), rear L, R (slave SCSP)

    // Queue levels in input frames
    static constexpr size_t kCapacity = 1 << 16;   // ~1.5 s
    static constexpr size_t kTarget = 2048;        // ~46 ms: what the rate control aims for
    static constexpr size_t kRefill = 1536;        // after running dry, wait for this much
    static constexpr size_t kMaxFill = 8820;       // 200 ms: beyond this, skip back to kTarget

    AudioStream();
    // Reading side only, before it pulls (or between pulls on the same thread)
    void set_output_rate(int rate);

    // Writing side: frames of kInChannels floats (full scale = 1). If the queue
    // is full the rest is dropped (the reader then skips with a fade).
    void push(const float* frames, size_t count);
    size_t queued() const;

    // How the four channels reach the listener
    enum class Speakers {
        Stereo,       // front and rear mixed on two speakers
        Surround,     // front and rear on their own speakers (4.0, 5.1, 7.1)
        Headphones,   // stereo with crossfeed; the rear pair duller and wider, as from behind
    };
    static int channels(Speakers s) { return s == Speakers::Surround ? 4 : 2; }

    // Reading side: `count` frames of channels(mode) floats at the output rate
    void pull(float* out, size_t count, Speakers mode = Speakers::Stereo);

    // Diagnostics
    double rate_adjust() const { return adjust_; }
    uint32_t underruns() const { return underruns_; }
    uint32_t skips() const { return skips_; }

private:
    void next_input();
    void headphones(const float* in, float* out);

    std::vector<float> ring_;
    std::atomic<size_t> read_{0}, write_{0};

    // Reading side state
    static constexpr int kTaps = 16, kPhases = 256;
    int out_rate_ = 0;
    std::vector<float> table_;                 // (kPhases + 1) x kTaps sinc weights
    float hist_[kTaps * 2][kInChannels] = {};  // last kTaps inputs, stored twice to read without wrapping
    int hpos_ = 0;
    double frac_ = 0, adjust_ = 0, drift_ = 0, avg_fill_ = double(kTarget);
    float gain_ = 0, ramp_ = 0;
    Limiter limiter_;
    bool starved_ = true, skipping_ = false;
    uint32_t underruns_ = 0, skips_ = 0;

    // Headphones: one-pole low-passes and a short delay for the crossfeed
    static constexpr int kDelay = 64;
    float cross_lp_[kInChannels] = {}, rear_lp_[2] = {};
    float delay_[kDelay][kInChannels] = {};
    int dpos_ = 0, dlen_ = 1;
    float a_cross_ = 0, a_rear_ = 0;
};
