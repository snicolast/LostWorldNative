#include "vk_presenter.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../common/clock.h"
#include "dlss.h"
#include "image_io.h"

namespace {

bool check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        fprintf(stderr, "[vulkan] %s failed (%d)\n", what, int(r));
        return false;
    }
    return true;
}

}  // namespace

void vk_image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                      VkImageAspectFlags aspect, uint32_t layers) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, layers};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
}

uint32_t VkPresenter::find_memory(uint32_t type_bits, VkMemoryPropertyFlags flags) const {
    for (uint32_t i = 0; i < memory_.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
}

LRESULT CALLBACK VkPresenter::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<VkPresenter*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    if (self && self->input_hook) {
        switch (msg) {
        case WM_KEYDOWN: case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MOUSEWHEEL:
            if (self->input_hook(msg, wp, lp)) return 0;
            break;
        }
    }
    switch (msg) {
    case WM_INPUT:
        if (self && self->raw_input) self->raw_input(reinterpret_cast<HRAWINPUT>(lp));
        break;   // DefWindowProc cleans up after it
    case WM_CLOSE:
        if (self) self->quit_ = true;
        return 0;
    case WM_SIZE:
        if (self) self->resized_ = true;
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && self) self->quit_ = true;
        if (wp == VK_F11 && self) self->toggle_fullscreen();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {   // the renderer draws its own crosshair (menus use the arrow)
            SetCursor(self && self->show_cursor_ ? LoadCursor(nullptr, IDC_ARROW) : nullptr);
            return TRUE;
        }
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && self) { self->toggle_fullscreen(); return 0; }   // Alt+Enter
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void VkPresenter::set_cursor_visible(bool visible) {
    if (visible == show_cursor_) return;
    show_cursor_ = visible;
    // WM_SETCURSOR only comes when the mouse moves: change it now if it is over the window
    POINT p;
    if (GetForegroundWindow() == hwnd_ && GetCursorPos(&p) && WindowFromPoint(p) == hwnd_ &&
        SendMessageA(hwnd_, WM_NCHITTEST, 0, MAKELPARAM(p.x, p.y)) == HTCLIENT)
        SetCursor(visible ? LoadCursor(nullptr, IDC_ARROW) : nullptr);
}

void VkPresenter::toggle_fullscreen() {
    DWORD style = GetWindowLongA(hwnd_, GWL_STYLE);
    if (!fullscreen_) {
        MONITORINFO mi{sizeof(mi)};
        GetWindowPlacement(hwnd_, &placement_);
        GetMonitorInfoA(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLongA(hwnd_, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongA(hwnd_, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd_, &placement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    fullscreen_ = !fullscreen_;
    resized_ = true;
}

bool VkPresenter::init(const char* title, int width, int height, bool with_ngx) {
    HINSTANCE inst = GetModuleHandleA(nullptr);
    WNDCLASSA wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));  // res/lostworld.rc
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    wc.lpszClassName = "LostWorldNative";
    RegisterClassA(&wc);
    RECT r{0, 0, width, height};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd_ = CreateWindowA(wc.lpszClassName, title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                          r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!hwnd_) return false;
    SetWindowLongPtrA(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    if (!check(volkInitialize(), "volkInitialize (is the Vulkan driver missing?)")) return false;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Lost World Native";
    app.apiVersion = VK_API_VERSION_1_3;
    std::vector<std::string> inst_names = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    if (with_ngx) {
        uint32_t count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> avail(count);
        vkEnumerateInstanceExtensionProperties(nullptr, &count, avail.data());
        for (const std::string& e : dlss::instance_extensions())
            for (auto& a : avail)
                if (e == a.extensionName && std::find(inst_names.begin(), inst_names.end(), e) == inst_names.end())
                    inst_names.push_back(e);
    }
    std::vector<const char*> inst_ext;
    for (auto& e : inst_names) inst_ext.push_back(e.c_str());
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(inst_ext.size());
    ici.ppEnabledExtensionNames = inst_ext.data();
    if (!check(vkCreateInstance(&ici, nullptr, &instance_), "vkCreateInstance")) return false;
    volkLoadInstance(instance_);

    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = inst;
    sci.hwnd = hwnd_;
    if (!check(vkCreateWin32SurfaceKHR(instance_, &sci, nullptr, &surface_), "vkCreateWin32SurfaceKHR")) return false;

    // GPU: prefer a discrete one with Vulkan 1.3 and a graphics queue that can present
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    vkEnumeratePhysicalDevices(instance_, &n, gpus.data());
    int best_score = -1;
    for (auto g : gpus) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(g, &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, qs.data());
        for (uint32_t q = 0; q < qn; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(g, q, surface_, &present);
            if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > best_score) {
                best_score = score;
                gpu_ = g;
                queue_family_ = q;
                device_name_ = p.deviceName;
            }
            break;
        }
    }
    if (!gpu_) {
        fprintf(stderr, "[vulkan] no GPU with Vulkan 1.3 that can present to the window\n");
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(gpu_, &memory_);   // for find_memory
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu_, &props);
    limits_ = props.limits;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    std::vector<std::string> dev_names = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    // The extensions this GPU/driver offers
    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> avail(ext_count);
    vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &ext_count, avail.data());
    auto has_extension = [&](const char* name) {
        for (const VkExtensionProperties& a : avail)
            if (!strcmp(a.extensionName, name)) return true;
        return false;
    };
    if (with_ngx)   // only the ones offered
        for (const std::string& e : dlss::device_extensions(instance_, gpu_))
            if (has_extension(e.c_str()) && std::find(dev_names.begin(), dev_names.end(), e) == dev_names.end())
                dev_names.push_back(e);
    // Present ids and waiting for them: when each image is really on screen
    VkPhysicalDevicePresentWaitFeaturesKHR fwait{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
    VkPhysicalDevicePresentIdFeaturesKHR fid{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    fid.pNext = &fwait;
    {
        if (has_extension(VK_KHR_PRESENT_ID_EXTENSION_NAME) && has_extension(VK_KHR_PRESENT_WAIT_EXTENSION_NAME)) {
            VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            query.pNext = &fid;
            vkGetPhysicalDeviceFeatures2(gpu_, &query);
            present_wait_ = fid.presentId && fwait.presentWait;
        }
        if (present_wait_) {
            dev_names.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
            dev_names.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
        }
    }
    // Ray queries (sun shadows traced through the game world): only when the
    // driver offers all three extensions and the features they need
    VkPhysicalDeviceAccelerationStructureFeaturesKHR fas{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR frq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    {
        if (has_extension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) && has_extension(VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
            has_extension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)) {
            VkPhysicalDeviceVulkan12Features have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            frq.pNext = &have;
            fas.pNext = &frq;
            VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            query.pNext = &fas;
            vkGetPhysicalDeviceFeatures2(gpu_, &query);
            ray_query_ = fas.accelerationStructure && frq.rayQuery && have.bufferDeviceAddress;
        }
        if (ray_query_) {
            for (const char* e : {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
                                  VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME})
                if (std::find(dev_names.begin(), dev_names.end(), e) == dev_names.end()) dev_names.push_back(e);
            fas = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
            fas.accelerationStructure = VK_TRUE;
            frq = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
            frq.rayQuery = VK_TRUE;
            fas.pNext = &frq;
        }
    }
    std::vector<const char*> dev_ext;
    for (auto& e : dev_names) dev_ext.push_back(e.c_str());
    // Only the features this GPU has are asked for (asking for one it lacks
    // fails vkCreateDevice with no word of which): what is missing is said
    // here and the renderer does without it
    VkPhysicalDeviceVulkan12Features have12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    have.pNext = &have12;
    vkGetPhysicalDeviceFeatures2(gpu_, &have);
    std::string missing;
    auto want = [&](VkBool32 has, const char* name) {
        if (!has) missing += std::string(missing.empty() ? "" : ", ") + name;
        return has;
    };
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    // Texture pack and 2D replacements: arrays of images indexed per polygon,
    // filled while earlier frames may still use the set
    bool partially = want(have12.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound");
    bool pending = want(have12.descriptorBindingUpdateUnusedWhilePending, "descriptorBindingUpdateUnusedWhilePending");
    bool nonuniform = want(have12.shaderSampledImageArrayNonUniformIndexing, "shaderSampledImageArrayNonUniformIndexing");
    descriptor_indexing_ = partially && pending && nonuniform;
    if (descriptor_indexing_) {
        f12.descriptorBindingPartiallyBound = VK_TRUE;
        f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
        f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    }
    if (with_ngx) f12.bufferDeviceAddress = have12.bufferDeviceAddress;   // NGX reads its own buffers through device addresses
    if (ray_query_) f12.bufferDeviceAddress = VK_TRUE;   // acceleration structures are built from buffer addresses
    anisotropy_ = want(have.features.samplerAnisotropy, "samplerAnisotropy");
    // DLAA: the translucent pass blends colour but leaves the motion vectors alone
    if (with_ngx && !want(have.features.independentBlend, "independentBlend")) with_ngx = false;
    printf("[vulkan] %s: Vulkan %u.%u, %u sampled images per stage, %u bytes of push constants%s%s\n", device_name_.c_str(),
           VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
           limits_.maxPerStageDescriptorSampledImages, limits_.maxPushConstantsSize, missing.empty() ? "" : "; without ",
           missing.c_str());
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.pNext = &f12;
    f13.dynamicRendering = VK_TRUE;
    if (present_wait_) {
        fwait.pNext = f13.pNext;
        f13.pNext = &fid;
    }
    if (ray_query_) {
        frq.pNext = f13.pNext;
        f13.pNext = &fas;
    }
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &f13;
    features.features.samplerAnisotropy = anisotropy_ ? VK_TRUE : VK_FALSE;
    features.features.independentBlend = with_ngx ? VK_TRUE : VK_FALSE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &features;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(dev_ext.size());
    dci.ppEnabledExtensionNames = dev_ext.data();
    if (!check(vkCreateDevice(gpu_, &dci, nullptr, &device_), "vkCreateDevice")) return false;
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queue_family_;
    vkCreateCommandPool(device_, &pci, nullptr, &pool_);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkAllocateCommandBuffers(device_, &cai, &cmd_);
    VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(device_, &semi, nullptr, &acquired_);
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(device_, &fi, nullptr, &fence_);

    return create_swapchain();
}

bool VkPresenter::create_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps);
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return false;   // minimized
    extent_ = caps.currentExtent;

    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &fn, formats.data());
    VkSurfaceFormatKHR format = formats[0];
    for (auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) format = f;
    format_ = format.format;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = std::min(std::max(caps.minImageCount, 3u), caps.maxImageCount ? caps.maxImageCount : 8u);
    ci.imageFormat = format.format;
    ci.imageColorSpace = format.colorSpace;
    ci.imageExtent = extent_;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;   // vsync at the monitor's refresh rate
    if (!vsync_) {   // the first one the driver has: immediate (tearing), mailbox, or vsync after all
        uint32_t mn = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &mn, nullptr);
        std::vector<VkPresentModeKHR> modes(mn);
        vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &mn, modes.data());
        for (VkPresentModeKHR m : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR})
            if (ci.presentMode == VK_PRESENT_MODE_FIFO_KHR && std::find(modes.begin(), modes.end(), m) != modes.end())
                ci.presentMode = m;
    }
    if (ci.presentMode != present_mode_) {
        present_mode_ = ci.presentMode;
        printf("[video] vsync %s\n", present_mode_ == VK_PRESENT_MODE_FIFO_KHR ? "on"
                                    : present_mode_ == VK_PRESENT_MODE_IMMEDIATE_KHR ? "off (immediate)"
                                                                                     : "off (mailbox)");
    }
    ci.clipped = VK_TRUE;
    std::unique_lock<std::shared_mutex> lock(swapchain_mutex_);
    swapchain_first_id_ = present_id_ + 1;
    VkSwapchainKHR old = swapchain_;
    ci.oldSwapchain = old;
    if (!check(vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_), "vkCreateSwapchainKHR")) return false;
    for (uint32_t i = 0; i < image_count_; i++)
        if (views_[i]) vkDestroyImageView(device_, views_[i], nullptr);
    if (old) vkDestroySwapchainKHR(device_, old, nullptr);

    // The driver decides how many images the swapchain holds: ask, then fetch
    if (!check(vkGetSwapchainImagesKHR(device_, swapchain_, &image_count_, nullptr), "vkGetSwapchainImagesKHR")) return false;
    images_.assign(image_count_, VK_NULL_HANDLE);
    views_.assign(image_count_, VK_NULL_HANDLE);
    if (!check(vkGetSwapchainImagesKHR(device_, swapchain_, &image_count_, images_.data()), "vkGetSwapchainImagesKHR")) return false;
    if (rendered_.size() < image_count_) rendered_.resize(image_count_, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < image_count_; i++) {
        if (!rendered_[i]) vkCreateSemaphore(device_, &semi, nullptr, &rendered_[i]);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = images_[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(device_, &vi, nullptr, &views_[i]);
    }
    return true;
}

void VkPresenter::destroy_swapchain() {
    std::unique_lock<std::shared_mutex> lock(swapchain_mutex_);
    for (uint32_t i = 0; i < image_count_; i++)
        if (views_[i]) vkDestroyImageView(device_, views_[i], nullptr);
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

bool VkPresenter::pump() {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return !quit_;
}

void VkPresenter::run_once(const std::function<void(VkCommandBuffer)>& fn) {
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_, &cai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    fn(cmd);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, pool_, 1, &cmd);
}

void VkPresenter::frame(const RecordFn& record) {
    if (!device_) return;
    if (resized_) {
        vkDeviceWaitIdle(device_);
        if (!create_swapchain()) { Sleep(16); return; }
        resized_ = false;
    }
    if (!swapchain_) return;

    double t0 = now_seconds();
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    double t_fence = now_seconds();
    uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, acquired_, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { resized_ = true; return; }
    vkResetFences(device_, 1, &fence_);
    double t1 = now_seconds();

    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);
    vk_image_barrier(cmd_, images_[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    record(cmd_, views_[index], extent_);
    double t2 = now_seconds();
    bool capture = !capture_path_.empty();
    if (capture) {
        // Copy the finished image to a host-visible buffer
        if (capture_buffer_) {
            vkDestroyBuffer(device_, capture_buffer_, nullptr);
            vkFreeMemory(device_, capture_memory_, nullptr);
        }
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = VkDeviceSize(extent_.width) * extent_.height * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vkCreateBuffer(device_, &bci, nullptr, &capture_buffer_);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, capture_buffer_, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device_, &mai, nullptr, &capture_memory_);
        vkBindBufferMemory(device_, capture_buffer_, capture_memory_, 0);
        vk_image_barrier(cmd_, images_[index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {extent_.width, extent_.height, 1};
        vkCmdCopyImageToBuffer(cmd_, images_[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, capture_buffer_, 1, &c);
        vk_image_barrier(cmd_, images_[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    } else {
        vk_image_barrier(cmd_, images_[index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    vkEndCommandBuffer(cmd_);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquired_;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &rendered_[index];
    vkQueueSubmit(queue_, 1, &si, fence_);

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered_[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    VkPresentIdKHR pid{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
    uint64_t id = 0;
    if (present_wait_) {
        id = ++present_id_;
        pid.swapchainCount = 1;
        pid.pPresentIds = &id;
        pi.pNext = &pid;
    }
    r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) resized_ = true;
    double t3 = now_seconds();
    timing_.gpu_wait += t1 - t0;
    timing_.record += t2 - t1;
    timing_.present += t3 - t2;
    timing_.frames++;
    last_ = {t_fence - t0, t1 - t_fence, t2 - t1, t3 - t2, id};
    if (capture) {
        vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
        void* data = nullptr;
        vkMapMemory(device_, capture_memory_, 0, VK_WHOLE_SIZE, 0, &data);
        bmp_save(capture_path_, int(extent_.width), int(extent_.height), data);   // BGRA swapchain = BMP order
        vkUnmapMemory(device_, capture_memory_);
        capture_path_.clear();
    }
}

bool VkPresenter::wait_present(uint64_t id, uint64_t timeout_ns) {
    std::shared_lock<std::shared_mutex> lock(swapchain_mutex_);
    if (!present_wait_ || !swapchain_ || id < swapchain_first_id_) return false;
    VkResult r = vkWaitForPresentKHR(device_, swapchain_, id, timeout_ns);
    return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
}

double VkPresenter::limit_queue(int depth, uint64_t timeout_ns) {
    if (!present_wait_ || !swapchain_ || depth <= 0 || present_id_ < uint64_t(depth)) return 0;
    uint64_t id = present_id_ + 1 - uint64_t(depth);
    if (id < swapchain_first_id_ || vkWaitForPresentKHR(device_, swapchain_, id, timeout_ns) != VK_SUCCESS) return 0;
    return now_seconds();
}

void VkPresenter::shutdown() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        destroy_swapchain();
        for (VkSemaphore sem : rendered_)
            if (sem) vkDestroySemaphore(device_, sem, nullptr);
        if (acquired_) vkDestroySemaphore(device_, acquired_, nullptr);
        if (fence_) vkDestroyFence(device_, fence_, nullptr);
        if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);   // (and cmd_)
        if (capture_buffer_) vkDestroyBuffer(device_, capture_buffer_, nullptr);
        if (capture_memory_) vkFreeMemory(device_, capture_memory_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
    if (hwnd_) DestroyWindow(hwnd_);
}
