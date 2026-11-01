#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {
std::mutex g_window_mutex;
ANativeWindow* g_window = nullptr;

std::string probe_vulkan() {
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "GTA V Android Port";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "Android port probe";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    const char* instance_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    create_info.enabledExtensionCount = 2;
    create_info.ppEnabledExtensionNames = instance_extensions;

    VkInstance instance = VK_NULL_HANDLE;
    const VkResult created = vkCreateInstance(&create_info, nullptr, &instance);
    if (created != VK_SUCCESS) {
        return "Vulkan instance creation failed (VkResult " +
               std::to_string(static_cast<int>(created)) + ").";
    }

    ANativeWindow* window = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_window_mutex);
        if (g_window) { window = g_window; ANativeWindow_acquire(window); }
    }
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (window) {
        VkAndroidSurfaceCreateInfoKHR surface_info{};
        surface_info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        surface_info.window = window;
        const VkResult surface_result = vkCreateAndroidSurfaceKHR(instance, &surface_info, nullptr, &surface);
        ANativeWindow_release(window);
        if (surface_result != VK_SUCCESS) {
            vkDestroyInstance(instance, nullptr);
            return "Android Vulkan surface creation failed (VkResult " +
                   std::to_string(static_cast<int>(surface_result)) + ").";
        }
    }

    uint32_t count = 0;
    VkResult result = vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (result != VK_SUCCESS || count == 0) {
        if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
        return "Vulkan is present, but no physical GPU was enumerated.";
    }
    std::vector<VkPhysicalDevice> devices(count);
    result = vkEnumeratePhysicalDevices(instance, &count, devices.data());
    if (result != VK_SUCCESS) {
        if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
        return "Vulkan GPU enumeration failed (VkResult " +
               std::to_string(static_cast<int>(result)) + ").";
    }

    std::ostringstream report;
    report << "Vulkan instance: OK\nPhysical devices: " << count << "\n";
    bool graphics_found = false;
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(devices[i], &properties);
        uint32_t queue_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &queue_count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queue_count);
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &queue_count, queues.data());
        int graphics_family = -1;
        int present_family = -1;
        for (uint32_t q = 0; q < queue_count; ++q) {
            if (!queues[q].queueCount) continue;
            if (queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) graphics_family = static_cast<int>(q);
            if (surface) {
                VkBool32 supported = VK_FALSE;
                if (vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], q, surface, &supported) == VK_SUCCESS && supported)
                    present_family = static_cast<int>(q);
            }
        }
        uint32_t ext_count = 0;
        bool swapchain = false;
        if (vkEnumerateDeviceExtensionProperties(devices[i], nullptr, &ext_count, nullptr) == VK_SUCCESS) {
            std::vector<VkExtensionProperties> exts(ext_count);
            if (vkEnumerateDeviceExtensionProperties(devices[i], nullptr, &ext_count, exts.data()) == VK_SUCCESS) {
                for (const auto& ext : exts) {
                    if (std::string(ext.extensionName) == VK_KHR_SWAPCHAIN_EXTENSION_NAME) {
                        swapchain = true;
                        break;
                    }
                }
            }
        }
        report << "\nGPU " << (i + 1) << ": " << properties.deviceName << "\n"
               << "Vulkan API: " << VK_VERSION_MAJOR(properties.apiVersion) << "."
               << VK_VERSION_MINOR(properties.apiVersion) << "."
               << VK_VERSION_PATCH(properties.apiVersion) << "\n"
               << "Graphics queue: " << (graphics_family >= 0 ? "available" : "not found") << "\n"
               << "Android surface: " << (surface ? "created" : "waiting for SurfaceView") << "\n"
               << "Present queue: " << (present_family >= 0 ? "available" : (surface ? "not found" : "not tested")) << "\n"
               << "VK_KHR_swapchain: " << (swapchain ? "available" : "not available") << "\n";
        graphics_found |= graphics_family >= 0;
    }
    if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    if (!graphics_found) report << "\nNo graphics-capable queue was found.";
    return report.str();
}

std::string surface_status() {
    std::lock_guard<std::mutex> lock(g_window_mutex);
    if (!g_window) return "Android native window: waiting for SurfaceView.";
    return "Android native window: connected (" +
           std::to_string(ANativeWindow_getWidth(g_window)) + " x " +
           std::to_string(ANativeWindow_getHeight(g_window)) + ").";
}
}  // namespace

extern "C" JNIEXPORT void JNICALL
Java_org_macnativegames_gtavandroid_MainActivity_nativeSetSurface(
    JNIEnv* env, jobject /* thiz */, jobject surface) {
    ANativeWindow* next = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    std::lock_guard<std::mutex> lock(g_window_mutex);
    if (g_window) ANativeWindow_release(g_window);
    g_window = next;
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_macnativegames_gtavandroid_MainActivity_nativePortStatus(
    JNIEnv* env, jobject /* thiz */) {
    const std::string status =
        "Android ARM64 native shell is active.\n"
        "Game runtime is not connected.\n\n" +
        surface_status() + "\n\n" + probe_vulkan() +
        "\n\nProbe only: Vulkan surface/swapchain, NVN renderer, and GTA runtime are not connected.";
    return env->NewStringUTF(status.c_str());
}
