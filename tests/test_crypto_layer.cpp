// tests/test_crypto_layer.cpp
// 测试加密层的正确性和安全性

#include "test_helpers.h"
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>
#include <future>
#include <chrono>

#include "VeritasSync/common/CryptoLayer.h"

using namespace VeritasSync;

REGISTER_VERITAS_TEST_ENV();

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

// =====================================================
// 移动语义测试 (新增优化相关)
// =====================================================

TEST_F(CryptoLayerTest, MoveConstruction) {
    CryptoLayer source;
    source.set_key("test_key");
    std::string plaintext = "Move test";
    
    // 加密一些数据
    std::string ciphertext = source.encrypt(plaintext);
    ASSERT_FALSE(ciphertext.empty());
    
    // 移动构造
    CryptoLayer target(std::move(source));
    
    // 目标应该能正常工作
    EXPECT_TRUE(target.has_key());
    std::string decrypted = target.decrypt(ciphertext);
    EXPECT_EQ(decrypted, plaintext);
}

TEST_F(CryptoLayerTest, MoveAssignment) {
    CryptoLayer source;
    source.set_key("source_key");
    std::string plaintext = "Assignment test";
    std::string ciphertext = source.encrypt(plaintext);
    
    CryptoLayer target;
    target = std::move(source);
    
    EXPECT_TRUE(target.has_key());
    std::string decrypted = target.decrypt(ciphertext);
    EXPECT_EQ(decrypted, plaintext);
}

// =====================================================
// 多线程安全性测试 (新增优化相关)
// =====================================================

TEST_F(CryptoLayerTest, ThreadSafety_ConcurrentEncrypt) {
    // 测试并发加密是否安全
    const int NUM_THREADS = 10;
    const int OPS_PER_THREAD = 100;
    
    std::vector<std::future<bool>> futures;
    std::string plaintext = "Concurrent encryption test";
    
    for (int t = 0; t < NUM_THREADS; ++t) {
        futures.push_back(std::async(std::launch::async, [&]() {
            for (int i = 0; i < OPS_PER_THREAD; ++i) {
                std::string ciphertext = crypto.encrypt(plaintext);
                if (ciphertext.empty()) return false;
                
                std::string decrypted = crypto.decrypt(ciphertext);
                if (decrypted != plaintext) return false;
            }
            return true;
        }));
    }
    
    for (auto& f : futures) {
        EXPECT_TRUE(f.get());
    }
}

TEST_F(CryptoLayerTest, ThreadSafety_ConcurrentDecrypt) {
    // 先加密一些数据
    std::vector<std::string> ciphertexts;
    std::string plaintext = "Concurrent decryption test";
    for (int i = 0; i < 100; ++i) {
        ciphertexts.push_back(crypto.encrypt(plaintext));
    }
    
    // 并发解密
    const int NUM_THREADS = 10;
    std::vector<std::future<bool>> futures;
    
    for (int t = 0; t < NUM_THREADS; ++t) {
        futures.push_back(std::async(std::launch::async, [&, t]() {
            for (int i = 0; i < 100; ++i) {
                int idx = (t * 10 + i) % ciphertexts.size();
                std::string decrypted = crypto.decrypt(ciphertexts[idx]);
                if (decrypted != plaintext) return false;
            }
            return true;
        }));
    }
    
    for (auto& f : futures) {
        EXPECT_TRUE(f.get());
    }
}

TEST_F(CryptoLayerTest, ThreadSafety_MixedOperations) {
    // 混合读写操作
    const int NUM_THREADS = 8;
    std::atomic<int> success_count{0};
    std::atomic<int> failure_count{0};
    std::string plaintext = "Mixed operations test";
    
    std::vector<std::thread> threads;
    
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < 50; ++i) {
                std::string ciphertext = crypto.encrypt(plaintext);
                std::string decrypted = crypto.decrypt(ciphertext);
                
                if (decrypted == plaintext) {
                    success_count++;
                } else {
                    failure_count++;
                }
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(failure_count.load(), 0);
    EXPECT_EQ(success_count.load(), NUM_THREADS * 50);
}

// =====================================================
// 性能测试 (新增优化相关)
// =====================================================

TEST_F(CryptoLayerTest, Performance_ManySmallMessages) {
    // 测试大量小消息的加解密性能
    std::string plaintext = "Short message";
    const int NUM_ITERATIONS = 1000;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        std::string ciphertext = crypto.encrypt(plaintext);
        std::string decrypted = crypto.decrypt(ciphertext);
        ASSERT_EQ(decrypted, plaintext);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    std::cout << "[Performance] " << NUM_ITERATIONS << " small messages in " << duration << "ms\n";
    
    // 应该在合理时间内完成 (< 5 秒)
    EXPECT_LT(duration, 5000);
}

TEST_F(CryptoLayerTest, Performance_LargeData) {
    // 测试大数据块的加解密性能
    std::string plaintext(10 * 1024 * 1024, 'X');  // 10MB
    
    auto start = std::chrono::high_resolution_clock::now();
    
    std::string ciphertext = crypto.encrypt(plaintext);
    std::string decrypted = crypto.decrypt(ciphertext);
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    EXPECT_EQ(decrypted, plaintext);
    
    std::cout << "[Performance] 10MB data in " << duration << "ms\n";
    
    // 应该在合理时间内完成 (< 2 秒)
    EXPECT_LT(duration, 2000);
}

// =====================================================
// 密钥重置测试
// =====================================================

TEST_F(CryptoLayerTest, KeyReset_InvalidatesOldCiphertext) {
    std::string plaintext = "Key reset test";
    std::string ciphertext = crypto.encrypt(plaintext);
    
    // 验证可以解密
    EXPECT_EQ(crypto.decrypt(ciphertext), plaintext);
    
    // 重置密钥
    crypto.set_key("new_different_key");
    
    // 旧密文应该无法解密
    std::string result = crypto.decrypt(ciphertext);
    EXPECT_TRUE(result.empty() || result != plaintext);
    
    // 但新加密的数据应该能正常工作
    std::string new_ciphertext = crypto.encrypt(plaintext);
    EXPECT_EQ(crypto.decrypt(new_ciphertext), plaintext);
}

// =====================================================
// 压力与缓存一致性测试 (针对 thread_local 优化)
// =====================================================

TEST_F(CryptoLayerTest, ThreadSafety_MultipleInstancesConcurrent) {
    // 验证多个实例在多个线程中穿插使用，是否会因为共享 thread_local 上下文而冲突
    const int NUM_THREADS = 8;
    const int NUM_INSTANCES = 4;
    
    std::vector<std::unique_ptr<CryptoLayer>> instances;
    for(int i=0; i<NUM_INSTANCES; ++i) {
        auto c = std::make_unique<CryptoLayer>();
        c->set_key("instance_key_" + std::to_string(i));
        instances.push_back(std::move(c));
    }

    std::vector<std::thread> threads;
    std::atomic<bool> failed{false};

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < 200; ++i) {
                // 每个线程随机挑选实例使用
                int inst_idx = (t + i) % NUM_INSTANCES;
                std::string plain = "Payload from thread " + std::to_string(t) + " iter " + std::to_string(i);
                
                std::string cipher = instances[inst_idx]->encrypt(plain);
                std::string dec = instances[inst_idx]->decrypt(cipher);
                
                if (dec != plain) {
                    failed = true;
                    return;
                }
            }
        });
    }

    for (auto& t : threads) t.join();
    EXPECT_FALSE(failed.load()) << "多线程多实例混合使用发现上下文状态污染！";
}

TEST_F(CryptoLayerTest, StressTest_Reuse) {
    // 极高频率地创建、使用并销毁 CryptoLayer 对象
    // 这在大容量同步场景下很常见，旨在测试 thread_local 是否稳定
    for (int i = 0; i < 500; ++i) {
        CryptoLayer temp_crypto;
        temp_crypto.set_key("ephemeral_key_" + std::to_string(i));
        
        std::string plain = "Short-lived test data " + std::to_string(i);
        std::string cipher = temp_crypto.encrypt(plain);
        EXPECT_EQ(temp_crypto.decrypt(cipher), plain);
    }
}

TEST_F(CryptoLayerTest, Performance_ThreadLocalEfficiency) {
    // 对比测试：验证在多线程下，新架构是否真的能跑得更快（消除锁竞争）
    const int NUM_THREADS = 8;
    const int TOTAL_OPS = 2000;
    std::string plaintext(1024, 'A'); // 1KB 数据

    auto start = std::chrono::high_resolution_clock::now();
    
    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < TOTAL_OPS / NUM_THREADS; ++i) {
                std::string c = crypto.encrypt(plaintext);
                crypto.decrypt(c);
            }
        });
    }
    for (auto& t : threads) t.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    std::cout << "[Performance] Concurrent Multi-thread context speed: " << ms << "ms for " << TOTAL_OPS << " ops\n";
    
    // 优化后的代码在 8 核机器上应该远低于 1 秒
    EXPECT_LT(ms, 1000);
}

