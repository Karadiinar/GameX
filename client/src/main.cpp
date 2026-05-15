#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <algorithm> // Required for std::clamp if used

// Must be included before custom graphics headers to ensure Vulkan macros are set
#include <GLFW/glfw3.h>

#include "NetworkManager.hpp"
#include "VulkanRenderer.hpp"
#include "ThreadUtility.hpp"

// Global shutdown flag to synchronize thread termination
std::atomic<bool> g_Running{ true };

/**
 * Logic Thread: Responsible for game state evolution, physics, and packet processing.
 */
void LogicThreadEntry(NetworkManager* network) {
    const std::chrono::microseconds TICK_TIME(1000000 / 64);
    std::cout << "[Logic] Engine tick thread started.\n";

    while (g_Running) {
        auto start = std::chrono::steady_clock::now();

        // Drain the mailbox
        while (auto packetOpt = network->getPacketQueue().try_pop()) {
    auto& packet = *packetOpt;

    // SMSG_AUTH_RESPONSE (0x0004) from your Protocol.hpp
    if (packet.header.opcode == static_cast<uint16_t>(Rebel::Opcode::SMSG_AUTH_RESPONSE)) {
        std::cout << "[Logic] Handshake Successful! Reconnecting to Game Server...\n";
        
        network->disconnect();
        // Connect to the Game Server port
        network->connect("127.0.0.1", "12345"); 
    }
}

        auto end = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

        if (elapsed < TICK_TIME) {
            std::this_thread::sleep_for(TICK_TIME - elapsed);
        }
    }
    std::cout << "[Logic] Engine tick thread exiting...\n";
} // <--- THIS WAS THE MISSING BRACE

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

    // --- 3. LOGIC SUBSYSTEM ---
    std::thread logicThread(LogicThreadEntry, &network);
    if (coreMap.size() > 2) {
        std::cout << "[Main] Pinning Logic subsystem to Core " << coreMap[2] << "\n";
        Rebel::ThreadUtils::SetThreadAffinity(logicThread, coreMap[2]);
    }

    // --- 4. GRAPHICS SUBSYSTEM ---
    try {
        if (!glfwInit()) {
            throw std::runtime_error("[Graphics] Failed to initialize GLFW!");
        }
        
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE); 

        GLFWwindow* window = glfwCreateWindow(1280, 720, "Rebel Client", nullptr, nullptr);
        if (!window) {
            throw std::runtime_error("[Graphics] Failed to create window!");
        }

        VulkanRenderer renderer;
        renderer.init(window);
        
        std::vector<uint32_t> graphicsWorkers;
        for (size_t i = 3; i < coreMap.size(); ++i) {
            graphicsWorkers.push_back(coreMap[i]);
        }
        
        // Initial Auth Request
       Rebel::PacketHeader authReq;
        authReq.opcode = static_cast<uint16_t>(Rebel::Opcode::CMSG_AUTH_SESSION);
        authReq.size = sizeof(Rebel::PacketHeader); 
        network.sendPacket(authReq);

        while (!renderer.shouldClose()) {
            renderer.pollEvents();
            renderer.drawFrame();
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