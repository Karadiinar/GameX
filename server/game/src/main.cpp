#include <iostream>
#include <asio.hpp>
#include <chrono>
#include <vector>
#include <thread>
#include <algorithm>
#include <atomic>
#include <memory>
#include <cstdlib>
#include <cstring>
#include "Protocol.hpp"
#include "Version.hpp"
#include "Movement.hpp"
#include <entt/entt.hpp>
#include <pqxx/pqxx>



entt::registry world;
std::unique_ptr<pqxx::connection> g_db;

// Define some basic components
struct Position { float x, y, z, yaw = 0.0f; };
struct PlayerData { std::string name; int character_id = -1; };
struct PlayerInput { uint8_t moveFlags = 0; float yaw = 0.0f; }; // Latest intent from the client — not a position

class PlayerSession;
std::vector<std::weak_ptr<PlayerSession>> g_sessions; // Only touched from code posted to game_strand
void broadcast_player_states();
void broadcast_player_leave(int character_id);
void broadcast_chat(Rebel::ChatChannel channel, const std::string &sender,
                    float senderX, float senderZ, const std::string &target,
                    const std::string &message);

constexpr float SAY_RANGE = 15.0f;  // World units
constexpr float YELL_RANGE = 60.0f; // World units — there's only one "zone", so this covers it

// Persists a player's world position back to the characters table.
// Only ever called from code posted to game_strand, so g_db is never touched concurrently.
void save_character(int character_id, float x, float y, float z) {
    if (!g_db || character_id < 0) return;
    try {
        pqxx::work txn(*g_db);
        txn.exec_params(
            "UPDATE characters SET pos_x = $1, pos_y = $2, pos_z = $3, last_saved_at = now() WHERE id = $4",
            x, y, z, character_id);
        txn.commit();
    } catch (const std::exception &e) {
        std::cerr << "[WORLD] Failed to save character " << character_id << ": " << e.what() << std::endl;
    }
}

constexpr float TICK_DT_SEC = 0.05f;         // 20Hz
constexpr int AUTOSAVE_INTERVAL_TICKS = 600; // Every 30s at 20Hz

// Advances every player's Position from their latest PlayerInput. This is the
// server's one and only place that mutates Position from movement — the client
// only ever sends intent (CMSG_PLAYER_MOVE), never a position.
void simulate_movement() {
    const float delta = Rebel::PLAYER_SPEED_PER_SEC * TICK_DT_SEC;
    auto view = world.view<PlayerInput, Position>();
    view.each([delta](auto entity, auto &input, auto &pos) {
        Rebel::Vec2 moved = Rebel::apply_move_flags({pos.x, pos.z}, input.moveFlags, delta, input.yaw);
        pos.x = moved.x;
        pos.z = moved.z;
        pos.yaw = input.yaw; // No longer purely cosmetic — it now also drives movement direction
                              // above, though the server still doesn't validate its rate of change.
    });
}

// Function to handle each tick of the 20Hz game loop
void update_game_world(int current_tick) {
    simulate_movement();
    broadcast_player_states();

    bool shouldLog = (current_tick % 20 == 0); // Only log ~once/second to keep the terminal readable
    bool shouldAutosave = current_tick != 0 && (current_tick % AUTOSAVE_INTERVAL_TICKS == 0);
    if (!shouldLog && !shouldAutosave) return;

    auto view = world.view<PlayerData, Position>();

    if (shouldLog && view.size_hint() == 0) {
        std::cout << "[WORLD] Heartbeat - Tick: " << current_tick << std::endl;
    }

    view.each([current_tick, shouldLog, shouldAutosave](auto entity, auto &data, auto &pos) {
        if (shouldLog) {
            std::cout << "[WORLD] Tick: " << current_tick
                      << " | Player: " << data.name
                      << " | X: " << pos.x
                      << " | Y: " << pos.y << std::endl;
        }
        if (shouldAutosave) {
            save_character(data.character_id, pos.x, pos.y, pos.z);
        }
    });
}

// The clean Asio tick orchestrator
void game_loop_tick(asio::steady_timer& timer, 
                    asio::strand<asio::io_context::executor_type>& strand, 
                    std::atomic<int>& tick_count, 
                    std::atomic<bool>& running) {
    if (!running.load()) return;

    // 1. Execute the isolated game logic
    int current_tick = tick_count++;
    update_game_world(current_tick);

    // 2. Schedule the next tick precisely 50ms into the future (20Hz)
    timer.expires_at(timer.expiry() + std::chrono::milliseconds(50));
    
    // Using a simple lambda wrapper to forward the next call smoothly
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
          state_(SessionState::WAITING_FOR_AUTH) {}

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
                    auto &data = world.get<PlayerData>(id);
                    auto &pos = world.get<Position>(id);
                    save_character(data.character_id, pos.x, pos.y, pos.z);
                    int character_id = data.character_id;
                    world.destroy(id);
                    std::cout << "[WORLD] Entity destroyed (Player disconnected)." << std::endl;
                    broadcast_player_leave(character_id);
                }
            });
        }
    }

    void send_player_state(uint32_t character_id, float x, float y, float z, float yaw) {
        auto packet = std::make_shared<std::vector<uint8_t>>(
            sizeof(Rebel::PacketHeader) + sizeof(Rebel::MsgPlayerState));
        auto *h = reinterpret_cast<Rebel::PacketHeader *>(packet->data());
        h->size = packet->size();
        h->opcode = static_cast<uint16_t>(Rebel::Opcode::SMSG_PLAYER_STATE);
        auto *m = reinterpret_cast<Rebel::MsgPlayerState *>(
            packet->data() + sizeof(Rebel::PacketHeader));
        m->character_id = character_id;
        m->x = x;
        m->y = y;
        m->z = z;
        m->yaw = yaw;

        auto self(shared_from_this());
        asio::async_write(socket_, asio::buffer(*packet),
            [self, packet](const asio::error_code &, std::size_t) {});
    }

    void send_player_leave(uint32_t character_id) {
        auto packet = std::make_shared<std::vector<uint8_t>>(
            sizeof(Rebel::PacketHeader) + sizeof(Rebel::MsgPlayerLeave));
        auto *h = reinterpret_cast<Rebel::PacketHeader *>(packet->data());
        h->size = packet->size();
        h->opcode = static_cast<uint16_t>(Rebel::Opcode::SMSG_PLAYER_LEAVE);
        auto *m = reinterpret_cast<Rebel::MsgPlayerLeave *>(
            packet->data() + sizeof(Rebel::PacketHeader));
        m->character_id = character_id;

        auto self(shared_from_this());
        asio::async_write(socket_, asio::buffer(*packet),
            [self, packet](const asio::error_code &, std::size_t) {});
    }

    // Used by broadcast_chat() to find/filter recipients — this session's own
    // identity, looked up from the entity it already owns rather than cached
    // separately.
    bool getUsername(std::string &outName) const {
        if (!has_entity_ || !world.valid(entity_id_)) return false;
        outName = world.get<PlayerData>(entity_id_).name;
        return true;
    }

    bool getPosition(float &outX, float &outZ) const {
        if (!has_entity_ || !world.valid(entity_id_)) return false;
        auto &pos = world.get<Position>(entity_id_);
        outX = pos.x;
        outZ = pos.z; // Ground-plane axis under the Y-up convention — y is height
        return true;
    }

    void send_chat(Rebel::ChatChannel channel, const std::string &sender,
                  const std::string &message) {
        auto packet = std::make_shared<std::vector<uint8_t>>(
            sizeof(Rebel::PacketHeader) + sizeof(Rebel::MsgChatRecv) + message.size());
        auto *h = reinterpret_cast<Rebel::PacketHeader *>(packet->data());
        h->size = packet->size();
        h->opcode = static_cast<uint16_t>(Rebel::Opcode::SMSG_CHAT_SAY);
        auto *m = reinterpret_cast<Rebel::MsgChatRecv *>(
            packet->data() + sizeof(Rebel::PacketHeader));
        m->channel = static_cast<uint8_t>(channel);
        std::memset(m->sender, 0, sizeof(m->sender));
        std::strncpy(m->sender, sender.c_str(), sizeof(m->sender) - 1);
        std::memcpy(packet->data() + sizeof(Rebel::PacketHeader) + sizeof(Rebel::MsgChatRecv),
                   message.data(), message.size());

        auto self(shared_from_this());
        asio::async_write(socket_, asio::buffer(*packet),
            [self, packet](const asio::error_code &, std::size_t) {});
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
            if (opcode == Rebel::Opcode::CMSG_GAME_AUTH) {
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

                // 3. Just record intent — simulate_movement() applies it once per
                // tick, so we're never mutating Position off the tick's own step.
                // No mutex needed because we are on the game_strand!
                if (has_entity_ && world.valid(entity_id_)) {
                    auto& input = world.get<PlayerInput>(entity_id_);
                    input.moveFlags = move->moveFlags;
                    input.yaw = move->yaw;
                }
                break;
            }

            case Rebel::Opcode::CMSG_CHAT_SAY: {
                if (payload_.size() < sizeof(Rebel::MsgChatSend)) {
                    std::cerr << "[SESSION] Malformed chat packet size." << std::endl;
                    break;
                }
                if (!has_entity_ || !world.valid(entity_id_)) break;

                auto* chat = reinterpret_cast<Rebel::MsgChatSend*>(payload_.data());
                auto channel = static_cast<Rebel::ChatChannel>(chat->channel);
                std::string target(chat->target);

                std::string message(
                    reinterpret_cast<char*>(payload_.data() + sizeof(Rebel::MsgChatSend)),
                    payload_.size() - sizeof(Rebel::MsgChatSend));
                if (message.size() > Rebel::MAX_CHAT_MESSAGE_LEN) {
                    message.resize(Rebel::MAX_CHAT_MESSAGE_LEN);
                }

                auto& senderData = world.get<PlayerData>(entity_id_);
                auto& senderPos = world.get<Position>(entity_id_);
                broadcast_chat(channel, senderData.name, senderPos.x, senderPos.z, target, message);
                break;
            }
            default:
                break;
        }
    }

    void handle_auth() {
        if (payload_.size() < sizeof(Rebel::MsgGameAuth)) {
            std::cerr << "[SESSION] Game auth packet too small." << std::endl;
            socket_.close();
            return;
        }

        auto* msg = reinterpret_cast<Rebel::MsgGameAuth*>(payload_.data());
        std::string token(msg->session_token, sizeof(msg->session_token));

        std::string username;
        int character_id = -1;
        float px = 0.0f, py = 0.0f, pz = 0.0f;

        try {
            pqxx::work txn(*g_db);

            // The token proves the LoginServer already authenticated this player —
            // we no longer trust a resent username/password here.
            auto sess = txn.exec_params(
                "SELECT sessions.account_id, accounts.username FROM sessions "
                "JOIN accounts ON accounts.id = sessions.account_id "
                "WHERE sessions.token = $1 AND sessions.expires_at > now()",
                token);
            if (sess.empty()) {
                std::cerr << "[SESSION] Invalid or expired session token." << std::endl;
                send_auth_fail();
                return;
            }
            int account_id = sess[0]["account_id"].as<int>();
            username = sess[0]["username"].as<std::string>();

            // Single-use: consume the token now that it's been validated.
            txn.exec_params("DELETE FROM sessions WHERE token = $1", token);

            std::cout << "[SESSION] Player '" << username << "' authenticated via session token." << std::endl;

            // One character per account for now — there's no character-select screen yet.
            auto chars = txn.exec_params(
                "SELECT id, pos_x, pos_y, pos_z FROM characters WHERE account_id = $1 ORDER BY id LIMIT 1",
                account_id);

            if (chars.empty()) {
                auto created = txn.exec_params(
                    "INSERT INTO characters (account_id, name) VALUES ($1, $2) "
                    "RETURNING id, pos_x, pos_y, pos_z",
                    account_id, username);
                character_id = created[0]["id"].as<int>();
                px = created[0]["pos_x"].as<float>();
                py = created[0]["pos_y"].as<float>();
                pz = created[0]["pos_z"].as<float>();
            } else {
                character_id = chars[0]["id"].as<int>();
                px = chars[0]["pos_x"].as<float>();
                py = chars[0]["pos_y"].as<float>();
                pz = chars[0]["pos_z"].as<float>();
            }

            txn.commit();
        } catch (const std::exception &e) {
            std::cerr << "[SESSION] DB error validating session for token: " << e.what() << std::endl;
            send_auth_fail();
            return;
        }

        state_ = SessionState::AUTHENTICATED;

        entity_id_ = world.create();
        world.emplace<PlayerData>(entity_id_, username, character_id);
        world.emplace<Position>(entity_id_, px, py, pz);
        world.emplace<PlayerInput>(entity_id_);
        has_entity_ = true;
        g_sessions.push_back(weak_from_this());

        std::cout << "[WORLD] Loaded character " << character_id << " for " << username
                  << " at (" << px << ", " << py << ", " << pz << ")" << std::endl;

        auto full_packet = std::make_shared<std::vector<uint8_t>>(
            sizeof(Rebel::PacketHeader) + sizeof(Rebel::MsgGameAuthOk));
        auto *h = reinterpret_cast<Rebel::PacketHeader *>(full_packet->data());
        h->size = full_packet->size();
        h->opcode = static_cast<uint16_t>(Rebel::Opcode::SMSG_GAME_AUTH_OK);
        auto *m = reinterpret_cast<Rebel::MsgGameAuthOk *>(
            full_packet->data() + sizeof(Rebel::PacketHeader));
        m->character_id = static_cast<uint32_t>(character_id);

        auto self(shared_from_this());
        asio::async_write(socket_, asio::buffer(*full_packet),
            [this, self, full_packet](const asio::error_code& ec, std::size_t) {
                if (ec) {
                    socket_.close();
                }
            });
    }

    void send_auth_fail() {
        auto response = std::make_shared<Rebel::PacketHeader>();
        response->size = sizeof(Rebel::PacketHeader);
        response->opcode = static_cast<uint16_t>(Rebel::Opcode::SMSG_GAME_AUTH_FAIL);

        auto self(shared_from_this());
        asio::async_write(socket_, asio::buffer(response.get(), sizeof(Rebel::PacketHeader)),
            [this, self, response](const asio::error_code&, std::size_t) {
                socket_.close();
            });
    }

    void send_pong() {
        auto response = std::make_shared<Rebel::PacketHeader>();
        response->size = sizeof(Rebel::PacketHeader);
        response->opcode = static_cast<uint16_t>(Rebel::Opcode::SMSG_PONG);

        auto self(shared_from_this());
        asio::async_write(socket_, asio::buffer(response.get(), sizeof(Rebel::PacketHeader)),
            [self, response](const asio::error_code &, std::size_t) {});
    }

    asio::ip::tcp::socket socket_;
    asio::strand<asio::io_context::executor_type>& strand_;
    SessionState state_;
    Rebel::PacketHeader header_;
    std::vector<uint8_t> payload_;
};

// Sends every player's current position to every connected session (including
// their own — the client filters by character_id from SMSG_GAME_AUTH_OK).
// Prunes sessions whose PlayerSession has already been destroyed.
void broadcast_player_states() {
    auto view = world.view<PlayerData, Position>();
    for (auto entity : view) {
        auto &data = view.get<PlayerData>(entity);
        auto &pos = view.get<Position>(entity);
        uint32_t character_id = static_cast<uint32_t>(data.character_id);

        for (auto it = g_sessions.begin(); it != g_sessions.end();) {
            if (auto session = it->lock()) {
                session->send_player_state(character_id, pos.x, pos.y, pos.z, pos.yaw);
                ++it;
            } else {
                it = g_sessions.erase(it);
            }
        }
    }
}

void broadcast_player_leave(int character_id) {
    uint32_t id = static_cast<uint32_t>(character_id);
    for (auto it = g_sessions.begin(); it != g_sessions.end();) {
        if (auto session = it->lock()) {
            session->send_player_leave(id);
            ++it;
        } else {
            it = g_sessions.erase(it);
        }
    }
}

// Routes one chat message per its channel's rules: Say/Yell are range-limited
// broadcasts from the sender's own position, Whisper goes to exactly one
// named recipient, and Guild is a documented no-op — there's no guild system
// to route it to yet, but the channel already exists on the wire so adding
// one later won't need a protocol change.
void broadcast_chat(Rebel::ChatChannel channel, const std::string &sender,
                    float senderX, float senderZ, const std::string &target,
                    const std::string &message) {
    switch (channel) {
        case Rebel::ChatChannel::Whisper: {
            for (auto it = g_sessions.begin(); it != g_sessions.end();) {
                if (auto session = it->lock()) {
                    std::string name;
                    if (session->getUsername(name) && name == target) {
                        session->send_chat(channel, sender, message);
                    }
                    ++it;
                } else {
                    it = g_sessions.erase(it);
                }
            }
            break;
        }
        case Rebel::ChatChannel::Say:
        case Rebel::ChatChannel::Yell: {
            float range = (channel == Rebel::ChatChannel::Say) ? SAY_RANGE : YELL_RANGE;
            for (auto it = g_sessions.begin(); it != g_sessions.end();) {
                if (auto session = it->lock()) {
                    float x, z;
                    if (session->getPosition(x, z)) {
                        float dx = x - senderX;
                        float dz = z - senderZ;
                        if (dx * dx + dz * dz <= range * range) {
                            session->send_chat(channel, sender, message);
                        }
                    }
                    ++it;
                } else {
                    it = g_sessions.erase(it);
                }
            }
            break;
        }
        case Rebel::ChatChannel::Guild:
        default:
            std::cerr << "[CHAT] Guild channel has no backing system yet — dropping message from '"
                      << sender << "'." << std::endl;
            break;
    }
}

void start_accept(asio::ip::tcp::acceptor& acceptor, asio::io_context& io_context, asio::strand<asio::io_context::executor_type>& strand) {
    acceptor.async_accept([&acceptor, &io_context, &strand](const asio::error_code& error, asio::ip::tcp::socket socket) {
        if (!error) {
            std::cout << "[GAME] Incoming connection from: " << socket.remote_endpoint() << std::endl;

            std::make_shared<PlayerSession>(std::move(socket), strand)->start();
        }
        start_accept(acceptor, io_context, strand);
    });
}

int main() {
    std::cout << "--- REBEL SERVER STARTING ---" << std::endl;

    const char *conn_str = std::getenv("REBEL_DB_CONNSTR");
    std::string connection_string =
        conn_str ? conn_str
                 : "dbname=rebelmmo user=rebel password=rebel host=127.0.0.1";
    try {
        g_db = std::make_unique<pqxx::connection>(connection_string);
        if (!g_db->is_open()) {
            std::cerr << "[GAME] Failed to open database connection." << std::endl;
            return 1;
        }
        std::cout << "[GAME] Connected to database: " << g_db->dbname() << std::endl;
    } catch (const std::exception &e) {
        std::cerr << "[GAME] DB connection error: " << e.what() << std::endl;
        return 1;
    }

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