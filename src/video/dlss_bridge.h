// The boundary between the game and dlss_bridge.dll.
//
// NVIDIA's NGX SDK is a static library built for Microsoft's compiler: it
// only links with MSVC. dlss_bridge.dll is dlss.cpp plus that library, built
// once with MSVC (tools/dlss_bridge); a game built with any other compiler
// loads it at run time (dlss_loader.cpp) and reaches DLAA through the table
// below. Plain C types only: the two sides do not share a C++ runtime.
#pragma once
#include "vk_presenter.h"

// Bumped with any change to this file. A DLL of another version is refused.
#define DLSS_BRIDGE_ABI 1
#define DLSS_BRIDGE_FILE L"dlss_bridge.dll"

struct DlssBridgeImage {
    VkImage image;
    VkImageView view;
    VkFormat format;
};

// One finished line of log (ends in '\n'); error: 1 for stderr, 0 for stdout
typedef void (*DlssBridgeLog)(int error, const char* line);

struct DlssBridgeApi {
    // The names stay valid until the next call of the same function
    uint32_t (*instance_extensions)(const char* const** names);
    uint32_t (*device_extensions)(VkInstance instance, VkPhysicalDevice gpu, const char* const** names);
    // nullptr if DLAA is not available. loader: the game's vkGetInstanceProcAddr
    void* (*create)(PFN_vkGetInstanceProcAddr loader, VkInstance instance, VkPhysicalDevice gpu, VkDevice device);
    // As dlss::Dlaa::evaluate. 0 once it has stopped working
    int (*evaluate)(void* dlaa, VkCommandBuffer cmd, const DlssBridgeImage* color, const DlssBridgeImage* depth,
                    const DlssBridgeImage* motion, const DlssBridgeImage* output, uint32_t width, uint32_t height,
                    float jitter_x, float jitter_y, int reset, int channel);
    void (*destroy)(void* dlaa);
};

// The only export of the DLL. nullptr if abi is not the one it was built with.
#define DLSS_BRIDGE_ENTRY "dlss_bridge_api"
typedef const DlssBridgeApi* (*DlssBridgeEntry)(int abi, DlssBridgeLog log);
