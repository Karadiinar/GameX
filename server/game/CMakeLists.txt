# server/game/CMakeLists.txt

# 1. Find the ASIO and libpqxx packages (vcpkg provides these)
find_package(asio CONFIG REQUIRED)
find_package(libpqxx CONFIG REQUIRED)

set(GAME_SERVER_SOURCES
    src/main.cpp
)

add_executable(GameServer ${GAME_SERVER_SOURCES})

target_include_directories(GameServer PRIVATE include)

# asio::asio is the standard target name for the header-only version
target_link_libraries(GameServer
    PRIVATE
        common
        asio::asio
        libpqxx::pqxx
)

target_compile_features(GameServer PRIVATE cxx_std_20)