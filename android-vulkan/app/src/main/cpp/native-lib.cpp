#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

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


extern "C" JNIEXPORT jstring JNICALL
Java_com_ravi_gtavnative_MainActivity_nativeRenderFrame(
    JNIEnv* env, jobject, jobject javaSurface, jint requestedWidth, jint requestedHeight) {
    ANativeWindow* window = ANativeWindow_fromSurface(env, javaSurface);
    if (window == nullptr) return env->NewStringUTF("Vulkan frame: Android window unavailable.");

    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkImageView> imageViews;
    std::vector<VkImage> images;
    std::string message = "Vulkan frame presentation failed.";

    auto cleanup = [&]() {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            for (auto framebuffer : framebuffers) vkDestroyFramebuffer(device, framebuffer, nullptr);
            for (auto view : imageViews) vkDestroyImageView(device, view, nullptr);
            if (renderPass != VK_NULL_HANDLE) vkDestroyRenderPass(device, renderPass, nullptr);
            if (commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(device, commandPool, nullptr);
            if (imageAvailable != VK_NULL_HANDLE) vkDestroySemaphore(device, imageAvailable, nullptr);
            if (renderFinished != VK_NULL_HANDLE) vkDestroySemaphore(device, renderFinished, nullptr);
            if (swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(device, swapchain, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance, surface, nullptr);
        if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
        ANativeWindow_release(window);
    };
    auto fail = [&](const char* step, VkResult result) {
        std::ostringstream out;
        out << step << " (VkResult " << result << ")";
        message = out.str();
        cleanup();
        return env->NewStringUTF(message.c_str());
    };

    if (!hasInstanceExtension(VK_KHR_SURFACE_EXTENSION_NAME) ||
        !hasInstanceExtension(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) {
        cleanup();
        return env->NewStringUTF("Vulkan Android surface extensions unavailable.");
    }

    const char* instanceExtensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
    };
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Independent Android Vulkan Renderer";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo instanceCreate{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceCreate.pApplicationInfo = &app;
    instanceCreate.enabledExtensionCount = 2;
    instanceCreate.ppEnabledExtensionNames = instanceExtensions;
    VkResult result = vkCreateInstance(&instanceCreate, nullptr, &instance);
    if (result != VK_SUCCESS) return fail("vkCreateInstance", result);

    auto createSurface = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(
        vkGetInstanceProcAddr(instance, "vkCreateAndroidSurfaceKHR"));
    if (createSurface == nullptr) {
        cleanup();
        return env->NewStringUTF("vkCreateAndroidSurfaceKHR entry point missing.");
    }
    VkAndroidSurfaceCreateInfoKHR androidInfo{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    androidInfo.window = window;
    result = createSurface(instance, &androidInfo, nullptr, &surface);
    if (result != VK_SUCCESS) return fail("vkCreateAndroidSurfaceKHR", result);

    uint32_t physicalCount = 0;
    result = vkEnumeratePhysicalDevices(instance, &physicalCount, nullptr);
    if (result != VK_SUCCESS || physicalCount == 0)
        return fail("No Vulkan physical device", result);

    std::vector<VkPhysicalDevice> physicalDevices(physicalCount);
    result = vkEnumeratePhysicalDevices(instance, &physicalCount, physicalDevices.data());
    if (result != VK_SUCCESS) return fail("vkEnumeratePhysicalDevices", result);

    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t queueFamily = UINT32_MAX;
    for (auto candidate : physicalDevices) {
        if (!hasDeviceExtension(candidate, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());
        for (uint32_t i = 0; i < count; ++i) {
            VkBool32 present = VK_FALSE;
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &present) == VK_SUCCESS &&
                present == VK_TRUE) {
                physical = candidate;
                queueFamily = i;
                break;
            }
        }
        if (physical != VK_NULL_HANDLE) break;
    }
    if (physical == VK_NULL_HANDLE) {
        cleanup();
        return env->NewStringUTF("No graphics queue supports Android surface + VK_KHR_swapchain.");
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueCreate{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueCreate.queueFamilyIndex = queueFamily;
    queueCreate.queueCount = 1;
    queueCreate.pQueuePriorities = &priority;
    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo deviceCreate{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceCreate.queueCreateInfoCount = 1;
    deviceCreate.pQueueCreateInfos = &queueCreate;
    deviceCreate.enabledExtensionCount = 1;
    deviceCreate.ppEnabledExtensionNames = deviceExtensions;
    result = vkCreateDevice(physical, &deviceCreate, nullptr, &device);
    if (result != VK_SUCCESS) return fail("vkCreateDevice", result);
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    VkSurfaceCapabilitiesKHR caps{};
    result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
    if (result != VK_SUCCESS) return fail("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result);
    uint32_t formatCount = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, nullptr);
    if (result != VK_SUCCESS || formatCount == 0) return fail("No surface formats", result);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, formats.data());
    if (result != VK_SUCCESS) return fail("vkGetPhysicalDeviceSurfaceFormatsKHR", result);
    VkSurfaceFormatKHR chosenFormat = formats[0];
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_R8G8B8A8_UNORM ||
            format.format == VK_FORMAT_B8G8R8A8_UNORM) {
            chosenFormat = format;
            break;
        }
    }
    VkExtent2D extent{};
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    } else {
        extent.width = std::max(caps.minImageExtent.width,
            std::min(caps.maxImageExtent.width, static_cast<uint32_t>(std::max(1, requestedWidth))));
        extent.height = std::max(caps.minImageExtent.height,
            std::min(caps.maxImageExtent.height, static_cast<uint32_t>(std::max(1, requestedHeight))));
    }
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);
    VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    const VkCompositeAlphaFlagBitsKHR alphaOptions[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
    };
    for (auto option : alphaOptions) {
        if (caps.supportedCompositeAlpha & option) { composite = option; break; }
    }
    VkSwapchainCreateInfoKHR swapCreate{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swapCreate.surface = surface;
    swapCreate.minImageCount = imageCount;
    swapCreate.imageFormat = chosenFormat.format;
    swapCreate.imageColorSpace = chosenFormat.colorSpace;
    swapCreate.imageExtent = extent;
    swapCreate.imageArrayLayers = 1;
    swapCreate.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapCreate.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapCreate.preTransform = caps.currentTransform;
    swapCreate.compositeAlpha = composite;
    swapCreate.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapCreate.clipped = VK_TRUE;
    result = vkCreateSwapchainKHR(device, &swapCreate, nullptr, &swapchain);
    if (result != VK_SUCCESS) return fail("vkCreateSwapchainKHR", result);

    uint32_t actualImageCount = 0;
    result = vkGetSwapchainImagesKHR(device, swapchain, &actualImageCount, nullptr);
    if (result != VK_SUCCESS || actualImageCount == 0) return fail("vkGetSwapchainImagesKHR", result);
    images.resize(actualImageCount);
    result = vkGetSwapchainImagesKHR(device, swapchain, &actualImageCount, images.data());
    if (result != VK_SUCCESS) return fail("vkGetSwapchainImagesKHR(images)", result);

    VkAttachmentDescription attachment{};
    attachment.format = chosenFormat.format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo renderCreate{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    renderCreate.attachmentCount = 1;
    renderCreate.pAttachments = &attachment;
    renderCreate.subpassCount = 1;
    renderCreate.pSubpasses = &subpass;
    renderCreate.dependencyCount = 1;
    renderCreate.pDependencies = &dependency;
    result = vkCreateRenderPass(device, &renderCreate, nullptr, &renderPass);
    if (result != VK_SUCCESS) return fail("vkCreateRenderPass", result);

    framebuffers.resize(images.size(), VK_NULL_HANDLE);
    imageViews.resize(images.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < images.size(); ++i) {
        VkImageViewCreateInfo viewCreate{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewCreate.image = images[i];
        viewCreate.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewCreate.format = chosenFormat.format;
        viewCreate.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                  VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        viewCreate.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewCreate.subresourceRange.levelCount = 1;
        viewCreate.subresourceRange.layerCount = 1;
        VkImageView view = VK_NULL_HANDLE;
        result = vkCreateImageView(device, &viewCreate, nullptr, &view);
        if (result != VK_SUCCESS) return fail("vkCreateImageView", result);
        imageViews[i] = view;
        VkFramebufferCreateInfo fbCreate{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbCreate.renderPass = renderPass;
        fbCreate.attachmentCount = 1;
        fbCreate.pAttachments = &view;
        fbCreate.width = extent.width;
        fbCreate.height = extent.height;
        fbCreate.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        result = vkCreateFramebuffer(device, &fbCreate, nullptr, &framebuffer);
        if (result != VK_SUCCESS) return fail("vkCreateFramebuffer", result);
        framebuffers[i] = framebuffer;
    }

    VkCommandPoolCreateInfo poolCreate{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolCreate.queueFamilyIndex = queueFamily;
    poolCreate.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    result = vkCreateCommandPool(device, &poolCreate, nullptr, &commandPool);
    if (result != VK_SUCCESS) return fail("vkCreateCommandPool", result);
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = commandPool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    result = vkAllocateCommandBuffers(device, &allocate, &commandBuffer);
    if (result != VK_SUCCESS) return fail("vkAllocateCommandBuffers", result);
    VkSemaphoreCreateInfo semaphoreCreate{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    result = vkCreateSemaphore(device, &semaphoreCreate, nullptr, &imageAvailable);
    if (result != VK_SUCCESS) return fail("vkCreateSemaphore(imageAvailable)", result);
    result = vkCreateSemaphore(device, &semaphoreCreate, nullptr, &renderFinished);
    if (result != VK_SUCCESS) return fail("vkCreateSemaphore(renderFinished)", result);

    uint32_t imageIndex = 0;
    result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, imageAvailable,
                                   VK_NULL_HANDLE, &imageIndex);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        return fail("vkAcquireNextImageKHR", result);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(commandBuffer, &begin);
    if (result != VK_SUCCESS) return fail("vkBeginCommandBuffer", result);
    VkClearValue clear{};
    clear.color = {{0.035f, 0.055f, 0.085f, 1.0f}};
    VkRenderPassBeginInfo renderBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    renderBegin.renderPass = renderPass;
    renderBegin.framebuffer = framebuffers[imageIndex];
    renderBegin.renderArea.extent = extent;
    renderBegin.clearValueCount = 1;
    renderBegin.pClearValues = &clear;
    vkCmdBeginRenderPass(commandBuffer, &renderBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(commandBuffer);
    result = vkEndCommandBuffer(commandBuffer);
    if (result != VK_SUCCESS) return fail("vkEndCommandBuffer", result);

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &imageAvailable;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderFinished;
    result = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) return fail("vkQueueSubmit", result);
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &imageIndex;
    result = vkQueuePresentKHR(queue, &present);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        return fail("vkQueuePresentKHR", result);
    vkQueueWaitIdle(queue);

    std::ostringstream success;
    success << "Vulkan swapchain active: " << extent.width << " x " << extent.height
            << "\nOne clear-color frame submitted and presented successfully.";
    message = success.str();
    cleanup();
    return env->NewStringUTF(message.c_str());
}
