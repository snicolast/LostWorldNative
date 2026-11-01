#include "r3d_raytrace.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

bool R3DRayTracer::create_buffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (vkCreateBuffer(dev_, &bi, nullptr, &b.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, b.buffer, &req);
    VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &fi;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = vk_->find_memory(req.memoryTypeBits, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                                   : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev_, &ai, nullptr, &b.memory) != VK_SUCCESS) return false;
    vkBindBufferMemory(dev_, b.buffer, b.memory, 0);
    if (host) vkMapMemory(dev_, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped);
    VkBufferDeviceAddressInfo addr{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addr.buffer = b.buffer;
    b.address = vkGetBufferDeviceAddress(dev_, &addr);
    b.size = size;
    return true;
}

void R3DRayTracer::destroy_buffer(Buffer& b) {
    if (b.buffer) vkDestroyBuffer(dev_, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(dev_, b.memory, nullptr);
    b = Buffer{};
}

// A bottom level: its solid triangles, then its cut-outs right after them in
// the position buffer
void R3DRayTracer::build_info(VkAccelerationStructureBuildGeometryInfoKHR& info,
                              VkAccelerationStructureGeometryKHR geo[2], const Level& level) const {
    for (int i = 0; i < 2; i++) {
        geo[i] = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geo[i].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        auto& t = geo[i].geometry.triangles;
        t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        t.vertexStride = 12;
        t.indexType = VK_INDEX_TYPE_NONE_KHR;
        uint32_t count = i == 0 ? level.solid : level.cut;
        t.maxVertex = std::max(count * 3, 1u) - 1;
        t.vertexData.deviceAddress = positions_.address + VkDeviceSize(level.first + (i == 0 ? 0 : level.solid)) * 36;
    }
    geo[0].flags = VK_GEOMETRY_OPAQUE_BIT_KHR;   // a hit is a hit
    geo[1].flags = 0;                            // the shader looks at the texture first
    info = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    // Built for each new game frame and refitted for the images between
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR |
                 VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 2;
    info.pGeometries = geo;
}

bool R3DRayTracer::reserve(uint32_t triangles) {
    if (triangles <= capacity_ && levels_[0].blas) return true;
    uint32_t cap = std::max({triangles + triangles / 2, capacity_, 8192u});
    vkDeviceWaitIdle(dev_);
    for (Level& l : levels_) {
        if (l.blas) vkDestroyAccelerationStructureKHR(dev_, l.blas, nullptr);
        l.blas = VK_NULL_HANDLE;
        destroy_buffer(l.storage);
    }
    for (Buffer* b : {&positions_, &cutouts_, &scratch_}) destroy_buffer(*b);
    capacity_ = 0;
    if (!create_buffer(positions_, VkDeviceSize(cap) * 36,
                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true) ||
        !create_buffer(cutouts_, VkDeviceSize(cap) * sizeof(Cutout), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true))
        return false;
    // Each level sized for all of them (either may hold most of the world)
    Level sizing;
    sizing.solid = sizing.cut = cap;
    VkAccelerationStructureBuildGeometryInfoKHR info;
    VkAccelerationStructureGeometryKHR geo[2];
    build_info(info, geo, sizing);
    uint32_t counts[2] = {cap, cap};
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(dev_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, counts, &sizes);
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    ci.size = sizes.accelerationStructureSize;
    auto align = [&](VkDeviceSize v) { return (v + scratch_align_ - 1) / scratch_align_ * scratch_align_; };
    VkDeviceSize scratch = 0;
    for (Level& l : levels_) {
        if (!create_buffer(l.storage, sizes.accelerationStructureSize,
                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false))
            return false;
        ci.buffer = l.storage.buffer;
        if (vkCreateAccelerationStructureKHR(dev_, &ci, nullptr, &l.blas) != VK_SUCCESS) return false;
        VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        ai.accelerationStructure = l.blas;
        l.address = vkGetAccelerationStructureDeviceAddressKHR(dev_, &ai);
        // Their own parts of the scratch buffer: they are built side by side
        l.scratch_offset = scratch;
        scratch = align(scratch + std::max(sizes.buildScratchSize, sizes.updateScratchSize));
    }

    // The top level: its part of the scratch buffer after the bottom levels'
    VkAccelerationStructureGeometryKHR tgeo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tgeo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tgeo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    VkAccelerationStructureBuildGeometryInfoKHR tinfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tinfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tinfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    tinfo.geometryCount = 1;
    tinfo.pGeometries = &tgeo;
    uint32_t two = 2;
    VkAccelerationStructureBuildSizesInfoKHR tsizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(dev_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tinfo, &two, &tsizes);
    tlas_scratch_offset_ = scratch;
    if (!create_buffer(scratch_, scratch + tsizes.buildScratchSize + scratch_align_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       false))
        return false;
    if (!tlas_) {
        if (!create_buffer(tlas_buffer_, tsizes.accelerationStructureSize,
                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false))
            return false;
        ci.buffer = tlas_buffer_.buffer;
        ci.size = tsizes.accelerationStructureSize;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        if (vkCreateAccelerationStructureKHR(dev_, &ci, nullptr, &tlas_) != VK_SUCCESS) return false;
    }
    capacity_ = cap;
    generation_++;
    return true;
}

bool R3DRayTracer::init(VkPresenter& vk) {
    if (!vk.ray_query()) return false;
    vk_ = &vk;
    dev_ = vk.device();
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asp{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props.pNext = &asp;
    vkGetPhysicalDeviceProperties2(vk.gpu(), &props);
    scratch_align_ = std::max<VkDeviceSize>(asp.minAccelerationStructureScratchOffsetAlignment, 1);
    if (!create_buffer(instances_, 2 * sizeof(VkAccelerationStructureInstanceKHR),
                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true) ||
        !reserve(0)) {
        fprintf(stderr, "[raytrace] could not create the acceleration structures\n");
        shutdown();
        return false;
    }
    // Built once empty, so the shaders always see a valid structure
    VkAccelerationStructureInstanceKHR inst[2] = {};
    for (int i = 0; i < 2; i++) {
        inst[i].mask = i == 0 ? RT_LOW : RT_HIGH;
        inst[i].accelerationStructureReference = levels_[i].address;
    }
    memcpy(instances_.mapped, inst, sizeof(inst));
    dirty_ = rebuild_ = true;
    vk.run_once([&](VkCommandBuffer cmd) { record(cmd); });
    return true;
}

void R3DRayTracer::shutdown() {
    if (!dev_) return;
    vkDeviceWaitIdle(dev_);
    for (Level& l : levels_) {
        if (l.blas) vkDestroyAccelerationStructureKHR(dev_, l.blas, nullptr);
        l.blas = VK_NULL_HANDLE;
        destroy_buffer(l.storage);
    }
    if (tlas_) vkDestroyAccelerationStructureKHR(dev_, tlas_, nullptr);
    tlas_ = VK_NULL_HANDLE;
    for (Buffer* b : {&positions_, &cutouts_, &tlas_buffer_, &scratch_, &instances_}) destroy_buffer(*b);
    capacity_ = 0;
    dev_ = VK_NULL_HANDLE;
}

void R3DRayTracer::prepare(const R3DScene& scene, uint64_t serial, const R3DViewport& world, float interp,
                           const float up[3], float camera_height, float max_height, bool high) {
    Key key;
    key.serial = serial;
    key.world = &world;
    key.interp = interp;
    for (int i = 0; i < 3; i++) key.up[i] = up[i];
    key.camera_height = camera_height;
    key.max_height = max_height;
    key.high = high;
    if (key == key_) return;   // the same image again: the structures hold it (or will, if not recorded yet)
    bool same_scene = key.serial == key_.serial && key.world == key_.world;
    key_ = key;
    for (Level& l : levels_) {
        l.solid_pos.clear();
        l.cut_pos.clear();
        l.cut_data.clear();
    }
    size_t end = std::min<size_t>(size_t(world.opaque_first) + world.opaque_count, scene.vertices.size());
    for (size_t i = world.opaque_first; i + 2 < end; i += 3) {
        const R3DVertex* v = &scene.vertices[i];
        float p[9], lowest = 1e30f;
        for (int k = 0; k < 3; k++) {
            for (int c = 0; c < 3; c++) p[k * 3 + c] = v[k].prev[c] + (v[k].pos[c] - v[k].prev[c]) * interp;
            lowest = std::min(lowest, p[k * 3] * up[0] + p[k * 3 + 1] * up[1] + p[k * 3 + 2] * up[2] + camera_height);
        }
        // Canopies and roofs high above would put whole scenes in the dark:
        // they cast no sun shadows, only the ambient occlusion sees them
        bool above = lowest > max_height;
        if (above && !high) continue;
        Level& l = levels_[above ? 1 : 0];
        uint32_t flags = v[0].flags;
        if ((flags & R3D_TEXTURED) && (flags & (R3D_ALPHA_TEST | R3D_TEX_ALPHA))) {
            l.cut_pos.insert(l.cut_pos.end(), p, p + 9);
            Cutout c;
            for (int k = 0; k < 3; k++) {
                c.uv[k * 2] = v[k].uv[0];
                c.uv[k * 2 + 1] = v[k].uv[1];
            }
            c.tex = v[0].tex;
            c.flags = flags;
            c.replace = v[0].replace;
            c.alpha = float(v[0].color >> 24) / 255.0f;
            l.cut_data.push_back(c);
        } else {
            l.solid_pos.insert(l.solid_pos.end(), p, p + 9);
        }
    }
    uint32_t total = 0;
    for (Level& l : levels_) total += uint32_t((l.solid_pos.size() + l.cut_pos.size()) / 9);
    if (!reserve(total)) {
        for (Level& l : levels_) l.solid = l.cut = 0;
        dirty_ = rebuild_ = true;
        key_ = Key{};
        return;
    }
    float* pos = static_cast<float*>(positions_.mapped);
    Cutout* cut = static_cast<Cutout*>(cutouts_.mapped);
    uint32_t first = 0, first_cut = 0;
    VkAccelerationStructureInstanceKHR inst[2] = {};
    for (int i = 0; i < 2; i++) {
        Level& l = levels_[i];
        l.solid = uint32_t(l.solid_pos.size() / 9);
        l.cut = uint32_t(l.cut_data.size());
        l.first = first;
        if (l.solid) memcpy(pos + size_t(first) * 9, l.solid_pos.data(), l.solid_pos.size() * sizeof(float));
        if (l.cut) {
            memcpy(pos + size_t(first + l.solid) * 9, l.cut_pos.data(), l.cut_pos.size() * sizeof(float));
            memcpy(cut + first_cut, l.cut_data.data(), l.cut * sizeof(Cutout));
        }
        // The world as it is: no transform; the custom index finds the cut-outs
        inst[i].transform.matrix[0][0] = inst[i].transform.matrix[1][1] = inst[i].transform.matrix[2][2] = 1.0f;
        inst[i].instanceCustomIndex = first_cut;
        inst[i].mask = i == 0 ? RT_LOW : RT_HIGH;
        inst[i].flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst[i].accelerationStructureReference = l.address;
        first += l.solid + l.cut;
        first_cut += l.cut;
    }
    memcpy(instances_.mapped, inst, sizeof(inst));
    // Between two game frames the same triangles only move: a refit, unless
    // the buffers were made again or a triangle crossed max_height (other counts)
    bool same_counts = true;
    for (const Level& l : levels_) same_counts &= l.solid == l.built_solid && l.cut == l.built_cut;
    rebuild_ = !same_scene || !same_counts || generation_ != built_generation_;
    dirty_ = true;
}

void R3DRayTracer::record(VkCommandBuffer cmd) {
    if (!dirty_) return;
    VkDeviceAddress scratch = (scratch_.address + scratch_align_ - 1) / scratch_align_ * scratch_align_;
    VkAccelerationStructureBuildGeometryInfoKHR infos[2];
    VkAccelerationStructureGeometryKHR geos[2][2];
    VkAccelerationStructureBuildRangeInfoKHR ranges[2][2];
    const VkAccelerationStructureBuildRangeInfoKHR* pr[2];
    for (int i = 0; i < 2; i++) {
        const Level& l = levels_[i];
        build_info(infos[i], geos[i], l);
        infos[i].dstAccelerationStructure = l.blas;
        if (!rebuild_) {   // in place, from what it holds
            infos[i].mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
            infos[i].srcAccelerationStructure = l.blas;
        }
        infos[i].scratchData.deviceAddress = scratch + l.scratch_offset;
        ranges[i][0] = {l.solid, 0, 0, 0};
        ranges[i][1] = {l.cut, 0, 0, 0};
        pr[i] = ranges[i];
    }
    vkCmdBuildAccelerationStructuresKHR(cmd, 2, infos, pr);

    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &mb, 0, nullptr, 0, nullptr);

    VkAccelerationStructureGeometryKHR tgeo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tgeo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tgeo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tgeo.geometry.instances.data.deviceAddress = instances_.address;
    VkAccelerationStructureBuildGeometryInfoKHR tinfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tinfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tinfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    tinfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tinfo.geometryCount = 1;
    tinfo.pGeometries = &tgeo;
    tinfo.dstAccelerationStructure = tlas_;
    tinfo.scratchData.deviceAddress = scratch + tlas_scratch_offset_;
    VkAccelerationStructureBuildRangeInfoKHR trange{2, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ptr = &trange;
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &tinfo, &ptr);

    mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    for (Level& l : levels_) {
        l.built_solid = l.solid;
        l.built_cut = l.cut;
    }
    built_generation_ = generation_;
    dirty_ = rebuild_ = false;
}
