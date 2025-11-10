#pragma once

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <mutex>
#include <atomic>
#include "VeritasSync/Config.h"
#include "VeritasSync/Logger.h"

namespace VeritasSync {

// Web UI HTTP 服务器
class WebUIServer {
public:
    WebUIServer(boost::asio::io_context& ioc, unsigned short port, const std::string& config_path)
        : m_ioc(ioc), 
          m_acceptor(ioc, boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port)),
          m_config_path(config_path),
          m_port(port) {
        reload_config();
    }

    void start() { do_accept(); }
    
    unsigned short get_port() const { return m_port; }
    
    // 重新加载配置
    void reload_config() {
        std::lock_guard<std::mutex> lock(m_config_mutex);
        try {
            std::ifstream f(m_config_path);
            if (f.good()) {
                nlohmann::json j;
                f >> j;
                m_config = j.get<Config>();
            }
        } catch (const std::exception& e) {
            if (g_logger) {
                g_logger->error("[WebUI] 加载配置失败: {}", e.what());
            }
        }
    }
    
    // 保存配置到文件
    bool save_config() {
        std::lock_guard<std::mutex> lock(m_config_mutex);
        try {
            std::ofstream o(m_config_path);
            o << nlohmann::json(m_config).dump(4) << std::endl;
            return true;
        } catch (const std::exception& e) {
            if (g_logger) {
                g_logger->error("[WebUI] 保存配置失败: {}", e.what());
            }
            return false;
        }
    }

private:
    void do_accept() {
        m_acceptor.async_accept([this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
            if (!ec) {
                handle_session(std::move(socket));
            }
            do_accept();
        });
    }

    static std::string read_request(boost::asio::ip::tcp::socket& socket) {
        boost::asio::streambuf buf;
        boost::system::error_code ec;
        boost::asio::read_until(socket, buf, "\r\n\r\n", ec);
        if (ec && ec != boost::asio::error::eof) return {};
        
        // 读取请求头
        std::istream is(&buf);
        std::string header;
        std::getline(is, header);
        
        // 继续读取剩余头部
        std::string line;
        size_t content_length = 0;
        while (std::getline(is, line) && line != "\r") {
            if (line.find("Content-Length:") == 0) {
                content_length = std::stoul(line.substr(15));
            }
        }
        
        // 如果有 body，读取 body
        std::string body;
        if (content_length > 0 && buf.size() > 0) {
            std::vector<char> body_buf(content_length);
            is.read(body_buf.data(), std::min(content_length, buf.size()));
            size_t remaining = content_length - buf.size();
            if (remaining > 0) {
                boost::asio::read(socket, boost::asio::buffer(body_buf.data() + buf.size(), remaining), ec);
            }
            body = std::string(body_buf.begin(), body_buf.end());
        }
        
        return header + "\r\n\r\n" + body;
    }

    static std::pair<std::string, std::string> parse_request(const std::string& req) {
        auto pos = req.find("\r\n");
        std::string first_line = pos == std::string::npos ? req : req.substr(0, pos);
        std::istringstream iss(first_line);
        std::string method, path, ver;
        iss >> method >> path >> ver;
        
        // 提取 body
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
        auto len = size - start;
        std::string content(len, '\0');
        ifs.seekg(start, std::ios::beg);
        ifs.read(&content[0], len);
        return content;
    }

    static void write_response(boost::asio::ip::tcp::socket& socket, const std::string& status_line,
                              const std::string& content_type, const std::string& body) {
        std::ostringstream oss;
        oss << status_line << "\r\n"
            << "Content-Type: " << content_type << "\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Connection: close\r\n\r\n"
            << body;
        auto s = oss.str();
        boost::system::error_code ec;
        boost::asio::write(socket, boost::asio::buffer(s), ec);
    }

    std::string build_html() {
        std::lock_guard<std::mutex> lock(m_config_mutex);
        
        std::ostringstream html;
        html << R"(<!doctype html>
<html><head><meta charset='utf-8'><title>VeritasSync 控制台</title>
<style>
body{font-family:system-ui,Segoe UI,Arial;margin:0;padding:20px;background:#f5f5f5}
.container{max-width:1200px;margin:0 auto;background:#fff;padding:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1)}
h2{color:#333;border-bottom:2px solid #4CAF50;padding-bottom:10px}
h3{color:#555;margin-top:30px}
.status{display:inline-block;padding:4px 8px;border-radius:4px;font-size:12px;font-weight:bold}
.status.source{background:#4CAF50;color:#fff}
.status.destination{background:#2196F3;color:#fff}
table{width:100%;border-collapse:collapse;margin:15px 0}
th,td{border:1px solid #ddd;padding:12px;text-align:left}
th{background:#f0f0f0;font-weight:600}
tr:hover{background:#f9f9f9}
code{background:#eee;padding:2px 6px;border-radius:3px;font-family:Consolas,monospace}
button{background:#4CAF50;color:#fff;border:none;padding:8px 16px;border-radius:4px;cursor:pointer;margin:5px}
button:hover{background:#45a049}
button.delete{background:#f44336}
button.delete:hover{background:#da190b}
input,select{padding:8px;border:1px solid #ddd;border-radius:4px}
.form-group{margin:15px 0;display:flex;align-items:center;gap:10px}
label{min-width:100px;font-weight:600}
input[type="text"]{flex:1;min-width:200px}
select{min-width:150px}
#log{max-height:400px;overflow:auto;background:#1e1e1e;color:#d4d4d4;padding:15px;border-radius:4px;font-family:Consolas,monospace;font-size:13px;line-height:1.5;display:none}
#log.expanded{display:block}
.log-header{cursor:pointer;user-select:none;display:flex;align-items:center;gap:8px}
.log-header:hover{color:#4CAF50}
.toggle-icon{transition:transform 0.2s}
.toggle-icon.expanded{transform:rotate(90deg)}
.info-box{background:#e3f2fd;border-left:4px solid #2196F3;padding:12px;margin:15px 0;border-radius:4px}
</style>
</head><body>
<div class='container'>
<h2>🔄 VeritasSync 控制台</h2>
<div class='info-box'>
<strong>Tracker:</strong> )";
        html << m_config.tracker_host << ":" << m_config.tracker_port;
        if (!m_config.turn_host.empty()) {
            html << " | <strong>TURN:</strong> " << m_config.turn_host << ":" << m_config.turn_port;
        }
        html << R"(
</div>

<h3>同步任务</h3>
<table id='tasks'>
<tr><th>Sync Key</th><th>角色</th><th>同步文件夹</th><th>日志</th><th>操作</th></tr>)";

        for (size_t i = 0; i < m_config.tasks.size(); ++i) {
            const auto& t = m_config.tasks[i];
            html << "<tr><td><code>" << t.sync_key << "</code></td>"
                 << "<td><span class='status " << t.role << "'>" << t.role << "</span></td>"
                 << "<td>" << t.sync_folder << "</td>"
                 << "<td><button onclick='viewLog(" << i << ")'>查看日志</button></td>"
                 << "<td><button class='delete' onclick='deleteTask(" << i << ")'>删除</button></td></tr>";
        }

        html << R"(</table>

<h3>添加新任务</h3>
<div class='form-group'>
<label>Sync Key:</label><input type='text' id='newKey' placeholder='my-sync-key'/>
</div>
<div class='form-group'>
<label>角色:</label><select id='newRole'>
<option value='source'>Source</option>
<option value='destination'>Destination</option>
</select>
</div>
<div class='form-group'>
<label>文件夹:</label>
<input type='text' id='newFolder' placeholder='D:\\MyFolder 或拖放文件夹到此处'/>
<button onclick='addTask()'>添加任务</button>
</div>

<div id='logModal' style='display:none;position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,0.5);z-index:1000' onclick='closeLog()'>
<div style='background:#fff;margin:50px auto;max-width:900px;max-height:80vh;overflow:auto;border-radius:8px;padding:20px' onclick='event.stopPropagation()'>
<h3 id='logTitle'>任务日志</h3>
<pre id='taskLog' style='background:#1e1e1e;color:#d4d4d4;padding:15px;border-radius:4px;max-height:500px;overflow:auto'></pre>
<button onclick='closeLog()'>关闭</button>
</div>
</div>

<h3 class='log-header' onclick='toggleLog()'>
<span class='toggle-icon' id='logToggle'>▶</span>
全局日志
</h3>
<pre id='log'>加载中...</pre>

<script>
function toggleLog(){
 const log=document.getElementById('log');
 const icon=document.getElementById('logToggle');
 log.classList.toggle('expanded');
 icon.classList.toggle('expanded');
 icon.textContent=log.classList.contains('expanded')?'▼':'▶';
}
async function loadLog(){
 try{
  let r=await fetch('/api/log');
  document.getElementById('log').textContent=await r.text();
 }catch(e){console.error(e)}
}
async function viewLog(idx){
 try{
  let r=await fetch('/api/tasks/'+idx+'/log');
  let log=await r.text();
  document.getElementById('logTitle').textContent='任务 #'+idx+' 日志';
  document.getElementById('taskLog').textContent=log||'暂无日志';
  document.getElementById('logModal').style.display='block';
 }catch(e){alert('加载失败:'+e)}
}
function closeLog(){
 document.getElementById('logModal').style.display='none';
}

// 支持拖放文件夹
const folderInput=document.getElementById('newFolder');
folderInput.addEventListener('dragover',e=>{e.preventDefault();e.dataTransfer.dropEffect='copy'});
folderInput.addEventListener('drop',e=>{
 e.preventDefault();
 const files=e.dataTransfer.files;
 if(files.length>0){
  let path=files[0].path||files[0].webkitRelativePath||files[0].name;
  // 尝试获取完整路径
  if(path){
   folderInput.value=path.replace(/\\/g,'/');
  }
 }
});

async function addTask(){
 let key=document.getElementById('newKey').value;
 let role=document.getElementById('newRole').value;
 let folder=document.getElementById('newFolder').value;
 if(!key||!folder){alert('请填写完整信息');return}
 try{
  let r=await fetch('/api/tasks',{
   method:'POST',
   headers:{'Content-Type':'application/json'},
   body:JSON.stringify({sync_key:key,role:role,sync_folder:folder})
  });
  if(r.ok){alert('添加成功，请重启程序以应用更改');location.reload()}
  else alert('添加失败')
 }catch(e){alert('错误:'+e)}
}
async function deleteTask(idx){
 if(!confirm('确定删除此任务?'))return;
 try{
  let r=await fetch('/api/tasks/'+idx,{method:'DELETE'});
  if(r.ok){alert('删除成功，请重启程序以应用更改');location.reload()}
  else alert('删除失败')
 }catch(e){alert('错误:'+e)}
}
loadLog();
setInterval(loadLog,3000);
</script>
</div>
</body></html>)";
        
        return html.str();
    }

    void handle_session(boost::asio::ip::tcp::socket socket) {
        try {
            auto req = read_request(socket);
            auto [path, body] = parse_request(req);

            if (path == "/") {
                write_response(socket, "HTTP/1.1 200 OK", "text/html; charset=utf-8", build_html());
            } 
            else if (path == "/api/log") {
                auto log_content = tail_log("veritas_sync.log", 16384);
                write_response(socket, "HTTP/1.1 200 OK", "text/plain; charset=utf-8", log_content);
            }
            else if (path == "/api/config") {
                std::lock_guard<std::mutex> lock(m_config_mutex);
                write_response(socket, "HTTP/1.1 200 OK", "application/json; charset=utf-8", 
                             nlohmann::json(m_config).dump(2));
            }
            else if (path == "/api/tasks" && req.find("POST") == 0) {
                try {
                    auto j = nlohmann::json::parse(body);
                    SyncTask task;
                    task.sync_key = j.at("sync_key").get<std::string>();
                    task.role = j.at("role").get<std::string>();
                    task.sync_folder = j.at("sync_folder").get<std::string>();
                    
                    {
                        std::lock_guard<std::mutex> lock(m_config_mutex);
                        m_config.tasks.push_back(task);
                    }
                    
                    if (save_config()) {
                        write_response(socket, "HTTP/1.1 200 OK", "application/json", "{\"success\":true}");
                    } else {
                        write_response(socket, "HTTP/1.1 500 Internal Server Error", "application/json", 
                                     "{\"success\":false,\"error\":\"保存失败\"}");
                    }
                } catch (const std::exception& e) {
                    write_response(socket, "HTTP/1.1 400 Bad Request", "application/json", 
                                 "{\"success\":false,\"error\":\"" + std::string(e.what()) + "\"}");
                }
            }
            else if (path.find("/api/tasks/") == 0 && path.find("/log") != std::string::npos) {
                // GET /api/tasks/0/log
                try {
                    size_t idx_end = path.find("/log");
                    size_t idx = std::stoul(path.substr(11, idx_end - 11));
                    
                    std::string task_log_content;
                    {
                        std::lock_guard<std::mutex> lock(m_config_mutex);
                        if (idx < m_config.tasks.size()) {
                            // 根据 sync_key 查找对应的日志
                            const auto& task = m_config.tasks[idx];
                            task_log_content = tail_log("veritas_sync.log", 32768);
                            
                            // 过滤出包含该任务 sync_key 的日志行
                            std::ostringstream filtered;
                            std::istringstream iss(task_log_content);
                            std::string line;
                            while (std::getline(iss, line)) {
                                if (line.find(task.sync_key) != std::string::npos ||
                                    line.find(task.sync_folder) != std::string::npos) {
                                    filtered << line << "\n";
                                }
                            }
                            task_log_content = filtered.str();
                            if (task_log_content.empty()) {
                                task_log_content = "暂无相关日志";
                            }
                        } else {
                            task_log_content = "任务不存在";
                        }
                    }
                    write_response(socket, "HTTP/1.1 200 OK", "text/plain; charset=utf-8", task_log_content);
                } catch (const std::exception& e) {
                    write_response(socket, "HTTP/1.1 400 Bad Request", "text/plain", std::string("错误: ") + e.what());
                }
            }
            else if (path.find("/api/tasks/") == 0 && req.find("DELETE") == 0) {
                try {
                    size_t idx = std::stoul(path.substr(11));
                    {
                        std::lock_guard<std::mutex> lock(m_config_mutex);
                        if (idx < m_config.tasks.size()) {
                            m_config.tasks.erase(m_config.tasks.begin() + idx);
                        } else {
                            throw std::out_of_range("索引超出范围");
                        }
                    }
                    
                    if (save_config()) {
                        write_response(socket, "HTTP/1.1 200 OK", "application/json", "{\"success\":true}");
                    } else {
                        write_response(socket, "HTTP/1.1 500 Internal Server Error", "application/json", 
                                     "{\"success\":false,\"error\":\"保存失败\"}");
                    }
                } catch (const std::exception& e) {
                    write_response(socket, "HTTP/1.1 400 Bad Request", "application/json", 
                                 "{\"success\":false,\"error\":\"" + std::string(e.what()) + "\"}");
                }
            }
            else {
                write_response(socket, "HTTP/1.1 404 Not Found", "text/plain", "Not Found");
            }
        } catch (...) {
            // 连接错误直接关闭
        }
        
        boost::system::error_code ec;
        socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        socket.close(ec);
    }

    boost::asio::io_context& m_ioc;
    boost::asio::ip::tcp::acceptor m_acceptor;
    std::string m_config_path;
    unsigned short m_port;
    Config m_config;
    std::mutex m_config_mutex;
};

} // namespace VeritasSync
