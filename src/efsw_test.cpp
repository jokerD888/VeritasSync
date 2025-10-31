#include <chrono>
#include <efsw/efsw.hpp>
#include <filesystem>
#include <iomanip>  // 用于 std::setw
#include <iostream>
#include <sstream>  // 用于 std::stringstream
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

// 辅助函数：将 efsw::Action 枚举转换为可读的字符串
std::string ActionToString(efsw::Action action) {
    switch (action) {
    case efsw::Actions::Add:
        return "Add";
    case efsw::Actions::Delete:
        return "Delete";
    case efsw::Actions::Modified:
        return "Modified";
    case efsw::Actions::Moved:
        return "Moved";
    default:
        return "Unknown";
    }
}

// 辅助函数：获取高精度时间戳
std::string GetTimestamp() {
    // --- 修复：使用 system_clock 来获取可转换为 time_t 的 wall clock 时间 ---
    auto now = std::chrono::system_clock::now();

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) %
        1000;

    // --- 修复：to_time_t 是 system_clock 的静态成员 ---
    time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm bt;

    // 使用 localtime_s (Windows) 或 localtime_r (Linux)
#if defined(_WIN32)
    localtime_s(&bt, &t);
#else
    localtime_r(&t, &bt);
#endif

    char buffer[100];
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &bt);

    std::stringstream ss;
    ss << buffer << "." << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

/**
 * 这是一个专门用于打印所有原始回调的监听器
 * 它不进行任何防抖或逻辑处理
 */
class EventPrinter : public efsw::FileWatchListener {
public:
    void handleFileAction(efsw::WatchID, const std::string& dir,
        const std::string& filename, efsw::Action action,
        std::string oldFilename = "") override {
        // 我们只关心文件，忽略目录本身的变化 (比如 '.')
        if (filename == "." || filename == "..") {
            return;
        }

        std::cout << GetTimestamp() << " | "
            << "Action: [" << std::setw(8) << std::left
            << ActionToString(action) << "] | "
            << "File: " << filename;

        if (!oldFilename.empty()) {
            std::cout << " (Renamed from: " << oldFilename << ")";
        }

        std::cout << std::endl;
    }
};

int main() {
#if defined(_WIN32)
    // 确保 Windows 控制台能正确显示中文
    SetConsoleOutputCP(CP_UTF8);
#endif

    std::string watchDir = "./EFSW_TEST_FOLDER";

    // 1. 创建测试目录
    if (!std::filesystem::exists(watchDir)) {
        std::filesystem::create_directory(watchDir);
    }
    std::filesystem::path watchPath = std::filesystem::absolute(watchDir);

    // 2. 创建 FileWatcher 和 Listener
    efsw::FileWatcher fileWatcher;
    EventPrinter listener;

    // 3. 添加 Watch
    // (true 表示递归监视)
    efsw::WatchID watchID =
        fileWatcher.addWatch(watchPath.string(), &listener, true);

    if (watchID < 0) {
        std::cerr << "错误: 无法启动监视器。请检查路径。" << std::endl;
        return 1;
    }

    // 4. 启动监视
    fileWatcher.watch();

    std::cout << "--- eFsw 回调事件监视器 ---" << std::endl;
    std::cout << "正在监视: " << watchPath.string() << std::endl;
    std::cout << "\n请打开上述文件夹，执行以下操作并观察控制台输出："
        << std::endl;
    std::cout << " 1. 右键 -> 新建 -> 文本文档" << std::endl;
    std::cout << " 2. (立即) 将'新建 文本文档.txt'重命名为 'final.txt'"
        << std::endl;
    std::cout << "\n按 Ctrl+C 退出。" << std::endl;

    // 保持程序运行
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
