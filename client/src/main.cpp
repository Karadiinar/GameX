#include <iostream>
#include <GLFW/glfw3.h>

#include "NetworkManager.hpp"
#include "VulkanRenderer.hpp"
#include "Version.hpp"

int main() {
    std::cout << "--- REBEL CLIENT STARTING ---" << std::endl;
    std::cout << "Version: " << Rebel::VERSION_MAJOR << "." << Rebel::VERSION_MINOR << std::endl;

    // --- 1. NETWORK SETUP ---
    NetworkManager network; 

    // Register handlers BEFORE connecting
    network.registerHandler(Rebel::Opcode::SMSG_PONG, [](auto header) {
        std::cout << "[GAME] Server responded to Ping. Connection is healthy!" << std::endl;
    });

    network.registerHandler(Rebel::Opcode::SMSG_AUTH_RESPONSE, [](auto header) {
        std::cout << "[GAME] Received Login/Auth response from server." << std::endl;
        // In the future, trigger UI transition here
    });

    if (!network.connect("127.0.0.1", "12345")) {
        std::cerr << "[WARNING] Starting in offline mode (Server not found)." << std::endl;
    }

    // --- 2. WINDOW SETUP ---
    if (!glfwInit()) return -1;
    
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Rebel Client", nullptr, nullptr);

    if (window) {
        VulkanRenderer renderer;
        try {
            // Lock onto that RTX 4080!
            renderer.init(window);

            // --- 3. MAIN LOOP ---
            bool loginSent = false;
            while (!glfwWindowShouldClose(window)) {

                while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    // Check for Left Mouse Button
    if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS && !loginSent) {
        std::cout << "[GAME] Mouse click detected! Sending Auth..." << std::endl;
        
        Rebel::PacketHeader auth_packet;
        auth_packet.size = sizeof(Rebel::PacketHeader);
        auth_packet.opcode = static_cast<uint16_t>(Rebel::Opcode::CMSG_AUTH_SESSION);
        
        network.sendPacket(auth_packet);
        loginSent = true;
    }

    // Reset when the button is released
    if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_RELEASE) {
        loginSent = false;
    }

    renderer.draw();
}
              if (glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS) {
        std::cout << "[GAME] Login button pressed! Sending Auth..." << std::endl;
        
        Rebel::PacketHeader auth_packet;
        auth_packet.size = sizeof(Rebel::PacketHeader);
        auth_packet.opcode = static_cast<uint16_t>(Rebel::Opcode::CMSG_AUTH_SESSION);
        
        network.sendPacket(auth_packet);
    }

        renderer.draw();
        if (glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS && !loginSent) {
    // ... send packet logic ...
    loginSent = true; 
}

if (glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_RELEASE) {
    loginSent = false; // Reset so they can try again if auth fails
}
            }

        } catch (const std::exception& e) {
            std::cerr << "[FATAL] " << e.what() << std::endl;
        }

        renderer.cleanup();
        glfwDestroyWindow(window);
    }

    glfwTerminate();
    std::cout << "--- REBEL CLIENT SHUTTING DOWN ---" << std::endl;
    return 0;
}