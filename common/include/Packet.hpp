#pragma once
#include <cstdint>

namespace Rebel {

// Network structures MUST be packed. 
// This prevents the compiler from injecting empty "padding" bytes to align memory,
// which would corrupt the data when sent across the network.
#pragma pack(push, 1)

struct PacketHeader {
    uint16_t size;   // The total size of the packet (Header + Payload)
    uint16_t opcode; // The command ID
};

#pragma pack(pop)

// Our Dictionary of Commands
enum class Opcode : uint16_t {
    // 0x0000 - 0x00FF: System and Handshakes
    CMSG_PING = 0x0001,           // Client -> Server: "Are you there?"
    SMSG_PONG = 0x0002,           // Server -> Client: "Yes, I am."
    
    CMSG_AUTH_SESSION = 0x0003,   // Client -> Server: "Here is my login token"
    SMSG_AUTH_RESPONSE = 0x0004,  // Server -> Client: "Login success/fail"

    // 0x0100 - 0x01FF: Chat System
    CMSG_CHAT_SAY = 0x0100,       // Client -> Server: "I want to say 'Hello'"
    SMSG_CHAT_SAY = 0x0101,       // Server -> Client: "Player X said 'Hello'"

    // 0x0200+ : Movement, Combat, etc...
    CMSG_PLAYER_MOVE = 0x0200
};

} // namespace Rebel