add_executable(server src/main.cpp)

# Link to our shared code and our libraries
target_link_libraries(server PRIVATE common asio::asio EnTT::EnTT)