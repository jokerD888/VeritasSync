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

// 用于存储文件块的结构体
struct FileChunk {
  std::string data;
};

// 封装了KCP对象和其他与该节点相关的状态
struct PeerContext {
  udp::endpoint endpoint;
  ikcpcb* kcp = nullptr;
  std::shared_ptr<P2PManager> p2p_manager_ptr;  // 一个指回P2PManager的指针

  PeerContext(udp::endpoint ep, std::shared_ptr<P2PManager> manager_ptr);
  ~PeerContext();
  void setup_kcp(uint32_t conv);
};

class StateManager; 

class P2PManager : public std::enable_shared_from_this<P2PManager> {
 public:
  boost::asio::io_context& get_io_context();
  // 用于安全创建 shared_ptr 的工厂方法
  static std::shared_ptr<P2PManager> create(unsigned short port);
  // 依赖注入
  void set_state_manager(StateManager* sm);
  static int kcp_output_callback(const char* buf, int len, ikcpcb* kcp,
                                 void* user);

  ~P2PManager();
  void connect_to_peers(const std::vector<std::string>& peer_addresses);

  // 公共方法，用于通过UDP发送原始数据 (由KCP回调函数使用)
  void raw_udp_send(const char* data, size_t len,
                    const udp::endpoint& endpoint);

  // 当文件系统发生变化时，StateManager将调用此方法
  void broadcast_current_state();

 private:
     // --- 成员变量 ---
  static constexpr size_t MAX_UDP_PAYLOAD = 16384;
  static constexpr size_t CHUNK_DATA_SIZE = 8192;
  // 构造函数设为私有，以强制通过工厂方法创建实例
  P2PManager(unsigned short port);
  void init();  // 构造后执行的初始化

  // --- 核心网络循环 ---
  void start_receive();
  void handle_receive(
      const boost::system::error_code& error, std::size_t bytes_transferred,
      std::shared_ptr<udp::endpoint> remote_endpoint,
      std::shared_ptr<std::array<char, MAX_UDP_PAYLOAD>> recv_buffer);

  // --- KCP 集成 ---
  void schedule_kcp_update();
  void update_all_kcps();

  std::shared_ptr<PeerContext> get_or_create_peer_context(
      const udp::endpoint& endpoint);

  // --- 上层应用逻辑 (由KCP调用) ---
  void send_over_kcp(const std::string& msg,
                     const udp::endpoint& target_endpoint);
  void handle_kcp_message(const std::string& msg,
                          const udp::endpoint& from_endpoint);

  // --- 具体的消息处理器 ---
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

  // 文件重组缓冲区
  std::map<std::string, std::pair<int, std::map<int, std::string>>>
      m_file_assembly_buffer;
};

}