#include <iostream>
#include "Version.hpp"

int main() {
    std::cout << "--- REBEL CLIENT STARTING ---" << std::endl;
    std::cout << "Version: " << Rebel::VERSION_MAJOR << "." << Rebel::VERSION_MINOR << std::endl;
    
    // This is where Vulkan will eventually start
    return 0;
}