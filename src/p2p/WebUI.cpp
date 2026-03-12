#include "VeritasSync/p2p/WebUI.h"

#ifdef _WIN32
// 注意：WebUI.h 已经 include 了 <windows.h>（通过 httplib），
// 所以这里只需补充 shell API 相关头文件
#include <shellapi.h>
#include <shlobj.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#endif

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

#include <openssl/rand.h>

#include "VeritasSync/common/EncodingUtils.h"
#include "VeritasSync/common/Logger.h"

namespace VeritasSync {

// E-1: 魔数统一为命名常量
static constexpr size_t LOG_TAIL_SIZE_STATUS = 16384;   // GET /api/log 尾部读取字节数
static constexpr size_t LOG_TAIL_SIZE_TASK   = 32768;   // GET /api/tasks/:id/log 尾部读取字节数
static constexpr int    RESTART_DELAY_MS     = 1000;    // 重启前等待延迟（毫秒）
static constexpr size_t AUTH_TOKEN_BYTES     = 32;      // 认证令牌字节数

// ═══════════════════════════════════════════════════════════════
// 构造函数
// ═══════════════════════════════════════════════════════════════

WebUIServer::WebUIServer(int port, const std::string& config_path)
    : m_port(port), m_config_path(config_path) {
    try {
        m_absolute_config_path = std::filesystem::absolute(m_config_path).string();
    } catch (...) {
        m_absolute_config_path = m_config_path;
    }
    m_auth_token = generate_token(AUTH_TOKEN_BYTES);
    reload_config();

    // 初始化自然语言过滤规则生成器的 LLM 配置（如果有配置的话）
    if (!m_config.llm_api_url.empty() && !m_config.llm_api_key.empty()) {
        NLFilterGenerator::LLMConfig llm_cfg;
        llm_cfg.api_url = m_config.llm_api_url;
        llm_cfg.api_key = m_config.llm_api_key;
        llm_cfg.model = m_config.llm_model;
        m_nl_filter.set_llm_config(llm_cfg);
    }

    setup_routes();
}

// ═══════════════════════════════════════════════════════════════
// 公有方法
// ═══════════════════════════════════════════════════════════════

void WebUIServer::start() {
    if (g_logger) g_logger->info("[WebUI] 服务启动于 http://127.0.0.1:{}", m_port);
    m_svr.listen("127.0.0.1", m_port);
}

void WebUIServer::stop() { m_svr.stop(); }

std::string WebUIServer::get_auth_url() const {
    return "http://127.0.0.1:" + std::to_string(m_port) + "/?token=" + m_auth_token;
}

void WebUIServer::reload_config() {
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

bool WebUIServer::save_config() {
    std::lock_guard<std::mutex> lock(m_config_mutex);
    return save_config_internal();
}

// ═══════════════════════════════════════════════════════════════
// 静态工具方法
// ═══════════════════════════════════════════════════════════════

void WebUIServer::open_folder_in_os(const std::string& p) {
    std::filesystem::path path_obj = Utf8ToPath(p);
    try {
        path_obj = std::filesystem::absolute(path_obj);
    } catch (const std::exception& e) {
        if (g_logger) g_logger->error("[WebUI] 路径解析异常: {} ({})", p, e.what());
        return;
    }

#ifdef _WIN32
    ShellExecuteW(NULL, L"open", path_obj.c_str(), NULL, NULL, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string cmd = "open \"" + path_obj.string() + "\"";
    system(cmd.c_str());
#else
    std::string cmd = "xdg-open \"" + path_obj.string() + "\"";
    system(cmd.c_str());
#endif
}

void WebUIServer::open_url(const std::string& url) {
#ifdef _WIN32
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

std::string WebUIServer::pick_folder_dialog() {
#ifdef _WIN32
    std::string path;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        // 如果已经初始化过，也是 OK 的
    }

    IFileDialog* pfd = NULL;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
        DWORD dwOptions;
        if (SUCCEEDED(pfd->GetOptions(&dwOptions)))
            pfd->SetOptions(dwOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);

        HWND hParent = GetConsoleWindow();

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

// ═══════════════════════════════════════════════════════════════
// 私有方法
// ═══════════════════════════════════════════════════════════════

std::string WebUIServer::generate_token(size_t bytes) {
    // S-1 安全修复: 使用 OpenSSL CSPRNG 替代 mt19937
    std::vector<unsigned char> buf(bytes);
    if (RAND_bytes(buf.data(), static_cast<int>(bytes)) != 1) {
        // 极罕见的回退方案
        std::random_device rd;
        for (auto& b : buf) b = static_cast<unsigned char>(rd());
    }

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (auto b : buf) {
        oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
}

bool WebUIServer::check_auth(const httplib::Request& req, httplib::Response& res) const {
    // 1. 检查 Authorization: Bearer <token> 请求头
    auto auth_header = req.get_header_value("Authorization");
    if (auth_header.size() > 7 && auth_header.substr(0, 7) == "Bearer ") {
        if (auth_header.substr(7) == m_auth_token) return true;
    }
    // 2. 检查 URL 参数 ?token=xxx
    if (req.has_param("token") && req.get_param_value("token") == m_auth_token) {
        return true;
    }
    // 3. 认证失败
    res.status = 401;
    res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
    return false;
}

// C-1: setup_routes() 拆分为按功能域分组的子函数
void WebUIServer::setup_routes() {
    setup_page_routes();
    setup_status_routes();
    setup_config_routes();
    setup_task_routes();
    setup_task_detail_routes();
    setup_ignore_routes();
    setup_nl_filter_routes();
}

// ─── C-1 子函数：首页路由 ───────────────────────────────────────

void WebUIServer::setup_page_routes() {
    m_svr.Get("/", [this](const httplib::Request& req, httplib::Response& res) {
        if (req.has_param("token") && req.get_param_value("token") == m_auth_token) {
            res.set_content(get_index_html(), "text/html; charset=utf-8");
        } else {
            res.status = 401;
            res.set_content(
                R"(<!DOCTYPE html><html><head><meta charset="UTF-8"><title>VeritasSync</title></head>)"
                R"(<body style="background:#0b1121;color:#94a3b8;display:flex;justify-content:center;align-items:center;height:100vh;font-family:system-ui">)"
                R"(<div style="text-align:center"><h2 style="color:#f1f5f9;margin-bottom:12px">🔒 需要授权</h2>)"
                R"(<p>请通过系统托盘的 "打开控制台" 菜单访问 WebUI</p></div></body></html>)",
                "text/html; charset=utf-8");
        }
    });
}

// ─── C-1 子函数：状态与日志路由 ─────────────────────────────────

void WebUIServer::setup_status_routes() {
    // GET /api/status
    m_svr.Get("/api/status", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        nlohmann::json j;
        if (m_status_provider) {
            j = m_status_provider();
        } else {
            j = {{"global", {{"tracker_online", false}, {"session_done", 0}, {"session_total", 0}}},
                 {"nodes", nlohmann::json::array()}};
        }
        res.set_content(j.dump(), "application/json");
    });

    // GET /api/log
    m_svr.Get("/api/log", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        res.set_content(tail_log("veritas_sync.log", LOG_TAIL_SIZE_STATUS), "text/plain; charset=utf-8");
    });
}

// ─── C-1 子函数：配置与重启路由 ─────────────────────────────────

void WebUIServer::setup_config_routes() {
    // GET /api/config
    m_svr.Get("/api/config", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        std::lock_guard<std::mutex> lock(m_config_mutex);
        res.set_content(nlohmann::json(m_config).dump(2), "application/json; charset=utf-8");
    });

    // POST /api/config
    m_svr.Post("/api/config", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        std::lock_guard<std::mutex> lock(m_config_mutex);
        try {
            auto j = nlohmann::json::parse(req.body);

            std::string new_tracker_host = j.value("tracker_host", m_config.tracker_host);
            int new_tracker_port = j.value("tracker_port", (int)m_config.tracker_port);

            std::string new_stun_host = j.value("stun_host", m_config.stun_host);
            int new_stun_port = j.value("stun_port", (int)m_config.stun_port);

            std::string new_turn_host = j.value("turn_host", m_config.turn_host);
            int new_turn_port = j.value("turn_port", (int)m_config.turn_port);

            if (new_tracker_port < 1 || new_tracker_port > 65535 || new_stun_port < 1 || new_stun_port > 65535 ||
                new_turn_port < 1 || new_turn_port > 65535) {
                if (g_logger) g_logger->warn("[WebUI] 配置保存失败: 端口号必须在 1-65535 之间");
                res.status = 400;
                res.set_content("{\"error\":\"端口号无效，请输入 1-65535 之间的整数\"}",
                                "application/json; charset=utf-8");
                return;
            }

            m_config.tracker_host = new_tracker_host;
            m_config.tracker_port = (unsigned short)new_tracker_port;
            m_config.stun_host = new_stun_host;
            m_config.stun_port = (unsigned short)new_stun_port;
            m_config.turn_host = new_turn_host;
            m_config.turn_port = (unsigned short)new_turn_port;

            if (j.contains("turn_username"))
                m_config.turn_username = j.value("turn_username", m_config.turn_username);
            if (j.contains("turn_password"))
                m_config.turn_password = j.value("turn_password", m_config.turn_password);

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

    // POST /api/restart
    m_svr.Post("/api/restart", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        res.set_content("{\"success\":true}", "application/json");
        std::thread([]() { restart_application(); }).detach();
    });
}

// ─── C-1 子函数：任务增删路由 ───────────────────────────────────

void WebUIServer::setup_task_routes() {
    // POST /api/tasks — 添加任务
    m_svr.Post("/api/tasks", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
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

            {
                std::lock_guard<std::mutex> lock(m_config_mutex);
                m_config.tasks.push_back(task);
                if (!save_config_internal()) {
                    res.status = 500;
                    return;
                }

                if (m_on_task_add) {
                    m_on_task_add(task, m_config);
                }
            }

            res.set_content("{\"success\":true}", "application/json");
        } catch (const std::exception& e) {
            if (g_logger) g_logger->error("[WebUI] Add Task Failed: {}", e.what());
            res.status = 400;
        }
    });

    // DELETE /api/tasks/:id — 删除任务
    // C-5 正确性修复: 删除任务时同步停止对应的 SyncNode
    m_svr.Delete("/api/tasks/:id", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        try {
            size_t idx = std::stoul(req.path_params.at("id"));
            std::string removed_sync_key;
            bool save_ok = false;
            
            {
                std::lock_guard<std::mutex> lock(m_config_mutex);
                if (idx < m_config.tasks.size()) {
                    removed_sync_key = m_config.tasks[idx].sync_key;
                    m_config.tasks.erase(m_config.tasks.begin() + idx);
                    save_ok = save_config_internal();
                } else {
                    res.status = 404;
                    return;
                }
            }
            
            if (!save_ok) {
                res.status = 500;
                return;
            }
            
            // 在配置锁外通知上层停止并清理 SyncNode
            if (m_on_task_remove && !removed_sync_key.empty()) {
                m_on_task_remove(removed_sync_key);
            }
            
            res.set_content("{\"success\":true}", "application/json");
        } catch (...) {
            res.status = 400;
        }
    });
}

// ─── C-1 子函数：任务详情路由（日志、打开文件夹、选择文件夹） ──

void WebUIServer::setup_task_detail_routes() {
    // GET /api/tasks/:id/log
    m_svr.Get("/api/tasks/:id/log", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        try {
            size_t idx = std::stoul(req.path_params.at("id"));
            std::string log_content;
            {
                std::lock_guard<std::mutex> lock(m_config_mutex);
                if (idx < m_config.tasks.size()) {
                    std::string raw = tail_log("veritas_sync.log", LOG_TAIL_SIZE_TASK);
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

    // POST /api/tasks/:id/open
    m_svr.Post("/api/tasks/:id/open", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
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

    // POST /api/utils/pick_folder
    m_svr.Post("/api/utils/pick_folder", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
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

// ─── C-1 子函数：忽略规则路由 ───────────────────────────────────

void WebUIServer::setup_ignore_routes() {
    // GET /api/tasks/:id/ignore
    m_svr.Get("/api/tasks/:id/ignore", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        try {
            size_t idx = std::stoul(req.path_params.at("id"));
            std::string content;
            {
                std::lock_guard<std::mutex> lock(m_config_mutex);
                if (idx < m_config.tasks.size()) {
                    std::filesystem::path ignore_path = Utf8ToPath(m_config.tasks[idx].sync_folder) / ".veritasignore";
                    if (std::filesystem::exists(ignore_path)) {
                        std::ifstream f(ignore_path);
                        if (f.good()) {
                            std::ostringstream oss;
                            oss << f.rdbuf();
                            content = oss.str();
                        }
                    }
                }
            }
            nlohmann::json j;
            j["success"] = true;
            j["content"] = content;
            res.set_content(j.dump(), "application/json");
        } catch (const std::exception& e) {
            nlohmann::json j;
            j["success"] = false;
            j["error"] = e.what();
            res.set_content(j.dump(), "application/json");
            res.status = 400;
        }
    });

    // POST /api/tasks/:id/ignore
    m_svr.Post("/api/tasks/:id/ignore", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        try {
            size_t idx = std::stoul(req.path_params.at("id"));
            nlohmann::json body = nlohmann::json::parse(req.body);
            std::string content = body.value("content", "");
            
            std::filesystem::path ignore_path;
            {
                std::lock_guard<std::mutex> lock(m_config_mutex);
                if (idx < m_config.tasks.size()) {
                    ignore_path = Utf8ToPath(m_config.tasks[idx].sync_folder) / ".veritasignore";
                }
            }
            
            if (!ignore_path.empty()) {
                std::ofstream f(ignore_path);
                if (f.good()) {
                    f << content;
                    f.close();
                    nlohmann::json j;
                    j["success"] = true;
                    res.set_content(j.dump(), "application/json");
                    if (g_logger) g_logger->info("[WebUI] 已保存忽略规则: {}", PathToUtf8(ignore_path));
                } else {
                    res.status = 500;
                    res.set_content("{\"success\":false,\"error\":\"无法写入文件\"}", "application/json");
                }
            } else {
                res.status = 404;
                res.set_content("{\"success\":false,\"error\":\"任务不存在\"}", "application/json");
            }
        } catch (const std::exception& e) {
            nlohmann::json j;
            j["success"] = false;
            j["error"] = e.what();
            res.set_content(j.dump(), "application/json");
            res.status = 400;
        }
    });
}

// ─── C-1 子函数：自然语言过滤规则生成路由 ────────────────────────

void WebUIServer::setup_nl_filter_routes() {
    // POST /api/tasks/:id/ignore/generate — 自然语言生成过滤规则
    m_svr.Post("/api/tasks/:id/ignore/generate", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        try {
            size_t idx = std::stoul(req.path_params.at("id"));
            nlohmann::json body = nlohmann::json::parse(req.body);
            std::string description = body.value("description", "");

            if (description.empty()) {
                res.status = 400;
                res.set_content("{\"success\":false,\"error\":\"请输入描述\"}", "application/json");
                return;
            }

            // 读取现有规则（供 LLM 参考，避免重复）
            std::string existing_rules;
            {
                std::lock_guard<std::mutex> lock(m_config_mutex);
                if (idx < m_config.tasks.size()) {
                    std::filesystem::path ignore_path = Utf8ToPath(m_config.tasks[idx].sync_folder) / ".veritasignore";
                    if (std::filesystem::exists(ignore_path)) {
                        std::ifstream f(ignore_path);
                        if (f.good()) {
                            std::ostringstream oss;
                            oss << f.rdbuf();
                            existing_rules = oss.str();
                        }
                    }
                } else {
                    res.status = 404;
                    res.set_content("{\"success\":false,\"error\":\"任务不存在\"}", "application/json");
                    return;
                }
            }

            // 调用自然语言规则生成器
            auto result = m_nl_filter.generate(description, existing_rules);

            nlohmann::json j;
            j["success"] = result.success;
            if (result.success) {
                j["rules"] = result.rules;
                j["explanation"] = result.explanation;
                j["source"] = result.source;
            } else {
                j["error"] = result.error;
            }
            res.set_content(j.dump(), "application/json");

            if (g_logger) {
                if (result.success) {
                    g_logger->info("[WebUI] 自然语言规则生成成功: source={}, 描述=\"{}\"",
                        result.source, description);
                } else {
                    g_logger->warn("[WebUI] 自然语言规则生成失败: 描述=\"{}\", 错误={}",
                        description, result.error);
                }
            }
        } catch (const std::exception& e) {
            nlohmann::json j;
            j["success"] = false;
            j["error"] = e.what();
            res.set_content(j.dump(), "application/json");
            res.status = 400;
        }
    });

    // POST /api/tasks/:id/ignore/append — 将生成的规则追加到现有规则
    m_svr.Post("/api/tasks/:id/ignore/append", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        try {
            size_t idx = std::stoul(req.path_params.at("id"));
            nlohmann::json body = nlohmann::json::parse(req.body);
            std::string new_rules = body.value("rules", "");

            if (new_rules.empty()) {
                res.status = 400;
                res.set_content("{\"success\":false,\"error\":\"规则不能为空\"}", "application/json");
                return;
            }

            std::filesystem::path ignore_path;
            std::string existing_content;
            {
                std::lock_guard<std::mutex> lock(m_config_mutex);
                if (idx < m_config.tasks.size()) {
                    ignore_path = Utf8ToPath(m_config.tasks[idx].sync_folder) / ".veritasignore";
                    // 读取现有内容
                    if (std::filesystem::exists(ignore_path)) {
                        std::ifstream f(ignore_path);
                        if (f.good()) {
                            std::ostringstream oss;
                            oss << f.rdbuf();
                            existing_content = oss.str();
                        }
                    }
                } else {
                    res.status = 404;
                    res.set_content("{\"success\":false,\"error\":\"任务不存在\"}", "application/json");
                    return;
                }
            }

            if (!ignore_path.empty()) {
                // 构建追加后的完整内容
                std::string final_content = existing_content;
                if (!final_content.empty() && final_content.back() != '\n') {
                    final_content += "\n";
                }
                if (!final_content.empty()) {
                    final_content += "\n# --- 以下规则由自然语言生成 ---\n";
                }
                final_content += new_rules;
                if (final_content.back() != '\n') {
                    final_content += "\n";
                }

                std::ofstream f(ignore_path);
                if (f.good()) {
                    f << final_content;
                    f.close();
                    nlohmann::json j;
                    j["success"] = true;
                    j["content"] = final_content;
                    res.set_content(j.dump(), "application/json");
                    if (g_logger) g_logger->info("[WebUI] 已追加自然语言生成的忽略规则: {}", PathToUtf8(ignore_path));
                } else {
                    res.status = 500;
                    res.set_content("{\"success\":false,\"error\":\"无法写入文件\"}", "application/json");
                }
            }
        } catch (const std::exception& e) {
            nlohmann::json j;
            j["success"] = false;
            j["error"] = e.what();
            res.set_content(j.dump(), "application/json");
            res.status = 400;
        }
    });

    // GET /api/nl-filter/status — 查询自然语言生成器状态
    m_svr.Get("/api/nl-filter/status", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        nlohmann::json j;
        j["success"] = true;
        j["template_engine"] = true;  // 模板引擎始终可用
        j["llm_available"] = m_nl_filter.has_llm();
        res.set_content(j.dump(), "application/json");
    });
}

bool WebUIServer::save_config_internal() {
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

std::filesystem::path WebUIServer::get_exe_dir() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    if (GetModuleFileNameW(NULL, buffer, MAX_PATH) > 0) {
        return std::filesystem::path(buffer).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

std::string WebUIServer::get_index_html() {
    std::filesystem::path exe_dir = get_exe_dir();

    std::vector<std::filesystem::path> try_paths;
    try_paths.push_back(exe_dir / "web" / "index.html");
    try_paths.push_back("web/index.html");
    try_paths.push_back("src/web/index.html");
    try_paths.push_back("../src/web/index.html");

    for (const auto& path : try_paths) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec) && !ec) {
            std::ifstream t(Utf8ToPath(PathToUtf8(path)));
            if (t.is_open()) {
                if (g_logger) g_logger->info("[WebUI] Loaded UI from: {}", PathToUtf8(path));
                std::stringstream buffer;
                buffer << t.rdbuf();
                return buffer.str();
            }
        }
    }

    return R"(<html><body><h1 style="color:red;text-align:center;margin-top:50px">404 - Web UI Not Found</h1></body></html>)";
}

std::string WebUIServer::tail_log(const std::string& file, std::size_t max_bytes) {
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

void WebUIServer::restart_application() {
    if (g_logger) g_logger->info("[WebUI] 正在重启...");
    std::this_thread::sleep_for(std::chrono::milliseconds(RESTART_DELAY_MS));
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

}  // namespace VeritasSync
