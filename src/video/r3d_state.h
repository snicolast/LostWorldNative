// Real3D memory as seen by the renderer.
//
// The game thread writes culling/polygon RAM through the emulated bus. When it
// writes the command port (0x88000000, "flush") the current contents are
// published as an immutable FrameState: unchanged 64 KB pages are shared with
// the previous state, so publishing is cheap. The render thread grabs the
// latest FrameState without blocking the game.
//
// See docs/real3d_spec.md for all formats. All device words are the
// little-endian interpretation of the bytes the CPU wrote (spec 1.2).
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

using R3DPage = std::array<uint8_t, 0x10000>;

struct R3DFrameState {
    // Key = PPC address >> 16 (0x8C00.., 0x8E00.., 0x9800..)
    std::unordered_map<uint32_t, std::shared_ptr<const R3DPage>> pages;
    uint64_t frame = 0;
    // Seconds (performance counter): `time` is when the frame belongs on the
    // game's own timeline (the vblank it was made in, so frames are evenly
    // spaced for the interpolation), `flushed` when the game finished it
    double time = 0, flushed = 0;
    uint64_t vblank = 0;   // the game's frame counter when it was finished

    uint32_t read(uint32_t ppc_addr) const {
        auto it = pages.find(ppc_addr >> 16);
        if (it == pages.end()) return 0;
        uint32_t v;
        memcpy(&v, it->second->data() + (ppc_addr & 0xFFFC), 4);
        return v;   // little-endian device word
    }
    // Culling address space (word addresses): 0x000000-0x0FFFFF low, 0x800000-0x83FFFF high
    bool cull_valid(uint32_t addr) const {
        addr &= 0xFFFFFF;
        return addr < 0x100000 || (addr >= 0x800000 && addr < 0x840000);
    }
    uint32_t cull(uint32_t addr) const {
        addr &= 0xFFFFFF;
        if (addr >= 0x800000 && addr < 0x840000) return read(0x8E000000 + (addr & 0x3FFFF) * 4);
        if (addr < 0x100000) return read(0x8C000000 + addr * 4);
        return 0;
    }
    float cullf(uint32_t addr) const {
        uint32_t v = cull(addr);
        float f;
        memcpy(&f, &v, 4);
        return f;
    }
    uint32_t poly(uint32_t addr) const { return read(0x98000000 + (addr & 0xFFFFF) * 4); }
};

// The same reads with the page of the last one kept per memory (culling and
// polygon RAM: a scene build reads thousands of words in a row from each), so
// most reads skip the page lookup. For one thread, over one frame at a time
class R3DStateReader {
public:
    void attach(const R3DFrameState* s) {
        s_ = s;
        for (Slot& slot : slots_) slot = Slot{};
    }
    const R3DFrameState* state() const { return s_; }

    bool cull_valid(uint32_t addr) const { return s_->cull_valid(addr); }
    uint32_t cull(uint32_t addr) {
        addr &= 0xFFFFFF;
        if (addr >= 0x800000 && addr < 0x840000) return read(0x8E000000 + (addr & 0x3FFFF) * 4, slots_[1]);
        if (addr < 0x100000) return read(0x8C000000 + addr * 4, slots_[0]);
        return 0;
    }
    float cullf(uint32_t addr) {
        uint32_t v = cull(addr);
        float f;
        memcpy(&f, &v, 4);
        return f;
    }
    uint32_t poly(uint32_t addr) { return read(0x98000000 + (addr & 0xFFFFF) * 4, slots_[2]); }

private:
    struct Slot {
        uint32_t key = ~0u;              // PPC address >> 16 of the page below, ~0 = none looked up
        const uint8_t* page = nullptr;   // null: the frame has no such page (reads 0)
    };
    uint32_t read(uint32_t ppc_addr, Slot& slot) {
        uint32_t key = ppc_addr >> 16;
        if (key != slot.key) {
            slot.key = key;
            auto it = s_->pages.find(key);
            slot.page = it == s_->pages.end() ? nullptr : it->second->data();
        }
        if (!slot.page) return 0;
        uint32_t v;
        memcpy(&v, slot.page + (ppc_addr & 0xFFFC), 4);
        return v;
    }
    const R3DFrameState* s_ = nullptr;
    Slot slots_[3];   // culling RAM low, culling RAM high, polygon RAM
};

// 2048x2048 16-bit texture RAM (two 2048x1024 pages), filled from the
// texture FIFO at each flush (spec 6.1 - 6.4).
class R3DTextureRam {
public:
    R3DTextureRam() : texels_(2048 * 2048, 0), generation_(64 * 64, 0) {}

    // Parses the FIFO words collected since the last flush
    void upload_fifo(const std::vector<uint32_t>& fifo);
    // Texture port (0x90000000): texels read straight from VROM/polygon RAM
    void upload_direct(uint32_t header, const uint16_t* src, size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        upload(header, src, count);
    }

    // Debugging: raw 16-bit texture RAM to a file
    void dump(const char* path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (FILE* f = fopen(path, "wb")) {
            fwrite(texels_.data(), 2, texels_.size(), f);
            fclose(f);
        }
    }
    // Debug snapshots (F9 / --replay): whole texture RAM
    void copy_all(std::vector<uint16_t>& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        out = texels_;
    }
    void load_all(const std::vector<uint16_t>& in) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (in.size() != texels_.size()) return;
        texels_ = in;
        for (auto& g : generation_) g++;
        dirty_y0_ = 0;
        dirty_y1_ = 2048;
    }
    // Copies rows touched since the last call (returns false if nothing changed)
    bool take_dirty(std::vector<uint16_t>& out, int& y0, int& y1);

    // Texture packs: copies a w x h texture at (x, y) with the same wrapping
    // as the shader (x over the whole RAM, y inside its 1024-row page) and
    // returns a value that changes whenever any texel of it is rewritten
    uint64_t read_texture(int x, int y, int w, int h, std::vector<uint16_t>& out);
    uint64_t texture_generation(int x, int y, int w, int h);

private:
    void upload(uint32_t header, const uint16_t* src, size_t count);
    void store_level(int x, int y, int w, int h, const uint16_t* src, size_t count, bool bit16, uint32_t header,
                     size_t& used);

    std::mutex mutex_;
    std::vector<uint16_t> texels_;
    std::vector<uint32_t> generation_;   // write counter per 32x32 block
    uint64_t generation_locked(int x, int y, int w, int h) const;
    int dirty_y0_ = 2048, dirty_y1_ = 0;
};
