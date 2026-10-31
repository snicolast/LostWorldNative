#include "weather_sound.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

uint32_t hash(uint32_t k) {
    k ^= k >> 16;
    k *= 0x7feb352du;
    k ^= k >> 15;
    k *= 0x846ca68bu;
    k ^= k >> 16;
    return k;
}
float hash01(uint32_t k) { return float(hash(k) & 0xFFFFFF) / float(0x1000000); }

const float kRainGain = 0.9f;       // the loop under the game's mix
const float kThunderGain = 1.0f;    // a clap overhead: as loud as a roar
const float kRainFade = 1.5f;       // seconds to follow the rain in or out

}   // namespace

// RIFF WAVE: 8, 16, 24 or 32-bit PCM, or 32-bit float, mono or stereo, any
// rate (resampled to kRate linearly: these are noises, not music)
bool WeatherSound::read_wav(const std::string& path, Clip& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> file;
    uint8_t buf[65536];
    for (size_t got; (got = fread(buf, 1, sizeof buf, f)) > 0;) file.insert(file.end(), buf, buf + got);
    fclose(f);
    auto u16 = [&](size_t at) { return uint32_t(file[at]) | uint32_t(file[at + 1]) << 8; };
    auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
    if (file.size() < 12 || memcmp(&file[0], "RIFF", 4) != 0 || memcmp(&file[8], "WAVE", 4) != 0) return false;
    int format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t* data = nullptr;
    size_t data_size = 0;
    for (size_t at = 12; at + 8 <= file.size();) {
        uint32_t size = u32(at + 4);
        size_t body = at + 8;
        if (size > file.size() - body) size = uint32_t(file.size() - body);
        if (memcmp(&file[at], "fmt ", 4) == 0 && size >= 16) {
            format = int(u16(body));
            channels = int(u16(body + 2));
            rate = u32(body + 4);
            bits = int(u16(body + 14));
            if (format == 0xFFFE && size >= 26) format = int(u16(body + 24));   // WAVE_FORMAT_EXTENSIBLE: its sub-format
        } else if (memcmp(&file[at], "data", 4) == 0) {
            data = &file[body];
            data_size = size;
        }
        at = body + size + (size & 1);
    }
    bool pcm = format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    bool flt = format == 3 && bits == 32;
    if (!data || (!pcm && !flt) || channels < 1 || rate == 0) {
        fprintf(stderr, "[weather] %s: not a WAV file it can read\n", path.c_str());
        return false;
    }
    const int bytes = bits / 8;
    const size_t frames = data_size / size_t(bytes * channels);
    auto sample = [&](size_t frame, int c) -> float {
        const uint8_t* p = data + (frame * size_t(channels) + size_t(std::min(c, channels - 1))) * size_t(bytes);
        if (flt) {
            float v;
            memcpy(&v, p, 4);
            return v;
        }
        switch (bits) {
        case 8: return (float(p[0]) - 128.0f) / 128.0f;
        case 16: return float(int16_t(uint16_t(p[0] | p[1] << 8))) / 32768.0f;
        case 24: return float(int32_t(uint32_t(p[0] << 8 | p[1] << 16 | p[2] << 24)) >> 8) / 8388608.0f;
        default: return float(int32_t(uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24))) / 2147483648.0f;
        }
    };
    const double ratio = double(rate) / kRate;
    const size_t n = size_t(double(frames) / ratio);
    out.pcm.resize(n * 2);
    for (size_t i = 0; i < n; i++) {
        double x = double(i) * ratio;
        size_t a = size_t(x);
        size_t b = std::min(a + 1, frames - 1);
        float t = float(x - double(a));
        for (int c = 0; c < 2; c++) out.pcm[i * 2 + size_t(c)] = sample(a, c) + (sample(b, c) - sample(a, c)) * t;
    }
    return n > 0;
}

bool WeatherSound::load(const std::string& dir) {
    read_wav(dir + "/rain_loop.wav", rain_);
    for (int i = 1; i <= 16; i++) {
        Clip c;
        if (!read_wav(dir + "/thunder_" + std::to_string(i) + ".wav", c)) break;
        thunder_.push_back(std::move(c));
    }
    printf("[weather] sound: %s, %d thunder clap(s)\n", rain_.frames() ? "rain loop" : "no rain loop", int(thunder_.size()));
    return rain_.frames() > 0 || !thunder_.empty();
}

void WeatherSound::set_rain(float amount) { rain_target_ = std::clamp(amount, 0.0f, 1.0f); }

void WeatherSound::strike(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_.size() < 8) pending_.push_back(id);
}

void WeatherSound::mix(float* frames, int n, float volume) {
    const float level = level_ * volume;
    // New strikes: each one's distance sets how late, how loud and how dull its clap is
    std::vector<int> strikes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        strikes.swap(pending_);
    }
    for (int id : strikes) {
        if (thunder_.empty() || level <= 0) continue;
        uint32_t k = uint32_t(id) * 7919u;
        float d = hash01(k + 1);   // 0 overhead .. 1 far off
        int clip = int(hash01(k + 2) * float(thunder_.size())) % int(thunder_.size());
        if (clip == last_clip_ && thunder_.size() > 1) clip = (clip + 1) % int(thunder_.size());
        last_clip_ = clip;
        // The voice to use: a free one, or the one nearest its end
        Voice* v = &voices_[0];
        float best = -1;
        for (Voice& c : voices_) {
            float done = c.clip ? float(c.pos / double(c.clip->frames())) : 2.0f;
            if (done > best) best = done, v = &c;
        }
        *v = Voice{};
        v->clip = &thunder_[size_t(clip)];
        v->delay = int((0.08f + 2.2f * d * d) * float(kRate));   // sound is slower than light
        v->gain = 1.0f - 0.6f * d;
        v->step = 1.0 - 0.12 * d + 0.06 * (double(hash01(k + 3)) - 0.5);   // farther: a little deeper
        v->pan = 0.2f + 0.6f * hash01(k + 4);
        v->rear = 0.35f + 0.3f * hash01(k + 5);
        float cutoff = 16000.0f * std::pow(1500.0f / 16000.0f, d);
        v->lowpass = 1.0f - std::exp(-6.2831853f * cutoff / float(kRate));
    }

    // The rain loop, following the rain over a second or two
    if (rain_.frames() > 0) {
        float target = rain_target_.load() * level;
        float from = rain_gain_;
        float to = from + (target - from) * (1.0f - std::exp(-float(n) / (kRainFade * float(kRate))));
        rain_gain_ = to;
        const size_t len = rain_.frames();
        if (from > 1e-4f || to > 1e-4f) {
            for (int i = 0; i < n; i++) {
                float g = (from + (to - from) * float(i) / float(n)) * kRainGain;
                size_t p = size_t(rain_pos_);
                float l = rain_.pcm[p * 2] * g, r = rain_.pcm[p * 2 + 1] * g;
                float* f = frames + size_t(i) * 4;
                f[0] += l;
                f[1] += r;
                f[2] += l * 0.7f;   // all around, a little quieter behind
                f[3] += r * 0.7f;
                if (++rain_pos_ >= double(len)) rain_pos_ = 0;
            }
        } else {
            rain_pos_ = std::fmod(rain_pos_ + n, double(len));
        }
    }

    // The thunder
    for (Voice& v : voices_) {
        if (!v.clip) continue;
        const Clip& c = *v.clip;
        const size_t len = c.frames();
        const float g = v.gain * level * kThunderGain;
        const float gl = std::sqrt(1.0f - v.pan) * 1.41421356f, gr = std::sqrt(v.pan) * 1.41421356f;
        for (int i = 0; i < n; i++) {
            if (v.delay > 0) {
                v.delay--;
                continue;
            }
            size_t a = size_t(v.pos);
            if (a + 1 >= len) {
                v.clip = nullptr;
                break;
            }
            float t = float(v.pos - double(a));
            for (int ch = 0; ch < 2; ch++) {
                float s = c.pcm[a * 2 + size_t(ch)] + (c.pcm[(a + 1) * 2 + size_t(ch)] - c.pcm[a * 2 + size_t(ch)]) * t;
                v.lp[ch] += (s - v.lp[ch]) * v.lowpass;
            }
            float l = v.lp[0] * g * gl, r = v.lp[1] * g * gr;
            float* f = frames + size_t(i) * 4;
            f[0] += l * (1.0f - v.rear);
            f[1] += r * (1.0f - v.rear);
            f[2] += l * v.rear;
            f[3] += r * v.rear;
            v.pos += v.step;
        }
    }
}
