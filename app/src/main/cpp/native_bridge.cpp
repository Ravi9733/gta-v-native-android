#include <jni.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace {

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

    VkInstance instance = VK_NULL_HANDLE;
    const VkResult created = vkCreateInstance(&create_info, nullptr, &instance);
    if (created != VK_SUCCESS) {
        return "Vulkan instance creation failed (VkResult " +
               std::to_string(static_cast<int>(created)) + ").";
    }

    uint32_t physical_count = 0;
    VkResult result = vkEnumeratePhysicalDevices(instance, &physical_count, nullptr);
    if (result != VK_SUCCESS || physical_count == 0) {
        vkDestroyInstance(instance, nullptr);
        return "Vulkan is present, but no physical Vulkan GPU was enumerated.";
    }

    std::vector<VkPhysicalDevice> devices(physical_count);
    result = vkEnumeratePhysicalDevices(instance, &physical_count, devices.data());
    if (result != VK_SUCCESS) {
        vkDestroyInstance(instance, nullptr);
        return "Vulkan GPU enumeration failed (VkResult " +
               std::to_string(static_cast<int>(result)) + ").";
    }

    std::ostringstream report;
    report << "Vulkan instance: OK\n";
    report << "Physical devices: " << physical_count << "\n";

    bool found_graphics = false;
    for (uint32_t index = 0; index < physical_count; ++index) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(devices[index], &properties);

        uint32_t queue_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &queue_count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queue_count);
        vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &queue_count, queues.data());

        int32_t graphics_family = -1;
        for (uint32_t q = 0; q < queue_count; ++q) {
            if ((queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
                queues[q].queueCount > 0) {
                graphics_family = static_cast<int32_t>(q);
                break;
            }
        }

        uint32_t extension_count = 0;
        const VkResult ext_result = vkEnumerateDeviceExtensionProperties(
            devices[index], nullptr, &extension_count, nullptr);
        bool has_swapchain = false;
        if (ext_result == VK_SUCCESS) {
            std::vector<VkExtensionProperties> extensions(extension_count);
            if (vkEnumerateDeviceExtensionProperties(
                    devices[index], nullptr, &extension_count, extensions.data()) == VK_SUCCESS) {
                for (const auto& extension : extensions) {
                    if (std::string(extension.extensionName) == VK_KHR_SWAPCHAIN_EXTENSION_NAME) {
                        has_swapchain = true;
                        break;
                    }
                }
            }
        }

        report << "\nGPU " << (index + 1) << ": " << properties.deviceName << "\n";
        report << "Vulkan API: " << VK_VERSION_MAJOR(properties.apiVersion) << "."
               << VK_VERSION_MINOR(properties.apiVersion) << "."
               << VK_VERSION_PATCH(properties.apiVersion) << "\n";
        report << "Graphics queue: "
               << (graphics_family >= 0 ? "available (family " +
                    std::to_string(graphics_family) + ")" : "not found") << "\n";
        report << "VK_KHR_swapchain: " << (has_swapchain ? "available" : "not available") << "\n";
        found_graphics = found_graphics || graphics_family >= 0;
    }

    vkDestroyInstance(instance, nullptr);
    report << "\nProbe only: no Android surface, Vulkan device, NVN renderer, or game runtime is connected.";
    if (!found_graphics) {
        report << "\nNo graphics-capable Vulkan queue was found.";
    }
    return report.str();
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_org_macnativegames_gtavandroid_MainActivity_nativePortStatus(
    JNIEnv* env, jobject /* thiz */) {
    constexpr char kHeader[] =
        "Android ARM64 native shell is active.\n"
        "Game runtime is not connected.\n\n";
    const std::string status = std::string(kHeader) + probe_vulkan();
    return env->NewStringUTF(status.c_str());
}
