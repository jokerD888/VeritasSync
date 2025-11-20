#pragma once

#ifdef _WIN32
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <windows.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#endif

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>
#include <vector>

#include "VeritasSync/EncodingUtils.h"

// 【移除】不再包含嵌入资源头文件
// #include "embedded_resources.h"

#include "VeritasSync/Config.h"
#include "VeritasSync/Logger.h"

namespace VeritasSync {

class WebUIServer {
public:
    WebUIServer(int port, const std::string& config_path) : m_port(port), m_config_path(config_path) {
        try {
            m_absolute_config_path = std::filesystem::absolute(m_config_path).string();
        } catch (...) {
            m_absolute_config_path = m_config_path;
        }
        reload_config();
        setup_routes();
    }

    using StatusProvider = std::function<std::vector<nlohmann::json>()>;
    void set_status_provider(StatusProvider provider) { m_status_provider = provider; }

    void start() {
        if (g_logger) g_logger->info("[WebUI] 服务启动于 http://127.0.0.1:{}", m_port);
        m_svr.listen("0.0.0.0", m_port);
    }

    void stop() { m_svr.stop(); }

    unsigned short get_port() const { return static_cast<unsigned short>(m_port); }

    void reload_config() {
        std::lock_guard<std::mutex> lock(m_config_mutex);
        try {
            std::ifstream f(m_absolute_config_path);
            if (f.good()) {
                nlohmann::json j;
                f >> j;
                m_config = j.get<Config>();
            }
        } catch (const std::exception& e) {
            if (g_logger) g_logger->error("[WebUI] 加载配置失败: {}", e.what());
        }
    }

    bool save_config() {
        std::lock_guard<std::mutex> lock(m_config_mutex);
        return save_config_internal();
    }

private:
    httplib::Server m_svr;
    int m_port;
    std::string m_config_path;
    std::string m_absolute_config_path;
    Config m_config;
    std::mutex m_config_mutex;
    StatusProvider m_status_provider;

    void setup_routes() {
        // 1. 首页
        m_svr.Get("/", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(get_index_html(), "text/html; charset=utf-8");
        });

        // 2. 获取状态
        m_svr.Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
            nlohmann::json j = nlohmann::json::array();
            if (m_status_provider) {
                auto status_list = m_status_provider();
                for (const auto& item : status_list) j.push_back(item);
            }
            res.set_content(j.dump(), "application/json");
        });

        // 3. 获取日志
        m_svr.Get("/api/log", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(tail_log("veritas_sync.log", 16384), "text/plain; charset=utf-8");
        });

        // 4. 获取配置
        m_svr.Get("/api/config", [this](const httplib::Request&, httplib::Response& res) {
            std::lock_guard<std::mutex> lock(m_config_mutex);
            res.set_content(nlohmann::json(m_config).dump(2), "application/json; charset=utf-8");
        });

        // 5. 更新配置
        m_svr.Post("/api/config", [this](const httplib::Request& req, httplib::Response& res) {
            std::lock_guard<std::mutex> lock(m_config_mutex);
            try {
                auto j = nlohmann::json::parse(req.body);
                if (j.contains("tracker_host")) m_config.tracker_host = j.value("tracker_host", m_config.tracker_host);
                if (j.contains("tracker_port")) m_config.tracker_port = j.value("tracker_port", m_config.tracker_port);
                if (j.contains("stun_host")) m_config.stun_host = j.value("stun_host", m_config.stun_host);
                if (j.contains("stun_port")) m_config.stun_port = j.value("stun_port", m_config.stun_port);
                if (j.contains("turn_host")) m_config.turn_host = j.value("turn_host", m_config.turn_host);
                if (j.contains("turn_port")) m_config.turn_port = j.value("turn_port", m_config.turn_port);
                if (j.contains("turn_username"))
                    m_config.turn_username = j.value("turn_username", m_config.turn_username);
                if (j.contains("turn_password"))
                    m_config.turn_password = j.value("turn_password", m_config.turn_password);

                if (save_config_internal())
                    res.set_content("{\"success\":true}", "application/json");
                else
                    res.status = 500;
            } catch (const std::exception& e) {
                if (g_logger) g_logger->error("[WebUI] Config POST Error: {}", e.what());
                res.status = 400;
                res.set_content("{\"error\":\"Invalid JSON\"}", "application/json");
            }
        });

        // 6. 重启应用
        m_svr.Post("/api/restart", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content("{\"success\":true}", "application/json");
            std::thread([]() { restart_application(); }).detach();
        });

        // 7. 添加任务
        m_svr.Post("/api/tasks", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto j = nlohmann::json::parse(req.body);
                SyncTask task;
                task.sync_key = j.at("sync_key").get<std::string>();
                task.role = j.at("role").get<std::string>();
                task.sync_folder = j.at("sync_folder").get<std::string>();

                // [新增] 解析同步模式 (依赖 Config.h 中的 JSON 序列化宏)
                if (j.contains("mode")) {
                    task.mode = j.at("mode").get<SyncMode>();
                } else {
                    task.mode = SyncMode::OneWay;  // 默认单向
                }

                std::lock_guard<std::mutex> lock(m_config_mutex);
                m_config.tasks.push_back(task);

                if (save_config_internal())
                    res.set_content("{\"success\":true}", "application/json");
                else
                    res.status = 500;
            } catch (const std::exception& e) {
                if (g_logger) g_logger->error("[WebUI] Add Task Failed: {}", e.what());
                res.status = 400;
            }
        });

        // 8. 删除任务
        m_svr.Delete("/api/tasks/:id", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                size_t idx = std::stoul(req.path_params.at("id"));
                std::lock_guard<std::mutex> lock(m_config_mutex);
                if (idx < m_config.tasks.size()) {
                    m_config.tasks.erase(m_config.tasks.begin() + idx);
                    if (save_config_internal())
                        res.set_content("{\"success\":true}", "application/json");
                    else
                        res.status = 500;
                } else {
                    res.status = 404;
                }
            } catch (...) {
                res.status = 400;
            }
        });

        // 9. 获取任务日志
        m_svr.Get("/api/tasks/:id/log", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                size_t idx = std::stoul(req.path_params.at("id"));
                std::string log_content;
                {
                    std::lock_guard<std::mutex> lock(m_config_mutex);
                    if (idx < m_config.tasks.size()) {
                        std::string raw = tail_log("veritas_sync.log", 32768);
                        std::ostringstream oss;
                        std::istringstream iss(raw);
                        std::string line;
                        while (std::getline(iss, line)) {
                            if (line.find(m_config.tasks[idx].sync_key) != std::string::npos) oss << line << "\n";
                        }
                        log_content = oss.str();
                        if (log_content.empty()) log_content = "暂无相关日志";
                    }
                }
                res.set_content(log_content, "text/plain; charset=utf-8");
            } catch (...) {
                res.status = 400;
            }
        });

        // 10. 打开文件夹
        m_svr.Post("/api/tasks/:id/open", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                size_t idx = std::stoul(req.path_params.at("id"));
                std::string p;
                {
                    std::lock_guard<std::mutex> lock(m_config_mutex);
                    if (idx < m_config.tasks.size()) p = m_config.tasks[idx].sync_folder;
                }
                if (!p.empty()) {
                    open_folder_in_os(p);
                    res.set_content("{\"success\":true}", "application/json");
                } else {
                    res.status = 404;
                }
            } catch (...) {
                res.status = 500;
            }
        });

        // 11. 选择文件夹
        m_svr.Post("/api/utils/pick_folder", [](const httplib::Request&, httplib::Response& res) {
            std::string p = pick_folder_dialog();
            if (!p.empty()) {
                std::replace(p.begin(), p.end(), '\\', '/');
                nlohmann::json j;
                j["success"] = true;
                j["path"] = p;
                res.set_content(j.dump(), "application/json");
            } else {
                res.set_content("{\"success\":false}", "application/json");
            }
        });
    }

    bool save_config_internal() {
        try {
            std::ofstream o(m_absolute_config_path);
            if (!o.is_open()) {
                if (g_logger) g_logger->error("[WebUI] 无法打开文件写入: {}", m_absolute_config_path);
                return false;
            }
            o << nlohmann::json(m_config).dump(4) << std::endl;
            return true;
        } catch (const std::exception& e) {
            if (g_logger) g_logger->error("[WebUI] 保存异常: {}", e.what());
            return false;
        }
    }

    // --- 从磁盘读取 HTML ---
    static std::string get_index_html() {
        // 尝试多个路径，确保在开发和部署时都能找到
        std::vector<std::string> try_paths = {
            "web/index.html",        // 部署模式 (配合 CMake Copy 步骤)
            "src/web/index.html",    // 开发模式 (源码根目录)
            "../src/web/index.html"  // 开发模式 (Build 目录)
        };

        for (const auto& path : try_paths) {
            if (std::filesystem::exists(path)) {
                std::ifstream t(path);
                if (t.is_open()) {
                    std::stringstream buffer;
                    buffer << t.rdbuf();
                    return buffer.str();
                }
            }
        }

        // 如果找不到文件，返回错误提示
        return R"(<html>
            <head><meta charset="UTF-8"><title>Error</title></head>
            <body style="font-family:sans-serif; text-align:center; padding:50px;">
                <h1 style="color:#ef4444">404 - Web UI Not Found</h1>
                <p>Could not find <code>index.html</code> in 'web/' or 'src/web/'.</p>
                <p>Please ensure the <code>web</code> folder is copied next to the executable.</p>
            </body>
        </html>)";
    }

    static std::string tail_log(const std::string& file, std::size_t max_bytes) {
        std::ifstream ifs(file, std::ios::binary);
        if (!ifs) return "";
        ifs.seekg(0, std::ios::end);
        auto size = static_cast<std::size_t>(ifs.tellg());
        auto start = size > max_bytes ? size - max_bytes : 0;
        std::string content(size - start, '\0');
        ifs.seekg(start, std::ios::beg);
        ifs.read(&content[0], content.size());
        return content;
    }

    static void open_folder_in_os(const std::string& p) {
        // 1. 使用 EncodingUtils 的 Utf8ToPath 将 UTF-8 字符串转为 filesystem::path
        //    这解决了 Windows 下 std::filesystem::path(std::string) 默认视作 ANSI (GBK) 的问题
        std::filesystem::path path_obj = Utf8ToPath(p);

        try {
            // 2. 转换为绝对路径 (解决 explorer 对相对路径支持不佳的问题)
            path_obj = std::filesystem::absolute(path_obj);
        } catch (const std::exception& e) {
            if (g_logger) g_logger->error("[WebUI] 路径解析异常: {} ({})", p, e.what());
            return;
        }

#ifdef _WIN32
        // Windows: 使用 ShellExecuteW (Unicode API)
        // 关键点：C++ filesystem::path 在 Windows 下的 .c_str() 方法直接返回 const wchar_t*
        // 这样可以直接传递给 ShellExecuteW，完美支持中文路径
        ShellExecuteW(NULL, L"open", path_obj.c_str(), NULL, NULL, SW_SHOWNORMAL);
#elif defined(__APPLE__)
        // macOS: 使用 open 命令
        // 注意：path_obj.string() 在 macOS 下返回 UTF-8，加上引号处理空格
        std::string cmd = "open \"" + path_obj.string() + "\"";
        system(cmd.c_str());
#else
        // Linux: 使用 xdg-open
        std::string cmd = "xdg-open \"" + path_obj.string() + "\"";
        system(cmd.c_str());
#endif
    }

    static void restart_application() {
        if (g_logger) g_logger->info("[WebUI] 正在重启...");
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
#ifdef _WIN32
        char szPath[MAX_PATH];
        if (GetModuleFileNameA(NULL, szPath, MAX_PATH)) {
            ShellExecuteA(NULL, "open", szPath, NULL, NULL, SW_SHOWNORMAL);
        }
        std::exit(0);
#else
        system("./veritas_sync &");
        std::exit(0);
#endif
    }

    static std::string pick_folder_dialog() {
#ifdef _WIN32
        std::string path;
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        IFileDialog* pfd = NULL;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
            DWORD dwOptions;
            if (SUCCEEDED(pfd->GetOptions(&dwOptions)))
                pfd->SetOptions(dwOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
            if (SUCCEEDED(pfd->Show(NULL))) {
                IShellItem* psi;
                if (SUCCEEDED(pfd->GetResult(&psi))) {
                    PWSTR pszPath;
                    if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
                        std::wstring wPathStr(pszPath);
                        path = WideToUtf8(wPathStr);
                        CoTaskMemFree(pszPath);
                    }
                    psi->Release();
                }
            }
            pfd->Release();
        }
        CoUninitialize();
        return path;
#else
        return "";
#endif
    }
};

}  // namespace VeritasSync