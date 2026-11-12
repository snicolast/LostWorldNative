// Music for the menus: your own third_party/menu/music.mp3 (or .wav) if there
// is one, else res/menu/music.mp3 (in the repository, made with Suno), looped while a menu is up over the backdrop (not over a paused
// game) and faded out when the game starts.
// The file is decoded into memory once, on a thread of its own so the game
// does not wait for it, and mixed into the game's own audio output (AudioOut
// calls mix()): it plays wherever the game has sound and needs nothing from
// Windows. Portable C++: tests/menu_music_test.cpp.
#pragma once
#include <atomic>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

class MenuMusic {
public:
    ~MenuMusic() { close(); }
    bool open();    // false: there is no music file. The file is read in the background
    // One file: 16-bit PCM WAV or MP3, by its content. False if it is neither
    bool load(const char* path);
    void close();
    // Every frame: whether the music should be heard, at what volume (0..10, the
    // game's), and the seconds since the last call
    void update(bool want, int volume, double dt);

    // Audio thread: adds the music to `frames` frames of `channels` floats at
    // `rate` Hz (the first two channels are the front pair). Nothing is added
    // until a file is loaded, or while the music is faded out
    void mix(float* out, size_t frames, int channels, int rate);

    int rate() const { return rate_; }
    size_t length() const { return pcm_.size() / 2; }   // frames

private:
    std::thread loader_;
    std::string path_;

    // Written before ready_ is set, only read afterwards
    std::vector<float> pcm_;   // stereo, full scale = 1
    int rate_ = 0;
    std::atomic<bool> ready_{false};

    // Main thread to audio thread
    std::atomic<bool> want_{false};
    std::atomic<float> volume_{0};   // 0..1, the gain for the game's volume

    // Audio thread
    double pos_ = 0;    // frames into pcm_
    float level_ = 0;   // the fade, 0..1
};
