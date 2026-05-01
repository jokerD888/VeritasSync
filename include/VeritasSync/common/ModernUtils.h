#pragma once

// 现代 C++20 工具集
// 提供兼容性包装和通用类型定义

#include <cstddef>
#include <string>
#include <variant>
#include <optional>
#include <span>

namespace VeritasSync {

// 待集成

// =============================================================================
// std::span 辅助工具
// =============================================================================

/**
 * @brief 从 std::string 创建 byte span
 */
 // 利用std::span 实现的一组工具函数，用于实现 高性能，零拷贝的内存视图管理

inline std::span<const std::byte> as_bytes(const std::string& str) {
    return std::as_bytes(std::span{str.data(), str.size()});
}

/**
 * @brief 从 char 指针创建 span
 */
inline std::span<const char> make_span(const char* data, size_t size) {
    return std::span<const char>{data, size};
}

/**
 * @brief 从 std::string 创建 char span
 */
inline std::span<const char> make_span(const std::string& str) {
    return std::span<const char>{str.data(), str.size()};
}

/**
 * @brief 从 span 创建 std::string
 */
inline std::string to_string(std::span<const char> data) {
    return std::string{data.data(), data.size()};
}

// =============================================================================
// 结构化错误类型
// =============================================================================

/**
 * @brief 错误分类枚举
 */
enum class ErrorCategory {
    None,           // 无错误
    IO,             // I/O 相关错误
    Network,        // 网络相关错误
    Crypto,         // 加密解密错误
    Protocol,       // 协议解析错误
    Internal,       // 内部逻辑错误
    Timeout,        // 超时错误
    Cancelled       // 操作被取消
};

/**
 * @brief 结构化错误信息
 */
struct Error {
    ErrorCategory category = ErrorCategory::None;
    int code = 0;                  // 系统错误码或自定义错误码
    std::string message;           // 人类可读的错误描述
    std::string context;           // 额外上下文信息（如文件路径）
    
    // 便捷构造
    static Error none() {
        return Error{};
    }
    
    static Error io(int code, std::string msg, std::string ctx = "") {
        return Error{ErrorCategory::IO, code, std::move(msg), std::move(ctx)};
    }
    
    static Error network(int code, std::string msg, std::string ctx = "") {
        return Error{ErrorCategory::Network, code, std::move(msg), std::move(ctx)};
    }
    
    static Error crypto(std::string msg) {
        return Error{ErrorCategory::Crypto, 0, std::move(msg), ""};
    }
    
    static Error protocol(std::string msg, std::string ctx = "") {
        return Error{ErrorCategory::Protocol, 0, std::move(msg), std::move(ctx)};
    }
    
    static Error internal(std::string msg) {
        return Error{ErrorCategory::Internal, 0, std::move(msg), ""};
    }
    
    static Error timeout(std::string msg) {
        return Error{ErrorCategory::Timeout, 0, std::move(msg), ""};
    }
    
    static Error cancelled() {
        return Error{ErrorCategory::Cancelled, 0, "Operation cancelled", ""};
    }
    
    // 判断是否有错误
    explicit operator bool() const {
        return category != ErrorCategory::None;
    }
    
    bool ok() const {
        return category == ErrorCategory::None;
    }
    
    bool is_error() const {
        return category != ErrorCategory::None;
    }
    
    // 获取完整错误描述
    std::string full_message() const {
        if (!is_error()) return "";
        
        std::string result = message;
        if (code != 0) {
            result += " (code: " + std::to_string(code) + ")";
        }
        if (!context.empty()) {
            result += " [" + context + "]";
        }
        return result;
    }
};

// =============================================================================
// Result 类型（简化版 std::expected）
// =============================================================================

/**
 * @brief 操作结果类型
 * 
 * 使用方式：
 *   auto result = some_operation();
 *   if (result.has_value()) {
 *       use(*result);
 *   } else {
 *       handle_error(result.error());
 *   }
 */
template<typename T>
class Result {
public:
    // 成功构造 (显式，避免隐式转换歧义)
    Result(T value) : m_data(std::move(value)) {}
    
    // 失败构造
    Result(Error error) : m_data(std::move(error)) {}
    
    // 静态工厂方法：从 std::optional<T> 创建
    static Result from_optional(std::optional<T> opt, Error error_if_empty = Error::internal("No value")) {
        if (opt) {
            return Result(std::move(*opt));
        } else {
            return Result(std::move(error_if_empty));
        }
    }
    
    bool has_value() const { return std::holds_alternative<T>(m_data); }
    bool has_error() const { return std::holds_alternative<Error>(m_data); }
    
    explicit operator bool() const { return has_value(); }
    
    T& value() & { return std::get<T>(m_data); }
    const T& value() const& { return std::get<T>(m_data); }
    T&& value() && { return std::get<T>(std::move(m_data)); }
    
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T&& operator*() && { return std::move(*this).value(); }
    
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
    
    Error& error() & { return std::get<Error>(m_data); }
    const Error& error() const& { return std::get<Error>(m_data); }
    
    // 获取值或默认值
    T value_or(T default_value) const& {
        return has_value() ? value() : std::move(default_value);
    }
    
    // 映射函数
    template<typename Fn>
    auto map(Fn&& fn) -> Result<decltype(fn(std::declval<T>()))> {
        using U = decltype(fn(std::declval<T>()));
        if (has_value()) {
            return Result<U>(fn(value()));
        } else {
            return Result<U>(error());
        }
    }
    
private:
    std::variant<T, Error> m_data;
};

/**
 * @brief void 特化版本
 */
template<>
class Result<void> {
public:
    Result() : m_error(Error::none()) {}
    Result(Error error) : m_error(std::move(error)) {}
    
    bool has_value() const { return m_error.ok(); }
    bool has_error() const { return m_error.is_error(); }
    
    explicit operator bool() const { return has_value(); }
    
    Error& error() & { return m_error; }
    const Error& error() const& { return m_error; }
    
private:
    Error m_error;
};

// =============================================================================
// 常用类型别名
// =============================================================================

using ByteSpan = std::span<const std::byte>;
using MutableByteSpan = std::span<std::byte>;
using CharSpan = std::span<const char>;
using MutableCharSpan = std::span<char>;

// 字符串结果类型
using StringResult = Result<std::string>;

// 布尔结果类型
using BoolResult = Result<bool>;

// 无返回值结果类型
using VoidResult = Result<void>;

}  // namespace VeritasSync
