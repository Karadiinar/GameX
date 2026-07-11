# RebelMMO

A from-scratch, old-school MMO built in C++20 — third-person 3D, WoW/FF14-inspired, with a hand-rolled Vulkan renderer, a custom binary network protocol over ASIO, and a Postgres-backed account/character system. No game engine, no middleware — everything from the client's render loop to the server's tick loop is written by hand.

This is an early-stage hobby project, not a finished game. Expect placeholder art (a procedurally generated checkerboard texture stands in for real character models) and a short but growing feature list. It's built deliberately in the spirit of **vanilla-era MMOs**: one persistent world, no sharding, no server-side "layering" — just a login server handing off to a single game server, the same shape MMOs had before cloud-scale architecture became the default.

## What's actually in here right now

- **Accounts & login** — Postgres-backed accounts with real Argon2id password hashing, session-token handoff from the login server to the game server (single-use tokens, not resent credentials).
- **Server-authoritative movement** — the client only ever sends input intent (which keys are held, not a position); the server simulates and owns the real position. The client also predicts locally for responsiveness, then reconciles against the server's answer.
- **A real third-person 3D camera** — mouse-orbit camera behind the character (hold right-click to look around, scroll to zoom), depth-tested rendering, and facing-relative movement (forward/back/strafe relative to where the camera is looking), in the same spirit as WoW/FF14's default camera.
- **Chat** — Say, Yell, and Whisper channels with real range-limiting server-side; a Guild channel exists on the wire and is ready for a real guild system whenever one gets built.
- **Character persistence** — position is saved to Postgres on disconnect and periodically while playing.
- **A small automated test suite** — Catch2 tests for the pure movement math and the wire protocol's struct layout, run via CTest.

## What's not here yet

No real character models (a textured quad billboard stands in for a player right now), no world geometry, no combat, no guild system, no in-client chat UI (chat currently only prints to the console — there's no text-rendering pipeline yet), no character-select screen (one account = one character, by design, not a limitation waiting to be lifted).

## Tech stack

| Piece | What it uses |
|---|---|
| Rendering | Hand-written Vulkan (via the Vulkan SDK), GLFW for windowing, GLM for math |
| Networking | [ASIO](https://think-async.com/Asio/) (standalone, not Boost) over raw TCP with a custom binary protocol |
| Game state | [EnTT](https://github.com/skypjack/entt) (ECS) on both client and server |
| Database | PostgreSQL via [libpqxx](https://github.com/jtv/libpqxx) |
| Auth | [Argon2](https://github.com/P-H-C/phc-winner-argon2) (Argon2id) |
| Tests | [Catch2](https://github.com/catchorg/Catch2) via CTest |
| Build | CMake + [vcpkg](https://vcpkg.io/) for dependency management |

## Getting started

### Prerequisites

- A C++20 compiler and CMake 3.22+
- [vcpkg](https://vcpkg.io/) (dependencies are pulled automatically via `vcpkg.json` once you point CMake at your vcpkg toolchain file)
- The Vulkan SDK (found via CMake's system `find_package(Vulkan)`, not vcpkg) and a Vulkan-capable GPU/driver
- `glslc` (ships with the Vulkan SDK) for compiling shaders
- A local PostgreSQL instance

### Build

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<path-to-vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build -j$(nproc)
```

This produces three binaries: `build/server/login/LoginServer`, `build/server/game/GameServer`, and `build/client/client`.

### Database setup

Create a Postgres database and apply the schema files in order:

```bash
psql -d your_db -f server/login/schema/001_init.sql
psql -d your_db -f server/login/schema/002_sessions_and_characters.sql
```

By default the servers connect with `dbname=rebelmmo user=rebel password=rebel host=127.0.0.1`; override with the `REBEL_DB_CONNSTR` environment variable if your setup differs.

### Running it

Start both servers, then the client — in that order, since the client dials the login server first and gets redirected to the game server:

```bash
./build/server/login/LoginServer &
./build/server/game/GameServer &
cd client && ../build/client/client   # run from client/ so the shaders/ folder resolves
```

There's currently no login screen — the client connects with a hardcoded development account (`Karadiinar` / `changeme`, seeded by the schema). A real login UI is on the list of things to build.

**Controls**: WASD to move, hold right mouse button and drag to look around, scroll to zoom.

### Running the tests

```bash
cmake --build build --target unit_tests -j$(nproc)
ctest --test-dir build --output-on-failure
```

## Project layout

```
client/    Vulkan renderer, networking, input, and the client-side game loop
server/
  login/   Account auth, session-token issuance, schema files
  game/    The authoritative world simulation, movement, chat routing
common/    Shared wire protocol, movement math, and small utilities used by all three binaries
tests/     Catch2 unit tests (movement math, protocol struct sizes)
```

## A note for anyone poking around the code

There's a `CLAUDE.md` in the repo root — it's a working-notes file aimed at AI coding assistants (Claude Code specifically), documenting architecture decisions, known quirks, and in-progress work in a lot more implementation detail than this README goes into. If you want the deep dive on *why* something is built the way it is, that's the place to look.
