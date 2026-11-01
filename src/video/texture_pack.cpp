#include "texture_pack.h"

#include "../common/fnv1a.h"

#include <windows.h>

#include <cstdio>

namespace {

// Same decoding as r3d.frag decode(), to 8-bit RGBA
void decode(uint16_t t, uint32_t fmt, uint8_t* o) {
    auto lum = [&](uint32_t l, uint32_t max, bool opaque, uint32_t a) {
        o[0] = o[1] = o[2] = uint8_t((l * 255 + max / 2) / max);
        o[3] = opaque ? uint8_t(a) : uint8_t(0);
    };
    auto l4 = [&](uint32_t l, uint32_t a) { lum(l, 15, true, (a * 255 + 7) / 15); };
    switch (fmt) {
    case 0:
        o[0] = uint8_t((((t >> 10) & 31) * 255 + 15) / 31);
        o[1] = uint8_t((((t >> 5) & 31) * 255 + 15) / 31);
        o[2] = uint8_t(((t & 31) * 255 + 15) / 31);
        o[3] = (t & 0x8000) ? 0 : 255;
        break;
    case 1: l4(t & 15, (t >> 4) & 15); break;
    case 2: l4((t >> 4) & 15, t & 15); break;
    case 3: l4((t >> 8) & 15, (t >> 12) & 15); break;
    case 4: l4((t >> 12) & 15, (t >> 8) & 15); break;
    case 5: lum(t & 255, 255, (t & 255) != 255, 255); break;
    case 6: lum((t >> 8) & 255, 255, ((t >> 8) & 255) != 255, 255); break;
    case 7:
        o[0] = uint8_t(((t >> 12) & 15) * 17);
        o[1] = uint8_t(((t >> 8) & 15) * 17);
        o[2] = uint8_t(((t >> 4) & 15) * 17);
        o[3] = uint8_t((t & 15) * 17);
        break;
    default: {
        uint32_t n = (t >> (4 * (fmt - 8))) & 15;
        lum(n, 15, n != 15, 255);
    }
    }
}

}  // namespace

void TexturePack::init(const std::string& dir, const std::string& dump_dir) {
    dir_ = dir;
    dump_dir_ = dump_dir;
    list_hashes(dir, available_);
    if (!dump_dir_.empty()) {
        CreateDirectoryA(dump_dir_.c_str(), nullptr);
        // Textures already dumped by earlier runs are not written again
        list_hashes(dump_dir_, dumped_);
    }
}

uint32_t TexturePack::lookup(uint32_t tex, R3DTextureRam& ram) {
    int x = int(tex & 2047), y = int((tex >> 11) & 2047);
    int w = 32 << ((tex >> 22) & 7), h = 32 << ((tex >> 25) & 7);
    uint64_t generation = ram.texture_generation(x, y, w, h);
    auto it = cache_.find(tex);
    if (it != cache_.end() && it->second.generation == generation) return it->second.slot;
    uint32_t slot = resolve(tex, ram, generation);
    cache_[tex] = {generation, slot};
    return slot;
}

uint32_t TexturePack::resolve(uint32_t tex, R3DTextureRam& ram, uint64_t& generation) {
    int x = int(tex & 2047), y = int((tex >> 11) & 2047);
    int w = 32 << ((tex >> 22) & 7), h = 32 << ((tex >> 25) & 7);
    uint32_t fmt = tex >> 28;
    generation = ram.read_texture(x, y, w, h, texels_);
    rgba_.resize(texels_.size() * 4);
    for (size_t i = 0; i < texels_.size(); i++) decode(texels_[i], fmt, &rgba_[i * 4]);
    uint32_t dims[2] = {uint32_t(w), uint32_t(h)};
    uint64_t hash = fnv1a_bytes(rgba_.data(), rgba_.size(), fnv1a_bytes(dims, sizeof(dims)));
    char name[32];
    snprintf(name, sizeof(name), "/%016llx.png", (unsigned long long)hash);

    if (!dump_dir_.empty() && dumped_.insert(hash).second) png_save(dump_dir_ + name, w, h, rgba_.data());

    auto loaded = slots_.find(hash);
    if (loaded != slots_.end()) return loaded->second;
    if (!available_.count(hash) || slots_.size() + 1 >= max_slots_) return 0;
    RgbaImage img;
    if (!png_load(dir_ + name, img.w, img.h, img.rgba)) {
        fprintf(stderr, "[textures] could not read %s%s\n", dir_.c_str(), name);
        available_.erase(hash);
        return 0;
    }
    uint32_t slot = uint32_t(slots_.size() + 1);
    img.slot = slot;
    slots_[hash] = slot;
    pending_.push_back(std::move(img));
    return slot;
}

std::vector<RgbaImage> TexturePack::take_new_images() {
    std::vector<RgbaImage> out;
    out.swap(pending_);
    return out;
}
