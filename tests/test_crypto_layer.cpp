// tests/test_crypto_layer.cpp
// 测试加密层的正确性和安全性

#include <gtest/gtest.h>
#include <string>

#include "VeritasSync/common/CryptoLayer.h"
#include "VeritasSync/common/Logger.h"

using namespace VeritasSync;

// 全局测试环境：初始化 Logger
class CryptoTestEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        init_logger();
    }
};

static ::testing::Environment* const crypto_env =
    ::testing::AddGlobalTestEnvironment(new CryptoTestEnvironment());

class CryptoLayerTest : public ::testing::Test {
protected:
    CryptoLayer crypto;
    
    void SetUp() override {
        // 使用固定密钥进行测试
        crypto.set_key("test_encryption_key_12345");
    }
};

// --- 基础功能测试 ---

TEST_F(CryptoLayerTest, EncryptDecryptSimple) {
    std::string plaintext = "Hello, World!";
    
    std::string ciphertext = crypto.encrypt(plaintext);
    ASSERT_FALSE(ciphertext.empty());
    EXPECT_NE(ciphertext, plaintext); // 密文应与明文不同
    
    std::string decrypted = crypto.decrypt(ciphertext);
    EXPECT_EQ(decrypted, plaintext);
}

TEST_F(CryptoLayerTest, EncryptDecryptEmpty) {
    std::string plaintext = "";
    
    std::string ciphertext = crypto.encrypt(plaintext);
    ASSERT_FALSE(ciphertext.empty()); // 即使明文为空，密文也应包含 IV 和 Tag
    
    std::string decrypted = crypto.decrypt(ciphertext);
    EXPECT_EQ(decrypted, plaintext);
}

TEST_F(CryptoLayerTest, EncryptDecryptBinaryData) {
    // 测试二进制数据（包含 null 字节）
    std::string plaintext(256, '\0');
    for (size_t i = 0; i < plaintext.size(); ++i) {
        plaintext[i] = static_cast<char>(i);
    }
    
    std::string ciphertext = crypto.encrypt(plaintext);
    ASSERT_FALSE(ciphertext.empty());
    
    std::string decrypted = crypto.decrypt(ciphertext);
    EXPECT_EQ(decrypted.size(), plaintext.size());
    EXPECT_EQ(decrypted, plaintext);
}

TEST_F(CryptoLayerTest, EncryptDecryptLargeData) {
    // 测试大数据加解密
    std::string plaintext(1024 * 1024, 'X'); // 1MB
    
    std::string ciphertext = crypto.encrypt(plaintext);
    ASSERT_FALSE(ciphertext.empty());
    
    std::string decrypted = crypto.decrypt(ciphertext);
    EXPECT_EQ(decrypted, plaintext);
}

// --- 随机性测试 ---

TEST_F(CryptoLayerTest, EncryptProducesRandomIV) {
    std::string plaintext = "Same message";
    
    // 同一明文多次加密应产生不同密文 (因为 IV 随机)
    std::string ciphertext1 = crypto.encrypt(plaintext);
    std::string ciphertext2 = crypto.encrypt(plaintext);
    std::string ciphertext3 = crypto.encrypt(plaintext);
    
    EXPECT_NE(ciphertext1, ciphertext2);
    EXPECT_NE(ciphertext2, ciphertext3);
    EXPECT_NE(ciphertext1, ciphertext3);
    
    // 但解密后应该相同
    EXPECT_EQ(crypto.decrypt(ciphertext1), plaintext);
    EXPECT_EQ(crypto.decrypt(ciphertext2), plaintext);
    EXPECT_EQ(crypto.decrypt(ciphertext3), plaintext);
}

// --- 密钥测试 ---

TEST_F(CryptoLayerTest, DifferentKeysProduceDifferentResults) {
    std::string plaintext = "Secret message";
    
    CryptoLayer crypto1, crypto2;
    crypto1.set_key("key_one_123");
    crypto2.set_key("key_two_456");
    
    std::string ciphertext1 = crypto1.encrypt(plaintext);
    std::string ciphertext2 = crypto2.encrypt(plaintext);
    
    // 不同密钥产生不同密文
    EXPECT_NE(ciphertext1, ciphertext2);
    
    // 解密时必须使用正确的密钥
    EXPECT_EQ(crypto1.decrypt(ciphertext1), plaintext);
    EXPECT_EQ(crypto2.decrypt(ciphertext2), plaintext);
}

TEST_F(CryptoLayerTest, WrongKeyFailsDecryption) {
    std::string plaintext = "Secret message";
    std::string ciphertext = crypto.encrypt(plaintext);
    
    // 使用错误密钥解密
    CryptoLayer wrong_crypto;
    wrong_crypto.set_key("wrong_key");
    
    std::string result = wrong_crypto.decrypt(ciphertext);
    
    // 应该返回空字符串或解密失败
    EXPECT_TRUE(result.empty() || result != plaintext);
}

// --- 篡改检测测试 ---

TEST_F(CryptoLayerTest, TamperedCiphertextFailsDecryption) {
    std::string plaintext = "Authenticated message";
    std::string ciphertext = crypto.encrypt(plaintext);
    
    // 篡改密文
    if (!ciphertext.empty()) {
        ciphertext[ciphertext.size() / 2] ^= 0xFF;
    }
    
    // 解密应该失败 (GCM 模式会检测篡改)
    std::string result = crypto.decrypt(ciphertext);
    EXPECT_TRUE(result.empty() || result != plaintext);
}

TEST_F(CryptoLayerTest, TruncatedCiphertextFailsDecryption) {
    std::string plaintext = "Message to be truncated";
    std::string ciphertext = crypto.encrypt(plaintext);
    
    // 截断密文
    if (ciphertext.size() > 10) {
        ciphertext = ciphertext.substr(0, ciphertext.size() - 10);
    }
    
    std::string result = crypto.decrypt(ciphertext);
    EXPECT_TRUE(result.empty());
}

// --- 边界情况测试 ---

TEST_F(CryptoLayerTest, DecryptInvalidData) {
    std::string invalid_data = "not_encrypted_data";
    std::string result = crypto.decrypt(invalid_data);
    EXPECT_TRUE(result.empty());
}

TEST_F(CryptoLayerTest, DecryptEmptyString) {
    std::string result = crypto.decrypt("");
    EXPECT_TRUE(result.empty());
}

TEST_F(CryptoLayerTest, MultipleRoundtrips) {
    // 测试多次加解密的稳定性
    std::string plaintext = "Stability test message";
    
    for (int i = 0; i < 100; ++i) {
        std::string ciphertext = crypto.encrypt(plaintext);
        std::string decrypted = crypto.decrypt(ciphertext);
        ASSERT_EQ(decrypted, plaintext) << "Failed at iteration " << i;
    }
}

// --- 密钥检查测试 ---

TEST_F(CryptoLayerTest, HasKeyAfterSet) {
    CryptoLayer c;
    EXPECT_FALSE(c.has_key());
    
    c.set_key("some_key");
    EXPECT_TRUE(c.has_key());
}

TEST_F(CryptoLayerTest, SameKeyProducesSameDecryption) {
    CryptoLayer crypto1, crypto2;
    std::string password = "my_secret_password";
    
    crypto1.set_key(password);
    crypto2.set_key(password);
    
    std::string plaintext = "Test message";
    
    // 相同密码派生的密钥应该能相互解密
    std::string ciphertext = crypto1.encrypt(plaintext);
    std::string decrypted = crypto2.decrypt(ciphertext);
    
    EXPECT_EQ(decrypted, plaintext);
}
