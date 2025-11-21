# VeritasSync

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C++](https://img.shields.io/badge/Language-C%2B%2B20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux-lightgrey.svg)]()

**[ [English](#english) ]** | **[ [中文](#中文) ]**

---

<a name="english"></a>
# VeritasSync

**VeritasSync** is a high-performance P2P file synchronization tool built with modern C++20.

It is designed to solve file synchronization challenges across LAN/WAN environments, utilizing reliable UDP (KCP) for data transmission and ICE protocol for NAT traversal, adhering to industrial-grade engineering standards.

## ✨ Key Features

* **Reliable P2P Transmission**: Built on **KCP** (Reliable UDP), providing lower latency and higher throughput than TCP in weak network environments.
* **Robust NAT Traversal**: Integrated **LibJuice** (ICE/STUN/TURN) enables direct device connection in complex network environments by automatically detecting the best path (P2P direct or Relay).
* **System Tray Integration**: Native Windows **System Tray** support. Run quietly in the background with Auto-start capability and quick access menu.
* **UTF-8 Everywhere**: Implements a **UTF-8 Everywhere** strategy, completely resolving garbled characters in paths, console output issues, and cross-platform filename compatibility on Windows.
* **Bi-directional Sync**: Supports both One-Way (Source -> Destination) and Bi-directional synchronization modes to flexibly meet backup and collaboration needs.
* **O(1) Memory Usage**: Uses **Streaming Transfer** mechanisms. Memory usage remains in the KB range regardless of file size (e.g., 10GB+ videos), eliminating OOM (Out of Memory) issues.
* **Strong Consistency & Safety**:
    * **Atomic Write**: Ensures file integrity via temporary buffering and atomic renaming upon download completion.
    * **Infinite Loop Prevention**: Built-in **Smart Ignore List** automatically blocks database files (`.veritas.db`) to prevent recursive synchronization loops.
* **Smart Incremental Scanning**: Utilizes **SQLite** to cache metadata, combining mtime and file size for rapid comparison, significantly reducing redundant SHA-256 calculations.
* **Real-time Monitoring**: Built-in embedded **WebUI** displays transfer progress, node status, and network connection details in real-time.
* **Quality Assurance**: Core algorithms are fully covered by **Google Test**, with integrated GitHub Actions CI/CD pipelines.

## 🛠️ Tech Stack

* **Language**: C++20
* **Build System**: CMake, vcpkg (Manifest Mode)
* **Network & Async I/O**: Boost.Asio
* **P2P / NAT**: LibJuice (Interactive Connectivity Establishment)
* **Transport Protocol**: KCP (ARQ Reliable UDP)
* **Metadata Storage**: **SQLite3**
* **File Watching**: efsw (Entangled File System Watcher)
* **System Interaction**: Win32 API (Tray Icon), Shell API
* **Serialization**: nlohmann/json
* **Crypto & Hashing**: OpenSSL
* **Logging**: spdlog (Async logging with Virtual Terminal support)
* **Testing**: Google Test

## 🚀 Getting Started

### Prerequisites

* C++ Compiler (MSVC 2019+, GCC 10+, Clang 11+)
* CMake 3.15+
* Git

### Build Steps

This project uses `vcpkg` for dependency management.

```bash
# 1. Clone the repository
git clone [https://github.com/jokerd888/veritassync.git](https://github.com/jokerd888/veritassync.git)
cd veritassync

# 2. Install vcpkg (if not installed)
git clone [https://github.com/microsoft/vcpkg.git](https://github.com/microsoft/vcpkg.git)
./vcpkg/bootstrap-vcpkg.sh  # Use .\vcpkg\bootstrap-vcpkg.bat on Windows

# 3. Configure project (CMake will automatically use vcpkg)
# Replace <path_to_vcpkg> with your actual path
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<path_to_vcpkg>/scripts/buildsystems/vcpkg.cmake

# 4. Build (Release mode)
cmake --build build --config Release
```

### Packaging (Create Installer)

To generate a Windows Installer (`.exe`) and Portable Zip:

```bash
cd build
cpack -C Release
```
Artifacts will be generated in the `build/` directory.

### Running

* **Developer Mode**: Run `./build/release/veritas_sync.exe`.
* **User Mode**: Install via the generated `.exe`, then launch from Start Menu/Desktop.
    * The app runs in the background. Check the **System Tray** (bottom-right corner) for the icon.
    * Right-click the tray icon to Open WebUI, Configure Auto-start, or Exit.

#### 1. Start Tracker

```bash
./bin/veritas_tracker
# Listens on port 9988 by default
```

#### 2. Start Client & Access WebUI

No manual configuration file editing is required. Start the client directly:

```bash
./bin/veritas_sync
```

After startup, open your browser and visit: `http://127.0.0.1:8800`.

In the WebUI:
1.  Configure the Tracker address (e.g., `127.0.0.1:9988`).
2.  Click **"New Task"** (新建任务).
3.  **Note**: When entering the `Sync Key`, it is recommended to click the **"🎲 Generate"** button next to it.
4.  Select the sync directory and role (Source/Destination).

## 📂 Project Structure

```text
VeritasSync/
├── include/VeritasSync/   # Headers
│   ├── EncodingUtils.h    # Cross-platform UTF-8 encoding utilities
│   ├── P2PManager.h       # Core P2P connection & transfer logic
│   ├── TrayIcon.h         # System Tray Interface
│   ├── Database.h         # SQLite wrapper
│   └── ...
├── src/
│   ├── peer/              # Client core implementation
│   │   ├── StateManager.cpp # State management & file scanning
│   │   └── ...
│   ├── platform/          # OS-specific implementations
│   │   └── TrayIcon.cpp   # Windows System Tray implementation
│   ├── tracker/           # Signaling server implementation
│   └── main.cpp           # Client entry point
├── web/                   # WebUI assets
├── tests/                 # GTest unit tests
├── vcpkg.json             # Dependency manifest
└── CMakeLists.txt         # Build script
```

## 📄 License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

---

<a name="中文"></a>
# VeritasSync

**VeritasSync** 是一个高性能、基于现代 C++20 开发的 P2P 文件同步工具。

它旨在解决跨局域网/广域网环境下的文件同步问题，利用可靠 UDP (KCP) 进行数据传输，通过 ICE 协议实现 NAT 穿透，并具备工业级的工程实践标准。

## ✨ 核心特性

* **可靠的 P2P 传输**: 基于应用层协议封装 **KCP** (Reliable UDP)，在弱网环境下提供比 TCP 更低延迟、更高吞吐的传输体验。
* **强大的 NAT 穿透**: 集成 **LibJuice** (ICE/STUN/TURN)，支持复杂网络环境下的设备直连，自动探测最佳传输路径（P2P 直连或中继）。
* **系统托盘集成**: 原生 Windows **系统托盘**支持。程序静默后台运行，支持开机自启、右键快捷菜单。
* **全链路 UTF-8 支持**: 采用 **UTF-8 Everywhere** 策略，彻底解决了 Windows 平台下的中文路径乱码、控制台显示异常以及跨平台文件名兼容性问题。
* **双向同步支持**: 支持单向（Source -> Destination）及双向（Bi-directional）同步模式，灵活满足备份与协作需求。
* **O(1) 内存大文件传输**: 采用 **流式传输 (Streaming Transfer)** 机制，无论文件多大（如 10GB+ 视频），内存占用始终保持在 KB 级别，彻底解决了内存溢出 (OOM) 问题。
* **数据强一致性与防死循环**:
    * 实现 **原子写入 (Atomic Write)** 机制，通过临时文件缓冲与下载完成后的原子重命名，确保文件完整性。
    * 内置 **智能忽略列表**，自动屏蔽数据库自身文件（`.veritas.db`），防止无限同步循环。
* **智能增量扫描**: 利用 **SQLite 数据库** 缓存元数据，结合修改时间 (mtime) 和文件大小快速比对，显著减少冗余 SHA-256 计算。
* **实时可视化监控**: 内置嵌入式 **WebUI**，实时展示传输进度、节点状态和网络连接详情。
* **工程化质量保障**: 核心算法由 **Google Test** 全面覆盖，集成 GitHub Actions 自动化流水线。

## 🛠️ 技术栈

* **语言**: C++20
* **构建系统**: CMake, vcpkg (Manifest Mode)
* **网络与异步 I/O**: Boost.Asio
* **P2P / NAT**: LibJuice (Interactive Connectivity Establishment)
* **传输协议**: KCP (ARQ Reliable UDP)
* **元数据存储**: **SQLite3**
* **文件监控**: efsw (Entangled File System Watcher)
* **系统交互**: Win32 API (托盘图标), Shell API
* **序列化**: nlohmann/json
* **加密与哈希**: OpenSSL
* **日志**: spdlog (支持虚拟终端的异步日志)
* **测试框架**: Google Test

## 🚀 快速开始

### 前置要求

* C++ 编译器 (MSVC 2019+, GCC 10+, Clang 11+)
* CMake 3.15+
* Git

### 构建步骤

本项目使用 `vcpkg` 进行依赖管理。

```bash
# 1. 克隆仓库
git clone [https://github.com/jokerd888/veritassync.git](https://github.com/jokerd888/veritassync.git)
cd veritassync

# 2. 安装 vcpkg (如果尚未安装)
git clone [https://github.com/microsoft/vcpkg.git](https://github.com/microsoft/vcpkg.git)
./vcpkg/bootstrap-vcpkg.sh  # Windows 下使用 .\vcpkg\bootstrap-vcpkg.bat

# 3. 配置项目 (CMake 会自动调用 vcpkg 安装依赖)
# 请将 <path_to_vcpkg> 替换为实际路径
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<path_to_vcpkg>/scripts/buildsystems/vcpkg.cmake

# 4. 编译 (Release 模式)
cmake --build build --config Release
```

### 打包发布 (生成安装程序)

编译完成后，运行以下命令生成 Windows 安装包 (`.exe`) 和绿色版 (`.zip`)：

```bash
cd build
cpack -C Release
```
生成的文件将位于 `build/` 目录下。

### 运行说明

* **开发者模式**: 直接运行 `./build/release/veritas_sync.exe`。
* **用户模式**: 运行生成的安装包进行安装。
    * 启动后程序会自动隐藏到**系统托盘**（任务栏右下角）。
    * **右键点击**托盘图标可进行操作：打开控制台 (WebUI)、打开文件夹、设置开机自启或退出程序。

#### 1. 启动 Tracker

```bash
./bin/veritas_tracker
# 默认监听 9988 端口
```

#### 2. 启动 Client 并访问 WebUI

无需手动编辑配置文件，直接启动客户端：

```bash
./bin/veritas_sync
```

启动后，打开浏览器访问：`http://127.0.0.1:8800`。

在 WebUI 中：
1.  配置 Tracker 地址（例如 `127.0.0.1:9988`）。
2.  点击 **"新建任务"**。
3.  **注意**：在输入 `Sync Key` 时，建议点击旁边的 **"🎲 生成"** 按钮。
4.  选择同步目录和角色（Source/Destination）。

## 📂 项目结构

```text
VeritasSync/
├── include/VeritasSync/   # 头文件
│   ├── EncodingUtils.h    # UTF-8 跨平台编码转换工具
│   ├── P2PManager.h       # P2P 连接与数据传输核心
│   ├── TrayIcon.h         # 系统托盘接口
│   ├── Database.h         # SQLite 数据库封装
│   └── ...
├── src/
│   ├── peer/              # 客户端核心实现
│   │   ├── StateManager.cpp # 状态管理与文件扫描
│   │   └── ...
│   ├── platform/          # 平台相关实现
│   │   └── TrayIcon.cpp   # Windows 系统托盘实现
│   ├── tracker/           # 信令服务器实现
│   └── main.cpp           # 客户端入口
├── web/                   # WebUI 资源
├── tests/                 # GTest 单元测试
├── vcpkg.json             # 依赖清单
└── CMakeLists.txt         # 构建脚本
```

## 📄 License

本项目采用 MIT 许可证。详情请参阅 LICENSE 文件。