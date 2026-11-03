#include <jni.h>
#include <vulkan/vulkan.h>

#include <sstream>
#include <string>
#include <vector>

extern "C" JNIEXPORT jstring JNICALL
Java_com_ravi_gtavnative_MainActivity_nativeVulkanInfo(JNIEnv* env, jobject) {
    uint32_t instanceVersion = VK_API_VERSION_1_0;
    auto getInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));

    if (getInstanceVersion != nullptr) {
        getInstanceVersion(&instanceVersion);
    }

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "GTA V Native Android";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "Independent Android Runtime";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = instanceVersion;

    VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    createInfo.pApplicationInfo = &appInfo;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&createInfo, nullptr, &instance);

    std::ostringstream out;
    out << "Vulkan loader: " << VK_VERSION_MAJOR(instanceVersion) << "."
        << VK_VERSION_MINOR(instanceVersion) << "."
        << VK_VERSION_PATCH(instanceVersion) << "\n";

    if (result != VK_SUCCESS) {
        out << "Vulkan instance creation failed (VkResult " << result << ").\n"
            << "This device/driver may not expose a usable Vulkan implementation.";
        return env->NewStringUTF(out.str().c_str());
    }

    uint32_t deviceCount = 0;
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (result != VK_SUCCESS || deviceCount == 0) {
        out << "No Vulkan physical device found (VkResult " << result << ").";
    } else {
        std::vector<VkPhysicalDevice> devices(deviceCount);
        result = vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
        if (result == VK_SUCCESS) {
            out << "Detected " << deviceCount << " Vulkan device(s):\n";
            for (const auto device : devices) {
                VkPhysicalDeviceProperties properties{};
                vkGetPhysicalDeviceProperties(device, &properties);
                out << "• " << properties.deviceName << " — Vulkan "
                    << VK_VERSION_MAJOR(properties.apiVersion) << "."
                    << VK_VERSION_MINOR(properties.apiVersion) << "."
                    << VK_VERSION_PATCH(properties.apiVersion) << "\n";
            }
        } else {
            out << "Device enumeration failed (VkResult " << result << ").";
        }
    }

    vkDestroyInstance(instance, nullptr);
    return env->NewStringUTF(out.str().c_str());
}
