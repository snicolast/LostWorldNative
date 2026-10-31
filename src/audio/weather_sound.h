// Sound of the weather (not in the original): a loop of rain while our own
// rain falls (the weather of RANDOM SPAWNS and survival) and a clap of
// thunder after each lightning strike. Mixed by the sound board into its four
// channels, after the game's own sound. The samples are res/sound/rain_loop.wav
// and res/sound/thunder_N.wav (N from 1), CC0 recordings from Freesound; without
// them it stays silent. Portable C++ (no Windows).
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class WeatherSound {
public:
    static constexpr int kRate = 44100;

    // Reads the samples from `dir`; false when there is no rain loop and no thunder
    bool load(const std::string& dir);
    // Any thread. How much of our rain falls, 0..1: the loop fades to it over a second or two
    void set_rain(float amount);
    // Any thread. A lightning strike has begun (id: which strike, seeds its
    // distance, so its delay, loudness and the clap picked)
    void strike(int id);
    // Any thread. Its own level (WEATHER SOUND): 0 off, 1 normal
    void set_level(float level) { level_ = level; }
    // The sound board's thread: adds `n` frames of 4 channels (front L, R, rear
    // L, R; full scale = 1), `volume` = the game's volume (1 at the default)
    void mix(float* frames, int n, float volume);

private:
    struct Clip {
        std::vector<float> pcm;   // stereo, interleaved, kRate
        size_t frames() const { return pcm.size() / 2; }
    };
    struct Voice {
        const Clip* clip = nullptr;
        double pos = 0, step = 1;   // frames into the clip, and per output frame (its pitch)
        int delay = 0;              // output frames before it starts
        float gain = 0, pan = 0.5f, rear = 0.5f;
        float lowpass = 1, lp[2] = {};   // distant claps lose their crack: one-pole coefficient and state
    };
    static bool read_wav(const std::string& path, Clip& out);

    Clip rain_;
    std::vector<Clip> thunder_;
    double rain_pos_ = 0;
    float rain_gain_ = 0;           // eased towards rain_target_
    std::atomic<float> rain_target_{0};
    std::atomic<float> level_{1};
    int last_clip_ = -1;
    Voice voices_[4];
    std::mutex mutex_;              // the strikes waiting for the sound board's thread
    std::vector<int> pending_;
};
