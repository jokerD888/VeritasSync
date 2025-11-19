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

#include <atomic>
#include <boost/asio.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <thread>

#include "VeritasSync/Config.h"
#include "VeritasSync/Logger.h"

namespace VeritasSync {

class WebUIServer {
public:
    WebUIServer(boost::asio::io_context& ioc, unsigned short port, const std::string& config_path)
        : m_ioc(ioc),
          m_acceptor(ioc, boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port)),
          m_config_path(config_path),
          m_port(port) {
        try {
            m_absolute_config_path = std::filesystem::absolute(m_config_path).string();
        } catch (...) {
            m_absolute_config_path = m_config_path;
        }
        reload_config();
    }
    using StatusProvider = std::function<std::vector<nlohmann::json>()>;
    void set_status_provider(StatusProvider provider) { m_status_provider = provider; }
    void start() { do_accept(); }
    unsigned short get_port() const { return m_port; }

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

    // 公开的保存接口：自动加锁
    bool save_config() {
        std::lock_guard<std::mutex> lock(m_config_mutex);
        return save_config_internal();
    }

private:
    StatusProvider m_status_provider;
    // --- 内部无锁保存逻辑 (防止死锁) ---
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
            if (SUCCEEDED(pfd->GetOptions(&dwOptions))) {
                pfd->SetOptions(dwOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
            }
            if (SUCCEEDED(pfd->Show(NULL))) {
                IShellItem* psi;
                if (SUCCEEDED(pfd->GetResult(&psi))) {
                    PWSTR pszPath;
                    if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
                        int len = WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, NULL, 0, NULL, NULL);
                        if (len > 0) {
                            path.resize(len - 1);
                            WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, &path[0], len, NULL, NULL);
                        }
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

    void do_accept() {
        m_acceptor.async_accept([this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
            if (!ec) handle_session(std::move(socket));
            do_accept();
        });
    }

    static std::string read_request(boost::asio::ip::tcp::socket& socket) {
        boost::asio::streambuf buf;
        boost::system::error_code ec;
        boost::asio::read_until(socket, buf, "\r\n\r\n", ec);
        if (ec && ec != boost::asio::error::eof) return {};

        std::istream is(&buf);
        std::string header, line;
        std::stringstream header_ss;
        size_t content_length = 0;
        while (std::getline(is, line) && line != "\r") {
            header_ss << line << "\n";
            if (line.find("Content-Length:") == 0) {
                try {
                    content_length = std::stoul(line.substr(15));
                } catch (...) {
                }
            }
        }
        header = header_ss.str();

        std::string body;
        if (content_length > 0) {
            body.resize(content_length);
            size_t bytes_in_buf = buf.size();
            if (bytes_in_buf > 0) buf.sgetn(&body[0], std::min(bytes_in_buf, content_length));
            size_t read_so_far = std::min(bytes_in_buf, content_length);
            size_t remaining = content_length - read_so_far;
            if (remaining > 0) {
                boost::asio::read(socket, boost::asio::buffer(&body[read_so_far], remaining), ec);
            }
        }
        return header + "\r\n\r\n" + body;
    }

    static std::pair<std::string, std::string> parse_request(const std::string& req) {
        auto pos = req.find("\r\n");
        std::string first_line = pos == std::string::npos ? req : req.substr(0, pos);
        std::istringstream iss(first_line);
        std::string method, path, ver;
        iss >> method >> path >> ver;
        auto body_pos = req.find("\r\n\r\n");
        std::string body = body_pos != std::string::npos ? req.substr(body_pos + 4) : "";
        return {path.empty() ? "/" : path, body};
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

    static void write_response(boost::asio::ip::tcp::socket& socket, const std::string& status, const std::string& type,
                               const std::string& body) {
        std::ostringstream oss;
        oss << status << "\r\nContent-Type: " << type << "\r\nContent-Length: " << body.size()
            << "\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n"
            << body;
        boost::system::error_code ec;
        boost::asio::write(socket, boost::asio::buffer(oss.str()), ec);
    }

    std::string build_html() {
        std::lock_guard<std::mutex> lock(m_config_mutex);
        std::ostringstream html;
        html << R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>VeritasSync 控制台</title>
<style>
:root{--primary:#2563eb;--bg:#f3f4f6;--surface:#fff;--text:#111827;--sub:#6b7280;--border:#e5e7eb;--danger:#ef4444;--success:#10b981;}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Inter',system-ui,sans-serif;background:var(--bg);color:var(--text);height:100vh;display:flex}
.sidebar{width:260px;background:var(--surface);border-right:1px solid var(--border);padding:1.5rem;display:flex;flex-direction:column;position:fixed;height:100%;z-index:10}
.brand{font-size:1.25rem;font-weight:700;color:var(--primary);margin-bottom:2rem;display:flex;align-items:center;gap:8px}
.nav-item{padding:0.75rem 1rem;border-radius:0.5rem;color:var(--sub);cursor:pointer;margin-bottom:0.5rem;font-weight:500;transition:0.2s}
.nav-item:hover,.nav-item.active{background:#eff6ff;color:var(--primary)}
.main{flex:1;margin-left:260px;padding:2rem;overflow-y:auto;height:100vh}
.header{display:flex;justify-content:space-between;align-items:center;margin-bottom:2rem}
.page-title{font-size:1.5rem;font-weight:600}
.section-hidden{display:none!important}
.card{background:var(--surface);border-radius:0.75rem;padding:1.5rem;border:1px solid var(--border);box-shadow:0 1px 2px rgba(0,0,0,0.05);margin-bottom:1.5rem}
.status-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:1.5rem;margin-bottom:2rem}
.stat-val{font-size:1.5rem;font-weight:700;margin-top:0.5rem;display:flex;align-items:center;gap:8px}
.dot{width:10px;height:10px;border-radius:50%;background:var(--success);box-shadow:0 0 0 2px #d1fae5}
.task-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(340px,1fr));gap:1.5rem}
.task-card{background:var(--surface);border-radius:0.75rem;border:1px solid var(--border);overflow:hidden;transition:0.2s;display:flex;flex-direction:column}
.task-card:hover{transform:translateY(-2px);box-shadow:0 10px 15px -3px rgba(0,0,0,0.1)}
.task-header{padding:1rem;background:#f9fafb;border-bottom:1px solid var(--border);display:flex;justify-content:space-between;align-items:center;font-weight:600}
.role{padding:2px 8px;border-radius:99px;font-size:0.7rem;text-transform:uppercase}
.role-src{background:#dbeafe;color:#1e40af}.role-dst{background:#e0e7ff;color:#3730a3}
.task-body{padding:1rem;flex:1}
.info-item{margin-bottom:0.8rem}
.info-label{font-size:0.75rem;color:var(--sub);margin-bottom:2px}
.info-val{font-family:monospace;background:#f3f4f6;padding:2px 6px;border-radius:4px;font-size:0.85rem;word-break:break-all;cursor:pointer}
.info-val:hover{background:#e5e7eb}
.task-footer{padding:0.8rem 1rem;border-top:1px solid var(--border);display:flex;gap:0.5rem}
.btn{padding:0.5rem 1rem;border-radius:0.5rem;font-size:0.875rem;font-weight:500;cursor:pointer;border:1px solid var(--border);background:white;transition:0.2s;display:inline-flex;align-items:center;gap:4px;justify-content:center;white-space:nowrap}
.btn:hover{background:#f9fafb;border-color:#d1d5db}
.btn:disabled{opacity:0.6;cursor:not-allowed}
.btn-primary{background:var(--primary);color:white;border:none}
.btn-primary:hover{background:#1d4ed8}
.btn-danger{background:#fff1f2;color:var(--danger);border-color:#fecdd3}
.btn-danger:hover{background:#fee2e2}
.form-input{width:100%;padding:0.6rem;border:1px solid var(--border);border-radius:0.5rem;margin-top:4px}
.form-group{margin-bottom:1rem}
.input-group{display:flex;gap:8px}
.log-container{background:#1e1e1e;color:#e5e7eb;padding:1rem;border-radius:0.5rem;font-family:monospace;font-size:13px;height:65vh;overflow-y:auto;border:1px solid #374151;white-space:pre-wrap}
.log-toolbar{display:flex;gap:10px;align-items:center}
.modal{display:none;position:fixed;inset:0;background:rgba(0,0,0,0.4);z-index:50;justify-content:center;align-items:center;backdrop-filter:blur(2px)}
.modal.open{display:flex}
.modal-box{background:white;padding:2rem;border-radius:1rem;width:480px;max-width:90%;box-shadow:0 20px 25px -5px rgba(0,0,0,0.1)}
.overlay{position:fixed;inset:0;background:rgba(255,255,255,0.9);z-index:999;display:flex;justify-content:center;align-items:center;flex-direction:column;display:none}
.overlay.active{display:flex}
.spinner{width:40px;height:40px;border:4px solid #f3f3f3;border-top:4px solid var(--primary);border-radius:50%;animation:spin 1s linear infinite;margin-bottom:1rem}
@keyframes spin{0%{transform:rotate(0deg)}100%{transform:rotate(360deg)}}
</style>
</head>
<body>
<div class="overlay" id="restart-overlay">
    <div class="spinner"></div><h2>正在重启应用程序...</h2><p style="color:var(--sub)">请稍候，页面将自动刷新</p>
</div>
<div class="sidebar">
    <div class="brand">⚡ VeritasSync</div>
    <div class="nav-item active" onclick="switchTab('dashboard', this)">📊 总览仪表盘</div>
    <div class="nav-item" onclick="switchTab('settings', this)">⚙️ 全局设置</div>
    <div class="nav-item" onclick="switchTab('logs', this)">📜 系统日志</div>
</div>
<div class="main">
    <div id="view-dashboard" class="view-section">
        <div class="header"><h1 class="page-title">任务管理</h1><button class="btn btn-primary" onclick="openModal()">+ 新建任务</button></div>
        <div class="status-grid">
            <div class="card"><div style="color:var(--sub);font-size:0.875rem">Tracker</div><div class="stat-val"><div class="dot"></div> 在线</div><div style="font-size:12px;color:var(--sub);margin-top:4px">)HTML"
             << m_config.tracker_host << ":" << m_config.tracker_port << R"HTML(</div></div>
            <div class="card"><div style="color:var(--sub);font-size:0.875rem">活跃任务</div><div class="stat-val" id="task-count">0</div></div>
        </div>
        <div class="card">
            <h3 style="margin-bottom:1rem">🚀 正在传输</h3>
            <div id="transfer-container" style="color:#666;font-size:0.9rem">暂无活跃传输</div>
        </div>
        <h3 style="margin-bottom:1rem">我的同步目录</h3><div class="task-grid" id="task-container"></div>
    </div>
    <div id="view-settings" class="view-section section-hidden">
        <div class="header"><h1 class="page-title">全局配置</h1><div style="display:flex;gap:10px"><button class="btn" onclick="restartApp()" id="btnRestart">🔄 重启应用</button><button class="btn btn-primary" onclick="saveSettings()" id="btnSave">💾 保存设置</button></div></div>
        <div class="card" style="max-width:600px">
            <h3 style="margin-bottom:1.5rem">连接设置</h3>
            <div class="form-group"><label>Tracker IP 地址</label><input id="cfgTrackerHost" class="form-input" value=")HTML"
             << m_config.tracker_host << R"HTML("></div>
            <div class="form-group"><label>Tracker 端口</label><input id="cfgTrackerPort" class="form-input" type="number" value=")HTML"
             << m_config.tracker_port << R"HTML("></div>
            <hr style="margin:1.5rem 0;border:0;border-top:1px solid var(--border)">
            <h3 style="margin-bottom:1.5rem">NAT 穿透 (STUN/TURN)</h3>
            <div class="form-group"><label>STUN 服务器</label><input id="cfgStunHost" class="form-input" value=")HTML"
             << m_config.stun_host << R"HTML("></div>
            <div class="form-group"><label>TURN 服务器 (可选)</label><input id="cfgTurnHost" class="form-input" value=")HTML"
             << m_config.turn_host << R"HTML("></div>
            <div class="form-group"><label>TURN 用户名</label><input id="cfgTurnUser" class="form-input" value=")HTML"
             << m_config.turn_username << R"HTML("></div>
            <div class="form-group"><label>TURN 密码</label><input id="cfgTurnPass" class="form-input" type="password" value=")HTML"
             << m_config.turn_password << R"HTML("></div>
        </div>
    </div>
    <div id="view-logs" class="view-section section-hidden">
        <div class="header"><h1 class="page-title">运行日志</h1><div class="log-toolbar"><input type="text" id="logFilter" class="form-input" style="width:180px;margin:0;padding:0.4rem" placeholder="🔍 关键词..." onkeyup="renderLog()"><label style="font-size:13px;cursor:pointer;display:flex;align-items:center;gap:4px"><input type="checkbox" id="autoScroll" checked> 自动滚动</label><button class="btn" onclick="fetchGlobalLog()">刷新</button></div></div>
        <div class="log-container" id="global-log-area">加载中...</div>
    </div>
</div>
<div class="modal" id="add-modal">
    <div class="modal-box">
        <h2 style="margin-bottom:1.5rem">新建任务</h2>
        <div class="form-group"><label>Sync Key</label><div class="input-group"><input id="newKey" class="form-input" placeholder="例如：project-01"><button class="btn" onclick="genKey()" style="margin-top:4px">🎲 生成</button></div></div>
        <div class="form-group"><label>角色</label><select id="newRole" class="form-input"><option value="source">Source (发送)</option><option value="destination">Destination (接收)</option></select></div>
        <div class="form-group"><label>路径</label><div class="input-group"><input id="newFolder" class="form-input"><button class="btn" onclick="pickFolder()" style="margin-top:4px">📂 浏览</button></div></div>
        <div style="display:flex;justify-content:flex-end;gap:10px;margin-top:2rem"><button class="btn" onclick="closeModal()">取消</button><button class="btn btn-primary" onclick="addTask()">创建</button></div>
    </div>
</div>
<div class="modal" id="log-modal">
    <div class="modal-box" style="width:800px">
        <div style="display:flex;justify-content:space-between;margin-bottom:1rem"><h3>任务日志</h3><button class="btn" onclick="closeLogModal()">关闭</button></div>
        <div class="log-container" id="task-log-content" style="height:400px"></div>
    </div>
</div>
<script>
    let rawLog="";
    const id=x=>document.getElementById(x);
    function switchTab(i,e){document.querySelectorAll('.view-section').forEach(x=>x.classList.add('section-hidden'));id('view-'+i).classList.remove('section-hidden');document.querySelectorAll('.nav-item').forEach(x=>x.classList.remove('active'));e.classList.add('active');if(i==='dashboard')loadTasks();if(i==='logs')fetchGlobalLog();}
    async function loadTasks(){try{const r=await fetch('/api/config');const c=await r.json();id('task-count').innerText=c.tasks.length;id('task-container').innerHTML=c.tasks.map((t,i)=>`
        <div class="task-card"><div class="task-header"><span>📁 ${t.sync_folder.split(/[\\/]/).pop()||'Root'}</span><span class="role ${t.role==='source'?'role-src':'role-dst'}">${t.role}</span></div>
        <div class="task-body"><div class="info-item"><div class="info-label">KEY</div><div class="info-val" onclick="copy('${t.sync_key}')">${t.sync_key}</div></div><div class="info-item"><div class="info-label">PATH</div><div class="info-val" title="${t.sync_folder}">${t.sync_folder}</div></div></div>
        <div class="task-footer"><button class="btn btn-primary" style="flex:1" onclick="openFolder(${i})">📂 打开</button><button class="btn" style="flex:1" onclick="viewTaskLog(${i})">📄 日志</button><button class="btn btn-danger" onclick="delTask(${i})">🗑</button></div></div>`).join('')||'<div style="padding:2rem;text-align:center;color:#999">暂无任务</div>';}catch(e){console.error(e)}}
    async function saveSettings(){const b=id('btnSave');b.disabled=true;b.innerText="保存中...";const d={tracker_host:id('cfgTrackerHost').value,tracker_port:parseInt(id('cfgTrackerPort').value||9988),stun_host:id('cfgStunHost').value,turn_host:id('cfgTurnHost').value,turn_username:id('cfgTurnUser').value,turn_password:id('cfgTurnPass').value};try{const r=await fetch('/api/config',{method:'POST',body:JSON.stringify(d)});if(r.ok)alert("已保存，请重启");else alert("保存失败");}catch(e){alert("网络错误");}b.disabled=false;b.innerText="💾 保存设置";}
    async function restartApp(){if(!confirm("确定要重启吗？"))return;id('restart-overlay').classList.add('active');try{fetch('/api/restart',{method:'POST'});}catch(e){}let c=0;const t=setInterval(async()=>{c++;try{const ctl=new AbortController();setTimeout(()=>ctl.abort(),2000);const r=await fetch('/api/config',{signal:ctl.signal});if(r.ok){clearInterval(t);location.reload();}}catch(e){if(c>30){clearInterval(t);location.reload();}}},2000);}
    function genKey(){const c='abcdefghijklmnopqrstuvwxyz0123456789',l=c.length;let k='sync-';for(let i=0;i<8;i++)k+=c.charAt(Math.floor(Math.random()*l));id('newKey').value=k;}
    function closeLogModal(){id('log-modal').classList.remove('open');}
    async function openFolder(i){try{const r=await fetch(`/api/tasks/${i}/open`,{method:'POST'});if(!r.ok)alert('打开失败');}catch(e){}}
    async function pickFolder(){const b=event.target;b.disabled=true;try{const r=await fetch('/api/utils/pick_folder',{method:'POST'});const d=await r.json();if(d.path)id('newFolder').value=d.path;}catch(e){}b.disabled=false;}
    async function addTask(){const k=id('newKey').value,r=id('newRole').value,f=id('newFolder').value;if(!k||!f)return alert('不完整');await fetch('/api/tasks',{method:'POST',body:JSON.stringify({sync_key:k,role:r,sync_folder:f})});closeModal();location.reload();}
    async function delTask(i){if(confirm('删除?')){await fetch(`/api/tasks/${i}`,{method:'DELETE'});location.reload();}}
    async function fetchGlobalLog(){const r=await fetch('/api/log');rawLog=await r.text();renderLog();}
    function renderLog(){const f=id('logFilter').value.toLowerCase(),e=id('global-log-area');e.textContent=f?rawLog.split('\n').filter(l=>l.toLowerCase().includes(f)).join('\n'):rawLog;if(id('autoScroll').checked)e.scrollTop=e.scrollHeight;}
    async function viewTaskLog(i){const r=await fetch(`/api/tasks/${i}/log`);id('task-log-content').innerText=await r.text();id('log-modal').classList.add('open');}
    function copy(t){navigator.clipboard.writeText(t).then(()=>alert('已复制'));}
    function openModal(){id('add-modal').classList.add('open');}
    function closeModal(){id('add-modal').classList.remove('open');}
    const fi=id('newFolder');fi.addEventListener('dragover',e=>e.preventDefault());fi.addEventListener('drop',e=>{e.preventDefault();if(e.dataTransfer.files[0])fi.value=(e.dataTransfer.files[0].path||e.dataTransfer.files[0].name).replace(/\\/g,'/');});
    setInterval(()=>{if(!id('view-logs').classList.contains('section-hidden')&&id('autoScroll').checked)fetchGlobalLog()},3000);
    loadTasks();
    setInterval(async () => {
            try {
                const r = await fetch('/api/status');
                const data = await r.json();
                const el = id('transfer-container');
                if (data.length === 0) {
                    el.innerHTML = '暂无活跃传输';
                } else {
                    el.innerHTML = data.map(d => `
                        <div style="margin-bottom:10px">
                            <div style="display:flex;justify-content:space-between;margin-bottom:4px">
                                <span>${d.path}</span>
                                <span style="font-weight:bold;color:var(--primary)">${d.progress.toFixed(1)}%</span>
                            </div>
                            <div style="background:#e5e7eb;height:6px;border-radius:3px;overflow:hidden">
                                <div style="background:var(--primary);height:100%;width:${d.progress}%"></div>
                            </div>
                            <div style="font-size:12px;color:#999;margin-top:2px">
                                任务: ${d.key} | 块: ${d.recv}/${d.total}
                            </div>
                        </div>
                    `).join('');
                }
            } catch(e){}
        }, 1000);
</script>
</body>
</html>)HTML";
        return html.str();
    }

    void handle_session(boost::asio::ip::tcp::socket socket) {
        try {
            auto req = read_request(socket);
            auto [path, body] = parse_request(req);

            if (path == "/") {
                write_response(socket, "HTTP/1.1 200 OK", "text/html; charset=utf-8", build_html());
            } else if (path == "/api/status") {
                nlohmann::json j = nlohmann::json::array();
                if (m_status_provider) {
                    auto status_list = m_status_provider();
                    for (const auto& item : status_list) j.push_back(item);
                }
                write_response(socket, "HTTP/1.1 200 OK", "application/json", j.dump());
            } else if (path == "/api/log") {
                write_response(socket, "HTTP/1.1 200 OK", "text/plain; charset=utf-8",
                               tail_log("veritas_sync.log", 16384));
            } else if (path == "/api/config") {
                // 这里加锁，但后续调用无锁的 save_config_internal
                std::lock_guard<std::mutex> lock(m_config_mutex);

                if (req.find("POST") == 0) {
                    try {
                        if (body.empty()) throw std::runtime_error("Empty body");
                        auto j = nlohmann::json::parse(body);
                        if (j.contains("tracker_host"))
                            m_config.tracker_host = j.value("tracker_host", m_config.tracker_host);
                        if (j.contains("tracker_port"))
                            m_config.tracker_port = j.value("tracker_port", m_config.tracker_port);
                        if (j.contains("stun_host")) m_config.stun_host = j.value("stun_host", m_config.stun_host);
                        if (j.contains("turn_host")) m_config.turn_host = j.value("turn_host", m_config.turn_host);
                        if (j.contains("turn_username"))
                            m_config.turn_username = j.value("turn_username", m_config.turn_username);
                        if (j.contains("turn_password"))
                            m_config.turn_password = j.value("turn_password", m_config.turn_password);

                        // 使用 internal 版本，避免死锁
                        if (save_config_internal())
                            write_response(socket, "HTTP/1.1 200 OK", "application/json", "{\"success\":true}");
                        else
                            write_response(socket, "HTTP/1.1 500 Error", "application/json",
                                           "{\"error\":\"Save failed\"}");
                    } catch (const std::exception& e) {
                        if (g_logger) g_logger->error("[WebUI] Config POST Error: {}", e.what());
                        write_response(socket, "HTTP/1.1 400 Bad Request", "application/json",
                                       "{\"error\":\"Invalid JSON\"}");
                    }
                } else {
                    write_response(socket, "HTTP/1.1 200 OK", "application/json; charset=utf-8",
                                   nlohmann::json(m_config).dump(2));
                }
            } else if (path == "/api/restart" && req.find("POST") == 0) {
                write_response(socket, "HTTP/1.1 200 OK", "application/json", "{\"success\":true}");
                std::thread([]() { restart_application(); }).detach();
            } else if (path == "/api/tasks" && req.find("POST") == 0) {
                try {
                    if (body.empty()) throw std::runtime_error("Empty body");
                    auto j = nlohmann::json::parse(body);
                    SyncTask task;
                    task.sync_key = j.at("sync_key").get<std::string>();
                    task.role = j.at("role").get<std::string>();
                    task.sync_folder = j.at("sync_folder").get<std::string>();
                    {
                        std::lock_guard<std::mutex> lock(m_config_mutex);
                        m_config.tasks.push_back(task);
                        // 锁内调用 internal
                        if (save_config_internal())
                            write_response(socket, "HTTP/1.1 200 OK", "application/json", "{\"success\":true}");
                        else
                            write_response(socket, "HTTP/1.1 500 Error", "application/json", "{\"success\":false}");
                    }
                } catch (...) {
                    write_response(socket, "HTTP/1.1 400 Bad Request", "application/json", "{\"success\":false}");
                }
            } else if (path.find("/api/tasks/") == 0 && path.find("/log") != std::string::npos) {
                try {
                    size_t idx = std::stoul(path.substr(11, path.find("/log") - 11));
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
                    write_response(socket, "HTTP/1.1 200 OK", "text/plain; charset=utf-8", log_content);
                } catch (...) {
                    write_response(socket, "HTTP/1.1 400 Bad Request", "text/plain", "Error");
                }
            } else if (path.find("/api/tasks/") == 0 && path.find("/open") != std::string::npos &&
                       req.find("POST") == 0) {
                try {
                    size_t idx = std::stoul(path.substr(11, path.find("/open") - 11));
                    std::string p;
                    {
                        std::lock_guard<std::mutex> lock(m_config_mutex);
                        if (idx < m_config.tasks.size()) p = m_config.tasks[idx].sync_folder;
                    }
                    if (!p.empty()) {
                        std::string cmd;
#ifdef _WIN32
                        cmd = "explorer \"" + p + "\"";
#elif defined(__APPLE__)
                        cmd = "open \"" + p + "\"";
#else
                        cmd = "xdg-open \"" + p + "\"";
#endif
                        system(cmd.c_str());
                        write_response(socket, "HTTP/1.1 200 OK", "application/json", "{\"success\":true}");
                    } else
                        write_response(socket, "HTTP/1.1 404 Not Found", "application/json",
                                       "{\"error\":\"Not found\"}");
                } catch (...) {
                    write_response(socket, "HTTP/1.1 500 Error", "application/json", "{\"error\":\"Error\"}");
                }
            } else if (path == "/api/utils/pick_folder" && req.find("POST") == 0) {
                std::string p = pick_folder_dialog();
                if (!p.empty()) {
                    std::replace(p.begin(), p.end(), '\\', '/');
                    nlohmann::json j;
                    j["success"] = true;
                    j["path"] = p;
                    write_response(socket, "HTTP/1.1 200 OK", "application/json", j.dump());
                } else
                    write_response(socket, "HTTP/1.1 200 OK", "application/json", "{\"success\":false}");
            } else if (path.find("/api/tasks/") == 0 && req.find("DELETE") == 0) {
                try {
                    size_t idx = std::stoul(path.substr(11));
                    {
                        std::lock_guard<std::mutex> lock(m_config_mutex);
                        if (idx < m_config.tasks.size()) {
                            m_config.tasks.erase(m_config.tasks.begin() + idx);
                            // 锁内调用 internal
                            if (save_config_internal())
                                write_response(socket, "HTTP/1.1 200 OK", "application/json", "{\"success\":true}");
                            else
                                write_response(socket, "HTTP/1.1 500 Error", "application/json", "{\"success\":false}");
                        }
                    }
                } catch (...) {
                    write_response(socket, "HTTP/1.1 400 Bad Request", "application/json", "{\"success\":false}");
                }
            } else {
                write_response(socket, "HTTP/1.1 404 Not Found", "text/plain", "Not Found");
            }
        } catch (...) {
            // connection error
        }
        boost::system::error_code ec;
        socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        socket.close(ec);
    }

    boost::asio::io_context& m_ioc;
    boost::asio::ip::tcp::acceptor m_acceptor;
    std::string m_config_path;
    std::string m_absolute_config_path;
    unsigned short m_port;
    Config m_config;
    std::mutex m_config_mutex;
};

}  // namespace VeritasSync