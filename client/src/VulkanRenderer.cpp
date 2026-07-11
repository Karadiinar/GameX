#include "VulkanRenderer.hpp"
#include "GraphicsConfig.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <limits>
#include <mutex>
#include <set> // Added for std::set
#include <stdexcept>
#include <string> // Added for std::string
#include <vector>

// Real quad geometry (replaces the old hardcoded-in-shader triangle). Kept
// file-scope, not in the header — this is a renderer implementation detail,
// unlike PlayerRenderState which crosses the logic/render thread boundary.
struct Vertex {
  glm::vec2 pos;
  glm::vec2 uv;
};

static const std::vector<Vertex> kQuadVertices = {
    {{-0.5f, -0.5f}, {0.0f, 0.0f}}, // bottom-left
    {{0.5f, -0.5f}, {1.0f, 0.0f}},  // bottom-right
    {{0.5f, 0.5f}, {1.0f, 1.0f}},   // top-right
    {{-0.5f, 0.5f}, {0.0f, 1.0f}},  // top-left
};

// Same winding as the previous hardcoded triangle under this pipeline's
// existing cullMode=BACK/frontFace=CLOCKWISE state — verified by shoelace
// sign, not just assumed. Don't "fix" this without rechecking that.
static const std::vector<uint16_t> kQuadIndices = {0, 1, 2, 2, 3, 0};

// Camera matrices, uploaded once per frame via a per-frame-in-flight UBO
// (unlike the static placeholder texture, this changes every frame).
struct CameraUBO {
  glm::mat4 view;
  glm::mat4 proj;
};

// Mouse-look/orbit-camera tunables.
static constexpr float kMinPitch = -0.785398f; // -45 degrees
static constexpr float kMaxPitch = 1.48353f;   //  85 degrees — stays short of glm::lookAt's ±90° degenerate case
static constexpr float kMinDistance = 2.0f;
static constexpr float kMaxDistance = 15.0f;
static constexpr float kMouseSensitivity = 0.005f; // radians per pixel of mouse delta
static constexpr float kScrollZoomStep = 1.0f;     // world units per scroll notch

void VulkanRenderer::init(GLFWwindow *window, const GraphicsConfig &config) {
  window_ = window;
  activeConfig_ = config;

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

  uint32_t glfwExtensionCount = 0;
  const char **glfwExtensions =
      glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

  createInfo.enabledExtensionCount = glfwExtensionCount;
  createInfo.ppEnabledExtensionNames = glfwExtensions;
  const char *validationLayers[] = {"VK_LAYER_KHRONOS_validation"};
  createInfo.enabledLayerCount = 1;
  createInfo.ppEnabledLayerNames = validationLayers;

  if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS) {
    throw std::runtime_error("[VULKAN] Failed to create instance!");
  }

  // --- 2. CREATE SURFACE ---
  if (glfwCreateWindowSurface(instance_, window_, nullptr, &surface_) !=
      VK_SUCCESS) {
    throw std::runtime_error("[VULKAN] Failed to create window surface!");
  }

  // --- 3. PICK HARDWARE ---
  pickPhysicalDevice();
  createLogicalDevice();
  createSwapChain();
  createImageViews();
  createRenderPass();          // Also resolves+caches depthFormat_ as its first line
  createDescriptorSetLayout(); // Must precede createGraphicsPipeline(): the pipeline
                               // layout it builds references descriptorSetLayout_
  createGraphicsPipeline();
  createDepthResources();      // Needs swapChainExtent_ + depthFormat_, both ready by now.
                               // Must precede createFramebuffers().
  createFramebuffers();
  createCommandPool(); // Must exist before any one-time transfer command buffer
  createVertexBuffer();
  createIndexBuffer();
  createTextureImage(); // Needs commandPool_ for the staging transfer
  createTextureImageView();
  createTextureSampler();
  createCameraUniformBuffers(); // Only needs device_/physicalDevice_
  createDescriptorPool();       // Sized for 2 uniform buffers + 2 combined-image-samplers now
  createDescriptorSets();       // Last of this group: needs the layout + the finished
                                // texture view/sampler + the finished UBO buffers
  createCommandBuffers();
  createSyncObjects();

  std::cout << "[VULKAN] Full rendering pipeline initialized." << std::endl;
}

void VulkanRenderer::createLogicalDevice() {
  QueueFamilyIndices indices = findQueueFamilies(physicalDevice_);

  std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
  std::set<uint32_t> uniqueQueueFamilies = {indices.graphicsFamily.value(),
                                            indices.presentFamily.value()};

  float queuePriority = 1.0f;
  for (uint32_t queueFamily : uniqueQueueFamilies) {
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = queueFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;
    queueCreateInfos.push_back(queueCreateInfo);
  }

  // No optional device features enabled — nothing built so far needs any.
  VkPhysicalDeviceFeatures deviceFeatures{};

  VkDeviceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

  createInfo.queueCreateInfoCount =
      static_cast<uint32_t>(queueCreateInfos.size());
  createInfo.pQueueCreateInfos = queueCreateInfos.data();

  createInfo.pEnabledFeatures = &deviceFeatures;

  createInfo.enabledExtensionCount =
      static_cast<uint32_t>(deviceExtensions.size());
  createInfo.ppEnabledExtensionNames = deviceExtensions.data();

  // No validation layers explicitly enabled here for the device,
  // modern Vulkan handles validation at the instance level anyway.
  createInfo.enabledLayerCount = 0;

  if (vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_) !=
      VK_SUCCESS) {
    throw std::runtime_error("[Vulkan] Failed to create logical device!");
  }

  vkGetDeviceQueue(device_, indices.graphicsFamily.value(), 0, &graphicsQueue_);
  vkGetDeviceQueue(device_, indices.presentFamily.value(), 0, &presentQueue_);
}

void VulkanRenderer::createSwapChain() {
  SwapChainSupportDetails swapChainSupport =
      querySwapChainSupport(physicalDevice_);

  VkSurfaceFormatKHR surfaceFormat =
      chooseSwapSurfaceFormat(swapChainSupport.formats);
  VkPresentModeKHR presentMode =
      chooseSwapPresentMode(swapChainSupport.presentModes);
  VkExtent2D extent = chooseSwapExtent(swapChainSupport.capabilities);

  uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
  if (swapChainSupport.capabilities.maxImageCount > 0 &&
      imageCount > swapChainSupport.capabilities.maxImageCount) {
    imageCount = swapChainSupport.capabilities.maxImageCount;
  }

  VkSwapchainCreateInfoKHR createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  createInfo.surface = surface_;
  createInfo.minImageCount = imageCount;
  createInfo.imageFormat = surfaceFormat.format;
  createInfo.imageColorSpace = surfaceFormat.colorSpace;
  createInfo.imageExtent = extent;
  createInfo.imageArrayLayers = 1;
  createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

  QueueFamilyIndices indices = findQueueFamilies(physicalDevice_);
  uint32_t queueFamilyIndices[] = {indices.graphicsFamily.value(),
                                   indices.presentFamily.value()};

  if (indices.graphicsFamily != indices.presentFamily) {
    createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    createInfo.queueFamilyIndexCount = 2;
    createInfo.pQueueFamilyIndices = queueFamilyIndices;
  } else {
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  }

  createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
  createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  createInfo.presentMode = presentMode;
  createInfo.clipped = VK_TRUE;

  if (vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapChain_) !=
      VK_SUCCESS) {
    throw std::runtime_error("[VULKAN] Failed to create swap chain!");
  }

  vkGetSwapchainImagesKHR(device_, swapChain_, &imageCount, nullptr);
  swapChainImages_.resize(imageCount);
  vkGetSwapchainImagesKHR(device_, swapChain_, &imageCount,
                          swapChainImages_.data());

  swapChainImageFormat_ = surfaceFormat.format;
  swapChainExtent_ = extent;
}

void VulkanRenderer::createImageViews() {
  swapChainImageViews_.resize(swapChainImages_.size());

  for (size_t i = 0; i < swapChainImages_.size(); i++) {
    VkImageViewCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    createInfo.image = swapChainImages_[i];
    createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    createInfo.format = swapChainImageFormat_;
    createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    createInfo.subresourceRange.baseMipLevel = 0;
    createInfo.subresourceRange.levelCount = 1;
    createInfo.subresourceRange.baseArrayLayer = 0;
    createInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device_, &createInfo, nullptr,
                          &swapChainImageViews_[i]) != VK_SUCCESS) {
      throw std::runtime_error("[VULKAN] Failed to create image views!");
    }
  }
}

void VulkanRenderer::createRenderPass() {
  depthFormat_ = findDepthFormat(); // Also reused later by createDepthResources()

  VkAttachmentDescription colorAttachment{};
  colorAttachment.format = swapChainImageFormat_;
  colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
  colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  VkAttachmentDescription depthAttachment{};
  depthAttachment.format = depthFormat_;
  depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
  depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; // Never sampled/read back
  depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference colorAttachmentRef{};
  colorAttachmentRef.attachment = 0;
  colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentReference depthAttachmentRef{};
  depthAttachmentRef.attachment = 1;
  depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &colorAttachmentRef;
  subpass.pDepthStencilAttachment = &depthAttachmentRef;

  VkSubpassDependency dependency{};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dependency.srcAccessMask = 0;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

  std::array<VkAttachmentDescription, 2> attachments = {colorAttachment, depthAttachment};
  VkRenderPassCreateInfo renderPassInfo{};
  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
  renderPassInfo.pAttachments = attachments.data();
  renderPassInfo.subpassCount = 1;
  renderPassInfo.pSubpasses = &subpass;
  renderPassInfo.dependencyCount = 1;
  renderPassInfo.pDependencies = &dependency;

  if (vkCreateRenderPass(device_, &renderPassInfo, nullptr, &renderPass_) !=
      VK_SUCCESS) {
    throw std::runtime_error("[VULKAN] Failed to create render pass!");
  }
}

static std::vector<uint32_t>
readFile(const std::string &filename) {
  // Path is relative to the current working directory — run from client/ (or
  // wherever shaders/ was copied next to the binary).
  std::ifstream file("shaders/" + filename, std::ios::ate | std::ios::binary);

  if (!file.is_open()) {
    throw std::runtime_error("[VULKAN] Failed to open shader file: shaders/" +
                             filename);
  }

  size_t fileSize = (size_t)file.tellg();

  // SPIR-V files must be a multiple of 4 bytes
  if (fileSize % 4 != 0) {
    throw std::runtime_error(
        "[VULKAN] Shader file size is not a multiple of 4 bytes: " + filename);
  }

  std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));

  file.seekg(0);
  file.read(reinterpret_cast<char *>(buffer.data()), fileSize);
  file.close();

  return buffer;
}

void VulkanRenderer::createGraphicsPipeline() {
  auto vertShaderCode = readFile("vert.spv");
  auto fragShaderCode = readFile("frag.spv");

  VkShaderModule vertShaderModule = createShaderModule(vertShaderCode);
  VkShaderModule fragShaderModule = createShaderModule(fragShaderCode);

  VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
  vertShaderStageInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
  vertShaderStageInfo.module = vertShaderModule;
  vertShaderStageInfo.pName = "main";

  VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
  fragShaderStageInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  fragShaderStageInfo.module = fragShaderModule;
  fragShaderStageInfo.pName = "main";

  VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo,
                                                    fragShaderStageInfo};

  VkVertexInputBindingDescription bindingDescription{};
  bindingDescription.binding = 0;
  bindingDescription.stride = sizeof(Vertex);
  bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

  std::array<VkVertexInputAttributeDescription, 2> attributeDescriptions{};
  attributeDescriptions[0].location = 0;
  attributeDescriptions[0].binding = 0;
  attributeDescriptions[0].format = VK_FORMAT_R32G32_SFLOAT;
  attributeDescriptions[0].offset = offsetof(Vertex, pos);
  attributeDescriptions[1].location = 1;
  attributeDescriptions[1].binding = 0;
  attributeDescriptions[1].format = VK_FORMAT_R32G32_SFLOAT;
  attributeDescriptions[1].offset = offsetof(Vertex, uv);

  VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
  vertexInputInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInputInfo.vertexBindingDescriptionCount = 1;
  vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
  vertexInputInfo.vertexAttributeDescriptionCount =
      static_cast<uint32_t>(attributeDescriptions.size());
  vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  inputAssembly.primitiveRestartEnable = VK_FALSE;

  VkPipelineViewportStateCreateInfo viewportState{};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.depthClampEnable = VK_FALSE;
  rasterizer.rasterizerDiscardEnable = VK_FALSE;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.lineWidth = 1.0f;
  // Billboards always face the camera by construction, so there's no
  // meaningful back face to cull. Also: the proj[1][1] *= -1 Y-flip (see
  // drawFrame()) flips effective winding, which would otherwise interact
  // badly with frontFace=CLOCKWISE below in a way that's undiagnosable
  // without visual access — disabling culling entirely sidesteps that.
  rasterizer.cullMode = VK_CULL_MODE_NONE;
  rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;

  VkPipelineMultisampleStateCreateInfo multisampling{};
  multisampling.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisampling.sampleShadingEnable = VK_FALSE;
  multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendAttachmentState colorBlendAttachment{};
  colorBlendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  colorBlendAttachment.blendEnable = VK_FALSE;

  VkPipelineColorBlendStateCreateInfo colorBlending{};
  colorBlending.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlending.logicOpEnable = VK_FALSE;
  colorBlending.attachmentCount = 1;
  colorBlending.pAttachments = &colorBlendAttachment;

  std::vector<VkDynamicState> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT,
                                               VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
  dynamicState.pDynamicStates = dynamicStates.data();

  // Define the push constant range telling Vulkan what the shaders expect.
  // Both stages read it now (vertex for position, fragment for is_local).
  VkPushConstantRange pushConstantRange{};
  pushConstantRange.stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  pushConstantRange.offset = 0;   // Zero offset
  pushConstantRange.size =
      sizeof(float) * 4; // x, y, z, and an isLocal flag (0.0/1.0)

  VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.setLayoutCount = 1;
  pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout_;
  pipelineLayoutInfo.pushConstantRangeCount = 1; // Set this to 1
  pipelineLayoutInfo.pPushConstantRanges =
      &pushConstantRange; // Bind the range data

  if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr,
                             &pipelineLayout_) != VK_SUCCESS) {
    throw std::runtime_error("[VULKAN] Failed to create pipeline layout!");
  }

  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_TRUE;
  depthStencil.depthWriteEnable = VK_TRUE;
  depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
  depthStencil.depthBoundsTestEnable = VK_FALSE;
  depthStencil.stencilTestEnable = VK_FALSE;

  VkGraphicsPipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineInfo.stageCount = 2;
  pipelineInfo.pStages = shaderStages;
  pipelineInfo.pVertexInputState = &vertexInputInfo;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState = &viewportState;
  pipelineInfo.pRasterizationState = &rasterizer;
  pipelineInfo.pMultisampleState = &multisampling;
  pipelineInfo.pColorBlendState = &colorBlending;
  pipelineInfo.pDepthStencilState = &depthStencil;
  pipelineInfo.pDynamicState = &dynamicState;
  pipelineInfo.layout = pipelineLayout_;
  pipelineInfo.renderPass = renderPass_;
  pipelineInfo.subpass = 0;

  if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo,
                                nullptr, &graphicsPipeline_) != VK_SUCCESS) {
    throw std::runtime_error("[VULKAN] Failed to create graphics pipeline!");
  }

  vkDestroyShaderModule(device_, fragShaderModule, nullptr);
  vkDestroyShaderModule(device_, vertShaderModule, nullptr);
}

void VulkanRenderer::createFramebuffers() {
  swapChainFramebuffers_.resize(swapChainImageViews_.size());

  for (size_t i = 0; i < swapChainImageViews_.size(); i++) {
    VkImageView attachments[] = {swapChainImageViews_[i], depthImageView_};

    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = renderPass_;
    framebufferInfo.attachmentCount = 2;
    framebufferInfo.pAttachments = attachments;
    framebufferInfo.width = swapChainExtent_.width;
    framebufferInfo.height = swapChainExtent_.height;
    framebufferInfo.layers = 1;

    if (vkCreateFramebuffer(device_, &framebufferInfo, nullptr,
                            &swapChainFramebuffers_[i]) != VK_SUCCESS) {
      throw std::runtime_error("[VULKAN] Failed to create framebuffer!");
    }
  }
}

void VulkanRenderer::createCommandPool() {
  QueueFamilyIndices queueFamilyIndices = findQueueFamilies(physicalDevice_);

  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();

  if (vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_) !=
      VK_SUCCESS) {
    throw std::runtime_error("[VULKAN] Failed to create command pool!");
  }
}

void VulkanRenderer::createCommandBuffers() {
  commandBuffers_.resize(MAX_FRAMES_IN_FLIGHT);

  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.commandPool = commandPool_;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = (uint32_t)commandBuffers_.size();

  if (vkAllocateCommandBuffers(device_, &allocInfo, commandBuffers_.data()) !=
      VK_SUCCESS) {
    throw std::runtime_error("[VULKAN] Failed to allocate command buffers!");
  }
}

uint32_t VulkanRenderer::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
  VkPhysicalDeviceMemoryProperties memProperties;
  vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProperties);

  for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
    if ((typeFilter & (1 << i)) &&
        (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
      return i;
    }
  }

  throw std::runtime_error("[VULKAN] Failed to find a suitable memory type!");
}

void VulkanRenderer::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                  VkMemoryPropertyFlags properties,
                                  VkBuffer& buffer, VkDeviceMemory& bufferMemory) {
  VkBufferCreateInfo bufferInfo{};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = size;
  bufferInfo.usage = usage;
  bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  if (vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
    throw std::runtime_error("[VULKAN] Failed to create buffer!");
  }

  VkMemoryRequirements memRequirements;
  vkGetBufferMemoryRequirements(device_, buffer, &memRequirements);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memRequirements.size;
  allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

  if (vkAllocateMemory(device_, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
    throw std::runtime_error("[VULKAN] Failed to allocate buffer memory!");
  }

  vkBindBufferMemory(device_, buffer, bufferMemory, 0);
}

void VulkanRenderer::createImage(uint32_t width, uint32_t height, VkFormat format,
                                 VkImageTiling tiling, VkImageUsageFlags usage,
                                 VkMemoryPropertyFlags properties,
                                 VkImage& image, VkDeviceMemory& imageMemory) {
  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.extent.width = width;
  imageInfo.extent.height = height;
  imageInfo.extent.depth = 1;
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.format = format;
  imageInfo.tiling = tiling;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageInfo.usage = usage;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  if (vkCreateImage(device_, &imageInfo, nullptr, &image) != VK_SUCCESS) {
    throw std::runtime_error("[VULKAN] Failed to create image!");
  }

  VkMemoryRequirements memRequirements;
  vkGetImageMemoryRequirements(device_, image, &memRequirements);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memRequirements.size;
  allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

  if (vkAllocateMemory(device_, &allocInfo, nullptr, &imageMemory) != VK_SUCCESS) {
    throw std::runtime_error("[VULKAN] Failed to allocate image memory!");
  }

  vkBindImageMemory(device_, image, imageMemory, 0);
}

VkImageView VulkanRenderer::createImageView(VkImage image, VkFormat format,
                                            VkImageAspectFlags aspectFlags) {
  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image = image;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = format;
  viewInfo.subresourceRange.aspectMask = aspectFlags;
  viewInfo.subresourceRange.baseMipLevel = 0;
  viewInfo.subresourceRange.levelCount = 1;
  viewInfo.subresourceRange.baseArrayLayer = 0;
  viewInfo.subresourceRange.layerCount = 1;

  VkImageView imageView;
  if (vkCreateImageView(device_, &viewInfo, nullptr, &imageView) != VK_SUCCESS) {
    throw std::runtime_error("[VULKAN] Failed to create texture image view!");
  }
  return imageView;
}

VkFormat VulkanRenderer::findSupportedFormat(const std::vector<VkFormat>& candidates,
                                             VkImageTiling tiling, VkFormatFeatureFlags features) {
  for (VkFormat format : candidates) {
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(physicalDevice_, format, &props);
    if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features) {
      return format;
    }
    if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features) {
      return format;
    }
  }
  throw std::runtime_error("[VULKAN] Failed to find a supported depth format!");
}

VkFormat VulkanRenderer::findDepthFormat() {
  return findSupportedFormat(
      {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
      VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
}

void VulkanRenderer::createDepthResources() {
  createImage(swapChainExtent_.width, swapChainExtent_.height, depthFormat_,
             VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, depthImage_, depthImageMemory_);
  depthImageView_ = createImageView(depthImage_, depthFormat_, VK_IMAGE_ASPECT_DEPTH_BIT);
  // No transitionImageLayout() call needed — the render pass's attachment
  // description transitions UNDEFINED -> DEPTH_STENCIL_ATTACHMENT_OPTIMAL
  // automatically on first use, same mechanism the color attachment already
  // relies on. That helper only knows the two color-image transitions from
  // the texture task and would throw if called with depth layouts.
}

VkCommandBuffer VulkanRenderer::beginSingleTimeCommands() {
  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandPool = commandPool_;
  allocInfo.commandBufferCount = 1;

  VkCommandBuffer commandBuffer;
  vkAllocateCommandBuffers(device_, &allocInfo, &commandBuffer);

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

  vkBeginCommandBuffer(commandBuffer, &beginInfo);
  return commandBuffer;
}

void VulkanRenderer::endSingleTimeCommands(VkCommandBuffer commandBuffer) {
  vkEndCommandBuffer(commandBuffer);

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &commandBuffer;

  // Runs exactly twice, total, at startup — synchronous wait is simpler and
  // fine here; a fence-pool pattern would be overkill for one-time setup.
  vkQueueSubmit(graphicsQueue_, 1, &submitInfo, VK_NULL_HANDLE);
  vkQueueWaitIdle(graphicsQueue_);

  vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
}

void VulkanRenderer::transitionImageLayout(VkImage image, VkFormat format,
                                           VkImageLayout oldLayout, VkImageLayout newLayout) {
  VkCommandBuffer commandBuffer = beginSingleTimeCommands();

  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;

  VkPipelineStageFlags sourceStage;
  VkPipelineStageFlags destinationStage;

  if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
      newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
             newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  } else {
    throw std::runtime_error("[VULKAN] Unsupported image layout transition!");
  }

  vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr,
                       1, &barrier);

  endSingleTimeCommands(commandBuffer);
}

void VulkanRenderer::copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width,
                                       uint32_t height) {
  VkCommandBuffer commandBuffer = beginSingleTimeCommands();

  VkBufferImageCopy region{};
  region.bufferOffset = 0;
  region.bufferRowLength = 0;
  region.bufferImageHeight = 0;
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = 1;
  region.imageOffset = {0, 0, 0};
  region.imageExtent = {width, height, 1};

  vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                         &region);

  endSingleTimeCommands(commandBuffer);
}

void VulkanRenderer::createVertexBuffer() {
  VkDeviceSize bufferSize = sizeof(Vertex) * kQuadVertices.size();

  // Small, static, never re-uploaded — host-visible|coherent direct mapping
  // is simpler than staging+device-local and costs nothing measurable here.
  createBuffer(bufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
              vertexBuffer_, vertexBufferMemory_);

  void* data;
  vkMapMemory(device_, vertexBufferMemory_, 0, bufferSize, 0, &data);
  std::memcpy(data, kQuadVertices.data(), static_cast<size_t>(bufferSize));
  vkUnmapMemory(device_, vertexBufferMemory_);
}

void VulkanRenderer::createIndexBuffer() {
  VkDeviceSize bufferSize = sizeof(uint16_t) * kQuadIndices.size();

  createBuffer(bufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
              indexBuffer_, indexBufferMemory_);

  void* data;
  vkMapMemory(device_, indexBufferMemory_, 0, bufferSize, 0, &data);
  std::memcpy(data, kQuadIndices.data(), static_cast<size_t>(bufferSize));
  vkUnmapMemory(device_, indexBufferMemory_);
}

// No real art exists yet, and adding an image-loading library is out of
// scope for this pass — generate a placeholder directly instead. This is a
// classic magenta/black "missing texture" checker, chosen specifically so it
// reads as "placeholder," not accidental art.
static std::vector<uint8_t> generateCheckerboardPixels(uint32_t width, uint32_t height,
                                                        uint32_t cellSize) {
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      bool even = ((x / cellSize) + (y / cellSize)) % 2 == 0;
      uint8_t r = even ? 230 : 20;
      uint8_t g = even ? 40 : 20;
      uint8_t b = even ? 220 : 20;
      size_t o = (static_cast<size_t>(y) * width + x) * 4;
      pixels[o + 0] = r;
      pixels[o + 1] = g;
      pixels[o + 2] = b;
      pixels[o + 3] = 255;
    }
  }
  return pixels;
}

void VulkanRenderer::createTextureImage() {
  constexpr uint32_t kTexWidth = 64;
  constexpr uint32_t kTexHeight = 64;
  constexpr uint32_t kCheckerCell = 8;
  constexpr VkFormat kTexFormat = VK_FORMAT_R8G8B8A8_SRGB;

  std::vector<uint8_t> pixels = generateCheckerboardPixels(kTexWidth, kTexHeight, kCheckerCell);
  VkDeviceSize imageSize = static_cast<VkDeviceSize>(pixels.size());

  VkBuffer stagingBuffer;
  VkDeviceMemory stagingMemory;
  createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
              stagingBuffer, stagingMemory);

  void* data;
  vkMapMemory(device_, stagingMemory, 0, imageSize, 0, &data);
  std::memcpy(data, pixels.data(), static_cast<size_t>(imageSize));
  vkUnmapMemory(device_, stagingMemory);

  createImage(kTexWidth, kTexHeight, kTexFormat, VK_IMAGE_TILING_OPTIMAL,
             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, textureImage_, textureImageMemory_);

  transitionImageLayout(textureImage_, kTexFormat, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  copyBufferToImage(stagingBuffer, textureImage_, kTexWidth, kTexHeight);
  transitionImageLayout(textureImage_, kTexFormat, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  vkDestroyBuffer(device_, stagingBuffer, nullptr);
  vkFreeMemory(device_, stagingMemory, nullptr);
}

void VulkanRenderer::createTextureImageView() {
  textureImageView_ = createImageView(textureImage_, VK_FORMAT_R8G8B8A8_SRGB);
}

void VulkanRenderer::createTextureSampler() {
  VkSamplerCreateInfo samplerInfo{};
  samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  // Nearest filtering: crisp for this checker placeholder now, and avoids
  // blurring glyph edges if a future font atlas reuses this same pipeline.
  samplerInfo.magFilter = VK_FILTER_NEAREST;
  samplerInfo.minFilter = VK_FILTER_NEAREST;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.anisotropyEnable = VK_FALSE;
  samplerInfo.maxAnisotropy = 1.0f;
  samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
  samplerInfo.unnormalizedCoordinates = VK_FALSE;
  samplerInfo.compareEnable = VK_FALSE;
  samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
  samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  samplerInfo.minLod = 0.0f;
  samplerInfo.maxLod = 0.0f;
  samplerInfo.mipLodBias = 0.0f;

  if (vkCreateSampler(device_, &samplerInfo, nullptr, &textureSampler_) != VK_SUCCESS) {
    throw std::runtime_error("[VULKAN] Failed to create texture sampler!");
  }
}

void VulkanRenderer::createDescriptorSetLayout() {
  // Binding 0: combined-image-sampler — deliberately generic (not named
  // after "player texture") so a future feature (e.g. a font atlas) can
  // reuse this same layout/pipeline shape rather than inventing its own.
  VkDescriptorSetLayoutBinding samplerLayoutBinding{};
  samplerLayoutBinding.binding = 0;
  samplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  samplerLayoutBinding.descriptorCount = 1;
  samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  // Binding 1: camera view/projection matrices.
  VkDescriptorSetLayoutBinding uboLayoutBinding{};
  uboLayoutBinding.binding = 1;
  uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  uboLayoutBinding.descriptorCount = 1;
  uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

  std::array<VkDescriptorSetLayoutBinding, 2> bindings = {samplerLayoutBinding, uboLayoutBinding};
  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
  layoutInfo.pBindings = bindings.data();

  if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorSetLayout_) !=
      VK_SUCCESS) {
    throw std::runtime_error("[VULKAN] Failed to create descriptor set layout!");
  }
}

void VulkanRenderer::createCameraUniformBuffers() {
  VkDeviceSize bufferSize = sizeof(CameraUBO);
  cameraUboBuffers_.resize(MAX_FRAMES_IN_FLIGHT);
  cameraUboBuffersMemory_.resize(MAX_FRAMES_IN_FLIGHT);
  cameraUboMapped_.resize(MAX_FRAMES_IN_FLIGHT);
  for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
    createBuffer(bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                cameraUboBuffers_[i], cameraUboBuffersMemory_[i]);
    // Persistently mapped for the app's lifetime — this buffer is rewritten
    // every frame (see drawFrame()), so map/unmap-per-frame would be pure
    // overhead with no correctness benefit; host-coherent memory means no
    // explicit flush is needed either.
    vkMapMemory(device_, cameraUboBuffersMemory_[i], 0, bufferSize, 0, &cameraUboMapped_[i]);
  }
}

void VulkanRenderer::createDescriptorPool() {
  std::array<VkDescriptorPoolSize, 2> poolSizes{};
  poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  poolSizes[0].descriptorCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
  poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  poolSizes[1].descriptorCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);

  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
  poolInfo.pPoolSizes = poolSizes.data();
  poolInfo.maxSets = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);

  if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
    throw std::runtime_error("[VULKAN] Failed to create descriptor pool!");
  }
}

void VulkanRenderer::createDescriptorSets() {
  std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, descriptorSetLayout_);
  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = descriptorPool_;
  allocInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
  allocInfo.pSetLayouts = layouts.data();

  descriptorSets_.resize(MAX_FRAMES_IN_FLIGHT);
  if (vkAllocateDescriptorSets(device_, &allocInfo, descriptorSets_.data()) != VK_SUCCESS) {
    throw std::runtime_error("[VULKAN] Failed to allocate descriptor sets!");
  }

  // Descriptor bindings are still written exactly once, here, at init — only
  // the *data behind* the UBO binding changes per-frame, via the mapped
  // pointer (see drawFrame()), not the binding itself. Binding 0 (the
  // texture) points at the same shared resource in every set; binding 1
  // points at that slot's own UBO buffer.
  for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = textureImageView_;
    imageInfo.sampler = textureSampler_;

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = cameraUboBuffers_[i];
    bufferInfo.offset = 0;
    bufferInfo.range = sizeof(CameraUBO);

    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descriptorSets_[i];
    writes[0].dstBinding = 0;
    writes[0].dstArrayElement = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &imageInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = descriptorSets_[i];
    writes[1].dstBinding = 1;
    writes[1].dstArrayElement = 0;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].descriptorCount = 1;
    writes[1].pBufferInfo = &bufferInfo;

    vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
  }
}

void VulkanRenderer::recordCommandBuffer(VkCommandBuffer commandBuffer,
                                         uint32_t imageIndex,
                                         const std::vector<PlayerRenderState>& players) {
  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

  if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
    throw std::runtime_error(
        "[VULKAN] Failed to begin recording command buffer!");
  }

  VkRenderPassBeginInfo renderPassInfo{};
  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  renderPassInfo.renderPass = renderPass_;
  renderPassInfo.framebuffer = swapChainFramebuffers_[imageIndex];
  renderPassInfo.renderArea.offset = {0, 0};
  renderPassInfo.renderArea.extent = swapChainExtent_;

  std::array<VkClearValue, 2> clearValues{};
  clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  clearValues[1].depthStencil = {1.0f, 0};
  renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
  renderPassInfo.pClearValues = clearValues.data();

  vkCmdBeginRenderPass(commandBuffer, &renderPassInfo,
                       VK_SUBPASS_CONTENTS_INLINE);

  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    graphicsPipeline_);

  // Viewport/scissor are dynamic state (declared as such in the pipeline),
  // so they must be set here every frame rather than baked into the pipeline.
  VkViewport viewport{};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = (float)swapChainExtent_.width;
  viewport.height = (float)swapChainExtent_.height;
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

  VkRect2D scissor{};
  scissor.offset = {0, 0};
  scissor.extent = swapChainExtent_;
  vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

  // Geometry and texture are shared by every player this pass — bind once,
  // not per-player. Only the push constants change per draw below.
  VkDeviceSize offsets[] = {0};
  vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer_, offsets);
  vkCmdBindIndexBuffer(commandBuffer, indexBuffer_, 0, VK_INDEX_TYPE_UINT16);
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                          &descriptorSets_[currentFrame_], 0, nullptr);

  // =================================================================
  // 3. One draw call per known player (local + every tracked remote)
  // =================================================================
  for (const auto& player : players) {
    float pushConstants[4] = {player.x, player.y, player.z, player.isLocal ? 1.0f : 0.0f};

    vkCmdPushConstants(
        commandBuffer,
        pipelineLayout_,            // Your compiled pipeline layout
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, // Both stages read this now
        0,                          // Offset
        sizeof(pushConstants),      // x, y, isLocal
        pushConstants
    );

    vkCmdDrawIndexed(commandBuffer, static_cast<uint32_t>(kQuadIndices.size()), 1, 0, 0, 0);
  }
  // =================================================================

  vkCmdEndRenderPass(commandBuffer);

  if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
    throw std::runtime_error("[VULKAN] Failed to record command buffer!");
  }
}

void VulkanRenderer::createSyncObjects() {
  imageAvailableSemaphores_.resize(MAX_FRAMES_IN_FLIGHT);
  inFlightFences_.resize(MAX_FRAMES_IN_FLIGHT);

  // Tied to swapchain images, not frames-in-flight — a semaphore must be
  // fully waited-on before it's safe to signal again, and presentation
  // completion is scoped to the image, not the CPU frame slot.
  renderFinishedSemaphores_.resize(swapChainImages_.size());

  VkSemaphoreCreateInfo semaphoreInfo{};
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

  for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
    if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr,
                          &imageAvailableSemaphores_[i]) != VK_SUCCESS ||
        vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[i]) !=
            VK_SUCCESS) {
      throw std::runtime_error(
          "[VULKAN] Failed to create per-frame synchronization objects!");
    }
  }

  for (size_t i = 0; i < renderFinishedSemaphores_.size(); i++) {
    if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr,
                          &renderFinishedSemaphores_[i]) != VK_SUCCESS) {
      throw std::runtime_error(
          "[VULKAN] Failed to create per-image render-finished semaphores!");
    }
  }
}

void VulkanRenderer::pickPhysicalDevice() {
  uint32_t deviceCount = 0;
  vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);

  if (deviceCount == 0) {
    throw std::runtime_error(
        "[VULKAN] Failed to find GPUs with Vulkan support!");
  }

  std::vector<VkPhysicalDevice> devices(deviceCount);
  vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());

  for (const auto &device : devices) {
    if (isDeviceSuitable(device)) {
      physicalDevice_ = device;
      break;
    }
  }

  if (physicalDevice_ == VK_NULL_HANDLE) {
    throw std::runtime_error("[VULKAN] Failed to find a suitable GPU!");
  }

  VkPhysicalDeviceProperties deviceProperties;
  vkGetPhysicalDeviceProperties(physicalDevice_, &deviceProperties);
  std::cout << "[VULKAN] Locked onto GPU: " << deviceProperties.deviceName
            << std::endl;
}

bool VulkanRenderer::isDeviceSuitable(VkPhysicalDevice device) {
  QueueFamilyIndices indices = findQueueFamilies(device);
  // Check for required device extension support (e.g., swapchain)
  bool extensionsSupported = checkDeviceExtensionSupport(device);

  // Check for adequate swap chain support (formats and present modes)
  bool swapChainAdequate = false;
  if (extensionsSupported) { // Only query if extensions are supported
    SwapChainSupportDetails swapChainSupport = querySwapChainSupport(device);
    swapChainAdequate = !swapChainSupport.formats.empty() &&
                        !swapChainSupport.presentModes.empty();
  }
  return indices.isComplete() && extensionsSupported && swapChainAdequate;
}

QueueFamilyIndices VulkanRenderer::findQueueFamilies(VkPhysicalDevice device) {
  QueueFamilyIndices indices;

  uint32_t queueFamilyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

  std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount,
                                           queueFamilies.data());

  int i = 0;
  for (const auto &queueFamily : queueFamilies) {
    if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      indices.graphicsFamily = i;
    }

    VkBool32 presentSupport = false;
    vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &presentSupport);

    if (presentSupport) {
      indices.presentFamily = i;
    }

    if (indices.isComplete())
      break;
    i++;
  }

  return indices;
}

VkShaderModule
VulkanRenderer::createShaderModule(const std::vector<uint32_t> &code) {
  VkShaderModuleCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;

  // codeSize wants bytes, not element count — code is a vector<uint32_t>.
  createInfo.codeSize = code.size() * sizeof(uint32_t);
  createInfo.pCode = code.data();

  VkShaderModule shaderModule;
  if (vkCreateShaderModule(device_, &createInfo, nullptr, &shaderModule) !=
      VK_SUCCESS) {
    throw std::runtime_error("[VULKAN] Failed to create shader module!");
  }

  return shaderModule;
}

bool VulkanRenderer::checkDeviceExtensionSupport(VkPhysicalDevice device) {
  uint32_t extensionCount;
  vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount,
                                       nullptr);

  std::vector<VkExtensionProperties> availableExtensions(extensionCount);
  vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount,
                                       availableExtensions.data());

  std::set<std::string> requiredExtensions(deviceExtensions.begin(),
                                           deviceExtensions.end());

  for (const auto &extension : availableExtensions) {
    std::string extName(extension.extensionName);

    if (requiredExtensions.count(extName)) {
      std::cout << "  Extension: " << extName << " (SUPPORTED)\n";
      requiredExtensions.erase(extName);
    }
  }
  return requiredExtensions.empty();
}

SwapChainSupportDetails
VulkanRenderer::querySwapChainSupport(VkPhysicalDevice device) {
  SwapChainSupportDetails details;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_,
                                            &details.capabilities);

  uint32_t formatCount;
  vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, nullptr);
  if (formatCount != 0) {
    details.formats.resize(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount,
                                         details.formats.data());
  }

  uint32_t presentModeCount;
  vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount,
                                            nullptr);
  if (presentModeCount != 0) {
    details.presentModes.resize(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(
        device, surface_, &presentModeCount, details.presentModes.data());
  }
  return details;
}

VkSurfaceFormatKHR VulkanRenderer::chooseSwapSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR> &availableFormats) {
  for (const auto &availableFormat : availableFormats) {
    if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB &&
        availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      return availableFormat;
    }
  }
  return availableFormats[0];
}

VkPresentModeKHR VulkanRenderer::chooseSwapPresentMode(
    const std::vector<VkPresentModeKHR> &availablePresentModes) {

  // --- 1. USER SETTING: TRIPLE BUFFERING (MAILBOX) ---
  if (activeConfig_.presentMode == PresentModeSetting::TripleBuffer) {
    for (const auto &mode : availablePresentModes) {
      if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
        std::cout
            << "[VULKAN CONFIG] Present Mode: Triple Buffering (MAILBOX).\n";
        return mode;
      }
    }
  }

  // --- 2. USER SETTING: UNCAPPED PERFORMANCE (IMMEDIATE) ---
  if (activeConfig_.presentMode == PresentModeSetting::Immediate) {
    for (const auto &mode : availablePresentModes) {
      if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
        std::cout << "[VULKAN CONFIG] Present Mode: Uncapped Performance "
                     "(IMMEDIATE).\n";
        return mode;
      }
    }
  }

  // --- 3. FALLBACK / DEFAULT VSYNC (FIFO) ---
  // Enforced if user selected VSync OR if their hardware layout rejected
  // MAILBOX/IMMEDIATE
  std::cout << "[VULKAN CONFIG] Present Mode: VSync Enabled (FIFO).\n";
  return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D
VulkanRenderer::chooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities) {
  if (capabilities.currentExtent.width !=
      std::numeric_limits<uint32_t>::max()) {
    return capabilities.currentExtent;
  } else {
    int width, height;
    glfwGetFramebufferSize(window_, &width, &height);
    VkExtent2D actualExtent = {static_cast<uint32_t>(width),
                               static_cast<uint32_t>(height)};
    actualExtent.width =
        std::clamp(actualExtent.width, capabilities.minImageExtent.width,
                   capabilities.maxImageExtent.width);
    actualExtent.height =
        std::clamp(actualExtent.height, capabilities.minImageExtent.height,
                   capabilities.maxImageExtent.height);
    return actualExtent;
  }
}

void VulkanRenderer::drawFrame(SharedRenderState *renderState) {
  // 1. Critical Safety Check!
  if (device_ == VK_NULL_HANDLE) {
    return;
  }

  // 2. Handle an explicit resize flag before acquiring an image (e.g.,
  // immediate F11 toggle)
  if (framebufferResized_) {
    int width = 0, height = 0;
    glfwGetFramebufferSize(window_, &width, &height);

    // Handle window minimization (pause rendering if window is minimized)
    while (width == 0 || height == 0) {
      glfwGetFramebufferSize(window_, &width, &height);
      glfwWaitEvents();
    }

    vkDeviceWaitIdle(device_);
    recreateSwapchain(window_); // Rebuilds swapchain, image views, render pass,
                                // framebuffers, etc.
    framebufferResized_ = false;
    return; // Skip this frame and try again with the new layout
  }

  // Wait for the previous frame's fence
  vkWaitForFences(device_, 1, &inFlightFences_[currentFrame_], VK_TRUE,
                  UINT64_MAX);

  uint32_t imageIndex;
  VkResult result = vkAcquireNextImageKHR(
      device_, swapChain_, UINT64_MAX, imageAvailableSemaphores_[currentFrame_],
      VK_NULL_HANDLE, &imageIndex);

  // If the swapchain became out of date right during acquisition (e.g., screen
  // configuration change)
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    vkDeviceWaitIdle(device_);
    recreateSwapchain(window_);
    framebufferResized_ = false;
    return;
  } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    throw std::runtime_error("[VULKAN] Failed to acquire swap chain image!");
  }

  // Only reset the fence if we are actually succeeding and committing to work
  vkResetFences(device_, 1, &inFlightFences_[currentFrame_]);
  vkResetCommandBuffer(commandBuffers_[currentFrame_], 0);

  // =======================================================
  // 3. THE RENDER BRIDGE: Safely grab a snapshot of every known player
  // =======================================================
  std::vector<PlayerRenderState> players;
  if (renderState) {
    std::lock_guard<std::mutex> lock(renderState->mtx);
    players = renderState->players; // Copy and unlock quickly
  }

  // 3b. Update this frame's camera UBO — must happen before the command
  // buffer that references it is recorded, below.
  glm::vec3 localPlayerPos(0.0f);
  for (const auto& p : players) {
    if (p.isLocal) {
      localPlayerPos = glm::vec3(p.x, p.y, p.z);
      break;
    }
  }

  constexpr float kCameraTargetHeightOffset = 1.0f; // Chest/head height, not the ground origin
  glm::vec3 target = localPlayerPos + glm::vec3(0.0f, kCameraTargetHeightOffset, 0.0f);
  glm::vec3 camOffset(
      cameraDistance_ * std::cos(cameraPitch_) * std::sin(cameraYaw_),
      cameraDistance_ * std::sin(cameraPitch_),
      cameraDistance_ * std::cos(cameraPitch_) * std::cos(cameraYaw_));
  glm::vec3 camPos = target + camOffset;

  CameraUBO ubo{};
  ubo.view = glm::lookAt(camPos, target, glm::vec3(0.0f, 1.0f, 0.0f));
  float aspect = static_cast<float>(swapChainExtent_.width) / static_cast<float>(swapChainExtent_.height);
  ubo.proj = glm::perspective(glm::radians(60.0f), aspect, 0.1f, 100.0f);
  ubo.proj[1][1] *= -1.0f; // glm assumes GL clip space (Y up); Vulkan's is Y down
  std::memcpy(cameraUboMapped_[currentFrame_], &ubo, sizeof(ubo));

  // 4. Pass the snapshot down into the command buffer recorder!
  recordCommandBuffer(commandBuffers_[currentFrame_], imageIndex, players);

  // =======================================================

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

  VkSemaphore waitSemaphores[] = {imageAvailableSemaphores_[currentFrame_]};
  VkPipelineStageFlags waitStages[] = {
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};

  submitInfo.waitSemaphoreCount = 1;
  submitInfo.pWaitSemaphores = waitSemaphores;
  submitInfo.pWaitDstStageMask = waitStages;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &commandBuffers_[currentFrame_];

  // Indexed by imageIndex now, not currentFrame_ — this is the actual fix.
  VkSemaphore signalSemaphores[] = {renderFinishedSemaphores_[imageIndex]};
  submitInfo.signalSemaphoreCount = 1;
  submitInfo.pSignalSemaphores = signalSemaphores;

  if (vkQueueSubmit(graphicsQueue_, 1, &submitInfo,
                    inFlightFences_[currentFrame_]) != VK_SUCCESS) {
    throw std::runtime_error("[VULKAN] Failed to submit draw command buffer!");
  }

  VkPresentInfoKHR presentInfo{};
  presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  presentInfo.waitSemaphoreCount = 1;
  presentInfo.pWaitSemaphores = signalSemaphores; // same array, still correct

  VkSwapchainKHR swapChains[] = {swapChain_};
  presentInfo.swapchainCount = 1;
  presentInfo.pSwapchains = swapChains;
  presentInfo.pImageIndices = &imageIndex;

  result = vkQueuePresentKHR(presentQueue_, &presentInfo);

  // Handle out-of-date or suboptimal results on presentation
  if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
      framebufferResized_) {
    vkDeviceWaitIdle(device_);
    recreateSwapchain(window_);
    framebufferResized_ = false;
  } else if (result != VK_SUCCESS) {
    throw std::runtime_error("[VULKAN] Failed to present swap chain image!");
  }

  currentFrame_ = (currentFrame_ + 1) % MAX_FRAMES_IN_FLIGHT;
}

void VulkanRenderer::cleanup() {
  if (device_ == VK_NULL_HANDLE)
    return;

  vkDeviceWaitIdle(device_);

  // Loop through actual vector sizes, not MAX_FRAMES_IN_FLIGHT
  for (auto semaphore : renderFinishedSemaphores_) {
    if (semaphore != VK_NULL_HANDLE)
      vkDestroySemaphore(device_, semaphore, nullptr);
  }
  for (auto semaphore : imageAvailableSemaphores_) {
    if (semaphore != VK_NULL_HANDLE)
      vkDestroySemaphore(device_, semaphore, nullptr);
  }
  for (auto fence : inFlightFences_) {
    if (fence != VK_NULL_HANDLE)
      vkDestroyFence(device_, fence, nullptr);
  }

  if (commandPool_ != VK_NULL_HANDLE)
    vkDestroyCommandPool(device_, commandPool_, nullptr);

  for (auto framebuffer : swapChainFramebuffers_) {
    if (framebuffer != VK_NULL_HANDLE)
      vkDestroyFramebuffer(device_, framebuffer, nullptr);
  }

  if (graphicsPipeline_ != VK_NULL_HANDLE)
    vkDestroyPipeline(device_, graphicsPipeline_, nullptr);
  if (pipelineLayout_ != VK_NULL_HANDLE)
    vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
  if (renderPass_ != VK_NULL_HANDLE)
    vkDestroyRenderPass(device_, renderPass_, nullptr);

  for (auto imageView : swapChainImageViews_) {
    if (imageView != VK_NULL_HANDLE)
      vkDestroyImageView(device_, imageView, nullptr);
  }

  if (swapChain_ != VK_NULL_HANDLE)
    vkDestroySwapchainKHR(device_, swapChain_, nullptr);

  // Depth resources — cleanup() doesn't call cleanupSwapChain() (it inlines
  // its own duplicate teardown of framebuffers/image views/swapchain above),
  // so depth needs the same explicit duplication here.
  if (depthImageView_ != VK_NULL_HANDLE)
    vkDestroyImageView(device_, depthImageView_, nullptr);
  if (depthImage_ != VK_NULL_HANDLE)
    vkDestroyImage(device_, depthImage_, nullptr);
  if (depthImageMemory_ != VK_NULL_HANDLE)
    vkFreeMemory(device_, depthImageMemory_, nullptr);

  // Camera UBO buffers — persistent for the app's lifetime, not swapchain-
  // dependent. Unmap before destroy/free since these were persistently
  // mapped at creation. Iterate actual vector size, not MAX_FRAMES_IN_FLIGHT,
  // to stay safe if createCameraUniformBuffers() never ran (partial-init exception path).
  for (size_t i = 0; i < cameraUboBuffers_.size(); ++i) {
    if (cameraUboMapped_[i]) {
      vkUnmapMemory(device_, cameraUboBuffersMemory_[i]);
    }
    if (cameraUboBuffers_[i] != VK_NULL_HANDLE)
      vkDestroyBuffer(device_, cameraUboBuffers_[i], nullptr);
    if (cameraUboBuffersMemory_[i] != VK_NULL_HANDLE)
      vkFreeMemory(device_, cameraUboBuffersMemory_[i], nullptr);
  }

  // New geometry/texture/descriptor resources — all swapchain-independent,
  // so nothing here needs to touch cleanupSwapChain()/recreateSwapchain().
  if (descriptorPool_ != VK_NULL_HANDLE)
    vkDestroyDescriptorPool(device_, descriptorPool_, nullptr); // Also frees descriptorSets_
  if (textureSampler_ != VK_NULL_HANDLE)
    vkDestroySampler(device_, textureSampler_, nullptr);
  if (textureImageView_ != VK_NULL_HANDLE)
    vkDestroyImageView(device_, textureImageView_, nullptr);
  if (textureImage_ != VK_NULL_HANDLE)
    vkDestroyImage(device_, textureImage_, nullptr);
  if (textureImageMemory_ != VK_NULL_HANDLE)
    vkFreeMemory(device_, textureImageMemory_, nullptr);
  if (descriptorSetLayout_ != VK_NULL_HANDLE)
    vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
  if (indexBuffer_ != VK_NULL_HANDLE)
    vkDestroyBuffer(device_, indexBuffer_, nullptr);
  if (indexBufferMemory_ != VK_NULL_HANDLE)
    vkFreeMemory(device_, indexBufferMemory_, nullptr);
  if (vertexBuffer_ != VK_NULL_HANDLE)
    vkDestroyBuffer(device_, vertexBuffer_, nullptr);
  if (vertexBufferMemory_ != VK_NULL_HANDLE)
    vkFreeMemory(device_, vertexBufferMemory_, nullptr);

  vkDestroyDevice(device_, nullptr);

  if (instance_ != VK_NULL_HANDLE) {
    if (surface_ != VK_NULL_HANDLE)
      vkDestroySurfaceKHR(instance_, surface_, nullptr);
    vkDestroyInstance(instance_, nullptr);
  }
  std::cout << "[VULKAN] Cleaned up successfully." << std::endl;
}
void VulkanRenderer::toggleFullscreen(GLFWwindow *window) {
  windowState_.isFullscreen = !windowState_.isFullscreen;

  if (windowState_.isFullscreen) {
    // 1. Save current window position and size before making the jump
    glfwGetWindowPos(window, &windowState_.windowedX, &windowState_.windowedY);
    glfwGetWindowSize(window, &windowState_.windowedWidth,
                      &windowState_.windowedHeight);

    // 2. Determine which monitor the window is currently on
    GLFWmonitor *monitor = glfwGetWindowMonitor(window);
    if (!monitor) {
      // If windowed, find the monitor that contains the top-left corner of our
      // window
      int monitorCount;
      GLFWmonitor **monitors = glfwGetMonitors(&monitorCount);
      monitor = monitors[0]; // fallback default

      int wx, wy;
      glfwGetWindowPos(window, &wx, &wy);

      for (int i = 0; i < monitorCount; ++i) {
        int mx, my, mw, mh;
        glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
        if (wx >= mx && wx < mx + mw && wy >= my && wy < my + mh) {
          monitor = monitors[i];
          break;
        }
      }
    }

    // 3. Get the native resolution of that monitor (3440x1440, 1920x1080, etc.)
    const GLFWvidmode *mode = glfwGetVideoMode(monitor);

    // 4. Set to true exclusive fullscreen
    // Pass the monitor pointer, target width, target height, and target refresh
    // rate
    glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height,
                         mode->refreshRate);
    std::cout << "[Graphics] Switched to Fullscreen: " << mode->width << "x"
              << mode->height << " @" << mode->refreshRate << "Hz\n";
  } else {
    // Restore windowed state back to 720p at its previous position
    glfwSetWindowMonitor(window, nullptr, windowState_.windowedX,
                         windowState_.windowedY, windowState_.windowedWidth,
                         windowState_.windowedHeight, 0);
    std::cout << "[Graphics] Restored to Windowed Mode: "
              << windowState_.windowedWidth << "x"
              << windowState_.windowedHeight << "\n";
  }

  // Force a swapchain recreation because our frame boundaries completely
  // changed
  framebufferResized_ = true;
}
void VulkanRenderer::cleanupSwapChain() {
  // 1. Tear down old framebuffers bound to previous resolution extents
  for (auto framebuffer : swapChainFramebuffers_) {
    if (framebuffer != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
  }
  swapChainFramebuffers_.clear();

  // 2. Clean up image views wrapping the old swapchain images
  for (auto imageView : swapChainImageViews_) {
    if (imageView != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, imageView, nullptr);
    }
  }
  swapChainImageViews_.clear();

  // 3. Destroy the actual VkSwapchainKHR handle
  if (swapChain_ != VK_NULL_HANDLE) {
    vkDestroySwapchainKHR(device_, swapChain_, nullptr);
    swapChain_ = VK_NULL_HANDLE;
  }

  // 4. Depth resources — swapchain-extent-dependent, unlike the
  // vertex/index/texture resources from the texture task.
  if (depthImageView_ != VK_NULL_HANDLE) {
    vkDestroyImageView(device_, depthImageView_, nullptr);
    depthImageView_ = VK_NULL_HANDLE;
  }
  if (depthImage_ != VK_NULL_HANDLE) {
    vkDestroyImage(device_, depthImage_, nullptr);
    depthImage_ = VK_NULL_HANDLE;
  }
  if (depthImageMemory_ != VK_NULL_HANDLE) {
    vkFreeMemory(device_, depthImageMemory_, nullptr);
    depthImageMemory_ = VK_NULL_HANDLE;
  }
}

void VulkanRenderer::recreateSwapchain(GLFWwindow *window) {
  window_ = window;

  int width = 0, height = 0;
  glfwGetFramebufferSize(window_, &width, &height);

  while (width == 0 || height == 0) {
    glfwGetFramebufferSize(window_, &width, &height);
    glfwWaitEvents();
  }

  vkDeviceWaitIdle(device_);

  cleanupSwapChain();

  createSwapChain();
  createImageViews();
  createDepthResources(); // Needs the fresh swapChainExtent_; depthFormat_ is a device
                          // capability, not swapchain-dependent, so it doesn't need re-resolving
  createFramebuffers();

  // Image count may change across recreation — rebuild the per-image
  // semaphores to match rather than risk a stale/mismatched array.
  for (auto semaphore : renderFinishedSemaphores_) {
    if (semaphore != VK_NULL_HANDLE)
      vkDestroySemaphore(device_, semaphore, nullptr);
  }
  renderFinishedSemaphores_.resize(swapChainImages_.size());
  VkSemaphoreCreateInfo semaphoreInfo{};
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  for (size_t i = 0; i < renderFinishedSemaphores_.size(); i++) {
    if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr,
                          &renderFinishedSemaphores_[i]) != VK_SUCCESS) {
      throw std::runtime_error(
          "[VULKAN] Failed to recreate render-finished semaphores!");
    }
  }

  std::cout << "[Vulkan] Swapchain safely recreated at resolution: " << width
            << "x" << height << "\n";
}

void VulkanRenderer::glfw_key_callback(GLFWwindow *window, int key,
                                       int scancode, int action, int mods) {
  auto *renderer =
      reinterpret_cast<VulkanRenderer *>(glfwGetWindowUserPointer(window));
  if (renderer) {
    renderer->handleKeyInput(key, scancode, action, mods);
  }
}

void VulkanRenderer::glfw_cursor_pos_callback(GLFWwindow *window, double xpos, double ypos) {
  auto *renderer =
      reinterpret_cast<VulkanRenderer *>(glfwGetWindowUserPointer(window));
  if (renderer) {
    renderer->handleCursorPos(xpos, ypos);
  }
}

void VulkanRenderer::glfw_mouse_button_callback(GLFWwindow *window, int button, int action, int mods) {
  auto *renderer =
      reinterpret_cast<VulkanRenderer *>(glfwGetWindowUserPointer(window));
  if (renderer) {
    renderer->handleMouseButton(button, action, mods);
  }
}

void VulkanRenderer::glfw_scroll_callback(GLFWwindow *window, double xoffset, double yoffset) {
  auto *renderer =
      reinterpret_cast<VulkanRenderer *>(glfwGetWindowUserPointer(window));
  if (renderer) {
    renderer->handleScroll(xoffset, yoffset);
  }
}

void VulkanRenderer::handleMouseButton(int button, int action, int mods) {
  if (button != GLFW_MOUSE_BUTTON_RIGHT) return;
  if (action == GLFW_PRESS) {
    isRightMouseDown_ = true;
    glfwGetCursorPos(window_, &lastMouseX_, &lastMouseY_); // baseline so the first delta isn't a huge jump
    glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
  } else if (action == GLFW_RELEASE) {
    isRightMouseDown_ = false;
    glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
  }
  // Releasing intentionally does nothing to cameraYaw_/cameraPitch_ — the
  // camera holds its last orientation.
}

void VulkanRenderer::handleCursorPos(double xpos, double ypos) {
  if (!isRightMouseDown_) {
    lastMouseX_ = xpos;
    lastMouseY_ = ypos; // keep the baseline current even while not looking
    return;
  }
  double deltaX = xpos - lastMouseX_;
  double deltaY = ypos - lastMouseY_;
  lastMouseX_ = xpos;
  lastMouseY_ = ypos;

  cameraYaw_ += static_cast<float>(deltaX) * kMouseSensitivity;
  cameraPitch_ += static_cast<float>(-deltaY) * kMouseSensitivity; // GLFW Y grows downward — invert
  cameraPitch_ = std::clamp(cameraPitch_, kMinPitch, kMaxPitch);
}

void VulkanRenderer::handleScroll(double xoffset, double yoffset) {
  (void)xoffset;
  cameraDistance_ -= static_cast<float>(yoffset) * kScrollZoomStep;
  cameraDistance_ = std::clamp(cameraDistance_, kMinDistance, kMaxDistance);
}

void VulkanRenderer::handleKeyInput(int key, int scancode, int action,
                                    int mods) {
  if (action == GLFW_PRESS) {
    if (key == GLFW_KEY_LEFT || key == GLFW_KEY_A)
      is_moving_left_ = true;
    if (key == GLFW_KEY_RIGHT || key == GLFW_KEY_D)
      is_moving_right_ = true;
    if (key == GLFW_KEY_UP || key == GLFW_KEY_W)
      is_moving_up_ = true;
    if (key == GLFW_KEY_DOWN || key == GLFW_KEY_S)
      is_moving_down_ = true;

    if (key == GLFW_KEY_F11) {
      toggleFullscreen(window_);
    }
  } else if (action == GLFW_RELEASE) {
    if (key == GLFW_KEY_LEFT || key == GLFW_KEY_A)
      is_moving_left_ = false;
    if (key == GLFW_KEY_RIGHT || key == GLFW_KEY_D)
      is_moving_right_ = false;
    if (key == GLFW_KEY_UP || key == GLFW_KEY_W)
      is_moving_up_ = false;
    if (key == GLFW_KEY_DOWN || key == GLFW_KEY_S)
      is_moving_down_ = false;
  }
}
