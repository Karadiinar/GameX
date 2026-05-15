# Find the necessary system threading and Asio
find_package(Threads REQUIRED)

add_executable(client 
    src/main.cpp
    src/VulkanRenderer.cpp
    src/NetworkManager.cpp
)

target_include_directories(client PRIVATE 
    include 
    ${CMAKE_SOURCE_DIR}/common/include
)

# Link to our shared code, graphics, and networking
target_link_libraries(client PRIVATE 
    common 
    Vulkan::Vulkan 
    glfw 
    glm::glm
    Threads::Threads
    asio  # Ensure your system's asio find_package or alias is defined
)

# Shader copy command remains the same
add_custom_command(
    TARGET client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders
    $<TARGET_FILE_DIR:client>/shaders
)