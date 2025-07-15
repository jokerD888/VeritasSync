#include <iostream>
#include <thread>

#include "VeritasSync/P2PManager.h"
#include "VeritasSync/TrackerClient.h"

int main(int argc, char* argv[]) {
  try {
    std::cout << "--- Veritas Sync Peer ---" << std::endl;

    // 1. ��ȡ�����Լ���P2P�˿�
    unsigned short my_port = 9001;
    if (argc > 1) {
      my_port = static_cast<unsigned short>(std::stoul(argv[1]));
    }
    std::cout << "[Main] Our P2P port is " << my_port << std::endl;

    // 2. ����Tracker����ȡ�ڵ��б�
    VeritasSync::TrackerClient tracker_client("127.0.0.1", 9988);
    std::string sync_key = "my-secret-room";
    std::vector<std::string> peers =
        tracker_client.register_and_query(sync_key, my_port);
    std::cout << "[Main] Found " << peers.size()
              << " other peer(s) from tracker." << std::endl;

    // 3. ��ʼ��P2P������
    VeritasSync::P2PManager p2p_manager(my_port);

    // 4. ���������з��ֵĽڵ㷢�͡��򶴡���Ϣ
    if (!peers.empty()) {
      p2p_manager.connect_to_peers(peers);
    }

    // 5. ��������������У��Ա��̨�߳��ܴ���P2P��Ϣ
    std::cout << "[Main] P2P Manager is running. Press Enter to exit."
              << std::endl;
    std::cin.get();

  } catch (const std::exception& e) {
    std::cerr << "Exception in main: " << e.what() << std::endl;
  }

  return 0;
}