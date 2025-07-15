cmake_minimum_required(VERSION 3.15)

project(VeritasSync VERSION 1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --------------------------------------------------------------------
# 这会告诉CMake去寻找OpenSSL。当配合vcpkg工具链使用时，
# vcpkg会自动告诉CMake在哪里可以找到它。
find_package(OpenSSL REQUIRED)
find_package(efsw CONFIG REQUIRED)
find_package(Boost 1.88.0 REQUIRED COMPONENTS system asio)
# --------------------------------------------------------------------

# 定义可执行文件目标1
add_executable(veritas_sync src/main.cpp)

target_sources(veritas_sync PRIVATE
    src/peer/Hashing.cpp
    src/peer/TrackerClient.cpp
    src/peer/P2PManager.cpp
)

# --------------------------------------------------------------------
# 通过链接到OpenSSL::Crypto这个“目标”，CMake会自动将OpenSSL的头文件目录添加到我们的项目中。
target_link_libraries(veritas_sync PRIVATE
    OpenSSL::Crypto
    efsw::efsw
    Boost::system
    Boost::asio
)
# --------------------------------------------------------------------
target_include_directories(veritas_sync PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

# -----------------------------------------------------------
# 目标 2: Tracker服务器程序
add_executable(veritas_tracker src/tracker/main.cpp)
# Tracker也需要链接Boost::system (Asio的依赖)
target_link_libraries(veritas_tracker PRIVATE
    Boost::system
    Boost::asio
)
# -----------------------------------------------------------