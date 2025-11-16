/**
 * LibJuice NAT穿透详细测试程序
 * 功能：
 * 1. 检测本地NAT类型
 * 2. 详细输出ICE候选地址收集过程
 * 3. 展示完整的P2P连接建立流程
 * 4. 通过手动复制SDP完成信令交换
 */

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <cstring>
#include <atomic>
#include <juice/juice.h>

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

using namespace std;

// 全局变量
static juice_agent_t* agent = nullptr;
static std::atomic<bool> gathering_done(false);
static std::atomic<bool> connected(false);
static std::atomic<int> candidate_count(0);

// NAT类型字符串映射
const char* nat_type_to_string(int type) {
    switch (type) {
        case 0: return "未知(Unknown)";
        case 1: return "开放网络/无NAT(Open Internet)";
        case 2: return "完全锥形NAT(Full Cone NAT) - NAT1";
        case 3: return "地址受限锥形NAT(Address Restricted Cone NAT) - NAT2";
        case 4: return "端口受限锥形NAT(Port Restricted Cone NAT) - NAT3";
        case 5: return "对称NAT(Symmetric NAT) - NAT4";
        default: return "未知类型";
    }
}

// ICE候选类型字符串
const char* candidate_type_to_string(const char* type) {
    if (!type) return "unknown";
    if (strcmp(type, "host") == 0) return "host(本地地址)";
    if (strcmp(type, "srflx") == 0) return "srflx(STUN反射地址)";
    if (strcmp(type, "relay") == 0) return "relay(TURN中继地址)";
    if (strcmp(type, "prflx") == 0) return "prflx(对等反射地址)";
    return type;
}

// ICE状态字符串
const char* state_to_string(juice_state_t state) {
    switch (state) {
        case JUICE_STATE_DISCONNECTED: return "已断开(Disconnected)";
        case JUICE_STATE_GATHERING: return "收集候选地址中(Gathering)";
        case JUICE_STATE_CONNECTING: return "连接中(Connecting)";
        case JUICE_STATE_CONNECTED: return "已连接(Connected)";
        case JUICE_STATE_COMPLETED: return "连接完成(Completed)";
        case JUICE_STATE_FAILED: return "连接失败(Failed)";
        default: return "未知状态";
    }
}

// 回调：状态变化
void on_state_changed(juice_agent_t* agent, juice_state_t state, void* user_ptr) {
    cout << "\n========================================" << endl;
    cout << "📡 ICE状态变化: " << state_to_string(state) << endl;
    cout << "========================================" << endl;

    if (state == JUICE_STATE_CONNECTED) {
        connected = true;
        cout << "✅ P2P连接建立成功！可以开始传输数据。" << endl;
        
        // 获取选中的候选对信息
        char local[256], remote[256];
        if (juice_get_selected_candidates(agent, local, sizeof(local), remote, sizeof(remote)) == 0) {
            cout << "\n🎯 选中的候选地址对:" << endl;
            cout << "   本地: " << local << endl;
            cout << "   远端: " << remote << endl;
        }
    } else if (state == JUICE_STATE_FAILED) {
        cout << "❌ P2P连接失败，可能需要TURN中继服务器。" << endl;
    }
}

// 回调：候选地址收集
void on_candidate(juice_agent_t* agent, const char* sdp, void* user_ptr) {
    candidate_count++;
    
    cout << "[DEBUG] 候选地址回调被触发 #" << candidate_count.load() << endl;
    
    // 解析候选地址类型
    string sdp_str(sdp);
    string type = "unknown";
    
    size_t typ_pos = sdp_str.find("typ ");
    if (typ_pos != string::npos) {
        size_t start = typ_pos + 4;
        size_t end = sdp_str.find(" ", start);
        if (end == string::npos) end = sdp_str.length();
        type = sdp_str.substr(start, end - start);
    }
    
    cout << "\n🔍 候选地址 #" << candidate_count.load() << " [" 
         << candidate_type_to_string(type.c_str()) << "]" << endl;
    cout << "   " << sdp << endl;
}

// 回调：候选地址收集完成
void on_gathering_done(juice_agent_t* agent, void* user_ptr) {
    cout << "[DEBUG] gathering_done 回调被触发" << endl;
    gathering_done = true;
    
    cout << "\n========================================" << endl;
    cout << "✅ ICE候选地址收集完成！" << endl;
    cout << "   共收集到 " << candidate_count.load() << " 个候选地址" << endl;
    cout << "========================================" << endl;
}

// 回调：接收数据
void on_recv(juice_agent_t* agent, const char* data, size_t size, void* user_ptr) {
    string msg(data, size);
    cout << "\n📩 收到消息: " << msg << endl;
}

// 打印帮助信息
void print_usage() {
    cout << "\n╔══════════════════════════════════════════════════════════╗" << endl;
    cout << "║         LibJuice NAT穿透详细测试程序                     ║" << endl;
    cout << "╚══════════════════════════════════════════════════════════╝" << endl;
    cout << "\n请选择运行模式:" << endl;
    cout << "  1 - 作为发起方(Controlling/Offerer)" << endl;
    cout << "  2 - 作为响应方(Controlled/Answerer)" << endl;
    cout << "\n提示：需要在两台不同局域网的电脑上运行，" << endl;
    cout << "      一台选择模式1，另一台选择模式2。" << endl;
}

// 等待用户输入
string get_multiline_input(const string& prompt) {
    cout << prompt << endl;
    cout << "（输入完成后，在新行输入 'END' 结束）" << endl;
    
    string result;
    string line;
    
    while (true) {
        getline(cin, line);
        if (line == "END" || line == "end") {
            break;
        }
        if (!result.empty()) {
            result += "\n";
        }
        result += line;
    }
    
    return result;
}

// 检测NAT类型
void detect_nat_type(const char* stun_server, int stun_port) {
    cout << "\n🔍 开始检测NAT类型..." << endl;
    cout << "   使用STUN服务器: " << stun_server << ":" << stun_port << endl;
    
    // 创建临时agent进行NAT检测
    juice_config_t config;
    memset(&config, 0, sizeof(config));
    
    config.stun_server_host = stun_server;
    config.stun_server_port = stun_port;
    config.cb_state_changed = nullptr;
    config.cb_candidate = nullptr;
    config.cb_gathering_done = nullptr;
    config.cb_recv = nullptr;
    config.user_ptr = nullptr;
    
    juice_agent_t* test_agent = juice_create(&config);
    if (!test_agent) {
        cout << "⚠️  NAT类型检测失败：无法创建测试agent" << endl;
        return;
    }
    
    // 等待STUN绑定完成
    this_thread::sleep_for(chrono::seconds(2));
    
    // 尝试获取NAT类型（注意：libjuice可能不直接提供NAT类型API）
    // 这里我们通过观察候选地址来推断
    cout << "   检测完成（具体NAT类型需要观察候选地址）" << endl;
    cout << "\n💡 NAT类型判断提示：" << endl;
    cout << "   - 如果有 host 候选地址且为公网IP → 无NAT或NAT1" << endl;
    cout << "   - 如果 srflx 地址端口固定 → 锥形NAT (NAT2/3)" << endl;
    cout << "   - 如果每次连接 srflx 端口都变化 → 对称NAT (NAT4)" << endl;
    
    juice_destroy(test_agent);
}

int main() {
#ifdef _WIN32
    // Windows控制台UTF-8支持
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    cout << "[DEBUG] 程序启动" << endl;

    print_usage();
    
    cout << "\n请输入模式 (1 或 2): ";
    int mode;
    cin >> mode;
    cin.ignore();
    
    if (mode != 1 && mode != 2) {
        cout << "❌ 无效的模式选择！" << endl;
        return 1;
    }
    
    bool is_controlling = (mode == 1);
    
    cout << "\n请输入STUN服务器地址 [默认: stun.l.google.com]: ";
    string stun_server;
    getline(cin, stun_server);
    if (stun_server.empty()) {
        stun_server = "stun.l.google.com";
    }
    
    // 询问 STUN 端口
    cout << "请输入STUN端口 [默认: 19302，coturn默认: 3478]: ";
    string stun_port_str;
    getline(cin, stun_port_str);
    int stun_port = 19302;
    if (!stun_port_str.empty()) {
        stun_port = stoi(stun_port_str);
    }
    cout << "[DEBUG] 使用 STUN 服务器: " << stun_server << ":" << stun_port << endl;
    
    cout << "\n⚠️  提示：如果使用自定义STUN服务器，请确保端口为3478或19302" << endl;
    
    cout << "\n是否使用TURN服务器? (y/n) [默认: n]: ";
    string use_turn;
    getline(cin, use_turn);
    
    string turn_server, turn_username, turn_password;
    if (use_turn == "y" || use_turn == "Y") {
        cout << "请输入TURN服务器地址: ";
        getline(cin, turn_server);
        cout << "请输入TURN用户名: ";
        getline(cin, turn_username);
        cout << "请输入TURN密码: ";
        getline(cin, turn_password);
    }
    
    // NAT类型检测
    detect_nat_type(stun_server.c_str(), stun_port);
    
    // 配置ICE agent
    cout << "\n========================================" << endl;
    cout << "🚀 初始化 ICE Agent..." << endl;
    cout << "   角色: " << (is_controlling ? "发起方(Controlling)" : "响应方(Controlled)") << endl;
    cout << "========================================" << endl;
    
    juice_config_t config;
    memset(&config, 0, sizeof(config));
    
    cout << "[DEBUG] 配置 ICE Agent..." << endl;
    
    // 设置并发模式：使用内部线程
    config.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
    
    // 重要：使用静态字符串，避免悬空指针
    static string stun_server_static;
    static string turn_server_static;
    static string turn_username_static;
    static string turn_password_static;
    
    stun_server_static = stun_server;
    config.stun_server_host = stun_server_static.c_str();
    config.stun_server_port = stun_port;
    
    if (!turn_server.empty()) {
        turn_server_static = turn_server;
        turn_username_static = turn_username;
        turn_password_static = turn_password;
        
        config.turn_servers = new juice_turn_server_t[1];
        config.turn_servers_count = 1;
        config.turn_servers[0].host = turn_server_static.c_str();
        config.turn_servers[0].port = 3478;
        config.turn_servers[0].username = turn_username_static.c_str();
        config.turn_servers[0].password = turn_password_static.c_str();
        
        cout << "   TURN服务器: " << turn_server << ":3478" << endl;
    }
    
    config.cb_state_changed = on_state_changed;
    config.cb_candidate = on_candidate;
    config.cb_gathering_done = on_gathering_done;
    config.cb_recv = on_recv;
    config.user_ptr = nullptr;
    
    cout << "[DEBUG] 回调函数已设置" << endl;
    cout << "[DEBUG] cb_candidate = " << (void*)config.cb_candidate << endl;
    cout << "[DEBUG] cb_gathering_done = " << (void*)config.cb_gathering_done << endl;
    
    // 创建agent
    agent = juice_create(&config);
    if (!agent) {
        cout << "❌ 创建ICE Agent失败！" << endl;
        if (config.turn_servers) delete[] config.turn_servers;
        return 1;
    }
    
    cout << "✅ ICE Agent创建成功！" << endl;
    cout << "[DEBUG] agent 指针 = " << (void*)agent << endl;
    
    // 显式启动候选地址收集
    cout << "[DEBUG] 调用 juice_gather_candidates()..." << endl;
    int gather_ret = juice_gather_candidates(agent);
    cout << "[DEBUG] juice_gather_candidates() 返回: " << gather_ret << endl;
    
    if (gather_ret < 0) {
        cout << "⚠️  启动候选地址收集失败，返回值: " << gather_ret << endl;
    }
    
    // 开始收集候选地址
    cout << "\n========================================" << endl;
    cout << "🔍 开始收集ICE候选地址..." << endl;
    cout << "========================================" << endl;
    
    char local_sdp[4096];
    
    if (is_controlling) {
        // 发起方：创建Offer
        if (juice_get_local_description(agent, local_sdp, sizeof(local_sdp)) < 0) {
            cout << "❌ 获取本地SDP失败！" << endl;
            juice_destroy(agent);
            if (config.turn_servers) delete[] config.turn_servers;
            return 1;
        }
    } else {
        // 响应方：先等待对方的Offer
        juice_get_local_description(agent, local_sdp, sizeof(local_sdp));
    }
    
    // 等待候选地址收集完成
    cout << "\n⏳ 等待候选地址收集..." << endl;
    int wait_count = 0;
    while (!gathering_done && wait_count < 60) {
        this_thread::sleep_for(chrono::milliseconds(100));
        wait_count++;
    }
    
    if (!gathering_done) {
        cout << "⚠️  候选地址收集超时（6秒），但可以继续" << endl;
    }
    
    // 检查是否有srflx候选地址
    cout << "\n[DEBUG] 候选地址统计：共 " << candidate_count.load() << " 个" << endl;
    
    // 获取完整的本地SDP
    if (juice_get_local_description(agent, local_sdp, sizeof(local_sdp)) < 0) {
        cout << "❌ 获取完整SDP失败！" << endl;
        juice_destroy(agent);
        if (config.turn_servers) delete[] config.turn_servers;
        return 1;
    }
    
    // 显示本地SDP
    cout << "\n========================================" << endl;
    cout << "📋 本地SDP描述 (" << (is_controlling ? "Offer" : "Answer") << "):" << endl;
    cout << "========================================" << endl;
    cout << local_sdp << endl;
    cout << "========================================" << endl;
    
    // 检查是否有STUN反射地址
    string sdp_str(local_sdp);
    bool has_srflx = (sdp_str.find("typ srflx") != string::npos);
    bool has_relay = (sdp_str.find("typ relay") != string::npos);
    
    if (!has_srflx && !has_relay) {
        cout << "\n⚠️  警告：未检测到STUN反射地址（srflx）！" << endl;
        cout << "   这意味着：" << endl;
        cout << "   1. STUN服务器 '" << stun_server_static << ":" << stun_port << "' 可能不可达" << endl;
        cout << "   2. 或者防火墙阻止了UDP通信" << endl;
        cout << "   3. P2P连接可能会失败（除非双方在同一局域网）" << endl;
        cout << "\n   建议：" << endl;
        cout << "   - 使用 stun.l.google.com 或 stun.stunprotocol.org" << endl;
        cout << "   - 或者启用TURN服务器" << endl;
    } else if (has_srflx) {
        cout << "\n✅ 检测到STUN反射地址，P2P穿透有希望！" << endl;
    }
    
    cout << "\n请将上述SDP复制并发送给对方！" << endl;
    
    // 获取远端SDP
    string remote_sdp = get_multiline_input(
        "\n请粘贴对方的SDP描述："
    );
    
    if (remote_sdp.empty()) {
        cout << "❌ 远端SDP为空！" << endl;
        juice_destroy(agent);
        if (config.turn_servers) delete[] config.turn_servers;
        return 1;
    }
    
    // 设置远端描述
    cout << "\n========================================" << endl;
    cout << "🔄 设置远端SDP并开始连接..." << endl;
    cout << "========================================" << endl;
    
    if (juice_set_remote_description(agent, remote_sdp.c_str()) < 0) {
        cout << "❌ 设置远端SDP失败！" << endl;
        juice_destroy(agent);
        if (config.turn_servers) delete[] config.turn_servers;
        return 1;
    }
    
    cout << "✅ 远端SDP设置成功，开始ICE连接协商..." << endl;
    
    // 如果是响应方，现在生成Answer
    if (!is_controlling) {
        char answer_sdp[4096];
        if (juice_get_local_description(agent, answer_sdp, sizeof(answer_sdp)) >= 0) {
            cout << "\n========================================" << endl;
            cout << "📋 生成的Answer SDP:" << endl;
            cout << "========================================" << endl;
            cout << answer_sdp << endl;
            cout << "========================================" << endl;
            cout << "\n⚠️  请将上述Answer SDP发送给对方！" << endl;
        }
    }
    
    // 等待连接建立
    cout << "\n⏳ 等待P2P连接建立..." << endl;
    cout << "   （这可能需要几秒到几十秒，请耐心等待）" << endl;
    
    wait_count = 0;
    while (!connected && wait_count < 300) {  // 30秒超时
        this_thread::sleep_for(chrono::milliseconds(100));
        wait_count++;
        
        if (wait_count % 10 == 0) {
            cout << "." << flush;
        }
    }
    
    cout << endl;
    
    if (!connected) {
        cout << "\n⚠️  连接超时（30秒）" << endl;
        cout << "   当前状态: " << state_to_string(juice_get_state(agent)) << endl;
        cout << "   可能原因：" << endl;
        cout << "   1. 双方都在对称NAT后面，需要TURN服务器" << endl;
        cout << "   2. 防火墙阻止了UDP通信" << endl;
        cout << "   3. STUN服务器不可达" << endl;
        cout << "   4. 候选地址收集失败（共" << candidate_count.load() << "个）" << endl;
        
        cout << "\n按回车键退出...";
        cin.get();
    } else {
        // 进入消息收发循环
        cout << "\n========================================" << endl;
        cout << "💬 进入消息收发模式" << endl;
        cout << "========================================" << endl;
        cout << "输入消息并按回车发送，输入 'quit' 退出" << endl;
        
        string message;
        while (true) {
            cout << "\n> ";
            getline(cin, message);
            
            if (message == "quit" || message == "exit") {
                break;
            }
            
            if (!message.empty()) {
                int ret = juice_send(agent, message.c_str(), message.length());
                if (ret < 0) {
                    cout << "❌ 发送失败！" << endl;
                } else {
                    cout << "✅ 已发送: " << message << endl;
                }
            }
        }
    }
    
    // 清理
    cout << "\n🔚 正在关闭连接..." << endl;
    juice_destroy(agent);
    
    if (config.turn_servers) {
        delete[] config.turn_servers;
    }
    
    cout << "👋 程序结束" << endl;
    
    return 0;
}
