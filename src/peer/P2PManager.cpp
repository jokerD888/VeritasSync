#include "VeritasSync/P2PManager.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>  // 【修复】 包含 <set>
#include <sstream>

#include "VeritasSync/Protocol.h"
#include "VeritasSync/StateManager.h"
#include "VeritasSync/SyncManager.h"

#define BUFFERSIZE 8192

#include <b64/decode.h>
#include <b64/encode.h>

namespace VeritasSync {
// ... (PeerContext 实现保持不变) ...
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

// ... (P2PManager 的其他实现保持不变) ...
boost::asio::io_context& P2PManager::get_io_context() { return m_io_context; }
void P2PManager::broadcast_current_state() {
  if (!m_state_manager) return;
  std::cout << "[P2P] 文件系统发生变化，正在向所有节点广播最新状态..."
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
void P2PManager::init() {
  start_receive();
  schedule_kcp_update();
}
P2PManager::~P2PManager() { m_io_context.stop(); }
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
        received_messages.emplace_back(std::string(buffer, size), endpoint);
      }
    }
  }
  for (const auto& msg_pair : received_messages) {
    handle_kcp_message(msg_pair.first, msg_pair.second);
  }
  schedule_kcp_update();
}
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
      std::cout << "[KCP] 对方已准备就绪，通过KCP发送我们的文件状态..."
                << std::endl;
      boost::asio::post(m_io_context, [this, remote_endpoint]() {
        if (!m_state_manager) return;
        m_state_manager->scan_directory();
        std::string json_state = m_state_manager->get_state_as_json_string();
        send_over_kcp(json_state, *remote_endpoint);
      });
    } else {
      auto peer_context = get_or_create_peer_context(*remote_endpoint);
      ikcp_input(peer_context->kcp, recv_buffer->data(), bytes_transferred);
    }
  }
  start_receive();
}
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
void P2PManager::handle_kcp_message(const std::string& msg,
                                    const udp::endpoint& from_endpoint) {
  try {
    auto json = nlohmann::json::parse(msg);
    const std::string msg_type = json.at(Protocol::MSG_TYPE).get<std::string>();
    auto& payload = json.at(Protocol::MSG_PAYLOAD);

    if (msg_type == Protocol::TYPE_SHARE_STATE) {
      handle_share_state(payload, from_endpoint);
    } else if (msg_type == Protocol::TYPE_REQUEST_FILE) {
      handle_file_request(payload, from_endpoint);
    } else if (msg_type == Protocol::TYPE_FILE_CHUNK) {
      handle_file_chunk(payload);
    }
  } catch (const nlohmann::json::parse_error& e) {
    std::cerr << "[P2P] JSON 解析错误: " << e.what() << std::endl;
    std::cerr << "       原始消息 (前 100 字节): " << msg.substr(0, 100)
              << std::endl;
  } catch (const nlohmann::json::exception& e) {
    std::cerr << "[P2P] 处理 JSON 时发生错误: " << e.what() << std::endl;
    std::cerr << "       原始消息 (前 100 字节): " << msg.substr(0, 100)
              << std::endl;
  } catch (const std::exception& e) {
    std::cerr << "[P2P] 处理 KCP 消息时发生未知错误: " << e.what() << std::endl;
  }
}
void P2PManager::send_over_kcp(const std::string& msg,
                               const udp::endpoint& target_endpoint) {
  std::lock_guard<std::mutex> lock(m_peers_mutex);
  auto it = m_peers.find(target_endpoint);
  if (it != m_peers.end()) {
    int result = ikcp_send(it->second->kcp, msg.c_str(), (int)msg.length());
    if (result < 0) {
      std::cerr << "[KCP] ikcp_send 错误: " << result << std::endl;
    }
  } else {
    std::cerr << "[KCP] 错误: 尝试向一个未建立 KCP 上下文的对等点发送消息: "
              << target_endpoint << std::endl;
  }
}

void P2PManager::handle_share_state(const nlohmann::json& payload,
                                    const udp::endpoint& from_endpoint) {
  std::cout << "[KCP] 收到来自 " << from_endpoint << " 的 'share_state' 消息。"
            << std::endl;
  if (!m_state_manager) {
    std::cerr << "[P2P] 错误: StateManager 未设置!" << std::endl;
    return;
  }

  std::vector<FileInfo> remote_files_vec;
  try {
    remote_files_vec = payload.at("files").get<std::vector<FileInfo>>();
  } catch (const nlohmann::json::exception& e) {
    std::cerr << "[P2P] 解析 'files' 字段失败: " << e.what() << std::endl;
    return;
  }

  std::map<std::string, FileInfo> remote_map;  // key 是 UTF-8 string
  for (const auto& info : remote_files_vec) {
    remote_map[info.path] = info;
  }

  m_state_manager->scan_directory();
  const auto& local_map =
      m_state_manager->get_file_map();  // key 是 UTF-8 string

  SyncActions actions = SyncManager::analyze_states(local_map, remote_map);

  if (!actions.conflicts.empty()) {
    std::cout << "[Conflict] 检测到 " << actions.conflicts.size() << " 个冲突。"
              << std::endl;
    for (const auto& path_str : actions.conflicts) {  // path_str 是 UTF-8
      try {
        auto local_it = local_map.find(path_str);
        if (local_it != local_map.end() &&
            local_it->second.hash == "DIRECTORY") {
          std::cout << "[Conflict] -> 跳过对目录的冲突重命名: " << path_str
                    << std::endl;
          continue;
        }

        std::filesystem::path relative_path = std::filesystem::u8path(path_str);
        std::filesystem::path full_path =
            m_state_manager->get_root_path() / relative_path;

        std::error_code exist_ec;
        if (!std::filesystem::exists(full_path, exist_ec) || exist_ec) {
          // 【修复】 使用 .string() 进行日志记录
          std::cerr << "[Conflict] 错误: 尝试重命名的本地文件不存在: "
                    << full_path.string() << std::endl;
          continue;
        }

        std::string conflict_suffix =
            " (conflict from " + from_endpoint.address().to_string() + ")";
        std::filesystem::path conflict_path = full_path;

        std::wstring new_stem =
            conflict_path.stem().wstring() +
            std::wstring(conflict_suffix.begin(), conflict_suffix.end());
        conflict_path.replace_filename(new_stem);
        if (full_path.has_extension()) {
          conflict_path += full_path.extension();
        }

        std::error_code ec;
        std::filesystem::rename(full_path, conflict_path, ec);
        if (ec) {
          // 【修复】 使用 .string() 进行日志记录
          std::cerr << "[Conflict] 重命名失败: " << full_path.string() << " -> "
                    << conflict_path.string() << " Error: " << ec.message()
                    << std::endl;
        } else {
          // 【修复】 使用 .string() 进行日志记录
          std::cout << "[Conflict] -> 本地文件已重命名为: "
                    << conflict_path.filename().string() << std::endl;
        }

        actions.request_from_remote.push_back(path_str);

      } catch (const std::exception& e) {
        std::cerr << "[Conflict] 处理 " << path_str
                  << " 时发生严重错误: " << e.what() << std::endl;
      }
    }
  }

  std::set<std::string> unique_requests(actions.request_from_remote.begin(),
                                        actions.request_from_remote.end());
  if (!unique_requests.empty()) {
    std::cout << "[KCP] 计划向 " << from_endpoint << " 请求 "
              << unique_requests.size() << " 个条目。" << std::endl;
    for (const auto& file_path : unique_requests) {
      nlohmann::json request_msg;
      request_msg[Protocol::MSG_TYPE] = Protocol::TYPE_REQUEST_FILE;
      request_msg[Protocol::MSG_PAYLOAD] = {{"path", file_path}};
      send_over_kcp(request_msg.dump(-1), from_endpoint);
    }
  }

  if (!actions.push_to_remote.empty()) {
    std::cout << "[KCP] 对方状态不完整 (" << actions.push_to_remote.size()
              << " 个条目)，回复我们的状态以供其合并。" << std::endl;
    std::string json_state = m_state_manager->get_state_as_json_string();
    send_over_kcp(json_state, from_endpoint);
  }

  if (actions.conflicts.empty() && unique_requests.empty() &&
      actions.push_to_remote.empty()) {
    std::cout << "[KCP] 完美！双方状态已完全合并一致。" << std::endl;
  }
}

void P2PManager::handle_file_request(const nlohmann::json& payload,
                                     const udp::endpoint& from_endpoint) {
  const std::string requested_path_str = payload.at("path").get<std::string>();
  std::cout << "[KCP] 收到来自 " << from_endpoint << " 对条目 '"
            << requested_path_str << "' 的请求。" << std::endl;
  if (!m_state_manager) return;

  std::filesystem::path relative_path =
      std::filesystem::u8path(requested_path_str);
  std::filesystem::path full_path =
      m_state_manager->get_root_path() / relative_path;

  std::error_code ec;
  bool exists = std::filesystem::exists(full_path, ec);
  if (ec) {
    // 【修复】 使用 .string() 进行日志记录
    std::cerr << "[P2P] 检查条目是否存在时出错: " << full_path.string() << ": "
              << ec.message() << std::endl;
    return;
  }

  if (!exists) {
    // 【修复】 使用 .string() 进行日志记录
    std::cerr << "[P2P] 被请求的条目不存在: " << full_path.string()
              << std::endl;
    return;
  }

  bool is_dir = std::filesystem::is_directory(full_path, ec);
  if (ec) {
    // 【修复】 使用 .string() 进行日志记录
    std::cerr << "[P2P] 检查条目类型时出错: " << full_path.string() << ": "
              << ec.message() << std::endl;
    return;
  }

  if (is_dir) {
    std::cout << "[KCP] 正在确认目录 '" << requested_path_str << "' 的存在..."
              << std::endl;
    nlohmann::json chunk_msg;
    chunk_msg[Protocol::MSG_TYPE] = Protocol::TYPE_FILE_CHUNK;
    chunk_msg[Protocol::MSG_PAYLOAD] = {{"path", requested_path_str},
                                        {"chunk_index", 0},
                                        {"total_chunks", 1},
                                        {"data", ""},
                                        {"hash", "DIRECTORY"}};
    send_over_kcp(chunk_msg.dump(-1), from_endpoint);
    return;
  }

  std::ifstream file(full_path.wstring(), std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    // 【修复】 使用 .string() 进行日志记录
    std::cerr << "[P2P] 无法打开文件: " << full_path.string() << std::endl;
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
    send_over_kcp(chunk_msg.dump(-1), from_endpoint);
    return;
  }

  constexpr size_t CHUNK_DATA_SIZE = 8192;
  int total_chunks =
      static_cast<int>((size + CHUNK_DATA_SIZE - 1) / CHUNK_DATA_SIZE);
  std::vector<char> buffer(CHUNK_DATA_SIZE);

  std::cout << "[KCP] 正在将文件 '" << requested_path_str << "' (" << size
            << " 字节) 分成 " << total_chunks << " 块发送给 " << from_endpoint
            << std::endl;

  for (int i = 0; i < total_chunks; ++i) {
    file.read(buffer.data(), buffer.size());
    std::streamsize bytes_read = file.gcount();
    if (bytes_read <= 0 && !file.eof()) {
      // 【修复】 使用 .string() 进行日志记录
      std::cerr << "[P2P] 读取文件时出错: " << full_path.string() << std::endl;
      break;
    }
    if (bytes_read == 0 && file.eof()) break;

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
    send_over_kcp(chunk_msg.dump(-1), from_endpoint);
  }
}

void P2PManager::handle_file_chunk(const nlohmann::json& payload) {
  if (!m_state_manager) return;
  std::string file_path_str = payload.at("path").get<std::string>();
  int chunk_index = payload.at("chunk_index").get<int>();
  int total_chunks = payload.at("total_chunks").get<int>();

  if (payload.contains("hash") &&
      payload.at("hash").get<std::string>() == "DIRECTORY") {
    std::cout << "[KCP] 收到目录创建请求: '" << file_path_str << "'"
              << std::endl;
    std::filesystem::path relative_path =
        std::filesystem::u8path(file_path_str);
    std::filesystem::path full_path =
        m_state_manager->get_root_path() / relative_path;

    std::error_code ec;
    if (!std::filesystem::exists(full_path, ec)) {
      if (std::filesystem::create_directories(full_path, ec) && !ec) {
        std::cout << "[P2P] 成功: 目录 '" << file_path_str << "' 已创建。"
                  << std::endl;
      } else {
        // 【修复】 使用 .string() 进行日志记录
        std::cerr << "[P2P] 创建目录失败: " << full_path.string() << ": "
                  << ec.message() << std::endl;
      }
    } else if (ec) {
      // 【修复】 使用 .string() 进行日志记录
      std::cerr << "[P2P] 检查目录是否存在时出错: " << full_path.string()
                << ": " << ec.message() << std::endl;
    }
    return;
  }

  std::string encoded_data = payload.at("data").get<std::string>();
  std::stringstream encoded_stream(encoded_data);
  std::stringstream decoded_stream;
  base64::decoder D;
  try {
    D.decode(encoded_stream, decoded_stream);
  } catch (const std::exception& e) {
    std::cerr << "[P2P] Base64 解码错误 for " << file_path_str << ": "
              << e.what() << std::endl;
    return;
  }

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

    std::filesystem::path relative_path =
        std::filesystem::u8path(file_path_str);
    std::filesystem::path full_path =
        m_state_manager->get_root_path() / relative_path;

    std::error_code ec;
    if (full_path.has_parent_path()) {
      if (!std::filesystem::create_directories(full_path.parent_path(), ec) &&
          ec) {
        // 【修复】 使用 .string() 进行日志记录
        std::cerr << "[P2P] 创建父目录失败: "
                  << full_path.parent_path().string() << ": " << ec.message()
                  << std::endl;
        m_file_assembly_buffer.erase(file_path_str);
        return;
      }
    }

    std::ofstream output_file(full_path.wstring(), std::ios::binary);
    if (!output_file.is_open()) {
      // 【修复】 使用 .string() 进行日志记录
      std::cerr << "[P2P] 创建文件失败: " << full_path.string() << std::endl;
      m_file_assembly_buffer.erase(file_path_str);
      return;
    }

    bool write_error = false;
    for (int i = 0; i < total_chunks; ++i) {
      if (assembly_info.second.count(i)) {
        output_file.write(assembly_info.second[i].data(),
                          assembly_info.second[i].length());
        if (output_file.bad()) {
          // 【修复】 使用 .string() 进行日志记录
          std::cerr << "[P2P] 写入文件时出错: " << full_path.string()
                    << std::endl;
          write_error = true;
          break;
        }
      } else {
        std::cerr << "[P2P] 错误：丢失文件块 " << i << " for " << file_path_str
                  << std::endl;
        write_error = true;
        break;
      }
    }
    output_file.close();

    if (!write_error) {
      std::cout << "[P2P] 成功: 文件 '" << file_path_str << "' 已保存。"
                << std::endl;
    } else {
      std::filesystem::remove(full_path, ec);
    }
    m_file_assembly_buffer.erase(file_path_str);
  }
}

}  // namespace VeritasSync