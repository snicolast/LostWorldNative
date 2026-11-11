#include "dlss.h"

#include <cstdio>

#include "../common/paths.h"
#include "nvsdk_ngx_helpers_vk.h"
#include "nvsdk_ngx_vk.h"

// Inside dlss_bridge.dll the log goes through the game (dlss_bridge.cpp)
#ifdef DLSS_BRIDGE_BUILD
void dlss_bridge_log(bool error, const char* format, ...);
#define LOG_ERROR(...) dlss_bridge_log(true, __VA_ARGS__)
#define LOG_INFO(...) dlss_bridge_log(false, __VA_ARGS__)
#else
#define LOG_ERROR(...) fprintf(stderr, __VA_ARGS__)
#define LOG_INFO(...) printf(__VA_ARGS__)
#endif

namespace dlss {
namespace {

// Project identifier for NGX (a random GUID, as NVIDIA asks of custom engines)
const char* kProjectId = "5e1c7a3e-3b8f-4d2a-9f6e-1d4c2b7a9e10";
const char* kEngineVersion = "1.0";

struct Discovery {
    std::wstring dir = exe_folder_w();
    const wchar_t* paths[1];
    NVSDK_NGX_FeatureCommonInfo common{};
    NVSDK_NGX_FeatureDiscoveryInfo info{};
    Discovery() {
        paths[0] = dir.c_str();
        common.PathListInfo.Path = paths;
        common.PathListInfo.Length = 1;
        info.SDKVersion = NVSDK_NGX_Version_API;
        info.FeatureID = NVSDK_NGX_Feature_SuperSampling;
        info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
        info.Identifier.v.ProjectDesc.ProjectId = kProjectId;
        info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
        info.Identifier.v.ProjectDesc.EngineVersion = kEngineVersion;
        info.ApplicationDataPath = dir.c_str();
        info.FeatureInfo = &common;
    }
};

std::vector<std::string> names(uint32_t count, const VkExtensionProperties* props) {
    std::vector<std::string> out;
    for (uint32_t i = 0; i < count; i++) out.push_back(props[i].extensionName);
    return out;
}

NVSDK_NGX_Resource_VK resource(const Image& img, uint32_t w, uint32_t h, bool rw, VkImageAspectFlags aspect) {
    return NVSDK_NGX_Create_ImageView_Resource_VK(img.view, img.image, {aspect, 0, 1, 0, 1}, img.format, w, h, rw);
}

}  // namespace

std::vector<std::string> instance_extensions() {
    Discovery d;
    uint32_t count = 0;
    VkExtensionProperties* props = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&d.info, &count, &props))) return {};
    return names(count, props);
}

std::vector<std::string> device_extensions(VkInstance instance, VkPhysicalDevice gpu) {
    Discovery d;
    uint32_t count = 0;
    VkExtensionProperties* props = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, gpu, &d.info, &count, &props)))
        return {};
    return names(count, props);
}

bool Dlaa::init(VkInstance instance, VkPhysicalDevice gpu, VkDevice device) {
    device_ = device;
    Discovery d;
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, d.dir.c_str(), instance, gpu, device,
        vkGetInstanceProcAddr, vkGetDeviceProcAddr, &d.common);
    if (NVSDK_NGX_FAILED(r)) {
        LOG_ERROR("[dlaa] NGX could not start (0x%08X)\n", unsigned(r));
        return false;
    }
    NVSDK_NGX_Parameter* caps = nullptr;
    int available = 0;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&caps)) && caps) {
        NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        NVSDK_NGX_VULKAN_DestroyParameters(caps);
    }
    if (!available) {
        LOG_ERROR("[dlaa] DLSS not available on this GPU, or nvngx_dlss.dll is missing next to the executable\n");
        NVSDK_NGX_VULKAN_Shutdown1(device);
        return false;
    }
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_AllocateParameters(&params_)) || !params_) {
        params_ = nullptr;
        NVSDK_NGX_VULKAN_Shutdown1(device);
        return false;
    }
    LOG_INFO("[dlaa] DLSS ready (DLAA at native resolution)\n");
    return true;
}

bool Dlaa::ensure_feature(VkCommandBuffer cmd, uint32_t width, uint32_t height, int channel) {
    NVSDK_NGX_Handle*& feature = feature_[channel];
    if (feature && width == width_[channel] && height == height_[channel]) return true;
    if (feature) {
        vkDeviceWaitIdle(device_);
        NVSDK_NGX_VULKAN_ReleaseFeature(feature);
        feature = nullptr;
    }
    NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                              NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth = create.Feature.InTargetWidth = width;
    create.Feature.InHeight = create.Feature.InTargetHeight = height;
    create.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
    // Reverse-Z depth; motion vectors at render (= output) resolution.
    // The alpha (where it is 0 the composition shows the 2D background) goes
    // through the second instance.
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    // The image itself is HDR (16-bit float, highlights above 1); its alpha is not
    if (channel == 0)
        create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT1(device_, cmd, 1, 1, &feature, params_, &create);
    if (NVSDK_NGX_FAILED(r) || !feature) {
        LOG_ERROR("[dlaa] could not create DLSS %ux%u (0x%08X)\n", width, height, unsigned(r));
        feature = nullptr;
        failed_ = true;
        return false;
    }
    width_[channel] = width;
    height_[channel] = height;
    return true;
}

bool Dlaa::evaluate(VkCommandBuffer cmd, const Image& color, const Image& depth, const Image& motion,
                    const Image& output, uint32_t width, uint32_t height, float jitter_x, float jitter_y,
                    bool reset, int channel) {
    if (!params_ || failed_) return false;
    bool fresh = !feature_[channel] || width != width_[channel] || height != height_[channel];
    if (!ensure_feature(cmd, width, height, channel)) return false;
    NVSDK_NGX_Resource_VK in_color = resource(color, width, height, false, VK_IMAGE_ASPECT_COLOR_BIT);
    NVSDK_NGX_Resource_VK in_depth = resource(depth, width, height, false, VK_IMAGE_ASPECT_DEPTH_BIT);
    NVSDK_NGX_Resource_VK in_motion = resource(motion, width, height, false, VK_IMAGE_ASPECT_COLOR_BIT);
    NVSDK_NGX_Resource_VK out = resource(output, width, height, true, VK_IMAGE_ASPECT_COLOR_BIT);
    NVSDK_NGX_VK_DLSS_Eval_Params eval{};
    eval.Feature.pInColor = &in_color;
    eval.Feature.pInOutput = &out;
    eval.pInDepth = &in_depth;
    eval.pInMotionVectors = &in_motion;
    eval.InJitterOffsetX = jitter_x;
    eval.InJitterOffsetY = jitter_y;
    eval.InRenderSubrectDimensions = {width, height};
    eval.InReset = reset || fresh ? 1 : 0;
    eval.InMVScaleX = eval.InMVScaleY = 1.0f;   // motion vectors are already in pixels
    eval.InPreExposure = eval.InExposureScale = 1.0f;
    NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, feature_[channel], params_, &eval);
    if (NVSDK_NGX_FAILED(r)) {
        LOG_ERROR("[dlaa] DLSS evaluation failed (0x%08X); disabling it\n", unsigned(r));
        failed_ = true;
        return false;
    }
    return true;
}

void Dlaa::shutdown() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    for (NVSDK_NGX_Handle*& feature : feature_) {
        if (feature) NVSDK_NGX_VULKAN_ReleaseFeature(feature);
        feature = nullptr;
    }
    if (params_) {
        NVSDK_NGX_VULKAN_DestroyParameters(params_);
        NVSDK_NGX_VULKAN_Shutdown1(device_);
    }
    params_ = nullptr;
    device_ = VK_NULL_HANDLE;
}

}  // namespace dlss
