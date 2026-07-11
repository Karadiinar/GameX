#include <catch2/catch_test_macros.hpp>
#include "Protocol.hpp"

// These sizes ARE the wire contract between client, LoginServer, and
// GameServer. If one of these changes unintentionally, something just broke
// compatibility between the three binaries.
TEST_CASE("Protocol struct sizes match the wire contract", "[protocol]") {
  CHECK(sizeof(Rebel::PacketHeader) == 4);
  CHECK(sizeof(Rebel::MsgLogin) == 68);
  CHECK(sizeof(Rebel::MsgRedirect) == 50);
  CHECK(sizeof(Rebel::MsgPlayerMove) == 5);
  CHECK(sizeof(Rebel::MsgGameAuth) == 32);
  CHECK(sizeof(Rebel::MsgGameAuthOk) == 4);
  CHECK(sizeof(Rebel::MsgPlayerState) == 20);
  CHECK(sizeof(Rebel::MsgPlayerLeave) == 4);
  CHECK(sizeof(Rebel::MsgChatSend) == 33);
  CHECK(sizeof(Rebel::MsgChatRecv) == 33);
}

TEST_CASE("MoveFlags bits don't collide", "[protocol]") {
  CHECK((Rebel::MoveFlags::Up & Rebel::MoveFlags::Down) == 0);
  CHECK((Rebel::MoveFlags::Left & Rebel::MoveFlags::Right) == 0);
  CHECK((Rebel::MoveFlags::Up & Rebel::MoveFlags::Left) == 0);
  CHECK((Rebel::MoveFlags::Up & Rebel::MoveFlags::Right) == 0);
  CHECK((Rebel::MoveFlags::Down & Rebel::MoveFlags::Left) == 0);
  CHECK((Rebel::MoveFlags::Down & Rebel::MoveFlags::Right) == 0);
}
