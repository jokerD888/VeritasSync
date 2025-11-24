#pragma once

#include <memory>
#include <string>

#include "VeritasSync/Config.h"

namespace VeritasSync {

// 前向声明，减少头文件依赖
class P2PManager;
class TrackerClient;
class StateManager;

class SyncNode {
public:
    SyncNode(SyncTask task, const Config& global_config);
    ~SyncNode();

    std::shared_ptr<P2PManager> get_p2p();
    std::string get_key() const;
    std::string get_root_path() const;

    void start();

private:
    SyncTask m_task;
    Config m_global_config;

    std::shared_ptr<TrackerClient> m_tracker_client;
    std::shared_ptr<P2PManager> m_p2p_manager;
    std::unique_ptr<StateManager> m_state_manager;
};

}  // namespace VeritasSync