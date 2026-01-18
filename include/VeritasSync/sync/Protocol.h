#pragma once

#include <cstdint>  // 用于 uint64_t
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace VeritasSync {

    // 用于描述单个文件状态的结构体
    struct FileInfo {
        std::string path;             // 文件的相对路径
        std::string hash;             // 文件的SHA-256哈希值
        std::uint64_t modified_time;  // 文件的最后修改时间 (例如，自纪元以来的秒数)
        std::uint64_t size = 0;       // 文件大小（字节），用于断点续传校验

        // 为了方便，重载一下比较操作符
        bool operator==(const FileInfo& other) const {
            return path == other.path && hash == other.hash &&
                modified_time == other.modified_time && size == other.size;
        }
    };

    // --- nlohmann/json 集成魔法 ---
    inline void to_json(nlohmann::json& j, const FileInfo& info) {
        j = nlohmann::json{
            {"path", info.path}, {"hash", info.hash}, {"mtime", info.modified_time}, {"size", info.size} };
    }

    inline void from_json(const nlohmann::json& j, FileInfo& info) {
        j.at("path").get_to(info.path);
        j.at("hash").get_to(info.hash);
        j.at("mtime").get_to(info.modified_time);
        info.size = j.value("size", static_cast<std::uint64_t>(0));  // 兼容旧版本，默认为 0
    }

    // --- 协议消息类型定义 ---
    struct Protocol {
        // 消息类型
        static constexpr const char* MSG_TYPE = "type";
        static constexpr const char* MSG_PAYLOAD = "payload";

        // `type` 字段的各种值
        static constexpr const char* TYPE_SHARE_STATE = "share_state";
        static constexpr const char* TYPE_REQUEST_FILE = "request_file";
        static constexpr const char* TYPE_FILE_CHUNK = "file_chunk";

        // --- 增量更新所需 ---
        static constexpr const char* TYPE_FILE_UPDATE = "file_update";  // 文件新增或修改
        static constexpr const char* TYPE_FILE_DELETE = "file_delete";  // 文件删除

        static constexpr const char* TYPE_DIR_CREATE = "dir_create";  // 目录创建
        static constexpr const char* TYPE_DIR_DELETE = "dir_delete";  // 目录删除
        
        // --- 批量更新 (减少消息开销) ---
        static constexpr const char* TYPE_FILE_UPDATE_BATCH = "file_update_batch";  // 批量文件更新
        static constexpr const char* TYPE_FILE_DELETE_BATCH = "file_delete_batch";  // 批量文件删除
        static constexpr const char* TYPE_DIR_BATCH = "dir_batch";                  // 批量目录变更
        // --------------------

        // --- 状态同步握手协议 ---
        static constexpr const char* TYPE_SYNC_BEGIN = "sync_begin";  // 开始同步，携带预期文件/目录数量
        static constexpr const char* TYPE_SYNC_ACK = "sync_ack";      // 确认收到的数量，请求补发
        static constexpr const char* TYPE_SYNC_COMPLETE = "sync_complete";  // 同步完成确认
        // -----------------------

        // --- 断点续传相关 ---
        static constexpr const char* TYPE_GOODBYE = "goodbye";  // 程序正常退出通知
        // --------------------
        
        // --- 心跳保活 ---
        static constexpr const char* TYPE_HEARTBEAT = "heartbeat";      // 心跳请求
        static constexpr const char* TYPE_HEARTBEAT_ACK = "heartbeat_ack";  // 心跳响应
        // ----------------
    };

}  // namespace VeritasSync
