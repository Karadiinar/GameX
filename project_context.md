# Project Structure
```
.
 |-CMakeLists.txt
 |-client
 | |-CMakeLists.txt
 | |-include
 | | |-GraphicsConfig.hpp
 | | |-NetworkManager.hpp
 | | |-VulkanRenderer.hpp
 | |-shaders
 | | |-frag.spv
 | | |-shader.frag
 | | |-shader.vert
 | | |-vert.spv
 | |-src
 | | |-main.cpp
 | | |-NetworkManager.cpp
 | | |-VulkanRenderer.cpp
 |-common
 | |-CMakeLists.txt
 | |-include
 | | |-ThreadUtility.hpp
 | | |-Version.hpp
 | | |-Protocol.hpp
 | | |-Movement.hpp
 |-project_context.md
 |-server
 | |-CMakeLists.txt
 | |-game
 | | |-CMakeLists.txt
 | | |-src
 | | | |-main.cpp
 | |-login
 | | |-src
 | | | |-main.cpp
 | | |-schema
 | | | |-001_init.sql
 | | | |-002_sessions_and_characters.sql
 | | |-CMakeLists.txt
 |-run.sh
 |-dump.sh
 |-CLAUDE.md
 |-compile_commands.json
 |-tests
 | |-test_protocol.cpp
 | |-test_movement.cpp
 | |-CMakeLists.txt
 |-dump_complete.sh
```

# Source and Build Files
## File: ./CMakeLists.txt
```cmake
cmake_minimum_required(VERSION 3.22)
project(RebelMMO CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 1. Find our dependencies
find_package(Vulkan REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(EnTT REQUIRED)

# 2. Add our sub-projects
add_subdirectory(common)
add_subdirectory(server)
add_subdirectory(client)

# 3. Tests
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()```

## File: ./client/CMakeLists.txt
```cmake
# Find the necessary system threading and Asio
find_package(Threads REQUIRED)

add_executable(client 
    src/main.cpp
    src/VulkanRenderer.cpp
    src/NetworkManager.cpp
)

target_include_directories(client PRIVATE 
    include 
    ${CMAKE_SOURCE_DIR}/common/include
)

# Link to our shared code, graphics, and networking
target_link_libraries(client PRIVATE 
    common 
    Vulkan::Vulkan 
    glfw 
    glm::glm
    Threads::Threads
    asio  # Ensure your system's asio find_package or alias is defined
)

# Shader copy command remains the same
add_custom_command(
    TARGET client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders
    $<TARGET_FILE_DIR:client>/shaders
)```

## File: ./client/include/GraphicsConfig.hpp
```cpp
#pragma once
#include <vulkan/vulkan.h>

enum class PresentModeSetting {
    Immediate = 0, // Uncapped (8000 FPS room-heater mode)
    VSync,         // Frame-capped, synchronized (FIFO)
    TripleBuffer   // Low-latency, frame-capped fallback (MAILBOX)
};

struct GraphicsConfig {
    // Present mode configuration
    PresentModeSetting presentMode = PresentModeSetting::TripleBuffer;
    
    // Window settings
    int windowWidth = 1280;
    int windowHeight = 720;
    bool fullscreen = false;
    
    // Future settings placeholders
    bool enableValidationLayers = true;
    bool shadowQuality = true;
};```

## File: ./client/include/NetworkManager.hpp
```cpp
#pragma once
#include <asio.hpp>
#include <memory>
#include <thread>
#include <map>
#include <functional>
#include <vector>
#include <optional>
#include "Protocol.hpp"
#include "ThreadUtility.hpp" // For Rebel::Concurrent::ThreadSafeQueue

namespace Rebel
{
    // Wrapper for packets stored in the queue
    struct InboundPacket
    {
        PacketHeader header;
        std::vector<uint8_t> payload;
    };
}

// Inside NetworkManager.hpp
class NetworkManager
{
public:
    NetworkManager();
    ~NetworkManager();

    std::thread &getThread() { return network_thread_; }
    // onConnected (if given) runs on the network thread once the socket is
    // up, so callers can send whatever auth packet fits this connection
    // (initial login vs. a post-redirect game-server token) without
    // NetworkManager needing to know which.
    void connect(const std::string &host, const std::string &port,
                 std::function<void()> onConnected = nullptr);
    void disconnect();
    void sendPacket(const Rebel::PacketHeader &header, const void *payload = nullptr, std::size_t payloadSize = 0);

    // This is how the Logic Thread gets its data now
    Rebel::Concurrent::ThreadSafeQueue<Rebel::InboundPacket> &getPacketQueue()
    {
        return inbound_queue_;
    }

private:
    void startRead();
    void readPayload(uint16_t size);

    asio::io_context io_context_;
    asio::strand<asio::io_context::executor_type> strand_;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work_guard_;

    std::optional<asio::ip::tcp::socket> socket_;
    std::thread network_thread_;

    Rebel::PacketHeader incoming_header_;
    std::vector<uint8_t> incoming_payload_;

    // The mailbox replacing the handlers_ map
    Rebel::Concurrent::ThreadSafeQueue<Rebel::InboundPacket> inbound_queue_;
};
```

## File: ./client/include/VulkanRenderer.hpp
```cpp
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

    static void glfw_key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);
    float getPlayerX() const { return player_x_; }
    
    void handleKeyInput(int key, int scancode, int action, int mods);
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
    WindowState  windowState_;;
    bool framebufferResized_ = false;
    GLFWwindow* window_ = nullptr;
    
    void recreateSwapchain(GLFWwindow* window);

    bool is_moving_left_ = false;
    bool is_moving_right_ = false;
    bool is_moving_up_ = false;
    bool is_moving_down_ = false;

    float player_x_ = 0.0f;
    
};```

## File: ./client/shaders/shader.frag
```glsl
#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(fragColor, 1.0);
}```

## File: ./client/shaders/shader.vert
```glsl
#version 450

layout(location = 0) out vec3 fragColor;

// 1. Declare the Push Constant block: position plus a flag for "is this me?"
layout(push_constant) uniform PushConstants {
    vec2 player_pos;
    float is_local;
} pc;

vec2 positions[3] = vec2[](
    vec2(0.0, -0.5),
    vec2(0.5, 0.5),
    vec2(-0.5, 0.5)
);

vec3 colors[3] = vec3[](
    vec3(1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0),
    vec3(0.0, 0.0, 1.0)
);

void main() {
    // 2. Add the vec2 offset directly to our base vertex coordinates!
    gl_Position = vec4(positions[gl_VertexIndex] + pc.player_pos, 0.0, 1.0);

    // Remote players are tinted toward red so the local player stands out.
    vec3 remoteTint = vec3(1.0, 0.4, 0.4);
    fragColor = mix(colors[gl_VertexIndex] * remoteTint, colors[gl_VertexIndex], pc.is_local);
}```

## File: ./client/src/main.cpp
```cpp
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <entt/entt.hpp>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Movement.hpp"

// Must be included before custom graphics headers to ensure Vulkan macros are
// set
#include <GLFW/glfw3.h>

#include "NetworkManager.hpp"
#include "ThreadUtility.hpp"
#include "VulkanRenderer.hpp"
#include <mutex>

// Global shutdown flag to synchronize thread termination
std::atomic<bool> g_Running{true};

struct Transform {
  float x, y, z;
  float yaw;
};

struct LocalPlayerTag {};
struct RemotePlayerTag {};

/**
 * Logic Thread: Responsible for game state evolution, physics, and packet
 * processing.
 */
void LogicThreadEntry(NetworkManager *network, SharedRenderState *renderState,
                      const VulkanRenderer *renderer) {
  entt::registry client_registry; // <-- The Client's World Data
  entt::entity local_player =
      entt::null; // <-- Handle to our specific character
  uint32_t local_character_id = 0; // From SMSG_GAME_AUTH_OK — identifies "self" in SMSG_PLAYER_STATE
  std::unordered_map<uint32_t, entt::entity> remote_players; // character_id -> entity
  bool inWorld = false;

  const std::chrono::microseconds TICK_TIME(1000000 / 64);
  std::cout << "[Logic] Engine tick thread started.\n";

  while (g_Running) {
    auto start = std::chrono::steady_clock::now();

    // Drain the mailbox
    while (auto packetOpt = network->getPacketQueue().try_pop()) {
      auto &packet = *packetOpt;

      if (packet.header.opcode ==
              static_cast<uint16_t>(Rebel::Opcode::SMSG_AUTH_RESPONSE) &&
          packet.payload.size() >= sizeof(Rebel::MsgRedirect)) {
        auto *redirect =
            reinterpret_cast<Rebel::MsgRedirect *>(packet.payload.data());
        std::string gameIp(redirect->ip);
        uint16_t gamePort = redirect->port;
        std::string sessionToken(redirect->session_token,
                                 sizeof(redirect->session_token));
        std::cout << "[Logic] Redirecting to Game Server at " << gameIp
                  << ":" << gamePort << "\n";

        network->disconnect();
        network->connect(gameIp, std::to_string(gamePort),
                         [network, sessionToken]() {
                           Rebel::MsgGameAuth authData;
                           std::memcpy(authData.session_token,
                                       sessionToken.data(),
                                       sizeof(authData.session_token));

                           Rebel::PacketHeader header;
                           header.size = sizeof(Rebel::PacketHeader) +
                                         sizeof(Rebel::MsgGameAuth);
                           header.opcode = static_cast<uint16_t>(
                               Rebel::Opcode::CMSG_GAME_AUTH);

                           network->sendPacket(header, &authData,
                                               sizeof(Rebel::MsgGameAuth));
                         });
      } else if (packet.header.opcode ==
                 static_cast<uint16_t>(Rebel::Opcode::SMSG_GAME_AUTH_OK)) {
        if (packet.payload.size() >= sizeof(Rebel::MsgGameAuthOk)) {
          auto *ok =
              reinterpret_cast<Rebel::MsgGameAuthOk *>(packet.payload.data());
          local_character_id = ok->character_id;
        }
        std::cout << "[Logic] Successfully authenticated with Game Server. "
                     "Entering world...\n";
        inWorld = true;

        // --- SPAWN THE LOCAL ENTITY ---
        local_player = client_registry.create();
        client_registry.emplace<Transform>(local_player, 0.0f, 0.0f, 0.0f,
                                           0.0f);
        client_registry.emplace<LocalPlayerTag>(local_player);
        std::cout << "[Logic] Local player entity spawned in client ECS.\n";
      } else if (packet.header.opcode ==
                 static_cast<uint16_t>(Rebel::Opcode::SMSG_GAME_AUTH_FAIL)) {
        std::cerr << "[Logic] Game Server rejected our session token.\n";
      } else if (packet.header.opcode ==
                 static_cast<uint16_t>(Rebel::Opcode::SMSG_PLAYER_STATE)) {
        if (packet.payload.size() >= sizeof(Rebel::MsgPlayerState)) {
          auto *state = reinterpret_cast<Rebel::MsgPlayerState *>(
              packet.payload.data());

          if (inWorld && state->character_id == local_character_id &&
              client_registry.valid(local_player)) {
            // Reconciliation: snap our predicted position to the server's.
            auto &transform = client_registry.get<Transform>(local_player);
            transform.x = state->x;
            transform.y = state->y;
            transform.z = state->z;
            transform.yaw = state->yaw;
          } else if (inWorld && state->character_id != local_character_id) {
            // Someone else — get-or-create their entity and update it.
            auto it = remote_players.find(state->character_id);
            entt::entity remote;
            if (it == remote_players.end()) {
              remote = client_registry.create();
              client_registry.emplace<Transform>(remote, 0.0f, 0.0f, 0.0f, 0.0f);
              client_registry.emplace<RemotePlayerTag>(remote);
              remote_players[state->character_id] = remote;
              std::cout << "[Logic] Tracking new remote player, character_id="
                        << state->character_id << "\n";
            } else {
              remote = it->second;
            }
            auto &transform = client_registry.get<Transform>(remote);
            transform.x = state->x;
            transform.y = state->y;
            transform.z = state->z;
            transform.yaw = state->yaw;
          }
        }
      } else if (packet.header.opcode ==
                 static_cast<uint16_t>(Rebel::Opcode::SMSG_PLAYER_LEAVE)) {
        if (packet.payload.size() >= sizeof(Rebel::MsgPlayerLeave)) {
          auto *leave = reinterpret_cast<Rebel::MsgPlayerLeave *>(
              packet.payload.data());
          auto it = remote_players.find(leave->character_id);
          if (it != remote_players.end()) {
            if (client_registry.valid(it->second)) {
              client_registry.destroy(it->second);
            }
            remote_players.erase(it);
          }
        }
      }
    }

    // --- PREDICT LOCALLY, REPORT INTENT, PUBLISH THE FULL RENDER SNAPSHOT ---
    if (inWorld && client_registry.valid(local_player)) {
      auto &transform = client_registry.get<Transform>(local_player);

      uint8_t moveFlags = 0;
      if (renderer->isMovingLeft()) {
        moveFlags |= Rebel::MoveFlags::Left;
      }
      if (renderer->isMovingRight()) {
        moveFlags |= Rebel::MoveFlags::Right;
      }
      if (renderer->isMovingUp()) {
        moveFlags |= Rebel::MoveFlags::Up;
      }
      if (renderer->isMovingDown()) {
        moveFlags |= Rebel::MoveFlags::Down;
      }

      // 1. PREDICT: apply the exact same movement math the server uses, at
      // this thread's 64Hz rate, so movement feels instant instead of
      // waiting on a round trip. SMSG_PLAYER_STATE (handled above) then
      // reconciles/corrects `transform` whenever the server's own tick
      // (20Hz) catches up — a hard snap, no smoothing, by design for now.
      const float clientDelta =
          Rebel::PLAYER_SPEED_PER_SEC *
          (std::chrono::duration<float>(TICK_TIME).count());
      Rebel::Vec2 predicted = Rebel::apply_move_flags(
          {transform.x, transform.y}, moveFlags, clientDelta);
      transform.x = predicted.x;
      transform.y = predicted.y;

      // 2. UPDATE THE RENDER BRIDGE — a simple camera centered on us, so we
      // never drift off-screen ourselves. Everyone else is positioned
      // relative to our own world position and scaled into NDC; this is
      // gameplay-ish logic (camera behavior), so it belongs here on the
      // logic thread, not in VulkanRenderer. World-space math above (in
      // step 1) is untouched by this — only what we hand to the renderer
      // is transformed.
      constexpr float VIEW_SCALE = 0.1f; // World units -> NDC; tune freely
      {
        std::vector<PlayerRenderState> snapshot;
        snapshot.push_back({local_character_id, 0.0f, 0.0f, transform.z, true});
        for (auto &[character_id, entity] : remote_players) {
          if (!client_registry.valid(entity)) continue;
          auto &remoteTransform = client_registry.get<Transform>(entity);
          snapshot.push_back({character_id,
                              (remoteTransform.x - transform.x) * VIEW_SCALE,
                              (remoteTransform.y - transform.y) * VIEW_SCALE,
                              remoteTransform.z, false});
        }

        std::lock_guard<std::mutex> lock(renderState->mtx);
        renderState->players = std::move(snapshot);
      }

      // 3. Send our input intent — not a position — for the server to simulate
      Rebel::MsgPlayerMove moveData;
      moveData.moveFlags = moveFlags;
      moveData.yaw = transform.yaw;

      Rebel::PacketHeader head;
      head.opcode = static_cast<uint16_t>(Rebel::Opcode::CMSG_PLAYER_MOVE);
      head.size = sizeof(Rebel::PacketHeader) + sizeof(Rebel::MsgPlayerMove);

      network->sendPacket(head, &moveData, sizeof(Rebel::MsgPlayerMove));
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    if (elapsed < TICK_TIME) {
      std::this_thread::sleep_for(TICK_TIME - elapsed);
    }
  }
  std::cout << "[Logic] Engine tick thread exiting...\n";
}

int main() {
  std::cout << "--- Rebel MMO Project (Client) ---\n";

  // --- 1. HARDWARE DISCOVERY ---
  auto coreMap = Rebel::ThreadUtils::GetPhysicalCoreMap();
  std::cout << "[Main] Discovered " << coreMap.size() << " physical cores.\n";

  // --- 2. NETWORK SUBSYSTEM ---
  NetworkManager network;
  if (coreMap.size() > 1) {
    std::cout << "[Main] Pinning Network subsystem to Core " << coreMap[1]
              << "\n";
    Rebel::ThreadUtils::SetThreadAffinity(network.getThread(), coreMap[1]);
  }

  // TODO: replace with real login-screen credentials once one exists.
  network.connect("127.0.0.1", "54321", [&network]() {
    Rebel::MsgLogin loginData;
    std::strncpy(loginData.username, "Karadiinar", sizeof(loginData.username));
    std::strncpy(loginData.password, "changeme", sizeof(loginData.password));
    loginData.version = 1;

    Rebel::PacketHeader header;
    header.size = sizeof(Rebel::PacketHeader) + sizeof(Rebel::MsgLogin);
    header.opcode = static_cast<uint16_t>(Rebel::Opcode::CMSG_AUTH_SESSION);

    network.sendPacket(header, &loginData, sizeof(Rebel::MsgLogin));
  });

  // --- INSTANTIATE THE BRIDGE ---
  SharedRenderState sharedRenderState;

  // --- 4. GRAPHICS SUBSYSTEM ENVIRONMENT INITIALIZATION ---
  try {
    // Fire up GLFW before calling window manipulation functions
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    if (!glfwInit()) {
      throw std::runtime_error("[Graphics] Failed to initialize GLFW!");
    }

    // Setup default configuration settings
    GraphicsConfig userGraphicsSettings;
    userGraphicsSettings.presentMode = PresentModeSetting::TripleBuffer;
    userGraphicsSettings.windowWidth = 1280;
    userGraphicsSettings.windowHeight = 720;

    // Flush hints and explicitly declare Vulkan API support before generating
    // the container
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    // Create the window context
    GLFWwindow *window = glfwCreateWindow(userGraphicsSettings.windowWidth,
                                          userGraphicsSettings.windowHeight,
                                          "Rebel Client", nullptr, nullptr);

    if (!window) {
      throw std::runtime_error("[Graphics] Failed to create GLFW window!");
    }

    // --- DECLARED FIRST: The Renderer instance now exists in memory ---
    VulkanRenderer renderer;

    // --- 3. SPAWN LOGIC SUBSYSTEM (Safe to pass &renderer now!) ---
    std::thread logicThread(LogicThreadEntry, &network, &sharedRenderState,
                            &renderer);
    if (coreMap.size() > 2) {
      std::cout << "[Main] Pinning Logic subsystem to Core " << coreMap[2]
                << "\n";
      Rebel::ThreadUtils::SetThreadAffinity(logicThread, coreMap[2]);
    }

    // Pass the window context and config to the initialization pipeline
    renderer.init(window, userGraphicsSettings);

    // Tell GLFW: "Hey, associate this 'renderer' C++ object with this specific
    // window."
    glfwSetWindowUserPointer(window, &renderer);

    // Tell GLFW: "When a key is pressed, execute our static callback handler."
    glfwSetKeyCallback(window, VulkanRenderer::glfw_key_callback);

    // Set up the window resize callback lambda
    glfwSetFramebufferSizeCallback(
        window, [](GLFWwindow *win, int width, int height) {
          auto *rend =
              reinterpret_cast<VulkanRenderer *>(glfwGetWindowUserPointer(win));
          if (rend) {
            rend->framebufferResizeCallback();
          }
        });

    // --- UNIFIED MAIN RENDER LOOP ---
    while (!renderer.shouldClose()) {
      renderer.pollEvents(); // Processes keystrokes & dispatches the callback
                             // functions
      renderer.drawFrame(&sharedRenderState);
    }

    // --- 5. CLEAN SHUTDOWN SEQUENCE ---
    std::cout
        << "\n[Main] Shutdown signal received. Terminating subsystems...\n";
    g_Running = false;

    if (logicThread.joinable()) {
      logicThread.join();
    }

    renderer.cleanup();
    glfwDestroyWindow(window);
    glfwTerminate();
  } catch (const std::exception &e) {
    std::cerr << "\n[Fatal Error] " << e.what() << std::endl;
    g_Running = false;
    glfwTerminate();
    return -1;
  }

  std::cout << "[Main] Clean shutdown complete.\n";
  return 0;
}
```

## File: ./client/src/NetworkManager.cpp
```cpp
#include "NetworkManager.hpp"
#include <iostream>

NetworkManager::NetworkManager()
    : io_context_(), strand_(asio::make_strand(io_context_)),
      work_guard_(std::make_unique<
                  asio::executor_work_guard<asio::io_context::executor_type>>(
          asio::make_work_guard(io_context_))) {
  // The network thread is exclusively responsible for driving ASIO
  network_thread_ = std::thread([this]() { io_context_.run(); });
}

NetworkManager::~NetworkManager() {
  disconnect();
  work_guard_.reset(); // Allow io_context_.run() to exit when work is done
  io_context_.stop();
  if (network_thread_.joinable()) {
    network_thread_.join();
  }
}

void NetworkManager::connect(const std::string &host, const std::string &port,
                             std::function<void()> onConnected) {
  // Post the connection attempt to the strand so it executes safely on the
  // network thread
  asio::post(strand_, [this, host, port, onConnected]() {
    try {
      asio::ip::tcp::resolver resolver(io_context_);
      auto endpoints = resolver.resolve(host, port);

      socket_.emplace(io_context_);

      asio::async_connect(
          *socket_, endpoints,
          asio::bind_executor(strand_, [this, onConnected](std::error_code ec,
                                              asio::ip::tcp::endpoint) {
            if (!ec) {
              std::cout << "[Network] Connected to server.\n";
              startRead();
              if (onConnected) {
                onConnected();
              }
            } else {
              std::cerr << "[Network] Connect error: " << ec.message() << "\n";
            }
          }));
    } catch (const std::exception &e) {
      std::cerr << "[Network] Exception during connect: " << e.what() << "\n";
    }
  });
}

void NetworkManager::disconnect() {
  asio::post(strand_, [this]() {
    if (socket_ && socket_->is_open()) {
      std::error_code ec;
      socket_->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
      socket_->close(ec);
      socket_.reset();
      std::cout << "[Network] Disconnected.\n";
    }
  });
}

void NetworkManager::sendPacket(const Rebel::PacketHeader &header,
                                const void *payload, std::size_t payloadSize) {
  // We must copy the data before posting, because the caller (Logic Thread)
  // might destroy or modify the original buffers before the async write
  // happens.
  auto buffer = std::make_shared<std::vector<uint8_t>>(
      sizeof(Rebel::PacketHeader) + payloadSize);
  std::memcpy(buffer->data(), &header, sizeof(Rebel::PacketHeader));
  if (payloadSize > 0 && payload != nullptr) {
    std::memcpy(buffer->data() + sizeof(Rebel::PacketHeader), payload,
                payloadSize);
  }

  // Post the write operation to the strand
  asio::post(strand_, [this, buffer]() {
    if (!socket_ || !socket_->is_open())
      return;

    asio::async_write(
        *socket_, asio::buffer(*buffer),
        asio::bind_executor(
            strand_, [buffer](std::error_code ec, std::size_t /*length*/) {
              if (ec) {
                std::cerr << "[Network] Write error: " << ec.message() << "\n";
              }
            }));
  });
}

void NetworkManager::startRead() {
  if (!socket_ || !socket_->is_open())
    return;

  asio::async_read(
      *socket_, asio::buffer(&incoming_header_, sizeof(Rebel::PacketHeader)),
      asio::bind_executor(strand_, [this](std::error_code ec,
                                          std::size_t /*length*/) {
        if (!ec) {
          if (incoming_header_.size > sizeof(Rebel::PacketHeader)) {
            // Calculate payload size (Total size - Header size)
            readPayload(incoming_header_.size - sizeof(Rebel::PacketHeader));
          } else {
            // No payload: Push header immediately to the Logic Thread queue
            Rebel::InboundPacket pkg;
            pkg.header = incoming_header_;
            inbound_queue_.push(std::move(pkg));

            startRead(); // Wait for next packet
          }
        } else {
          if (ec != asio::error::operation_aborted) {
            std::cerr << "[Network] Read header error: " << ec.message()
                      << "\n";
            disconnect();
          }
        }
      }));
}

void NetworkManager::readPayload(uint16_t payloadSize) {
  incoming_payload_.resize(payloadSize);

  asio::async_read(
      *socket_, asio::buffer(incoming_payload_.data(), payloadSize),
      asio::bind_executor(strand_, [this](std::error_code ec,
                                          std::size_t /*length*/) {
        if (!ec) {
          // Packet complete: Push to the Logic Thread queue
          Rebel::InboundPacket pkg;
          pkg.header = incoming_header_;
          pkg.payload = incoming_payload_;

          inbound_queue_.push(std::move(pkg));

          startRead(); // Wait for next packet
        } else {
          std::cerr << "[Network] Read payload error: " << ec.message() << "\n";
          disconnect();
        }
      }));
}
```

## File: ./client/src/VulkanRenderer.cpp
```cpp
#include "VulkanRenderer.hpp"
#include "GraphicsConfig.hpp"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <set> // Added for std::set
#include <stdexcept>
#include <string> // Added for std::string
#include <vector>

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
  createRenderPass();
  createGraphicsPipeline();
  createFramebuffers();
  createCommandPool();
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

  // Specifying the device features we'll need (can leave empty for now)
  VkPhysicalDeviceFeatures deviceFeatures{};

  // Packing it all into the main creation struct
  VkDeviceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

  createInfo.queueCreateInfoCount =
      static_cast<uint32_t>(queueCreateInfos.size());
  createInfo.pQueueCreateInfos = queueCreateInfos.data();

  createInfo.pEnabledFeatures = &deviceFeatures;

  // Enable the swapchain extension you defined in your header
  createInfo.enabledExtensionCount =
      static_cast<uint32_t>(deviceExtensions.size());
  createInfo.ppEnabledExtensionNames = deviceExtensions.data();

  // No validation layers explicitly enabled here for the device,
  // modern Vulkan handles validation at the instance level anyway.
  createInfo.enabledLayerCount = 0;

  // 1. The moment of truth: actually creating the device!
  if (vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_) !=
      VK_SUCCESS) {
    throw std::runtime_error("[Vulkan] Failed to create logical device!");
  }

  // 2. Retrieve the queue handles so we can actually submit command buffers
  // later
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
  VkAttachmentDescription colorAttachment{};
  colorAttachment.format = swapChainImageFormat_;
  colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
  colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  VkAttachmentReference colorAttachmentRef{};
  colorAttachmentRef.attachment = 0;
  colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &colorAttachmentRef;

  VkSubpassDependency dependency{};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.srcAccessMask = 0;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

  VkRenderPassCreateInfo renderPassInfo{};
  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  renderPassInfo.attachmentCount = 1;
  renderPassInfo.pAttachments = &colorAttachment;
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
readFile(const std::string &filename) { // Changed return type to uint32_t
  // Ensure your working directory has access to this relative path!
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

  std::vector<uint32_t> buffer(fileSize /
                               sizeof(uint32_t)); // Corrected size calculation

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

  VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
  vertexInputInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

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
  rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
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

  // Define the push constant range telling Vulkan what the vertex shader
  // expects
  VkPushConstantRange pushConstantRange{};
  pushConstantRange.stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT; // Targeted shader stage
  pushConstantRange.offset = 0;   // Zero offset
  pushConstantRange.size =
      sizeof(float) * 3; // x, y, and an isLocal flag (0.0/1.0)

  VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.setLayoutCount = 0;
  pipelineLayoutInfo.pSetLayouts = nullptr;
  pipelineLayoutInfo.pushConstantRangeCount = 1; // Set this to 1
  pipelineLayoutInfo.pPushConstantRanges =
      &pushConstantRange; // Bind the range data

  if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr,
                             &pipelineLayout_) != VK_SUCCESS) {
    throw std::runtime_error("[VULKAN] Failed to create pipeline layout!");
  }

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
    VkImageView attachments[] = {swapChainImageViews_[i]};

    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = renderPass_;
    framebufferInfo.attachmentCount = 1;
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

  VkClearValue clearColor = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
  renderPassInfo.clearValueCount = 1;
  renderPassInfo.pClearValues = &clearColor;

  vkCmdBeginRenderPass(commandBuffer, &renderPassInfo,
                       VK_SUBPASS_CONTENTS_INLINE);

  // 1. Bind your pipeline
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    graphicsPipeline_);

  // 2. Set your dynamic states
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

  // =================================================================
  // 3. One draw call per known player (local + every tracked remote)
  // =================================================================
  for (const auto& player : players) {
    float pushConstants[3] = {player.x, player.y, player.isLocal ? 1.0f : 0.0f};

    vkCmdPushConstants(
        commandBuffer,
        pipelineLayout_,            // Your compiled pipeline layout
        VK_SHADER_STAGE_VERTEX_BIT, // Target the vertex shader stage
        0,                          // Offset
        sizeof(pushConstants),      // x, y, isLocal
        pushConstants
    );

    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
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

  // Fix: Multiply by 4 (sizeof(uint32_t)) to get the size in bytes!
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

void VulkanRenderer::handleKeyInput(int key, int scancode, int action,
                                    int mods) {
  if (action == GLFW_PRESS) {
    if (key == GLFW_KEY_LEFT || key == GLFW_KEY_A)
      is_moving_left_ = true;
    if (key == GLFW_KEY_RIGHT || key == GLFW_KEY_D)
      is_moving_right_ = true;
    if (key == GLFW_KEY_UP || key == GLFW_KEY_W)
      is_moving_up_ = true; // <-- ADD THIS
    if (key == GLFW_KEY_DOWN || key == GLFW_KEY_S)
      is_moving_down_ = true; // <-- ADD THIS

    if (key == GLFW_KEY_F11) {
      toggleFullscreen(window_);
    }
  } else if (action == GLFW_RELEASE) {
    if (key == GLFW_KEY_LEFT || key == GLFW_KEY_A)
      is_moving_left_ = false;
    if (key == GLFW_KEY_RIGHT || key == GLFW_KEY_D)
      is_moving_right_ = false;
    if (key == GLFW_KEY_UP || key == GLFW_KEY_W)
      is_moving_up_ = false; // <-- ADD THIS
    if (key == GLFW_KEY_DOWN || key == GLFW_KEY_S)
      is_moving_down_ = false; // <-- ADD THIS
  }
}
```

## File: ./common/CMakeLists.txt
```cmake
add_library(common INTERFACE)

# This tells other parts of the project where the .hpp files are
target_include_directories(common INTERFACE include)```

## File: ./common/include/ThreadUtility.hpp
```cpp
#pragma once
#include <vector>
#include <thread>
#include <set>
#include <queue>
#include <mutex>
#include <optional>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <pthread.h>
    #include <sched.h>
    #include <fstream>
    #include <string>
#endif

namespace Rebel::Concurrent {

    template<typename T>
    class ThreadSafeQueue {
    public:
        void push(T item) {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(item));
        }

        std::optional<T> try_pop() {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) {
                return std::nullopt;
            }
            T item = std::move(queue_.front());
            queue_.pop();
            return item;
        }

        bool empty() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return queue_.empty();
        }

    private:
        std::queue<T> queue_;
        mutable std::mutex mutex_;
    };

}

namespace Rebel::ThreadUtils {

    inline std::vector<uint32_t> GetPhysicalCoreMap() {
        std::vector<uint32_t> coreIds;
#ifdef _WIN32
        DWORD length = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
        if (length == 0) return coreIds;

        std::vector<uint8_t> buffer(length);
        if (GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer.data(), &length)) {
            auto* ptr = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer.data();
            for (DWORD i = 0; i < length; ) {
                for (int bit = 0; bit < 64; ++bit) {
                    if ((ptr->Processor.GroupMask[0].Mask >> bit) & 1) {
                        coreIds.push_back(bit);
                        break; 
                    }
                }
                i += ptr->Size;
                ptr = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)((uint8_t*)ptr + ptr->Size);
            }
        }
#else
        std::set<int> seen;
        for (uint32_t i = 0; i < std::thread::hardware_concurrency(); ++i) {
            std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/topology/core_id";
            std::ifstream f(path);
            int physId;
            if (f >> physId && seen.find(physId) == seen.end()) {
                coreIds.push_back(i);
                seen.insert(physId);
            }
        }
#endif
        return coreIds;
    }

    inline void SetThreadAffinity(std::thread& t, uint32_t logicalId) {
#ifdef _WIN32
        SetThreadAffinityMask((HANDLE)t.native_handle(), (DWORD_PTR)1 << logicalId);
#else
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(logicalId, &cpuset);
        pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
#endif
    }
}```

## File: ./common/include/Version.hpp
```cpp
#pragma once // This prevents the file from being loaded twice

namespace Rebel {
    // Constant for our game version
    inline constexpr int VERSION_MAJOR = 0;
    inline constexpr int VERSION_MINOR = 1;
}```

## File: ./common/include/Protocol.hpp
```cpp
#pragma once
#include <cstdint>

namespace Rebel {

enum class Opcode : uint16_t {
  CMSG_PING = 0x0001,
  SMSG_PONG = 0x0002,
  CMSG_AUTH_SESSION = 0x0003,
  SMSG_AUTH_RESPONSE = 0x0004,
  CMSG_GAME_AUTH = 0x0005, // client -> game server, replaces resending MsgLogin
  SMSG_GAME_AUTH_OK = 0x0006,
  SMSG_GAME_AUTH_FAIL = 0x0007,

  CMSG_CHAT_SAY = 0x0100,
  SMSG_CHAT_SAY = 0x0101,

  CMSG_PLAYER_MOVE = 0x0200,  // client -> server: movement intent, not a position
  SMSG_PLAYER_STATE = 0x0201, // server -> all clients: authoritative position for one player
  SMSG_PLAYER_LEAVE = 0x0202  // server -> all clients: a player's entity was removed
};

namespace MoveFlags {
constexpr uint8_t Up = 1 << 0;
constexpr uint8_t Down = 1 << 1;
constexpr uint8_t Left = 1 << 2;
constexpr uint8_t Right = 1 << 3;
} // namespace MoveFlags

#pragma pack(push, 1)
struct PacketHeader {
  uint16_t size;   // Total size
  uint16_t opcode; // Changed to uint16_t to match enum
};

struct MsgLogin {
  char username[32];
  char password[32];
  uint32_t version;
};

struct MsgRedirect {
  char ip[16];
  uint16_t port;
  char session_token[32];
};
struct MsgPlayerMove {
  uint8_t moveFlags; // Bitmask of MoveFlags — the client's current input, not a position
  float yaw;         // Essential for knowing which way the dwarf is looking!
};

struct MsgGameAuth {
  char session_token[32]; // Matches MsgRedirect::session_token / sessions.token
};

struct MsgGameAuthOk {
  uint32_t character_id; // Tells the client which SMSG_PLAYER_STATE updates are "self"
};

struct MsgPlayerState {
  uint32_t character_id;
  float x, y, z;
  float yaw;
};

struct MsgPlayerLeave {
  uint32_t character_id;
};
#pragma pack(pop)

} // namespace Rebel
```

## File: ./common/include/Movement.hpp
```cpp
#pragma once
#include "Protocol.hpp"

namespace Rebel {

struct Vec2 {
  float x = 0.0f;
  float y = 0.0f;
};

// Shared by the client (local prediction) and GameServer (authoritative
// simulation) so both compute movement at the exact same rate. If they ever
// drift apart, the client will see constant correction snapping even under
// perfect network conditions.
constexpr float PLAYER_SPEED_PER_SEC = 1.28f; // Matches the old client-side feel (0.02 units/tick @ 64Hz)

// Pure movement-simulation step: given a position, the client's latest input
// flags, and a per-tick distance, returns the advanced position. No
// entt/asio/pqxx dependency, so it's safe to unit test in isolation, and to
// call from both GameServer's simulate_movement() and the client's local
// prediction step.
inline Vec2 apply_move_flags(Vec2 pos, uint8_t moveFlags, float delta) {
  // Screen-space Y grows downward, matching the client-side convention.
  if (moveFlags & MoveFlags::Left) pos.x -= delta;
  if (moveFlags & MoveFlags::Right) pos.x += delta;
  if (moveFlags & MoveFlags::Up) pos.y -= delta;
  if (moveFlags & MoveFlags::Down) pos.y += delta;
  return pos;
}

} // namespace Rebel
```

## File: ./server/CMakeLists.txt
```cmake
add_subdirectory(login)
add_subdirectory(game)```

## File: ./server/game/CMakeLists.txt
```cmake
# server/game/CMakeLists.txt

# 1. Find the ASIO and libpqxx packages (vcpkg provides these)
find_package(asio CONFIG REQUIRED)
find_package(libpqxx CONFIG REQUIRED)

set(GAME_SERVER_SOURCES
    src/main.cpp
)

add_executable(GameServer ${GAME_SERVER_SOURCES})

target_include_directories(GameServer PRIVATE include)

# 2. Link asio, libpqxx, AND your common interface
# asio::asio is the standard target name for the header-only version
target_link_libraries(GameServer
    PRIVATE
        common
        asio::asio
        libpqxx::pqxx
)

target_compile_features(GameServer PRIVATE cxx_std_20)```

## File: ./server/game/src/main.cpp
```cpp
#include <iostream>
#include <asio.hpp>
#include <chrono>
#include <vector>
#include <thread>
#include <algorithm>
#include <atomic>
#include <memory>
#include <cstdlib>
#include "Protocol.hpp"
#include "Version.hpp"
#include "Movement.hpp"
#include <entt/entt.hpp>
#include <pqxx/pqxx>



entt::registry world;
std::unique_ptr<pqxx::connection> g_db;

// Define some basic components
struct Position { float x, y, z, yaw = 0.0f; };
struct PlayerData { std::string name; int character_id = -1; };
struct PlayerInput { uint8_t moveFlags = 0; float yaw = 0.0f; }; // Latest intent from the client — not a position

class PlayerSession;
std::vector<std::weak_ptr<PlayerSession>> g_sessions; // Only touched from code posted to game_strand
void broadcast_player_states();
void broadcast_player_leave(int character_id);

// Persists a player's world position back to the characters table.
// Only ever called from code posted to game_strand, so g_db is never touched concurrently.
void save_character(int character_id, float x, float y, float z) {
    if (!g_db || character_id < 0) return;
    try {
        pqxx::work txn(*g_db);
        txn.exec_params(
            "UPDATE characters SET pos_x = $1, pos_y = $2, pos_z = $3, last_saved_at = now() WHERE id = $4",
            x, y, z, character_id);
        txn.commit();
    } catch (const std::exception &e) {
        std::cerr << "[WORLD] Failed to save character " << character_id << ": " << e.what() << std::endl;
    }
}

constexpr float TICK_DT_SEC = 0.05f;         // 20Hz
constexpr int AUTOSAVE_INTERVAL_TICKS = 600; // Every 30s at 20Hz

// Advances every player's Position from their latest PlayerInput. This is the
// server's one and only place that mutates Position from movement — the client
// only ever sends intent (CMSG_PLAYER_MOVE), never a position.
void simulate_movement() {
    const float delta = Rebel::PLAYER_SPEED_PER_SEC * TICK_DT_SEC;
    auto view = world.view<PlayerInput, Position>();
    view.each([delta](auto entity, auto &input, auto &pos) {
        Rebel::Vec2 moved = Rebel::apply_move_flags({pos.x, pos.y}, input.moveFlags, delta);
        pos.x = moved.x;
        pos.y = moved.y;
        pos.yaw = input.yaw; // Cosmetic only — trusted as-is, not simulated/validated
    });
}

// Function to handle each tick of the 20Hz game loop
void update_game_world(int current_tick) {
    simulate_movement();
    broadcast_player_states();

    bool shouldLog = (current_tick % 20 == 0); // Only log ~once/second to keep the terminal readable
    bool shouldAutosave = current_tick != 0 && (current_tick % AUTOSAVE_INTERVAL_TICKS == 0);
    if (!shouldLog && !shouldAutosave) return;

    auto view = world.view<PlayerData, Position>();

    if (shouldLog && view.size_hint() == 0) {
        std::cout << "[WORLD] Heartbeat - Tick: " << current_tick << std::endl;
    }

    view.each([current_tick, shouldLog, shouldAutosave](auto entity, auto &data, auto &pos) {
        if (shouldLog) {
            // VERIFICATION PRINT: This will print out the full 2D position!
            std::cout << "[WORLD] Tick: " << current_tick
                      << " | Player: " << data.name
                      << " | X: " << pos.x
                      << " | Y: " << pos.y << std::endl; // <-- Verified!
        }
        if (shouldAutosave) {
            save_character(data.character_id, pos.x, pos.y, pos.z);
        }
    });
}

// The clean Asio tick orchestrator
void game_loop_tick(asio::steady_timer& timer, 
                    asio::strand<asio::io_context::executor_type>& strand, 
                    std::atomic<int>& tick_count, 
                    std::atomic<bool>& running) {
    if (!running.load()) return;

    // 1. Execute the isolated game logic
    int current_tick = tick_count++;
    update_game_world(current_tick);

    // 2. Schedule the next tick precisely 50ms into the future (20Hz)
    timer.expires_at(timer.expiry() + std::chrono::milliseconds(50));
    
    // Using a simple lambda wrapper to forward the next call smoothly
    timer.async_wait(asio::bind_executor(strand, 
        [&timer, &strand, &tick_count, &running](const asio::error_code& error) {
            if (!error) {
                game_loop_tick(timer, strand, tick_count, running);
            }
        }));
}

class PlayerSession : public std::enable_shared_from_this<PlayerSession> {
public:

enum class SessionState {
        WAITING_FOR_AUTH,
        AUTHENTICATED,
        DISCONNECTED
    };
    PlayerSession(asio::ip::tcp::socket socket, asio::strand<asio::io_context::executor_type>& strand) 
        : socket_(std::move(socket)), 
          strand_(strand), 
          state_(SessionState::WAITING_FOR_AUTH) {} // Initialize at the gate!

    void start() {
        try {
            std::cout << "[SESSION] Player connected from: " << socket_.remote_endpoint() << std::endl;
            read_header();
        } catch (const std::exception& e) {
            std::cerr << "[SESSION] Error starting session: " << e.what() << std::endl;
        }
    }

    ~PlayerSession() {
        if (has_entity_) {
            // We MUST post this to the strand because EnTT's registry
            // is not thread-safe by default. This ensures the destruction
            // happens in between game loop ticks.
            auto id = entity_id_;
            asio::post(strand_, [id]() {
                if (world.valid(id)) {
                    auto &data = world.get<PlayerData>(id);
                    auto &pos = world.get<Position>(id);
                    save_character(data.character_id, pos.x, pos.y, pos.z);
                    int character_id = data.character_id;
                    world.destroy(id);
                    std::cout << "[WORLD] Entity destroyed (Player disconnected)." << std::endl;
                    broadcast_player_leave(character_id);
                }
            });
        }
    }

    void send_player_state(uint32_t character_id, float x, float y, float z, float yaw) {
        auto packet = std::make_shared<std::vector<uint8_t>>(
            sizeof(Rebel::PacketHeader) + sizeof(Rebel::MsgPlayerState));
        auto *h = reinterpret_cast<Rebel::PacketHeader *>(packet->data());
        h->size = packet->size();
        h->opcode = static_cast<uint16_t>(Rebel::Opcode::SMSG_PLAYER_STATE);
        auto *m = reinterpret_cast<Rebel::MsgPlayerState *>(
            packet->data() + sizeof(Rebel::PacketHeader));
        m->character_id = character_id;
        m->x = x;
        m->y = y;
        m->z = z;
        m->yaw = yaw;

        auto self(shared_from_this());
        asio::async_write(socket_, asio::buffer(*packet),
            [self, packet](const asio::error_code &, std::size_t) {});
    }

    void send_player_leave(uint32_t character_id) {
        auto packet = std::make_shared<std::vector<uint8_t>>(
            sizeof(Rebel::PacketHeader) + sizeof(Rebel::MsgPlayerLeave));
        auto *h = reinterpret_cast<Rebel::PacketHeader *>(packet->data());
        h->size = packet->size();
        h->opcode = static_cast<uint16_t>(Rebel::Opcode::SMSG_PLAYER_LEAVE);
        auto *m = reinterpret_cast<Rebel::MsgPlayerLeave *>(
            packet->data() + sizeof(Rebel::PacketHeader));
        m->character_id = character_id;

        auto self(shared_from_this());
        asio::async_write(socket_, asio::buffer(*packet),
            [self, packet](const asio::error_code &, std::size_t) {});
    }

private:

entt::entity entity_id_{ entt::null };
    bool has_entity_{ false };

    void read_header() {
        auto self(shared_from_this()); 
        asio::async_read(socket_, asio::buffer(&header_, sizeof(Rebel::PacketHeader)),
            [this, self](const asio::error_code& ec, std::size_t) {
                if (!ec) {
                    uint16_t payload_size = header_.size - sizeof(Rebel::PacketHeader);
                    if (payload_size > 0) {
                        payload_.resize(payload_size);
                        read_payload();
                    } else {
                        on_packet_received();
                        
                    }
                }
            });
    }

    void read_payload() {
        auto self(shared_from_this());
        asio::async_read(socket_, asio::buffer(payload_.data(), payload_.size()),
            [this, self](const asio::error_code& ec, std::size_t) {
                if (!ec) {
                    on_packet_received();
                    
                }
            });
    }

    void on_packet_received() {
        // Ensure processing happens on the strand
        asio::post(strand_, [self = shared_from_this()]() {
            self->process_packet();
            
            // Start reading the next packet ONLY NOW that the buffer is safely processed
            self->read_header(); 
        });
    }

    void process_packet() {
        Rebel::Opcode opcode = static_cast<Rebel::Opcode>(header_.opcode);

        if (state_ == SessionState::WAITING_FOR_AUTH) {
            if (opcode == Rebel::Opcode::CMSG_GAME_AUTH) {
                handle_auth();
            } else {
                std::cout << "[SESSION] Unauthorized opcode " << (int)opcode << ". Closing." << std::endl;
                socket_.close();
            }
            return;
        }

        switch (opcode) {
            case Rebel::Opcode::CMSG_PING:
                send_pong();
                break;

            case Rebel::Opcode::CMSG_PLAYER_MOVE: {
                // 1. Check if the payload matches our movement struct
                if (payload_.size() < sizeof(Rebel::MsgPlayerMove)) {
                    std::cerr << "[SESSION] Malformed move packet size." << std::endl;
                    break;
                }

                // 2. Map the raw bytes to our struct
                auto* move = reinterpret_cast<Rebel::MsgPlayerMove*>(payload_.data());

                // 3. Just record intent — simulate_movement() applies it once per
                // tick, so we're never mutating Position off the tick's own step.
                // No mutex needed because we are on the game_strand!
                if (has_entity_ && world.valid(entity_id_)) {
                    auto& input = world.get<PlayerInput>(entity_id_);
                    input.moveFlags = move->moveFlags;
                    input.yaw = move->yaw;
                }
                break;
            }
            default:
                break;
        }
    }

    void handle_auth() {
    if (payload_.size() < sizeof(Rebel::MsgGameAuth)) {
        std::cerr << "[SESSION] Game auth packet too small." << std::endl;
        socket_.close();
        return;
    }

    auto* msg = reinterpret_cast<Rebel::MsgGameAuth*>(payload_.data());
    std::string token(msg->session_token, sizeof(msg->session_token));

    std::string username;
    int character_id = -1;
    float px = 0.0f, py = 0.0f, pz = 0.0f;

    try {
        pqxx::work txn(*g_db);

        // The token proves the LoginServer already authenticated this player —
        // we no longer trust a resent username/password here.
        auto sess = txn.exec_params(
            "SELECT sessions.account_id, accounts.username FROM sessions "
            "JOIN accounts ON accounts.id = sessions.account_id "
            "WHERE sessions.token = $1 AND sessions.expires_at > now()",
            token);
        if (sess.empty()) {
            std::cerr << "[SESSION] Invalid or expired session token." << std::endl;
            send_auth_fail();
            return;
        }
        int account_id = sess[0]["account_id"].as<int>();
        username = sess[0]["username"].as<std::string>();

        // Single-use: consume the token now that it's been validated.
        txn.exec_params("DELETE FROM sessions WHERE token = $1", token);

        std::cout << "[SESSION] Player '" << username << "' authenticated via session token." << std::endl;

        // One character per account for now — there's no character-select screen yet.
        auto chars = txn.exec_params(
            "SELECT id, pos_x, pos_y, pos_z FROM characters WHERE account_id = $1 ORDER BY id LIMIT 1",
            account_id);

        if (chars.empty()) {
            auto created = txn.exec_params(
                "INSERT INTO characters (account_id, name) VALUES ($1, $2) "
                "RETURNING id, pos_x, pos_y, pos_z",
                account_id, username);
            character_id = created[0]["id"].as<int>();
            px = created[0]["pos_x"].as<float>();
            py = created[0]["pos_y"].as<float>();
            pz = created[0]["pos_z"].as<float>();
        } else {
            character_id = chars[0]["id"].as<int>();
            px = chars[0]["pos_x"].as<float>();
            py = chars[0]["pos_y"].as<float>();
            pz = chars[0]["pos_z"].as<float>();
        }

        txn.commit();
    } catch (const std::exception &e) {
        std::cerr << "[SESSION] DB error validating session for token: " << e.what() << std::endl;
        send_auth_fail();
        return;
    }

    state_ = SessionState::AUTHENTICATED;

    entity_id_ = world.create();
    world.emplace<PlayerData>(entity_id_, username, character_id);
    world.emplace<Position>(entity_id_, px, py, pz);
    world.emplace<PlayerInput>(entity_id_);
    has_entity_ = true;
    g_sessions.push_back(weak_from_this());

    std::cout << "[WORLD] Loaded character " << character_id << " for " << username
              << " at (" << px << ", " << py << ", " << pz << ")" << std::endl;

    auto full_packet = std::make_shared<std::vector<uint8_t>>(
        sizeof(Rebel::PacketHeader) + sizeof(Rebel::MsgGameAuthOk));
    auto *h = reinterpret_cast<Rebel::PacketHeader *>(full_packet->data());
    h->size = full_packet->size();
    h->opcode = static_cast<uint16_t>(Rebel::Opcode::SMSG_GAME_AUTH_OK);
    auto *m = reinterpret_cast<Rebel::MsgGameAuthOk *>(
        full_packet->data() + sizeof(Rebel::PacketHeader));
    m->character_id = static_cast<uint32_t>(character_id);

    auto self(shared_from_this());
    asio::async_write(socket_, asio::buffer(*full_packet),
        [this, self, full_packet](const asio::error_code& ec, std::size_t) {
            if (ec) {
                socket_.close();
            }
        });
}

    void send_auth_fail() {
        auto response = std::make_shared<Rebel::PacketHeader>();
        response->size = sizeof(Rebel::PacketHeader);
        response->opcode = static_cast<uint16_t>(Rebel::Opcode::SMSG_GAME_AUTH_FAIL);

        auto self(shared_from_this());
        asio::async_write(socket_, asio::buffer(response.get(), sizeof(Rebel::PacketHeader)),
            [this, self, response](const asio::error_code&, std::size_t) {
                socket_.close();
            });
    }

    void send_pong() {
        // ... (your existing send_pong logic)
    }

    asio::ip::tcp::socket socket_;
    asio::strand<asio::io_context::executor_type>& strand_;
    SessionState state_; // Tracking the state
    Rebel::PacketHeader header_;
    std::vector<uint8_t> payload_;
};

// Sends every player's current position to every connected session (including
// their own — the client filters by character_id from SMSG_GAME_AUTH_OK).
// Prunes sessions whose PlayerSession has already been destroyed.
void broadcast_player_states() {
    auto view = world.view<PlayerData, Position>();
    for (auto entity : view) {
        auto &data = view.get<PlayerData>(entity);
        auto &pos = view.get<Position>(entity);
        uint32_t character_id = static_cast<uint32_t>(data.character_id);

        for (auto it = g_sessions.begin(); it != g_sessions.end();) {
            if (auto session = it->lock()) {
                session->send_player_state(character_id, pos.x, pos.y, pos.z, pos.yaw);
                ++it;
            } else {
                it = g_sessions.erase(it);
            }
        }
    }
}

void broadcast_player_leave(int character_id) {
    uint32_t id = static_cast<uint32_t>(character_id);
    for (auto it = g_sessions.begin(); it != g_sessions.end();) {
        if (auto session = it->lock()) {
            session->send_player_leave(id);
            ++it;
        } else {
            it = g_sessions.erase(it);
        }
    }
}

void start_accept(asio::ip::tcp::acceptor& acceptor, asio::io_context& io_context, asio::strand<asio::io_context::executor_type>& strand) {
    acceptor.async_accept([&acceptor, &io_context, &strand](const asio::error_code& error, asio::ip::tcp::socket socket) {
        if (!error) {
            // --- THE LOUD PRINT ---
            std::cout << "[GAME] Incoming connection from: " << socket.remote_endpoint() << std::endl;
            
            std::make_shared<PlayerSession>(std::move(socket), strand)->start();
        }
        start_accept(acceptor, io_context, strand);
    });
}

int main() {
    std::cout << "--- REBEL SERVER STARTING ---" << std::endl;

    const char *conn_str = std::getenv("REBEL_DB_CONNSTR");
    std::string connection_string =
        conn_str ? conn_str
                 : "dbname=rebelmmo user=rebel password=rebel host=127.0.0.1";
    try {
        g_db = std::make_unique<pqxx::connection>(connection_string);
        if (!g_db->is_open()) {
            std::cerr << "[GAME] Failed to open database connection." << std::endl;
            return 1;
        }
        std::cout << "[GAME] Connected to database: " << g_db->dbname() << std::endl;
    } catch (const std::exception &e) {
        std::cerr << "[GAME] DB connection error: " << e.what() << std::endl;
        return 1;
    }

    asio::io_context io_context;
    auto work_guard = asio::make_work_guard(io_context);
    auto game_strand = asio::make_strand(io_context);

    asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), 12345);
    asio::ip::tcp::acceptor acceptor(io_context, endpoint);

    start_accept(acceptor, io_context, game_strand);

    const unsigned int num_threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> workers;
    for (unsigned int i = 0; i < num_threads; ++i) {
        workers.emplace_back([&io_context]() { io_context.run(); });
    }

    std::atomic<bool> running(true);
    asio::steady_timer timer(io_context);
    std::atomic<int> tick_count(0);

    timer.expires_at(std::chrono::steady_clock::now() + std::chrono::milliseconds(50));
    game_loop_tick(timer, game_strand, tick_count, running);

    std::cout << "Press Enter to stop..." << std::endl;
    std::cin.get();

    running.store(false);
    io_context.stop();
    for (auto& t : workers) t.join();

    return 0;
}```

## File: ./server/login/src/main.cpp
```cpp
#include "Protocol.hpp"
#include "Version.hpp"
#include <asio.hpp>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <pqxx/pqxx>
#include <random>
#include <sstream>
#include <vector>

// TODO: replace with real Argon2 verification (argon2::verify).
// Placeholder just to give the auth path something to actually run.
bool verify_password_hash(const std::string &password,
                          const std::string &stored_hash) {
  return password == stored_hash;
}

// 32 hex characters == 16 random bytes, matching sessions.token CHAR(32)
// and MsgRedirect::session_token exactly (no null terminator needed).
std::string generate_session_token() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dist;
  std::ostringstream oss;
  oss << std::hex << std::setfill('0') << std::setw(16) << dist(gen)
      << std::setw(16) << dist(gen);
  return oss.str();
}

class LoginSession : public std::enable_shared_from_this<LoginSession> {
public:
  LoginSession(asio::ip::tcp::socket socket, pqxx::connection &db)
      : socket_(std::move(socket)), db_(db) {}

  void start() {
    asio::error_code ec;
    auto endpoint = socket_.remote_endpoint(ec);
    if (!ec) {
      std::cout << "[LOGIN] New auth attempt from: " << endpoint << std::endl;
    }
    read_header();
  }

private:
  void read_header() {
    auto self(shared_from_this());
    asio::async_read(
        socket_, asio::buffer(&header_, sizeof(Rebel::PacketHeader)),
        [this, self](const asio::error_code &ec, std::size_t) {
          if (!ec) {
            read_payload();
          } else {
            std::cout << "[LOGIN] Client disconnected." << std::endl;
          }
        });
  }

  void read_payload() {
    auto self(shared_from_this());
    std::size_t payload_size = header_.size - sizeof(Rebel::PacketHeader);
    payload_.resize(payload_size);

    asio::async_read(socket_, asio::buffer(payload_.data(), payload_size),
                     [this, self](const asio::error_code &ec, std::size_t) {
                       if (!ec) {
                         process_login_request();
                       } else {
                         std::cout
                             << "[LOGIN] Payload read failed. Disconnecting."
                             << std::endl;
                       }
                     });
  }

  void process_login_request() {
    Rebel::Opcode opcode = static_cast<Rebel::Opcode>(header_.opcode);

    if (opcode != Rebel::Opcode::CMSG_AUTH_SESSION) {
      std::cerr << "[LOGIN] Unexpected opcode: 0x" << std::hex << (int)opcode
                << std::dec << std::endl;
      return;
    }

    if (payload_.size() < sizeof(Rebel::MsgLogin)) {
      std::cerr << "[LOGIN] Auth packet too small." << std::endl;
      socket_.close();
      return;
    }

    auto *msg = reinterpret_cast<Rebel::MsgLogin *>(payload_.data());
    std::cout << "[LOGIN] Received login request for '" << msg->username
              << "'. Authenticating..." << std::endl;

    if (!verify_credentials(msg->username, msg->password)) {
      std::cerr << "[LOGIN] Auth failed for '" << msg->username << "'."
                << std::endl;
      socket_.close();
      return;
    }

    std::cout << "[LOGIN] '" << msg->username << "' authenticated successfully."
              << std::endl;

    if (!issue_session_token()) {
      std::cerr << "[LOGIN] Failed to create session for '" << msg->username
                << "'." << std::endl;
      socket_.close();
      return;
    }

    send_login_response();
  }

  bool verify_credentials(const std::string &username,
                          const std::string &password) {
    try {
      pqxx::work txn(db_);
      auto result = txn.exec_params(
          "SELECT id, password_hash FROM accounts WHERE username = $1",
          username);

      if (result.empty()) {
        return false;
      }

      std::string stored_hash = result[0]["password_hash"].as<std::string>();
      if (!verify_password_hash(password, stored_hash)) {
        return false;
      }

      account_id_ = result[0]["id"].as<int>();
      return true;
    } catch (const std::exception &e) {
      std::cerr << "[LOGIN] DB error: " << e.what() << std::endl;
      return false;
    }
  }

  // Generates a short-lived, single-use token GameServer will trade for a
  // world entry instead of trusting a resent username/password.
  bool issue_session_token() {
    try {
      session_token_ = generate_session_token();
      pqxx::work txn(db_);
      txn.exec_params(
          "INSERT INTO sessions (token, account_id, expires_at) "
          "VALUES ($1, $2, now() + interval '5 minutes')",
          session_token_, account_id_);
      txn.commit();
      return true;
    } catch (const std::exception &e) {
      std::cerr << "[LOGIN] DB error creating session: " << e.what()
                << std::endl;
      return false;
    }
  }

  void send_login_response() {
    auto self(shared_from_this());
    auto full_packet = std::make_shared<std::vector<uint8_t>>();
    full_packet->resize(sizeof(Rebel::PacketHeader) +
                        sizeof(Rebel::MsgRedirect));

    auto *h = reinterpret_cast<Rebel::PacketHeader *>(full_packet->data());
    h->size = full_packet->size();
    h->opcode = static_cast<uint16_t>(Rebel::Opcode::SMSG_AUTH_RESPONSE);

    auto *r = reinterpret_cast<Rebel::MsgRedirect *>(
        full_packet->data() + sizeof(Rebel::PacketHeader));
    std::memset(r->ip, 0, 16);
    std::strncpy(r->ip, "127.0.0.1", 15);
    r->port = 12345;
    std::memcpy(r->session_token, session_token_.data(),
                sizeof(r->session_token));

    asio::async_write(
        socket_, asio::buffer(full_packet->data(), full_packet->size()),
        [this, self, full_packet](const asio::error_code &ec, std::size_t) {
          if (!ec) {
            std::cout << "[LOGIN] Redirect sent. Closing connection."
                      << std::endl;
            socket_.shutdown(asio::ip::tcp::socket::shutdown_both);
            socket_.close();
          }
        });
  }

  asio::ip::tcp::socket socket_;
  pqxx::connection &db_;
  Rebel::PacketHeader header_;
  std::vector<uint8_t> payload_;
  int account_id_ = -1;
  std::string session_token_;
};

void start_accept(asio::ip::tcp::acceptor &acceptor,
                  asio::io_context &io_context, pqxx::connection &db) {
  acceptor.async_accept(
      [&acceptor, &io_context, &db](const asio::error_code &error,
                                    asio::ip::tcp::socket socket) {
        if (!error) {
          std::make_shared<LoginSession>(std::move(socket), db)->start();
        }
        start_accept(acceptor, io_context, db);
      });
}

int main() {
  std::cout << "--- REBEL LOGIN SERVER STARTING ---" << std::endl;

  try {
    // TODO: pull from config/env instead of hardcoding once there's a
    // config layer. For now this just needs to point at a local dev DB.
    const char *conn_str = std::getenv("REBEL_DB_CONNSTR");
    std::string connection_string =
        conn_str ? conn_str
                 : "dbname=rebelmmo user=rebel password=rebel host=127.0.0.1";

    pqxx::connection db(connection_string);
    if (!db.is_open()) {
      std::cerr << "[LOGIN] Failed to open database connection." << std::endl;
      return 1;
    }
    std::cout << "[LOGIN] Connected to database: " << db.dbname() << std::endl;

    asio::io_context io_context;

    // Changed to 54321 to match your client's initial connection code
    asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), 54321);
    asio::ip::tcp::acceptor acceptor(io_context, endpoint);

    std::cout << "Login Server listening on port 54321..." << std::endl;
    start_accept(acceptor, io_context, db);

    io_context.run();
  } catch (const std::exception &e) {
    std::cerr << "[LOGIN] Exception: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
```

## File: ./server/login/schema/001_init.sql
```sql
-- 001_init.sql
-- Minimal schema for login server auth. Framing only — no migrations tooling yet.

CREATE TABLE IF NOT EXISTS accounts (
    id            SERIAL PRIMARY KEY,
    username      VARCHAR(32) UNIQUE NOT NULL,
    password_hash TEXT NOT NULL,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_login_at TIMESTAMPTZ
);

-- Convenience seed row for local testing.
-- password_hash is a plaintext placeholder until Argon2 is wired in.
INSERT INTO accounts (username, password_hash)
VALUES ('Karadiinar', 'changeme')
ON CONFLICT (username) DO NOTHING;
```

## File: ./server/login/schema/002_sessions_and_characters.sql
```sql
-- 002_sessions_and_characters.sql

CREATE TABLE IF NOT EXISTS sessions (
    token         CHAR(32) PRIMARY KEY,       -- hex-encoded random bytes
    account_id    INT NOT NULL REFERENCES accounts(id),
    issued_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at    TIMESTAMPTZ NOT NULL
);

CREATE TABLE IF NOT EXISTS characters (
    id            SERIAL PRIMARY KEY,
    account_id    INT NOT NULL REFERENCES accounts(id),
    name          VARCHAR(32) UNIQUE NOT NULL,
    pos_x         REAL NOT NULL DEFAULT 0,
    pos_y         REAL NOT NULL DEFAULT 0,
    pos_z         REAL NOT NULL DEFAULT 0,
    yaw           REAL NOT NULL DEFAULT 0,
    inventory     JSONB NOT NULL DEFAULT '[]',
    last_saved_at TIMESTAMPTZ
);
```

## File: ./server/login/CMakeLists.txt
```cmake
find_package(asio CONFIG REQUIRED)
find_package(libpqxx CONFIG REQUIRED)

add_executable(LoginServer src/main.cpp)

target_include_directories(LoginServer PRIVATE include)

# Link the common headers and asio
target_link_libraries(LoginServer PRIVATE common asio::asio libpqxx::pqxx)
target_compile_features(LoginServer PRIVATE cxx_std_20)
```

## File: ./tests/test_protocol.cpp
```cpp
#include <catch2/catch_test_macros.hpp>
#include "Protocol.hpp"

// These sizes ARE the wire contract between client, LoginServer, and
// GameServer. If one of these changes unintentionally, something just broke
// compatibility between the three binaries.
TEST_CASE("Protocol struct sizes match the wire contract", "[protocol]") {
  CHECK(sizeof(Rebel::PacketHeader) == 4);
  CHECK(sizeof(Rebel::MsgLogin) == 68);
  CHECK(sizeof(Rebel::MsgRedirect) == 50);
  CHECK(sizeof(Rebel::MsgPlayerMove) == 5);
  CHECK(sizeof(Rebel::MsgGameAuth) == 32);
  CHECK(sizeof(Rebel::MsgGameAuthOk) == 4);
  CHECK(sizeof(Rebel::MsgPlayerState) == 20);
  CHECK(sizeof(Rebel::MsgPlayerLeave) == 4);
}

TEST_CASE("MoveFlags bits don't collide", "[protocol]") {
  CHECK((Rebel::MoveFlags::Up & Rebel::MoveFlags::Down) == 0);
  CHECK((Rebel::MoveFlags::Left & Rebel::MoveFlags::Right) == 0);
  CHECK((Rebel::MoveFlags::Up & Rebel::MoveFlags::Left) == 0);
  CHECK((Rebel::MoveFlags::Up & Rebel::MoveFlags::Right) == 0);
  CHECK((Rebel::MoveFlags::Down & Rebel::MoveFlags::Left) == 0);
  CHECK((Rebel::MoveFlags::Down & Rebel::MoveFlags::Right) == 0);
}
```

## File: ./tests/test_movement.cpp
```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "Movement.hpp"

using Catch::Approx;

TEST_CASE("No input flags means no movement", "[movement]") {
  Rebel::Vec2 pos{1.0f, 2.0f};
  auto result = Rebel::apply_move_flags(pos, 0, 0.064f);
  CHECK(result.x == Approx(1.0f));
  CHECK(result.y == Approx(2.0f));
}

TEST_CASE("Each direction moves the expected axis", "[movement]") {
  Rebel::Vec2 origin{0.0f, 0.0f};
  const float delta = 0.064f;

  auto right = Rebel::apply_move_flags(origin, Rebel::MoveFlags::Right, delta);
  CHECK(right.x == Approx(delta));
  CHECK(right.y == Approx(0.0f));

  auto left = Rebel::apply_move_flags(origin, Rebel::MoveFlags::Left, delta);
  CHECK(left.x == Approx(-delta));

  // Screen-space Y grows downward: Down increases y, Up decreases it.
  auto down = Rebel::apply_move_flags(origin, Rebel::MoveFlags::Down, delta);
  CHECK(down.y == Approx(delta));

  auto up = Rebel::apply_move_flags(origin, Rebel::MoveFlags::Up, delta);
  CHECK(up.y == Approx(-delta));
}

TEST_CASE("Opposing flags held together cancel out", "[movement]") {
  Rebel::Vec2 pos{5.0f, 5.0f};
  const float delta = 0.064f;

  auto horizontal = Rebel::apply_move_flags(
      pos, Rebel::MoveFlags::Left | Rebel::MoveFlags::Right, delta);
  CHECK(horizontal.x == Approx(5.0f));

  auto vertical = Rebel::apply_move_flags(
      pos, Rebel::MoveFlags::Up | Rebel::MoveFlags::Down, delta);
  CHECK(vertical.y == Approx(5.0f));
}

TEST_CASE("Diagonal input advances both axes independently", "[movement]") {
  Rebel::Vec2 origin{0.0f, 0.0f};
  const float delta = 0.064f;

  auto result = Rebel::apply_move_flags(
      origin, Rebel::MoveFlags::Right | Rebel::MoveFlags::Down, delta);
  CHECK(result.x == Approx(delta));
  CHECK(result.y == Approx(delta));
}

TEST_CASE("Repeated ticks accumulate linearly", "[movement]") {
  Rebel::Vec2 pos{0.0f, 0.0f};
  const float delta = 0.064f;

  for (int tick = 0; tick < 20; ++tick) {
    pos = Rebel::apply_move_flags(pos, Rebel::MoveFlags::Right, delta);
  }

  // 20 ticks at 20Hz == 1 second of holding the key.
  CHECK(pos.x == Approx(1.28f));
}
```

## File: ./tests/CMakeLists.txt
```cmake
find_package(Catch2 3 CONFIG REQUIRED)

add_executable(unit_tests
    test_protocol.cpp
    test_movement.cpp
)

target_include_directories(unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/common/include
    ${CMAKE_SOURCE_DIR}/server/game/include
)

target_link_libraries(unit_tests PRIVATE Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(unit_tests)
```

