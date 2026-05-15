#include <iostream>
#include <asio.hpp>
#include <memory>
#include <vector>
#include "Protocol.hpp"
#include "Version.hpp"

class LoginSession : public std::enable_shared_from_this<LoginSession> {
public:
    LoginSession(asio::ip::tcp::socket socket) : socket_(std::move(socket)) {}

    void start() {
        // Safe way to print remote endpoint
        asio::error_code ec;
        auto endpoint = socket_.remote_endpoint(ec);
        if (!ec) {
            std::cout << "[LOGIN] New auth attempt from: " << endpoint << std::endl;
        }
        read_header();
    }

private:
    void read_header() {
        auto self(shared_from_this());
        asio::async_read(socket_, asio::buffer(&header_, sizeof(Rebel::PacketHeader)),
            [this, self](const asio::error_code& ec, std::size_t) {
                if (!ec) {
                    process_login_request();
                } else {
                    std::cout << "[LOGIN] Client disconnected." << std::endl;
                }
            });
    }

    void process_login_request() {
        Rebel::Opcode opcode = static_cast<Rebel::Opcode>(header_.opcode);

        // Matching the client's CMSG_AUTH_SESSION opcode
        if (opcode == Rebel::Opcode::CMSG_AUTH_SESSION) {
            std::cout << "[LOGIN] Received login request. Authenticating..." << std::endl;
            send_login_response();
        } else {
            std::cerr << "[LOGIN] Unexpected opcode: 0x" << std::hex << (int)opcode << std::dec << std::endl;
        }
    }

    void send_login_response() {
        auto self(shared_from_this());
        
        // Match the client's expectation for SMSG_AUTH_RESPONSE + MsgRedirect
        auto full_packet = std::make_shared<std::vector<uint8_t>>();
        full_packet->resize(sizeof(Rebel::PacketHeader) + sizeof(Rebel::MsgRedirect));

        auto* h = reinterpret_cast<Rebel::PacketHeader*>(full_packet->data());
        h->size = full_packet->size();
        h->opcode = static_cast<uint16_t>(Rebel::Opcode::SMSG_AUTH_RESPONSE);

        auto* r = reinterpret_cast<Rebel::MsgRedirect*>(full_packet->data() + sizeof(Rebel::PacketHeader));
        std::memset(r->ip, 0, 16);
        std::strncpy(r->ip, "127.0.0.1", 15);
        r->port = 12345; // The Game Server Port

        asio::async_write(socket_, asio::buffer(full_packet->data(), full_packet->size()),
            [this, self, full_packet](const asio::error_code& ec, std::size_t) {
                if (!ec) {
                    std::cout << "[LOGIN] Redirect sent. Closing connection." << std::endl;
                    // Give the OS a tiny moment to flush the buffer before hard close
                    socket_.shutdown(asio::ip::tcp::socket::shutdown_both);
                    socket_.close(); 
                }
            });
    }

    asio::ip::tcp::socket socket_;
    Rebel::PacketHeader header_;
};

// FIXED: Explicitly passing the io_context to create the socket
void start_accept(asio::ip::tcp::acceptor& acceptor, asio::io_context& io_context) {
    acceptor.async_accept(
        [&acceptor, &io_context](const asio::error_code& error, asio::ip::tcp::socket socket) {
            if (!error) {
                std::make_shared<LoginSession>(std::move(socket))->start();
            }
            start_accept(acceptor, io_context);
        });
}

int main() {
    std::cout << "--- REBEL LOGIN SERVER STARTING ---" << std::endl;
    
    try {
        asio::io_context io_context;
        
        // Changed to 54321 to match your client's initial connection code
        asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), 54321);
        asio::ip::tcp::acceptor acceptor(io_context, endpoint);

        std::cout << "Login Server listening on port 54321..." << std::endl;
        start_accept(acceptor, io_context);

        io_context.run();
    } catch (std::exception& e) {
        std::cerr << "[LOGIN] Exception: " << e.what() << std::endl;
    }

    return 0;
}