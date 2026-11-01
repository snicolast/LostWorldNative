#include "ui_overlay.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "compose.vert.h"
#include "ui.frag.h"

namespace {
VkShaderModule make_module(VkDevice dev, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &m);
    return m;
}
}  // namespace

bool UiOverlay::init(VkPresenter& vk) {
    vk_ = &vk;
    dev_ = vk.device();

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(dev_, &si, nullptr, &sampler_);

    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &b;
    vkCreateDescriptorSetLayout(dev_, &li, nullptr, &set_layout_);
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    vkCreateDescriptorPool(dev_, &pi, nullptr, &pool_);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &set_layout_;
    vkAllocateDescriptorSets(dev_, &ai, &set_);
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout_;
    vkCreatePipelineLayout(dev_, &pli, nullptr, &layout_);

    VkShaderModule vs = make_module(dev_, spv_compose_vert, sizeof(spv_compose_vert));
    VkShaderModule fs = make_module(dev_, spv_ui_frag, sizeof(spv_ui_frag));
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    cba.blendEnable = VK_TRUE;   // premultiplied alpha
    cba.srcColorBlendFactor = cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstColorBlendFactor = cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = cba.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkFormat format = vk.swap_format();
    VkPipelineRenderingCreateInfo rend{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rend.colorAttachmentCount = 1;
    rend.pColorAttachmentFormats = &format;
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
    gp.layout = layout_;
    if (vkCreateGraphicsPipelines(dev_, vk_->pipeline_cache(), 1, &gp, nullptr, &pipeline_) != VK_SUCCESS)
        fprintf(stderr, "[vulkan] could not create the menu pipeline\n");
    vkDestroyShaderModule(dev_, vs, nullptr);
    vkDestroyShaderModule(dev_, fs, nullptr);
    return pipeline_ != VK_NULL_HANDLE;
}

void UiOverlay::set_image(const uint32_t* bgra, uint32_t w, uint32_t h) {
    pending_.assign(bgra, bgra + size_t(w) * h);
    pending_w_ = w;
    pending_h_ = h;
    dirty_y0_ = 0, dirty_y1_ = h;
    dirty_ = visible_ = true;
}

void UiOverlay::update_rows(const uint32_t* bgra, uint32_t w, uint32_t h, uint32_t y0, uint32_t y1) {
    y1 = std::min(y1, h);
    if (w != pending_w_ || h != pending_h_ || pending_.size() != size_t(w) * h) return set_image(bgra, w, h);
    if (y0 >= y1) return;
    memcpy(pending_.data() + size_t(y0) * w, bgra + size_t(y0) * w, size_t(y1 - y0) * w * 4);
    if (!dirty_) dirty_y0_ = y0, dirty_y1_ = y1;
    else dirty_y0_ = std::min(dirty_y0_, y0), dirty_y1_ = std::max(dirty_y1_, y1);
    dirty_ = visible_ = true;
}

void UiOverlay::destroy_image() {
    if (view_) vkDestroyImageView(dev_, view_, nullptr);
    if (image_) vkDestroyImage(dev_, image_, nullptr);
    if (image_memory_) vkFreeMemory(dev_, image_memory_, nullptr);
    view_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
    image_memory_ = VK_NULL_HANDLE;
    w_ = h_ = 0;
}

void UiOverlay::record(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent) {
    if (!visible_ || !pipeline_) return;
    if (dirty_) {
        size_t bytes = pending_.size() * 4;
        if (pending_w_ != w_ || pending_h_ != h_ || bytes > staging_size_) {
            filled_ = false;
            vkDeviceWaitIdle(dev_);
            destroy_image();
            if (staging_) {
                vkDestroyBuffer(dev_, staging_, nullptr);
                vkFreeMemory(dev_, staging_memory_, nullptr);
                staging_ = VK_NULL_HANDLE;
            }
            VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ii.imageType = VK_IMAGE_TYPE_2D;
            ii.format = VK_FORMAT_B8G8R8A8_UNORM;
            ii.extent = {pending_w_, pending_h_, 1};
            ii.mipLevels = ii.arrayLayers = 1;
            ii.samples = VK_SAMPLE_COUNT_1_BIT;
            ii.tiling = VK_IMAGE_TILING_OPTIMAL;
            ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            vkCreateImage(dev_, &ii, nullptr, &image_);
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(dev_, image_, &req);
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = vk_->find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            vkAllocateMemory(dev_, &mai, nullptr, &image_memory_);
            vkBindImageMemory(dev_, image_, image_memory_, 0);
            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = image_;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = ii.format;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCreateImageView(dev_, &vci, nullptr, &view_);
            w_ = pending_w_;
            h_ = pending_h_;

            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = bytes;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            vkCreateBuffer(dev_, &bi, nullptr, &staging_);
            vkGetBufferMemoryRequirements(dev_, staging_, &req);
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = vk_->find_memory(
                req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkAllocateMemory(dev_, &mai, nullptr, &staging_memory_);
            vkBindBufferMemory(dev_, staging_, staging_memory_, 0);
            vkMapMemory(dev_, staging_memory_, 0, VK_WHOLE_SIZE, 0, &mapped_);
            staging_size_ = bytes;

            VkDescriptorImageInfo info{sampler_, view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set_;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &info;
            vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
        }
        // The previous frame has finished (the presenter waits for it), so
        // the staging buffer is free to rewrite
        uint32_t y0 = filled_ ? std::min(dirty_y0_, h_) : 0, y1 = filled_ ? std::min(dirty_y1_, h_) : h_;
        if (y1 <= y0) y0 = 0, y1 = std::min(1u, h_);   // nothing new: one row again, as it was
        size_t row = size_t(w_) * 4;
        if (y1 > y0) memcpy(static_cast<uint8_t*>(mapped_) + y0 * row, pending_.data() + size_t(y0) * w_, (y1 - y0) * row);
        vk_image_barrier(cmd, image_, filled_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy c{};
        c.bufferOffset = y0 * row;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageOffset = {0, int32_t(y0), 0};
        c.imageExtent = {w_, y1 - y0, 1};
        vkCmdCopyBufferToImage(cmd, staging_, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        vk_image_barrier(cmd, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        filled_ = true;
        dirty_ = false;
    }
    if (!image_) return;

    VkRenderingAttachmentInfo ta{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ta.imageView = target;
    ta.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ta.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ta.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ta;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
    VkViewport full{0, 0, float(extent.width), float(extent.height), 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &full);
    VkRect2D all{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &all);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void UiOverlay::shutdown() {
    if (!dev_) return;
    vkDeviceWaitIdle(dev_);
    destroy_image();
    if (staging_) {
        vkDestroyBuffer(dev_, staging_, nullptr);
        vkFreeMemory(dev_, staging_memory_, nullptr);
    }
    if (pipeline_) vkDestroyPipeline(dev_, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(dev_, layout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(dev_, pool_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(dev_, set_layout_, nullptr);
    if (sampler_) vkDestroySampler(dev_, sampler_, nullptr);
}
