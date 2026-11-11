// NVIDIA DLAA (DLSS Super Resolution at native resolution) through the
// official NGX SDK (third_party/ngx). Replaces MSAA with NVIDIA's temporal
// AI anti-aliasing: it takes the 3D image, its depth and per-pixel motion
// vectors and writes a cleaner image of the same size. Needs an RTX GPU and
// nvngx_dlss.dll next to the executable; everything falls back to the
// normal path if any of that is missing.
#pragma once
#include <string>
#include <vector>

#include "vk_presenter.h"

struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

namespace dlss {

// Extensions NGX asks for, queried before the instance / device exist
std::vector<std::string> instance_extensions();
std::vector<std::string> device_extensions(VkInstance instance, VkPhysicalDevice gpu);

struct Image {
    VkImage image;
    VkImageView view;
    VkFormat format;
};

class Dlaa {
public:
    bool init(VkInstance instance, VkPhysicalDevice gpu, VkDevice device);
    void shutdown();
    // false if it never started or stopped working
    bool ready() const { return (params_ != nullptr || bridge_ != nullptr) && !failed_; }

    // Records the evaluation. Inputs in SHADER_READ_ONLY layout, output in
    // GENERAL with storage usage. jitter: sub-pixel offset of the projection
    // in pixels; reset: history must be discarded (camera cut, resize).
    // channel 0 is the image itself; channel 1 runs a second, independent
    // DLAA over its alpha (presented as grey colour), which DLSS's own alpha
    // upscaling leaves without temporal accumulation.
    bool evaluate(VkCommandBuffer cmd, const Image& color, const Image& depth, const Image& motion,
                  const Image& output, uint32_t width, uint32_t height, float jitter_x, float jitter_y,
                  bool reset, int channel = 0);

private:
    bool ensure_feature(VkCommandBuffer cmd, uint32_t width, uint32_t height, int channel);

    VkDevice device_ = VK_NULL_HANDLE;
    NVSDK_NGX_Parameter* params_ = nullptr;
    NVSDK_NGX_Handle* feature_[2] = {};
    uint32_t width_[2] = {}, height_[2] = {};
    bool failed_ = false;
    // dlss_loader.cpp: the Dlaa inside dlss_bridge.dll that does the work
    void* bridge_ = nullptr;
};

}  // namespace dlss
