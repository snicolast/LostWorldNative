// Yamaha SCSP (YMF292): 32 PCM slots with envelopes, LFOs and FM, plus the
// effects DSP. Behaviour follows docs/sound_spec.md sections 3 and 4.
#pragma once
#include <cstdint>

class Scsp {
public:
    void init(uint8_t* ram, uint32_t ram_size, bool master);

    // Register window access from the 68000 (offset = address & 0xFFFF)
    uint16_t read16(uint32_t off);
    uint8_t read8(uint32_t off);
    void write16(uint32_t off, uint16_t v);
    void write8(uint32_t off, uint8_t v);

    // One 44.1 kHz output sample (slots, DSP and effect returns), on the
    // 16-bit scale but without dropping the bits below it
    void render(float& left, float& right);
    void tick_timers();
    int irq_level();
    // Without the effects DSP (most of the cost), e.g. while the silent boot
    // screens are skipped at full speed
    void set_dsp_enabled(bool on) { dsp_on_ = on; }
    // Sample interpolation of both chips: linear as the hardware, or 4-point
    // cubic (smoother low-pitched samples, less high-frequency grit)
    static void set_cubic(bool on) { cubic_ = on; }
    // Key-ons on stopped slots that the Supermodel rule would have ignored (diagnostics)
    static uint32_t restarted() { return restarted_; }

    void midi_in(uint8_t b);
    bool midi_pending() const { return midi_r_ != midi_w_; }

    // Game states (sound_board.cpp): all of `from` but this chip's RAM and DSP switch
    void load_state(const Scsp& from) {
        uint8_t* ram = ram_;
        bool dsp = dsp_on_;
        *this = from;
        ram_ = ram;
        dsp_on_ = dsp;
    }

private:
    enum EgState { ATTACK, DECAY1, DECAY2, RELEASE };
    struct Slot {
        bool active = false, back = false;
        int32_t cur = 0, nxt = 0;
        uint32_t base = 0, step = 0;
        int32_t level = 0;   // EG level, 10.16 fixed point
        EgState state = RELEASE;
        uint32_t ar = 0, d1r = 0, d2r = 0, rr = 0;
        int dl_thr = 0;
        bool eg_hold = false;
        uint16_t plfo_phase = 0, alfo_phase = 0, plfo_step = 0, alfo_step = 0;
    };

    uint16_t& sreg(int slot, int word) { return slot_regs_[slot][word]; }
    void key_on_off();
    void start_slot(int s);
    void stop_slot(int s);
    void update_slot_rates(int s, bool all);
    void update_pitch(int s);
    void update_lfo(int s);
    int32_t slot_sample(int s);
    int32_t eg_update(int s);
    int plfo(int s);
    int alfo(int s);
    uint16_t ram16(uint32_t byte_addr) const;
    void dsp_step();
    void write_common(int word, uint16_t v);

    static inline bool cubic_ = false;   // outside the state: game states keep their size
    static inline uint32_t restarted_ = 0;

    uint8_t* ram_ = nullptr;
    uint32_t ram_mask_ = 0;
    bool master_ = false;

    uint16_t slot_regs_[32][16] = {};
    uint16_t common_[0x18] = {};
    Slot slots_[32];
    int16_t stack_[64] = {};
    int stack_ptr_ = 0;
    bool dsp_on_ = true;

    // Timers (8.8 counters, 0xFFFF = expired) and MIDI
    uint32_t timer_[3] = {0xFFFF, 0xFFFF, 0xFFFF};
    uint8_t midi_fifo_[256] = {};
    uint8_t midi_r_ = 0, midi_w_ = 0;
    uint32_t midi_dropped_ = 0;   // bytes lost to a full FIFO (diagnostics)
    int monitor_slot_ = 0;

    // DSP
    int16_t coef_[64] = {};
    uint16_t madrs_[32] = {};
    uint16_t mpro_[128 * 4] = {};
    int32_t temp_[128] = {}, mems_[32] = {}, mixs_[16] = {};
    int16_t efreg_[16] = {};
    uint32_t dec_ = 0;
    bool dsp_running_ = false;
};
