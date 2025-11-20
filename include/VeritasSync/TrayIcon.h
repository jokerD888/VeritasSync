#pragma once

#include <functional>
#include <string>
#include <vector>

namespace VeritasSync {

class TrayIcon {
public:
    using VoidCallback = std::function<void()>;

    struct MenuItem {
        std::string text;
        bool checked = false;
        VoidCallback callback;
    };

    struct Impl;

    TrayIcon();
    ~TrayIcon();

    bool init(const std::string& tooltip);
    void add_menu_item(const std::string& text, VoidCallback callback, bool checked = false);
    void add_separator();
    void run_loop();
    void quit();

    static bool is_autostart_enabled();
    static void set_autostart(bool enable);

private:
    Impl* m_impl = nullptr;
};

}  // namespace VeritasSync