#pragma once

#include <string>
#include <vector>

namespace VeritasSync {

// AES-256-GCM 所需的常量
constexpr int AES_KEY_BITS = 256;
constexpr int AES_KEY_BYTES = AES_KEY_BITS / 8;
constexpr int GCM_IV_BYTES = 12;   // GCM 推荐 12 字节 IV
constexpr int GCM_TAG_BYTES = 16;  // GCM 认证标签

class Crypto {
 public:
  // 使用 PBKDF2 从用户密码派生一个 256 位的密钥
  static std::vector<unsigned char> derive_key(const std::string& password);

  // 使用 AES-256-GCM 加密
  // 返回值: [12 字节 IV] + [加密后的密文] + [16 字节 TAG]
  static std::string encrypt(const std::string& plaintext,
                             const std::vector<unsigned char>& key);

  // 使用 AES-256-GCM 解密
  // 输入: [12 字节 IV] + [加密后的密文] + [16 字节 TAG]
  // 如果解密/认证失败，将抛出 std::runtime_error
  static std::string decrypt(const std::string& ciphertext_package,
                             const std::vector<unsigned char>& key);
};

}  // namespace VeritasSync
