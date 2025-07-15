#pragma once

#include <string>
#include <vector>
#include <cstdint> // 用于 uint64_t

#include <nlohmann/json.hpp>

namespace VeritasSync {

  // 用于描述单个文件状态的结构体
  struct FileInfo {
    std::string path;              // 文件的相对路径
    std::string hash;              // 文件的SHA-256哈希值
    std::uint64_t modified_time;   // 文件的最后修改时间 (例如，自纪元以来的秒数)

    // 为了方便，重载一下比较操作符
    bool operator==(const FileInfo& other) const {
      return path == other.path && hash == other.hash && modified_time == other.modified_time;
    }
  };

  // --- nlohmann/json 集成魔法 ---
  // 通过在我们的命名空间中定义这两个函数，nlohmann/json库就能自动知道
  // 如何将我们的 FileInfo 结构体与JSON对象相互转换。

  // 将 FileInfo 对象转换为 JSON 对象
  inline void to_json(nlohmann::json& j, const FileInfo& info) {
    j = nlohmann::json{
        {"path", info.path},
        {"hash", info.hash},
        {"mtime", info.modified_time}
    };
  }

  // 将 JSON 对象转换为 FileInfo 对象
  inline void from_json(const nlohmann::json& j, FileInfo& info) {
    j.at("path").get_to(info.path);
    j.at("hash").get_to(info.hash);
    j.at("mtime").get_to(info.modified_time);
  }


  // --- 协议消息类型定义 ---
  // 使用一个结构体来存放所有协议中用到的字符串常量，便于管理
  struct Protocol {
    // 消息类型
    static constexpr const char* MSG_TYPE = "type";
    static constexpr const char* MSG_PAYLOAD = "payload";

    // `type` 字段的各种值
    static constexpr const char* TYPE_SHARE_STATE = "share_state";
    static constexpr const char* TYPE_REQUEST_FILE = "request_file";
    // ... 未来我们可以在这里添加更多消息类型, 如 MSG_FILE_CHUNK ...
  };


} // namespace VeritasSync