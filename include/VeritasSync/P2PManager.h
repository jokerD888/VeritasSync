#pragma once

#include <array>
#include <boost/asio.hpp>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace VeritasSync {
class StateManager;

using boost::asio::ip::udp;

class P2PManager {
 public:
  P2PManager(unsigned short port, StateManager& state_manager);
  void connect_to_peers(const std::vector<std::string>& peer_addresses);

 private:
  void start_receive();

  void handle_receive(const boost::system::error_code& error,
                      std::size_t bytes_transferred,
                      std::shared_ptr<udp::endpoint> remote_endpoint,
                      std::shared_ptr<std::array<char, 4096>> recv_buffer);

  void send(const std::string& msg, const udp::endpoint& target_endpoint);

  boost::asio::io_context m_io_context;
  udp::socket m_socket;
  std::jthread m_thread;
  StateManager& m_state_manager;

};
}