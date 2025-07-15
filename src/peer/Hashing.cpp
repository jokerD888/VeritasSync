#include "VeritasSync/Hashing.h"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#include <openssl/sha.h>

namespace VeritasSync {

std::string Hashing::CalculateSHA256(const std::filesystem::path& filePath) {
  // 1. 检查文件是否存在且为常规文件
  if (!std::filesystem::exists(filePath) ||
      !std::filesystem::is_regular_file(filePath)) {
    return "";  // 返回空表示失败
  }

  // 2. 以二进制模式打开文件
  std::ifstream file(filePath, std::ios::binary);
  if (!file.is_open()) {
    return "";
  }

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

}