#include "VeritasSync/P2PManager.h"
#include "VeritasSync/StateManager.h"
#include "VeritasSync/Protocol.h" 
#include "VeritasSync/SyncManager.h"
#include <iostream>
#include <sstream>

namespace VeritasSync {

P2PManager::P2PManager(unsigned short port, StateManager& state_manager)
    : m_socket(m_io_context, udp::endpoint(udp::v4(), port)),
      m_state_manager(state_manager) {  // 初始化引用
  m_thread = std::jthread([this]() {
    std::cout << "[P2P] IO context running in background thread..."
              << std::endl;
    m_io_context.run();
  });

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
  auto recv_buffer = std::make_shared<std::array<char, 4096>>();

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
    std::shared_ptr<std::array<char, 4096>> recv_buffer) {
  if (!error) {
    std::string received_msg(recv_buffer->data(), bytes_transferred);
    if (received_msg == "PING") {
      std::cout << "[P2P] Received PING from " << *remote_endpoint
                << ". Replying with state..." << std::endl;
      m_state_manager.scan_directory();
      std::string json_state = m_state_manager.get_state_as_json_string();
      send(json_state, *remote_endpoint);
    } else {
      // 尝试将收到的消息作为JSON处理
      try {
        auto json = nlohmann::json::parse(received_msg);
        const std::string msg_type = json.at(Protocol::MSG_TYPE);

        // --- 核心逻辑修改 ---
        if (msg_type == Protocol::TYPE_SHARE_STATE) {
          std::cout << "[P2P] Received 'share_state' message from "
                    << *remote_endpoint << "." << std::endl;

          // a. 获取远程文件列表
          std::vector<FileInfo> remote_files =
              json.at(Protocol::MSG_PAYLOAD)
                  .at("files")
                  .get<std::vector<FileInfo>>();

          // b. 获取本地文件列表
          m_state_manager.scan_directory();
          // (为了调用 compare_states... 我们需要一个FileInfo的vector, 而非map)
          nlohmann::json temp_json =
              nlohmann::json::parse(m_state_manager.get_state_as_json_string());
          std::vector<FileInfo> local_files =
              temp_json.at(Protocol::MSG_PAYLOAD)
                  .at("files")
                  .get<std::vector<FileInfo>>();

          // c. 调用SyncManager进行比较
          std::vector<std::string> needed_files =
              SyncManager::compare_states_and_get_requests(local_files,
                                                           remote_files);

          // d. 对每个需要的文件发送请求
          if (!needed_files.empty()) {
            std::cout << "[P2P] Sending " << needed_files.size()
                      << " file request(s) to " << *remote_endpoint
                      << std::endl;
            for (const auto& file_path : needed_files) {
              nlohmann::json request_msg;
              request_msg[Protocol::MSG_TYPE] = Protocol::TYPE_REQUEST_FILE;
              request_msg[Protocol::MSG_PAYLOAD] = {{"path", file_path}};
              send(request_msg.dump(), *remote_endpoint);
            }
          }
        } else if (msg_type == Protocol::TYPE_REQUEST_FILE) {
          // e. 处理收到的文件请求
          const std::string requested_path =
              json.at(Protocol::MSG_PAYLOAD).at("path");
          std::cout << "[P2P] Received a request for file: '" << requested_path
                    << "' from " << *remote_endpoint << std::endl;
          // (在下一阶段，我们将在这里实现文件发送逻辑)
        } else {
          std::cerr << "[P2P] Received unknown message type: " << msg_type
                    << std::endl;
        }

      } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "[P2P] Failed to parse received message as JSON: "
                  << e.what() << std::endl;
      } catch (const nlohmann::json::out_of_range& e) {
        std::cerr << "[P2P] JSON message is missing a required field: "
                  << e.what() << std::endl;
      }
    }
  }
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