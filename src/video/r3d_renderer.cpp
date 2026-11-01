#include "r3d_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include "ao.frag.h"
#include "ao_rt.frag.h"
#include "ao_apply.frag.h"
#include "bloom.frag.h"
#include "compose.frag.h"
#include "compose.vert.h"
#include "r3d.frag.h"
#include "r3d_rt.frag.h"
#include "r3d.vert.h"
#include "rays.frag.h"
#include "sky_fields.frag.h"
#include "../common/clock.h"
#include "tilegen.h"

namespace {

constexpr size_t TEXT_CELLS_BYTES = TextHD::ROW_TEXELS * TextHD::ROWS * 4;
constexpr size_t SKY_COORDS_BYTES = size_t(TileGen::SKY_COORDS_W) * TileGen::SKY_COORDS_H * 4;

struct ViewportPush {
    float proj[16];
    float sun[4];
    float fog_color[4];
    float params[4];    // ambient, fog density, fog start, interpolation of the previous image
    float quality[4];   // MSAA samples (negative: translucent pass; fraction: DLAA alpha noise seed),
                        // max anisotropic taps
};
static_assert(sizeof(ViewportPush) <= 128, "push constant budget");

// Uniform block of atmosphere.glsl
struct AtmosphereUniform {
    float axes[12];
    float camera[4];
    float sun[4];
    float view[4];
    float haze[4];
    float range[4];
    float backdrop[4];
    float scroll_fog[4];
    float scroll_rect[4];
    float shadow_matrix[16];
    float shadow[4];
    float soft[4];
    float mist[4];
    float sunlight[4];
    float spot[4];
    float spot_color[4];
    float spot_fog[4];
    float night[4];
    float rain[4];
    float wind[4];
    float bolt[4];
    float bolt_light[4];
    float bolt_seg[R3DRenderer::BOLT_SEGMENTS * 4];
    float relief[4];
    float sky_sun[4];
    float rt[4];
    float weather[4];
};

constexpr uint32_t SHADOW_SIZE = 2048;   // shadow map, square
constexpr float SHADOW_RANGE = 220.0f;   // how far in front of the camera shadows are drawn (view units)

// Seconds since the first frame: the drift of the procedural clouds
float launch_seconds() {
    static const double launch = now_seconds();
    return float(now_seconds() - launch);
}

VkShaderModule make_module(VkDevice dev, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (VkResult r = vkCreateShaderModule(dev, &ci, nullptr, &m); r != VK_SUCCESS)
        fprintf(stderr, "[vulkan] vkCreateShaderModule failed (%d)\n", int(r));
    return m;
}

}  // namespace

ScreenLayout compute_layout(float win_w, float win_h, float max_aspect) {
    ScreenLayout l{};
    float aspect = win_h > 0 ? win_w / win_h : 4.0f / 3.0f;
    float max_w = SCREEN_H * std::max(max_aspect, float(SCREEN_W) / SCREEN_H);
    l.view_w = std::clamp(SCREEN_H * aspect, float(SCREEN_W), max_w);
    l.scale = std::min(win_w / l.view_w, win_h / SCREEN_H);
    l.ox = (win_w - l.view_w * l.scale) / 2;
    l.oy = (win_h - SCREEN_H * l.scale) / 2;
    l.game_x0 = (l.view_w - SCREEN_W) / 2;
    return l;
}

bool R3DRenderer::create_buffer(Buffer& b, size_t size, VkBufferUsageFlags usage, bool device_local) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    if (VkResult r = vkCreateBuffer(dev_, &bi, nullptr, &b.buffer); r != VK_SUCCESS) {
        fprintf(stderr, "[vulkan] vkCreateBuffer of %zu bytes failed (%d)\n", size, int(r));
        return false;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, b.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    // The GPU's own memory the host can write (the BAR: 256 MB on most discrete
    // GPUs, all of it with resizable BAR or on an integrated GPU) saves it a
    // trip over the bus for every read of what is drawn from this buffer
    VkResult r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (device_local) {
        ai.memoryTypeIndex = vk_->find_memory(req.memoryTypeBits, host | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ai.memoryTypeIndex != UINT32_MAX) r = vkAllocateMemory(dev_, &ai, nullptr, &b.memory);
        b.device_local = r == VK_SUCCESS;
    }
    if (r != VK_SUCCESS) {
        ai.memoryTypeIndex = vk_->find_memory(req.memoryTypeBits, host);
        if (ai.memoryTypeIndex == UINT32_MAX) {
            fprintf(stderr, "[vulkan] no host-visible memory for a buffer of %zu bytes (types %x)\n", size,
                    req.memoryTypeBits);
            return false;
        }
        if (r = vkAllocateMemory(dev_, &ai, nullptr, &b.memory); r != VK_SUCCESS) {
            fprintf(stderr, "[vulkan] vkAllocateMemory of %llu bytes for a buffer failed (%d)\n",
                    (unsigned long long)req.size, int(r));
            return false;
        }
    }
    vkBindBufferMemory(dev_, b.buffer, b.memory, 0);
    vkMapMemory(dev_, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped);
    b.size = size;
    return true;
}

void R3DRenderer::destroy_buffer(Buffer& b) {
    if (b.buffer) vkDestroyBuffer(dev_, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(dev_, b.memory, nullptr);
    b = Buffer{};
}

bool R3DRenderer::create_image(Image& img, uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage,
                               VkImageAspectFlags aspect, VkSampleCountFlagBits samples) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {w, h, 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = samples;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    if (VkResult r = vkCreateImage(dev_, &ii, nullptr, &img.image); r != VK_SUCCESS) {
        fprintf(stderr, "[vulkan] vkCreateImage %ux%u format %d usage %x failed (%d)\n", w, h, int(format), usage,
                int(r));
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev_, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = vk_->find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) {
        fprintf(stderr, "[vulkan] no device-local memory for a %ux%u image format %d (types %x)\n", w, h,
                int(format), req.memoryTypeBits);
        return false;
    }
    if (VkResult r = vkAllocateMemory(dev_, &ai, nullptr, &img.memory); r != VK_SUCCESS) {
        fprintf(stderr, "[vulkan] vkAllocateMemory of %llu bytes for a %ux%u image failed (%d)\n",
                (unsigned long long)req.size, w, h, int(r));
        return false;
    }
    vkBindImageMemory(dev_, img.image, img.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCreateImageView(dev_, &vi, nullptr, &img.view);
    img.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    img.w = w;
    img.h = h;
    return true;
}

void R3DRenderer::destroy_image(Image& img) {
    if (img.view) vkDestroyImageView(dev_, img.view, nullptr);
    if (img.alpha_view) vkDestroyImageView(dev_, img.alpha_view, nullptr);
    if (img.image) vkDestroyImage(dev_, img.image, nullptr);
    if (img.memory) vkFreeMemory(dev_, img.memory, nullptr);
    img = Image{};
}

void R3DRenderer::transition(VkCommandBuffer cmd, Image& img, VkImageLayout to, VkImageAspectFlags aspect) {
    if (img.layout == to) return;
    vk_image_barrier(cmd, img.image, img.layout, to, aspect);
    img.layout = to;
}

bool R3DRenderer::init(VkPresenter& vk, float supersample, int msaa, int aniso, bool dlaa) {
    vk_ = &vk;
    dev_ = vk.device();
    supersample_ = std::clamp(supersample, 0.5f, 4.0f);
    aniso_ = float(std::clamp(aniso, 0, 16));
    // DLAA does the anti-aliasing itself from aliased single-sample input
    if (dlaa && dlaa_.init(vk.instance(), vk.gpu(), dev_)) msaa = 1;
    msaa_wanted_ = msaa;
    samples_ = msaa_level(msaa);
    const VkPhysicalDeviceLimits& limits = vk.limits();
    // The texture pack is an array of images indexed per polygon: it needs the
    // descriptor indexing features, and as many slots as the GPU's limits
    // leave after the other bindings of the sets that go with it (16 to spare)
    if (!vk.descriptor_indexing()) {
        fprintf(stderr, "[vulkan] this GPU lacks the descriptor indexing features the renderer needs (see above)\n");
        return false;
    }
    uint32_t room = std::min({limits.maxPerStageDescriptorSampledImages, limits.maxPerStageDescriptorSamplers,
                              limits.maxDescriptorSetSampledImages, limits.maxDescriptorSetSamplers, limits.maxPerStageResources});
    replacement_slots_ = std::clamp(room - std::min(room, 32u + Pack2D::MAX_SLOTS), 1u, TexturePack::MAX_SLOTS);
    if (replacement_slots_ < TexturePack::MAX_SLOTS)
        printf("[vulkan] descriptor limits of this GPU: %u texture pack slots instead of %u\n", replacement_slots_,
               TexturePack::MAX_SLOTS);

    const VkImageUsageFlags sampled = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!create_image(texture_ram_, 2048, 2048, VK_FORMAT_R16_UINT, sampled, VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    if (!create_image(back_, BACK_W, SCREEN_H, VK_FORMAT_B8G8R8A8_UNORM, sampled, VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    if (!create_image(front_, SCREEN_W, SCREEN_H, VK_FORMAT_B8G8R8A8_UNORM, sampled, VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    if (!create_buffer(texture_staging_, 2048 * 2048 * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return false;
    if (!create_image(text_cells_, TextHD::ROW_TEXELS, TextHD::ROWS, VK_FORMAT_B8G8R8A8_UNORM,
                      sampled, VK_IMAGE_ASPECT_COLOR_BIT))
        return false;
    if (!create_image(sky_coords_, TileGen::SKY_COORDS_W, TileGen::SKY_COORDS_H, VK_FORMAT_B8G8R8A8_UNORM, sampled,
                      VK_IMAGE_ASPECT_COLOR_BIT))
        return false;
    if (!create_buffer(layer_staging_, (BACK_W + SCREEN_W) * SCREEN_H * 4 + TEXT_CELLS_BYTES + SKY_COORDS_BYTES,
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
        return false;
    // What the GPU reads every image (the vertices, several times a frame)
    // goes to its own memory when there is some the host can write
    if (!create_buffer(vertices_, 8u << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true)) return false;
    if (!create_buffer(atmosphere_uniform_, sizeof(AtmosphereUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true))
        return false;
    memset(atmosphere_uniform_.mapped, 0, sizeof(AtmosphereUniform));
    printf("[vulkan] vertices in %s memory\n", vertices_.device_local ? "the GPU's own" : "system");
    // The procedural sky's clouds, drawn a strip per frame (record_sky_fields)
    if (!create_image(sky_fields_, SKY_FIELDS_W, SKY_FIELDS_H, VK_FORMAT_R8G8B8A8_UNORM,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
        return false;

    // Clear the texture RAM and the 2D layers once
    if (!create_image(shadow_map_, SHADOW_SIZE, SHADOW_SIZE, VK_FORMAT_D32_SFLOAT,
                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT))
        return false;
    if (!create_image(exposure_, 1, 1, VK_FORMAT_R16_SFLOAT,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT))
        return false;
    vk.run_once([&](VkCommandBuffer cmd) {
        transition(cmd, shadow_map_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        VkClearColorValue start{};
        start.float32[0] = std::log(0.17f);   // a typical scene: exposure 1
        VkImageSubresourceRange one{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        transition(cmd, exposure_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(cmd, exposure_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &start, 1, &one);
        transition(cmd, exposure_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkClearColorValue zero{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (Image* img : {&texture_ram_, &back_, &front_, &text_cells_, &sky_coords_}) {
            transition(cmd, *img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
            transition(cmd, *img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    });

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(dev_, &si, nullptr, &nearest_);
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    vkCreateSampler(dev_, &si, nullptr, &linear_);
    // Replacement textures: trilinear + anisotropic, repeating (mirroring is done in the shader)
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.anisotropyEnable = vk.anisotropy() ? VK_TRUE : VK_FALSE;
    si.maxAnisotropy = vk.anisotropy() ? std::min(16.0f, limits.maxSamplerAnisotropy) : 1.0f;
    si.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(dev_, &si, nullptr, &replace_sampler_);
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(dev_, &si, nullptr, &overlay_sampler_);
    // The sky panorama wraps around the camera (x), from horizon to zenith (y)
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.anisotropyEnable = VK_FALSE;
    si.maxAnisotropy = 1.0f;
    si.maxLod = 0.0f;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    vkCreateSampler(dev_, &si, nullptr, &sky_sampler_);

    // Ray tracing, when the GPU has it: its acceleration structure is built once
    // empty here, so the descriptors always point at a valid one
    if (rt_.init(vk)) printf("[video] ray tracing available (sun shadows)\n");

    // Descriptor sets: texture RAM for the 3D pass; 3D image + two 2D layers for composition;
    // both also see the atmosphere parameters
    // (3: the 2D back layer, which the distance haze fades the far scenery into)
    VkDescriptorSetLayoutBinding b0[8] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, replacement_slots_, VK_SHADER_STAGE_FRAGMENT_BIT},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},   // sun shadow map
        {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},   // world depth (soft particles)
        {6, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_FRAGMENT_BIT},   // ray tracing: the world
        {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},   // ray tracing: cut-out triangles
    };
    const uint32_t r3d_bindings = rt_.ready() ? 8 : 6;
    // Replacement slots are filled while earlier frames may still be in flight
    VkDescriptorBindingFlags bflags[8] = {
        0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT, 0, 0, 0, 0, 0, 0};
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bfi.bindingCount = r3d_bindings;
    bfi.pBindingFlags = bflags;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.pNext = &bfi;
    li.bindingCount = r3d_bindings;
    li.pBindings = b0;
    vkCreateDescriptorSetLayout(dev_, &li, nullptr, &r3d_set_layout_);
    li.pNext = nullptr;
    // 5: high-resolution text atlas (only bound when there is one), 6: text cells,
    // 7: alpha of the 3D image (in red), 8: bloom, 9: backdrop replacement coordinates,
    // 10: light shafts, 11: exposure, 12: diffusion, 13: the players' own crosshair pictures,
    // 14: the procedural sky's clouds
    VkDescriptorSetLayoutBinding cb[15];
    for (uint32_t i = 0; i < 3; i++)
        cb[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, Pack2D::MAX_SLOTS, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[4] = {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[5] = {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[6] = {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[7] = {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[8] = {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[9] = {9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[10] = {10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[11] = {11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[12] = {12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[13] = {13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[14] = {14, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorBindingFlags cflags[15] = {0, 0, 0, bflags[1], 0, bflags[1], 0, 0, 0, 0, 0, 0, 0, bflags[1], 0};
    bfi.bindingCount = 15;
    bfi.pBindingFlags = cflags;
    li.pNext = &bfi;
    li.bindingCount = 15;
    li.pBindings = cb;
    vkCreateDescriptorSetLayout(dev_, &li, nullptr, &compose_set_layout_);
    li.pNext = nullptr;
    // Bloom passes: one source image each
    li.bindingCount = 1;
    li.pBindings = cb;
    vkCreateDescriptorSetLayout(dev_, &li, nullptr, &bloom_set_layout_);
    // Ambient occlusion: world depth and the raw occlusion
    li.bindingCount = 2;
    vkCreateDescriptorSetLayout(dev_, &li, nullptr, &ao_set_layout_);

    VkDescriptorPoolSize ps[4] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 35 + replacement_slots_ + Pack2D::MAX_SLOTS},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
    };
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 9;
    pi.poolSizeCount = rt_.ready() ? 4 : 2;
    pi.pPoolSizes = ps;
    vkCreateDescriptorPool(dev_, &pi, nullptr, &pool_);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &r3d_set_layout_;
    vkAllocateDescriptorSets(dev_, &ai, &r3d_set_);
    ai.pSetLayouts = &compose_set_layout_;
    vkAllocateDescriptorSets(dev_, &ai, &compose_set_);
    ai.pSetLayouts = &bloom_set_layout_;
    for (VkDescriptorSet& s : bloom_sets_) vkAllocateDescriptorSets(dev_, &ai, &s);
    vkAllocateDescriptorSets(dev_, &ai, &rays_set_);
    for (VkDescriptorSet& s : diff_sets_) vkAllocateDescriptorSets(dev_, &ai, &s);
    ai.pSetLayouts = &ao_set_layout_;
    vkAllocateDescriptorSets(dev_, &ai, &ao_set_);

    VkDescriptorImageInfo tex{nearest_, texture_ram_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = r3d_set_;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &tex;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
    VkDescriptorImageInfo backdrop{linear_, back_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    w.dstBinding = 3;
    w.pImageInfo = &backdrop;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
    VkDescriptorImageInfo shadow{nearest_, shadow_map_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    w.dstBinding = 4;
    w.pImageInfo = &shadow;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
    if (rt_.ready()) {
        VkAccelerationStructureKHR tlas = rt_.tlas();
        VkWriteDescriptorSetAccelerationStructureKHR as{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        as.accelerationStructureCount = 1;
        as.pAccelerationStructures = &tlas;
        VkWriteDescriptorSet aw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        aw.pNext = &as;
        aw.dstSet = r3d_set_;
        aw.dstBinding = 6;
        aw.descriptorCount = 1;
        aw.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        vkUpdateDescriptorSets(dev_, 1, &aw, 0, nullptr);
        VkDescriptorBufferInfo cut{rt_.cutouts(), 0, VK_WHOLE_SIZE};
        aw.pNext = nullptr;
        aw.dstBinding = 7;
        aw.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        aw.pBufferInfo = &cut;
        vkUpdateDescriptorSets(dev_, 1, &aw, 0, nullptr);
        rt_generation_ = rt_.generation();
    }
    w.dstBinding = 0;
    VkDescriptorBufferInfo atmosphere_info{atmosphere_uniform_.buffer, 0, sizeof(AtmosphereUniform)};
    VkWriteDescriptorSet fw[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    for (int i = 0; i < 2; i++) {
        fw[i].dstSet = i == 0 ? r3d_set_ : compose_set_;
        fw[i].dstBinding = i == 0 ? 2 : 4;
        fw[i].descriptorCount = 1;
        fw[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        fw[i].pBufferInfo = &atmosphere_info;
    }
    vkUpdateDescriptorSets(dev_, 2, fw, 0, nullptr);
    VkDescriptorImageInfo exposure{nearest_, exposure_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    w.dstSet = compose_set_;
    w.dstBinding = 11;
    w.pImageInfo = &exposure;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
    VkDescriptorImageInfo cells{nearest_, text_cells_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    w.dstSet = compose_set_;
    w.dstBinding = 6;
    w.pImageInfo = &cells;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
    VkDescriptorImageInfo sky{nearest_, sky_coords_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    w.dstBinding = 9;
    w.pImageInfo = &sky;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
    VkDescriptorImageInfo fields{sky_sampler_, sky_fields_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    w.dstBinding = 14;
    w.pImageInfo = &fields;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);

    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ViewportPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &r3d_set_layout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    vkCreatePipelineLayout(dev_, &pli, nullptr, &r3d_layout_);
    VkPushConstantRange cpr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ComposePush)};
    pli.pSetLayouts = &compose_set_layout_;
    pli.pPushConstantRanges = &cpr;
    vkCreatePipelineLayout(dev_, &pli, nullptr, &compose_layout_);
    VkPushConstantRange bpr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    pli.pSetLayouts = &bloom_set_layout_;
    pli.pPushConstantRanges = &bpr;
    vkCreatePipelineLayout(dev_, &pli, nullptr, &bloom_layout_);
    pli.setLayoutCount = 0;   // the sky panorama: push constants only
    vkCreatePipelineLayout(dev_, &pli, nullptr, &sky_layout_);
    pli.setLayoutCount = 1;
    VkPushConstantRange apr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80};
    // Traced, the ambient occlusion also sees the 3D pass's set (the world's rays)
    VkDescriptorSetLayout ao_sets[2] = {ao_set_layout_, r3d_set_layout_};
    pli.setLayoutCount = rt_.ready() ? 2 : 1;
    pli.pSetLayouts = ao_sets;
    pli.pPushConstantRanges = &apr;
    vkCreatePipelineLayout(dev_, &pli, nullptr, &ao_layout_);

    return create_pipelines();
}

VkPipeline R3DRenderer::make_pipeline(const PipelineDesc& d) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, d.vs, "main"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, d.fs, "main",
                 d.fs_spec};

    VkVertexInputBindingDescription binding{0, sizeof(R3DVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[9] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(R3DVertex, pos)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(R3DVertex, nrm)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(R3DVertex, uv)},
        {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(R3DVertex, color)},
        {4, 0, VK_FORMAT_R32_UINT, offsetof(R3DVertex, tex)},
        {5, 0, VK_FORMAT_R32_UINT, offsetof(R3DVertex, flags)},
        {6, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(R3DVertex, shade)},
        {7, 0, VK_FORMAT_R32_UINT, offsetof(R3DVertex, replace)},
        {8, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(R3DVertex, prev)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (d.vertex_input) {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = 9;
        vi.pVertexAttributeDescriptions = attrs;
    }
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;   // facing is decided on the CPU with the face normal
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = d.samples;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d.depth;
    ds.depthWriteEnable = d.depth_write;
    ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;   // reverse Z
    VkPipelineColorBlendAttachmentState cbas[3]{};
    VkPipelineColorBlendAttachmentState& cba = cbas[0];
    cba.colorWriteMask = 0xF;
    // Motion vectors: written by the opaque pass only, translucent surfaces keep what is behind
    cbas[1].colorWriteMask = d.depth_write ? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT : 0;
    if (d.blend) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    if (d.additive) {   // colour = colour + output (the sky's glow added to the bloom)
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    if (d.ease > 0) {   // colour moves a fraction of the way towards the output (the exposure)
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
    }
    if (d.multiply) {   // colour = colour * output (ambient occlusion); alpha kept
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    }
    // Bloom source: blended like the colour. With it the motion slot is always
    // there (without an image when DLAA is off)
    cbas[2] = cba;
    if (d.depth_only)
        for (auto& c : cbas) c.colorWriteMask = 0;
    uint32_t attachments = d.glow ? 3 : d.motion ? 2 : 1;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = attachments;
    cb.blendConstants[3] = d.ease;
    cb.pAttachments = cbas;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;

    VkPipelineRenderingCreateInfo rend{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkFormat formats[3] = {d.color, d.motion ? VK_FORMAT_R16G16_SFLOAT : VK_FORMAT_UNDEFINED, VK_FORMAT_R8G8B8A8_UNORM};
    rend.colorAttachmentCount = attachments;
    rend.pColorAttachmentFormats = formats;
    rend.depthAttachmentFormat = d.depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &rend;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = d.layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (VkResult r = vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &p); r != VK_SUCCESS)
        fprintf(stderr, "[vulkan] vkCreateGraphicsPipelines failed (%d)\n", int(r));
    return p;
}

bool R3DRenderer::create_pipelines() {
    VkShaderModule r3d_vs = make_module(dev_, spv_r3d_vert, sizeof(spv_r3d_vert));
    // With ray queries the 3D pass can also trace the sun shadows
    VkShaderModule r3d_fs = rt_.ready() ? make_module(dev_, spv_r3d_rt_frag, sizeof(spv_r3d_rt_frag))
                                        : make_module(dev_, spv_r3d_frag, sizeof(spv_r3d_frag));
    VkShaderModule c_vs = make_module(dev_, spv_compose_vert, sizeof(spv_compose_vert));
    VkShaderModule c_fs = make_module(dev_, spv_compose_frag, sizeof(spv_compose_frag));
    VkShaderModule b_fs = make_module(dev_, spv_bloom_frag, sizeof(spv_bloom_frag));
    VkShaderModule rays_fs = make_module(dev_, spv_rays_frag, sizeof(spv_rays_frag));
    VkShaderModule ao_fs = rt_.ready() ? make_module(dev_, spv_ao_rt_frag, sizeof(spv_ao_rt_frag))
                                       : make_module(dev_, spv_ao_frag, sizeof(spv_ao_frag));
    VkShaderModule aoa_fs = make_module(dev_, spv_ao_apply_frag, sizeof(spv_ao_apply_frag));
    VkShaderModule sky_fs = make_module(dev_, spv_sky_fields_frag, sizeof(spv_sky_fields_frag));
    bool motion = motion_pass_ = dlaa_.ready();
    // r3d.frag sizes its texture pack array by this constant (constant_id 0)
    VkSpecializationMapEntry slots_entry{0, 0, sizeof(uint32_t)};
    VkSpecializationInfo r3d_spec{1, &slots_entry, sizeof(replacement_slots_), &replacement_slots_};
    // The world: 3D geometry into the HDR image, with the motion and glow slots
    // (the glow only while the bloom is on: without it nothing reads it)
    PipelineDesc world{.vs = r3d_vs, .fs = r3d_fs, .layout = r3d_layout_, .color = VK_FORMAT_R16G16B16A16_SFLOAT,
                       .depth = true, .depth_write = true, .vertex_input = true, .samples = samples_,
                       .motion = motion, .glow = glow_pass_, .fs_spec = &r3d_spec};
    opaque_ = make_pipeline(world);
    PipelineDesc translucent = world;
    translucent.depth_write = false;
    translucent.blend = true;
    translucent_ = make_pipeline(translucent);
    PipelineDesc trans_depth = world;
    trans_depth.depth_only = true;
    trans_depth_ = make_pipeline(trans_depth);
    shadow_ = make_pipeline({.vs = r3d_vs, .fs = r3d_fs, .layout = r3d_layout_, .color = VK_FORMAT_UNDEFINED,
                             .depth = true, .depth_write = true, .vertex_input = true, .fs_spec = &r3d_spec});
    // Full-screen passes: a triangle over the target, no vertex input
    compose_format_ = vk_->swap_format();
    PipelineDesc compose{.vs = c_vs, .fs = c_fs, .layout = compose_layout_, .color = compose_format_};
    compose_ = make_pipeline(compose);
    PipelineDesc blur{.vs = c_vs, .fs = b_fs, .layout = bloom_layout_, .color = VK_FORMAT_R8G8B8A8_UNORM};
    bloom_ = make_pipeline(blur);
    blur.fs = rays_fs;
    rays_ = make_pipeline(blur);
    // A twentieth of the way per frame: the eye adapts over about a second
    PipelineDesc measure = compose;
    measure.color = VK_FORMAT_R16_SFLOAT;
    measure.ease = 0.05f;
    compose_measure_ = make_pipeline(measure);
    PipelineDesc glow = compose;
    glow.color = VK_FORMAT_R8G8B8A8_UNORM;
    glow.additive = true;
    compose_glow_ = make_pipeline(glow);
    ao_pass_ = make_pipeline({.vs = c_vs, .fs = ao_fs, .layout = ao_layout_, .color = VK_FORMAT_R8_UNORM});
    ao_apply_ = make_pipeline({.vs = c_vs, .fs = aoa_fs, .layout = ao_layout_, .color = VK_FORMAT_R16G16B16A16_SFLOAT,
                               .samples = samples_, .multiply = true});
    sky_fields_pipe_ = make_pipeline({.vs = c_vs, .fs = sky_fs, .layout = sky_layout_, .color = VK_FORMAT_R8G8B8A8_UNORM});
    for (auto m : {r3d_vs, r3d_fs, c_vs, c_fs, b_fs, rays_fs, ao_fs, aoa_fs, sky_fs}) vkDestroyShaderModule(dev_, m, nullptr);
    // Name the ones missing, so a report says which pass this GPU could not build
    bool ok = true;
    for (auto [p, name] : {std::pair{opaque_, "opaque"}, {translucent_, "translucent"}, {trans_depth_, "trans_depth"},
                           {shadow_, "shadow"}, {rays_, "rays"}, {compose_measure_, "compose_measure"},
                           {compose_, "compose"}, {bloom_, "bloom"}, {ao_pass_, "ao"}, {ao_apply_, "ao_apply"},
                           {sky_fields_pipe_, "sky_fields"}})
        if (!p) {
            fprintf(stderr, "[vulkan] no %s pipeline%s\n", name, rt_.ready() ? " (ray tracing on)" : "");
            ok = false;
        }
    return ok;
}

void R3DRenderer::destroy_pipelines() {
    for (VkPipeline* p : {&opaque_, &translucent_, &trans_depth_, &shadow_, &rays_, &compose_measure_, &compose_, &bloom_,
                          &compose_glow_, &ao_pass_, &ao_apply_, &sky_fields_pipe_})
        if (*p) vkDestroyPipeline(dev_, *p, nullptr), *p = VK_NULL_HANDLE;
}

VkSampleCountFlagBits R3DRenderer::msaa_level(int samples) const {
    VkSampleCountFlags counts = vk_->limits().framebufferColorSampleCounts & vk_->limits().framebufferDepthSampleCounts;
    VkSampleCountFlagBits level = VK_SAMPLE_COUNT_1_BIT;
    for (int s = 2; s <= std::min(samples, 8); s *= 2)
        if (counts & s) level = VkSampleCountFlagBits(s);
    return level;
}

// MSAA changed in the menu, or the bloom went on or off: the world pipelines
// and the targets depend on the sample count and on whether the glow is
// drawn, so both are made again (between frames: nothing in flight)
void R3DRenderer::apply_pending() {
    VkSampleCountFlagBits level = samples_;
    if (pending_msaa_) {
        int wanted = pending_msaa_;
        pending_msaa_ = 0;
        if (!dlaa_.ready() && wanted != msaa_wanted_) {
            msaa_wanted_ = wanted;
            level = msaa_level(wanted);
        }
    }
    bool glow = bloom_strength_ > 0;
    if (level == samples_ && glow == glow_pass_) return;
    vkDeviceWaitIdle(dev_);
    bool msaa_changed = level != samples_;
    samples_ = level;
    glow_pass_ = glow;
    destroy_pipelines();
    if (!create_pipelines())
        fprintf(stderr, "[vulkan] could not make the pipelines again (MSAA %dx, bloom %s)\n", int(samples_),
                glow ? "on" : "off");
    destroy_image(color_);   // ensure_targets makes every target again
    can_repeat_ = false;
    if (msaa_changed) printf("[video] antialiasing MSAA %dx\n", int(samples_));
}

void R3DRenderer::ensure_targets(uint32_t w, uint32_t h) {
    if (color_.image && color_.w == w && color_.h == h) return;
    vkDeviceWaitIdle(dev_);
    destroy_image(color_);
    destroy_image(color_ms_);
    destroy_image(depth_);
    destroy_image(motion_);
    destroy_image(dlaa_out_);
    destroy_image(dlaa_alpha_);
    for (Image* img : {&glow_, &glow_ms_, &bloom_a_, &bloom_b_, &depth_resolved_, &ao_, &rays_a_, &rays_b_, &diff_a_, &diff_b_})
        destroy_image(*img);
    create_image(color_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageViewCreateInfo avi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    avi.image = color_.image;
    avi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    avi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    avi.components = {VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_ONE};
    avi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(dev_, &avi, nullptr, &color_.alpha_view);
    if (samples_ != VK_SAMPLE_COUNT_1_BIT)
        create_image(color_ms_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, samples_);
    bool dlaa = motion_pass_;
    create_image(depth_, w, h, VK_FORMAT_D32_SFLOAT,
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                 VK_IMAGE_ASPECT_DEPTH_BIT, samples_);
    // The world's depth, kept after its opaque surfaces (sample 0 of it with
    // MSAA, a copy without) for the ambient occlusion and the soft particles;
    // and the raw occlusion
    create_image(depth_resolved_, w, h, VK_FORMAT_D32_SFLOAT,
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                 VK_IMAGE_ASPECT_DEPTH_BIT);
    depth_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    // The occlusion at half the resolution each way (a quarter of the rays),
    // smoothed back up to full resolution by ao_apply.frag
    create_image(ao_, (w + 1) / 2, (h + 1) / 2, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 VK_IMAGE_ASPECT_COLOR_BIT);
    {
        VkDescriptorImageInfo src[2] = {
            {nearest_, depth_resolved_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {nearest_, ao_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet aw[2];
        for (int i = 0; i < 2; i++) {
            aw[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            aw[i].dstSet = ao_set_;
            aw[i].dstBinding = uint32_t(i);
            aw[i].descriptorCount = 1;
            aw[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            aw[i].pImageInfo = &src[i];
        }
        vkUpdateDescriptorSets(dev_, 2, aw, 0, nullptr);
        // ... and for the soft particles in the 3D pass
        VkWriteDescriptorSet dw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        dw.dstSet = r3d_set_;
        dw.dstBinding = 5;
        dw.descriptorCount = 1;
        dw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        dw.pImageInfo = &src[0];
        vkUpdateDescriptorSets(dev_, 1, &dw, 0, nullptr);
    }
    if (dlaa) {
        create_image(motion_, w, h, VK_FORMAT_R16G16_SFLOAT,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        create_image(dlaa_out_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        create_image(dlaa_alpha_, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    // Bloom: glow written by the 3D pass (only while the bloom is on), then
    // reduced to a quarter and blurred
    const VkImageUsageFlags target = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (glow_pass_) {
        create_image(glow_, w, h, VK_FORMAT_R8G8B8A8_UNORM, target, VK_IMAGE_ASPECT_COLOR_BIT);
        if (samples_ != VK_SAMPLE_COUNT_1_BIT)
            create_image(glow_ms_, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT, samples_);
    }
    for (Image* img : {&bloom_a_, &bloom_b_})
        create_image(*img, std::max(1u, w / 4), std::max(1u, h / 4), VK_FORMAT_R8G8B8A8_UNORM, target,
                     VK_IMAGE_ASPECT_COLOR_BIT);
    {
        VkDescriptorImageInfo src[4] = {{linear_, glow_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {linear_, bloom_a_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {linear_, bloom_b_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {linear_, bloom_a_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet bw[4];
        for (int i = 0; i < 4; i++) {
            bw[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            bw[i].dstSet = i < 3 ? bloom_sets_[i] : compose_set_;
            bw[i].dstBinding = i < 3 ? 0 : 8;
            bw[i].descriptorCount = 1;
            bw[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bw[i].pImageInfo = &src[i];
        }
        // Without the glow image its set keeps what it had (nothing reads it)
        int first = glow_pass_ ? 0 : 1;
        vkUpdateDescriptorSets(dev_, uint32_t(4 - first), bw + first, 0, nullptr);
    }
    // Light shafts: the mask and the shafts, a quarter of the size
    for (Image* img : {&rays_a_, &rays_b_})
        create_image(*img, std::max(1u, w / 4), std::max(1u, h / 4), VK_FORMAT_R8G8B8A8_UNORM, target,
                     VK_IMAGE_ASPECT_COLOR_BIT);
    {
        VkDescriptorImageInfo src[2] = {{linear_, rays_a_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {linear_, rays_b_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet rw[2];
        for (int i = 0; i < 2; i++) {
            rw[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            rw[i].dstSet = i == 0 ? rays_set_ : compose_set_;
            rw[i].dstBinding = i == 0 ? 0 : 10;
            rw[i].descriptorCount = 1;
            rw[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            rw[i].pImageInfo = &src[i];
        }
        vkUpdateDescriptorSets(dev_, 2, rw, 0, nullptr);
    }
    // Diffusion: the picture behind the HUD and its blur, a quarter of the size
    for (Image* img : {&diff_a_, &diff_b_})
        create_image(*img, std::max(1u, w / 4), std::max(1u, h / 4), VK_FORMAT_R8G8B8A8_UNORM, target,
                     VK_IMAGE_ASPECT_COLOR_BIT);
    {
        VkDescriptorImageInfo src[3] = {{linear_, diff_a_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {linear_, diff_b_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {linear_, diff_a_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet dw[3];
        for (int i = 0; i < 3; i++) {
            dw[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            dw[i].dstSet = i < 2 ? diff_sets_[i] : compose_set_;
            dw[i].dstBinding = i < 2 ? 0 : 12;
            dw[i].descriptorCount = 1;
            dw[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            dw[i].pImageInfo = &src[i];
        }
        vkUpdateDescriptorSets(dev_, 3, dw, 0, nullptr);
    }
    composed_view_ = VK_NULL_HANDLE;
    if (dlaa_.ready()) bind_scene_image(dlaa_out_.view, dlaa_alpha_.view);
    else bind_scene_image(color_.view, color_.alpha_view);
    VkDescriptorImageInfo infos[3] = {
        {linear_, color_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {nearest_, front_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {nearest_, back_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    VkWriteDescriptorSet w3[3];
    for (uint32_t i = 1; i < 3; i++) {
        w3[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w3[i].dstSet = compose_set_;
        w3[i].dstBinding = i;
        w3[i].descriptorCount = 1;
        w3[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w3[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(dev_, 2, w3 + 1, 0, nullptr);
}

// Bindings 0 and 7 of the composition: the plain 3D image or the DLAA output,
// and its alpha
void R3DRenderer::bind_scene_image(VkImageView view, VkImageView alpha) {
    if (view == composed_view_) return;
    VkDescriptorImageInfo info[2] = {{linear_, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                     {linear_, alpha, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet w[2];
    for (int i = 0; i < 2; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = compose_set_;
        w[i].dstBinding = i ? 7 : 0;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &info[i];
    }
    // (only while recording a frame: the one before is over, and this one has
    // not bound the composition's set yet)
    vkUpdateDescriptorSets(dev_, 2, w, 0, nullptr);
    composed_view_ = view;
}

void R3DRenderer::set_layers(const uint32_t* back, const uint32_t* front) {
    uint8_t* dst = static_cast<uint8_t*>(layer_staging_.mapped);
    memcpy(dst, back, BACK_W * SCREEN_H * 4);
    memcpy(dst + BACK_W * SCREEN_H * 4, front, SCREEN_W * SCREEN_H * 4);
    layers_dirty_ = true;
    // Average colour of each row of the back layer, for the horizon colour
    for (int y = 0; y < SCREEN_H; y++) {
        float sum[3] = {0, 0, 0}, n = 0;
        for (int x = 0; x < BACK_W; x += 2) {
            uint32_t p = back[y * BACK_W + x];
            float a = float(p >> 24);
            if (a < 128) continue;
            sum[0] += float((p >> 16) & 255) / a;   // premultiplied
            sum[1] += float((p >> 8) & 255) / a;
            sum[2] += float(p & 255) / a;
            n++;
        }
        for (int i = 0; i < 3; i++) back_rows_[y * 4 + i] = sum[i];
        back_rows_[y * 4 + 3] = n;
    }
    // Nothing but black (or nothing at all) behind the 3D image (the night at the trailer)
    // in the 4:3 area
    bool black = true;
    for (int y = 0; y < SCREEN_H && black; y++)
        for (int x = BACK_MARGIN; x < BACK_MARGIN + SCREEN_W; x++)
            if (back[y * BACK_W + x] & 0xFFFFFF) {
                black = false;
                break;
            }
    back_black_ = black;
}

void R3DRenderer::set_text_cells(const uint32_t* cells) {
    memcpy(static_cast<uint8_t*>(layer_staging_.mapped) + (BACK_W + SCREEN_W) * SCREEN_H * 4, cells,
           TEXT_CELLS_BYTES);
}

void R3DRenderer::set_sky_coords(const uint32_t* coords) {
    memcpy(static_cast<uint8_t*>(layer_staging_.mapped) + (BACK_W + SCREEN_W) * SCREEN_H * 4 + TEXT_CELLS_BYTES, coords,
           SKY_COORDS_BYTES);
}

void R3DRenderer::set_sky_rows(uint64_t frame, const int32_t* rows) { sky_motion_.feed(frame, rows); }

// (the motion is solved here, once the frame's layers, scene and shift are
// all in, so that the caller can read it before the frame is drawn)
void R3DRenderer::set_back_shift(float vblanks, bool camera) {
    back_shift_ = vblanks;
    back_shift_camera_ = camera;
    solve_sky_motion();
}

// The motion of the back layers for the 2D frame fed last (SkyMotion), with
// the world camera's turn from the previous 3D frame when the one drawn is
// of the same vblank; its texels go to the sky coordinates' last two rows
void R3DRenderer::solve_sky_motion() {
    if (sky_motion_.pending()) {
        const R3DViewport* w = nullptr;
        if (scene_)
            for (const R3DViewport& v : scene_->viewports)
                if (v.world) w = &v;
        refresh_pose(w);
        bool camera = back_shift_camera_ && !pose_cut_;
        float turn[2] = {0, 0};
        if (camera) {   // yaw and pitch of the camera's forward axis (the world's -z in view space)
            auto yaw = [](const float* a) { return std::atan2(-a[2], -a[8]); };
            auto pitch = [](const float* a) { return std::asin(std::clamp(-a[5], -1.0f, 1.0f)); };
            float dyaw = std::remainder(yaw(pose_now_.axes) - yaw(pose_prev_.axes), 6.2831853f);
            turn[0] = SkyMotion::PX_PER_RADIAN * dyaw;
            turn[1] = -SkyMotion::PX_PER_RADIAN * (pitch(pose_now_.axes) - pitch(pose_prev_.axes));
        }
        sky_motion_.solve(camera, turn[0], turn[1]);
        sky_motion_serial_++;
        layers_dirty_ = true;
    }
}

void R3DRenderer::update_sky_motion() {
    solve_sky_motion();
    if (layers_dirty_)
        memcpy(static_cast<uint8_t*>(layer_staging_.mapped) + (BACK_W + SCREEN_W) * SCREEN_H * 4 + TEXT_CELLS_BYTES +
                   size_t(SCREEN_H + 1) * TileGen::SKY_COORDS_W * 4,
               sky_motion_.texels(), 2 * SCREEN_H * 4);
}

void R3DRenderer::set_text_atlas(const TextHD& text) {
    if (text.atlas_rgba.empty()) return;
    upload_slot(compose_set_, 5, overlay_sampler_, 0, text.atlas_w, text.atlas_h, text.atlas_rgba);
}

void R3DRenderer::set_crosshair_picture(int player, int w, int h, const std::vector<uint8_t>& rgba) {
    crosshair_picture_[player] = !rgba.empty();
    if (!rgba.empty()) upload_slot(compose_set_, 13, overlay_sampler_, uint32_t(player), w, h, rgba, &crosshair_images_);
}

void R3DRenderer::upload_texture_rows(const std::vector<uint16_t>& rows, int y0, int y1) {
    memcpy(static_cast<uint8_t*>(texture_staging_.mapped) + size_t(y0) * 2048 * 2, rows.data(), rows.size() * 2);
    if (tex_y0_ >= tex_y1_) {
        tex_y0_ = y0;
        tex_y1_ = y1;
    } else {
        tex_y0_ = std::min(tex_y0_, y0);
        tex_y1_ = std::max(tex_y1_, y1);
    }
}

// A picture for a descriptor slot. It is not copied to the GPU here: frames
// in flight may still read the slot's previous picture, so its copy, its mip
// chain and the slot itself wait for the start of the next frame's commands
// (flush_uploads), when the frame before is over. A batch that grows past
// UPLOAD_BATCH (a whole texture pack at once) goes out right away in one submit
// instead, rather than holding that much staging memory.
constexpr size_t UPLOAD_BATCH = 64 << 20;

void R3DRenderer::upload_slot(VkDescriptorSet set, uint32_t binding, VkSampler sampler, uint32_t slot, int w, int h,
                              const std::vector<uint8_t>& rgba, std::unordered_map<uint32_t, Image>* replace) {
    uint32_t mips = 1;
    while ((std::max(w, h) >> mips) > 0) mips++;
    Image img;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {uint32_t(w), uint32_t(h), 1};
    ii.mipLevels = mips;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(dev_, &ii, nullptr, &img.image) != VK_SUCCESS) return;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev_, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = vk_->find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev_, &ai, nullptr, &img.memory) != VK_SUCCESS) {
        vkDestroyImage(dev_, img.image, nullptr);
        return;
    }
    vkBindImageMemory(dev_, img.image, img.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    vkCreateImageView(dev_, &vi, nullptr, &img.view);
    img.w = uint32_t(w);
    img.h = uint32_t(h);

    Upload u{.img = img, .mips = mips, .set = set, .binding = binding, .slot = slot, .sampler = sampler, .replace = replace};
    if (!create_buffer(u.staging, rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
        destroy_image(u.img);
        return;
    }
    memcpy(u.staging.mapped, rgba.data(), rgba.size());
    uploads_.push_back(u);
    upload_bytes_ += rgba.size();
    if (upload_bytes_ >= UPLOAD_BATCH) {
        vk_->run_once([&](VkCommandBuffer cmd) { record_pending_uploads(cmd); });   // (it waits for the queue)
        bind_pending_uploads(true);
    }
}

// The copies of the waiting pictures and their mip chains (each level a
// filtered blit of the previous one), ready to be sampled
void R3DRenderer::record_pending_uploads(VkCommandBuffer cmd) {
    for (Upload& u : uploads_) {
        Image& img = u.img;
        auto barrier = [&](uint32_t level, VkImageLayout from, VkImageLayout to) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            b.oldLayout = from;
            b.newLayout = to;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img.image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &b);
        };
        barrier(0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {img.w, img.h, 1};
        vkCmdCopyBufferToImage(cmd, u.staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        for (uint32_t l = 1; l < u.mips; l++) {
            barrier(l - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            barrier(l, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, 1};
            blit.srcOffsets[1] = {int32_t(std::max(1u, img.w >> (l - 1))), int32_t(std::max(1u, img.h >> (l - 1))), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
            blit.dstOffsets[1] = {int32_t(std::max(1u, img.w >> l)), int32_t(std::max(1u, img.h >> l)), 1};
            vkCmdBlitImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            barrier(l - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        barrier(u.mips - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        img.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
}

// Their slots, once the commands that copy them are recorded or done. What
// they replace (and their staging) may still be in use by the commands in
// flight unless the GPU is idle: freed at the next frame
void R3DRenderer::bind_pending_uploads(bool idle) {
    for (Upload& u : uploads_) {
        VkDescriptorImageInfo info{u.sampler, u.img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr.dstSet = u.set;
        wr.dstBinding = u.binding;
        wr.dstArrayElement = u.slot;
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &info;
        vkUpdateDescriptorSets(dev_, 1, &wr, 0, nullptr);
        if (u.replace) {
            auto old = u.replace->find(u.slot);
            if (old != u.replace->end()) spent_images_.push_back(old->second);
            (*u.replace)[u.slot] = u.img;
        } else {
            replacements_.push_back(u.img);
        }
        spent_staging_.push_back(u.staging);
    }
    uploads_.clear();
    upload_bytes_ = 0;
    if (idle) free_spent();
}

void R3DRenderer::free_spent() {
    for (Image& img : spent_images_) destroy_image(img);
    for (Buffer& b : spent_staging_) destroy_buffer(b);
    spent_images_.clear();
    spent_staging_.clear();
}

// At the start of a frame's commands, the frame before is over (one frame in
// flight): what it left spent is freed, and the waiting pictures go out
void R3DRenderer::flush_uploads(VkCommandBuffer cmd) {
    free_spent();
    if (uploads_.empty()) return;
    record_pending_uploads(cmd);
    bind_pending_uploads(false);
}

void R3DRenderer::add_replacements(const std::vector<RgbaImage>& images) {
    for (const RgbaImage& src : images)
        upload_slot(r3d_set_, 1, replace_sampler_, src.slot, src.w, src.h, src.rgba);
}

void R3DRenderer::add_overlays(const std::vector<RgbaImage>& images) {
    for (const RgbaImage& src : images)
        upload_slot(compose_set_, 3, overlay_sampler_, src.slot, src.w, src.h, src.rgba, &overlay_images_);
}

void R3DRenderer::add_sky_pictures(const std::vector<RgbaImage>& images) {
    for (const RgbaImage& src : images)
        upload_slot(compose_set_, 3, overlay_sampler_, src.slot, src.w, src.h, src.rgba, &overlay_images_);
}

// The background layers continue beside the 4:3 area only while the 3D
// scene is widened too; a small 3D window (camera effects) keeps black sides
bool R3DRenderer::back_layer_extended() const {
    if (!scene_ || scene_->viewports.empty()) return true;
    int scene_priority = scene_->viewports.front().priority;
    for (const R3DViewport& v : scene_->viewports)
        if (v.priority == scene_priority && v.x <= 0.5f && v.w >= SCREEN_W - 0.5f) return true;
    return false;
}

// Scroll fog (spec 7.4): from the lowest priority that has any, the viewport
// with the strongest scroll fog. It is drawn only when a viewport with the
// same fog colour actually fogs something.
static const R3DViewport* scroll_fog_viewport(const std::vector<R3DViewport>& vps) {
    const R3DViewport* best = nullptr;
    for (const R3DViewport& v : vps) {   // sorted by priority
        if (best && v.priority != best->priority) break;
        if (v.scroll_fog > 0 && (!best || v.scroll_fog > best->scroll_fog)) best = &v;
    }
    if (!best) return nullptr;
    auto same = [&](const float* c) { return c[0] == best->fog_color[0] && c[1] == best->fog_color[1] && c[2] == best->fog_color[2]; };
    for (const R3DViewport& v : vps) {
        bool spot = v.fog_attenuation > 0 && (v.spot_fog_color[0] + v.spot_fog_color[1] + v.spot_fog_color[2]) > 0;
        if (same(v.fog_color) && (v.fog_density > 0 || v.fog_start > 0 || v.scroll_attenuation > 0 || spot)) return best;
    }
    return nullptr;
}

static float lightning_hash(uint32_t k) {
    k ^= k >> 16;
    k *= 0x7FEB352Du;
    k ^= k >> 15;
    k *= 0x846CA68Bu;
    k ^= k >> 16;
    return float(k & 0xFFFFFF) / float(0x1000000);
}

// The lightning at a game time in seconds: every 4 seconds a strike happens
// (three times out of four) at a random moment, so they come every 5 or 6
// seconds on average, irregularly. A strike flickers three times and dies away; its
// channel shows while it flickers.
struct LightningStrike {
    float flash = 0;   // brightness of the light (0 most of the time)
    float bolt = 0;    // brightness of the visible channel
    int id = -1;       // which strike (seeds its shape and place)
};

static LightningStrike lightning_strike(float time) {
    const float SLOT = 4.0f;
    LightningStrike s;
    int slot = int(std::floor(time / SLOT));
    for (int k = slot - 1; k <= slot; k++) {
        if (k < 0 || lightning_hash(uint32_t(k) * 2u) > 0.75f) continue;
        float t = time - (float(k) + 0.75f * lightning_hash(uint32_t(k) * 2u + 1u)) * SLOT;
        if (t < 0 || t > 2.5f) continue;
        auto pulse = [&](float at, float width, float height) {
            float x = (t - at) / width;
            return height * std::exp(-x * x);
        };
        float f = pulse(0.0f, 0.04f, 0.7f) + pulse(0.16f, 0.05f, 0.4f) + pulse(0.30f, 0.06f, 0.65f);
        float bolt = t < 0.6f ? std::max(std::min(f / 0.4f, 1.0f), 0.35f * (1.0f - t / 0.6f)) : 0.0f;
        if (t > 0.30f) f += 0.25f * std::exp(-(t - 0.30f) / 0.4f);
        if (f > s.flash) s.flash = f, s.bolt = bolt, s.id = k;
    }
    return s;
}

// The channel of a strike, in directions around the camera: azimuth (from
// the strike's own, radians) and elevation at both ends of each segment. A
// main channel zigzags from the clouds down to the horizon, with a few
// shorter branches off it. Returns the number of main segments.
static int lightning_channel(int id, float* seg, int& count) {
    uint32_t seed = uint32_t(id) * 7919u + 17u;
    auto rnd = [&]() { return lightning_hash(seed++); };
    const int MAIN = 12;
    float az = 0, top = 0.55f + 0.25f * rnd();
    float points[MAIN + 1][2];
    for (int i = 0; i <= MAIN; i++) {   // even steps down, each a little longer or shorter
        float jitter = i > 0 && i < MAIN ? (rnd() - 0.5f) * 0.6f : 0.0f;
        points[i][0] = az;
        points[i][1] = top * (1.0f - (float(i) + jitter) / float(MAIN));
        az += (rnd() - 0.5f) * 0.07f;
    }
    count = 0;
    for (int i = 0; i < MAIN; i++, count++) {
        float* s = seg + count * 4;
        s[0] = points[i][0], s[1] = points[i][1], s[2] = points[i + 1][0], s[3] = points[i + 1][1];
    }
    for (int b = 0; b < 3 && count + 4 <= R3DRenderer::BOLT_SEGMENTS; b++) {
        int from = 1 + int(rnd() * 5.0f);
        float bx = points[from][0], by = points[from][1];
        float side = rnd() < 0.5f ? -1.0f : 1.0f;
        for (int i = 0; i < 4; i++, count++) {
            float nx = bx + side * (0.03f * rnd() - 0.004f), ny = by - (0.015f + 0.035f * rnd());
            float* s = seg + count * 4;
            s[0] = bx, s[1] = by, s[2] = nx, s[3] = ny;
            bx = nx, by = std::max(ny, 0.0f);
        }
    }
    return MAIN;
}

// The world viewport's camera between the previous game frame and the latest
// one, as the scenery is drawn (the interpolation); as in the latest frame
// after a cut (a jump of the camera, a sudden turn)
void R3DRenderer::refresh_pose(const R3DViewport* w) {
    if (pose_serial_ != scene_serial_) {
        pose_serial_ = scene_serial_;
        pose_prev_ = pose_now_;
        pose_now_.valid = w != nullptr;
        if (w) {
            memcpy(pose_now_.axes, w->world_axes, sizeof(pose_now_.axes));
            memcpy(pose_now_.camera, w->camera, sizeof(pose_now_.camera));
            float sl = std::sqrt(w->sun[0] * w->sun[0] + w->sun[1] * w->sun[1] + w->sun[2] * w->sun[2]);
            for (int i = 0; i < 3; i++) pose_now_.sun[i] = sl > 0 ? w->sun[i] / sl : 0;
        }
    }
    const WorldPose &a = pose_prev_, &b = pose_now_;
    auto dot = [](const float* p, const float* q) { return p[0] * q[0] + p[1] * q[1] + p[2] * q[2]; };
    float d[3] = {b.camera[0] - a.camera[0], b.camera[1] - a.camera[1], b.camera[2] - a.camera[2]};
    pose_cut_ = !a.valid || !b.valid || dot(d, d) > 10.0f * 10.0f || dot(a.axes + 6, b.axes + 6) < 0.9f ||
                dot(a.axes + 3, b.axes + 3) < 0.9f;
}

R3DRenderer::WorldPose R3DRenderer::world_pose(const R3DViewport* w) {
    refresh_pose(w);
    const WorldPose &a = pose_prev_, &b = pose_now_;
    auto dot = [](const float* p, const float* q) { return p[0] * q[0] + p[1] * q[1] + p[2] * q[2]; };
    if (pose_cut_ || interp_ >= 1.0f) return b;
    WorldPose p = b;
    float t = interp_;
    for (int i = 0; i < 9; i++) p.axes[i] = a.axes[i] + (b.axes[i] - a.axes[i]) * t;
    for (int i = 0; i < 3; i++) p.camera[i] = a.camera[i] + (b.camera[i] - a.camera[i]) * t;
    for (int i = 0; i < 3; i++) p.sun[i] = a.sun[i] + (b.sun[i] - a.sun[i]) * t;
    for (float* v : {p.axes, p.axes + 3, p.axes + 6, p.sun}) {   // back to unit length
        float l = std::sqrt(dot(v, v));
        if (l > 0)
            for (int i = 0; i < 3; i++) v[i] /= l;
    }
    return p;
}

// The sun drawn in the sky and its light shafts (view space, unit length).
// The original's sky is a 2D picture that the game scrolls as the camera
// turns, though not always as far as the 3D world turns: looking up at the
// brachiosaurs in stage 2 the clouds swing from side to side while the
// game's light, high above, hardly moves. So the sun starts where the game's
// light comes from and then goes with the clouds, by the scroll of the row
// it is on; back to the game's light after a cut, where there is no sky
// picture, or once it has gone well outside the picture
void R3DRenderer::step_sky_sun(const WorldPose& pose, const float view[4], float view_w, float out[3]) {
    float light_uv[2];
    const float* L = pose.sun;
    bool in_front = L[2] < -0.05f;
    if (in_front) {
        light_uv[0] = (view[0] * L[0] / -L[2] - view[2]) * 0.5f + 0.5f;
        light_uv[1] = (view[1] * L[1] / -L[2] - view[3]) * 0.5f + 0.5f;
    }
    if (pose_cut_) sky_sun_carried_ = false;
    if (sky_sun_serial_ != sky_motion_serial_) {   // once per 2D frame
        sky_sun_serial_ = sky_motion_serial_;
        sky_sun_rate_[0] = sky_sun_rate_[1] = 0;
        if (sky_sun_carried_) {
            int row = std::clamp(int(sky_sun_uv_[1] * SCREEN_H), 0, SCREEN_H - 1);
            SkyMotion::Row mx = sky_motion_.row(row, 0), my = sky_motion_.row(row, 1);
            if (mx.known) {   // the picture moves left as the scroll goes up
                sky_sun_uv_[0] -= mx.move / view_w;
                sky_sun_uv_[1] -= my.move / float(SCREEN_H);
                sky_sun_rate_[0] = -mx.velocity / view_w;
                sky_sun_rate_[1] = -my.velocity / float(SCREEN_H);
            }
            float outside = std::max({-sky_sun_uv_[0], sky_sun_uv_[0] - 1.0f, -sky_sun_uv_[1], sky_sun_uv_[1] - 1.0f});
            sky_sun_carried_ = mx.known && outside < 0.5f;
        }
        if (!sky_sun_carried_ && in_front) {
            sky_sun_carried_ = true;
            sky_sun_uv_[0] = light_uv[0];
            sky_sun_uv_[1] = light_uv[1];
        }
    }
    if (!sky_sun_carried_ || view[0] == 0 || view[1] == 0) {
        memcpy(out, L, sizeof(float) * 3);
        return;
    }
    // Between 2D frames moved on like the sky (set_back_shift)
    float x = (sky_sun_uv_[0] + sky_sun_rate_[0] * back_shift_) * 2.0f - 1.0f;
    float y = (sky_sun_uv_[1] + sky_sun_rate_[1] * back_shift_) * 2.0f - 1.0f;
    float d[3] = {(x + view[2]) / view[0], (y + view[3]) / view[1], -1.0f};
    float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int i = 0; i < 3; i++) out[i] = d[i] / l;
}

// The weather (r3d_weather.h), once per game frame: the shown one moves
// towards the one asked for over a few seconds, and it shows only outdoors
// in daylight, with a backdrop behind the world (not in the caves, the
// corridors or the night, where the backdrop is black)
void R3DRenderer::step_weather(const R3DScene& scene) {
    const R3DViewport* w = nullptr;
    for (const R3DViewport& v : scene.viewports)
        if (v.world) w = &v;
    float outdoor = w && !back_black_ && w->sun_intensity > 0.15f ? 1.0f : 0.0f;
    outdoor_ += 0.02f * (outdoor - outdoor_);
    const float k = 0.01f;   // about four seconds to change
    R3DWeather& a = weather_;
    const R3DWeather& b = weather_target_;
    auto ease = [&](float& x, float to) { x += k * (to - x); };
    ease(a.sun, b.sun);
    ease(a.ambient, b.ambient);
    ease(a.overcast, b.overcast);
    ease(a.fog, b.fog);
    ease(a.rain, b.rain);
    ease(a.storm, b.storm);
    for (int i = 0; i < 3; i++) ease(a.sky[i], b.sky[i]), ease(a.fog_color[i], b.fog_color[i]);
    rain_.set_forced(a.rain * outdoor_);
}

void R3DRenderer::weather_light(const R3DViewport& v, float& sun, float& ambient, float& fog_density,
                                float fog_color[4]) const {
    sun = v.sun_intensity;
    ambient = v.ambient;
    fog_density = v.fog_density;
    for (int i = 0; i < 3; i++) fog_color[i] = v.fog_color[i];
    fog_color[3] = v.fog_ambient;
    if (!v.world || outdoor_ <= 0.001f) return;
    const R3DWeather& wx = weather_;
    sun *= 1.0f + (wx.sun - 1.0f) * outdoor_;
    ambient *= 1.0f + (wx.ambient - 1.0f) * outdoor_;
    // The weather's fog where it is thicker than the game's, taking its colour
    // as it does (the game's fog colour times its own ambient)
    float fog = wx.fog * outdoor_;
    if (fog > fog_density) {
        float t = fog_density > 0 ? 1.0f - fog_density / fog : 1.0f;
        for (int i = 0; i < 3; i++) fog_color[i] = v.fog_color[i] * v.fog_ambient * (1 - t) + wx.fog_color[i] * t;
        fog_color[3] = 1;
        fog_density = fog;
    }
}

// Atmosphere parameters for this frame (atmosphere.glsl)
void R3DRenderer::update_atmosphere(const ScreenLayout& lay, uint32_t target_w, uint32_t target_h) {
    AtmosphereUniform u{};
    rt_frame_ = rt_ao_ = false;
    u.backdrop[0] = 1.0f / float(target_w);
    u.backdrop[1] = 1.0f / float(target_h);
    // The original scroll fog, laid over the 2D layers behind the 3D image
    const R3DViewport* sf = scene_ && !scene_->viewports.empty() ? scroll_fog_viewport(scene_->viewports) : nullptr;
    if (sf) {
        for (int i = 0; i < 3; i++) u.scroll_fog[i] = sf->fog_color[i] * sf->fog_ambient;
        u.scroll_fog[3] = sf->scroll_fog;
        // Its rectangle in the 3D image, widened like the viewport when it fills the screen
        bool full = sf->x <= 0.5f && sf->w >= SCREEN_W - 0.5f;
        float x0 = full ? 0.0f : sf->x + lay.game_x0, w = full ? lay.view_w : sf->w;
        u.scroll_rect[0] = x0 / lay.view_w;
        u.scroll_rect[1] = sf->y / SCREEN_H;
        u.scroll_rect[2] = (x0 + w) / lay.view_w;
        u.scroll_rect[3] = (sf->y + sf->h) / SCREEN_H;
    }
    const R3DViewport* w = nullptr;
    if (scene_)   // the sun direction is also used by the sky's sun
        for (const R3DViewport& v : scene_->viewports)
            if (v.world) w = &v;
    WorldPose pose = world_pose(w);
    if (w) {
        // Game time (about 57.5 frames per second), so a replay drifts the same way
        float time = (float(scene_->frame % 100000) + interp_) / 57.5f;
        for (int k = 0; k < 3; k++)
            for (int i = 0; i < 3; i++) u.axes[k * 4 + i] = pose.axes[k * 3 + i];
        u.camera[0] = pose.camera[0];
        u.camera[1] = pose.camera[1];
        u.camera[2] = pose.camera[2];
        u.camera[3] = time;
        // The game's light and fog with the weather's
        float w_sun, w_ambient, w_fog, w_fog_color[4];
        weather_light(*w, w_sun, w_ambient, w_fog, w_fog_color);
        const float overcast = weather_.overcast * outdoor_;
        u.weather[0] = overcast;
        for (int i = 0; i < 3; i++) u.weather[1 + i] = weather_.sky[i];
        for (int i = 0; i < 3; i++) u.sun[i] = pose.sun[i];
        u.sun[3] = w_sun;
        static const char* force_sun = getenv("LW_FOGSUN");   // debugging aid: "x,y,z" in view space
        if (force_sun) {
            float f[3] = {0, 0, -1};
            sscanf(force_sun, "%f,%f,%f", &f[0], &f[1], &f[2]);
            float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
            for (int i = 0; i < 3; i++) u.sun[i] = f[i] / fl;
            u.sun[3] = 1;
        }
        // Projection as used for the world viewport (widened when it fills the screen)
        bool full = w->x <= 0.5f && w->w >= SCREEN_W - 0.5f;
        float widen = full ? lay.view_w / SCREEN_W : 1.0f;
        u.view[0] = w->proj[0] / widen;
        u.view[1] = w->proj[5];
        u.view[2] = w->proj[8] / widen;
        u.view[3] = w->proj[9];

        // Distance haze. Horizon colour: a pale haze, tinted by the back
        // layer just above the horizon (where a level view direction meets
        // the screen), which is the sky or the painted far landscape
        float haze[3] = {0.60f, 0.66f, 0.64f};
        float target[3] = {haze[0], haze[1], haze[2]};
        const float* up = pose.axes + 3;
        if (std::fabs(up[1]) > 0.2f && u.view[0] != 0 && u.view[1] != 0) {
            float ndc_y = (up[2] - up[0] * u.view[2] / u.view[0]) * u.view[1] / up[1] - u.view[3];
            int row = int((ndc_y * 0.5f + 0.5f) * SCREEN_H);
            float sum[3] = {0, 0, 0}, n = 0;
            for (int y = std::max(row - 48, 0); y < std::min(row - 4, SCREEN_H); y++) {
                for (int i = 0; i < 3; i++) sum[i] += back_rows_[y * 4 + i];
                n += back_rows_[y * 4 + 3];
            }
            if (n > 500)
                for (int i = 0; i < 3; i++) {
                    float sky = sum[i] / n;
                    sky += (u.scroll_fog[i] - sky) * u.scroll_fog[3];
                    target[i] = 0.3f * sky + 0.7f * haze[i];
                }
        }
        for (int i = 0; i < 3; i++) target[i] += (weather_.sky[i] - target[i]) * overcast;   // a grey sky
        for (int i = 0; i < 3; i++) haze_color_[i] += 0.05f * (target[i] - haze_color_[i]);
        for (int i = 0; i < 3; i++) u.haze[i] = haze_color_[i];
        u.haze[3] = haze_strength_;
        u.range[1] = std::max(w->scenery_far, 50.0f);
        u.range[0] = 0.1f * u.range[1];   // the tint starts past the near scenery
        u.range[2] = 0.6f;          // desaturation
        u.range[3] = back_layer_extended() ? 1.0f : 0.0f;
        u.backdrop[2] = lay.game_x0 / lay.view_w;
        u.backdrop[3] = SCREEN_W / lay.view_w;

        // World up and the camera's height above the ground: what may cast
        // sun shadows (the shadow map, the traced ones)
        for (int i = 0; i < 3; i++) shadow_up_[i] = pose.axes[3 + i];
        shadow_height_ = pose.camera[1] - w->ground;
        // Ray tracing: the ambient occlusion through the world (all of it)
        if (ray_tracing_ >= 2 && rt_.ready() && ao_strength_ > 0) {
            rt_frame_ = rt_ao_ = true;
            rt_world_ = w;
        }
        // Sun shadows: an orthographic view from the sun covering what the
        // camera sees up to SHADOW_RANGE, plus whatever stands between it and the sun
        shadow_frame_ = shadow_strength_ > 0 && u.sun[3] > 0 && u.view[0] != 0 && u.view[1] != 0;
        if (shadow_frame_) {
            const float* L = u.sun;   // towards the sun (view space)
            float sun_up[3] = {0, 1, 0};   // any axis not parallel to the sun
            if (std::fabs(L[1]) > 0.95f) sun_up[0] = 1, sun_up[1] = 0;
            auto cross = [](const float* a, const float* b, float* o) {
                o[0] = a[1] * b[2] - a[2] * b[1];
                o[1] = a[2] * b[0] - a[0] * b[2];
                o[2] = a[0] * b[1] - a[1] * b[0];
            };
            float X[3], Y[3];
            cross(sun_up, L, X);
            float xl = std::sqrt(X[0] * X[0] + X[1] * X[1] + X[2] * X[2]);
            for (float& v : X) v /= xl;
            cross(L, X, Y);
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (float z : {1.0f, SHADOW_RANGE})
                for (float nx : {-1.0f, 1.0f})
                    for (float ny : {-1.0f, 1.0f}) {
                        float p[3] = {z * (nx + u.view[2]) / u.view[0], z * (ny + u.view[3]) / u.view[1], -z};
                        float l[3] = {p[0] * X[0] + p[1] * X[1] + p[2] * X[2], p[0] * Y[0] + p[1] * Y[1] + p[2] * Y[2],
                                      p[0] * L[0] + p[1] * L[1] + p[2] * L[2]};
                        for (int i = 0; i < 3; i++) lo[i] = std::min(lo[i], l[i]), hi[i] = std::max(hi[i], l[i]);
                    }
            // Square, on whole texels, so the shadows do not crawl as the camera turns
            float size = std::max(hi[0] - lo[0], hi[1] - lo[1]);
            float texel = size / float(SHADOW_SIZE);
            float cx = std::floor((lo[0] + hi[0]) * 0.5f / texel) * texel, cy = std::floor((lo[1] + hi[1]) * 0.5f / texel) * texel;
            float zmin = lo[2], zmax = hi[2] + 400.0f, zr = zmax - zmin;
            float* m = shadow_matrix_;   // column-major
            for (int i = 0; i < 3; i++) {
                m[i * 4 + 0] = X[i] * 2.0f / size;
                m[i * 4 + 1] = Y[i] * 2.0f / size;
                m[i * 4 + 2] = L[i] / zr;
                m[i * 4 + 3] = 0;
            }
            m[12] = -cx * 2.0f / size;
            m[13] = -cy * 2.0f / size;
            m[14] = -zmin / zr;
            m[15] = 1;
            memcpy(u.shadow_matrix, m, sizeof(u.shadow_matrix));
            u.shadow[0] = std::min(shadow_strength_, 1.0f) * 0.75f * (1.0f + (std::min(weather_.sun, 1.0f) - 1.0f) * outdoor_);
            u.shadow[1] = 1.0f / float(SHADOW_SIZE);
            u.shadow[2] = 2.5f * texel;   // normal offset
            u.shadow[3] = 1.5f * texel / zr;   // depth bias
            // Ray tracing: the same shadows traced through the world instead (no map drawn)
            if (ray_tracing_ >= 1 && rt_.ready()) {
                rt_frame_ = true;
                rt_world_ = w;
                static const bool split = getenv("LW_RTSPLIT");   // debugging aid: traced left, shadow map right
                shadow_frame_ = split;
                u.rt[0] = split ? 2.0f : 1.0f;
                u.rt[1] = 0.012f;   // a little more than the real sun's 0.27 degrees: softer edges
                u.rt[2] = 4;
                // With DLAA the spread of the rays turns every image, and DLAA averages them
                float golden = float(jitter_index_) * 0.618034f;
                u.rt[3] = dlaa_.ready() ? golden - std::floor(golden) : 0.0f;
            }
        }
        // The sun in the 3D image, for the light shafts
        step_sky_sun(pose, u.view, lay.view_w, u.sky_sun);
        if (force_sun) memcpy(u.sky_sun, u.sun, sizeof(float) * 3);
        const float* S = u.sky_sun;
        sun_light_ = 0;
        if (S[2] < -0.05f && u.sun[3] > 0) {
            sun_uv_[0] = (u.view[0] * S[0] / -S[2] - u.view[2]) * 0.5f + 0.5f;
            sun_uv_[1] = (u.view[1] * S[1] / -S[2] - u.view[3]) * 0.5f + 0.5f;
            // Fades out as it goes far outside the picture
            float out = std::max({-sun_uv_[0], sun_uv_[0] - 1.0f, -sun_uv_[1], sun_uv_[1] - 1.0f, 0.0f});
            sun_light_ = u.sun[3] * std::clamp(1.0f - out / 0.6f, 0.0f, 1.0f);
        }
        u.soft[0] = soft_particles_ ? 1.0f : 0.0f;
        u.soft[1] = 2.0f;
        // Light in the game's fog, with the distance haze: strength, brighter
        // upwards, drifting banks
        if (w_fog > 0) {
            u.mist[0] = std::min(haze_strength_, 1.0f);
            u.mist[1] = 0.45f;
            u.mist[2] = 0.12f;
            static const char* tune = getenv("LW_MIST");   // debugging aid: "upwards,banks"
            if (tune) sscanf(tune, "%f,%f", &u.mist[1], &u.mist[2]);
        }
        // Lightning in a dark grey fog (the rainy forest of the T-Rex): the
        // fog lights up for a moment, so the trees stand out against it
        float fc[3], lo = 1, hi = 0;
        for (int i = 0; i < 3; i++) {
            fc[i] = w_fog_color[i] * w_fog_color[3];
            lo = std::min(lo, fc[i]);
            hi = std::max(hi, fc[i]);
        }
        float luma = 0.2126f * fc[0] + 0.7152f * fc[1] + 0.0722f * fc[2];
        LightningStrike strike;
        // A storm: the game's rain (the whole T-Rex stretch, the cave and the
        // jeep included), its dark grey fog, or the weather's storm
        bool grey_fog = w_fog > 0.003f && luma > 0.05f && luma < 0.4f && hi - lo < 0.08f;
        bool storm = weather_.storm * outdoor_ > 0.5f;
        if (lightning_ && (grey_fog || storm || rain_.game_rain() > 0.5f)) strike = lightning_strike(time);
        static const char* force_flash = getenv("LW_FLASH");   // debugging aid: fixed brightness, channel shown
        if (force_flash) strike.flash = float(atof(force_flash)), strike.bolt = 1, strike.id = 1;
        u.mist[3] = strike.flash;
        if (strike.id >= 0 && strike.flash > 0.005f) {
            // Where it strikes: picked when it begins, a little to either side
            // of where the camera looks then, and kept for the whole strike
            if (strike.id != bolt_id_ || force_flash) {   // (LW_FLASH: kept where the camera looks)
                if (strike.id != bolt_id_) strikes_++;   // its thunder (strikes)
                bolt_id_ = strike.id;
                const float* a = w->world_axes;
                float yaw = std::atan2(-a[8], -a[2]);   // the view direction in the world frame
                bolt_azimuth_ = yaw + (lightning_hash(uint32_t(strike.id) * 31u + 5u) - 0.5f) * 1.6f;
                if (force_flash) bolt_azimuth_ = yaw;   // in the middle of the picture, to judge it
                bolt_main_ = lightning_channel(strike.id, bolt_seg_, bolt_count_);
            }
            // The channel only where the fog or the weather's grey sky hides the backdrop (painted foliage elsewhere)
            u.bolt[0] = grey_fog || storm || force_flash ? strike.bolt : 0.0f;
            u.bolt[1] = float(bolt_count_);
            u.bolt[2] = bolt_azimuth_;
            u.bolt[3] = float(bolt_main_);
            memcpy(u.bolt_seg, bolt_seg_, sizeof(u.bolt_seg));
            // Its light comes from the clouds above it
            float el = 0.6f;
            float d[3] = {std::cos(el) * std::cos(bolt_azimuth_), std::sin(el), std::cos(el) * std::sin(bolt_azimuth_)};
            for (int i = 0; i < 3; i++)
                u.bolt_light[i] = w->world_axes[i] * d[0] + w->world_axes[3 + i] * d[1] + w->world_axes[6 + i] * d[2];
        }
        u.rain[0] = rain_.amount();
        R3DRain::wind(time, u.wind);
    } else {
        shadow_frame_ = false;
        sun_light_ = 0;
    }
    u.soft[2] = hdr_ ? 1.0f : 0.0f;
    // SUN: more contrast in clear outdoor scenes (sunlit, little of the game's
    // own fog); the fogged, dark and indoor scenes keep the game's light
    if (w && sun_ && u.sun[3] > 0.3f) {
        float fogged = std::clamp(w->scroll_fog * 2.0f + std::max(w->fog_density, weather_.fog * outdoor_) * 300.0f +
                                  weather_.overcast * outdoor_, 0.0f, 1.0f);
        float k = 1.0f - fogged;
        u.sunlight[0] = 1.0f + 0.45f * k;
        u.sunlight[1] = 1.0f - 0.4f * k;
        u.sunlight[2] = k;
    }
    if (w && rim_) u.sunlight[3] = 0.6f;
    if (relief_) {
        u.relief[0] = 2.5f;
        u.relief[1] = 120.0f;   // detail only where it can be seen (it costs two more texture reads)
    }
    // The world viewport's spotlight (spec 7.1), applied to the game world
    if (w && w->spot_radius[0] > 0 && w->spot_radius[1] > 0) {
        u.spot[0] = w->spot_center[0];
        u.spot[1] = w->spot_center[1];
        u.spot[2] = 1.0f / w->spot_radius[0];
        u.spot[3] = 1.0f / w->spot_radius[1];
        for (int i = 0; i < 3; i++) {
            u.spot_color[i] = w->spot_color[i];
            u.spot_fog[i] = w->spot_fog_color[i] * w->fog_attenuation * w->fog_color[i];
        }
        u.spot_color[3] = w->spot_start;
        u.spot_fog[3] = w->spot_extent;
    }
    // Night (SKY: PROCEDURAL): a dim world on a black backdrop gets a night sky
    // It comes and goes gradually, so a frame or two of a different backdrop
    // (a fade, a flash) cannot make it blink
    bool night = w && night_sky_ && back_black_ && w->sun_intensity < 0.25f && w->ambient < 0.25f;
    night_level_ += 0.06f * ((night ? 1.0f : 0.0f) - night_level_);
    if (!night_sky_) night_level_ = 0;
    u.night[0] = night_level_;
    u.night[1] = modern_torch_ ? 1.0f : 0.0f;
    memcpy(atmosphere_uniform_.mapped, &u, sizeof(u));
}

// Texture RAM rows, 2D layers and the scene's vertices changed since the last frame
void R3DRenderer::record_uploads(const Frame& f) {
    VkCommandBuffer cmd = f.cmd;
    const size_t vcount = f.vcount;
    // Uploads
    if (tex_y0_ < tex_y1_) {
        transition(cmd, texture_ram_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy c{};
        c.bufferOffset = VkDeviceSize(tex_y0_) * 2048 * 2;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageOffset = {0, tex_y0_, 0};
        c.imageExtent = {2048, uint32_t(tex_y1_ - tex_y0_), 1};
        vkCmdCopyBufferToImage(cmd, texture_staging_.buffer, texture_ram_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        transition(cmd, texture_ram_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        tex_y0_ = tex_y1_ = 0;
    }
    update_sky_motion();
    if (layers_dirty_) {
        Image* imgs[2] = {&back_, &front_};
        for (int i = 0; i < 2; i++) {
            transition(cmd, *imgs[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy c{};
            c.bufferOffset = VkDeviceSize(i) * BACK_W * SCREEN_H * 4;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = {imgs[i]->w, SCREEN_H, 1};
            vkCmdCopyBufferToImage(cmd, layer_staging_.buffer, imgs[i]->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            transition(cmd, *imgs[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        transition(cmd, text_cells_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy c{};
        c.bufferOffset = VkDeviceSize(BACK_W + SCREEN_W) * SCREEN_H * 4;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {text_cells_.w, text_cells_.h, 1};
        vkCmdCopyBufferToImage(cmd, layer_staging_.buffer, text_cells_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        transition(cmd, text_cells_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transition(cmd, sky_coords_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        c.bufferOffset += TEXT_CELLS_BYTES;
        c.imageExtent = {sky_coords_.w, sky_coords_.h, 1};
        vkCmdCopyBufferToImage(cmd, layer_staging_.buffer, sky_coords_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        transition(cmd, sky_coords_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        layers_dirty_ = false;
    }
    size_t total = vcount + rain_vertices_.size();
    if (total * sizeof(R3DVertex) > vertices_.size) {
        vkDeviceWaitIdle(dev_);
        destroy_buffer(vertices_);
        create_buffer(vertices_, total * sizeof(R3DVertex) * 2, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        uploaded_serial_ = ~0ull;   // the new buffer has nothing in it yet
    }
    R3DVertex* mapped = static_cast<R3DVertex*>(vertices_.mapped);
    // The scene's vertices change once per game frame, while images go out
    // at the monitor's rate: they are copied again only for a new scene (the
    // previous image's GPU work is over, so the buffer still holds them)
    bool fresh = uploaded_serial_ != scene_serial_;
    if (fresh && vcount) memcpy(mapped, scene_->vertices.data(), vcount * sizeof(R3DVertex));
    uploaded_serial_ = scene_serial_;
    if (!rain_vertices_.empty())
        memcpy(mapped + vcount, rain_vertices_.data(), rain_vertices_.size() * sizeof(R3DVertex));
    // The game's own rain fades out as ours comes in. Its colours come from the
    // scene (the buffer is write-combined memory, slow to read back), and are
    // only rewritten when the fade changes (RAIN can change in the menu)
    float keep = 1.0f - rain_.amount();
    if (vcount && (fresh ? keep < 1.0f : keep != uploaded_keep_))
        for (uint32_t first : rain_.game_streaks())
            for (uint32_t k = first; k < first + 3 && k < vcount; k++) {
                uint32_t color = scene_->vertices[k].color;
                uint32_t a = uint32_t(float(color >> 24) * keep);
                mapped[k].color = (color & 0xFFFFFFu) | (a << 24);
            }
    uploaded_keep_ = keep;
}

// Ray tracing: this image's world into the acceleration structure the 3D pass
// traces the sun shadows through. Before anything binds the 3D pass's
// descriptors, which may have to point at a new cut-out buffer
void R3DRenderer::record_ray_tracing(const Frame& f) {
    if (!rt_frame_ || !rt_world_) return;
    // Nothing traces what is higher up any more: the ambient occlusion leaves
    // the canopies out too (ao_common.glsl, AO_HIGH)
    rt_.prepare(*scene_, *rt_world_, interp_, shadow_up_, shadow_height_, 35.0f, false);
    if (rt_.generation() != rt_generation_) {
        rt_generation_ = rt_.generation();
        VkDescriptorBufferInfo cut{rt_.cutouts(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = r3d_set_;
        w.dstBinding = 7;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &cut;
        vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
    }
    rt_.record(f.cmd);
}

// The procedural sky's clouds into their panorama (sky_fields.frag): a strip
// of SKY_FIELDS_ROWS rows per frame, the whole of it the first time. The
// clouds drift about a layer pixel every three seconds, so the strips drawn
// in turn over the panorama's 32 frames differ by far less than a pixel
void R3DRenderer::record_sky_fields(const Frame& f) {
    VkCommandBuffer cmd = f.cmd;
    uint32_t y0 = sky_fields_all_ ? 0 : sky_fields_row_;
    uint32_t rows = sky_fields_all_ ? sky_fields_.h : SKY_FIELDS_ROWS;
    sky_fields_all_ = false;
    sky_fields_row_ = (y0 + rows) % sky_fields_.h;
    transition(cmd, sky_fields_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    a.imageView = sky_fields_.view;
    a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // the other strips stay
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, int32_t(y0)}, {sky_fields_.w, rows}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &a;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_fields_pipe_);
    VkViewport whole{0, 0, float(sky_fields_.w), float(sky_fields_.h), 0, 1};   // uv over the whole panorama
    vkCmdSetViewport(cmd, 0, 1, &whole);
    vkCmdSetScissor(cmd, 0, 1, &ri.renderArea);
    float sp[4] = {launch_seconds() * 0.005f, 0, 0, 0};   // the drift, as compose.frag had it
    vkCmdPushConstants(cmd, sky_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(sp), sp);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    transition(cmd, sky_fields_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void R3DRenderer::record_shadows(const Frame& f) {
    VkCommandBuffer cmd = f.cmd;
    const size_t vcount = f.vcount;
    const float before = f.before;
    // Sun shadows: the opaque game world drawn from the sun, depth only
    if (shadow_frame_ && vcount) {
        transition(cmd, shadow_map_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        VkRenderingAttachmentInfo none{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};   // no colour image
        VkRenderingAttachmentInfo sd{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        sd.imageView = shadow_map_.view;
        sd.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        sd.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        sd.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        sd.clearValue.depthStencil = {0.0f, 0};
        VkRenderingInfo sr{VK_STRUCTURE_TYPE_RENDERING_INFO};
        sr.renderArea = {{0, 0}, {SHADOW_SIZE, SHADOW_SIZE}};
        sr.layerCount = 1;
        sr.colorAttachmentCount = 1;
        sr.pColorAttachments = &none;
        sr.pDepthAttachment = &sd;
        vkCmdBeginRendering(cmd, &sr);
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertices_.buffer, &zero);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r3d_layout_, 0, 1, &r3d_set_, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_);
        VkViewport sv{0, 0, float(SHADOW_SIZE), float(SHADOW_SIZE), 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &sv);
        VkRect2D ss{{0, 0}, {SHADOW_SIZE, SHADOW_SIZE}};
        vkCmdSetScissor(cmd, 0, 1, &ss);
        for (const R3DViewport& v : scene_->viewports) {
            if (!v.world || !v.opaque_count) continue;
            ViewportPush pc{};
            memcpy(pc.proj, shadow_matrix_, 64);
            pc.params[3] = before;
            pc.quality[0] = 1.0f;   // opaque rules (alpha test), no MSAA coverage
            pc.quality[1] = aniso_;
            pc.quality[3] = interp_;   // sun.w = 0: no lighting, so no shadow lookup while drawing it
            // Shadow pass marker, and only casters up to this height above the ground
            for (int i = 0; i < 3; i++) pc.sun[i] = shadow_up_[i];
            pc.fog_color[0] = shadow_height_;
            pc.fog_color[1] = 35.0f;
            pc.fog_color[3] = -1.0f;
            vkCmdPushConstants(cmd, r3d_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(pc), &pc);
            vkCmdDraw(cmd, v.opaque_count, 1, v.opaque_first, 0);
        }
        vkCmdEndRendering(cmd);
        transition(cmd, shadow_map_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    }
}

// The game world and HUD into color_ (and glow_, motion_, depth_)
void R3DRenderer::record_world(const Frame& f) {
    VkCommandBuffer cmd = f.cmd;
    const ScreenLayout& lay = f.lay;
    const uint32_t out_w = f.out_w, out_h = f.out_h;
    const size_t vcount = f.vcount;
    const float before = f.before, jitter_x = f.jitter_x, jitter_y = f.jitter_y, alpha_seed = f.alpha_seed;
    // 3D pass
    bool msaa = samples_ != VK_SAMPLE_COUNT_1_BIT;
    color_.layout = VK_IMAGE_LAYOUT_UNDEFINED;   // contents are cleared every frame
    color_ms_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    motion_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    transition(cmd, color_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (msaa) transition(cmd, color_ms_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (motion_pass_) transition(cmd, motion_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (glow_pass_) {
        glow_.layout = glow_ms_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        transition(cmd, glow_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if (msaa) transition(cmd, glow_ms_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    transition(cmd, depth_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkRenderingAttachmentInfo cas[3] = {{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},
                                        {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},
                                        {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
    VkRenderingAttachmentInfo& ca = cas[0];
    cas[1].imageView = motion_pass_ ? motion_.view : VK_NULL_HANDLE;
    cas[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    cas[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    cas[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.imageView = msaa ? color_ms_.view : color_.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    // The ambient occlusion splits the 3D pass in two: the first part must keep everything
    bool ao_on = ao_strength_ > 0 && has_world_;
    // Soft particles need the world's depth too, so they split it as well
    bool split = ao_on || (soft_particles_ && has_world_);
    ca.storeOp = msaa && !split ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    if (msaa) {   // samples are averaged into the single-sample image at the end of the pass
        ca.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        ca.resolveImageView = color_.view;
        ca.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    ca.clearValue.color = {{0, 0, 0, 0}};
    cas[2] = ca;   // bloom source, cleared to no glow (an attachment only with the bloom on)
    cas[2].imageView = msaa ? glow_ms_.view : glow_.view;
    if (msaa) cas[2].resolveImageView = glow_.view;
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView = depth_.view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = motion_pass_ || split ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    if (split && msaa) {   // sample 0 of the depth, readable without multisampling
        transition(cmd, depth_resolved_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        da.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        da.resolveImageView = depth_resolved_.view;
        da.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }
    da.clearValue.depthStencil = {0.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {out_w, out_h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = glow_pass_ ? 3 : motion_pass_ ? 2 : 1;   // as the pipelines (make_pipeline)
    ri.pColorAttachments = cas;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);

    if (vcount) {
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertices_.buffer, &zero);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r3d_layout_, 0, 1, &r3d_set_, 0, nullptr);
        float sx = float(out_w) / lay.view_w, sy = float(out_h) / SCREEN_H;
        float widen = lay.view_w / SCREEN_W;
        const auto& vps = scene_->viewports;
        size_t i = 0;
        bool first_priority = true;
        while (i < vps.size()) {
            size_t j = i;
            while (j < vps.size() && vps[j].priority == vps[i].priority) j++;
            if (!first_priority) {   // each priority starts with a cleared depth buffer
                VkClearAttachment clr{VK_IMAGE_ASPECT_DEPTH_BIT, 0, {}};
                clr.clearValue.depthStencil = {0.0f, 0};
                VkClearRect rect{{{0, 0}, {out_w, out_h}}, 0, 1};
                vkCmdClearAttachments(cmd, 1, &clr, 1, &rect);
            }
            first_priority = false;
            bool world_here = false;
            for (size_t k = i; k < j; k++) world_here |= vps[k].world;
            for (int pass = 0; pass < 2; pass++) {
                // Between the opaque and the translucent surfaces of the world:
                // the ambient occlusion, from its depth (the HUD clears it later)
                if (pass == 1 && split && world_here) {
                    const R3DViewport* w = nullptr;
                    for (size_t k = i; k < j; k++)
                        if (vps[k].world) w = &vps[k];
                    vkCmdEndRendering(cmd);
                    gpu_time_mark(cmd, 4);
                    record_world_depth(f, *w, msaa, ao_on);
                    gpu_time_mark(cmd, 5);
                    // The 3D pass goes on where it was; the world depth stays
                    // readable (no resolve into it any more this frame)
                    VkRenderingAttachmentInfo again[3] = {cas[0], cas[1], cas[2]};
                    VkRenderingAttachmentInfo dagain = da;
                    for (auto& a : again) a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                    dagain.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                    dagain.resolveMode = VK_RESOLVE_MODE_NONE;
                    dagain.resolveImageView = VK_NULL_HANDLE;
                    VkRenderingInfo rag = ri;
                    rag.pColorAttachments = again;
                    rag.pDepthAttachment = &dagain;
                    vkCmdBeginRendering(cmd, &rag);
                    vkCmdBindVertexBuffers(cmd, 0, 1, &vertices_.buffer, &zero);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r3d_layout_, 0, 1, &r3d_set_, 0,
                                            nullptr);
                }
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass == 0 ? opaque_ : translucent_);
                for (size_t k = i; k < j; k++) {
                    const R3DViewport& v = vps[k];
                    uint32_t count = pass == 0 ? v.opaque_count : v.trans_count;
                    bool rain_here = pass == 1 && v.world && !rain_vertices_.empty();
                    if (!count && !rain_here) continue;
                    // Full-screen viewports are widened (wider horizontal field of
                    // view, same vertical one); smaller ones keep their 4:3 place
                    bool full = v.x <= 0.5f && v.w >= SCREEN_W - 0.5f;
                    float vx = full ? 0.0f : v.x + lay.game_x0, vw = full ? lay.view_w : v.w;
                    VkViewport viewport{vx * sx + jitter_x, v.y * sy + jitter_y, vw * sx, v.h * sy, 0.0f, 1.0f};
                    vkCmdSetViewport(cmd, 0, 1, &viewport);
                    int32_t x0 = std::max(0, int32_t(vx * sx)), y0 = std::max(0, int32_t(v.y * sy));
                    VkRect2D sc{{x0, y0},
                                {std::min(out_w - uint32_t(x0), uint32_t(vw * sx + 0.5f)),
                                 std::min(out_h - uint32_t(y0), uint32_t(v.h * sy + 0.5f))}};
                    vkCmdSetScissor(cmd, 0, 1, &sc);
                    ViewportPush pc{};
                    memcpy(pc.proj, v.proj, 64);
                    if (full) {
                        pc.proj[0] /= widen;
                        pc.proj[8] /= widen;
                    }
                    pc.sun[0] = v.sun[0];
                    pc.sun[1] = v.sun[1];
                    pc.sun[2] = v.sun[2];
                    // The --light-gain knob: more light on the sun and on the ambient. It lands here
                    // rather than in the push block because that block is exactly 128 bytes,
                    // the guaranteed minimum, and there is no room left in it.
                    float sun, ambient, fog_density;
                    weather_light(v, sun, ambient, fog_density, pc.fog_color);
                    pc.sun[3] = sun * light_gain_;
                    pc.params[0] = ambient * light_gain_;
                    pc.params[1] = fog_density;
                    pc.params[2] = v.fog_start;
                    pc.params[3] = before;
                    pc.quality[0] = pass == 0 ? float(samples_) + alpha_seed : -float(samples_);
                    pc.quality[1] = aniso_;
                    pc.quality[2] = full ? 2.0f * lay.game_x0 / lay.view_w : 0.0f;
                    pc.quality[3] = interp_;
                    // Translucent surfaces that are not effects (a whole model made
                    // see-through, like the Carnotaurus' camouflage): only the
                    // nearest one shows at each pixel, as on the Real3D (spec 7.2),
                    // so their depth goes first. Fire, smoke and other texture-alpha
                    // or luminous effects keep blending in list order.
                    if (pass == 1) {
                        ViewportPush pd = pc;
                        pd.quality[0] = -1000.0f;
                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, trans_depth_);
                        vkCmdPushConstants(cmd, r3d_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                           0, sizeof(pd), &pd);
                        vkCmdDraw(cmd, count, 1, v.trans_first, 0);
                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, translucent_);
                    }
                    vkCmdPushConstants(cmd, r3d_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                       sizeof(pc), &pc);
                    vkCmdDraw(cmd, count, 1, pass == 0 ? v.opaque_first : v.trans_first, 0);
                    // Our rain falls among the world's see-through surfaces
                    if (rain_here) vkCmdDraw(cmd, uint32_t(rain_vertices_.size()), 1, uint32_t(vcount), 0);
                }
            }
            i = j;
        }
    }
    vkCmdEndRendering(cmd);
    transition(cmd, color_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (glow_pass_) transition(cmd, glow_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// Between the opaque and the translucent surfaces of the world: its depth
// made readable (soft particles, ambient occlusion) and, with ao_on, the
// ambient occlusion computed from it and multiplied into the colour so far.
// Called with no rendering in progress; the caller resumes the 3D pass.
void R3DRenderer::record_world_depth(const Frame& f, const R3DViewport& w, bool msaa, bool ao_on) {
    VkCommandBuffer cmd = f.cmd;
    const ScreenLayout& lay = f.lay;
    const uint32_t out_w = f.out_w, out_h = f.out_h;
    const float jitter_x = f.jitter_x, jitter_y = f.jitter_y, alpha_seed = f.alpha_seed;
    float sx = float(out_w) / lay.view_w, sy = float(out_h) / SCREEN_H;
    float widen = lay.view_w / SCREEN_W;
    if (!msaa) {   // a copy of the depth (with MSAA its sample 0 was resolved)
        transition(cmd, depth_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        depth_resolved_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        transition(cmd, depth_resolved_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        copy.extent = {out_w, out_h, 1};
        vkCmdCopyImage(cmd, depth_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depth_resolved_.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        transition(cmd, depth_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    Image& dsrc = depth_resolved_;
    transition(cmd, dsrc, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (ao_on) {
    // The occlusion fades with the fog actually drawn, the weather's included
    float ao_sun, ao_ambient, ao_fog, ao_fog_color[4];
    weather_light(w, ao_sun, ao_ambient, ao_fog, ao_fog_color);
    bool full = w.x <= 0.5f && w.w >= SCREEN_W - 0.5f;
    float vx = full ? 0.0f : w.x + lay.game_x0, vw = full ? lay.view_w : w.w;
    static const bool rt_split = getenv("LW_RTSPLIT");   // debugging aid, as in the shadow pass
    float ap[20] = {w.proj[0] / (full ? widen : 1.0f), w.proj[5], w.proj[8] / (full ? widen : 1.0f),
                    w.proj[9], vx * sx + jitter_x, w.y * sy + jitter_y, vw * sx, w.h * sy,
                    12.0f, std::min(ao_strength_, 1.0f) * 0.85f, ao_fog, w.fog_start,
                    alpha_seed, 0.12f * float(out_h), rt_ao_ ? 6.0f : 0.0f, rt_split ? 1.0f : 0.0f,
                    shadow_up_[0], shadow_up_[1], shadow_up_[2], shadow_height_};
    auto fullscreen = [&](VkPipeline pipe, Image& target, VkImageView view, uint32_t tw, uint32_t th) {
        VkRenderingAttachmentInfo at{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        at.imageView = view;
        at.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        at.loadOp = &target == &ao_ ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
        at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ar{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ar.renderArea = {{0, 0}, {tw, th}};
        ar.layerCount = 1;
        ar.colorAttachmentCount = 1;
        ar.pColorAttachments = &at;
        vkCmdBeginRendering(cmd, &ar);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ao_layout_, 0, 1, &ao_set_, 0,
                                nullptr);
        if (rt_.ready())   // the traced ambient occlusion's rays (the layout always has the set)
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ao_layout_, 1, 1, &r3d_set_, 0, nullptr);
        VkViewport fv{0, 0, float(tw), float(th), 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &fv);
        VkRect2D fr{{0, 0}, {tw, th}};
        vkCmdSetScissor(cmd, 0, 1, &fr);
        vkCmdPushConstants(cmd, ao_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ap), ap);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
    };
    ao_.layout = VK_IMAGE_LAYOUT_UNDEFINED;   // fully rewritten
    transition(cmd, ao_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    fullscreen(ao_pass_, ao_, ao_.view, ao_.w, ao_.h);
    transition(cmd, ao_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // Multiplied into the colour as drawn so far (the opaque world)
    Image& ctarget = msaa ? color_ms_ : color_;
    fullscreen(ao_apply_, ctarget, ctarget.view, out_w, out_h);
    }
}

void R3DRenderer::record_dlaa(const Frame& f) {
    VkCommandBuffer cmd = f.cmd;
    const uint32_t out_w = f.out_w, out_h = f.out_h;
    const float jitter_x = f.jitter_x, jitter_y = f.jitter_y;
    // DLAA: colour, depth and motion vectors in, anti-aliased image out
    if (dlaa_.ready()) {
        transition(cmd, depth_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        transition(cmd, motion_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        dlaa_out_.layout = VK_IMAGE_LAYOUT_UNDEFINED;   // fully rewritten
        transition(cmd, dlaa_out_, VK_IMAGE_LAYOUT_GENERAL);
        bool ok = dlaa_.evaluate(cmd, {color_.image, color_.view, VK_FORMAT_R16G16B16A16_SFLOAT},
                                 {depth_.image, depth_.view, VK_FORMAT_D32_SFLOAT},
                                 {motion_.image, motion_.view, VK_FORMAT_R16G16_SFLOAT},
                                 {dlaa_out_.image, dlaa_out_.view, VK_FORMAT_R16G16B16A16_SFLOAT}, out_w, out_h, jitter_x,
                                 jitter_y, false);
        // Its alpha separately: where the 3D image is partly transparent the
        // 2D background shows through, and a jittered alpha would make every
        // edge against it shimmer (the ammo counter over the sky)
        dlaa_alpha_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        transition(cmd, dlaa_alpha_, VK_IMAGE_LAYOUT_GENERAL);
        ok = ok && dlaa_.evaluate(cmd, {color_.image, color_.alpha_view, VK_FORMAT_R16G16B16A16_SFLOAT},
                                  {depth_.image, depth_.view, VK_FORMAT_D32_SFLOAT},
                                  {motion_.image, motion_.view, VK_FORMAT_R16G16_SFLOAT},
                                  {dlaa_alpha_.image, dlaa_alpha_.view, VK_FORMAT_R8G8B8A8_UNORM}, out_w, out_h,
                                  jitter_x, jitter_y, false, 1);
        transition(cmd, dlaa_out_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transition(cmd, dlaa_alpha_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!ok) bind_scene_image(color_.view, color_.alpha_view);   // stopped working: plain image from now on
    }
}

// The composition's push constants for this frame
R3DRenderer::ComposePush R3DRenderer::compose_push(const Frame& f) const {
    static_assert(sizeof(ComposePush) == 128, "compose.frag push constant block");
    const VkExtent2D extent = f.extent;
    const ScreenLayout& lay = f.lay;
    float W = float(extent.width), H = float(extent.height);
    bool extend_back = back_layer_extended();
    float seconds = launch_seconds();
    ComposePush pc{.scale = {lay.view_w * lay.scale / W, SCREEN_H * lay.scale / H},
                   .offset = {lay.ox / W, lay.oy / H},
                   .crosshair = {-1, -1},
                   .window = {W, H},
                   .layer2d = {lay.game_x0 / lay.view_w, SCREEN_W / lay.view_w},
                   .filter2d = {smooth_2d_ ? 1.0f : 0.0f, lay.scale},
                   .hud = {hud_edges_ && hud_band_ ? lay.game_x0 : 0.0f, hdr_ ? 1.0f : 0.0f, overlay_brightness_,
                           extend_back ? 1.0f : 0.0f},
                   .sky = {seconds, 0, sun_ && weather_.overcast * outdoor_ < 0.5f ? 1.0f : 0.0f, 0},
                   .effects = {bloom_strength_, saturation_, gamma_, 0},
                   .motion = {back_shift_, (crosshair_picture_[0] ? 1.0f : 0.0f) + (crosshair_picture_[1] ? 2.0f : 0.0f), 0, 0},
                   .crosshair2 = {-1, -1, sharpen_, 0}};
    place_crosshairs(lay, pc);
    return pc;
}

void R3DRenderer::place_crosshairs(const ScreenLayout& lay, ComposePush& pc) const {
    float* at[2] = {pc.crosshair, pc.crosshair2};
    for (int p = 0; p < 2; p++) {
        at[p][0] = at[p][1] = -1;
        if (crosshair_visible_[p]) {   // game-screen fraction -> window pixels
            at[p][0] = lay.ox + (lay.game_x0 + crosshair_x_[p] * SCREEN_W) * lay.scale;
            at[p][1] = lay.oy + crosshair_y_[p] * SCREEN_H * lay.scale;
        }
    }
}

// The reduced passes (glow, light shaft mask, diffusion) see the whole 3D
// view in their own target, without crosshair or sky glow
R3DRenderer::ComposePush R3DRenderer::reduced(const ComposePush& pc, const Image& to) {
    ComposePush p = pc;
    p.scale[0] = p.scale[1] = 1;
    p.offset[0] = p.offset[1] = 0;
    p.crosshair[0] = p.crosshair[1] = p.crosshair2[0] = p.crosshair2[1] = -1;
    p.filter2d[1] = float(to.h) / SCREEN_H;
    p.sky[1] = 0;
    return p;
}

// Starts rendering into a quarter-size image (light shafts, diffusion)
void R3DRenderer::begin_quarter(VkCommandBuffer cmd, Image& to, VkAttachmentLoadOp load) {
    to.layout = VK_IMAGE_LAYOUT_UNDEFINED;   // fully rewritten
    transition(cmd, to, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ra{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ra.imageView = to.view;
    ra.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ra.loadOp = load;
    ra.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri2{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri2.renderArea = {{0, 0}, {to.w, to.h}};
    ri2.layerCount = 1;
    ri2.colorAttachmentCount = 1;
    ri2.pColorAttachments = &ra;
    vkCmdBeginRendering(cmd, &ri2);
    VkViewport rv{0, 0, float(to.w), float(to.h), 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &rv);
    VkRect2D rr{{0, 0}, {to.w, to.h}};
    vkCmdSetScissor(cmd, 0, 1, &rr);
}

void R3DRenderer::record_bloom(const Frame& f, const ComposePush& pc) {
    VkCommandBuffer cmd = f.cmd;
    // Bloom: the glow reduced to a quarter of the size, then blurred across and down
    if (glow_pass_) {
        struct Step {
            int source;   // bloom_sets_ index
            Image* from;
            Image* to;
            float mode, spread;
        } steps[5] = {{0, &glow_, &bloom_a_, 0, 0},
                      {1, &bloom_a_, &bloom_b_, 1, 2.0f},
                      {2, &bloom_b_, &bloom_a_, 2, 2.0f},
                      // A second, wider round: the glow spreads across the screen
                      {1, &bloom_a_, &bloom_b_, 1, 5.0f},
                      {2, &bloom_b_, &bloom_a_, 2, 5.0f}};
        // The sky glows less than fire: it is large and only its brightest part
        // counts. Only behind the game world: title and menu screens are 2D pictures
        float sky_glow = has_world_ ? 0.6f : 0.0f;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloom_);
        for (const Step& st : steps) {
            st.to->layout = VK_IMAGE_LAYOUT_UNDEFINED;   // fully rewritten
            transition(cmd, *st.to, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            VkRenderingAttachmentInfo ba{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            ba.imageView = st.to->view;
            ba.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ba.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            ba.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo bi{VK_STRUCTURE_TYPE_RENDERING_INFO};
            bi.renderArea = {{0, 0}, {st.to->w, st.to->h}};
            bi.layerCount = 1;
            bi.colorAttachmentCount = 1;
            bi.pColorAttachments = &ba;
            vkCmdBeginRendering(cmd, &bi);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloom_layout_, 0, 1, &bloom_sets_[st.source], 0,
                                    nullptr);
            VkViewport bv{0, 0, float(st.to->w), float(st.to->h), 0, 1};
            vkCmdSetViewport(cmd, 0, 1, &bv);
            VkRect2D br{{0, 0}, {st.to->w, st.to->h}};
            vkCmdSetScissor(cmd, 0, 1, &br);
            float bp[4] = {1.0f / float(st.from->w), 1.0f / float(st.from->h), st.mode, st.spread};
            vkCmdPushConstants(cmd, bloom_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(bp), bp);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            if (st.mode == 0 && sky_glow > 0) {
                // The sky as the composition shows it (procedural or replaced,
                // with the fogs, minus what the 3D covers): its brightest part
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_glow_);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_layout_, 0, 1, &compose_set_, 0,
                                        nullptr);
                ComposePush gp = reduced(pc, *st.to);
                gp.sky[1] = sky_glow;   // glow mode
                vkCmdPushConstants(cmd, compose_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(gp), &gp);
                vkCmdDraw(cmd, 3, 1, 0, 0);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloom_);
            }
            vkCmdEndRendering(cmd);
            transition(cmd, *st.to, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    } else if (bloom_a_.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        transition(cmd, bloom_a_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);   // bound, though not read
    }
}

void R3DRenderer::record_light_shafts(const Frame& f, const ComposePush& pc, bool shafts) {
    VkCommandBuffer cmd = f.cmd;
    if (shafts) {
        begin_quarter(cmd, rays_a_, VK_ATTACHMENT_LOAD_OP_CLEAR);   // cleared: the mask pass adds onto it
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_glow_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_layout_, 0, 1, &compose_set_, 0, nullptr);
        ComposePush mp = reduced(pc, rays_a_);
        mp.sky[3] = 1;   // mask mode
        vkCmdPushConstants(cmd, compose_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(mp), &mp);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        transition(cmd, rays_a_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        begin_quarter(cmd, rays_b_, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rays_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloom_layout_, 0, 1, &rays_set_, 0, nullptr);
        float rp[4] = {sun_uv_[0], sun_uv_[1], 0.5f * std::min(rays_strength_, 2.0f) * sun_light_, 0};
        vkCmdPushConstants(cmd, bloom_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(rp), rp);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        transition(cmd, rays_b_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    } else if (rays_b_.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        transition(cmd, rays_b_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);   // bound, though not read
    }
}

void R3DRenderer::record_diffusion(const Frame& f, const ComposePush& pc) {
    VkCommandBuffer cmd = f.cmd;
    // Diffusion (BLOOM): the picture behind the HUD, then blurred across and down
    if (bloom_strength_ > 0) {
        begin_quarter(cmd, diff_a_, VK_ATTACHMENT_LOAD_OP_CLEAR);   // cleared: the scene pass adds onto it
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_glow_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_layout_, 0, 1, &compose_set_, 0, nullptr);
        ComposePush dp = reduced(pc, diff_a_);
        dp.sky[3] = 3;       // scene mode
        dp.effects[0] = 0;   // (no diffusion of the diffusion)
        vkCmdPushConstants(cmd, compose_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(dp), &dp);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        transition(cmd, diff_a_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloom_);
        for (int pass = 0; pass < 2; pass++) {
            Image& to = pass == 0 ? diff_b_ : diff_a_;
            begin_quarter(cmd, to, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloom_layout_, 0, 1, &diff_sets_[pass], 0,
                                    nullptr);
            float bp[4] = {1.0f / float(to.w), 1.0f / float(to.h), pass == 0 ? 1.0f : 2.0f, 3.0f};
            vkCmdPushConstants(cmd, bloom_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(bp), bp);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
            transition(cmd, to, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    } else if (diff_a_.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        transition(cmd, diff_a_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);   // bound, though not read
    }
}

void R3DRenderer::record_exposure(const Frame& f, const ComposePush& pc) {
    VkCommandBuffer cmd = f.cmd;
    // HDR lighting: the 3D image's average brightness, eased into exposure_
    if (hdr_) {
        transition(cmd, exposure_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);   // keeps its value
        VkRenderingAttachmentInfo ea{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ea.imageView = exposure_.view;
        ea.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ea.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ea.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo er{VK_STRUCTURE_TYPE_RENDERING_INFO};
        er.renderArea = {{0, 0}, {1, 1}};
        er.layerCount = 1;
        er.colorAttachmentCount = 1;
        er.pColorAttachments = &ea;
        vkCmdBeginRendering(cmd, &er);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_measure_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_layout_, 0, 1, &compose_set_, 0, nullptr);
        VkViewport ev{0, 0, 1, 1, 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &ev);
        VkRect2D eb{{0, 0}, {1, 1}};
        vkCmdSetScissor(cmd, 0, 1, &eb);
        ComposePush ep = pc;
        ep.sky[3] = 2;   // measure mode
        vkCmdPushConstants(cmd, compose_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ep), &ep);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        transition(cmd, exposure_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

void R3DRenderer::record_compose(const Frame& f, const ComposePush& pc, VkImageView target) {
    VkCommandBuffer cmd = f.cmd;
    const VkExtent2D extent = f.extent;
    // Composition into the swapchain image
    VkRenderingAttachmentInfo ta{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ta.imageView = target;
    ta.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ta.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ta.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ti{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ti.renderArea = {{0, 0}, extent};
    ti.layerCount = 1;
    ti.colorAttachmentCount = 1;
    ti.pColorAttachments = &ta;
    vkCmdBeginRendering(cmd, &ti);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_layout_, 0, 1, &compose_set_, 0, nullptr);
    VkViewport full{0, 0, float(extent.width), float(extent.height), 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &full);
    VkRect2D all{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &all);
    vkCmdPushConstants(cmd, compose_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

// A frame, pass by pass (each one in its own function above)
void R3DRenderer::record(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent) {
    apply_pending();
    flush_uploads(cmd);
    // Placement in the window (widescreen or 4:3) and internal 3D resolution
    Frame f{};
    f.cmd = cmd;
    f.extent = extent;
    f.lay = compute_layout(float(extent.width), float(extent.height), max_aspect_);
    f.out_w = std::max(1u, uint32_t(f.lay.view_w * f.lay.scale * supersample_));
    f.out_h = std::max(1u, uint32_t(SCREEN_H * f.lay.scale * supersample_));
    ensure_targets(f.out_w, f.out_h);
    f.vcount = scene_ ? scene_->vertices.size() : 0;
    // Our rain, in the world viewport's view space, after the scene's vertices
    rain_vertices_.clear();
    if (scene_)
        for (const R3DViewport& v : scene_->viewports)
            if (v.world && v.proj[5] != 0) {
                float time = (float(scene_->frame % 100000) + interp_) / 57.5f;
                rain_.build(v, time, 2.0f / (std::fabs(v.proj[5]) * float(f.out_h)), rain_vertices_);
            }
    record_uploads(f);
    update_atmosphere(f.lay, f.out_w, f.out_h);

    // Motion vectors point to where each surface was in the previously
    // displayed image: the same game frame part way back, or the previous
    // game frame (extrapolated backwards when it was itself interpolated)
    float before = scene_serial_ != drawn_serial_ ? drawn_interp_ - 1.0f : drawn_interp_;
    drawn_serial_ = scene_serial_;
    drawn_interp_ = interp_;
    // DLAA: the image is shifted by a different sub-pixel amount each frame
    // (Halton 2,3 over 8 phases) so it can gather detail between pixels
    float jitter_x = 0, jitter_y = 0, alpha_seed = 0;
    if (dlaa_.ready()) {
        auto halton = [](uint32_t i, uint32_t base) {
            float digit = 1, r = 0;
            for (; i; i /= base) {
                digit /= float(base);
                r += digit * float(i % base);
            }
            return r;
        };
        uint32_t phase = uint32_t(jitter_index_++ % 8) + 1;
        jitter_x = halton(phase, 2) - 0.5f;
        jitter_y = halton(phase, 3) - 0.5f;
        // Alpha-tested edges (foliage): noise threshold that changes every frame
        float golden = float(jitter_index_) * 0.618034f;
        alpha_seed = 0.01f + 0.98f * (golden - std::floor(golden));
    }
    f.before = before;
    f.jitter_x = jitter_x;
    f.jitter_y = jitter_y;
    f.alpha_seed = alpha_seed;

    // LW_GPUTIME: the previous frame is done (one frame in flight); its times first
    static const bool gpu_time = getenv("LW_GPUTIME");
    if (gpu_time && !gpu_times_) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = GPU_MARKS;
        vkCreateQueryPool(dev_, &qi, nullptr, &gpu_times_);
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(vk_->gpu(), &props);
        timestamp_period_ = props.limits.timestampPeriod;
    }
    if (gpu_times_ && gpu_pending_) {
        // Each pass is the time from the previous mark written (a pass left out
        // this frame wrote none, and counts 0)
        uint64_t t[GPU_MARKS][2];   // timestamp, availability
        VkResult r = vkGetQueryPoolResults(dev_, gpu_times_, 0, GPU_MARKS, sizeof(t), t, sizeof(t[0]),
                                           VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if ((r == VK_SUCCESS || r == VK_NOT_READY) && t[0][1] && t[GPU_MARKS - 1][1]) {
            uint64_t last = t[0][0];
            for (int i = 1; i < GPU_MARKS; i++)
                if (t[i][1]) {
                    gpu_sum_[i] += double(t[i][0] - last) * timestamp_period_ * 1e-6;
                    last = t[i][0];
                }
            gpu_sum_[0] += double(t[GPU_MARKS - 1][0] - t[0][0]) * timestamp_period_ * 1e-6;
            if (++gpu_frames_ == 300) {
                static const char* const names[GPU_MARKS] = {"whole frame", "sky panorama", "ray tracing build",
                                                             "shadow map", "3D opaque", "ambient occlusion", "3D rest",
                                                             "DLAA", "bloom", "light shafts", "diffusion", "exposure",
                                                             "composition"};
                printf("[gpu] ms/image at %ux%u (msaa %d, %zu traced triangles):", f.out_w, f.out_h, int(samples_),
                       rt_frame_ ? rt_.triangles() : size_t(0));
                for (int i = 0; i < GPU_MARKS; i++) printf("%s %s %.3f", i ? "," : "", names[i], gpu_sum_[i] / 300);
                printf("\n");
                fflush(stdout);
                for (double& v : gpu_sum_) v = 0;
                gpu_frames_ = 0;
            }
        }
    }
    if (gpu_times_) {
        vkCmdResetQueryPool(cmd, gpu_times_, 0, GPU_MARKS);
        gpu_time_mark(cmd, 0);
    }
    record_sky_fields(f);
    gpu_time_mark(cmd, 1);
    record_ray_tracing(f);
    gpu_time_mark(cmd, 2);
    record_shadows(f);
    gpu_time_mark(cmd, 3);
    record_world(f);
    gpu_time_mark(cmd, 6);
    record_dlaa(f);
    gpu_time_mark(cmd, 7);
    ComposePush pc = compose_push(f);
    record_bloom(f, pc);
    gpu_time_mark(cmd, 8);
    // Light shafts: the visible sky around the sun, then smeared towards it
    bool shafts = rays_strength_ > 0 && sun_light_ > 0;
    pc.effects[3] = shafts ? 1.0f : 0.0f;
    record_light_shafts(f, pc, shafts);
    gpu_time_mark(cmd, 9);
    record_diffusion(f, pc);
    gpu_time_mark(cmd, 10);
    record_exposure(f, pc);
    gpu_time_mark(cmd, 11);
    record_compose(f, pc, target);
    gpu_time_mark(cmd, GPU_MARKS - 1);
    if (gpu_times_) gpu_pending_ = true;
    repeat_frame_ = f;
    repeat_push_ = pc;
    can_repeat_ = true;
}

void R3DRenderer::record_repeat(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent) {
    if (!can_repeat_ || extent.width != repeat_frame_.extent.width || extent.height != repeat_frame_.extent.height) {
        record(cmd, target, extent);
        return;
    }
    // Everything the composition reads is as the last record() left it,
    // with the pictures uploaded since
    flush_uploads(cmd);
    Frame f = repeat_frame_;
    f.cmd = cmd;
    ComposePush pc = repeat_push_;
    place_crosshairs(f.lay, pc);
    record_compose(f, pc, target);
}

void R3DRenderer::gpu_time_mark(VkCommandBuffer cmd, uint32_t index) {
    if (gpu_times_) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gpu_times_, index);
}

void R3DRenderer::shutdown() {
    if (!dev_) return;
    vkDeviceWaitIdle(dev_);
    destroy_pipelines();
    dlaa_.shutdown();
    rt_.shutdown();
    if (gpu_times_) vkDestroyQueryPool(dev_, gpu_times_, nullptr);
    for (Image* img : {&texture_ram_, &back_, &front_, &text_cells_, &sky_coords_, &color_, &color_ms_, &depth_, &motion_, &dlaa_out_, &dlaa_alpha_,
                        &glow_, &glow_ms_, &bloom_a_, &bloom_b_, &depth_resolved_, &ao_, &shadow_map_, &rays_a_, &rays_b_, &exposure_, &diff_a_, &diff_b_,
                        &sky_fields_})
        destroy_image(*img);
    for (Image& img : replacements_) destroy_image(img);
    for (auto& [slot, img] : overlay_images_) destroy_image(img);
    for (auto& [slot, img] : crosshair_images_) destroy_image(img);
    for (Upload& u : uploads_) destroy_image(u.img), destroy_buffer(u.staging);
    uploads_.clear();
    free_spent();
    for (Buffer* b : {&texture_staging_, &layer_staging_, &vertices_, &atmosphere_uniform_}) destroy_buffer(*b);
    for (VkSampler sm : {nearest_, linear_, replace_sampler_, overlay_sampler_, sky_sampler_})
        if (sm) vkDestroySampler(dev_, sm, nullptr);
    for (VkPipelineLayout l : {r3d_layout_, compose_layout_, bloom_layout_, ao_layout_, sky_layout_})
        if (l) vkDestroyPipelineLayout(dev_, l, nullptr);
    if (pool_) vkDestroyDescriptorPool(dev_, pool_, nullptr);   // (and the sets from it)
    for (VkDescriptorSetLayout l : {r3d_set_layout_, compose_set_layout_, bloom_set_layout_, ao_set_layout_})
        if (l) vkDestroyDescriptorSetLayout(dev_, l, nullptr);
}
