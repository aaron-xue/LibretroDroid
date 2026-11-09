#include "vulkancontext.h"
#include "log.h"
#include "environment.h"
#include <cstring>
#include <algorithm>

namespace libretrodroid {

VulkanContext::VulkanContext() {
    memset(&renderInterface_, 0, sizeof(renderInterface_));
    memset(&vulkanContext_, 0, sizeof(vulkanContext_));
}

bool VulkanContext::initialize(
    retro_hw_render_context_negotiation_interface_vulkan* negotiationInterface) {

    negotiationInterface_ = negotiationInterface;
    if (!negotiationInterface_ || !negotiationInterface_->get_application_info) {
        LOGE("Vulkan negotiation interface missing get_application_info");
        return false;
    }

    const VkApplicationInfo* appInfo = negotiationInterface_->get_application_info();
    if (!appInfo) {
        LOGE("Core returned null VkApplicationInfo");
        return false;
    }

    if (!createInstance(appInfo)) return false;
    if (!pickPhysicalDevice()) return false;
    if (!callCoreCreateDevice()) return false;
    if (!createCommandPool()) return false;
    if (!createSyncObjects()) return false;

    renderInterface_.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
    renderInterface_.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
    renderInterface_.handle = this;
    renderInterface_.instance = instance_;
    renderInterface_.gpu = physicalDevice_;
    renderInterface_.device = device_;
    renderInterface_.queue = queue_;
    renderInterface_.queue_index = queueFamilyIndex_;
    renderInterface_.get_device_proc_addr = nullptr;
    renderInterface_.get_instance_proc_addr = nullptr;
    renderInterface_.set_image = &VulkanContext::setImage;
    renderInterface_.set_command_buffers = nullptr;
    renderInterface_.get_sync_index = &VulkanContext::getSyncIndex;
    renderInterface_.get_sync_index_mask = &VulkanContext::getSyncIndexMask;
    renderInterface_.wait_sync_index = &VulkanContext::waitSyncIndex;
    renderInterface_.lock_queue = &VulkanContext::lockQueue;
    renderInterface_.unlock_queue = &VulkanContext::unlockQueue;
    renderInterface_.set_signal_semaphore = nullptr;

    if (vkGetInstanceProcAddr) {
        renderInterface_.get_instance_proc_addr = vkGetInstanceProcAddr;
    }
    if (vkGetDeviceProcAddr) {
        renderInterface_.get_device_proc_addr = vkGetDeviceProcAddr;
    }

    Environment::getInstance().setHwRenderInterface(
        reinterpret_cast<retro_hw_render_interface*>(&renderInterface_));

    LOGD("VulkanContext initialized successfully");
    return true;
}

bool VulkanContext::createInstance(const VkApplicationInfo* appInfo) {
    std::vector<const char*> extensions = {
        "VK_KHR_surface",
        "VK_KHR_android_surface"
    };

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    VkResult result = vkCreateInstance(&createInfo, nullptr, &instance_);
    if (result != VK_SUCCESS) {
        LOGE("Failed to create Vulkan instance: %d", result);
        return false;
    }

    LOGD("Vulkan instance created successfully");
    return true;
}

bool VulkanContext::pickPhysicalDevice() {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);
    if (deviceCount == 0) {
        LOGE("No Vulkan physical devices found");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());

    VkPhysicalDevice bestDevice = VK_NULL_HANDLE;
    int bestScore = -1;

    for (const auto& device : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);

        int score = 0;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            score += 1000;
        } else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            score += 100;
        }

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

        for (uint32_t i = 0; i < queueFamilyCount; i++) {
            if (queueFamilies[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
                score += 1;
                break;
            }
        }

        if (score > bestScore) {
            bestScore = score;
            bestDevice = device;
        }
    }

    if (!bestDevice) {
        LOGE("No suitable Vulkan physical device found");
        return false;
    }

    physicalDevice_ = bestDevice;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physicalDevice_, &props);
    LOGD("Selected Vulkan device: %s", props.deviceName);

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            queueFamilyIndex_ = i;
            break;
        }
    }

    return true;
}

bool VulkanContext::callCoreCreateDevice() {
    if (!negotiationInterface_->create_device) {
        LOGE("Core does not provide create_device callback");
        return false;
    }

    vulkanContext_.gpu = physicalDevice_;
    vulkanContext_.queue = VK_NULL_HANDLE;
    vulkanContext_.queue_family_index = queueFamilyIndex_;
    vulkanContext_.presentation_queue = VK_NULL_HANDLE;
    vulkanContext_.presentation_queue_family_index = 0;

    bool result = negotiationInterface_->create_device(
        &vulkanContext_,
        instance_,
        physicalDevice_,
        VK_NULL_HANDLE,
        vkGetInstanceProcAddr,
        nullptr,
        0,
        nullptr,
        0,
        nullptr
    );

    if (!result) {
        LOGE("Core create_device callback failed");
        return false;
    }

    device_ = vulkanContext_.device;
    queue_ = vulkanContext_.queue;
    queueFamilyIndex_ = vulkanContext_.queue_family_index;

    LOGD("Core create_device succeeded");
    return true;
}

bool VulkanContext::createSurface(ANativeWindow* window) {
    VkAndroidSurfaceCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    createInfo.window = window;

    VkResult result = vkCreateAndroidSurfaceKHR(instance_, &createInfo, nullptr, &surface_);
    if (result != VK_SUCCESS) {
        LOGE("Failed to create Android surface: %d", result);
        return false;
    }

    LOGD("Android surface created successfully");
    return true;
}

bool VulkanContext::createSwapchain(unsigned width, unsigned height) {
    if (width == 0 || height == 0) {
        LOGE("Invalid swapchain dimensions: %ux%u", width, height);
        return false;
    }

    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &capabilities);

    VkExtent2D extent;
    if (capabilities.currentExtent.width != UINT32_MAX) {
        extent = capabilities.currentExtent;
    } else {
        extent.width = std::clamp(width,
            capabilities.minImageExtent.width,
            capabilities.maxImageExtent.width);
        extent.height = std::clamp(height,
            capabilities.minImageExtent.height,
            capabilities.maxImageExtent.height);
    }

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }
    if (imageCount < 2) imageCount = 2;

    swapchainExtent_ = extent;

    VkSwapchainCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface_;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = swapchainFormat_;
    createInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    createInfo.clipped = VK_TRUE;

    VkResult result = vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_);
    if (result != VK_SUCCESS) {
        LOGE("Failed to create swapchain: %d", result);
        return false;
    }

    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
    swapchainImages_.resize(imageCount);
    swapchainImageViews_.resize(imageCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data());

    for (uint32_t i = 0; i < imageCount; i++) {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = swapchainImages_[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = swapchainFormat_;
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        result = vkCreateImageView(device_, &viewInfo, nullptr, &swapchainImageViews_[i]);
        if (result != VK_SUCCESS) {
            LOGE("Failed to create swapchain image view %d: %d", i, result);
            return false;
        }
    }

    LOGD("Swapchain created: %ux%u, %zu images",
        extent.width, extent.height, swapchainImages_.size());
    return true;
}

bool VulkanContext::createCommandPool() {
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = queueFamilyIndex_;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    VkResult result = vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_);
    if (result != VK_SUCCESS) {
        LOGE("Failed to create command pool: %d", result);
        return false;
    }

    return true;
}

bool VulkanContext::createCommandBuffers() {
    commandBuffers_.resize(MAX_FRAMES_IN_FLIGHT);

    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = MAX_FRAMES_IN_FLIGHT;

    VkResult result = vkAllocateCommandBuffers(device_, &allocInfo, commandBuffers_.data());
    if (result != VK_SUCCESS) {
        LOGE("Failed to allocate command buffers: %d", result);
        return false;
    }

    return true;
}

bool VulkanContext::createSyncObjects() {
    frameFences_.resize(MAX_FRAMES_IN_FLIGHT);
    imageAvailableSemaphores_.resize(MAX_FRAMES_IN_FLIGHT);
    renderFinishedSemaphores_.resize(MAX_FRAMES_IN_FLIGHT);

    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkSemaphoreCreateInfo semaphoreInfo = {};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkResult result;
        result = vkCreateFence(device_, &fenceInfo, nullptr, &frameFences_[i]);
        if (result != VK_SUCCESS) return false;

        result = vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailableSemaphores_[i]);
        if (result != VK_SUCCESS) return false;

        result = vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderFinishedSemaphores_[i]);
        if (result != VK_SUCCESS) return false;
    }

    return true;
}

bool VulkanContext::onSurfaceCreated(ANativeWindow* window, unsigned width, unsigned height) {
    if (!window) {
        LOGE("onSurfaceCreated: null window");
        return false;
    }

    if (!createSurface(window)) return false;
    if (!createSwapchain(width, height)) return false;
    if (!createCommandBuffers()) return false;

    currentFrame_ = 0;
    syncIndex_ = 0;
    hasPendingImage_ = false;

    auto contextReset = Environment::getInstance().getHwContextReset();
    if (contextReset) {
        LOGD("Calling hw_context_reset");
        contextReset();
    }

    LOGD("VulkanContext::onSurfaceCreated complete");
    return true;
}

void VulkanContext::onSurfaceDestroyed() {
    vkDeviceWaitIdle(device_);
    cleanupSwapchain();
}

void VulkanContext::cleanupSwapchain() {
    for (auto& view : swapchainImageViews_) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, view, nullptr);
            view = VK_NULL_HANDLE;
        }
    }
    swapchainImages_.clear();
    swapchainImageViews_.clear();

    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }

    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }

    for (auto& cmdBuf : commandBuffers_) {
        cmdBuf = VK_NULL_HANDLE;
    }
    commandBuffers_.clear();
}

void VulkanContext::destroySwapchain() {
    vkDeviceWaitIdle(device_);
    cleanupSwapchain();
}

void VulkanContext::destroy() {
    vkDeviceWaitIdle(device_);
    cleanupSwapchain();

    if (negotiationInterface_ && negotiationInterface_->destroy_device) {
        negotiationInterface_->destroy_device();
    }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (frameFences_[i] != VK_NULL_HANDLE) {
            vkDestroyFence(device_, frameFences_[i], nullptr);
        }
        if (imageAvailableSemaphores_[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, imageAvailableSemaphores_[i], nullptr);
        }
        if (renderFinishedSemaphores_[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, renderFinishedSemaphores_[i], nullptr);
        }
    }

    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
    }

    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }

    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    Environment::getInstance().setHwRenderInterface(nullptr);
}

void VulkanContext::handleVideoRefresh() {
    if (!hasPendingImage_ || pendingImage_.image == VK_NULL_HANDLE) {
        return;
    }

    if (swapchain_ == VK_NULL_HANDLE || swapchainImages_.empty()) {
        return;
    }

    presentFrame();

    hasPendingImage_ = false;
    pendingImage_ = {};
}

void VulkanContext::presentFrame() {
    vkWaitForFences(device_, 1, &frameFences_[currentFrame_], VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &frameFences_[currentFrame_]);

    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(
        device_, swapchain_, UINT64_MAX,
        imageAvailableSemaphores_[currentFrame_],
        VK_NULL_HANDLE, &imageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        return;
    }
    if (result != VK_SUCCESS) {
        LOGE("Failed to acquire swapchain image: %d", result);
        return;
    }

    VkCommandBuffer cmdBuf = commandBuffers_[currentFrame_];
    vkResetCommandBuffer(cmdBuf, 0);

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmdBuf, &beginInfo);

    VkImageSubresourceRange subresourceRange = {};
    subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    subresourceRange.baseMipLevel = 0;
    subresourceRange.levelCount = 1;
    subresourceRange.baseArrayLayer = 0;
    subresourceRange.layerCount = 1;

    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = swapchainImages_[imageIndex];
    barrier.subresourceRange = subresourceRange;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkImageMemoryBarrier barrier2 = {};
    barrier2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier2.oldLayout = pendingImage_.imageLayout;
    barrier2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier2.image = pendingImage_.image;
    barrier2.subresourceRange = subresourceRange;
    barrier2.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier2);

    VkImageBlit blitRegion = {};
    blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitRegion.srcSubresource.layerCount = 1;
    blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitRegion.dstSubresource.layerCount = 1;
    blitRegion.srcOffsets[0] = { 0, 0, 0 };
    blitRegion.srcOffsets[1] = {
        static_cast<int32_t>(swapchainExtent_.width),
        static_cast<int32_t>(swapchainExtent_.height),
        1
    };
    blitRegion.dstOffsets[0] = { 0, 0, 0 };
    blitRegion.dstOffsets[1] = {
        static_cast<int32_t>(swapchainExtent_.width),
        static_cast<int32_t>(swapchainExtent_.height),
        1
    };

    vkCmdBlitImage(cmdBuf,
        pendingImage_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        swapchainImages_[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blitRegion, VK_FILTER_LINEAR);

    VkImageMemoryBarrier barrier3 = {};
    barrier3.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier3.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier3.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier3.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier3.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier3.image = swapchainImages_[imageIndex];
    barrier3.subresourceRange = subresourceRange;
    barrier3.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier3.dstAccessMask = 0;

    vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier3);

    vkEndCommandBuffer(cmdBuf);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &imageAvailableSemaphores_[currentFrame_];
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffers_[currentFrame_];
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &renderFinishedSemaphores_[currentFrame_];

    vkQueueSubmit(queue_, 1, &submitInfo, frameFences_[currentFrame_]);

    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderFinishedSemaphores_[currentFrame_];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &imageIndex;

    vkQueuePresentKHR(queue_, &presentInfo);

    currentFrame_ = (currentFrame_ + 1) % MAX_FRAMES_IN_FLIGHT;
}

void VulkanContext::blitCurrentImageToSwapchain(uint32_t) {
}

void VulkanContext::setImage(void* handle,
    const retro_vulkan_image* image,
    uint32_t num_semaphores,
    const VkSemaphore* semaphores,
    uint32_t src_queue_family) {

    auto* self = static_cast<VulkanContext*>(handle);
    if (!self || !image) return;

    self->pendingImage_.image = image->create_info.image;
    self->pendingImage_.imageView = image->image_view;
    self->pendingImage_.imageLayout = image->image_layout;
    self->hasPendingImage_ = true;

    if (num_semaphores > 0 && semaphores) {
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = num_semaphores;
        submitInfo.pWaitSemaphores = semaphores;
        submitInfo.pWaitDstStageMask = &waitStage;

        vkQueueSubmit(self->queue_, 1, &submitInfo, VK_NULL_HANDLE);
    }
}

uint32_t VulkanContext::getSyncIndex(void* handle) {
    auto* self = static_cast<VulkanContext*>(handle);
    if (!self) return 0;
    return self->syncIndex_;
}

uint32_t VulkanContext::getSyncIndexMask(void* handle) {
    return MAX_FRAMES_IN_FLIGHT - 1;
}

void VulkanContext::waitSyncIndex(void* handle) {
    auto* self = static_cast<VulkanContext*>(handle);
    if (!self) return;

    vkWaitForFences(
        self->device_,
        1,
        &self->frameFences_[self->syncIndex_ % MAX_FRAMES_IN_FLIGHT],
        VK_TRUE,
        UINT64_MAX
    );

    vkResetFences(
        self->device_,
        1,
        &self->frameFences_[self->syncIndex_ % MAX_FRAMES_IN_FLIGHT]
    );

    self->syncIndex_++;
}

void VulkanContext::lockQueue(void* handle) {
    auto* self = static_cast<VulkanContext*>(handle);
    if (!self) return;
    self->queueMutex_.lock();
}

void VulkanContext::unlockQueue(void* handle) {
    auto* self = static_cast<VulkanContext*>(handle);
    if (!self) return;
    self->queueMutex_.unlock();
}

} // namespace libretrodroid