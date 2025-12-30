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
    
    // 性能优化：使用线程局部缓存 (Thread Local Storage)
    // 理由：EVP_CIPHER_CTX 的创建和销毁非常耗时。
    // 在多线程环境下，使用一把全局锁会造成严重的性能瓶颈。
    // 通过 thread_local，每个线程拥有一套自用的加速上下文，彻底消除锁竞争。
    static EVP_CIPHER_CTX* get_thread_encrypt_ctx();
    static EVP_CIPHER_CTX* get_thread_decrypt_ctx();
};

}  // namespace VeritasSync