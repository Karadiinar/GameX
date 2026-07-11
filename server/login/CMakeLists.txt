find_package(asio CONFIG REQUIRED)
find_package(libpqxx CONFIG REQUIRED)
find_package(unofficial-argon2 CONFIG REQUIRED)

add_executable(LoginServer src/main.cpp)

target_include_directories(LoginServer PRIVATE include)

# Link the common headers, asio, and Argon2 for real password verification
target_link_libraries(LoginServer PRIVATE common asio::asio libpqxx::pqxx unofficial::argon2::libargon2)
target_compile_features(LoginServer PRIVATE cxx_std_20)
