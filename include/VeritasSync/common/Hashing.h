#pragma once  

#include <filesystem>  
#include <string>  

namespace VeritasSync {

    class Hashing {
    public:
        // 计算文件的SHA-256哈希值
        // 输入: 文件路径
        // 输出: 64个字符的十六进制哈希字符串，如果失败则返回空字符串
        static std::string CalculateSHA256(const std::filesystem::path& filePath);

        // 使用 MMIO (Memory-Mapped I/O) 计算 SHA256
        // 对于大文件性能更优，小文件或无法映射时自动回退到标准实现
        static std::string CalculateSHA256_MMIO(const std::filesystem::path& filePath);

        // 计算内存缓冲区的 SHA256 (用于验证)
        static std::string CalculateSHA256_Buffer(const char* data, size_t length);

        /// 设置文件哈希读取缓冲区大小（从 Config::Advanced::hash_read_buffer_size_kb 注入）
        static void set_read_buffer_size(size_t bytes);
    };

} // namespace VeritasSync
