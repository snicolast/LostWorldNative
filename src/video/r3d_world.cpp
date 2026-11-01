#include "r3d_world.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tuple>

#include "r3d_scene.h"

// Finds the viewport showing the game world and the camera position in a
// fixed world frame, for the atmosphere. The orientation shared by most
// of the drawn polygons is the world's; the camera position follows from
// how the scenery models moved relative to it since the previous frame.
void R3DWorldTracker::track(const std::vector<std::vector<R3DInstance>>& viewport_instances, R3DScene& out) {
    auto same_axes = [](const float* a, const float* b) {
        for (int i = 0; i < 9; i++)
            if (std::fabs(a[i] - b[i]) > 0.01f) return false;
        return true;
    };
    int best = -1;
    uint32_t best_polygons = 0;
    float axes[9];
    // The group that goes on with last frame's scenery models, if any
    int kept_vp = -1;
    uint32_t kept_models = 0, kept_polygons = 0;
    float kept_axes[9];
    for (size_t vi = 0; vi < viewport_instances.size() && vi < out.viewports.size(); vi++) {
        const auto& list = viewport_instances[vi];
        // Orientation groups weighted by polygons; the scenery is the largest
        struct Group {
            uint32_t polygons;
            size_t first;          // its first instance
            uint32_t was_scenery;  // instances of models that were scenery last frame
        };
        std::vector<Group> groups;
        for (size_t i = 0; i < list.size(); i++) {
            uint32_t was = tracking_ && scenery_models_.count(list[i].model) ? 1 : 0;
            bool found = false;
            for (auto& g : groups)
                if (same_axes(list[g.first].axes, list[i].axes)) {
                    g.polygons += list[i].polygons;
                    g.was_scenery += was;
                    found = true;
                    break;
                }
            if (!found) groups.push_back({list[i].polygons, i, was});
        }
        for (auto& g : groups) {
            if (g.polygons > best_polygons) {
                best_polygons = g.polygons;
                best = int(vi);
                memcpy(axes, list[g.first].axes, sizeof(axes));
            }
            if (g.was_scenery > kept_models) {
                kept_models = g.was_scenery;
                kept_polygons = g.polygons;
                kept_vp = int(vi);
                memcpy(kept_axes, list[g.first].axes, sizeof(axes));
            }
        }
    }
    // A big model close to the camera made of parts that share one
    // orientation (the Deinosuchus of stage 2 is two dozen) can outweigh
    // the scenery behind it, and the largest group would then go back and
    // forth between the two from frame to frame, flashing the haze, the
    // fade-in and the rim light. The scenery stays the world as long as most
    // of its models are still drawn with one orientation; a cut to another
    // place has other models, and the largest group wins again.
    if (kept_vp >= 0 && kept_models >= 8 && kept_models * 2 >= scenery_models_.size() &&
        kept_polygons * 4 >= best_polygons) {
        best = kept_vp;
        best_polygons = kept_polygons;
        memcpy(axes, kept_axes, sizeof(axes));
    }
    // Scenery models of this frame, by instance and by model (the n-th time
    // a model is drawn): the game sometimes rebuilds its display list in
    // another place, which changes every instance key at once
    Scenery& scenery = scenery_now_;
    scenery.by_key.clear();
    scenery.by_model.clear();
    scenery_models_.clear();
    if (best >= 0) {
        std::unordered_map<uint32_t, uint32_t> seen;
        for (const R3DInstance& in : viewport_instances[best])
            if (same_axes(in.axes, axes)) {
                scenery_models_.insert(in.model);
                Placement p;
                memcpy(p.origin, in.origin, sizeof(p.origin));
                memcpy(p.axes, in.axes, sizeof(p.axes));
                scenery.by_key[in.key] = p;
                scenery.by_model[r3d_mix_key(in.model, seen[in.model]++)] = p;
            }
    }
    // A world is many models placed around the camera; the title screens and
    // menus show one or two big models (logos) instead
    if (best < 0 || best_polygons < 300 || scenery.by_key.size() < 8) {
        tracking_ = false;
        scenery_prev_ = {};
        return;
    }
    // World position of a point given in view space, with the given axes and camera
    auto to_world = [](const float* ax, const float* cam, const float* p, float* w) {
        for (int k = 0; k < 3; k++) w[k] = cam[k] + ax[k * 3] * p[0] + ax[k * 3 + 1] * p[1] + ax[k * 3 + 2] * p[2];
    };
    // Every model seen in both frames gives an estimate of the camera: where
    // it was placed last frame minus where it is now relative to the camera,
    // each with its own axes (they differ slightly from model to model, which
    // is a whole unit of error on a model a hundred units away).
    // Most of them are fixed scenery and agree; the median ignores the ones
    // that move (animals, effects) or follow the camera (sky, the player's
    // vehicle), which would otherwise drag the fog along with the view.
    int agree = 0, common = 0;
    auto estimate = [&](const std::unordered_map<uint32_t, Placement>& now_map,
                        const std::unordered_map<uint32_t, Placement>& prev_map, float* cam) {
        std::vector<float> est[3];
        for (auto& [k, o] : now_map) {
            auto prev = prev_map.find(k);
            if (prev == prev_map.end()) continue;
            float was[3], now[3];
            to_world(prev->second.axes, camera_, prev->second.origin, was);
            to_world(o.axes, camera_, o.origin, now);
            for (int c = 0; c < 3; c++) est[c].push_back(camera_[c] + was[c] - now[c]);
        }
        int n = int(est[0].size()), ok = 0;
        if (n == 0) return std::make_pair(0, 0);
        for (int c = 0; c < 3; c++) {
            std::vector<float> e = est[c];
            std::nth_element(e.begin(), e.begin() + n / 2, e.end());
            cam[c] = e[n / 2];
        }
        for (int i = 0; i < n; i++)
            if (std::fabs(est[0][i] - cam[0]) + std::fabs(est[1][i] - cam[1]) + std::fabs(est[2][i] - cam[2]) < 0.5f)
                ok++;
        return std::make_pair(ok, n);
    };
    bool fresh = !tracking_;
    float camera_was[3];   // last frame's camera, for where the scenery stood then
    memcpy(camera_was, camera_, sizeof(camera_was));
    if (tracking_) {
        float cam[3], cam2[3];
        std::tie(agree, common) = estimate(scenery.by_key, scenery_prev_.by_key, cam);
        auto [agree2, common2] = estimate(scenery.by_model, scenery_prev_.by_model, cam2);
        if (agree2 > agree) {
            agree = agree2;
            common = common2;
            memcpy(cam, cam2, sizeof(cam));
        }
        if (agree >= 4 && agree * 3 >= common) {
            // The whole world jumping at once while the models still agree is
            // the game looping a stretch of scenery (the T-Rex chase runs
            // through the same forest every 2.4 s, 375 units back): the
            // atmosphere carries on from where the camera was heading, or
            // its fog banks would jump with the loop
            float jump = 0, speed = 0;
            for (int c = 0; c < 3; c++) {
                jump += std::fabs(cam[c] - camera_[c] - velocity_[c]);
                speed += std::fabs(velocity_[c]);
            }
            if (jump > 30.0f && jump > 8.0f * speed)
                for (int c = 0; c < 3; c++) loop_offset_[c] += camera_[c] + velocity_[c] - cam[c];
            else
                for (int c = 0; c < 3; c++) velocity_[c] = cam[c] - camera_[c];
            memcpy(camera_, cam, sizeof(cam));
            lost_ = 0;
        } else if (++lost_ <= 3) {
            // A frame that cannot be matched: the camera keeps its motion
            for (int c = 0; c < 3; c++) camera_[c] += velocity_[c];
        } else {
            fresh = true;   // a different scene: the camera stays where it was
            memset(velocity_, 0, sizeof(velocity_));
        }
    }
    // Scenery that appears far away while the camera moves on (the game adds
    // it as it comes within its draw distance) dissolves in from the backdrop
    // over half a second instead of popping in. A cut (most models somewhere
    // else) shows everything at once, as the game does.
    // A model counts as new only if the same model has not stood at the same
    // place in the world during the last two seconds: scenery the game culls
    // and redraws (the camera swaying in a fight, display list blocks that
    // change order) must not dissolve again and again.
    R3DViewport& view = out.viewports[best];
    size_t kept = 0;
    for (auto& [k, p] : scenery.by_key) kept += scenery_prev_.by_key.count(k);
    // Most models still where they were, by instance or by model: the game
    // also reorders its display list while the camera goes on (the plain
    // of stage 2 does it a couple of frames after adding a stretch), and
    // that must not cut short what is fading in
    bool continuous = !fresh && (kept * 2 >= scenery.by_key.size() || size_t(agree) * 2 >= scenery.by_key.size());
    const uint64_t frame = out.frame, memory = 120;
    if (fresh) seen_.clear();   // the camera position starts over: old places no longer match
    auto& seen = seen_now_;
    seen.clear();
    size_t appearing = 0;
    for (const R3DInstance& in : viewport_instances[best]) {
        if (!same_axes(in.axes, axes)) continue;
        Seen now;
        to_world(axes, camera_, in.origin, now.world);
        now.frame = frame;
        now.fade = 0;
        // The camera estimate wobbles by about 1% of the distance
        float d = std::sqrt(in.origin[0] * in.origin[0] + in.origin[1] * in.origin[1] + in.origin[2] * in.origin[2]);
        float tolerance = 2.0f + 0.025f * d;
        const Seen* match = nullptr;
        auto it = seen_.find(in.model);
        if (it != seen_.end())
            for (const Seen& s : it->second)
                if (frame - s.frame <= memory &&
                    std::fabs(s.world[0] - now.world[0]) + std::fabs(s.world[1] - now.world[1]) +
                            std::fabs(s.world[2] - now.world[2]) < tolerance &&
                    (!match || s.frame > match->frame))
                    match = &s;
        // Drawn last frame through the same path but somewhere else: a prop
        // that moves a little each frame is not new, while one that jumps
        // is a piece of scenery the game took from behind the camera and put
        // down again ahead (the plain of stage 2 recycles a fixed set of
        // bushes and rocks along the way), which is new
        bool prop = false;
        if (auto prev = scenery_prev_.by_key.find(in.key); !match && prev != scenery_prev_.by_key.end()) {
            float was[3];
            to_world(prev->second.axes, camera_was, prev->second.origin, was);
            prop = std::fabs(was[0] - now.world[0]) + std::fabs(was[1] - now.world[1]) + std::fabs(was[2] - now.world[2]) <
                    30.0f + tolerance;
        }
        if (prop) {
            // Not new
        } else if (match) {
            // Still fading in only if it was drawn without a break (a few game
            // frames the scene builder skipped are not one)
            if (continuous && frame - match->frame <= 15)
                now.fade = std::max(match->fade - float(frame - match->frame) / 30.0f, 0.0f);
        } else if (continuous) {
            if (d > 0.3f * far_) now.fade = 1;
        }
        seen[in.model].push_back(now);
        if (now.fade <= 0) continue;
        appearing++;
        uint32_t code = uint32_t(now.fade * 255.0f + 0.5f) << R3D_FADE_IN_SHIFT;
        for (uint32_t i = in.opaque[0]; i < in.opaque[1]; i++) out.vertices[view.opaque_first + i].flags |= code;
        for (uint32_t i = in.trans[0]; i < in.trans[1]; i++) out.vertices[view.trans_first + i].flags |= code;
    }
    // Keep the places not drawn this frame for a while
    for (auto& [model, list] : seen_)
        for (const Seen& s : list) {
            if (frame - s.frame > memory) continue;
            auto& now = seen[model];
            bool covered = false;
            for (const Seen& n : now)
                if (std::fabs(s.world[0] - n.world[0]) + std::fabs(s.world[1] - n.world[1]) + std::fabs(s.world[2] - n.world[2]) <
                    4.0f)
                    covered = true;
            if (!covered) now.push_back(s);
        }
    seen_.swap(seen);

    tracking_ = true;
    std::swap(scenery_prev_, scenery_now_);

    // Ground: median height of the upward-facing surfaces below the camera
    R3DViewport& v = out.viewports[best];
    const float* up = axes + 3;
    std::vector<float> heights;
    for (uint32_t i = v.opaque_first; i < v.opaque_first + v.opaque_count; i += 3) {
        const R3DVertex& p = out.vertices[i];
        float nl = std::sqrt(p.nrm[0] * p.nrm[0] + p.nrm[1] * p.nrm[1] + p.nrm[2] * p.nrm[2]);
        if (nl <= 0 || (p.nrm[0] * up[0] + p.nrm[1] * up[1] + p.nrm[2] * up[2]) < 0.85f * nl) continue;
        float h = p.pos[0] * up[0] + p.pos[1] * up[1] + p.pos[2] * up[2];
        if (h < 0) heights.push_back(h);
    }
    if (heights.size() >= 16) {
        std::nth_element(heights.begin(), heights.begin() + heights.size() / 2, heights.end());
        float g = camera_[1] + heights[heights.size() / 2];
        ground_ = fresh ? g : ground_ + 0.05f * (g - ground_);
    } else if (fresh) {
        ground_ = camera_[1] - 20.0f;
    }

    // Draw distance: nearly all the scenery is nearer than this. Only the
    // scenery counts: models that move on their own can stand far beyond it
    // (the wind turbines on the horizon of stage 2's plain, 34000 units
    // away, are a tenth of its polygons), which would push the haze and the
    // fade-in of appearing scenery out to where nothing is
    std::vector<float> dists;
    for (const R3DInstance& in : viewport_instances[best]) {
        if (!same_axes(in.axes, axes)) continue;
        for (uint32_t i = in.opaque[0]; i + 2 < in.opaque[1]; i += 3) {
            const float* p = out.vertices[v.opaque_first + i].pos;
            dists.push_back(std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]));
        }
    }
    if (!dists.empty()) {
        size_t k = dists.size() * 95 / 100;
        std::nth_element(dists.begin(), dists.begin() + k, dists.end());
        float f = std::max(dists[k], 50.0f);
        far_ = fresh || far_ <= 0 ? f : far_ + 0.05f * (f - far_);
    }

    v.world = true;
    // Axes in view space: column k of the world-to-view rotation is world axis k
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < 3; i++) v.world_axes[k * 3 + i] = axes[k * 3 + i];
    for (int c = 0; c < 3; c++) v.camera[c] = camera_[c] + loop_offset_[c];
    v.ground = ground_;
    v.scenery_far = far_;
    for (uint32_t i = v.opaque_first; i < v.opaque_first + v.opaque_count; i++) out.vertices[i].flags |= R3D_WORLD;
    for (uint32_t i = v.trans_first; i < v.trans_first + v.trans_count; i++) out.vertices[i].flags |= R3D_WORLD;
    // Models not aligned with the scenery move on their own (dinosaurs, people,
    // vehicles): the rim light outlines them, never the scenery
    for (const R3DInstance& in : viewport_instances[best]) {
        if (same_axes(in.axes, axes)) continue;
        for (uint32_t i = in.opaque[0]; i < in.opaque[1]; i++) out.vertices[v.opaque_first + i].flags |= R3D_ACTOR;
        for (uint32_t i = in.trans[0]; i < in.trans[1]; i++) out.vertices[v.trans_first + i].flags |= R3D_ACTOR;
    }
    static const bool debug = getenv("LW_FOGDBG") != nullptr;   // debugging aid
    if (debug)
        printf("[fog] frame %llu camera %.1f %.1f %.1f ground %.1f (height %.1f) far %.0f agree %d/%d models %zu "
               "polygons %u grounds %zu appearing %zu\n",
               (unsigned long long)out.frame, camera_[0], camera_[1], camera_[2], ground_, camera_[1] - ground_, far_,
               agree, common, scenery_prev_.by_key.size(), best_polygons, heights.size(), appearing);
}
