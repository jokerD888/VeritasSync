#include "VeritasSync/SyncManager.h"

#include <iostream>
#include <map>

namespace VeritasSync {

std::vector<std::string> SyncManager::compare_states_and_get_requests(
    const std::vector<FileInfo>& local_files,
    const std::vector<FileInfo>& remote_files) {
  std::vector<std::string> files_to_request;

  // Ϊ�˸�Ч���ң��������ļ��б�ת��Ϊһ��map
  // ��: �ļ�·��, ֵ: �ļ���ϣֵ
  std::map<std::string, std::string> local_file_hashes;
  for (const auto& info : local_files) {
    local_file_hashes[info.path] = info.hash;
  }

  std::cout << "[SyncManager] ���ڱȽϱ���״̬ (" << local_files.size()
            << " ���ļ�) ��Զ��״̬ (" << remote_files.size() << " ���ļ�)."
            << std::endl;

  // ����Զ�̽ڵ�ӵ�е�ÿһ���ļ�
  for (const auto& remote_file : remote_files) {
    auto it = local_file_hashes.find(remote_file.path);

    // ���һ: ������ȫû������ļ�����������
    if (it == local_file_hashes.end()) {
      std::cout << "[SyncManager] -> ��Ҫ�������ļ�: " << remote_file.path
                << std::endl;
      files_to_request.push_back(remote_file.path);
    }
    // �����: ����������ļ�������ϣֵ��ͬ����������
    // (ע��: �����ӵ�ʵ�ֿ��ܻ������޸�ʱ��)
    else if (it->second != remote_file.hash) {
      std::cout << "[SyncManager] -> ��Ҫ������µ��ļ�: " << remote_file.path
                << std::endl;
      files_to_request.push_back(remote_file.path);
    }
    // �����: ����ӵ����ͬ�汾���ļ���ʲô��������
  }

  if (files_to_request.empty()) {
    std::cout << "[SyncManager] �����ļ����������¡���������" << std::endl;
  }

  return files_to_request;
}

}  // namespace VeritasSync