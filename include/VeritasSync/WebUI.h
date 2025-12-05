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

    using StatusProvider = std::function<nlohmann::json()>;
    // 定义添加任务的回调类型
    using OnTaskAddCallback = std::function<void(const SyncTask&, const Config&)>;
    void set_status_provider(StatusProvider provider) { m_status_provider = provider; }
    void set_on_task_add(OnTaskAddCallback cb) { m_on_task_add = cb; }

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

    static void open_url(const std::string& url) {
#ifdef _WIN32
        // URL 不需要转 filesystem path，直接转宽字符即可
        std::wstring wUrl = Utf8ToWide(url);
        ShellExecuteW(NULL, L"open", wUrl.c_str(), NULL, NULL, SW_SHOWNORMAL);
#elif defined(__APPLE__)
        std::string cmd = "open \"" + url + "\"";
        system(cmd.c_str());
#else
        std::string cmd = "xdg-open \"" + url + "\"";
        system(cmd.c_str());
#endif
    }

    static std::string pick_folder_dialog() {
#ifdef _WIN32
        std::string path;
        // 确保单线程单元初始化
        HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(hr)) {
            // 如果已经初始化过，也是 OK 的
        }

        IFileDialog* pfd = NULL;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
            DWORD dwOptions;
            if (SUCCEEDED(pfd->GetOptions(&dwOptions)))
                pfd->SetOptions(dwOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);

            // 获取当前控制台窗口句柄，作为父窗口
            // 如果没有控制台 (GUI应用)，GetConsoleWindow 返回 NULL，效果和传 NULL 一样
            // 但如果有控制台，这能确保弹窗在控制台之上
            HWND hParent = GetConsoleWindow();

            // 显示对话框
            if (SUCCEEDED(pfd->Show(hParent))) {
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

private:
    httplib::Server m_svr;
    int m_port;
    std::string m_config_path;
    std::string m_absolute_config_path;
    Config m_config;
    std::mutex m_config_mutex;
    StatusProvider m_status_provider;
    OnTaskAddCallback m_on_task_add;

    void setup_routes() {
        // 1. 首页
        m_svr.Get("/", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(get_index_html(), "text/html; charset=utf-8");
        });

        // 2. 获取状态
        m_svr.Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
            nlohmann::json j;
            if (m_status_provider) {
                j = m_status_provider();  // 获取完整对象
            } else {
                // 默认空结构
                j = {{"global", {{"tracker_online", false}, {"session_done", 0}, {"session_total", 0}}},
                     {"nodes", nlohmann::json::array()}};
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

                // --- 临时变量用于校验，防止直接污染 m_config ---
                std::string new_tracker_host = j.value("tracker_host", m_config.tracker_host);
                int new_tracker_port = j.value("tracker_port", (int)m_config.tracker_port);

                std::string new_stun_host = j.value("stun_host", m_config.stun_host);
                int new_stun_port = j.value("stun_port", (int)m_config.stun_port);

                std::string new_turn_host = j.value("turn_host", m_config.turn_host);
                int new_turn_port = j.value("turn_port", (int)m_config.turn_port);



                // --- 校验逻辑 ---
                if (new_tracker_port < 1 || new_tracker_port > 65535 || new_stun_port < 1 || new_stun_port > 65535 ||
                    new_turn_port < 1 || new_turn_port > 65535) {
                    if (g_logger) g_logger->warn("[WebUI] 配置保存失败: 端口号必须在 1-65535 之间");
                    res.status = 400;
                    res.set_content("{\"error\":\"端口号无效，请输入 1-65535 之间的整数\"}",
                                    "application/json; charset=utf-8");
                    return;
                }

                // --- 校验通过，应用配置 ---
                m_config.tracker_host = new_tracker_host;
                m_config.tracker_port = (unsigned short)new_tracker_port;

                m_config.stun_host = new_stun_host;
                m_config.stun_port = (unsigned short)new_stun_port;  // 保存 STUN 端口

                m_config.turn_host = new_turn_host;
                m_config.turn_port = (unsigned short)new_turn_port;  // 保存 TURN 端口


                if (j.contains("turn_username"))
                    m_config.turn_username = j.value("turn_username", m_config.turn_username);
                if (j.contains("turn_password"))
                    m_config.turn_password = j.value("turn_password", m_config.turn_password);

                // 保存到磁盘
                if (save_config_internal()) {
                    res.set_content("{\"success\":true}", "application/json");
                } else {
                    res.status = 500;
                    res.set_content("{\"error\":\"无法写入配置文件\"}", "application/json");
                }
            } catch (const std::exception& e) {
                if (g_logger) g_logger->error("[WebUI] Config POST Error: {}", e.what());
                res.status = 400;
                res.set_content("{\"error\":\"无效的 JSON 格式或参数错误\"}", "application/json");
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

                if (j.contains("mode")) {
                    task.mode = j.at("mode").get<SyncMode>();
                } else {
                    task.mode = SyncMode::OneWay;
                }

                // 1. 保存到配置
                {
                    std::lock_guard<std::mutex> lock(m_config_mutex);
                    m_config.tasks.push_back(task);
                    if (!save_config_internal()) {
                        res.status = 500;
                        return;
                    }

                    // 2. [新增] 触发回调，通知主程序立即启动新任务
                    if (m_on_task_add) {
                        // 传入当前的配置副本（包含Tracker信息等）
                        m_on_task_add(task, m_config);
                    }
                }

                res.set_content("{\"success\":true}", "application/json");
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

    // 获取当前可执行文件所在的目录 (绝对路径)
    static std::filesystem::path get_exe_dir() {
#ifdef _WIN32
        wchar_t buffer[MAX_PATH];
        // GetModuleFileNameW 直接填充宽字符，filesystem::path 构造函数完美兼容
        if (GetModuleFileNameW(NULL, buffer, MAX_PATH) > 0) {
            return std::filesystem::path(buffer).parent_path();
        }
#endif
        return std::filesystem::current_path();
    }

    // --- 从磁盘读取 HTML ---
    static std::string get_index_html() {
        std::filesystem::path exe_dir = get_exe_dir();

        std::vector<std::filesystem::path> try_paths;
        // 1. 发布模式: bin/web/index.html
        try_paths.push_back(exe_dir / "web" / "index.html");
        // 2. 开发模式: 相对路径
        try_paths.push_back("web/index.html");
        try_paths.push_back("src/web/index.html");
        try_paths.push_back("../src/web/index.html");

        for (const auto& path : try_paths) {
            std::error_code ec;
            if (std::filesystem::exists(path, ec) && !ec) {
                // 使用 Utf8ToPath 打开文件流
                std::ifstream t(Utf8ToPath(PathToUtf8(path)));
                // 上面略显啰嗦是因为 fstream 在 Windows 下需要 wstring (Utf8ToPath返回的 path 包含 wstring)
                // 或者是直接用 t(path) 也是可以的，因为 MSVC 的 fstream 支持 path 重载

                if (t.is_open()) {
                    // 【日志打印】这里必须用 PathToUtf8，否则中文路径在控制台乱码
                    if (g_logger) g_logger->info("[WebUI] Loaded UI from: {}", PathToUtf8(path));
                    std::stringstream buffer;
                    buffer << t.rdbuf();
                    return buffer.str();
                }
            }
        }

        return R"(<html><body><h1 style="color:red;text-align:center;margin-top:50px">404 - Web UI Not Found</h1></body></html>)";
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
};

}  // namespace VeritasSync