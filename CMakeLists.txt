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
find_package(openssl CONFIG REQUIRED)
find_package(efsw CONFIG CONFIG REQUIRED)
find_package(Boost 1.88.0 REQUIRED CONFIG COMPONENTS system asio)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(kcp CONFIG REQUIRED)
find_package(Snappy CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(LibJuice CONFIG REQUIRED)
find_package(unofficial-b64 CONFIG REQUIRED)




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

target_include_directories(veritas_sync PRIVATE ${PROJECT_SOURCE_DIR}/include)

# --- 为 veritas_sync 链接库 ---
target_link_libraries(veritas_sync PRIVATE
    OpenSSL::Crypto
    efsw::efsw
    Boost::system
    Boost::asio
    nlohmann_json::nlohmann_json
    unofficial::b64::b64
    kcp::kcp
    Snappy::snappy
    spdlog::spdlog
    LibJuice::LibJuice
)



# ===========================================================
# 目标 2: veritas_tracker (服务器程序)
# ===========================================================
add_executable(veritas_tracker src/tracker/main.cpp)
target_include_directories(veritas_tracker PRIVATE ${PROJECT_SOURCE_DIR}/include)

add_executable(efsw_test
    src/efsw_test.cpp
)


target_link_libraries(efsw_test PRIVATE efsw::efsw)


# --- 为 veritas_tracker 链接库 ---
target_link_libraries(veritas_tracker PRIVATE
    Boost::system
    Boost::asio
)
