#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <asio.hpp>
#include <GLFW/glfw3.h>
#include "Packet.hpp"
#include <asio/read.hpp>
#include "VulkanRenderer.hpp"

#include "Version.hpp"

using asio::ip::tcp;

// GLFW error callback function
void glfwErrorCallback(int error, const char* description) {
    std::cerr << "[GLFW ERROR] Code: " << error << ", Description: " << description << std::endl;
}

int main() {
    std::cout << "--- REBEL CLIENT STARTING ---" << std::endl;
    std::cout << "Version: " << Rebel::VERSION_MAJOR << "." << Rebel::VERSION_MINOR << std::endl;

    // --- NETWORK SETUP ---
    asio::io_context io_context;
    auto work_guard = asio::make_work_guard(io_context);

    // Run the network context on a background thread so it doesn't freeze our window
    std::thread network_thread([&io_context]() {
        io_context.run();
    });

    // Create our socket and resolver
    tcp::socket socket(io_context);
    tcp::resolver resolver(io_context);
    auto endpoints = resolver.resolve("127.0.0.1", "12345");

    // Asynchronously connect to the server
    asio::async_connect(socket, endpoints,
        [&socket](const asio::error_code& error, const tcp::endpoint& endpoint) {
            if (!error) {
                std::cout << "[NETWORK] Connected to server at " << endpoint << "!" << std::endl;

                // 1. Send the Ping
                auto ping_packet = std::make_shared<Rebel::PacketHeader>();
                ping_packet->size = sizeof(Rebel::PacketHeader);
                ping_packet->opcode = static_cast<uint16_t>(Rebel::Opcode::CMSG_PING);
                
                asio::async_write(socket, asio::buffer(ping_packet.get(), sizeof(Rebel::PacketHeader)),
                    [ping_packet](const asio::error_code& ec, std::size_t length) {
                        if (!ec) {
                            std::cout << "[NETWORK] Sent CMSG_PING to server." << std::endl;
                        }
                    });

                // 2. Start listening for the Server's reply (The Catch)
                // We allocate a header buffer on the heap to catch the incoming 4 bytes
                auto response_header = std::make_shared<Rebel::PacketHeader>();
                asio::async_read(socket, asio::buffer(response_header.get(), sizeof(Rebel::PacketHeader)),
                    [response_header](const asio::error_code& ec, std::size_t length) {
                        if (!ec) {
                            if (response_header->opcode == static_cast<uint16_t>(Rebel::Opcode::SMSG_PONG)) {
                                std::cout << "[NETWORK] Received SMSG_PONG from server! Connection is 100% bidirectional." << std::endl;
                            }
                        }
                    });

            } else {
                std::cerr << "[NETWORK] Failed to connect: " << error.message() << std::endl;
            }
        });
    // ---------------------

    // --- WINDOW SETUP ---
    glfwSetErrorCallback(glfwErrorCallback); // Set the error callback
    if (!glfwInit()) return -1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Rebel Client", nullptr, nullptr);

    if (!window) {
        std::cerr << "[ERROR] Failed to create GLFW window!" << std::endl;
        glfwTerminate();
        // Cleanup network before exiting
        goto network_cleanup;
    }

   // --- VULKAN SETUP ---
    VulkanRenderer renderer;
    try {
        renderer.init(window);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << std::endl;
        
        // We MUST join the thread before exiting, or C++ will nuke the program.
        asio::error_code ec;
        socket.close(ec);
        work_guard.reset();
        io_context.stop();
        if (network_thread.joinable()) {
            network_thread.join();
        }
        
        return -1;
    }

    // Main render loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        renderer.draw(); // Eventually, this will draw our triangle!
    }
    // ---------------------

    // --- CLEANUP ---
    renderer.cleanup();
    glfwDestroyWindow(window);
    glfwTerminate();
    // ---------------------
    
network_cleanup:
    // Close network cleanly
    asio::error_code ec;
    socket.close(ec);
    work_guard.reset();
    io_context.stop();
    network_thread.join();

    std::cout << "--- REBEL CLIENT SHUTTING DOWN ---" << std::endl;
    return 0;
}