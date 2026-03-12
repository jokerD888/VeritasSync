// tests/test_parallel_scan.cpp
// 测试 StateManager 并行扫描功能的正确性

#include "test_helpers.h"
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "VeritasSync/common/Hashing.h"

using namespace VeritasSync;

REGISTER_VERITAS_TEST_ENV();

class ParallelHashingTest : public ::testing::Test {
protected:
    std::filesystem::path test_dir = "test_parallel_scan";
    
    void SetUp() override {
        // 创建测试目录
        std::filesystem::create_directories(test_dir);
    }
    
    void TearDown() override {
        // 清理测试目录
        std::error_code ec;
        std::filesystem::remove_all(test_dir, ec);
    }
    
    // 创建指定大小的随机文件（固定种子保证可重复性）
    void CreateRandomFile(const std::filesystem::path& path, size_t size, uint32_t seed) {
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
    
    // 创建多个测试文件
    void CreateTestFiles(int count, size_t size_each) {
        for (int i = 0; i < count; ++i) {
            auto file_path = test_dir / ("file_" + std::to_string(i) + ".bin");
            CreateRandomFile(file_path, size_each, static_cast<uint32_t>(i + 1));
        }
    }
};

// =====================================================
// 并行哈希正确性测试
// =====================================================

TEST_F(ParallelHashingTest, ParallelHash_MatchesSerial) {
    // 创建多个测试文件
    const int NUM_FILES = 20;
    const size_t FILE_SIZE = 256 * 1024;  // 256KB each
    
    CreateTestFiles(NUM_FILES, FILE_SIZE);
    
    // 收集所有文件路径
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(test_dir)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }
    
    ASSERT_EQ(files.size(), NUM_FILES);
    
    // 串行计算所有哈希
    std::vector<std::string> serial_hashes;
    for (const auto& file : files) {
        serial_hashes.push_back(Hashing::CalculateSHA256(file));
    }
    
    // 并行计算所有哈希 (使用 MMIO)
    std::vector<std::string> parallel_hashes(files.size());
    std::vector<std::thread> threads;
    
    for (size_t i = 0; i < files.size(); ++i) {
        threads.emplace_back([&, i]() {
            parallel_hashes[i] = Hashing::CalculateSHA256_MMIO(files[i]);
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    // 验证所有哈希一致
    for (size_t i = 0; i < files.size(); ++i) {
        EXPECT_EQ(serial_hashes[i], parallel_hashes[i]) 
            << "Mismatch at file " << files[i];
        EXPECT_FALSE(serial_hashes[i].empty());
    }
}

TEST_F(ParallelHashingTest, ConcurrentAccess_SameFile) {
    // 测试多线程同时访问同一文件
    auto file_path = test_dir / "shared_file.bin";
    CreateRandomFile(file_path, 1024 * 1024, 42);  // 1MB
    
    std::string reference_hash = Hashing::CalculateSHA256(file_path);
    ASSERT_FALSE(reference_hash.empty());
    
    const int NUM_THREADS = 10;
    std::vector<std::string> hashes(NUM_THREADS);
    std::vector<std::thread> threads;
    
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([&, i]() {
            // 每个线程读取相同的文件
            hashes[i] = Hashing::CalculateSHA256_MMIO(file_path);
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    // 所有线程应该得到相同的哈希
    for (int i = 0; i < NUM_THREADS; ++i) {
        EXPECT_EQ(hashes[i], reference_hash) << "Thread " << i << " got different hash";
    }
}

TEST_F(ParallelHashingTest, MixedFileSizes) {
    // 测试混合大小的文件
    std::vector<std::pair<std::string, size_t>> file_specs = {
        {"tiny.bin", 100},                    // 100 bytes
        {"small.bin", 32 * 1024},             // 32KB
        {"threshold.bin", 64 * 1024},         // 64KB (MMIO 阈值)
        {"medium.bin", 512 * 1024},           // 512KB
        {"large.bin", 2 * 1024 * 1024},       // 2MB
    };
    
    uint32_t seed = 1;
    for (const auto& [name, size] : file_specs) {
        auto file_path = test_dir / name;
        CreateRandomFile(file_path, size, seed++);
    }
    
    // 收集所有文件
    std::vector<std::filesystem::path> files;
    for (const auto& [name, _] : file_specs) {
        files.push_back(test_dir / name);
    }
    
    // 并行计算
    std::vector<std::string> mmio_hashes(files.size());
    std::vector<std::thread> threads;
    
    for (size_t i = 0; i < files.size(); ++i) {
        threads.emplace_back([&, i]() {
            mmio_hashes[i] = Hashing::CalculateSHA256_MMIO(files[i]);
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    // 验证与串行标准实现一致
    for (size_t i = 0; i < files.size(); ++i) {
        std::string std_hash = Hashing::CalculateSHA256(files[i]);
        EXPECT_EQ(mmio_hashes[i], std_hash) << "Mismatch at " << files[i];
        EXPECT_FALSE(mmio_hashes[i].empty());
    }
}

// =====================================================
// 性能对比测试
// =====================================================

TEST_F(ParallelHashingTest, Performance_ParallelVsSerial) {
    // 创建多个中等大小的文件
    const int NUM_FILES = 16;
    const size_t FILE_SIZE = 1 * 1024 * 1024;  // 1MB each
    
    CreateTestFiles(NUM_FILES, FILE_SIZE);
    
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(test_dir)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }
    
    // 预热缓存
    for (const auto& file : files) {
        Hashing::CalculateSHA256_MMIO(file);
    }
    
    // 测量串行时间
    auto start_serial = std::chrono::high_resolution_clock::now();
    std::vector<std::string> serial_hashes;
    for (const auto& file : files) {
        serial_hashes.push_back(Hashing::CalculateSHA256_MMIO(file));
    }
    auto end_serial = std::chrono::high_resolution_clock::now();
    auto duration_serial = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_serial - start_serial).count();
    
    // 测量并行时间
    auto start_parallel = std::chrono::high_resolution_clock::now();
    std::vector<std::string> parallel_hashes(files.size());
    std::vector<std::thread> threads;
    
    for (size_t i = 0; i < files.size(); ++i) {
        threads.emplace_back([&, i]() {
            parallel_hashes[i] = Hashing::CalculateSHA256_MMIO(files[i]);
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    auto end_parallel = std::chrono::high_resolution_clock::now();
    auto duration_parallel = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_parallel - start_parallel).count();
    
    // 输出性能信息
    std::cout << "[Performance] " << NUM_FILES << " files (" << FILE_SIZE/1024 << "KB each):\n";
    std::cout << "  Serial:   " << duration_serial << "ms\n";
    std::cout << "  Parallel: " << duration_parallel << "ms\n";
    std::cout << "  Speedup:  " << (double)duration_serial / duration_parallel << "x\n";
    
    // 验证正确性
    for (size_t i = 0; i < files.size(); ++i) {
        EXPECT_EQ(serial_hashes[i], parallel_hashes[i]);
    }
    
    // 并行版本应该不慢于串行版本（考虑到 I/O 瓶颈，不强制要求加速）
    // 在多核 CPU 上，理想情况下应该能看到加速
}

// =====================================================
// 边界条件测试
// =====================================================

TEST_F(ParallelHashingTest, EmptyDirectory) {
    // 空目录应该正常处理
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(test_dir)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }
    
    EXPECT_EQ(files.size(), 0);
}

TEST_F(ParallelHashingTest, SingleFile) {
    // 单个文件应该正常处理
    auto file_path = test_dir / "single.bin";
    CreateRandomFile(file_path, 100 * 1024, 1);
    
    std::string hash = Hashing::CalculateSHA256_MMIO(file_path);
    EXPECT_FALSE(hash.empty());
    
    std::string verify = Hashing::CalculateSHA256(file_path);
    EXPECT_EQ(hash, verify);
}

TEST_F(ParallelHashingTest, ManySmallFiles) {
    // 大量小文件
    const int NUM_FILES = 100;
    const size_t FILE_SIZE = 1024;  // 1KB each
    
    CreateTestFiles(NUM_FILES, FILE_SIZE);
    
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(test_dir)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }
    
    ASSERT_EQ(files.size(), NUM_FILES);
    
    // 并行计算
    std::vector<std::string> hashes(files.size());
    std::vector<std::thread> threads;
    
    for (size_t i = 0; i < files.size(); ++i) {
        threads.emplace_back([&, i]() {
            hashes[i] = Hashing::CalculateSHA256_MMIO(files[i]);
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    // 验证所有哈希不为空
    for (size_t i = 0; i < files.size(); ++i) {
        EXPECT_FALSE(hashes[i].empty()) << "Empty hash for file " << i;
    }
    
    // 不同文件应该有不同的哈希
    std::set<std::string> unique_hashes(hashes.begin(), hashes.end());
    EXPECT_EQ(unique_hashes.size(), hashes.size()) << "Some files have duplicate hashes";
}

// =====================================================
// 数据完整性测试
// =====================================================

TEST_F(ParallelHashingTest, DataIntegrity_ModifiedFile) {
    // 修改文件后哈希应该改变
    auto file_path = test_dir / "integrity.bin";
    CreateRandomFile(file_path, 100 * 1024, 1);
    
    std::string hash_before = Hashing::CalculateSHA256_MMIO(file_path);
    
    // 修改文件的一个字节
    {
        std::fstream f(file_path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(50000);
        char byte = 'X';
        f.write(&byte, 1);
        f.close();
    }
    
    std::string hash_after = Hashing::CalculateSHA256_MMIO(file_path);
    
    EXPECT_NE(hash_before, hash_after);
}

TEST_F(ParallelHashingTest, DataIntegrity_IdenticalFiles) {
    // 相同内容的文件应该有相同的哈希
    auto file1 = test_dir / "copy1.bin";
    auto file2 = test_dir / "copy2.bin";
    
    // 使用相同的种子创建相同内容
    CreateRandomFile(file1, 100 * 1024, 42);
    CreateRandomFile(file2, 100 * 1024, 42);
    
    std::string hash1 = Hashing::CalculateSHA256_MMIO(file1);
    std::string hash2 = Hashing::CalculateSHA256_MMIO(file2);
    
    EXPECT_EQ(hash1, hash2);
}

TEST_F(ParallelHashingTest, DataIntegrity_DifferentFiles) {
    // 不同内容的文件应该有不同的哈希
    auto file1 = test_dir / "diff1.bin";
    auto file2 = test_dir / "diff2.bin";
    
    // 使用不同的种子
    CreateRandomFile(file1, 100 * 1024, 1);
    CreateRandomFile(file2, 100 * 1024, 2);
    
    std::string hash1 = Hashing::CalculateSHA256_MMIO(file1);
    std::string hash2 = Hashing::CalculateSHA256_MMIO(file2);
    
    EXPECT_NE(hash1, hash2);
}
