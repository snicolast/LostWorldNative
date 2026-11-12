#include "menu_music.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

// The MP3 decoder lives here: nothing else in the game uses it. The tests
// that build this file get it with it
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD   // decoded once, on a thread of its own: plain C is plenty
#include "minimp3.h"

namespace {

bool read_file(const char* path, std::vector<uint8_t>& bytes) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = size > 0 && size < (256l << 20);
    if (ok) {
        bytes.resize(size_t(size));
        ok = fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
    }
    fclose(f);
    return ok;
}

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

// 16-bit PCM, one or two channels
bool decode_wav(const std::vector<uint8_t>& b, std::vector<float>& pcm, int& rate) {
    if (b.size() < 12 || memcmp(b.data(), "RIFF", 4) != 0 || memcmp(b.data() + 8, "WAVE", 4) != 0) return false;
    int channels = 0, bits = 0, format = 0;
    for (size_t p = 12; p + 8 <= b.size();) {
        size_t size = le32(&b[p + 4]), body = p + 8;
        size = std::min(size, b.size() - body);
        if (memcmp(&b[p], "fmt ", 4) == 0 && size >= 16) {
            format = le16(&b[body]);
            channels = le16(&b[body + 2]);
            rate = int(le32(&b[body + 4]));
            bits = le16(&b[body + 14]);
            if (format == 0xFFFE && size >= 26) format = le16(&b[body + 24]);   // extensible: the real format
        } else if (memcmp(&b[p], "data", 4) == 0) {
            if (format != 1 || bits != 16 || channels < 1 || channels > 2 || rate < 8000 || rate > 192000) return false;
            size_t frames = size / (size_t(channels) * 2);
            pcm.resize(frames * 2);
            for (size_t i = 0; i < frames; i++) {
                const uint8_t* s = &b[body + i * size_t(channels) * 2];
                float l = float(int16_t(le16(s))) / 32768.0f;
                float r = channels == 2 ? float(int16_t(le16(s + 2))) / 32768.0f : l;
                pcm[i * 2] = l;
                pcm[i * 2 + 1] = r;
            }
            return frames > 0;
        }
        p = body + size + (size & 1);
    }
    return false;
}

bool decode_mp3(const std::vector<uint8_t>& b, std::vector<float>& pcm, int& rate) {
    size_t pos = 0;
    // An ID3v2 tag (it can carry a cover picture) is skipped whole
    if (b.size() > 10 && memcmp(b.data(), "ID3", 3) == 0)
        pos = 10 + (size_t(b[6] & 0x7F) << 21 | size_t(b[7] & 0x7F) << 14 | size_t(b[8] & 0x7F) << 7 | size_t(b[9] & 0x7F));
    mp3dec_t dec;
    mp3dec_init(&dec);
    rate = 0;
    while (pos < b.size()) {
        mp3d_sample_t frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
        mp3dec_frame_info_t info{};
        int n = mp3dec_decode_frame(&dec, b.data() + pos, int(std::min<size_t>(b.size() - pos, 1 << 20)), frame, &info);
        if (info.frame_bytes == 0) break;
        pos += size_t(info.frame_bytes);
        if (n <= 0 || info.hz <= 0) continue;
        if (!rate) rate = info.hz;
        if (info.hz != rate) continue;   // a stray frame at another rate
        for (int i = 0; i < n; i++) {
            float l = float(frame[i * info.channels]) / 32768.0f;
            float r = info.channels > 1 ? float(frame[i * info.channels + 1]) / 32768.0f : l;
            pcm.push_back(l);
            pcm.push_back(r);
        }
    }
    return rate > 0 && !pcm.empty();
}

}  // namespace

bool MenuMusic::load(const char* path) {
    if (ready_) return true;
    std::vector<uint8_t> bytes;
    if (!read_file(path, bytes)) return false;
    std::vector<float> pcm;
    int rate = 0;
    if (!decode_wav(bytes, pcm, rate)) {
        pcm.clear();
        if (!decode_mp3(bytes, pcm, rate)) return false;
    }
    pcm_ = std::move(pcm);
    rate_ = rate;
    ready_.store(true, std::memory_order_release);
    return true;
}

bool MenuMusic::open() {
    if (loader_.joinable() || ready_) return true;
    for (const char* path : {"third_party/menu/music.mp3", "third_party/menu/music.wav", "res/menu/music.mp3"}) {
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        fclose(f);
        path_ = path;
        loader_ = std::thread([this] {
            if (load(path_.c_str())) printf("[menu] music %s (%.0f s at %d Hz)\n", path_.c_str(), double(length()) / rate_, rate_);
            else fprintf(stderr, "[menu] could not read the music %s: it is not a 16-bit WAV or an MP3\n", path_.c_str());
        });
        return true;
    }
    return false;
}

void MenuMusic::close() {
    want_ = false;
    if (loader_.joinable()) loader_.join();
}

void MenuMusic::update(bool want, int volume, double) {
    // A square-root curve so it sits a little louder in the middle: the game's
    // volume 5 (the default) plays it at 0.71, 10 is still full
    volume_.store(std::sqrt(std::clamp(float(volume) / 10.0f, 0.0f, 1.0f)), std::memory_order_relaxed);
    want_.store(want, std::memory_order_relaxed);
}

void MenuMusic::mix(float* out, size_t frames, int channels, int rate) {
    if (!ready_.load(std::memory_order_acquire) || channels < 2 || rate <= 0) return;
    const bool want = want_.load(std::memory_order_relaxed);
    if (!want && level_ <= 0) return;   // faded out: it waits where it was
    const float volume = volume_.load(std::memory_order_relaxed);
    // A second to come in, half a second to go when the game starts
    const float up = 1.0f / float(rate), down = 2.0f / float(rate);
    const double step = double(rate_) / rate;
    const size_t n = pcm_.size() / 2;
    for (size_t i = 0; i < frames; i++) {
        level_ = want ? std::min(1.0f, level_ + up) : std::max(0.0f, level_ - down);
        if (level_ <= 0 && !want) break;
        size_t a = size_t(pos_) % n, b = (a + 1) % n;   // the end runs into the start: it loops
        float t = float(pos_ - std::floor(pos_)), g = level_ * volume;
        float* o = out + i * size_t(channels);
        for (int c = 0; c < 2; c++) {
            float v = pcm_[a * 2 + c] + (pcm_[b * 2 + c] - pcm_[a * 2 + c]) * t;
            o[c] = std::clamp(o[c] + v * g, -1.0f, 1.0f);
        }
        pos_ += step;
        if (pos_ >= double(n)) pos_ -= double(n);
    }
}
