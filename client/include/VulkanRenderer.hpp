#pragma once

#include <vulkan/vulkan.h>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <iostream>

class VulkanRenderer {
public:
    // Pass the window in so Vulkan can attach a surface to it
    void init(GLFWwindow* window);
    
    // We will call this in the main loop later
    void draw(); 
    
    // Clean up all the Vulkan memory
    void cleanup();

private:
    GLFWwindow* window_;
    VkInstance instance_;
    VkSurfaceKHR surface_;
    
    // We will add the Physical Device and Logical Device here next!
};