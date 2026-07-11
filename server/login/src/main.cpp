#include "Protocol.hpp"
#include "Version.hpp"
#include <argon2.h>
#include <asio.hpp>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <pqxx/pqxx>
#include <random>
#include <sstream>
#include <vector>

// accounts.password_hash stores a self-describing PHC-format Argon2id hash
// (algorithm/version/params/salt all embedded), so verification needs
// nothing but the encoded string itself and the candidate password.
bool verify_password_hash(const std::string &password,
                          const std::string &stored_hash) {
  int result = argon2id_verify(stored_hash.c_str(), password.data(), password.size());
  return result == ARGON2_OK;
}

// 32 hex characters == 16 random bytes, matching sessions.token CHAR(32)
// and MsgRedirect::session_token exactly (no null terminator needed).
std::string generate_session_token() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dist;
  std::ostringstream oss;
  oss << std::hex << std::setfill('0') << std::setw(16) << dist(gen)
      << std::setw(16) << dist(gen);
  return oss.str();
}

class LoginSession : public std::enable_shared_from_this<LoginSession> {
public:
  LoginSession(asio::ip::tcp::socket socket, pqxx::connection &db)
      : socket_(std::move(socket)), db_(db) {}

  void start() {
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
    asio::async_read(
        socket_, asio::buffer(&header_, sizeof(Rebel::PacketHeader)),
        [this, self](const asio::error_code &ec, std::size_t) {
          if (!ec) {
            read_payload();
          } else {
            std::cout << "[LOGIN] Client disconnected." << std::endl;
          }
        });
  }

  void read_payload() {
    auto self(shared_from_this());
    std::size_t payload_size = header_.size - sizeof(Rebel::PacketHeader);
    payload_.resize(payload_size);

    asio::async_read(socket_, asio::buffer(payload_.data(), payload_size),
                     [this, self](const asio::error_code &ec, std::size_t) {
                       if (!ec) {
                         process_login_request();
                       } else {
                         std::cout
                             << "[LOGIN] Payload read failed. Disconnecting."
                             << std::endl;
                       }
                     });
  }

  void process_login_request() {
    Rebel::Opcode opcode = static_cast<Rebel::Opcode>(header_.opcode);

    if (opcode != Rebel::Opcode::CMSG_AUTH_SESSION) {
      std::cerr << "[LOGIN] Unexpected opcode: 0x" << std::hex << (int)opcode
                << std::dec << std::endl;
      return;
    }

    if (payload_.size() < sizeof(Rebel::MsgLogin)) {
      std::cerr << "[LOGIN] Auth packet too small." << std::endl;
      socket_.close();
      return;
    }

    auto *msg = reinterpret_cast<Rebel::MsgLogin *>(payload_.data());
    std::cout << "[LOGIN] Received login request for '" << msg->username
              << "'. Authenticating..." << std::endl;

    if (!verify_credentials(msg->username, msg->password)) {
      std::cerr << "[LOGIN] Auth failed for '" << msg->username << "'."
                << std::endl;
      socket_.close();
      return;
    }

    std::cout << "[LOGIN] '" << msg->username << "' authenticated successfully."
              << std::endl;

    if (!issue_session_token()) {
      std::cerr << "[LOGIN] Failed to create session for '" << msg->username
                << "'." << std::endl;
      socket_.close();
      return;
    }

    send_login_response();
  }

  bool verify_credentials(const std::string &username,
                          const std::string &password) {
    try {
      pqxx::work txn(db_);
      auto result = txn.exec_params(
          "SELECT id, password_hash FROM accounts WHERE username = $1",
          username);

      if (result.empty()) {
        return false;
      }

      std::string stored_hash = result[0]["password_hash"].as<std::string>();
      if (!verify_password_hash(password, stored_hash)) {
        return false;
      }

      account_id_ = result[0]["id"].as<int>();
      return true;
    } catch (const std::exception &e) {
      std::cerr << "[LOGIN] DB error: " << e.what() << std::endl;
      return false;
    }
  }

  // Generates a short-lived, single-use token GameServer will trade for a
  // world entry instead of trusting a resent username/password.
  bool issue_session_token() {
    try {
      session_token_ = generate_session_token();
      pqxx::work txn(db_);
      txn.exec_params(
          "INSERT INTO sessions (token, account_id, expires_at) "
          "VALUES ($1, $2, now() + interval '5 minutes')",
          session_token_, account_id_);
      txn.commit();
      return true;
    } catch (const std::exception &e) {
      std::cerr << "[LOGIN] DB error creating session: " << e.what()
                << std::endl;
      return false;
    }
  }

  void send_login_response() {
    auto self(shared_from_this());
    auto full_packet = std::make_shared<std::vector<uint8_t>>();
    full_packet->resize(sizeof(Rebel::PacketHeader) +
                        sizeof(Rebel::MsgRedirect));

    auto *h = reinterpret_cast<Rebel::PacketHeader *>(full_packet->data());
    h->size = full_packet->size();
    h->opcode = static_cast<uint16_t>(Rebel::Opcode::SMSG_AUTH_RESPONSE);

    auto *r = reinterpret_cast<Rebel::MsgRedirect *>(
        full_packet->data() + sizeof(Rebel::PacketHeader));
    std::memset(r->ip, 0, 16);
    std::strncpy(r->ip, "127.0.0.1", 15);
    r->port = 12345;
    std::memcpy(r->session_token, session_token_.data(),
                sizeof(r->session_token));

    asio::async_write(
        socket_, asio::buffer(full_packet->data(), full_packet->size()),
        [this, self, full_packet](const asio::error_code &ec, std::size_t) {
          if (!ec) {
            std::cout << "[LOGIN] Redirect sent. Closing connection."
                      << std::endl;
            socket_.shutdown(asio::ip::tcp::socket::shutdown_both);
            socket_.close();
          }
        });
  }

  asio::ip::tcp::socket socket_;
  pqxx::connection &db_;
  Rebel::PacketHeader header_;
  std::vector<uint8_t> payload_;
  int account_id_ = -1;
  std::string session_token_;
};

void start_accept(asio::ip::tcp::acceptor &acceptor,
                  asio::io_context &io_context, pqxx::connection &db) {
  acceptor.async_accept(
      [&acceptor, &io_context, &db](const asio::error_code &error,
                                    asio::ip::tcp::socket socket) {
        if (!error) {
          std::make_shared<LoginSession>(std::move(socket), db)->start();
        }
        start_accept(acceptor, io_context, db);
      });
}

int main() {
  std::cout << "--- REBEL LOGIN SERVER STARTING ---" << std::endl;

  try {
    // TODO: pull from config/env instead of hardcoding once there's a
    // config layer. For now this just needs to point at a local dev DB.
    const char *conn_str = std::getenv("REBEL_DB_CONNSTR");
    std::string connection_string =
        conn_str ? conn_str
                 : "dbname=rebelmmo user=rebel password=rebel host=127.0.0.1";

    pqxx::connection db(connection_string);
    if (!db.is_open()) {
      std::cerr << "[LOGIN] Failed to open database connection." << std::endl;
      return 1;
    }
    std::cout << "[LOGIN] Connected to database: " << db.dbname() << std::endl;

    asio::io_context io_context;

    // Must match the client's hardcoded LoginServer port in client/src/main.cpp.
    asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), 54321);
    asio::ip::tcp::acceptor acceptor(io_context, endpoint);

    std::cout << "Login Server listening on port 54321..." << std::endl;
    start_accept(acceptor, io_context, db);

    io_context.run();
  } catch (const std::exception &e) {
    std::cerr << "[LOGIN] Exception: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
