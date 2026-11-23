// NVIDIA DLAA (DLSS Super Resolution at native resolution) and DLSS itself
// through the official NGX SDK (third_party/ngx). Replaces MSAA with NVIDIA's
// temporal AI anti-aliasing: it takes the 3D image, its depth and per-pixel
// motion vectors and writes a cleaner image of the same size (DLAA) or of a
// larger one (DLSS: the 3D is drawn smaller, and faster). Needs an RTX GPU and
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

// NGX's render presets (NVSDK_NGX_DLSS_Hint_Render_Preset_*)
constexpr int PRESET_K = 11, PRESET_L = 12, PRESET_M = 13;
constexpr int MODE_COUNT = 5;
// How many times smaller each way the 3D is drawn in each mode (NVIDIA's
// ratios: 100%, 66.7%, 58%, 50%, 33.3% of the output's size)
constexpr float MODE_SCALE[MODE_COUNT] = {1.0f, 1.5f, 1.0f / 0.58f, 2.0f, 3.0f};
// Tensor cores with FP8 (RTX 40 and newer), told by the GPU's name: on RTX
// 20 and 30 the transformer presets M and L cost noticeably more than K
bool has_fp8(const char* gpu_name);
// The preset used: model 0 AUTO (K for DLAA, QUALITY and BALANCED, M for
// PERFORMANCE, L for ULTRA PERFORMANCE; K for all without FP8), 1 K, 2 M, 3 L
int preset_for(int mode, int model, bool fp8);

struct Image {
    VkImage image;
    VkImageView view;
    VkFormat format;
};

class Dlaa {
public:
    bool init(VkInstance instance, VkPhysicalDevice gpu, VkDevice device);
    void shutdown();
    // Before the first evaluate. mode: 0 DLAA, 1 QUALITY, 2 BALANCED,
    // 3 PERFORMANCE, 4 ULTRA PERFORMANCE (the input that much smaller: MODE_SCALE).
    // preset: NGX's render preset (PRESET_K / _L / _M), 0 its own default
    void configure(int mode, int preset);
    // false if it never started or stopped working
    bool ready() const { return (params_ != nullptr || bridge_ != nullptr) && !failed_; }
    // Whether evaluate takes a smaller part of the inputs (draw_width):
    // through dlss_bridge.dll too, since its ABI 4
    bool dynamic_resolution() const { return ready(); }
    // Before configure: the inputs may be drawn smaller from image to image
    // (draw_width) at DLAA's size too
    void set_dynamic(bool on) { dynamic_ = on; }

    // Records the evaluation. Inputs in SHADER_READ_ONLY layout, output in
    // GENERAL with storage usage. jitter: sub-pixel offset of the projection
    // in pixels; reset: history must be discarded (camera cut, resize).
    // channel 0 is the image itself; channel 1 runs a second, independent
    // DLAA (no longer used: the renderer smooths the alpha over time itself,
    // alpha_taa.frag, for a fraction of the cost).
    // width, height: the inputs' size; out_width, out_height: the output's
    // (the same: DLAA; larger: DLSS at its quality setting)
    // draw_width, draw_height: the part of the inputs drawn this image, from
    // their top left corner (dynamic resolution: smaller where the graphics
    // card would miss the refresh); 0: all of them
    bool evaluate(VkCommandBuffer cmd, const Image& color, const Image& depth, const Image& motion,
                  const Image& output, uint32_t width, uint32_t height, uint32_t out_width, uint32_t out_height,
                  float jitter_x, float jitter_y, bool reset, int channel = 0, uint32_t draw_width = 0,
                  uint32_t draw_height = 0);

private:
    bool ensure_feature(VkCommandBuffer cmd, uint32_t width, uint32_t height, uint32_t out_width, uint32_t out_height,
                        int channel);

    VkDevice device_ = VK_NULL_HANDLE;
    NVSDK_NGX_Parameter* params_ = nullptr;
    NVSDK_NGX_Handle* feature_[2] = {};
    uint32_t width_[2] = {}, height_[2] = {}, out_width_[2] = {}, out_height_[2] = {};
    bool failed_ = false;
    int mode_ = 0, preset_ = 0;
    bool dynamic_ = false;
    // dlss_loader.cpp: the Dlaa inside dlss_bridge.dll that does the work
    void* bridge_ = nullptr;
};

inline bool has_fp8(const char* gpu_name) {
    std::string n = gpu_name ? gpu_name : "";
    auto has = [&](const char* t) { return n.find(t) != std::string::npos; };
    if (!has("RTX")) return false;
    if (has("Ada") || has("Blackwell") || has("RTX PRO")) return true;   // (RTX 2000 Ada and the like)
    if (has("RTX 20") || has("RTX 30") || has("TITAN RTX") || has("Quadro RTX")) return false;
    for (size_t i = n.find("RTX A"); i != std::string::npos; i = n.find("RTX A", i + 1))   // RTX A2000..A6000: Ampere
        if (i + 5 < n.size() && n[i + 5] >= '0' && n[i + 5] <= '9') return false;
    return true;   // RTX 40, 50 and what comes after
}

inline int preset_for(int mode, int model, bool fp8) {
    static const int AUTO[MODE_COUNT] = {PRESET_K, PRESET_K, PRESET_K, PRESET_M, PRESET_L};
    if (model == 1) return PRESET_K;
    if (model == 2) return PRESET_M;
    if (model == 3) return PRESET_L;
    return fp8 && mode >= 0 && mode < MODE_COUNT ? AUTO[mode] : PRESET_K;
}

}  // namespace dlss
