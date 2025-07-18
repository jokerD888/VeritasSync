#pragma once

#include <array>
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace VeritasSync {
class StateManager;

using boost::asio::ip::udp;

// ���ڴ洢�ļ���Ľṹ��
struct FileChunk {
  std::string data;
};

class P2PManager {
 public:
  P2PManager(unsigned short port, StateManager& state_manager);
  void connect_to_peers(const std::vector<std::string>& peer_addresses);

 private:
  // ������������16KB�����ɸ�����ļ���
  static constexpr size_t MAX_UDP_PAYLOAD = 16384; 
  void start_receive();

  void handle_receive(const boost::system::error_code& error,
                      std::size_t bytes_transferred,
                      std::shared_ptr<udp::endpoint> remote_endpoint,
                      std::shared_ptr<std::array<char, MAX_UDP_PAYLOAD>> recv_buffer);

  void send(const std::string& msg, const udp::endpoint& target_endpoint);

  void handle_file_request(const std::string& file_path,const udp::endpoint& peer);
  void handle_file_chunk(const nlohmann::json& payload);

  boost::asio::io_context m_io_context;
  udp::socket m_socket;
  std::jthread m_thread;
  StateManager& m_state_manager;

  // ���������ļ��Ļ�����
  // Key: �ļ�·��, Value: <�ܿ���, <������, ����>>
  std::map<std::string, std::pair<int, std::map<int, FileChunk>>> m_file_assembly_buffer;

};
}