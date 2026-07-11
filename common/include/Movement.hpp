#pragma once
#include "Protocol.hpp"
#include <cmath>

namespace Rebel {

struct Vec2 {
  float x = 0.0f;
  float z = 0.0f; // Ground-plane secondary axis — named z, not y, so the
                   // field itself documents this isn't the height axis.
                   // Position/Transform's y is height (Y-up convention);
                   // x/z is the ground plane.
};

// Shared by the client (local prediction) and GameServer (authoritative
// simulation) so both compute movement at the exact same rate. If they ever
// drift apart, the client will see constant correction snapping even under
// perfect network conditions.
constexpr float PLAYER_SPEED_PER_SEC = 1.28f; // Matches the old client-side feel (0.02 units/tick @ 64Hz)

// Facing-relative movement step. yawRadians rotates around world +Y using
// this project's one fixed convention:
//   forward(yaw) = (sin(yaw), cos(yaw))   in (x, z)
//   right(yaw)   = (cos(yaw), -sin(yaw))  in (x, z)
// At yaw=0, forward is +Z and right is +X. MoveFlags::Up/Down mean
// forward/backward (not a raw world axis); Left/Right mean strafe.
// No entt/asio/pqxx/Vulkan/glm dependency — safe to unit test in isolation,
// and to call from both GameServer's simulate_movement() and the client's
// local prediction step. Hand-rolled sin/cos, not glm, to keep this one
// dependency-light (see tests/ — this is the header the Catch2 binary links
// without pulling in the heavier runtime deps).
// No diagonal normalization: each active flag independently contributes a
// full delta-length step along its local axis, so holding two directions is
// faster than one — a pre-existing, accepted simplification.
inline Vec2 apply_move_flags(Vec2 pos, uint8_t moveFlags, float delta, float yawRadians) {
  const float forwardX = std::sin(yawRadians);
  const float forwardZ = std::cos(yawRadians);
  const float rightX = std::cos(yawRadians);
  const float rightZ = -std::sin(yawRadians);

  float localForward = 0.0f; // +1 = forward, -1 = backward
  float localRight = 0.0f;   // +1 = right,   -1 = left
  if (moveFlags & MoveFlags::Up) localForward += 1.0f;
  if (moveFlags & MoveFlags::Down) localForward -= 1.0f;
  if (moveFlags & MoveFlags::Right) localRight += 1.0f;
  if (moveFlags & MoveFlags::Left) localRight -= 1.0f;

  pos.x += (localForward * forwardX + localRight * rightX) * delta;
  pos.z += (localForward * forwardZ + localRight * rightZ) * delta;
  return pos;
}

} // namespace Rebel
