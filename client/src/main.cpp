#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <asio.hpp>
#include <GLFW/glfw3.h>

#include "Version.hpp"

using asio::ip::tcp;

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

                // The Kiss! Send a test payload to the server.
                // We use a shared_ptr so the string stays alive in memory until asio is done sending it.
                auto msg = std::make_shared<std::string>("<3 Hello from the Rebel Client! <3");
                
                asio::async_write(socket, asio::buffer(*msg),
                    [msg](const asio::error_code& ec, std::size_t length) {
                        if (!ec) {
                            std::cout << "[NETWORK] Sent greeting (" << length << " bytes) to server." << std::endl;
                        }
                    });
            } else {
                std::cerr << "[NETWORK] Failed to connect: " << error.message() << std::endl;
            }
        });
    // ---------------------

    // --- WINDOW SETUP ---
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return -1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    const int WIDTH = 1280;
    const int HEIGHT = 720;
    GLFWwindow* window = glfwCreateWindow(WIDTH, HEIGHT, "Rebel Client", nullptr, nullptr);

    if (!window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }

    // Main render loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
    }
    // ---------------------

    // --- CLEANUP ---
    std::cout << "Initiating client shutdown..." << std::endl;
    
    // Close network cleanly
    asio::error_code ec;
    socket.close(ec);
    work_guard.reset();
    io_context.stop();
    network_thread.join();

    // Close window cleanly
    glfwDestroyWindow(window);
    glfwTerminate();

    std::cout << "--- REBEL CLIENT SHUTTING DOWN ---" << std::endl;
    return 0;
}