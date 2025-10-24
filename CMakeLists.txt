cmake_minimum_required(VERSION 3.15)

project(VeritasSync VERSION 1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- 为Windows平台添加必要的宏定义 ---
if(WIN32)
  add_definitions(-D_WIN32_WINNT=0x0A00) # 将目标平台设置为 Windows 10
endif()

# --- 查找依赖包 ---
find_package(OpenSSL REQUIRED)
find_package(Boost 1.88.0 REQUIRED COMPONENTS system asio program_options)
find_package(Boost 1.88.0 REQUIRED COMPONENTS system asio)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(kcp CONFIG REQUIRED)
find_package(GTest REQUIRED) # 【新增】 查找 Google Test

# --- 包含目录 ---
include_directories(${PROJECT_SOURCE_DIR}/include)
include_directories("d:/vcpkg/installed/x64-windows-static/include")
# 【新增】为 gtest 添加 include 路径 (如果 vcpkg 需要)
include_directories("d:/vcpkg/installed/x64-windows-static/include/gtest")


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
    Boost::system
    Boost::asio
    Boost::program_options
    nlohmann_json::nlohmann_json
    b64
    kcp::kcp
)

target_link_directories(veritas_sync PRIVATE "d:/vcpkg/installed/x64-windows-static/lib")


# ===========================================================
# 
# ===========================================================
add_executable(veritas_tracker src/tracker/main.cpp)


# --- 为 veritas_tracker 链接库 ---
target_link_libraries(veritas_tracker PRIVATE
    Boost::system
    Boost::asio
)


# ===========================================================
# 【新增】 目标 3: 单元测试
# ===========================================================
enable_testing() # 启用 CTest

add_executable(veritas_tests
    tests/main.cpp
    tests/test_hashing.cpp
    tests/test_sync_manager.cpp
    
    # 链接我们需要测试的源文件
    src/peer/Hashing.cpp
    src/peer/SyncManager.cpp
)

# --- 【修改】为 veritas_tests 链接库 ---
# 
# 使用 GTest::gtest_main (而不是 GTest::gtest 和 GTest::gtest_main)
# GTest::gtest_main 会自动拉入 gtest 和 gtest_main 库，
# 并且 vcpkg 的 CMake 魔法会确保在 Debug 模式下
# 链接 Debug 库，在 Release 模式下链接 Release 库。
#
target_link_libraries(veritas_tests PRIVATE
    GTest::gtest_main
    OpenSSL::Crypto # Hashing 测试需要
)

# 添加到 CTest
add_test(NAME UnitTests COMMAND veritas_tests)