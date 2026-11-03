// Minimal stand-in for Vita3K's util/log.h (spdlog based), used only by the standalone harness.
#pragma once

#include <fmt/format.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>

// Vita3K's log_hex(): formats a number (or pointer) as 0x... in log messages
template <typename T>
inline std::string log_hex(T value) {
    if constexpr (std::is_pointer_v<T>)
        return fmt::format("0x{:x}", static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value)));
    else
        return fmt::format("0x{:x}", static_cast<uint64_t>(value));
}

#define NH_LOG(level, ...)                                                    \
    do {                                                                      \
        std::fputs("[" level "] ", stderr);                                   \
        std::fputs(fmt::format(__VA_ARGS__).c_str(), stderr);                 \
        std::fputc('\n', stderr);                                             \
    } while (0)

#define LOG_TRACE(...) NH_LOG("TRACE", __VA_ARGS__)
#define LOG_DEBUG(...) NH_LOG("DEBUG", __VA_ARGS__)
#define LOG_INFO(...) NH_LOG("INFO", __VA_ARGS__)
#define LOG_WARN(...) NH_LOG("WARN", __VA_ARGS__)
#define LOG_ERROR(...) NH_LOG("ERROR", __VA_ARGS__)
#define LOG_CRITICAL(...) NH_LOG("CRITICAL", __VA_ARGS__)

#define LOG_TRACE_IF(cond, ...) do { if (cond) LOG_TRACE(__VA_ARGS__); } while (0)
#define LOG_DEBUG_IF(cond, ...) do { if (cond) LOG_DEBUG(__VA_ARGS__); } while (0)
#define LOG_INFO_IF(cond, ...) do { if (cond) LOG_INFO(__VA_ARGS__); } while (0)
#define LOG_WARN_IF(cond, ...) do { if (cond) LOG_WARN(__VA_ARGS__); } while (0)
#define LOG_ERROR_IF(cond, ...) do { if (cond) LOG_ERROR(__VA_ARGS__); } while (0)
#define LOG_CRITICAL_IF(cond, ...) do { if (cond) LOG_CRITICAL(__VA_ARGS__); } while (0)
