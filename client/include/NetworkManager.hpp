#pragma once
#include <asio.hpp>
#include <thread>
#include <memory>
#include <functional>
#include <map>
#include "Protocol.hpp"

using asio::ip::tcp;

class NetworkManager {
public:
    NetworkManager();
    ~NetworkManager();

    bool connect(const std::string& host, const std::string& port);
    void disconnect();
    void sendPacket(const Rebel::PacketHeader& header, const void* payload = nullptr, std::size_t payloadSize = 0);
    void update(); // Optional: for polling events

    using PacketHandler = std::function<void(std::shared_ptr<Rebel::PacketHeader>)>;

void registerHandler(Rebel::Opcode opcode, PacketHandler handler) {
    handlers_[opcode] = handler;
}

private:
    void startRead();
    
    asio::io_context io_context_;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work_guard_;
    tcp::socket socket_;
    std::thread network_thread_;

std::map<Rebel::Opcode, PacketHandler> handlers_;

};
