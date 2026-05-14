#include <iostream>
#include <string>
#include <cstring> 
#include <GLFW/glfw3.h>

#include "NetworkManager.hpp"
#include "VulkanRenderer.hpp"
#include "Version.hpp"
#include "Protocol.hpp" 

int main() {
    std::cout << "--- REBEL CLIENT STARTING ---" << std::endl;
    std::cout << "Version: " << Rebel::VERSION_MAJOR << "." << Rebel::VERSION_MINOR << std::endl;

    // --- 1. NETWORK SETUP ---
    NetworkManager network; 

    // Handle redirection from Login Server to Game Server
    network.registerHandler(Rebel::Opcode::SMSG_AUTH_RESPONSE, [&](auto header) {
        // The payload starts AFTER the 4-byte header. 
        // We must offset the pointer to skip the header bytes.
        auto payloadPtr = reinterpret_cast<uint8_t*>(header.get()) + sizeof(Rebel::PacketHeader);
        auto redirect = reinterpret_cast<Rebel::MsgRedirect*>(payloadPtr);
        
        // --- DEFENSIVE STRING FIX ---
        // Create a strictly null-terminated buffer for the IP
        char cleanIp[17]; 
        std::memset(cleanIp, 0, 17);
        std::memcpy(cleanIp, redirect->ip, 16); 
        
        std::string targetIp(cleanIp);
        uint16_t targetPort = redirect->port;
        // ---------------------------

        std::cout << "[AUTH] Redirecting to Game Server: " << targetIp << ":" << targetPort << std::endl;

        network.disconnect();

        // Small delay isn't needed with a clean socket_ logic, 
        // but ensure the connect call uses the new target.
        if (network.connect(targetIp, std::to_string(targetPort))) {
            std::cout << "[GAME] Successfully connected to Game Server!" << std::endl;
            
            // Handshake with Game Server
            Rebel::PacketHeader join;
            join.size = sizeof(Rebel::PacketHeader);
            join.opcode = static_cast<uint16_t>(Rebel::Opcode::CMSG_PLAYER_MOVE);
            network.sendPacket(join);
        } else {
            std::cerr << "[NETWORK] Connection to Game Server failed." << std::endl;
        }
    });

    network.registerHandler(Rebel::Opcode::SMSG_PONG, [](auto header) {
        std::cout << "[GAME] Server Pong received!" << std::endl;
    });

    // Initial connection to Login Server
    if (!network.connect("127.0.0.1", "54321")) {
        std::cerr << "[WARNING] Login Server not found." << std::endl;
    }

    // --- 2. WINDOW & RENDERER SETUP ---
    if (!glfwInit()) return -1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Rebel Client", nullptr, nullptr);

    if (window) {
        VulkanRenderer renderer;
        try {
            renderer.init(window); 

            // --- 3. MAIN LOOP ---
            bool loginSent = false;
            while (!glfwWindowShouldClose(window)) {
                glfwPollEvents();

                bool isInteracting = (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) || 
                                     (glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS);

                if (isInteracting && !loginSent) {
                    std::cout << "[GAME] Sending Login Request..." << std::endl;
                    
                    // We must actually send the login data payload so the server doesn't hang
                    Rebel::MsgLogin loginData;
                    std::memset(&loginData, 0, sizeof(loginData));

                    Rebel::PacketHeader h;
                    h.size = sizeof(Rebel::PacketHeader) + sizeof(Rebel::MsgLogin);
                    h.opcode = static_cast<uint16_t>(Rebel::Opcode::CMSG_AUTH_SESSION);
                    
                    network.sendPacket(h, &loginData, sizeof(Rebel::MsgLogin));
                    loginSent = true;
                }

                if (!isInteracting) {
                    loginSent = false; 
                }

                renderer.draw(); 
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