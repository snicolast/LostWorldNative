// The weather's sound (weather_sound.cpp): WAV files of several formats read
// and resampled, the rain fading in and out, a strike's clap arriving late,
// the level and the voices staying bounded. With a folder argument it also
// renders a minute of rain and strikes from that folder's samples to
// weather_demo.wav, to listen to the mix.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "weather_sound.h"

static int g_failed = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAILED line %d: %s\n", __LINE__, #cond);            \
            g_failed++;                                                 \
        }                                                               \
    } while (0)

namespace {

void write_wav(const std::string& path, int rate, int channels, int bits, const std::vector<float>& samples) {
    FILE* f = fopen(path.c_str(), "wb");
    auto put32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto put16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    uint32_t data = uint32_t(samples.size()) * uint32_t(bits / 8);
    fwrite("RIFF", 1, 4, f);
    put32(36 + 12 + data);
    fwrite("WAVE", 1, 4, f);
    fwrite("bext", 1, 4, f);   // a chunk to skip, as recorders write
    put32(4);
    put32(0);
    fwrite("fmt ", 1, 4, f);
    put32(16);
    put16(1);
    put16(uint16_t(channels));
    put32(uint32_t(rate));
    put32(uint32_t(rate * channels * bits / 8));
    put16(uint16_t(channels * bits / 8));
    put16(uint16_t(bits));
    fwrite("data", 1, 4, f);
    put32(data);
    for (float s : samples) {
        int32_t v = int32_t(std::lround(s * (bits == 16 ? 32767.0 : 8388607.0)));
        fwrite(&v, size_t(bits / 8), 1, f);   // little-endian: the low bytes
    }
    fclose(f);
}

// Mixes `seconds` into a silent 4-channel buffer; returns the RMS of the front left per tenth of a second
std::vector<float> run(WeatherSound& w, float seconds, std::vector<float>* all = nullptr) {
    std::vector<float> rms;
    const int block = 4410;
    std::vector<float> buf(size_t(block) * 4);
    for (int b = 0; b < int(seconds * 10); b++) {
        std::fill(buf.begin(), buf.end(), 0.0f);
        w.mix(buf.data(), block, 1.0f);
        double acc = 0;
        for (int i = 0; i < block; i++) acc += double(buf[size_t(i) * 4]) * buf[size_t(i) * 4];
        rms.push_back(float(std::sqrt(acc / block)));
        if (all) all->insert(all->end(), buf.begin(), buf.end());
    }
    return rms;
}

}   // namespace

int main(int argc, char** argv) {
    if (argc > 1) {   // a demo of the real samples
        WeatherSound w;
        if (!w.load(argv[1])) return 1;
        std::vector<float> all;
        w.set_rain(0.6f);
        run(w, 10, &all);
        w.set_rain(1.0f);
        for (int i = 0; i < 8; i++) {
            w.strike(100 + i * 3);
            run(w, 5.5f, &all);
        }
        w.set_rain(0);
        run(w, 6, &all);
        std::vector<float> st;
        for (size_t i = 0; i < all.size(); i += 4) st.push_back(0.5f * (all[i] + all[i + 2])), st.push_back(0.5f * (all[i + 1] + all[i + 3]));
        float peak = 0;
        for (float s : st) peak = std::max(peak, std::fabs(s));
        printf("demo: %.1f s, peak %.2f\n", double(st.size()) / 2 / 44100, double(peak));
        write_wav("weather_demo.wav", 44100, 2, 16, st);
        return 0;
    }

    const std::string dir = "weather_sound_test_tmp";
    std::filesystem::create_directories(dir);
    {   // rain: 16-bit mono at 48 kHz, a steady tone; thunder: 24-bit stereo at 22.05 kHz, a burst
        std::vector<float> rain(48000 * 2);
        for (size_t i = 0; i < rain.size(); i++) rain[i] = 0.5f * float(std::sin(double(i) * 0.05));
        write_wav(dir + "/rain_loop.wav", 48000, 1, 16, rain);
        std::vector<float> clap(22050 * 2 * 2);
        for (size_t i = 0; i < clap.size(); i++) clap[i] = 0.8f * float(std::sin(double(i) * 0.01)) * float(std::exp(-double(i) / 40000.0));
        write_wav(dir + "/thunder_1.wav", 22050, 2, 24, clap);
        write_wav(dir + "/thunder_2.wav", 22050, 2, 24, clap);
    }
    WeatherSound w;
    CHECK(w.load(dir));
    {   // no rain, no strike: silence
        auto r = run(w, 1);
        CHECK(r.back() == 0.0f);
    }
    {   // the rain fades in over a second or two, then holds
        w.set_rain(1);
        auto r = run(w, 6);
        CHECK(r[0] < r[10] && r[10] < r[59]);
        CHECK(std::fabs(r[59] - r[50]) < 0.02f * r[59]);
        CHECK(r[59] > 0.1f && r[59] < 0.5f);
        // and out again
        w.set_rain(0);
        r = run(w, 8);
        CHECK(r.back() < 0.01f * r[0]);
        r = run(w, 10);
        CHECK(r.back() == 0.0f);   // gone, not just faint
    }
    {   // a strike: silence first (sound is slower than light), then the clap, then silence again
        w.strike(12345);
        auto r = run(w, 6);
        CHECK(r[0] == 0.0f);
        float peak = 0;
        for (float x : r) peak = std::max(peak, x);
        CHECK(peak > 0.05f);
        CHECK(r.back() == 0.0f);
    }
    {   // many strikes at once: bounded voices, no runaway level, no NaN
        for (int i = 0; i < 20; i++) w.strike(i);
        std::vector<float> all;
        run(w, 4, &all);
        bool finite = true;
        float peak = 0;
        for (float s : all) finite = finite && std::isfinite(s), peak = std::max(peak, std::fabs(s));
        CHECK(finite && peak < 4.0f);
    }
    {   // level 0: nothing, whatever happens
        w.set_level(0);
        w.set_rain(1);
        w.strike(7);
        auto r = run(w, 4);
        CHECK(r.back() == 0.0f);
    }
    {   // no files: it stays silent and says so
        WeatherSound none;
        CHECK(!none.load(dir + "/missing"));
        none.set_rain(1);
        none.strike(1);
        auto r = run(none, 1);
        CHECK(r.back() == 0.0f);
    }
    printf(g_failed ? "weather_sound_test: %d FAILED\n" : "weather_sound_test: ok\n", g_failed);
    return g_failed ? 1 : 0;
}
