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
#include <entt/entt.hpp>



entt::registry world;

// Define some basic components
struct Position { float x, y, z; };
struct PlayerData { std::string name; };

// Function to handle each tick of the 20Hz game loop
void game_loop_tick(asio::steady_timer& timer, 
                    asio::strand<asio::io_context::executor_type>& strand, 
                    std::atomic<int>& tick_count, 
                    std::atomic<bool>& running) {
    if (!running.load()) return;

    // 1. Logic & Logging
    int current_tick = tick_count++;
    
    // Every 20 ticks (approx 1 second at 20Hz)
    if (current_tick % 20 == 0) {
        auto view = world.view<PlayerData, Position>();
        
        // If the world is empty, just print a heartbeat
        if (view.size_hint() == 0) {
    std::cout << "[WORLD] Heartbeat - Tick: " << current_tick << std::endl;
} else {
            view.each([current_tick](auto entity, auto &data, auto &pos) {
                std::cout << "[WORLD] Tick: " << current_tick 
                          << " | Player: " << data.name 
                          << " | X: " << pos.x << std::endl;
            });
        }
    }

    // 2. Schedule the next tick
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

enum class SessionState {
        WAITING_FOR_AUTH,
        AUTHENTICATED,
        DISCONNECTED
    };
    PlayerSession(asio::ip::tcp::socket socket, asio::strand<asio::io_context::executor_type>& strand) 
        : socket_(std::move(socket)), 
          strand_(strand), 
          state_(SessionState::WAITING_FOR_AUTH) {} // Initialize at the gate!

    void start() {
        try {
            std::cout << "[SESSION] Player connected from: " << socket_.remote_endpoint() << std::endl;
            read_header();
        } catch (const std::exception& e) {
            std::cerr << "[SESSION] Error starting session: " << e.what() << std::endl;
        }
    }

    ~PlayerSession() {
        if (has_entity_) {
            // We MUST post this to the strand because EnTT's registry 
            // is not thread-safe by default. This ensures the destruction 
            // happens in between game loop ticks.
            auto id = entity_id_;
            asio::post(strand_, [id]() {
                if (world.valid(id)) {
                    world.destroy(id);
                    std::cout << "[WORLD] Entity destroyed (Player disconnected)." << std::endl;
                }
            });
        }
    }

private:

entt::entity entity_id_{ entt::null };
    bool has_entity_{ false };

    void read_header() {
        auto self(shared_from_this()); 
        asio::async_read(socket_, asio::buffer(&header_, sizeof(Rebel::PacketHeader)),
            [this, self](const asio::error_code& ec, std::size_t) {
                if (!ec) {
                    uint16_t payload_size = header_.size - sizeof(Rebel::PacketHeader);
                    if (payload_size > 0) {
                        payload_.resize(payload_size);
                        read_payload();
                    } else {
                        on_packet_received();
                        
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
                    
                }
            });
    }

    void on_packet_received() {
        // Ensure processing happens on the strand
        asio::post(strand_, [self = shared_from_this()]() {
            self->process_packet();
            
            // Start reading the next packet ONLY NOW that the buffer is safely processed
            self->read_header(); 
        });
    }

    void process_packet() {
        Rebel::Opcode opcode = static_cast<Rebel::Opcode>(header_.opcode);

        if (state_ == SessionState::WAITING_FOR_AUTH) {
            if (opcode == Rebel::Opcode::CMSG_AUTH_SESSION) {
                handle_auth();
            } else {
                std::cout << "[SESSION] Unauthorized opcode " << (int)opcode << ". Closing." << std::endl;
                socket_.close();
            }
            return;
        }

        switch (opcode) {
            case Rebel::Opcode::CMSG_PING:
                send_pong();
                break;

            case Rebel::Opcode::CMSG_PLAYER_MOVE: {
                // 1. Check if the payload matches our movement struct
                if (payload_.size() < sizeof(Rebel::MsgPlayerMove)) {
                    std::cerr << "[SESSION] Malformed move packet size." << std::endl;
                    break;
                }

                // 2. Map the raw bytes to our struct
                auto* move = reinterpret_cast<Rebel::MsgPlayerMove*>(payload_.data());

                // 3. Update the EnTT registry
                // No mutex needed because we are on the game_strand!
                if (has_entity_ && world.valid(entity_id_)) {
                    auto& pos = world.get<Position>(entity_id_);
                    pos.x = move->x;
                    pos.y = move->y;
                    pos.z = move->z;
                    // Note: You could add a Rotation component for the 'yaw' here too
                }
                break;
            }
            default:
                break;
        }
    }

    void handle_auth() {
    if (payload_.size() < sizeof(Rebel::MsgLogin)) {
        std::cerr << "[SESSION] Auth packet too small." << std::endl;
        
        socket_.close();
        return;
    }

    auto* msg = reinterpret_cast<Rebel::MsgLogin*>(payload_.data());
    std::cout << "[SESSION] Player '" << msg->username 
              << "' (Ver: " << msg->version << ") authenticated." << std::endl;

    state_ = SessionState::AUTHENTICATED;

    entity_id_ = world.create();
    world.emplace<PlayerData>(entity_id_, std::string(msg->username));
    world.emplace<Position>(entity_id_, 0.0f, 0.0f, 0.0f);
    has_entity_ = true;
    
    std::cout << "[WORLD] Spawned entity for " << msg->username << std::endl;

    auto response = std::make_shared<Rebel::PacketHeader>();
    response->size = sizeof(Rebel::PacketHeader);
    response->opcode = static_cast<uint16_t>(Rebel::Opcode::SMSG_AUTH_RESPONSE);

    auto self(shared_from_this());
    asio::async_write(socket_, asio::buffer(response.get(), sizeof(Rebel::PacketHeader)),
        [this, self, response](const asio::error_code& ec, std::size_t) {
            if (ec) {
                socket_.close();
            }
        });
}

    void send_pong() {
        // ... (your existing send_pong logic)
    }

    asio::ip::tcp::socket socket_;
    asio::strand<asio::io_context::executor_type>& strand_;
    SessionState state_; // Tracking the state
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