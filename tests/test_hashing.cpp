#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <vector>
#include <chrono>

#include "VeritasSync/common/Hashing.h"
#include "VeritasSync/common/Logger.h"

using namespace VeritasSync;

// 全局测试环境：初始化 Logger
class HashingTestEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        init_logger();
    }
};

static ::testing::Environment* const hashing_env =
    ::testing::AddGlobalTestEnvironment(new HashingTestEnvironment());

class HashingTest : public ::testing::Test {
protected:
    std::string temp_file = "test_hash.tmp";
    std::string large_file = "test_hash_large.tmp";
    std::string mmio_threshold_file = "test_hash_threshold.tmp";

    void SetUp() override {
        // 每个测试前创建一个已知内容的文件
        std::ofstream f(temp_file);
        f << "hello world";  // sha256: b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9
        f.close();
    }

    void TearDown() override {
        std::filesystem::remove(temp_file);
        std::filesystem::remove(large_file);
        std::filesystem::remove(mmio_threshold_file);
    }
    
    // 创建指定大小的随机文件（固定种子保证可重复性）
    void CreateRandomFile(const std::string& path, size_t size, uint32_t seed = 12345) {
        std::ofstream f(path, std::ios::binary);
        std::mt19937 rng(seed);
        std::uniform_int_distribution<int> dist(0, 255);
        
        std::vector<char> buffer(4096);
        size_t written = 0;
        while (written < size) {
            size_t to_write = std::min(buffer.size(), size - written);
            for (size_t i = 0; i < to_write; ++i) {
                buffer[i] = static_cast<char>(dist(rng));
            }
            f.write(buffer.data(), to_write);
            written += to_write;
        }
        f.close();
    }
    
    // 创建指定大小的零填充文件
    void CreateZeroFile(const std::string& path, size_t size) {
        std::ofstream f(path, std::ios::binary);
        std::vector<char> buffer(4096, 0);
        size_t written = 0;
        while (written < size) {
            size_t to_write = std::min(buffer.size(), size - written);
            f.write(buffer.data(), to_write);
            written += to_write;
        }
        f.close();
    }
};

// =====================================================
// 基础功能测试
// =====================================================

TEST_F(HashingTest, CalculateCorrectSHA256) {
    std::string hash = Hashing::CalculateSHA256(temp_file);
    // "hello world" 的 SHA256 (已知值)
    EXPECT_EQ(hash, "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
}

TEST_F(HashingTest, HandleMissingFile) {
    std::string hash = Hashing::CalculateSHA256("non_existent_file.xyz");
    EXPECT_EQ(hash, "");
}

TEST_F(HashingTest, HandleDirectory) {
    std::filesystem::create_directory("test_dir_temp");
    std::string hash = Hashing::CalculateSHA256("test_dir_temp");
    EXPECT_EQ(hash, "");  // 目录应返回空
    std::filesystem::remove("test_dir_temp");
}

// =====================================================
// MMIO 与标准实现一致性测试
// =====================================================

TEST_F(HashingTest, MMIO_MatchesStandard_SmallFile) {
    // 对于小文件 (< 64KB)，MMIO 应该自动回退到标准实现
    std::string hash_std = Hashing::CalculateSHA256(temp_file);
    std::string hash_mmio = Hashing::CalculateSHA256_MMIO(temp_file);
    
    EXPECT_EQ(hash_std, hash_mmio);
    EXPECT_EQ(hash_mmio, "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
}

TEST_F(HashingTest, MMIO_MatchesStandard_ThresholdFile) {
    // 测试正好在阈值边界的文件 (64KB)
    CreateRandomFile(mmio_threshold_file, 64 * 1024);
    
    std::string hash_std = Hashing::CalculateSHA256(mmio_threshold_file);
    std::string hash_mmio = Hashing::CalculateSHA256_MMIO(mmio_threshold_file);
    
    EXPECT_EQ(hash_std, hash_mmio);
    EXPECT_FALSE(hash_std.empty());
}

TEST_F(HashingTest, MMIO_MatchesStandard_AboveThreshold) {
    // 测试刚超过阈值的文件 (65KB，应该使用 MMIO)
    CreateRandomFile(mmio_threshold_file, 65 * 1024);
    
    std::string hash_std = Hashing::CalculateSHA256(mmio_threshold_file);
    std::string hash_mmio = Hashing::CalculateSHA256_MMIO(mmio_threshold_file);
    
    EXPECT_EQ(hash_std, hash_mmio);
    EXPECT_FALSE(hash_std.empty());
}

TEST_F(HashingTest, MMIO_MatchesStandard_MediumFile) {
    // 创建一个 1MB 的随机文件
    CreateRandomFile(large_file, 1024 * 1024);
    
    std::string hash_std = Hashing::CalculateSHA256(large_file);
    std::string hash_mmio = Hashing::CalculateSHA256_MMIO(large_file);
    
    // 两种方法必须产生相同的哈希值
    EXPECT_EQ(hash_std, hash_mmio);
    EXPECT_FALSE(hash_std.empty());
}

TEST_F(HashingTest, MMIO_MatchesStandard_LargeFile) {
    // 创建一个 10MB 的随机文件
    CreateRandomFile(large_file, 10 * 1024 * 1024);
    
    std::string hash_std = Hashing::CalculateSHA256(large_file);
    std::string hash_mmio = Hashing::CalculateSHA256_MMIO(large_file);
    
    EXPECT_EQ(hash_std, hash_mmio);
    EXPECT_FALSE(hash_std.empty());
}

// 注意：100MB 测试较慢，标记为可选
TEST_F(HashingTest, MMIO_MatchesStandard_VeryLargeFile) {
    // 创建一个 100MB 的随机文件以测试分块映射
    CreateRandomFile(large_file, 100 * 1024 * 1024);
    
    std::string hash_std = Hashing::CalculateSHA256(large_file);
    std::string hash_mmio = Hashing::CalculateSHA256_MMIO(large_file);
    
    EXPECT_EQ(hash_std, hash_mmio);
    EXPECT_FALSE(hash_std.empty());
}

TEST_F(HashingTest, MMIO_MatchesStandard_ZeroFile) {
    // 测试全零文件
    CreateZeroFile(large_file, 1024 * 1024);  // 1MB 全零
    
    std::string hash_std = Hashing::CalculateSHA256(large_file);
    std::string hash_mmio = Hashing::CalculateSHA256_MMIO(large_file);
    
    EXPECT_EQ(hash_std, hash_mmio);
    EXPECT_FALSE(hash_std.empty());
}

TEST_F(HashingTest, MMIO_MatchesStandard_DifferentContent) {
    // 使用不同种子创建多个文件，验证哈希都正确
    for (uint32_t seed = 1; seed <= 5; ++seed) {
        CreateRandomFile(large_file, 256 * 1024, seed);  // 256KB
        
        std::string hash_std = Hashing::CalculateSHA256(large_file);
        std::string hash_mmio = Hashing::CalculateSHA256_MMIO(large_file);
        
        EXPECT_EQ(hash_std, hash_mmio) << "Seed: " << seed;
    }
}

// =====================================================
// 缓冲区哈希测试
// =====================================================

TEST_F(HashingTest, BufferHash_MatchesFileHash) {
    // 验证缓冲区哈希与文件哈希一致
    std::string content = "hello world";
    std::string hash_file = Hashing::CalculateSHA256(temp_file);
    std::string hash_buffer = Hashing::CalculateSHA256_Buffer(content.data(), content.size());
    
    EXPECT_EQ(hash_file, hash_buffer);
}

TEST_F(HashingTest, BufferHash_EmptyData) {
    // 空数据应该返回空字符串
    std::string hash = Hashing::CalculateSHA256_Buffer("", 0);
    EXPECT_EQ(hash, "");
    
    hash = Hashing::CalculateSHA256_Buffer(nullptr, 0);
    EXPECT_EQ(hash, "");
}

TEST_F(HashingTest, BufferHash_KnownValues) {
    // 测试已知 SHA256 值
    // "abc" -> ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
    std::string hash = Hashing::CalculateSHA256_Buffer("abc", 3);
    EXPECT_EQ(hash, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_F(HashingTest, BufferHash_BinaryData) {
    // 测试二进制数据（包含 null 字节）
    std::vector<char> data(256);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<char>(i);
    }
    
    std::string hash = Hashing::CalculateSHA256_Buffer(data.data(), data.size());
    EXPECT_FALSE(hash.empty());
    EXPECT_EQ(hash.length(), 64);  // SHA256 = 32 bytes = 64 hex chars
}

// =====================================================
// 哈希格式测试
// =====================================================

TEST_F(HashingTest, HashFormat_CorrectLength) {
    std::string hash = Hashing::CalculateSHA256(temp_file);
    EXPECT_EQ(hash.length(), 64);  // SHA256 输出 64 个十六进制字符
}

TEST_F(HashingTest, HashFormat_AllLowercase) {
    std::string hash = Hashing::CalculateSHA256(temp_file);
    for (char c : hash) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) 
            << "Invalid character: " << c;
    }
}

TEST_F(HashingTest, HashFormat_Deterministic) {
    // 同一文件多次计算应得到相同结果
    std::string hash1 = Hashing::CalculateSHA256(temp_file);
    std::string hash2 = Hashing::CalculateSHA256(temp_file);
    std::string hash3 = Hashing::CalculateSHA256(temp_file);
    
    EXPECT_EQ(hash1, hash2);
    EXPECT_EQ(hash2, hash3);
}

TEST_F(HashingTest, HashFormat_MMIO_Deterministic) {
    CreateRandomFile(large_file, 1024 * 1024);
    
    std::string hash1 = Hashing::CalculateSHA256_MMIO(large_file);
    std::string hash2 = Hashing::CalculateSHA256_MMIO(large_file);
    std::string hash3 = Hashing::CalculateSHA256_MMIO(large_file);
    
    EXPECT_EQ(hash1, hash2);
    EXPECT_EQ(hash2, hash3);
}

// =====================================================
// 性能基准测试（非严格测试，仅供参考）
// =====================================================

TEST_F(HashingTest, Performance_MMIO_NotSlower) {
    // 创建一个 50MB 的文件
    CreateRandomFile(large_file, 50 * 1024 * 1024);
    
    // 预热
    Hashing::CalculateSHA256(large_file);
    Hashing::CalculateSHA256_MMIO(large_file);
    
    // 测量标准实现
    auto start_std = std::chrono::high_resolution_clock::now();
    std::string hash_std = Hashing::CalculateSHA256(large_file);
    auto end_std = std::chrono::high_resolution_clock::now();
    auto duration_std = std::chrono::duration_cast<std::chrono::milliseconds>(end_std - start_std).count();
    
    // 测量 MMIO 实现
    auto start_mmio = std::chrono::high_resolution_clock::now();
    std::string hash_mmio = Hashing::CalculateSHA256_MMIO(large_file);
    auto end_mmio = std::chrono::high_resolution_clock::now();
    auto duration_mmio = std::chrono::duration_cast<std::chrono::milliseconds>(end_mmio - start_mmio).count();
    
    // 验证正确性
    EXPECT_EQ(hash_std, hash_mmio);
    
    // 输出性能信息（不作为测试条件，因为性能受 I/O 影响大）
    std::cout << "[Performance] Standard: " << duration_std << "ms, MMIO: " << duration_mmio << "ms\n";
    
    // MMIO 不应该显著慢于标准实现 (允许 50% 波动)
    EXPECT_LT(duration_mmio, duration_std * 1.5 + 100);  // +100ms 容错
}

// =====================================================
// Unicode 路径测试
// =====================================================

TEST_F(HashingTest, UnicodePath_Chinese) {
    std::string unicode_file = "测试文件.tmp";
    std::ofstream f(unicode_file);
    f << "test content";
    f.close();
    
    std::string hash = Hashing::CalculateSHA256(unicode_file);
    EXPECT_FALSE(hash.empty());
    
    std::filesystem::remove(unicode_file);
}

TEST_F(HashingTest, UnicodePath_Mixed) {
    std::string unicode_file = "test_文件_αβγ.tmp";
    std::ofstream f(unicode_file);
    f << "test content";
    f.close();
    
    std::string hash_std = Hashing::CalculateSHA256(unicode_file);
    std::string hash_mmio = Hashing::CalculateSHA256_MMIO(unicode_file);
    
    EXPECT_FALSE(hash_std.empty());
    EXPECT_EQ(hash_std, hash_mmio);
    
    std::filesystem::remove(unicode_file);
}