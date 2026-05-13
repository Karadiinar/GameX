#include <iostream>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/error_code.hpp>
#include <asio/ip/tcp.hpp>
#include <chrono>
#include <vector>
#include <thread>
#include <algorithm> // Required for std::max
#include <atomic>
#include <memory>

#include "Version.hpp" // Look! We are using our shared file

// Function to handle each tick of the 20Hz game loop
void game_loop_tick(asio::steady_timer& timer, asio::io_context& io_context, std::atomic<int>& tick_count, std::atomic<bool>& running) {
    if (!running.load()) {
        std::cout << "Game loop stopping." << std::endl;
        return;
    }

    // Simulate game logic here
    std::cout << "Server tick: " << tick_count++ << " on thread ID: " << std::this_thread::get_id() << std::endl;

    // Reschedule the timer for the next tick (50ms later for 20Hz)
    timer.expires_at(timer.expiry() + std::chrono::milliseconds(50));
    timer.async_wait([&](const asio::error_code& error) {
        if (!error) {
            // If no error, continue the loop
            game_loop_tick(timer, io_context, tick_count, running);
        } else {
            // Handle timer error (e.g., timer cancelled due to io_context.stop())
            std::cerr << "Timer error in game loop: " << error.message() << std::endl;
        }
    });
}

// New class for handling player sessions
class PlayerSession : public std::enable_shared_from_this<PlayerSession> {
public:
    // Take ownership of the socket using std::move
    PlayerSession(asio::ip::tcp::socket socket) 
        : socket_(std::move(socket)) {}

    // Start listening for data from this player
    void start() {
        std::cout << "[SESSION] Player session started from: " << socket_.remote_endpoint() << std::endl;
        do_read();
    }

private:
    void do_read() {
        // Create a shared pointer to THIS object to keep it alive during the async operation
        auto self(shared_from_this()); 
        
        socket_.async_read_some(asio::buffer(data_, max_length),
            [this, self](const asio::error_code& ec, std::size_t length) {
                if (!ec) {
                    // For now, just print that we received raw bytes
                    std::cout << "[SESSION] Received " << length << " bytes from player." << std::endl;
                    
                    // Keep listening for the next packet
                    do_read(); 
                } else {
                    // If the client closes the game, or their internet drops, we land here.
                    std::cout << "[SESSION] Player disconnected: " << ec.message() << std::endl;
                }
            });
    }

    asio::ip::tcp::socket socket_;
    enum { max_length = 1024 };
    char data_[max_length]; // A raw buffer to catch incoming bytes
};

// Placeholder for handling new client connections
void start_accept(asio::ip::tcp::acceptor& acceptor, asio::io_context& io_context) {
    // Create a new socket on the heap so it outlives this initiation call
    auto socket = std::make_shared<asio::ip::tcp::socket>(io_context);

    // Asynchronously accept a new connection
    acceptor.async_accept(*socket,
        [&, socket](const asio::error_code& error) {
            if (!error) {
                // Create and start the session, moving the socket into it
                std::make_shared<PlayerSession>(std::move(*socket))->start();
            }
            start_accept(acceptor, io_context); // Continue accepting new connections
        });
}

int main() {
    std::cout << "--- REBEL SERVER STARTING ---" << std::endl;
    std::cout << "Version: " << Rebel::VERSION_MAJOR << "." << Rebel::VERSION_MINOR << std::endl;

    asio::io_context io_context;

    // Create a work guard to prevent io_context.run() from exiting prematurely.
    // This ensures the io_context stays alive even if there's no pending work.
    asio::executor_work_guard<asio::io_context::executor_type> work_guard = asio::make_work_guard(io_context);

    // Start listening on port 12345
    asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), 12345);
    asio::ip::tcp::acceptor acceptor(io_context, endpoint);
    std::cout << "Server listening on port " << endpoint.port() << "..." << std::endl;

    // Start the acceptance loop
    start_accept(acceptor, io_context);

    // Determine the number of threads to use for the thread pool.
    // Using hardware_concurrency() is a good starting point, but can be adjusted.
    const unsigned int num_threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> workers;
    workers.reserve(num_threads);

    // Launch worker threads, each running the io_context's event loop.
    for (unsigned int i = 0; i < num_threads; ++i) {
        workers.emplace_back([&io_context, i]() {
            std::cout << "Worker thread " << i << " started (ID: " << std::this_thread::get_id() << ")" << std::endl;
            io_context.run();
            std::cout << "Worker thread " << i << " stopped (ID: " << std::this_thread::get_id() << ")" << std::endl;
        });
    }

    // Atomic flag to signal the game loop to stop
    std::atomic<bool> running(true);

    // Initialize the game loop timer.
    // This timer will be managed by the io_context and executed on one of the worker threads.
    asio::steady_timer timer(io_context); // This timer will be managed by the io_context and executed on one of the worker threads.
    std::atomic<int> tick_count(0);

    // Set the initial expiry time for the timer to start the 20Hz loop
    timer.expires_at(std::chrono::steady_clock::now() + std::chrono::milliseconds(50));

    // Start the asynchronous wait operation for the game loop
    // This posts the first tick to the io_context, which will be picked up by a worker thread.
    game_loop_tick(timer, io_context, tick_count, running);

    std::cout << "Main thread is now free to do other work or wait for shutdown (ID: " << std::this_thread::get_id() << ")" << std::endl;
    std::cout << "Press Enter to stop the server..." << std::endl;

    // Main thread waits for user input to initiate shutdown
    std::cin.get();

    std::cout << "Initiating server shutdown..." << std::endl;
    running.store(false); // Signal game loop to stop rescheduling
    work_guard.reset();   // Release the work guard, allowing io_context.run() to exit if no other work
    io_context.stop();    // Stop the io_context, causing io_context.run() to return in worker threads

    // Join all worker threads to ensure they complete before main exits
    for (std::thread& worker : workers) {
        worker.join();
    }

    std::cout << "--- REBEL SERVER SHUTTING DOWN ---" << std::endl;
    return 0;
}