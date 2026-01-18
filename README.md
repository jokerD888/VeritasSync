# VeritasSync

<div align="center">
  <img src="app.ico" alt="VeritasSync Logo" width="128" height="128" />
  <br />

  [![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
  [![C++](https://img.shields.io/badge/Language-C%2B%2B20-blue.svg)](https://en.cppreference.com/w/cpp/20)
  [![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux-lightgrey.svg)]()
  [![Build Status](https://img.shields.io/badge/Build-Passing-brightgreen)]()

  <h3>基于 C++20 的高性能 P2P 文件同步工具</h3>

  <p>
    <strong>中文</strong> | <a href="README_EN.md">English</a>
  </p>
</div>

---

**VeritasSync** 是一个现代化的、去中心化的 P2P 文件同步解决方案。它利用可靠 UDP (KCP) 进行高速数据传输，通过 ICE 协议实现复杂的 NAT 穿透，并提供了一个美观的 Web 控制台进行管理。

无论是局域网内的大文件快速传输，还是跨广域网的多端双向同步，VeritasSync 都能提供稳定、安全、高效的体验。

## ✨ 核心特性

### 🚀 高性能网络传输
* **可靠 UDP (KCP)**: 基于 ARQ 机制的可靠 UDP 传输，在丢包率较高的弱网环境下，吞吐量和延迟表现远优于传统 TCP。
* **智能 NAT 穿透 (ICE)**: 集成 **LibJuice** (STUN/TURN)，支持 Full Cone、Restricted Cone 等多种 NAT 类型穿透。自动探测最佳路径（P2P 直连优先，Relay 中继保底）。
* **多 WAN 并发探测**: 独有的 **Multi-WAN Probing** 技术，自动利用所有可用出口 IP 进行连通性探测，显著提高多宽带环境下的穿透成功率。
* **断点续传**: 支持传输中断后自动恢复，无需从头开始传输。

### 🔄 灵活的同步逻辑
* **双向同步 (Bi-Directional)**: 支持多端互相同步，内置 **源头回声抑制 (Source-side Echo Suppression)** 算法，从源头阻止回声广播，节省带宽。
* **智能增量更新**: 利用 **SQLite** 缓存文件元数据 (Hash + mtime)，结合 **efsw** 文件监控，实现毫秒级变更检测与增量同步。
* **冲突解决策略**: 当多端同时修改同一文件时，自动检测冲突并保留副本（重命名为 `filename.conflict.<timestamp>.ext`），确保数据零丢失。
* **自定义忽略规则**: 支持通过 `.veritasignore` 文件配置忽略规则，Web UI 提供可视化编辑器。

### 🛡️ 安全与工程化
* **端到端加密**: 通信链路采用 **AES-256-GCM** 加密，密钥由 SHA-256 派生，确保数据传输安全。
* **UTF-8 Everywhere**: 彻底解决 Windows 平台下的中文路径乱码问题，跨平台文件名完美兼容。
* **O(1) 内存占用**: 采用流式传输 (Streaming) 与 Snappy 压缩，无论同步 10GB 视频还是百万小文件，内存占用始终保持低位。
* **单实例保护**: 防止同一设备上运行多个实例导致冲突。

### 🖥️ 现代交互体验
* **WebUI 控制台**: 内置基于 `httplib` 的 Web 服务器，提供赛博朋克风格的深色仪表盘。实时监控传输速度、节点状态、P2P 连接详情及配置任务。
* **系统托盘集成**: 原生 Windows 托盘支持，支持开机自启、后台静默运行。
* **忽略规则编辑器**: 可视化编辑 `.veritasignore` 文件，配置需要排除同步的文件和目录。

## 🛠️ 技术栈

* **核心语言**: C++20 (std::jthread, std::span, std::shared_mutex)
* **构建系统**: CMake, vcpkg (Manifest Mode)
* **网络通信**: Boost.Asio, KCP, LibJuice (ICE), miniUPnPc
* **Web 服务**: cpp-httplib, nlohmann/json
* **数据存储**: SQLite3
* **加密压缩**: OpenSSL, Snappy
* **系统集成**: Win32 API (Tray, Mutex), efsw (File Watcher)
* **日志系统**: spdlog (Async)

## 🚀 快速开始

### 环境要求

* **编译器**: MSVC 2019+ (Windows) 或 GCC 10+/Clang 11+ (Linux)
* **工具**: CMake 3.15+, Git

### 编译步骤

本项目使用 `vcpkg` 的 Manifest 模式管理依赖，编译过程非常简单。

```bash
# 1. 克隆仓库
git clone https://github.com/jokerD888/VeritasSync.git
cd VeritasSync

# 2. 安装 vcpkg (如果尚未安装)
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh  # Windows 下运行 .\vcpkg\bootstrap-vcpkg.bat

# 3. 配置项目 (自动下载并编译依赖，首次运行可能较慢)
# 请将 <path_to_vcpkg> 替换为实际 vcpkg 路径
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<path_to_vcpkg>/scripts/buildsystems/vcpkg.cmake

# 4. 编译 (Release 模式)
cmake --build build --config Release
```

### 运行说明

#### 1. 启动信号服务器 (Tracker)

Tracker 用于节点间的发现与信令交换。

```bash
./bin/veritas_tracker
# 默认监听端口: 9988
```

#### 2. 启动客户端 (Sync Node)

客户端启动后会自动最小化到托盘，并启动 Web 控制台。

```bash
./bin/veritas_sync
```

#### 3. 配置与使用

1.  打开浏览器访问 **WebUI**: `http://127.0.0.1:8800`
2.  在 **全局配置** 中设置 Tracker 地址（例如 `127.0.0.1:9988`）。
3.  点击 **"新建任务"**：
      * **Sync Key**: 点击 🎲 生成唯一密钥（多端需使用相同 Key）。
      * **同步模式**: 选择 "单向" 或 "双向"。
      * **本地路径**: 选择要同步的文件夹。
4.  在另一台设备上重复上述步骤，使用 **相同的 Sync Key**。
5.  点击任务卡片上的 **"忽略规则"** 按钮可配置需要排除同步的文件。

## 📂 项目结构

```text
VeritasSync/
├── include/VeritasSync/   # 头文件
│   ├── common/            # 通用工具 (Config, Logger, Hashing, Encoding)
│   ├── net/               # 网络层 (KcpSession, IceTransport)
│   ├── p2p/               # P2P 核心 (P2PManager, PeerController, TrackerClient, WebUI)
│   ├── storage/           # 存储层 (StateManager, Database, FileFilter)
│   └── sync/              # 同步层 (SyncNode, TransferManager, Protocol)
├── src/
│   ├── common/            # 通用工具实现
│   ├── net/               # 网络层实现
│   ├── p2p/               # P2P 层实现
│   ├── storage/           # 存储层实现
│   ├── sync/              # 同步层实现
│   ├── tracker/           # 信令服务器实现
│   └── web/               # Web 前端资源 (HTML/CSS/JS)
├── vcpkg.json             # 依赖包清单
└── CMakeLists.txt         # 构建脚本
```

## 🔧 配置文件

### config.json

```json
{
    "tracker_host": "your-tracker-server.com",
    "tracker_port": 9988,
    "stun_host": "stun.l.google.com",
    "stun_port": 19302,
    "enable_multi_stun_probing": true,
    "tasks": [
        {
            "sync_key": "your-sync-key",
            "sync_folder": "/path/to/folder",
            "role": "source",
            "mode": "bidirectional"
        }
    ]
}
```

### .veritasignore

在同步目录下创建 `.veritasignore` 文件可自定义忽略规则：

```
# 忽略日志文件
*.log

# 忽略临时文件
*.tmp
*.temp

# 忽略目录
node_modules/
.git/
__pycache__/
```

## 📄 开源协议

本项目采用 [MIT License](LICENSE) 授权。
