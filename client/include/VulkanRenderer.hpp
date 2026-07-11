#pragma once
#include "GraphicsConfig.hpp"
#include <cstdint>
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <optional>
#include <vector>
#include <mutex>

struct WindowState {
    bool isFullscreen = false;
    int windowedX = 100;
    int windowedY = 100;
    int windowedWidth = 1280;   // Default 720p
    int windowedHeight = 720;
};

struct PlayerRenderState {
    uint32_t character_id = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    bool isLocal = false;
};

struct SharedRenderState {
    std::mutex mtx;
    std::vector<PlayerRenderState> players; // Local player + every known remote player
};

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

    bool isComplete() {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

class VulkanRenderer {
public:


    bool isMovingLeft() const { return is_moving_left_; }
    bool isMovingRight() const { return is_moving_right_; }
    bool isMovingUp() const { return is_moving_up_; }
    bool isMovingDown() const { return is_moving_down_; }
    float getCameraYaw() const { return cameraYaw_; }

    static void glfw_key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void glfw_cursor_pos_callback(GLFWwindow* window, double xpos, double ypos);
    static void glfw_mouse_button_callback(GLFWwindow* window, int button, int action, int mods);
    static void glfw_scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
    float getPlayerX() const { return player_x_; }

    void handleKeyInput(int key, int scancode, int action, int mods);
    void handleCursorPos(double xpos, double ypos);
    void handleMouseButton(int button, int action, int mods);
    void handleScroll(double xoffset, double yoffset);
    void toggleFullscreen(GLFWwindow* window);
    void framebufferResizeCallback() { framebufferResized_ = true; }
    void init(GLFWwindow* window, const GraphicsConfig& config);
    void cleanup();
    
    // Renamed from draw() to match what main.cpp is calling
    void drawFrame(SharedRenderState* renderState);

    // Added wrappers for the GLFW window state
    bool shouldClose() const {
        return window_ && glfwWindowShouldClose(window_);
    }

    void pollEvents() const {
        glfwPollEvents();
    }
private:
    // Core pipeline setups
    void createLogicalDevice();
    void createSwapChain();
    void createImageViews();
    void createRenderPass();
    void createGraphicsPipeline();
    void createFramebuffers();
    void createCommandPool();
    void createCommandBuffers();
    void createSyncObjects();
    void cleanupSwapChain();

    // Device & Swapchain helpers (Deduplicated!)
    void pickPhysicalDevice();
    bool isDeviceSuitable(VkPhysicalDevice device);
    bool checkDeviceExtensionSupport(VkPhysicalDevice device);
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device);
    SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device);
    
    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
    VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities);
    VkShaderModule createShaderModule(const std::vector<uint32_t>& code);
   void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex, const std::vector<PlayerRenderState>& players);

    // Geometry/texture/descriptor setup — draws a real textured quad per
    // player instead of one hardcoded triangle offset in-shader.
    void createVertexBuffer();
    void createIndexBuffer();
    void createTextureImage();
    void createTextureImageView();
    void createTextureSampler();
    void createDescriptorSetLayout();
    void createDescriptorPool();
    void createDescriptorSets();
    void createCameraUniformBuffers();

    // Depth buffer — needed once there's a real 3D scene with more than one
    // draw at different depths (a flat 2D scene never needed this).
    void createDepthResources();
    VkFormat findDepthFormat();
    VkFormat findSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling,
                                 VkFormatFeatureFlags features);

    // Shared low-level helpers
    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                      VkBuffer& buffer, VkDeviceMemory& bufferMemory);
    void createImage(uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling,
                     VkImageUsageFlags usage, VkMemoryPropertyFlags properties,
                     VkImage& image, VkDeviceMemory& imageMemory);
    VkImageView createImageView(VkImage image, VkFormat format,
                               VkImageAspectFlags aspectFlags = VK_IMAGE_ASPECT_COLOR_BIT);
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    VkCommandBuffer beginSingleTimeCommands();
    void endSingleTimeCommands(VkCommandBuffer commandBuffer);
    void transitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);
    void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);

    // Private Member variables
    GraphicsConfig activeConfig_;
    
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapChain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapChainImages_;
    VkFormat swapChainImageFormat_;
    VkExtent2D swapChainExtent_;
    std::vector<VkImageView> swapChainImageViews_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline graphicsPipeline_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> swapChainFramebuffers_;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;

    std::vector<VkSemaphore> imageAvailableSemaphores_;
    std::vector<VkSemaphore> renderFinishedSemaphores_;
    std::vector<VkFence> inFlightFences_;
    uint32_t currentFrame_ = 0;
    const int MAX_FRAMES_IN_FLIGHT = 2;

    const std::vector<const char*> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };
    WindowState windowState_;
    bool framebufferResized_ = false;
    GLFWwindow* window_ = nullptr;

    // Real quad geometry + a single placeholder texture, shared by every
    // player draw this pass — see createTextureImage() for the placeholder.
    VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertexBufferMemory_ = VK_NULL_HANDLE;
    VkBuffer indexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory indexBufferMemory_ = VK_NULL_HANDLE;

    VkImage textureImage_ = VK_NULL_HANDLE;
    VkDeviceMemory textureImageMemory_ = VK_NULL_HANDLE;
    VkImageView textureImageView_ = VK_NULL_HANDLE;
    VkSampler textureSampler_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets_; // One per frame-in-flight — freed implicitly with descriptorPool_

    // Camera matrices, rewritten every frame (unlike the static texture
    // above) — one UBO per frame-in-flight, persistently mapped.
    std::vector<VkBuffer> cameraUboBuffers_;
    std::vector<VkDeviceMemory> cameraUboBuffersMemory_;
    std::vector<void*> cameraUboMapped_;

    // Depth buffer — swapchain-extent-dependent, unlike everything above.
    VkImage depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory depthImageMemory_ = VK_NULL_HANDLE;
    VkImageView depthImageView_ = VK_NULL_HANDLE;
    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED; // Resolved once in createRenderPass(), reused by createDepthResources()

    void recreateSwapchain(GLFWwindow* window);

    bool is_moving_left_ = false;
    bool is_moving_right_ = false;
    bool is_moving_up_ = false;
    bool is_moving_down_ = false;

    float player_x_ = 0.0f;

    // Third-person orbit camera, driven by mouse input. Character facing
    // follows cameraYaw_ every logic tick (see LogicThreadEntry) — the
    // simplified first-pass control scheme, no decoupled strafe mode yet.
    float cameraYaw_ = 0.0f;
    float cameraPitch_ = 0.349066f; // ~20 degrees, a modest initial downward tilt
    float cameraDistance_ = 6.0f;   // World units, orbit radius from the look target
    bool isRightMouseDown_ = false;
    double lastMouseX_ = 0.0;
    double lastMouseY_ = 0.0;

};