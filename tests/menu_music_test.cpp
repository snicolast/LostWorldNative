// The menus' music (src/ui/menu_music.cpp): a WAV and the game's MP3 are read, mixed into the
// output at another rate, fades in and out, pauses where it was and loops
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "menu_music.h"

static int g_failed = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAILED line %d: %s\n", __LINE__, #cond);            \
            g_failed++;                                                 \
        }                                                               \
    } while (0)

// A 16-bit WAV of `frames` frames at `hz`: a 440 Hz tone at half scale
static void write_wav(const char* path, int hz, int channels, int frames) {
    FILE* f = fopen(path, "wb");
    auto put32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto put16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    uint32_t data = uint32_t(frames * channels * 2);
    fwrite("RIFF", 1, 4, f);
    put32(36 + data);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(16);
    put16(1);
    put16(uint16_t(channels));
    put32(uint32_t(hz));
    put32(uint32_t(hz * channels * 2));
    put16(uint16_t(channels * 2));
    put16(16);
    fwrite("data", 1, 4, f);
    put32(data);
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < channels; c++) put16(uint16_t(int16_t(16384 * std::sin(2 * 3.14159265358979 * 440 * i / hz))));
    fclose(f);
}

static float peak(const std::vector<float>& v, size_t from, size_t to, int channels, int channel) {
    float p = 0;
    for (size_t i = from; i < to; i++) p = std::max(p, std::fabs(v[i * size_t(channels) + size_t(channel)]));
    return p;
}

int main(int argc, char** argv) {
    const char* path = "menu_music_test.wav";
    const int rate = 48000;

    {   // not a sound file
        FILE* f = fopen(path, "wb");
        fputs("this is not music", f);
        fclose(f);
        MenuMusic m;
        CHECK(!m.load(path));
        CHECK(!m.load("there_is_no_such_file.wav"));
    }

    write_wav(path, 32000, 2, 16000);   // half a second, as the game's own tune: 32 kHz stereo
    MenuMusic m;
    std::vector<float> out(size_t(rate) * 4, 0.0f);   // 1 s, 4 channels
    m.mix(out.data(), rate, 4, rate);                 // nothing loaded: nothing added
    CHECK(peak(out, 0, rate, 4, 0) == 0);
    CHECK(m.load(path));
    CHECK(m.rate() == 32000 && m.length() == 16000);

    m.mix(out.data(), rate, 4, rate);   // loaded but not wanted yet
    CHECK(peak(out, 0, rate, 4, 0) == 0);

    // Wanted at full volume: comes in over a second, on the front pair only,
    // and keeps sounding past the end of the file (it loops)
    m.update(true, 10, 0);
    m.mix(out.data(), rate, 4, rate);
    CHECK(peak(out, 0, 480, 4, 0) < 0.01f);
    CHECK(peak(out, rate - 4800, rate, 4, 0) > 0.4f);
    CHECK(peak(out, rate - 4800, rate, 4, 1) > 0.4f);
    CHECK(peak(out, 0, rate, 4, 2) == 0 && peak(out, 0, rate, 4, 3) == 0);
    std::fill(out.begin(), out.end(), 0.0f);
    m.mix(out.data(), rate, 4, rate);
    CHECK(peak(out, 0, 4800, 4, 0) > 0.45f && peak(out, rate - 4800, rate, 4, 0) > 0.45f);
    CHECK(peak(out, 0, rate, 4, 0) <= 0.51f);   // half scale stays half scale: no gain on the way

    // The game's default volume (5) plays it at 0.71
    m.update(true, 5, 0);
    std::fill(out.begin(), out.end(), 0.0f);
    m.mix(out.data(), rate, 4, rate);
    float half = peak(out, 0, rate, 4, 0);
    CHECK(half > 0.33f && half < 0.37f);

    // Not wanted: gone in half a second, then silent
    m.update(false, 5, 0);
    std::fill(out.begin(), out.end(), 0.0f);
    m.mix(out.data(), rate, 4, rate);
    CHECK(peak(out, 0, 480, 4, 0) > 0.3f);
    CHECK(peak(out, rate / 2 + 480, rate, 4, 0) == 0);

    // Added to what is already there, never past full scale
    m.update(true, 10, 0);
    std::fill(out.begin(), out.end(), 0.9f);
    m.mix(out.data(), rate, 4, rate);
    m.mix(out.data(), rate, 4, rate);
    CHECK(peak(out, 0, rate, 4, 0) <= 1.0f);

    // One channel in the file is heard on both sides
    write_wav(path, 44100, 1, 4410);
    MenuMusic mono;
    CHECK(mono.load(path));
    mono.update(true, 10, 0);
    std::vector<float> two(size_t(rate) * 2 * 2, 0.0f);
    mono.mix(two.data(), size_t(rate) * 2, 2, rate);
    CHECK(peak(two, rate, size_t(rate) * 2, 2, 0) > 0.45f && peak(two, rate, size_t(rate) * 2, 2, 1) > 0.45f);

    remove(path);

    // The tune of the repository (an MP3), when the test is given its path
    if (argc > 1) {
        MenuMusic tune;
        CHECK(tune.load(argv[1]));
        CHECK(tune.rate() == 48000 && tune.length() > size_t(48000) * 60);
    }
    if (g_failed) return printf("%d failed\n", g_failed), 1;
    printf("menu music: ok\n");
    return 0;
}
