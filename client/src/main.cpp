#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <entt/entt.hpp>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Movement.hpp"

// Must be included before custom graphics headers to ensure Vulkan macros are
// set
#include <GLFW/glfw3.h>

#include "NetworkManager.hpp"
#include "ThreadUtility.hpp"
#include "VulkanRenderer.hpp"
#include <mutex>

// Global shutdown flag to synchronize thread termination
std::atomic<bool> g_Running{true};

struct Transform {
  float x, y, z;
  float yaw;
};

struct LocalPlayerTag {};
struct RemotePlayerTag {};

const char *chatChannelName(Rebel::ChatChannel channel) {
  switch (channel) {
    case Rebel::ChatChannel::Say: return "Say";
    case Rebel::ChatChannel::Yell: return "Yell";
    case Rebel::ChatChannel::Whisper: return "Whisper";
    case Rebel::ChatChannel::Guild: return "Guild";
    default: return "Unknown";
  }
}

/**
 * Logic Thread: Responsible for game state evolution, physics, and packet
 * processing.
 */
void LogicThreadEntry(NetworkManager *network, SharedRenderState *renderState,
                      const VulkanRenderer *renderer) {
  entt::registry client_registry; // The client's own world data (local player + remote players)
  entt::entity local_player = entt::null;
  uint32_t local_character_id = 0; // From SMSG_GAME_AUTH_OK — identifies "self" in SMSG_PLAYER_STATE
  std::unordered_map<uint32_t, entt::entity> remote_players; // character_id -> entity
  bool inWorld = false;

  const std::chrono::microseconds TICK_TIME(1000000 / 64);
  std::cout << "[Logic] Engine tick thread started.\n";

  while (g_Running) {
    auto start = std::chrono::steady_clock::now();

    // Drain the mailbox
    while (auto packetOpt = network->getPacketQueue().try_pop()) {
      auto &packet = *packetOpt;

      if (packet.header.opcode ==
              static_cast<uint16_t>(Rebel::Opcode::SMSG_AUTH_RESPONSE) &&
          packet.payload.size() >= sizeof(Rebel::MsgRedirect)) {
        auto *redirect =
            reinterpret_cast<Rebel::MsgRedirect *>(packet.payload.data());
        std::string gameIp(redirect->ip);
        uint16_t gamePort = redirect->port;
        std::string sessionToken(redirect->session_token,
                                 sizeof(redirect->session_token));
        std::cout << "[Logic] Redirecting to Game Server at " << gameIp
                  << ":" << gamePort << "\n";

        network->disconnect();
        network->connect(gameIp, std::to_string(gamePort),
                         [network, sessionToken]() {
                           Rebel::MsgGameAuth authData;
                           std::memcpy(authData.session_token,
                                       sessionToken.data(),
                                       sizeof(authData.session_token));

                           Rebel::PacketHeader header;
                           header.size = sizeof(Rebel::PacketHeader) +
                                         sizeof(Rebel::MsgGameAuth);
                           header.opcode = static_cast<uint16_t>(
                               Rebel::Opcode::CMSG_GAME_AUTH);

                           network->sendPacket(header, &authData,
                                               sizeof(Rebel::MsgGameAuth));
                         });
      } else if (packet.header.opcode ==
                 static_cast<uint16_t>(Rebel::Opcode::SMSG_GAME_AUTH_OK)) {
        if (packet.payload.size() >= sizeof(Rebel::MsgGameAuthOk)) {
          auto *ok =
              reinterpret_cast<Rebel::MsgGameAuthOk *>(packet.payload.data());
          local_character_id = ok->character_id;
        }
        std::cout << "[Logic] Successfully authenticated with Game Server. "
                     "Entering world...\n";
        inWorld = true;

        local_player = client_registry.create();
        client_registry.emplace<Transform>(local_player, 0.0f, 0.0f, 0.0f,
                                           0.0f);
        client_registry.emplace<LocalPlayerTag>(local_player);
        std::cout << "[Logic] Local player entity spawned in client ECS.\n";
      } else if (packet.header.opcode ==
                 static_cast<uint16_t>(Rebel::Opcode::SMSG_GAME_AUTH_FAIL)) {
        std::cerr << "[Logic] Game Server rejected our session token.\n";
      } else if (packet.header.opcode ==
                 static_cast<uint16_t>(Rebel::Opcode::SMSG_PLAYER_STATE)) {
        if (packet.payload.size() >= sizeof(Rebel::MsgPlayerState)) {
          auto *state = reinterpret_cast<Rebel::MsgPlayerState *>(
              packet.payload.data());

          if (inWorld && state->character_id == local_character_id &&
              client_registry.valid(local_player)) {
            // Reconciliation: snap our predicted position to the server's.
            auto &transform = client_registry.get<Transform>(local_player);
            transform.x = state->x;
            transform.y = state->y;
            transform.z = state->z;
            transform.yaw = state->yaw;
          } else if (inWorld && state->character_id != local_character_id) {
            // Someone else — get-or-create their entity and update it.
            auto it = remote_players.find(state->character_id);
            entt::entity remote;
            if (it == remote_players.end()) {
              remote = client_registry.create();
              client_registry.emplace<Transform>(remote, 0.0f, 0.0f, 0.0f, 0.0f);
              client_registry.emplace<RemotePlayerTag>(remote);
              remote_players[state->character_id] = remote;
              std::cout << "[Logic] Tracking new remote player, character_id="
                        << state->character_id << "\n";
            } else {
              remote = it->second;
            }
            auto &transform = client_registry.get<Transform>(remote);
            transform.x = state->x;
            transform.y = state->y;
            transform.z = state->z;
            transform.yaw = state->yaw;
          }
        }
      } else if (packet.header.opcode ==
                 static_cast<uint16_t>(Rebel::Opcode::SMSG_PLAYER_LEAVE)) {
        if (packet.payload.size() >= sizeof(Rebel::MsgPlayerLeave)) {
          auto *leave = reinterpret_cast<Rebel::MsgPlayerLeave *>(
              packet.payload.data());
          auto it = remote_players.find(leave->character_id);
          if (it != remote_players.end()) {
            if (client_registry.valid(it->second)) {
              client_registry.destroy(it->second);
            }
            remote_players.erase(it);
          }
        }
      } else if (packet.header.opcode ==
                 static_cast<uint16_t>(Rebel::Opcode::SMSG_CHAT_SAY)) {
        if (packet.payload.size() >= sizeof(Rebel::MsgChatRecv)) {
          auto *chat =
              reinterpret_cast<Rebel::MsgChatRecv *>(packet.payload.data());
          std::string sender(chat->sender);
          std::string message(
              reinterpret_cast<char *>(packet.payload.data() +
                                       sizeof(Rebel::MsgChatRecv)),
              packet.payload.size() - sizeof(Rebel::MsgChatRecv));
          // No in-game chat UI/text rendering yet — this is the receive path
          // only, printed to the console so the feature is exercisable and
          // testable. Sending from the real client needs a text-input UI,
          // which doesn't exist in this renderer yet (deliberately deferred).
          std::cout << "[Chat:" << chatChannelName(static_cast<Rebel::ChatChannel>(chat->channel))
                    << "] " << sender << ": " << message << "\n";
        }
      }
    }

    // --- PREDICT LOCALLY, REPORT INTENT, PUBLISH THE FULL RENDER SNAPSHOT ---
    if (inWorld && client_registry.valid(local_player)) {
      auto &transform = client_registry.get<Transform>(local_player);

      // Facing always matches the camera's yaw — the simplified first-pass
      // control scheme (no decoupled strafe-while-camera-independently-
      // orbits mode yet). Must happen before moveFlags/prediction below,
      // since both need the up-to-date yaw this tick.
      transform.yaw = renderer->getCameraYaw();

      uint8_t moveFlags = 0;
      if (renderer->isMovingLeft()) {
        moveFlags |= Rebel::MoveFlags::Left;
      }
      if (renderer->isMovingRight()) {
        moveFlags |= Rebel::MoveFlags::Right;
      }
      if (renderer->isMovingUp()) {
        moveFlags |= Rebel::MoveFlags::Up;
      }
      if (renderer->isMovingDown()) {
        moveFlags |= Rebel::MoveFlags::Down;
      }

      // 1. PREDICT: apply the exact same movement math the server uses, at
      // this thread's 64Hz rate, so movement feels instant instead of
      // waiting on a round trip. SMSG_PLAYER_STATE (handled above) then
      // reconciles/corrects `transform` whenever the server's own tick
      // (20Hz) catches up — a hard snap, no smoothing, by design for now.
      const float clientDelta =
          Rebel::PLAYER_SPEED_PER_SEC *
          (std::chrono::duration<float>(TICK_TIME).count());
      Rebel::Vec2 predicted = Rebel::apply_move_flags(
          {transform.x, transform.z}, moveFlags, clientDelta, transform.yaw);
      transform.x = predicted.x;
      transform.z = predicted.z;

      // 2. UPDATE THE RENDER BRIDGE — publish every player's raw world-space
      // position. The camera transform (view/projection, driven by
      // mouse-look) now lives entirely on the GPU side via the camera UBO
      // in VulkanRenderer, so there's nothing left to pre-scale/pre-offset
      // client-side anymore.
      {
        std::vector<PlayerRenderState> snapshot;
        snapshot.push_back({local_character_id, transform.x, transform.y, transform.z, true});
        for (auto &[character_id, entity] : remote_players) {
          if (!client_registry.valid(entity)) continue;
          auto &remoteTransform = client_registry.get<Transform>(entity);
          snapshot.push_back({character_id, remoteTransform.x, remoteTransform.y,
                              remoteTransform.z, false});
        }

        std::lock_guard<std::mutex> lock(renderState->mtx);
        renderState->players = std::move(snapshot);
      }

      // 3. Send our input intent — not a position — for the server to simulate
      Rebel::MsgPlayerMove moveData;
      moveData.moveFlags = moveFlags;
      moveData.yaw = transform.yaw;

      Rebel::PacketHeader head;
      head.opcode = static_cast<uint16_t>(Rebel::Opcode::CMSG_PLAYER_MOVE);
      head.size = sizeof(Rebel::PacketHeader) + sizeof(Rebel::MsgPlayerMove);

      network->sendPacket(head, &moveData, sizeof(Rebel::MsgPlayerMove));
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    if (elapsed < TICK_TIME) {
      std::this_thread::sleep_for(TICK_TIME - elapsed);
    }
  }
  std::cout << "[Logic] Engine tick thread exiting...\n";
}

int main() {
  std::cout << "--- Rebel MMO Project (Client) ---\n";

  // Hardware discovery, used below to pin the network/logic threads to physical cores.
  auto coreMap = Rebel::ThreadUtils::GetPhysicalCoreMap();
  std::cout << "[Main] Discovered " << coreMap.size() << " physical cores.\n";

  // Network subsystem
  NetworkManager network;
  if (coreMap.size() > 1) {
    std::cout << "[Main] Pinning Network subsystem to Core " << coreMap[1]
              << "\n";
    Rebel::ThreadUtils::SetThreadAffinity(network.getThread(), coreMap[1]);
  }

  // TODO: replace with real login-screen credentials once one exists.
  network.connect("127.0.0.1", "54321", [&network]() {
    Rebel::MsgLogin loginData;
    std::strncpy(loginData.username, "Karadiinar", sizeof(loginData.username));
    std::strncpy(loginData.password, "changeme", sizeof(loginData.password));
    loginData.version = 1;

    Rebel::PacketHeader header;
    header.size = sizeof(Rebel::PacketHeader) + sizeof(Rebel::MsgLogin);
    header.opcode = static_cast<uint16_t>(Rebel::Opcode::CMSG_AUTH_SESSION);

    network.sendPacket(header, &loginData, sizeof(Rebel::MsgLogin));
  });

  // Shared state bridging the logic thread's simulation to the render thread.
  SharedRenderState sharedRenderState;

  // Graphics subsystem
  try {
    // Fire up GLFW before calling window manipulation functions
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    if (!glfwInit()) {
      throw std::runtime_error("[Graphics] Failed to initialize GLFW!");
    }

    // Setup default configuration settings
    GraphicsConfig userGraphicsSettings;
    userGraphicsSettings.presentMode = PresentModeSetting::TripleBuffer;
    userGraphicsSettings.windowWidth = 1280;
    userGraphicsSettings.windowHeight = 720;

    // Flush hints and explicitly declare Vulkan API support before generating
    // the container
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    // Create the window context
    GLFWwindow *window = glfwCreateWindow(userGraphicsSettings.windowWidth,
                                          userGraphicsSettings.windowHeight,
                                          "Rebel Client", nullptr, nullptr);

    if (!window) {
      throw std::runtime_error("[Graphics] Failed to create GLFW window!");
    }

    // Declared before the logic thread spawns below, so &renderer is safe to hand off.
    VulkanRenderer renderer;

    std::thread logicThread(LogicThreadEntry, &network, &sharedRenderState,
                            &renderer);
    if (coreMap.size() > 2) {
      std::cout << "[Main] Pinning Logic subsystem to Core " << coreMap[2]
                << "\n";
      Rebel::ThreadUtils::SetThreadAffinity(logicThread, coreMap[2]);
    }

    // Pass the window context and config to the initialization pipeline
    renderer.init(window, userGraphicsSettings);

    // Associates `renderer` with this window so the static GLFW callbacks
    // below can recover it via glfwGetWindowUserPointer().
    glfwSetWindowUserPointer(window, &renderer);

    glfwSetKeyCallback(window, VulkanRenderer::glfw_key_callback);

    // Mouse-look/orbit camera input: right-click-drag to look, scroll to zoom.
    glfwSetCursorPosCallback(window, VulkanRenderer::glfw_cursor_pos_callback);
    glfwSetMouseButtonCallback(window, VulkanRenderer::glfw_mouse_button_callback);
    glfwSetScrollCallback(window, VulkanRenderer::glfw_scroll_callback);

    // Set up the window resize callback lambda
    glfwSetFramebufferSizeCallback(
        window, [](GLFWwindow *win, int width, int height) {
          auto *rend =
              reinterpret_cast<VulkanRenderer *>(glfwGetWindowUserPointer(win));
          if (rend) {
            rend->framebufferResizeCallback();
          }
        });

    while (!renderer.shouldClose()) {
      renderer.pollEvents(); // Dispatches the key/mouse/resize callbacks above
      renderer.drawFrame(&sharedRenderState);
    }

    std::cout
        << "\n[Main] Shutdown signal received. Terminating subsystems...\n";
    g_Running = false;

    if (logicThread.joinable()) {
      logicThread.join();
    }

    renderer.cleanup();
    glfwDestroyWindow(window);
    glfwTerminate();
  } catch (const std::exception &e) {
    std::cerr << "\n[Fatal Error] " << e.what() << std::endl;
    g_Running = false;
    glfwTerminate();
    return -1;
  }

  std::cout << "[Main] Clean shutdown complete.\n";
  return 0;
}
