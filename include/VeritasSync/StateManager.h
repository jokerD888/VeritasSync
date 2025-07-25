#pragma once

#include "VeritasSync/Protocol.h" // ������Ҫ�õ�FileInfo�ṹ��
#include <filesystem>
#include <string>
#include <map>
#include <functional>
#include <memory> 


namespace efsw {
class FileWatcher;
class FileWatchListener;
}



namespace VeritasSync {
class P2PManager;

  class StateManager {
  public:
    // ���캯������һ�� P2PManager ������
    StateManager(const std::string& root_path, P2PManager& p2p_manager);
    ~StateManager();

    // ɨ��ͬ��Ŀ¼�����ɵ�ǰ�����ļ���״̬����
    void scan_directory();

    // ����ǰ���ļ�״̬�����һ�� share_state ���͵�JSON�ַ���
    std::string get_state_as_json_string();

    // (���ڵ���) ��ӡ��ǰ�����ļ���״̬������̨
    void print_current_state() const;

    const std::filesystem::path& get_root_path() const { return m_root_path; }

  private:
    // ͬ��Ŀ¼�ĸ�·��
    std::filesystem::path m_root_path;

    // �ļ�״̬�ĺ��Ĵ洢�ṹ
    // key: �ļ������·�� (���� "docs/report.txt")
    // value: �ļ�����ϸ��Ϣ (FileInfo)
    std::map<std::string, FileInfo> m_file_map;
    std::unique_ptr<efsw::FileWatcher> m_file_watcher;
    std::unique_ptr<efsw::FileWatchListener> m_listener;
  };

} // namespace VeritasSync