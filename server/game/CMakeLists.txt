# server/game/CMakeLists.txt

# 1. Find the ASIO package (vcpkg provides this)
find_package(asio CONFIG REQUIRED)

set(GAME_SERVER_SOURCES
    src/main.cpp
)

add_executable(GameServer ${GAME_SERVER_SOURCES})

target_include_directories(GameServer PRIVATE include)

# 2. Link asio AND your common interface
# asio::asio is the standard target name for the header-only version
target_link_libraries(GameServer 
    PRIVATE 
        common 
        asio::asio
)

target_compile_features(GameServer PRIVATE cxx_std_20)