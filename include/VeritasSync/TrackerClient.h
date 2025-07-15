#pragma once

#include <string>
#include <vector>

namespace VeritasSync {

class TrackerClient {
 public:
  // ���캯����ָ��tracker�������ĵ�ַ�Ͷ˿�
  TrackerClient(std::string host, unsigned short port);

  // ���Ĺ��ܣ���Trackerע���Լ�������ѯͬһͬ����������ڵ�
  // ����ֵ�� "IP:Port" ��ʽ���ַ����б�
  // Ϊ�˼򻯣�����������ʹ��ͬ��(����)�ķ�ʽִ��
  std::vector<std::string> register_and_query(const std::string& sync_key,
                                              unsigned short p2p_port);

 private:
  std::string m_host;
  unsigned short m_port;
};

}