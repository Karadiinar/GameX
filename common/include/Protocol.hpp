#include <cstdint>
#pragma pack(push, 1) // Ensures no "padding" so the bits match over the wire
struct PacketHeader {
    uint16_t size;
    uint8_t type;
};

struct MsgLogin {
    char username[32];
    uint32_t version;
};
#pragma pack(pop)