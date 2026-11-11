// Built instead of dlss.cpp when the NVIDIA DLSS SDK is not in third_party/ngx
// (its download failed, or it was left out on purpose): DLAA is never
// available, and the renderer keeps MSAA, as on a GPU without DLSS.
#include "dlss.h"

#include <cstdio>

namespace dlss {

std::vector<std::string> instance_extensions() { return {}; }

std::vector<std::string> device_extensions(VkInstance, VkPhysicalDevice) { return {}; }

bool Dlaa::init(VkInstance, VkPhysicalDevice, VkDevice) {
    fprintf(stderr, "[dlaa] built without the NVIDIA DLSS SDK: using MSAA\n");
    failed_ = true;
    return false;
}

bool Dlaa::ensure_feature(VkCommandBuffer, uint32_t, uint32_t, int) { return false; }

bool Dlaa::evaluate(VkCommandBuffer, const Image&, const Image&, const Image&, const Image&, uint32_t, uint32_t, float,
                    float, bool, int) {
    return false;
}

void Dlaa::shutdown() {}

}  // namespace dlss
