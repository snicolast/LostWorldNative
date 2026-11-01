#include "r3d_scene.h"

#include "r3d_screen.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

using std::uint32_t;

inline float as_float(uint32_t v) {
    float f;
    memcpy(&f, &v, 4);
    return f;
}

// Column-major 4x4 helpers
struct M4 {
    static void identity(float* m) {
        memset(m, 0, 64);
        m[0] = m[5] = m[10] = m[15] = 1;
    }
    static void mul(const float* a, const float* b, float* out) {
        float r[16];
        for (int c = 0; c < 4; c++)
            for (int rr = 0; rr < 4; rr++)
                r[c * 4 + rr] = a[0 * 4 + rr] * b[c * 4 + 0] + a[1 * 4 + rr] * b[c * 4 + 1] +
                                a[2 * 4 + rr] * b[c * 4 + 2] + a[3 * 4 + rr] * b[c * 4 + 3];
        memcpy(out, r, 64);
    }
    static void point(const float* m, const float* p, float* out) {
        for (int i = 0; i < 3; i++) out[i] = m[i] * p[0] + m[4 + i] * p[1] + m[8 + i] * p[2] + m[12 + i];
    }
    static void vec(const float* m, const float* v, float* out) {
        for (int i = 0; i < 3; i++) out[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2];
    }
};

// Real3D 16-bit radius float: 6-bit signed exponent, 10-bit mantissa (spec 3.7)
float float16(uint32_t u) {
    int e = int(u >> 10) & 63;
    if (e >= 32) e -= 64;
    return std::ldexp(1.0f + float(u & 0x3FF) / 1024.0f, e);
}

inline float fixed_normal(uint32_t w) {   // 2.22 fixed point in bits 8-31
    return float(int32_t(w) >> 8) / 4194304.0f;
}

inline float vertex_normal(uint32_t w) {   // signed byte in bits 0-7
    return (2.0f * float(int8_t(w & 0xFF)) + 1.0f) / 255.0f;
}

}  // namespace

R3DSceneBuilder::Mat R3DSceneBuilder::matrix(uint32_t index) {
    // 12 floats: translation, then rows 0..2 (spec 4.1)
    float f[12];
    for (int i = 0; i < 12; i++) f[i] = s_.cullf(matrix_base_ + index * 12 + i);
    Mat m{};
    m.m[0] = f[3]; m.m[4] = f[4]; m.m[8] = f[5];  m.m[12] = f[0];
    m.m[1] = f[6]; m.m[5] = f[7]; m.m[9] = f[8];  m.m[13] = f[1];
    m.m[2] = f[9]; m.m[6] = f[10]; m.m[10] = f[11]; m.m[14] = f[2];
    m.m[15] = 1;
    return m;
}

uint32_t R3DSceneBuilder::model_word(uint32_t addr) {
    addr &= 0xFFFFFF;
    if (addr < 0x100000) return s_.poly(addr);
    if (!vrom_words_) return 0;
    uint32_t v;
    memcpy(&v, vrom_ + size_t(addr % vrom_words_) * 4, 4);   // VROM mirrored over 64 MB
    return v;
}

void R3DSceneBuilder::build(const R3DFrameState& state, R3DScene& out) {
    s_.attach(&state);
    out_ = &out;
    keys_.clear();
    instance_count_.clear();
    node_key_ = 0;
    out.vertices.clear();
    out.viewports.clear();
    out.frame = state.frame;
    out.polygons = 0;
    visited_viewports_.clear();
    viewport_instances_.clear();
    viewport(0x800000, 0);
    world_.track(viewport_instances_, out);
    // Draw order: priority 0..3, list order within a priority
    std::stable_sort(out.viewports.begin(), out.viewports.end(),
                     [](const R3DViewport& a, const R3DViewport& b) { return a.priority < b.priority; });
    if (remove_letterbox_) r3d_strip_letterbox(out);
    if (hud_edges_) r3d_anchor_hud(out);
    if (pack_ && texture_ram_ && pack_->active()) apply_texture_pack(out);
    link_previous(out);
}

uint32_t R3DSceneBuilder::instance_key(uint32_t model_addr) {
    uint32_t k = r3d_mix_key(r3d_mix_key(node_key_, model_addr), viewport_key_);
    return r3d_mix_key(k, instance_count_[k]++);   // the same model drawn again through the same path
}

// Copies of one model (the ammo counter's cartridges) are told apart by the
// path and order they are drawn in, and the game reorders its list from frame
// to frame: a cartridge would then be interpolated towards its neighbour and
// bob up and down. Each copy takes the key of the nearest copy of the
// previous frame instead.
void R3DSceneBuilder::pair_copies() {
    auto& now = copies_now_;
    auto& before = copies_before_;
    auto& rekey = rekey_;
    // The lists keep their room; the paths come and go slowly (with the
    // stages), so the tables start over before they fill up with old ones
    if (now.size() > 4096) now.clear();
    if (before.size() > 4096) before.clear();
    for (auto& [path, list] : now) list.clear();
    for (auto& [path, list] : before) list.clear();
    rekey.clear();
    for (auto& list : viewport_instances_)
        for (R3DInstance& in : list) now[in.path].push_back(&in);
    for (const PrevInstance& p : prev_instances_) before[p.path].push_back(&p);
    for (auto& [path, copies] : now) {
        if (copies.empty()) continue;
        auto it = before.find(path);   // (an empty list: the path was not drawn last frame)
        if (it == before.end() || it->second.empty() || (copies.size() < 2 && it->second.size() < 2)) continue;
        if (copies.size() > 64 || it->second.size() > 64) continue;   // scenery repeated everywhere: as drawn
        const auto& old = it->second;
        // Closest pairs first. Moving copies (fence posts as the camera pans)
        // keep their order partner; a swapped list puts a copy right where
        // another one was.
        struct Pair { float d; uint32_t a, b; };
        std::vector<Pair> pairs;
        for (uint32_t a = 0; a < copies.size(); a++)
            for (uint32_t b = 0; b < old.size(); b++) {
                float d = 0;
                for (int c = 0; c < 3; c++) d += std::fabs(copies[a]->origin[c] - old[b]->origin[c]);
                // The partner found by list order wins unless another is clearly closer
                if (copies[a]->key == old[b]->key) d *= 0.5f;
                pairs.push_back({d, a, b});
            }
        std::sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.d < y.d; });
        std::vector<uint32_t> key(copies.size(), 0);
        std::vector<bool> paired(copies.size()), taken(old.size());
        std::unordered_set<uint32_t> used;
        for (const Pair& p : pairs)
            if (!paired[p.a] && !taken[p.b]) {
                paired[p.a] = taken[p.b] = true;
                key[p.a] = old[p.b]->key;
                used.insert(old[p.b]->key);
            }
        // Copies without a partner keep a key of the group nobody else has
        uint32_t n = 0;
        for (uint32_t a = 0; a < copies.size(); a++) {
            if (paired[a]) continue;
            uint32_t k;
            do k = r3d_mix_key(path, n++);
            while (used.count(k));
            used.insert(k);
            key[a] = k;
        }
        for (uint32_t a = 0; a < copies.size(); a++)
            if (key[a] != copies[a]->key) {
                rekey[copies[a]->key] = key[a];
                copies[a]->key = key[a];
            }
    }
    if (!rekey.empty())
        for (uint64_t& k : keys_) {
            auto it = rekey.find(uint32_t(k >> 32));
            if (it != rekey.end()) k = uint64_t(it->second) << 32 | (k & 0xFFFFFFFFull);
        }
    prev_instances_.clear();
    for (const auto& list : viewport_instances_)
        for (const R3DInstance& in : list)
            prev_instances_.push_back({in.key, in.path, {in.origin[0], in.origin[1], in.origin[2]}});
}

// Fills R3DVertex::prev from the previous frame. Vertices that are new, or
// jumped too far (a camera cut, a reused model), are not interpolated.
// Neither are models that turned a lot since the previous frame (wheels):
// blending their vertices in a straight line folds the polygons.
void R3DSceneBuilder::link_previous(R3DScene& out) {
    out.moving = 0;
    pair_copies();
    auto& axes = axes_;
    auto& spinning = spinning_;
    axes.clear();
    spinning.clear();
    for (const auto& list : viewport_instances_)
        for (const R3DInstance& in : list) {
            std::array<float, 9>& a = axes[in.key];
            memcpy(a.data(), in.axes, sizeof(in.axes));
            auto it = prev_axes_.find(in.key);
            if (it == prev_axes_.end()) continue;
            // Rotation angle between both frames: trace(prev^T now) = 1 + 2 cos(angle)
            float trace = 0;
            for (int i = 0; i < 9; i++) trace += a[i] * it->second[i];
            if (trace < 1.0f + 2.0f * 0.866f) spinning.insert(in.key);   // more than 30 degrees
        }
    prev_axes_.swap(axes);
    index_.reset(out.vertices.size());
    for (size_t i = 0; i < out.vertices.size(); i++) {
        R3DVertex& v = out.vertices[i];
        memcpy(v.prev, v.pos, sizeof(v.pos));
        index_.insert(keys_[i], uint32_t(i));
        uint32_t was = prev_index_.find(keys_[i]);
        if (was == KeyIndex::EMPTY || (!spinning.empty() && spinning.count(uint32_t(keys_[i] >> 32)))) continue;
        const float* p = prev_positions_[was].data();
        float dx = p[0] - v.pos[0], dy = p[1] - v.pos[1], dz = p[2] - v.pos[2];
        float limit = 0.25f * std::max(1.0f, std::fabs(v.pos[2]));
        if (dx * dx + dy * dy + dz * dz > limit * limit) continue;
        memcpy(v.prev, p, sizeof(v.prev));
        out.moving += dx != 0 || dy != 0 || dz != 0;
    }
    std::swap(prev_index_, index_);
    prev_positions_.resize(out.vertices.size());
    for (size_t i = 0; i < out.vertices.size(); i++) memcpy(prev_positions_[i].data(), out.vertices[i].pos, 12);
}

void R3DSceneBuilder::KeyIndex::reset(size_t count) {
    size_t size = 1024;
    while (size < 2 * count) size *= 2;
    if (vertex.size() != size) {
        keys.resize(size);
        vertex.assign(size, EMPTY);
        mask = uint32_t(size - 1);
    } else {
        std::fill(vertex.begin(), vertex.end(), EMPTY);
    }
}

void R3DSceneBuilder::KeyIndex::insert(uint64_t key, uint32_t v) {
    for (uint32_t at = uint32_t((key * 0x9E3779B97F4A7C15ull) >> 32) & mask;; at = (at + 1) & mask) {
        if (vertex[at] == EMPTY) {
            keys[at] = key;
            vertex[at] = v;
            return;
        }
        if (keys[at] == key) return;
    }
}

uint32_t R3DSceneBuilder::KeyIndex::find(uint64_t key) const {
    if (vertex.empty()) return EMPTY;
    for (uint32_t at = uint32_t((key * 0x9E3779B97F4A7C15ull) >> 32) & mask;; at = (at + 1) & mask) {
        if (vertex[at] == EMPTY) return EMPTY;
        if (keys[at] == key) return vertex[at];
    }
}

// Marks every textured triangle with its replacement slot (0 = none)
void R3DSceneBuilder::apply_texture_pack(R3DScene& out) {
    frame_slots_.clear();
    for (size_t i = 0; i + 2 < out.vertices.size(); i += 3) {
        R3DVertex* tri = &out.vertices[i];
        if (!(tri[0].flags & R3D_TEXTURED)) continue;
        auto it = frame_slots_.find(tri[0].tex);
        uint32_t slot = it != frame_slots_.end() ? it->second
                                                 : (frame_slots_[tri[0].tex] = pack_->lookup(tri[0].tex, *texture_ram_));
        tri[0].replace = tri[1].replace = tri[2].replace = slot;
    }
}

void R3DSceneBuilder::viewport(uint32_t addr, int depth) {
    if (depth > 64 || !s_.cull_valid(addr) || !visited_viewports_.insert(addr & 0xFFFFFF).second) return;
    auto vp = [&](uint32_t i) { return s_.cull(addr + i); };
    auto vpf = [&](uint32_t i) { return s_.cullf(addr + i); };
    uint32_t w0 = vp(0);
    if (vp(1) == 0) return;   // memory not set up yet

    if (!(w0 & 0x20)) {
        R3DViewport v;
        v.priority = (w0 >> 3) & 3;
        v.w = float(vp(0x14) & 0xFFFF) / 4.0f;
        v.h = float(vp(0x14) >> 16) / 4.0f;
        v.x = float(vp(0x1A) & 0xFFFF) / 16.0f;
        v.y = float(vp(0x1A) >> 16) / 16.0f;

        // Frustum from the viewport coefficients (spec 4.4); Vulkan clip space has Y down
        float cv = vpf(0x08), cw = vpf(0x09), io = vpf(0x0A), jo = vpf(0x0B);
        if (cv == 0 || cw == 0) { cv = 1; cw = 1; io = 0.5f; jo = 0.5f; }
        float l = -jo / cv, r = (1 - jo) / cv, b = -(1 - io) / cw, t = io / cw;
        const float near_z = 1e-3f;
        float* p = v.proj;
        memset(p, 0, 64);
        p[0] = 2 / (r - l);
        p[8] = (r + l) / (r - l);
        p[5] = -2 / (t - b);
        p[9] = -(t + b) / (t - b);
        p[14] = near_z;
        p[11] = -1;

        v.sun[0] = vpf(0x05);
        v.sun[1] = -vpf(0x06);
        v.sun[2] = -vpf(0x04);
        v.sun_intensity = std::clamp(vpf(0x07), 0.0f, 1.0f);
        v.ambient = float((vp(0x24) >> 8) & 0xFF) / 255.0f;
        uint32_t fc = vp(0x22);
        v.fog_color[0] = float((fc >> 16) & 0xFF) / 255.0f;
        v.fog_color[1] = float((fc >> 8) & 0xFF) / 255.0f;
        v.fog_color[2] = float(fc & 0xFF) / 255.0f;
        float density = std::fabs(vpf(0x23));
        v.fog_density = density < 99999.0f ? density : 0.0f;
        v.fog_start = float(int16_t(vp(0x25) & 0xFFFF)) / 255.0f;
        v.fog_ambient = float((vp(0x25) >> 16) & 0xFF) / 255.0f;
        static const bool debug_no_fog = getenv("LW_NOFOG") != nullptr;   // debugging aid
        if (debug_no_fog) v.fog_density = v.fog_start = 0;
        v.scroll_fog = float(vp(0x20) & 0xFF) / 255.0f;
        v.scroll_attenuation = float(vp(0x24) & 0xFF) / 255.0f;
        v.fog_attenuation = float((vp(0x24) >> 16) & 0xFF) / 255.0f;
        uint32_t spot_fog = (vp(0x20) >> 8) & 7;   // colour index: bit 2 = R, bit 1 = G, bit 0 = B
        for (int i = 0; i < 3; i++) v.spot_fog_color[i] = float((spot_fog >> (2 - i)) & 1);
        uint32_t spot = (vp(0x20) >> 11) & 7;
        for (int i = 0; i < 3; i++) v.spot_color[i] = float((spot >> (2 - i)) & 1);
        {
            // Centre in screen pixels from the top left, radius 2047 / size pixels per axis
            float cx = float(int16_t(vp(0x1E) & 0xFFFF)) / 8.0f, cy = float(int16_t(vp(0x1D) & 0xFFFF)) / 8.0f;
            float rx = std::round(2047.0f / float(std::max(1u, vp(0x1E) >> 16)));
            float ry = std::round(2047.0f / float(std::max(1u, vp(0x1D) >> 16)));
            // Pixel -> slope through the projection: ndc = p0 * x/-z - p8, ndc_y = p5 * y/-z - p9
            v.spot_center[0] = ((cx - v.x) / v.w * 2.0f - 1.0f + p[8]) / p[0];
            v.spot_center[1] = ((cy - v.y) / v.h * 2.0f - 1.0f + p[9]) / p[5];
            v.spot_radius[0] = rx * 2.0f / (v.w * std::fabs(p[0]));
            v.spot_radius[1] = ry * 2.0f / (v.h * std::fabs(p[5]));
            float start = vpf(0x21);
            v.spot_start = start != 0 && std::isfinite(1.0f / start) ? 1.0f / start : 0.0f;
            float extent = std::fabs(vpf(0x1F));
            v.spot_extent = std::isfinite(extent) ? extent : 0.0f;
        }

        matrix_base_ = vp(0x16) & 0xFFFFFF;
        lod_base_ = vp(0x17) & 0xFFFFFF;

        // Coordinate system: GL view = C * matrix 0 (spec 4.2)
        Mat c{};
        c.m[1] = 0;
        float cm[16] = {0, 0, -1, 0,   // column 0: R.x -> GL -z
                        1, 0, 0, 0,    // column 1: R.y -> GL x
                        0, -1, 0, 0,   // column 2: R.z -> GL -y
                        0, 0, 0, 1};
        memcpy(c.m, cm, 64);
        Mat m0 = matrix(0);
        Mat base{};
        M4::mul(c.m, m0.m, base.m);
        coord_sys_ = base;
        coord_sys_.m[12] = coord_sys_.m[13] = coord_sys_.m[14] = 0;

        opaque_.clear();
        trans_.clear();
        opaque_keys_.clear();
        trans_keys_.clear();
        viewport_key_ = addr;
        instances_.clear();
        node_ptr(vp(0x02), base, Attribs{}, 0);
        viewport_instances_.push_back(std::move(instances_));
        instances_.clear();

        auto& verts = out_->vertices;
        v.opaque_first = uint32_t(verts.size());
        v.opaque_count = uint32_t(opaque_.size());
        verts.insert(verts.end(), opaque_.begin(), opaque_.end());
        keys_.insert(keys_.end(), opaque_keys_.begin(), opaque_keys_.end());
        v.trans_first = uint32_t(verts.size());
        v.trans_count = uint32_t(trans_.size());
        verts.insert(verts.end(), trans_.begin(), trans_.end());
        keys_.insert(keys_.end(), trans_keys_.begin(), trans_keys_.end());
        out_->viewports.push_back(v);
    }

    uint32_t next = vp(1);
    if (next != 0x01000000 && (next & 0xFFFFFF)) viewport(next & 0xFFFFFF, depth + 1);
}

void R3DSceneBuilder::node_ptr(uint32_t ptr, const Mat& m, const Attribs& a, int depth) {
    uint32_t addr = ptr & 0xFFFFFF;
    if (!addr) return;
    switch ((ptr >> 24) & 5) {
    case 0: node(addr, m, a, depth); break;
    case 1: model(addr, m, a); break;
    case 4: pointer_list(addr, m, a, depth); break;
    default: break;
    }
}

void R3DSceneBuilder::pointer_list(uint32_t addr, const Mat& m, const Attribs& a, int depth) {
    for (uint32_t i = 0; i < 0x10000; i++) {
        uint32_t e = s_.cull(addr + i);
        if (!(e & 0x01000000)) node(e & 0xFFFFFF, m, a, depth + 1);
        if (e & 0x02000000) break;
    }
}

void R3DSceneBuilder::node(uint32_t addr, const Mat& parent, Attribs a, int depth) {
    if (depth > 1024 || !s_.cull_valid(addr)) return;
    uint32_t w[10];
    for (int i = 0; i < 10; i++) w[i] = s_.cull(addr + i);
    if ((w[0] & 3) == 0) return;               // viewport reached as a node
    if ((w[0] & 0x300) == 0x300) return;       // discarded with its children

    // Siblings first (spec 3.2)
    uint32_t sib = w[8] & 0x1FFFFFF;
    bool colour_table_node = (w[0] & 7) == 6, point_ref = w[0] & 0x10000000;
    if ((!colour_table_node || point_ref) && !(sib & 0x1000000) && sib) node(sib, parent, a, depth + 1);

    // Interpolation key of everything below this node
    struct KeyScope {
        uint32_t& key;
        uint32_t saved;
        ~KeyScope() { key = saved; }
    } key_scope{node_key_, node_key_};
    node_key_ = r3d_mix_key(node_key_, addr);

    if (w[0] & 0x04)
        color_table_ = ((w[3] >> 19) | ((w[7] >> 28) << 13) | ((w[8] >> 25) << 17)) & 0x0FFFFF;

    if (w[1] & 1) a.model_scale = as_float(w[1] & ~3u);
    if (w[1] & 2) a.no_cull = true;
    if (w[2] & 0x8000) {
        a.tex_x = 32 * ((w[2] >> 7) & 0x3F);
        a.tex_y = 32 * (w[2] & 0x1F);
        a.page = (w[2] >> 14) & 1;
    }

    Mat m = parent;
    if (w[0] & 0x10) {
        Mat t{};
        M4::identity(t.m);
        t.m[12] = as_float(w[4]);
        t.m[13] = as_float(w[5]);
        t.m[14] = as_float(w[6]);
        M4::mul(parent.m, t.m, m.m);
    } else if (w[3] & 0xFFF) {
        Mat n = matrix(w[3] & 0xFFF);
        M4::mul(parent.m, n.m, m.m);
    }
    if (w[0] & 0x80) {   // reset matrix: keep translation and scale, rotation = coordinate system
        float len[3];
        for (int c = 0; c < 3; c++)
            len[c] = std::sqrt(m.m[c * 4] * m.m[c * 4] + m.m[c * 4 + 1] * m.m[c * 4 + 1] + m.m[c * 4 + 2] * m.m[c * 4 + 2]);
        Mat r = coord_sys_;
        for (int c = 0; c < 3; c++)
            for (int i = 0; i < 3; i++) r.m[c * 4 + i] *= len[c];
        r.m[12] = m.m[12];
        r.m[13] = m.m[13];
        r.m[14] = m.m[14];
        m = r;
    }

    // LOD selection from the blend radius and the distance to the camera
    uint32_t lod_index = (w[3] >> 12) & 0x7F;
    bool no_lod = lod_index == 0x7F;
    float dist = std::sqrt(m.m[12] * m.m[12] + m.m[13] * m.m[13] + m.m[14] * m.m[14]);
    float blend_r = float16(w[9] >> 16) * a.model_scale;
    float lod_scale = (a.no_cull || no_lod || dist == 0) ? 1e30f : std::max(0.0f, blend_r / dist);
    float del[4], fac[4];
    for (int k = 0; k < 4; k++) {
        del[k] = s_.cullf(lod_base_ + lod_index * 8 + k * 2);
        fac[k] = s_.cullf(lod_base_ + lod_index * 8 + k * 2 + 1);
    }
    static const bool debug_no_lod = getenv("LW_NOLOD") != nullptr;   // debugging aid
    if (!debug_no_lod && !a.no_cull && !no_lod && lod_scale < del[3]) return;   // too far away

    uint32_t child = w[7] & 0x07FFFFFF;
    if (w[0] & 0x08) {   // 4-entry LOD table
        uint32_t table = child & 0xFFFFFF;
        uint32_t entry[4];
        for (int k = 0; k < 4; k++) entry[k] = s_.cull(table + k);
        int k = 0;
        for (; k < 3; k++)
            if (lod_scale >= del[k] && (entry[k] & 0x01000000)) break;
        float fade = std::clamp(fac[k] * (lod_scale - del[k]), 0.0f, 1.0f);
        if (no_lod || a.no_cull || fade > 31.0f / 32.0f) fade = 1;
        else if (fade < 1.0f / 32.0f) fade = 0;
        float parent_alpha = a.alpha;
        bool models = child & 0x01000000;
        auto visit = [&](uint32_t e, float alpha) {
            Attribs b = a;
            b.alpha = alpha;
            if (models) model(e & 0xFFFFFF, m, b);
            else node(e & 0xFFFFFF, m, b, depth + 1);
        };
        if (fade > 0) visit(entry[k], parent_alpha * fade);
        if (fade < 1 && k != 3) visit(entry[k + 1], parent_alpha * (1 - fade));
    } else {
        if (!no_lod && !a.no_cull) a.alpha *= std::clamp(fac[3] * (lod_scale - del[3]), 0.0f, 1.0f);
        node_ptr(child, m, a, depth + 1);
    }
}

void R3DSceneBuilder::model(uint32_t addr, const Mat& m, const Attribs& a) {
    if (a.alpha <= 0) return;
    // Rotation part used for normals; normals are divided by the model scale
    float inv_scale = a.model_scale != 0 ? 1.0f / a.model_scale : 1.0f;
    uint32_t end = (addr & 0xFFFFFF) < 0x100000 ? 0x100000 : 0x1000000;
    int tex_w = 32, tex_h = 32;
    float uv_scale = 1.0f / 8;
    uint32_t path = r3d_mix_key(addr, viewport_key_);
    uint64_t instance = uint64_t(instance_key(addr)) << 32;
    uint32_t drawn = 0;
    uint32_t opaque_from = uint32_t(opaque_.size()), trans_from = uint32_t(trans_.size());

    for (int guard = 0; guard < 65536; guard++) {
        uint32_t h[7];
        for (int i = 0; i < 7; i++) h[i] = model_word(addr + i);
        if (h[6] == 0) break;
        int nverts = (h[0] & 0x40) ? 4 : 3;
        int shared = __popcnt(h[0] & 0xF);
        if (addr + 7 + 4 * uint32_t(std::max(0, nverts - shared)) > end) break;

        // Texture state of this polygon (affects reused vertices' UVs as well)
        int wcode = (h[3] >> 3) & 7, hcode = h[3] & 7;
        if (wcode > 5) wcode = 0;
        if (hcode > 5) hcode = 0;
        tex_w = 32 << wcode;
        tex_h = 32 << hcode;
        uv_scale = (h[1] & 0x40) ? 1.0f : 1.0f / 8;

        bool smooth = h[1] & 0x08;
        bool fixed_shading = (h[1] & 0x20) && !smooth;
        bool specular = h[0] & 0x80;
        float face[3] = {fixed_normal(h[1]), fixed_normal(h[2]), fixed_normal(h[3])};

        PrevVertex v[4];
        int j = 0;
        for (int i = 0; i < 4; i++)
            if ((h[0] >> i) & 1) v[j++] = prev_[i];
        uint32_t p = addr + 7;
        for (; j < nverts; j++) {
            uint32_t x = model_word(p), y = model_word(p + 1), z = model_word(p + 2), t = model_word(p + 3);
            p += 4;
            v[j].pos[0] = float(int32_t(x) >> 8) / 2048.0f;
            v[j].pos[1] = float(int32_t(y) >> 8) / 2048.0f;
            v[j].pos[2] = float(int32_t(z) >> 8) / 2048.0f;
            v[j].nrm[0] = vertex_normal(x);
            v[j].nrm[1] = vertex_normal(y);
            v[j].nrm[2] = vertex_normal(z);
            v[j].shade = specular ? float(x & 0xFF) / 255.0f : vertex_normal(x);
            v[j].u = uint16_t(t >> 16);
            v[j].v = uint16_t(t & 0xFFFF);
        }
        if (!smooth)
            for (int i = 0; i < nverts; i++) memcpy(v[i].nrm, face, 12);
        for (int i = 0; i < 4; i++) prev_[i] = v[i];
        addr = p;

        bool last = h[1] & 0x04;
        bool discard = (h[0] & 0x300) == 0x300;
        if (!discard) {
            out_->polygons++;
            drawn++;
            // Colour and alpha
            uint32_t r, g, b;
            if (h[1] & 0x02) {
                r = h[4] >> 24;
                g = (h[4] >> 16) & 0xFF;
                b = (h[4] >> 8) & 0xFF;
            } else {
                uint32_t c = s_.poly(color_table_ + ((h[4] >> 8) & 0xFFF));
                r = (c >> 16) & 0xFF;
                g = (c >> 8) & 0xFF;
                b = c & 0xFF;
            }
            bool poly_alpha = !(h[6] & 0x800000);
            float alpha = poly_alpha ? float((h[6] >> 18) & 0x3F) / 32.0f : 1.0f;
            if (h[0] & 0x200) alpha *= 0.5f;
            alpha = std::min(1.0f, alpha * a.alpha);
            if (alpha > 0) {
                uint32_t color = r | g << 8 | b << 16 | uint32_t(alpha * 255.0f + 0.5f) << 24;

                bool textured = h[6] & 0x400;
                uint32_t fmt = (h[6] >> 7) & 7;
                bool contour = h[6] & 0x80000000;
                if (contour && fmt >= 1 && fmt <= 4) fmt += 7;
                int tx = 32 * (int(((h[4] & 0x1F) << 1) | ((h[5] >> 7) & 1))) + a.tex_x;
                int ty = 32 * int(h[5] & 0x1F) + a.tex_y;
                int page = int((h[4] >> 6) & 1) ^ a.page;
                uint32_t tex = uint32_t(tx & 2047) | uint32_t((ty + 1024 * page) & 2047) << 11 |
                               uint32_t(wcode) << 22 | uint32_t(hcode) << 25 | fmt << 28;

                bool tex_alpha = (h[6] & 7) != 0;
                uint32_t flags = 0;
                if (textured) flags |= R3D_TEXTURED;
                if (contour) flags |= R3D_ALPHA_TEST;
                if (h[2] & 2) flags |= R3D_MIRROR_U;
                if (h[2] & 1) flags |= R3D_MIRROR_V;
                if (h[3] & 0x80) flags |= R3D_SMOOTH_U;
                if (h[3] & 0x40) flags |= R3D_SMOOTH_V;
                if (h[6] & 0x10000) flags |= R3D_LUMINOUS;
                if (fixed_shading) flags |= R3D_FIXED_SHADE;
                if (specular) flags |= R3D_SPECULAR;
                if (tex_alpha) flags |= R3D_TEX_ALPHA;
                flags |= ((h[6] >> 5) & 3) << R3D_SHININESS_SHIFT;
                if (((h[6] >> 24) & 0x7F) == 2) flags |= R3D_INVERT_RGB;
                if (h[4] & 0x80) flags |= R3D_TRANSLATOR;
                float fog = float((h[6] >> 11) & 0x1F) / 16.0f;
                float spec_coef = float(h[0] >> 26) / 63.0f;

                // To view space
                R3DVertex out[4];
                for (int i = 0; i < nverts; i++) {
                    M4::point(m.m, v[i].pos, out[i].pos);
                    float n[3];
                    M4::vec(m.m, v[i].nrm, n);
                    for (int k = 0; k < 3; k++) out[i].nrm[k] = n[k] * inv_scale;
                    out[i].uv[0] = float(v[i].u) * uv_scale / float(tex_w);
                    out[i].uv[1] = float(v[i].v) * uv_scale / float(tex_h);
                    out[i].color = color;
                    out[i].tex = tex;
                    out[i].flags = flags;
                    out[i].shade = v[i].shade;
                    out[i].fog = fog;
                    out[i].spec = spec_coef;
                    out[i].replace = 0;
                }
                // Back-face test with the face normal (spec 5.4)
                float fn[3];
                M4::vec(m.m, face, fn);
                float facing = out[0].pos[0] * fn[0] + out[0].pos[1] * fn[1] + out[0].pos[2] * fn[2];
                bool double_sided = h[1] & 0x10;
                // A solid polygon with texture alpha goes to both passes: its
                // solid texels are drawn opaque (and occlude by depth), the
                // others translucent (spec 7.2)
                bool translucent = poly_alpha || a.alpha < 1.0f;
                uint64_t poly_key = instance | (uint64_t(guard) << 3);
                auto emit_to = [&](std::vector<R3DVertex>& list, std::vector<uint64_t>& keys, const R3DVertex* q,
                                   uint64_t key) {
                    list.push_back(q[0]);
                    list.push_back(q[1]);
                    list.push_back(q[2]);
                    if (nverts == 4) {
                        list.push_back(q[0]);
                        list.push_back(q[2]);
                        list.push_back(q[3]);
                    }
                    for (int c = 0; c < (nverts == 4 ? 6 : 3); c++) keys.push_back(key | uint64_t(c));
                };
                auto emit = [&](R3DVertex* q) {
                    if (!translucent) emit_to(opaque_, opaque_keys_, q, poly_key);
                    if (translucent || tex_alpha) emit_to(trans_, trans_keys_, q, poly_key | (1ull << 31));
                };
                if (facing <= 0) emit(out);
                else if (double_sided) {
                    for (int i = 0; i < nverts; i++)
                        for (int k = 0; k < 3; k++) out[i].nrm[k] = -out[i].nrm[k];
                    emit(out);
                }
            }
        }
        if (last) break;
    }
    if (drawn) {   // world tracking: where this model is and how it is turned
        R3DInstance in{uint32_t(instance >> 32), addr, {}, {m.m[12], m.m[13], m.m[14]}, drawn,
                    {opaque_from, uint32_t(opaque_.size())}, {trans_from, uint32_t(trans_.size())}};
        for (int c = 0; c < 3; c++) {
            const float* col = m.m + c * 4;
            float len = std::sqrt(col[0] * col[0] + col[1] * col[1] + col[2] * col[2]);
            for (int i = 0; i < 3; i++) in.axes[c * 3 + i] = len > 0 ? col[i] / len : 0;
        }
        in.path = path;
        instances_.push_back(in);
    }
}
