#include "VeritasSync/P2PManager.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "VeritasSync/Hashing.h"  // <-- 需要 Hashing
#include "VeritasSync/Protocol.h"
#include "VeritasSync/StateManager.h"
#include "VeritasSync/SyncManager.h"

#define BUFFERSIZE 8192

#include <b64/decode.h>
#include <b64/encode.h>

namespace VeritasSync {

//================================================================================
// PeerContext 实现
//================================================================================
PeerContext::PeerContext(udp::endpoint ep,
                         std::shared_ptr<P2PManager> manager_ptr)
    : endpoint(std::move(ep)), p2p_manager_ptr(std::move(manager_ptr)) {}

PeerContext::~PeerContext() {
  if (kcp) {
    ikcp_release(kcp);
    kcp = nullptr;
  }
}

void PeerContext::setup_kcp(uint32_t conv) {
  kcp = ikcp_create(conv, this);
  kcp->output = &P2PManager::kcp_output_callback;
  ikcp_nodelay(kcp, 1, 10, 2, 1);
  ikcp_wndsize(kcp, 256, 256);
}

//================================================================================
// P2PManager 实现
//================================================================================

boost::asio::io_context& P2PManager::get_io_context() { return m_io_context; }

// --- 增量更新：全量广播 (用于启动) ---
void P2PManager::broadcast_current_state() {
  if (m_role != SyncRole::Source) return;
  if (!m_state_manager) return;

  std::cout << "[P2P] (Source) 文件系统发生变化，正在向所有节点广播最新状态..."
            << std::endl;

  m_state_manager->scan_directory();
  std::string json_state = m_state_manager->get_state_as_json_string();

  std::vector<udp::endpoint> endpoints;
  {
    std::lock_guard<std::mutex> lock(m_peers_mutex);
    for (const auto& pair : m_peers) {
      endpoints.push_back(pair.first);
    }
  }

  for (const auto& endpoint : endpoints) {
    send_over_kcp(json_state, endpoint);
  }
}

// --- 增量更新：广播单个文件更新 ---
void P2PManager::broadcast_file_update(const FileInfo& file_info) {
  if (m_role != SyncRole::Source) return;

  std::cout << "[P2P] (Source) 广播增量更新: " << file_info.path << std::endl;

  nlohmann::json msg;
  msg[Protocol::MSG_TYPE] = Protocol::TYPE_FILE_UPDATE;
  msg[Protocol::MSG_PAYLOAD] = file_info;  // 利用 to_json 自动转换

  std::vector<udp::endpoint> endpoints;
  {
    std::lock_guard<std::mutex> lock(m_peers_mutex);
    for (const auto& pair : m_peers) {
      endpoints.push_back(pair.first);
    }
  }

  for (const auto& endpoint : endpoints) {
    send_over_kcp(msg.dump(), endpoint);
  }
}

// --- 增量更新：广播单个文件删除 ---
void P2PManager::broadcast_file_delete(const std::string& relative_path) {
  if (m_role != SyncRole::Source) return;

  std::cout << "[P2P] (Source) 广播增量删除: " << relative_path << std::endl;

  nlohmann::json msg;
  msg[Protocol::MSG_TYPE] = Protocol::TYPE_FILE_DELETE;
  msg[Protocol::MSG_PAYLOAD] = {{"path", relative_path}};

  std::vector<udp::endpoint> endpoints;
  {
    std::lock_guard<std::mutex> lock(m_peers_mutex);
    for (const auto& pair : m_peers) {
      endpoints.push_back(pair.first);
    }
  }

  for (const auto& endpoint : endpoints) {
    send_over_kcp(msg.dump(), endpoint);
  }
}

// --- 静态工厂与构造函数 ---
std::shared_ptr<P2PManager> P2PManager::create(unsigned short port) {
  struct P2PManagerMaker : public P2PManager {
    P2PManagerMaker(unsigned short p) : P2PManager(p) {}
  };
  auto manager = std::make_shared<P2PManagerMaker>(port);
  manager->init();
  return manager;
}

P2PManager::P2PManager(unsigned short port)
    : m_socket(m_io_context, udp::endpoint(udp::v4(), port)),
      m_kcp_update_timer(m_io_context) {}

void P2PManager::set_state_manager(StateManager* sm) { m_state_manager = sm; }

void P2PManager::set_role(SyncRole role) { m_role = role; }

void P2PManager::init() {
  m_thread = std::jthread([this]() {
    std::cout << "[P2P] IO context 在后台线程运行..." << std::endl;
    auto work_guard = boost::asio::make_work_guard(m_io_context);
    m_io_context.run();
  });
  start_receive();
  schedule_kcp_update();
}

P2PManager::~P2PManager() {
  m_io_context.stop();
  if (m_thread.joinable()) {
    m_thread.join();
  }
}

// --- KCP 核心集成 ---
int P2PManager::kcp_output_callback(const char* buf, int len, ikcpcb* kcp,
                                    void* user) {
  PeerContext* context = static_cast<PeerContext*>(user);
  if (context && context->p2p_manager_ptr) {
    context->p2p_manager_ptr->raw_udp_send(buf, len, context->endpoint);
  }
  return 0;
}

void P2PManager::schedule_kcp_update() {
  m_kcp_update_timer.expires_after(std::chrono::milliseconds(10));
  m_kcp_update_timer.async_wait(
      [self = shared_from_this()](const boost::system::error_code& ec) {
        if (!ec) {
          self->update_all_kcps();
        }
      });
}

// --- 回退：移除解密 ---
void P2PManager::update_all_kcps() {
  auto current_time_ms =
      (IUINT32)std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();

  std::vector<std::pair<std::string, udp::endpoint>> received_messages;

  {
    std::lock_guard<std::mutex> lock(m_peers_mutex);
    for (auto const& [endpoint, context] : m_peers) {
      ikcp_update(context->kcp, current_time_ms);

      char buffer[MAX_UDP_PAYLOAD];
      int size;
      while ((size = ikcp_recv(context->kcp, buffer, sizeof(buffer))) > 0) {
        // 直接存入明文
        received_messages.emplace_back(std::string(buffer, size), endpoint);
      }
    }
  }

  for (const auto& msg_pair : received_messages) {
    handle_kcp_message(msg_pair.first, msg_pair.second);
  }

  schedule_kcp_update();
}

// --- 原始网络 I/O ---
void P2PManager::raw_udp_send(const char* data, size_t len,
                              const udp::endpoint& endpoint) {
  m_socket.async_send_to(boost::asio::buffer(data, len), endpoint,
                         [](const boost::system::error_code&, std::size_t) {});
}

void P2PManager::connect_to_peers(
    const std::vector<std::string>& peer_addresses) {
  udp::resolver resolver(m_io_context);
  for (const auto& addr_str : peer_addresses) {
    size_t colon_pos = addr_str.find(':');
    if (colon_pos == std::string::npos) continue;

    std::string host = addr_str.substr(0, colon_pos);
    std::string port_str = addr_str.substr(colon_pos + 1);

    boost::system::error_code ec;
    udp::resolver::results_type endpoints =
        resolver.resolve(host, port_str, ec);
    if (!ec && !endpoints.empty()) {
      udp::endpoint target_endpoint = *endpoints.begin();
      std::cout << "[P2P] 正在向 " << target_endpoint << " 发送PING以进行握手。"
                << std::endl;
      get_or_create_peer_context(target_endpoint);
      raw_udp_send("PING", 4, target_endpoint);
    }
  }
}

// --- 核心网络接收循环 ---
void P2PManager::start_receive() {
  auto remote_endpoint = std::make_shared<udp::endpoint>();
  auto recv_buffer = std::make_shared<std::array<char, MAX_UDP_PAYLOAD>>();

  m_socket.async_receive_from(
      boost::asio::buffer(*recv_buffer), *remote_endpoint,
      [self = shared_from_this(), remote_endpoint, recv_buffer](
          const boost::system::error_code& error, std::size_t bytes) {
        self->handle_receive(error, bytes, remote_endpoint, recv_buffer);
      });
}

void P2PManager::handle_receive(
    const boost::system::error_code& error, std::size_t bytes_transferred,
    std::shared_ptr<udp::endpoint> remote_endpoint,
    std::shared_ptr<std::array<char, MAX_UDP_PAYLOAD>> recv_buffer) {
  if (!error && bytes_transferred > 0) {
    if (bytes_transferred == 4 &&
        std::string(recv_buffer->data(), 4) == "PING") {
      std::cout << "[P2P] 收到来自 " << *remote_endpoint
                << " 的 PING 握手请求。" << std::endl;
      auto peer_context = get_or_create_peer_context(*remote_endpoint);

      if (m_role == SyncRole::Source) {
        std::cout
            << "[KCP] (Source) 对方已准备就绪，通过KCP发送我们的文件状态..."
            << std::endl;
        m_state_manager->scan_directory();
        std::string json_state = m_state_manager->get_state_as_json_string();
        send_over_kcp(json_state, *remote_endpoint);
      } else {
        std::cout << "[KCP] (Destination) 已收到 PING，等待 Source 状态。"
                  << std::endl;
      }
    } else {
      auto peer_context = get_or_create_peer_context(*remote_endpoint);
      ikcp_input(peer_context->kcp, recv_buffer->data(), bytes_transferred);
    }
  }
  start_receive();
}

// --- 对等点管理 ---
std::shared_ptr<PeerContext> P2PManager::get_or_create_peer_context(
    const udp::endpoint& endpoint) {
  std::lock_guard<std::mutex> lock(m_peers_mutex);
  auto it = m_peers.find(endpoint);
  if (it != m_peers.end()) {
    return it->second;
  }

  std::cout << "[KCP] 检测到新的对等点，为其创建KCP上下文: " << endpoint
            << std::endl;
  auto new_context =
      std::make_shared<PeerContext>(endpoint, shared_from_this());
  uint32_t conv = 12345;
  new_context->setup_kcp(conv);
  m_peers[endpoint] = new_context;
  return new_context;
}

// --- 上层应用消息处理 ---

// --- 回退：移除加密 ---
void P2PManager::send_over_kcp(const std::string& msg,
                               const udp::endpoint& target_endpoint) {
  std::lock_guard<std::mutex> lock(m_peers_mutex);
  auto it = m_peers.find(target_endpoint);
  if (it != m_peers.end()) {
    // 发送明文
    ikcp_send(it->second->kcp, msg.c_str(), msg.length());
  } else {
    std::cerr << "[KCP] 错误: 尝试向一个未建立KCP上下文的对等点发送消息: "
              << target_endpoint << std::endl;
  }
}

// --- 增量更新：处理所有消息类型 ---
void P2PManager::handle_kcp_message(const std::string& msg,
                                    const udp::endpoint& from_endpoint) {
  try {
    auto json = nlohmann::json::parse(msg);
    const std::string msg_type = json.at(Protocol::MSG_TYPE).get<std::string>();
    auto& payload = json.at(Protocol::MSG_PAYLOAD);

    if (msg_type == Protocol::TYPE_SHARE_STATE &&
        m_role == SyncRole::Destination) {
      handle_share_state(payload, from_endpoint);
    } else if (msg_type == Protocol::TYPE_FILE_UPDATE &&
               m_role == SyncRole::Destination) {
      handle_file_update(payload, from_endpoint);
    } else if (msg_type == Protocol::TYPE_FILE_DELETE &&
               m_role == SyncRole::Destination) {
      handle_file_delete(payload, from_endpoint);
    } else if (msg_type == Protocol::TYPE_REQUEST_FILE &&
               m_role == SyncRole::Source) {
      handle_file_request(payload, from_endpoint);
    } else if (msg_type == Protocol::TYPE_FILE_CHUNK &&
               m_role == SyncRole::Destination) {
      handle_file_chunk(payload);
    }

  } catch (const std::exception& e) {
    std::cerr << "[P2P] 处理KCP消息时发生错误: " << e.what() << std::endl;
    std::cerr << "       原始消息: " << msg << std::endl;
  }
}

// --- 增量更新：处理全量状态 ---
void P2PManager::handle_share_state(const nlohmann::json& payload,
                                    const udp::endpoint& from_endpoint) {
  if (m_role != SyncRole::Destination) return;

  std::cout << "[KCP] (Destination) 收到来自 " << from_endpoint
            << " (Source) 的 'share_state' 消息。" << std::endl;

  std::vector<FileInfo> remote_files =
      payload.at("files").get<std::vector<FileInfo>>();

  m_state_manager->scan_directory();
  nlohmann::json temp_json =
      nlohmann::json::parse(m_state_manager->get_state_as_json_string());
  std::vector<FileInfo> local_files = temp_json.at(Protocol::MSG_PAYLOAD)
                                          .at("files")
                                          .get<std::vector<FileInfo>>();

  SyncActions actions =
      SyncManager::compare_states_and_get_requests(local_files, remote_files);

  if (!actions.files_to_delete.empty()) {
    std::cout << "[Sync] 计划删除 " << actions.files_to_delete.size()
              << " 个本地多余的文件。" << std::endl;
    for (const auto& file_path_str : actions.files_to_delete) {
      std::filesystem::path relative_path(
          reinterpret_cast<const char8_t*>(file_path_str.c_str()));
      std::filesystem::path full_path =
          m_state_manager->get_root_path() / relative_path;

      std::error_code ec;
      if (std::filesystem::remove(full_path, ec)) {
        std::cout << "[Sync] -> 已删除: " << full_path.string() << std::endl;
      } else if (ec != std::errc::no_such_file_or_directory) {
        std::cerr << "[Sync] -> 删除失败: " << full_path.string()
                  << " Error: " << ec.message() << std::endl;
      }
    }
  }

  if (!actions.files_to_request.empty()) {
    std::cout << "[KCP] 计划向 " << from_endpoint << " (Source) 请求 "
              << actions.files_to_request.size() << " 个缺失/过期的文件。"
              << std::endl;
    for (const auto& file_path : actions.files_to_request) {
      nlohmann::json request_msg;
      request_msg[Protocol::MSG_TYPE] = Protocol::TYPE_REQUEST_FILE;
      request_msg[Protocol::MSG_PAYLOAD] = {{"path", file_path}};
      send_over_kcp(request_msg.dump(), from_endpoint);
    }
  }
}

// --- 增量更新：处理单个文件更新 ---
void P2PManager::handle_file_update(const nlohmann::json& payload,
                                    const udp::endpoint& from_endpoint) {
  if (m_role != SyncRole::Destination) return;

  FileInfo remote_info;
  try {
    remote_info = payload.get<FileInfo>();  // 利用 from_json 自动转换
  } catch (const std::exception& e) {
    std::cerr << "[KCP] (Destination) 解析 file_update 失败: " << e.what()
              << std::endl;
    return;
  }

  std::cout << "[KCP] (Destination) 收到增量更新: " << remote_info.path
            << std::endl;

  std::filesystem::path relative_path(
      reinterpret_cast<const char8_t*>(remote_info.path.c_str()));
  std::filesystem::path full_path =
      m_state_manager->get_root_path() / relative_path;

  std::error_code ec;
  bool should_request = false;

  if (!std::filesystem::exists(full_path, ec) || ec) {
    std::cout << "[Sync] -> 本地不存在, 需要请求。" << std::endl;
    should_request = true;
  } else {
    std::string local_hash = Hashing::CalculateSHA256(full_path);
    if (local_hash != remote_info.hash) {
      std::cout << "[Sync] -> 哈希不匹配 (本地: " << local_hash.substr(0, 7)
                << " vs 远程: " << remote_info.hash.substr(0, 7)
                << "), 需要请求。" << std::endl;
      should_request = true;
    } else {
      std::cout << "[Sync] -> 哈希匹配, 已是最新。" << std::endl;
    }
  }

  if (should_request) {
    nlohmann::json request_msg;
    request_msg[Protocol::MSG_TYPE] = Protocol::TYPE_REQUEST_FILE;
    request_msg[Protocol::MSG_PAYLOAD] = {{"path", remote_info.path}};
    send_over_kcp(request_msg.dump(), from_endpoint);
  }
}

// --- 增量更新：处理单个文件删除 ---
void P2PManager::handle_file_delete(const nlohmann::json& payload,
                                    const udp::endpoint& from_endpoint) {
  if (m_role != SyncRole::Destination) return;

  std::string relative_path_str;
  try {
    relative_path_str = payload.at("path").get<std::string>();
  } catch (const std::exception& e) {
    std::cerr << "[KCP] (Destination) 解析 file_delete 失败: " << e.what()
              << std::endl;
    return;
  }

  std::cout << "[KCP] (Destination) 收到增量删除: " << relative_path_str
            << std::endl;

  std::filesystem::path relative_path(
      reinterpret_cast<const char8_t*>(relative_path_str.c_str()));
  std::filesystem::path full_path =
      m_state_manager->get_root_path() / relative_path;

  std::error_code ec;
  if (std::filesystem::remove(full_path, ec)) {
    std::cout << "[Sync] -> 已删除本地文件: " << full_path.string()
              << std::endl;
    m_state_manager->remove_path_from_map(relative_path_str);
  } else {
    // 修复：使用 ec != ... 来正确比较
    if (ec != std::errc::no_such_file_or_directory) {
      std::cerr << "[Sync] -> 删除本地文件失败: " << full_path.string()
                << " Error: " << ec.message() << std::endl;
    } else {
      std::cout << "[Sync] -> 本地文件已不存在, 无需操作。" << std::endl;
    }
  }
}

// --- handle_file_request 和 handle_file_chunk 不变 ---
void P2PManager::handle_file_request(const nlohmann::json& payload,
                                     const udp::endpoint& from_endpoint) {
  // ... (此函数不变) ...
  const std::string requested_path_str = payload.at("path").get<std::string>();
  std::cout << "[KCP] 收到来自 " << from_endpoint << " 对文件 '"
            << requested_path_str << "' 的请求。" << std::endl;

  std::filesystem::path relative_path(
      reinterpret_cast<const char8_t*>(requested_path_str.c_str()));
  std::filesystem::path full_path =
      m_state_manager->get_root_path() / relative_path;

  if (!std::filesystem::exists(full_path)) {
    std::cerr << "[P2P] 被请求的文件不存在: " << full_path << std::endl;
    return;
  }

  std::ifstream file(full_path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    std::cerr << "[P2P] 无法打开文件: " << full_path << std::endl;
    return;
  }

  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);

  if (size == 0) {
    std::cout << "[KCP] 正在发送零字节文件 '" << requested_path_str
              << "' 的元信息..." << std::endl;
    nlohmann::json chunk_msg;
    chunk_msg[Protocol::MSG_TYPE] = Protocol::TYPE_FILE_CHUNK;
    chunk_msg[Protocol::MSG_PAYLOAD] = {{"path", requested_path_str},
                                        {"chunk_index", 0},
                                        {"total_chunks", 1},
                                        {"data", ""}};
    send_over_kcp(chunk_msg.dump(), from_endpoint);
    return;
  }

  int total_chunks =
      static_cast<int>((size + CHUNK_DATA_SIZE - 1) / CHUNK_DATA_SIZE);
  std::vector<char> buffer(CHUNK_DATA_SIZE);

  std::cout << "[KCP] 正在将文件 '" << requested_path_str << "' (" << size
            << " 字节) 分成 " << total_chunks << " 块发送给 " << from_endpoint
            << std::endl;

  for (int i = 0; i < total_chunks; ++i) {
    file.read(buffer.data(), CHUNK_DATA_SIZE);
    std::streamsize bytes_read = file.gcount();

    std::stringstream raw_data_stream;
    raw_data_stream.write(buffer.data(), bytes_read);
    std::stringstream encoded_stream;
    base64::encoder E;
    E.encode(raw_data_stream, encoded_stream);

    nlohmann::json chunk_msg;
    chunk_msg[Protocol::MSG_TYPE] = Protocol::TYPE_FILE_CHUNK;
    chunk_msg[Protocol::MSG_PAYLOAD] = {{"path", requested_path_str},
                                        {"chunk_index", i},
                                        {"total_chunks", total_chunks},
                                        {"data", encoded_stream.str()}};
    send_over_kcp(chunk_msg.dump(), from_endpoint);
  }
}

void P2PManager::handle_file_chunk(const nlohmann::json& payload) {
  // ... (此函数不变) ...
  std::string file_path_str = payload.at("path").get<std::string>();
  int chunk_index = payload.at("chunk_index").get<int>();
  int total_chunks = payload.at("total_chunks").get<int>();
  std::string encoded_data = payload.at("data").get<std::string>();

  std::stringstream encoded_stream(encoded_data);
  std::stringstream decoded_stream;
  base64::decoder D;
  D.decode(encoded_stream, decoded_stream);

  auto& assembly_info = m_file_assembly_buffer[file_path_str];
  assembly_info.first = total_chunks;
  assembly_info.second[chunk_index] = decoded_stream.str();

  std::cout << "[KCP] 收到文件 '" << file_path_str << "' 的块 "
            << chunk_index + 1 << "/" << total_chunks << " ("
            << assembly_info.second[chunk_index].size() << " 字节)."
            << std::endl;

  if (assembly_info.second.size() == total_chunks) {
    std::cout << "[KCP] 文件 '" << file_path_str
              << "' 的所有块已收齐，正在重组..." << std::endl;

    std::filesystem::path relative_path(
        reinterpret_cast<const char8_t*>(file_path_str.c_str()));
    std::filesystem::path full_path =
        m_state_manager->get_root_path() / relative_path;

    if (full_path.has_parent_path()) {
      std::filesystem::create_directories(full_path.parent_path());
    }

    std::ofstream output_file(full_path, std::ios::binary);
    if (!output_file.is_open()) {
      std::cerr << "[P2P] 创建文件失败: " << full_path.string() << std::endl;
      m_file_assembly_buffer.erase(file_path_str);
      return;
    }

    for (int i = 0; i < total_chunks; ++i) {
      output_file.write(assembly_info.second[i].data(),
                        assembly_info.second[i].length());
    }
    output_file.close();

    std::cout << "[P2P] 成功: 文件 '" << file_path_str << "' 已保存。"
              << std::endl;
    m_file_assembly_buffer.erase(file_path_str);
  }
}

}  // namespace VeritasSync
