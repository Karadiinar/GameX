#!/bin/bash
# Move to the project root
cd "$(dirname "$0")"
PROJECT_ROOT=$(pwd)

BUILD_DIR="build"
# Absolute path to your vcpkg toolchain
VCPKG_PATH="/home/Karadiina/vcpkg/scripts/buildsystems/vcpkg.cmake"

# 1. THE NUCLEAR OPTION (Clean build every time)
echo "[Rebel] Nuking old build directory..."
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# 2. CONFIGURE
echo "[Rebel] Configuring with vcpkg..."
cmake .. -DCMAKE_TOOLCHAIN_FILE="$VCPKG_PATH"

# 3. COMPILE
echo "[Rebel] Compiling..."
make -j$(nproc) || exit 1

# 4. LAUNCH SERVERS (in background cosmic-terms)
echo "[Rebel] Launching Servers..."
alacritty -e bash -c "$PROJECT_ROOT/build/server/login/LoginServer; exec bash" &
alacritty -e bash -c "$PROJECT_ROOT/build/server/game/GameServer; exec bash" &
# Give the servers a second to bind to their ports
sleep 2

# === NEW SHADER COMPILATION STEP ===
echo "[Rebel] Compiling shaders to SPIR-V..."
glslc "$PROJECT_ROOT/client/shaders/shader.vert" -o "$PROJECT_ROOT/client/shaders/vert.spv"
glslc "$PROJECT_ROOT/client/shaders/shader.frag" -o "$PROJECT_ROOT/client/shaders/frag.spv"
# ===================================

# 5. LAUNCH CLIENT
echo "[Rebel] Starting Client..."
# Move to the source client folder so shaders/ are found
cd "$PROJECT_ROOT/client"
# Run the binary from the build folder
"$PROJECT_ROOT/build/client/client"

