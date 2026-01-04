#pragma once
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <deque>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>
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

using boost::asio::ip::tcp;

class TrackerClient : public std::enable_shared_from_this<TrackerClient> {
   public:
    enum class State { DISCONNECTED, CONNECTING, REGISTERING, CONNECTED };

    static constexpr size_t MAX_PACKET_SIZE = 1024 * 1024;  // 1MB
    static constexpr std::chrono::seconds RECONNECT_INTERVAL{5};

    TrackerClient(std::string host, unsigned short port);
    ~TrackerClient();

    void set_p2p_manager(P2PManager* p2p);
    void set_device_id(const std::string& device_id);

    void connect(const std::string& sync_key, std::function<void(std::vector<std::string>)> on_ready);
    void stop();

    void send_signaling_message(const std::string& to_peer_id, const std::string& type, const std::string& sdp);
    std::string get_self_id() const { return m_self_id; }

    bool is_connected() const { return m_state == State::CONNECTED; }

private:
    void do_connect();
    void do_register();
    void do_read_header();
    void do_read_body(unsigned int msg_len);

    void handle_message(const nlohmann::json& msg);
    void register_handlers();

    void do_write(const std::string& msg);
    void start_write_next();
    void schedule_reconnect();
    void close_socket();

    boost::asio::io_context m_io_context;
    std::jthread m_thread;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> m_work_guard;

    tcp::resolver m_resolver;
    tcp::socket m_socket;

    std::string m_host;
    unsigned short m_port;
    std::string m_sync_key;
    std::string m_self_id;
    std::string m_device_id;  // 设备唯一标识符，用于 Tracker 注册

    std::function<void(std::vector<std::string>)> m_on_ready_callback;

    P2PManager* m_p2p_manager = nullptr;
    boost::asio::streambuf m_read_buffer;
    std::deque<std::string> m_write_queue;

    boost::asio::steady_timer m_retry_timer;

    using MessageHandler = std::function<void(const nlohmann::json&)>;
    std::unordered_map<std::string, MessageHandler> m_handlers;

    std::atomic<State> m_state{State::DISCONNECTED};
};

}  // namespace VeritasSync
