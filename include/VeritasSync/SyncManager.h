#pragma once

#include <string>
#include <vector>

#include "VeritasSync/Protocol.h"  // ��Ҫ������ʹ�� FileInfo �ṹ��

namespace VeritasSync {

class SyncManager {
 public:
  // �Ƚϱ��غ�Զ�̵�״̬����ȷ�����ؿͻ�����Ҫ��Զ�̽ڵ�������Щ�ļ���
  //
  // @param local_files ������Ŀ¼״̬�� FileInfo ������
  // @param remote_files ����Զ��Ŀ¼״̬�� FileInfo ������
  // @return һ���ַ���������ÿ���ַ�������һ����Ҫ������ļ������·����
  static std::vector<std::string> compare_states_and_get_requests(
      const std::vector<FileInfo>& local_files,
      const std::vector<FileInfo>& remote_files);
};

}  // namespace VeritasSync