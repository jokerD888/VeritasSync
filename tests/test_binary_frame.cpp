// tests/test_binary_frame.cpp
// 测试统一二进制 Frame 协议的编解码正确性

#include <gtest/gtest.h>
#include <string>
#include <cstdint>

#include "VeritasSync/net/BinaryFrame.h"

using namespace VeritasSync;

class BinaryFrameTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// --- 编码测试 ---

TEST_F(BinaryFrameTest, EncodeJsonMessage) {
    std::string payload = R"({"type":"hello","data":123})";
    std::string frame = BinaryFrame::encode_json(payload);
    
    // 检查帧长度
    ASSERT_EQ(frame.size(), BinaryFrame::HEADER_SIZE + payload.size());
    
    // 检查魔数
    EXPECT_EQ(static_cast<uint8_t>(frame[0]), BinaryFrame::MAGIC_BYTE_1);
    EXPECT_EQ(static_cast<uint8_t>(frame[1]), BinaryFrame::MAGIC_BYTE_2);
    
    // 检查消息类型
    EXPECT_EQ(static_cast<uint8_t>(frame[2]), static_cast<uint8_t>(BinaryFrame::MessageType::JSON));
}

TEST_F(BinaryFrameTest, EncodeBinaryChunk) {
    std::string binary_data(1024, '\x00');
    for (size_t i = 0; i < binary_data.size(); ++i) {
        binary_data[i] = static_cast<char>(i % 256);
    }
    
    std::string frame = BinaryFrame::encode_binary_chunk(binary_data);
    
    ASSERT_EQ(frame.size(), BinaryFrame::HEADER_SIZE + binary_data.size());
    EXPECT_EQ(static_cast<uint8_t>(frame[2]), static_cast<uint8_t>(BinaryFrame::MessageType::BINARY_CHUNK));
}

TEST_F(BinaryFrameTest, EncodeEmptyPayload) {
    std::string empty_payload = "";
    std::string frame = BinaryFrame::encode_json(empty_payload);
    
    ASSERT_EQ(frame.size(), BinaryFrame::HEADER_SIZE);
    
    // 检查长度字段应为 0
    uint32_t len = 
        (static_cast<uint32_t>(static_cast<uint8_t>(frame[3])) << 24) |
        (static_cast<uint32_t>(static_cast<uint8_t>(frame[4])) << 16) |
        (static_cast<uint32_t>(static_cast<uint8_t>(frame[5])) << 8) |
        static_cast<uint32_t>(static_cast<uint8_t>(frame[6]));
    EXPECT_EQ(len, 0);
}

// --- 解码测试 ---

TEST_F(BinaryFrameTest, DecodeJsonMessage) {
    std::string original_payload = R"({"message":"test"})";
    std::string frame = BinaryFrame::encode_json(original_payload);
    
    auto result = BinaryFrame::decode(frame);
    
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type, BinaryFrame::MessageType::JSON);
    EXPECT_EQ(result->payload, original_payload);
}

TEST_F(BinaryFrameTest, DecodeBinaryChunk) {
    std::string original_data = "binary\x00data\xFFtest";
    std::string frame = BinaryFrame::encode_binary_chunk(original_data);
    
    auto result = BinaryFrame::decode(frame);
    
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type, BinaryFrame::MessageType::BINARY_CHUNK);
    EXPECT_EQ(result->payload, original_data);
}

TEST_F(BinaryFrameTest, DecodeEmptyPayload) {
    std::string frame = BinaryFrame::encode_json("");
    auto result = BinaryFrame::decode(frame);
    
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload, "");
}

// --- 错误处理测试 ---

TEST_F(BinaryFrameTest, DecodeInvalidMagic) {
    std::string invalid_frame = "XXYYZZZ";
    auto result = BinaryFrame::decode(invalid_frame);
    
    EXPECT_FALSE(result.has_value());
}

TEST_F(BinaryFrameTest, DecodeTooShort) {
    std::string too_short = "VS";  // 只有魔数，缺少其他字段
    auto result = BinaryFrame::decode(too_short);
    
    EXPECT_FALSE(result.has_value());
}

TEST_F(BinaryFrameTest, DecodeIncompletePayload) {
    // 创建一个声称有 1000 字节载荷但实际只有 10 字节的帧
    std::string frame;
    frame.push_back(static_cast<char>(BinaryFrame::MAGIC_BYTE_1));
    frame.push_back(static_cast<char>(BinaryFrame::MAGIC_BYTE_2));
    frame.push_back(static_cast<char>(BinaryFrame::MessageType::JSON));
    frame.push_back(static_cast<char>(0x00));
    frame.push_back(static_cast<char>(0x00));
    frame.push_back(static_cast<char>(0x03));
    frame.push_back(static_cast<char>(0xE8)); // 1000 in big-endian
    frame.append("only10byte");
    
    auto result = BinaryFrame::decode(frame);
    
    EXPECT_FALSE(result.has_value());
}

// --- 往返测试 (Roundtrip) ---

TEST_F(BinaryFrameTest, RoundtripLargePayload) {
    // 测试大载荷的编解码正确性
    std::string large_payload(65536, 'A');
    for (size_t i = 0; i < large_payload.size(); ++i) {
        large_payload[i] = static_cast<char>(i % 256);
    }
    
    std::string frame = BinaryFrame::encode_json(large_payload);
    auto result = BinaryFrame::decode(frame);
    
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload.size(), large_payload.size());
    EXPECT_EQ(result->payload, large_payload);
}

// --- 消息类型判断测试 ---

TEST_F(BinaryFrameTest, IsJsonMessage) {
    EXPECT_TRUE(BinaryFrame::is_json_message(BinaryFrame::MessageType::JSON));
    EXPECT_FALSE(BinaryFrame::is_json_message(BinaryFrame::MessageType::BINARY_CHUNK));
}

TEST_F(BinaryFrameTest, IsBinaryMessage) {
    EXPECT_TRUE(BinaryFrame::is_binary_message(BinaryFrame::MessageType::BINARY_CHUNK));
    EXPECT_TRUE(BinaryFrame::is_binary_message(BinaryFrame::MessageType::BINARY_ACK));
    EXPECT_FALSE(BinaryFrame::is_binary_message(BinaryFrame::MessageType::JSON));
}
