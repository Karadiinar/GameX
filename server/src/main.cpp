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
#include "Packet.hpp"
#include <asio/read.hpp>
#include <asio/write.hpp>

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
    PlayerSession(asio::ip::tcp::socket socket) 
        : socket_(std::move(socket)) {}

    void start() {
        std::cout << "[SESSION] Player session started from: " << socket_.remote_endpoint() << std::endl;
        read_header(); // Start the first stage of the read loop
    }

private:
    void read_header() {
        auto self(shared_from_this()); 
        
        // Stage 1: Read exactly the size of our PacketHeader (4 bytes)
        asio::async_read(socket_, asio::buffer(&header_, sizeof(Rebel::PacketHeader)),
            [this, self](const asio::error_code& ec, std::size_t /*length*/) {
                if (!ec) {
                    // Security check: Drop the player if they send an impossible size
                    if (header_.size < sizeof(Rebel::PacketHeader) || header_.size > 8192) {
                        std::cerr << "[SESSION] Malformed packet size: " << header_.size << ". Dropping." << std::endl;
                        return;
                    }

                    // Calculate how much payload data is attached to this header
                    uint16_t payload_size = header_.size - sizeof(Rebel::PacketHeader);
                    
                    if (payload_size > 0) {
                        payload_.resize(payload_size);
                        read_payload(); // Go to Stage 2
                    } else {
                        process_packet(); // No payload, handle the header immediately
                        read_header();    // Loop back to listen for the next packet
                    }
                } else {
                    std::cout << "[SESSION] Player disconnected: " << ec.message() << std::endl;
                }
            });
    }

    void read_payload() {
        auto self(shared_from_this());

        // Stage 2: Read the exact amount of payload bytes dictated by the header
        asio::async_read(socket_, asio::buffer(payload_.data(), payload_.size()),
            [this, self](const asio::error_code& ec, std::size_t /*length*/) {
                if (!ec) {
                    process_packet(); // We have the full packet now
                    read_header();    // Loop back to listen for the next packet
                } else {
                    std::cout << "[SESSION] Player disconnected during payload transfer." << std::endl;
                }
            });
    }

    void process_packet() {
        Rebel::Opcode opcode = static_cast<Rebel::Opcode>(header_.opcode);

        switch (opcode) {
            case Rebel::Opcode::CMSG_PING:
                std::cout << "[SESSION] Received CMSG_PING! Sending SMSG_PONG back..." << std::endl;
                send_pong(); // <--- Call the new reply function
                break;
            default:
                std::cout << "[SESSION] Unknown Opcode: 0x" << std::hex << header_.opcode << std::dec << std::endl;
                break;
        }
    }

    void send_pong() {
        // Create a shared pointer to keep the session alive
        auto self(shared_from_this());
        
        // Allocate the packet on the heap so it survives the async_write operation
        auto pong_packet = std::make_shared<Rebel::PacketHeader>();
        pong_packet->size = sizeof(Rebel::PacketHeader);
        pong_packet->opcode = static_cast<uint16_t>(Rebel::Opcode::SMSG_PONG);

        // Write the exact 4 bytes back to the client
        asio::async_write(socket_, asio::buffer(pong_packet.get(), sizeof(Rebel::PacketHeader)),
            [this, self, pong_packet](const asio::error_code& ec, std::size_t /*length*/) {
                if (!ec) {
                    // Success!
                    std::cout << "[SESSION] SMSG_PONG delivered." << std::endl;
                } else {
                    std::cerr << "[SESSION] Failed to send PONG: " << ec.message() << std::endl;
                }
            });
    }

    asio::ip::tcp::socket socket_;
    Rebel::PacketHeader header_;
    std::vector<uint8_t> payload_;
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