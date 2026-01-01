#include "VeritasSync/common/Hashing.h"

#include <openssl/sha.h>
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <vector>
#include <format>

#include "VeritasSync/common/EncodingUtils.h"
#include "VeritasSync/common/Logger.h"

namespace VeritasSync {

    // --- 辅助函数：将二进制哈希转换为十六进制字符串 ---
    static std::string HashToHexString(const unsigned char* hash, size_t length) {
        std::string res;
        res.reserve(length * 2); // 预留空间，避免频繁扩容
        for (size_t i = 0; i < length; ++i) {
            // {:02x} 表示 16 进制、2位宽、0填充
            res += std::format("{:02x}", hash[i]);
        }
        return res;
    }

    // --- 缓冲区版本的 SHA256 (用于验证和小数据) ---
    std::string Hashing::CalculateSHA256_Buffer(const char* data, size_t length) {
        // 注意：即使 length == 0，也应该计算有效的 SHA256
        // 空数据的 SHA256 是一个固定值：e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
        // 之前的 bug：返回 "" 会被 StateManager 误判为"哈希失败"，导致空文件无限重试

        SHA256_CTX sha256Context;
        if (!SHA256_Init(&sha256Context)) {     // 初始化上下文
            return "";
        }

        // 只有当有数据时才更新，但 length == 0 时跳过更新也是合法的
        if (data && length > 0) {
            if (!SHA256_Update(&sha256Context, data, length)) {     // 处理数据
                return "";
            }
        }

        unsigned char hash[SHA256_DIGEST_LENGTH];
        if (!SHA256_Final(hash, &sha256Context)) {     // 计算最终哈希
            return "";
        }

        return HashToHexString(hash, SHA256_DIGEST_LENGTH);
    }

    // --- 跨平台 MMIO 实现 (使用 Boost.Interprocess) ---
    std::string Hashing::CalculateSHA256_MMIO(const std::filesystem::path& filePath) {
        namespace bi = boost::interprocess;

        // 1. 检查文件是否存在且为常规文件
        std::error_code ec;
        if (!std::filesystem::exists(filePath, ec) || ec ||
            !std::filesystem::is_regular_file(filePath, ec) || ec) {
            return "";
        }

        // 获取文件大小
        auto file_size = std::filesystem::file_size(filePath, ec);
        if (ec) {
            return "";
        }

        // 对于空文件，返回空字符串的 SHA256
        if (file_size == 0) {
            return CalculateSHA256_Buffer("", 0);
        }

        // 对于小文件（< 64KB），直接使用标准实现避免 MMIO 开销
        constexpr size_t MMIO_THRESHOLD = 64 * 1024;
        if (file_size < MMIO_THRESHOLD) {
            return CalculateSHA256(filePath);
        }

        try {
            // 2. 创建文件映射 (Boost 会根据平台自动处理宽字符路径或 UTF-8)
            // 在 Windows 上，filePath.c_str() 返回 wchar_t*。
            // 在 Linux 上，filePath.c_str() 返回 char*。
            bi::file_mapping m_file(filePath.c_str(), bi::read_only);

            // 3. 初始化 SHA256 上下文
            SHA256_CTX sha256Context;
            if (!SHA256_Init(&sha256Context)) {
                return "";
            }

            // 4. 分块映射并计算哈希 (滑动窗口)
            // 使用 256MB 的视图窗口，平衡内存使用和性能
            constexpr uint64_t VIEW_SIZE = 256 * 1024 * 1024;
            uint64_t offset = 0;
            uint64_t remaining = file_size;

            while (remaining > 0) {
                size_t view_size = static_cast<size_t>((remaining > VIEW_SIZE) ? VIEW_SIZE : remaining);

                // 映射视图 (RAII 自动管理生命周期)
                bi::mapped_region region(m_file, bi::read_only, offset, view_size);

                // 更新哈希
                if (!SHA256_Update(&sha256Context, region.get_address(), region.get_size())) {
                    return "";
                }

                offset += view_size;
                remaining -= view_size;
            }

            // 5. 计算最终哈希
            unsigned char hash[SHA256_DIGEST_LENGTH];
            if (!SHA256_Final(hash, &sha256Context)) {
                return "";
            }

            return HashToHexString(hash, SHA256_DIGEST_LENGTH);

        } catch (const std::exception& e) {
            if (g_logger) {
                g_logger->debug("[Hashing] Boost MMIO 失败: {} (Error: {})", 
                              PathToUtf8(filePath), e.what());
            }
            // 回退到标准实现
            return CalculateSHA256(filePath);
        }
    }

    // --- 原始标准实现 (保留作为回退和小文件处理) ---
    std::string Hashing::CalculateSHA256(const std::filesystem::path& filePath) {
        // 使用 non-throwing (ec) 重载
        std::error_code ec;

        // 1. 检查文件是否存在且为常规文件
        if (!std::filesystem::exists(filePath, ec) || ec ||
            !std::filesystem::is_regular_file(filePath, ec) || ec) {
            return "";
        }

        // 2. 以二进制模式打开文件
        std::ifstream file(filePath, std::ios::binary);

        // 处理文件锁定的重试逻辑
        if (!file.is_open()) {
            std::string sys_err = GetLastSystemError();
            if (g_logger) {
                g_logger->warn("[Hashing] ⚠️ 文件被锁定, 250ms 后重试: {} | {}", PathToUtf8(filePath), sys_err);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            file.open(filePath, std::ios::binary);

            if (!file.is_open()) {
                std::string sys_err2 = GetLastSystemError();
                if (g_logger) {
                    g_logger->error("[Hashing] ❌ 无法打开文件(重试后失败): {} | {}", PathToUtf8(filePath), sys_err2);
                }
                return "";
            }
        }

        // 3. 初始化SHA256上下文
        SHA256_CTX sha256Context;
        if (!SHA256_Init(&sha256Context)) {
            return "";
        }

        // 4. 分块读取文件并更新哈希值
        // 使用 64KB 缓冲区，对于顺序读取是较优的块大小
        std::vector<char> buffer(64 * 1024);
        while (file.good()) {
            file.read(buffer.data(), buffer.size());
            std::streamsize bytesRead = file.gcount();
            if (bytesRead > 0) {
                if (!SHA256_Update(&sha256Context, buffer.data(), bytesRead)) {
                    return "";
                }
            }
        }

        // 5. 计算最终的哈希摘要
        unsigned char hash[SHA256_DIGEST_LENGTH];
        if (!SHA256_Final(hash, &sha256Context)) {
            return "";
        }

        return HashToHexString(hash, SHA256_DIGEST_LENGTH);
    }

}  // namespace VeritasSync
 