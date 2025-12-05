#pragma once
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

namespace VeritasSync {

class P2PManager;

namespace SignalProto {
constexpr const char* MSG_TYPE = "type";
constexpr const char* MSG_PAYLOAD = "payload";
constexpr const char* TYPE_REGISTER = "REGISTER";
constexpr const char* TYPE_REG_ACK = "REG_ACK";
constexpr const char* TYPE_PEER_JOIN = "PEER_JOIN";
constexpr const char* TYPE_PEER_LEAVE = "PEER_LEAVE";
constexpr const char* TYPE_SIGNAL = "SIGNAL";
}  // namespace SignalProto

// --- 2. 添加 using ---
using boost::asio::ip::tcp;

class TrackerClient : public std::enable_shared_from_this<TrackerClient> {
   public:
    TrackerClient(std::string host, unsigned short port);
    ~TrackerClient();

    void set_p2p_manager(P2PManager* p2p);

    void connect(const std::string& sync_key, std::function<void(std::vector<std::string>)> on_ready);

    void send_signaling_message(const std::string& to_peer_id, const std::string& type, const std::string& sdp);
    std::string get_self_id() const { return m_self_id; }

    bool is_connected() const { return m_connected; }

private:
    void do_connect();
    void do_register();
    void do_read_header();
    void handle_read_header(const boost::system::error_code& ec, std::size_t bytes);
    void do_read_body(unsigned int msg_len);
    void handle_read_body(const boost::system::error_code& ec, std::size_t bytes);

    void handle_message(const nlohmann::json& msg);

    void do_write(const std::string& msg);
    void schedule_reconnect();
    void close_socket();

    boost::asio::io_context m_io_context;
    std::jthread m_thread;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> m_work_guard;

    // --- 3. 现在 tcp::socket 是已知的 ---
    tcp::socket m_socket;

    std::string m_host;
    unsigned short m_port;
    std::string m_sync_key;
    std::string m_self_id;

    std::function<void(std::vector<std::string>)> m_on_ready_callback;

    P2PManager* m_p2p_manager = nullptr;
    boost::asio::streambuf m_read_buffer;
    std::vector<char> m_write_buffer;

    boost::asio::steady_timer m_retry_timer;

    std::atomic<bool> m_connected{false};
};

}  // namespace VeritasSync
