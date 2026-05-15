#pragma once
#include <asio.hpp>
#include <memory>
#include <thread>
#include <map>
#include <functional>
#include <vector>
#include <optional>
#include "Protocol.hpp"
#include "ThreadUtility.hpp" // For Rebel::Concurrent::ThreadSafeQueue

namespace Rebel
{
    // Wrapper for packets stored in the queue
    struct InboundPacket
    {
        PacketHeader header;
        std::vector<uint8_t> payload;
    };
}

// Inside NetworkManager.hpp
class NetworkManager
{
public:
    NetworkManager();
    ~NetworkManager();

    std::thread &getThread() { return network_thread_; }
    void connect(const std::string &host, const std::string &port);
    void disconnect();
    void sendPacket(const Rebel::PacketHeader &header, const void *payload = nullptr, std::size_t payloadSize = 0);

    // This is how the Logic Thread gets its data now
    Rebel::Concurrent::ThreadSafeQueue<Rebel::InboundPacket> &getPacketQueue()
    {
        return inbound_queue_;
    }

private:
    void startRead();
    void readPayload(uint16_t size);

    asio::io_context io_context_;
    asio::strand<asio::io_context::executor_type> strand_;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work_guard_;

    std::optional<asio::ip::tcp::socket> socket_;
    std::thread network_thread_;

    Rebel::PacketHeader incoming_header_;
    std::vector<uint8_t> incoming_payload_;

    // The mailbox replacing the handlers_ map
    Rebel::Concurrent::ThreadSafeQueue<Rebel::InboundPacket> inbound_queue_;
};
