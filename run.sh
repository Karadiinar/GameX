#!/bin/bash
# Move to the project root
cd "$(dirname "$0")"

BUILD_DIR="build"
VCPKG_PATH="$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake"

# 1. CLEAN & CONFIGURE
# If the 'build/build' mess exists, wipe it all
if [ -d "$BUILD_DIR/build" ]; then
    echo "[Rebel] Cleaning up nested build mess..."
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "[Rebel] Configuring with vcpkg..."
# Use .. to ensure we look at the Project Root, not the current folder
cmake .. -DCMAKE_TOOLCHAIN_FILE="$VCPKG_PATH"

# 2. COMPILE
echo "[Rebel] Compiling..."
make -j$(nproc) || exit 1

# 3. LAUNCH SERVERS
# These paths are relative to the 'build' directory
echo "[Rebel] Launching Servers..."
cosmic-term -e bash -c "./server/login/LoginServer; exec bash" &
cosmic-term -e bash -c "./server/game/GameServer; exec bash" &

sleep 2

# 4. LAUNCH CLIENT
# We move into the client folder so 'shaders/' paths work
echo "[Rebel] Starting Client..."
cd client
./client