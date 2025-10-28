#pragma once

#include <string>
#include <vector>

namespace VeritasSync {

class TrackerClient {
 public:
  // 构造函数，指定tracker服务器的地址和端口
  TrackerClient(std::string host, unsigned short port);

  // 核心功能：向Tracker注册自己，并查询同一同步组的其他节点
  // 返回值是 "IP:Port" 格式的字符串列表
  // 为了简化，我们在这里使用同步(阻塞)的方式执行
  std::vector<std::string> register_and_query(const std::string& sync_key,
                                              unsigned short p2p_port);

 private:
  std::string m_host;
  unsigned short m_port;
};

}