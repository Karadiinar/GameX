# Project Structure
```
.
 |-CMakeLists.txt
 |-common
 | |-src
 | |-CMakeLists.txt
 | |-include
 | | |-Protocol.hpp
 | | |-ThreadUtility.hpp
 | | |-Version.hpp
 |-project_context.md
 |-server
 | |-login
 | | |-src
 | | | |-main.cpp
 | | |-CMakeLists.txt
 | | |-include
 | |-CMakeLists.txt
 | |-game
 | | |-src
 | | | |-main.cpp
 | | |-CMakeLists.txt
 | | |-include
 |-client
 | |-src
 | | |-NetworkManager.cpp
 | | |-main.cpp
 | | |-VulkanRenderer.cpp
 | |-CMakeLists.txt
 | |-include
 | | |-GraphicsConfig.hpp
 | | |-VulkanRenderer.hpp
 | | |-NetworkManager.hpp
 | |-shaders
 | | |-vert.spv
 | | |-frag.spv
 | | |-shader.frag
 | | |-shader.vert
 |-cmake
 |-third_party
 |-run.sh
 |-assets
 |-dump.sh
 |-dump_complete.sh
```

# Source and Build Files
## File: ./CMakeLists.txt
```cmake
cmake_minimum_required(VERSION 3.22)
project(RebelMMO CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Find our dependencies
find_package(Vulkan REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(EnTT REQUIRED)

# 2. Add our sub-projects
add_subdirectory(common)
add_subdirectory(server)
add_subdirectory(client)```

## File: ./common/CMakeLists.txt
```cmake
add_library(common INTERFACE)

# This tells other parts of the project where the .hpp files are
target_include_directories(common INTERFACE include)```

## File: ./common/include/Protocol.hpp
```cpp
#pragma once
#include <cstdint>

namespace Rebel {

    enum class Opcode : uint16_t {
        CMSG_PING           = 0x0001,
        SMSG_PONG           = 0x0002,
        CMSG_AUTH_SESSION   = 0x0003,
        SMSG_AUTH_RESPONSE  = 0x0004,
        
        CMSG_CHAT_SAY       = 0x0100,
        SMSG_CHAT_SAY       = 0x0101,
        
        CMSG_PLAYER_MOVE    = 0x0200
    };

#pragma pack(push, 1)
    struct PacketHeader {
        uint16_t size;   // Total size
        uint16_t opcode; // Changed to uint16_t to match enum
    };

    struct MsgLogin {
        char username[32];
        uint32_t version;
    };

    struct MsgRedirect {
        char ip[16];
        uint16_t port;
    };
    struct MsgPlayerMove {
    float x, y, z;
    float yaw; // Essential for knowing which way the dwarf is looking!
};
#pragma pack(pop)

} // namespace Rebel```

## File: ./common/include/ThreadUtility.hpp
```cpp
#pragma once
#include <vector>
#include <thread>
#include <set>
#include <queue>
#include <mutex>
#include <optional>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <pthread.h>
    #include <sched.h>
    #include <fstream>
    #include <string>
#endif

namespace Rebel::Concurrent {

    template<typename T>
    class ThreadSafeQueue {
    public:
        void push(T item) {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(item));
        }

        std::optional<T> try_pop() {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) {
                return std::nullopt;
            }
            T item = std::move(queue_.front());
            queue_.pop();
            return item;
        }

        bool empty() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return queue_.empty();
        }

    private:
        std::queue<T> queue_;
        mutable std::mutex mutex_;
    };

}

namespace Rebel::ThreadUtils {

    inline std::vector<uint32_t> GetPhysicalCoreMap() {
        std::vector<uint32_t> coreIds;
#ifdef _WIN32
        DWORD length = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
        if (length == 0) return coreIds;

        std::vector<uint8_t> buffer(length);
        if (GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer.data(), &length)) {
            auto* ptr = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer.data();
            for (DWORD i = 0; i < length; ) {
                for (int bit = 0; bit < 64; ++bit) {
                    if ((ptr->Processor.GroupMask[0].Mask >> bit) & 1) {
                        coreIds.push_back(bit);
                        break; 
                    }
                }
                i += ptr->Size;
                ptr = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)((uint8_t*)ptr + ptr->Size);
            }
        }
#else
        std::set<int> seen;
        for (uint32_t i = 0; i < std::thread::hardware_concurrency(); ++i) {
            std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/topology/core_id";
            std::ifstream f(path);
            int physId;
            if (f >> physId && seen.find(physId) == seen.end()) {
                coreIds.push_back(i);
                seen.insert(physId);
            }
        }
#endif
        return coreIds;
    }

    inline void SetThreadAffinity(std::thread& t, uint32_t logicalId) {
#ifdef _WIN32
        SetThreadAffinityMask((HANDLE)t.native_handle(), (DWORD_PTR)1 << logicalId);
#else
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(logicalId, &cpuset);
        pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
#endif
    }
}```

## File: ./common/include/Version.hpp
```cpp
#pragma once // This prevents the file from being loaded twice

namespace Rebel {
    // Constant for our game version
    inline constexpr int VERSION_MAJOR = 0;
    inline constexpr int VERSION_MINOR = 1;
}```

## File: ./server/login/src/main.cpp
```cpp
#include <iostream>
#include <asio.hpp>
#include <memory>
#include <vector>
#include "Protocol.hpp"
#include "Version.hpp"

class LoginSession : public std::enable_shared_from_this<LoginSession> {
public:
    LoginSession(asio::ip::tcp::socket socket) : socket_(std::move(socket)) {}

    void start() {
        // Safe way to print remote endpoint
        asio::error_code ec;
        auto endpoint = socket_.remote_endpoint(ec);
        if (!ec) {
            std::cout << "[LOGIN] New auth attempt from: " << endpoint << std::endl;
        }
        read_header();
    }

private:
    void read_header() {
        auto self(shared_from_this());
        asio::async_read(socket_, asio::buffer(&header_, sizeof(Rebel::PacketHeader)),
            [this, self](const asio::error_code& ec, std::size_t) {
                if (!ec) {
                    process_login_request();
                } else {
                    std::cout << "[LOGIN] Client disconnected." << std::endl;
                }
            });
    }

    void process_login_request() {
        Rebel::Opcode opcode = static_cast<Rebel::Opcode>(header_.opcode);

        // Matching the client's CMSG_AUTH_SESSION opcode
        if (opcode == Rebel::Opcode::CMSG_AUTH_SESSION) {
            std::cout << "[LOGIN] Received login request. Authenticating..." << std::endl;
            send_login_response();
        } else {
            std::cerr << "[LOGIN] Unexpected opcode: 0x" << std::hex << (int)opcode << std::dec << std::endl;
        }
    }

    void send_login_response() {
        auto self(shared_from_this());
        
        // Match the client's expectation for SMSG_AUTH_RESPONSE + MsgRedirect
        auto full_packet = std::make_shared<std::vector<uint8_t>>();
        full_packet->resize(sizeof(Rebel::PacketHeader) + sizeof(Rebel::MsgRedirect));

        auto* h = reinterpret_cast<Rebel::PacketHeader*>(full_packet->data());
        h->size = full_packet->size();
        h->opcode = static_cast<uint16_t>(Rebel::Opcode::SMSG_AUTH_RESPONSE);

        auto* r = reinterpret_cast<Rebel::MsgRedirect*>(full_packet->data() + sizeof(Rebel::PacketHeader));
        std::memset(r->ip, 0, 16);
        std::strncpy(r->ip, "127.0.0.1", 15);
        r->port = 12345; // The Game Server Port

        asio::async_write(socket_, asio::buffer(full_packet->data(), full_packet->size()),
            [this, self, full_packet](const asio::error_code& ec, std::size_t) {
                if (!ec) {
                    std::cout << "[LOGIN] Redirect sent. Closing connection." << std::endl;
                    // Give the OS a tiny moment to flush the buffer before hard close
                    socket_.shutdown(asio::ip::tcp::socket::shutdown_both);
                    socket_.close(); 
                }
            });
    }

    asio::ip::tcp::socket socket_;
    Rebel::PacketHeader header_;
};

// FIXED: Explicitly passing the io_context to create the socket
void start_accept(asio::ip::tcp::acceptor& acceptor, asio::io_context& io_context) {
    acceptor.async_accept(
        [&acceptor, &io_context](const asio::error_code& error, asio::ip::tcp::socket socket) {
            if (!error) {
                std::make_shared<LoginSession>(std::move(socket))->start();
            }
            start_accept(acceptor, io_context);
        });
}

int main() {
    std::cout << "--- REBEL LOGIN SERVER STARTING ---" << std::endl;
    
    try {
        asio::io_context io_context;
        
        // Changed to 54321 to match your client's initial connection code
        asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), 54321);
        asio::ip::tcp::acceptor acceptor(io_context, endpoint);

        std::cout << "Login Server listening on port 54321..." << std::endl;
        start_accept(acceptor, io_context);

        io_context.run();
    } catch (std::exception& e) {
        std::cerr << "[LOGIN] Exception: " << e.what() << std::endl;
    }

    return 0;
}```

## File: ./server/login/CMakeLists.txt
```cmake
find_package(asio CONFIG REQUIRED)

add_executable(LoginServer src/main.cpp)

target_include_directories(LoginServer PRIVATE include)

# Link the common headers and asio
target_link_libraries(LoginServer 
    PRIVATE 
        common 
        asio::asio
)

target_compile_features(LoginServer PRIVATE cxx_std_20)```

## File: ./server/CMakeLists.txt
```cmake
add_subdirectory(login)
add_subdirectory(game)```

## File: ./server/game/src/main.cpp
```cpp
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
}```

## File: ./server/game/CMakeLists.txt
```cmake
# server/game/CMakeLists.txt

# 1. Find the ASIO package (vcpkg provides this)
find_package(asio CONFIG REQUIRED)

set(GAME_SERVER_SOURCES
    src/main.cpp
)

add_executable(GameServer ${GAME_SERVER_SOURCES})

target_include_directories(GameServer PRIVATE include)

# 2. Link asio AND your common interface
# asio::asio is the standard target name for the header-only version
target_link_libraries(GameServer 
    PRIVATE 
        common 
        asio::asio
)

target_compile_features(GameServer PRIVATE cxx_std_20)```

## File: ./client/src/NetworkManager.cpp
```cpp
#include "NetworkManager.hpp"
#include <iostream>

NetworkManager::NetworkManager()
    : io_context_(),
      strand_(asio::make_strand(io_context_)),
      work_guard_(std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(asio::make_work_guard(io_context_)))
{
    // The network thread is exclusively responsible for driving ASIO
    network_thread_ = std::thread([this]()
                                  { io_context_.run(); });
}

NetworkManager::~NetworkManager()
{
    disconnect();
    work_guard_.reset(); // Allow io_context_.run() to exit when work is done
    io_context_.stop();
    if (network_thread_.joinable())
    {
        network_thread_.join();
    }
}

void NetworkManager::connect(const std::string &host, const std::string &port)
{
    // Post the connection attempt to the strand so it executes safely on the network thread
    asio::post(strand_, [this, host, port]()
               {
        try {
            asio::ip::tcp::resolver resolver(io_context_);
            auto endpoints = resolver.resolve(host, port);
            
            socket_.emplace(io_context_);
            
            asio::async_connect(*socket_, endpoints, 
                asio::bind_executor(strand_, [this](std::error_code ec, asio::ip::tcp::endpoint) {
                    if (!ec) {
                        std::cout << "[Network] Connected to server.\n";
                        // 1. Prepare the Auth Payload
            Rebel::MsgLogin loginData;
            strncpy(loginData.username, "Karadiinar", 32);
            loginData.version = 1; // From your Version.hpp

            // 2. Prepare the Header
            Rebel::PacketHeader header;
            header.size = sizeof(Rebel::PacketHeader) + sizeof(Rebel::MsgLogin);
            header.opcode = static_cast<uint16_t>(Rebel::Opcode::CMSG_AUTH_SESSION);

            // 3. Send the Auth Packet immediately
            sendPacket(header, &loginData, sizeof(Rebel::MsgLogin));

            // 4. Now start listening for the server's response
            startRead();
                    } else {
                        std::cerr << "[Network] Connect error: " << ec.message() << "\n";
                    }
                }));
        } catch (const std::exception& e) {
            std::cerr << "[Network] Exception during connect: " << e.what() << "\n";
        } });
}

void NetworkManager::disconnect()
{
    asio::post(strand_, [this]()
               {
        if (socket_ && socket_->is_open()) {
            std::error_code ec;
            socket_->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            socket_->close(ec);
            socket_.reset();
            std::cout << "[Network] Disconnected.\n";
        } });
}

void NetworkManager::sendPacket(const Rebel::PacketHeader &header, const void *payload, std::size_t payloadSize)
{
    // We must copy the data before posting, because the caller (Logic Thread)
    // might destroy or modify the original buffers before the async write happens.
    auto buffer = std::make_shared<std::vector<uint8_t>>(sizeof(Rebel::PacketHeader) + payloadSize);
    std::memcpy(buffer->data(), &header, sizeof(Rebel::PacketHeader));
    if (payloadSize > 0 && payload != nullptr)
    {
        std::memcpy(buffer->data() + sizeof(Rebel::PacketHeader), payload, payloadSize);
    }

    // Post the write operation to the strand
    asio::post(strand_, [this, buffer]()
               {
        if (!socket_ || !socket_->is_open()) return;

        asio::async_write(*socket_, asio::buffer(*buffer),
            asio::bind_executor(strand_, [buffer](std::error_code ec, std::size_t /*length*/) {
                if (ec) {
                    std::cerr << "[Network] Write error: " << ec.message() << "\n";
                }
            })); });
}

void NetworkManager::startRead() {
    if (!socket_ || !socket_->is_open()) return;

    asio::async_read(*socket_, asio::buffer(&incoming_header_, sizeof(Rebel::PacketHeader)),
        asio::bind_executor(strand_, [this](std::error_code ec, std::size_t /*length*/) {
            if (!ec) {
                if (incoming_header_.size > sizeof(Rebel::PacketHeader)) {
                    // Calculate payload size (Total size - Header size)
                    readPayload(incoming_header_.size - sizeof(Rebel::PacketHeader));
                } else {
                    // No payload: Push header immediately to the Logic Thread queue
                    Rebel::InboundPacket pkg;
                    pkg.header = incoming_header_;
                    inbound_queue_.push(std::move(pkg));
                    
                    startRead(); // Wait for next packet
                }
            } else {
                if (ec != asio::error::operation_aborted) {
                    std::cerr << "[Network] Read header error: " << ec.message() << "\n";
                    disconnect();
                }
            }
        }));
}

void NetworkManager::readPayload(uint16_t payloadSize) {
    incoming_payload_.resize(payloadSize);

    asio::async_read(*socket_, asio::buffer(incoming_payload_.data(), payloadSize),
        asio::bind_executor(strand_, [this](std::error_code ec, std::size_t /*length*/) {
            if (!ec) {
                // Packet complete: Push to the Logic Thread queue
                Rebel::InboundPacket pkg;
                pkg.header = incoming_header_;
                pkg.payload = incoming_payload_;

                inbound_queue_.push(std::move(pkg));
                
                startRead(); // Wait for next packet
            } else {
                std::cerr << "[Network] Read payload error: " << ec.message() << "\n";
                disconnect();
            }
        }));
}```

## File: ./client/src/main.cpp
```cpp
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <algorithm> 
#include <entt/entt.hpp>

// Must be included before custom graphics headers to ensure Vulkan macros are set
#include <GLFW/glfw3.h>

#include "NetworkManager.hpp"
#include "VulkanRenderer.hpp"
#include "ThreadUtility.hpp"
#include <mutex>

// Global shutdown flag to synchronize thread termination
std::atomic<bool> g_Running{ true };
VulkanRenderer* g_RendererPtr = nullptr;

struct Transform {
    float x, y, z;
    float yaw;
};

struct LocalPlayerTag {};

void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (key == GLFW_KEY_F11 && action == GLFW_PRESS) {
        if (g_RendererPtr) {
            g_RendererPtr->toggleFullscreen(window);
        }
    }
}

/**
 * Logic Thread: Responsible for game state evolution, physics, and packet processing.
 */
void LogicThreadEntry(NetworkManager* network, SharedRenderState* renderState) {
    entt::registry client_registry;       // <-- The Client's World Data
    entt::entity local_player = entt::null; // <-- Handle to our specific character
    bool inWorld = false;

    const std::chrono::microseconds TICK_TIME(1000000 / 64);
    std::cout << "[Logic] Engine tick thread started.\n";

    while (g_Running) {
        auto start = std::chrono::steady_clock::now();

        // Drain the mailbox
        while (auto packetOpt = network->getPacketQueue().try_pop()) {
            auto& packet = *packetOpt;

            if (packet.header.opcode == static_cast<uint16_t>(Rebel::Opcode::SMSG_AUTH_RESPONSE)) {
                
                if (packet.payload.size() >= sizeof(Rebel::MsgRedirect)) {
                    auto* redirect = reinterpret_cast<Rebel::MsgRedirect*>(packet.payload.data());
                    std::cout << "[Logic] Redirecting to Game Server at " << redirect->ip << ":" << redirect->port << "\n";
                    
                    network->disconnect();
                    network->connect(redirect->ip, std::to_string(redirect->port));
                } 
                else {
                    std::cout << "[Logic] Successfully authenticated with Game Server. Entering world...\n";
                    inWorld = true; 

                    // --- SPAWN THE LOCAL ENTITY ---
                    local_player = client_registry.create();
                    client_registry.emplace<Transform>(local_player, 0.0f, 0.0f, 0.0f, 0.0f);
                    client_registry.emplace<LocalPlayerTag>(local_player);
                    std::cout << "[Logic] Local player entity spawned in client ECS.\n";
                }
            }
        }

        // --- UPDATE ECS AND SEND NETWORK PACKET ---
        if (inWorld && client_registry.valid(local_player)) {
            auto& transform = client_registry.get<Transform>(local_player);
            transform.x += 0.05f; 

            // 1. UPDATE THE RENDER BRIDGE
            {
                std::lock_guard<std::mutex> lock(renderState->mtx);
                renderState->player_x = transform.x;
                renderState->player_y = transform.y;
                renderState->player_z = transform.z;
            }

            // Build the network packet using the ECS data
            Rebel::MsgPlayerMove moveData;
            moveData.x = transform.x;
            moveData.y = transform.y;
            moveData.z = transform.z;
            moveData.yaw = transform.yaw;

            Rebel::PacketHeader head;
            head.opcode = static_cast<uint16_t>(Rebel::Opcode::CMSG_PLAYER_MOVE);
            head.size = sizeof(Rebel::PacketHeader) + sizeof(Rebel::MsgPlayerMove);

            network->sendPacket(head, &moveData, sizeof(Rebel::MsgPlayerMove));
        }

        auto end = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        if (elapsed < TICK_TIME) {
            std::this_thread::sleep_for(TICK_TIME - elapsed);
        }
    }
    std::cout << "[Logic] Engine tick thread exiting...\n";
}

int main() {
    std::cout << "--- Rebel MMO Project (Client) ---\n";

    // --- 1. HARDWARE DISCOVERY ---
    auto coreMap = Rebel::ThreadUtils::GetPhysicalCoreMap();
    std::cout << "[Main] Discovered " << coreMap.size() << " physical cores.\n";

    // --- 2. NETWORK SUBSYSTEM ---
    NetworkManager network;
    if (coreMap.size() > 1) {
        std::cout << "[Main] Pinning Network subsystem to Core " << coreMap[1] << "\n";
        Rebel::ThreadUtils::SetThreadAffinity(network.getThread(), coreMap[1]);
    }
    
    network.connect("127.0.0.1", "54321"); 

    // --- INSTANTIATE THE BRIDGE ---
    SharedRenderState sharedRenderState;

    // --- 3. LOGIC SUBSYSTEM ---
    std::thread logicThread(LogicThreadEntry, &network, &sharedRenderState);
    if (coreMap.size() > 2) {
        std::cout << "[Main] Pinning Logic subsystem to Core " << coreMap[2] << "\n";
        Rebel::ThreadUtils::SetThreadAffinity(logicThread, coreMap[2]);
    }

    // --- 4. GRAPHICS SUBSYSTEM ---
    try {
        // FIX 1: Fire up GLFW before calling window manipulation functions
        if (!glfwInit()) {
            throw std::runtime_error("[Graphics] Failed to initialize GLFW!");
        }
        
        // Setup default configuration settings
        GraphicsConfig userGraphicsSettings;
        userGraphicsSettings.presentMode = PresentModeSetting::TripleBuffer; 
        userGraphicsSettings.windowWidth = 1280;
        userGraphicsSettings.windowHeight = 720;

        // FIX 2: Flush hints and explicitly declare Vulkan API support before generating the container
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE); 

        // Create the window context
        GLFWwindow* window = glfwCreateWindow(
            userGraphicsSettings.windowWidth, 
            userGraphicsSettings.windowHeight, 
            "Rebel Client", 
            nullptr, 
            nullptr
        );

        if (!window) {
            throw std::runtime_error("[Graphics] Failed to create GLFW window!");
        }

        // Initialize using the default constructor
        VulkanRenderer renderer;
        
        // Pass the window context and config to the initialization pipeline
        renderer.init(window, userGraphicsSettings);

        // Assign global/local pointer mappings for our key events dispatching
        g_RendererPtr = &renderer;
        glfwSetKeyCallback(window, key_callback);

        // Map window user pointer context directly to instance address
        glfwSetWindowUserPointer(window, &renderer);
        
        // Set up the window resize callback lambda
        glfwSetFramebufferSizeCallback(window, [](GLFWwindow* win, int width, int height) {
            auto* rend = reinterpret_cast<VulkanRenderer*>(glfwGetWindowUserPointer(win));
            if (rend) {
                rend->framebufferResizeCallback();
            }
        });

        // --- UNIFIED MAIN RENDER LOOP ---
        while (!renderer.shouldClose()) {
            renderer.pollEvents(); // Processes keystrokes & dispatches the callback functions
            renderer.drawFrame(&sharedRenderState);
        }

        // --- 5. CLEAN SHUTDOWN SEQUENCE ---
        std::cout << "\n[Main] Shutdown signal received. Terminating subsystems...\n";
        g_Running = false;
        
        if (logicThread.joinable()) {
            logicThread.join();
        }

        renderer.cleanup();
        glfwDestroyWindow(window);
        glfwTerminate();
    }
    catch (const std::exception& e) {
        std::cerr << "\n[Fatal Error] " << e.what() << std::endl;
        g_Running = false;
        if (logicThread.joinable()) {
            logicThread.join();
        }
        glfwTerminate(); // Keep environment states cleanly balanced on failures
        return -1;
    }

    std::cout << "[Main] Clean shutdown complete.\n";
    return 0;
}```

## File: ./client/src/VulkanRenderer.cpp
```cpp
#include "VulkanRenderer.hpp"
#include "GraphicsConfig.hpp"
#include <stdexcept>
#include <vector>
#include <iostream>
#include <fstream>
#include <string> // Added for std::string
#include <set>    // Added for std::set
#include <algorithm>
#include <limits>
#include <cstdint>
#include <mutex>

void VulkanRenderer::init(GLFWwindow* window, const GraphicsConfig& config) {
    window_ = window;
    activeConfig_ = config;

    // --- 1. CREATE INSTANCE ---
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Rebel Client";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "Rebel Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    
    createInfo.enabledExtensionCount = glfwExtensionCount;
    createInfo.ppEnabledExtensionNames = glfwExtensions;
    const char* validationLayers[] = { "VK_LAYER_KHRONOS_validation" };
createInfo.enabledLayerCount = 1;
createInfo.ppEnabledLayerNames = validationLayers; 

    if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS) {
        throw std::runtime_error("[VULKAN] Failed to create instance!");
    }

    // --- 2. CREATE SURFACE ---
    if (glfwCreateWindowSurface(instance_, window_, nullptr, &surface_) != VK_SUCCESS) {
        throw std::runtime_error("[VULKAN] Failed to create window surface!");
    }

    // --- 3. PICK HARDWARE ---
    pickPhysicalDevice();
    createLogicalDevice();
    createSwapChain();
    createImageViews();
    createRenderPass();
    createGraphicsPipeline();
    createFramebuffers();
    createCommandPool();
    createCommandBuffers();
    createSyncObjects();
    
    std::cout << "[VULKAN] Full rendering pipeline initialized." << std::endl;
}

void VulkanRenderer::createLogicalDevice() {
    QueueFamilyIndices indices = findQueueFamilies(physicalDevice_);

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    std::set<uint32_t> uniqueQueueFamilies = {indices.graphicsFamily.value(), indices.presentFamily.value()};

    float queuePriority = 1.0f;
    for (uint32_t queueFamily : uniqueQueueFamilies) {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    // Specifying the device features we'll need (can leave empty for now)
    VkPhysicalDeviceFeatures deviceFeatures{};

    // Packing it all into the main creation struct
    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();

    createInfo.pEnabledFeatures = &deviceFeatures;

    // Enable the swapchain extension you defined in your header
    createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    // No validation layers explicitly enabled here for the device, 
    // modern Vulkan handles validation at the instance level anyway.
    createInfo.enabledLayerCount = 0;

    // 1. The moment of truth: actually creating the device!
    if (vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_) != VK_SUCCESS) {
        throw std::runtime_error("[Vulkan] Failed to create logical device!");
    }

    // 2. Retrieve the queue handles so we can actually submit command buffers later
    vkGetDeviceQueue(device_, indices.graphicsFamily.value(), 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, indices.presentFamily.value(), 0, &presentQueue_);
}

void VulkanRenderer::createSwapChain() {
    SwapChainSupportDetails swapChainSupport = querySwapChainSupport(physicalDevice_);

    VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(swapChainSupport.formats);
    VkPresentModeKHR presentMode = chooseSwapPresentMode(swapChainSupport.presentModes);
    VkExtent2D extent = chooseSwapExtent(swapChainSupport.capabilities);

    uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
    if (swapChainSupport.capabilities.maxImageCount > 0 && imageCount > swapChainSupport.capabilities.maxImageCount) {
        imageCount = swapChainSupport.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface_;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    QueueFamilyIndices indices = findQueueFamilies(physicalDevice_);
    uint32_t queueFamilyIndices[] = {indices.graphicsFamily.value(), indices.presentFamily.value()};

    if (indices.graphicsFamily != indices.presentFamily) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapChain_) != VK_SUCCESS) {
        throw std::runtime_error("[VULKAN] Failed to create swap chain!");
    }

    vkGetSwapchainImagesKHR(device_, swapChain_, &imageCount, nullptr);
    swapChainImages_.resize(imageCount);
    vkGetSwapchainImagesKHR(device_, swapChain_, &imageCount, swapChainImages_.data());

    swapChainImageFormat_ = surfaceFormat.format;
    swapChainExtent_ = extent;
}

void VulkanRenderer::createImageViews() {
    swapChainImageViews_.resize(swapChainImages_.size());

    for (size_t i = 0; i < swapChainImages_.size(); i++) {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = swapChainImages_[i];
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = swapChainImageFormat_;
        createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device_, &createInfo, nullptr, &swapChainImageViews_[i]) != VK_SUCCESS) {
            throw std::runtime_error("[VULKAN] Failed to create image views!");
        }
    }
}

void VulkanRenderer::createRenderPass() {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = swapChainImageFormat_;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(device_, &renderPassInfo, nullptr, &renderPass_) != VK_SUCCESS) {
        throw std::runtime_error("[VULKAN] Failed to create render pass!");
    }
}

static std::vector<uint32_t> readFile(const std::string& filename) { // Changed return type to uint32_t
    // Ensure your working directory has access to this relative path!
    std::ifstream file("shaders/" + filename, std::ios::ate | std::ios::binary); 
    
    if (!file.is_open()) {
        throw std::runtime_error("[VULKAN] Failed to open shader file: shaders/" + filename);
    }

    size_t fileSize = (size_t)file.tellg();
    
    // SPIR-V files must be a multiple of 4 bytes
    if (fileSize % 4 != 0) {
        throw std::runtime_error("[VULKAN] Shader file size is not a multiple of 4 bytes: " + filename);
    }

    std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t)); // Corrected size calculation

    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), fileSize);
    file.close();

    return buffer;
}

void VulkanRenderer::createGraphicsPipeline() {
    auto vertShaderCode = readFile("vert.spv");
    auto fragShaderCode = readFile("frag.spv");

    VkShaderModule vertShaderModule = createShaderModule(vertShaderCode);
    VkShaderModule fragShaderModule = createShaderModule(fragShaderCode);

    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    std::vector<VkDynamicState> dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

// Define the push constant range telling Vulkan what the vertex shader expects
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT; // Targeted shader stage
    pushConstantRange.offset = 0;                             // Zero offset
    pushConstantRange.size = sizeof(float);                   // Pushing exactly one 32-bit float

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 0;
    pipelineLayoutInfo.pSetLayouts = nullptr;
    pipelineLayoutInfo.pushConstantRangeCount = 1;             // Set this to 1
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange; // Bind the range data

    if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        throw std::runtime_error("[VULKAN] Failed to create pipeline layout!");
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout_;
    pipelineInfo.renderPass = renderPass_;
    pipelineInfo.subpass = 0;

    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline_) != VK_SUCCESS) {
        throw std::runtime_error("[VULKAN] Failed to create graphics pipeline!");
    }

    vkDestroyShaderModule(device_, fragShaderModule, nullptr);
    vkDestroyShaderModule(device_, vertShaderModule, nullptr);
}

void VulkanRenderer::createFramebuffers() {
    swapChainFramebuffers_.resize(swapChainImageViews_.size());

    for (size_t i = 0; i < swapChainImageViews_.size(); i++) {
        VkImageView attachments[] = { swapChainImageViews_[i] };

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass_;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = swapChainExtent_.width;
        framebufferInfo.height = swapChainExtent_.height;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &swapChainFramebuffers_[i]) != VK_SUCCESS) {
            throw std::runtime_error("[VULKAN] Failed to create framebuffer!");
        }
    }
}

void VulkanRenderer::createCommandPool() {
    QueueFamilyIndices queueFamilyIndices = findQueueFamilies(physicalDevice_);

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();

    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_) != VK_SUCCESS) {
        throw std::runtime_error("[VULKAN] Failed to create command pool!");
    }
}

void VulkanRenderer::createCommandBuffers() {
    commandBuffers_.resize(MAX_FRAMES_IN_FLIGHT);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = (uint32_t)commandBuffers_.size();

    if (vkAllocateCommandBuffers(device_, &allocInfo, commandBuffers_.data()) != VK_SUCCESS) {
        throw std::runtime_error("[VULKAN] Failed to allocate command buffers!");
    }
}

void VulkanRenderer::recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex, float playerX) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("[VULKAN] Failed to begin recording command buffer!");
    }

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass_;
    renderPassInfo.framebuffer = swapChainFramebuffers_[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = swapChainExtent_;

    VkClearValue clearColor = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearColor;

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    
    // 1. Bind your pipeline
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline_);

    // 2. Set your dynamic states
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)swapChainExtent_.width;
    viewport.height = (float)swapChainExtent_.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapChainExtent_;
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    // =================================================================
    // 3. THE MISSING LINK: Push the X coordinate to the vertex shader!
    // =================================================================
    vkCmdPushConstants(
        commandBuffer,
        pipelineLayout_,             // Your compiled pipeline layout
        VK_SHADER_STAGE_VERTEX_BIT,  // Target the vertex shader stage
        0,                           // Offset
        sizeof(float),               // Size of data
        &playerX                     // Pointer to our local float data
    );
    // =================================================================

    // 4. Issue the draw call
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    
    vkCmdEndRenderPass(commandBuffer);

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("[VULKAN] Failed to record command buffer!");
    }
}

void VulkanRenderer::createSyncObjects() {
    imageAvailableSemaphores_.resize(MAX_FRAMES_IN_FLIGHT);
    renderFinishedSemaphores_.resize(MAX_FRAMES_IN_FLIGHT);
    inFlightFences_.resize(MAX_FRAMES_IN_FLIGHT);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailableSemaphores_[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderFinishedSemaphores_[i]) != VK_SUCCESS ||
            vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[i]) != VK_SUCCESS) {
            throw std::runtime_error("[VULKAN] Failed to create synchronization objects!");
        }
    }
}

void VulkanRenderer::pickPhysicalDevice() {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);

    if (deviceCount == 0) {
        throw std::runtime_error("[VULKAN] Failed to find GPUs with Vulkan support!");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());

    for (const auto& device : devices) {
        if (isDeviceSuitable(device)) {
            physicalDevice_ = device;
            break;
        }
    }

    if (physicalDevice_ == VK_NULL_HANDLE) {
        throw std::runtime_error("[VULKAN] Failed to find a suitable GPU!");
    }

    VkPhysicalDeviceProperties deviceProperties;
    vkGetPhysicalDeviceProperties(physicalDevice_, &deviceProperties);
    std::cout << "[VULKAN] Locked onto GPU: " << deviceProperties.deviceName << std::endl;
}

bool VulkanRenderer::isDeviceSuitable(VkPhysicalDevice device) {
    QueueFamilyIndices indices = findQueueFamilies(device);
    // Check for required device extension support (e.g., swapchain)
    bool extensionsSupported = checkDeviceExtensionSupport(device);

    // Check for adequate swap chain support (formats and present modes)
    bool swapChainAdequate = false;
    if (extensionsSupported) { // Only query if extensions are supported
        SwapChainSupportDetails swapChainSupport = querySwapChainSupport(device);
        swapChainAdequate = !swapChainSupport.formats.empty() && !swapChainSupport.presentModes.empty();
    }
    return indices.isComplete() && extensionsSupported && swapChainAdequate;
}

QueueFamilyIndices VulkanRenderer::findQueueFamilies(VkPhysicalDevice device) {
    QueueFamilyIndices indices;

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

    int i = 0;
    for (const auto& queueFamily : queueFamilies) {
        if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            indices.graphicsFamily = i;
        }

        VkBool32 presentSupport = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &presentSupport);

        if (presentSupport) {
            indices.presentFamily = i;
        }

        if (indices.isComplete()) break;
        i++;
    }

    return indices;
}

VkShaderModule VulkanRenderer::createShaderModule(const std::vector<uint32_t>& code) {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    
    // Fix: Multiply by 4 (sizeof(uint32_t)) to get the size in bytes!
    createInfo.codeSize = code.size() * sizeof(uint32_t); 
    createInfo.pCode = code.data();

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(device_, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("[VULKAN] Failed to create shader module!");
    }

    return shaderModule;
}

bool VulkanRenderer::checkDeviceExtensionSupport(VkPhysicalDevice device) {
    uint32_t extensionCount;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

    std::set<std::string> requiredExtensions(deviceExtensions.begin(), deviceExtensions.end());

    for (const auto& extension : availableExtensions) {
        std::string extName(extension.extensionName);

        if (requiredExtensions.count(extName)) {
            std::cout << "  Extension: " << extName << " (SUPPORTED)\n";
            requiredExtensions.erase(extName);
        }
    }
    return requiredExtensions.empty();
}

SwapChainSupportDetails VulkanRenderer::querySwapChainSupport(VkPhysicalDevice device) {
    SwapChainSupportDetails details;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_, &details.capabilities);
    
    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, nullptr);
    if (formatCount != 0) {
        details.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, details.formats.data());
    }
    
    uint32_t presentModeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount, nullptr);
    if (presentModeCount != 0) {
        details.presentModes.resize(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount, details.presentModes.data());
    }
    return details;
}

VkSurfaceFormatKHR VulkanRenderer::chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats) {
    for (const auto& availableFormat : availableFormats) {
        if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB && availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return availableFormat;
        }
    }
    return availableFormats[0];
}

VkPresentModeKHR VulkanRenderer::chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes) {
    
    // --- 1. USER SETTING: TRIPLE BUFFERING (MAILBOX) ---
    if (activeConfig_.presentMode == PresentModeSetting::TripleBuffer) {
        for (const auto& mode : availablePresentModes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                std::cout << "[VULKAN CONFIG] Present Mode: Triple Buffering (MAILBOX).\n";
                return mode;
            }
        }
    }

    // --- 2. USER SETTING: UNCAPPED PERFORMANCE (IMMEDIATE) ---
    if (activeConfig_.presentMode == PresentModeSetting::Immediate) {
        for (const auto& mode : availablePresentModes) {
            if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
                std::cout << "[VULKAN CONFIG] Present Mode: Uncapped Performance (IMMEDIATE).\n";
                return mode;
            }
        }
    }

    // --- 3. FALLBACK / DEFAULT VSYNC (FIFO) ---
    // Enforced if user selected VSync OR if their hardware layout rejected MAILBOX/IMMEDIATE
    std::cout << "[VULKAN CONFIG] Present Mode: VSync Enabled (FIFO).\n";
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanRenderer::chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) {
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    } else {
        int width, height;
        glfwGetFramebufferSize(window_, &width, &height);
        VkExtent2D actualExtent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
        actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        return actualExtent;
    }
}

void VulkanRenderer::drawFrame(SharedRenderState* renderState) {
    // 1. Critical Safety Check!
    if (device_ == VK_NULL_HANDLE) {
        return; 
    }

    // 2. Handle an explicit resize flag before acquiring an image (e.g., immediate F11 toggle)
    if (framebufferResized_) {
        int width = 0, height = 0;
        glfwGetFramebufferSize(window_, &width, &height);
        
        // Handle window minimization (pause rendering if window is minimized)
        while (width == 0 || height == 0) {
            glfwGetFramebufferSize(window_, &width, &height);
            glfwWaitEvents();
        }

        vkDeviceWaitIdle(device_);
        recreateSwapchain(window_); // Rebuilds swapchain, image views, render pass, framebuffers, etc.
        framebufferResized_ = false;
        return; // Skip this frame and try again with the new layout
    }

    // Wait for the previous frame's fence
    vkWaitForFences(device_, 1, &inFlightFences_[currentFrame_], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(
        device_, 
        swapChain_, 
        UINT64_MAX, 
        imageAvailableSemaphores_[currentFrame_], 
        VK_NULL_HANDLE, 
        &imageIndex
    );

    // If the swapchain became out of date right during acquisition (e.g., screen configuration change)
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        vkDeviceWaitIdle(device_);
        recreateSwapchain(window_);
        framebufferResized_ = false;
        return;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("[VULKAN] Failed to acquire swap chain image!");
    }

    // Only reset the fence if we are actually succeeding and committing to work
    vkResetFences(device_, 1, &inFlightFences_[currentFrame_]);
    vkResetCommandBuffer(commandBuffers_[currentFrame_], 0);

    // =======================================================
    // 3. THE RENDER BRIDGE: Safely grab the player's X position
    // =======================================================
    float current_x = 0.0f;
    if (renderState) {
        std::lock_guard<std::mutex> lock(renderState->mtx);
        current_x = renderState->player_x; // Fast read and unlock!
    }

    // 4. Pass the extracted float down into the command buffer recorder!
    recordCommandBuffer(commandBuffers_[currentFrame_], imageIndex, current_x);

    // =======================================================

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    
    VkSemaphore waitSemaphores[] = {imageAvailableSemaphores_[currentFrame_]};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffers_[currentFrame_];
    
    VkSemaphore signalSemaphores[] = {renderFinishedSemaphores_[currentFrame_]};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (vkQueueSubmit(graphicsQueue_, 1, &submitInfo, inFlightFences_[currentFrame_]) != VK_SUCCESS) {
        throw std::runtime_error("[VULKAN] Failed to submit draw command buffer!");
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    
    VkSwapchainKHR swapChains[] = {swapChain_};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapChains;
    presentInfo.pImageIndices = &imageIndex;

    result = vkQueuePresentKHR(presentQueue_, &presentInfo);

    // Handle out-of-date or suboptimal results on presentation
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || framebufferResized_) {
        vkDeviceWaitIdle(device_);
        recreateSwapchain(window_);
        framebufferResized_ = false;
    } else if (result != VK_SUCCESS) {
        throw std::runtime_error("[VULKAN] Failed to present swap chain image!");
    }

    currentFrame_ = (currentFrame_ + 1) % MAX_FRAMES_IN_FLIGHT;
}

void VulkanRenderer::cleanup() {
    if (device_ == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(device_);

    // Loop through actual vector sizes, not MAX_FRAMES_IN_FLIGHT
    for (auto semaphore : renderFinishedSemaphores_) {
        if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device_, semaphore, nullptr);
    }
    for (auto semaphore : imageAvailableSemaphores_) {
        if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device_, semaphore, nullptr);
    }
    for (auto fence : inFlightFences_) {
        if (fence != VK_NULL_HANDLE) vkDestroyFence(device_, fence, nullptr);
    }

    if (commandPool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, commandPool_, nullptr);

    for (auto framebuffer : swapChainFramebuffers_) {
        if (framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }

    if (graphicsPipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, graphicsPipeline_, nullptr);
    if (pipelineLayout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (renderPass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_, renderPass_, nullptr);

    for (auto imageView : swapChainImageViews_) {
        if (imageView != VK_NULL_HANDLE) vkDestroyImageView(device_, imageView, nullptr);
    }

    if (swapChain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_, swapChain_, nullptr);
    
    vkDestroyDevice(device_, nullptr);

    if (instance_ != VK_NULL_HANDLE) {
        if (surface_ != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        vkDestroyInstance(instance_, nullptr);
    }
    std::cout << "[VULKAN] Cleaned up successfully." << std::endl;
}
void VulkanRenderer::toggleFullscreen(GLFWwindow* window) {
    windowState_.isFullscreen = !windowState_.isFullscreen;

    if (windowState_.isFullscreen) {
        // 1. Save current window position and size before making the jump
        glfwGetWindowPos(window, &windowState_.windowedX, &windowState_.windowedY);
        glfwGetWindowSize(window, &windowState_.windowedWidth, &windowState_.windowedHeight);

        // 2. Determine which monitor the window is currently on
        GLFWmonitor* monitor = glfwGetWindowMonitor(window);
        if (!monitor) {
            // If windowed, find the monitor that contains the top-left corner of our window
            int monitorCount;
            GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
            monitor = monitors[0]; // fallback default

            int wx, wy;
            glfwGetWindowPos(window, &wx, &wy);

            for (int i = 0; i < monitorCount; ++i) {
                int mx, my, mw, mh;
                glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
                if (wx >= mx && wx < mx + mw && wy >= my && wy < my + mh) {
                    monitor = monitors[i];
                    break;
                }
            }
        }

        // 3. Get the native resolution of that monitor (3440x1440, 1920x1080, etc.)
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);

        // 4. Set to true exclusive fullscreen
        // Pass the monitor pointer, target width, target height, and target refresh rate
        glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        std::cout << "[Graphics] Switched to Fullscreen: " << mode->width << "x" << mode->height << " @" << mode->refreshRate << "Hz\n";
    } else {
        // Restore windowed state back to 720p at its previous position
        glfwSetWindowMonitor(window, nullptr, 
                             windowState_.windowedX, windowState_.windowedY, 
                             windowState_.windowedWidth, windowState_.windowedHeight, 0);
        std::cout << "[Graphics] Restored to Windowed Mode: " << windowState_.windowedWidth << "x" << windowState_.windowedHeight << "\n";
    }

    // Force a swapchain recreation because our frame boundaries completely changed
    framebufferResized_ = true; 
}
void VulkanRenderer::cleanupSwapChain() {
    // 1. Tear down old framebuffers bound to previous resolution extents
    for (auto framebuffer : swapChainFramebuffers_) {
        if (framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device_, framebuffer, nullptr);
        }
    }
    swapChainFramebuffers_.clear();

    // 2. Clean up image views wrapping the old swapchain images
    for (auto imageView : swapChainImageViews_) {
        if (imageView != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, imageView, nullptr);
        }
    }
    swapChainImageViews_.clear();

    // 3. Destroy the actual VkSwapchainKHR handle
    if (swapChain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapChain_, nullptr);
        swapChain_ = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::recreateSwapchain(GLFWwindow* window) {
    window_ = window;

    int width = 0, height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    
    // Handle window minimization safely (pause execution loop if window surface collapses)
    while (width == 0 || height == 0) {
        glfwGetFramebufferSize(window_, &width, &height);
        glfwWaitEvents();
    }

    // Wait until the graphics queues are completely drained before swapping memory resources
    vkDeviceWaitIdle(device_);

    // Clean up old surface memory objects
    cleanupSwapChain();

    // Rebuild the surface layers to match the new screen metrics (1080p, 3440x1440, etc.)
    createSwapChain();      // Fetches updated resolution metrics internally via chooseSwapExtent
    createImageViews();     // Wraps new swapchain handles
    createFramebuffers();   // Reconstructs render target arrays
    
    std::cout << "[Vulkan] Swapchain safely recreated at resolution: " << width << "x" << height << "\n";
}
```

## File: ./client/CMakeLists.txt
```cmake
# Find the necessary system threading and Asio
find_package(Threads REQUIRED)

add_executable(client 
    src/main.cpp
    src/VulkanRenderer.cpp
    src/NetworkManager.cpp
)

target_include_directories(client PRIVATE 
    include 
    ${CMAKE_SOURCE_DIR}/common/include
)

# Link to our shared code, graphics, and networking
target_link_libraries(client PRIVATE 
    common 
    Vulkan::Vulkan 
    glfw 
    glm::glm
    Threads::Threads
    asio  # Ensure your system's asio find_package or alias is defined
)

# Shader copy command remains the same
add_custom_command(
    TARGET client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders
    $<TARGET_FILE_DIR:client>/shaders
)```

## File: ./client/include/GraphicsConfig.hpp
```cpp
#pragma once
#include <vulkan/vulkan.h>

enum class PresentModeSetting {
    Immediate = 0, // Uncapped (8000 FPS room-heater mode)
    VSync,         // Frame-capped, synchronized (FIFO)
    TripleBuffer   // Low-latency, frame-capped fallback (MAILBOX)
};

struct GraphicsConfig {
    // Present mode configuration
    PresentModeSetting presentMode = PresentModeSetting::TripleBuffer;
    
    // Window settings
    int windowWidth = 1280;
    int windowHeight = 720;
    bool fullscreen = false;
    
    // Future settings placeholders
    bool enableValidationLayers = true;
    bool shadowQuality = true;
};```

## File: ./client/include/VulkanRenderer.hpp
```cpp
#pragma once
#include "GraphicsConfig.hpp"
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <optional>
#include <vector>
#include <mutex>

struct WindowState {
    bool isFullscreen = false;
    int windowedX = 100;
    int windowedY = 100;
    int windowedWidth = 1280;   // Default 720p
    int windowedHeight = 720;
};

struct SharedRenderState {
    std::mutex mtx;
    float player_x = 0.0f;
    float player_y = 0.0f;
    float player_z = 0.0f;
};

struct SharedRenderState;

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

    bool isComplete() {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

class VulkanRenderer {
public:
void toggleFullscreen(GLFWwindow* window);
    void framebufferResizeCallback() { framebufferResized_ = true; }
    void init(GLFWwindow* window, const GraphicsConfig& config);
    void cleanup();
    
    // Renamed from draw() to match what main.cpp is calling
    void drawFrame(SharedRenderState* renderState);

    // Added wrappers for the GLFW window state
    bool shouldClose() const {
        return window_ && glfwWindowShouldClose(window_);
    }

    void pollEvents() const {
        glfwPollEvents();
    }
private:
    // Core pipeline setups
    void createLogicalDevice();
    void createSwapChain();
    void createImageViews();
    void createRenderPass();
    void createGraphicsPipeline();
    void createFramebuffers();
    void createCommandPool();
    void createCommandBuffers();
    void createSyncObjects();
    void cleanupSwapChain();

    // Device & Swapchain helpers (Deduplicated!)
    void pickPhysicalDevice();
    bool isDeviceSuitable(VkPhysicalDevice device);
    bool checkDeviceExtensionSupport(VkPhysicalDevice device);
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device);
    SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device);
    
    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
    VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities);
    VkShaderModule createShaderModule(const std::vector<uint32_t>& code);
    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex, float playerX);

    // Private Member variables
    GraphicsConfig activeConfig_;
    
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapChain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapChainImages_;
    VkFormat swapChainImageFormat_;
    VkExtent2D swapChainExtent_;
    std::vector<VkImageView> swapChainImageViews_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline graphicsPipeline_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> swapChainFramebuffers_;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;

    std::vector<VkSemaphore> imageAvailableSemaphores_;
    std::vector<VkSemaphore> renderFinishedSemaphores_;
    std::vector<VkFence> inFlightFences_;
    uint32_t currentFrame_ = 0;
    const int MAX_FRAMES_IN_FLIGHT = 2;

    const std::vector<const char*> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };
    WindowState  windowState_;;
    bool framebufferResized_ = false;
    GLFWwindow* window_ = nullptr;
    
    void recreateSwapchain(GLFWwindow* window); // Your existing swapchain recreation function
};```

## File: ./client/include/NetworkManager.hpp
```cpp
#pragma once
#include <asio.hpp>
#include <memory>
#include <thread>
#include <map>
#include <functional>
#include <vector>
#include <optional>
#include "Protocol.hpp"
#include "ThreadUtility.hpp" // For Rebel::Concurrent::ThreadSafeQueue

namespace Rebel
{
    // Wrapper for packets stored in the queue
    struct InboundPacket
    {
        PacketHeader header;
        std::vector<uint8_t> payload;
    };
}

// Inside NetworkManager.hpp
class NetworkManager
{
public:
    NetworkManager();
    ~NetworkManager();

    std::thread &getThread() { return network_thread_; }
    void connect(const std::string &host, const std::string &port);
    void disconnect();
    void sendPacket(const Rebel::PacketHeader &header, const void *payload = nullptr, std::size_t payloadSize = 0);

    // This is how the Logic Thread gets its data now
    Rebel::Concurrent::ThreadSafeQueue<Rebel::InboundPacket> &getPacketQueue()
    {
        return inbound_queue_;
    }

private:
    void startRead();
    void readPayload(uint16_t size);

    asio::io_context io_context_;
    asio::strand<asio::io_context::executor_type> strand_;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work_guard_;

    std::optional<asio::ip::tcp::socket> socket_;
    std::thread network_thread_;

    Rebel::PacketHeader incoming_header_;
    std::vector<uint8_t> incoming_payload_;

    // The mailbox replacing the handlers_ map
    Rebel::Concurrent::ThreadSafeQueue<Rebel::InboundPacket> inbound_queue_;
};
```

## File: ./client/shaders/shader.frag
```glsl
#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(fragColor, 1.0);
}```

## File: ./client/shaders/shader.vert
```glsl
#version 450

layout(location = 0) out vec3 fragColor;

// 1. Declare the Push Constant block
layout(push_constant) uniform PushConstants {
    float player_x;
} pc;

vec2 positions[3] = vec2[](
    vec2(0.0, -0.5),
    vec2(0.5, 0.5),
    vec2(-0.5, 0.5)
);

vec3 colors[3] = vec3[](
    vec3(1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0),
    vec3(0.0, 0.0, 1.0)
);

void main() {
    // 2. Add the player_x to the triangle's X position!
    // Note: We multiply by 0.05 because Vulkan screen space is only -1.0 to 1.0. 
    // Since your server X coordinate goes up to 60+, the triangle would instantly 
    // fly off the screen if we didn't scale it down!
    float scaled_x = pc.player_x * 0.05; 

    gl_Position = vec4(positions[gl_VertexIndex].x + scaled_x, positions[gl_VertexIndex].y, 0.0, 1.0);
    fragColor = colors[gl_VertexIndex];
}```

