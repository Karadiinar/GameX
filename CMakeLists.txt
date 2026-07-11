cmake_minimum_required(VERSION 3.22)
project(RebelMMO CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 1. Find our dependencies
find_package(Vulkan REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(EnTT REQUIRED)

# 2. Add our sub-projects
add_subdirectory(common)
add_subdirectory(server)
add_subdirectory(client)

# 3. Tests
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()