#include "VeritasSync/P2PManager.h"

#include <iostream>
#include <sstream>

namespace VeritasSync {

P2PManager::P2PManager(unsigned short port)
    : m_socket(m_io_context, udp::endpoint(udp::v4(), port)) {
  m_thread = std::jthread([this]() {
    std::cout << "[P2P] IO context running in background thread..."
              << std::endl;
    m_io_context.run();
  });

  // 第一次启动接收
  start_receive();
}

void P2PManager::connect_to_peers(
    const std::vector<std::string>& peer_addresses) {
  udp::resolver resolver(m_io_context);
  for (const auto& addr_str : peer_addresses) {
    size_t colon_pos = addr_str.find(':');
    if (colon_pos == std::string::npos) continue;

    std::string host = addr_str.substr(0, colon_pos);
    std::string port = addr_str.substr(colon_pos + 1);

    boost::system::error_code ec;
    udp::resolver::results_type endpoints = resolver.resolve(host, port, ec);
    if (!ec && !endpoints.empty()) {
      udp::endpoint target_endpoint = *endpoints.begin();
      std::cout << "[P2P] Sending PING to " << target_endpoint << std::endl;
      send("PING", target_endpoint);
    }
  }
}

void P2PManager::start_receive() {
  auto remote_endpoint = std::make_shared<udp::endpoint>();
  auto recv_buffer = std::make_shared<std::array<char, 128>>();

  m_socket.async_receive_from(
      boost::asio::buffer(*recv_buffer), *remote_endpoint,
      [this, remote_endpoint, recv_buffer](
          const boost::system::error_code& error, std::size_t bytes) {
        // 当回调被触发时，使用的是捕获进来的局部变量，而不是共享的成员变量
        this->handle_receive(error, bytes, remote_endpoint, recv_buffer);
      });
}

void P2PManager::handle_receive(
    const boost::system::error_code& error, std::size_t bytes_transferred,
    std::shared_ptr<udp::endpoint> remote_endpoint,
    std::shared_ptr<std::array<char, 128>> recv_buffer) {
  if (!error) {
    std::string received_msg(recv_buffer->data(), bytes_transferred);
    std::cout << "[P2P] Received message '" << received_msg << "' from "
              << *remote_endpoint << std::endl;

    if (received_msg == "PING") {
      try {
        std::cout << "[P2P] Replying with PONG to " << *remote_endpoint
                  << std::endl;
        // 回复PONG时，使用的是这次接收到的、独立的 remote_endpoint
        m_socket.send_to(boost::asio::buffer("PONG"), *remote_endpoint);
      } catch (const std::exception& e) {
        std::cerr << "[P2P] Send PONG failed: " << e.what() << std::endl;
      }
    }
  }

  // 在所有处理完成后，再安全地准备下一次接收。
  // 这会创建一组全新的局部变量，与本次的完全无关。
  start_receive();
}

void P2PManager::send(const std::string& msg,
                      const udp::endpoint& target_endpoint) {
  auto message_to_send = std::make_shared<std::string>(msg);
  m_socket.async_send_to(
      boost::asio::buffer(*message_to_send), target_endpoint,
      [message_to_send](const boost::system::error_code&, std::size_t) {});
}

}