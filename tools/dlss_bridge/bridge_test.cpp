// Loads dlss_bridge.dll from next to this program the way the game does
// (dlss_loader.cpp) and checks that the two sides understand each other.
#include <cstdio>

#include "dlss.h"
#include "dlss_bridge.h"

int main() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring file(path, n);
    file = file.substr(0, file.find_last_of(L"\\/")) + L"\\" + DLSS_BRIDGE_FILE;
    HMODULE dll = LoadLibraryExW(file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) {
        fprintf(stderr, "dlss_bridge.dll does not load (error %lu)\n", GetLastError());
        return 1;
    }
    DlssBridgeEntry entry = reinterpret_cast<DlssBridgeEntry>(reinterpret_cast<void*>(GetProcAddress(dll, DLSS_BRIDGE_ENTRY)));
    if (!entry) {
        fprintf(stderr, "dlss_bridge.dll does not export %s\n", DLSS_BRIDGE_ENTRY);
        return 1;
    }
    if (entry(DLSS_BRIDGE_ABI + 1, nullptr)) {
        fprintf(stderr, "a bridge of another version was not refused\n");
        return 1;
    }
    if (!entry(DLSS_BRIDGE_ABI, nullptr)) {
        fprintf(stderr, "the bridge refused its own version\n");
        return 1;
    }
    // Through the loader now. Without an NVIDIA driver the list is empty
    std::vector<std::string> extensions = dlss::instance_extensions();
    printf("instance extensions for DLSS: %zu\n", extensions.size());
    for (const std::string& e : extensions) printf("  %s\n", e.c_str());
    dlss::Dlaa dlaa;
    if (dlaa.ready()) {
        fprintf(stderr, "a Dlaa that was never started says it is ready\n");
        return 1;
    }
    dlaa.shutdown();
    printf("ok\n");
    return 0;
}
