#include "VulkanRenderer.hpp"
#include <stdexcept>
#include <vector>

void VulkanRenderer::init(GLFWwindow* window) {
    window_ = window;

    // --- 1. CREATE INSTANCE ---
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Rebel Client";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "Rebel Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    // Get extensions required by GLFW for window surface creation
    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    
    createInfo.enabledExtensionCount = glfwExtensionCount;
    createInfo.ppEnabledExtensionNames = glfwExtensions;
    createInfo.enabledLayerCount = 0; 

    if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS) {
        throw std::runtime_error("[VULKAN] Failed to create instance!");
    }

    // --- 2. CREATE SURFACE ---
    if (glfwCreateWindowSurface(instance_, window_, nullptr, &surface_) != VK_SUCCESS) {
        throw std::runtime_error("[VULKAN] Failed to create window surface!");
    }
    
    std::cout << "[VULKAN] Initialized Instance and Surface successfully." << std::endl;
}

void VulkanRenderer::draw() {
    // Render commands will go here
}

void VulkanRenderer::cleanup() {
    if (instance_ != VK_NULL_HANDLE) {
        if (surface_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
        }
        vkDestroyInstance(instance_, nullptr);
    }
    std::cout << "[VULKAN] Cleaned up renderer resources." << std::endl;
}