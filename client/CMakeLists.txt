add_executable(client 
src/main.cpp
src/VulkanRenderer.cpp
)

target_include_directories(client PRIVATE include)


# Link to our shared code and graphics libraries
target_link_libraries(client PRIVATE 
    common 
    Vulkan::Vulkan 
    glfw 
    glm::glm
)