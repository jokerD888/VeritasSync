#include "test_helpers.h"
#include <gtest/gtest.h>

#include "VeritasSync/net/StunProber.h"
#include "VeritasSync/common/Config.h"

#include <array>
#include <cstring>
#include <nlohmann/json.hpp>

using namespace VeritasSync;
using json = nlohmann::json;

REGISTER_VERITAS_TEST_ENV();

// ============================================================================
// StunProber: STUN 协议报文构造/解析测试
// ============================================================================

class StunProberTest : public ::testing::Test {};

TEST_F(StunProberTest, BuildBindingRequest_CorrectSize) {
    std::array<uint8_t, 12> txn_id{};
    auto packet = StunProber::build_binding_request(txn_id);

    // STUN Binding Request = 20 bytes header, no body
    EXPECT_EQ(packet.size(), 20u);
}

TEST_F(StunProberTest, BuildBindingRequest_CorrectHeader) {
    std::array<uint8_t, 12> txn_id{};
    auto packet = StunProber::build_binding_request(txn_id);

    // Message Type: 0x0001 (Binding Request)
    EXPECT_EQ(packet[0], 0x00);
    EXPECT_EQ(packet[1], 0x01);

    // Message Length: 0
    EXPECT_EQ(packet[2], 0x00);
    EXPECT_EQ(packet[3], 0x00);

    // Magic Cookie: 0x2112A442
    EXPECT_EQ(packet[4], 0x21);
    EXPECT_EQ(packet[5], 0x12);
    EXPECT_EQ(packet[6], 0xA4);
    EXPECT_EQ(packet[7], 0x42);
}

TEST_F(StunProberTest, BuildBindingRequest_TransactionIdCopied) {
    std::array<uint8_t, 12> txn_id{};
    auto packet = StunProber::build_binding_request(txn_id);

    // Transaction ID 应匹配
    EXPECT_EQ(std::memcmp(packet.data() + 8, txn_id.data(), 12), 0);
}

TEST_F(StunProberTest, BuildBindingRequest_UniqueTransactionIds) {
    std::array<uint8_t, 12> txn_id1{};
    std::array<uint8_t, 12> txn_id2{};
    StunProber::build_binding_request(txn_id1);
    StunProber::build_binding_request(txn_id2);

    // 两次生成的 Transaction ID 应该不同（极小概率相同，可以忽略）
    EXPECT_NE(txn_id1, txn_id2);
}

TEST_F(StunProberTest, ParseBindingResponse_ValidXorMappedAddress) {
    // 构造一个模拟的 STUN Binding Response
    // 目标：解析出 IP=203.0.113.1 (0xCB007101), Port=12345 (0x3039)

    std::array<uint8_t, 12> txn_id = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                        0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C};

    uint32_t magic_cookie = 0x2112A442;

    // XOR 编码
    uint16_t xor_port = 12345 ^ static_cast<uint16_t>(magic_cookie >> 16);  // 12345 ^ 0x2112
    uint32_t xor_addr = 0xCB007101 ^ magic_cookie;  // 203.0.113.1 ^ 0x2112A442

    // 构造报文
    std::vector<uint8_t> response;

    // Header: Binding Response (0x0101), Length=12, Magic Cookie, Transaction ID
    response.push_back(0x01); response.push_back(0x01);  // type
    response.push_back(0x00); response.push_back(0x0C);  // length = 12 (XOR-MAPPED-ADDRESS attr)
    response.push_back(0x21); response.push_back(0x12);  // magic cookie
    response.push_back(0xA4); response.push_back(0x42);
    for (auto b : txn_id) response.push_back(b);         // transaction id

    // Attribute: XOR-MAPPED-ADDRESS (0x0020), length=8
    response.push_back(0x00); response.push_back(0x20);  // attr type
    response.push_back(0x00); response.push_back(0x08);  // attr length

    // Value: reserved(0), family(0x01=IPv4), X-Port, X-Address
    response.push_back(0x00);  // reserved
    response.push_back(0x01);  // family: IPv4
    response.push_back(static_cast<uint8_t>((xor_port >> 8) & 0xFF));
    response.push_back(static_cast<uint8_t>(xor_port & 0xFF));
    response.push_back(static_cast<uint8_t>((xor_addr >> 24) & 0xFF));
    response.push_back(static_cast<uint8_t>((xor_addr >> 16) & 0xFF));
    response.push_back(static_cast<uint8_t>((xor_addr >> 8) & 0xFF));
    response.push_back(static_cast<uint8_t>(xor_addr & 0xFF));

    std::string ip;
    uint16_t port = 0;
    bool ok = StunProber::parse_binding_response(
        response.data(), response.size(), txn_id, ip, port);

    EXPECT_TRUE(ok);
    EXPECT_EQ(ip, "203.0.113.1");
    EXPECT_EQ(port, 12345);
}

TEST_F(StunProberTest, ParseBindingResponse_WrongTransactionId) {
    std::array<uint8_t, 12> txn_id = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                        0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C};
    std::array<uint8_t, 12> wrong_txn = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    // 用 wrong_txn 构造报文，但用 txn_id 去验证
    std::vector<uint8_t> response(32, 0);
    response[0] = 0x01; response[1] = 0x01;  // Binding Response
    response[2] = 0x00; response[3] = 0x0C;
    response[4] = 0x21; response[5] = 0x12;
    response[6] = 0xA4; response[7] = 0x42;
    std::memcpy(response.data() + 8, wrong_txn.data(), 12);

    // XOR-MAPPED-ADDRESS
    response[20] = 0x00; response[21] = 0x20;
    response[22] = 0x00; response[23] = 0x08;
    response[24] = 0x00; response[25] = 0x01;  // IPv4
    // ... port and addr don't matter

    std::string ip;
    uint16_t port = 0;
    bool ok = StunProber::parse_binding_response(
        response.data(), response.size(), txn_id, ip, port);

    EXPECT_FALSE(ok);  // Should fail due to transaction ID mismatch
}

TEST_F(StunProberTest, ParseBindingResponse_TooShort) {
    std::array<uint8_t, 12> txn_id{};
    std::vector<uint8_t> short_data(10, 0);  // Too short for STUN header

    std::string ip;
    uint16_t port = 0;
    EXPECT_FALSE(StunProber::parse_binding_response(
        short_data.data(), short_data.size(), txn_id, ip, port));
}

TEST_F(StunProberTest, ParseBindingResponse_WrongMessageType) {
    std::array<uint8_t, 12> txn_id{};

    std::vector<uint8_t> response(32, 0);
    response[0] = 0x00; response[1] = 0x01;  // Binding Request (not response!)
    response[4] = 0x21; response[5] = 0x12;
    response[6] = 0xA4; response[7] = 0x42;

    std::string ip;
    uint16_t port = 0;
    EXPECT_FALSE(StunProber::parse_binding_response(
        response.data(), response.size(), txn_id, ip, port));
}

// ============================================================================
// Config: extra_stun_servers 序列化测试
// ============================================================================

class ConfigMultiStunTest : public ::testing::Test {};

TEST_F(ConfigMultiStunTest, DefaultValues) {
    Config cfg;
    EXPECT_TRUE(cfg.network.enable_multi_stun_probing);
    EXPECT_FALSE(cfg.network.stun_list_url.empty());
    EXPECT_TRUE(cfg.network.extra_stun_servers.empty());
}

TEST_F(ConfigMultiStunTest, JsonRoundTrip_EnableFlag) {
    Config cfg;
    cfg.device_id = "test-uuid";
    cfg.network.enable_multi_stun_probing = false;
    cfg.network.stun_list_url = "https://example.com/stun.txt";

    json j = cfg;
    Config restored = j.get<Config>();

    EXPECT_FALSE(restored.network.enable_multi_stun_probing);
    EXPECT_EQ(restored.network.stun_list_url, "https://example.com/stun.txt");
}

TEST_F(ConfigMultiStunTest, JsonRoundTrip_WithExtraServers) {
    Config cfg;
    cfg.device_id = "test-uuid";
    cfg.network.extra_stun_servers.push_back({"stun1.example.com", 3478});
    cfg.network.extra_stun_servers.push_back({"stun2.example.com", 19302});

    json j = cfg;
    Config restored = j.get<Config>();

    ASSERT_EQ(restored.network.extra_stun_servers.size(), 2u);
    EXPECT_EQ(restored.network.extra_stun_servers[0].host, "stun1.example.com");
    EXPECT_EQ(restored.network.extra_stun_servers[0].port, 3478);
    EXPECT_EQ(restored.network.extra_stun_servers[1].host, "stun2.example.com");
    EXPECT_EQ(restored.network.extra_stun_servers[1].port, 19302);
}

TEST_F(ConfigMultiStunTest, JsonRoundTrip_EmptyServers_NotSerialized) {
    Config cfg;
    cfg.device_id = "test-uuid";
    // extra_stun_servers 为空，不应出现在 JSON 中
    json j = cfg;
    EXPECT_FALSE(j["network"].contains("extra_stun_servers"));
}

TEST_F(ConfigMultiStunTest, FromJson_MissingMultiStunFields_UseDefaults) {
    // 旧配置文件不含新字段
    json j = {
        {"network", {
            {"tracker_host", "1.2.3.4"},
            {"tracker_port", 9988}
        }},
        {"tasks", json::array()}
    };

    Config cfg = j.get<Config>();
    EXPECT_TRUE(cfg.network.enable_multi_stun_probing);  // 默认值
    EXPECT_FALSE(cfg.network.stun_list_url.empty());
    EXPECT_TRUE(cfg.network.extra_stun_servers.empty());
}

TEST_F(ConfigMultiStunTest, FromJson_EmptyHostFiltered) {
    json j = {
        {"network", {
            {"tracker_host", "1.2.3.4"},
            {"tracker_port", 9988},
            {"extra_stun_servers", json::array({
                {{"host", ""}, {"port", 3478}},  // 空 host，应被过滤
                {{"host", "valid.stun.com"}, {"port", 3478}}
            })}
        }},
        {"tasks", json::array()}
    };

    Config cfg = j.get<Config>();
    ASSERT_EQ(cfg.network.extra_stun_servers.size(), 1u);
    EXPECT_EQ(cfg.network.extra_stun_servers[0].host, "valid.stun.com");
}

TEST_F(ConfigMultiStunTest, Validation_InvalidExtraStunServer) {
    Config cfg;
    cfg.device_id = "test-uuid";
    cfg.network.tracker_host = "1.2.3.4";
    cfg.network.stun_host = "stun.l.google.com";

    // 添加一个空 host 的服务器（应该在 from_json 时过滤掉，但如果手动构造则验证应报错）
    cfg.network.extra_stun_servers.push_back({"", 3478});

    auto errors = validate_config(cfg);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("extra_stun_servers") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "Should report error for empty host in extra_stun_servers";
}
