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

# --- Release 模式编译优化 ---
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(MSVC)
        add_compile_options(/O2 /GL /arch:AVX2)
        add_link_options(/LTCG)
        message(STATUS "启用 MSVC Release 优化: /O2 /GL /LTCG")
    else()
        add_compile_options(-O3 -march=native)
        message(STATUS "启用 GCC/Clang Release 优化: -O3 -march=native")
    endif()
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
find_package(miniupnpc CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)


# ===========================================================
# 目标 1: veritas_sync (主程序)
# ===========================================================
add_executable(veritas_sync WIN32
    veritas.rc
    src/main.cpp
    src/platform/TrayIcon.cpp
    src/peer/Hashing.cpp
    src/peer/TrackerClient.cpp
    src/peer/P2PManager.cpp
    src/peer/StateManager.cpp
    src/peer/SyncManager.cpp
    src/peer/FileFilter.cpp
    src/peer/CryptoLayer.cpp
    src/peer/TransferManager.cpp
    src/peer/Database.cpp
    src/peer/SyncNode.cpp 
    src/peer/Logger.cpp
)

if(WIN32)
    target_link_options(veritas_sync PRIVATE "/ENTRY:mainCRTStartup")
endif()
add_custom_command(TARGET veritas_sync POST_BUILD
    # 2. 在 exe 旁边创建一个 web 文件夹
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:veritas_sync>/web
    
    # 3. 把 src/web/index.html 复制过去
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/src/web/index.html
        ${CMAKE_CURRENT_SOURCE_DIR}/src/web/app.svg
        ${CMAKE_CURRENT_SOURCE_DIR}/src/web/app.ico
        $<TARGET_FILE_DIR:veritas_sync>/web/
    
    # 4. (可选) 打印一条日志让你知道发生了什么
    COMMENT "Copying WebUI assets to output directory..."
)

target_include_directories(veritas_sync PRIVATE ${PROJECT_SOURCE_DIR}/include)

# --- 为 veritas_sync 链接库 ---
target_link_libraries(veritas_sync PRIVATE
    OpenSSL::Crypto
    OpenSSL::SSL
    efsw::efsw
    Boost::system
    Boost::asio
    nlohmann_json::nlohmann_json
    unofficial::b64::b64
    kcp::kcp
    Snappy::snappy
    spdlog::spdlog
    LibJuice::LibJuice
    miniupnpc::miniupnpc
    httplib::httplib
    unofficial::sqlite3::sqlite3
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


# ===========================================================
# 目标 4: nat_demo (NAT穿透测试程序)
# ===========================================================
add_executable(nat_demo src/nat_demo.cpp)
target_include_directories(nat_demo PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(nat_demo PRIVATE
    LibJuice::LibJuice
)


# --- 为 veritas_tracker 链接库 ---
target_link_libraries(veritas_tracker PRIVATE
    Boost::system
    Boost::asio
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

# ===========================================================
# 安装规则：自动收集所有依赖DLL并生成发行版
# ===========================================================
set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "Installation Directory")

# 安装所有可执行文件到 bin 目录
install(TARGETS veritas_sync veritas_tracker efsw_test nat_demo
    RUNTIME DESTINATION bin
    COMPONENT Runtime
)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/web"
    DESTINATION bin
    COMPONENT Runtime
)

if(WIN32)
    # 1. 尝试定位 vcpkg 的 bin 目录 (Manifest 模式下，通常在构建目录的 vcpkg_installed 中)
    # 我们通过查找一个已知的 DLL (如 brotlienc.dll) 来定位目录
    find_path(VCPKG_BIN_DIR "brotlienc.dll"
        PATHS
            "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/bin"
            "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/bin"
            "$ENV{VCPKG_ROOT}/installed/x64-windows/bin"
        NO_DEFAULT_PATH
    )

    if(VCPKG_BIN_DIR)
        message(STATUS "✅ 成功定位依赖 DLL 目录: ${VCPKG_BIN_DIR}")
        
        # 将该目录下的所有 .dll 文件复制到安装包的 bin 目录
        # [修复] COMPONENT 必须放在 FILES_MATCHING 之前
        install(DIRECTORY "${VCPKG_BIN_DIR}/"
            DESTINATION bin
            COMPONENT Runtime
            FILES_MATCHING PATTERN "*.dll"
        )
    else()
        message(WARNING "⚠️ 无法自动找到 vcpkg 的 bin 目录。尝试使用依赖扫描作为后备方案...")
        
        # 备用方案：自动扫描依赖 (可能不完整，但在找不到路径时聊胜于无)
        install(CODE [[
            file(GET_RUNTIME_DEPENDENCIES
                EXECUTABLES "${CMAKE_INSTALL_PREFIX}/bin/veritas_sync.exe"
                RESOLVED_DEPENDENCIES_VAR _r_deps
                UNRESOLVED_DEPENDENCIES_VAR _u_deps
                DIRECTORIES "$ENV{VCPKG_ROOT}/installed/x64-windows/bin"
            )
            foreach(_file ${_r_deps})
                string(TOLOWER "${_file}" _file_lower)
                if(_file_lower MATCHES "vcpkg")
                    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE SHARED_LIBRARY FILES "${_file}")
                endif()
            endforeach()
        ]] COMPONENT Runtime)
    endif()
endif()




# ===========================================================
# 单元测试 (GTest)
# ===========================================================
enable_testing()

find_package(GTest CONFIG REQUIRED)

# 定义测试可执行文件
add_executable(unit_tests 
    tests/test_sync_logic.cpp 
    tests/test_protocol.cpp
    tests/test_hashing.cpp
    tests/test_database.cpp
    src/peer/SyncManager.cpp 
    src/peer/Hashing.cpp
    src/peer/Database.cpp
    src/peer/Logger.cpp

)

# 包含头文件路径
target_include_directories(unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)

# 链接依赖
target_link_libraries(unit_tests PRIVATE 
    GTest::gtest 
    GTest::gtest_main
    nlohmann_json::nlohmann_json
    spdlog::spdlog 
    OpenSSL::Crypto
    unofficial::sqlite3::sqlite3
)

# 注册测试
include(GoogleTest)
gtest_discover_tests(unit_tests)



# ===========================================================
# CPack 打包配置 (生成 Windows 安装程序)
# ===========================================================

# 1. 基础信息设置
set(CPACK_PACKAGE_NAME "VeritasSync")
set(CPACK_PACKAGE_VENDOR "jokerD888")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "VeritasSync - 高性能 P2P 文件同步工具")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/jokerd888/veritassync")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}") # 读取项目开头的版本号 (1.0)
set(CPACK_PACKAGE_INSTALL_DIRECTORY "VeritasSync") # 安装路径: C:/Program Files/VeritasSync
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.txt")

# 2. 图标与资源配置
# 安装包的图标 (左上角图标)
set(CPACK_NSIS_MUI_ICON "${CMAKE_CURRENT_SOURCE_DIR}/app.ico") 
# 控制面板"卸载程序"列表显示的图标
set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\veritas_sync.exe")
# 安装向导的侧边栏图片 (可选，需要bmp格式，这里先留空使用默认)
# set(CPACK_NSIS_MUI_WELCOMEFINISHPAGE_BITMAP "${CMAKE_CURRENT_SOURCE_DIR}/sidebar.bmp")

# 3. 创建快捷方式 (桌面 & 开始菜单)
# 语法: CreateShortCut "快捷方式路径" "指向的目标exe"
set(CPACK_NSIS_CREATE_ICONS_EXTRA "
    CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\VeritasSync.lnk' '$INSTDIR\\\\bin\\\\veritas_sync.exe'
    CreateShortCut '$DESKTOP\\\\VeritasSync.lnk' '$INSTDIR\\\\bin\\\\veritas_sync.exe'
")

# --- [新增] 4. 自动配置防火墙规则 (安装时添加，卸载时删除) ---
# 使用 netsh 命令，在安装结束时执行
set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
    ExecWait 'netsh advfirewall firewall add rule name=\"VeritasSync\" dir=in action=allow program=\"$INSTDIR\\\\bin\\\\veritas_sync.exe\" enable=yes'
")

# 在卸载时删除规则
set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "
    ExecWait 'netsh advfirewall firewall delete rule name=\"VeritasSync\" program=\"$INSTDIR\\\\bin\\\\veritas_sync.exe\"'
")

# 5. 定义生成器
# NSIS = 生成 .exe 安装包
# ZIP  = 生成 .zip 绿色免安装包
set(CPACK_GENERATOR "NSIS;ZIP") 

# 6. 必须放在最后包含！
include(CPack)