# VeritasSync

**VeritasSync** 是一个高性能、基于现代 C++20 开发的 P2P 文件同步工具。

它旨在解决跨局域网/广域网环境下的文件同步问题，利用可靠 UDP (KCP) 进行数据传输，通过 ICE 协议实现 NAT 穿透，并具备工业级的工程实践标准。

## ✨ 核心特性 (Key Features)

  * **可靠的 P2P 传输**: 基于应用层协议封装 **KCP** (Reliable UDP)，在弱网环境下提供比 TCP 更低延迟、更高吞吐的传输体验。
  * **强大的 NAT 穿透**: 集成 **LibJuice** (ICE/STUN/TURN)，支持复杂网络环境下的设备直连。
  * **O(1) 内存大文件传输**: 采用**流式传输 (Streaming Transfer)** 机制，无论文件多大（如 10GB+ 视频），内存占用始终保持在 KB 级别，彻底解决了内存溢出 (OOM) 问题。
  * **数据强一致性**: 实现**原子写入 (Atomic Write)** 机制。通过临时文件缓冲与下载完成后的原子重命名，确保在断电或崩溃时不会产生损坏文件。
  * **智能增量扫描**: 利用**元数据缓存 (Metadata Caching)** 技术，通过修改时间 (mtime) 和文件大小快速比对，减少 90% 以上的冗余 SHA-256 计算。
  * **实时可视化监控**: 内置嵌入式 **WebUI**，实时展示传输进度、节点状态和网络连接详情。
  * **工程化质量保障**:
      * **单元测试**: 核心同步算法由 **Google Test** 全面覆盖。
      * **CI/CD**: 集成 **GitHub Actions** 流水线，实现自动化构建与回归测试。

## 🛠️ 技术栈 (Tech Stack)

  * **语言**: C++20
  * **构建系统**: CMake, vcpkg (Manifest Mode)
  * **网络与异步 I/O**: Boost.Asio
  * **P2P / NAT**: LibJuice (Interactive Connectivity Establishment)
  * **传输协议**: KCP (ARQ Reliable UDP)
  * **文件监控**: efsw (Entangled File System Watcher)
  * **序列化**: nlohmann/json
  * **加密与哈希**: OpenSSL
  * **日志**: spdlog (Async logging)
  * **测试框架**: Google Test

## 🚀 快速开始 (Getting Started)

### 前置要求

  * C++ 编译器 (MSVC 2019+, GCC 10+, Clang 11+)
  * CMake 3.15+
  * Git

### 构建步骤

本项目使用 `vcpkg` 进行依赖管理。

```bash
# 1. 克隆仓库
git clone https://github.com/jokerd888/veritassync.git
cd veritassync

# 2. 安装 vcpkg (如果尚未安装)
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh  # Windows 下使用 .\vcpkg\bootstrap-vcpkg.bat

# 3. 配置项目 (CMake 会自动调用 vcpkg 安装依赖)
# 请将 <path_to_vcpkg> 替换为实际路径
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<path_to_vcpkg>/scripts/buildsystems/vcpkg.cmake

# 4. 编译 (Release 模式)
cmake --build build --config Release
```

### 运行

编译完成后，`bin/` 目录下会生成以下可执行文件：

1.  **`veritas_tracker`**: 信令服务器（负责节点发现和交换 SDP）。
2.  **`veritas_sync`**: 同步客户端节点。

#### 1\. 启动 Tracker

```bash
./bin/veritas_tracker
# 默认监听 9988 端口
```

#### 2\. 配置并启动 Client

在客户端目录创建 `config.json` (可参考 `config.example.json`)：

```json
{
    "tracker_host": "127.0.0.1",
    "tracker_port": 9988,
    "stun_host": "stun.l.google.com",
    "stun_port": 19302,
    "tasks": [
        {
            "sync_key": "my-project-secret",
            "role": "source", 
            "sync_folder": "./data_source"
        },
        {
            "sync_key": "my-project-secret",
            "role": "destination",
            "sync_folder": "./data_backup"
        }
    ]
}
```

*注意：通常需要在两台不同的机器上分别配置 `source` 和 `destination` 角色。*

启动客户端：

```bash
./bin/veritas_sync
```

#### 3\. 访问 WebUI

启动客户端后，打开浏览器访问：`http://127.0.0.1:8800` 即可查看同步状态。

## 📂 项目结构

```text
VeritasSync/
├── include/VeritasSync/   # 头文件
│   ├── P2PManager.h       # P2P 连接与数据传输核心
│   ├── SyncManager.h      # 文件差异对比算法
│   ├── StateManager.h     # 文件系统扫描与监控
│   └── ...
├── src/
│   ├── peer/              # 客户端核心实现
│   ├── tracker/           # 信令服务器实现
│   └── main.cpp           # 客户端入口
├── tests/                 # GTest 单元测试
├── vcpkg.json             # 依赖清单
├── CMakeLists.txt         # 构建脚本
└── .github/workflows/     # CI/CD 配置
```

## 📄 License

本项目采用 MIT 许可证。详情请参阅 [LICENSE](https://www.google.com/search?q=LICENSE) 文件。