#pragma once  

#include <filesystem>  
#include <string>  

namespace VeritasSync {  

class Hashing {  
 public:  
  // �����ļ���SHA-256��ϣֵ  
  // ����: �ļ�·��  
  // ���: 64���ַ���ʮ�����ƹ�ϣ�ַ��������ʧ���򷵻ؿ��ַ���  
  static std::string CalculateSHA256(const std::filesystem::path& filePath);  
};  

} // namespace VeritasSync
