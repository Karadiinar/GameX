#include <iostream>
#include <asio.hpp>
#include <chrono>
#include <vector>
#include <thread>
#include <algorithm>
#include <atomic>
#include <memory>
#include "Protocol.hpp"
#include "Version.hpp"

// Function to handle each tick of the 20Hz game loop
void game_loop_tick(asio::steady_timer& timer, 
                    asio::strand<asio::io_context::executor_type>& strand, 
                    std::atomic<int>& tick_count, 
                    std::atomic<bool>& running) {
    if (!running.load()) return;

    // Everything here is now thread-safe relative to session packet processing
    std::cout << "Tick: " << tick_count++ << std::endl;

    timer.expires_at(timer.expiry() + std::chrono::milliseconds(50));
    
    timer.async_wait(asio::bind_executor(strand, 
        [&timer, &strand, &tick_count, &running](const asio::error_code& error) {
            if (!error) {
                game_loop_tick(timer, strand, tick_count, running);
            }
        }));
}

class PlayerSession : public std::enable_shared_from_this<PlayerSession> {
public:
    PlayerSession(asio::ip::tcp::socket socket, asio::strand<asio::io_context::executor_type>& strand) 
        : socket_(std::move(socket)), strand_(strand) {}

    void start() {
        try {
            std::cout << "[SESSION] Player connected from: " << socket_.remote_endpoint() << std::endl;
            read_header();
        } catch (const std::exception& e) {
            std::cerr << "[SESSION] Error starting session: " << e.what() << std::endl;
        }
    }

private:
    void read_header() {
        auto self(shared_from_this()); 
        asio::async_read(socket_, asio::buffer(&header_, sizeof(Rebel::PacketHeader)),
            [this, self](const asio::error_code& ec, std::size_t) {
                if (!ec) {
                    if (header_.size < sizeof(Rebel::PacketHeader) || header_.size > 8192) {
                        std::cerr << "[SESSION] Malformed packet size. Dropping." << std::endl;
                        return;
                    }

                    uint16_t payload_size = header_.size - sizeof(Rebel::PacketHeader);
                    if (payload_size > 0) {
                        payload_.resize(payload_size);
                        read_payload();
                    } else {
                        on_packet_received();
                        read_header();
                    }
                }
            });
    }

    void read_payload() {
        auto self(shared_from_this());
        asio::async_read(socket_, asio::buffer(payload_.data(), payload_.size()),
            [this, self](const asio::error_code& ec, std::size_t) {
                if (!ec) {
                    on_packet_received();
                    read_header();
                }
            });
    }

    void on_packet_received() {
        // Post the processing to the strand so it doesn't conflict with the game tick
        asio::post(strand_, [self = shared_from_this()]() {
            self->process_packet();
        });
    }

    void process_packet() {
        Rebel::Opcode opcode = static_cast<Rebel::Opcode>(header_.opcode);
        switch (opcode) {
            case Rebel::Opcode::CMSG_PING:
                send_pong();
                break;
            case Rebel::Opcode::CMSG_PLAYER_MOVE:
                std::cout << "[SESSION] Player Move received (Strand Safe)" << std::endl;
                break;
            default:
                break;
        }
    }

    void send_pong() {
        auto self(shared_from_this());
        auto pong_packet = std::make_shared<Rebel::PacketHeader>();
        pong_packet->size = sizeof(Rebel::PacketHeader);
        pong_packet->opcode = static_cast<uint16_t>(Rebel::Opcode::SMSG_PONG);

        asio::async_write(socket_, asio::buffer(pong_packet.get(), sizeof(Rebel::PacketHeader)),
            [this, self, pong_packet](const asio::error_code& ec, std::size_t) {
                if (ec) std::cerr << "[SESSION] PONG error: " << ec.message() << std::endl;
            });
    }

    asio::ip::tcp::socket socket_;
    asio::strand<asio::io_context::executor_type>& strand_;
    Rebel::PacketHeader header_;
    std::vector<uint8_t> payload_;
};

void start_accept(asio::ip::tcp::acceptor& acceptor, asio::io_context& io_context, asio::strand<asio::io_context::executor_type>& strand) {
    acceptor.async_accept([&acceptor, &io_context, &strand](const asio::error_code& error, asio::ip::tcp::socket socket) {
        if (!error) {
            // --- THE LOUD PRINT ---
            std::cout << "[GAME] Incoming connection from: " << socket.remote_endpoint() << std::endl;
            
            std::make_shared<PlayerSession>(std::move(socket), strand)->start();
        }
        start_accept(acceptor, io_context, strand);
    });
}

int main() {
    std::cout << "--- REBEL SERVER STARTING ---" << std::endl;
    asio::io_context io_context;
    auto work_guard = asio::make_work_guard(io_context);
    auto game_strand = asio::make_strand(io_context);

    asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), 12345);
    asio::ip::tcp::acceptor acceptor(io_context, endpoint);

    start_accept(acceptor, io_context, game_strand);

    const unsigned int num_threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> workers;
    for (unsigned int i = 0; i < num_threads; ++i) {
        workers.emplace_back([&io_context]() { io_context.run(); });
    }

    std::atomic<bool> running(true);
    asio::steady_timer timer(io_context);
    std::atomic<int> tick_count(0);

    timer.expires_at(std::chrono::steady_clock::now() + std::chrono::milliseconds(50));
    game_loop_tick(timer, game_strand, tick_count, running);

    std::cout << "Press Enter to stop..." << std::endl;
    std::cin.get();

    running.store(false);
    io_context.stop();
    for (auto& t : workers) t.join();

    return 0;
}