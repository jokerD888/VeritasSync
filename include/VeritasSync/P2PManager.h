#pragma once

#include <array>
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include <map>
#include <memory>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include <ikcp.h>

namespace VeritasSync {
class StateManager;
class P2PManager;

using boost::asio::ip::udp;

// ���ڴ洢�ļ���Ľṹ��
struct FileChunk {
  std::string data;
};

// ��װ��KCP�����������ýڵ���ص�״̬
struct PeerContext {
  udp::endpoint endpoint;
  ikcpcb* kcp = nullptr;
  std::shared_ptr<P2PManager> p2p_manager_ptr;  // һ��ָ��P2PManager��ָ��

  PeerContext(udp::endpoint ep, std::shared_ptr<P2PManager> manager_ptr);
  ~PeerContext();
  void setup_kcp(uint32_t conv);
};

class StateManager; 

class P2PManager : public std::enable_shared_from_this<P2PManager> {
 public:
  boost::asio::io_context& get_io_context();
  // ���ڰ�ȫ���� shared_ptr �Ĺ�������
  static std::shared_ptr<P2PManager> create(unsigned short port);
  // ����ע��
  void set_state_manager(StateManager* sm);
  static int kcp_output_callback(const char* buf, int len, ikcpcb* kcp,
                                 void* user);

  ~P2PManager();
  void connect_to_peers(const std::vector<std::string>& peer_addresses);

  // ��������������ͨ��UDP����ԭʼ���� (��KCP�ص�����ʹ��)
  void raw_udp_send(const char* data, size_t len,
                    const udp::endpoint& endpoint);

  // ���ļ�ϵͳ�����仯ʱ��StateManager�����ô˷���
  void broadcast_current_state();

 private:
     // --- ��Ա���� ---
  static constexpr size_t MAX_UDP_PAYLOAD = 16384;
  static constexpr size_t CHUNK_DATA_SIZE = 8192;
  // ���캯����Ϊ˽�У���ǿ��ͨ��������������ʵ��
  P2PManager(unsigned short port);
  void init();  // �����ִ�еĳ�ʼ��

  // --- ��������ѭ�� ---
  void start_receive();
  void handle_receive(
      const boost::system::error_code& error, std::size_t bytes_transferred,
      std::shared_ptr<udp::endpoint> remote_endpoint,
      std::shared_ptr<std::array<char, MAX_UDP_PAYLOAD>> recv_buffer);

  // --- KCP ���� ---
  void schedule_kcp_update();
  void update_all_kcps();

  std::shared_ptr<PeerContext> get_or_create_peer_context(
      const udp::endpoint& endpoint);

  // --- �ϲ�Ӧ���߼� (��KCP����) ---
  void send_over_kcp(const std::string& msg,
                     const udp::endpoint& target_endpoint);
  void handle_kcp_message(const std::string& msg,
                          const udp::endpoint& from_endpoint);

  // --- �������Ϣ������ ---
  void handle_share_state(const nlohmann::json& payload,
                          const udp::endpoint& from_endpoint);
  void handle_file_request(const nlohmann::json& payload,
                           const udp::endpoint& from_endpoint);
  void handle_file_chunk(const nlohmann::json& payload);



  boost::asio::io_context m_io_context;
  udp::socket m_socket;
  std::jthread m_thread;
  boost::asio::steady_timer m_kcp_update_timer;

  StateManager* m_state_manager = nullptr;

  std::map<udp::endpoint, std::shared_ptr<PeerContext>> m_peers;
  std::mutex m_peers_mutex;

  // �ļ����黺����
  std::map<std::string, std::pair<int, std::map<int, std::string>>>
      m_file_assembly_buffer;
};

}