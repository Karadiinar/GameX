add_executable(client src/main.cpp)

# Link to our shared code and graphics libraries
target_link_libraries(client PRIVATE 
    common 
    Vulkan::Vulkan 
    glfw 
    glm::glm
)