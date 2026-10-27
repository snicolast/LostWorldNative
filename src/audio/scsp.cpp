#include "scsp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

enum Common {
    C_MVOL = 0, C_RING = 1, C_MIDI = 2, C_MOBUF = 3, C_MONITOR = 4,
    C_TIMA = 12, C_TIMB = 13, C_TIMC = 14, C_SCIEB = 15, C_SCIPD = 16, C_SCIRE = 17,
    C_SCILV0 = 18, C_SCILV1 = 19, C_SCILV2 = 20, C_MCIEB = 21, C_MCIPD = 22, C_MCIRE = 23,
};

struct Tables {
    uint32_t ar[64], dr[64];
    int32_t eg[1024];
    uint32_t pan4l[256][32][8], pan4r[256][32][8];
    int plfo[4][256], alfo[4][256];
    int plfo_scale[8][256];   // 4.12 pitch ratio per (depth, waveform value + 128)
    int alfo_scale[8][256];   // 4.12 gain per (depth, waveform value)
    uint16_t lfo_step[32];

    Tables() {
        static const double ar_ms[64] = {0, 0, 8100, 6900, 6000, 4800, 4000, 3400, 3000, 2400, 2000, 1700, 1500,
            1200, 1000, 860, 760, 600, 500, 430, 380, 300, 250, 220, 190, 150, 130, 110, 95, 76, 63, 55, 47, 38,
            31, 27, 24, 19, 15, 13, 12, 9.4, 7.9, 6.8, 6.0, 4.7, 3.8, 3.4, 3.0, 2.4, 2.0, 1.8, 1.6, 1.3, 1.1,
            0.93, 0.85, 0.65, 0.53, 0.44, 0.40, 0.35, 0, 0};
        static const double dr_ms[64] = {0, 0, 118200, 101300, 88600, 70900, 59100, 50700, 44300, 35500, 29600,
            25300, 22200, 17700, 14800, 12700, 11100, 8900, 7400, 6300, 5500, 4400, 3700, 3200, 2800, 2200, 1800,
            1600, 1400, 1100, 920, 790, 690, 550, 460, 390, 340, 270, 230, 200, 170, 140, 110, 98, 85, 68, 57, 49,
            43, 34, 28, 25, 22, 18, 14, 12, 11, 8.5, 7.1, 6.1, 5.4, 4.3, 3.6, 3.1};
        for (int i = 0; i < 64; i++) {
            if (i < 2) { ar[i] = dr[i] = 0; continue; }
            ar[i] = ar_ms[i] == 0 ? (1024u << 16) : uint32_t(1023.0 * 1000.0 / (44100.0 * ar_ms[i]) * 65536.0);
            dr[i] = uint32_t(1023.0 * 1000.0 / (44100.0 * dr_ms[i]) * 65536.0);
        }
        for (int i = 0; i < 1024; i++) eg[i] = int32_t(4096.0 * std::pow(10.0, (3.0 * (i - 1023) / 32.0) / 20.0));

        static const double tl_db[8] = {0.4, 0.8, 1.5, 3, 6, 12, 24, 48};
        static const double sdl_db[8] = {0, -36, -30, -24, -18, -12, -6, 0};
        for (int tl = 0; tl < 256; tl++) {
            double db = 0;
            for (int b = 0; b < 8; b++)
                if (tl & (1 << b)) db += tl_db[b];
            double tlg = std::pow(10.0, -db / 20.0);
            for (int pan = 0; pan < 32; pan++) {
                double pdb = 3.0 * (pan & 1) + 6.0 * ((pan >> 1) & 1) + 12.0 * ((pan >> 2) & 1) + 24.0 * ((pan >> 3) & 1);
                double p = (pan & 0xF) == 0xF ? 0.0 : std::pow(10.0, -pdb / 20.0);
                double l = (pan & 0x10) ? 1.0 : p, r = (pan & 0x10) ? p : 1.0;
                for (int sdl = 0; sdl < 8; sdl++) {
                    double sg = sdl == 0 ? 0.0 : std::pow(10.0, sdl_db[sdl] / 20.0);
                    pan4l[tl][pan][sdl] = uint32_t(4096.0 * 4.0 * l * tlg * sg);
                    pan4r[tl][pan][sdl] = uint32_t(4096.0 * 4.0 * r * tlg * sg);
                }
            }
        }

        static const double freq[32] = {0.17, 0.19, 0.23, 0.27, 0.34, 0.39, 0.45, 0.55, 0.68, 0.78, 0.92, 1.10,
            1.39, 1.60, 1.87, 2.27, 2.87, 3.31, 3.92, 4.79, 6.15, 7.18, 8.60, 10.8, 14.4, 17.2, 21.5, 28.7, 43.1,
            57.4, 86.1, 172.3};
        for (int i = 0; i < 32; i++) lfo_step[i] = uint16_t(256.0 * (freq[i] * 256.0 / 44100.0));
        for (int i = 0; i < 256; i++) {
            plfo[0][i] = i < 128 ? i : i - 256;
            plfo[1][i] = i < 128 ? 127 : -128;
            plfo[2][i] = i < 64 ? 2 * i : i < 128 ? 255 - 2 * i : i < 192 ? 256 - 2 * i : 2 * i - 511;
            plfo[3][i] = 128 - (std::rand() & 0xFF);
            alfo[0][i] = 255 - i;
            alfo[1][i] = i < 128 ? 255 : 0;
            alfo[2][i] = i < 128 ? 255 - 2 * i : 2 * i - 256;
            alfo[3][i] = std::rand() & 0xFF;
        }
        static const double pscale[8] = {0, 7, 13.5, 27, 55, 112, 230, 494};
        static const double ascale[8] = {0, 0.4, 0.8, 1.5, 3, 6, 12, 24};
        for (int d = 0; d < 8; d++) {
            for (int v = 0; v < 256; v++) {
                double pv = v - 128;
                plfo_scale[d][v] = int(256.0 * std::pow(2.0, (pscale[d] * pv / 128.0) / 1200.0)) << 4;
                alfo_scale[d][v] = int(256.0 * std::pow(10.0, (-ascale[d] * v / 256.0) / 20.0)) << 4;
            }
        }
    }
};

const Tables& tables() {
    static Tables t;
    return t;
}

inline int32_t sext24(int32_t v) { return (v << 8) >> 8; }
inline int32_t sext13(int32_t v) { return (v << 19) >> 19; }

// DSP ring-buffer float format (spec 4.4)
uint16_t pack(int32_t v) {
    uint32_t s = (v >> 23) & 1;
    uint32_t t = (uint32_t(v) ^ (uint32_t(v) << 1)) & 0xFFFFFF;
    int e = 0;
    while (e < 12 && !(t & (0x800000u >> e))) e++;
    uint32_t u = e < 12 ? (uint32_t(v) << e) & 0x3FFFFF : uint32_t(v) << 11;
    uint32_t m = (u >> 11) & 0x7FF;
    return uint16_t(s << 15 | uint32_t(e) << 11 | m);
}

int32_t unpack(uint16_t w) {
    uint32_t s = (w >> 15) & 1, e = (w >> 11) & 0xF, m = w & 0x7FF;
    uint32_t u = m << 11;
    if (e > 11) {
        e = 11;
        u |= s << 22;
    } else {
        u |= (s ^ 1) << 22;
    }
    u |= s << 23;
    return sext24(int32_t(u)) >> e;
}

}  // namespace

// ---- LW_SNDLOG: how each sound is played (to find why one is too quiet) ----
// out/voices.csv: one line per note (registers at key-on, length, loudest
// sample before and after TL/DISDL). out/mix.csv: per chip and second, the
// level of the direct mix and of the effect (DSP) return. Outside the chip
// state, so game states keep their size.
namespace {
struct VoiceLog {
    bool on = false;
    uint64_t start = 0;
    int32_t peak_raw = 0, peak_out = 0, peak_send = 0;
    uint16_t regs[16] = {};
};
struct ChipLog {
    uint64_t clock = 0;
    double dir2 = 0, fx2 = 0;
    int32_t dir_peak = 0, fx_peak = 0, voices = 0;
    int32_t stem[4] = {};   // this sample (L+R): DISDL >= 4 with a DSP send, DISDL >= 4 without, the rest, effect return
    VoiceLog voice[32];
};
bool g_log_checked = false;
FILE* g_voices = nullptr;
FILE* g_mix = nullptr;
FILE* g_stems = nullptr;   // out/stems.raw: 8 x int16 per sample, chip 0 then chip 1
ChipLog g_chip_log[2];

bool sndlog() {
    if (!g_log_checked) {
        g_log_checked = true;
        if (getenv("LW_SNDLOG")) {
            g_voices = fopen("out/voices.csv", "w");
            g_mix = fopen("out/mix.csv", "w");
            g_stems = fopen("out/stems.raw", "wb");
            if (g_voices)
                fprintf(g_voices, "t;dur;chip;slot;sa;lsa;lea;pcm8;lpctl;ssctl;oct;fns;tl;disdl;dipan;imxl;isel;efsdl;sdir;"
                                  "plfos;alfos;mdl;ar;d1r;d2r;rr;dl;mvol;peak_sample;peak_direct;peak_effect\n");
            if (g_mix) fprintf(g_mix, "t;chip;direct_rms;direct_peak;effect_rms;effect_peak;max_voices\n");
        }
    }
    return g_voices != nullptr;
}

void log_voice_end(int chip, int s, uint16_t mvol) {
    ChipLog& c = g_chip_log[chip];
    VoiceLog& v = c.voice[s];
    if (!v.on) return;
    v.on = false;
    const uint16_t* r = v.regs;
    fprintf(g_voices, "%.3f;%.3f;%d;%d;%05X;%04X;%04X;%d;%d;%d;%d;%d;%d;%d;%d;%d;%d;%d;%d;%d;%d;%d;%d;%d;%d;%d;%d;%d;%d;%d;%d\n",
            double(v.start) / 44100.0, double(c.clock - v.start) / 44100.0, chip, s,
            unsigned((r[0] & 0xF) << 16 | r[1]), r[2], r[3], (r[0] >> 4) & 1, (r[0] >> 5) & 3, (r[0] >> 7) & 3,
            (((r[8] >> 11) & 0xF) ^ 8) - 8, r[8] & 0x3FF, r[6] & 0xFF, (r[11] >> 13) & 7, (r[11] >> 8) & 0x1F, r[10] & 7,
            (r[10] >> 3) & 0xF, s < 16 ? (r[11] >> 5) & 7 : -1, (r[6] >> 8) & 1, (r[9] >> 5) & 7, r[9] & 7,
            (r[7] >> 12) & 0xF, r[4] & 0x1F, (r[4] >> 6) & 0x1F, (r[4] >> 11) & 0x1F, r[5] & 0x1F, (r[5] >> 5) & 0x1F,
            mvol & 0xF, v.peak_raw, v.peak_out, v.peak_send);
}
}  // namespace

void Scsp::init(uint8_t* ram, uint32_t ram_size, bool master) {
    ram_ = ram;
    ram_mask_ = ram_size - 1;
    master_ = master;
    tables();
}

uint16_t Scsp::ram16(uint32_t a) const {
    a &= ram_mask_ & ~1u;
    return uint16_t(ram_[a] << 8 | ram_[a + 1]);
}

void Scsp::midi_in(uint8_t b) {
    // 256-byte FIFO: when the 68k falls behind, the new byte is dropped rather
    // than overwriting the oldest one still queued (a torn MIDI message would
    // leave a note hanging)
    if (uint8_t(midi_w_ + 1) == midi_r_) {
        midi_dropped_++;
        return;
    }
    midi_fifo_[midi_w_++] = b;
}

// ---- Register access ----
uint16_t Scsp::read16(uint32_t off) {
    off &= 0xFFFE;
    if (off < 0x400) return slot_regs_[off >> 5][(off & 0x1F) >> 1];
    if (off < 0x430) {
        int w = (off - 0x400) >> 1;
        if (w == C_MIDI) {   // reading MIDI-in pops the FIFO
            uint16_t v = uint16_t((common_[C_MIDI] & 0xFF00) | midi_fifo_[midi_r_]);
            if (midi_r_ != midi_w_) midi_r_++;
            return v;
        }
        if (w == C_MONITOR) {
            const Slot& s = slots_[monitor_slot_];
            uint16_t ca = uint16_t((uint32_t(s.cur) >> 24) & 0xF);
            uint16_t eg = uint16_t(0x1F - std::min(0x1F, s.level >> 21));
            return uint16_t(monitor_slot_ << 11 | ca << 7 | int(s.state) << 5 | eg);
        }
        return common_[w];
    }
    if (off >= 0x600 && off < 0x680) return uint16_t(stack_[(off - 0x600) >> 1]);
    if (off >= 0x700 && off < 0x780) return uint16_t(coef_[(off - 0x700) >> 1]);
    if (off >= 0x780 && off < 0x800) return madrs_[((off - 0x780) >> 1) & 31];
    if (off >= 0x800 && off < 0xC00) return mpro_[(off - 0x800) >> 1];
    auto split = [&](int32_t v) { return uint16_t((off & 2) ? (v & 0xFFFF) : ((v >> 16) & 0xFF)); };
    if (off >= 0xC00 && off < 0xE00) return split(temp_[(off >> 2) & 0x7F]);
    if (off >= 0xE00 && off < 0xE80) return split(mems_[((off - 0xE00) >> 2) & 31]);
    if (off >= 0xE80 && off < 0xEC0) return split(mixs_[((off - 0xE80) >> 2) & 15]);
    if (off >= 0xEC0 && off < 0xEE0) return uint16_t(efreg_[((off - 0xEC0) >> 1) & 15]);
    return 0;
}

uint8_t Scsp::read8(uint32_t off) {
    if ((off & 0xFFFF) >= 0x700) return 0;
    uint16_t v = read16(off);
    return (off & 1) ? uint8_t(v) : uint8_t(v >> 8);
}

void Scsp::write8(uint32_t off, uint8_t v) {
    off &= 0xFFFF;
    uint32_t w = off & ~1u;
    uint16_t cur;
    if (w < 0x400) cur = slot_regs_[w >> 5][(w & 0x1F) >> 1];
    else if (w < 0x430) cur = common_[(w - 0x400) >> 1];
    else if (w >= 0x700 && w < 0x780) cur = uint16_t(coef_[(w - 0x700) >> 1]);
    else if (w >= 0x780 && w < 0x800) cur = madrs_[((w - 0x780) >> 1) & 31];
    else if (w >= 0x800 && w < 0xC00) cur = mpro_[(w - 0x800) >> 1];
    else if (w >= 0x600 && w < 0x680) cur = uint16_t(stack_[(w - 0x600) >> 1]);
    else cur = 0;
    cur = (off & 1) ? uint16_t((cur & 0xFF00) | v) : uint16_t((cur & 0x00FF) | (v << 8));
    write16(w, cur);
}

void Scsp::write16(uint32_t off, uint16_t v) {
    off &= 0xFFFE;
    if (off < 0x400) {
        int s = off >> 5, word = (off & 0x1F) >> 1;
        slot_regs_[s][word] = v;
        if (word == 0 && (v & 0x1000)) {
            key_on_off();
            slot_regs_[s][0] &= ~0x1000;   // KYONEX reads back as 0
        } else if (word == 5) {
            update_slot_rates(s, false);
        } else if (word == 8) {
            update_pitch(s);
        } else if (word == 9) {
            update_lfo(s);
        }
        return;
    }
    if (off < 0x430) {
        write_common((off - 0x400) >> 1, v);
        return;
    }
    if (off >= 0x600 && off < 0x680) stack_[(off - 0x600) >> 1] = int16_t(v);
    else if (off >= 0x700 && off < 0x780) coef_[(off - 0x700) >> 1] = int16_t(v);
    else if (off >= 0x780 && off < 0x800) madrs_[((off - 0x780) >> 1) & 31] = v;
    else if (off >= 0x800 && off < 0xC00) {
        mpro_[(off - 0x800) >> 1] = v;
        if (off == 0xBF0) dsp_running_ = true;   // the 68K starts the DSP by writing step 126
    }
}

void Scsp::write_common(int w, uint16_t v) {
    switch (w) {
    case C_MIDI:
        common_[w] = uint16_t((common_[w] & 0x00FF) | (v & 0xFF00));
        return;
    case C_MOBUF:
        common_[w] = v;   // MIDI out: nothing listens on the PPC side
        return;
    case C_MONITOR:
        monitor_slot_ = (v >> 11) & 0x1F;
        common_[w] = v & 0xF800;
        return;
    case C_TIMA: case C_TIMB: case C_TIMC:
        common_[w] = v;
        timer_[w - C_TIMA] = uint32_t(v & 0xFE) << 8;
        return;
    case C_SCIRE:
        common_[C_SCIPD] &= ~v;
        for (int t = 0; t < 3; t++)
            if (timer_[t] == 0xFFFF) common_[C_SCIPD] |= uint16_t(0x40 << t);
        return;
    case C_MCIRE:
        common_[C_MCIPD] &= ~v;
        return;
    default:
        common_[w] = v;
    }
}

// ---- Interrupts and timers (only the master chip drives the 68000) ----
void Scsp::tick_timers() {
    if (!master_) return;
    for (int t = 0; t < 3; t++) {
        uint32_t& c = timer_[t];
        if (c > 0xFF00) continue;
        int prescale = (common_[C_TIMA + t] >> 8) & 7;
        c += 1u << (8 - prescale);
        if (c > 0xFF00) {
            c = 0xFFFF;
            common_[C_SCIPD] |= uint16_t(0x40 << t);
        }
        common_[C_TIMA + t] = uint16_t((common_[C_TIMA + t] & 0xFF00) | ((c >> 8) & 0xFF));
    }
}

int Scsp::irq_level() {
    if (!master_) return 0;
    uint16_t pend = common_[C_SCIPD];
    if (midi_pending()) pend |= 0x08;
    uint16_t en = common_[C_SCIEB];
    auto lv = [&](int bit) {
        return ((common_[C_SCILV0] >> bit) & 1) | (((common_[C_SCILV1] >> bit) & 1) << 1) |
               (((common_[C_SCILV2] >> bit) & 1) << 2);
    };
    int level = 0;
    if (pend & en & 0x040) level = lv(6);
    if (pend & en & 0x180) level = std::max(level, lv(7));
    if (pend & en & 0x008) level = std::max(level, lv(3));
    return level;
}

// ---- Slots ----
void Scsp::key_on_off() {
    for (int s = 0; s < 32; s++) {
        bool kyonb = sreg(s, 0) & 0x800;
        // A slot whose one-shot sample ran out stopped with its EG still in
        // attack or decay. Keying it on again must restart it (as MAME does):
        // Supermodel waits for RELEASE, so the game's next use of that slot
        // stayed silent until some other key-off happened to come first.
        // That swallowed T-Rex roars and other effects now and then.
        bool stopped = !slots_[s].active && slots_[s].state != RELEASE;
        if (kyonb && (slots_[s].state == RELEASE || stopped)) {
            if (stopped) restarted_++;
            start_slot(s);
        }
        else if (!kyonb) slots_[s].state = RELEASE;
    }
}

void Scsp::update_pitch(int s) {
    int oct = ((sreg(s, 8) >> 11) & 0xF);
    oct = (oct ^ 8) - 8;
    uint32_t fns = sreg(s, 8) & 0x3FF;
    int e = oct + 2;
    slots_[s].step = e >= 0 ? (1024 + fns) << e : (1024 + fns) >> -e;
}

void Scsp::update_slot_rates(int s, bool all) {
    const Tables& t = tables();
    Slot& sl = slots_[s];
    uint16_t w4 = sreg(s, 4), w5 = sreg(s, 5);
    auto clamp63 = [](int v) { return std::clamp(v, 0, 63); };
    if (all) {
        int oct = ((sreg(s, 8) >> 11) & 0xF);
        oct = (oct ^ 8) - 8;
        int krs = (w5 >> 10) & 0xF;
        int base = krs != 0xF ? oct + 2 * krs + ((sreg(s, 8) >> 9) & 1) : 0;
        sl.ar = t.ar[clamp63(base + 2 * (w4 & 0x1F))];
        sl.d1r = t.dr[clamp63(base + 2 * ((w4 >> 6) & 0x1F))];
        sl.d2r = t.dr[clamp63(base + 2 * ((w4 >> 11) & 0x1F))];
        sl.rr = t.dr[clamp63(base + 2 * (w5 & 0x1F))];
        sl.eg_hold = (w4 >> 5) & 1;
    } else {
        sl.rr = t.dr[clamp63(2 * (w5 & 0x1F))];
    }
    sl.dl_thr = 0x1F - ((w5 >> 5) & 0x1F);
}

void Scsp::update_lfo(int s) {
    const Tables& t = tables();
    uint16_t w9 = sreg(s, 9);
    uint16_t step = t.lfo_step[(w9 >> 10) & 0x1F];
    slots_[s].plfo_step = step;
    slots_[s].alfo_step = step;
}

void Scsp::start_slot(int s) {
    Slot& sl = slots_[s];
    uint32_t sa = uint32_t(sreg(s, 0) & 0xF) << 16 | sreg(s, 1);
    bool pcm8 = sreg(s, 0) & 0x10;
    sl.active = true;
    sl.back = false;
    sl.cur = 0;
    sl.nxt = 1 << 12;
    sl.base = pcm8 ? sa : (sa & 0x7FFFE);
    update_pitch(s);
    update_slot_rates(s, true);
    sl.level = 0x17F << 16;
    sl.state = ATTACK;
    update_lfo(s);
    if (sndlog()) {
        int chip = master_ ? 0 : 1;
        log_voice_end(chip, s, common_[C_MVOL]);
        VoiceLog& v = g_chip_log[chip].voice[s];
        v = VoiceLog{};
        v.on = true;
        v.start = g_chip_log[chip].clock;
        std::copy_n(slot_regs_[s], 16, v.regs);
    }
}

void Scsp::stop_slot(int s) {
    if (sndlog()) log_voice_end(master_ ? 0 : 1, s, common_[C_MVOL]);
    slots_[s].active = false;
    sreg(s, 0) &= ~0x800;   // KYONB cleared, visible to the 68K
}

int Scsp::plfo(int s) {
    const Tables& t = tables();
    Slot& sl = slots_[s];
    sl.plfo_phase = uint16_t(sl.plfo_phase + sl.plfo_step);
    uint16_t w9 = sreg(s, 9);
    int v = t.plfo[(w9 >> 8) & 3][sl.plfo_phase >> 8];
    return t.plfo_scale[(w9 >> 5) & 0xE][v + 128];
}

int Scsp::alfo(int s) {
    const Tables& t = tables();
    Slot& sl = slots_[s];
    sl.alfo_phase = uint16_t(sl.alfo_phase + sl.alfo_step);
    uint16_t w9 = sreg(s, 9);
    int v = t.alfo[(w9 >> 3) & 3][sl.alfo_phase >> 8];
    return t.alfo_scale[w9 & 7][v];
}

int32_t Scsp::eg_update(int s) {
    Slot& sl = slots_[s];
    bool lpslnk = sreg(s, 5) & 0x4000;
    switch (sl.state) {
    case ATTACK:
        sl.level += int32_t(sl.ar);
        if (sl.level >= (0x3FF << 16)) {
            sl.level = 0x3FF << 16;
            if (!lpslnk) sl.state = sl.d1r >= (1024u << 16) ? DECAY2 : DECAY1;
        }
        if (sl.eg_hold) return 0xFFC;
        break;
    case DECAY1:
        sl.level = std::max(0, sl.level - int32_t(sl.d1r));
        if ((sl.level >> 21) <= sl.dl_thr) sl.state = DECAY2;
        break;
    case DECAY2:
        if (((sreg(s, 4) >> 11) & 0x1F) == 0) return (sl.level >> 16) << 2;
        sl.level = std::max(0, sl.level - int32_t(sl.d2r));
        break;
    case RELEASE:
        sl.level -= int32_t(sl.rr);
        if (sl.level <= 0) {
            sl.level = 0;
            stop_slot(s);
        }
        break;
    }
    return (sl.level >> 16) << 2;
}

int32_t Scsp::slot_sample(int s) {
    const Tables& t = tables();
    Slot& sl = slots_[s];
    if (!sl.active) return 0;
    uint16_t w0 = sreg(s, 0);
    if ((w0 >> 7) & 3) return 0;   // SSCTL: noise/other sources not implemented
    bool pcm8 = w0 & 0x10;
    uint16_t w7 = sreg(s, 7), w9 = sreg(s, 9);

    uint32_t st = sl.step;
    if ((w9 >> 5) & 0xE) st = uint32_t((uint64_t(sl.step) * uint32_t(plfo(s))) >> 12);

    uint32_t a1, a2;
    if (pcm8) {
        a1 = uint32_t(sl.cur) >> 12;
        a2 = uint32_t(sl.nxt) >> 12;
    } else {
        a1 = (uint32_t(sl.cur) >> 11) & 0x7FFFE;
        a2 = (uint32_t(sl.nxt) >> 11) & 0x7FFFE;
    }
    int mdl = (w7 >> 12) & 0xE, mdxsl = (w7 >> 6) & 0x3F, mdysl = w7 & 0x3F;
    if (mdl || mdxsl || mdysl) {   // FM from recent slot outputs
        int32_t m = (stack_[(stack_ptr_ + mdxsl) & 63] + stack_[(stack_ptr_ + mdysl) & 63]) / 2;
        m = (m << 10) >> (24 - mdl);
        if (pcm8) {
            a1 = (a1 + m) & 0x7FFFF;
            a2 = (a2 + m) & 0x7FFFF;
        } else {
            m <<= 1;
            a1 = (a1 + m) & 0x7FFFE;
            a2 = (a2 + m) & 0x7FFFE;
        }
    }
    int32_t p1, p2;
    if (pcm8) {
        p1 = int32_t(int8_t(ram_[(sl.base + a1) & ram_mask_])) << 8;
        p2 = int32_t(int8_t(ram_[(sl.base + a2) & ram_mask_])) << 8;
    } else {
        p1 = int16_t(ram16(sl.base + a1));
        p2 = int16_t(ram16(sl.base + a2));
    }
    int32_t f = sl.cur & 0xFFF;
    int32_t sample;
    if (cubic_) {   // Catmull-Rom through the samples before a1 and after a2
        int32_t p0, p3;
        if (pcm8) {
            p0 = int32_t(int8_t(ram_[(sl.base + a1 - 1) & ram_mask_])) << 8;
            p3 = int32_t(int8_t(ram_[(sl.base + a2 + 1) & ram_mask_])) << 8;
        } else {
            p0 = int16_t(ram16(sl.base + a1 - 2));
            p3 = int16_t(ram16(sl.base + a2 + 2));
        }
        float x = float(f) * (1.0f / 4096.0f);
        float v = float(p1) + 0.5f * x *
                                  (float(p2 - p0) + x * (float(2 * p0 - 5 * p1 + 4 * p2 - p3) +
                                                         x * float(3 * (p1 - p2) + p3 - p0)));
        sample = std::clamp(int32_t(std::lround(v)), -32768, 32767);
    } else {
        sample = (p1 * (4096 - f) + p2 * f) >> 12;
    }
    int sbctl = (w0 >> 9) & 3;
    if (sbctl & 1) sample ^= 0x7FFF;
    if (sbctl & 2) sample = int16_t(sample ^ 0x8000);

    // Advance and loop
    sl.cur = sl.back ? sl.cur - int32_t(st) : sl.cur + int32_t(st);
    sl.nxt = sl.cur + 4096;
    int32_t lsa = sreg(s, 2), lea = sreg(s, 3);
    if ((sl.cur >> 12) >= lsa && !sl.back && (sreg(s, 5) & 0x4000) && sl.state == ATTACK) sl.state = DECAY1;
    int lpctl = (w0 >> 5) & 3;
    auto loop = [&](int32_t& x) {
        int32_t pos = x >> 12;
        switch (lpctl) {
        case 0:
            if (pos >= lsa && pos >= lea) stop_slot(s);
            break;
        case 1:
            if (pos >= lea) x = (lsa << 12) + (x - (lea << 12));
            break;
        case 2:
            if (pos >= lsa && !sl.back) {
                x = (lea << 12) - (x - (lsa << 12));
                sl.back = true;
            } else if (sl.back && (pos < lsa || x < 0)) {
                x = (lea << 12) - ((lsa << 12) - x);
            }
            break;
        case 3:
            if (pos >= lea) {
                x = (lea << 12) - (x - (lea << 12));
                sl.back = true;
            } else if (sl.back && (pos < lsa || x < 0)) {
                x = (lsa << 12) + ((lsa << 12) - x);
                sl.back = false;
            }
            break;
        }
    };
    loop(sl.cur);
    loop(sl.nxt);

    uint16_t w6 = sreg(s, 6);
    bool sdir = w6 & 0x100;
    if (!sdir) {
        if (w9 & 7) sample = (sample * alfo(s)) >> 12;
        bool attacking = sl.state == ATTACK;
        int32_t eg = eg_update(s);
        sample = attacking ? (sample * eg) >> 12 : (sample * t.eg[(eg >> 2) & 1023]) >> 12;
    }
    if (!(w6 & 0x200)) {   // STWINH = 0: output kept for FM
        int32_t st_val = sdir ? 2 * sample : int32_t((int64_t(sample) * t.pan4l[w6 & 0xFF][0][7]) >> 13);
        stack_[stack_ptr_] = int16_t(st_val);
    }
    return sample;
}

// ---- DSP (spec 4.3) ----
void Scsp::dsp_step() {
    if (!dsp_running_) return;
    memset(efreg_, 0, sizeof(efreg_));
    int32_t acc = 0, shifted = 0, inputs = 0, memval = 0, frc = 0, yreg = 0, adrs = 0;
    uint32_t rbl = 8192u << ((common_[C_RING] >> 7) & 3);
    uint32_t rbp = uint32_t(common_[C_RING] & 0x7F) << 12;
    for (int step = 0; step < 128; step++) {
        const uint16_t* w = &mpro_[step * 4];
        uint32_t tra = (w[0] >> 8) & 0x7F, twt = (w[0] >> 7) & 1, twa = w[0] & 0x7F;
        uint32_t xsel = (w[1] >> 15) & 1, ysel = (w[1] >> 13) & 3, ira = (w[1] >> 6) & 0x3F, iwt = (w[1] >> 5) & 1,
                 iwa = w[1] & 0x1F;
        uint32_t table = (w[2] >> 15) & 1, mwt = (w[2] >> 14) & 1, mrd = (w[2] >> 13) & 1, ewt = (w[2] >> 12) & 1,
                 ewa = (w[2] >> 8) & 0xF, adrl = (w[2] >> 7) & 1, frcl = (w[2] >> 6) & 1, shift = (w[2] >> 4) & 3,
                 yrl = (w[2] >> 3) & 1, negb = (w[2] >> 2) & 1, zero = (w[2] >> 1) & 1, bsel = w[2] & 1;
        uint32_t nofl = (w[3] >> 15) & 1, coef = (w[3] >> 9) & 0x3F, masa = (w[3] >> 2) & 0x1F,
                 adreb = (w[3] >> 1) & 1, nxadr = w[3] & 1;

        if (ira <= 0x1F) inputs = mems_[ira];
        else if (ira <= 0x2F) inputs = mixs_[ira - 0x20] << 4;
        else if (ira <= 0x31) inputs = 0;   // EXTS
        else return;                         // invalid source aborts the program
        inputs = sext24(inputs);

        if (iwt) {
            mems_[iwa] = memval;
            if (ira == iwa) inputs = memval;
        }
        int32_t b = 0;
        if (!zero) {
            b = bsel ? acc : sext24(temp_[(tra + dec_) & 0x7F]);
            if (negb) b = -b;
        }
        int32_t x = xsel ? inputs : sext24(temp_[(tra + dec_) & 0x7F]);
        int32_t y;
        switch (ysel) {
        case 0: y = frc; break;
        case 1: y = coef_[coef] >> 3; break;
        case 2: y = (yreg >> 11) & 0x1FFF; break;
        default: y = (yreg >> 4) & 0x0FFF; break;
        }
        if (yrl) yreg = inputs;
        switch (shift) {
        case 0: shifted = std::clamp(acc, -0x800000, 0x7FFFFF); break;
        case 1: shifted = std::clamp(acc * 2, -0x800000, 0x7FFFFF); break;
        case 2: shifted = sext24(acc * 2); break;
        default: shifted = sext24(acc); break;
        }
        y = sext13(y);
        acc = int32_t((int64_t(x) * y) >> 12) + b;
        if (twt) temp_[(twa + dec_) & 0x7F] = shifted;
        if (frcl) frc = shift == 3 ? (shifted & 0xFFF) : ((shifted >> 11) & 0x1FFF);
        if (mrd || mwt) {
            uint32_t a = madrs_[masa];
            if (!table) a += dec_;
            if (adreb) a += uint32_t(adrs) & 0xFFF;
            if (nxadr) a += 1;
            a &= table ? 0xFFFF : (rbl - 1);
            a += rbp;
            if (a > 0x7FFFF) a = 0;
            if (step & 1) {
                uint32_t byte = (a * 2) & ram_mask_;
                if (mrd) {
                    uint16_t v = uint16_t(ram_[byte] << 8 | ram_[byte + 1]);
                    memval = nofl ? int32_t(v) << 8 : unpack(v);
                }
                if (mwt) {
                    uint16_t v = nofl ? uint16_t(shifted >> 8) : pack(shifted);
                    ram_[byte] = uint8_t(v >> 8);
                    ram_[byte + 1] = uint8_t(v);
                }
            }
        }
        if (adrl) adrs = shift == 3 ? ((shifted >> 12) & 0xFFF) : (inputs >> 16);
        if (ewt) efreg_[ewa] = int16_t(efreg_[ewa] + (shifted >> 8));
    }
    dec_--;
    memset(mixs_, 0, sizeof(mixs_));
}

// ---- One output sample ----
void Scsp::render(float& left, float& right) {
    const Tables& t = tables();
    int32_t l = 0, r = 0;
    for (int s = 0; s < 32; s++) {
        int32_t sample = slot_sample(s);
        stack_ptr_ = (stack_ptr_ + 1) & 63;
        if (!slots_[s].active && sample == 0) continue;
        uint16_t w6 = sreg(s, 6), w10 = sreg(s, 10), w11 = sreg(s, 11);
        int tl = w6 & 0xFF;
        int disdl = (w11 >> 13) & 7, dipan = (w11 >> 8) & 0x1F;
        int imxl = w10 & 7;
        if (disdl) {
            l += int32_t((int64_t(sample) * t.pan4l[tl][dipan][disdl]) >> 12);
            r += int32_t((int64_t(sample) * t.pan4r[tl][dipan][disdl]) >> 12);
        }
        if (imxl) mixs_[(w10 >> 3) & 0xF] += int32_t((int64_t(sample) * t.pan4l[tl][0][imxl]) >> 10);
        if (g_voices) {
            if (disdl) g_chip_log[master_ ? 0 : 1].stem[disdl < 4 ? 2 : imxl ? 0 : 1] += int32_t((int64_t(sample) * (t.pan4l[tl][dipan][disdl] + t.pan4r[tl][dipan][disdl])) >> 13);
            VoiceLog& v = g_chip_log[master_ ? 0 : 1].voice[s];
            v.peak_raw = std::max(v.peak_raw, std::abs(sample));
            if (disdl)
                v.peak_out = std::max(v.peak_out, int32_t(std::max(std::abs((int64_t(sample) * t.pan4l[tl][dipan][disdl]) >> 12),
                                                                   std::abs((int64_t(sample) * t.pan4r[tl][dipan][disdl]) >> 12))));
            if (imxl) v.peak_send = std::max(v.peak_send, int32_t(std::abs((int64_t(sample) * t.pan4l[tl][0][imxl]) >> 12)));
        }
    }
    const int32_t direct_l = l, direct_r = r;
    if (dsp_on_) dsp_step();
    for (int i = 0; i < 16; i++) {
        uint16_t w11 = sreg(i, 11);
        int efsdl = (w11 >> 5) & 7, efpan = w11 & 0x1F;
        if (!efsdl) continue;
        l += int32_t((int64_t(efreg_[i]) * t.pan4l[0][efpan][efsdl]) >> 12);
        r += int32_t((int64_t(efreg_[i]) * t.pan4r[0][efpan][efsdl]) >> 12);
    }
    if (g_voices) {   // levels per second, in the units of l and r
        ChipLog& c = g_chip_log[master_ ? 0 : 1];
        c.stem[3] = ((l - direct_l) + (r - direct_r)) / 2;
        if (!master_ && g_stems) {   // the slave renders second: both chips are done
            int16_t pcm[8];
            for (int k = 0; k < 8; k++) {
                pcm[k] = int16_t(std::clamp(g_chip_log[k / 4].stem[k % 4] / 4, -32768, 32767));
                g_chip_log[k / 4].stem[k % 4] = 0;
            }
            fwrite(pcm, sizeof(pcm), 1, g_stems);
        }
        int32_t dir = std::max(std::abs(direct_l), std::abs(direct_r));
        int32_t fx = std::max(std::abs(l - direct_l), std::abs(r - direct_r));
        c.dir2 += double(dir) * dir;
        c.fx2 += double(fx) * fx;
        c.dir_peak = std::max(c.dir_peak, dir);
        c.fx_peak = std::max(c.fx_peak, fx);
        int n = 0;
        for (const Slot& sl : slots_) n += sl.active;
        c.voices = std::max(c.voices, n);
        if (++c.clock % 44100 == 0 && g_mix) {
            fprintf(g_mix, "%llu;%d;%.0f;%d;%.0f;%d;%d\n", (unsigned long long)(c.clock / 44100), master_ ? 0 : 1,
                    std::sqrt(c.dir2 / 44100), c.dir_peak, std::sqrt(c.fx2 / 44100), c.fx_peak, c.voices);
            fflush(g_mix);
            fflush(g_voices);
            if (g_stems) fflush(g_stems);
            c.dir2 = c.fx2 = 0;
            c.dir_peak = c.fx_peak = c.voices = 0;
        }
    }
    l = std::clamp(l, -131072, 131071);
    r = std::clamp(r, -131072, 131071);
    if (common_[C_MVOL] & 0x100) {   // 18-bit DAC
        left = float(l);
        right = float(r);
    } else {   // 16-bit DAC: the same range, the two low bits kept as a fraction
        left = float(l) * 0.25f;
        right = float(r) * 0.25f;
    }
}
