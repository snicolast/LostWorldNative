// dlss_bridge.dll (dlss_bridge.h): dlss.cpp behind a table of C functions.
// Built with MSVC by tools/dlss_bridge, never as part of the game.
#include "dlss_bridge.h"

#include <cstdarg>
#include <cstdio>
#include <utility>

#include "dlss.h"

namespace {

DlssBridgeLog g_log = nullptr;

// The lists handed out by the two *_extensions calls
std::vector<std::string> g_names[2];
std::vector<const char*> g_pointers[2];

uint32_t hand_out(int slot, std::vector<std::string> names, const char* const** out) {
    g_names[slot] = std::move(names);
    g_pointers[slot].clear();
    for (const std::string& n : g_names[slot]) g_pointers[slot].push_back(n.c_str());
    *out = g_pointers[slot].data();
    return uint32_t(g_pointers[slot].size());
}

dlss::Image image(const DlssBridgeImage* i) { return {i->image, i->view, i->format}; }

// No C++ exception may cross into the game: it was built with another compiler

uint32_t instance_extensions(const char* const** names) {
    try {
        return hand_out(0, dlss::instance_extensions(), names);
    } catch (...) {
        return 0;
    }
}

uint32_t device_extensions(VkInstance instance, VkPhysicalDevice gpu, const char* const** names) {
    try {
        return hand_out(1, dlss::device_extensions(instance, gpu), names);
    } catch (...) {
        return 0;
    }
}

void* create(PFN_vkGetInstanceProcAddr loader, VkInstance instance, VkPhysicalDevice gpu, VkDevice device) {
    if (!loader) return nullptr;
    try {
        // This DLL's own volk, pointed at the Vulkan the game already loaded
        volkInitializeCustom(loader);
        volkLoadInstanceOnly(instance);
        volkLoadDevice(device);
        dlss::Dlaa* dlaa = new dlss::Dlaa;
        if (dlaa->init(instance, gpu, device)) return dlaa;
        delete dlaa;
    } catch (...) {
    }
    return nullptr;
}

int evaluate(void* dlaa, VkCommandBuffer cmd, const DlssBridgeImage* color, const DlssBridgeImage* depth,
             const DlssBridgeImage* motion, const DlssBridgeImage* output, uint32_t width, uint32_t height,
             float jitter_x, float jitter_y, int reset, int channel) {
    if (!dlaa || channel < 0 || channel > 1) return 0;
    try {
        return static_cast<dlss::Dlaa*>(dlaa)->evaluate(cmd, image(color), image(depth), image(motion), image(output),
                                                        width, height, jitter_x, jitter_y, reset != 0, channel);
    } catch (...) {
        return 0;
    }
}

void destroy(void* dlaa) {
    if (!dlaa) return;
    try {
        static_cast<dlss::Dlaa*>(dlaa)->shutdown();
    } catch (...) {
    }
    delete static_cast<dlss::Dlaa*>(dlaa);
}

const DlssBridgeApi kApi = {instance_extensions, device_extensions, create, evaluate, destroy};

}  // namespace

// The log of dlss.cpp goes to the game, which owns the console and the log file
void dlss_bridge_log(bool error, const char* format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (g_log) g_log(error ? 1 : 0, line);
}

extern "C" __declspec(dllexport) const DlssBridgeApi* dlss_bridge_api(int abi, DlssBridgeLog log) {
    if (abi != DLSS_BRIDGE_ABI) return nullptr;
    g_log = log;
    return &kApi;
}
