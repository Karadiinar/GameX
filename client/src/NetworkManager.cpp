#include "NetworkManager.hpp"
#include <iostream>

NetworkManager::NetworkManager() 
    : socket_(io_context_) {
    work_guard_ = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(asio::make_work_guard(io_context_));
    network_thread_ = std::thread([this]() { io_context_.run(); });
}

NetworkManager::~NetworkManager() {
    asio::error_code ec;
    socket_.close(ec);
    io_context_.stop();
    if (network_thread_.joinable()) network_thread_.join();
}

bool NetworkManager::connect(const std::string& host, const std::string& port) {
    try {
        tcp::resolver resolver(io_context_);
        auto endpoints = resolver.resolve(host, port);
        asio::connect(socket_, endpoints);
        std::cout << "[NETWORK] Connected to " << host << ":" << port << std::endl;
        startRead(); // Start the infinite reading loop
        return true;
    } catch (std::exception& e) {
        std::cerr << "[NETWORK] Connection failed: " << e.what() << std::endl;
        return false;
    }
}

void NetworkManager::startRead() {
    auto header = std::make_shared<Rebel::PacketHeader>();
    
    asio::async_read(socket_, asio::buffer(header.get(), sizeof(Rebel::PacketHeader)),
        [this, header](asio::error_code ec, std::size_t length) {
            if (!ec) {
                Rebel::Opcode opcode = static_cast<Rebel::Opcode>(header->opcode);

                // Look up the handler in our map
                auto it = handlers_.find(opcode);
                if (it != handlers_.end()) {
                    // Execute the function we found!
                    it->second(header);
                } else {
                    std::cout << "[NETWORK] No handler registered for Opcode: " << header->opcode << std::endl;
                }

                startRead(); // Keep listening for the next packet
            }
        });
}

void NetworkManager::sendPacket(const Rebel::PacketHeader& packet) {
    // We send the packet over the socket using a synchronous write for simplicity here,
    // though async_write is often preferred for high-performance engines.
    asio::error_code ec;
    asio::write(socket_, asio::buffer(&packet, sizeof(Rebel::PacketHeader)), ec);

    if (ec) {
        std::cerr << "[NETWORK] Failed to send packet: " << ec.message() << std::endl;
    }
}