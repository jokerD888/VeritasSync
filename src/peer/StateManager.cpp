#include "VeritasSync/StateManager.h"
#include "VeritasSync/Hashing.h"
#include <iostream>

namespace VeritasSync {

  StateManager::StateManager(const std::string& root_path)
    : m_root_path(root_path) {
    // ȷ��ͬ��Ŀ¼���ڣ�����������򴴽���
    if (!std::filesystem::exists(m_root_path)) {
      std::cout << "[StateManager] Root path " << m_root_path << " does not exist. Creating it." << std::endl;
      std::filesystem::create_directory(m_root_path);
    }
  }

  // �� src/peer/StateManager.cpp ��

  void StateManager::scan_directory() {
    std::cout << "[StateManager] Scanning directory: " << m_root_path << std::endl;
    m_file_map.clear();

    // --- �ؼ��޸� ---
    // Ϊ���������캯���ṩһ��error_code�����Ա�����·������ʱ�׳��쳣
    std::error_code ec;
    auto iterator = std::filesystem::recursive_directory_iterator(m_root_path, ec);

    if (ec) {
      std::cerr << "[StateManager] Error creating directory iterator for path "
        << m_root_path << ": " << ec.message() << std::endl;
      return; // �������������ʧ�ܣ���ֱ�ӷ���
    }

    for (const auto& entry : iterator) {
      // ��ѭ���ڲ�Ҳ���м�飬ȷ��entry��������Ч��
      if (!entry.is_regular_file(ec) || ec) {
        // ������ǳ����ļ�����ʱ�������������Ŀ
        continue;
      }

      FileInfo info;

      // 1. ��ȡ���·��
      info.path = std::filesystem::relative(entry.path(), m_root_path).generic_string();

      // 2. ��ȡ����޸�ʱ��
      auto ftime = std::filesystem::last_write_time(entry, ec);
      if (ec) continue; // �����ȡʱ��ʧ�ܣ�����
      auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(ftime);
      info.modified_time = sctp.time_since_epoch().count();

      // 3. �����ϣֵ
      info.hash = Hashing::CalculateSHA256(entry.path());
      if (info.hash.empty()) continue; // ��������ϣʧ�ܣ�����

      // 4. ����map
      m_file_map[info.path] = info;
    }
    std::cout << "[StateManager] Scan complete. Found " << m_file_map.size() << " files." << std::endl;
  }

  std::string StateManager::get_state_as_json_string() {
    // 1. ��map�е�FileInfo����ת����һ��vector
    std::vector<FileInfo> files;
    for (const auto& pair : m_file_map) {
      files.push_back(pair.second);
    }

    // 2. ʹ��nlohmann/json�⹹��JSON����
    nlohmann::json payload;
    payload["files"] = files; // ��������������֮ǰ����� to_json ����

    nlohmann::json message;
    message[Protocol::MSG_TYPE] = Protocol::TYPE_SHARE_STATE;
    message[Protocol::MSG_PAYLOAD] = payload;

    // 3. ��JSON�������л�Ϊ�ַ���
    return message.dump(2); // dump(2) ��ʾʹ��2���ո���и�ʽ���������Ķ�
  }

  void StateManager::print_current_state() const {
    std::cout << "\n--- Current Directory State ---" << std::endl;
    for (const auto& pair : m_file_map) {
      std::cout << "  - Path: " << pair.second.path << std::endl;
      std::cout << "    MTime: " << pair.second.modified_time << std::endl;
      std::cout << "    Hash: " << pair.second.hash.substr(0, 12) << "..." << std::endl;
    }
    std::cout << "-----------------------------" << std::endl;
  }
}