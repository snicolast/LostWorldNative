#include "sky_pack.h"

#include "../common/fnv1a.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "sky_procedural.h"
#include "tilegen.h"

static_assert(SkyPack::SIZE == SKY_SIZE && SkyPack::BANDS == SKY_BANDS);

namespace {

void save_plane(const std::string& dir, uint64_t hash, const std::vector<uint32_t>& plane) {
    CreateDirectoryA(dir.c_str(), nullptr);
    char name[32];
    snprintf(name, sizeof(name), "/%016llx.png", (unsigned long long)hash);
    png_save_bgra(dir + name, SkyPack::SIZE, SkyPack::SIZE, plane.data());
    printf("[sky] backdrop saved to %s%s\n", dir.c_str(), name);
}
}  // namespace

void SkyPack::init(const std::string& dir, const std::string& dump_dir) {
    dir_ = dir;
    dump_dir_ = dump_dir;
    pictures_.reserve(LAST_SLOT - FIRST_SLOT + 1);   // find() hands out pointers into it
    capture_dir_ = dump_dir.empty() ? "out/sky" : dump_dir;
    list_hashes(dir, available_);
    for (const std::string& name : list_files(dir, "*.txt")) {
        unsigned long long hash = 0;
        if (sscanf(name.c_str(), "%16llx", &hash) != 1) continue;
        FILE* f = fopen((dir + "/" + name).c_str(), "r");
        if (!f) continue;
        char line[128];
        Keep k;
        while (fgets(line, sizeof(line), f))
            if (sscanf(line, "keep %d %d %d %d", &k.x, &k.y, &k.w, &k.h) == 4 && k.w > 0 && k.h > 0) {
                int i = 0;
                while (i < int(keeps_.size()) && memcmp(&keeps_[i], &k, sizeof(k)) != 0) i++;
                if (i == int(keeps_.size())) keeps_.push_back(k);
                keep_of_[hash] = i;
                break;   // one box per replacement
            }
        fclose(f);
    }
    if (!dump_dir_.empty()) list_hashes(dump_dir_, dumped_);   // already written by earlier runs
    if (!available_.empty()) printf("[sky] %zu backdrop replacements\n", available_.size());
}

const SkyPack::Picture* SkyPack::find(const uint8_t* vram, int layer, bool four_bpp) {
    // The picture is only rebuilt and hashed when the layer's data changes
    uint64_t key = TileGen::layer_key(vram, layer, four_bpp);
    auto it = hashes_.find(key);
    bool rendered = false, seen = it != hashes_.end();
    uint64_t hash;
    if (seen) {
        hash = it->second;
    } else {
        if (hashes_.size() > 4096) hashes_.clear();   // animated title screens
        TileGen::layer_plane(vram, layer, four_bpp, plane_);
        rendered = true;
        hash = fnv1a_words(plane_.data(), plane_.size());
        hashes_[key] = hash;
    }
    bool dump = !dump_dir_.empty() && !seen && dumped_.insert(hash).second;
    if (dump || capture_) {
        if (!rendered) TileGen::layer_plane(vram, layer, four_bpp, plane_);
        save_plane(capture_ ? capture_dir_ : dump_dir_, hash, plane_);
    }
    // Procedural: skies are 8 bpp pictures in the back pair (layer 3, the
    // alternate, is the one that continues above) that wrap around as a
    // panorama, unlike framed pictures such as the title screen
    if (procedural_on_ && layer >= 2 && !four_bpp) {
        auto p = panorama_.find(hash);
        if (p == panorama_.end()) {
            if (!rendered) TileGen::layer_plane(vram, layer, four_bpp, plane_);
            rendered = true;
            int opaque = 0, lit = 0, edge = 0;
            for (int y = 0; y < SIZE; y++)
                for (int x = 0; x < SIZE; x++) {
                    uint32_t c = plane_[y * SIZE + x];
                    opaque += (c >> 24) != 0;
                    if (x >= 496) {   // the columns the screen does not show
                        edge++;
                        lit += ((c >> 16) & 255) >= 24 || ((c >> 8) & 255) >= 24 || (c & 255) >= 24;
                    }
                }
            p = panorama_.emplace(hash, opaque > SIZE * SIZE * 95 / 100 && lit * 10 > edge * 9).first;
        }
        if (p->second) {
            if (!procedural_.count(hash) && !rendered) TileGen::layer_plane(vram, layer, four_bpp, plane_);
            return procedural(hash, layer == 3);
        }
    }
    if (available_.count(hash)) return load(hash);
    uint64_t kept = with_keep(hash, vram, layer, four_bpp);
    return kept ? load(kept) : nullptr;
}

// The replacement that matches the picture once its box is cleared, 0 = none
uint64_t SkyPack::with_keep(uint64_t hash, const uint8_t* vram, int layer, bool four_bpp) {
    if (keeps_.empty()) return 0;
    auto it = kept_.find(hash);
    if (it != kept_.end()) return it->second;
    if (kept_.size() > 4096) kept_.clear();
    TileGen::layer_plane(vram, layer, four_bpp, plane_);
    uint64_t found = 0;
    std::vector<uint32_t> cleared;
    for (int i = 0; i < int(keeps_.size()) && !found; i++) {
        const Keep& k = keeps_[i];
        cleared = plane_;
        for (int y = (std::max)(k.y, 0); y < (std::min)(k.y + k.h, SIZE); y++)
            for (int x = (std::max)(k.x, 0); x < (std::min)(k.x + k.w, SIZE); x++) cleared[y * SIZE + x] = 0;
        uint64_t h = fnv1a_words(cleared.data(), cleared.size());
        auto o = keep_of_.find(h);
        if (o != keep_of_.end() && o->second == i && available_.count(h)) found = h;
    }
    kept_[hash] = found;
    return found;
}

void SkyPack::frame_done() {
    capture_ = false;
    frame_++;
}

const SkyPack::Picture* SkyPack::load(uint64_t hash) {
    auto it = loaded_.find(hash);
    if (it != loaded_.end()) {
        pictures_[it->second].used = frame_;
        return &pictures_[it->second];
    }
    // A free slot, or else the one of the picture unused longest (a screen
    // with many states, like the stage select, cycles through its pictures)
    int index = int(pictures_.size());
    uint32_t capacity = LAST_SLOT - uint32_t(procedural_.size()) - FIRST_SLOT + 1;
    if (pictures_.size() >= capacity) {
        index = -1;
        for (int i = 0; i < int(pictures_.size()); i++)
            if (pictures_[i].used < frame_ && (index < 0 || pictures_[i].used < pictures_[index].used)) index = i;
    }
    uint32_t slot = index < 0 ? LAST_SLOT + 1 : index < int(pictures_.size()) ? pictures_[index].slot
                                                                                : FIRST_SLOT + uint32_t(index);
    RgbaImage img;
    char name[32];
    snprintf(name, sizeof(name), "/%016llx.png", (unsigned long long)hash);
    if (index < 0) return nullptr;   // every slot is on screen this frame: the original stays
    if (slot > LAST_SLOT - uint32_t(procedural_.size()) || !png_load(dir_ + name, img.w, img.h, img.rgba)) {
        fprintf(stderr, "[sky] could not load %s%s\n", dir_.c_str(), name);
        available_.erase(hash);
        return nullptr;
    }
    img.slot = slot;
    // Reduction to the layer's own resolution: average of the source area
    // behind each pixel, opaque where most of it is
    Picture p;
    p.slot = slot;
    p.low.resize(SIZE * SIZE);
    for (int y = 0; y < SIZE; y++) {
        int y0 = y * img.h / SIZE, y1 = (std::max)(y0 + 1, (y + 1) * img.h / SIZE);
        for (int x = 0; x < SIZE; x++) {
            int x0 = x * img.w / SIZE, x1 = (std::max)(x0 + 1, (x + 1) * img.w / SIZE);
            uint64_t sum[4] = {0, 0, 0, 0};
            for (int sy = y0; sy < y1; sy++)
                for (int sx = x0; sx < x1; sx++) {
                    const uint8_t* s = &img.rgba[(size_t(sy) * img.w + sx) * 4];
                    for (int c = 0; c < 3; c++) sum[c] += uint64_t(s[c]) * s[3];
                    sum[3] += s[3];
                }
            uint32_t n = uint32_t((x1 - x0) * (y1 - y0));
            if (sum[3] * 2 < uint64_t(n) * 255) continue;
            p.low[y * SIZE + x] = 0xFF000000u | uint32_t(sum[0] / sum[3]) << 16 | uint32_t(sum[1] / sum[3]) << 8 |
                                  uint32_t(sum[2] / sum[3]);
        }
    }
    printf("[sky] replacement %s%s (%dx%d)\n", dir_.c_str(), name, img.w, img.h);
    pending_.push_back(std::move(img));
    p.hash = hash;
    p.used = frame_;
    auto keep = keep_of_.find(hash);
    if (keep != keep_of_.end()) {
        const Keep& k = keeps_[keep->second];
        p.keep_x = k.x, p.keep_y = k.y, p.keep_w = k.w, p.keep_h = k.h;
    }
    if (index < int(pictures_.size())) {   // takes over a slot
        loaded_.erase(pictures_[index].hash);
        pictures_[index] = std::move(p);
    } else {
        pictures_.push_back(std::move(p));
    }
    loaded_[hash] = index;
    return &pictures_[index];
}

// ---- Procedural sky (sky_procedural.h) ----
// Without the clouds' drift: it stands in for the layer's own pixels (haze,
// bloom, horizon). plane_ holds the original picture
const SkyPack::Picture* SkyPack::procedural(uint64_t hash, bool upper) {
    auto it = procedural_.find(hash);
    if (it != procedural_.end()) {
        if (!upper) last_lower_ = &it->second;
        return &it->second;
    }
    uint32_t slot = LAST_SLOT - uint32_t(procedural_.size());
    if (slot < FIRST_SLOT + uint32_t(pictures_.size())) return nullptr;   // no slot left: the original stays
    Picture& p = procedural_[hash];
    p.slot = slot;
    p.upper = upper;
    p.procedural = true;
    p.low.resize(SIZE * SIZE);
    // Colours per band: the clear sky (a quarter of the way up from the
    // darkest) and the lit clouds (nine tenths), leaving out the trees
    auto lum = [](uint32_t c) { return 0.2126f * ((c >> 16) & 255) + 0.7152f * ((c >> 8) & 255) + 0.0722f * (c & 255); };
    for (int band = 0; band < BANDS; band++) {
        std::vector<uint32_t> px;
        for (int y = band * 16; y < band * 16 + 16; y++)
            for (int x = 0; x < SIZE; x += 2) {
                uint32_t c = plane_[y * SIZE + x];
                if (!upper && y > 455 && lum(c) < 0.2f * 255) continue;
                px.push_back(c);
            }
        if (px.empty()) {
            p.bands[band * 2] = band ? p.bands[band * 2 - 2] : 0xFF203860u;
            p.bands[band * 2 + 1] = band ? p.bands[band * 2 - 1] : 0xFFE0E0E8u;
            continue;
        }
        std::sort(px.begin(), px.end(), [&](uint32_t a, uint32_t b) { return lum(a) < lum(b); });
        p.bands[band * 2] = px[px.size() / 4] | 0xFF000000u;
        p.bands[band * 2 + 1] = px[px.size() * 9 / 10] | 0xFF000000u;
    }
    // Smooth from band to band, and clouds always lighter than the sky
    for (int pass = 0; pass < 2; pass++) {
        uint32_t old[BANDS * 2];
        memcpy(old, p.bands, sizeof(old));
        for (int band = 0; band < BANDS; band++)
            for (int which = 0; which < 2; which++) {
                int a = (std::max)(band - 1, 0), b = (std::min)(band + 1, BANDS - 1);
                uint32_t out = 0xFF000000u;
                for (int shift = 0; shift <= 16; shift += 8) {
                    uint32_t v = ((old[a * 2 + which] >> shift & 255) + 2 * (old[band * 2 + which] >> shift & 255) +
                                  (old[b * 2 + which] >> shift & 255) + 2) / 4;
                    out |= v << shift;
                }
                p.bands[band * 2 + which] = out;
            }
    }
    // The upper sky's bottom rows meet the top of the lower one: blend into it
    if (upper && last_lower_)
        for (int band = BANDS - 8; band < BANDS; band++) {
            float w = float(band - (BANDS - 9)) / 9;
            for (int which = 0; which < 2; which++) {
                uint32_t a = p.bands[band * 2 + which], b = last_lower_->bands[which], out = 0xFF000000u;
                for (int shift = 0; shift <= 16; shift += 8)
                    out |= uint32_t(float(a >> shift & 255) * (1 - w) + float(b >> shift & 255) * w + 0.5f) << shift;
                p.bands[band * 2 + which] = out;
            }
        }
    for (int band = 0; band < BANDS; band++) {
        uint32_t sky = p.bands[band * 2], lit = p.bands[band * 2 + 1], out = 0xFF000000u;
        for (int shift = 0; shift <= 16; shift += 8) {
            uint32_t least = (std::min)(uint32_t((sky >> shift & 255) * 1.3f + 20), 255u);
            out |= (std::max)(lit >> shift & 255, least) << shift;
        }
        p.bands[band * 2 + 1] = out;
    }
    RgbaImage img{slot, SIZE, SIZE, std::vector<uint8_t>(SIZE * SIZE * 4)};
    for (int y = 0; y < SIZE; y++)
        for (int x = 0; x < SIZE; x++) {
            uint32_t o = plane_[y * SIZE + x];
            float orig[3] = {float((o >> 16) & 255) / 255, float((o >> 8) & 255) / 255, float(o & 255) / 255};
            uint8_t* d = &img.rgba[(y * SIZE + x) * 4];
            d[0] = uint8_t(o >> 16);
            d[1] = uint8_t(o >> 8);
            d[2] = uint8_t(o);
            d[3] = 255;
            float c[3];
            sky_colour(x + 0.5f, y + 0.5f, upper, p.bands, 0, c);
            // Trees on the horizon: the dark parts of the original's bottom rows
            float tree = !upper && y > 455 ? 1 - sky_smooth(0.10f, 0.20f, lum(o) / 255) : 0;
            uint32_t rgb = 0;
            for (int k = 0; k < 3; k++) {
                float v = c[k] + (orig[k] * 0.9f - c[k]) * tree;
                rgb = rgb << 8 | uint32_t(std::clamp(v, 0.0f, 1.0f) * 255 + 0.5f);
            }
            p.low[y * SIZE + x] = 0xFF000000u | rgb;
        }
    if (!upper) {
        last_lower_ = &p;
        pending_.push_back(std::move(img));
    }   // the upper one has no trees to show
    printf("[sky] procedural sky for %016llx%s\n", (unsigned long long)hash, upper ? " (upper)" : "");
    return &p;
}

std::vector<RgbaImage> SkyPack::take_new_images() {
    std::vector<RgbaImage> out;
    out.swap(pending_);
    return out;
}
