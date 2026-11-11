// Built instead of dlss.cpp when the game is not built with MSVC, or with
// -DDLSS_BRIDGE=ON: NVIDIA's NGX library only links with MSVC, so DLAA comes
// from dlss_bridge.dll (dlss_bridge.h), looked for next to the executable.
// Without that DLL the renderer keeps MSAA, as on a GPU without DLSS.
#include <cstdio>

#include "dlss.h"
#include "dlss_bridge.h"

namespace dlss {
namespace {

void log_line(int error, const char* line) {
    fputs(line, error ? stderr : stdout);
}

const DlssBridgeApi* find() {
    // By its full path: never a dlss_bridge.dll from the current folder or PATH
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring file(path, n);
    size_t sep = file.find_last_of(L"\\/");
    file = (sep == std::wstring::npos ? std::wstring(L".") : file.substr(0, sep)) + L"\\" + DLSS_BRIDGE_FILE;
    HMODULE dll = LoadLibraryExW(file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) {
        fprintf(stderr, "[dlaa] dlss_bridge.dll is not next to the executable: using MSAA\n");
        return nullptr;
    }
    DlssBridgeEntry entry = reinterpret_cast<DlssBridgeEntry>(reinterpret_cast<void*>(GetProcAddress(dll, DLSS_BRIDGE_ENTRY)));
    const DlssBridgeApi* api = entry ? entry(DLSS_BRIDGE_ABI, log_line) : nullptr;
    if (!api) {
        fprintf(stderr, "[dlaa] dlss_bridge.dll is of another version than this build: using MSAA\n");
        FreeLibrary(dll);
    }
    return api;
}

const DlssBridgeApi* bridge() {
    static const DlssBridgeApi* api = find();
    return api;
}

std::vector<std::string> names(uint32_t count, const char* const* list) {
    std::vector<std::string> out;
    for (uint32_t i = 0; i < count; i++) out.push_back(list[i]);
    return out;
}

DlssBridgeImage image(const Image& i) { return {i.image, i.view, i.format}; }

}  // namespace

std::vector<std::string> instance_extensions() {
    const DlssBridgeApi* api = bridge();
    if (!api) return {};
    const char* const* list = nullptr;
    uint32_t count = api->instance_extensions(&list);
    return names(count, list);
}

std::vector<std::string> device_extensions(VkInstance instance, VkPhysicalDevice gpu) {
    const DlssBridgeApi* api = bridge();
    if (!api) return {};
    const char* const* list = nullptr;
    uint32_t count = api->device_extensions(instance, gpu, &list);
    return names(count, list);
}

bool Dlaa::init(VkInstance instance, VkPhysicalDevice gpu, VkDevice device) {
    const DlssBridgeApi* api = bridge();
    if (api) bridge_ = api->create(vkGetInstanceProcAddr, instance, gpu, device);
    failed_ = bridge_ == nullptr;
    return bridge_ != nullptr;
}

bool Dlaa::evaluate(VkCommandBuffer cmd, const Image& color, const Image& depth, const Image& motion,
                    const Image& output, uint32_t width, uint32_t height, float jitter_x, float jitter_y,
                    bool reset, int channel) {
    if (!bridge_ || failed_) return false;
    DlssBridgeImage c = image(color), d = image(depth), m = image(motion), o = image(output);
    // The bridge only refuses once DLSS has stopped working, and it stays so
    if (!bridge()->evaluate(bridge_, cmd, &c, &d, &m, &o, width, height, jitter_x, jitter_y, reset ? 1 : 0, channel))
        failed_ = true;
    return !failed_;
}

void Dlaa::shutdown() {
    if (bridge_) bridge()->destroy(bridge_);
    bridge_ = nullptr;
}

}  // namespace dlss
