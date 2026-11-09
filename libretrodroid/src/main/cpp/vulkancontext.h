#pragma once

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <android/native_window.h>
#include <memory>
#include <vector>
#include <mutex>

#include "libretro.h"
#include "vulkancommon/libretro_vulkan.h"

namespace libretrodroid {

struct VulkanImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};

class VulkanContext {
public:
    VulkanContext();

    bool initialize(retro_hw_render_context_negotiation_interface_vulkan* negotiationInterface);
    bool onSurfaceCreated(ANativeWindow* window, unsigned width, unsigned height);
    void onSurfaceDestroyed();
    void destroy();

    void handleVideoRefresh();

    const retro_hw_render_interface_vulkan* getRenderInterface() const { return &renderInterface_; }

    static void setImage(void* handle,
        const retro_vulkan_image* image,
        uint32_t num_semaphores,
        const VkSemaphore* semaphores,
        uint32_t src_queue_family);

    static uint32_t getSyncIndex(void* handle);
    static uint32_t getSyncIndexMask(void* handle);
    static void waitSyncIndex(void* handle);
    static void lockQueue(void* handle);
    static void unlockQueue(void* handle);

private:
    bool createInstance(const VkApplicationInfo* appInfo);
    bool pickPhysicalDevice();
    bool callCoreCreateDevice();
    bool createSurface(ANativeWindow* window);
    bool createSwapchain(unsigned width, unsigned height);
    bool createCommandPool();
    bool createCommandBuffers();
    bool createSyncObjects();
    void destroySwapchain();
    void cleanupSwapchain();
    void presentFrame();
    void blitCurrentImageToSwapchain(uint32_t swapchainIndex);

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex_ = 0;

    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D swapchainExtent_ = {};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;

    std::vector<VkFence> frameFences_;
    std::vector<VkSemaphore> imageAvailableSemaphores_;
    std::vector<VkSemaphore> renderFinishedSemaphores_;

    uint32_t currentFrame_ = 0;
    uint32_t syncIndex_ = 0;
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;

    std::mutex queueMutex_;

    VulkanImage pendingImage_ = {};
    bool hasPendingImage_ = false;

    retro_hw_render_context_negotiation_interface_vulkan* negotiationInterface_ = nullptr;
    retro_hw_render_interface_vulkan renderInterface_ = {};

    retro_vulkan_context vulkanContext_ = {};
};

} // namespace libretrodroid