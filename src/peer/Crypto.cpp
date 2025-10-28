#include "VeritasSync/Crypto.h"

#include <openssl/evp.h>
#include <openssl/kdf.h>   // 用于 PBKDF2
#include <openssl/rand.h>  // 用于 RAND_bytes

#include <iostream>
#include <stdexcept>

namespace VeritasSync {

// 使用 PBKDF2-HMAC-SHA256 从密码派生密钥
std::vector<unsigned char> Crypto::derive_key(const std::string& password) {
  std::vector<unsigned char> key(AES_KEY_BYTES);

  // 使用一个静态的 salt。
  // 在真实应用中，salt 应该是随机生成并与密文一起存储的，
  // 但在我们的 P2P 模型中，双方只需要能从同一个密码派生出同一个密钥即可。
  const unsigned char* salt = (const unsigned char*)"veritas-sync-static-salt";
  const int salt_len = 24;
  const int iterations = 10000;  // 推荐的迭代次数

  if (PKCS5_PBKDF2_HMAC(password.c_str(), password.length(), salt, salt_len,
                        iterations, EVP_sha256(), AES_KEY_BYTES,
                        key.data()) != 1) {
    throw std::runtime_error("PBKDF2 密钥派生失败");
  }

  return key;
}

// 加密
std::string Crypto::encrypt(const std::string& plaintext,
                            const std::vector<unsigned char>& key) {
  if (key.size() != AES_KEY_BYTES) {
    throw std::invalid_argument("无效的密钥长度");
  }

  // 1. 初始化 IV 和 Tag 缓冲区
  unsigned char iv[GCM_IV_BYTES];
  unsigned char tag[GCM_TAG_BYTES];

  // 2. 生成随机 IV
  if (RAND_bytes(iv, sizeof(iv)) != 1) {
    throw std::runtime_error("生成 IV 失败");
  }

  // 3. 准备密文缓冲区 (大小 = 明文大小 + 预留)
  std::vector<unsigned char> ciphertext(plaintext.length());
  int len = 0;  // 实际密文长度

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) throw std::runtime_error("创建 EVP context 失败");

  try {
    // 4. 初始化加密
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1)
      throw std::runtime_error("EVP_EncryptInit_ex 失败");
    // 5. 设置 IV 长度
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, GCM_IV_BYTES, NULL) !=
        1)
      throw std::runtime_error("设置 IV 长度失败");
    // 6. 设置密钥和 IV
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key.data(), iv) != 1)
      throw std::runtime_error("设置密钥和 IV 失败");

    // 7. 加密数据
    if (EVP_EncryptUpdate(ctx, ciphertext.data(), &len,
                          (const unsigned char*)plaintext.c_str(),
                          plaintext.length()) != 1)
      throw std::runtime_error("EVP_EncryptUpdate 失败");

    // 8. 结束加密 (GCM 模式下这一步通常不产生数据)
    int tmplen = 0;
    if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &tmplen) != 1)
      throw std::runtime_error("EVP_EncryptFinal_ex 失败 (数据可能被篡改)");
    len += tmplen;

    // 9. 获取 GCM Tag
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, GCM_TAG_BYTES, tag) != 1)
      throw std::runtime_error("获取 GCM Tag 失败");

    // 10. 清理
    EVP_CIPHER_CTX_free(ctx);

  } catch (...) {
    EVP_CIPHER_CTX_free(ctx);
    throw;
  }

  // 11. 组装包: IV + Ciphertext + Tag
  std::string result(GCM_IV_BYTES + len + GCM_TAG_BYTES, '\0');
  memcpy(&result[0], iv, GCM_IV_BYTES);
  memcpy(&result[GCM_IV_BYTES], ciphertext.data(), len);
  memcpy(&result[GCM_IV_BYTES + len], tag, GCM_TAG_BYTES);

  return result;
}

// 解密
std::string Crypto::decrypt(const std::string& ciphertext_package,
                            const std::vector<unsigned char>& key) {
  if (key.size() != AES_KEY_BYTES) {
    throw std::invalid_argument("无效的密钥长度");
  }
  if (ciphertext_package.length() < GCM_IV_BYTES + GCM_TAG_BYTES) {
    throw std::invalid_argument("密文包太短");
  }

  // 1. 拆解包: IV, Ciphertext, Tag
  const unsigned char* iv = (const unsigned char*)&ciphertext_package[0];
  const unsigned char* tag =
      (const unsigned char*)&ciphertext_package[ciphertext_package.length() -
                                                GCM_TAG_BYTES];
  const unsigned char* ciphertext =
      (const unsigned char*)&ciphertext_package[GCM_IV_BYTES];
  int ciphertext_len =
      ciphertext_package.length() - GCM_IV_BYTES - GCM_TAG_BYTES;

  // 2. 准备明文缓冲区
  std::vector<unsigned char> plaintext(ciphertext_len);
  int len = 0;  // 实际明文长度

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) throw std::runtime_error("创建 EVP context 失败");

  try {
    // 3. 初始化解密
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1)
      throw std::runtime_error("EVP_DecryptInit_ex 失败");
    // 4. 设置 IV 长度
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, GCM_IV_BYTES, NULL) !=
        1)
      throw std::runtime_error("设置 IV 长度失败");
    // 5. 设置密钥和 IV
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key.data(), iv) != 1)
      throw std::runtime_error("设置密钥和 IV 失败");

    // 6. 解密数据
    if (EVP_DecryptUpdate(ctx, plaintext.data(), &len, ciphertext,
                          ciphertext_len) != 1)
      throw std::runtime_error("EVP_DecryptUpdate 失败");

    // 7. 设置 GCM Tag (这一步必须在 Final 之前)
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, GCM_TAG_BYTES,
                            (void*)tag) != 1)
      throw std::runtime_error("设置 GCM Tag 失败");

    // 8. 结束解密
    // GCM 模式下，这一步会验证 Tag。如果 Tag 不匹配 (数据被篡改或密钥错误)，
    // 这一步将返回 0 或负数。
    int tmplen = 0;
    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &tmplen) <= 0) {
      throw std::runtime_error("解密失败或认证失败 (密钥错误或数据被篡改)");
    }
    len += tmplen;

    // 9. 清理
    EVP_CIPHER_CTX_free(ctx);

    return std::string((char*)plaintext.data(), len);

  } catch (...) {
    EVP_CIPHER_CTX_free(ctx);
    throw;
  }
}

}  // namespace VeritasSync
