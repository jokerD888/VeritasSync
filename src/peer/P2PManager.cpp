#include "VeritasSync/P2PManager.h"
#include "VeritasSync/StateManager.h"
#include "VeritasSync/Protocol.h" 
#include "VeritasSync/SyncManager.h"

#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <functional>

#define BUFFERSIZE 8192

#include <b64/decode.h>
#include <b64/encode.h>



namespace VeritasSync {

//================================================================================
// PeerContext ʵ��
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
  kcp =
      ikcp_create(conv, this);  // 'this' (PeerContext*) �Ǵ��ݸ��ص����û�ָ��
  kcp->output = &P2PManager::kcp_output_callback;
  ikcp_nodelay(kcp, 1, 10, 2, 1);  // ����Ϊ����ģʽ
  ikcp_wndsize(kcp, 256, 256);     // ���󴰿ڴ�С
}




// �����ļ����С (���� 8KB)
// ����С�� MAX_UDP_PAYLOAD ������JSONԪ���ݵĿռ�
constexpr size_t CHUNK_DATA_SIZE = 8192;


//================================================================================
// P2PManager ʵ��
//================================================================================

boost::asio::io_context& P2PManager::get_io_context() { return m_io_context; }

void P2PManager::broadcast_current_state() {
  if (!m_state_manager) return;  // ����һ����ȫ���

  // ��� StateManager �Ƿ��Ѿ�������
  // �����ǵ��¼ܹ��У����ǽ�ȷ����ʹ��ǰ�����Ǳ����úõ�
  std::cout << "[P2P] �ļ�ϵͳ�����仯�����������нڵ�㲥����״̬..."
            << std::endl;

  // 1. ��ȡ����״̬��JSON�ַ���
  m_state_manager->scan_directory();  // ȷ��״̬�����µ�
  std::string json_state = m_state_manager->get_state_as_json_string();

  // 2. �������жԵȽڵ㲢����
  // �������ǲ����� m_peers_mutex ���ڷ��ͣ������ȸ���һ�� endpoint �б�
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


// --- ��̬�����빹�캯�� ---
std::shared_ptr<P2PManager> P2PManager::create(unsigned short port) {
  // ... �����ṹ�� P2PManagerMaker �Ķ��岻�� ...
  struct P2PManagerMaker : public P2PManager {
    P2PManagerMaker(unsigned short p) : P2PManager(p) {}
  };
  auto manager = std::make_shared<P2PManagerMaker>(port);
  manager->init();
  return manager;
}

P2PManager::P2PManager(unsigned short port)
    : m_socket(m_io_context, udp::endpoint(udp::v4(), port)),
      m_kcp_update_timer(m_io_context)
// m_state_manager �����ﱻĬ�ϳ�ʼ��Ϊ nullptr
{}

void P2PManager::set_state_manager(StateManager* sm) { m_state_manager = sm; }

void P2PManager::init() {
  // ���� shared_from_this() ǰ����������һ�����ڵ� shared_ptr ʵ������ this��
  // create �����е� make_shared ȷ������һ�㡣
  m_thread = std::jthread([this]() {
    std::cout << "[P2P] IO context �ں�̨�߳�����..." << std::endl;
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

// --- KCP ���ļ��� ---
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
  // ����һ����ʱ���У����ڴ�Ŵ��������Ϣ������������ִ�и��ӻص�
  std::vector<std::pair<std::string, udp::endpoint>> received_messages;

  {  // ��һ�������������������������
    std::lock_guard<std::mutex> lock(m_peers_mutex);
    for (auto const& [endpoint, context] : m_peers) {
      ikcp_update(context->kcp, current_time_ms);

      char buffer[MAX_UDP_PAYLOAD];
      int size;
      // ѭ�����գ�ȷ��һ���԰�KCP���������������ݶ�������
      while ((size = ikcp_recv(context->kcp, buffer, sizeof(buffer))) > 0) {
        // --- �������������2 ---
        // ��������ֱ�Ӵ������Ǵ������
        received_messages.emplace_back(std::string(buffer, size), endpoint);
      }
    }
  }  // <--- lock_guard �����ﱻ������m_peers_mutex �����ͷ�

  // �����Ѿ��ͷŵİ�ȫ����ͳһ���������յ�����Ϣ
  for (const auto& msg_pair : received_messages) {
    handle_kcp_message(msg_pair.first, msg_pair.second);
  }

  schedule_kcp_update();  // ���µ�����һ�θ���
}

// --- ԭʼ���� I/O ---
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
      std::cout << "[P2P] ������ " << target_endpoint << " ����PING�Խ������֡�"
                << std::endl;
      // ��ȡ/�����Եȵ������ģ��˲���Ҳ���ʼ��KCP
      get_or_create_peer_context(target_endpoint);
      // ����һ���򵥵�PING��Ϣ������UDP���򶴡�
      raw_udp_send("PING", 4, target_endpoint);
    }
  }
}



// --- �����������ѭ�� ---

void P2PManager::start_receive() {
  // Ϊ���ղ��������µĻ�������endpoint���󣬱��Ⲣ������
  auto remote_endpoint = std::make_shared<udp::endpoint>();
  auto recv_buffer = std::make_shared<std::array<char, MAX_UDP_PAYLOAD>>();

  m_socket.async_receive_from(
      boost::asio::buffer(*recv_buffer), *remote_endpoint,
      [self = shared_from_this(), remote_endpoint, recv_buffer](
          const boost::system::error_code& error, std::size_t bytes) {
        // ʹ�� self (һ��shared_ptr) ��ȷ�� P2PManager �ڻص��ڼ��Ǵ���
        self->handle_receive(error, bytes, remote_endpoint, recv_buffer);
      });
}

// `handle_receive` ���ڷǳ��򵥣�ֻ����ԭʼUDP��
void P2PManager::handle_receive(
    const boost::system::error_code& error, std::size_t bytes_transferred,
    std::shared_ptr<udp::endpoint> remote_endpoint,
    std::shared_ptr<std::array<char, MAX_UDP_PAYLOAD>> recv_buffer) {
  if (!error && bytes_transferred > 0) {
    // ����Ƿ������ڡ��򶴡���PING��Ϣ
    if (bytes_transferred == 4 &&
        std::string(recv_buffer->data(), 4) == "PING") {
      std::cout << "[P2P] �յ����� " << *remote_endpoint
                << " �� PING ��������" << std::endl;
      // ��ȡ�򴴽��˶Եȵ��������
      auto peer_context = get_or_create_peer_context(*remote_endpoint);

      // ��Ϊ��Ӧ������ͨ��KCP�������ǵ�״̬
      std::cout << "[KCP] �Է���׼��������ͨ��KCP�������ǵ��ļ�״̬..."
                << std::endl;
      m_state_manager->scan_directory();
      std::string json_state = m_state_manager->get_state_as_json_string();
      send_over_kcp(json_state, *remote_endpoint);
    } else {
      // �����������ݰ���Ӧ����ΪKCP����
      auto peer_context = get_or_create_peer_context(*remote_endpoint);
      // ���յ���UDPԭʼ����ι��KCP���д���
      ikcp_input(peer_context->kcp, recv_buffer->data(), bytes_transferred);
    }
  }
  start_receive();  // ����������һ��UDP��
}

// --- �Եȵ���� ---
std::shared_ptr<PeerContext> P2PManager::get_or_create_peer_context(
    const udp::endpoint& endpoint) {
  std::lock_guard<std::mutex> lock(m_peers_mutex);
  auto it = m_peers.find(endpoint);
  if (it != m_peers.end()) {
    return it->second;
  }

  std::cout << "[KCP] ��⵽�µĶԵȵ㣬Ϊ�䴴��KCP������: " << endpoint
            << std::endl;
  auto new_context =
      std::make_shared<PeerContext>(endpoint, shared_from_this());
  // �ỰID (conv) ������ͨ��˫���䱣��һ�¡�
  // ��ʵ��Ӧ���У����Ի���sync_key��˫��IP�˿ڼ���һ��Ψһ��ID��
  // ����Ϊ�˼򻯣�����ʹ��һ���̶���ID��
  uint32_t conv = 12345;
  new_context->setup_kcp(conv);
  m_peers[endpoint] = new_context;
  return new_context;
}

// --- �ϲ�Ӧ����Ϣ���� ---

// ����ڣ�����KCP�յ��Ŀɿ���Ϣ�ַ�����ͬ�Ĵ�����
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
  } catch (const std::exception& e) {
    std::cerr << "[P2P] ����KCP��Ϣʱ��������: " << e.what() << std::endl;
    std::cerr << "       ԭʼ��Ϣ: " << msg << std::endl;
  }
}

void P2PManager::send_over_kcp(const std::string& msg,
                               const udp::endpoint& target_endpoint) {
  std::lock_guard<std::mutex> lock(m_peers_mutex);
  auto it = m_peers.find(target_endpoint);
  if (it != m_peers.end()) {
    // ʹ�� ikcp_send �������ݣ�KCP�ᱣ֤��ɿ�����
    ikcp_send(it->second->kcp, msg.c_str(), msg.length());
  } else {
    std::cerr << "[KCP] ����: ������һ��δ����KCP�����ĵĶԵȵ㷢����Ϣ: "
              << target_endpoint << std::endl;
  }
}

// ����״̬������Ϣ
void P2PManager::handle_share_state(const nlohmann::json& payload,
                                    const udp::endpoint& from_endpoint) {
  std::cout << "[KCP] �յ����� " << from_endpoint << " �� 'share_state' ��Ϣ��"
            << std::endl;

  // 1. ��ȡԶ��״̬
  std::vector<FileInfo> remote_files =
      payload.at("files").get<std::vector<FileInfo>>();

  // 2. ��ȡ�Լ���ǰ�ı���״̬
  m_state_manager->scan_directory();
  nlohmann::json temp_json =
      nlohmann::json::parse(m_state_manager->get_state_as_json_string());
  std::vector<FileInfo> local_files = temp_json.at(Protocol::MSG_PAYLOAD)
                                          .at("files")
                                          .get<std::vector<FileInfo>>();

  // 3. ������������ֻ�ҳ�����Ҫ�ӶԷ���ȡ���ļ������ӷ���
  std::vector<std::string> files_to_request =
      SyncManager::compare_states_and_get_requests(local_files, remote_files);

  // 4. ִ������
  if (!files_to_request.empty()) {
    std::cout << "[KCP] �ƻ��� " << from_endpoint << " ���� "
              << files_to_request.size() << " ��ȱʧ���ļ���" << std::endl;
    for (const auto& file_path : files_to_request) {
      nlohmann::json request_msg;
      request_msg[Protocol::MSG_TYPE] = Protocol::TYPE_REQUEST_FILE;
      request_msg[Protocol::MSG_PAYLOAD] = {{"path", file_path}};
      send_over_kcp(request_msg.dump(), from_endpoint);
    }
  } else {
    std::cout << "[KCP] �����ļ�����������Զ���ļ�����������" << std::endl;
  }

  // 5. ���������������Է��Ƿ�Ҳ��Ҫ���ǵ��ļ�
  //    ����Է���״̬�������������е��ļ����ͻظ����ǵ�״̬���Ա�Է������ӷ���
  std::vector<std::string> remote_needs_our_files =
      SyncManager::compare_states_and_get_requests(remote_files, local_files);

  if (!remote_needs_our_files.empty()) {
    std::cout << "[KCP] �Է�״̬���������ظ����ǵ�״̬�Թ���ϲ���"
              << std::endl;
    send_over_kcp(temp_json.dump(), from_endpoint);
  } else {
    std::cout << "[KCP] ������˫��״̬����ȫ�ϲ�һ�¡�" << std::endl;
  }
}

// �����ļ�����
void P2PManager::handle_file_request(const nlohmann::json& payload,
                                     const udp::endpoint& from_endpoint) {
  const std::string requested_path_str = payload.at("path").get<std::string>();
  std::cout << "[KCP] �յ����� " << from_endpoint << " ���ļ� '"
            << requested_path_str << "' ������" << std::endl;

    // 1. ���յ���UTF-8�ַ���·����ͨ�� u8string ���캯����ȫ��ת��Ϊһ��
  // std::filesystem::path ����
  //    ������캯������ȷ�ؽ�UTF-8�ֽ����н���Ϊ��Ч��·����
  std::filesystem::path relative_path(
      reinterpret_cast<const char8_t*>(requested_path_str.c_str()));

  // 2. �����ļ�������·��
  std::filesystem::path full_path =
      m_state_manager->get_root_path() / relative_path;

  if (!std::filesystem::exists(full_path)) {
    std::cerr << "[P2P] ��������ļ�������: " << full_path << std::endl;
    return;
  }

  std::ifstream file(full_path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    std::cerr << "[P2P] �޷����ļ�: " << full_path << std::endl;
    return;
  }

  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);

      // --- ���ֽ��ļ���������޸� ---
  if (size == 0) {
    std::cout << "[KCP] ���ڷ������ֽ��ļ� '" << requested_path_str
              << "' ��Ԫ��Ϣ..." << std::endl;
    nlohmann::json chunk_msg;
    chunk_msg[Protocol::MSG_TYPE] = Protocol::TYPE_FILE_CHUNK;
    chunk_msg[Protocol::MSG_PAYLOAD] = {
        {"path", requested_path_str},
        {"chunk_index", 0},   // Ψһ�Ŀ飬����Ϊ0
        {"total_chunks", 1},  // �ܹ���1����
        {"data", ""}          // ����Ϊ��
    };
    send_over_kcp(chunk_msg.dump(), from_endpoint);
    return;  // ������ϣ�ֱ�ӷ���
  }

  int total_chunks =
      static_cast<int>((size + CHUNK_DATA_SIZE - 1) / CHUNK_DATA_SIZE);
  std::vector<char> buffer(CHUNK_DATA_SIZE);

  std::cout << "[KCP] ���ڽ��ļ� '" << requested_path_str << "' (" << size
            << " �ֽ�) �ֳ� " << total_chunks << " �鷢�͸� " << from_endpoint
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
    send_over_kcp(chunk_msg.dump(), from_endpoint);  // ͨ��KCP����
  }
}

// �����ļ��� 
void P2PManager::handle_file_chunk(const nlohmann::json& payload) {
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

  std::cout << "[KCP] �յ��ļ� '" << file_path_str << "' �Ŀ� "
            << chunk_index + 1 << "/" << total_chunks << " ("
            << assembly_info.second[chunk_index].size() << " �ֽ�)."
            << std::endl;

  if (assembly_info.second.size() == total_chunks) {
    std::cout << "[KCP] �ļ� '" << file_path_str
              << "' �����п������룬��������..." << std::endl;

    std::filesystem::path relative_path(
        reinterpret_cast<const char8_t*>(file_path_str.c_str()));
    std::filesystem::path full_path =
        m_state_manager->get_root_path() / relative_path;

    if (full_path.has_parent_path()) {
      std::filesystem::create_directories(full_path.parent_path());
    }

    std::ofstream output_file(full_path, std::ios::binary);
    if (!output_file.is_open()) {
      std::cerr << "[P2P] �����ļ�ʧ��: " << full_path.string() << std::endl;
      m_file_assembly_buffer.erase(file_path_str);
      return;
    }

    for (int i = 0; i < total_chunks; ++i) {
      output_file.write(assembly_info.second[i].data(),
                        assembly_info.second[i].length());
    }
    output_file.close();

    std::cout << "[P2P] �ɹ�: �ļ� '" << file_path_str << "' �ѱ��档"
              << std::endl;
    m_file_assembly_buffer.erase(file_path_str);
  }
}

}  // namespace VeritasSync