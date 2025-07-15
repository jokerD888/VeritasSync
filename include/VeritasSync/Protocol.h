#pragma once

#include <string>
#include <vector>
#include <cstdint> // ���� uint64_t

#include <nlohmann/json.hpp>

namespace VeritasSync {

  // �������������ļ�״̬�Ľṹ��
  struct FileInfo {
    std::string path;              // �ļ������·��
    std::string hash;              // �ļ���SHA-256��ϣֵ
    std::uint64_t modified_time;   // �ļ�������޸�ʱ�� (���磬�Լ�Ԫ����������)

    // Ϊ�˷��㣬����һ�±Ƚϲ�����
    bool operator==(const FileInfo& other) const {
      return path == other.path && hash == other.hash && modified_time == other.modified_time;
    }
  };

  // --- nlohmann/json ����ħ�� ---
  // ͨ�������ǵ������ռ��ж���������������nlohmann/json������Զ�֪��
  // ��ν����ǵ� FileInfo �ṹ����JSON�����໥ת����

  // �� FileInfo ����ת��Ϊ JSON ����
  inline void to_json(nlohmann::json& j, const FileInfo& info) {
    j = nlohmann::json{
        {"path", info.path},
        {"hash", info.hash},
        {"mtime", info.modified_time}
    };
  }

  // �� JSON ����ת��Ϊ FileInfo ����
  inline void from_json(const nlohmann::json& j, FileInfo& info) {
    j.at("path").get_to(info.path);
    j.at("hash").get_to(info.hash);
    j.at("mtime").get_to(info.modified_time);
  }


  // --- Э����Ϣ���Ͷ��� ---
  // ʹ��һ���ṹ�����������Э�����õ����ַ������������ڹ���
  struct Protocol {
    // ��Ϣ����
    static constexpr const char* MSG_TYPE = "type";
    static constexpr const char* MSG_PAYLOAD = "payload";

    // `type` �ֶεĸ���ֵ
    static constexpr const char* TYPE_SHARE_STATE = "share_state";
    static constexpr const char* TYPE_REQUEST_FILE = "request_file";
    // ... δ�����ǿ�����������Ӹ�����Ϣ����, �� MSG_FILE_CHUNK ...
  };


} // namespace VeritasSync