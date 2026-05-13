#include <iostream>
#include "Version.hpp" // Look! We are using our shared file

int main() {
    std::cout << "--- REBEL SERVER STARTING ---" << std::endl;
    std::cout << "Version: " << Rebel::VERSION_MAJOR << "." << Rebel::VERSION_MINOR << std::endl;
    
    // This is where our 20Hz loop will eventually go
    return 0;
}