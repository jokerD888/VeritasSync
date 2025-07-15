#pragma once  

#include <filesystem>  
#include <string>  

namespace VeritasSync {  

class Hashing {  
 public:  
  // 计算文件的SHA-256哈希值  
  // 输入: 文件路径  
  // 输出: 64个字符的十六进制哈希字符串，如果失败则返回空字符串  
  static std::string CalculateSHA256(const std::filesystem::path& filePath);  
};  

} // namespace VeritasSync
