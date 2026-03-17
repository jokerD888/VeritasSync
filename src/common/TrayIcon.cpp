#include "VeritasSync/common/TrayIcon.h"

#include "VeritasSync/common/EncodingUtils.h"
#include "VeritasSync/common/Logger.h"

#ifdef _WIN32
#include <shellapi.h>
#include <shlobj.h>  // for IsUserAnAdmin if needed, usually not for HKCU
#include <windows.h>
#endif

#include <iostream>
#include <map>

namespace VeritasSync {

#ifdef _WIN32

// 定义自定义消息 ID
#define WM_TRAYICON (WM_USER + 1)
// 菜单 ID 从 2000 开始
#define ID_MENU_BASE 2000

struct TrayIcon::Impl {
    HWND hwnd = nullptr;
    NOTIFYICONDATAW nid = {};
    std::vector<MenuItem> menus;
    std::map<int, VoidCallback> menu_callbacks;
    bool running = false;

    // 静态指针用于 WndProc 回调
    static Impl* global_instance;
};

TrayIcon::Impl* TrayIcon::Impl::global_instance = nullptr;

// 窗口过程回调
LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* impl = TrayIcon::Impl::global_instance;
    if (!impl) return DefWindowProc(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_TRAYICON:
            // 鼠标右键点击图标 -> 弹出菜单
            if (lParam == WM_RBUTTONUP) {
                POINT pt;
                GetCursorPos(&pt);

                // 必要的 hack: 将窗口置前，否则菜单点击外面不会消失
                SetForegroundWindow(hwnd);

                HMENU hMenu = CreatePopupMenu();
                if (hMenu) {
                    impl->menu_callbacks.clear();
                    int id_counter = ID_MENU_BASE;

                    for (const auto& item : impl->menus) {
                        if (item.text == "-") {
                            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
                        } else {
                            UINT flags = MF_STRING;
                            if (item.checked_cb && item.checked_cb()) {
                                flags |= MF_CHECKED;
                            }

                            // 使用 EncodingUtils 转宽字符，解决菜单中文乱码
                            std::wstring wText = Utf8ToWide(item.text);
                            AppendMenuW(hMenu, flags, id_counter, wText.c_str());

                            impl->menu_callbacks[id_counter] = item.callback;
                            id_counter++;
                        }
                    }

                    // 阻塞弹出菜单
                    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
                    DestroyMenu(hMenu);
                }
            }
            // 鼠标左键双击 -> 执行第一个菜单项 (通常是打开 WebUI)
            else if (lParam == WM_LBUTTONDBLCLK) {
                if (!impl->menus.empty() && impl->menus[0].callback) {
                    impl->menus[0].callback();
                }
            }
            break;

        case WM_COMMAND: {
            int cmdId = LOWORD(wParam);
            if (impl->menu_callbacks.count(cmdId)) {
                impl->menu_callbacks[cmdId]();
            }
        } break;

        case WM_DESTROY:
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

TrayIcon::TrayIcon() : m_impl(new Impl()) { Impl::global_instance = m_impl; }

TrayIcon::~TrayIcon() {
    if (m_impl->hwnd) {
        Shell_NotifyIconW(NIM_DELETE, &m_impl->nid);
        DestroyWindow(m_impl->hwnd);
    }
    delete m_impl;
}

bool TrayIcon::init(const std::string& tooltip) {
    // 1. 注册窗口类 (创建一个看不见的窗口来接收消息)
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"VeritasSyncTrayClass";
    RegisterClassExW(&wc);

    // 2. 创建窗口
    m_impl->hwnd =
        CreateWindowExW(0, L"VeritasSyncTrayClass", L"VeritasSync Tray", 0, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);

    if (!m_impl->hwnd) return false;

    // 3. 初始化托盘图标数据
    m_impl->nid.cbSize = sizeof(NOTIFYICONDATAW);
    m_impl->nid.hWnd = m_impl->hwnd;
    m_impl->nid.uID = 1;
    m_impl->nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_impl->nid.uCallbackMessage = WM_TRAYICON;

    // 加载系统默认图标 (Application Icon)，如果没有则使用系统问号图标
    // 你可以在 .rc 资源文件中定义 IDI_APPLICATION 图标
    m_impl->nid.hIcon =
        (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(101), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                         GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR | LR_SHARED);

    // 2. 如果加载失败，回退到默认大图标
    if (!m_impl->nid.hIcon) {
        m_impl->nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(101));
    }

    // 3. 最后的保底
    if (!m_impl->nid.hIcon) {
        m_impl->nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    }

    // 设置提示文字 (转宽字符)
    std::wstring wTooltip = Utf8ToWide(tooltip);
    wcsncpy_s(m_impl->nid.szTip, wTooltip.c_str(), _TRUNCATE);

    // 4. 添加到托盘
    return Shell_NotifyIconW(NIM_ADD, &m_impl->nid);
}

void TrayIcon::add_menu_item(const std::string& text, VoidCallback callback, CheckCallback check_cb) {
    m_impl->menus.push_back({text, check_cb, callback});
}

void TrayIcon::add_separator() { m_impl->menus.push_back({"-", nullptr, nullptr}); }

void TrayIcon::run_loop() {
    m_impl->running = true;
    MSG msg;
    while (m_impl->running && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

void TrayIcon::quit() {
    m_impl->running = false;
    PostMessage(m_impl->hwnd, WM_DESTROY, 0, 0);
}

// --- 开机自启实现 (注册表) ---
static const wchar_t* REG_PATH = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* APP_NAME = L"VeritasSync";

bool TrayIcon::is_autostart_enabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    wchar_t value[MAX_PATH];
    DWORD size = sizeof(value);
    DWORD type = REG_SZ;
    long result = RegQueryValueExW(hKey, APP_NAME, NULL, &type, (LPBYTE)value, &size);
    RegCloseKey(hKey);

    return (result == ERROR_SUCCESS);
}

void TrayIcon::set_autostart(bool enable) {
    HKEY hKey;
    LONG result = RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, KEY_SET_VALUE, &hKey);
    if (result != ERROR_SUCCESS) {
        g_logger->error("[Tray] 无法打开注册表项用于设置自启动，错误码: {}", result);
        return;
    }

    if (enable) {
        // 获取当前 EXE 路径
        wchar_t path[MAX_PATH];
        DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) {
            g_logger->error("[Tray] 获取 EXE 路径失败");
            RegCloseKey(hKey);
            return;
        }
        
        // 加上引号，防止路径有空格时出错
        std::wstring cmd = std::wstring(L"\"") + path + L"\"";
        
        // 写入注册表
        result = RegSetValueExW(hKey, APP_NAME, 0, REG_SZ, 
                                (const BYTE*)cmd.c_str(), 
                                (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        
        if (result == ERROR_SUCCESS) {
            g_logger->info("[Tray] 已开启开机自启，路径: {}", WideToUtf8(cmd));
        } else {
            g_logger->error("[Tray] 写入注册表失败，错误码: {}", result);
        }
    } else {
        result = RegDeleteValueW(hKey, APP_NAME);
        if (result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND) {
            g_logger->info("[Tray] 已关闭开机自启");
        } else {
            g_logger->error("[Tray] 删除注册表值失败，错误码: {}", result);
        }
    }
    RegCloseKey(hKey);
}

#else
// 非 Windows 平台的空实现 (占位)
struct TrayIcon::Impl {};
TrayIcon::TrayIcon() : m_impl(nullptr), m_running(true) {}
TrayIcon::~TrayIcon() {}
bool TrayIcon::init(const std::string&) { return false; }
void TrayIcon::add_menu_item(const std::string&, VoidCallback, CheckCallback) {}
void TrayIcon::add_separator() {}
void TrayIcon::run_loop() {
    // 【修复】Linux/Mac 使用 atomic 标志控制循环退出
    while (m_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
void TrayIcon::quit() { 
    m_running.store(false);
    exit(0); 
}
bool TrayIcon::is_autostart_enabled() { return false; }
void TrayIcon::set_autostart(bool) {}
#endif

}  // namespace VeritasSync