#include "VeritasSync/P2PManager.h"
#include "VeritasSync/StateManager.h"
#include "VeritasSync/Protocol.h" 
#include "VeritasSync/SyncManager.h"
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <fstream>
#include <algorithm>

#define BUFFERSIZE 8192

#include <b64/decode.h>
#include <b64/encode.h>



namespace VeritasSync {

// 定义文件块大小 (例如 8KB)
// 必须小于 MAX_UDP_PAYLOAD 以留出JSON元数据的空间
constexpr size_t CHUNK_DATA_SIZE = 8192;

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
  auto recv_buffer = std::make_shared<std::array<char, MAX_UDP_PAYLOAD>>();

  std::cout << "ok -=----------------" << std::endl;
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
    std::shared_ptr<std::array<char, MAX_UDP_PAYLOAD>> recv_buffer) {
  std::cout << "ok A -=----------------" << std::endl;

  if (!error) {
    std::string received_msg(recv_buffer->data(), bytes_transferred);
    if (received_msg == "PING") {
      std::cout << "[P2P] Received PING from " << *remote_endpoint
                << ". Replying with state..." << std::endl;
      m_state_manager.scan_directory();
      std::string json_state = m_state_manager.get_state_as_json_string();
      send(json_state, *remote_endpoint);
      std::cout << "ok B-=----------------" << std::endl;

    } else {
      std::cout << "ok C-=----------------" << std::endl;
      // 尝试将收到的消息作为JSON处理
      try {
        auto json = nlohmann::json::parse(received_msg);
        const std::string msg_type = json.at(Protocol::MSG_TYPE);
        auto& payload = json.at(Protocol::MSG_PAYLOAD);

        // 
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
            std::cout<<"send ok----------------------"<<std::endl;
          }

          std::sort(local_files.begin(), local_files.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
          std::sort(remote_files.begin(), remote_files.end(), [](const auto& a, const auto& b) { return a.path < b.path; });

          // --- FIX: Use a custom comparator that ignores modification time ---
          bool states_are_equal = std::equal(
            local_files.begin(), local_files.end(),
            remote_files.begin(), remote_files.end(),
            [](const FileInfo& a, const FileInfo& b) {
              // Only compare path and hash for sync completion check
              return a.path == b.path && a.hash == b.hash;
            }
          );

          if (!states_are_equal) {
            std::cout << "[P2P] States are different. Replying with our own state to " << *remote_endpoint << " to continue sync." << std::endl;
            send(temp_json.dump(), *remote_endpoint);
          }
          else {
            std::cout << "[P2P] States are identical (path/hash). Synchronization with " << *remote_endpoint << " is complete." << std::endl;
          }

        }
        else if (msg_type == Protocol::TYPE_REQUEST_FILE) {
          const std::string requested_path = payload.at("path");
          std::cout << "[P2P] Received a request for file: '" << requested_path
            << "' from " << *remote_endpoint << std::endl;
          handle_file_request(requested_path, *remote_endpoint);
        }
        else if (msg_type == Protocol::TYPE_FILE_CHUNK) {
          handle_file_chunk(payload);
        }

      } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "[P2P] JSON PARSE ERROR: " << e.what() << std::endl;
        std::cerr << "       Problematic message: " << received_msg
                  << std::endl;
      } catch (const nlohmann::json::exception&
                   e) {  // Catch other nlohmann exceptions
        std::cerr << "[P2P] JSON LOGIC ERROR: " << e.what() << std::endl;
      } catch (const std::exception& e) {  // Catch all standard exceptions
        std::cerr << "[P2P] UNHANDLED STANDARD EXCEPTION: " << e.what()
                  << std::endl;
      } catch (...) {  // Catch anything else
        std::cerr << "[P2P] UNKNOWN EXCEPTION CAUGHT. Program will continue."
                  << std::endl;
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

void P2PManager::handle_file_request(const std::string& file_path,
                                     const udp::endpoint& peer) {
  // 构建文件的完整路径
  std::filesystem::path full_path = m_state_manager.get_root_path() / file_path;

  if (!std::filesystem::exists(full_path)) {
    std::cerr << "[P2P] Requested file does not exist: " << full_path
              << std::endl;
    return;
  }

  std::ifstream file(full_path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    std::cerr << "[P2P] Could not open file: " << full_path << std::endl;
    return;
  }

  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);

  int total_chunks =
      static_cast<int>((size + CHUNK_DATA_SIZE - 1) / CHUNK_DATA_SIZE);
  std::vector<char> buffer(CHUNK_DATA_SIZE);

  std::cout << "[P2P] Sending file '" << file_path << "' (" << size
            << " bytes) in " << total_chunks << " chunk(s) to " << peer
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
    chunk_msg[Protocol::MSG_PAYLOAD] = {
        {"path", file_path},
        {"chunk_index", i},
        {"total_chunks", total_chunks},
        {"data", encoded_stream.str()}  // 3. 将编码后的字符串放入JSON
    };

    send(chunk_msg.dump(), peer);
  }
}

void P2PManager::handle_file_chunk(const nlohmann::json& payload) {
  std::string file_path = payload.at("path");
  int chunk_index = payload.at("chunk_index");
  int total_chunks = payload.at("total_chunks");

  // 从JSON中获取Base64字符串
  std::string encoded_data = payload.at("data").get<std::string>();

  // 使用b64流式接口进行解码
  std::stringstream encoded_stream(encoded_data);
  std::stringstream decoded_stream;
  base64::decoder D;
  D.decode(encoded_stream, decoded_stream);

  // 将解码后的二进制数据存入文件重组缓冲区
  auto& assembly_info = m_file_assembly_buffer[file_path];
  assembly_info.first = total_chunks;
  assembly_info.second[chunk_index].data = decoded_stream.str();

  std::cout << "[P2P] Received chunk " << chunk_index + 1 << "/" << total_chunks
            << " for file '" << file_path << "' ("
            << assembly_info.second[chunk_index].data.size() << " bytes)."
            << std::endl;

  if (assembly_info.second.size() == total_chunks) {
    std::cout << "[P2P] All chunks for '" << file_path
              << "' received. Assembling file..." << std::endl;

    std::filesystem::path full_path =
        m_state_manager.get_root_path() / file_path;
    if (full_path.has_parent_path()) {
      std::filesystem::create_directories(full_path.parent_path());
    }

    std::ofstream output_file(full_path, std::ios::binary);
    if (!output_file.is_open()) {
      std::cerr << "[P2P] Failed to create file for writing: " << full_path
                << std::endl;
      return;
    }

    for (int i = 0; i < total_chunks; ++i) {
      output_file.write(assembly_info.second[i].data.c_str(),
                        assembly_info.second[i].data.length());
    }
    output_file.close();

    std::cout << "[P2P] SUCCESS: File '" << file_path << "' has been saved."
              << std::endl;

    m_file_assembly_buffer.erase(file_path);
  }
}

}