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
    // Stage 1: Read the header to know the packet size
    auto headerOnly = std::make_shared<Rebel::PacketHeader>();
    asio::async_read(socket_, asio::buffer(headerOnly.get(), sizeof(Rebel::PacketHeader)),
        [this, headerOnly](asio::error_code ec, std::size_t length) {
            if (!ec) {
                uint16_t fullSize = headerOnly->size;
                // Prevent buffer underflow or invalid reads
                if (fullSize < sizeof(Rebel::PacketHeader)) return;
                
                // Stage 2: Allocate a buffer for the full packet and read the payload if it exists
                auto fullPacketData = std::shared_ptr<uint8_t[]>(new uint8_t[fullSize]);
                std::memcpy(fullPacketData.get(), headerOnly.get(), sizeof(Rebel::PacketHeader));

                asio::async_read(socket_, asio::buffer(fullPacketData.get() + sizeof(Rebel::PacketHeader), fullSize - sizeof(Rebel::PacketHeader)),
                    [this, fullPacketData](asio::error_code ec2, std::size_t length2) {
                        if (!ec2) {
                            auto packet = std::shared_ptr<Rebel::PacketHeader>(fullPacketData, reinterpret_cast<Rebel::PacketHeader*>(fullPacketData.get()));
                            Rebel::Opcode opcode = static_cast<Rebel::Opcode>(packet->opcode);

                            auto it = handlers_.find(opcode);
                            if (it != handlers_.end()) {
                                it->second(packet);
                            }
                            
                            // Stage 3: ONLY loop back once the entire packet (including payload) is processed
                            startRead();
                        }
                    });
            }
        });
}

void NetworkManager::sendPacket(const Rebel::PacketHeader& header, const void* payload, std::size_t payloadSize) {
    // Use a scatter-gather write to send the header and payload in one atomic operation
    std::vector<asio::const_buffer> buffers;
    buffers.push_back(asio::buffer(&header, sizeof(Rebel::PacketHeader)));
    
    if (payload && payloadSize > 0) {
        buffers.push_back(asio::buffer(payload, payloadSize));
    }

    asio::error_code ec;
    asio::write(socket_, buffers, ec);

    if (ec) {
        std::cerr << "[NETWORK] Failed to send packet: " << ec.message() << std::endl;
    }
}

void NetworkManager::disconnect() {
    asio::error_code ec;
    socket_.shutdown(tcp::socket::shutdown_both, ec);
    socket_.close(ec);
    
    // We don't stop the io_context_ here because we want 
    // the network_thread_ to stay alive for the next connection.
}