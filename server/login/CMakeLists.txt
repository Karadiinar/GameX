find_package(asio CONFIG REQUIRED)

add_executable(LoginServer src/main.cpp)

target_include_directories(LoginServer PRIVATE include)

# Link the common headers and asio
target_link_libraries(LoginServer 
    PRIVATE 
        common 
        asio::asio
)

target_compile_features(LoginServer PRIVATE cxx_std_20)