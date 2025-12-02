#define CPPHTTPLIB_OPENSSL_SUPPORT  // 启用 HTTPS 支持
#include <httplib.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <boost/asio.hpp>
#include <boost/asio/detail/socket_ops.hpp>  // 用于网络字节序转换
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>

#if defined(_WIN32)
#include <locale.h>
#include <windows.h>
#endif

using boost::asio::ip::tcp;
using nlohmann::json;

std::shared_ptr<spdlog::logger> g_logger;

// (协议类型定义 - 必须与 TrackerClient 中的 SignalProto 匹配)
namespace SignalProto {
constexpr const char* MSG_TYPE = "type";
constexpr const char* MSG_PAYLOAD = "payload";
constexpr const char* TYPE_REGISTER = "REGISTER";
constexpr const char* TYPE_REG_ACK = "REG_ACK";
constexpr const char* TYPE_PEER_JOIN = "PEER_JOIN";
constexpr const char* TYPE_PEER_LEAVE = "PEER_LEAVE";
constexpr const char* TYPE_SIGNAL = "SIGNAL";
}  // namespace SignalProto

class TrackerServer;  // 前向声明

// Session 类现在处理持久连接和 JSON 消息
class Session : public std::enable_shared_from_this<Session> {
   public:
       Session(tcp::socket socket, TrackerServer& server)
           : m_socket(std::move(socket)),
             m_server(server)  // <-- 保存对 server 的引用
       {
           // 为此会话生成一个唯一的 ID
           std::stringstream ss;
           ss << m_socket.remote_endpoint().address().to_string() << ":" << m_socket.remote_endpoint().port();
           m_id = ss.str();
           g_logger->info("[Session] {} 已连接。", m_id);
       }

    ~Session() { g_logger->info("[Session] {} 已断开连接。", m_id); }

    void start() { do_read_header(); }

    // --- 获取此会话的唯一 ID ---
    std::string get_id() const { return m_id; }

    // --- 获取此会话所属的同步组 ---
    std::string get_sync_key() const { return m_sync_key; }

    // --- 异步发送 JSON 消息 ---
    void send(const json& msg);

   private:
    void do_read_header();
    void handle_read_header(const boost::system::error_code& ec, std::size_t bytes);
    void do_read_body(unsigned int msg_len);
    void handle_read_body(const boost::system::error_code& ec, std::size_t bytes);

    // --- 消息处理器 ---
    void handle_message(const json& msg);
    void handle_register(const json& payload);
    void handle_signal(const json& msg);

    void do_write();

    tcp::socket m_socket;
    TrackerServer& m_server;
    std::string m_id;
    std::string m_sync_key;

    boost::asio::streambuf m_read_buffer;
    std::vector<char> m_write_buffer;  // 用于写入
};

// TrackerServer 类，负责接受连接和管理会话
class TrackerServer {
   public:
       TrackerServer(boost::asio::io_context& io_context, short port)
           : m_acceptor(io_context, tcp::endpoint(tcp::v4(), port)) {
           start_accept();
       }

    // --- 会话管理 ---
    void join(std::shared_ptr<Session> session, const std::string& sync_key);
    void leave(std::shared_ptr<Session> session);
    void forward(const std::string& to_peer_id, const json& msg);

   private:
    void start_accept() {
        m_acceptor.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) {
                // 传递对 'this' (TrackerServer) 的引用
                std::make_shared<Session>(std::move(socket), *this)->start();
            }
            start_accept();
        });
    }

    tcp::acceptor m_acceptor;

    // --- 修改：数据结构 ---
    // (sync_key -> 一组 Session 指针)
    std::map<std::string, std::set<std::shared_ptr<Session>>> m_peer_groups;
    // (peer_id -> Session 指针，用于快速查找)
    std::map<std::string, std::shared_ptr<Session>> m_peers_by_id;
    std::mutex m_mutex;
};

// --- TrackerServer 方法实现 ---

void TrackerServer::join(std::shared_ptr<Session> session, const std::string& sync_key) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto& peer_set = m_peer_groups[sync_key];
    std::string new_peer_id = session->get_id();

    // 1. 准备 ACK 消息，包含当前房间中的所有 peers
    json reg_ack_payload;
    reg_ack_payload["self_id"] = new_peer_id;

    json peers_list = json::array();
    for (const auto& peer : peer_set) {
        peers_list.push_back(peer->get_id());
    }
    reg_ack_payload["peers"] = peers_list;

    json reg_ack_msg;
    reg_ack_msg[SignalProto::MSG_TYPE] = SignalProto::TYPE_REG_ACK;
    reg_ack_msg[SignalProto::MSG_PAYLOAD] = reg_ack_payload;

    session->send(reg_ack_msg);

    // 2. 准备 PEER_JOIN 广播消息
    json peer_join_payload;
    peer_join_payload["peer_id"] = new_peer_id;
    json peer_join_msg;
    peer_join_msg[SignalProto::MSG_TYPE] = SignalProto::TYPE_PEER_JOIN;
    peer_join_msg[SignalProto::MSG_PAYLOAD] = peer_join_payload;

    // 2.A. 广播 PEER_JOIN 给房间中所有 *其他* peers
    for (const auto& peer : peer_set) {
        peer->send(peer_join_msg);
    }

    // 3. 将新 peer 加入
    peer_set.insert(session);
    m_peers_by_id[new_peer_id] = session;

    g_logger->info("[Tracker] {} 已加入组 '{}'", new_peer_id, sync_key);
}

void TrackerServer::leave(std::shared_ptr<Session> session) {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::string peer_id = session->get_id();
    std::string sync_key = session->get_sync_key();
    if (sync_key.empty()) return;  // 从未注册

    // 1. 从 m_peers_by_id 移除
    m_peers_by_id.erase(peer_id);

    // 2. 从 m_peer_groups 移除
    auto it = m_peer_groups.find(sync_key);
    if (it != m_peer_groups.end()) {
        auto& peer_set = it->second;
        peer_set.erase(session);

        // 3. 准备 PEER_LEAVE 广播消息
        json peer_leave_payload;
        peer_leave_payload["peer_id"] = peer_id;
        json peer_leave_msg;
        peer_leave_msg[SignalProto::MSG_TYPE] = SignalProto::TYPE_PEER_LEAVE;
        peer_leave_msg[SignalProto::MSG_PAYLOAD] = peer_leave_payload;

        // 3.A. 广播 PEER_LEAVE 给房间中所有 *剩余* peers
        for (const auto& peer : peer_set) {
            peer->send(peer_leave_msg);
        }
        g_logger->info("[Tracker] {} 已离开组 '{}'", peer_id, sync_key);

        if (peer_set.empty()) {
            m_peer_groups.erase(it);
            g_logger->info("[Tracker] 组 '{}' 已清空。", sync_key);
        }
    }
}

void TrackerServer::forward(const std::string& to_peer_id, const json& msg) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_peers_by_id.find(to_peer_id);
    if (it != m_peers_by_id.end()) {
        // 找到目标，转发
        it->second->send(msg);
    } else {
        g_logger->warn("[Tracker] 无法转发：未找到 peer {}", to_peer_id);
    }
}

// --- Session 方法实现 ---

void Session::send(const json& msg) {
    std::string s = msg.dump();

    // 1. 准备 4 字节的头部
    uint32_t len_net = boost::asio::detail::socket_ops::host_to_network_long(static_cast<uint32_t>(s.length()));

    // 2. 准备写入缓冲区（头部 + 消息体）
    // (注意：这里没有使用 m_write_buffer，因为 send 可能被多个线程调用)
    // (一个更健壮的实现会使用一个带锁的队列)
    // (为简单起见，我们直接分配)
    auto write_buf = std::make_shared<std::vector<char>>(4 + s.length());
    std::memcpy(write_buf->data(), &len_net, 4);
    std::memcpy(write_buf->data() + 4, s.c_str(), s.length());

    // 3. 异步发送
    boost::asio::async_write(
        m_socket, boost::asio::buffer(*write_buf),
        [self = shared_from_this(), write_buf](const boost::system::error_code& ec, std::size_t bytes) {
            if (ec) {
                g_logger->error("[Session] {} 写入失败: {}", self->m_id, ec.message());
                // 写入失败时，读取循环会自动检测到 EOF 并触发 leave
            }
        });
}

void Session::do_read_header() {
    // 异步读取 4 字节的消息长度头部
    m_read_buffer.consume(m_read_buffer.size());
    boost::asio::async_read(m_socket, m_read_buffer, boost::asio::transfer_exactly(4),
                            [self = shared_from_this()](const boost::system::error_code& ec, std::size_t bytes) {
                                self->handle_read_header(ec, bytes);
                            });
}

void Session::handle_read_header(const boost::system::error_code& ec, std::size_t bytes) {
    if (ec) {
        if (ec != boost::asio::error::eof) {
#if defined(_WIN32)
            // 在Windows上，将系统错误消息转换为UTF-8
            LPWSTR messageBuffer = nullptr;
            size_t size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                         NULL, ec.value(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&messageBuffer, 0, NULL);
            std::wstring message(messageBuffer, size);
            LocalFree(messageBuffer);
            
            // 转换为UTF-8
            int size_needed = WideCharToMultiByte(CP_UTF8, 0, &message[0], (int)message.size(), NULL, 0, NULL, NULL);
            std::string utf8_message(size_needed, 0);
            WideCharToMultiByte(CP_UTF8, 0, &message[0], (int)message.size(), &utf8_message[0], size_needed, NULL, NULL);
            
            // 移除末尾的换行符
            utf8_message.erase(utf8_message.find_last_not_of("\r\n") + 1);
            
            g_logger->error("[Session] {} 读取头部失败: {}", m_id, utf8_message);
#else
            g_logger->error("[Session] {} 读取头部失败: {}", m_id, ec.message());
#endif
        } else {
            g_logger->warn("[Session] {} 触发 EOF。", m_id);
        }
        m_server.leave(shared_from_this());  // <-- 关键：断开连接时通知 server
        return;
    }

    std::istream is(&m_read_buffer);
    uint32_t msg_len_net;
    is.read(reinterpret_cast<char*>(&msg_len_net), 4);
    unsigned int msg_len = boost::asio::detail::socket_ops::network_to_host_long(msg_len_net);

    if (msg_len > 65536) {
        g_logger->error("[Session] {} 消息体过长 ( {} bytes)。断开连接。", m_id, msg_len);
        m_server.leave(shared_from_this());
        return;
    }

    do_read_body(msg_len);
}

void Session::do_read_body(unsigned int msg_len) {
    boost::asio::async_read(m_socket, m_read_buffer, boost::asio::transfer_exactly(msg_len),
                            [self = shared_from_this()](const boost::system::error_code& ec, std::size_t bytes) {
                                self->handle_read_body(ec, bytes);
                            });
}

void Session::handle_read_body(const boost::system::error_code& ec, std::size_t bytes) {
    if (ec) {
#if defined(_WIN32)
        // 在Windows上，将系统错误消息转换为UTF-8
        LPWSTR messageBuffer = nullptr;
        size_t size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                     NULL, ec.value(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&messageBuffer, 0, NULL);
        std::wstring message(messageBuffer, size);
        LocalFree(messageBuffer);
        
        // 转换为UTF-8
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &message[0], (int)message.size(), NULL, 0, NULL, NULL);
        std::string utf8_message(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &message[0], (int)message.size(), &utf8_message[0], size_needed, NULL, NULL);
        
        // 移除末尾的换行符
        utf8_message.erase(utf8_message.find_last_not_of("\r\n") + 1);
        
        g_logger->error("[Session] {} 读取消息体失败: {}", m_id, utf8_message);
#else
        g_logger->error("[Session] {} 读取消息体失败: {}", m_id, ec.message());
#endif
        m_server.leave(shared_from_this());
        return;
    }

    std::istream is(&m_read_buffer);
    std::string body(bytes, '\0');
    is.read(&body[0], bytes);

    try {
        json msg = json::parse(body);
        handle_message(msg);
    } catch (const std::exception& e) {
        g_logger->error("[Session] {} 解析 JSON 失败: {}", m_id, e.what());
    }

    // 循环读取下一条消息
    do_read_header();
}

void Session::handle_message(const json& msg) {
    try {
        const std::string type = msg.at(SignalProto::MSG_TYPE).get<std::string>();
        const auto& payload = msg.at(SignalProto::MSG_PAYLOAD);

        if (type == SignalProto::TYPE_REGISTER) {
            handle_register(payload);
        } else if (type == SignalProto::TYPE_SIGNAL) {
            handle_signal(msg);
        }

    } catch (const std::exception& e) {
        g_logger->error("[Session] {} 处理消息失败: {}", m_id, e.what());
    }
}

void Session::handle_register(const json& payload) {
    m_sync_key = payload.at("sync_key").get<std::string>();
    if (m_sync_key.empty()) {
        g_logger->error("[Session] {} 注册失败：sync_key 为空。", m_id);
        return;
    }

    // 通知 Server 将此 Session 加入
    m_server.join(shared_from_this(), m_sync_key);
}

void Session::handle_signal(const json& msg) {
    // --- 从 msg 中解析 payload ---
    const auto& payload = msg.at(SignalProto::MSG_PAYLOAD);
    std::string to_peer_id = payload.at("to").get<std::string>();

    if (to_peer_id.empty()) {
        g_logger->error("[Session] {} 转发失败：'to' 字段为空。", m_id);
        return;
    }

    // 通知 Server 转发此消息 (msg 现在在作用域中)
    m_server.forward(to_peer_id, msg);
}

void init_tracker_logger() {
    try {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(spdlog::level::debug);

        // Tracker 比较简单，只输出到控制台
        g_logger = std::make_shared<spdlog::logger>("veritas_tracker", spdlog::sinks_init_list{console_sink});

        g_logger->set_level(spdlog::level::info);  // 默认级别设为 info
        g_logger->flush_on(spdlog::level::info);

        spdlog::register_logger(g_logger);
        spdlog::set_default_logger(g_logger);
    } catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "Log initialization failed: " << ex.what() << std::endl;
        exit(1);
    }
}

// --- main ---
int main() {
#if defined(_WIN32)
    // 设置控制台编码为UTF-8
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);  // 输入编码也设置UTF-8
    // 设置区域为UTF-8，确保系统错误消息正确显示
    std::setlocale(LC_ALL, ".UTF-8");
#endif

    init_tracker_logger();
    try {
        boost::asio::io_context io_context;
        TrackerServer server(io_context, 9988);
        g_logger->info("Tracker server (JSON async) started on port 9988...");
        io_context.run();
    } catch (const std::exception& e) {
        g_logger->critical("Exception: {}", e.what());
    }
    return 0;
}
