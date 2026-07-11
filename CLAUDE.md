# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

RebelMMO is an early-stage C++20 multiplayer game project with three executables sharing a common protocol library:

- **client** — GLFW window + hand-rolled Vulkan renderer, plus its own networking and game-logic threads.
- **LoginServer** (`server/login`) — ASIO TCP server that authenticates against Postgres (via libpqxx) and redirects clients to the GameServer.
- **GameServer** (`server/game`) — ASIO TCP server running a 20Hz authoritative world tick over an EnTT ECS registry.
- **common** (`common/`) — header-only interface library shared by all three: the wire protocol (`Protocol.hpp`), thread-affinity helpers (`ThreadUtility.hpp`), and version constants (`Version.hpp`).

Dependencies are managed via vcpkg (`vcpkg.json`): asio, glm, entt, nlohmann-json, libpqxx, catch2, argon2. Windowing/graphics use system GLFW and the Vulkan SDK (found via CMake `find_package(Vulkan REQUIRED)`, not vcpkg).

## Design philosophy

This project is deliberately going for an **old-school/vanilla-MMO architecture**, not a modern cloud-scaled one:

- **No sharding, no layering/phasing.** One `GameServer` process owns one persistent world with one `entt::registry`. Don't introduce realm-sharding, dynamic layering, or per-player world instances of the open world when designing network/world code — that's an explicit non-goal here, not an oversight.
- **Instancing exists, but only partially** — reserved for dungeons/raids (separate, bounded, group-scoped instances), not for the open world. When that gets built, expect it to be additional isolated `entt::registry` instances (or similar) spun up per group, not a generalization of the whole world into instances.
- **When a design question doesn't have an obvious answer from the code, default to how vanilla World of Warcraft did it** (client/server split with a login server handing off to a realm/world server, server-authoritative combat and movement validation, periodic character saves, group-scoped dungeon instances, etc.). That's the reference point for this project's intended shape, not a modern ECS-multiplayer-framework or MMO-as-a-service pattern.
- **This is a 3D game, third-person, WoW/FF14-style.** A real mouse-orbit camera, depth buffer, and view/projection pipeline exist now (see Vulkan renderer, below). **Coordinate convention: Y-up, ground plane = X/Z** (X=right, Y=up/height, Z=forward at yaw=0). This is a deliberate, permanent convention — don't reintroduce `y` as a ground-plane axis anywhere. Character models are still a camera-facing billboard quad (no real 3D meshes/animation yet) and the control scheme is the simplified first-pass version (character facing always matches camera yaw, no decoupled strafe-while-camera-independently-orbits mode) — both are known, intentional simplifications, not the final shape.

## Build and run

The project builds with CMake + the vcpkg toolchain file. `run.sh` is the author's one-shot dev workflow, but it's tailored to their machine (hardcoded vcpkg path, launches servers in separate `alacritty` terminal windows) — **it won't work in a headless/non-interactive session**. Prefer the raw commands below when working as Claude Code:

```bash
# Configure (first time or after CMakeLists changes)
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<path-to-vcpkg>/scripts/buildsystems/vcpkg.cmake

# Build everything
cmake --build build -j$(nproc)

# Resulting binaries:
#   build/client/client
#   build/server/login/LoginServer
#   build/server/game/GameServer
```

Shaders are precompiled SPIR-V checked into `client/shaders/` (`vert.spv`, `frag.spv`), rebuilt from `shader.vert`/`shader.frag` with `glslc`. The client copies `client/shaders/` next to its binary as a post-build step, so it must be run from a directory where that `shaders/` folder resolves (or from `client/`, matching what `run.sh` does).

**Running requires a local Postgres instance.** Both LoginServer and GameServer read `REBEL_DB_CONNSTR` (falls back to `dbname=rebelmmo user=rebel password=rebel host=127.0.0.1`) and expect the schema in `server/login/schema/*.sql` to already be applied — there's no migration tooling, apply the numbered `.sql` files by hand in order (`001_init.sql` for `accounts`, `002_sessions_and_characters.sql` for `sessions`/`characters`).

Startup order matters: LoginServer (port 54321) and GameServer (port 12345, hardcoded) must both be running before the client connects; the client always dials the LoginServer first and gets redirected.

There's a small, deliberately narrow Catch2 unit test suite (`tests/`) — see "Tests" below. There's still no linter or CI configured. When making changes, verify by building, running the relevant tests, and — for anything touching network/DB behavior — exercising the actual client/server flow, since the test suite intentionally doesn't cover that yet.

## Tests

```bash
# Build just the test binary
cmake --build build --target unit_tests -j$(nproc)

# Run everything
ctest --test-dir build --output-on-failure

# Run a single test by name, or by Catch2 tag
ctest --test-dir build -R "Diagonal input"
build/tests/unit_tests "[movement]"
```

`tests/` is intentionally narrow right now: `test_protocol.cpp` pins the wire-format struct sizes in `Protocol.hpp` (catches accidental wire-compat breaks), and `test_movement.cpp` covers the pure movement math in `common/include/Movement.hpp`. That header exists specifically so movement logic has *something* testable without linking entt/asio/pqxx/Vulkan into the test binary — when extracting more logic for testing, prefer the same pattern (a dependency-free header the real code calls into) over pulling heavy runtime deps into `tests/`. There's no DB/integration testing yet (Postgres round-trips, the auth/session flow) — that's a deliberate near-term gap, not an oversight; the manual verification approach (spin up the real servers, drive them with raw sockets) covers it for now.

## Architecture

### Wire protocol (`common/include/Protocol.hpp`)

All three executables share one packed binary protocol: a `PacketHeader{size, opcode}` followed by an opcode-specific payload struct (`MsgLogin`, `MsgRedirect`, `MsgPlayerMove`, `MsgGameAuth`, `MsgGameAuthOk`, `MsgPlayerState`, `MsgPlayerLeave`, `MsgChatSend`, `MsgChatRecv`). Opcodes are a single flat `enum class Opcode : uint16_t`; movement intent bits live in `Rebel::MoveFlags` (`Up`/`Down`/`Left`/`Right`); chat channels live in `Rebel::ChatChannel` (see Chat, below). This header is the actual contract between client and servers — changing a struct's layout or field order breaks wire compatibility across all three binaries simultaneously. Note that `MsgChatSend`/`MsgChatRecv` are the one exception to "fixed-size payload struct": both have a variable-length message trailing the struct, sized by `PacketHeader::size` rather than `sizeof()`.

### Auth / handoff flow

1. Client → LoginServer: `CMSG_AUTH_SESSION` + `MsgLogin` (username/password/version).
2. LoginServer checks `accounts.password_hash` in Postgres via real Argon2id verification (`argon2id_verify()`, from vcpkg's `argon2` port — CMake target `unofficial::argon2::libargon2`). `password_hash` is a self-describing PHC-format string (algorithm/version/params/salt all embedded), so verification needs nothing but that string and the candidate password — no separate salt column. On success it generates a random 32-hex-char session token (`generate_session_token()`), inserts it into the `sessions` table (`account_id`, 5-minute expiry), and replies `SMSG_AUTH_RESPONSE` + `MsgRedirect{ip, port, session_token}` pointing at the GameServer (currently hardcoded to `127.0.0.1:12345`), then closes the socket.
3. Client disconnects from LoginServer, reconnects to the redirect address, and sends `CMSG_GAME_AUTH` + `MsgGameAuth{session_token}` — the raw token only, not username/password again.
4. GameServer's `PlayerSession::handle_auth()` joins `sessions` → `accounts` on that token (rejecting if missing/expired), **deletes the session row immediately** so the token is single-use, then get-or-creates the account's **single character row** in Postgres (one character per account for now — there's no character-select screen yet) and loads `pos_x/pos_y/pos_z` from it. It spawns an EnTT entity (`PlayerData{name, character_id}` + `Position`) seeded with that loaded position, and replies `SMSG_GAME_AUTH_OK` (or `SMSG_GAME_AUTH_FAIL` + close on any failure) — no payload-size heuristics needed, these are now dedicated opcodes.

The seed account in `001_init.sql` (`Karadiinar` / `changeme`) stores a real Argon2id hash of that dev-only password — it's still meant purely for local development (matching the client's hardcoded dev login in `client/src/main.cpp`), just no longer stored as plaintext. There's no account-creation flow yet, so a new account currently means hand-generating a hash with `argon2id_hash_encoded()` (mirroring `verify_password_hash()`'s use of `argon2id_verify()`) and inserting it directly via SQL.

On the client, `NetworkManager::connect()` takes an optional `onConnected` callback instead of hardcoding what to send after connecting — `client/src/main.cpp` uses it to send the initial `MsgLogin` on the LoginServer connection and the `MsgGameAuth` token on the post-redirect GameServer connection. `LogicThreadEntry` distinguishes "got a redirect" (`SMSG_AUTH_RESPONSE` + `MsgRedirect`-sized payload) from "now in world" (`SMSG_GAME_AUTH_OK`) by opcode, not payload size.

### GameServer concurrency model (`server/game/src/main.cpp`)

A single global `entt::registry world` is shared across connections and is **not** thread-safe on its own. Safety comes entirely from funneling all registry access through one `asio::strand` (`game_strand`):
- Multiple worker threads (`hardware_concurrency()` of them) all call `io_context.run()`, so any given async handler can run on any thread.
- Per-session packet handling is posted onto `game_strand` before touching `world` (see `PlayerSession::on_packet_received`).
- The 20Hz world tick (`game_loop_tick`, self-rescheduling via `asio::steady_timer`) also runs bound to the same strand.
- `PlayerSession`'s destructor posts entity cleanup onto the strand too, since a session can be destroyed from any worker thread.

If you touch anything that reads/writes `world`, keep it on `game_strand` — don't add a mutex instead, and don't call registry methods from a handler that isn't posted through the strand.

**Database access follows the same rule.** There's a single global `pqxx::connection g_db`, opened once in `main()` against the same Postgres instance LoginServer uses (`REBEL_DB_CONNSTR`, same default connection string). `g_db` is only ever touched from code already posted to `game_strand` (auth-time character load/create in `handle_auth()`, disconnect-time save in `~PlayerSession()`, and the periodic autosave in `update_game_world()` — every `AUTOSAVE_INTERVAL_TICKS` ticks, currently 600 = ~30s at 20Hz, all online players' positions get written back). This mirrors how `world` is protected: the strand's serialization is what makes a single non-thread-safe connection safe to share, not a mutex. Queries are synchronous/blocking (`pqxx::work` + `exec_params`), same as LoginServer — acceptable for occasional auth/save calls, but be aware a slow query would stall the strand (and thus the whole tick and all packet processing) for its duration. Don't add per-request async DB pooling for this unless the project's scale genuinely calls for it — see Design philosophy above.

**Movement is server-authoritative and facing-relative.** `CMSG_PLAYER_MOVE`'s payload (`MsgPlayerMove{moveFlags, yaw}`) is intent, not a position — the client never asserts where it is. `process_packet` just writes the latest `moveFlags`/`yaw` into a `PlayerInput` component; `simulate_movement()` (called once per tick from `update_game_world`, so always on `game_strand`) is the *only* server code that mutates `Position` from movement, via the shared `Rebel::apply_move_flags(pos, moveFlags, delta, yawRadians)` (see `common/include/Movement.hpp` — same header the client's prediction step calls, below). **`yaw` now actively drives movement direction, not just cosmetics**: `MoveFlags::Up/Down` mean forward/backward and `Left/Right` mean strafe, all rotated by `yaw` (`forward(yaw) = (sin(yaw), cos(yaw))` in `(x,z)`, `right(yaw) = (cos(yaw), -sin(yaw))` — this is the one fixed convention, used everywhere a yaw becomes a direction, don't introduce a second one). The server still doesn't validate `yaw`'s rate of change — it's trusted as-is, just no longer purely decorative. After simulating, every tick calls `broadcast_player_states()`, which sends `SMSG_PLAYER_STATE{character_id, x, y, z, yaw}` to *every* connected session (via the `g_sessions` weak_ptr registry, populated on successful auth) — including the player's own state back to themselves, which is how the client tells "self" apart from others (`SMSG_GAME_AUTH_OK` carries `MsgGameAuthOk{character_id}` for exactly this). On disconnect, `~PlayerSession` broadcasts `SMSG_PLAYER_LEAVE{character_id}` before destroying the entity.

**Client-side prediction and remote-player tracking are implemented, in `LogicThreadEntry` (`client/src/main.cpp`):**
- **Facing:** `transform.yaw = renderer->getCameraYaw();` is set once per logic tick, before anything else — the character's facing always matches the camera's mouse-driven orbit yaw (see Vulkan renderer, below), the simplified first-pass control scheme.
- **Prediction:** every 64Hz logic tick, the client calls the exact same `Rebel::apply_move_flags()` the server uses (with `Rebel::PLAYER_SPEED_PER_SEC` scaled by the client's own tick `dt`, and the yaw set above) to move its local `Transform` immediately, instead of waiting on a round trip. When `SMSG_PLAYER_STATE` for `local_character_id` arrives, it hard-snaps `Transform` to the server's value — reconciliation with no smoothing/interpolation, by design for now. Under real latency this will show visible correction snaps; that's a known, acceptable limitation at this stage, not a bug.
- **Remote players:** a `character_id -> entt::entity` map (`remote_players`) tracks everyone else. `SMSG_PLAYER_STATE` for an unknown `character_id` creates a `Transform` + `RemotePlayerTag` entity on demand; `SMSG_PLAYER_LEAVE` destroys it and erases the map entry. There's no interpolation on remote players either — they jump to each new `SMSG_PLAYER_STATE` (the server broadcasts at 20Hz).

### Chat (`Rebel::ChatChannel` in `Protocol.hpp`, routing in `server/game/src/main.cpp`)

One opcode pair handles every channel: `CMSG_CHAT_SAY` + `MsgChatSend{channel, target}` (client → server) and `SMSG_CHAT_SAY` + `MsgChatRecv{channel, sender}` (server → client). Neither struct has a fixed message field — the chat text is every byte after the struct, up to `PacketHeader::size` (the same variable-payload trick every packet already supports, just used for the first time here). `Rebel::ChatChannel` currently has `Say`, `Yell`, `Whisper`, and a **reserved** `Guild`. Adding a real channel later (e.g. `Party` once grouping exists) means adding an enum value and a case in `broadcast_chat()` — no wire-format change.

`GameServer::broadcast_chat()` (called from `process_packet`'s `CMSG_CHAT_SAY` handler, using the sender's own `Position`) routes per channel:
- **Say** / **Yell** — range-limited broadcasts from the sender's world position (`SAY_RANGE = 15.0f`, `YELL_RANGE = 60.0f`; squared-distance check on `x,z` — the ground-plane axes under the Y-up convention, not `x,y` — against every session's `Position`, no `sqrt` needed). There's only one "zone" (no sharding — see Design philosophy), so Yell's larger radius is what stands in for "whole zone."
- **Whisper** — routed to exactly one session whose character name (`PlayerSession::getUsername()`) matches `MsgChatSend::target`; everyone else gets nothing.
- **Guild** — a documented no-op (`std::cerr`-logged, message dropped). There's no guild system — no table, no membership — so there's nobody to route it to yet. The channel exists on the wire specifically so adding a guild system later doesn't need a protocol change, only a real case in `broadcast_chat()`.

**Scope boundary:** the client only implements the *receive* side — `SMSG_CHAT_SAY` is parsed and printed to the console (`[Chat:Say] sender: message`). There's no in-client way to *send* chat yet, because there's no text-input UI at all in this renderer (no font/text rendering pipeline, no keyboard-to-string capture beyond the movement key flags) — building that is a separate, substantially bigger feature than the chat routing itself, and was deliberately left out of this pass. Verify the full send path (all four channels) via raw sockets, the same way GameServer/LoginServer features have been tested throughout this project.

### Client threading model (`client/src/main.cpp`, `NetworkManager`, `VulkanRenderer`)

Three independent loops, each pinned to a specific physical core where available (via `Rebel::ThreadUtils::GetPhysicalCoreMap`/`SetThreadAffinity` in `common/include/ThreadUtility.hpp`):

- **Main/render thread** — owns the GLFW window and `VulkanRenderer`, runs `pollEvents()`/`drawFrame()` in a tight loop.
- **NetworkManager's own thread** (`NetworkManager.cpp`) — dedicated to `io_context.run()`; all socket I/O is serialized through its own strand. Inbound packets are decoded and pushed onto a `Rebel::Concurrent::ThreadSafeQueue<InboundPacket>` for the logic thread to drain — there's no shared-mutable-state handoff for received data.
- **Logic thread** (`LogicThreadEntry`) — runs at a fixed 64Hz, owns its own client-side `entt::registry` (local player + remote players, see above), drains the inbound packet queue, applies input polled from `VulkanRenderer` (`isMovingLeft()` etc.), and sends `MsgPlayerMove` packets. Each tick it publishes a full snapshot of every known player (`std::vector<PlayerRenderState>`) to the render thread via `SharedRenderState` (mutex-guarded struct in `VulkanRenderer.hpp`, *not* `GraphicsConfig.hpp`), which is the only data the render thread reads back from logic.

When adding client gameplay logic, it belongs on the logic thread, not in `VulkanRenderer` — the renderer should stay limited to reading input state and `SharedRenderState`.

### Vulkan renderer (`client/src/VulkanRenderer.cpp`, ~1600 lines)

Standard manual Vulkan setup (instance → device → swapchain → render pass → pipeline → framebuffers → command buffers → sync objects), following the conventional tutorial-style pipeline order reflected in the method list on `VulkanRenderer`. Swapchain is torn down and recreated on both framebuffer-resize and fullscreen-toggle (`recreateSwapchain`), driven by `framebufferResized_` and the `WindowState` fullscreen tracking in `GraphicsConfig.hpp`.

**Real geometry: a camera-facing billboard quad per player**, not a hardcoded triangle. `Vertex{glm::vec2 pos, glm::vec2 uv}` (file-scope in `VulkanRenderer.cpp`, not the header) backs a host-visible/coherent `vertexBuffer_`/`indexBuffer_` (4 verts, 6 indices, uploaded once at init). `recordCommandBuffer` binds the vertex/index buffer and that frame's descriptor set **once**, before the per-player loop — only the push constant (`{x, y, z, isLocal}`, 4 floats) varies per player — then issues `vkCmdDrawIndexed(..., 6, ...)`. `rasterizer.cullMode` is `VK_CULL_MODE_NONE` (billboards have no meaningful back face, and `proj[1][1] *= -1` flips effective winding — disabling culling sidesteps a whole "nothing renders" failure class that isn't diagnosable without visual access).

**Texture pipeline** — same as before: `createTextureImage()` procedurally generates a 64×64 magenta/black checkerboard placeholder (no image-loading library, no real art), uploaded via a staging buffer + layout transitions. Deliberately generic (binding 0 = combined-image-sampler, not named after "player texture") so a future feature (a bitmap font atlas, for the still-unbuilt chat UI) can reuse the same shape.

**Real third-person camera**, WoW/FF14-style, driven by mouse input owned by `VulkanRenderer` (`cameraYaw_`, `cameraPitch_`, `cameraDistance_` — same ownership pattern as the existing `is_moving_left_` etc. key state): holding **right-mouse-button** captures the cursor (`GLFW_CURSOR_DISABLED`) and free-looks (`handleCursorPos`, pitch clamped `-45°/+85°`); releasing restores the cursor and the camera holds its orientation. Scroll wheel zooms (`handleScroll`, distance clamped `2.0-15.0` world units). `getCameraYaw()` is polled once per logic tick by `LogicThreadEntry` to drive character facing (see above) — the camera is the *source* of yaw, not a consumer of it.

**Depth buffer**: `depthImage_`/`depthImageView_` (format from `findDepthFormat()`, tried in order `D32_SFLOAT` → `D32_SFLOAT_S8_UINT` → `D24_UNORM_S8_UINT`), a second render-pass attachment, `VkPipelineDepthStencilStateCreateInfo` (`depthCompareOp=LESS`) in the pipeline. **Unlike the vertex/index/texture resources above, depth is swapchain-extent-dependent** — torn down/recreated in `cleanupSwapChain()`/`recreateSwapchain()` (in addition to `cleanup()`, since this file's `cleanup()` doesn't call `cleanupSwapChain()`, it inlines its own duplicate teardown — matched, not refactored).

**Camera matrices via a per-frame-in-flight UBO** — `CameraUBO{mat4 view; mat4 proj;}`, `MAX_FRAMES_IN_FLIGHT` (2) buffers, persistently mapped (`cameraUboMapped_`, mapped once at creation, unmapped in `cleanup()`). This is the one binding that changes every frame, which is why it needs **`descriptorSets_` (plural)** now — one full set per frame-in-flight, not the single shared set from the texture-only design, because a single shared UBO buffer would be a real read/write hazard once two frames are genuinely in flight. Binding 0 (texture) points at the same shared resource in every set; binding 1 (UBO) points at that slot's own buffer. Descriptor *bindings* are still written exactly once at init (`createDescriptorSets()`) — only the *data behind* binding 1 changes per-frame, via the mapped pointer, in `drawFrame()`:
```cpp
// per frame, before recordCommandBuffer():
ubo.view = glm::lookAt(camPos, target, {0,1,0});       // camPos = target + orbit offset from cameraYaw_/Pitch_/Distance_
ubo.proj = glm::perspective(glm::radians(60.0f), aspect, 0.1f, 100.0f);
ubo.proj[1][1] *= -1.0f;  // glm assumes GL clip space (Y up); Vulkan's is Y down
std::memcpy(cameraUboMapped_[currentFrame_], &ubo, sizeof(ubo));
```
`target` is the local player's world position (found by scanning the `players` snapshot for `isLocal`) plus a small height offset (chest/head level, not the ground origin); falls back to world origin if no local player yet (pre-spawn) — harmless.

**Billboarding** happens in `shader.vert`: it extracts the camera's world-space right/up axes from the view matrix's **rows** (not columns — the view matrix is the inverse of the camera's orientation, so rows hold its right/up axes; in GLSL's column-major `mat[col][row]` indexing that means fixing the row, varying the column) and offsets the quad's local `-0.5..0.5` corners along those axes before projecting. `shader.frag` samples `layout(binding=0) uniform sampler2D` and applies the `is_local` tint (unchanged in spirit) — its `PushConstants` block must stay byte-identical to the vertex shader's even though it never reads `player_pos`; `glslc` compiles stages independently and won't catch a mismatch here, only careful manual diffing will.

If you touch the push-constant layout or vertex/UV attribute layout, keep `VulkanRenderer.cpp` and both shader files in sync, then recompile with `glslc shader.vert -o vert.spv` / `glslc shader.frag -o frag.spv` (from `client/shaders/`). **Gotcha:** the `POST_BUILD` copy-shaders-to-output step only reruns when the `client` target itself rebuilds — a shader-only edit + `cmake --build` will silently run against stale `.spv` files in `build/`. Touch a `.cpp` file (or otherwise force the target to relink), or just always do a full `cmake --build build`.

**`LogicThreadEntry`'s render-bridge snapshot is now trivial**: every player's *raw* world-space `x,y,z` goes straight into `PlayerRenderState` (local player included, no longer pinned to a fake `(0,0)`) — the old `VIEW_SCALE` NDC-prescaling scheme is gone entirely, fully superseded by the GPU-side view/projection transform above. There's nothing left to precompute client-side; the renderer owns 100% of the camera transform now.

Explicitly still out of scope, worth knowing before extending further: no real art/asset loading (the checkerboard is 100% procedural, no stb_image or similar), no font/glyph rendering, no sprite variety (every player shares the one texture), no real 3D character meshes/animation (still a billboard), no decoupled camera-vs-character-facing, no vertical movement/jumping, no world geometry/collision/camera-collision. All noted, none built.

## Repo hygiene notes

- `build/` contains a committed CMake build tree (generated artifacts, including compiled `.o`/binaries). It's not authoritative — never hand-edit anything under it; regenerate via the build commands above.
- `project_context.md` (repo root) is a stale, regenerable full-source dump produced by `dump.sh`. It is not a source of truth — read the real files under `client/`, `server/`, `common/` instead.
- `compile_commands.json` at the repo root is a symlink to `build/compile_commands.json` (generated because `CMAKE_EXPORT_COMPILE_COMMANDS` is set in the root `CMakeLists.txt`). It's what lets clangd/editor tooling resolve the vcpkg/system include paths for this project — if it goes stale or missing after a fresh clone, just re-run the configure step above and re-create the symlink (`ln -sf build/compile_commands.json compile_commands.json`).
