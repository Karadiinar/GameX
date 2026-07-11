#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "Movement.hpp"

using Catch::Approx;

TEST_CASE("No input flags means no movement", "[movement]") {
  Rebel::Vec2 pos{1.0f, 2.0f};
  auto result = Rebel::apply_move_flags(pos, 0, 0.064f, 0.0f);
  CHECK(result.x == Approx(1.0f));
  CHECK(result.z == Approx(2.0f));
}

TEST_CASE("Facing yaw=0, forward/backward/strafe move along the defined local axes", "[movement]") {
  Rebel::Vec2 origin{0.0f, 0.0f};
  const float delta = 0.064f;
  const float yaw0 = 0.0f;

  auto forward = Rebel::apply_move_flags(origin, Rebel::MoveFlags::Up, delta, yaw0);
  CHECK(forward.x == Approx(0.0f));
  CHECK(forward.z == Approx(delta));

  auto backward = Rebel::apply_move_flags(origin, Rebel::MoveFlags::Down, delta, yaw0);
  CHECK(backward.x == Approx(0.0f));
  CHECK(backward.z == Approx(-delta));

  auto right = Rebel::apply_move_flags(origin, Rebel::MoveFlags::Right, delta, yaw0);
  CHECK(right.x == Approx(delta));
  CHECK(right.z == Approx(0.0f));

  auto left = Rebel::apply_move_flags(origin, Rebel::MoveFlags::Left, delta, yaw0);
  CHECK(left.x == Approx(-delta));
  CHECK(left.z == Approx(0.0f));
}

TEST_CASE("Facing yaw=90 degrees rotates forward/strafe onto the other axis", "[movement]") {
  Rebel::Vec2 origin{0.0f, 0.0f};
  const float delta = 0.064f;
  const float yaw90 = 1.5707963f; // pi/2

  auto forward = Rebel::apply_move_flags(origin, Rebel::MoveFlags::Up, delta, yaw90);
  CHECK(forward.x == Approx(delta).margin(0.0001));
  CHECK(forward.z == Approx(0.0f).margin(0.0001));

  auto right = Rebel::apply_move_flags(origin, Rebel::MoveFlags::Right, delta, yaw90);
  CHECK(right.x == Approx(0.0f).margin(0.0001));
  CHECK(right.z == Approx(-delta).margin(0.0001));
}

TEST_CASE("Opposing flags held together cancel out regardless of yaw", "[movement]") {
  Rebel::Vec2 pos{5.0f, 5.0f};
  const float delta = 0.064f;
  const float yaw = 0.7f; // arbitrary non-axis-aligned yaw, proves cancellation isn't yaw-dependent

  auto horizontal = Rebel::apply_move_flags(
      pos, Rebel::MoveFlags::Left | Rebel::MoveFlags::Right, delta, yaw);
  CHECK(horizontal.x == Approx(5.0f));
  CHECK(horizontal.z == Approx(5.0f));

  auto vertical = Rebel::apply_move_flags(
      pos, Rebel::MoveFlags::Up | Rebel::MoveFlags::Down, delta, yaw);
  CHECK(vertical.x == Approx(5.0f));
  CHECK(vertical.z == Approx(5.0f));
}

TEST_CASE("Diagonal input advances both local axes independently", "[movement]") {
  Rebel::Vec2 origin{0.0f, 0.0f};
  const float delta = 0.064f;

  auto result = Rebel::apply_move_flags(
      origin, Rebel::MoveFlags::Right | Rebel::MoveFlags::Up, delta, 0.0f);
  CHECK(result.x == Approx(delta));
  CHECK(result.z == Approx(delta));
}

TEST_CASE("Repeated ticks accumulate linearly", "[movement]") {
  Rebel::Vec2 pos{0.0f, 0.0f};
  const float delta = 0.064f;

  for (int tick = 0; tick < 20; ++tick) {
    pos = Rebel::apply_move_flags(pos, Rebel::MoveFlags::Right, delta, 0.0f);
  }

  // 20 ticks at 20Hz == 1 second of holding the key.
  CHECK(pos.x == Approx(1.28f));
}
