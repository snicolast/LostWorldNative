// The emulator: Model 3 memory, CROM and banks, hardware registers, vblank
// and decrementer, the game thread, game states and a diagnostic report. The
// window, the renderer and the menus are in app.cpp; runtime.h is the seam.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <atomic>
#include <mutex>
#include <thread>

#include "bosses.h"
#include "eeprom.h"
#include "inputs.h"
#include "fns.h"
#include "runtime.h"
#include "scsi53c810.h"
#include "../video/tilegen.h"
#include "../audio/audio_out.h"
#include "../audio/sound_board.h"
#include "../video/r3d_state.h"
#include "stats.h"
#include "random_spawns.h"
#include "survival.h"

Ctx g_ctx;
uint8_t* g_ram;
uint8_t g_trace_bits[512];
std::map<uint32_t, uint64_t> g_trace_hits;   // function -> hit count
uint32_t g_recent_fn[64];
uint32_t g_recent_idx = 0;

void rt_print_recent() {
    fprintf(stderr, "  last functions:");
    for (int i = 24; i >= 1; i--) fprintf(stderr, " %X", g_recent_fn[(g_recent_idx - i) & 63]);
    fprintf(stderr, "\n");
}

// --diff FN@FRAME: on the first call of FN at or after FRAME, saves the CPU
// state and RAM before and after running it (out/diff_before.bin,
// out/diff_after.bin) plus every device read (out/diff_mmio.bin), then exits.
// tools/diff_unicorn.py replays the call on an independent PowerPC emulator.
uint32_t g_diff_fn = 0xFFFFFFFF;
uint64_t g_diff_frame = 0;
bool g_diff_active = false;
std::vector<uint32_t> g_diff_mmio;   // (address, size, value) triples
void diff_run(Ctx& c, uint32_t fn);

void rt_trace(Ctx& c, uint32_t fn) {
    survival_enter(c, fn);
    random_spawns_enter(c, fn);
    stats_enter(c, fn);
    if (fn == g_diff_fn && !g_diff_active) {
        diff_run(c, fn);
        return;
    }
    auto it = g_trace_hits.find(fn);
    if (it == g_trace_hits.end()) return;   // filter collision
    if (++it->second > 3) return;            // full detail only for the first 3
    printf("[trace] fn_%08X lr=%08X\n", fn, c.lr);
    for (int i = 0; i < 32; i++) printf("  r%-2d=%08X%s", i, c.r[i], i % 4 == 3 ? "\n" : "");
    for (int i = 3; i <= 10; i++) {
        uint32_t p = c.r[i];
        if (p < RAM_SIZE - 32 || p >= 0xFF000000) {
            printf("  [r%d]:", i);
            for (int k = 0; k < 8; k++) printf(" %08X", rd32(p + 4 * k));
            printf("\n");
        }
    }
}

// ---- Shared with the window side (app.cpp): see runtime.h ----
std::vector<uint8_t> g_crom, g_banked, g_vrom;
std::string g_data_dir = "data";
int g_frame_limit = INT32_MAX;   // --frames; with --headless, 600 by default
bool g_collect_stats = false;    // keep access statistics and the full texture stream for report()
Eeprom93C46 g_eeprom;
SoundBoard g_sound;
AudioOut g_audio;
AutoInput g_auto;
std::shared_ptr<const R3DFrameState> g_r3d_latest;                  // last closed 3D frame
std::condition_variable g_r3d_cv;                                   // notified when it changes
std::mutex g_r3d_mutex;
std::thread g_state_writer;   // writes a captured game state to its file (rt_state_point) off the game thread
R3DTextureRam g_texture_ram;
std::mutex g_video_mutex;
std::vector<uint8_t> g_video_vram(TILEGEN_SIZE);
uint64_t g_video_frame = 0;
uint64_t g_video_page_serial[TILEGEN_PAGES] = {};
bool g_realtime = false;
LARGE_INTEGER g_qpf, g_start_time;
// CPU time of the game thread per emulated frame, without the waits (frame
// pacing, menus): summed in microseconds, read by the window thread once a second
std::atomic<uint64_t> g_game_busy_us{0};
std::atomic<uint64_t> g_game_frames{0};
// The WARNING notice and the copyright line shown at power-on are run through
// at full speed with nothing on screen: the game appears at the SEGA logo.
std::atomic<bool> g_boot_skip{false};
// Menus: the game stands still (and so does its sound) while set. The boot
// screens still run through first, so Play starts at the SEGA logo.
std::atomic<bool> g_hold{false};
// Shutdown: the window thread sets g_quit; the game thread parks itself at
// its next frame event (its native stack cannot be unwound) and reports it
// in g_parked, so the audio can be stopped with nobody pushing samples.
std::atomic<bool> g_quit{false}, g_parked{false};
// --subtitles off|en|jp / settings: 0 none, 1 English, 2 Japanese (the board's own)
std::atomic<int> g_subtitles{0};

namespace {

constexpr uint64_t CPU_HZ = 100000000;      // PowerPC 603e at 100 MHz
constexpr double FRAME_HZ = 57.524;         // Model 3 refresh rate
constexpr uint64_t FRAME_CYCLES = uint64_t(CPU_HZ / FRAME_HZ);
constexpr uint64_t SAMPLE_CYCLES = 200000;  // sampling profiler
constexpr uint32_t MSR_EE = 0x8000, MSR_IP = 0x40;

uint64_t g_frame = 0, g_next_sample = SAMPLE_CYCLES;
// Frame timeline (424 lines: 40 of VBlank, then 384 active; spec section 8)
constexpr uint64_t LINE_CYCLES = FRAME_CYCLES / 424;
uint64_t g_frame_start = 0, g_next_phase = 0;
int g_phase = 0;              // 0 = VBlank start, 1 = end of VBlank, 2 = ping-pong flip, 3 = last line
uint8_t g_midi_ctrl = 0;      // sound board MIDI control port
int g_midi_pulses = 0;        // MIDI interrupts left in this VBlank
uint64_t g_midi_next = 0;
uint32_t flip_line();

// Interrupt controller (0xF0100014 mask, 0xF0100018 status)
uint8_t g_irq_state = 0, g_irq_enable = 0;   // level-triggered, acknowledged by writing 0xF0100018
bool g_in_irq = false;
uint64_t g_irq_count[8] = {};

// Decrementer
uint32_t g_dec_value = 0xFFFFFFFF;
uint64_t g_dec_base = 0;
bool g_dec_armed = false;
uint64_t g_dec_irqs = 0;

[[noreturn]] void exit_from_game_thread(int code);
uint8_t g_adc_channel = 0;
Ncr53c810 g_scsi;

// Time base (TB): the game resets it to 0 and measures delays with it
uint64_t g_tb_base = 0;

// Real3D: a status bit (0x84000000) toggles every frame;
// the game uses it to know the chip has finished drawing.
uint32_t g_real3d_status = 0xFFFFFFFD;   // CPU view: bit 1 = ping-pong

// PCI configuration space (0xF0800CF8 address, 0xF0C00CFC data)
std::map<uint32_t, uint32_t> g_pci_cfg;

uint32_t le32_at(const uint8_t* p) { return uint32_t(p[0]) | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

uint32_t pci_read(uint32_t cfg) {
    uint32_t dev = (cfg >> 11) & 0x1F, reg = cfg & 0xFC;
    if (reg == 0) {
        if (dev == 0x0D) return 0x16C311DB;   // Real3D Pro-1000
        if (dev == 0x0E) return 0x00011000;   // NCR 53C810
        return 0xFFFFFFFF;                    // no device
    }
    auto it = g_pci_cfg.find(cfg & 0xFFFFFFFC);
    return it == g_pci_cfg.end() ? 0 : it->second;
}

// Device memory: 64 KB pages allocated on demand
std::unordered_map<uint32_t, std::vector<uint8_t>> g_dev;
std::vector<uint8_t> g_texture_fifo;   // the part not yet uploaded (the whole stream with g_collect_stats)
std::unordered_set<uint32_t> g_r3d_dirty;                          // pages touched since the last frame
uint32_t g_r3d_last_dirty = 0xFFFFFFFF;                             // a page known to be in g_r3d_dirty
size_t g_fifo_consumed = 0;                                         // bytes of the FIFO already uploaded
uint64_t g_r3d_frames = 0;
void r3d_flush();
void texture_port_word(uint32_t w);

struct Stat { uint64_t reads = 0, writes = 0; uint32_t last = 0; };
std::unordered_map<uint32_t, Stat> g_stats;  // per address (control) or per page (everything else)
std::map<uint32_t, uint64_t> g_missing;      // called functions that don't exist
std::map<uint32_t, uint64_t> g_bad;          // untranslated instructions
std::unordered_map<uint32_t, uint64_t> g_samples;

// Control registers by exact address; memories (Real3D, backup RAM) by page
bool is_page(uint32_t a) {
    if (a >= 0xC1000000 && a < 0xC1000100) return false;   // 53C810 registers
    return !(a >= 0xF0000000 && a < 0xF1000000 && !(a >= 0xF00C0000 && a < 0xF0100000));
}
uint32_t stat_key(uint32_t a) { return is_page(a) ? (a & ~0xFFFu) : a; }

uint8_t* dev_ptr(uint32_t a) {
    static uint32_t last_key = 0xFFFFFFFF;
    static uint8_t* last_page = nullptr;
    if ((a >> 16) != last_key) {
        auto& page = g_dev[a >> 16];
        if (page.empty()) page.assign(0x10000, 0);
        last_key = a >> 16;
        last_page = page.data();
    }
    return last_page + (a & 0xFFFF);
}

// The game writes a Real3D page many times in a row: the set is only touched
// when the page changes
inline void mark_r3d_dirty(uint32_t page) {
    if (page == g_r3d_last_dirty) return;
    g_r3d_dirty.insert(page);
    g_r3d_last_dirty = page;
}

// The tilegen pages written since the last video_snapshot, one bit each
uint32_t g_tilegen_dirty = (1u << TILEGEN_PAGES) - 1;
inline void mark_tilegen_dirty(uint32_t a) {
    uint32_t page = (a - TILEGEN_BASE) >> 16;   // (huge below the tilegen)
    if (page < TILEGEN_PAGES) g_tilegen_dirty |= 1u << page;
}

// Plain device memories, where nothing in dev_read8 / dev_write8 reacts to
// an access besides the dirty pages and the FIFO stream: the Real3D culling
// and polygon memories (0x8C, 0x8E, 0x98), the texture FIFO (0x94) and the
// tilegen's layer memory and palette (below its registers at 0xF1180000).
// Whole halfwords and words go in and out at once there, byte for byte the
// same as the byte path; n bytes from a, within one 64 KB page.
enum class Plain { No, R3D, Fifo, Memory };
inline Plain plain_memory(uint32_t a, uint32_t n) {
    if ((a & 0xFFFF) > 0x10000 - n) return Plain::No;   // runs into the next page
    switch (a >> 24) {
    case 0x8C: case 0x8E: case 0x98: return Plain::R3D;
    case 0x94: return Plain::Fifo;
    case 0xF1: return a < 0xF1180000 ? Plain::Memory : Plain::No;
    default: return Plain::No;
    }
}

const uint8_t* rom_ptr(uint32_t a) {
    // 8 MB CROM space: the fixed 2 MB at the top and, below them, the first 6 MB
    // of bank 0 (models, index lists...)
    if (a >= 0xFFE00000) return &g_crom[a & (uint32_t(g_crom.size()) - 1)];
    if (a >= 0xFF800000 && !g_banked.empty()) return &g_banked[a - 0xFF800000];
    if (a >= 0xFF000000 && !g_banked.empty()) {
        uint32_t bank = (~*dev_ptr(0xF0100008)) & 7;
        size_t off = size_t(bank) * 0x800000 + (a - 0xFF000000);
        return &g_banked[off % g_banked.size()];
    }
    return nullptr;
}

uint8_t dev_read8(uint32_t a) {
    if (a == 0xF0100018) return g_irq_state;
    if (a == 0xF0100014) return g_irq_enable;
    if (a == 0xF010001C) return 0xFF;
    if (a == 0xF0080000) return 0x00;   // MIDI data port
    if (a == 0xF0080004) return 0x83;   // MIDI control port
    if (a == 0xF0140001) return 0x03;   // RTC battery voltage
    if (a == 0xF0040004) {
        bool bank1 = *dev_ptr(0xF0040000) & 0x01;   // bank selected by writing 0xF0040000
        uint8_t v = g_inputs.system_port(bank1);
        return bank1 ? uint8_t((v & 0xDF) | (g_eeprom.read_do() << 5)) : v;
    }
    // Player buttons: active low, 0xFF = nothing pressed
    if (a == 0xF0040008) return g_inputs.player_port(0);
    if (a == 0xF004000C) return g_inputs.player_port(1);
    if (a == 0xF0040018) return 0x7F;
    if (a == 0xF0040034) return 0x00;   // serial FIFO flags (no light gun board)
    if (a == 0xF004003C) {              // ADC: gun position, channel auto-increments
        uint8_t v = g_inputs.adc(g_adc_channel & 7);
        g_adc_channel++;
        return v;
    }
    if (a > 0xF0040000 && a < 0xF0040040 && (a & 3) == 0) return 0xFF;   // unused inputs read as 1s (0x00 reads back the bank)
    if (a >= 0x84000000 && a < 0x84000004) return uint8_t(g_real3d_status >> (24 - 8 * (a & 3)));
    if (a >= 0xF0C00CFC && a < 0xF0C00D00) {
        uint32_t v = pci_read(le32_at(dev_ptr(0xF0800CF8)));
        return uint8_t(v >> (8 * (a - 0xF0C00CFC)));
    }
    if (a >= 0xC1000000 && a < 0xC1000100) {
        uint8_t v;
        if (g_scsi.read8(a & 0xFF, v)) return v;
    }
    return *dev_ptr(a);
}

void dev_write8(uint32_t a, uint8_t v) {
    if (a == 0xF0100014) { g_irq_enable = v; g_ctx.next_event = g_ctx.cycles; }
    if (a == 0xF0100018) g_irq_state &= ~v;   // acknowledge
    if (a == 0xF1180010) g_irq_state &= ~v;   // tilegen IRQ acknowledge register (LE low byte)
    if (a == 0xF0080000) {   // MIDI byte to the sound board
        g_irq_state &= ~0x40;
        g_sound.midi_write(v);
    }
    if (a == 0xF0080004) {
        g_midi_ctrl = v;
        if (!(v & 0x20)) g_irq_state &= ~0x40;
    }
    if (a == 0xF0040000) g_eeprom.write_pins(v);
    if (a == 0xF004003C) g_adc_channel = v & 7;
    *dev_ptr(a) = v;
    mark_tilegen_dirty(a);
    if (a == 0xF0C00CFF)
        g_pci_cfg[le32_at(dev_ptr(0xF0800CF8)) & 0xFFFFFFFC] = le32_at(dev_ptr(0xF0C00CFC));
    if (a >= 0xC1000000 && a < 0xC1000100) g_scsi.wrote8(a & 0xFF, v, dev_ptr(0xC1000000));
    // 0x94000000 is a texture FIFO: the whole stream is stored in order
    if ((a >> 24) == 0x94) g_texture_fifo.push_back(v);
    // 0x90000000: texture port, three words (source, header, trigger) per upload
    if ((a >> 24) == 0x90 && (a & 3) == 3) texture_port_word(le32_at(dev_ptr(a & ~3u)));
    uint32_t region = a >> 24;
    if (region == 0x8C || region == 0x8E || region == 0x98) mark_r3d_dirty(a >> 16);
    // Writing to the command port (0x88000000) closes the 3D frame
    if (a == 0x88000003) r3d_flush();
}

void texture_port_word(uint32_t w) {
    static uint32_t words[2];
    static int count = 0;
    if (count < 2) {
        words[count++] = w;
        return;
    }
    count = 0;   // third write triggers the upload; its data is ignored
    uint32_t src = words[0] & 0xFFFFFF, header = words[1];
    if (src >= 0x100000) {
        if (g_vrom.empty()) return;
        size_t words_total = g_vrom.size() / 4;
        size_t start = src % words_total;
        const uint16_t* p = reinterpret_cast<const uint16_t*>(g_vrom.data() + start * 4);
        g_texture_ram.upload_direct(header, p, (words_total - start) * 2);
    } else {
        // Polygon RAM source: the rest of it (at most 1M words), page by page
        // into a buffer kept between uploads. The device words are stored
        // little-endian, so their 16-bit halves are the texels in order
        static std::vector<uint16_t> buf;
        buf.resize((0x100000 - src) * 2);
        uint32_t byte = src * 4;   // into polygon RAM
        size_t done = 0;
        while (byte < 0x400000) {
            uint32_t page_end = (byte | 0xFFFF) + 1, n = std::min(page_end, 0x400000u) - byte;
            auto it = g_dev.find((0x98000000 + byte) >> 16);
            if (it == g_dev.end() || it->second.empty()) memset(buf.data() + done, 0, n);
            else memcpy(buf.data() + done, it->second.data() + (byte & 0xFFFF), n);
            done += n / 2;
            byte += n;
        }
        g_texture_ram.upload_direct(header, buf.data(), buf.size());
    }
}

// When the game writes the command port the Real3D memory is published as an
// immutable frame (unchanged pages are shared) and the texture FIFO collected
// since the previous flush is applied to texture RAM.

void r3d_flush() {
    auto state = std::make_shared<R3DFrameState>();
    state->flushed = state->time = now_seconds();
    state->vblank = g_frame;
    // At real speed the frame is dated by its vblank: the game finishes its
    // frames a few ms after the vblank, by a varying amount, and that jitter
    // would make the interpolated motion uneven
    if (g_realtime && !g_boot_skip) {
        double vblank = double(g_start_time.QuadPart) / double(g_qpf.QuadPart) + double(g_frame) / FRAME_HZ;
        if (state->flushed >= vblank && state->flushed < vblank + 0.05) state->time = vblank;
    }
    {
        std::lock_guard<std::mutex> lock(g_r3d_mutex);
        if (g_r3d_latest) state->pages = g_r3d_latest->pages;
    }
    for (uint32_t key : g_r3d_dirty)
        state->pages[key] = std::make_shared<const R3DPage>(*reinterpret_cast<const R3DPage*>(g_dev[key].data()));
    g_r3d_dirty.clear();
    g_r3d_last_dirty = 0xFFFFFFFF;
    state->frame = ++g_r3d_frames;

    size_t words = (g_texture_fifo.size() - g_fifo_consumed) / 4;
    if (words) {
        std::vector<uint32_t> fifo(words);
        memcpy(fifo.data(), g_texture_fifo.data() + g_fifo_consumed, words * 4);
        g_fifo_consumed += words * 4;
        g_texture_ram.upload_fifo(fifo);
        // The whole stream is only needed by dump_video_memory (report());
        // otherwise the uploaded part is dropped so the vector does not grow all session
        if (!g_collect_stats) {
            g_texture_fifo.erase(g_texture_fifo.begin(), g_texture_fifo.begin() + ptrdiff_t(g_fifo_consumed));
            g_fifo_consumed = 0;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_r3d_mutex);
        g_r3d_latest = std::move(state);
    }
    g_r3d_cv.notify_all();
}

// Dumps the Real3D and 2D layer memories for the future renderer
void dump_video_memory() {
    CreateDirectoryA("out", nullptr);   // CreateDirectory does not create the parent
    CreateDirectoryA("out/real3d", nullptr);
    struct Region { uint32_t base, size; const char* name; };
    const Region regions[] = {
        {0x8C000000, 0x01000000, "culling_lo"}, {0x8E000000, 0x01000000, "culling_hi"},
        {0x98000000, 0x01000000, "polygon"},    {0xF1000000, 0x00200000, "tilegen"},
    };
    for (auto& r : regions) {
        // The 3D memories are taken from the last closed frame (consistent state)
        std::unordered_map<uint32_t, const uint8_t*> pages;
        if ((r.base >> 24) != 0xF1 && g_r3d_latest)
            for (auto& [key, page] : g_r3d_latest->pages) pages[key] = page->data();
        else
            for (auto& [key, page] : g_dev) pages[key] = page.data();
        uint32_t top = 0;
        for (auto& [key, page] : pages)
            if ((key << 16) >= r.base && (key << 16) < r.base + r.size) top = std::max(top, (key << 16) + 0x10000 - r.base);
        if (!top) continue;
        std::vector<uint8_t> buf(top, 0);
        for (auto& [key, page] : pages)
            if ((key << 16) >= r.base && (key << 16) < r.base + top) memcpy(&buf[(key << 16) - r.base], page, 0x10000);
        std::string path = std::string("out/real3d/") + r.name + ".bin";
        if (FILE* f = fopen(path.c_str(), "wb")) { fwrite(buf.data(), 1, buf.size(), f); fclose(f); }
        else fprintf(stderr, "[warning] could not write %s\n", path.c_str());
        printf("  %-12s %08X  %u KB\n", r.name, r.base, top / 1024);
    }
    if (FILE* f = fopen("out/real3d/texture_fifo.bin", "wb")) {
        fwrite(g_texture_fifo.data(), 1, g_texture_fifo.size(), f);
        fclose(f);
    }
    printf("  texture_fifo %08X  %zu KB\n", 0x94000000u, g_texture_fifo.size() / 1024);
}

void note(uint32_t a, bool write, uint32_t v) {
    if (!g_collect_stats) return;   // the report only comes with --frames / --headless
    auto& s = g_stats[stat_key(a)];
    (write ? s.writes : s.reads)++;
    s.last = v;
}

// The recompiled function at addr (g_fn_table is sorted by address), or null
static inline const FnEntry* fn_lookup(uint32_t addr) {
    const FnEntry* end = g_fn_table + g_fn_count;
    const FnEntry* e = std::lower_bound(g_fn_table, end, addr, [](const FnEntry& f, uint32_t a) { return f.addr < a; });
    return e != end && e->addr == addr ? e : nullptr;
}

// The start of the function holding pc (0: before the first one)
uint32_t find_function(uint32_t pc) {
    const FnEntry* e = std::upper_bound(g_fn_table, g_fn_table + g_fn_count, pc,
                                        [](uint32_t a, const FnEntry& f) { return a < f.addr; });
    return e != g_fn_table ? e[-1].addr : 0;
}

// Ping-pong flip line: tilegen register 0x08 (device word = LE bytes)
uint32_t flip_line() {
    uint32_t v = le32_at(dev_ptr(0xF1180008));
    return v ? v : 238;
}

void schedule(Ctx& c) {
    if (g_diff_active) { c.next_event = ~0ull; return; }
    uint64_t n = std::min(g_next_phase, g_next_sample);
    if (g_dec_armed) n = std::min(n, g_dec_base + uint64_t(g_dec_value) * 4);
    if (g_midi_pulses > 0) n = std::min(n, std::max(g_midi_next, c.cycles + 1));
    if ((g_irq_state & g_irq_enable) && (c.msr & MSR_EE)) n = c.cycles;
    c.next_event = n;
}

void exception(Ctx& c, uint32_t vector) {
    g_in_irq = true;
    c.srr0 = c.pc_hint;
    c.srr1 = c.msr;
    c.msr &= ~(MSR_EE | 0x4000u /*PR*/ | 0x30u /*IR/DR*/);
    rt_call(c, ((c.srr1 & MSR_IP) ? 0xFFF00000u : 0u) + vector);
    c.msr = c.srr1;   // rfi
    g_in_irq = false;
}

void report() {
    Ctx& c = g_ctx;
    printf("\n================ REPORT ================\n");
    printf("Frames: %llu  cycles: %llu  MSR: %08X\n", (unsigned long long)g_frame,
           (unsigned long long)c.cycles, c.msr);
    printf("Interrupts: mask=%02X state=%02X  delivered per bit:", g_irq_enable, g_irq_state);
    for (int i = 0; i < 8; i++) printf(" %llu", (unsigned long long)g_irq_count[i]);
    printf("  decrementer: %llu\n", (unsigned long long)g_dec_irqs);

    printf("\nWhere the time goes (samples per function):\n");
    std::map<uint32_t, uint64_t> per_fn;
    std::vector<std::pair<uint64_t, uint32_t>> hot;
    for (auto& [pc, n] : g_samples) { per_fn[find_function(pc)] += n; hot.push_back({n, pc}); }
    std::vector<std::pair<uint64_t, uint32_t>> fns;
    for (auto& [f, n] : per_fn) fns.push_back({n, f});
    std::sort(fns.rbegin(), fns.rend());
    std::sort(hot.rbegin(), hot.rend());
    for (size_t i = 0; i < fns.size() && i < 12; i++)
        printf("  fn_%08X  %llu\n", fns[i].second, (unsigned long long)fns[i].first);
    printf("Hottest single spots:\n");
    for (size_t i = 0; i < hot.size() && i < 8; i++)
        printf("  %08X  %llu\n", hot[i].second, (unsigned long long)hot[i].first);

    printf("\nSound: %u notes restarted on stopped voices (they used to be lost)\n", unsigned(Scsp::restarted()));
    printf("\nHardware accesses (register or memory range, reads, writes, last value):\n");
    std::map<uint32_t, Stat> stats(g_stats.begin(), g_stats.end());
    for (auto it = stats.begin(); it != stats.end();) {
        // Consecutive 4 KB pages are merged into a single range
        uint32_t first = it->first, last = first;
        Stat s = it->second;
        auto next = std::next(it);
        bool page = is_page(first);
        while (page && next != stats.end() && next->first == last + 0x1000) {
            last = next->first;
            s.reads += next->second.reads;
            s.writes += next->second.writes;
            s.last = next->second.last;
            ++next;
        }
        if (page)
            printf("  %08X-%08X  R=%-9llu W=%-9llu last=%08X\n", first, last + 0xFFF,
                   (unsigned long long)s.reads, (unsigned long long)s.writes, s.last);
        else
            printf("  %08X           R=%-9llu W=%-9llu last=%08X\n", first,
                   (unsigned long long)s.reads, (unsigned long long)s.writes, s.last);
        it = next;
    }

    g_scsi.report();
    g_sound.close_wav();
    if (!g_trace_hits.empty()) {
        printf("\nTraced functions (times called):\n");
        for (auto& [fn, n] : g_trace_hits) printf("  fn_%08X  %llu\n", fn, (unsigned long long)n);
    }
    if (FILE* f = fopen("out/ram.bin", "wb")) {   // for analysis with the tools
        fwrite(g_ram, 1, RAM_SIZE, f);
        fclose(f);
    }
    printf("\nVideo memories dumped to out/real3d/:\n");
    dump_video_memory();

    if (!g_missing.empty()) {
        printf("\nCalls to functions not recompiled:\n");
        for (auto& [a, n] : g_missing) printf("  %08X x%llu\n", a, (unsigned long long)n);
    }
    if (!g_bad.empty()) {
        printf("\nInvalid instructions executed:\n");
        for (auto& [a, n] : g_bad) printf("  %08X x%llu\n", a, (unsigned long long)n);
    }
    fflush(stdout);
}

}  // namespace

bool load_rom(const std::string& name, std::vector<uint8_t>& out, bool required) {
    std::string path = g_data_dir + "/" + name;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        if (required) fprintf(stderr, "Cannot find %s (run tools/romtool.py first)\n", path.c_str());
        return false;
    }
    fseek(f, 0, SEEK_END);
    out.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(out.data(), 1, out.size(), f);
    fclose(f);
    return true;
}

DWORD WINAPI game_thread(LPVOID) {
    Ctx& c = g_ctx;
    c.msr = MSR_IP;
    c.next_event = 0;
    printf("Running the boot code from 0xFFF00100...\n");
    fn_FFF00100(c);
    printf("Boot done: jumping to %08X (MSR=%08X)\n", c.lr, c.msr);
    rt_call(c, c.lr);
    printf("The main program returned (unexpected)\n");
    report();
    return 0;
}

// ---- Slow memory: CROM, banks and devices ----
uint8_t rt_rd8_slow(uint32_t a) {
    if (auto p = rom_ptr(a)) return *p;
    uint8_t v = dev_read8(a);
    note(a, false, v);
    if (g_diff_active) g_diff_mmio.insert(g_diff_mmio.end(), {a, 1u, v});
    return v;
}
uint16_t rt_rd16_slow(uint32_t a) {
    if (auto p = rom_ptr(a)) return uint16_t(p[0] << 8 | p[1]);
    uint16_t v;
    if (plain_memory(a, 2) != Plain::No) {
        const uint8_t* p = dev_ptr(a);
        v = uint16_t(p[0] << 8 | p[1]);
    } else {
        v = uint16_t(dev_read8(a) << 8 | dev_read8(a + 1));
    }
    note(a, false, v);
    if (g_diff_active) g_diff_mmio.insert(g_diff_mmio.end(), {a, 2u, v});
    return v;
}
uint32_t rt_rd32_slow(uint32_t a) {
    if (auto p = rom_ptr(a)) return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
    uint32_t v;
    if (plain_memory(a, 4) != Plain::No) {
        uint32_t w;
        memcpy(&w, dev_ptr(a), 4);
        v = bswap32(w);
    } else {
        v = uint32_t(dev_read8(a)) << 24 | dev_read8(a + 1) << 16 | dev_read8(a + 2) << 8 | dev_read8(a + 3);
    }
    note(a, false, v);
    if (g_diff_active) g_diff_mmio.insert(g_diff_mmio.end(), {a, 4u, v});
    return v;
}
// Stores outside RAM and outside any device usually mean a bad pointer:
// report the first ones with the recent call history
static void check_wild_write(uint32_t a, uint32_t v) {
    static int reported = 0;
    if (a >= RAM_SIZE && a < 0x80000000u && reported < 8) {
        reported++;
        fprintf(stderr, "[warning] write outside RAM %08X = %08X (pc ~%08X, frame %llu)\n", a, v, g_ctx.pc_hint,
                (unsigned long long)g_frame);
        rt_print_recent();
    }
}

void rt_wr8_slow(uint32_t a, uint8_t v) { check_wild_write(a, v); note(a, true, v); dev_write8(a, v); }
// A store to plain memory (plain_memory), as its bytes in memory order
static void plain_write(uint32_t a, const uint8_t* bytes, uint32_t n, Plain kind) {
    memcpy(dev_ptr(a), bytes, n);
    if (kind == Plain::R3D) mark_r3d_dirty(a >> 16);
    else if (kind == Plain::Memory) mark_tilegen_dirty(a);
    else if (kind == Plain::Fifo) g_texture_fifo.insert(g_texture_fifo.end(), bytes, bytes + n);
}

void rt_wr16_slow(uint32_t a, uint16_t v) {
    check_wild_write(a, v);
    note(a, true, v);
    const uint8_t bytes[2] = {uint8_t(v >> 8), uint8_t(v)};
    if (Plain kind = plain_memory(a, 2); kind != Plain::No) return plain_write(a, bytes, 2, kind);
    dev_write8(a, bytes[0]);
    dev_write8(a + 1, bytes[1]);
}
void rt_wr32_slow(uint32_t a, uint32_t v) {
    check_wild_write(a, v);
    note(a, true, v);
    const uint8_t bytes[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    if (Plain kind = plain_memory(a, 4); kind != Plain::No) return plain_write(a, bytes, 4, kind);
    for (int i = 0; i < 4; i++) dev_write8(a + i, bytes[i]);
}

// Access statistics for a run of bytes, as the byte-by-byte path would count them
static void note_range(uint32_t a, uint32_t n, bool write, uint8_t last) {
    if (!g_collect_stats) return;
    while (n) {
        uint32_t run = std::min(n, 0x1000 - (a & 0xFFF));
        auto& s = g_stats[stat_key(a)];
        (write ? s.writes : s.reads) += run;
        s.last = last;
        a += run;
        n -= run;
    }
}

// Block copy for the 53C810 Memory Move. Plain memories (RAM, ROM, the
// Real3D pages and the texture FIFO) are copied in runs that stay inside one
// 64 KB page, with the same side effects as the byte path (dirty pages,
// FIFO stream, statistics); registers and ports go byte by byte through
// rd8/wr8 so their side effects stay exact.
void rt_dma_copy(uint32_t dst, uint32_t src, uint32_t n) {
    while (n) {
        uint32_t run = std::min(n, 0x10000 - (src & 0xFFFF));
        run = std::min(run, 0x10000 - (dst & 0xFFFF));
        const uint8_t* sp = nullptr;
        if (src < RAM_SIZE) sp = g_ram + src;
        else if (src >= 0xFF000000) sp = rom_ptr(src);   // contiguous within a 64 KB page
        uint8_t* dp = nullptr;
        uint32_t region = dst >> 24;
        bool r3d = region == 0x8C || region == 0x8E || region == 0x98, fifo = region == 0x94;
        if (dst < RAM_SIZE) dp = g_ram + dst;
        else if (r3d || fifo) dp = dev_ptr(dst);
        if (!sp || !dp) {   // a device: one byte at a time, as the chip would
            wr8(dst, rd8(src));
            dst++, src++, n--;
            continue;
        }
        memmove(dp, sp, run);   // RAM to RAM may overlap
        if (dst >= RAM_SIZE) {
            note_range(dst, run, true, sp[run - 1]);
            if (r3d) mark_r3d_dirty(dst >> 16);
            if (fifo) g_texture_fifo.insert(g_texture_fifo.end(), sp, sp + run);
        }
        dst += run, src += run, n -= run;
    }
}

// ---- Differential testing (--diff) ----
static void diff_save(const char* path, const Ctx& c, uint32_t fn) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint32_t hdr[8] = {0x4C574446 /*LWDF*/, fn, c.cr, c.xer, c.lr, c.ctr, c.msr, c.fpscr};
    fwrite(hdr, 4, 8, f);
    fwrite(c.r, 4, 32, f);
    for (int i = 0; i < 32; i++) fwrite(&c.f[i].u, 8, 1, f);
    uint8_t bank = *dev_ptr(0xF0100008);
    fwrite(&bank, 1, 1, f);
    fwrite(g_ram, 1, RAM_SIZE, f);
    fclose(f);
}

void diff_run(Ctx& c, uint32_t fn) {
    if (g_frame < g_diff_frame) return;
    g_diff_active = true;
    c.next_event = ~0ull;
    diff_save("out/diff_before.bin", c, fn);
    if (const FnEntry* e = fn_lookup(fn)) e->fn(c);   // run the function once, with events held off
    diff_save("out/diff_after.bin", c, fn);
    if (FILE* f = fopen("out/diff_mmio.bin", "wb")) {
        fwrite(g_diff_mmio.data(), 4, g_diff_mmio.size(), f);
        fclose(f);
    }
    printf("diff: fn_%08X at frame %llu saved (%zu device reads)\n", fn,
           (unsigned long long)g_frame, g_diff_mmio.size() / 3);
    exit_from_game_thread(0);
}

// ---- Indirect calls ----
void rt_call(Ctx& c, uint32_t addr) {
    if (c.lr == SURVIVAL_SPAWN_RETURN && survival_spawn_call(c, addr)) return;
    if (addr == 0 && g_frame > 0) {   // a null function pointer restarts the game on real hardware
        static int reported = 0;
        if (reported++ < 3) {
            fprintf(stderr, "[warning] call to address 0 (frame %llu, lr=%08X, r3=%08X)\n",
                    (unsigned long long)g_frame, c.lr, c.r[3]);
            for (int i = 0; i < 8; i++) fprintf(stderr, " %08X", rd32(c.r[3] + 4 * i));
            fprintf(stderr, "\n");
            rt_print_recent();
        }
    }
    if (const FnEntry* e = fn_lookup(addr)) {
        e->fn(c);
        return;
    }
    if (g_missing[addr]++ == 0) {
        fprintf(stderr, "[warning] call to %08X, not recompiled (from ~%08X)\n", addr, c.pc_hint);
        // recomp.py will include it in the next build
        if (FILE* f = fopen("out/missing.txt", "a")) {
            fprintf(f, "%08X  # called from ~%08X\n", addr, c.pc_hint);
            fclose(f);
        }
    }
}

void rt_bad(Ctx&, uint32_t addr) {
    if (g_bad[addr]++ == 0) fprintf(stderr, "[warning] invalid instruction at %08X\n", addr);
}

void rt_sc(Ctx& c, uint32_t addr) {
    c.pc_hint = addr;
    exception(c, 0xC00);
}

// ---- Events: frames, vblank, decrementer, sampling ----
// ---- Video: copy of the 2D layer on every vblank for the window thread ----
namespace {

// (only the pages written since the last one)
void video_snapshot() {
    std::lock_guard<std::mutex> lock(g_video_mutex);
    for (uint32_t p = 0; p < TILEGEN_PAGES; p++) {
        if (!(g_tilegen_dirty >> p & 1)) continue;
        auto it = g_dev.find((TILEGEN_BASE >> 16) + p);
        if (it != g_dev.end()) memcpy(&g_video_vram[p << 16], it->second.data(), 0x10000);
        g_video_page_serial[p]++;
    }
    g_tilegen_dirty = 0;
    g_video_frame = g_frame;
}

double g_wait_seconds = 0;   // time spent waiting since the last frame event (game thread only)

// Keeps the game at the original speed (vblank at FRAME_HZ)
void pace_frame() {
    double wait_from = now_seconds();
    struct Done {   // whichever way pace_frame returns, the wait is counted
        double from;
        ~Done() { g_wait_seconds += now_seconds() - from; }
    } done{wait_from};
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double target = double(g_frame) / FRAME_HZ;
    double elapsed = double(now.QuadPart - g_start_time.QuadPart) / double(g_qpf.QuadPart);
    if (elapsed > target + 0.25) {   // far behind (e.g. window being dragged): don't try to catch up
        g_start_time.QuadPart = now.QuadPart - LONGLONG(target * double(g_qpf.QuadPart));
        return;
    }
    while (elapsed < target) {
        if (target - elapsed > 0.002) Sleep(1);
        QueryPerformanceCounter(&now);
        elapsed = double(now.QuadPart - g_start_time.QuadPart) / double(g_qpf.QuadPart);
    }
}

ULONGLONG g_boot_ms = GetTickCount64();

// Exit from the game thread itself: nothing else pushes audio, so it is
// stopped here before the process goes away
[[noreturn]] void exit_from_game_thread(int code) {
    fflush(stdout);
    if (g_state_writer.joinable()) g_state_writer.join();   // a state still being written
    g_audio.stop();
    ExitProcess(code);
}

// The SEGA logo is the first boot screen that fills the screen (grey backdrop);
// the ones before it are a few lines of text on black
bool boot_logo_up() {
    static std::vector<uint32_t> back(BACK_W * SCREEN_H), front(SCREEN_W * SCREEN_H), raw(SCREEN_W * SCREEN_H);
    TileGen::compose(g_video_vram.data(), {.back = back.data(), .front = front.data(), .front_raw = raw.data()});
    int lit = 0;
    for (int y = 0; y < SCREEN_H; y++)
        for (int x = 0; x < SCREEN_W; x++) {
            uint32_t b = back[y * BACK_W + BACK_MARGIN + x], f = raw[y * SCREEN_W + x];
            lit += ((f >> 24) && (f & 0xFFFFFF)) || ((b >> 24) && (b & 0xFFFFFF));
        }
    return lit * 4 > SCREEN_W * SCREEN_H;
}
}  // namespace

bool g_fast_idle = false;

// ---- Game states (boss mode) ----
// The recompiled code keeps the whole CPU in g_ctx and the emulated RAM; the
// native stack only mirrors the chain of PowerPC calls. Every frame the game
// waits for the vblank in one loop (IDLE in ppc_rt.h), called from its main
// loop, so a state taken there can be put back later, from inside the same
// loop, when the chain of saved return addresses on the PowerPC stack is the
// same: CPU registers, RAM, device memory (Real3D, 2D layer, banks) and the
// texture RAM are replaced; the timing, the sound board and the inputs carry on.
std::atomic<int> g_state_op{0};   // 0 nothing, 1 save, 2 load, 3 keep as home, 4 keep as the mark (read by IDLE)
namespace {
constexpr uint32_t STATE_MAGIC = 0x5357474C;   // "LGWS"
constexpr uint32_t STATE_VERSION = 2;

struct GameState {
    std::vector<uint32_t> chain;
    Ctx ctx{};
    std::vector<uint8_t> ram;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> pages;
    // Real3D pages unchanged since the last frame closed: shared with it
    // (r3d_flush keeps them immutable) instead of copied
    std::vector<std::pair<uint32_t, std::shared_ptr<const R3DPage>>> shared;
    std::vector<uint16_t> texture;
    std::vector<uint8_t> sound;   // empty without the sound board
    uint8_t midi_ctrl = 0;
};
std::mutex g_state_mutex;
std::string g_state_path;            // file to write (save)
std::unique_ptr<GameState> g_state;  // state to put back (load)
std::unique_ptr<GameState> g_home;   // the arcade game as it was before the first boss fight
std::unique_ptr<GameState> g_mark;   // survival: the arena's first stop, where each lap starts again
bool g_state_keep_home = false;
std::atomic<int> g_state_result{0};  // 1 done, -1 failed
uint64_t g_state_since = 0;   // the frame a load was asked at (it gives up 600 frames later)
bool g_logo_up = false;       // the boot has reached the SEGA logo (the game's main loop) once

// Return addresses saved on the PowerPC stack, innermost first
std::vector<uint32_t> call_chain(const uint8_t* ram, uint32_t sp) {
    auto rd = [&](uint32_t a) { return a < RAM_SIZE - 3 ? bswap32(*reinterpret_cast<const uint32_t*>(ram + a)) : 0u; };
    std::vector<uint32_t> chain;
    for (int i = 0; i < 32 && sp && sp < RAM_SIZE - 8; i++) {
        uint32_t prev = rd(sp);
        if (!prev || prev <= sp) break;
        chain.push_back(rd(prev + 4));
        sp = prev;
    }
    return chain;
}

// Zero runs and literal runs: the RAM and the device pages are mostly empty
void put_packed(FILE* f, const uint8_t* p, size_t n) {
    fwrite(&n, sizeof(n), 1, f);
    size_t i = 0;
    while (i < n) {
        uint32_t zeros = 0, lit = 0;
        while (i + zeros < n && p[i + zeros] == 0) zeros++;
        size_t s = i + zeros;
        while (s + lit < n) {
            size_t z = 0;
            while (s + lit + z < n && z < 16 && p[s + lit + z] == 0) z++;
            if (z >= 16 || s + lit + z == n) break;
            lit += uint32_t(z) + 1;
        }
        fwrite(&zeros, 4, 1, f);
        fwrite(&lit, 4, 1, f);
        fwrite(p + s, 1, lit, f);
        i = s + lit;
    }
}
bool get_packed(FILE* f, std::vector<uint8_t>& out) {
    size_t n = 0;
    if (fread(&n, sizeof(n), 1, f) != 1 || n > (64u << 20)) return false;
    out.assign(n, 0);
    size_t i = 0;
    while (i < n) {
        uint32_t zeros, lit;
        if (fread(&zeros, 4, 1, f) != 1 || fread(&lit, 4, 1, f) != 1 || i + zeros + lit > n) return false;
        i += zeros;
        if (fread(out.data() + i, 1, lit, f) != lit) return false;
        i += lit;
    }
    return true;
}

std::unique_ptr<GameState> capture_state(const Ctx& c) {
    auto st = std::make_unique<GameState>();
    st->chain = call_chain(g_ram, c.r[1]);
    st->ctx = c;
    st->ram.assign(g_ram, g_ram + RAM_SIZE);
    // A Real3D page not written since the last flush is the one that frame
    // holds (only this thread writes them and r3d_flush copies the dirty ones)
    const R3DFrameState* latest = g_r3d_latest.get();
    for (auto& [key, page] : g_dev) {
        uint32_t region = key >> 8;
        if (latest && (region == 0x8C || region == 0x8E || region == 0x98) && !g_r3d_dirty.count(key))
            if (auto it = latest->pages.find(key); it != latest->pages.end()) {
                st->shared.emplace_back(key, it->second);
                continue;
            }
        st->pages.emplace_back(key, std::vector<uint8_t>(page.begin(), page.end()));
    }
    g_texture_ram.copy_all(st->texture);
    g_sound.save_state(st->sound);
    st->midi_ctrl = g_midi_ctrl;
    return st;
}

bool write_state(const char* path, const GameState& st) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    uint32_t head[4] = {STATE_MAGIC, STATE_VERSION, uint32_t(sizeof(Ctx)), uint32_t(st.chain.size())};
    fwrite(head, 4, 4, f);
    fwrite(st.chain.data(), 4, st.chain.size(), f);
    fwrite(&st.ctx, sizeof(Ctx), 1, f);
    put_packed(f, st.ram.data(), st.ram.size());
    uint32_t count = uint32_t(st.pages.size() + st.shared.size());
    fwrite(&count, 4, 1, f);
    for (auto& [key, page] : st.pages) {
        fwrite(&key, 4, 1, f);
        put_packed(f, page.data(), page.size());
    }
    for (auto& [key, page] : st.shared) {
        fwrite(&key, 4, 1, f);
        put_packed(f, page->data(), page->size());
    }
    put_packed(f, reinterpret_cast<const uint8_t*>(st.texture.data()), st.texture.size() * 2);
    put_packed(f, st.sound.data(), st.sound.size());
    fwrite(&st.midi_ctrl, 1, 1, f);
    bool ok = ferror(f) == 0;
    fclose(f);
    return ok;
}

std::unique_ptr<GameState> read_state(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    auto st = std::make_unique<GameState>();
    uint32_t head[4];
    bool ok = fread(head, 4, 4, f) == 4 && head[0] == STATE_MAGIC && head[1] == STATE_VERSION &&
              head[2] == sizeof(Ctx) && head[3] <= 32;
    if (ok) {
        st->chain.resize(head[3]);
        ok = fread(st->chain.data(), 4, head[3], f) == head[3] && fread(&st->ctx, sizeof(Ctx), 1, f) == 1 &&
             get_packed(f, st->ram) && st->ram.size() == RAM_SIZE;
    }
    uint32_t count = 0;
    ok = ok && fread(&count, 4, 1, f) == 1 && count < 4096;
    for (uint32_t i = 0; ok && i < count; i++) {
        uint32_t key;
        std::vector<uint8_t> page;
        ok = fread(&key, 4, 1, f) == 1 && get_packed(f, page) && page.size() == 0x10000;
        if (ok) st->pages.emplace_back(key, std::move(page));
    }
    std::vector<uint8_t> tex;
    ok = ok && get_packed(f, tex) && tex.size() == 2048 * 2048 * 2;
    ok = ok && get_packed(f, st->sound) && fread(&st->midi_ctrl, 1, 1, f) == 1;
    fclose(f);
    if (!ok) return nullptr;
    st->texture.resize(tex.size() / 2);
    memcpy(st->texture.data(), tex.data(), tex.size());
    return st;
}

void apply_state(Ctx& c, const GameState& st) {
    uint64_t cycles = c.cycles, next_event = c.next_event;
    c = st.ctx;
    c.cycles = cycles;
    c.next_event = next_event;
    memcpy(g_ram, st.ram.data(), RAM_SIZE);
    for (auto& [key, page] : g_dev) memset(page.data(), 0, page.size());
    for (auto& [key, page] : st.pages) memcpy(dev_ptr(key << 16), page.data(), 0x10000);
    for (auto& [key, page] : st.shared) memcpy(dev_ptr(key << 16), page->data(), 0x10000);
    for (auto& [key, page] : g_dev)
        if ((key >> 8) == 0x8C || (key >> 8) == 0x8E || (key >> 8) == 0x98) g_r3d_dirty.insert(key);
    g_tilegen_dirty = (1u << TILEGEN_PAGES) - 1;
    g_texture_fifo.clear();
    g_fifo_consumed = 0;
    g_texture_ram.load_all(st.texture);
    if (g_sound.load_state(st.sound)) g_midi_ctrl = st.midi_ctrl;
}
}  // namespace

// Asks the game thread to save its state to a file / put one back, at its
// next wait for the vblank. game_state_result(): 0 pending, 1 done, -1 failed.
// keep_home: the game as it is just before the first load is kept in memory,
// and game_state_load_home() puts it back (to leave boss mode).
void game_state_save(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_state_writer.joinable()) g_state_writer.join();   // the previous file first (its result is read already)
    g_state_path = path;
    g_state_result = 0;
    g_state_op = 1;
}
static bool start_load(std::unique_ptr<GameState> st, bool keep_home) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_state_result = st ? 0 : -1;
    if (!st) return false;
    g_state = std::move(st);
    g_state_keep_home = keep_home;
    g_state_since = g_frame;
    g_state_op = 2;
    return true;
}
bool game_state_load(const std::string& path, bool keep_home) {
    return start_load(read_state(path.c_str()), keep_home);
}
bool game_state_load_home() {
    std::unique_ptr<GameState> st;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (g_home) st = std::make_unique<GameState>(*g_home);
    }
    return st && start_load(std::move(st), false);
}
// The game as it is becomes the home state (survival mode starts from it).
// Only the first time: with a home already kept it would only replace a load
// still waiting (QUIT TO MAIN MENU puts the home back when the menu closes,
// and CLASSIC ARCADE right after it used to cancel that load: the game went on
// from where it was left, stuck when that was a survival arena)
void game_state_keep_home() {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_home || g_state_op == 2) return;
    g_state_result = 0;
    g_state_since = g_frame;
    g_state_op = 3;
}
// The game as it is becomes the mark (replacing the last one); load_mark puts it back
void game_state_mark() {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_state_result = 0;
    g_state_since = g_frame;
    g_state_op = 4;
}
bool game_state_load_mark() {
    std::unique_ptr<GameState> st;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (g_mark) st = std::make_unique<GameState>(*g_mark);
    }
    return st && start_load(std::move(st), false);
}
bool game_state_has_home() {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    return g_home != nullptr;
}
int game_state_result() { return g_state_result; }
bool game_state_valid(const std::string& path) {   // a state file this build can read
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint32_t head[4] = {};
    bool ok = fread(head, 4, 4, f) == 4 && head[0] == STATE_MAGIC && head[1] == STATE_VERSION && head[2] == sizeof(Ctx);
    fclose(f);
    return ok;
}

void rt_state_point(Ctx& c) {
    if (g_in_irq) return;
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_state_op == 1) {
        // The copy is brief; the file is written by another thread, so the
        // game does not stop for the disk (the result comes when it is done)
        if (g_state_writer.joinable()) g_state_writer.join();
        g_state_writer = std::thread([path = g_state_path, st = std::shared_ptr<GameState>(capture_state(c))] {
            g_state_result = write_state(path.c_str(), *st) ? 1 : -1;
        });
        g_state_op = 0;
    } else if (g_state_op == 2 && g_state) {
        // Only from the same chain of calls (the main loop); anywhere else
        // (a loading screen waiting in a nested loop) it tries again later.
        // Counted in frames: a nested wait goes round its loop thousands of
        // times a frame, and 600 of those made a load fail now and then (QUIT
        // TO MAIN MENU leaving the game in the survival arena)
        if (call_chain(g_ram, c.r[1]) == g_state->chain) {
            if (g_state_keep_home && !g_home) g_home = capture_state(c);
            apply_state(c, *g_state);
            stats_state_loaded();
            g_state_result = 1;
        } else if (g_boot_skip || g_frame - g_state_since < 600) {
            return;
        } else {
            g_state_result = -1;
        }
        g_state.reset();
        g_state_op = 0;
    } else if (g_state_op == 3) {
        // Not before the SEGA logo: the boot screens wait in nested loops, and
        // a home kept in one of them could never be loaded again (the game
        // does not come back to that chain of calls)
        if (!g_logo_up) return;
        if (!g_home) g_home = capture_state(c);
        g_state_result = 1;
        g_state_op = 0;
    } else if (g_state_op == 4) {
        g_mark = capture_state(c);
        g_state_result = 1;
        g_state_op = 0;
    }
}

// The place caption shown as each stage starts ("-Crossing point of rescue
// parties.") does not go through the subtitle function: it is one more image
// of the text table at 0x134B28 (16 bytes per text: Japanese image and width
// in tiles, English image and width). The caption is centred from its width
// and not drawn when it would start off the screen, so with subtitles off
// these widths are made too wide to fit; the originals come back when they are
// turned on again. Checked every frame: the table is reloaded at power-on.
static void hide_stage_captions(bool hide) {
    static const uint32_t IDS[] = {5, 18, 20, 46};
    static uint32_t orig[4][2];
    for (int i = 0; i < 4; i++)
        for (int lang = 0; lang < 2; lang++) {
            uint32_t a = 0x134B28 + IDS[i] * 16 + 4 + lang * 8;
            // not loaded yet (or the power-on RAM test running): leave it alone
            if (rd32(a - 4) >> 24 != 0xFF) return;
            uint32_t w = rd32(a);
            if (w != 0x7E) orig[i][lang] = w < 0x40 ? w : 0;
            uint32_t want = hide ? 0x7E : orig[i][lang];
            if (w != want && orig[i][lang]) wr32(a, want);
        }
}

// ---- Boss mode: making the boss states ----
// --make-boss-state N plays by itself from the attract to boss N: it puts a
// credit in, starts, picks the boss's stage in the course select (the run's
// stage list at 0x1C2AFC) and keeps shooting, with its lives frozen; when the
// stage script reaches the fight it clears the score and saves the state.
int g_boss_maker = -1;   // index in BOSSES, -1 when not making one

static void boss_maker_frame() {
    const BossInfo& b = BOSSES[g_boss_maker];
    static uint8_t lives = 0;
    static bool saving = false;
    uint64_t f = g_frame;
    std::string path = boss_state_path(g_boss_maker);
    if (saving) {
        int r = game_state_result();
        if (r == 0) return;
        bool ok = r > 0 && MoveFileExA((path + ".tmp").c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
        printf("[boss] %ls: %s (frame %llu; %u notes restarted on stopped voices)\n", b.name,
               ok ? "state saved" : "could not save", (unsigned long long)f, unsigned(Scsp::restarted()));
        exit_from_game_thread(ok ? 0 : 1);
    }
    if (f > 250000) {
        printf("[boss] %ls: the game did not reach the boss\n", b.name);
        exit_from_game_thread(1);
    }
    if (f < 900) return;   // past the boot (a button held there changes what it does)
    uint32_t select = rd32(0x1C2AD8);   // course select: 0 attract, 1-2 choosing, 0xC playing
    if (select == 0) {                  // a credit and start, again until the course select opens
        auto_coin_start(g_inputs.p[0], f);
        return;
    }
    g_inputs.p[0].coin = g_inputs.p[0].start = false;
    if (select != 0xC) {   // the boss's stage first in the run
        if (int32_t(rd32(0x1C2B10)) == -1) wr32(0x1C2AFC, uint32_t(b.stage_index));
        return;
    }
    if (!lives) lives = rd8(0x1A3723);
    if (lives) wr8(0x1A3723, lives);
    if (rd8(0x1A623C) == b.stage_index && int32_t(rd32(0x1A841C)) == b.label && int32_t(rd32(0x1A847C)) >= b.index) {
        g_inputs.p[0].trigger = g_inputs.p[0].offscreen = false;
        wr32(0x1A857C, 0);   // the score starts at zero
        wr32(0x1A8594, 0);
        game_state_save(path + ".tmp");
        saving = true;
        return;
    }
    auto_sweep_fire(g_inputs.p[0], f);
}

static void enemy_marks_frame();

void rt_event(Ctx& c) {
    if (c.cycles >= g_next_sample) {
        g_samples[c.pc_hint]++;
        g_next_sample = c.cycles + SAMPLE_CYCLES;
    }
    while (c.cycles >= g_next_phase) {
        switch (g_phase) {
        case 0: {   // start of VBlank: new frame
            g_frame++;
            g_frame_start = g_next_phase;
            enemy_marks_frame();
            {   // CPU time of the frame just finished, without the waits
                static double last_frame_time = 0;
                double now_s = now_seconds();
                if (last_frame_time > 0) {
                    g_game_busy_us += uint64_t(std::max(now_s - last_frame_time - g_wait_seconds, 0.0) * 1e6);
                    g_game_frames++;
                }
                last_frame_time = now_s;
                g_wait_seconds = 0;
            }
            {   // the game sets its language flag (0x1A6168) once at start: a later
                // change in the settings is written there so the next text uses it
                static int applied = g_subtitles;
                if (g_subtitles != applied) {
                    applied = g_subtitles;
                    wr32(0x1A6168, rt_text_region(rd8(0x1226)));
                }
            }
            hide_stage_captions(g_subtitles == 0);
            for (int p = 0; p < 2; p++) {   // player slots (0x10 bytes each): +0xD = 1 or 2 while playing
                uint8_t state = rd8(0x1A3791 + 0x10 * p);
                g_inputs.p[p].playing = state == 1 || state == 2;
            }
            if (g_frame % 60 == 0) {
                static ULONGLONG last_ms = GetTickCount64();
                ULONGLONG now = GetTickCount64();
                double fps = now > last_ms ? 60000.0 / double(now - last_ms) : 0.0;
                last_ms = now;
                printf("  frame %llu  (pc ~%08X)  %.0f fps\n", (unsigned long long)g_frame, c.pc_hint, fps);
                fflush(stdout);
            }
            g_auto.apply(g_frame);
            if (survival_frame(c)) g_hold = true;   // survival start and results screens
            stats_frame();
            random_spawns_frame();
            if (g_boss_maker >= 0) boss_maker_frame();
            video_snapshot();
            if (!g_logo_up && ((g_frame % 4 == 0 && boot_logo_up()) || g_frame > 3000)) g_logo_up = true;
            // Survival mode: on to the chosen stage at full speed, behind its loading screen
            bool boot_done = survival_enabled() ? !survival_owns_inputs() || g_frame > 200000   // (survival gives up first)
                                                : (g_frame % 4 == 0 && boot_logo_up()) || g_frame > 3000;
            if (g_boot_skip && boot_done) {
                g_boot_skip = g_fast_idle = false;
                // The arcade game at its SEGA logo: the home state, also when
                // CLASSIC ARCADE was picked before the boot got here (it keeps
                // it only once the boot is over)
                if (!survival_enabled()) game_state_keep_home();
                g_sound.set_dsp_enabled(true);
                g_sound.set_muted(false);   // fades in from silence
                printf("[boot] start screens skipped in %.1f s (frame %llu)\n",
                       double(GetTickCount64() - g_boot_ms) / 1000.0, (unsigned long long)g_frame);
                LARGE_INTEGER now;   // real-time pacing starts from here
                QueryPerformanceCounter(&now);
                g_start_time.QuadPart = now.QuadPart - LONGLONG(double(g_frame) / FRAME_HZ * double(g_qpf.QuadPart));
            }
            if (g_hold && !g_boot_skip) {
                double hold_from = now_seconds();
                while (g_hold && !g_quit) Sleep(4);
                g_wait_seconds += now_seconds() - hold_from;
                LARGE_INTEGER now;   // carry on at real speed from here, without catching up
                QueryPerformanceCounter(&now);
                g_start_time.QuadPart = now.QuadPart - LONGLONG(double(g_frame) / FRAME_HZ * double(g_qpf.QuadPart));
            }
            if (g_realtime && !g_boot_skip) pace_frame();
            frame_log("G %llu %.6f\n", (unsigned long long)g_frame, now_seconds());
            if (g_frame >= uint64_t(g_frame_limit)) {
                report();
                exit_from_game_thread(0);
            }
            if (g_quit) {   // the window is closing: stand still until the process ends
                g_parked = true;
                for (;;) Sleep(INFINITE);
            }
            g_sound.run_frame(FRAME_HZ);
            g_midi_pulses = (g_midi_ctrl & 0x20) ? 128 : 0;
            g_phase = 1;
            g_next_phase = g_frame_start + 40 * LINE_CYCLES;
            break;
        }
        case 1:   // end of VBlank
            g_irq_state |= 0x0D;
            g_midi_pulses = 0;
            g_phase = 2;
            g_next_phase = g_frame_start + (40 + std::min<uint32_t>(383, flip_line())) * LINE_CYCLES;
            break;
        case 2:   // Real3D ping-pong flip (line set in tilegen register 0x08)
            g_real3d_status ^= 0x00000002;   // the game reads it with lwz+stwbrx: bit 0x02000000 after the swap
            g_phase = 3;
            g_next_phase = g_frame_start + (40 + 383) * LINE_CYCLES;
            break;
        case 3:   // start of the last active line: VBlank interrupt
            g_irq_state |= 0x02;
            g_phase = 0;
            g_next_phase = g_frame_start + FRAME_CYCLES;
            break;
        }
    }
    // MIDI interrupts while the sound board wants commands (during VBlank)
    if (g_midi_pulses > 0 && (g_midi_ctrl & 0x20) && (g_irq_enable & 0x40) && !(g_irq_state & 0x40) &&
        c.cycles >= g_midi_next) {
        g_irq_state |= 0x40;
        g_midi_pulses--;
        g_midi_next = c.cycles + 1000;
    }
    if (!g_in_irq && (c.msr & MSR_EE)) {
        if (g_dec_armed && c.cycles >= g_dec_base + uint64_t(g_dec_value) * 4) {
            g_dec_armed = false;
            g_dec_irqs++;
            exception(c, 0x900);
        }
        uint8_t pend = g_irq_state & g_irq_enable;
        if (pend) {
            for (int i = 0; i < 8; i++) if (pend & (1 << i)) g_irq_count[i]++;
            exception(c, 0x500);
        }
    }
    schedule(c);
}

uint32_t rt_mfspr(Ctx& c, uint32_t spr) {
    switch (spr) {
    case 22: return g_dec_value - uint32_t((c.cycles - g_dec_base) / 4);
    case 268: return uint32_t((c.cycles - g_tb_base) / 4);
    case 269: return uint32_t(((c.cycles - g_tb_base) / 4) >> 32);
    default: return c.spr[spr];
    }
}

void rt_mtspr(Ctx& c, uint32_t spr, uint32_t v) {
    if (spr == 22) {
        g_dec_value = v;
        g_dec_base = c.cycles;
        g_dec_armed = !(v & 0x80000000u);
        schedule(c);
        return;
    }
    if (spr == 284) {   // TBL: only used to reset it to 0
        g_tb_base = c.cycles - uint64_t(v) * 4;
        return;
    }
    c.spr[spr] = v;
}

void rt_mtmsr(Ctx& c, uint32_t v) {
    c.msr = v;
    schedule(c);
}

void rt_lswi(Ctx& c, uint32_t ea, int rd, int nb) {
    int r = rd - 1, i = 0;
    while (nb > 0) {
        if (i == 0) { r = (r + 1) & 31; c.r[r] = 0; }
        c.r[r] |= uint32_t(rd8(ea++)) << (24 - i);
        i = (i + 8) & 31;
        nb--;
    }
}

void rt_stswi(Ctx& c, uint32_t ea, int rs, int nb) {
    int r = rs - 1, i = 0;
    while (nb > 0) {
        if (i == 0) r = (r + 1) & 31;
        wr8(ea++, uint8_t(c.r[r] >> (24 - i)));
        i = (i + 8) & 31;
        nb--;
    }
}

// The board is set to Japan (COUNTRY in the EEPROM, not editable in the test
// menu): the subtitles and hints come as Japanese images. Every one also has an
// English version in the ROM, used when the country is not Japan. Only the
// game's language flag sees USA; code that reads the setting directly (boot
// warning, coin defaults) keeps the board's value. With subtitles off the
// CONTINUE hints still show, in English.
uint32_t rt_text_region(uint32_t country) { return country == 0 && g_subtitles != 2 ? 1 : country; }
bool rt_subtitles_off() { return g_subtitles == 0; }

// Callers of fn_0003BE44 (return addresses) that are enemies: the raptor task
// (fn_00053D9C) and the one at 0x61E04. Others project other things (0x4F640:
// a fixed object of the scenery). LW_PROJ_LOG=path logs every call, to find more
static const uint32_t ENEMY_PROJECTIONS[] = {0x00053EB0, 0x00061E04};
static std::vector<EnemyMark> g_marks_building, g_marks;
static std::mutex g_marks_mutex;

void rt_projected(uint32_t lr, uint32_t vec, uint32_t px, uint32_t py) {
    static FILE* log = getenv("LW_PROJ_LOG") ? fopen(getenv("LW_PROJ_LOG"), "w") : nullptr;
    float sx = float(f2d(rd32(px))), sy = float(f2d(rd32(py))), vz = float(f2d(rd32(vec + 8)));
    float k = float(f2d(rd32(0x1A33FC)));   // the projection's focal length in pixels
    if (log) fprintf(log, "%llu %08X %.1f %.1f %.2f\n", (unsigned long long)g_frame, lr, 248 + sx, 192 - sy, vz);
    if (std::find(std::begin(ENEMY_PROJECTIONS), std::end(ENEMY_PROJECTIONS), lr) == std::end(ENEMY_PROJECTIONS)) return;
    float d = std::fabs(vz);
    if (vz <= 0 || !(d > 0.1f) || !std::isfinite(sx) || !std::isfinite(sy)) return;
    g_marks_building.push_back({248 + sx, 192 - sy, k / d});
}

// The gun's aim in a stage, as fn_0000CBF0 made it from the ADC values (x, y
// at `vec`, in units from the middle of the screen): in a wide 3D view it
// goes where the gun points, also in the side bands beyond the 4:3 screen
// (gun_aim_exact). The game aims its shots and draws its X from there
void rt_gun_aimed(uint32_t vec, uint32_t player) {
    float x, y;
    if (player > 1 || !gun_aim_exact(int(player), x, y)) return;
    wr32(vec, d2fbits(x));
    wr32(vec + 4, d2fbits(y));
}

static void enemy_marks_frame() {   // start of a frame: the last one's list is complete
    std::lock_guard<std::mutex> lock(g_marks_mutex);
    g_marks.swap(g_marks_building);
    g_marks_building.clear();
}

std::vector<EnemyMark> enemy_marks() {
    std::lock_guard<std::mutex> lock(g_marks_mutex);
    return g_marks;
}
