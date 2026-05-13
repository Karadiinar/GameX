#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <asio.hpp>
#include <GLFW/glfw3.h>
#include <asio/read.hpp>

#include "Packet.hpp"
#include "VulkanRenderer.hpp"
#include "Version.hpp"

using asio::ip::tcp;

int main() {
    std::cout << "--- REBEL CLIENT STARTING ---" << std::endl;
    std::cout << "Version: " << Rebel::VERSION_MAJOR << "." << Rebel::VERSION_MINOR << std::endl;

    // --- 1. NETWORK INITIALIZATION ---
    asio::io_context io_context;
    auto work_guard = asio::make_work_guard(io_context);
    std::thread network_thread([&io_context]() { io_context.run(); });

    tcp::socket socket(io_context);
    tcp::resolver resolver(io_context);
    auto endpoints = resolver.resolve("127.0.0.1", "12345");

    asio::async_connect(socket, endpoints, [&](const asio::error_code& error, const tcp::endpoint& endpoint) {
        if (!error) {
            std::cout << "[NETWORK] Connected to " << endpoint << std::endl;

            // Send Ping
            auto ping = std::make_shared<Rebel::PacketHeader>();
            ping->size = sizeof(Rebel::PacketHeader);
            ping->opcode = static_cast<uint16_t>(Rebel::Opcode::CMSG_PING);
            
            asio::async_write(socket, asio::buffer(ping.get(), sizeof(Rebel::PacketHeader)), [ping](const asio::error_code& ec, std::size_t) {});

            // Catch Pong
            auto pong = std::make_shared<Rebel::PacketHeader>();
            asio::async_read(socket, asio::buffer(pong.get(), sizeof(Rebel::PacketHeader)), [pong](const asio::error_code& ec, std::size_t) {
                if (!ec && pong->opcode == static_cast<uint16_t>(Rebel::Opcode::SMSG_PONG)) {
                    std::cout << "[NETWORK] Bidirectional connection confirmed." << std::endl;
                }
            });
        }
    });

    // --- 2. WINDOW & RENDERER SETUP ---
    if (!glfwInit()) return -1;
    
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Rebel Client", nullptr, nullptr);

    if (window) {
        VulkanRenderer renderer;
        try {
            renderer.init(window);

            // --- 3. MAIN LOOP ---
            while (!glfwWindowShouldClose(window)) {
                glfwPollEvents();
                renderer.draw();
            }

        } catch (const std::exception& e) {
            std::cerr << "[FATAL] " << e.what() << std::endl;
        }

        // Cleanup Renderer & Window
        renderer.cleanup();
        glfwDestroyWindow(window);
    }

    // --- 4. FINAL SHUTDOWN ---
    glfwTerminate();
    
    asio::error_code ec;
    socket.close(ec);
    work_guard.reset();
    io_context.stop();
    if (network_thread.joinable()) network_thread.join();

    std::cout << "--- REBEL CLIENT SHUTTING DOWN ---" << std::endl;
    return 0;
}