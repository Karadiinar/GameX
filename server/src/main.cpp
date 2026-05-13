#include <iostream>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <chrono> // Required for std::chrono::milliseconds

#include "Version.hpp" // Look! We are using our shared file

// Function to handle each tick of the 20Hz game loop
void game_loop_tick(asio::steady_timer& timer, asio::io_context& io_context, int& tick_count) {
    // Simulate game logic here
    std::cout << "Server tick: " << tick_count++ << std::endl;

    // Reschedule the timer for the next tick (50ms later for 20Hz)
    timer.expires_at(timer.expiry() + std::chrono::milliseconds(50));
    timer.async_wait([&](const asio::error_code& error) {
        if (!error) {
            // If no error, continue the loop
            game_loop_tick(timer, io_context, tick_count);
        } else {
            // Handle timer error (e.g., timer cancelled)
            std::cerr << "Timer error in game loop: " << error.message() << std::endl;
        }
    });
}

int main() {
    std::cout << "--- REBEL SERVER STARTING ---" << std::endl;
    std::cout << "Version: " << Rebel::VERSION_MAJOR << "." << Rebel::VERSION_MINOR << std::endl;
    
    // This is where our 20Hz loop will eventually go
    asio::io_context io_context;
    asio::steady_timer timer(io_context);
    int tick_count = 0;

    // Set the initial expiry time for the timer to start the 20Hz loop
    timer.expires_at(std::chrono::steady_clock::now() + std::chrono::milliseconds(50));

    // Start the asynchronous wait operation for the game loop
    game_loop_tick(timer, io_context, tick_count);

    // Run the io_context to start processing asynchronous operations (like our timer)
    io_context.run();

    std::cout << "--- REBEL SERVER SHUTTING DOWN ---" << std::endl;
    return 0;
}