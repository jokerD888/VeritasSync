# VeritasSync 开发备忘

## 编译方法

本项目使用 CMake + Ninja + MSVC (Visual Studio 2022 Build Tools) 构建，依赖 vcpkg 管理第三方库。

### 关键问题

在非 VS Developer Command Prompt 环境下（如 PowerShell、Git Bash、Claude Code），MSVC 标准库头文件路径未配置，会出现大量 `fatal error C1083: 无法打开包含文件: "atomic"` 等错误。**必须手动设置 INCLUDE/LIB/PATH 环境变量。**

### Debug 构建（推荐日常开发使用）

```bash
export MSVC_ROOT="C:/PROGRA~2/MICROS~2/2022/BUILDT~1/VC/Tools/MSVC/14.44.35207"
export WINSDK_VER="10.0.26100.0"
export WINSDK_INCLUDE="C:/Program Files (x86)/Windows Kits/10/Include/$WINSDK_VER"
export WINSDK_LIB="C:/Program Files (x86)/Windows Kits/10/Lib/$WINSDK_VER"
export INCLUDE="$MSVC_ROOT/include;$WINSDK_INCLUDE/ucrt;$WINSDK_INCLUDE/shared;$WINSDK_INCLUDE/um"
export LIB="$MSVC_ROOT/lib/x64;$WINSDK_LIB/ucrt/x64;$WINSDK_LIB/um/x64"
export PATH="$MSVC_ROOT/bin/Hostx64/x64:$PATH"
cmake --build build/debug 2>&1
```

### Release 构建

项目根目录提供了 `build_release.bat`，但需要先完成 cmake configure：

```bat
build_release.bat
```

注意：`build/release` 目录需要先执行过 cmake configure 才有 `build.ninja`。如果没有，需先执行：

```bash
cmake -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=C:/Users/alanmhlv/vcpkg/scripts/buildsystems/vcpkg.cmake
```

### 运行测试

```bash
cd build/debug && ./unit_tests.exe
```

## 项目结构

- `include/VeritasSync/` — 公共头文件
- `src/` — 源文件实现
- `tests/` — 单元测试 (Google Test)
- `build/debug/` — Debug 构建输出
- `build/release/` — Release 构建输出

### 核心模块

| 模块 | 目录 | 职责 |
|------|------|------|
| net | `net/` | 网络传输层（IceTransport, KcpSession, StunProber） |
| p2p | `p2p/` | P2P 连接管理（PeerController, P2PManager, TrackerClient） |
| sync | `sync/` | 文件同步（SyncNode, SyncSession, SyncHandler, TransferManager） |
| common | `common/` | 通用工具（Logger, CryptoLayer） |
