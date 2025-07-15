#include "VeritasSync/Hashing.h"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#include <openssl/sha.h>

namespace VeritasSync {

std::string Hashing::CalculateSHA256(const std::filesystem::path& filePath) {
  // 1. ����ļ��Ƿ������Ϊ�����ļ�
  if (!std::filesystem::exists(filePath) ||
      !std::filesystem::is_regular_file(filePath)) {
    return "";  // ���ؿձ�ʾʧ��
  }

  // 2. �Զ�����ģʽ���ļ�
  std::ifstream file(filePath, std::ios::binary);
  if (!file.is_open()) {
    return "";
  }

  // 3. ��ʼ��SHA256������
  SHA256_CTX sha256Context;
  if (!SHA256_Init(&sha256Context)) {
    return "";
  }

  // 4. �ֿ��ȡ�ļ������¹�ϣֵ
  std::vector<char> buffer(4096);  // 4KB�Ļ�����
  while (file.good()) {
    file.read(buffer.data(), buffer.size());
    std::streamsize bytesRead = file.gcount();
    if (bytesRead > 0) {
      if (!SHA256_Update(&sha256Context, buffer.data(), bytesRead)) {
        return "";
      }
    }
  }

  // 5. �������յĹ�ϣժҪ
  unsigned char hash[SHA256_DIGEST_LENGTH];
  if (!SHA256_Final(hash, &sha256Context)) {
    return "";
  }

  // 6. �������ƹ�ϣת��Ϊʮ�������ַ���
  std::stringstream ss;
  for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
    ss << std::hex << std::setw(2) << std::setfill('0')
       << static_cast<int>(hash[i]);
  }

  return ss.str();
}

}