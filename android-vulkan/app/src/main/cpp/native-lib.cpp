#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool hasInstanceExtension(const char* wanted) {
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()) != VK_SUCCESS) return false;
    return std::any_of(extensions.begin(), extensions.end(), [wanted](const auto& ext) {
        return std::string(ext.extensionName) == wanted;
    });
}

bool hasDeviceExtension(VkPhysicalDevice device, const char* wanted) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data()) != VK_SUCCESS) return false;
    return std::any_of(extensions.begin(), extensions.end(), [wanted](const auto& ext) {
        return std::string(ext.extensionName) == wanted;
    });
}

std::string inspectSurface(ANativeWindow* window) {
    std::ostringstream out;
    const char* surfaceExt = VK_KHR_SURFACE_EXTENSION_NAME;
    const char* androidExt = VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;

    if (!hasInstanceExtension(surfaceExt) || !hasInstanceExtension(androidExt)) {
        out << "Vulkan Android surface extensions are unavailable.\n"
            << "Required: VK_KHR_surface + VK_KHR_android_surface.";
        return out.str();
    }

    const char* enabledExtensions[] = {surfaceExt, androidExt};
    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "GTA V Native Android";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "Independent Android Runtime";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = 2;
    instanceInfo.ppEnabledExtensionNames = enabledExtensions;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance);
    if (result != VK_SUCCESS) {
        out << "Vulkan instance creation failed: " << result;
        return out.str();
    }

    auto createAndroidSurface = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(
        vkGetInstanceProcAddr(instance, "vkCreateAndroidSurfaceKHR"));
    if (createAndroidSurface == nullptr) {
        out << "vkCreateAndroidSurfaceKHR entry point is missing.";
        vkDestroyInstance(instance, nullptr);
        return out.str();
    }

    VkAndroidSurfaceCreateInfoKHR androidInfo{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    androidInfo.window = window;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    result = createAndroidSurface(instance, &androidInfo, nullptr, &surface);
    if (result != VK_SUCCESS) {
        out << "Android Vulkan surface creation failed: " << result;
        vkDestroyInstance(instance, nullptr);
        return out.str();
    }

    uint32_t deviceCount = 0;
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (result != VK_SUCCESS || deviceCount == 0) {
        out << "No Vulkan physical device found: " << result;
    } else {
        std::vector<VkPhysicalDevice> devices(deviceCount);
        result = vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
        if (result != VK_SUCCESS) {
            out << "Vulkan device enumeration failed: " << result;
        } else {
            out << "Android Vulkan surface created successfully.\n";
            out << "Detected " << deviceCount << " Vulkan device(s):\n";
            for (const auto device : devices) {
                VkPhysicalDeviceProperties properties{};
                vkGetPhysicalDeviceProperties(device, &properties);
                uint32_t familyCount = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
                std::vector<VkQueueFamilyProperties> families(familyCount);
                vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());

                bool canPresent = false;
                for (uint32_t i = 0; i < familyCount; ++i) {
                    VkBool32 supported = VK_FALSE;
                    if (vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &supported) == VK_SUCCESS &&
                        supported == VK_TRUE && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                        canPresent = true;
                        break;
                    }
                }
                const bool swapchain = hasDeviceExtension(device, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
                out << properties.deviceName << "\n"
                    << "  Graphics + surface presentation: " << (canPresent ? "YES" : "NO") << "\n"
                    << "  VK_KHR_swapchain: " << (swapchain ? "YES" : "NO") << "\n";
            }
            out << "\nThis is a capability test; swapchain rendering is the next milestone.";
        }
    }

    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    return out.str();
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_ravi_gtavnative_MainActivity_nativeVulkanInfo(JNIEnv* env, jobject) {
    uint32_t version = VK_API_VERSION_1_0;
    auto getVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    if (getVersion != nullptr) getVersion(&version);

    std::ostringstream out;
    out << "Vulkan loader: " << VK_VERSION_MAJOR(version) << "."
        << VK_VERSION_MINOR(version) << "." << VK_VERSION_PATCH(version);
    return env->NewStringUTF(out.str().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_ravi_gtavnative_MainActivity_nativeInspectSurface(JNIEnv* env, jobject, jobject javaSurface) {
    ANativeWindow* window = ANativeWindow_fromSurface(env, javaSurface);
    if (window == nullptr) {
        return env->NewStringUTF("Could not acquire Android native window.");
    }
    const std::string result = inspectSurface(window);
    ANativeWindow_release(window);
    return env->NewStringUTF(result.c_str());
}
