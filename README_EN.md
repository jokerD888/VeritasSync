# VeritasSync

<div align="center">
  <img src="app.ico" alt="VeritasSync Logo" width="128" height="128" />
  <br />

  [![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
  [![C++](https://img.shields.io/badge/Language-C%2B%2B20-blue.svg)](https://en.cppreference.com/w/cpp/20)
  [![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux-lightgrey.svg)]()
  [![Build Status](https://img.shields.io/badge/Build-Passing-brightgreen)]()

  <h3>High-Performance P2P File Synchronization Tool (C++20)</h3>

  <p>
    <a href="README.md">中文</a> | <strong>English</strong>
  </p>
</div>

---

**VeritasSync** is a modern, decentralized P2P file synchronization solution. It leverages reliable UDP (KCP) for high-speed data transmission, utilizes the ICE protocol for complex NAT traversal, and features a sleek Web console for management.

Whether for fast transfer of large files within a LAN or bi-directional synchronization across WANs, VeritasSync provides a stable, secure, and efficient experience.

## ✨ Key Features

### 🚀 High-Performance Networking
* **Reliable UDP (KCP)**: Built on ARQ-based reliable UDP transmission. In weak network environments with high packet loss, it offers significantly better throughput and latency compared to traditional TCP.
* **Smart NAT Traversal (ICE)**: Integrated with **LibJuice** (STUN/TURN), supporting traversal for various NAT types like Full Cone and Restricted Cone. It automatically detects the optimal path (prioritizing P2P direct connection, with Relay as a fallback).
* **Multi-WAN Probing**: Features unique **Multi-WAN Probing** technology that automatically utilizes all available egress IPs for connectivity probing, significantly increasing traversal success rates in multi-broadband environments.
* **Resume Transfer**: Supports automatic resumption after transfer interruption, no need to restart from the beginning.

### 🔄 Flexible Synchronization Logic
* **Bi-Directional Sync**: Supports mutual synchronization between multiple devices with built-in **Source-side Echo Suppression** algorithm that prevents echo broadcasts at the source, saving bandwidth.
* **Smart Incremental Updates**: Uses **SQLite** to cache file metadata (Hash + mtime) combined with **efsw** file monitoring to achieve millisecond-level change detection and incremental synchronization.
* **Conflict Resolution Strategy**: Automatically detects conflicts when the same file is modified on multiple ends simultaneously, preserving a copy (renamed to `filename.conflict.<timestamp>.ext`) to ensure zero data loss.
* **Custom Ignore Rules**: Supports configuring ignore rules via `.veritasignore` file, with a visual editor in the Web UI.

### 🛡️ Security & Engineering
* **End-to-End Encryption**: Communication links are encrypted using **AES-256-GCM**, with keys derived via SHA-256 to ensure data transmission security.
* **UTF-8 Everywhere**: Completely resolves garbled character issues with non-ASCII paths on Windows, ensuring perfect cross-platform filename compatibility.
* **O(1) Memory Usage**: Adopts Streaming Transfer and Snappy compression. Memory usage remains low regardless of whether you are syncing a 10GB video or millions of small files.
* **Single Instance Protection**: Prevents multiple instances from running on the same device to avoid conflicts.

### 🖥️ Modern User Experience
* **WebUI Console**: Built-in `httplib`-based Web server providing a Cyberpunk-style dark dashboard. Monitor transfer speeds, node status, P2P connection details, and configure tasks in real-time.
* **System Tray Integration**: Native Windows tray support with auto-start capability and silent background operation.
* **Ignore Rules Editor**: Visual editor for `.veritasignore` files to configure files and directories to exclude from synchronization.

## 🛠️ Tech Stack

* **Language**: C++20 (std::jthread, std::span, std::shared_mutex)
* **Build System**: CMake, vcpkg (Manifest Mode)
* **Networking**: Boost.Asio, KCP, LibJuice (ICE), miniUPnPc
* **Web Service**: cpp-httplib, nlohmann/json
* **Storage**: SQLite3
* **Encryption/Compression**: OpenSSL, Snappy
* **System Integration**: Win32 API (Tray, Mutex), efsw (File Watcher)
* **Logging**: spdlog (Async)

## 🚀 Getting Started

### Prerequisites

* **Compiler**: MSVC 2019+ (Windows) or GCC 10+/Clang 11+ (Linux)
* **Tools**: CMake 3.15+, Git

### Build Steps

This project uses `vcpkg` in Manifest mode for dependency management, making the build process very simple.

```bash
# 1. Clone the repository
git clone https://github.com/jokerD888/VeritasSync.git
cd VeritasSync

# 2. Install vcpkg (if not already installed)
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh  # Run .\vcpkg\bootstrap-vcpkg.bat on Windows

# 3. Configure the project (Dependencies will be downloaded and compiled automatically; first run may be slow)
# Please replace <path_to_vcpkg> with your actual vcpkg path
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<path_to_vcpkg>/scripts/buildsystems/vcpkg.cmake

# 4. Build (Release Mode)
cmake --build build --config Release
```

### Running

#### 1. Start the Signaling Server (Tracker)

The Tracker is used for discovery and signaling exchange between nodes.

```bash
./bin/veritas_tracker
# Default listening port: 9988
```

#### 2. Start the Client (Sync Node)

The client will automatically minimize to the system tray and launch the Web console upon startup.

```bash
./bin/veritas_sync
```

#### 3. Configuration & Usage

1.  Open your browser and visit **WebUI**: `http://127.0.0.1:8800`
2.  Set the Tracker address (e.g., `127.0.0.1:9988`) in **Global Config**.
3.  Click **"New Task"**:
      * **Sync Key**: Click 🎲 to generate a unique key (must use the same Key across devices).
      * **Sync Mode**: Select "One-way" or "Bi-directional".
      * **Local Path**: Select the folder to synchronize.
4.  Repeat the above steps on another device using the **same Sync Key**.
5.  Click the **"Ignore Rules"** button on the task card to configure files to exclude from synchronization.

## 📂 Project Structure

```text
VeritasSync/
├── include/VeritasSync/   # Header files
│   ├── common/            # Utilities (Config, Logger, Hashing, Encoding)
│   ├── net/               # Network layer (KcpSession, IceTransport)
│   ├── p2p/               # P2P core (P2PManager, PeerController, TrackerClient, WebUI)
│   ├── storage/           # Storage layer (StateManager, Database, FileFilter)
│   └── sync/              # Sync layer (SyncNode, TransferManager, Protocol)
├── src/
│   ├── common/            # Utilities implementation
│   ├── net/               # Network layer implementation
│   ├── p2p/               # P2P layer implementation
│   ├── storage/           # Storage layer implementation
│   ├── sync/              # Sync layer implementation
│   ├── tracker/           # Signaling server implementation
│   └── web/               # Web frontend resources (HTML/CSS/JS)
├── vcpkg.json             # Dependency manifest
└── CMakeLists.txt         # Build script
```

## � Configuration

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

Create a `.veritasignore` file in the sync directory to customize ignore rules:

```
# Ignore log files
*.log

# Ignore temporary files
*.tmp
*.temp

# Ignore directories
node_modules/
.git/
__pycache__/
```

## 📄 License

This project is licensed under the [MIT License](LICENSE).
