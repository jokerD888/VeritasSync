#include "VeritasSync/common/Hashing.h"

#include <openssl/sha.h>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <vector>

#include "VeritasSync/common/EncodingUtils.h"
#include "VeritasSync/common/Logger.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace VeritasSync {

    // --- 辅助函数：将二进制哈希转换为十六进制字符串 ---
    static std::string HashToHexString(const unsigned char* hash, size_t length) {
        std::stringstream ss;
        for (size_t i = 0; i < length; ++i) {
            ss << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<int>(hash[i]);
        }
        return ss.str();
    }

    // --- 缓冲区版本的 SHA256 (用于验证和小数据) ---
    std::string Hashing::CalculateSHA256_Buffer(const char* data, size_t length) {
        if (!data || length == 0) {
            return "";
        }

        SHA256_CTX sha256Context;
        if (!SHA256_Init(&sha256Context)) {
            return "";
        }

        if (!SHA256_Update(&sha256Context, data, length)) {
            return "";
        }

        unsigned char hash[SHA256_DIGEST_LENGTH];
        if (!SHA256_Final(hash, &sha256Context)) {
            return "";
        }

        return HashToHexString(hash, SHA256_DIGEST_LENGTH);
    }

#ifdef _WIN32
    // --- Windows MMIO 实现 ---
    std::string Hashing::CalculateSHA256_MMIO(const std::filesystem::path& filePath) {
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

        // 2. 打开文件 (使用宽字符路径支持 Unicode)
        HANDLE hFile = CreateFileW(
            filePath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,  // 允许其他进程读取
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,  // 顺序访问优化
            NULL
        );

        if (hFile == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            if (g_logger) {
                g_logger->debug("[Hashing] MMIO CreateFile 失败: {} (Error: {})", 
                              PathToUtf8(filePath), err);
            }
            // 回退到标准实现
            return CalculateSHA256(filePath);
        }

        // 3. 创建文件映射
        HANDLE hMapping = CreateFileMappingW(
            hFile,
            NULL,
            PAGE_READONLY,
            0, 0,  // 映射整个文件
            NULL
        );

        if (!hMapping) {
            DWORD err = GetLastError();
            CloseHandle(hFile);
            if (g_logger) {
                g_logger->debug("[Hashing] MMIO CreateFileMapping 失败: {} (Error: {})", 
                              PathToUtf8(filePath), err);
            }
            // 回退到标准实现
            return CalculateSHA256(filePath);
        }

        // 4. 初始化 SHA256 上下文
        SHA256_CTX sha256Context;
        if (!SHA256_Init(&sha256Context)) {
            CloseHandle(hMapping);
            CloseHandle(hFile);
            return "";
        }

        // 5. 分块映射并计算哈希
        // 使用 256MB 的视图窗口，平衡内存使用和性能
        constexpr size_t VIEW_SIZE = 256 * 1024 * 1024;
        size_t offset = 0;
        size_t remaining = static_cast<size_t>(file_size);

        bool success = true;
        while (remaining > 0) {
            size_t view_size = (remaining > VIEW_SIZE) ? VIEW_SIZE : remaining;

            // 计算高位和低位偏移
            DWORD offset_high = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFF);
            DWORD offset_low = static_cast<DWORD>(offset & 0xFFFFFFFF);

            // 映射视图
            void* pView = MapViewOfFile(
                hMapping,
                FILE_MAP_READ,
                offset_high,
                offset_low,
                view_size
            );

            if (!pView) {
                DWORD err = GetLastError();
                if (g_logger) {
                    g_logger->warn("[Hashing] MMIO MapViewOfFile 失败: {} offset={} (Error: {})", 
                                 PathToUtf8(filePath), offset, err);
                }
                success = false;
                break;
            }

            // 更新哈希
            if (!SHA256_Update(&sha256Context, pView, view_size)) {
                UnmapViewOfFile(pView);
                success = false;
                break;
            }

            // 取消映射
            UnmapViewOfFile(pView);

            offset += view_size;
            remaining -= view_size;
        }

        // 6. 清理资源
        CloseHandle(hMapping);
        CloseHandle(hFile);

        if (!success) {
            // 回退到标准实现
            return CalculateSHA256(filePath);
        }

        // 7. 计算最终哈希
        unsigned char hash[SHA256_DIGEST_LENGTH];
        if (!SHA256_Final(hash, &sha256Context)) {
            return "";
        }

        return HashToHexString(hash, SHA256_DIGEST_LENGTH);
    }

#else
    // --- 非 Windows 平台：直接使用标准实现 ---
    std::string Hashing::CalculateSHA256_MMIO(const std::filesystem::path& filePath) {
        return CalculateSHA256(filePath);
    }

#endif

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
