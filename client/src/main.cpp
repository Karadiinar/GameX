#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <algorithm> 
#include <entt/entt.hpp>

// Must be included before custom graphics headers to ensure Vulkan macros are set
#include <GLFW/glfw3.h>

#include "NetworkManager.hpp"
#include "VulkanRenderer.hpp"
#include "ThreadUtility.hpp"
#include <mutex>

// Global shutdown flag to synchronize thread termination
std::atomic<bool> g_Running{ true };

struct Transform {
    float x, y, z;
    float yaw;
};

struct LocalPlayerTag {};

/**
 * Logic Thread: Responsible for game state evolution, physics, and packet processing.
 */
void LogicThreadEntry(NetworkManager* network, SharedRenderState* renderState, const VulkanRenderer* renderer) {
    entt::registry client_registry;       // <-- The Client's World Data
    entt::entity local_player = entt::null; // <-- Handle to our specific character
    bool inWorld = false;

    const std::chrono::microseconds TICK_TIME(1000000 / 64);
    std::cout << "[Logic] Engine tick thread started.\n";

    while (g_Running) {
        auto start = std::chrono::steady_clock::now();

        // Drain the mailbox
        while (auto packetOpt = network->getPacketQueue().try_pop()) {
            auto& packet = *packetOpt;

            if (packet.header.opcode == static_cast<uint16_t>(Rebel::Opcode::SMSG_AUTH_RESPONSE)) {
                
                if (packet.payload.size() >= sizeof(Rebel::MsgRedirect)) {
                    auto* redirect = reinterpret_cast<Rebel::MsgRedirect*>(packet.payload.data());
                    std::cout << "[Logic] Redirecting to Game Server at " << redirect->ip << ":" << redirect->port << "\n";
                    
                    network->disconnect();
                    network->connect(redirect->ip, std::to_string(redirect->port));
                } 
                else {
                    std::cout << "[Logic] Successfully authenticated with Game Server. Entering world...\n";
                    inWorld = true; 

                    // --- SPAWN THE LOCAL ENTITY ---
                    local_player = client_registry.create();
                    client_registry.emplace<Transform>(local_player, 0.0f, 0.0f, 0.0f, 0.0f);
                    client_registry.emplace<LocalPlayerTag>(local_player);
                    std::cout << "[Logic] Local player entity spawned in client ECS.\n";
                }
            }
        }

        // --- UPDATE ECS AND SEND NETWORK PACKET ---
        if (inWorld && client_registry.valid(local_player)) {
            auto& transform = client_registry.get<Transform>(local_player);
            
            // Read input states from our renderer to change position smoothly at 64Hz
            float speed = 0.02f;
            if (renderer->isMovingLeft()) {
                transform.x -= speed;
            }
            if (renderer->isMovingRight()) {
                transform.x += speed;
            }
            // Remember: In screen coordinates / graphics APIs, Y conventionally goes DOWN, 
            // so pressing UP should decrease Y, and pressing DOWN should increase Y.
            if (renderer->isMovingUp()) {
                transform.y -= speed; // <-- ADD THIS
            }
            if (renderer->isMovingDown()) {
                transform.y += speed; // <-- ADD THIS
            }

            // 1. UPDATE THE RENDER BRIDGE
            {
                std::lock_guard<std::mutex> lock(renderState->mtx);
                renderState->player_x = transform.x;
                renderState->player_y = transform.y;
                renderState->player_z = transform.z;
            }

            // Build the network packet using the ECS data
            Rebel::MsgPlayerMove moveData;
            moveData.x = transform.x;
            moveData.y = transform.y;
            moveData.z = transform.z;
            moveData.yaw = transform.yaw;

            Rebel::PacketHeader head;
            head.opcode = static_cast<uint16_t>(Rebel::Opcode::CMSG_PLAYER_MOVE);
            head.size = sizeof(Rebel::PacketHeader) + sizeof(Rebel::MsgPlayerMove);

            network->sendPacket(head, &moveData, sizeof(Rebel::MsgPlayerMove));
        }

        auto end = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
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
        std::cout << "[Main] Pinning Network subsystem to Core " << coreMap[1] << "\n";
        Rebel::ThreadUtils::SetThreadAffinity(network.getThread(), coreMap[1]);
    }
    
    network.connect("127.0.0.1", "54321"); 

    // --- INSTANTIATE THE BRIDGE ---
    SharedRenderState sharedRenderState;

    // --- 4. GRAPHICS SUBSYSTEM ENVIRONMENT INITIALIZATION ---
    try {
        // Fire up GLFW before calling window manipulation functions
        if (!glfwInit()) {
            throw std::runtime_error("[Graphics] Failed to initialize GLFW!");
        }
        
        // Setup default configuration settings
        GraphicsConfig userGraphicsSettings;
        userGraphicsSettings.presentMode = PresentModeSetting::TripleBuffer; 
        userGraphicsSettings.windowWidth = 1280;
        userGraphicsSettings.windowHeight = 720;

        // Flush hints and explicitly declare Vulkan API support before generating the container
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE); 

        // Create the window context
        GLFWwindow* window = glfwCreateWindow(
            userGraphicsSettings.windowWidth, 
            userGraphicsSettings.windowHeight, 
            "Rebel Client", 
            nullptr, 
            nullptr
        );

        if (!window) {
            throw std::runtime_error("[Graphics] Failed to create GLFW window!");
        }

        // --- DECLARED FIRST: The Renderer instance now exists in memory ---
        VulkanRenderer renderer;

        // --- 3. SPAWN LOGIC SUBSYSTEM (Safe to pass &renderer now!) ---
        std::thread logicThread(LogicThreadEntry, &network, &sharedRenderState, &renderer);
        if (coreMap.size() > 2) {
            std::cout << "[Main] Pinning Logic subsystem to Core " << coreMap[2] << "\n";
            Rebel::ThreadUtils::SetThreadAffinity(logicThread, coreMap[2]);
        }
        
        // Pass the window context and config to the initialization pipeline
        renderer.init(window, userGraphicsSettings);

        // Tell GLFW: "Hey, associate this 'renderer' C++ object with this specific window."
        glfwSetWindowUserPointer(window, &renderer);

        // Tell GLFW: "When a key is pressed, execute our static callback handler."
        glfwSetKeyCallback(window, VulkanRenderer::glfw_key_callback);
        
        // Set up the window resize callback lambda
        glfwSetFramebufferSizeCallback(window, [](GLFWwindow* win, int width, int height) {
            auto* rend = reinterpret_cast<VulkanRenderer*>(glfwGetWindowUserPointer(win));
            if (rend) {
                rend->framebufferResizeCallback();
            }
        });

        // --- UNIFIED MAIN RENDER LOOP ---
        while (!renderer.shouldClose()) {
            renderer.pollEvents(); // Processes keystrokes & dispatches the callback functions
            renderer.drawFrame(&sharedRenderState);
        }

        // --- 5. CLEAN SHUTDOWN SEQUENCE ---
        std::cout << "\n[Main] Shutdown signal received. Terminating subsystems...\n";
        g_Running = false;
        
        if (logicThread.joinable()) {
            logicThread.join();
        }

        renderer.cleanup();
        glfwDestroyWindow(window);
        glfwTerminate();
    }
    catch (const std::exception& e) {
        std::cerr << "\n[Fatal Error] " << e.what() << std::endl;
        g_Running = false;
        glfwTerminate();
        return -1;
    }

    std::cout << "[Main] Clean shutdown complete.\n";
    return 0;
}