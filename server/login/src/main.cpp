#include <asio.hpp>
#include <iostream>
#include "Protocol.hpp"


using asio::ip::tcp;

void handle_login(tcp::socket socket) {
    try {
        Rebel::PacketHeader header;
        asio::error_code error;
        
        // 1. Read the Header
        asio::read(socket, asio::buffer(&header, sizeof(Rebel::PacketHeader)), error);

        // Consume any remaining payload so the socket is clean
        if (!error && header.size > sizeof(Rebel::PacketHeader)) {
            std::vector<uint8_t> drop(header.size - sizeof(Rebel::PacketHeader));
            asio::read(socket, asio::buffer(drop.data(), drop.size()), error);
        }

        if (!error && header.opcode == static_cast<uint16_t>(Rebel::Opcode::CMSG_AUTH_SESSION)) {
            std::cout << "[LOGIN] Auth request received. Sending redirect..." << std::endl;

            // 2. Prepare the Redirect Response
            Rebel::PacketHeader resHeader;
            resHeader.size = sizeof(Rebel::PacketHeader) + sizeof(Rebel::MsgRedirect);
            resHeader.opcode = static_cast<uint16_t>(Rebel::Opcode::SMSG_AUTH_RESPONSE);

            // --- THE PROPER FIX ---
            Rebel::MsgRedirect redirect;
            // Zero-fill the entire struct to wipe any "ghost" data in memory
            std::memset(&redirect, 0, sizeof(Rebel::MsgRedirect)); 

            // Copy the IP and manually ensure null termination within the 16-byte limit
            std::strncpy(redirect.ip, "127.0.0.1", sizeof(redirect.ip) - 1);
            redirect.port = 12345;
            // -----------------------

            // 3. Write them both to the socket
            std::vector<asio::const_buffer> buffers;
            buffers.push_back(asio::buffer(&resHeader, sizeof(resHeader)));
            buffers.push_back(asio::buffer(&redirect, sizeof(redirect)));
            
            asio::write(socket, buffers);
            
            // Log it for your own sanity
            std::cout << "[LOGIN] Redirected client to " << redirect.ip << ":" << redirect.port << std::endl;
        }
    } catch (std::exception& e) {
        std::cerr << "[LOGIN] Exception: " << e.what() << std::endl;
    }
}

int main() {
    asio::io_context io_context;
    // Login server usually sits on a different port, e.g., 54321
    tcp::acceptor acceptor(io_context, tcp::endpoint(tcp::v4(), 54321));

    std::cout << "--- REBEL LOGIN SERVER STARTING ON PORT 54321 ---" << std::endl;

    while (true) {
        tcp::socket socket(io_context);
        acceptor.accept(socket);
        handle_login(std::move(socket));
    }

    return 0;
}