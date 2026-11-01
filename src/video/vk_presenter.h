// Win32 window + Vulkan 1.3 device + swapchain. Each frame calls a
// function that records the draw commands onto the swapchain image.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include "volk.h"

class VkPresenter {
public:
    using RecordFn = std::function<void(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent)>;

    // with_ngx: also enables the Vulkan extensions NVIDIA's NGX features (DLSS,
    // DLAA) need: their list is fixed at device creation
    bool init(const char* title, int width, int height, bool with_ngx = false);
    void shutdown();

    // Processes window messages; returns false once it is closed
    bool pump();
    // Records and presents a frame (with vsync unless set_vsync(false)). The image arrives in
    // COLOR_ATTACHMENT_OPTIMAL and must be left that way when done.
    void frame(const RecordFn& record);

    std::string device_name() const { return device_name_; }
    // The device has ray queries and acceleration structures (ray traced effects)
    bool ray_query() const { return ray_query_; }
    // What else the GPU offers, asked before creating the device (the renderer
    // adapts instead of failing): arrays of textures indexed per polygon
    // (descriptor indexing: the texture pack and the 2D replacements) and
    // anisotropic samplers
    bool descriptor_indexing() const { return descriptor_indexing_; }
    bool anisotropy() const { return anisotropy_; }
    const VkPhysicalDeviceLimits& limits() const { return limits_; }
    HWND hwnd() const { return hwnd_; }
    void toggle_fullscreen();
    bool fullscreen() const { return fullscreen_; }
    void close() { quit_ = true; }
    // Vsync off: each image goes out as soon as it is drawn, without waiting
    // for the monitor's refresh (tearing; mailbox if the driver has no
    // immediate mode). Takes effect with the next frame (a new swapchain)
    void set_vsync(bool on) {
        if (on != vsync_) vsync_ = on, resized_ = true;
    }

    // Sees key presses and mouse messages first; returning true swallows
    // them (Esc then no longer closes the window)
    std::function<bool(UINT msg, WPARAM wp, LPARAM lp)> input_hook;
    // WM_INPUT (raw input registered by the owner), with its HRAWINPUT
    std::function<void(HRAWINPUT input)> raw_input;
    // Arrow pointer over the window instead of none (menus); takes effect at
    // once, without waiting for the mouse to move
    void set_cursor_visible(bool visible);

    VkInstance instance() const { return instance_; }
    VkDevice device() const { return device_; }
    VkPhysicalDevice gpu() const { return gpu_; }
    VkQueue queue() const { return queue_; }
    VkCommandPool pool() const { return pool_; }
    VkFormat swap_format() const { return format_; }
    // Compiled pipelines kept between runs (save/pipeline_cache.bin): every
    // vkCreateGraphicsPipelines goes through it. save_pipeline_cache() writes
    // the file if it grew (after start-up and from shutdown())
    VkPipelineCache pipeline_cache() const { return pipeline_cache_; }
    void save_pipeline_cache();
    uint32_t find_memory(uint32_t type_bits, VkMemoryPropertyFlags flags) const;
    // Runs one-off commands (initial uploads) and waits
    void run_once(const std::function<void(VkCommandBuffer)>& fn);
    // Saves the next presented frame as a BMP file
    void capture_next(const std::string& path) { capture_path_ = path; }

    // Where the time of frame() goes, in seconds, summed since the last take_timing():
    // gpu_wait: waiting for the previous frame's GPU work and for a swapchain image;
    // record: the record callback (CPU); present: vkQueuePresentKHR (vsync may block here)
    struct Timing {
        double gpu_wait = 0, record = 0, present = 0;
        int frames = 0;
    };
    Timing take_timing() {
        Timing t = timing_;
        timing_ = Timing{};
        return t;
    }
    // The last frame() alone, in seconds (debugging aid, LW_FRAMELOG): waits
    // for the previous frame's GPU work (fence) and for a swapchain image
    // (acquire), and the id its present was given (0 without VK_KHR_present_id)
    struct LastFrame {
        double fence = 0, acquire = 0, record = 0, present = 0;
        uint64_t present_id = 0;
    };
    const LastFrame& last_frame() const { return last_; }
    // VK_KHR_present_wait: blocks until the present with that id is on screen
    // (or the timeout, in ns, runs out); false if it could not be waited for.
    // Safe to call from another thread
    bool wait_present(uint64_t id, uint64_t timeout_ns);
    bool can_wait_present() const { return present_wait_; }
    // Keeps at most `depth` images queued for the screen: waits until the one
    // presented `depth` frames before the next is on screen, so what is drawn
    // next goes out `depth` refreshes later (less latency than a full
    // swapchain, and a steady one). Needs VK_KHR_present_wait; same thread as
    // frame(). Returns when it saw that image go out (seconds()), 0 if it did not wait
    double limit_queue(int depth, uint64_t timeout_ns);

private:
    bool create_swapchain();
    void destroy_swapchain();
    static LRESULT CALLBACK wndproc(HWND, UINT, WPARAM, LPARAM);

    HWND hwnd_ = nullptr;
    bool quit_ = false, resized_ = false, fullscreen_ = false, show_cursor_ = false, vsync_ = true;
    WINDOWPLACEMENT placement_{sizeof(WINDOWPLACEMENT)};

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};   // the GPU's memory types
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::string device_name_;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent_{};
    uint32_t image_count_ = 0;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;

    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkSemaphore acquired_ = VK_NULL_HANDLE;
    std::vector<VkSemaphore> rendered_;
    VkFence fence_ = VK_NULL_HANDLE;

    std::string capture_path_;
    Timing timing_;
    LastFrame last_;
    bool present_wait_ = false;
    VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_MAX_ENUM_KHR;   // of the swapchain (printed when it changes)
    bool ray_query_ = false, descriptor_indexing_ = false, anisotropy_ = false;
    bool khr_dynamic_rendering_ = false;   // a Vulkan 1.2 driver: dynamic rendering through the extension
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    size_t pipeline_cache_saved_ = 0;   // bytes of the file as last loaded or written
    VkPhysicalDeviceLimits limits_{};
    uint64_t present_id_ = 0, swapchain_first_id_ = 1;   // the first id presented to the current swapchain
    std::shared_mutex swapchain_mutex_;   // wait_present() against the swapchain being replaced
    VkBuffer capture_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory capture_memory_ = VK_NULL_HANDLE;
};

// Simple image barrier (all stages)
void vk_image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                      VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, uint32_t layers = 1);
