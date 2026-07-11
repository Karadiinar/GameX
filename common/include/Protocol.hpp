#pragma once
#include <cstddef>
#include <cstdint>

namespace Rebel {

enum class Opcode : uint16_t {
  CMSG_PING = 0x0001,
  SMSG_PONG = 0x0002,
  CMSG_AUTH_SESSION = 0x0003,
  SMSG_AUTH_RESPONSE = 0x0004,
  CMSG_GAME_AUTH = 0x0005, // client -> game server, replaces resending MsgLogin
  SMSG_GAME_AUTH_OK = 0x0006,
  SMSG_GAME_AUTH_FAIL = 0x0007,

  CMSG_CHAT_SAY = 0x0100,
  SMSG_CHAT_SAY = 0x0101,

  CMSG_PLAYER_MOVE = 0x0200,  // client -> server: movement intent, not a position
  SMSG_PLAYER_STATE = 0x0201, // server -> all clients: authoritative position for one player
  SMSG_PLAYER_LEAVE = 0x0202  // server -> all clients: a player's entity was removed
};

namespace MoveFlags {
constexpr uint8_t Up = 1 << 0;
constexpr uint8_t Down = 1 << 1;
constexpr uint8_t Left = 1 << 2;
constexpr uint8_t Right = 1 << 3;
} // namespace MoveFlags

// Add new channels here as they get real backing systems (e.g. Party once
// grouping exists) — the wire format doesn't need to change, just this enum.
enum class ChatChannel : uint8_t {
  Say = 0,     // Short range, GameServer-enforced
  Yell = 1,    // Long range, GameServer-enforced
  Whisper = 2, // Routed to one named recipient only
  Guild = 3,   // Reserved: no guild system exists yet, GameServer no-ops it
};

constexpr std::size_t MAX_CHAT_MESSAGE_LEN = 255;

#pragma pack(push, 1)
struct PacketHeader {
  uint16_t size;   // Total size
  uint16_t opcode; // Changed to uint16_t to match enum
};

struct MsgLogin {
  char username[32];
  char password[32];
  uint32_t version;
};

struct MsgRedirect {
  char ip[16];
  uint16_t port;
  char session_token[32];
};
struct MsgPlayerMove {
  uint8_t moveFlags; // Bitmask of MoveFlags — the client's current input, not a position
  float yaw;         // Facing, in radians — see Rebel::apply_move_flags for the convention
};

struct MsgGameAuth {
  char session_token[32]; // Matches MsgRedirect::session_token / sessions.token
};

struct MsgGameAuthOk {
  uint32_t character_id; // Tells the client which SMSG_PLAYER_STATE updates are "self"
};

struct MsgPlayerState {
  uint32_t character_id;
  float x, y, z;
  float yaw;
};

struct MsgPlayerLeave {
  uint32_t character_id;
};

// CMSG_CHAT_SAY (client -> server). `target` is only meaningful for
// ChatChannel::Whisper — ignored otherwise. The chat text itself is NOT a
// fixed field: it's every byte after this struct, up to PacketHeader::size
// (same variable-payload trick every packet already uses; there's just no
// fixed-size struct after this prefix this time).
struct MsgChatSend {
  uint8_t channel; // ChatChannel
  char target[32]; // Whisper recipient's character name
};

// SMSG_CHAT_SAY (server -> client). Same trailing-message convention as
// MsgChatSend. One opcode serves every channel — `channel` tells the client
// how to label/color it, so adding a new channel later doesn't need a new
// opcode.
struct MsgChatRecv {
  uint8_t channel; // ChatChannel
  char sender[32]; // Who sent it
};
#pragma pack(pop)

} // namespace Rebel
