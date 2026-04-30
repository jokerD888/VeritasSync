# VeritasSync

<div align="center">
  <img src="app.ico" alt="VeritasSync Logo" width="128" height="128" />
  <br />

  [![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
  [![C++](https://img.shields.io/badge/Language-C%2B%2B20-blue.svg)](https://en.cppreference.com/w/cpp/20)
  [![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux-lightgrey.svg)]()
  [![Tests](https://img.shields.io/badge/Tests-557%20Passing-brightgreen)]()

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
* **Smart NAT Traversal (ICE)**: Integrated with **LibJuice** (STUN/TURN), supporting traversal for various NAT types including Full Cone and Restricted Cone. Automatically detects the optimal path (prioritizing P2P direct connection, with Relay as a fallback).
* **Resumable Transfer**: Supports automatic resumption after transfer interruption with bitmap-based chunk tracking and contiguous verification for accurate recovery.

### 🔄 Flexible Synchronization Logic
* **Bi-Directional Sync**: Supports mutual synchronization between multiple devices with built-in **Source-side Echo Suppression** algorithm that prevents echo broadcasts at the source, saving bandwidth.
* **Smart Incremental Updates**: Uses **SQLite** to cache file metadata (Hash + mtime) with a **Write-Through in-memory cache layer** for O(1) metadata queries. Combined with **efsw** file monitoring for millisecond-level change detection and incremental synchronization.
* **Conflict Resolution Strategy**: Automatically detects conflicts when the same file is modified on multiple devices simultaneously, preserving a copy (renamed to `filename.conflict.<timestamp>.ext`) to ensure zero data loss.
* **Custom Ignore Rules**: Configurable via `.veritasignore` file (`.gitignore`-compatible syntax with `**` wildcards, `!` negation, character classes `[a-z]`), with a visual editor and AI-powered natural language rule generation in the Web UI.

### 🛡️ Security & Engineering
* **End-to-End Encryption**: Communication links encrypted using **AES-256-GCM**, with keys securely derived via **HKDF-SHA256** from the sync key.
* **Web Security**: XSS prevention (full dynamic content escaping), CSP security headers, session-scoped token storage (sessionStorage), sensitive field masking (passwords shown as `***` in API responses), path traversal protection (sync_folder forced absolute + canonicalization).
* **UTF-8 Everywhere**: Completely resolves garbled character issues with non-ASCII paths on Windows, ensuring perfect cross-platform filename compatibility.
* **O(1) Memory Usage**: Adopts Streaming Transfer and Snappy compression. Memory usage remains low regardless of syncing a 10GB video or millions of small files.
* **Single Instance Protection**: Windows named mutex + safe restart flow (CreateProcessW synchronous creation; Linux uses setsid + FD cleanup).
* **557 Unit Tests**: Covering core sync logic, transfer management, encryption/decryption, config validation, state management, and more.

### 🖥️ Modern User Experience
* **WebUI Console**: Built-in `httplib`-based Web server providing a Cyberpunk-style dark dashboard.
* **Real-time Task Status**: Each sync task displays a status badge (🔵 Syncing / 🟢 Idle / 🟡 Waiting / 🔴 Offline / ⚫ Stopped), auto-refreshed every second.
* **Transfer Monitoring**: Real-time progress bars, speed, and chunk status for active transfers; stall detection included.
* **P2P Connection Details**: Shows each peer's connection type (direct/relay), duration, and state.
* **System Tray Integration**: Native Windows tray support with auto-start capability and silent background operation.
* **AI Ignore Rule Generation**: Describe files to ignore in natural language, and rules are auto-generated (built-in template engine + optional LLM backend).

## 🛠️ Tech Stack

| Category | Technology |
|----------|-----------|
| **Language** | C++20 (std::jthread, std::span, std::shared_mutex, std::atomic) |
| **Build System** | CMake, vcpkg (Manifest Mode) |
| **Networking** | Boost.Asio, KCP, LibJuice (ICE), miniUPnPc |
| **Web Service** | cpp-httplib (CSP/CORS headers), nlohmann/json |
| **Storage** | SQLite3 (WAL mode) + Write-Through in-memory cache |
| **Encryption/Compression** | OpenSSL (AES-256-GCM, HKDF-SHA256), Snappy |
| **System Integration** | Win32 API (Tray, Mutex), efsw (File Watcher) |
| **Logging** | spdlog (Async) |
| **Testing** | Google Test (557 tests) |

## 🏗️ Architecture

```
┌─────────────────────────────────────────────────────┐
│                  WebUI (HTML/JS)                     │
│            Cyberpunk Dark Dashboard                  │
├─────────────────────────────────────────────────────┤
│               httplib REST API                       │
│      guarded_route + parse_task_index middleware     │
├──────────────┬──────────────┬────────────────────────┤
│  SyncNode    │ TrackerClient│    StateManager        │
│ (Task Lifecycle)│ (Signaling) │ (File State + DB Cache)│
├──────────────┼──────────────┼────────────────────────┤
│          P2PManager (Peer Connection Manager)        │
│    PeerController → IceTransport → KcpSession        │
├──────────────┼──────────────┼────────────────────────┤
│ SyncHandler  │ SyncSession  │  TransferManager       │
│ (Msg Dispatch)│ (Negotiation)│ (Chunked Transfer)    │
├──────────────┴──────────────┴────────────────────────┤
│            CryptoLayer (AES-256-GCM)                 │
│       CachedFileStore (Write-Through Cache)          │
│            Database (SQLite3 WAL)                    │
└─────────────────────────────────────────────────────┘
```

## 🚀 Getting Started

### Prerequisites

* **Compiler**: MSVC 2019+ (Windows) or GCC 10+/Clang 11+ (Linux)
* **Tools**: CMake 3.15+, Git

### Build Steps

```bash
# 1. Clone the repository
git clone https://github.com/jokerD888/VeritasSync.git
cd VeritasSync

# 2. Install vcpkg (if not already installed)
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh  # Run .\vcpkg\bootstrap-vcpkg.bat on Windows

# 3. Configure the project (Dependencies downloaded automatically; first run may be slow)
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<path_to_vcpkg>/scripts/buildsystems/vcpkg.cmake

# 4. Build (Release Mode)
cmake --build build --config Release

# 5. Run tests (optional)
cd build && ctest --output-on-failure
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

1.  Access WebUI via the **"Open Console"** menu in the system tray (auth token auto-injected).
2.  Set the Tracker address (e.g., `127.0.0.1:9988`) in **Global Config**.
3.  Click **"New Task"**:
      * **Sync Key**: Click 🎲 to generate a unique key (minimum 16 characters; must use the same Key across devices).
      * **Sync Mode**: Select "One-way" or "Bi-directional".
      * **Local Path**: Select the folder to synchronize (must be an absolute path).
4.  Repeat the above steps on another device using the **same Sync Key**.
5.  Watch the **status badge** on the task card to confirm connection state.
6.  Click **"Ignore Rules"** to configure files to exclude — supports manual editing or AI natural language generation.

## 📂 Project Structure

```text
VeritasSync/
├── include/VeritasSync/   # Header files
│   ├── common/            # Utilities (Config, Logger, Hashing, CryptoLayer, PathUtils)
│   ├── net/               # Network layer (KcpSession, IceTransport)
│   ├── p2p/               # P2P core (P2PManager, PeerController, TrackerClient, WebUI)
│   ├── storage/           # Storage layer (StateManager, Database, CachedFileStore, FileFilter)
│   └── sync/              # Sync layer (SyncNode, SyncHandler, TransferManager, Protocol)
├── src/
│   ├── common/            # Utilities implementation
│   ├── net/               # Network layer implementation
│   ├── p2p/               # P2P layer implementation
│   ├── storage/           # Storage layer implementation (incl. CachedFileStore Write-Through cache)
│   ├── sync/              # Sync layer implementation
│   ├── tracker/           # Signaling server implementation
│   └── web/               # Web frontend resources (HTML/CSS/JS)
├── tests/                 # Unit tests (30 test files, 557 test cases)
├── docs/                  # Architecture documentation
├── vcpkg.json             # Dependency manifest
└── CMakeLists.txt         # Build script
```

## 🔧 Configuration

### config.json

```json
{
    "device_id": "auto-generated-uuid",
    "tracker_host": "your-tracker-server.com",
    "tracker_port": 9988,
    "webui_port": 8800,
    "stun_host": "stun.l.google.com",
    "stun_port": 19302,
    "turn_host": "",
    "turn_port": 3478,
    "turn_username": "",
    "turn_password": "",
    "chunk_size": 16384,
    "kcp_window_size": 256,
    "kcp_update_interval_ms": 20,
    "tasks": [
        {
            "sync_key": "your-unique-sync-key-min-16-chars",
            "sync_folder": "/absolute/path/to/folder",
            "role": "source",
            "mode": "bidirectional"
        }
    ]
}
```

### .veritasignore

Create a `.veritasignore` file in the sync directory to customize ignore rules (`.gitignore`-compatible syntax):

```gitignore
# Ignore log and temp files
*.log
*.tmp
*.temp

# Ignore directories
node_modules/
.git/
__pycache__/

# Glob patterns
**/build/**
**/*.o

# Negation (don't ignore specific files)
!important.log

# Character classes
[Tt]humbs.db
```

## 📄 License

This project is licensed under the [MIT License](LICENSE).
