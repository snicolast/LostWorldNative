// Ray tracing (not in the original): the opaque game world of each displayed
// image goes into acceleration structures the shaders trace rays through
// (VK_KHR_ray_query): the sun shadows in the 3D pass (r3d.frag) and the
// ambient occlusion (ao.frag).
//
// The scene arrives as a triangle soup already in view space, rebuilt every
// game frame, so the world is built from scratch for each new game frame (a
// few thousand triangles: a fraction of a millisecond). The images between
// two game frames only move the same triangles (interpolation): the bottom
// levels are then refitted (updated in place), and an image that repeats the
// last one (half rate, no interpolation) builds nothing. Two bottom levels, two
// instances: what stands near the ground (instance mask RT_LOW, the only
// shadow casters, as with the shadow map, and what the ambient occlusion
// sees) and what is higher up (RT_HIGH: canopies and roofs, built only on
// request; nothing traces them now). Each has two
// geometries: solid triangles, which stop a ray at once, and cut-out ones
// (alpha-tested foliage, texture alpha), whose texture the shader reads to
// decide whether a ray passes through.
#pragma once
#include <cstdint>
#include <vector>

#include "r3d_scene.h"
#include "vk_presenter.h"

class R3DRayTracer {
public:
    // Per cut-out triangle, read by the shaders (struct Cutout in raytrace.glsl);
    // an instance's custom index is where its cut-outs start
    struct Cutout {
        float uv[6];        // the three corners
        uint32_t tex;       // R3DVertex::tex
        uint32_t flags;     // R3DVertex::flags
        uint32_t replace;   // texture pack slot
        float alpha;        // vertex alpha
    };
    static_assert(sizeof(Cutout) == 40, "layout is shared with raytrace.glsl");
    enum : uint32_t { RT_LOW = 1, RT_HIGH = 2 };   // instance masks (raytrace.glsl)

    // false: the device has no ray queries (VkPresenter::ray_query)
    bool init(VkPresenter& vk);
    void shutdown();
    bool ready() const { return tlas_ != VK_NULL_HANDLE; }

    // Collects the triangles of the world viewport for this image: positions
    // between the previous and the current game frame (interp). What stands up
    // to max_height above the ground (height of a view-space point:
    // dot(p, up) + camera_height) goes into the low level; with `high`, the
    // rest goes into the high one, otherwise it is left out. serial: the game
    // frame's (it changes with each new scene); the same serial and arguments
    // as the last call leave the structures as they are
    void prepare(const R3DScene& scene, uint64_t serial, const R3DViewport& world, float interp, const float up[3],
                 float camera_height, float max_height, bool high);
    // Builds (or refits) the structures; afterwards fragment shaders can trace
    // through them. Records nothing when prepare() found nothing new
    void record(VkCommandBuffer cmd);

    VkAccelerationStructureKHR tlas() const { return tlas_; }
    VkBuffer cutouts() const { return cutouts_.buffer; }
    // Changes when the cut-out buffer was replaced (descriptors to update)
    uint32_t generation() const { return generation_; }
    size_t triangles() const {
        size_t n = 0;
        for (const Level& l : levels_) n += l.solid + l.cut;
        return n;
    }

private:
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
        VkDeviceAddress address = 0;
    };
    struct Level {
        Buffer storage;
        VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
        VkDeviceAddress address = 0;
        VkDeviceSize scratch_offset = 0;
        uint32_t solid = 0, cut = 0;   // triangles this image
        uint32_t built_solid = 0, built_cut = 0;   // in the structure as last built (a refit needs the same)
        uint32_t first = 0;            // where its positions start (in triangles)
        std::vector<float> solid_pos, cut_pos;
        std::vector<Cutout> cut_data;
    };
    bool create_buffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, bool host);
    void destroy_buffer(Buffer& b);
    bool reserve(uint32_t triangles);   // capacity of each bottom level and of the inputs
    void build_info(VkAccelerationStructureBuildGeometryInfoKHR& info, VkAccelerationStructureGeometryKHR geo[2],
                    const Level& level) const;

    VkPresenter* vk_ = nullptr;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkDeviceSize scratch_align_ = 256;
    uint32_t capacity_ = 0;   // triangles
    Buffer positions_;        // 3 corners per triangle: per level, solid ones then the cut-outs
    Buffer cutouts_;          // Cutout per cut-out triangle, low level first
    Buffer tlas_buffer_, scratch_, instances_;
    VkDeviceSize tlas_scratch_offset_ = 0;
    Level levels_[2];
    VkAccelerationStructureKHR tlas_ = VK_NULL_HANDLE;
    uint32_t generation_ = 0;
    // What the structures hold: the arguments of the last prepare() that
    // changed them, and what the next record() has to do
    struct Key {
        uint64_t serial = ~0ull;
        const R3DViewport* world = nullptr;
        float interp = 0, up[3] = {}, camera_height = 0, max_height = 0;
        bool high = false;
        bool operator==(const Key& o) const {
            return serial == o.serial && world == o.world && interp == o.interp && up[0] == o.up[0] &&
                   up[1] == o.up[1] && up[2] == o.up[2] && camera_height == o.camera_height &&
                   max_height == o.max_height && high == o.high;
        }
    } key_;
    bool dirty_ = true;     // record() builds or refits
    bool rebuild_ = true;   // a full build (new scene, other triangle counts, new buffers)
    uint32_t built_generation_ = ~0u;
};
