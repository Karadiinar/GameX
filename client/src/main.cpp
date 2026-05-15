#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <algorithm> // Required for std::clamp if used
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
void LogicThreadEntry(NetworkManager* network, SharedRenderState* renderState) {

    entt::registry client_registry;       // <-- The Client's World Data
    entt::entity local_player = entt::null; // <-- Handle to our specific dwarf
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

        auto end = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        if (elapsed < TICK_TIME) {
            std::this_thread::sleep_for(TICK_TIME - elapsed);
        }

        // --- UPDATE ECS AND SEND NETWORK PACKET ---
        if (inWorld && client_registry.valid(local_player)) {
            auto& transform = client_registry.get<Transform>(local_player);
            transform.x += 0.05f; 

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
    // (Only make ONE of these!)
    SharedRenderState sharedRenderState;

    // --- 3. LOGIC SUBSYSTEM ---
    // Pass our single bridge to the logic thread
    std::thread logicThread(LogicThreadEntry, &network, &sharedRenderState);
    if (coreMap.size() > 2) {
        std::cout << "[Main] Pinning Logic subsystem to Core " << coreMap[2] << "\n";
        Rebel::ThreadUtils::SetThreadAffinity(logicThread, coreMap[2]);
    }

    // --- 4. GRAPHICS SUBSYSTEM ---
    try {
       if (!glfwInit()) {
            throw std::runtime_error("[Graphics] Failed to initialize GLFW!");
        }
        
        // Setup your default settings profile
        GraphicsConfig userGraphicsSettings;
        userGraphicsSettings.presentMode = PresentModeSetting::TripleBuffer; // Fast-sync
        userGraphicsSettings.windowWidth = 1280;
        userGraphicsSettings.windowHeight = 720;

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE); 

        // Use the settings values directly to spawn the window dimension sizes
        GLFWwindow* window = glfwCreateWindow(
            userGraphicsSettings.windowWidth, 
            userGraphicsSettings.windowHeight, 
            "Rebel Client", nullptr, nullptr
        );
        
        if (!window) {
            throw std::runtime_error("[Graphics] Failed to create window!");
        }

        VulkanRenderer renderer;
        renderer.init(window, userGraphicsSettings); // <-- Pass the settings into Vulkan!

        // (Optional: Your graphics workers setup can stay here)
        std::vector<uint32_t> graphicsWorkers;
        for (size_t i = 3; i < coreMap.size(); ++i) {
            graphicsWorkers.push_back(coreMap[i]);
        }

        // ONE unified render loop
        while (!renderer.shouldClose()) {
            renderer.pollEvents();
            
            // Pass the bridge into Vulkan!
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
        if (logicThread.joinable()) {
            logicThread.join();
        }
        return -1;
    }

    std::cout << "[Main] Clean shutdown complete.\n";
    return 0;
}