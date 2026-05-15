#include "NetworkManager.hpp"
#include <iostream>

NetworkManager::NetworkManager()
    : io_context_(),
      strand_(asio::make_strand(io_context_)),
      work_guard_(std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(asio::make_work_guard(io_context_)))
{
    // The network thread is exclusively responsible for driving ASIO
    network_thread_ = std::thread([this]()
                                  { io_context_.run(); });
}

NetworkManager::~NetworkManager()
{
    disconnect();
    work_guard_.reset(); // Allow io_context_.run() to exit when work is done
    io_context_.stop();
    if (network_thread_.joinable())
    {
        network_thread_.join();
    }
}

void NetworkManager::connect(const std::string &host, const std::string &port)
{
    // Post the connection attempt to the strand so it executes safely on the network thread
    asio::post(strand_, [this, host, port]()
               {
        try {
            asio::ip::tcp::resolver resolver(io_context_);
            auto endpoints = resolver.resolve(host, port);
            
            socket_.emplace(io_context_);
            
            asio::async_connect(*socket_, endpoints, 
                asio::bind_executor(strand_, [this](std::error_code ec, asio::ip::tcp::endpoint) {
                    if (!ec) {
                        std::cout << "[Network] Connected to server.\n";
                        // 1. Prepare the Auth Payload
            Rebel::MsgLogin loginData;
            strncpy(loginData.username, "Karadiinar", 32);
            loginData.version = 1; // From your Version.hpp

            // 2. Prepare the Header
            Rebel::PacketHeader header;
            header.size = sizeof(Rebel::PacketHeader) + sizeof(Rebel::MsgLogin);
            header.opcode = static_cast<uint16_t>(Rebel::Opcode::CMSG_AUTH_SESSION);

            // 3. Send the Auth Packet immediately
            sendPacket(header, &loginData, sizeof(Rebel::MsgLogin));

            // 4. Now start listening for the server's response
            startRead();
                    } else {
                        std::cerr << "[Network] Connect error: " << ec.message() << "\n";
                    }
                }));
        } catch (const std::exception& e) {
            std::cerr << "[Network] Exception during connect: " << e.what() << "\n";
        } });
}

void NetworkManager::disconnect()
{
    asio::post(strand_, [this]()
               {
        if (socket_ && socket_->is_open()) {
            std::error_code ec;
            socket_->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            socket_->close(ec);
            socket_.reset();
            std::cout << "[Network] Disconnected.\n";
        } });
}

void NetworkManager::sendPacket(const Rebel::PacketHeader &header, const void *payload, std::size_t payloadSize)
{
    // We must copy the data before posting, because the caller (Logic Thread)
    // might destroy or modify the original buffers before the async write happens.
    auto buffer = std::make_shared<std::vector<uint8_t>>(sizeof(Rebel::PacketHeader) + payloadSize);
    std::memcpy(buffer->data(), &header, sizeof(Rebel::PacketHeader));
    if (payloadSize > 0 && payload != nullptr)
    {
        std::memcpy(buffer->data() + sizeof(Rebel::PacketHeader), payload, payloadSize);
    }

    // Post the write operation to the strand
    asio::post(strand_, [this, buffer]()
               {
        if (!socket_ || !socket_->is_open()) return;

        asio::async_write(*socket_, asio::buffer(*buffer),
            asio::bind_executor(strand_, [buffer](std::error_code ec, std::size_t /*length*/) {
                if (ec) {
                    std::cerr << "[Network] Write error: " << ec.message() << "\n";
                }
            })); });
}

void NetworkManager::startRead() {
    if (!socket_ || !socket_->is_open()) return;

    asio::async_read(*socket_, asio::buffer(&incoming_header_, sizeof(Rebel::PacketHeader)),
        asio::bind_executor(strand_, [this](std::error_code ec, std::size_t /*length*/) {
            if (!ec) {
                if (incoming_header_.size > sizeof(Rebel::PacketHeader)) {
                    // Calculate payload size (Total size - Header size)
                    readPayload(incoming_header_.size - sizeof(Rebel::PacketHeader));
                } else {
                    // No payload: Push header immediately to the Logic Thread queue
                    Rebel::InboundPacket pkg;
                    pkg.header = incoming_header_;
                    inbound_queue_.push(std::move(pkg));
                    
                    startRead(); // Wait for next packet
                }
            } else {
                if (ec != asio::error::operation_aborted) {
                    std::cerr << "[Network] Read header error: " << ec.message() << "\n";
                    disconnect();
                }
            }
        }));
}

void NetworkManager::readPayload(uint16_t payloadSize) {
    incoming_payload_.resize(payloadSize);

    asio::async_read(*socket_, asio::buffer(incoming_payload_.data(), payloadSize),
        asio::bind_executor(strand_, [this](std::error_code ec, std::size_t /*length*/) {
            if (!ec) {
                // Packet complete: Push to the Logic Thread queue
                Rebel::InboundPacket pkg;
                pkg.header = incoming_header_;
                pkg.payload = incoming_payload_;

                inbound_queue_.push(std::move(pkg));
                
                startRead(); // Wait for next packet
            } else {
                std::cerr << "[Network] Read payload error: " << ec.message() << "\n";
                disconnect();
            }
        }));
}