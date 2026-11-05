// A splash video with its sound (res/menu/splash_snicolast.mp4), played by
// Windows (the Media Foundation media engine, no other library): the sound
// goes to the default device by itself, the pictures come back as BGRA for
// the menu to draw. Any format Windows plays (MP4 with H.264 and AAC).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

class SplashVideo {
public:
    SplashVideo() = default;
    ~SplashVideo() { close(); }
    SplashVideo(const SplashVideo&) = delete;
    SplashVideo& operator=(const SplashVideo&) = delete;

    // Starts loading it (paused); false when Windows has no media engine or
    // the file is not there. A file Windows cannot play shows up as failed()
    bool open(const std::wstring& path);
    void close();
    bool is_open() const { return impl_ != nullptr; }

    void play();
    void set_volume(float v);   // 0..1
    bool failed() const;        // Windows gave up on the file
    bool ended() const;         // played to its end (the last picture stays)
    double position() const;    // seconds into it
    bool size(int& w, int& h) const;   // its own size, once known

    // The picture now, scaled to w x h into px; false when there is none yet,
    // or (when `always` is false) when it has not changed since the last call
    bool frame(int w, int h, std::vector<uint32_t>& px, bool always);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
