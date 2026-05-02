#pragma once

#include <shared_mutex>
#include <string>

// Forward declarations for OpenSSL types
struct evp_cipher_ctx_st;
typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;

namespace VeritasSync {

inline constexpr int GCM_IV_LEN  = 12;  // AES-GCM IV 长度（字节）
inline constexpr int GCM_TAG_LEN = 16;  // AES-GCM Tag 长度（字节）

// AES-256-GCM 保证了单条消息的完整性和机密性

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

    // 从字符串派生密钥 (HKDF-SHA256)
    void set_key(const std::string& key_string);

    // AES-256-GCM 加密
    // 返回: IV + Ciphertext + Tag
    std::string encrypt(const std::string& plaintext) const;

    // AES-256-GCM 解密
    // 输入: IV + Ciphertext + Tag
    std::string decrypt(const std::string& ciphertext) const;

    bool has_key() const;

private:
    std::string m_key;
    // 保护 m_key 并发读写（set_key 写 + encrypt/decrypt 读）
    mutable std::shared_mutex m_key_mutex;
    
    // 性能优化：使用线程局部缓存 (Thread Local Storage)
    // 理由：EVP_CIPHER_CTX 的创建和销毁非常耗时。
    // 在多线程环境下，使用一把全局锁会造成严重的性能瓶颈。
    // 通过 thread_local，每个线程拥有一套自用的加速上下文，彻底消除锁竞争。
    static EVP_CIPHER_CTX* get_thread_encrypt_ctx();
    static EVP_CIPHER_CTX* get_thread_decrypt_ctx();
};

}  // namespace VeritasSync