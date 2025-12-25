#include "VeritasSync/common/CryptoLayer.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <cstring>
#include <memory>
#include <vector>

#include "VeritasSync/common/Logger.h"

namespace VeritasSync {

static const int GCM_IV_LEN = 12;
static const int GCM_TAG_LEN = 16;

// --- RAII 删除器 ---
static void evp_ctx_deleter(EVP_CIPHER_CTX* ctx) {
    if (ctx) {
        EVP_CIPHER_CTX_free(ctx);
    }
}

// --- 构造/析构 ---
CryptoLayer::CryptoLayer() 
    : m_encrypt_ctx(nullptr, evp_ctx_deleter)
    , m_decrypt_ctx(nullptr, evp_ctx_deleter) {
}

CryptoLayer::~CryptoLayer() = default;

CryptoLayer::CryptoLayer(CryptoLayer&& other) noexcept
    : m_key(std::move(other.m_key))
    , m_encrypt_ctx(std::move(other.m_encrypt_ctx))
    , m_decrypt_ctx(std::move(other.m_decrypt_ctx)) {
}

CryptoLayer& CryptoLayer::operator=(CryptoLayer&& other) noexcept {
    if (this != &other) {
        m_key = std::move(other.m_key);
        m_encrypt_ctx = std::move(other.m_encrypt_ctx);
        m_decrypt_ctx = std::move(other.m_decrypt_ctx);
    }
    return *this;
}

// --- 上下文缓存获取 ---
EVP_CIPHER_CTX* CryptoLayer::get_encrypt_ctx() const {
    std::lock_guard<std::mutex> lock(m_ctx_mutex);
    if (!m_encrypt_ctx) {
        m_encrypt_ctx.reset(EVP_CIPHER_CTX_new());
        if (m_encrypt_ctx) {
            // 预初始化加密算法（不设置 key 和 iv，只设置算法）
            EVP_EncryptInit_ex(m_encrypt_ctx.get(), EVP_aes_256_gcm(), NULL, NULL, NULL);
            EVP_CIPHER_CTX_ctrl(m_encrypt_ctx.get(), EVP_CTRL_GCM_SET_IVLEN, GCM_IV_LEN, NULL);
        }
    }
    return m_encrypt_ctx.get();
}

EVP_CIPHER_CTX* CryptoLayer::get_decrypt_ctx() const {
    std::lock_guard<std::mutex> lock(m_ctx_mutex);
    if (!m_decrypt_ctx) {
        m_decrypt_ctx.reset(EVP_CIPHER_CTX_new());
        if (m_decrypt_ctx) {
            // 预初始化解密算法
            EVP_DecryptInit_ex(m_decrypt_ctx.get(), EVP_aes_256_gcm(), NULL, NULL, NULL);
            EVP_CIPHER_CTX_ctrl(m_decrypt_ctx.get(), EVP_CTRL_GCM_SET_IVLEN, GCM_IV_LEN, NULL);
        }
    }
    return m_decrypt_ctx.get();
}

void CryptoLayer::set_key(const std::string& key_string) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, key_string.c_str(), key_string.length());
    SHA256_Final(hash, &sha256);
    m_key.assign(reinterpret_cast<const char*>(hash), SHA256_DIGEST_LENGTH);
    
    // 重置缓存的上下文，下次使用时重新初始化
    {
        std::lock_guard<std::mutex> lock(m_ctx_mutex);
        m_encrypt_ctx.reset();
        m_decrypt_ctx.reset();
    }
    
    g_logger->info("[Crypto] 加密密钥已设置 (SHA256 derived).");
}

std::string CryptoLayer::encrypt(const std::string& plaintext) const {
    if (m_key.empty()) {
        g_logger->error("[Crypto] 加密失败：密钥未设置。");
        return "";
    }

    // 生成随机 IV
    unsigned char iv[GCM_IV_LEN];
    if (RAND_bytes(iv, sizeof(iv)) != 1) {
        g_logger->error("[Crypto] 加密失败：无法生成 IV。");
        return "";
    }

    // 使用临时上下文以保证线程安全
    // 注意：虽然有缓存机制，但 GCM 模式每次加密都需要新的 IV，
    // 所以我们需要重新初始化。优化点在于避免重复分配 CTX 结构本身。
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(
        EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    
    if (!ctx) {
        g_logger->error("[Crypto] 加密失败：无法创建上下文。");
        return "";
    }

    // 初始化加密操作
    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) {
        return "";
    }
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, GCM_IV_LEN, NULL) != 1) {
        return "";
    }
    if (EVP_EncryptInit_ex(ctx.get(), NULL, NULL, 
                           reinterpret_cast<const unsigned char*>(m_key.c_str()), iv) != 1) {
        return "";
    }

    int out_len;
    // 分配足够的缓冲区：输入长度 + 块大小 (AES block size = 16)
    std::vector<unsigned char> ciphertext(plaintext.length() + EVP_MAX_BLOCK_LENGTH);

    // 加密数据
    if (EVP_EncryptUpdate(ctx.get(), ciphertext.data(), &out_len, 
                          reinterpret_cast<const unsigned char*>(plaintext.c_str()),
                          static_cast<int>(plaintext.length())) != 1) {
        return "";
    }
    int ciphertext_len = out_len;

    // 结束加密
    if (EVP_EncryptFinal_ex(ctx.get(), ciphertext.data() + out_len, &out_len) != 1) {
        return "";
    }
    ciphertext_len += out_len;

    // 获取 Tag
    unsigned char tag[GCM_TAG_LEN];
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, GCM_TAG_LEN, tag) != 1) {
        return "";
    }

    // 拼接: IV + Ciphertext + Tag
    std::string final_payload;
    final_payload.reserve(GCM_IV_LEN + ciphertext_len + GCM_TAG_LEN);
    final_payload.append(reinterpret_cast<const char*>(iv), GCM_IV_LEN);
    final_payload.append(reinterpret_cast<const char*>(ciphertext.data()), ciphertext_len);
    final_payload.append(reinterpret_cast<const char*>(tag), GCM_TAG_LEN);

    return final_payload;
}

std::string CryptoLayer::decrypt(const std::string& ciphertext) const {
    if (m_key.empty()) {
        g_logger->error("[Crypto] 解密失败：密钥未设置。");
        return "";
    }

    if (ciphertext.length() < GCM_IV_LEN + GCM_TAG_LEN) {
        g_logger->warn("[Crypto] 解密失败：数据包过短 ({} bytes)。", ciphertext.length());
        return "";
    }

    // 拆解包结构
    const unsigned char* iv = reinterpret_cast<const unsigned char*>(ciphertext.c_str());
    const unsigned char* tag =
        reinterpret_cast<const unsigned char*>(ciphertext.c_str() + ciphertext.length() - GCM_TAG_LEN);
    const unsigned char* encrypted_data = reinterpret_cast<const unsigned char*>(ciphertext.c_str() + GCM_IV_LEN);

    size_t overhead = GCM_IV_LEN + GCM_TAG_LEN;
    if (ciphertext.length() <= overhead) return "";
    int encrypted_data_len = static_cast<int>(ciphertext.length() - overhead);

    // 使用独立上下文以保证线程安全
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(
        EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    
    if (!ctx) {
        g_logger->error("[Crypto] 解密失败：无法创建上下文。");
        return "";
    }

    // 初始化解密操作
    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) return "";
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, GCM_IV_LEN, NULL) != 1) return "";
    if (EVP_DecryptInit_ex(ctx.get(), NULL, NULL, 
                           reinterpret_cast<const unsigned char*>(m_key.c_str()), iv) != 1)
        return "";

    int out_len;
    // 额外增加缓冲区，防止 OpenSSL 写入越界
    std::vector<unsigned char> plaintext(encrypted_data_len + EVP_MAX_BLOCK_LENGTH + 32, 0);

    // 解密数据
    if (EVP_DecryptUpdate(ctx.get(), plaintext.data(), &out_len, encrypted_data, encrypted_data_len) != 1) {
        g_logger->warn("[Crypto] 解密过程出错。");
        return "";
    }
    int plaintext_len = out_len;

    // 设置期望的 Tag
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, GCM_TAG_LEN, 
                            const_cast<unsigned char*>(tag)) != 1) {
        return "";
    }

    // 验证 Tag 并结束解密
    int ret = EVP_DecryptFinal_ex(ctx.get(), plaintext.data() + out_len, &out_len);

    if (ret > 0) {
        plaintext_len += out_len;
        return std::string(reinterpret_cast<const char*>(plaintext.data()), plaintext_len);
    } else {
        g_logger->warn("[Crypto] 解密失败：认证标签不匹配 (数据可能被篡改或密钥错误)。");
        return "";
    }
}

}  // namespace VeritasSync