#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Forward declarations for OpenSSL types
struct evp_cipher_ctx_st;
typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;

namespace VeritasSync {

class CryptoLayer {
public:
    CryptoLayer();
    ~CryptoLayer();
    
    // 禁止拷贝
    CryptoLayer(const CryptoLayer&) = delete;
    CryptoLayer& operator=(const CryptoLayer&) = delete;
    
    // 允许移动
    CryptoLayer(CryptoLayer&&) noexcept;
    CryptoLayer& operator=(CryptoLayer&&) noexcept;

    // 从字符串派生密钥 (SHA256)
    void set_key(const std::string& key_string);

    // AES-256-GCM 加密
    // 返回: IV + Ciphertext + Tag
    std::string encrypt(const std::string& plaintext) const;

    // AES-256-GCM 解密
    // 输入: IV + Ciphertext + Tag
    std::string decrypt(const std::string& ciphertext) const;

    bool has_key() const { return !m_key.empty(); }

private:
    // 获取或创建加密上下文（线程局部缓存）
    EVP_CIPHER_CTX* get_encrypt_ctx() const;
    EVP_CIPHER_CTX* get_decrypt_ctx() const;
    
    std::string m_key;
    
    // 缓存的加密/解密上下文（使用 mutable 允许在 const 方法中修改）
    // 注意：这些上下文在多线程环境中通过 thread_local 实现线程安全
    mutable std::unique_ptr<EVP_CIPHER_CTX, void(*)(EVP_CIPHER_CTX*)> m_encrypt_ctx;
    mutable std::unique_ptr<EVP_CIPHER_CTX, void(*)(EVP_CIPHER_CTX*)> m_decrypt_ctx;
    mutable std::mutex m_ctx_mutex;  // 保护上下文初始化
};

}  // namespace VeritasSync