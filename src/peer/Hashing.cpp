    #include "VeritasSync/Hashing.h"

#include <openssl/sha.h>

#include <chrono>  // 用于 sleep
#include <fstream>
#include <iomanip>
#include <iostream>  // 用于日志
#include <sstream>
#include <thread>  // 用于 sleep
#include <vector>

#include "VeritasSync/Logger.h"

namespace VeritasSync {

    std::string Hashing::CalculateSHA256(const std::filesystem::path& filePath) {
        // --- 使用 non-throwing (ec) 重载 ---
        std::error_code ec;

        // 1. 检查文件是否存在且为常规文件
        if (!std::filesystem::exists(filePath, ec) || ec ||
            !std::filesystem::is_regular_file(filePath, ec) || ec) {
            // 这是预期的行为，例如当检查一个目录时
            return "";
        }

        // 2. 以二进制模式打开文件
        std::ifstream file(filePath, std::ios::binary);

        // --- 处理文件锁定的重试逻辑 ---
        if (!file.is_open()) {
            // 使用 logger 替换 std::cerr
            if (g_logger) {
                g_logger->warn("[Hashing] 无法立即打开文件 (可能被锁定): {}. 250ms 后重试...", filePath.string());
            } else {
                std::cerr << "[Hashing] 无法立即打开文件 (可能被锁定): "
                    << filePath.string() << ". 250ms 后重试..." << std::endl;
            }

            // 等待 250 毫秒
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            // 再次尝试打开
            file.open(filePath, std::ios::binary);

            if (!file.is_open()) {
                if (g_logger) {
                    g_logger->error("[Hashing] 无法打开文件 (重试后): {}", filePath.string());
                } else {
                    std::cerr << "[Hashing] 无法打开文件 (重试后): " << filePath.string() << std::endl;
                }
                return "";  // 放弃
            }
        }
        // ------------------------------------

        // 3. 初始化SHA256上下文
        SHA256_CTX sha256Context;
        if (!SHA256_Init(&sha256Context)) {
            return "";
        }

        // 4. 分块读取文件并更新哈希值
        std::vector<char> buffer(4096);  // 4KB的缓冲区
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

        // 6. 将二进制哈希转换为十六进制字符串
        std::stringstream ss;
        for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
            ss << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(hash[i]);
        }

        return ss.str();
    }

}  // namespace VeritasSync
