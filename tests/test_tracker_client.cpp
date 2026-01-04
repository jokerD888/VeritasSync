#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include <thread>
#include <deque>
#include <memory>
#include "VeritasSync/p2p/TrackerClient.h"
#include "VeritasSync/p2p/P2PManager.h"
#include "VeritasSync/common/Logger.h"

using namespace VeritasSync;
using boost::asio::ip::tcp;

// --- Mock P2PManager ---
class MockP2PManager : public P2PManager {
public:
    MockP2PManager(boost::asio::io_context& ctx) : m_ctx(ctx) {}
    
    boost::asio::io_context& get_io_context() override { return m_ctx; }
    
    void connect_to_peers(const std::vector<std::string>& peer_addresses) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connected_peers.insert(m_connected_peers.end(), peer_addresses.begin(), peer_addresses.end());
    }
    
    void handle_peer_leave(const std::string& peer_id) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_left_peers.push_back(peer_id);
    }
    
    void handle_signaling_message(const std::string& from, const std::string& type, const std::string& sdp) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_signals.push_back({from, type, sdp});
    }

    struct Signal { std::string from, type, sdp; };
    std::vector<std::string> m_connected_peers;
    std::vector<std::string> m_left_peers;
    std::vector<Signal> m_signals;
    std::mutex m_mutex;

private:
    boost::asio::io_context& m_ctx;
};

// --- Mock Tracker Server ---
class MockTrackerServer {
public:
    MockTrackerServer(boost::asio::io_context& ctx, unsigned short port)
        : m_acceptor(ctx, tcp::endpoint(tcp::v4(), port)) {
        do_accept();
    }

    void stop() {
        m_acceptor.close();
        std::lock_guard<std::mutex> lock(m_sessions_mutex);
        for (auto& weak_session : m_sessions) {
            if (auto session = weak_session.lock()) {
                session->close();
            }
        }
        m_sessions.clear();
    }

private:
    void do_accept() {
        m_acceptor.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) {
                auto session = std::make_shared<Session>(std::move(socket));
                {
                    std::lock_guard<std::mutex> lock(m_sessions_mutex);
                    m_sessions.push_back(session);
                }
                session->start();
            }
            if (m_acceptor.is_open()) {
                do_accept();
            }
        });
    }

    struct Session : std::enable_shared_from_this<Session> {
        tcp::socket socket;
        boost::asio::streambuf buffer;

        Session(tcp::socket s) : socket(std::move(s)) {}

        void start() { do_read_header(); }
        void close() {
            boost::system::error_code ec;
            socket.close(ec);
        }

        void do_read_header() {
            auto self = shared_from_this();
            boost::asio::async_read(socket, buffer, boost::asio::transfer_exactly(4),
                [self](boost::system::error_code ec, std::size_t) {
                    if (!ec) {
                        std::istream is(&self->buffer);
                        uint32_t len_net;
                        is.read(reinterpret_cast<char*>(&len_net), 4);
                        uint32_t len = boost::asio::detail::socket_ops::network_to_host_long(len_net);
                        self->do_read_body(len);
                    }
                });
        }

        void do_read_body(uint32_t len) {
            auto self = shared_from_this();
            boost::asio::async_read(socket, buffer, boost::asio::transfer_exactly(len),
                [self, len](boost::system::error_code ec, std::size_t) {
                    if (!ec) {
                        std::istream is(&self->buffer);
                        std::string body(len, '\0');
                        is.read(&body[0], len);
                        self->handle_packet(body);
                    }
                });
        }

        void handle_packet(const std::string& body) {
            try {
                auto j = nlohmann::json::parse(body);
                std::string type = j["type"];
                if (type == "REGISTER") {
                    nlohmann::json resp;
                    resp["type"] = "REG_ACK";
                    resp["payload"] = {
                        {"self_id", "my_id"},
                        {"peers", std::vector<std::string>{"peer_A", "peer_B"}}
                    };
                    send_packet(resp.dump());
                } else if (type == "SIGNAL") {
                    send_packet(body); 
                }
            } catch(...) {}
            if (socket.is_open()) {
                do_read_header();
            }
        }

        void send_packet(const std::string& msg) {
            if (!socket.is_open()) return;
            auto len_net = boost::asio::detail::socket_ops::host_to_network_long(static_cast<uint32_t>(msg.length()));
            auto out_buf = std::make_shared<std::string>();
            out_buf->resize(4 + msg.length());
            std::memcpy(&(*out_buf)[0], &len_net, 4);
            std::memcpy(&(*out_buf)[4], msg.data(), msg.length());
            
            auto self = shared_from_this();
            boost::asio::async_write(socket, boost::asio::buffer(*out_buf),
                [self, out_buf](boost::system::error_code, std::size_t) {});
        }
    };

    std::vector<std::weak_ptr<Session>> m_sessions;
    std::mutex m_sessions_mutex;
    
public:
    tcp::acceptor m_acceptor;
};

class TrackerClientTest : public ::testing::Test {
protected:
    boost::asio::io_context m_server_ctx;
    std::unique_ptr<MockTrackerServer> m_server;
    std::jthread m_server_thread;
    unsigned short m_port = 0;

    void SetUp() override {
        init_logger();
        m_server = std::make_unique<MockTrackerServer>(m_server_ctx, m_port);
        // 如果端口是 0，获取实际绑定的端口
        if (m_port == 0) {
            m_port = m_server->m_acceptor.local_endpoint().port();
        }
        m_server_thread = std::jthread([this]() { m_server_ctx.run(); });
    }

    void TearDown() override {
        m_server_ctx.stop();
        if (m_server_thread.joinable()) m_server_thread.join();
    }
};

TEST_F(TrackerClientTest, ConnectionAndRegistration) {
    boost::asio::io_context p2p_ctx;
    auto p2p_work = boost::asio::make_work_guard(p2p_ctx);
    std::jthread p2p_thread([&]() { p2p_ctx.run(); });

    auto client = std::make_shared<TrackerClient>("127.0.0.1", m_port);
    MockP2PManager mock_p2p(p2p_ctx);
    client->set_p2p_manager(&mock_p2p);

    std::promise<std::vector<std::string>> ready_promise;
    auto ready_future = ready_promise.get_future();

    client->connect("test_sync_key", [&](std::vector<std::string> peers) {
        ready_promise.set_value(peers);
    });

    // 等待注册完成
    if (ready_future.wait_for(std::chrono::seconds(10)) == std::future_status::ready) {
        auto peers = ready_future.get();
        EXPECT_EQ(peers.size(), 2);
        EXPECT_EQ(peers[0], "peer_A");
        EXPECT_TRUE(client->is_connected());
        EXPECT_EQ(client->get_self_id(), "my_id");
    } else {
        FAIL() << "Registration timed out";
    }

    client->stop();
    p2p_ctx.stop();
}

TEST_F(TrackerClientTest, ConcurrencyWriteQueueStress) {
    boost::asio::io_context p2p_ctx;
    auto p2p_work = boost::asio::make_work_guard(p2p_ctx);
    std::jthread p2p_thread([&]() { p2p_ctx.run(); });

    auto client = std::make_shared<TrackerClient>("127.0.0.1", m_port);
    MockP2PManager mock_p2p(p2p_ctx);
    client->set_p2p_manager(&mock_p2p);

    std::promise<void> ready_promise;
    auto ready_future = ready_promise.get_future();
    client->connect("test_sync_key", [&](auto) { ready_promise.set_value(); });
    
    ASSERT_EQ(ready_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);

    // 压力测试：并发发送大量信令
    const int MSG_COUNT = 100;
    for (int i = 0; i < MSG_COUNT; ++i) {
        client->send_signaling_message("target", "offer", "sdp_data_" + std::to_string(i));
    }

    // 等待一段时间让消息处理完 (Mock server 会回传消息，消息会进入 mock_p2p)
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    std::lock_guard<std::mutex> lock(mock_p2p.m_mutex);
    // 验证消息是否至少成功通过了队列并发送到了 MockServer (Server 逻辑是原样返回)
    EXPECT_GT(mock_p2p.m_signals.size(), 0);

    client->stop();
    p2p_ctx.stop();
}

TEST_F(TrackerClientTest, ReconnectionLogic) {
    boost::asio::io_context p2p_ctx;
    auto p2p_work = boost::asio::make_work_guard(p2p_ctx);
    std::jthread p2p_thread([&]() { p2p_ctx.run(); });

    auto client = std::make_shared<TrackerClient>("127.0.0.1", m_port);
    MockP2PManager mock_p2p(p2p_ctx);
    client->set_p2p_manager(&mock_p2p);

    std::promise<void> connected_p;
    client->connect("test_sync_key", [&](auto){ connected_p.set_value(); });
    connected_p.get_future().wait();
    EXPECT_TRUE(client->is_connected());
    
    // 关闭服务器并关闭物理连接
    g_logger->info("Stopping Mock Server...");
    m_server->stop();
    m_server_ctx.stop();
    if (m_server_thread.joinable()) m_server_thread.join();
    
    // 等待 Client 感知到连接断开
    int retry = 50;
    while(client->is_connected() && retry-- > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_FALSE(client->is_connected());

    // 重启服务器
    g_logger->info("Restarting Mock Server...");
    m_server_ctx.restart();
    m_server = std::make_unique<MockTrackerServer>(m_server_ctx, m_port);
    m_server_thread = std::jthread([this]() { m_server_ctx.run(); });

    // 等待重连 (重连周期 5s)
    g_logger->info("Waiting for reconnection (approx 5s)...");
    retry = 200; // 200 * 50ms = 10s max
    bool reconnected = false;
    while(retry-- > 0) {
        if (client->is_connected()) {
            reconnected = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_TRUE(reconnected);
    client->stop();
    p2p_ctx.stop();
}
