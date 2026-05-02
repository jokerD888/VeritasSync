#include "VeritasSync/common/CryptoLayer.h"

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <openssl/crypto.h>

#include <cstring>
#include <memory>
#include <vector>

#include "VeritasSync/common/Logger.h"

namespace VeritasSync {


static constexpr size_t AES256_KEY_SIZE = 32;  // AES-256 密钥长度（字节）

// --- 构造/析构 ---
CryptoLayer::CryptoLayer() = default;

CryptoLayer::~CryptoLayer() {
    // 析构时清零密钥内存，防止残留在堆中被取证工具提取
    if (!m_key.empty()) {
        OPENSSL_cleanse(m_key.data(), m_key.size());
    }
}

CryptoLayer::CryptoLayer(CryptoLayer&& other) noexcept {
    std::unique_lock lock(other.m_key_mutex);
    m_key = std::move(other.m_key);
}

CryptoLayer& CryptoLayer::operator=(CryptoLayer&& other) noexcept {
    if (this != &other) {
        // 同时锁住双方，避免死锁：固定按地址顺序加锁
        std::unique_lock lock1(m_key_mutex, std::defer_lock);
        std::unique_lock lock2(other.m_key_mutex, std::defer_lock);
        std::lock(lock1, lock2);

        if (!m_key.empty()) {
            OPENSSL_cleanse(m_key.data(), m_key.size());
        }
        m_key = std::move(other.m_key);
    }
    return *this;
}

// --- 性能核心：线程局部缓存实现 ---
// 使用 thread_local 关键字，每个操作系统线程都会独立拥有一个上下文实例。
// 生命周期在线程退出时自动结束，彻底解决多线程并发下的“锁竞争”和“频繁分配内存”问题。
EVP_CIPHER_CTX* CryptoLayer::get_thread_encrypt_ctx() {
    // std::unique_ptr 的模板参数要求存储一个“删除器对象”。EVP_CIPHER_CTX_free 是一个函数（不是函数指针）
    // 如果类型推导为“函数类型”，unique_ptr 内部会尝试声明一个函数成员，这在 C++ 中是非法的（类成员只能是变量或指针）。
    // 所以，我们在模板参数里写 & 是为了辅助类型推导，确保 unique_ptr 知道它要存的是一个指针。
    // 而在构造函数参数里，编译器已经知道你要传的是指针，所以它会帮你自动完成转换
    thread_local std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> 
        t_encrypt_ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    
    if (!t_encrypt_ctx) [[unlikely]] {
        t_encrypt_ctx.reset(EVP_CIPHER_CTX_new());
    }
    
    return t_encrypt_ctx.get();
}

EVP_CIPHER_CTX* CryptoLayer::get_thread_decrypt_ctx() {
    thread_local std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> 
        t_decrypt_ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!t_decrypt_ctx) {
        t_decrypt_ctx.reset(EVP_CIPHER_CTX_new());
    }
    
    return t_decrypt_ctx.get();
}

void CryptoLayer::set_key(const std::string& key_string) {
    // 使用 HKDF-SHA256 从 sync_key 派生 AES-256 密钥
    // Salt: 固定的应用级盐值（防止跨应用碰撞）
    // Info: 绑定用途，防止密钥被误用于其他场景
    static const unsigned char salt[] = "VeritasSync-v1-salt";
    static const unsigned char info[] = "veritassync-aes256-gcm";

    unsigned char derived_key[AES256_KEY_SIZE]; // 256 bits

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
    if (!ctx) {
        g_logger->error("[Crypto] HKDF 上下文创建失败");
        return;
    }

    bool ok = (EVP_PKEY_derive_init(ctx) == 1);
    ok = ok && (EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) == 1);
    ok = ok && (EVP_PKEY_CTX_set1_hkdf_salt(ctx, salt, sizeof(salt) - 1) == 1);
    ok = ok && (EVP_PKEY_CTX_set1_hkdf_key(ctx,
            reinterpret_cast<const unsigned char*>(key_string.c_str()),
            static_cast<int>(key_string.length())) == 1);
    ok = ok && (EVP_PKEY_CTX_add1_hkdf_info(ctx, info, sizeof(info) - 1) == 1);

    size_t outlen = AES256_KEY_SIZE;
    ok = ok && (EVP_PKEY_derive(ctx, derived_key, &outlen) == 1);
    EVP_PKEY_CTX_free(ctx);

    if (!ok) {
        g_logger->error("[Crypto] HKDF 密钥派生失败，保留旧密钥");
        return;
    }

    {
        // 写锁保护 m_key 更新
        std::unique_lock lock(m_key_mutex);
        m_key.assign(reinterpret_cast<const char*>(derived_key), AES256_KEY_SIZE);
    }
    OPENSSL_cleanse(derived_key, sizeof(derived_key));  // 清零临时密钥

    // 注意：由于使用了 thread_local，这里的密钥更改是全局生效的，
    // 但上下文状态会在下次 EncryptInit 时由新的密钥覆盖，无需手动重置上下文。
    g_logger->info("[Crypto] 加密密钥已更新 (HKDF-SHA256 derived).");
}

bool CryptoLayer::has_key() const {
    std::shared_lock lock(m_key_mutex);
    return !m_key.empty();
}

std::string CryptoLayer::encrypt(const std::string& plaintext) const {
    // 读锁保护 m_key
    std::shared_lock lock(m_key_mutex);
    if (m_key.empty()) {
        g_logger->error("[Crypto] 加密失败：密钥未设置。");
        return "";
    }

    // 1. 生成高强度随机 IV
    unsigned char iv[GCM_IV_LEN];
    if (RAND_bytes(iv, sizeof(iv)) != 1) {
        g_logger->error("[Crypto] 加密失败：无法生成 IV。");
        return "";
    }

    // 2. 获取线程私有的 CTX (带自愈能力且锁无关)
    EVP_CIPHER_CTX* ctx = get_thread_encrypt_ctx();
    if (!ctx) [[unlikely]] return "";

    // 3. 执行加密初始化 (设置算法，设置IV长度，设置密钥，互不干扰，可以整合检查，使用 [[unlikely]] 优化常用分支性能)
    bool ok = (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1);
    ok &= (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, GCM_IV_LEN, NULL) == 1);
    ok &= (EVP_EncryptInit_ex(ctx, NULL, NULL, 
                               reinterpret_cast<const unsigned char*>(m_key.c_str()), iv) == 1);
    
    if (!ok) [[unlikely]] {
        g_logger->error("[Crypto] 加密引擎初始化失败 (OpenSSL 内部错误)。");
        return "";
    }

    // 4. 获取/准备线程局部的加密刮刮纸 (减少内存申请与清零开销)
    thread_local std::vector<unsigned char> t_encrypt_buf;
    size_t needed_len = plaintext.length() + EVP_MAX_BLOCK_LENGTH;
    if (t_encrypt_buf.size() < needed_len) [[unlikely]] {
        t_encrypt_buf.resize(needed_len);
    }

    // 5. 加密数据
    int out_len;
    if (EVP_EncryptUpdate(ctx, t_encrypt_buf.data(), &out_len, 
                          reinterpret_cast<const unsigned char*>(plaintext.c_str()),
                          static_cast<int>(plaintext.length())) != 1)[[unlikely]] return "";
    int ciphertext_len = out_len;

    if (EVP_EncryptFinal_ex(ctx, t_encrypt_buf.data() + out_len, &out_len) != 1)[[unlikely]] return "";
    ciphertext_len += out_len;

    // 6. 获取 GCM Tag (认证签名)
    unsigned char tag[GCM_TAG_LEN];
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, GCM_TAG_LEN, tag) != 1)[[unlikely]] return "";

    // 7. 拼接最终 Payload
    std::string result;
    result.reserve(GCM_IV_LEN + ciphertext_len + GCM_TAG_LEN);
    result.append(reinterpret_cast<const char*>(iv), GCM_IV_LEN);
    result.append(reinterpret_cast<const char*>(t_encrypt_buf.data()), ciphertext_len);
    result.append(reinterpret_cast<const char*>(tag), GCM_TAG_LEN);

    // 清理线程局部 buffer 中的明文残留
    OPENSSL_cleanse(t_encrypt_buf.data(), t_encrypt_buf.size());
    return result;
}

std::string CryptoLayer::decrypt(const std::string& ciphertext) const {
    // 读锁保护 m_key
    std::shared_lock lock(m_key_mutex);
    if (m_key.empty() || ciphertext.length() < GCM_IV_LEN + GCM_TAG_LEN) return "";

    // 1. 拆解数据包 (IV | Ciphertext | Tag)
    const unsigned char* iv = reinterpret_cast<const unsigned char*>(ciphertext.data());
    const unsigned char* tag_ptr = reinterpret_cast<const unsigned char*>(
        ciphertext.data() + ciphertext.length() - GCM_TAG_LEN);
    const unsigned char* encrypted_data = reinterpret_cast<const unsigned char*>(
        ciphertext.data() + GCM_IV_LEN);
    int encrypted_data_len = static_cast<int>(ciphertext.length() - GCM_IV_LEN - GCM_TAG_LEN);

    // 2. 获取线程私有 CTX
    EVP_CIPHER_CTX* ctx = get_thread_decrypt_ctx();
    if (!ctx) [[unlikely]] return "";

    // 3. 执行解密初始化
    bool ok = (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1);
    ok &= (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, GCM_IV_LEN, NULL) == 1);
    ok &= (EVP_DecryptInit_ex(ctx, NULL, NULL, 
                               reinterpret_cast<const unsigned char*>(m_key.c_str()), iv) == 1);
    
    if (!ok) [[unlikely]] {
        g_logger->error("[Crypto] 解密引擎初始化失败 (OpenSSL 内部错误)。");
        return "";
    }

    // 4. 准备线程局部的解密刮刮纸
    thread_local std::vector<unsigned char> t_decrypt_buf;
    size_t needed_len = encrypted_data_len + EVP_MAX_BLOCK_LENGTH;
    if (t_decrypt_buf.size() < needed_len) [[unlikely]] {
        t_decrypt_buf.resize(needed_len);
    }

    // 5. 解密数据
    int out_len;
    if (EVP_DecryptUpdate(ctx, t_decrypt_buf.data(), &out_len, encrypted_data, encrypted_data_len) != 1)[[unlikely]] return "";
    int plaintext_len = out_len;

    // 6. 设置预期的验证标签 (Tag)
    // const_cast: OpenSSL EVP_CIPHER_CTX_ctrl 的 tag 参数声明为 void* 而非 const void*，
    // 这是 OpenSSL API 的历史问题，实际不会写入该参数
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, GCM_TAG_LEN, const_cast<unsigned char*>(tag_ptr)) != 1)[[unlikely]] return "";

    // 7. 验证签名并完成解密 (这一步极其关键，防止数据篡改)
    if (EVP_DecryptFinal_ex(ctx, t_decrypt_buf.data() + out_len, &out_len) > 0) {
        plaintext_len += out_len;
        std::string result(reinterpret_cast<const char*>(t_decrypt_buf.data()), plaintext_len);
        // 清理线程局部 buffer 中的明文残留
        OPENSSL_cleanse(t_decrypt_buf.data(), t_decrypt_buf.size());
        return result;
    } else [[unlikely]]{
        OPENSSL_cleanse(t_decrypt_buf.data(), t_decrypt_buf.size());
        g_logger->warn("[Crypto] ❌ 解密失败：认证标签不匹配。数据可能已被篡改！");
        return "";
    }
}

}  // namespace VeritasSync