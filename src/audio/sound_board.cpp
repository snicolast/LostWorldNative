#include "sound_board.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "audio_out.h"
#include "audio_stream.h"

extern "C" {
#include "m68k.h"
}

namespace {
SoundBoard* g_board = nullptr;

constexpr uint32_t kRamSize = 0x100000;   // 1 MB per SCSP
}  // namespace

// Musashi bus callbacks
extern "C" {
unsigned int m68k_read_memory_8(unsigned int a) { return g_board->read8(a & 0xFFFFFF); }
unsigned int m68k_read_memory_16(unsigned int a) { return g_board->read16(a & 0xFFFFFF); }
unsigned int m68k_read_memory_32(unsigned int a) {
    return (unsigned(g_board->read16(a & 0xFFFFFF)) << 16) | g_board->read16((a + 2) & 0xFFFFFF);
}
unsigned int m68k_read_disassembler_8(unsigned int a) { return m68k_read_memory_8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { g_board->write8(a & 0xFFFFFF, uint8_t(v)); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { g_board->write16(a & 0xFFFFFF, uint16_t(v)); }
void m68k_write_memory_32(unsigned int a, unsigned int v) {
    g_board->write16(a & 0xFFFFFF, uint16_t(v >> 16));
    g_board->write16((a + 2) & 0xFFFFFF, uint16_t(v));
}
}

bool SoundBoard::init(std::vector<uint8_t> program, std::vector<uint8_t> samples, AudioOut* out) {
    if (program.empty()) return false;
    program_ = std::move(program);
    samples_ = std::move(samples);
    out_ = out;
    const bool dbg = getenv("LW_SNDDBG") != nullptr;
    for (int i = 0; i < 2; i++) {
        ram_[i].assign(kRamSize, 0);
        scsp_[i].init(ram_[i].data(), kRamSize, i == 0);
    }
    if (dbg) fprintf(stderr, "[snd] scsp ok\n");
    // Reset: the first 16 bytes of the program ROM (SSP, PC) are copied to RAM
    memcpy(ram_[0].data(), program_.data(), 16);
    g_board = this;
    m68k_init();
    if (dbg) fprintf(stderr, "[snd] m68k_init ok\n");
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    if (dbg) fprintf(stderr, "[snd] reset ok\n");
    ready_ = true;
    return true;
}

// Layout: a header (magic, version), both RAMs, both SCSPs field by field
// (Scsp::save_state), the 68000 context with its size, ctrl_. The SCSPs keep
// their own RAM pointers; the 68000 context carries pointers into this
// program (cycle tables, callbacks), set again after it is put back. States
// from before the header kept the SCSPs byte for byte and no context size.
constexpr uint32_t STATE_MAGIC = 0x53444E53;   // "SNDS"
constexpr uint32_t STATE_VERSION = 1;

void SoundBoard::save_state(std::vector<uint8_t>& out) const {
    out.clear();
    if (!ready_) return;
    auto put32 = [&](uint32_t v) { out.insert(out.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); };
    put32(STATE_MAGIC);
    put32(STATE_VERSION);
    for (int i = 0; i < 2; i++) out.insert(out.end(), ram_[i].begin(), ram_[i].end());
    for (int i = 0; i < 2; i++) scsp_[i].save_state(out);
    uint32_t ctx = uint32_t(m68k_context_size());
    put32(ctx);
    size_t at = out.size();
    out.resize(at + ctx + 1);
    m68k_get_context(out.data() + at);
    out[at + ctx] = ctrl_;
}

bool SoundBoard::load_state(const std::vector<uint8_t>& in) {
    if (!ready_) return false;
    size_t ctx = m68k_context_size();
    const uint8_t* p = in.data();
    auto get32 = [&] {
        uint32_t v;
        memcpy(&v, p, 4);
        p += 4;
        return v;
    };
    bool legacy = !(in.size() >= 8 && get32() == STATE_MAGIC);
    if (legacy) {
        if (in.size() != 2 * kRamSize + 2 * Scsp::legacy_size() + ctx + 1) return false;
        p = in.data();
    } else {
        // Checked before anything changes
        if (in.size() != 8 + 2 * kRamSize + 2 * Scsp::state_size() + 4 + ctx + 1 || get32() != STATE_VERSION) return false;
        uint32_t saved_ctx;
        memcpy(&saved_ctx, p + 2 * kRamSize + 2 * Scsp::state_size(), 4);
        if (saved_ctx != ctx) return false;
    }
    for (int i = 0; i < 2; i++, p += kRamSize) memcpy(ram_[i].data(), p, kRamSize);
    for (int i = 0; i < 2; i++) {
        if (legacy) scsp_[i].load_legacy(p), p += Scsp::legacy_size();
        else scsp_[i].load_state(p);
    }
    if (!legacy) p += 4;   // the context size, checked above
    std::vector<uint8_t> cpu(p, p + ctx);
    m68k_set_context(cpu.data());
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);   // the cycle tables of this run
    m68k_set_int_ack_callback(nullptr);       // nullptr: Musashi's own defaults, as after init
    m68k_set_bkpt_ack_callback(nullptr);
    m68k_set_reset_instr_callback(nullptr);
    m68k_set_pc_changed_callback(nullptr);
    m68k_set_tas_instr_callback(nullptr);
    m68k_set_illg_instr_callback(nullptr);
    m68k_set_trap_instr_callback(nullptr);
    m68k_set_fc_callback(nullptr);
    m68k_set_instr_hook_callback(nullptr);
    ctrl_ = p[ctx];
    sample_acc_ = 0;
    return true;
}

uint8_t SoundBoard::read8(uint32_t a) {
    switch ((a >> 20) & 0xF) {
    case 0x0: return ram_[0][a & 0xFFFFF];
    case 0x1: {
        uint8_t v = scsp_[0].read8(a & 0xFFFF);
        m68k_set_irq(unsigned(scsp_[0].irq_level()));   // reading MIDI-in may drop the interrupt
        return v;
    }
    case 0x2: return ram_[1][a & 0xFFFFF];
    case 0x3: return scsp_[1].read8(a & 0xFFFF);
    case 0x6: return program_[a & (program_.size() - 1)];
    default:
        if (a >= 0x800000 && !samples_.empty()) return samples_[(a & 0x7FFFFF) % samples_.size()];
        return 0;
    }
}

uint16_t SoundBoard::read16(uint32_t a) {
    switch ((a >> 20) & 0xF) {
    case 0x1: {
        uint16_t v = scsp_[0].read16(a & 0xFFFF);
        m68k_set_irq(unsigned(scsp_[0].irq_level()));
        return v;
    }
    case 0x3: return scsp_[1].read16(a & 0xFFFF);
    default: return uint16_t(read8(a) << 8 | read8(a + 1));
    }
}

void SoundBoard::write8(uint32_t a, uint8_t v) {
    switch ((a >> 20) & 0xF) {
    case 0x0: ram_[0][a & 0xFFFFF] = v; break;
    case 0x1:
        scsp_[0].write8(a & 0xFFFF, v);
        m68k_set_irq(unsigned(scsp_[0].irq_level()));   // e.g. SCIRE acknowledges
        break;
    case 0x2: ram_[1][a & 0xFFFFF] = v; break;
    case 0x3: scsp_[1].write8(a & 0xFFFF, v); break;
    case 0x4:
        if (a == 0x400001) ctrl_ = v;   // sample ROM bank (no effect with 8 MB of samples)
        break;
    default: break;
    }
}

void SoundBoard::write16(uint32_t a, uint16_t v) {
    switch ((a >> 20) & 0xF) {
    case 0x1:
        scsp_[0].write16(a & 0xFFFF, v);
        m68k_set_irq(unsigned(scsp_[0].irq_level()));
        break;
    case 0x3: scsp_[1].write16(a & 0xFFFF, v); break;
    default:
        write8(a, uint8_t(v >> 8));
        write8(a + 1, uint8_t(v));
    }
}

void SoundBoard::run_frame(double frame_hz) {
    if (!ready_) return;
    sample_acc_ += 44100.0 / frame_hz;
    int samples = int(sample_acc_);
    sample_acc_ -= samples;
    // Four channels as on the cabinet: the master SCSP drives the front
    // speakers and the slave the rear ones. Full scale = 1.
    buffer_.resize(size_t(samples) * AudioStream::kInChannels);
    const float scale = gain_ / 32768.0f;
    for (int i = 0; i < samples; i++) {
        float* f = &buffer_[size_t(i) * AudioStream::kInChannels];
        scsp_[0].render(f[0], f[1]);
        scsp_[1].render(f[2], f[3]);
        for (int c = 0; c < AudioStream::kInChannels; c++) f[c] *= c < 2 ? scale : scale * rear_gain_;
        scsp_[0].tick_timers();
        m68k_set_irq(unsigned(scsp_[0].irq_level()));
        int budget = 256 - overshoot_;
        overshoot_ = budget > 0 ? m68k_execute(budget) - budget : -budget;
    }
    weather_.mix(buffer_.data(), samples, gain_ / 5.0f);   // at the game's volume (5 by default)
    if (wav_) {
        std::vector<int16_t> pcm(size_t(samples) * 2);
        for (size_t i = 0; i < size_t(samples); i++) {   // the stereo mix, as the output plays it
            const float* f = &buffer_[i * AudioStream::kInChannels];
            float st[2] = {(f[0] + f[2]) * 0.5f, (f[1] + f[3]) * 0.5f};
            wav_limiter_.process(st, 2);
            for (int c = 0; c < 2; c++) pcm[i * 2 + c] = int16_t(std::lround(soft_limit(st[c]) * 32767.0f));
        }
        fwrite(pcm.data(), 4, size_t(samples), wav_);
        wav_frames_ += uint32_t(samples);
    }
    if (out_) {
        if (muted_) {
            was_muted_ = true;
            return;
        }
        if (was_muted_) {   // back from silence: no click
            was_muted_ = false;
            fade_ = 882;
        }
        for (int i = 0; i < samples && fade_ > 0; i++, fade_--) {
            float k = 1.0f - float(fade_) / 882.0f;
            float* f = &buffer_[size_t(i) * AudioStream::kInChannels];
            for (int c = 0; c < AudioStream::kInChannels; c++) f[c] *= k;
        }
        out_->push(buffer_.data(), size_t(samples));   // the output keeps the latency bounded
        frames_out_ += uint64_t(samples);
    }
}

void SoundBoard::dump_wav(const char* path) {
    wav_ = fopen(path, "wb");
    if (!wav_) return;
    wav_limiter_.set_rate(44100);
    uint8_t header[44] = {};
    fwrite(header, 1, 44, wav_);   // filled in by close_wav
}

void SoundBoard::close_wav() {
    if (!wav_) return;
    uint32_t data = wav_frames_ * 4;
    auto put32 = [&](uint32_t v) { fwrite(&v, 4, 1, wav_); };
    auto put16 = [&](uint16_t v) { fwrite(&v, 2, 1, wav_); };
    fseek(wav_, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, wav_);
    put32(36 + data);
    fwrite("WAVEfmt ", 1, 8, wav_);
    put32(16);
    put16(1);
    put16(2);
    put32(44100);
    put32(44100 * 4);
    put16(4);
    put16(16);
    fwrite("data", 1, 4, wav_);
    put32(data);
    fclose(wav_);
    wav_ = nullptr;
}
