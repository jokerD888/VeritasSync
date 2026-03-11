#pragma once

// httplib 必须在 windows.h 之前包含，因为它需要先引入 winsock2.h
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <functional>
#include <mutex>
#include <string>
#include <nlohmann/json.hpp>

#include "VeritasSync/common/Config.h"

namespace VeritasSync {

class WebUIServer {
public:
    WebUIServer(int port, const std::string& config_path);

    using StatusProvider = std::function<nlohmann::json()>;
    using OnTaskAddCallback = std::function<void(const SyncTask&, const Config&)>;
    /// C-5: 删除任务时的回调，参数为被删除任务的 sync_key
    using OnTaskRemoveCallback = std::function<void(const std::string& sync_key)>;
    void set_status_provider(StatusProvider provider) { m_status_provider = provider; }
    void set_on_task_add(OnTaskAddCallback cb) { m_on_task_add = cb; }
    void set_on_task_remove(OnTaskRemoveCallback cb) { m_on_task_remove = cb; }

    void start();
    void stop();

    unsigned short get_port() const { return static_cast<unsigned short>(m_port); }

    /// 获取带认证 Token 的 WebUI URL（用于从托盘打开浏览器）
    std::string get_auth_url() const;

    void reload_config();
    bool save_config();

    static void open_folder_in_os(const std::string& p);
    static void open_url(const std::string& url);
    static std::string pick_folder_dialog();

private:
    httplib::Server m_svr;
    int m_port;
    std::string m_config_path;
    std::string m_absolute_config_path;
    Config m_config;
    std::mutex m_config_mutex;
    StatusProvider m_status_provider;
    OnTaskAddCallback m_on_task_add;
    OnTaskRemoveCallback m_on_task_remove;
    std::string m_auth_token;

    static std::string generate_token(size_t bytes);
    bool check_auth(const httplib::Request& req, httplib::Response& res) const;
    void setup_routes();
    bool save_config_internal();
    static std::filesystem::path get_exe_dir();
    static std::string get_index_html();
    static std::string tail_log(const std::string& file, std::size_t max_bytes);
    static void restart_application();
};

}  // namespace VeritasSync
