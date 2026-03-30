#include "VeritasSync/common/Hashing.h"

#include <openssl/evp.h>
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>

#include <chrono>
#include <fstream>
#include <format>
#include <thread>
#include <vector>

#include "VeritasSync/common/EncodingUtils.h"
#include "VeritasSync/common/Logger.h"

namespace VeritasSync {

    // E-1: 魔数统一为命名常量（默认值，可通过 set_read_buffer_size 覆盖）
    static constexpr size_t DEFAULT_HASH_READ_BUFFER_SIZE = 64 * 1024;  // 文件哈希读取缓冲区 64KB
    static size_t s_hash_read_buffer_size = DEFAULT_HASH_READ_BUFFER_SIZE;
    static constexpr int FILE_LOCK_RETRY_DELAY_MS = 250;        // 文件锁定重试延迟（毫秒）

    void Hashing::set_read_buffer_size(size_t bytes) {
        if (bytes > 0) {
            s_hash_read_buffer_size = bytes;
        }
    }

    // --- EVP SHA256 上下文 RAII 封装 ---
    // 统一管理 EVP_MD_CTX 的生命周期，消除三个函数中重复的 Init/Update/Final 流程
    class SHA256Context {
    public:
        SHA256Context() : ctx_(EVP_MD_CTX_new()) {
            if (ctx_) {
                ok_ = (EVP_DigestInit_ex(ctx_, EVP_sha256(), nullptr) == 1);
            }
        }

        ~SHA256Context() {
            if (ctx_) {
                EVP_MD_CTX_free(ctx_);
            }
        }

        // 禁止拷贝
        SHA256Context(const SHA256Context&) = delete;
        SHA256Context& operator=(const SHA256Context&) = delete;

        // 是否初始化成功
        [[nodiscard]] bool valid() const { return ctx_ && ok_; }

        // 喂入数据
        bool update(const void* data, size_t length) {
            if (!valid()) return false;
            ok_ = (EVP_DigestUpdate(ctx_, data, length) == 1);
            return ok_;
        }

        // 计算最终摘要，返回十六进制字符串；失败返回空串
        [[nodiscard]] std::string finalize() {
            if (!valid()) return "";
            unsigned char hash[EVP_MAX_MD_SIZE];
            unsigned int hash_len = 0;
            if (EVP_DigestFinal_ex(ctx_, hash, &hash_len) != 1) {
                return "";
            }
            // 转换为十六进制字符串
            std::string res;
            res.reserve(hash_len * 2);
            for (unsigned int i = 0; i < hash_len; ++i) {
                res += std::format("{:02x}", hash[i]);
            }
            return res;
        }

    private:
        EVP_MD_CTX* ctx_ = nullptr;
        bool ok_ = false;
    };

    // --- 缓冲区版本的 SHA256 (用于验证和小数据) ---
    std::string Hashing::CalculateSHA256_Buffer(const char* data, size_t length) {
        // 注意：即使 length == 0，也应该计算有效的 SHA256
        // 空数据的 SHA256 是一个固定值：e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
        // 之前的 bug：返回 "" 会被 StateManager 误判为"哈希失败"，导致空文件无限重试

        SHA256Context ctx;
        if (!ctx.valid()) return "";

        // 只有当有数据时才更新，但 length == 0 时跳过更新也是合法的
        if (data && length > 0) {
            if (!ctx.update(data, length)) return "";
        }

        return ctx.finalize();
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

            // 3. 初始化 SHA256 上下文 (EVP RAII)
            SHA256Context ctx;
            if (!ctx.valid()) return "";

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
                if (!ctx.update(region.get_address(), region.get_size())) {
                    return "";
                }

                offset += view_size;
                remaining -= view_size;
            }

            // 5. 计算最终哈希
            return ctx.finalize();

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

            std::this_thread::sleep_for(std::chrono::milliseconds(FILE_LOCK_RETRY_DELAY_MS));
            file.open(filePath, std::ios::binary);

            if (!file.is_open()) {
                std::string sys_err2 = GetLastSystemError();
                if (g_logger) {
                    g_logger->error("[Hashing] ❌ 无法打开文件(重试后失败): {} | {}", PathToUtf8(filePath), sys_err2);
                }
                return "";
            }
        }

        // 3. 初始化 SHA256 上下文 (EVP RAII)
        SHA256Context ctx;
        if (!ctx.valid()) return "";

        // 4. 分块读取文件并更新哈希值
        // 使用 64KB 缓冲区，对于顺序读取是较优的块大小
        std::vector<char> buffer(s_hash_read_buffer_size);
        while (file.good()) {
            file.read(buffer.data(), buffer.size());
            std::streamsize bytesRead = file.gcount();
            if (bytesRead > 0) {
                if (!ctx.update(buffer.data(), bytesRead)) {
                    return "";
                }
            }
        }

        // 5. 计算最终的哈希摘要
        return ctx.finalize();
    }

}  // namespace VeritasSync
 