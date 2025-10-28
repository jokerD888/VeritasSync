#include <boost/asio.hpp>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>

using boost::asio::ip::tcp;

// Session类负责处理单个客户端的完整生命周期
class Session : public std::enable_shared_from_this<Session> {
 public:
  // 构造函数现在需要接收对服务器共享数据和互斥锁的引用
  Session(tcp::socket socket,
          std::map<std::string, std::set<std::string>>& peer_groups,
          std::mutex& mutex)
      : m_socket(std::move(socket)),
        m_peer_groups(peer_groups),
        m_mutex(mutex) {}

  void start() {
    // 异步读取数据，直到遇到换行符'\n'
    boost::asio::async_read_until(
        m_socket, m_buffer, '\n',
        [self = shared_from_this()](const boost::system::error_code& ec,
                                    std::size_t bytes) {
          if (!ec) {
            self->handle_request(bytes);
          }
        });
  }

 private:
  void handle_request(std::size_t bytes_transferred) {
    std::istream request_stream(&m_buffer);
    std::string request_line;
    std::getline(request_stream, request_line);

    std::istringstream iss(request_line);
    std::string command, sync_key;
    unsigned short port;
    iss >> command >> sync_key >> port;

    if (command == "REGISTER" && !sync_key.empty() && port > 0) {
      std::string remote_address =
          m_socket.remote_endpoint().address().to_string();
      std::string new_peer_id = remote_address + ":" + std::to_string(port);

      // 使用 lock_guard 来确保互斥锁在离开作用域时自动释放
      std::lock_guard<std::mutex> lock(m_mutex);

      auto& peer_set = m_peer_groups[sync_key];

      // 1. 准备响应数据，并将其存储在成员变量 m_response_data 中
      // 这样可以保证它的生命周期足够长
      std::ostringstream oss;
      for (const auto& peer : peer_set) {
        oss << peer << "\n";
      }
      m_response_data = oss.str();

      // 2. 将新节点加入列表
      peer_set.insert(new_peer_id);
      std::cout << "[Tracker] Registered " << new_peer_id << " to group '"
                << sync_key << "'" << std::endl;

      // 3. 使用成员变量 m_response_data 进行异步写入
      boost::asio::async_write(
          m_socket, boost::asio::buffer(m_response_data),
          [self = shared_from_this()](const boost::system::error_code& ec,
                                      std::size_t /*bytes*/) {
            if (!ec) {
              boost::system::error_code shutdown_ec;
              self->m_socket.shutdown(tcp::socket::shutdown_send, shutdown_ec);
            }
          });
    }
  }

  tcp::socket m_socket;
  boost::asio::streambuf m_buffer;
  std::string m_response_data;  // 用于安全地存储响应数据的成员变量

  // 通过引用访问服务器持有的共享数据
  std::map<std::string, std::set<std::string>>& m_peer_groups;
  std::mutex& m_mutex;
};

// TrackerServer类，负责接受连接和管理共享状态
class TrackerServer {
 public:
  TrackerServer(boost::asio::io_context& io_context, short port)
      : m_acceptor(io_context, tcp::endpoint(tcp::v4(), port)) {
    start_accept();
  }

 private:
  void start_accept() {
    m_acceptor.async_accept(
        [this](boost::system::error_code ec, tcp::socket socket) {
          if (!ec) {
            // 将共享数据和互斥锁的引用直接传递给Session
            std::make_shared<Session>(std::move(socket), m_peer_groups, m_mutex)
                ->start();
          }
          start_accept();
        });
  }

  tcp::acceptor m_acceptor;

  std::map<std::string, std::set<std::string>> m_peer_groups;
  std::mutex m_mutex;
};

int main() {
  try {
    boost::asio::io_context io_context;
    TrackerServer server(io_context, 9988);
    std::cout << "Tracker server started on port 9988..." << std::endl;
    io_context.run();
  } catch (const std::exception& e) {
    std::cerr << "Exception: " << e.what() << std::endl;
  }
  return 0;
}