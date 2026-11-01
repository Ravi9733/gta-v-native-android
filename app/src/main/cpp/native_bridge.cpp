#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {
std::mutex g_window_mutex;
ANativeWindow* g_window = nullptr;

std::string clear_present_test(VkPhysicalDevice physical, VkSurfaceKHR surface,
                               uint32_t graphics_family, uint32_t present_family,
                               uint32_t width, uint32_t height) {
    float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    VkDeviceQueueCreateInfo graphics_queue{};
    graphics_queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    graphics_queue.queueFamilyIndex = graphics_family;
    graphics_queue.queueCount = 1;
    graphics_queue.pQueuePriorities = &priority;
    queue_infos.push_back(graphics_queue);
    if (present_family != graphics_family) {
        VkDeviceQueueCreateInfo present_queue = graphics_queue;
        present_queue.queueFamilyIndex = present_family;
        queue_infos.push_back(present_queue);
    }

    const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
    device_info.pQueueCreateInfos = queue_infos.data();
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = device_extensions;

    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkSemaphore image_ready = VK_NULL_HANDLE;
    VkSemaphore render_done = VK_NULL_HANDLE;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    std::string outcome = "Vulkan device creation failed";
    VkResult result = vkCreateDevice(physical, &device_info, nullptr, &device);
    if (result != VK_SUCCESS) return outcome + " (VkResult " + std::to_string(result) + ").";

    do {
        VkSurfaceCapabilitiesKHR caps{};
        result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
        if (result != VK_SUCCESS) { outcome = "Surface capability query failed"; break; }

        uint32_t format_count = 0;
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, nullptr);
        if (result != VK_SUCCESS || format_count == 0) { outcome = "No Vulkan surface formats"; break; }
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, formats.data());
        if (result != VK_SUCCESS) { outcome = "Surface format query failed"; break; }
        VkSurfaceFormatKHR chosen = formats.front();
        for (const auto& format : formats) {
            if (format.format == VK_FORMAT_R8G8B8A8_UNORM ||
                format.format == VK_FORMAT_B8G8R8A8_UNORM) { chosen = format; break; }
        }

        VkExtent2D extent = caps.currentExtent;
        if (extent.width == UINT32_MAX) {
            extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (extent.width == 0 || extent.height == 0) { outcome = "Surface has zero drawable size"; break; }

        uint32_t image_count = caps.minImageCount + 1;
        if (caps.maxImageCount && image_count > caps.maxImageCount) image_count = caps.maxImageCount;
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if (!(caps.supportedCompositeAlpha & alpha)) {
            const VkCompositeAlphaFlagBitsKHR choices[] = {
                VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
            };
            for (auto choice : choices) if (caps.supportedCompositeAlpha & choice) { alpha = choice; break; }
        }

        VkSwapchainCreateInfoKHR swap_info{};
        swap_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        swap_info.surface = surface;
        swap_info.minImageCount = image_count;
        swap_info.imageFormat = chosen.format;
        swap_info.imageColorSpace = chosen.colorSpace;
        swap_info.imageExtent = extent;
        swap_info.imageArrayLayers = 1;
        swap_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        uint32_t families[] = {graphics_family, present_family};
        if (graphics_family != present_family) {
            swap_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            swap_info.queueFamilyIndexCount = 2;
            swap_info.pQueueFamilyIndices = families;
        } else {
            swap_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }
        swap_info.preTransform = caps.currentTransform;
        swap_info.compositeAlpha = alpha;
        swap_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        swap_info.clipped = VK_TRUE;
        result = vkCreateSwapchainKHR(device, &swap_info, nullptr, &swapchain);
        if (result != VK_SUCCESS) { outcome = "Swapchain creation failed"; break; }

        uint32_t actual_count = 0;
        result = vkGetSwapchainImagesKHR(device, swapchain, &actual_count, nullptr);
        if (result != VK_SUCCESS || actual_count == 0) { outcome = "Swapchain image query failed"; break; }
        std::vector<VkImage> images(actual_count);
        result = vkGetSwapchainImagesKHR(device, swapchain, &actual_count, images.data());
        if (result != VK_SUCCESS) { outcome = "Swapchain image retrieval failed"; break; }

        for (const auto& image : images) {
            VkImageViewCreateInfo view_info{};
            view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_info.image = image;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = chosen.format;
            view_info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                     VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
            view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view_info.subresourceRange.levelCount = 1;
            view_info.subresourceRange.layerCount = 1;
            VkImageView view = VK_NULL_HANDLE;
            result = vkCreateImageView(device, &view_info, nullptr, &view);
            if (result != VK_SUCCESS) { outcome = "Swapchain image-view creation failed"; break; }
            views.push_back(view);
        }
        if (result != VK_SUCCESS) break;

        VkAttachmentDescription attachment{};
        attachment.format = chosen.format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_ref;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo pass_info{};
        pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        pass_info.attachmentCount = 1;
        pass_info.pAttachments = &attachment;
        pass_info.subpassCount = 1;
        pass_info.pSubpasses = &subpass;
        pass_info.dependencyCount = 1;
        pass_info.pDependencies = &dependency;
        result = vkCreateRenderPass(device, &pass_info, nullptr, &render_pass);
        if (result != VK_SUCCESS) { outcome = "Clear render-pass creation failed"; break; }

        for (const auto view : views) {
            VkFramebufferCreateInfo fb_info{};
            fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fb_info.renderPass = render_pass;
            fb_info.attachmentCount = 1;
            fb_info.pAttachments = &view;
            fb_info.width = extent.width;
            fb_info.height = extent.height;
            fb_info.layers = 1;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            result = vkCreateFramebuffer(device, &fb_info, nullptr, &framebuffer);
            if (result != VK_SUCCESS) { outcome = "Framebuffer creation failed"; break; }
            framebuffers.push_back(framebuffer);
        }
        if (result != VK_SUCCESS) break;

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = graphics_family;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        result = vkCreateCommandPool(device, &pool_info, nullptr, &command_pool);
        if (result != VK_SUCCESS) { outcome = "Command-pool creation failed"; break; }

        VkSemaphoreCreateInfo semaphore_info{};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        result = vkCreateSemaphore(device, &semaphore_info, nullptr, &image_ready);
        if (result != VK_SUCCESS) { outcome = "Acquire semaphore creation failed"; break; }
        result = vkCreateSemaphore(device, &semaphore_info, nullptr, &render_done);
        if (result != VK_SUCCESS) { outcome = "Present semaphore creation failed"; break; }

        VkQueue graphics_queue_handle = VK_NULL_HANDLE;
        VkQueue present_queue_handle = VK_NULL_HANDLE;
        vkGetDeviceQueue(device, graphics_family, 0, &graphics_queue_handle);
        vkGetDeviceQueue(device, present_family, 0, &present_queue_handle);
        uint32_t image_index = 0;
        result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, image_ready, VK_NULL_HANDLE, &image_index);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) { outcome = "Swapchain image acquire failed"; break; }

        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = command_pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;
        result = vkAllocateCommandBuffers(device, &alloc_info, &command);
        if (result != VK_SUCCESS) { outcome = "Command-buffer allocation failed"; break; }
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command, &begin_info);
        if (result != VK_SUCCESS) { outcome = "Command-buffer begin failed"; break; }
        VkClearValue clear{};
        clear.color = {{0.035f, 0.075f, 0.14f, 1.0f}};
        VkRenderPassBeginInfo render_begin{};
        render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_begin.renderPass = render_pass;
        render_begin.framebuffer = framebuffers[image_index];
        render_begin.renderArea.extent = extent;
        render_begin.clearValueCount = 1;
        render_begin.pClearValues = &clear;
        vkCmdBeginRenderPass(command, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(command);
        result = vkEndCommandBuffer(command);
        if (result != VK_SUCCESS) { outcome = "Command-buffer end failed"; break; }

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &image_ready;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &render_done;
        result = vkQueueSubmit(graphics_queue_handle, 1, &submit, VK_NULL_HANDLE);
        if (result != VK_SUCCESS) { outcome = "Vulkan clear-frame submission failed"; break; }

        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &render_done;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &image_index;
        result = vkQueuePresentKHR(present_queue_handle, &present);
        vkQueueWaitIdle(present_queue_handle);
        vkQueueWaitIdle(graphics_queue_handle);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            outcome = "Vulkan present failed";
            break;
        }
        outcome = "Vulkan logical device: OK\nSwapchain: OK (" +
                  std::to_string(extent.width) + " x " + std::to_string(extent.height) +
                  ", " + std::to_string(actual_count) + " images)\nClear render pass: OK\nTest frame presented: OK";
    } while (false);

    vkDeviceWaitIdle(device);
    for (auto framebuffer : framebuffers) vkDestroyFramebuffer(device, framebuffer, nullptr);
    if (render_pass) vkDestroyRenderPass(device, render_pass, nullptr);
    for (auto view : views) vkDestroyImageView(device, view, nullptr);
    if (image_ready) vkDestroySemaphore(device, image_ready, nullptr);
    if (render_done) vkDestroySemaphore(device, render_done, nullptr);
    if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
    vkDestroyDevice(device, nullptr);
    if (outcome.find("failed") != std::string::npos || outcome.find("failed") == std::string::npos) {
        // Include the numeric Vulkan result in diagnostic failures.
        if (outcome.find("OK") == std::string::npos)
            outcome += " (VkResult " + std::to_string(static_cast<int>(result)) + ").";
    }
    return outcome;
}

std::string probe_vulkan() {
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "GTA V Android Port";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "Android Vulkan renderer test";
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
    if (created != VK_SUCCESS) return "Vulkan instance creation failed (VkResult " + std::to_string(created) + ").";

    ANativeWindow* window = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_window_mutex);
        if (g_window) { window = g_window; ANativeWindow_acquire(window); }
    }
    if (!window) {
        vkDestroyInstance(instance, nullptr);
        return "Vulkan instance: OK\nWaiting for Android SurfaceView.";
    }

    const uint32_t width = static_cast<uint32_t>(ANativeWindow_getWidth(window));
    const uint32_t height = static_cast<uint32_t>(ANativeWindow_getHeight(window));
    VkAndroidSurfaceCreateInfoKHR surface_info{};
    surface_info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    surface_info.window = window;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const VkResult surface_result = vkCreateAndroidSurfaceKHR(instance, &surface_info, nullptr, &surface);
    ANativeWindow_release(window);
    if (surface_result != VK_SUCCESS) {
        vkDestroyInstance(instance, nullptr);
        return "Android Vulkan surface creation failed (VkResult " + std::to_string(surface_result) + ").";
    }

    uint32_t count = 0;
    VkResult result = vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (result != VK_SUCCESS || count == 0) {
        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
        return "Vulkan is present, but no physical GPU was enumerated.";
    }
    std::vector<VkPhysicalDevice> devices(count);
    result = vkEnumeratePhysicalDevices(instance, &count, devices.data());
    if (result != VK_SUCCESS) {
        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
        return "Vulkan GPU enumeration failed (VkResult " + std::to_string(result) + ").";
    }

    std::ostringstream report;
    report << "Vulkan instance: OK\nPhysical devices: " << count << "\n";
    bool presented = false;
    for (uint32_t i = 0; i < count && !presented; ++i) {
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
            if (queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT && graphics_family < 0)
                graphics_family = static_cast<int>(q);
            VkBool32 supported = VK_FALSE;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], q, surface, &supported) == VK_SUCCESS &&
                supported && present_family < 0) present_family = static_cast<int>(q);
        }
        uint32_t ext_count = 0;
        bool swapchain = false;
        if (vkEnumerateDeviceExtensionProperties(devices[i], nullptr, &ext_count, nullptr) == VK_SUCCESS) {
            std::vector<VkExtensionProperties> exts(ext_count);
            if (vkEnumerateDeviceExtensionProperties(devices[i], nullptr, &ext_count, exts.data()) == VK_SUCCESS) {
                for (const auto& ext : exts)
                    if (std::string(ext.extensionName) == VK_KHR_SWAPCHAIN_EXTENSION_NAME) swapchain = true;
            }
        }
        report << "\nGPU " << (i + 1) << ": " << properties.deviceName
               << "\nVulkan API: " << VK_VERSION_MAJOR(properties.apiVersion) << "."
               << VK_VERSION_MINOR(properties.apiVersion) << "." << VK_VERSION_PATCH(properties.apiVersion)
               << "\nGraphics queue: " << (graphics_family >= 0 ? "available" : "not found")
               << "\nPresent queue: " << (present_family >= 0 ? "available" : "not found")
               << "\nVK_KHR_swapchain: " << (swapchain ? "available" : "not available") << "\n";
        if (swapchain && graphics_family >= 0 && present_family >= 0) {
            report << clear_present_test(devices[i], surface,
                        static_cast<uint32_t>(graphics_family),
                        static_cast<uint32_t>(present_family), width, height);
            presented = true;
        }
    }
    if (!presented) report << "\nNo GPU could complete the Vulkan swapchain test.";
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
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
        "GTA V game runtime is not connected.\n\n" +
        surface_status() + "\n\n" + probe_vulkan() +
        "\n\nVulkan clear/present smoke test only; original NVN renderer and GTA runtime are not connected.";
    return env->NewStringUTF(status.c_str());
}
