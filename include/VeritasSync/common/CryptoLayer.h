#pragma once

#include <string>
#include <vector>

namespace VeritasSync {

class CryptoLayer {
public:
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
    std::string m_key;
};

}  // namespace VeritasSync