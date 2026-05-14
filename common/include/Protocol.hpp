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
#pragma pack(pop)

} // namespace Rebel