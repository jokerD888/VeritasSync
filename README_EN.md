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

### 🔄 Flexible Synchronization Logic
* **Bi-Directional Sync**: Supports mutual synchronization between multiple devices with a built-in **Echo Detection** algorithm to prevent data loops.
* **Smart Incremental Updates**: Uses **SQLite** to cache file metadata (Hash + mtime) combined with **efsw** file monitoring to achieve millisecond-level change detection and incremental synchronization.
* **Conflict Resolution Strategy**: Automatically detects conflicts when the same file is modified on multiple ends simultaneously, preserving a copy (renamed to `filename.conflict.<timestamp>.ext`) to ensure zero data loss.

### 🛡️ Security & Engineering
* **End-to-End Encryption**: Communication links are encrypted using **AES-256-GCM**, with keys derived via SHA-256 to ensure data transmission security.
* **UTF-8 Everywhere**: Completely resolves garbled character issues with Chinese paths on Windows, ensuring perfect cross-platform filename compatibility.
* **O(1) Memory Usage**: Adopts Streaming Transfer and Snappy compression. Memory usage remains low regardless of whether you are syncing a 10GB video or millions of small files.

### 🖥️ Modern User Experience
* **WebUI Console**: Built-in `httplib`-based Web server providing a Cyberpunk-style dark dashboard. Monitor transfer speeds, node status, view system logs, and configure tasks in real-time.
* **System Tray Integration**: Native Windows tray support with auto-start capability and silent background operation.

## 🛠️ Tech Stack

* **Language**: C++20
* **Build System**: CMake, vcpkg (Manifest Mode)
* **Networking**: Boost.Asio, KCP, LibJuice (ICE), miniUPnPc
* **Web Service**: cpp-httplib, nlohmann/json
* **Storage**: SQLite3
* **Encryption/Compression**: OpenSSL, Snappy
* **System Integration**: Win32 API (Tray), efsw (File Watcher)
* **Logging**: spdlog (Async)

## 🚀 Getting Started

### Prerequisites

* **Compiler**: MSVC 2019+ (Windows) or GCC 10+/Clang 11+ (Linux)
* **Tools**: CMake 3.15+, Git

### Build Steps

This project uses `vcpkg` in Manifest mode for dependency management, making the build process very simple.

```bash
# 1. Clone the repository
git clone [https://github.com/jokerd888/veritassync.git](https://github.com/jokerd888/veritassync.git)
cd veritassync

# 2. Install vcpkg (if not already installed)
git clone [https://github.com/microsoft/vcpkg.git](https://github.com/microsoft/vcpkg.git)
./vcpkg/bootstrap-vcpkg.sh  # Run .\vcpkg\bootstrap-vcpkg.bat on Windows

# 3. Configure the project (Dependencies will be downloaded and compiled automatically; first run may be slow)
# Please replace <path_to_vcpkg> with your actual vcpkg path
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<path_to_vcpkg>/scripts/buildsystems/vcpkg.cmake

# 4. Build (Release Mode)
cmake --build build --config Release
````

### Running

#### 1\. Start the Signaling Server (Tracker)

The Tracker is used for discovery and signaling exchange between nodes.

```bash
./bin/veritas_tracker
# Default listening port: 9988
```

#### 2\. Start the Client (Sync Node)

The client will automatically minimize to the system tray and launch the Web console upon startup.

```bash
./bin/veritas_sync
```

#### 3\. Configuration & Usage

1.  Open your browser and visit **WebUI**: `http://127.0.0.1:8800`
2.  Set the Tracker address (e.g., `127.0.0.1:9988`) in **Global Config**.
3.  Click **"New Task"**:
      * **Sync Key**: Click 🎲 to generate a unique key (must use the same Key across devices).
      * **Sync Mode**: Select "One-way" or "Bi-directional".
      * **Local Path**: Select the folder to synchronize.
4.  Repeat the above steps on another device using the **same Sync Key**.

## 📂 Project Structure

```text
VeritasSync/
├── include/VeritasSync/   # Header files
│   ├── P2PManager.h       # Core P2P logic, NAT traversal
│   ├── SyncManager.h      # Sync state comparison algorithm
│   ├── WebUI.h            # Embedded Web server
│   └── ...
├── src/
│   ├── peer/              # Client implementation
│   │   ├── TransferManager.cpp # File chunking, encryption, transfer
│   │   ├── StateManager.cpp    # File scanning, DB interaction, conflict detection
│   │   └── ...
│   ├── tracker/           # Signaling server implementation
│   └── web/               # Web frontend resources (HTML/CSS/JS)
├── vcpkg.json             # Dependency manifest
└── CMakeLists.txt         # Build script
```

## 📄 License

This project is licensed under the [MIT License](https://www.google.com/search?q=LICENSE).

