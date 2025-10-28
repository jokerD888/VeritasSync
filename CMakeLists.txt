cmake_minimum_required(VERSION 3.15)

project(VeritasSync VERSION 1.0)

if(MSVC)
  add_compile_options(/utf-8)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- 为Windows平台添加必要的宏定义 ---
if(WIN32)
  add_definitions(-D_WIN32_WINNT=0x0A00) # 将目标平台设置为 Windows 10
endif()

# --- 查找依赖包 ---
find_package(OpenSSL REQUIRED)
find_package(efsw CONFIG REQUIRED)
find_package(Boost 1.88.0 REQUIRED COMPONENTS system asio)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(kcp CONFIG REQUIRED)

# --- 包含目录 ---
include_directories(${PROJECT_SOURCE_DIR}/include)
include_directories("d:/vcpkg/installed/x64-windows-static/include")


# ===========================================================
# 目标 1: veritas_sync (主程序)
# ===========================================================
add_executable(veritas_sync
    src/main.cpp
    src/peer/Hashing.cpp
    src/peer/TrackerClient.cpp
    src/peer/P2PManager.cpp
    
    src/peer/StateManager.cpp
    src/peer/SyncManager.cpp
)


# --- 为 veritas_sync 链接库 ---
target_link_libraries(veritas_sync PRIVATE
    OpenSSL::Crypto
    efsw::efsw
    Boost::system
    Boost::asio
    nlohmann_json::nlohmann_json
    b64
    kcp::kcp
)

target_link_directories(veritas_sync PRIVATE "d:/vcpkg/installed/x64-windows-static/lib")


# ===========================================================
# 目标 2: veritas_tracker (服务器程序)
# ===========================================================
add_executable(veritas_tracker src/tracker/main.cpp)


# --- 为 veritas_tracker 链接库 ---
target_link_libraries(veritas_tracker PRIVATE
    Boost::system
    Boost::asio
)
